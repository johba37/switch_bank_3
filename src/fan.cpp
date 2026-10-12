#include "fan.h"

#include <Arduino.h>
#include <N2kMessages.h>
#include <driver/pcnt.h>
#include "elapsedMillis.h"

#define FAN_PWM_PIN 25                // blue wires of both fans
#define FAN_PWM_CHANNEL 0             // LEDC channel
#define FAN_PWM_FREQ 25000            // 25 kHz, per fan datasheet
#define FAN_PWM_RESOLUTION 8
#define FAN_PULSES_PER_REV 2          // tach cycles per rotation, per fan datasheet
#define FAN_COMMAND_TIMEOUT_MS 30000  // full speed if no 127500 duty for this long
#define FAN_FAILSAFE_DUTY 100         // percent
#define FAN_CONNECTION_ID 1           // PGN 127500 Connection ID of the fans
#define NumberOfFans 2

// Tach inputs (white wires, 1k pull-up to 3.3V) and the engine instance each
// fan's RPM is reported as in PGN 127488
const gpio_num_t FanTachoPins[NumberOfFans] = { GPIO_NUM_26, GPIO_NUM_27 };
const pcnt_unit_t FanPcntUnits[NumberOfFans] = { PCNT_UNIT_0, PCNT_UNIT_1 };
const uint8_t FanEngineInstances[NumberOfFans] = { 2, 3 };

bool FanPowered = false;
int FanCommandedDuty = -1;            // last duty received from network, -1 = none yet
elapsedMillis TimeSinceFanCommand = 0;
elapsedMillis TimeSinceFanUpdate = 0;
double FanRpm[NumberOfFans] = { 0, 0 };

// Duty the fans should run at: network command, or full speed as failsafe
static bool FanFailsafe() {
    return FanCommandedDuty < 0 || TimeSinceFanCommand > FAN_COMMAND_TIMEOUT_MS;
}

static uint8_t FanEffectiveDuty() {
    return FanFailsafe() ? FAN_FAILSAFE_DUTY : FanCommandedDuty;
}

static void FanApplyPwm() {
    // Don't drive the PWM inputs of unpowered fans
    uint8_t Duty = FanPowered ? FanEffectiveDuty() : 0;
    ledcWrite(FAN_PWM_CHANNEL, (Duty * 255) / 100);
}

static void SetupTachoCounter(gpio_num_t Pin, pcnt_unit_t Unit) {
    pcnt_config_t Config = {};
    Config.pulse_gpio_num = Pin;
    Config.ctrl_gpio_num = PCNT_PIN_NOT_USED;
    Config.channel = PCNT_CHANNEL_0;
    Config.unit = Unit;
    Config.pos_mode = PCNT_COUNT_INC;   // count rising edges
    Config.neg_mode = PCNT_COUNT_DIS;
    Config.lctrl_mode = PCNT_MODE_KEEP;
    Config.hctrl_mode = PCNT_MODE_KEEP;
    Config.counter_h_lim = INT16_MAX;
    Config.counter_l_lim = 0;
    pcnt_unit_config(&Config);
    // Ignore glitches shorter than ~12.8us (1023 APB cycles), e.g. PWM crosstalk
    pcnt_set_filter_value(Unit, 1023);
    pcnt_filter_enable(Unit);
    pcnt_counter_clear(Unit);
    pcnt_counter_resume(Unit);
}

void FanSetup() {
    // PWM low until the fans are powered
    ledcSetup(FAN_PWM_CHANNEL, FAN_PWM_FREQ, FAN_PWM_RESOLUTION);
    ledcAttachPin(FAN_PWM_PIN, FAN_PWM_CHANNEL);
    FanApplyPwm();

    for (uint8_t i = 0; i < NumberOfFans; i++) {
        SetupTachoCounter(FanTachoPins[i], FanPcntUnits[i]);
    }
}

void FanSetPowered(bool Powered) {
    FanPowered = Powered;
    FanApplyPwm();
}

//*****************************************************************************
// PGN 127500 Load Controller Connection State/Control
// 8 single bytes: Sequence ID, Connection ID, State, Status,
// Operational Status & Control, PWM Duty Cycle, TimeON, TimeOFF
//*****************************************************************************
void FanHandleN2kMsg(const tN2kMsg &N2kMsg) {
    if (N2kMsg.PGN != 127500L || N2kMsg.DataLen < 6) return;

    int Index = 1;
    uint8_t ConnectionId = N2kMsg.GetByte(Index);
    if (ConnectionId != FAN_CONNECTION_ID) return;
    Index = 5;
    uint8_t Duty = N2kMsg.GetByte(Index);
    if (Duty > 100) return; // 0xFF = not available

    if (Duty != FanCommandedDuty) Serial.printf("Fan duty -> %d%%\n", Duty);
    bool WasFailsafe = FanFailsafe();
    FanCommandedDuty = Duty;
    TimeSinceFanCommand = 0;
    if (WasFailsafe) Serial.println("Fan duty received from network, leaving failsafe");
    FanApplyPwm();
}

static void SendFanStatus127500(tNMEA2000 &NMEA2000) {
    tN2kMsg N2kMsg;
    N2kMsg.SetPGN(127500L);
    N2kMsg.Priority = 6;
    N2kMsg.AddByte(0xff);                          // Sequence ID: not used
    N2kMsg.AddByte(FAN_CONNECTION_ID);
    N2kMsg.AddByte(FanPowered ? 1 : 0);            // State: fan power relay
    N2kMsg.AddByte(FanFailsafe() ? 1 : 0);         // Status: 1 = failsafe full speed
    N2kMsg.AddByte(0xff);                          // Operational Status & Control: not used
    N2kMsg.AddByte(FanPowered ? FanEffectiveDuty() : 0); // PWM Duty Cycle (%) actually applied
    N2kMsg.AddByte(0xff);                          // TimeON: not used
    N2kMsg.AddByte(0xff);                          // TimeOFF: not used
    NMEA2000.SendMsg(N2kMsg);
}

void FanUpdate(tNMEA2000 &NMEA2000) {
    // Measure RPM over the time since the last update
    double Seconds = TimeSinceFanUpdate / 1000.0;
    TimeSinceFanUpdate = 0;
    for (uint8_t i = 0; i < NumberOfFans; i++) {
        int16_t Pulses = 0;
        pcnt_get_counter_value(FanPcntUnits[i], &Pulses);
        pcnt_counter_clear(FanPcntUnits[i]);
        FanRpm[i] = Seconds > 0 ? Pulses / (double)FAN_PULSES_PER_REV / Seconds * 60.0 : 0;
    }

    // Re-apply in case the command timed out since the last update
    FanApplyPwm();

    tN2kMsg N2kMsg;
    for (uint8_t i = 0; i < NumberOfFans; i++) {
        SetN2kEngineParamRapid(N2kMsg, FanEngineInstances[i], FanRpm[i]);
        NMEA2000.SendMsg(N2kMsg);
    }
    SendFanStatus127500(NMEA2000);

    Serial.printf("Fans: power %s, duty %d%%%s, RPM %.0f / %.0f\n",
                  FanPowered ? "ON" : "OFF",
                  FanPowered ? FanEffectiveDuty() : 0,
                  FanFailsafe() ? " (failsafe)" : "",
                  FanRpm[0], FanRpm[1]);
}
