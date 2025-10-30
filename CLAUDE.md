# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is an ESP32-based NMEA 2000 switch bank controller that manages 8 relay outputs via CAN bus communication. The device listens for PGN 127502 (switch bank control) messages and broadcasts PGN 127501 (switch bank status) messages to maintain synchronization with Marine MFD displays.

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
- **Relay Pin Mapping** (src/main.cpp:32): Array `CzRelayPinMap[]` defines which GPIO pins control which relays
  - Relay 1-8 are mapped to GPIOs: 13, 5, 12, 23, 18, 26, 27, 36
- **Built-in LED**: GPIO 2 (toggles on CAN message reception)

## Architecture

### NMEA 2000 Communication

The firmware implements two PGN message types:

1. **PGN 127502** (incoming): Switch bank control commands
   - Parsed in `ParseN2kPGN127502()` to detect switch state change requests
   - Only processes messages matching `BinaryDeviceInstance` (0x03)

2. **PGN 127501** (outgoing): Switch bank status
   - Sent periodically every 10 seconds as a "heartbeat" via `SendN2k()`
   - Also sent immediately after any switch state change

### State Management

The device maintains dual state tracking:
- `CzSwitchState1/2`: Internal relay states (bits 0-7)
- `CzMfdDisplaySyncState1/2`: MFD display synchronization states (uses specific bit patterns for compatibility)

Switch indices map to specific bit patterns in the state bytes, with odd bit positions used for MFD sync (0x01, 0x04, 0x10, 0x40 pattern).

### Event Loop

Uses ReactESP for event-driven programming:
- 1ms interval: Parse NMEA messages and poll CAN bus status
- 100ms interval: Check for CAN RX timeout (reboots if no messages for 30s)
- 1s interval: Debug output and periodic status broadcasts

### CAN Bus Recovery

Implements automatic recovery from CAN bus-off state (src/main.cpp:162-183):
- Detects bus-off condition by reading SJA1000-compatible registers
- Attempts recovery every 1 second by resetting error counters
- If no CAN messages received within 30 seconds, triggers watchdog reboot

## Key Dependencies

- **NMEA2000_esp32**: ESP32-specific NMEA 2000 implementation
- **ReactESP**: Event-driven programming framework
- **N2kMessages**: NMEA 2000 message encoding/decoding

## Device Configuration

- Device Instance: 0x03 (used for both PGN 127501 and 127502)
- Number of Switches: 8 (configurable to 4 by changing `NumberOfSwitches`)
- Product Info: "Switch Bank 3", Version 1.000
- Device ID: 260001, Function: 140, Class: 30, Manufacturer: 717
