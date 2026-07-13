#pragma once

#include "App.h"
#include "JSONMsgRouter.h"
#include "WsprEncodedDynamic.h"
#include "WSPRMessageTransmitter.h"

#include "Configuration.h"
#include "CwEncoder.h"
#include "Scheduler.h"


class SubsystemTx
{
public:

    SubsystemTx()
    {
        Disable();

        SetupShell();
        SetupJSON();
    }

    bool ReadyToBeacon()
    {
        bool retVal = true;

        if (cfg_.Get() != true)
        {
            retVal = false;
            Log("ERR: ReadyToBeacon: Could not read config");
        }

        if (cfg_.callsign == "")
        {
            retVal = false;
            Log("ERR: ReadyToBeacon: Callsign blank");
        }

        if (cfg_.slots.empty())
        {
            retVal = false;
            Log("ERR: ReadyToBeacon: No slots configured");
        }

        return retVal;
    }

    Configuration &GetConfiguration() { return cfg_; }

    void Enable()
    {
        Log("TX Subsystem On");
        pinTxLoadSwitchOnOff_.DigitalWrite(0);
        PAL.Delay(5);
        enabled_ = true;
    }

    void RadioOn()
    {
        Log("Radio on");
        wsprMessageTransmitter_.RadioOn();
        on_ = true;
    }

    bool IsOn() { return on_; }

    // Switch the transmitter timing to the WSPR variant for this slot
    // (called per-slot by the scheduler so a mixed schedule transmits correctly).
    // CW slots don't use this — they call SendCwMessage() instead.
    void SetWsprMode(SlotMode mode)
    {
        if (mode == SlotMode::WSPR15)
        {
            wsprMessageTransmitter_.SetWsprMode(WSPRMessageTransmitter::WSPR15_TONE_SPACING_HUNDREDTHS_HZ,
                                                WSPRMessageTransmitter::WSPR15_DELAY_US);
        }
        else if (mode == SlotMode::WSPR2)
        {
            wsprMessageTransmitter_.SetWsprMode(WSPRMessageTransmitter::WSPR2_TONE_SPACING_HUNDREDTHS_HZ,
                                                WSPRMessageTransmitter::WSPR2_DELAY_US);
        }
        // SlotMode::CW: no-op, CW doesn't use WSPR symbol timing.
    }

    // Set the transmitter to a raw frequency (CW use). Assumes RadioOn() has been called.
    void SetupForCw(uint32_t freqHz)
    {
        Log("Setup Transmitter (CW): Freq=", Commas(freqHz));
        wsprMessageTransmitter_.SetFrequency(freqHz);
        wsprMessageTransmitter_.SetCorrection(cfg_.correction);
    }

    // Send a CW message by keying the Si5351 output on/off according to the
    // encoded element stream. Blocks until the message has been sent.
    // Caller is responsible for having called Enable() + RadioOn() + SetupForCw().
    void SendCwMessage(const string &text, uint8_t wpm)
    {
        Log("Sending CW: \"", text, "\" @ ", (int)wpm, " WPM (",
            CwEncoder::EstimateDurationMs(text, wpm), " ms)");

        auto events = CwEncoder::Encode(text, wpm);

        // Start with the radio keyed off — RadioOn left it enabled, we'll
        // toggle as the event stream dictates.
        wsprMessageTransmitter_.Key(false);

        for (const auto &ev : events)
        {
            Watchdog::Feed();
            wsprMessageTransmitter_.Key(ev.keyOn);
            PAL.DelayBusyUs((uint64_t)ev.durationMs * 1000);
        }

        // Leave radio in a known-off state after transmission.
        wsprMessageTransmitter_.Key(false);
        Log("CW sent");
    }

    // Set the transmitter to a slot's band+channel ahead of transmission.
    void SetupForSlot(const string &band, uint16_t channel)
    {
        WsprChannelMap::ChannelDetails cd = WsprChannelMap::GetChannelDetails(band.c_str(), channel);

        Log("Setup Transmitter (Slot): Band=", band, " Ch=", channel, " Freq=", Commas(cd.freq));

        wsprMessageTransmitter_.SetFrequency(cd.freq);
        wsprMessageTransmitter_.SetCorrection(cfg_.correction);
    }

