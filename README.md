# AirTag Raw Logger

An ESP32-S3-based experimental logger for capturing raw Bluetooth Low Energy (BLE) advertisements associated with Apple Find My / AirTag devices.

The project is intentionally designed around **raw data collection first and interpretation later**. The logger records the complete advertisement payload, BLE address, RSSI, and timestamp without attempting to deduplicate observations or reduce the data to an interpreted result.

This makes the collected data useful for investigating how AirTags and other Find My accessories advertise over time, including changes to their Bluetooth address, cryptographic material, status bytes, advertising behavior, and other observable characteristics.

> **Project status:** V6 is the current working version.

---

## Goals

The primary goal is to build a long-running, low-power data logger that can answer questions such as:

- How frequently does an AirTag advertise?
- How often does its BLE address change?
- How often does the payload change?
- Which parts of the payload remain constant?
- Which parts change periodically?
- Does the BLE address change independently of the payload?
- Does removing and reinstalling the battery affect the sequence of observed identities?
- Does an AirTag's behavior change when its owner is nearby?
- Does the behavior change when the AirTag is moved to another location?
- Can a transition be observed immediately before an AirTag disappears from ordinary BLE scanning?
- How do the observations compare between different AirTags or Find My accessories?

The logger deliberately avoids answering those questions during collection. It records the evidence so it can be analyzed afterward.

---

# Hardware

Current hardware:

- FREENOVE ESP32-S3-WROOM CAM
- ESP32-S3
- 16 MB flash
- 8 MB OPI PSRAM
- microSD card
- USB power

The current hardware does not use an OLED or other display.

The logger is intended to operate unattended, with the SD card containing the collected observations.

---

# SD Card

The FREENOVE board's microSD interface is operated in **1-bit SDMMC mode**.

Current pin configuration:

| Signal | GPIO |
|---|---:|
| CLK | 39 |
| CMD | 38 |
| DATA0 | 40 |

The firmware uses:

```cpp
SD_MMC.setPins(39, 38, 40);
SD_MMC.begin("/sdcard", true);
```

The SD card was independently tested before being integrated into the logger.

The test successfully:

- mounted the card
- reported the card capacity
- reported filesystem capacity
- created a file
- wrote data
- read the data back
- listed the filesystem contents

The current test card is approximately 960 MB physical capacity with approximately 957 MB available to the filesystem.

---

# Arduino IDE Configuration

Current configuration:

| Setting | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| Flash Size | 16MB |
| Partition Scheme | Huge APP (3MB No OTA/1MB SPIFFS) |
| PSRAM | OPI PSRAM |
| Flash Mode | QIO |
| Flash Frequency | 80 MHz |
| CPU Frequency | 240 MHz |
| Upload Mode | UART0 / Hardware CDC |
| Upload Speed | 921600 |
| ESP32 package | 3.3.11 |

The serial COM port will depend on the computer being used.

---

# Wi-Fi and Time Synchronization

The logger uses Wi-Fi only long enough to obtain accurate time.

Wi-Fi credentials are stored separately in `secrets.h`.

Example:

```cpp
#define WIFI_SSID "your-network"
#define WIFI_PASSWORD "your-password"
```

`secrets.h` is intentionally excluded from the repository.

The main sketch includes:

```cpp
#include "secrets.h"
```

## NTP

The logger uses:

- `pool.ntp.org`
- `time.nist.gov`

The current configuration allows up to three NTP attempts, with a 10-second timeout per attempt.

After successful synchronization, Wi-Fi is disabled:

```cpp
WiFi.disconnect(true);
WiFi.mode(WIFI_OFF);
```

This keeps Wi-Fi from unnecessarily consuming power during BLE collection.

---

# Arizona Time

The current logger is configured for Arizona local time:

- UTC-7
- no daylight-saving adjustment

This is intentional because the logger is currently being used in Arizona.

The recorded timestamps are generated using the ESP32 system clock after NTP synchronization.

---

# Timestamp Precision

