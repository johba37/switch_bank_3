# ESP32 NMEA2000 Switch Bank Controller

An ESP32-based NMEA 2000 switch bank controller that manages 4 relay outputs via CAN bus communication. This device listens for PGN 127502 (switch bank control) messages and broadcasts PGN 127501 (switch bank status) messages to maintain synchronization with Marine MFD displays.

## Features

- 4-channel relay control via NMEA2000 CAN bus
- C-Zone switch bank monitoring and piggyback control
- Automatic stern light activation based on navigation and motor lights
- Periodic status broadcasts for MFD synchronization
- Automatic CAN bus recovery from bus-off conditions
- Watchdog-based system recovery if CAN communication fails

## C-Zone Piggyback Functionality

This firmware monitors a C-Zone Contact 6 Plus switch bank (Bank 1) and automatically controls a stern light relay based on two C-Zone switches:

- **Navigation Light** (Bank 1, Switch 4): Controls mast navigation light
- **Motor Light** (Bank 1, Switch 6): Controls motor light

The stern light (relay 3, GPIO 12) is automatically turned **ON** when **either** the navigation light **OR** motor light is switched on. This provides automatic stern light control without requiring a separate switch.

### How It Works

1. The ESP32 listens to C-Zone proprietary PGNs (65284 and 65301) on the NMEA2000 network
2. Parses switch state messages from Bank 1
3. Monitors switches 4 (navigation) and 6 (motor)
4. Controls stern light relay with OR logic: `sternLight = navLight OR motorLight`
5. Uses inverted GPIO logic (LOW = ON) due to optocoupler + high-side switch circuit
6. Broadcasts PGN 127501 when the stern light changes, so the MFD shows its state

The stern light is passive: it only follows C-Zone. PGN 127502 commands for switch 3 are ignored, and the device answers them with its actual status so the MFD snaps back.

## Hardware Configuration

### Relay Pin Mapping

The firmware controls 4 relays mapped to specific ESP32 GPIO pins:

| Relay | GPIO | Purpose |
|-------|------|---------|
| 1 | 13 | Fan |
| 2 | 5 | Autopilot |
| 3 | **12** | **Stern Light (C-Zone Piggyback)** |
| 4 | 23 | Spare (not wired yet) |

### CAN Bus Connections

- **CAN RX**: GPIO 34
- **CAN TX**: GPIO 32
- **LED Indicator**: GPIO 2 (built-in LED, toggles on CAN message reception)

### Stern Light Control Circuit

The stern light relay (GPIO 12) uses an optocoupler-isolated high-side switch for safe load control:

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
- **Inverted logic**: GPIO LOW = Stern light ON, GPIO HIGH = OFF
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

// C-Zone piggyback configuration
#define CZONE_BANK_INSTANCE 1       // C-Zone bank to monitor
#define CZONE_NAV_LIGHT_SWITCH 4    // Navigation light switch number
#define CZONE_MOTOR_LIGHT_SWITCH 6  // Motor light switch number
#define STERN_LIGHT_SWITCH 3        // Relay driven only by C-Zone (stern light)

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

The firmware handles three types of PGN messages:

1. **PGN 127502** (Switch Bank Control) - Incoming
   - Receives switch change requests from MFD
   - Only processes messages matching configured device instance
   - Sets each addressed relay to the commanded state (ON/OFF); switches marked unavailable are left alone
   - Commands for the stern light (switch 3) are ignored
   - Broadcasts updated status

2. **PGN 127501** (Switch Bank Status) - Outgoing
   - Periodic broadcast every 10 seconds as "heartbeat"
   - Sent immediately after any switch state change
   - Maintains synchronization with MFD displays

3. **PGN 65284/65301** (C-Zone Switch Status) - Incoming
   - Proprietary C-Zone switch state messages
   - Parsed to extract navigation and motor light states
   - Triggers automatic stern light control

### State Management

`CzSwitchState[]` holds the actual state of each relay. All relay writes go through `WriteRelay()`, which handles the stern light's inverted logic, and PGN 127501 is built from that array, so the reported status always matches the relays.

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

**Symptoms**: C-Zone switches toggle but stern light doesn't activate

**Solutions**:
1. Check wiring between GPIO 12 and optocoupler
2. Verify BTS6163D pins 1&5 are shorted and connected to +12V
3. Confirm ground separation (GND_ESP32 vs GND_LOAD)
4. Test optocoupler: Measure voltage at SFH615A pin 3 (should be ~0V when GPIO 12 is LOW)
5. Check serial output for debug messages: `PIGGYBACK: Nav=... Motor=... -> Stern light...`

### Inverted Relay Logic

**Symptoms**: Relay turns on when it should be off and vice versa

**Solutions**:
- For stern light (GPIO 12): Logic is intentionally inverted (LOW = ON) due to optocoupler circuit
- For other relays: Modify logic in `WriteRelay()` if using different driver circuits

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
