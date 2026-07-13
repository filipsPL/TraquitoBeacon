#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>
using namespace std;

#include "Evm.h"
#include "Log.h"
#include "Shell.h"
#include "GPS.h"
#include "TimeClass.h"
#include "Timeline.h"
#include "Utl.h"


enum class SlotMode : uint8_t { WSPR2 = 0, WSPR15 = 1, CW = 2 };


class Scheduler
{
public:

    struct Slot
    {
        SlotMode mode    = SlotMode::WSPR2;
        string   band    = "";        // informational; selects which hardware filter is appropriate
        uint16_t channel = 0;          // WSPR-only: index into WsprChannelMap
        uint32_t frequencyHz = 0;      // CW-only: exact transmit frequency
        uint8_t  wpm     = 18;         // CW-only: keying speed
        // CW-only: precomputed by Application when building the slot list.
        // Lets the scheduler know how long the CW transmission will run without
        // needing to peek at Configuration / Morse encoder internals.
        uint32_t cwDurationMs = 0;
        string   cwText  = "";
    };

    static constexpr uint8_t SLOT_MAX = 16;

    // Alignment requirement (UTC minutes) for the slot's mode.
    // WSPR-2 → 2-min windows; WSPR-15 → 15-min windows; CW → no alignment (0).
    static uint8_t AlignmentMinutes(SlotMode m)
    {
        switch (m)
        {
            case SlotMode::WSPR15: return 15;
            case SlotMode::CW:     return 0;
            case SlotMode::WSPR2:
            default:               return 2;
        }
    }

    // Duration the scheduler should reserve for this slot.
    // WSPR: fixed period (2 or 15 minutes).
    // CW: actual transmission time + a small padding for setup/teardown.
    static uint64_t SlotDurationUs(const Slot &s)
    {
        if (s.mode == SlotMode::CW)
        {
            const uint64_t CW_PAD_US = 1'000'000ULL;  // 1 s pad after the last element
            return (uint64_t)s.cwDurationMs * 1000ULL + CW_PAD_US;
        }
        return (uint64_t)AlignmentMinutes(s.mode) * 60ULL * 1'000'000ULL;
    }


    Scheduler()
    {
        SetupShell();
        t_.SetMaxEvents(80);
    }


    /////////////////////////////////////////////////////////////////
    // Configuration
    /////////////////////////////////////////////////////////////////

    void SetSlots(const vector<Slot> &slots)
    {
        slots_ = slots;
        if (slots_.size() > SLOT_MAX) { slots_.resize(SLOT_MAX); }

        // Rebuild per-slot timers (unique_ptr, Timers are non-movable).
        timerSlotNames_.clear();
        timerSlotVec_.clear();
        timerWarmupNames_.clear();
        timerWarmupVec_.clear();
        timerPreNotifyNames_.clear();
        timerPreNotifyVec_.clear();
        // Reserve before pushing so string storage never relocates while
        // earlier Timers still hold c_str() pointers into it.
        timerSlotNames_.reserve(slots_.size());
        timerWarmupNames_.reserve(slots_.size());
        timerPreNotifyNames_.reserve(slots_.size());
        for (size_t i = 0; i < slots_.size(); ++i)
        {
            timerSlotNames_.push_back(string{"TIMER_SLOT_"} + to_string(i + 1));
            timerSlotVec_.push_back(make_unique<Timer>(timerSlotNames_.back().c_str()));
            timerWarmupNames_.push_back(string{"TIMER_WARMUP_"} + to_string(i + 1));
            timerWarmupVec_.push_back(make_unique<Timer>(timerWarmupNames_.back().c_str()));
            timerPreNotifyNames_.push_back(string{"TIMER_PRENOTIFY_"} + to_string(i + 1));
            timerPreNotifyVec_.push_back(make_unique<Timer>(timerPreNotifyNames_.back().c_str()));
        }
    }