    // Calibration: emit a stable tone at (band's dial freq + 1500 Hz) for tuning.
    void SetupForCalibration(const string &band, uint16_t channel, int32_t correction)
    {
        WsprChannelMap::ChannelDetails cd = WsprChannelMap::GetChannelDetails(band.c_str(), channel);

        Log("Setup Transmitter (Calibration mode)");
        Log("Band: ", band, ", Channel: ", channel);
        Log("Freq: ", Commas(cd.freqDial + 1500), ", Correction: ", correction);
        LogNL();

        wsprMessageTransmitter_.SetFrequency(cd.freqDial + 1500);
        wsprMessageTransmitter_.SetCorrection(correction);
    }

    void SetCallbackOnTxStart(function<void()> fn)   { wsprMessageTransmitter_.SetCallbackOnTxStart(fn); }
    void SetCallbackOnBitChange(function<void()> fn) { wsprMessageTransmitter_.SetCallbackOnBitChange(fn); }
    void SetCallbackOnTxEnd(function<void()> fn)     { wsprMessageTransmitter_.SetCallbackOnTxEnd(fn); }

    void SetTxQuitAfterMs(uint64_t ms)
    {
        if (ms == 0)
        {
            wsprMessageTransmitter_.SetQuitEarlyFunction([](uint64_t){ return false; });
        }
        else
        {
            wsprMessageTransmitter_.SetQuitEarlyFunction([=](uint64_t msSinceStart){
                return msSinceStart >= ms;
            });
        }
    }

    void SendRegularMessage(const string &callsign, const string &grid4, uint8_t powerDbm)
    {
        WsprMessageRegularType1 msg;
        msg.SetCallsign(callsign.c_str());
        msg.SetGrid4(grid4.c_str());
        msg.SetPowerDbm(powerDbm);

        Log("Sending regular msg: ", msg.GetCallsign(), " ", msg.GetGrid4(), " ", msg.GetPowerDbm());
        wsprMessageTransmitter_.Send(msg.GetCallsign(), msg.GetGrid4(), msg.GetPowerDbm());
        Log("Sent");
    }

    void RadioOff()
    {
        Log("Radio off");
        LogNL();
        wsprMessageTransmitter_.RadioOff();
        on_ = false;
    }

    void Disable()
    {
        Log("TX Subsystem Off");
        LogNL();
        pinTxLoadSwitchOnOff_.DigitalWrite(1);
        enabled_ = false;
    }


private:

    void TestCwSend(uint32_t freqHz, uint8_t wpm, const string &text)
    {
        Timeline::Global().SetMaxEvents(50);
        Timeline::Global().Reset();
        Timeline::Global().Event("TestCwSend Start");

        bool enableCache = enabled_;
        bool onCache = on_;

        if (!enableCache) { Enable(); }
        SetupForCw(freqHz);
        if (!onCache) { RadioOn(); }

        SendCwMessage(text, wpm);

        if (!onCache) { RadioOff(); }
        if (!enableCache) { Disable(); }

        Timeline::Global().Event("TestCwSend End");
        Timeline::Global().Report();
    }

    void TestWsprSend(const string &band, uint16_t channel, SlotMode mode,
                      const string &callsign, const string &grid, uint8_t powerDbm = 13)
    {
        Timeline::Global().SetMaxEvents(300);
        Timeline::Global().Reset();
        Timeline::Global().Event("TestWsprSend Start");

        bool enableCache = enabled_;
        bool onCache = on_;

        if (!enableCache) { Enable(); }
        SetWsprMode(mode);
        SetupForSlot(band, channel);
        if (!onCache) { RadioOn(); }

        Log("Sending");
        SendRegularMessage(callsign, grid, powerDbm);

        if (!onCache) { RadioOff(); }
        if (!enableCache) { Disable(); }

        Timeline::Global().Event("TestWsprSend End");
        Timeline::Global().Report();
    }

