#pragma once

#include <array>
#include <string>
#include <vector>
using namespace std;

#include "Flashable.h"
#include "JSONMsgRouter.h"
#include "Scheduler.h"
#include "WsprEncodedDynamic.h"


class Configuration
{
public:

    static constexpr uint8_t SLOT_MAX = Scheduler::SLOT_MAX;

    struct Slot
    {
        SlotMode mode        = SlotMode::WSPR2;
        string   band        = "";
        uint16_t channel     = 0;        // WSPR only
        uint32_t frequencyHz = 0;        // CW only
        uint8_t  wpm         = 18;       // CW only
        uint32_t idleMinutes = 0;        // IDLE only
    };

private:

    struct ConfigurationFlashState
    {
        array<char, 6 + 1> callsignStorage;
        int32_t            correction;
        array<char, 6 + 1> gridStorage;     // 6-char Maidenhead (CW beacon uses 6, WSPR uses first 4)
        uint8_t            txInterval;

        struct SlotEntry
        {
            uint8_t            mode;            // 0=WSPR2, 1=WSPR15, 2=CW, 3=IDLE
            array<char, 5 + 1> bandStorage;
            uint16_t           channel;         // WSPR only
            uint32_t           frequencyHz;     // CW only
            uint8_t            wpm;             // CW only
            uint32_t           idleMinutes;     // IDLE only
        };

        array<SlotEntry, SLOT_MAX> slots;
        uint8_t                    slotCount;
    };

    Flashable<ConfigurationFlashState> flashState_;

    void Reset()
    {
        flashState_.callsignStorage.fill(0);
        callsign = "";

        flashState_.correction = 0;
        correction = 0;

        flashState_.gridStorage.fill(0);
        grid = "";

        flashState_.txInterval = 1;
        txInterval = 1;

        for (uint8_t i = 0; i < SLOT_MAX; ++i)
        {
            flashState_.slots[i].mode = 0;
            flashState_.slots[i].bandStorage.fill(0);
            flashState_.slots[i].channel = 0;
            flashState_.slots[i].frequencyHz = 0;
            flashState_.slots[i].wpm = 18;
            flashState_.slots[i].idleMinutes = 0;
        }
        flashState_.slotCount = 0;
        slots.clear();
    }


public:

    Configuration()
    {
        Reset();

        bool cfgOk = Get();

        LogNL();
        if (cfgOk == false)
        {
            Log("INF: No saved configuration");
        }
        else
        {
            Log("INF: Restoring saved configuration");
        }

        SetupShell();
        SetupJSON();
    }

    bool Get()
    {
        bool retVal = flashState_.Get(true);

        if (retVal)
        {
            callsign   = (const char *)flashState_.callsignStorage.data();
            correction = flashState_.correction;
            grid       = (const char *)flashState_.gridStorage.data();
            txInterval = flashState_.txInterval < 1 ? 1 : flashState_.txInterval;

            uint8_t n = flashState_.slotCount;
            if (n > SLOT_MAX) { n = SLOT_MAX; }
            slots.clear();
            slots.reserve(n);
            for (uint8_t i = 0; i < n; ++i)
            {
                Slot s;
                uint8_t m = flashState_.slots[i].mode;
                s.mode        = (m == 3) ? SlotMode::IDLE
                              : (m == 2) ? SlotMode::CW
                              : (m == 1) ? SlotMode::WSPR15
                              :            SlotMode::WSPR2;
                s.band        = (const char *)flashState_.slots[i].bandStorage.data();
                s.channel     = flashState_.slots[i].channel;
                s.frequencyHz = flashState_.slots[i].frequencyHz;
                s.wpm         = flashState_.slots[i].wpm == 0 ? 18 : flashState_.slots[i].wpm;
                s.idleMinutes = flashState_.slots[i].idleMinutes;
                slots.push_back(s);
            }
        }

        return retVal;
    }