    void SetTxInterval(uint8_t interval)
    {
        txInterval_  = interval < 1 ? 1 : interval;
        skipCounter_ = 0;
    }


    /////////////////////////////////////////////////////////////////
    // Callbacks (set by Application)
    /////////////////////////////////////////////////////////////////

    void SetCallbackRequestNewGpsLock(function<void()> fn)        { fnCbRequestNewGpsLock_       = fn; }
    void SetCallbackCancelRequestNewGpsLock(function<void()> fn)  { fnCbCancelRequestNewGpsLock_ = fn; }
    void SetCallbackScheduleNow(function<void(bool haveGpsLock)> fn) { fnCbScheduleNow_ = fn; }
    void SetCallbackFirstCycleEnd(function<void()> fn)            { fnCbFirstCycleEnd_   = fn; }
    void SetCallbackRadioIsActive(function<bool()> fn)            { fnCbRadioIsActive_   = fn; }
    void SetCallbackStartRadioWarmup(function<void()> fn)         { fnCbStartRadioWarmup_= fn; }
    void SetCallbackStopRadio(function<void()> fn)                { fnCbStopRadio_       = fn; }
    void SetCallbackGoHighSpeed(function<void()> fn)              { fnCbGoHighSpeed_     = fn; }
    void SetCallbackGoLowSpeed(function<void()> fn)               { fnCbGoLowSpeed_      = fn; }
    // Called right before each slot's send to switch transmitter freq + WSPR mode.
    void SetCallbackPrepareSlot(function<void(uint8_t slot1Based, const Slot &)> fn) { fnCbPrepareSlot_   = fn; }
    // Called to actually transmit the message for this slot.
    void SetCallbackSendSlot(function<void(uint8_t slot1Based, const Slot &)> fn)    { fnCbSendSlot_      = fn; }
    // Called ~1 s before the slot fires — use to log/display TX_START before the blocking TX.
    void SetCallbackAnnounceSlot(function<void(uint8_t slot1Based, const Slot &)> fn){ fnCbAnnounceSlot_  = fn; }
    // Called at the end of PrepareCycleSchedule with the UTC time string of the first slot.
    void SetCallbackOnCycleArmed(function<void(const string &slot1TimeUtc)> fn)      { fnCbOnCycleArmed_  = fn; }


    /////////////////////////////////////////////////////////////////
    // Lifecycle
    /////////////////////////////////////////////////////////////////

    void Start()
    {
        if (running_) { return; }
        Mark("START");

        running_    = true;
        firstCycle_ = true;
        skipCounter_ = 0;

        if (slots_.empty())
        {
            Log("Scheduler::Start with no slots - no transmissions will occur");
        }

        RequestNewGpsLock();
        LogNL();
    }

    void Stop()
    {
        if (!running_) { return; }
        Mark("STOP");

        running_      = false;
        reqGpsActive_ = false;
        inLockout_    = false;
        scheduleDataActive_ = ScheduleData{};
        scheduleDataCache_  = ScheduleData{};

        CancelAllTimers();
        LogNL();
    }


    /////////////////////////////////////////////////////////////////
    // GPS event entry points (called by Application)
    /////////////////////////////////////////////////////////////////

    void OnGpsTimeLock(const FixTime &fix)
    {
        if (!running_) { return; }
        uint64_t timeNowUs = fix.timeAtPpsUs;

        if (reqGpsActive_ && !inLockout_)
        {
            Mark("ON_GPS_LOCK_TIME_APPLIED");
            scheduleDataActive_.gpsFixTime            = fix;
            scheduleDataActive_.timeAtGpsFixTimeSetUs = timeNowUs;
            ScheduleApplyTimeAndUpdateSchedule(fix, timeNowUs, false);
        }
        else if (reqGpsActive_ && inLockout_)
        {
            Mark("ON_GPS_LOCK_TIME_CACHED");
            scheduleDataCache_.gpsFixTime            = fix;
            scheduleDataCache_.timeAtGpsFixTimeSetUs = timeNowUs;
        }
        LogNL();
    }

