#pragma once

#include <algorithm>
using namespace std;

#include "ADCInternal.h"
#include "Blinker.h"
#include "JSONMsgRouter.h"
#include "CwEncoder.h"
#include "Scheduler.h"
#include "SubsystemGps.h"
#include "SubsystemTx.h"
#include "TimeClass.h"
#include "TempSensorInternal.h"
#include "USB.h"


struct TestConfiguration
{
    bool enabled = false;

    bool fastStartEvmOnly = false;

    bool watchdogOn = true;
    bool logAsync = true;
    bool evmOnly = false;
    bool apiMode = false;
};

TestConfiguration testCfg;



class Application
{
public:

    Application()
    {
        if (testCfg.enabled && testCfg.fastStartEvmOnly)
        {
            // do nothing
        }
        else
        {
            PowerSave();
        }

        I2C::Init1();
        I2C::SetupShell1();

        USB::SetStringManufacturer("Traquito");
        USB::SetStringProduct("Jetpack");
        USB::SetStringCdcInterface("Traquito Jetpack");
        USB::SetStringVendorInterface("Traquito Jetpack");
        USB::SetVid(0x2FE3);
        USB::SetPid(0x0008);
    }

    /////////////////////////////////////////////////////////////////
    // Program start - Decide Configuration or Beacon Mode
    /////////////////////////////////////////////////////////////////