    bool Put()
    {
        // Cap copies at size()-1 so the arrays always stay NUL-terminated —
        // Get() reads them back as C strings.
        flashState_.callsignStorage.fill(0);
        callsign.copy(flashState_.callsignStorage.data(), min(callsign.size(), flashState_.callsignStorage.size() - 1));

        flashState_.correction = correction;

        flashState_.gridStorage.fill(0);
        grid.copy(flashState_.gridStorage.data(), min(grid.size(), flashState_.gridStorage.size() - 1));

        flashState_.txInterval = txInterval;

        uint8_t n = (uint8_t)slots.size();
        if (n > SLOT_MAX) { n = SLOT_MAX; }
        for (uint8_t i = 0; i < SLOT_MAX; ++i)
        {
            flashState_.slots[i].bandStorage.fill(0);
            if (i < n)
            {
                flashState_.slots[i].mode = (slots[i].mode == SlotMode::IDLE)   ? 3
                                          : (slots[i].mode == SlotMode::CW)     ? 2
                                          : (slots[i].mode == SlotMode::WSPR15) ? 1
                                          :                                       0;
                slots[i].band.copy(flashState_.slots[i].bandStorage.data(),
                                   min(slots[i].band.size(), flashState_.slots[i].bandStorage.size() - 1));
                flashState_.slots[i].channel     = slots[i].channel;
                flashState_.slots[i].frequencyHz = slots[i].frequencyHz;
                flashState_.slots[i].wpm         = slots[i].wpm;
                flashState_.slots[i].idleMinutes = slots[i].idleMinutes;
            }
            else
            {
                flashState_.slots[i].mode        = 0;
                flashState_.slots[i].channel     = 0;
                flashState_.slots[i].frequencyHz = 0;
                flashState_.slots[i].wpm         = 18;
                flashState_.slots[i].idleMinutes = 0;
            }
        }
        flashState_.slotCount = n;

        return flashState_.Put();
    }


public:

    // 4- or 6-char Maidenhead locator, uppercase:
    // [A-R][A-R][0-9][0-9] plus optional [A-X][A-X] subsquare.
    // Matches WsprMessageRegularType1::Grid4IsValid for the first 4 chars, so
    // a grid accepted here is guaranteed to be accepted by the WSPR encoder
    // (which otherwise silently substitutes "AA00").
    static bool GridIsValid(const string &grid)
    {
        if (grid.size() != 4 && grid.size() != 6) { return false; }

        if (grid[0] < 'A' || grid[0] > 'R') { return false; }
        if (grid[1] < 'A' || grid[1] > 'R') { return false; }
        if (grid[2] < '0' || grid[2] > '9') { return false; }
        if (grid[3] < '0' || grid[3] > '9') { return false; }

        if (grid.size() == 6)
        {
            if (grid[4] < 'A' || grid[4] > 'X') { return false; }
            if (grid[5] < 'A' || grid[5] > 'X') { return false; }
        }

        return true;
    }


private:

    void SetupShell()
    {
        Shell::AddCommand("app.cfg.del", [this](vector<string>){
            flashState_.Delete();
            Reset();
            Log("Configuration Deleted, state reset");
        }, { .argCount = 0, .help = "delete config"});
    }