    void OnGps3DPlusLock(const Fix3DPlus &fix)
    {
        if (!running_) { return; }
        uint64_t timeNowUs = fix.timeAtPpsUs;

        if (reqGpsActive_ && !inLockout_)
        {
            Mark("ON_GPS_LOCK_3D_PLUS_APPLIED");
            scheduleDataActive_.gpsFix3DPlus            = fix;
            scheduleDataActive_.timeAtGpsFix3DPlusSetUs = timeNowUs;
            CancelRequestNewGpsLock();
            ScheduleApplyTimeAndUpdateSchedule(fix, timeNowUs, true);
        }
        else if (reqGpsActive_ && inLockout_)
        {
            Mark("ON_GPS_LOCK_3D_PLUS_CACHED");
            scheduleDataCache_.gpsFix3DPlus            = fix;
            scheduleDataCache_.timeAtGpsFix3DPlusSetUs = timeNowUs;
            CancelRequestNewGpsLock();
        }
        LogNL();
    }

    void CancelRequestNewGpsLock()
    {
        Mark("CANCEL_REQ_NEW_GPS_LOCK");
        reqGpsActive_ = false;
        fnCbCancelRequestNewGpsLock_();
    }


    /////////////////////////////////////////////////////////////////
    // Public utility
    /////////////////////////////////////////////////////////////////

    // Compute the fire times for one cycle starting at or after notMin/notSec/notUs.
    // Pure function — used by tests and live scheduling.
    struct CycleFires
    {
        vector<uint64_t> fireOffsetUs;  // microseconds from cycleStartHintUs
        uint64_t         cycleEndOffsetUs = 0;
    };

    static CycleFires ComputeCycleFires(const vector<Slot> &slots,
                                        uint8_t notMin, uint8_t notSec, uint32_t notUs)
    {
        CycleFires out;
        out.fireOffsetUs.resize(slots.size());

        uint64_t cursorUs   = 0;  // offset from notional "now"
        uint8_t  cursorMin  = notMin;
        uint8_t  cursorSec  = notSec;
        uint32_t cursorUsec = notUs;

        for (size_t i = 0; i < slots.size(); ++i)
        {
            uint64_t alignDeltaUs = ComputeAlignDeltaUs(slots[i].mode, cursorMin, cursorSec, cursorUsec);
            cursorUs += alignDeltaUs;
            out.fireOffsetUs[i] = cursorUs;

            uint64_t slotUs = SlotDurationUs(slots[i]);
            cursorUs += slotUs;

            // Advance the cursor's notional clock.
            uint64_t totalAddUs = alignDeltaUs + slotUs;
            AdvanceClock(cursorMin, cursorSec, cursorUsec, totalAddUs);
        }
        out.cycleEndOffsetUs = cursorUs;
        return out;
    }

    bool IsRunning() const { return running_; }


private:

    /////////////////////////////////////////////////////////////////
    // Alignment helpers
    /////////////////////////////////////////////////////////////////

    // Returns microseconds from (notMin,notSec,notUs) to the next valid start
    // for the given mode (start = aligned-minute boundary + 1 second for WSPR;
    // immediate for CW since it has no UTC alignment requirement).
    static uint64_t ComputeAlignDeltaUs(SlotMode mode, uint8_t notMin, uint8_t notSec, uint32_t notUs)
    {
        // CW: no UTC alignment — fire as soon as the previous slot ends.
        if (mode == SlotMode::CW) { return 0; }

        uint8_t align = AlignmentMinutes(mode);

        int minDiff = (int)(align - (notMin % align));
        if (minDiff == align) { minDiff = 0; }

        int  secDiff = 1 - (int)notSec;
        int  usDiff  = -(int)notUs;

        int64_t totalUs = 0;
        totalUs += (int64_t)minDiff * 60 * 1'000 * 1'000;
        totalUs += (int64_t)secDiff *      1'000 * 1'000;
        totalUs += (int64_t)usDiff;

        if (totalUs < 0)
        {
            totalUs += (int64_t)align * 60 * 1'000 * 1'000;
        }
        return (uint64_t)totalUs;
    }

