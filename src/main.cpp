#include <Arduino.h>
#include <NMEA2000_esp32.h>
#include <N2kMessages.h>
#include <N2kMsg.h>
#include <ReactESP.h>
#include <esp_task_wdt.h>
#include "elapsedMillis.h"
#include "fan.h"

using namespace reactesp;

ReactESP app;

#define CAN_RX_PIN GPIO_NUM_34
#define CAN_TX_PIN GPIO_NUM_32

#define RECOVERY_RETRY_MS 1000  // How long to attempt CAN bus recovery
#define MAX_RX_WAIT_TIME_MS 30000 // Time after which we should reboot if we haven't received any CAN messages
#define CzUpdatePeriod127501 10000
#define BinaryDeviceInstance 0x03 // Instance of 127501 switch state message
#define SwitchBankInstance 0x03   //Instance of 127502 change switch state message
#define NumberOfSwitches 4

#define FAN_POWER_SWITCH 1          // Relay 1 (GPIO 13): engine room fan power, speed see fan.cpp

// List here messages your device will transmit.
const unsigned long TransmitMessages[] PROGMEM = { 127488L, 127500L, 127501L, 127502L, 0 };

// Global Variables
uint8_t CzRelayPinMap[NumberOfSwitches] = { 13, 5, 12, 23 }; // esp32 pins driving relays i.e CzRelayPinMap[0] returns the pin number of Relay 1
bool CzSwitchState[NumberOfSwitches] = { false }; // current relay states, CzSwitchState[0] is Relay 1

tNMEA2000 *nmea2000;

int num_n2k_messages = 0;
elapsedMillis time_since_last_can_rx = 0;
String can_state;

void ToggleLed() {
  static bool led_state = false;
  digitalWrite(LED_BUILTIN, led_state);
  led_state = !led_state;
}

//*****************************************************************************
// Drive a relay (SwitchIndex 1..NumberOfSwitches) and remember its state
//*****************************************************************************
void WriteRelay(uint8_t SwitchIndex, bool On) {
    digitalWrite(CzRelayPinMap[SwitchIndex - 1], On ? HIGH : LOW);
    CzSwitchState[SwitchIndex - 1] = On;
    if (SwitchIndex == FAN_POWER_SWITCH) FanSetPowered(On);
}

// Broadcast the actual state of all relays (pgn 127501)
void SendSwitchBankStatus127501() {
    tN2kMsg N2kMsg;
    tN2kBinaryStatus BankStatus;
    N2kResetBinaryStatus(BankStatus);
    for (uint8_t i = 1; i <= NumberOfSwitches; i++) {
        N2kSetStatusBinaryOnStatus(BankStatus, CzSwitchState[i - 1] ? N2kOnOff_On : N2kOnOff_Off, i);
    }
    SetN2kPGN127501(N2kMsg, BinaryDeviceInstance, BankStatus);
    nmea2000->SendMsg(N2kMsg);
}

// Send out a change of one switch to other N2k switching devices on network (pgn 127502)
void SendSwitchChange127502(uint8_t SwitchIndex, bool On) {
    tN2kMsg N2kMsg;
    tN2kBinaryStatus BankStatus;
    N2kResetBinaryStatus(BankStatus);
    N2kSetStatusBinaryOnStatus(BankStatus, On ? N2kOnOff_On : N2kOnOff_Off, SwitchIndex);
    SetN2kPGN127502(N2kMsg, SwitchBankInstance, BankStatus);
    nmea2000->SendMsg(N2kMsg);
}

//*****************************************************************************
// Set the relays to the states requested in a 127502 switch bank control message
//*****************************************************************************
void HandleSwitchBankControl(const tN2kMsg& N2kMsg) {
    unsigned char TargetBankInstance;
    tN2kBinaryStatus BankStatus;
    if (!ParseN2kPGN127502(N2kMsg, TargetBankInstance, BankStatus) || TargetBankInstance != SwitchBankInstance) return; //Nothing to see here

    bool addressed = false;
    for (uint8_t i = 1; i <= NumberOfSwitches; i++) {
        tN2kOnOff State = N2kGetStatusOnBinaryStatus(BankStatus, i);
        if (State != N2kOnOff_On && State != N2kOnOff_Off) continue; // switch not part of this command
        addressed = true;
        bool On = (State == N2kOnOff_On);
        Serial.printf("Switch %d -> %s\n", i, On ? "ON" : "OFF");
        WriteRelay(i, On);
        SendSwitchChange127502(i, On);
    }
    if (addressed) SendSwitchBankStatus127501();
}

