# AirTag Raw Logger

An ESP32-based logger for capturing and studying Apple AirTag / Find My Bluetooth Low Energy advertisements.

The goal of this project is to collect the raw BLE advertisements emitted by an AirTag over an extended period of time and preserve the data for later analysis.

This is intentionally a **raw-data-first** project. The logger should capture the complete advertisement payload rather than interpreting or discarding fields that aren't currently understood.

## Project Goals

The primary goals are:

- Detect AirTag / Find My BLE advertisements.
- Capture every matching advertisement received during scanning.
- Preserve the complete raw advertisement payload.
- Record the Bluetooth address associated with each observation.
- Record RSSI.
- Record an accurate timestamp for every observation.
- Study address rotation and payload changes over time.
- Eventually operate unattended for weeks or longer.
- Store the collected data on a microSD card.
- Retain a local fallback copy if the SD card is unavailable.

The initial experiment uses a stationary AirTag that is separated from its owner's iPhone so that it produces regular Find My advertisements.

## Hardware

Current hardware:

- FREENOVE ESP32-S3-WROOM CAM
- ESP32-S3
- 16 MB Flash
- 8 MB PSRAM
- microSD card slot
- AirTag under observation

The board was selected because it provides BLE, Wi-Fi, substantial flash/PSRAM, and onboard microSD support in a relatively small package.

## Current Software

The current version is **AirTag Raw Logger V2**.

V2 currently:

1. Connects to Wi-Fi during startup.
2. Synchronizes the ESP32 system clock using NTP.
3. Uses Arizona local time (UTC-7) for timestamps.
4. Disconnects and disables Wi-Fi after time synchronization.
5. Scans for BLE advertisements.
6. Identifies advertisements matching known AirTag / Find My signatures.
7. Records every matching advertisement received.
8. Does not perform application-level MAC address deduplication.
9. Prints the complete raw payload to the serial console.

### Current Timestamp Format

Observations are currently timestamped like:

```text
2026-09-20 10:15:32.417
```

The timestamp is generated using the ESP32 system clock synchronized through NTP.

`gettimeofday()` is used to obtain the wall-clock timestamp, including the fractional second.

The displayed milliseconds should not be interpreted as ±1 ms UTC accuracy. Actual time accuracy is limited by NTP synchronization, network conditions, and the ESP32's clock behavior.

## AirTag Detection

The current scanner is based on the work of Matthew KuKanich:

https://github.com/MatthewKuKanich/ESP32-AirTag-Scanner

The detection code looks for the following byte sequences within BLE advertisement data:

```text
1E FF 4C 00
```

and:

```text
4C 00 12 19
```

These patterns are used to identify Apple Find My / AirTag advertisements.

The logger deliberately does not attempt to determine whether a particular advertisement is "new."

If an advertisement matches the detection pattern, it is logged.

This is important because the purpose of this project is to preserve the raw observations for later analysis.

## Why Raw Data?

It would be tempting to immediately:

- Deduplicate MAC addresses
- Decode known fields
- Ignore repeated advertisements
- Keep only the strongest RSSI
- Keep only one observation per second
- Discard advertisements that appear redundant

This project intentionally does none of those things at the collection stage.

The assumption is:

> Data that is discarded at collection time cannot be analyzed later.

The interpretation of the data can happen after collection.

## Planned Development

The current serial-only logger is the first functional stage.

Planned features include:

### microSD Logging

Store observations on the onboard microSD card.

A possible organization is:

```text
/AirTagLog/
    2026-09-20.csv
    2026-09-21.csv
    2026-09-22.csv
```

Daily files should keep individual files manageable and make long-running experiments easier to inspect.

### Local Flash Fallback

If the SD card is missing or unavailable, the logger should continue recording observations to the ESP32's internal flash filesystem.

LittleFS is the intended filesystem for pending log data.

When the SD card becomes available again, the logger should synchronize pending files from LittleFS to the SD card.

The initial synchronization strategy is intentionally simple:

- If a filename exists only on local flash, copy it to the SD card.
- If the same filename already exists on the SD card, append the pending data.
- After successful synchronization, remove the local copy.

The database/import stage can sort records by timestamp later, so temporary ordering differences during synchronization are acceptable.

### Scheduled Scanning

The eventual unattended logger should:

- Synchronize time after boot.
- Determine the next `:00` or `:30` boundary.
- Sleep until that boundary.
- Scan BLE for approximately one minute.
- Save observations.
- Return to sleep.
- Repeat.

The schedule should be based on the clock rather than a simple elapsed-time counter so that reboots do not introduce schedule drift.

For example:

```text
12:00 → scan
12:30 → scan
13:00 → scan
13:30 → scan
14:00 → scan
```

A reboot at 12:17 should cause the logger to resume at the next scheduled boundary rather than creating a new 30-minute cycle based on the reboot time.

## Data Philosophy

The architecture is intentionally:

**Raw data → analysis → interpretation**

The ESP32 should collect as little interpreted information as possible.

Later software can analyze:

- MAC address rotation
- Advertisement payload changes
- Advertisement frequency
- RSSI
- Timing patterns
- Behavior during owner separation
- Changes over long periods
- Relationships between address and payload changes

Keeping the original raw observations allows new analysis to be performed later without repeating the experiment.

## Current Status

### Completed

- [x] ESP32 BLE scanning
- [x] AirTag / Find My advertisement detection
- [x] Duplicate advertisement callbacks enabled
- [x] Application-level MAC deduplication removed
- [x] Raw payload capture
- [x] RSSI capture
- [x] Wi-Fi initialization
- [x] NTP time synchronization
- [x] Arizona local time conversion
- [x] Wall-clock millisecond timestamps
- [x] Wi-Fi shutdown after time synchronization
- [x] Initial Git repository

### In Progress

- [ ] First compile on the actual FREENOVE ESP32-S3 board
- [ ] First physical AirTag capture
- [ ] Verify duplicate advertisement behavior
- [ ] Verify timestamp behavior
- [ ] Verify long-duration scanning stability

### Planned

- [ ] microSD logging
- [ ] Daily log files
- [ ] LittleFS emergency storage
- [ ] SD/local synchronization
- [ ] Scheduled 30-minute scanning
- [ ] Deep sleep between scans
- [ ] Configuration storage
- [ ] Long-duration unattended testing
- [ ] Data analysis tools
- [ ] Visualization of MAC/payload changes

## Security / Credentials

Wi-Fi credentials must **not** be committed to this repository.

The current source contains placeholders:

```cpp
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
```

Actual credentials should be supplied locally and should never be committed to Git.

## Attribution

The initial BLE detection code is based on:

Matthew KuKanich  
ESP32-AirTag-Scanner

https://github.com/MatthewKuKanich/ESP32-AirTag-Scanner

The original project was used as the starting point for AirTag detection. This project modifies the approach to focus on continuous raw advertisement logging rather than maintaining a list of unique AirTags.

## Project Philosophy

This is an experiment in collecting first and interpreting later.

AirTag behavior is interesting partly because some of the information being transmitted may change over time, and assumptions about what matters today may turn out to be wrong later.

Therefore:

**Don't throw away the weird stuff. Log it.**
