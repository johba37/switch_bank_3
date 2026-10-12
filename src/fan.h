#pragma once

#include <NMEA2000.h>
#include <N2kMsg.h>

// Engine room fans: two 4-wire PWM fans (Orion OD1238-48HBVXC10A) mounted in
// series (one blowing into the other). Power is switched by relay 1, speed is
// commanded over NMEA 2000 (PGN 127500) and each fan's RPM is reported back
// (PGN 127488). See README.md for the message format.

#define FAN_UPDATE_PERIOD_MS 1000   // RPM measurement window and report interval

void FanSetup();
void FanSetPowered(bool Powered);               // follow fan power relay state
void FanHandleN2kMsg(const tN2kMsg &N2kMsg);    // 127500 speed commands
void FanUpdate(tNMEA2000 &NMEA2000);            // call every FAN_UPDATE_PERIOD_MS