    void Run()
    {
        if (testCfg.enabled && testCfg.fastStartEvmOnly)
        {
            SetupShell();
            SetupJSON();
            Log("Main Loop Only");
            return;
        }

        Timeline::Global().Event("Application");

        LogNL(2);
        Log("Module Details");
        Log("Software: ", Version::GetVersionShort());
        LogNL();

        if (testCfg.enabled == false || (testCfg.enabled && testCfg.watchdogOn == true))
        {
            Watchdog::SetTimeout(5'000);
            Watchdog::Start();
            Log("Watchdog enabled");
            LogNL();

            timerWatchdog_.SetName("TIMER_WATCHDOG_FEED");
            timerWatchdog_.SetCallback([]{ Watchdog::Feed(); });
            timerWatchdog_.TimeoutIntervalMs(2'000, 0);
        }

        blinker_.SetPin(pinLedGreen_);

        PowerTest();

        SetupShell();
        SetupJSON();

        if (testCfg.enabled && testCfg.logAsync == false)
        {
            Evm::DisableAutoLogAsync();
            Log("Async logging disabled");
        }

        ssTx_.SetCallbackOnTxStart([this]{
            Watchdog::Feed();
            blinker_.On();
        });
        ssTx_.SetCallbackOnBitChange([this]{
            Watchdog::Feed();
            blinker_.Toggle();
        });
        ssTx_.SetCallbackOnTxEnd([this]{
            Watchdog::Feed();
            BlinkerIdle();
        });

        configurationMode_ = false;
        if (testCfg.enabled == false)
        {
            USB::SetCallbackConnected([&]{
                Log("USB CONNECTED");
                configurationMode_ = true;
            });
            USB::SetCallbackDisconnected([]{
                LogModeSync();
                Log("USB DISCONNECTED");
                LogModeAsync();
                PAL.Reset();
            });
        }

        LogNL();
        Log("Determining startup mode");
        LogNL();
        timerStartupRole_.SetName("TIMER_STARTUP_ROLE");
        timerStartupRole_.SetCallback([this]{ EnableMode(); });
        timerStartupRole_.TimeoutInMs(1'000);
    }

    void EnableMode()
    {
        if (testCfg.enabled && testCfg.evmOnly)
        {
            BlinkerIdle();
            LogNL(2);
            Log("Main Loop Only");
            return;
        }

        bool cfgModeEnableBlink = true;
        if (testCfg.enabled)
        {
            if (testCfg.apiMode == true)
            {
                Log("API Mode Enabled");
                configurationMode_ = true;
                cfgModeEnableBlink = false;
            }
            else
            {
                configurationMode_ = false;
            }
        }

        if (configurationMode_)
        {
            ConfigurationMode(cfgModeEnableBlink);
        }
        else
        {
            BeaconMode();
        }

        if (testCfg.enabled == false ||
           (testCfg.enabled && testCfg.apiMode == false))
        {
            USB::SetCallbackConnected([&]{
                LogModeSync();
                Log("USB Connected");
                LogModeAsync();
                PAL.Reset();
            });
        }
    }

    /////////////////////////////////////////////////////////////////
    // Configuration Mode
    /////////////////////////////////////////////////////////////////

    void ConfigurationMode(bool enableBlink = true)
    {
        Log("Configuration Mode");

        ssGps_.EnableConfigurationMode();

        static Timer timerTemp("APP_TEMP_TIMER");
        timerTemp.SetCallback([this]{
            router_.Send([&](const auto &out){
                out["type"] = "TEMP";
                out["tempC"] = tempSensor_.GetTempC();
                out["tempF"] = tempSensor_.GetTempF();
            });
        });
        timerTemp.TimeoutIntervalMs(30000, 0);

        if (enableBlink) { BlinkerIdle(); }

        auto TryStartScheduler = [this]{
            if (!schedulerStarted_ && ssTx_.ReadyToBeacon())
            {
                schedulerStarted_ = true;
                Log("Config mode: callsign+slots valid, starting scheduler");
                SetupScheduler();
            }
        };
        TryStartScheduler();

        JSONMsgRouter::RegisterHandler("REQ_SET_CONFIG", [TryStartScheduler](auto &, auto &){
            TryStartScheduler();
        });
    }


    /////////////////////////////////////////////////////////////////
    // Beacon Mode (autonomous)
    /////////////////////////////////////////////////////////////////

    void BeaconMode()
    {
        LogNL();
        LogNL();
        Log("Beacon Mode");
        LogNL();

        if (testCfg.enabled)
        {
            Configuration &txCfg = ssTx_.GetConfiguration();
            txCfg.callsign = "KD3KDD";
            txCfg.correction = 0;
            txCfg.grid = "FN20";
            txCfg.txInterval = 1;
            txCfg.slots = { Configuration::Slot{ SlotMode::WSPR2, "20m", 414, 0, 18 } };
            txCfg.Put();
            txCfg.Get();
        }

        if (ssTx_.ReadyToBeacon() == false)
        {
            Log("ERR: ==== NOT READY - FATAL ====");

            while (true)
            {
                BlinkerBlinkOncePanic();
            }
        }
        else
        {
            const Configuration &txCfg = ssTx_.GetConfiguration();

            Log("==== Ok to beacon! ====");
            Log("Callsign  : ", txCfg.callsign);
            Log("Grid      : ", txCfg.grid);
            Log("Correction: ", txCfg.correction);
            Log("TxInterval: ", txCfg.txInterval);
            Log("Slots     : ", txCfg.slots.size());
            for (size_t i = 0; i < txCfg.slots.size(); ++i)
            {
                const auto &s = txCfg.slots[i];
                if (s.mode == SlotMode::CW)
                {
                    Log("  [", i + 1, "] CW      ",
                        s.band, " @ ", Commas(s.frequencyHz), " Hz, ", (int)s.wpm, " WPM");
                }
                else
                {
                    auto cd = WsprChannelMap::GetChannelDetails(s.band.c_str(), s.channel);
                    Log("  [", i + 1, "] ",
                        s.mode == SlotMode::WSPR15 ? "WSPR-15" : "WSPR-2 ",
                        " ", s.band, " ch ", s.channel, " freq ", Commas(cd.freq));
                }
            }
            LogNL();

            blinker_.Blink(4, 100, 100);
            Watchdog::Feed();
            PAL.Delay(1'500);
            Watchdog::Feed();

            SetupScheduler();
        }
    }


    /////////////////////////////////////////////////////////////////
    // Scheduler Integration
    /////////////////////////////////////////////////////////////////

    // Build the standard beacon CW message for a station with the given
    // callsign + grid: "VVV DE {call}/B {grid} {call}/B {grid} {call}/B {grid}".
    // Grid is used up to 6 chars (CW beacons traditionally include the 6-char locator).
    static string BuildCwBeaconMessage(const string &callsign, const string &grid)
    {
        string g = grid.substr(0, min((size_t)6, grid.size()));
        string callPart = callsign + "/B";
        string out = "VVV DE";
        for (int i = 0; i < 3; ++i)
        {
            out += " " + callPart + " " + g;
        }
        return out;
    }

    void SetupScheduler()
    {
        // Push slot list into the scheduler from current config.
        const Configuration &txCfg = ssTx_.GetConfiguration();
        vector<Scheduler::Slot> slots;
        slots.reserve(txCfg.slots.size());
        for (const auto &s : txCfg.slots)
        {
            Scheduler::Slot ss;
            ss.mode        = s.mode;
            ss.band        = s.band;
            ss.channel     = s.channel;
            ss.frequencyHz = s.frequencyHz;
            ss.wpm         = s.wpm;
            if (s.mode == SlotMode::CW)
            {
                ss.cwText       = BuildCwBeaconMessage(txCfg.callsign, txCfg.grid);
                ss.cwDurationMs = CwEncoder::EstimateDurationMs(ss.cwText, s.wpm);
            }
            slots.push_back(ss);
        }
        scheduler_.SetSlots(slots);
        scheduler_.SetTxInterval(txCfg.txInterval);

        SetupSchedulerGps();
        SetupSchedulerRadio();
        SetupSchedulerClockSpeed();
        SetupSchedulerSlotDispatch();

        scheduler_.SetCallbackOnCycleArmed([this](const string &slot1TimeUtc){
            router_.Send([&](auto &out){
                out["type"]         = "SCHED_CYCLE_ARMED";
                out["slot1FireUtc"] = slot1TimeUtc.c_str();
            });
        });

        scheduler_.Start();
    }

    void SetupSchedulerGps()
    {
        Shell::AddCommand("now", [this](vector<string>){
            fix3dPlus_ = GPSReader::GetFix3DPlusExample();
            fix3dPlus_.timeAtPpsUs = PAL.Micros();
            fix3dPlus_.minute = 5;
            fix3dPlus_.second = 55;
            fix3dPlus_.dateTime = GPSReader::MakeDateTimeFromFixTime(fix3dPlus_);
            scheduler_.OnGps3DPlusLock(fix3dPlus_);
        }, { .argCount = 0, .help = "trigger 3d lock"});

        scheduler_.SetCallbackRequestNewGpsLock([this]{
            BlinkerGpsSearch();

            t_.Reset();
            t_.SetMaxEvents(50);
            t_.Event("GPS_REQUESTED");

            if (testCfg.enabled == false)
            {
                ssGps_.DisableVerboseLogging();
            }
            ssGps_.EnableBeaconMode();
            t_.Event("GpsEnabled");

            Log("Requesting FixTime and Fix3DPlus");

            auto FnOnFixTime = [this](const FixTime &fixTime){
                t_.Event("FixTime");

                Log("GPS: time fix acquired (UTC sync OK), cancelling lock-or-die timer");
                CancelGpsLockOrDieTimer();

                lastGpsTime_ =
                    StrUtl::PadLeft(fixTime.hour,   '0', 2) + ":" +
                    StrUtl::PadLeft(fixTime.minute, '0', 2) + ":" +
                    StrUtl::PadLeft(fixTime.second, '0', 2);

                router_.Send([&](auto &out){
                    out["type"] = "GPS_FIX_TIME";
                    out["time"] = lastGpsTime_.c_str();
                });

                scheduler_.OnGpsTimeLock(fixTime);

                router_.Send([&](auto &out){
                    out["type"] = "SCHED_GPS_APPLIED";
                    out["time"] = lastGpsTime_.c_str();
                });
            };

            auto FnOnFix3DPlus = [this](const Fix3DPlus &fix3dPlus){
                t_.Event("Fix3DPlus");

                Log("GPS: 3D fix acquired, grid=", fix3dPlus.maidenheadGrid);

                lastGpsTime_ =
                    StrUtl::PadLeft(fix3dPlus.hour,   '0', 2) + ":" +
                    StrUtl::PadLeft(fix3dPlus.minute, '0', 2) + ":" +
                    StrUtl::PadLeft(fix3dPlus.second, '0', 2);

                router_.Send([&](auto &out){
                    out["type"]   = "GPS_FIX_2D";
                    out["latDeg"] = fix3dPlus.latDegMillionths / 1'000'000.0;
                    out["lngDeg"] = fix3dPlus.lngDegMillionths / 1'000'000.0;
                });
                router_.Send([&](auto &out){
                    out["type"]  = "GPS_FIX_3D";
                    out["altM"]  = fix3dPlus.altitudeM;
                });

                fix3dPlus_ = fix3dPlus;
                gotFix3dPlus_ = true;

                scheduler_.OnGps3DPlusLock(fix3dPlus_);
            };

            ssGps_.RequestNewFixTimeAnd3DPlus(FnOnFixTime, FnOnFix3DPlus);
            t_.Event("FixRequested");

            StartGpsLockOrDieTimer();
        });

        scheduler_.SetCallbackFirstCycleEnd([this]{
            Log("GPS: first cycle ended — 3D fix search window expired");
            scheduler_.CancelRequestNewGpsLock();
        });

        scheduler_.SetCallbackCancelRequestNewGpsLock([this]{
            t_.Event("CancelReqNewGpsLock");
            BlinkerIdle();
            ssGps_.Disable();

            if (gotFix3dPlus_)
            {
                Log("GPS: off — 3D fix was obtained, grid=", fix3dPlus_.maidenheadGrid);
            }
            else
            {
                Log("GPS: off — no 3D fix obtained");
            }
        });
    }

    void SetupSchedulerSlotDispatch()
    {
        scheduler_.SetCallbackScheduleNow([this](bool){
            // No per-slot dynamic decisions required in beacon mode —
            // slot dispatch is fully driven by SetCallbackPrepareSlot/SendSlot.
        });

        // Fires ~1 s before the slot so TX_START reaches the serial console
        // before the blocking transmission begins.
        scheduler_.SetCallbackAnnounceSlot([this](uint8_t slot1, const Scheduler::Slot &s){
            const Configuration &txCfg = ssTx_.GetConfiguration();
            const char *proto = s.mode == SlotMode::WSPR15 ? "WSPR-15"
                              : s.mode == SlotMode::CW     ? "CW"
                              :                              "WSPR-2";
            router_.Send([&](auto &out){
                out["type"]     = "TX_START";
                out["slot"]     = slot1;
                out["protocol"] = proto;
                out["band"]     = s.band.c_str();
                if (s.mode == SlotMode::CW)
                {
                    out["freqHz"] = s.frequencyHz;
                    out["wpm"]    = s.wpm;
                }
                else
                {
                    out["channel"]  = s.channel;
                    out["callsign"] = txCfg.callsign.c_str();
                    string grid4 = txCfg.grid.size() >= 4 ? txCfg.grid.substr(0, 4) : "";
                    out["grid"]     = grid4.c_str();
                    out["gps"]      = gotFix3dPlus_;
                    out["gpsTime"]  = lastGpsTime_.c_str();
                }
            });
        });

        scheduler_.SetCallbackPrepareSlot([this](uint8_t slot1, const Scheduler::Slot &s){
            if (s.mode == SlotMode::CW)
            {
                Log("Slot ", slot1, " prepare: CW ", s.band,
                    " @ ", Commas(s.frequencyHz), " Hz, ", (int)s.wpm, " WPM");
                ssTx_.SetupForCw(s.frequencyHz);
            }
            else
            {
                const char *modeName = s.mode == SlotMode::WSPR15 ? "WSPR-15" : "WSPR-2 ";
                Log("Slot ", slot1, " prepare: ", modeName,
                    " ", s.band, " ch ", s.channel);
                ssTx_.SetWsprMode(s.mode);
                ssTx_.SetupForSlot(s.band, s.channel);
            }
        });

        scheduler_.SetCallbackSendSlot([this](uint8_t /*slot1*/, const Scheduler::Slot &s){
            if (s.mode == SlotMode::CW)
            {
                SendCw(s.cwText, s.wpm);
            }
            else
            {
                SendRegularType1();
            }
            const char *proto = s.mode == SlotMode::WSPR15 ? "WSPR-15"
                              : s.mode == SlotMode::CW     ? "CW"
                              :                              "WSPR-2";
            router_.Send([&](auto &out){
                out["type"]     = "TX_DONE";
                out["protocol"] = proto;
                out["band"]     = s.band.c_str();
            });
        });
    }

    void SetupSchedulerRadio()
    {
        scheduler_.SetCallbackRadioIsActive([this]{ return ssTx_.IsOn(); });

        scheduler_.SetCallbackStartRadioWarmup([this]{
            ssTx_.Enable();
            ssTx_.RadioOn();
            BlinkerTransmit();
        });

        scheduler_.SetCallbackStopRadio([this]{
            ssTx_.RadioOff();
            ssTx_.Disable();
        });
    }

    void SetupSchedulerClockSpeed()
    {
        scheduler_.SetCallbackGoHighSpeed([this]{ Clock::SetClockMHz(48); });
        scheduler_.SetCallbackGoLowSpeed( [this]{ Clock::SetClockMHz(6);  });
    }


    /////////////////////////////////////////////////////////////////
    // Message Sending
    /////////////////////////////////////////////////////////////////

    void SendRegularType1()
    {
        const Configuration &txCfg = ssTx_.GetConfiguration();
        static const uint8_t POWER_DBM = 13;
        string grid4 = txCfg.grid.size() >= 4 ? txCfg.grid.substr(0, 4) : "";
        ssTx_.SendRegularMessage(txCfg.callsign, grid4, POWER_DBM);
    }

    void SendCw(const string &text, uint8_t wpm)
    {
        ssTx_.SendCwMessage(text, wpm);
    }


    /////////////////////////////////////////////////////////////////
    // GPS startup timers
    /////////////////////////////////////////////////////////////////

    void StartGpsLockOrDieTimer()
    {
        static const uint32_t TWENTY_MINUTES_MS = 20 * 60 * 1'000;

        timerGpsLockOrDie_.SetName("TIMER_GPS_LOCK_OR_DIE");
        timerGpsLockOrDie_.SetCallback([this]{
            LogModeSync();
            LogNL();
            Log("GPS: no time fix within 20 minutes — hard resetting GPS and rebooting");
            ssGps_.ModuleHardReset();
            while (true) { BlinkerBlinkOncePanic(); }
        });
        timerGpsLockOrDie_.TimeoutInMs(TWENTY_MINUTES_MS);
    }

    void CancelGpsLockOrDieTimer() { timerGpsLockOrDie_.Cancel(); }


    /////////////////////////////////////////////////////////////////
    // Blinker states
    /////////////////////////////////////////////////////////////////

    inline static const uint32_t WSPR_BIT_DURATION_MS = 683;

    void BlinkerIdle()
    {
        blinker_.SetBlinkOnOffTime(75, 4925);
        blinker_.EnableAsyncBlink();
    }

    void BlinkerGpsSearch()
    {
        blinker_.SetBlinkOnOffTime(75, 925);
        blinker_.EnableAsyncBlink();
    }

    void BlinkerTransmit()
    {
        blinker_.SetBlinkOnOffTime(WSPR_BIT_DURATION_MS, WSPR_BIT_DURATION_MS);
        blinker_.EnableAsyncBlink();
    }

    void BlinkerBlinkOncePanic() { blinker_.Blink(1, 40, 40); }


private:

    /////////////////////////////////////////////////////////////////
    // Power
    /////////////////////////////////////////////////////////////////

    void PowerSave()
    {
        Log("Power saving processing");

        Log("Prepare 48MHz clock speed");
        Clock::PrepareClockMHz(48);

        Log("Drop to 6MHz clock speed");
        Clock::SetClockMHz(6);
        LogNL();

        if (testCfg.enabled)
        {
            Clock::PrintAll();
            LogNL();
        }

        Log("Disable unused peripherals");
        PeripheralControl::DisablePeripheralList({
            PeripheralControl::SPI1,
            PeripheralControl::SPI0,
            PeripheralControl::PWM,
            PeripheralControl::PIO1,
            PeripheralControl::PIO0,
        });

        USB::SetCallbackVbusConnected([]{
            Log("App VBUS HIGH handler, switching to 48MHz");
            Clock::SetClockMHz(48);
        });
        USB::SetCallbackVbusDisconnected([]{
            Log("App VBUS LOW handler, switching to 6MHz");
            Clock::SetClockMHz(6);
        });
        USB::EnablePowerSaveMode();
        LogNL();
    }

    void PowerTest()
    {
        Log("Startup Power Test");

        PAL.Delay(1'000);
        blinker_.Blink(1, 500, 100);
        Watchdog::Feed();

        ssGps_.ModulePowerOnBatteryOn();
        PAL.Delay(500);
        blinker_.Blink(1, 500, 100);
        ssGps_.ModulePowerOff();
        Watchdog::Feed();

        ssTx_.Enable();
        ssTx_.RadioOn();
        PAL.Delay(1'000);
        blinker_.Blink(1, 500, 100);
        ssTx_.RadioOff();
        ssTx_.Disable();
        Watchdog::Feed();

        Log("Power test blinking sequence complete");
    }


    /////////////////////////////////////////////////////////////////
    // Startup
    /////////////////////////////////////////////////////////////////

    void SetupShell()
    {
        Shell::AddCommand("app.test.led.green.on",  [this](vector<string>){ pinLedGreen_.DigitalWrite(1); }, { .argCount = 0, .help = ""});
        Shell::AddCommand("app.test.led.green.off", [this](vector<string>){ pinLedGreen_.DigitalWrite(0); }, { .argCount = 0, .help = ""});

        static uint32_t count = 0;
        static bool show = false;
        UartAddLineStreamCallback(UART::UART_1, [](const string &line){
            UartTarget target(UART::UART_0);
            if (show) { Log(line); }
            ++count;
        });

        Shell::AddCommand("app.count", [](vector<string>){ Log(count); }, { .argCount = 0, .help = ""});
        Shell::AddCommand("app.show",  [](vector<string>){ show = !show; }, { .argCount = 0, .help = ""});
    }

    void SetupJSON()
    {
        UartAddLineStreamCallback(UART::UART_USB, [this](const string &line){
            router_.Route(line);
        });

        router_.SetOnReceiveCallback([this](const string &jsonStr){
            UartTarget target(UART::UART_USB);
            Log(jsonStr);
        });

        JSONMsgRouter::RegisterHandler("REQ_GET_DEVICE_INFO", [this](auto &, auto &out){
            out["type"] = "REP_GET_DEVICE_INFO";

            out["swVersion"] = Split(Version::GetVersion())[0];
            if (testCfg.enabled && testCfg.apiMode) { out["mode"] = "API";    }
            else if (configurationMode_)             { out["mode"] = "CONFIG"; }
            else                                     { out["mode"] = "BEACON"; }
        });
    }


private:

    bool configurationMode_ = false;
    bool schedulerStarted_  = false;

    Pin pinLedGreen_ = { 25 };

    Scheduler    scheduler_;
    SubsystemGps ssGps_;
    SubsystemTx  ssTx_;

    Fix3DPlus fix3dPlus_;
    bool gotFix3dPlus_ = false;
    string lastGpsTime_;

    JSONMsgRouter::Iface router_;

    Timer timerStartupRole_;
    Timer timerWatchdog_;
    Timer timerGpsLockOrDie_;

    Blinker blinker_;

    Timeline t_;

    TempSensorInternal tempSensor_;
};
