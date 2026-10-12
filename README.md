# ESP32 NMEA2000 Switch Bank Controller

An ESP32-based NMEA 2000 switch bank controller that manages 4 relay outputs via CAN bus communication. This device listens for PGN 127502 (switch bank control) messages and broadcasts PGN 127501 (switch bank status) messages to maintain synchronization with Marine MFD displays.

## Features

- 4-channel relay control via NMEA2000 CAN bus
- Stern light as a plain switch; COLREG coupling to sidelights and red over green done by a Pi plugin
- Engine room fan speed control (PGN 127500) with RPM feedback (PGN 127488) and failsafe
- Periodic status broadcasts for MFD synchronization
- Automatic CAN bus recovery from bus-off conditions
- Watchdog-based system recovery if CAN communication fails

## Navigation Lights

The stern light is a plain switch of this board (bank 3, switch 3). The COLREG logic lives on the Pi in the [`signalk-nav-lights`](signalk-nav-lights/) Signal K plugin, so the board knows nothing about other lights:

| Light | Switched by |
|---|---|
| Red over green, masthead (COLREG 25(c)) | C-Zone bank 1 switch 4 |
| Deck level sidelights | kiosk "Nav lights" = switch bank 4 switch 1 |
| Stern light | this board, bank 3 switch 3, set by the plugin |
| Anchor light | C-Zone bank 1 switch 5 (not touched) |
| Motor light | C-Zone bank 1 switch 6 (not touched) |

The plugin:

- keeps the **stern light on while the sidelights or red over green are on**
- switches the **sidelights on with red over green**, keeps them on while it is on, and **restores them** to how they were when red over green goes off
- reads C-Zone from PGN 65284 (canboatjs decodes it as "CZone Circuit Status": `dipswitch` = bank, `bitmap` bit n-1 = switch n)
- re-sends a command until the bank's 127501 status agrees, so the stern light comes back after the board reboots
- publishes `electrical.navigationLights.redOverGreen`, `.sidelights`, `.sternLight`

Deploy: copy the folder to `~/.signalk/node_modules/` on the Pi and restart Signal K.

## Engine Room Fans

Two Orion OD1238-48HBVXC10A 4-wire PWM fans, mounted in series (one blowing into the other), cool the electric motor compartment. The Pi decides how fast they run, e.g. from `propulsion.port.oilTemperature`; this board only executes and reports, so there is no hard coupling to the engine sensors.

- **Power**: relay 1 (GPIO 13), switched with PGN 127502 (bank 3, switch 1) as before
- **Speed**: PGN 127500 duty cycle command from the network
- **Feedback**: RPM of each fan (PGN 127488) and the applied duty (PGN 127500)

### Wiring

| Fan wire | Connection |
|---|---|
| Red (DC+) / Black (GND) | 48V supply via relay 1 |
| Blue (PWM), both fans | GPIO 25 |
| White (tach), fan 1 | GPIO 26, 1kΩ pull-up to 3.3V |
| White (tach), fan 2 | GPIO 27, 1kΩ pull-up to 3.3V |

Never pull the tach lines up to more than 3.3V. The tach inputs use the ESP32 pulse counter with a glitch filter, so short spikes (e.g. PWM crosstalk in the cable) are ignored.

### Speed command (Pi → board): PGN 127500

PGN 127500 "Load Controller Connection State/Control" is 8 single bytes. The board only looks at two of them:

| Byte | Field | Value |
|---|---|---|
| 1 | Connection ID | `1` (the fans) |
| 5 | PWM Duty Cycle | `0`–`100` (%) |

All other bytes are ignored (send `0xFF`). The Pi must repeat the command periodically, e.g. every 5 seconds.

**Failsafe**: the fans run at **100%** when no duty has been received since boot, or none for **30 seconds**. This matches the fans' own behaviour with a disconnected PWM wire. While relay 1 is off, the PWM output is held low so unpowered fans aren't driven.

### Feedback (board → Pi), every second

| PGN | Content | Signal K path |
|---|---|---|
| 127488 Engine Parameters, Rapid Update, instance 2 | Fan 1 RPM | `propulsion.2.revolutions` (Hz = RPM / 60) |
| 127488 Engine Parameters, Rapid Update, instance 3 | Fan 2 RPM | `propulsion.3.revolutions` (Hz = RPM / 60) |
| 127500, Connection ID 1 | State (byte 2): relay 1 on = 1; Status (byte 3): 1 = failsafe; PWM Duty (byte 5): duty actually applied | not mapped by Signal K, read the raw N2K data |

With the fans in series, a large difference between the two RPM readings means one fan is failing or blocked.

### Pi side: `signalk-engine-room-fans` plugin

The control logic lives in a small Signal K plugin on the Pi, kept in [`signalk-engine-room-fans/`](signalk-engine-room-fans/). It reads `propulsion.port.oilTemperature` and:

- switches relay 1 on at `fansOnTemp` (default 25 °C) and off below `fansOffTemp` (22 °C)
- while on, sends a duty every 5 s: `minDuty` (30 %) at `fansOnTemp`, rising linearly to 100 % at `fullSpeedTemp` (45 °C), to keep the electric motor below 50 °C
- re-sends the relay command until the 127501 status agrees, so the fans come back after a board reboot
- switches the fans on and stops sending duty if the temperature is stale for 60 s, so the board's 100 % failsafe takes over
- **manual mode**: switching the fans on by hand (kiosk "Blower", 127502 bank 3 switch 1 ON) while they are off runs them at `manualDuty` (50 %), or faster if the curve asks for more, until switched off. Switching off ends manual mode, but above `fansOnTemp` the fans stay on
- publishes `electrical.fans.engineRoom.*`: `state`, `mode` (auto/manual), `commandedDutyCycle`, and decoded from the board's 127500 status `dutyCycle`, `failsafe`, `powered`

All thresholds are configurable in the Signal K admin UI (Server → Plugin Config → Engine room fans).

Deploy: copy the folder to `~/.signalk/node_modules/` on the Pi and restart Signal K.

## Hardware Configuration

### Relay Pin Mapping

The firmware controls 4 relays mapped to specific ESP32 GPIO pins:

| Relay | GPIO | Purpose |
|-------|------|---------|
| 1 | 13 | Engine room fans power |
| 2 | 5 | Autopilot |
| 3 | 12 | Stern light (set by the `nav-lights` Pi plugin) |
| 4 | 23 | Spare (not wired yet) |

### CAN Bus Connections

- **CAN RX**: GPIO 34
- **CAN TX**: GPIO 32
- **LED Indicator**: GPIO 2 (built-in LED, toggles on CAN message reception)

### Stern Light Control Circuit

The stern light (GPIO 12) uses an optocoupler-isolated high-side switch. GPIO HIGH lights the optocoupler LED, its transistor pulls the BTS6163D input to load ground, and that switches the light **on**:

```
ESP32 GPIO 12 ──┬── 220Ω ──┬─→ SFH615A Pin 1 (Anode)
                │          │
                │          └─→ SFH615A Pin 2 (Cathode) ── GND_ESP32
                │
                └─→ 1kΩ ──→ GND_ESP32

12V Supply ─────→ BTS6163D Pin 1&5 (Vin, shorted together)

SFH615A Pin 3 ──→ BTS6163D Pin 2 (Input)
SFH615A Pin 4 ──→ GND_LOAD (connected to BTS6163D Pin 3)

BTS6163D Pin 4 ─→ Stern Light (+12V side)
Stern Light GND ─→ GND_LOAD

BTS6163D Pin 7 ─→ 1kΩ ─→ GND_LOAD (current sense)
```

#### Components Required

| Component | Part Number | Purpose |
|-----------|-------------|---------|
| Optocoupler | SFH615A | 5300V electrical isolation |
| High-Side Switch | BTS6163D PROFET | Smart power switch with protection |
| Resistor 1 | 220Ω | Optocoupler LED current limiting |
| Resistor 2 | 1kΩ (GPIO) | Pull-down for stable LOW state |
| Resistor 3 | 1kΩ (CS) | Current sense feedback |

#### Important Notes

- **BTS6163D Pins 1 & 5 must be shorted together** and connected to +12V supply
- Ground separation: ESP32 ground (GND_ESP32) and load ground (GND_LOAD) are isolated by the optocoupler
- **Logic**: GPIO HIGH = stern light ON, GPIO LOW = OFF, like all other relays
- BTS6163D provides overcurrent, thermal, and overvoltage protection

## Build and Deployment

This project uses PlatformIO for building and deployment.

### Prerequisites

- PlatformIO installed and in PATH
- ESP32 connected via USB

### Commands

```bash
# Build the firmware
pio run

# Upload to ESP32
pio run --target upload

# Monitor serial output
pio device monitor

# Build, upload, and monitor (complete workflow)
pio run --target upload && pio device monitor

# Clean build artifacts
pio run --target clean
```

### First-Time Setup

If PlatformIO is installed via ESP-IDF Python environment:

```bash
# Add to PATH (add to ~/.bashrc for persistence)
export PATH=$PATH:~/.espressif/python_env/idf5.2_py3.12_env/bin
```

## Configuration

### Device Settings

Edit `src/main.cpp` to modify device configuration:

```cpp
// Device identity
#define BinaryDeviceInstance 0x03   // Instance for PGN 127501
#define SwitchBankInstance 0x03     // Instance for PGN 127502
#define NumberOfSwitches 4          // Number of relays

#define FAN_POWER_SWITCH 1          // Relay switching the engine room fans

// Watchdog and recovery
#define RECOVERY_RETRY_MS 1000      // CAN bus recovery retry interval
#define MAX_RX_WAIT_TIME_MS 30000   // Reboot if no CAN messages for 30 seconds
#define CzUpdatePeriod127501 10000  // Status broadcast interval (10 seconds)
```

### Relay Pin Mapping