    static void AdvanceClock(uint8_t &min_, uint8_t &sec_, uint32_t &us_, uint64_t addUs)
    {
        uint64_t totalUs = (uint64_t)min_ * 60ULL * 1'000'000ULL
                         + (uint64_t)sec_ *         1'000'000ULL
                         + (uint64_t)us_;
        totalUs += addUs;
        totalUs %= 60ULL * 60ULL * 1'000'000ULL;  // wrap at one hour - we only care about minute-within-hour
        min_ = (uint8_t)(totalUs / (60ULL * 1'000'000ULL));
        uint64_t rem = totalUs % (60ULL * 1'000'000ULL);
        sec_ = (uint8_t)(rem / 1'000'000ULL);
        us_  = (uint32_t)(rem % 1'000'000ULL);
    }


    /////////////////////////////////////////////////////////////////
    // GPS lock + lockout state
    /////////////////////////////////////////////////////////////////

    struct ScheduleData
    {
        Fix3DPlus gpsFix3DPlus;
        uint64_t  timeAtGpsFix3DPlusSetUs = 0;
        FixTime   gpsFixTime;
        uint64_t  timeAtGpsFixTimeSetUs   = 0;
    };

    ScheduleData scheduleDataCache_;
    ScheduleData scheduleDataActive_;

    bool reqGpsActive_ = false;
    bool inLockout_    = false;
    bool running_      = false;
    bool firstCycle_   = true;

    void RequestNewGpsLock()
    {
        Mark("REQ_NEW_GPS_LOCK");
        reqGpsActive_ = true;
        fnCbRequestNewGpsLock_();
    }

    void OnScheduleLockoutStart()
    {
        Mark("SCHEDULE_LOCK_OUT_START");
        inLockout_ = true;
        LogNL();
    }

    void OnScheduleLockoutEnd()
    {
        LogNL();
        Mark("SCHEDULE_LOCK_OUT_END");
        t_.ReportNow();

        fnCbGoLowSpeed_();
        inLockout_ = false;

        if (firstCycle_)
        {
            firstCycle_ = false;
            Mark("FIRST_CYCLE_END");
            fnCbFirstCycleEnd_();
        }

        ScheduleApplyCache();
        LogNL();
    }

    void ScheduleApplyCache()
    {
        bool fix3dFresh = scheduleDataCache_.timeAtGpsFix3DPlusSetUs != 0;
        bool fixTimeFresh = scheduleDataCache_.timeAtGpsFixTimeSetUs != 0;

        if (fix3dFresh)
        {
            scheduleDataActive_.gpsFix3DPlus            = scheduleDataCache_.gpsFix3DPlus;
            scheduleDataActive_.timeAtGpsFix3DPlusSetUs = scheduleDataCache_.timeAtGpsFix3DPlusSetUs;
        }
        if (fixTimeFresh)
        {
            scheduleDataActive_.gpsFixTime            = scheduleDataCache_.gpsFixTime;
            scheduleDataActive_.timeAtGpsFixTimeSetUs = scheduleDataCache_.timeAtGpsFixTimeSetUs;
        }
        scheduleDataCache_ = ScheduleData{};

        if (fix3dFresh)
        {
            Mark("APPLY_CACHE_NEW_3D_PLUS");
            ScheduleApplyTimeAndUpdateSchedule(scheduleDataActive_.gpsFix3DPlus,
                                               scheduleDataActive_.timeAtGpsFix3DPlusSetUs, true);
        }
        else if (fixTimeFresh)
        {
            Mark("APPLY_CACHE_NEW_TIME");
            ScheduleApplyTimeAndUpdateSchedule(scheduleDataActive_.gpsFixTime,
                                               scheduleDataActive_.timeAtGpsFixTimeSetUs, false);
        }
        else if (scheduleDataActive_.timeAtGpsFix3DPlusSetUs &&
                 scheduleDataActive_.timeAtGpsFix3DPlusSetUs >= scheduleDataActive_.timeAtGpsFixTimeSetUs)
        {
            Mark("APPLY_CACHE_OLD_3D_PLUS");
            ScheduleApplyTimeAndUpdateSchedule(scheduleDataActive_.gpsFix3DPlus,
                                               scheduleDataActive_.timeAtGpsFix3DPlusSetUs, false);
        }
        else
        {
            Mark("APPLY_CACHE_OLD_TIME");
            ScheduleApplyTimeAndUpdateSchedule(scheduleDataActive_.gpsFixTime,
                                               scheduleDataActive_.timeAtGpsFixTimeSetUs, false);
        }
    }