    void SetupJSON()
    {
        JSONMsgRouter::RegisterHandler("REQ_GET_CONFIG", [this](auto &, auto &out){
            out["type"] = "REP_GET_CONFIG";

            out["callsign"]   = callsign;
            out["correction"] = correction;
            out["grid"]       = grid;
            out["txInterval"] = txInterval;

            out["callsignOk"] = WsprMessageRegularType1::CallsignIsValid(callsign.c_str());
            out["gridOk"]     = GridIsValid(grid);

            JsonArray slotsArr = out.createNestedArray("slots");
            for (const auto &s : slots)
            {
                JsonObject o = slotsArr.createNestedObject();
                o["mode"]    = (int)s.mode;
                o["band"]    = s.band;
                o["channel"] = s.channel;
                o["frequencyHz"] = s.frequencyHz;
                o["wpm"]     = s.wpm;
                o["idleMinutes"] = s.idleMinutes;
            }
        });

        JSONMsgRouter::RegisterHandler("REQ_DELETE_CONFIG", [this](auto &, auto &out){
            out["type"] = "REP_DELETE_CONFIG";
            flashState_.Delete();
            Reset();
            out["ok"] = true;
        });

        JSONMsgRouter::RegisterHandler("REQ_SET_CONFIG", [this](auto &in, auto &out){
            out["type"] = "REP_SET_CONFIG";

            string  callsignIn   = (const char *)in["callsign"];
            int32_t correctionIn = (int32_t)in["correction"];
            string  gridIn       = in.containsKey("grid") ? (const char *)in["grid"] : "";
            for (char &c : gridIn)
            {
                if (c >= 'a' && c <= 'z') { c = (char)(c - 'a' + 'A'); }
            }
            uint8_t txIntervalIn = in.containsKey("txInterval") ? (uint8_t)(int)in["txInterval"] : 1;
            if (txIntervalIn < 1) txIntervalIn = 1;

            vector<Slot> slotsIn;
            if (in.containsKey("slots"))
            {
                JsonArrayConst arr = in["slots"];
                for (size_t i = 0; i < arr.size() && slotsIn.size() < SLOT_MAX; ++i)
                {
                    JsonObjectConst o = arr[i];
                    Slot s;
                    int modeRaw = (int)o["mode"];
                    s.mode        = (modeRaw == 3) ? SlotMode::IDLE
                                  : (modeRaw == 2) ? SlotMode::CW
                                  : (modeRaw == 1) ? SlotMode::WSPR15
                                  :                  SlotMode::WSPR2;
                    s.band        = (const char *)o["band"];
                    s.channel     = (uint16_t)(int)o["channel"];
                    s.frequencyHz = o.containsKey("frequencyHz") ? (uint32_t)(uint64_t)o["frequencyHz"] : 0;
                    s.wpm         = o.containsKey("wpm")         ? (uint8_t)(int)o["wpm"]              : 18;
                    if (s.wpm == 0) s.wpm = 18;
                    s.idleMinutes = o.containsKey("idleMinutes")  ? (uint32_t)(int)o["idleMinutes"]     : 0;
                    slotsIn.push_back(s);
                }
            }

            bool   ok  = true;
            string err = "";
            string sep = "";

            if (WsprMessageRegularType1::CallsignIsValid(callsignIn.c_str()) == false)
            {
                ok = false;
                err += sep + "Invalid callsign";
                sep = ", ";
            }

            if (GridIsValid(gridIn) == false)
            {
                ok = false;
                err += sep + "Invalid grid (need 4 or 6 char Maidenhead, e.g. IO85 or IO85XW)";
                sep = ", ";
            }

            for (size_t i = 0; i < slotsIn.size(); ++i)
            {
                const Slot &s = slotsIn[i];

                // IDLE slots don't touch the radio - band/channel/frequency/wpm
                // are irrelevant. Only the wait duration needs validating.
                if (s.mode == SlotMode::IDLE)
                {
                    if (s.idleMinutes < 1 || s.idleMinutes > 10'080)  // 1 min .. 1 week
                    {
                        ok = false;
                        err += sep + "Invalid duration for IDLE slot " + to_string(i + 1)
                             + " (expected 1..10080 minutes)";
                        sep = ", ";
                    }
                    continue;
                }

                if (s.band.empty() ||
                    s.band != Wspr::GetDefaultBandIfNotValid(s.band.c_str()))
                {
                    ok = false;
                    err += sep + "Invalid band for slot " + to_string(i + 1);
                    sep = ", ";
                }
                // CW slots use frequencyHz directly; WSPR slots use band+channel.
                if (s.mode == SlotMode::CW)
                {
                    // 1 MHz .. 30 MHz sanity window (HF beacon use)
                    if (s.frequencyHz < 1'000'000u || s.frequencyHz > 30'000'000u)
                    {
                        ok = false;
                        err += sep + "Invalid frequency for CW slot " + to_string(i + 1)
                             + " (expected 1..30 MHz)";
                        sep = ", ";
                    }
                    if (s.wpm < 5 || s.wpm > 40)
                    {
                        ok = false;
                        err += sep + "Invalid WPM for CW slot " + to_string(i + 1)
                             + " (expected 5..40)";
                        sep = ", ";
                    }
                }
                else
                {
                    if (s.channel != WsprChannelMap::GetDefaultChannelIfNotValid((int16_t)s.channel))
                    {
                        ok = false;
                        err += sep + "Invalid channel for slot " + to_string(i + 1);
                        sep = ", ";
                    }
                }
            }

            if (ok)
            {
                callsign   = callsignIn;
                correction = correctionIn;
                grid       = gridIn;
                txInterval = txIntervalIn;
                slots      = slotsIn;

                ok = Put();
                if (!ok)
                {
                    err += sep + "Could not store to flash";
                    sep = ", ";
                }
            }

            Log("REQ_SET_CONFIG: callsign=", callsignIn, " correction=", correctionIn,
                " grid=", gridIn, " txInterval=", (int)txIntervalIn,
                " slots=", (int)slotsIn.size());
            Log("OK: ", ok, ", err: \"", err, "\"");

            out["ok"]  = ok;
            out["err"] = err;
        });
    }


public:

    string   callsign;
    int32_t  correction;
    string   grid;
    uint8_t  txInterval;
    vector<Slot> slots;
};
