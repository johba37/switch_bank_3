# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is an ESP32-based NMEA 2000 switch bank controller that manages 4 relay outputs via CAN bus communication. The device listens for PGN 127502 (switch bank control) messages and broadcasts PGN 127501 (switch bank status) messages to maintain synchronization with Marine MFD displays. It also listens to C-Zone switch status (PGN 65284/65301) and drives the stern light (relay 3) when C-Zone bank 1 switch 4 (nav light) or switch 6 (motor light) is on.

## Build System

This project uses PlatformIO for building and deployment:

- **Build**: `pio run`
- **Upload to device**: `pio run --target upload`
- **Serial monitor**: `pio device monitor`
- **Clean build**: `pio run --target clean`
- **Upload and monitor**: `pio run --target upload && pio device monitor`

The default environment is `esp32dev` targeting an ESP32 development board.

## Hardware Configuration

- **CAN Bus Pins**: RX on GPIO 34, TX on GPIO 32
- **Relay Pin Mapping**: Array `CzRelayPinMap[]` in src/main.cpp defines which GPIO pins control which relays
  - Relay 1-4 are mapped to GPIOs: 13 (fan), 5 (autopilot), 12 (stern light), 23 (spare)
  - Relay 3 (stern light) uses inverted logic: LOW = ON (optocoupler + high-side switch)
- **Built-in LED**: GPIO 2 (toggles on CAN message reception)

## Architecture

### NMEA 2000 Communication

1. **PGN 127502** (incoming): Switch bank control commands
   - Handled in `HandleSwitchBankControl()`; only messages for `SwitchBankInstance` (0x03)
   - Sets each addressed relay to the commanded state (not a toggle)
   - Commands for relay 3 (`STERN_LIGHT_SWITCH`) are ignored: it follows C-Zone only

2. **PGN 127501** (outgoing): Switch bank status
   - Sent periodically every 10 seconds as a "heartbeat" via `SendN2k()`
   - Also sent immediately after any switch state change
   - Built from `CzSwitchState[]`, so it always reports the actual relay states

3. **PGN 65284/65301** (incoming): C-Zone switch status
   - Parsed in `ParseCZoneSwitchState()` via `N2kCZone.h` from the NMEA2000 library
   - Stern light = C-Zone bank 1 switch 4 (nav) OR switch 6 (motor)

### State Management

`CzSwitchState[]` holds the state of each relay. All relay writes go through `WriteRelay()`, which applies the stern light's inverted logic and updates the state array.

### Event Loop

Uses ReactESP for event-driven programming:
- 1ms interval: Parse NMEA messages and poll CAN bus status
- 100ms interval: Check for CAN RX timeout (reboots if no messages for 30s)
- 1s interval: Debug output and periodic status broadcasts

### CAN Bus Recovery

Implements automatic recovery from CAN bus-off state (`RecoverFromCANBusOff()`):
- Detects bus-off condition by reading SJA1000-compatible registers
- Attempts recovery every 1 second by resetting error counters
- If no CAN messages received within 30 seconds, triggers watchdog reboot

## Key Dependencies

All versions are pinned in platformio.ini. Don't bump espressif32 to 7.x (Arduino core 3 / IDF 5 breaks the CAN driver and bus-off recovery) or ReactESP to 3.x (API change) without porting.

- **NMEA2000-library 4.24.1**: message encoding/decoding, incl. C-Zone (`N2kCZone.h`)
- **NMEA2000_esp32**: ESP32-specific NMEA 2000 implementation
- **ReactESP 2.x**: Event-driven programming framework

## Device Configuration

- Device Instance: 0x03 (used for both PGN 127501 and 127502)
- Number of Switches: 4 (`NumberOfSwitches`)
- Product Info: "Switch Bank 3", Version 1.000
- Device ID: 260001, Function: 140, Class: 30, Manufacturer: 717
