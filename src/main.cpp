#include <Arduino.h>
#include <NMEA2000_esp32.h>
#include <N2kMessages.h>
#include <N2kMsg.h>
#include <N2kCZone.h>
#include <ReactESP.h>
#include <esp_task_wdt.h>
#include "elapsedMillis.h"

using namespace reactesp;

ReactESP app;

#define CAN_RX_PIN GPIO_NUM_34
#define CAN_TX_PIN GPIO_NUM_32

#define RECOVERY_RETRY_MS 1000  // How long to attempt CAN bus recovery
#define MAX_RX_WAIT_TIME_MS 30000 // Time after which we should reboot if we haven't received any CAN messages
#define CzUpdatePeriod127501 10000
#define BinaryDeviceInstance 0x03 // Instance of 127501 switch state message
#define SwitchBankInstance 0x03   //Instance of 127502 change switch state message
#define NumberOfSwitches 8   // change to 4 for bit switch bank

// C-Zone piggyback configuration
#define CZONE_BANK_INSTANCE 1       // Bank instance for navigation and motor lights
#define CZONE_NAV_LIGHT_SWITCH 4    // Switch 4 on C-Zone bank 1 (navigation light)
#define CZONE_MOTOR_LIGHT_SWITCH 6  // Switch 6 on C-Zone bank 1 (motor light)
#define STERN_LIGHT_RELAY_INDEX 2   // Index in CzRelayPinMap (3rd relay, GPIO 12)

// List here messages your device will transmit.
const unsigned long TransmitMessages[] PROGMEM = { 127501L, 127502L, 0 };

// Global Variables
uint8_t CzRelayPinMap[] = { 13, 5, 12, 23, 18, 26, 27, 36 }; // esp32 pins driving relays i.e CzRelayPinMap[0] returns the pin number of Relay 1
tN2kBinaryStatus CzBankStatus;
uint8_t CzSwitchState1 = 0;
uint8_t CzSwitchState2 = 0;
uint8_t CzMfdDisplaySyncState1 = 0;
uint8_t CzMfdDisplaySyncState2 = 0;

tNMEA2000 *nmea2000;

int num_n2k_messages = 0;
elapsedMillis time_since_last_can_rx = 0;
String can_state;

void ToggleLed() {
  static bool led_state = false;
  digitalWrite(LED_BUILTIN, led_state);
  led_state = !led_state;
}

void SetCZoneSwitchState127501(unsigned char DeviceInstance) {

    tN2kMsg N2kMsg;
    tN2kBinaryStatus BankStatus;
    N2kResetBinaryStatus(BankStatus);
    BankStatus = (BankStatus & CzMfdDisplaySyncState2) << 8; //
    BankStatus = BankStatus | CzMfdDisplaySyncState1;
    BankStatus = BankStatus | 0xffffffffffff0000ULL;
    SetN2kPGN127501(N2kMsg, DeviceInstance, BankStatus);
    nmea2000->SendMsg(N2kMsg);
}

void SetCZoneSwitchChangeRequest127502(unsigned char DeviceInstance, uint8_t SwitchIndex, bool ItemStatus)
{
    tN2kMsg N2kMsg;
    N2kSetStatusBinaryOnStatus(CzBankStatus, ItemStatus ? N2kOnOff_On : N2kOnOff_Off, SwitchIndex);
    //send out to other N2k switching devices on network ( pgn 127502 )
    SetN2kPGN127502(N2kMsg, SwitchBankInstance, CzBankStatus);
    nmea2000->SendMsg(N2kMsg);
}

//*****************************************************************************
// Change the state of the relay requested and broadcast change to other N2K switching devices
//*****************************************************************************

void SetChangeSwitchState(uint8_t SwitchIndex, bool ItemStatus) {

    // Set or reset the relay
    if (ItemStatus) {
        Serial.println("writing pin high");
        digitalWrite(CzRelayPinMap[SwitchIndex - 1], HIGH); // adjust SwitchIndex to CzRelayPinMap value and set or reset
    } else {
        Serial.println("writing pin low");
        digitalWrite(CzRelayPinMap[SwitchIndex - 1], LOW);
    }
    //send out change and status to other N2k devices on network
    SetCZoneSwitchState127501(BinaryDeviceInstance);
    SetCZoneSwitchChangeRequest127502(SwitchBankInstance, SwitchIndex, ItemStatus);
}

//*****************************************************************************
void ParseCZoneSwitchState(const tN2kMsg& N2kMsg) {
    // Parse C-Zone PGNs 65284 and 65301 for switch state changes
    if (N2kMsg.PGN != 65284L && N2kMsg.PGN != 65301L) return;

    tN2kCZoneSwitchStatus Status;
    bool parsed = false;

    if (N2kMsg.PGN == 65284L) {
        parsed = ParseN2kPGN65284CZoneSwitchStatus(N2kMsg, Status);
    } else if (N2kMsg.PGN == 65301L) {
        parsed = ParseN2kPGN65301CZoneSwitchStatus(N2kMsg, Status);
    }

    if (!parsed || !Status.SwitchBitsValid) {
        return;  // Not a valid switch status message
    }

    // PIGGYBACK LOGIC: Monitor nav light (switch 4) and motor light (switch 6)
    // Stern light should be ON if EITHER switch is ON
    if (Status.BankInstance == CZONE_BANK_INSTANCE) {
        bool navLightOn = N2kCZoneIsSwitchOn(Status, CZONE_NAV_LIGHT_SWITCH);
        bool motorLightOn = N2kCZoneIsSwitchOn(Status, CZONE_MOTOR_LIGHT_SWITCH);
        bool sternLightShouldBeOn = navLightOn || motorLightOn;

        // Control stern light relay based on nav OR motor light state
        // INVERTED logic: LOW = ON (optocoupler + high-side switch)
        digitalWrite(CzRelayPinMap[STERN_LIGHT_RELAY_INDEX], sternLightShouldBeOn ? LOW : HIGH);

        Serial.printf("PIGGYBACK: Nav=%s Motor=%s -> Stern light %s (GPIO %d = %s)\n",
                      navLightOn ? "ON" : "OFF",
                      motorLightOn ? "ON" : "OFF",
                      sternLightShouldBeOn ? "ON" : "OFF",
                      CzRelayPinMap[STERN_LIGHT_RELAY_INDEX],
                      sternLightShouldBeOn ? "LOW" : "HIGH");
    }
}