    void SetupShell()
    {
        Shell::AddCommand("app.tx", [this](vector<string> argList){
            if (argList[0] == "on") { Enable();  }
            else                    { Disable(); }
        }, { .argCount = 1, .help = "power clockgen <on/off>"});

        Shell::AddCommand("app.radio", [this](vector<string> argList){
            if (argList[0] == "on") { Enable();  RadioOn();  }
            else                    {            RadioOff(); }
        }, { .argCount = 1, .help = "clockgen run <on/off>"});

        Shell::AddCommand("app.wspr.quitms", [this](vector<string> argList){
            uint64_t quitMs = (uint64_t)atoi(argList[0].c_str());
            SetTxQuitAfterMs(quitMs);
        }, { .argCount = 1, .help = "quit wspr tx <ms> after tx start, 0 to clear"});

        Shell::AddCommand("app.wspr.send", [this](vector<string> argList){
            // band channel mode callsign grid
            string  band     = argList[0];
            uint16_t channel = (uint16_t)atoi(argList[1].c_str());
            SlotMode mode    = (argList[2] == "15") ? SlotMode::WSPR15 : SlotMode::WSPR2;
            string  callsign = argList[3];
            string  grid     = argList[4];
            TestWsprSend(band, channel, mode, callsign, grid);
        }, { .argCount = 5, .help = "wspr send <band> <channel> <2|15> <callsign> <grid>"});

        Shell::AddCommand("app.cw.send", [this](vector<string> argList){
            // freqHz wpm text...
            uint32_t freqHz = (uint32_t)strtoul(argList[0].c_str(), nullptr, 10);
            uint8_t  wpm    = (uint8_t)atoi(argList[1].c_str());
            string   text;
            for (size_t i = 2; i < argList.size(); ++i)
            {
                if (i > 2) text += " ";
                text += argList[i];
            }
            TestCwSend(freqHz, wpm, text);
        }, { .argCount = -1, .help = "cw send <freqHz> <wpm> <text...>"});
    }

    void SetupJSON()
    {
        JSONMsgRouter::RegisterHandler("REQ_RADIO_POWER_ON",     [this](auto &, auto &){ Log("REQ_RADIO_POWER_ON");  Enable();  });
        JSONMsgRouter::RegisterHandler("REQ_RADIO_POWER_OFF",    [this](auto &, auto &){ Log("REQ_RADIO_POWER_OFF"); Disable(); });
        JSONMsgRouter::RegisterHandler("REQ_RADIO_OUTPUT_ENABLE",[this](auto &, auto &){ Log("REQ_RADIO_OUTPUT_ENABLE"); Enable(); RadioOn(); });

        JSONMsgRouter::RegisterHandler("REQ_RADIO_OUTPUT_DISABLE", [this](auto &, auto &){
            Log("REQ_RADIO_OUTPUT_DISABLE");
            if (enabled_) { RadioOff(); }
        });

        // interactive calibration (USB only) - does not commit to flash
        JSONMsgRouter::RegisterHandler("REQ_SET_CONFIG_TEMP", [this](auto &in, auto &){
            Log("REQ_SET_CONFIG_TEMP");
            string  band       = (const char *)in["band"];
            uint16_t channel   = (uint16_t)(int)in["channel"];
            int32_t correction = (int32_t)in["correction"];
            SetupForCalibration(band, channel, correction);
        });

        JSONMsgRouter::RegisterHandler("REQ_WSPR_SEND", [this](auto &in, auto &out){
            out["type"] = "REP_WSPR_SEND";
            Log("REQ_WSPR_SEND");

            string  band     = in.containsKey("band") ? (const char *)in["band"] : string("20m");
            uint16_t channel = in.containsKey("channel") ? (uint16_t)(int)in["channel"] : 0;
            SlotMode mode    = (in.containsKey("mode") && (int)in["mode"] == 1) ? SlotMode::WSPR15 : SlotMode::WSPR2;
            string  callsign = (const char *)in["callsign"];
            string  grid     = (const char *)in["grid"];
            uint8_t powerDbm = (uint8_t)(int)in["power"];

            Log("band: ", band, ", channel: ", channel, ", mode: ", (int)mode,
                ", callsign: ", callsign, ", grid: ", grid, ", powerDbm: ", powerDbm);

            TestWsprSend(band, channel, mode, callsign, grid, powerDbm);
        });
    }


private:

    Configuration cfg_;

    Pin pinTxLoadSwitchOnOff_{ 28, Pin::Type::OUTPUT, 1 };

    bool enabled_ = false;
    bool on_ = false;

    WSPRMessageTransmitter wsprMessageTransmitter_;
};
