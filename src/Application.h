#pragma once

#include <algorithm>
using namespace std;

#include "ADCInternal.h"
#include "Blinker.h"
#include "JSONMsgRouter.h"
#include "SubsystemCopilotControl.h"
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

        // we use both the second I2C instance also
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
    // Program start - Decide Configuration or Flight Mode
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

        // Watchdog setup
        if (testCfg.enabled == false || (testCfg.enabled && testCfg.watchdogOn == true))
        {
            Watchdog::SetTimeout(5'000);
            Watchdog::Start();
            Log("Watchdog enabled");
            LogNL();

            timerWatchdog_.SetName("TIMER_WATCHDOG_FEED");
            timerWatchdog_.SetCallback([]{
                Watchdog::Feed();
            });
            timerWatchdog_.TimeoutIntervalMs(2'000, 0);
        }

        // set up blinker
        blinker_.SetPin(pinLedGreen_);

        // Startup blinks indicate progressively higher power demand
        PowerTest();

        // Set up system elements common between modes
        SetupShell();
        SetupJSON();

        if (testCfg.enabled && testCfg.logAsync == false)
        {
            Evm::DisableAutoLogAsync();
            Log("Async logging disabled");
        }

        // Set TX watchdog feeders that also keep the blink going
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

        // Determine mode of operation
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
        // wait for USB events to fire
        timerStartupRole_.SetName("TIMER_STARTUP_ROLE");
        timerStartupRole_.SetCallback([this]{
            EnableMode();
        });
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
            // normally the testmode being enabled means you want
            // to test sending messages in flight mode.
            // however, the api mode override lets you enter
            // configuration mode instead
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
            FlightMode();
        }

        // now that mode is established, if you subsequently plug back
        // into USB the device should restart to re-evaluate.
        // In API mode no such switch happens, the devices runs without
        // interruption
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

        // announce the temperature regularly
        static Timer timerTemp("APP_TEMP_TIMER");
        timerTemp.SetCallback([this]{
            router_.Send([&](const auto &out){
                out["type"] = "TEMP";
                out["tempC"] = tempSensor_.GetTempC();
                out["tempF"] = tempSensor_.GetTempF();
            });
        });
        timerTemp.TimeoutIntervalMs(30000, 0);

        if (enableBlink)
        {
            // Set on async blinking journey
            BlinkerIdle();
        }

        // If already configured, start the scheduler so transmissions
        // can be observed in the console while USB is connected.
        // If not yet configured, start it after the first successful REQ_SET_CONFIG.
        auto TryStartScheduler = [this]{
            if (!schedulerStarted_ && ssTx_.ReadyToFly())
            {
                schedulerStarted_ = true;
                ssTx_.SetupTransmitterForFlight();
                Log("Config mode: callsign valid, starting scheduler");
                SetupScheduler();
            }
        };

        TryStartScheduler();

        // Register a second REQ_SET_CONFIG handler (runs after Configuration's own handler)
        // to start the scheduler once a valid callsign has been saved.
        JSONMsgRouter::RegisterHandler("REQ_SET_CONFIG", [TryStartScheduler](auto &, auto &){
            TryStartScheduler();
        });
    }


    /////////////////////////////////////////////////////////////////
    // Flight Mode
    /////////////////////////////////////////////////////////////////
    
    void FlightMode()
    {
        LogNL();
        LogNL();
        Log("Flight Mode");
        LogNL();

        if (testCfg.enabled)
        {
            // testing - fudge some configuration
            Configuration &txCfg = ssTx_.GetConfiguration();
            txCfg.band = "20m";
            txCfg.callsign = "KD3KDD";
            txCfg.channel = 414;
            txCfg.correction = 0;
            txCfg.Put();
            txCfg.Get();
        }

        // Load flight configuration -- ensure it exists
        if (ssTx_.ReadyToFly() == false)
        {
            Log("ERR: ==== NOT READY TO FLY - FATAL ====");

            // panic blink - let watchdog kill
            while (true)
            {
                BlinkerBlinkOncePanic();
            }
        }
        else
        {
            ssTx_.SetupTransmitterForFlight();

            Configuration &txCfg = ssTx_.GetConfiguration();
            auto cd = WsprChannelMap::GetChannelDetails(txCfg.band.c_str(), txCfg.channel);

            Log("==== Ok to fly! ====");
            Log("Callsign  : ", txCfg.callsign);
            Log("Band      : ", txCfg.band);
            Log("Channel   : ", txCfg.channel);
            Log("ID13      : ", cd.id13);
            Log("Min       : ", cd.min);
            Log("Lane      : ", cd.lane);
            Log("Freq      : ", Commas(cd.freq));
            Log("Correction: ", txCfg.correction);
            Log("TxInterval: ", txCfg.txInterval);
            Log("Slots:");
            for (uint8_t i = 0; i < SLOT_COUNT; ++i)
            {
                if (txCfg.SlotEnabled(i + 1))
                {
                    auto scd = WsprChannelMap::GetChannelDetails(txCfg.slotBand[i].c_str(), txCfg.slotChannel[i]);
                    Log("  Slot ", i + 1, " (min :", i * 2, "): ", txCfg.slotBand[i], " ch ", txCfg.slotChannel[i], " freq ", Commas(scd.freq));
                }
                else
                {
                    Log("  Slot ", i + 1, " (min :", i * 2, "): disabled");
                }
            }
            LogNL();

            // Signal ok
            blinker_.Blink(4, 100, 100);

            // give visual space to distinguish these "ok" blinks from
            // upcoming status blinks
            Watchdog::Feed();
            PAL.Delay(1'500);
            Watchdog::Feed();

            // Set up copilot control scheduler
            SetupScheduler();
        }
    }


    /////////////////////////////////////////////////////////////////
    // Scheduler Integration
    /////////////////////////////////////////////////////////////////
    
    void SetupScheduler()
    {
        SetupSchedulerGps();
        SetupSchedulerMessageSending();
        SetupSchedulerRadio();
        SetupSchedulerClockSpeed();
        SetupSchedulerWsprMinute();

        ssCc_.GetScheduler().Start();
    }

    void SetupSchedulerGps()
    {
        auto &scheduler = ssCc_.GetScheduler();

        Shell::AddCommand("now", [this, &scheduler](vector<string> argList){
            // 2025-01-02 19:42:02.025000
            fix3dPlus_ = GPSReader::GetFix3DPlusExample();

            // our debug channel = 414, so minute 6.
            // change the time to start more quickly.
            fix3dPlus_.timeAtPpsUs = PAL.Micros();
            fix3dPlus_.minute = 5;
            fix3dPlus_.second = 55;
            fix3dPlus_.dateTime = GPSReader::MakeDateTimeFromFixTime(fix3dPlus_);

            scheduler.OnGps3DPlusLock(fix3dPlus_);
        }, { .argCount = 0, .help = "trigger 3d lock"});

        scheduler.SetCallbackRequestNewGpsLock([this, &scheduler]{
            BlinkerGpsSearch();

            t_.Reset();
            t_.SetMaxEvents(50);
            t_.Event("GPS_REQUESTED");

            // Enable GPS in preparation for new request
            if (testCfg.enabled == false)
            {
                ssGps_.DisableVerboseLogging();
            }
            ssGps_.EnableFlightMode();
            t_.Event("GpsEnabled");

            // Request new fix
            Log("Requesting FixTime and Fix3DPlus");
            
            auto FnOnFixTime = [this, &scheduler](const FixTime &fixTime){
                t_.Event("FixTime");

                Log("GPS: time fix acquired (UTC sync OK), cancelling lock-or-die timer");
                CancelGpsLockOrDieTimer();

                // store GPS time for TX messages
                lastGpsTime_ =
                    StrUtl::PadLeft(fixTime.hour,   '0', 2) + ":" +
                    StrUtl::PadLeft(fixTime.minute, '0', 2) + ":" +
                    StrUtl::PadLeft(fixTime.second, '0', 2);

                // emit GPS_FIX_TIME JSON so the web console shows the fix
                router_.Send([&](auto &out){
                    out["type"] = "GPS_FIX_TIME";
                    out["time"] = lastGpsTime_.c_str();
                });

                // tell scheduler
                scheduler.OnGpsTimeLock(fixTime);
            };

            auto FnOnFix3DPlus = [this, &scheduler](const Fix3DPlus &fix3dPlus){
                t_.Event("Fix3DPlus");

                Log("GPS: 3D fix acquired, grid=", fix3dPlus.maidenheadGrid);

                // store GPS time for TX messages
                lastGpsTime_ =
                    StrUtl::PadLeft(fix3dPlus.hour,   '0', 2) + ":" +
                    StrUtl::PadLeft(fix3dPlus.minute, '0', 2) + ":" +
                    StrUtl::PadLeft(fix3dPlus.second, '0', 2);

                // emit GPS_FIX_2D and GPS_FIX_3D JSON so the web console shows the fix
                router_.Send([&](auto &out){
                    out["type"]   = "GPS_FIX_2D";
                    out["latDeg"] = fix3dPlus.latDegMillionths / 1'000'000.0;
                    out["lngDeg"] = fix3dPlus.lngDegMillionths / 1'000'000.0;
                });
                router_.Send([&](auto &out){
                    out["type"]  = "GPS_FIX_3D";
                    out["altM"]  = fix3dPlus.altitudeM;
                });

                // capture fix
                fix3dPlus_ = fix3dPlus;

                // note that the 3d fix was acquired
                gotFix3dPlus_ = true;

                // persist last-known grid to flash so it survives reboot
                Configuration &txCfg = ssTx_.GetConfiguration();
                if (fix3dPlus_.maidenheadGrid.size() >= 4)
                {
                    txCfg.grid = fix3dPlus_.maidenheadGrid;
                    txCfg.Put();
                    Log("GPS: grid persisted to flash: ", txCfg.grid);
                }

                // tell scheduler
                scheduler.OnGps3DPlusLock(fix3dPlus_);
            };

            ssGps_.RequestNewFixTimeAnd3DPlus(FnOnFixTime, FnOnFix3DPlus);
            t_.Event("FixRequested");

            // Reboot if no time fix within 20 minutes — time sync is essential for WSPR
            StartGpsLockOrDieTimer();
        });

        // After the first full 10-minute cycle, the 3D fix search window expires.
        // Cancel the GPS request — this triggers CancelRequestNewGpsLock which shuts GPS off.
        scheduler.SetCallbackFirstCycleEnd([this, &scheduler]{
            Log("GPS: first cycle ended — 3D fix search window expired");
            if (!gotFix3dPlus_)
            {
                Configuration &txCfg = ssTx_.GetConfiguration();
                Log("GPS: no 3D fix obtained, will use fallback grid=", txCfg.grid);
            }
            scheduler.CancelRequestNewGpsLock();
        });

        scheduler.SetCallbackCancelRequestNewGpsLock([this]{
            t_.Event("CancelReqNewGpsLock");

            // indicate idle state
            BlinkerIdle();

            // shut off gps
            ssGps_.Disable();

            if (gotFix3dPlus_)
            {
                Log("GPS: off — 3D fix was obtained, grid=", fix3dPlus_.maidenheadGrid);
            }
            else
            {
                Configuration &txCfg = ssTx_.GetConfiguration();
                Log("GPS: off — no 3D fix obtained, using fallback grid=", txCfg.grid);
            }
        });
    }

    void SetupSchedulerMessageSending()
    {
        auto &scheduler = ssCc_.GetScheduler();

        scheduler.SetCallbackScheduleNow([this, &scheduler](bool haveGpsLock){
            const Configuration &txCfg = ssTx_.GetConfiguration();

            // Arm each slot whose band is configured and valid.
            for (uint8_t slot = 1; slot <= SLOT_COUNT; ++slot)
            {
                scheduler.UnSetCallbackSendDefault(slot);
                if (txCfg.SlotEnabled(slot))
                {
                    scheduler.SetCallbackSendDefault(slot, false, [this](uint8_t, uint64_t){
                        SendRegularType1();
                    });
                }
            }
        });

        // Switch the transmitter frequency to the band configured for this slot
        // before the period fires its send callback.
        scheduler.SetCallbackSetSlotFrequency([this](uint8_t slot){
            Configuration &txCfg = ssTx_.GetConfiguration();
            if (slot >= 1 && slot <= SLOT_COUNT && txCfg.SlotEnabled(slot))
            {
                const string   &b = txCfg.slotBand[slot - 1];
                uint16_t        c = txCfg.slotChannel[slot - 1];
                auto cd = WsprChannelMap::GetChannelDetails(b.c_str(), c);
                Log("Slot ", slot, ": switching to ", b, " ch ", c, " freq ", Commas(cd.freq));
                ssTx_.SetupTransmitterForSlot(b, c, txCfg.correction);
            }
        });

        scheduler.SetCallbackSendUserDefined([this](uint8_t slot, MsgUD &msg, uint64_t quitAfterMs){
            SendUserDefined(slot, msg, quitAfterMs);
        });
    }

    void SetupSchedulerRadio()
    {
        auto &scheduler = ssCc_.GetScheduler();

        scheduler.SetCallbackRadioIsActive([this]{
            return ssTx_.IsOn();
        });

        scheduler.SetCallbackStartRadioWarmup([this]{
            ssTx_.Enable();
            ssTx_.RadioOn();
            // Frequency is set per-slot via SetCallbackSetSlotFrequency before each period.

            BlinkerTransmit();
        });

        scheduler.SetCallbackStopRadio([this]{
            ssTx_.RadioOff();
            ssTx_.Disable();
        });
    }

    void SetupSchedulerClockSpeed()
    {
        // Ignoring LED blinks, GPS, TX, etc, the following are the
        // current consumption measurements by clock speed:
        // Low-Jitter (not power-optimized):
        //  6 MHz: ~  4.7 mA
        // 12 MHz: ~  5.5 mA
        // 48 MHz: ~ 13.0 mA
        //
        // More-Jitter (power-optimized)
        //  6 MHz: ~ (not possible)
        // 12 MHz: ~ (not possible)
        // 48 MHz: ~ (exactly same)
        //
        // LED: ~ 3 mA
        //
        // Time to switch to a pre-cached clock speed:
        //             |  6 MHz | 12 MHz | 48 MHz
        // --------------------------------------
        // From  6 MHz |  32 ms |  36 ms |  40 ms
        // From 12 MHz |  40 ms |  26 ms |  25 ms
        // From 48 MHz |  32 ms |  19 ms |  17 ms

        auto &scheduler = ssCc_.GetScheduler();

        scheduler.SetCallbackGoHighSpeed([this]{
            Clock::SetClockMHz(48);
        });

        scheduler.SetCallbackGoLowSpeed([this]{
            Clock::SetClockMHz(6);
        });
    }

    void SetupSchedulerWsprMinute()
    {
        auto &scheduler = ssCc_.GetScheduler();

        Configuration &txCfg = ssTx_.GetConfiguration();
        // Every WSPR window is 2 minutes wide; align to even UTC minutes (startMin = 0).
        scheduler.SetStartMinute(0);
        scheduler.SetTxInterval(txCfg.txInterval);
    }


    /////////////////////////////////////////////////////////////////
    // Message Sending
    /////////////////////////////////////////////////////////////////

    void SendRegularType1()
    {
        const Configuration &txCfg = ssTx_.GetConfiguration();
        static const uint8_t POWER_DBM = 13;

        // use live grid if we have a fix, otherwise fall back to last-known grid from flash
        string grid = fix3dPlus_.maidenheadGrid.size() >= 4
                        ? fix3dPlus_.maidenheadGrid.substr(0, 4)
                        : txCfg.grid.substr(0, 4);

        router_.Send([&](auto &out){
            out["type"]     = "TX_START";
            out["callsign"] = txCfg.callsign.c_str();
            out["grid"]     = grid.c_str();
            out["gps"]      = gotFix3dPlus_;
            out["gpsTime"]  = lastGpsTime_.c_str();
        });
        ssTx_.SendRegularMessage(txCfg.callsign, grid, POWER_DBM);
        router_.Send([&](auto &out){
            out["type"] = "TX_DONE";
        });
    };

    void SendUserDefined(uint8_t slot, MsgUD &msg, uint64_t quitAfterMs)
    {
        const Configuration &txCfg = ssTx_.GetConfiguration();
        WsprChannelMap::ChannelDetails cd = WsprChannelMap::GetChannelDetails(txCfg.band.c_str(), txCfg.channel);

        msg.SetId13(cd.id13);
        msg.SetHdrSlot(slot - 1);
        msg.Encode();

        Log("Sending User-Defined Message in slot", slot, " (limit ", Commas(quitAfterMs)," ms): ", msg.GetCallsign(), " ", msg.GetGrid4(), " ", msg.GetPowerDbm());
        Log(CopilotControlUtl::GetMsgStateAsString(msg));
        ssTx_.SetTxQuitAfterMs(quitAfterMs);
        ssTx_.SendMessage(msg);
        ssTx_.SetTxQuitAfterMs(0);
        Log("Sent");
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

    void CancelGpsLockOrDieTimer()
    {
        timerGpsLockOrDie_.Cancel();
    }


    /////////////////////////////////////////////////////////////////
    // Flight Mode - Blinker states
    /////////////////////////////////////////////////////////////////

    inline static const uint32_t WSPR_BIT_DURATION_MS = 683;

    // once every 5 seconds
    void BlinkerIdle()
    {
        blinker_.SetBlinkOnOffTime(75, 4925);
        blinker_.EnableAsyncBlink();
    }

    // once every 1 seconds
    void BlinkerGpsSearch()
    {
        blinker_.SetBlinkOnOffTime(75, 925);
        blinker_.EnableAsyncBlink();
    }

    // alternating 683ms periods
    // also operated by hand, but also run during radio warmup
    void BlinkerTransmit()
    {
        blinker_.SetBlinkOnOffTime(WSPR_BIT_DURATION_MS, WSPR_BIT_DURATION_MS);
        blinker_.EnableAsyncBlink();
    }

    void BlinkerBlinkOncePanic()
    {
        blinker_.Blink(1, 40, 40);
    }


private:

    /////////////////////////////////////////////////////////////////
    // Power
    /////////////////////////////////////////////////////////////////

    void PowerSave()
    {
        Log("Power saving processing");

        // prepare to switch to 48MHz in the event that USB is connected
        // later.  willing to take a moment longer on high power to
        // speed up calculation and make device startup not annoyingly
        // longer. ~70ms to accomplish.
        Log("Prepare 48MHz clock speed");
        Clock::PrepareClockMHz(48);

        // 5mA baseline
        // takes ~10ms to accomplish
        Log("Drop to 6MHz clock speed");
        Clock::SetClockMHz(6);
        LogNL();

        if (testCfg.enabled)
        {
            Clock::PrintAll();
            LogNL();
        }

        // saves 5mA at 125MHz
        // saves 2mA at  48MHz
        Log("Disable unused peripherals");
        PeripheralControl::DisablePeripheralList({
            PeripheralControl::SPI1,
            PeripheralControl::SPI0,
            PeripheralControl::PWM,
            PeripheralControl::PIO1,
            PeripheralControl::PIO0,
        });

        // Set up USB to be off unless VBUS detected, but we'll get notified
        // before USB re-enabled so we can get up to speed to handle the bus.
        // empirically I see 45MHz is the minimum required but let's do 48MHz
        // just because.        
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
        // Initial blink pattern indicates testing of systems and the
        // sufficiency of the power source (ie solar).
        Log("Startup Power Test");

        // Blink 1 - CPU can run on this power
        PAL.Delay(1'000);
        blinker_.Blink(1, 500, 100);
        Watchdog::Feed();

        // Blink 2 - GPS can run on this power
        ssGps_.ModulePowerOnBatteryOn();
        // PAL.Delay(500);  // the power on has a delay of 500 in it already
        PAL.Delay(500);  // add another 500 for a total of 1 sec
        blinker_.Blink(1, 500, 100);
        ssGps_.ModulePowerOff();
        Watchdog::Feed();

        // Blink 3 - Transmitter can run on this power
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
        Shell::AddCommand("app.test.led.green.on", [this](vector<string> argList){
            pinLedGreen_.DigitalWrite(1);
        }, { .argCount = 0, .help = ""});

        Shell::AddCommand("app.test.led.green.off", [this](vector<string> argList){
            pinLedGreen_.DigitalWrite(0);
        }, { .argCount = 0, .help = ""});


        static uint32_t count = 0;
        static bool show = false;
        UartAddLineStreamCallback(UART::UART_1, [](const string &line){
            UartTarget target(UART::UART_0);
            if (show)
            {
                Log(line);
            }
            ++count;
        });

        Shell::AddCommand("app.count", [this](vector<string> argList){
            Log(count);
        }, { .argCount = 0, .help = ""});

        Shell::AddCommand("app.show", [this](vector<string> argList){
            show = !show;
        }, { .argCount = 0, .help = ""});
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

        JSONMsgRouter::RegisterHandler("REQ_GET_DEVICE_INFO", [this](auto &in, auto &out){
            out["type"] = "REP_GET_DEVICE_INFO";

            out["swVersion"] = Split(Version::GetVersion())[0];
            if (testCfg.enabled && testCfg.apiMode)
            {
                out["mode"] = "API";
            }
            else
            {
                out["mode"] = "TRACKER";
            }
        });
    }

private:

    bool configurationMode_ = false;
    bool schedulerStarted_  = false;

    Pin pinLedGreen_ = { 25 };

    SubsystemCopilotControl ssCc_;
    SubsystemGps ssGps_;
    SubsystemTx ssTx_;

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