To change relay pin assignments, modify the `CzRelayPinMap` array:

```cpp
uint8_t CzRelayPinMap[NumberOfSwitches] = { 13, 5, 12, 23 };
```

## Technical Details

### NMEA2000 Messages

The firmware handles these PGN messages:

1. **PGN 127502** (Switch Bank Control) - Incoming
   - Receives switch change requests from MFD
   - Only processes messages matching configured device instance
   - Sets each addressed relay to the commanded state (ON/OFF); switches marked unavailable are left alone
   - Broadcasts updated status

2. **PGN 127501** (Switch Bank Status) - Outgoing
   - Periodic broadcast every 10 seconds as "heartbeat"
   - Sent immediately after any switch state change
   - Maintains synchronization with MFD displays

3. **PGN 127500 / 127488** - fan speed and RPM, see [Engine Room Fans](#engine-room-fans)

### State Management

`CzSwitchState[]` holds the actual state of each relay. All relay writes go through `WriteRelay()`, and PGN 127501 is built from that array, so the reported status always matches the relays.

### Event Loop (ReactESP)

- **1ms interval**: Parse NMEA messages and poll CAN bus status
- **100ms interval**: Check for CAN RX timeout (triggers watchdog reboot if no messages for 30s)
- **1 second interval**: Debug output
- **10 second interval**: PGN 127501 status heartbeat

### CAN Bus Recovery

The firmware implements automatic recovery from CAN bus-off conditions:

1. Detects bus-off by reading SJA1000-compatible controller registers
2. Attempts recovery every 1 second by:
   - Aborting pending transmissions
   - Resetting error counters
   - Releasing reset mode
3. If recovery fails and no CAN messages received for 30 seconds, triggers watchdog reboot

## Troubleshooting

### USB Device Not Detected

**Symptoms**: `pio device monitor` shows no device on `/dev/ttyUSB0`

**Solutions**:
1. Check USB cable - ensure it's a data cable, not power-only
2. Disconnect and reconnect ESP32
3. Verify device appears: `ls /dev/ttyUSB*`
4. Check permissions: `sudo usermod -a -G dialout $USER` (logout/login required)

### Upload Fails: Port Busy

**Symptoms**: Upload fails with "port busy" or "permission denied"

**Solutions**:
1. Close any open serial monitors
2. Kill background processes: `killall -9 pio`
3. Disconnect and reconnect ESP32
4. Retry upload

### No CAN Messages Received

**Symptoms**: Serial output shows `RX: 0` continuously

**Solutions**:
1. Verify CAN bus wiring (RX=GPIO34, TX=GPIO32)
2. Check CAN bus termination resistors (120Ω at both ends)
3. Confirm NMEA2000 network is powered and active
4. Verify CAN H/L are not swapped

### Stern Light Not Responding

**Symptoms**: Sidelights or red over green are on, but the stern light isn't

**Solutions**:
1. Check `electrical.switches.bank.3.3.state` in Signal K: if it is 1, the logic works and the problem is in the circuit
2. Check the `Navigation lights` plugin status in the Signal K admin UI
3. Check wiring between GPIO 12 and optocoupler
4. Verify BTS6163D pins 1&5 are shorted and connected to +12V
5. Confirm ground separation (GND_ESP32 vs GND_LOAD)
6. Test optocoupler: Measure voltage at SFH615A pin 3 (should be ~0V when GPIO 12 is HIGH)

### Inverted Relay Logic

**Symptoms**: Relay turns on when it should be off and vice versa

**Solutions**:
- All relays are active HIGH. For a driver circuit that switches on with LOW, invert that relay in `WriteRelay()`

## Dependencies

- **NMEA2000** 4.24.1: first registry release that includes C-Zone parsing support ([PR #469](https://github.com/ttlappalainen/NMEA2000/pull/469))
- **NMEA2000_esp32** 1.0.3: ESP32-specific NMEA 2000 implementation
- **ReactESP** 2.1.0: Event-driven programming framework
- **elapsedMillis** 1.0.6: Efficient time tracking library

All dependencies, including the platform, are pinned to exact versions so rebuilds reproduce the firmware on the boat:

```ini
platform = espressif32@6.12.0
lib_deps =
    mairas/ReactESP@2.1.0
    ttlappalainen/NMEA2000-library@4.24.1
    ttlappalainen/NMEA2000_esp32@1.0.3
    pfeerick/elapsedMillis@1.0.6
```

Don't bump `espressif32` to 7.x or ReactESP to 3.x without porting: espressif32 7.x moves to Arduino core 3 / ESP-IDF 5, which breaks the CAN driver and the bus-off recovery code, and ReactESP 3.x changes its API.

## License

This project uses libraries with various open-source licenses. Please refer to individual library licenses for details.

## Credits

- C-Zone protocol reverse-engineered from C-Zone Contact 6 Plus switch bank
- NMEA2000 C-Zone support contributed via [PR #469](https://github.com/ttlappalainen/NMEA2000/pull/469)