    void ScheduleApplyTimeAndUpdateSchedule(const FixTime &gpsFixTime, uint64_t timeAtGpsFixTimeSetUs, bool haveGpsLock)
    {
        Mark("APPLY_TIME_AND_UPDATE_SCHEDULE");
        SetNotionalTimeFromGpsTime(gpsFixTime, timeAtGpsFixTimeSetUs);

        if (haveGpsLock)
        {
            Mark("COAST_CANCELED");
            timerCoast_.Cancel();
            ScheduleUpdateSchedule(true);
        }
        else
        {
            // arm coast: if no fresh lock arrives, fall back to fix-time at last moment
            timerCoast_.SetCallback([this]{
                Mark("COAST_TRIGGERED");
                CancelRequestNewGpsLock();
                ScheduleUpdateSchedule(false);
                LogNL();
            });

            const uint64_t COAST_LEAD_US = 7 * 1'000'000;
            uint64_t timeNowUs;
            uint64_t timeAtNextCycleStartHintUs = GetNextCycleStartHintUs(&timeNowUs);
            uint64_t timeAtTriggerCoastUs = timeAtNextCycleStartHintUs
                                          - min(COAST_LEAD_US, timeAtNextCycleStartHintUs - timeNowUs);
            timerCoast_.TimeoutAtUs(timeAtTriggerCoastUs);
            Mark("COAST_SCHEDULED");
        }
    }

    void ScheduleUpdateSchedule(bool haveGpsLock)
    {
        Mark("UPDATE_SCHEDULE");
        uint64_t timeNowUs;
        uint64_t cycleStartHintUs = GetNextCycleStartHintUs(&timeNowUs);

        // txInterval: skip this cycle entirely (but still consume time so the next
        // schedule cycle aligns to a future window).
        ++skipCounter_;
        if (skipCounter_ < txInterval_)
        {
            Mark("SKIP_CYCLE");
            Log("Skipping cycle (", skipCounter_, "/", txInterval_, ")");

            // Compute when this cycle would end (so we can re-evaluate after it).
            uint8_t notMin, notSec; uint32_t notUs;
            GetNotionalAtSystemUs(cycleStartHintUs, notMin, notSec, notUs);
            auto fires = ComputeCycleFires(slots_, notMin, notSec, notUs);
            uint64_t cycleEndUs = cycleStartHintUs + fires.cycleEndOffsetUs;

            // Arm lockout boundaries only — no warmup, no slot timers.
            timerLockoutStart_.SetCallback([this]{ OnScheduleLockoutStart(); });
            timerLockoutStart_.TimeoutAtUs(cycleStartHintUs);  // immediate-ish lockout
            timerLockoutEnd_.SetCallback([this]{ OnScheduleLockoutEnd(); });
            timerLockoutEnd_.TimeoutAtUs(cycleEndUs);
            return;
        }
        skipCounter_ = 0;

        fnCbScheduleNow_(haveGpsLock);
        PrepareCycleSchedule(timeNowUs, cycleStartHintUs);
    }