Records include milliseconds.

Example:

```text
2026-09-20 18:05:04.042
```

The timestamp is intended to make it possible to study:

- advertisement intervals
- gaps
- bursts
- scan-window boundaries
- changes between advertisements
- timing relationships between BLE address and payload changes

---

# BLE Scanning

The logger uses the ESP32 BLE stack to perform active BLE scans.

Duplicate advertisements are intentionally allowed.

There is **no application-level MAC-address deduplication**.

If the same device advertises repeatedly, each observation is recorded.

For example, a sequence such as:

```text
18:05:04.042  AA:BB:CC:11:22:33
18:05:06.044  AA:BB:CC:11:22:33
18:05:08.047  AA:BB:CC:11:22:33
18:05:10.047  AA:BB:CC:11:22:33
```

represents four separate observations.

This is intentional.

The timing information is part of the experiment.

---

# Find My / AirTag Detection

The current firmware recognizes Find My-related manufacturer data using the Apple company identifier and Find My message structure.

The current detection criteria include:

```text
1E FF 4C 00
```

or:

```text
4C 00 12 19
```

The important common identifiers are:

| Bytes | Meaning |
|---|---|
| `4C 00` | Apple company identifier |
| `12` | Find My message type |
| `19` | Find My payload length |
| `1E FF` | BLE advertising/manufacturer-data structure bytes |

The logger does not attempt to determine which specific AirTag an observation belongs to during collection.

It records the complete packet for later analysis.

---

# Raw Payload Logging

The raw payload is stored as uppercase hexadecimal bytes separated by spaces.

Example format:

```text
1E FF 4C 00 12 19 10 XX XX XX XX XX XX XX XX XX XX XX XX XX XX XX XX XX XX XX XX XX XX XX XX
```

The actual firmware records the real bytes to the SD card.

The example above uses placeholders because device-specific cryptographic material should not be unnecessarily published in documentation.

---

# CSV Format

Each observation is stored as one CSV row.

The header is:

```text
timestamp,mac,rssi,payload_length,payload
```

Fields:

| Field | Description |
|---|---|
| `timestamp` | Local date/time including milliseconds |
| `mac` | BLE address observed by the ESP32 |
| `rssi` | Received Signal Strength Indicator |
| `payload_length` | Length of captured payload |
| `payload` | Complete payload represented as hexadecimal bytes |

## Synthetic Example

The following example is **fictional demonstration data**.

The common Apple / Find My structural bytes are retained:

```text
1E FF 4C 00 12 19
```

The MAC address and device-specific payload bytes are synthetic and do **not** represent a real AirTag.

```csv
timestamp,mac,rssi,payload_length,payload
2026-09-20 18:05:04.042,3A:71:C4:9E:25:B8,-52,31,1E FF 4C 00 12 19 10 A7 3C 91 E2 5D 08 B4 6A F0 23 87 C1 4E 9A 15 D8 62 3F B9 04 7D E6 52
2026-09-20 18:05:06.044,3A:71:C4:9E:25:B8,-47,31,1E FF 4C 00 12 19 10 A7 3C 91 E2 5D 08 B4 6A F0 23 87 C1 4E 9A 15 D8 62 3F B9 04 7D E6 52
2026-09-20 18:05:08.047,3A:71:C4:9E:25:B8,-54,31,1E FF 4C 00 12 19 10 A7 3C 91 E2 5D 08 B4 6A F0 23 87 C1 4E 9A 15 D8 62 3F B9 04 7D E6 52
```

> **Note:** The example above is intentionally synthetic. It is not a capture from an actual AirTag.

---

# Daily Log Files

Logs are stored by date.

The directory structure is:

```text
/AirTagLog/
    2026-09-20.csv
    2026-09-21.csv
    2026-09-22.csv
```

A new CSV file is created for each day.

If the file does not already exist, the logger creates it and writes:

```text
timestamp,mac,rssi,payload_length,payload
```

as the header.

---

# Scheduled Scanning

The current firmware does not scan continuously.

