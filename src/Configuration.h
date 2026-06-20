#pragma once

#include <array>
#include <string>
using namespace std;

#include "Flashable.h"
#include "JSONMsgRouter.h"
#include "WsprEncodedDynamic.h"


static const uint8_t SLOT_COUNT = 5;

class Configuration
{
private:
    inline static const string DEFAULT_BAND = "20m";

    struct ConfigurationFlashState
    {
        array<char, 5 + 1> bandStorage;
        uint16_t channel;
        array<char, 6 + 1> callsignStorage;
        int32_t correction;
        array<char, 6 + 1> gridStorage;
        uint8_t txInterval;  // 1 = every 2-min window, 2 = every 4 min, etc.

        // Per-slot band/channel for the 5 WSPR windows (minutes 0,2,4,6,8).
        // slotBand[i] == "" means slot i+1 is disabled (no transmission).
        array<array<char, 5 + 1>, SLOT_COUNT> slotBandStorage;
        array<uint16_t, SLOT_COUNT>            slotChannel;
    };

    Flashable<ConfigurationFlashState> flashState_;

    void Reset()
    {
        flashState_.bandStorage.fill(0);
        band = DEFAULT_BAND;

        flashState_.channel = 0;
        channel = 0;

        flashState_.callsignStorage.fill(0);
        callsign = "";

        flashState_.correction = 0;
        correction = 0;

        flashState_.gridStorage.fill(0);
        grid = "";

        flashState_.txInterval = 1;
        txInterval = 1;

        for (uint8_t i = 0; i < SLOT_COUNT; ++i)
        {
            flashState_.slotBandStorage[i].fill(0);
            slotBand[i] = "";
            flashState_.slotChannel[i] = 0;
            slotChannel[i] = 0;
        }
        // Slot 0 (minute :00) defaults to the primary band/channel
        slotBand[0]    = DEFAULT_BAND;
        flashState_.slotBandStorage[0][0] = DEFAULT_BAND[0];
        flashState_.slotBandStorage[0][1] = DEFAULT_BAND[1];
        flashState_.slotBandStorage[0][2] = DEFAULT_BAND[2];
        flashState_.slotBandStorage[0][3] = 0;
    }

public:
    Configuration()
    {
        Reset();

        // Pull up stored configuration
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

        // the storage values are a write-through cache.
        // don't read from them unless you know you've written to them first
        if (retVal)
        {
            band       = (const char *)flashState_.bandStorage.data();
            channel    = (uint16_t)flashState_.channel;
            callsign   = (const char *)flashState_.callsignStorage.data();
            correction = flashState_.correction;
            grid       = (const char *)flashState_.gridStorage.data();
            txInterval = flashState_.txInterval < 1 ? 1 : flashState_.txInterval;

            for (uint8_t i = 0; i < SLOT_COUNT; ++i)
            {
                slotBand[i]    = (const char *)flashState_.slotBandStorage[i].data();
                slotChannel[i] = flashState_.slotChannel[i];
            }
        }

        return retVal;
    }

    bool Put()
    {
        flashState_.bandStorage.fill(0);
        band.copy(flashState_.bandStorage.data(), min(band.size(), flashState_.bandStorage.size()));

        flashState_.channel = channel;

        flashState_.callsignStorage.fill(0);
        callsign.copy(flashState_.callsignStorage.data(), min(callsign.size(), flashState_.callsignStorage.size()));

        flashState_.correction = correction;

        flashState_.gridStorage.fill(0);
        grid.copy(flashState_.gridStorage.data(), min(grid.size(), flashState_.gridStorage.size()));

        flashState_.txInterval = txInterval;

        for (uint8_t i = 0; i < SLOT_COUNT; ++i)
        {
            flashState_.slotBandStorage[i].fill(0);
            slotBand[i].copy(flashState_.slotBandStorage[i].data(),
                             min(slotBand[i].size(), flashState_.slotBandStorage[i].size()));
            flashState_.slotChannel[i] = slotChannel[i];
        }

        return flashState_.Put();
    }

    // Returns true if the slot has a valid band configured (non-empty, known band).
    bool SlotEnabled(uint8_t slot) const
    {
        if (slot < 1 || slot > SLOT_COUNT) return false;
        const string &b = slotBand[slot - 1];
        if (b.empty()) return false;
        return b == Wspr::GetDefaultBandIfNotValid(b.c_str());
    }


private:

    void SetupShell()
    {
        Shell::AddCommand("app.cfg.del", [this](vector<string> argList){
            flashState_.Delete();
            Reset();

            Log("Configuration Deleted, state reset");
        }, { .argCount = 0, .help = "delete config"});
    }