    // Cycle start hint = next valid window for slot[0] (or "now" if no slots).
    uint64_t GetNextCycleStartHintUs(uint64_t *timeNowUsRet = nullptr)
    {
        uint64_t timeNowUs = PAL.Micros();
        if (timeNowUsRet) { *timeNowUsRet = timeNowUs; }

        if (slots_.empty()) { return timeNowUs; }

        uint8_t notMin, notSec; uint32_t notUs;
        GetNotionalAtSystemUs(timeNowUs, notMin, notSec, notUs);
        uint64_t deltaUs = ComputeAlignDeltaUs(slots_[0].mode, notMin, notSec, notUs);
        return timeNowUs + deltaUs;
    }

    static void GetNotionalAtSystemUs(uint64_t systemUs, uint8_t &notMin, uint8_t &notSec, uint32_t &notUs)
    {
        auto t = Time::ParseDateTime(Time::GetNotionalDateTimeAtSystemUs(systemUs));
        notMin = t.minute;
        notSec = t.second;
        notUs  = t.us;
    }


    /////////////////////////////////////////////////////////////////
    // Per-cycle scheduling
    /////////////////////////////////////////////////////////////////

    void PrepareCycleSchedule(uint64_t timeNowUs, uint64_t cycleStartHintUs)
    {
        Mark("PREPARE_CYCLE_SCHEDULE_START");

        if (slots_.empty())
        {
            Log("PrepareCycleSchedule: no slots configured; nothing to do");
            return;
        }

        // Compute every slot's fire time + cycle end.
        uint8_t notMin, notSec; uint32_t notUs;
        GetNotionalAtSystemUs(cycleStartHintUs, notMin, notSec, notUs);
        auto fires = ComputeCycleFires(slots_, notMin, notSec, notUs);

        vector<uint64_t> fireAtUs(slots_.size());
        for (size_t i = 0; i < slots_.size(); ++i)
        {
            fireAtUs[i] = cycleStartHintUs + fires.fireOffsetUs[i];
        }
        uint64_t cycleEndUs = cycleStartHintUs + fires.cycleEndOffsetUs;

        Log("Cycle starts at ", TimeAt(fireAtUs[0]), ", ends at ", TimeAt(cycleEndUs));

        const uint64_t WARMUP_US = 30ULL * 1'000'000ULL;

        // Lockout start: a small grace before the first slot, capped by available time.
        const uint64_t LOCKOUT_LEAD_US = 1ULL * 1'000'000ULL;
        uint64_t availPreUs = (fireAtUs[0] > timeNowUs) ? (fireAtUs[0] - timeNowUs) : 0;
        uint64_t lockoutLeadUs = min(LOCKOUT_LEAD_US, availPreUs);
        timerLockoutStart_.SetCallback([this]{ OnScheduleLockoutStart(); });
        timerLockoutStart_.TimeoutAtUs(fireAtUs[0] - lockoutLeadUs);

        // Warmup + fire timers per slot.
        for (size_t i = 0; i < slots_.size(); ++i)
        {
            uint64_t prevEndUs = (i == 0) ? timeNowUs : (fireAtUs[i - 1] + SlotDurationUs(slots_[i - 1]));
            uint64_t availWarmupUs = (fireAtUs[i] > prevEndUs) ? (fireAtUs[i] - prevEndUs) : 0;
            uint64_t warmupUs = min(WARMUP_US, availWarmupUs);
            uint64_t warmupAtUs = fireAtUs[i] - warmupUs;

            timerWarmupVec_[i]->SetCallback([this, i]{
                string s = "WARMUP_" + to_string(i + 1);
                Mark(s.c_str());
                fnCbGoHighSpeed_();
                if (!fnCbRadioIsActive_()) { fnCbStartRadioWarmup_(); }
            });
            timerWarmupVec_[i]->TimeoutAtUs(warmupAtUs);

            // Pre-notify: fires up to 1 s before the slot so the announce message
            // can be flushed to serial before the blocking transmission starts.
            const uint64_t PRENOTIFY_US = 1ULL * 1'000'000ULL;
            uint64_t preNotifyAtUs = fireAtUs[i] - min(PRENOTIFY_US, availWarmupUs);

            Slot slot = slots_[i];
            uint8_t slot1 = (uint8_t)(i + 1);
            timerPreNotifyVec_[i]->SetCallback([this, slot1, slot]{
                fnCbAnnounceSlot_(slot1, slot);
            });
            timerPreNotifyVec_[i]->TimeoutAtUs(preNotifyAtUs);

            timerSlotVec_[i]->SetCallback([this, slot1, slot]{
                string s = "SLOT_" + to_string(slot1) + "_FIRE";
                Mark(s.c_str());
                if (fnCbPrepareSlot_) { fnCbPrepareSlot_(slot1, slot); }
                if (fnCbSendSlot_)    { fnCbSendSlot_(slot1, slot); }
                fnCbStopRadio_();
            });
            timerSlotVec_[i]->TimeoutAtUs(fireAtUs[i]);

            const char *modeName = slot.mode == SlotMode::WSPR15 ? "WSPR-15"
                                 : slot.mode == SlotMode::CW     ? "CW     "
                                 :                                 "WSPR-2 ";
            Log("Slot ", slot1, " (", modeName,
                ", ", slot.band, slot.mode == SlotMode::CW
                    ? string{" @ "} + to_string(slot.frequencyHz) + " Hz"
                    : string{" ch "} + to_string(slot.channel),
                "): warmup ", TimeAt(warmupAtUs), ", fire ", TimeAt(fireAtUs[i]));
        }

        // Lockout end (cycle end → triggers next cycle scheduling)
        timerLockoutEnd_.SetCallback([this]{ OnScheduleLockoutEnd(); });
        timerLockoutEnd_.TimeoutAtUs(cycleEndUs);

        Mark("PREPARE_CYCLE_SCHEDULE_END");

        fnCbOnCycleArmed_(TimeAt(fireAtUs[0]));

        PrintStatus();
        t_.Reset();
    }