It performs a 30-second BLE scan every 30 minutes.

Scan windows begin at:

```text
HH:00:00
HH:30:00
```

For example:

```text
18:00:00 - 18:00:30
18:30:00 - 18:30:30
19:00:00 - 19:00:30
19:30:00 - 19:30:30
```

The intention is to collect enough data to characterize normal behavior while allowing the ESP32 to spend most of its time asleep.

---

# Why 30 Seconds?

A continuously advertising AirTag is generally observed at roughly two-second intervals under favorable scanning conditions.

A 30-second scan therefore has the potential to capture approximately:

```text
30 / 2 = 15 observations
```

per scan window.

With two scan windows per hour:

```text
15 × 2 × 24 = approximately 720 observations/day
```

The actual number will vary because BLE scanning, radio conditions, advertisement timing, and device behavior are not perfectly deterministic.

The initial goal is not to maximize the number of observations.

The goal is to collect enough data to determine whether the 30-second windows are sufficient.

If an interesting transition occurs between scan windows, the experiment can later be modified to continuously monitor the relevant period.

---

# Deep Sleep

Between scan windows the ESP32 enters deep sleep.

Before sleeping, the firmware unmounts the SD card and prepares the next wake-up time.

This significantly reduces unnecessary activity between scans.

The device therefore operates approximately as:

```text
Wake
  |
  v
Synchronize time if required
  |
  v
Initialize storage
  |
  v
BLE scan for 30 seconds
  |
  v
Write/flush collected data
  |
  v
Unmount storage
  |
  v
Deep sleep
  |
  v
Wake at next :00 or :30
```

---

# SD Failure / LittleFS Fallback

The logger is intended to continue collecting data even if the SD card temporarily becomes unavailable.

When SD storage cannot be used, pending records can be written to LittleFS.

The pending data is stored under:

```text
/AirTagPending
```

When SD storage becomes available again, the firmware can replay the pending data to the appropriate SD log.

This provides some protection against temporary SD-card failures or initialization problems.

---

# Data Philosophy

The most important design decision in this project is:

> **Collect raw observations first. Interpret them later.**

The firmware intentionally does not attempt to turn an observation into a statement such as:

```text
"This is AirTag #1."
```

or:

```text
"This AirTag changed identities."
```

Those are analysis conclusions.

The logger records:

- when the advertisement was observed
- which BLE address was observed
- RSSI
- payload length
- complete payload

The analysis can then compare those observations afterward.

This avoids accidentally throwing away information that later turns out to be important.

---

# AirTag Identity Investigation

One of the major questions being investigated is how a Find My device can change its observable BLE identity while remaining associated with the same physical device and owner.

A useful conceptual model is:

```text
Physical AirTag
      |
      +---- BLE address
      |
      +---- Find My advertisement payload
      |
      +---- changing cryptographic material
      |
      v
Nearby Apple devices
      |
      v
Find My network
      |
      v
Encrypted location reports
```

The ESP32 cannot see the owner's Find My account relationship.

It can only observe what the AirTag broadcasts.

That makes repeated raw observations particularly valuable.

---

# BLE Address and Payload Correlation

A simple MAC-address-only analysis is not sufficient.

If an AirTag changes its BLE address, the interesting question is whether other parts of the advertisement change at the same time.

For example:

```text
Observation A
MAC = A
Payload = X

Observation B
MAC = A
Payload = X

Observation C
MAC = B
Payload = Y
```

The transition from `(A, X)` to `(B, Y)` is more informative than simply saying:

```text
MAC changed from A to B
```

The logger therefore preserves the complete payload.

---

# Battery Removal Experiment

A planned experiment is to observe an AirTag over an extended period, recording complete observations before and after removing and reinstalling its battery.

A possible experiment sequence:

```text
1. Start logging.
2. Allow the AirTag to naturally change observable state.
3. Record several distinct MAC/payload combinations.
4. Remove the AirTag battery.
5. Reinstall the battery.
6. Continue logging.
7. Compare the post-restart observations with the earlier observations.
```

