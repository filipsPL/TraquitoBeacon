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
            uint8_t            mode;            // 0=WSPR2, 1=WSPR15, 2=CW
            array<char, 5 + 1> bandStorage;
            uint16_t           channel;         // WSPR only
            uint32_t           frequencyHz;     // CW only
            uint8_t            wpm;             // CW only
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
                s.mode        = (m == 2) ? SlotMode::CW
                              : (m == 1) ? SlotMode::WSPR15
                              :            SlotMode::WSPR2;
                s.band        = (const char *)flashState_.slots[i].bandStorage.data();
                s.channel     = flashState_.slots[i].channel;
                s.frequencyHz = flashState_.slots[i].frequencyHz;
                s.wpm         = flashState_.slots[i].wpm == 0 ? 18 : flashState_.slots[i].wpm;
                slots.push_back(s);
            }
        }

        return retVal;
    }

    bool Put()
    {
        flashState_.callsignStorage.fill(0);
        callsign.copy(flashState_.callsignStorage.data(), min(callsign.size(), flashState_.callsignStorage.size()));

        flashState_.correction = correction;

        flashState_.gridStorage.fill(0);
        grid.copy(flashState_.gridStorage.data(), min(grid.size(), flashState_.gridStorage.size()));

        flashState_.txInterval = txInterval;

        uint8_t n = (uint8_t)slots.size();
        if (n > SLOT_MAX) { n = SLOT_MAX; }
        for (uint8_t i = 0; i < SLOT_MAX; ++i)
        {
            flashState_.slots[i].bandStorage.fill(0);
            if (i < n)
            {
                flashState_.slots[i].mode = (slots[i].mode == SlotMode::CW)     ? 2
                                          : (slots[i].mode == SlotMode::WSPR15) ? 1
                                          :                                       0;
                slots[i].band.copy(flashState_.slots[i].bandStorage.data(),
                                   min(slots[i].band.size(), flashState_.slots[i].bandStorage.size()));
                flashState_.slots[i].channel     = slots[i].channel;
                flashState_.slots[i].frequencyHz = slots[i].frequencyHz;
                flashState_.slots[i].wpm         = slots[i].wpm;
            }
            else
            {
                flashState_.slots[i].mode        = 0;
                flashState_.slots[i].channel     = 0;
                flashState_.slots[i].frequencyHz = 0;
                flashState_.slots[i].wpm         = 18;
            }
        }
        flashState_.slotCount = n;

        return flashState_.Put();
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

            JsonArray slotsArr = out.createNestedArray("slots");
            for (const auto &s : slots)
            {
                JsonObject o = slotsArr.createNestedObject();
                o["mode"]    = (int)s.mode;
                o["band"]    = s.band;
                o["channel"] = s.channel;
                o["frequencyHz"] = s.frequencyHz;
                o["wpm"]     = s.wpm;
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
                    s.mode        = (modeRaw == 2) ? SlotMode::CW
                                  : (modeRaw == 1) ? SlotMode::WSPR15
                                  :                  SlotMode::WSPR2;
                    s.band        = (const char *)o["band"];
                    s.channel     = (uint16_t)(int)o["channel"];
                    s.frequencyHz = o.containsKey("frequencyHz") ? (uint32_t)(uint64_t)o["frequencyHz"] : 0;
                    s.wpm         = o.containsKey("wpm")         ? (uint8_t)(int)o["wpm"]              : 18;
                    if (s.wpm == 0) s.wpm = 18;
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

            for (size_t i = 0; i < slotsIn.size(); ++i)
            {
                const Slot &s = slotsIn[i];
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