    /////////////////////////////////////////////////////////////////
    // Notional time
    /////////////////////////////////////////////////////////////////

    uint64_t MakeUsFromGps(const FixTime &gpsFixTime)
    {
        if (gpsFixTime.year != 0)
        {
            return Time::MakeUsFromDateTime(gpsFixTime.dateTime);
        }
        string dt = Time::MakeDateTime(gpsFixTime.hour,
                                       gpsFixTime.minute,
                                       gpsFixTime.second,
                                       gpsFixTime.millisecond * 1'000);
        return Time::MakeUsFromDateTime(dt);
    }

    void SetNotionalTimeFromGpsTime(const FixTime &gpsFixTime, uint64_t timeAtGpsFixTimeSetUs)
    {
        uint64_t notionalUs = MakeUsFromGps(gpsFixTime);
        Time::SetNotionalUs(notionalUs, timeAtGpsFixTimeSetUs);
        Mark("TIME_SYNC");
        Log("Time sync'd to GPS time: now ", Time::MakeDateTimeFromUs(notionalUs));
    }


    /////////////////////////////////////////////////////////////////
    // Status / utility
    /////////////////////////////////////////////////////////////////

    void CancelAllTimers()
    {
        timerCoast_.Cancel();
        timerLockoutStart_.Cancel();
        timerLockoutEnd_.Cancel();
        for (auto &t : timerWarmupVec_)     { t->Cancel(); }
        for (auto &t : timerPreNotifyVec_)  { t->Cancel(); }
        for (auto &t : timerSlotVec_)       { t->Cancel(); }
    }

    string TimeAt(uint64_t timeUs)
    {
        return Time::GetNotionalTimeAtSystemUs(timeUs);
    }

    void PrintStatus()
    {
        uint64_t timeNowUs = PAL.Micros();
        Log("Scheduler Status (", running_ ? "running" : "stopped",
            inLockout_ ? ", in lockout" : "", ")");
        Log("  Time now: ", Time::GetNotionalDateTimeAtSystemUs(timeNowUs));
    }

    void Mark(const char *str)
    {
        uint64_t timeUs = t_.Event(str);
        Log("[", TimeAt(timeUs), "] ", str);
    }


    /////////////////////////////////////////////////////////////////
    // Shell
    /////////////////////////////////////////////////////////////////

    void SetupShell()
    {
        Shell::AddCommand("start", [this](vector<string>){ Start(); }, { .argCount = 0, .help = "scheduler start"});
        Shell::AddCommand("stop",  [this](vector<string>){ Stop();  }, { .argCount = 0, .help = "scheduler stop"});
        Shell::AddCommand("show",  [this](vector<string>){ PrintStatus(); }, { .argCount = 0, .help = "scheduler status"});

        Shell::AddCommand("lock", [this](vector<string> argList){
            string type = argList[0];
            string dateTime = "2025-01-01 12:09:50.000";
            auto tp = Time::ParseDateTime(dateTime);

            if (type == "gps")
            {
                Fix3DPlus fix;
                fix.timeAtPpsUs = PAL.Micros();
                fix.year = tp.year; fix.hour = tp.hour; fix.minute = tp.minute;
                fix.second = tp.second; fix.millisecond = (uint16_t)(tp.us / 1'000);
                fix.dateTime = dateTime;
                OnGps3DPlusLock(fix);
            }
            else if (type == "time")
            {
                FixTime fix;
                fix.timeAtPpsUs = PAL.Micros();
                fix.year = tp.year; fix.hour = tp.hour; fix.minute = tp.minute;
                fix.second = tp.second; fix.millisecond = (uint16_t)(tp.us / 1'000);
                fix.dateTime = dateTime;
                OnGpsTimeLock(fix);
            }
            else { Log("usage: lock <gps|time>"); }
        }, { .argCount = 1, .help = "fake gps lock <gps|time>"});
    }


    /////////////////////////////////////////////////////////////////
    // State
    /////////////////////////////////////////////////////////////////

    vector<Slot>                   slots_;
    vector<string>                 timerSlotNames_;
    vector<unique_ptr<Timer>>      timerSlotVec_;
    vector<string>                 timerWarmupNames_;
    vector<unique_ptr<Timer>>      timerWarmupVec_;
    vector<string>                 timerPreNotifyNames_;
    vector<unique_ptr<Timer>>      timerPreNotifyVec_;

    Timer timerCoast_         { "TIMER_COAST" };
    Timer timerLockoutStart_  { "TIMER_LOCKOUT_START" };
    Timer timerLockoutEnd_    { "TIMER_LOCKOUT_END" };

    uint8_t txInterval_  = 1;
    uint8_t skipCounter_ = 0;

    function<void()>             fnCbRequestNewGpsLock_       = []{};
    function<void()>             fnCbCancelRequestNewGpsLock_ = []{};
    function<void(bool)>         fnCbScheduleNow_             = [](bool){};
    function<void()>             fnCbFirstCycleEnd_           = []{};
    function<bool()>             fnCbRadioIsActive_           = []{ return false; };
    function<void()>             fnCbStartRadioWarmup_        = []{};
    function<void()>             fnCbStopRadio_               = []{};
    function<void()>             fnCbGoHighSpeed_             = []{};
    function<void()>             fnCbGoLowSpeed_              = []{};
    function<void(uint8_t, const Slot &)> fnCbPrepareSlot_    = [](uint8_t, const Slot &){};
    function<void(uint8_t, const Slot &)> fnCbSendSlot_       = [](uint8_t, const Slot &){};
    function<void(uint8_t, const Slot &)> fnCbAnnounceSlot_   = [](uint8_t, const Slot &){};
    function<void(const string &)>        fnCbOnCycleArmed_   = [](const string &){};

    Timeline t_;
};