The important comparison is the complete observed state:

```text
MAC + complete payload
```

rather than the MAC alone.

If an exact previously observed combination appears again, that is a stronger experimental observation than merely seeing a previously used MAC address.

The experiment can then examine what happens afterward.

For example:

```text
Before battery removal:

A -> B -> C

Battery removed/reinstalled

After battery removal:

C -> D
```

versus:

```text
Before battery removal:

A -> B -> C

Battery removed/reinstalled

After battery removal:

A -> B -> C
```

The logger itself does not assume either behavior. It is intended to collect the data needed to determine what actually happens.

---

# Location Experiments

Another planned experiment is to move the same physical AirTag and logger together to another location.

Because both devices move together, this removes some ambiguity about whether an observed change is associated with:

- the AirTag
- the surrounding BLE environment
- nearby Apple devices
- physical location
- owner proximity

The experiment can compare the complete observation stream before and after moving.

---

# Owner Proximity

Another useful controlled variable is the presence or absence of the AirTag's owner.

The goal is to observe whether the AirTag's advertising behavior changes when:

- the owner's iPhone is nearby
- the owner is separated from the AirTag
- another person is carrying the AirTag
- the AirTag is stationary for an extended period

This is especially interesting because ordinary BLE scanning does not necessarily expose every state of an AirTag in the same way.

---

# Find My and Cryptographic Data

The Find My advertisement contains cryptographic material that changes over time.

The logger does not attempt to decode or reverse the cryptographic system.

Instead, it preserves the bytes so that changes can be analyzed empirically.

A typical registered Find My advertisement has the general structure:

```text
1E FF 4C 00 12 19 SS XX XX XX XX XX XX XX XX
XX XX XX XX XX XX XX XX XX XX XX XX XX XX XX
```

where:

```text
1E FF       BLE advertising structure
4C 00       Apple company identifier
12          Find My message type
19          Find My payload length
SS          status byte
XX...       remaining device-specific data
```

The exact interpretation of the changing bytes is intentionally left to later analysis.

---

# Status Byte

The byte immediately following:

```text
4C 00 12 19
```

is treated as a raw status byte by the logger.

For example:

```text
1E FF 4C 00 12 19 10 ...
                     ^^
```

The raw value is preserved.

Some reverse-engineering work has associated values such as:

```text
10
14
50
54
90
94
D0
D4
```

with combinations of battery state and other status information.

The logger does not currently convert those values into a human-readable battery percentage or condition.

This is intentional.

The raw byte remains available so that the interpretation can be changed later without having to recollect the original advertisement.

---

# Example Status Byte

For demonstration:

```text
0x10
```

is:

```text
00010000
```

and:

```text
0x54
```

is:

```text
01010100
```

The logger records the hexadecimal value directly rather than converting it during collection.

---

# Raw Data vs. Interpretation

The project deliberately separates:

## Collection

The ESP32 records:

```text
timestamp
MAC
RSSI
payload length
raw payload
```

## Analysis

A separate analysis process can later calculate:

- advertisement intervals
- unique observed addresses
- unique complete payloads
- address/payload combinations
- state transitions
- repeated states
- timing of changes
- status-byte changes
- RSSI trends
- behavior before and after battery removal
- behavior before and after moving location

This separation makes the logger useful even if the current understanding of the protocol changes.

---

# Example Analysis Concept

Suppose the logger captures:

```text
Time       MAC                 Status
18:00:02   A                   10
18:00:04   A                   10
18:00:06   A                   10
18:15:02   A                   10
18:15:04   A                   10
18:30:01   B                   10
18:30:03   B                   10
```

A later analysis program could determine that a state transition occurred around 18:30.

It could then inspect the complete payloads to determine exactly what changed.

The collection firmware does not need to know what the transition means.

---

# Example Raw Record

The following is a **fictional example**. The Apple/Find My structural identifiers are preserved, but the MAC address and device-specific payload bytes are scrambled and do not represent a real tag.