    void SetupJSON()
    {
        JSONMsgRouter::RegisterHandler("REQ_GET_CONFIG", [this](auto &in, auto &out){
            out["type"] = "REP_GET_CONFIG";

            out["band"]       = band;
            out["channel"]    = channel;
            out["callsign"]   = callsign;
            out["correction"] = correction;
            out["grid"]       = grid;
            out["txInterval"] = txInterval;

            out["callsignOk"] = WsprMessageRegularType1::CallsignIsValid(callsign.c_str());

            // Per-slot band/channel arrays
            JsonArray bandsArr    = out.createNestedArray("slotBands");
            JsonArray channelsArr = out.createNestedArray("slotChannels");
            for (uint8_t i = 0; i < SLOT_COUNT; ++i)
            {
                bandsArr.add(slotBand[i].c_str());
                channelsArr.add(slotChannel[i]);
            }
        });

        JSONMsgRouter::RegisterHandler("REQ_DELETE_CONFIG", [this](auto &in, auto &out){
            out["type"] = "REP_DELETE_CONFIG";
            flashState_.Delete();
            Reset();
            out["ok"] = true;
        });

        JSONMsgRouter::RegisterHandler("REQ_SET_CONFIG", [this](auto &in, auto &out){
            out["type"] = "REP_SET_CONFIG";

            string bandIn        = (const char *)in["band"];
            int16_t channelIn    = (int16_t)in["channel"];
            string callsignIn    = (const char *)in["callsign"];
            int32_t correctionIn = (int32_t)in["correction"];
            string gridIn        = in.containsKey("grid") ? (const char *)in["grid"] : "";
            uint8_t txIntervalIn = in.containsKey("txInterval") ? (uint8_t)(int)in["txInterval"] : 1;
            if (txIntervalIn < 1) txIntervalIn = 1;

            // Per-slot band/channel (optional; keep existing values if not provided)
            array<string, SLOT_COUNT>   slotBandIn    = slotBand;
            array<uint16_t, SLOT_COUNT> slotChannelIn = slotChannel;
            if (in.containsKey("slotBands") && in.containsKey("slotChannels"))
            {
                JsonArrayConst bandsArr    = in["slotBands"];
                JsonArrayConst channelsArr = in["slotChannels"];
                for (uint8_t i = 0; i < SLOT_COUNT && i < bandsArr.size(); ++i)
                {
                    slotBandIn[i]    = (const char *)bandsArr[i];
                    slotChannelIn[i] = (uint16_t)(int)channelsArr[i];
                }
            }

            bool ok = true;
            string err = "";
            string sep = "";

            if (bandIn != Wspr::GetDefaultBandIfNotValid(bandIn.c_str()))
            {
                ok = false;
                err += sep + "Invalid band";
                sep = ", ";
            }

            if (channelIn != WsprChannelMap::GetDefaultChannelIfNotValid(channelIn))
            {
                ok = false;
                err += sep + "Invalid channel";
                sep = ", ";
            }

            if (WsprMessageRegularType1::CallsignIsValid(callsignIn.c_str()) == false)
            {
                ok = false;
                err += sep + "Invalid callsign";
                sep = ", ";
            }

            // Validate per-slot bands (empty string = disabled, which is valid)
            for (uint8_t i = 0; i < SLOT_COUNT; ++i)
            {
                if (!slotBandIn[i].empty() &&
                    slotBandIn[i] != Wspr::GetDefaultBandIfNotValid(slotBandIn[i].c_str()))
                {
                    ok = false;
                    err += sep + "Invalid band for slot " + to_string(i + 1);
                    sep = ", ";
                }
            }

            if (ok)
            {
                // attempt to store
                Configuration cfgCache = *this;

                band       = bandIn;
                channel    = channelIn;
                callsign   = callsignIn;
                correction = correctionIn;
                grid       = gridIn;
                txInterval = txIntervalIn;
                slotBand    = slotBandIn;
                slotChannel = slotChannelIn;

                ok = Put();

                if (ok == false)
                {
                    *this = cfgCache;

                    err += sep + "Could not store to flash";
                    sep = ", ";
                }
            }

            Log("REQ_SET_CONFIG: ", bandIn, ", ", channelIn, ", ", callsignIn, ", ", correctionIn);
            Log("OK: ", ok, ", err: \"", err, "\"");
            Log("Current config: ", band, ", ", channel, ", ", callsign, ", ", correction);

            out["ok"] = ok;
            out["err"] = err;
        });
    }

public:

    string   band;
    uint16_t channel;
    string   callsign;
    int32_t  correction;
    string   grid;
    uint8_t  txInterval;

    // Per-slot config: index 0 = minute :00, 1 = :02, 2 = :04, 3 = :06, 4 = :08
    // slotBand[i] == "" means slot i+1 is disabled.
    array<string, SLOT_COUNT>   slotBand;
    array<uint16_t, SLOT_COUNT> slotChannel;
};

inline void LogNNL(const Configuration &c)
{
    Log("{");
    Log("  .band       = ", c.band);
    Log("  .callsign   = ", c.callsign);
    Log("  .channel    = ", c.channel);
    Log("  .correction = ", c.correction);
    LogNNL("}");
}

inline void Log(const Configuration &c)
{
    LogNNL(c);
    LogNL();
}