void ParseN2kPGN127502(const tN2kMsg& N2kMsg) {
    tN2kOnOff State;
    unsigned char ChangeIndex;
    int Index = 0;
    uint8_t DeviceBankInstance = N2kMsg.GetByte(Index);
    if (N2kMsg.PGN != 127502L || DeviceBankInstance  != BinaryDeviceInstance) return; //Nothing to see here
    uint16_t BankStatus = N2kMsg.Get2ByteUInt (Index);
    //Serial.println(BankStatus);
    for (ChangeIndex = 0; ChangeIndex < NumberOfSwitches; ChangeIndex++) {

        State = (tN2kOnOff)(BankStatus & 0x03);
        if (State != N2kOnOff_Unavailable) break; // index (0 to 7) found for switch and state
        BankStatus >>= 2;
    }
    Serial.println(ChangeIndex );
    Serial.println(State);

    switch (ChangeIndex) {

      case 0x00:  CzSwitchState1 ^= 0x01; // toggle state of the of switch bit
          CzMfdDisplaySyncState1 ^= 0x01; // toggle state of the of switch bit for MDF display sync
          SetChangeSwitchState(1, CzSwitchState1 & 0x01); // send the change out
          break;

      case 0x01:  CzSwitchState1 ^= 0x02;
          CzMfdDisplaySyncState1 ^= 0x04;
          SetChangeSwitchState(2, CzSwitchState1 & 0x02); // send the change out
          break;

      case 0x02:  CzSwitchState1 ^= 0x04;
          CzMfdDisplaySyncState1 ^= 0x10;
          SetChangeSwitchState(3, CzSwitchState1 & 0x04); // send the change out
          break;

      case 0x03:  CzSwitchState1 ^= 0x08;
          CzMfdDisplaySyncState1 ^= 0x40;
          SetChangeSwitchState(4, CzSwitchState1 & 0x08); // send the change out
          break;
          // second 4 switches 
      case 0x04:  CzSwitchState2 ^= 0x01; // state of the four switches
          CzMfdDisplaySyncState2 ^= 0x01; // for MDF display sync
          SetChangeSwitchState(5, CzSwitchState2 & 0x01); // send the change out
          break;
      case 0x05:  CzSwitchState2 ^= 0x02;
          CzMfdDisplaySyncState2 ^= 0x04;
          SetChangeSwitchState(6, CzSwitchState2 & 0x02); // send the change out
          break;
      case 0x06:  CzSwitchState2 ^= 0x04;
          CzMfdDisplaySyncState2 ^= 0x10;
          SetChangeSwitchState(7, CzSwitchState2 & 0x04); // send the change out
          break;
      case 0x07:  CzSwitchState2 ^= 0x08;
          CzMfdDisplaySyncState2 ^= 0x40;
          SetChangeSwitchState(8, CzSwitchState2 & 0x08); // send the change out
    }
}

void HandleStreamN2kMsg(const tN2kMsg &message) {
  // N2kMsg.Print(&Serial);
  num_n2k_messages++;
  time_since_last_can_rx = 0;
  ToggleLed();
  ParseCZoneSwitchState(message);  // Parse C-Zone messages
  ParseN2kPGN127502(message);       // Parse standard NMEA switch bank messages
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

// send periodic updates to maintain sync and as a "heartbeat" to the MFD

void SendN2k(void)
{
    static unsigned long CzUpdate127501 = millis();


    if (CzUpdate127501 + CzUpdatePeriod127501 < millis()) {
        CzUpdate127501 = millis();
        SetCZoneSwitchState127501(BinaryDeviceInstance);
    }
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

  // Initialize relay outputs
  pinMode(CzRelayPinMap[0], OUTPUT);
  digitalWrite(CzRelayPinMap[0], LOW);

  pinMode(CzRelayPinMap[1], OUTPUT);
  digitalWrite(CzRelayPinMap[1], LOW);

  // Initialize stern light relay (used for C-Zone piggyback)
  // Start with HIGH (OFF state due to inverted logic: optocoupler + high-side switch)
  pinMode(CzRelayPinMap[STERN_LIGHT_RELAY_INDEX], OUTPUT);
  digitalWrite(CzRelayPinMap[STERN_LIGHT_RELAY_INDEX], HIGH);

  Serial.printf("Stern light relay initialized on GPIO %d (relay index %d) - inverted logic\n",
                CzRelayPinMap[STERN_LIGHT_RELAY_INDEX], STERN_LIGHT_RELAY_INDEX + 1);


  // initialize initial switch state
  nmea2000 = new tNMEA2000_esp32(CAN_TX_PIN, CAN_RX_PIN);
  N2kResetBinaryStatus(CzBankStatus);

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

    SendN2k();
  });
}


void loop() { app.tick(); }