```text
Timestamp:
2026-09-20 18:05:04.042

MAC:
3A:71:C4:9E:25:B8

RSSI:
-52

Payload length:
31

Payload:
1E FF 4C 00 12 19 10 A7 3C 91 E2 5D 08 B4 6A F0
23 87 C1 4E 9A 15 D8 62 3F B9 04 7D E6 52
```

The structural bytes:

```text
1E FF 4C 00 12 19
```

are retained because they describe the advertisement structure being demonstrated.

The remaining device-specific bytes are fictional.

---

# Repository Structure

The primary sketch is:

```text
AirTag-Raw-Logger/
│
├── AirTagRawlogger.ino
├── README.md
├── secrets.h
└── ...
```

`secrets.h` should not be committed to the public repository.

---

# Security / Privacy

Raw BLE observations can contain information that is useful for identifying or correlating devices.

For that reason:

- Do not publish raw logs from personally owned AirTags without considering what information they contain.
- Do not publish real BLE addresses unnecessarily.
- Do not publish real cryptographic payload material unnecessarily.
- Use fictional/scrambled examples in documentation.
- Keep `secrets.h` out of source control.

The README examples intentionally use scrambled device-specific values.

---

# Current V6 Feature Set

V6 currently provides:

- ESP32-S3 BLE scanning
- Find My / AirTag advertisement detection
- duplicate BLE observations
- no MAC deduplication
- raw payload capture
- RSSI capture
- millisecond timestamps
- NTP time synchronization
- Arizona local time
- Wi-Fi shutdown after time synchronization
- SDMMC 1-bit storage
- daily CSV files
- CSV headers
- LittleFS pending-data fallback
- pending-data replay
- scheduled 30-second scans
- :00 and :30 scan windows
- deep sleep between scans
- storage unmount before sleep

---

# Current Experimental Strategy

The initial deployment is intentionally conservative.

The logger is being allowed to run for extended periods before making changes to the scan schedule.

The first objective is to determine:

1. What normal advertisement behavior looks like.
2. How frequently the target appears during a 30-second scan.
3. Whether address changes can be observed.
4. Whether payload changes can be observed.
5. Whether address and payload changes occur together.
6. Whether interesting transitions occur near the edges of the scan windows.
7. Whether the current 30-second windows provide enough information.

If the data shows an interesting transition, a later firmware version can add targeted continuous logging windows.

For example:

```text
Normal operation:
30-second scan every 30 minutes

Targeted experiment:
03:30:00 - 04:00:00 continuous scan
```

This allows the logger to remain relatively efficient while still allowing detailed investigation of a specific period.

---

# Future Work

Possible future additions include:

- configurable scan duration
- configurable scan interval
- targeted continuous-monitoring windows
- better event/transition analysis
- automated CSV analysis
- graphing advertisement intervals
- graphing RSSI
- tracking complete MAC/payload combinations
- status-byte interpretation
- battery-status alerts
- identifying recurring state sequences
- comparing multiple AirTags
- analyzing behavior before and after battery removal
- analyzing behavior while owner and tag are separated
- comparing observations at different locations
- a web-based data viewer

A future web interface could display:

- daily logs
- chronological observations
- state changes
- RSSI graphs
- payload comparisons
- status/battery warnings
- image uploads
- comments and observations associated with particular dates or experiments

---

# Design Principle

The project is deliberately built around one simple rule:

> **Don't throw away data just because we don't understand it yet.**

The firmware's job is to observe and record.

The analysis layer's job is to figure out what the observations mean.

That distinction is important for a reverse-engineering experiment where today's "irrelevant" byte may turn out to be tomorrow's most interesting clue.

---

# License

Copyright (c) 2026 Chris Neal

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for the full license text.

---

# Disclaimer

This project is an independent research and experimentation project.

It is not affiliated with, sponsored by, or endorsed by Apple.

Apple, AirTag, Find My, and related trademarks belong to their respective owners.