void HandleStreamN2kMsg(const tN2kMsg &message) {
  // N2kMsg.Print(&Serial);
  num_n2k_messages++;
  time_since_last_can_rx = 0;
  ToggleLed();
  HandleSwitchBankControl(message); // Parse standard NMEA switch bank messages
  FanHandleN2kMsg(message);         // Parse fan speed commands
}

void RecoverFromCANBusOff() {
  // This recovery routine first discussed in
  // https://www.esp32.com/viewtopic.php?t=5010 and also implemented in
  // https://github.com/wellenvogel/esp32-nmea2000
  static bool recovery_in_progress = false;
  static elapsedMillis recovery_timer;
  if (recovery_in_progress && recovery_timer < RECOVERY_RETRY_MS) {
    return;
  }
  recovery_in_progress = true;
  recovery_timer = 0;
  // Abort transmission
  MODULE_CAN->CMR.B.AT = 1;
  // read SR after write to CMR to settle register changes
  (void)MODULE_CAN->SR.U;

  // Reset error counters
  MODULE_CAN->TXERR.U = 127;
  MODULE_CAN->RXERR.U = 0;
  // Release Reset mode
  MODULE_CAN->MOD.B.RM = 0;
}

void PollCANStatus() {
  // CAN controller registers are SJA1000 compatible.
  // Bus status value 0 indicates bus-on; value 1 indicates bus-off.
  unsigned int bus_status = MODULE_CAN->SR.B.BS;

  switch (bus_status) {
    case 0:
      can_state = "RUNNING";
      break;
    case 1:
      can_state = "BUS-OFF";
      // try to automatically recover
      RecoverFromCANBusOff();
      break;
  }
}

void setup() {

  Serial.begin(115200);
  delay(100);

  // toggle the LED pin at rate of 1 Hz
  pinMode(LED_BUILTIN, OUTPUT);
  app.onRepeatMicros(1e6 / 1, []() { ToggleLed(); });

  FanSetup();

  // Initialize relay outputs, all off. Set the level before enabling the
  // output so no relay flashes on at boot.
  for (uint8_t i = 1; i <= NumberOfSwitches; i++) {
    WriteRelay(i, false);
    pinMode(CzRelayPinMap[i - 1], OUTPUT);
  }

  nmea2000 = new tNMEA2000_esp32(CAN_TX_PIN, CAN_RX_PIN);

  // setup the N2k parameters
  nmea2000->SetN2kCANSendFrameBufSize(250);
  nmea2000->SetN2kCANReceiveFrameBufSize(250);
  // Set product information
  nmea2000->SetProductInformation("00260001", 0001, "Switch Bank 3", "1.000 06/04/21", "My Yacht 8 Bit ");
  // Set device information
  nmea2000->SetDeviceInformation(260001, 140, 30, 717);
  nmea2000->SetMode(tNMEA2000::N2km_ListenAndNode, 169);
  nmea2000->SetForwardOwnMessages(false);  // do not echo own messages.
  nmea2000->ExtendTransmitMessages(TransmitMessages);
  nmea2000->SetMsgHandler(HandleStreamN2kMsg);
  nmea2000->Open();

  // No need to parse the messages at every single loop iteration; 1 ms will do
  app.onRepeat(1, []() {
    PollCANStatus();
    nmea2000->ParseMessages();
  });

  app.onRepeat(100, []() {
    if (time_since_last_can_rx > MAX_RX_WAIT_TIME_MS) {
      // No CAN messages received in a while; reboot
      esp_task_wdt_init(1, true);
      esp_task_wdt_add(NULL);
      while (true) {
        // wait for watchdog to trigger
      }
    }
  });

  app.onRepeat(1000, []() {
    Serial.printf("CAN: %s\n", can_state.c_str());
    Serial.printf("Uptime: %lu\n", millis() / 1000);
    Serial.printf("RX: %d\n", num_n2k_messages);
    num_n2k_messages = 0;
  });

  // measure and report fan RPM, apply fan failsafe
  app.onRepeat(FAN_UPDATE_PERIOD_MS, []() { FanUpdate(*nmea2000); });

  // send periodic updates to maintain sync and as a "heartbeat" to the MFD
  app.onRepeat(CzUpdatePeriod127501, []() { SendSwitchBankStatus127501(); });
}


void loop() { app.tick(); }