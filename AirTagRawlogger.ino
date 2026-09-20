// AirTag Raw Logger V3
//
// Based on:
// Matthew KuKanich - ESP32-AirTag-Scanner
// https://github.com/MatthewKuKanich/ESP32-AirTag-Scanner
//
// V3 changes:
//   - Keeps the V2 Wi-Fi/NTP Arizona local-time behavior.
//   - Adds SD card CSV logging using SDMMC 1-bit mode.
//   - Creates /AirTagLog/YYYY-MM-DD.csv for each local calendar day.
//   - Writes the CSV header when a new daily file is created.
//   - Logs every matching AirTag advertisement to Serial and SD.
//   - Keeps duplicate BLE callbacks enabled.
//   - Does not deduplicate by MAC address.
//   - Does not add LittleFS fallback or scheduled sleep.
//   - Test schedule: 10-second scan at each wall-clock minute mark.
//   - Requests raw BLE payloads so duplicate callbacks do not accumulate data.
//
// Board:
//   FREENOVE ESP32-S3-WROOM CAM
//
// SDMMC 1-bit pins on the FREENOVE board:
//   CLK   = GPIO39
//   CMD   = GPIO38
//   DATA0 = GPIO40

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <sys/time.h>
#include "secrets.h"

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

#include "FS.h"
#include "SD_MMC.h"

// ============================================================
// Wi-Fi / NTP configuration
// ============================================================

// WIFI_SSID and WIFI_PASSWORD are defined in secrets.h.
// Keep secrets.h gitignored.

// Arizona local time is UTC-7 and does not observe daylight saving time.
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.nist.gov";

const long GMT_OFFSET_SEC = -7 * 3600;
const int DAYLIGHT_OFFSET_SEC = 0;

// ============================================================
// BLE configuration
// ============================================================

// Wall-clock schedule, not elapsed-time scheduling.
//   1  = every minute at :00 during the test.
//   30 = later, every half-hour at :00 and :30.
const uint8_t CLOCK_MARK_MINUTES = 1;
const uint32_t SCAN_DURATION_SECONDS = 10;

BLEScan* pBLEScan;
unsigned long observationCount = 0;

// ============================================================
// SD card configuration
// ============================================================

const int SD_CLK_PIN = 39;
const int SD_CMD_PIN = 38;
const int SD_DATA0_PIN = 40;

const char* SD_MOUNT_POINT = "/sdcard";
const char* SD_LOG_DIRECTORY = "/AirTagLog";
const char* CSV_HEADER = "timestamp,mac,rssi,payload_length,payload";

bool sdAvailable = false;
bool timeSynchronized = false;

// ============================================================
// Timestamp helpers
// ============================================================

// Formats the current synchronized local time as:
// YYYY-MM-DD HH:MM:SS.mmm
bool getCurrentTimestamp(char* timestamp, size_t timestampSize) {

  struct timeval tv;

  if (gettimeofday(&tv, nullptr) != 0) {
    return false;
  }

  struct tm timeinfo;

  if (!localtime_r(&tv.tv_sec, &timeinfo)) {
    return false;
  }

  char dateTime[24];

  if (strftime(
        dateTime,
        sizeof(dateTime),
        "%Y-%m-%d %H:%M:%S",
        &timeinfo) == 0) {
    return false;
  }

  int milliseconds = static_cast<int>(tv.tv_usec / 1000);

  int written = snprintf(
      timestamp,
      timestampSize,
      "%s.%03d",
      dateTime,
      milliseconds);

  return written > 0 && static_cast<size_t>(written) < timestampSize;
}

void printCurrentTime() {

  char timestamp[32];

  if (!getCurrentTimestamp(timestamp, sizeof(timestamp))) {
    Serial.println("ERROR: System time is not available.");
    return;
  }

  Serial.print(timestamp);
}

// ============================================================
// Wi-Fi / NTP initialization
// ============================================================

bool initializeTime() {

  Serial.println();
  Serial.println("========================================");
  Serial.println("Initializing Wi-Fi / NTP");
  Serial.println("========================================");

  WiFi.mode(WIFI_STA);

  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startAttempt = millis();

  // Give Wi-Fi up to 20 seconds.
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startAttempt < 20000) {

    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {

    Serial.println("Wi-Fi connection failed.");
    Serial.println("Cannot synchronize clock.");

    return false;
  }

  Serial.println("Wi-Fi connected.");

  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  Serial.println("Synchronizing time with NTP...");

  configTime(
      GMT_OFFSET_SEC,
      DAYLIGHT_OFFSET_SEC,
      NTP_SERVER_1,
      NTP_SERVER_2);

  struct tm timeinfo;

  // Give NTP up to 15 seconds.
  if (!getLocalTime(&timeinfo, 15000)) {

    Serial.println("NTP synchronization failed.");

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    return false;
  }

  Serial.print("Arizona local time synchronized: ");
  printCurrentTime();
  Serial.println();

  // Wi-Fi is not needed during BLE scanning.
  Serial.println("Disconnecting Wi-Fi...");

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  Serial.println("Wi-Fi disabled.");

  timeSynchronized = true;

  return true;
}

// ============================================================
// SD card initialization and CSV helpers
// ============================================================

bool initializeSDCard() {

  Serial.println();
  Serial.println("========================================");
  Serial.println("Initializing SD card");
  Serial.println("========================================");

  // FREENOVE ESP32-S3-WROOM CAM SDMMC pins.
  // The onboard slot uses SDMMC 1-bit mode.
  SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_DATA0_PIN);

  if (!SD_MMC.begin(SD_MOUNT_POINT, true)) {

    Serial.println("ERROR: SD card mount failed.");
    Serial.println("SD logging will be disabled; Serial logging will continue.");

    return false;
  }

  Serial.println("SD card mounted successfully.");

  uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
  uint64_t totalSpace = SD_MMC.totalBytes() / (1024 * 1024);
  uint64_t usedSpace = SD_MMC.usedBytes() / (1024 * 1024);

  Serial.print("Card size: ");
  Serial.print(cardSize);
  Serial.println(" MB");

  Serial.print("Total filesystem space: ");
  Serial.print(totalSpace);
  Serial.println(" MB");

  Serial.print("Used filesystem space: ");
  Serial.print(usedSpace);
  Serial.println(" MB");

  if (!SD_MMC.exists(SD_LOG_DIRECTORY)) {

    if (!SD_MMC.mkdir(SD_LOG_DIRECTORY)) {

      Serial.println("ERROR: Could not create /AirTagLog directory.");
      Serial.println("SD logging will be disabled; Serial logging will continue.");

      return false;
    }

    Serial.println("Created /AirTagLog directory.");
  }

  Serial.println("SD logging ready.");

  return true;
}

String makeDailyLogPath(const char* timestamp) {

  // The timestamp begins with YYYY-MM-DD.
  String path = SD_LOG_DIRECTORY;
  path += "/";

  for (size_t i = 0; i < 10; i++) {
    path += timestamp[i];
  }

  path += ".csv";

  return path;
}

bool createDailyLogFileIfNeeded(const String& path) {

  if (SD_MMC.exists(path.c_str())) {
    return true;
  }

  Serial.print("Creating daily log file: ");
  Serial.println(path);

  File file = SD_MMC.open(path.c_str(), FILE_WRITE);

  if (!file) {

    Serial.println("ERROR: Could not create daily CSV file.");
    return false;
  }

  file.println(CSV_HEADER);
  file.close();

  return true;
}

void appendObservationToSD(
    const char* timestamp,
    const String& macAddress,
    int rssi,
    const uint8_t* payload,
    size_t payloadLength) {

  if (!sdAvailable) {
    return;
  }

  // A valid timestamp is required for YYYY-MM-DD file rotation.
  if (timestamp == nullptr || strlen(timestamp) < 10) {

    Serial.println("SD log skipped: timestamp is unavailable.");
    return;
  }

  String path = makeDailyLogPath(timestamp);

  if (!createDailyLogFileIfNeeded(path)) {
    return;
  }

  File file = SD_MMC.open(path.c_str(), FILE_APPEND);

  if (!file) {

    Serial.print("ERROR: Could not open CSV for append: ");
    Serial.println(path);
    return;
  }

  file.print(timestamp);
  file.print(",");
  file.print(macAddress);
  file.print(",");
  file.print(rssi);
  file.print(",");
  file.print(payloadLength);
  file.print(",");

  // Store the complete raw advertisement payload as uppercase hex bytes.
  for (size_t i = 0; i < payloadLength; i++) {

    if (payload[i] < 0x10) {
      file.print("0");
    }

    file.print(payload[i], HEX);

    if (i + 1 < payloadLength) {
      file.print(" ");
    }
  }

  file.println();
  file.close();
}

// ============================================================
// Wall-clock scheduling
// ============================================================

// Wait for the next wall-clock mark. With CLOCK_MARK_MINUTES = 1,
// this means the next HH:MM:00. With CLOCK_MARK_MINUTES = 30,
// it means the next HH:00:00 or HH:30:00.
//
// This is deliberately a normal delay for the current test. Later,
// the same calculated wake time can be used for deep sleep.
void waitUntilNextClockMark() {

  if (!timeSynchronized) {

    Serial.println("Clock unavailable; waiting 60 seconds without scheduling.");
    delay(60000UL);
    return;
  }

  struct timeval tv;

  if (gettimeofday(&tv, nullptr) != 0) {

    Serial.println("Clock read failed; retrying in one second.");
    delay(1000);
    return;
  }

  struct tm timeinfo;

  if (!localtime_r(&tv.tv_sec, &timeinfo)) {

    Serial.println("Local clock conversion failed; retrying in one second.");
    delay(1000);
    return;
  }

  uint32_t markLengthSeconds = CLOCK_MARK_MINUTES * 60UL;
  uint32_t secondsIntoHour =
      static_cast<uint32_t>(timeinfo.tm_min) * 60UL +
      static_cast<uint32_t>(timeinfo.tm_sec);

  // Always choose the next mark, even if we happen to be exactly on one.
  uint32_t nextMarkSeconds =
      ((secondsIntoHour / markLengthSeconds) + 1UL) * markLengthSeconds;

  uint32_t secondsUntilMark = nextMarkSeconds - secondsIntoHour;
  uint32_t millisecondsUntilMark =
      secondsUntilMark * 1000UL -
      static_cast<uint32_t>(tv.tv_usec / 1000);

  Serial.print("Waiting for next clock mark in ");
  Serial.print(millisecondsUntilMark / 1000UL);
  Serial.println(" seconds...");

  delay(millisecondsUntilMark);
}

// ============================================================
// BLE advertisement callback
// ============================================================

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {

  void onResult(BLEAdvertisedDevice advertisedDevice) {

    // Get the raw advertisement payload.
    uint8_t* payLoad = advertisedDevice.getPayload();
    size_t payLoadLength = advertisedDevice.getPayloadLength();

    // --------------------------------------------------------
    // Look for the same AirTag / Find My advertisement
    // signatures used by the working V2 logger:
    //
    //   1E FF 4C 00
    //   4C 00 12 19
    // --------------------------------------------------------

    bool patternFound = false;

    if (payLoadLength >= 4) {

      for (size_t i = 0; i <= payLoadLength - 4; i++) {

        if (payLoad[i] == 0x1E &&
            payLoad[i + 1] == 0xFF &&
            payLoad[i + 2] == 0x4C &&
            payLoad[i + 3] == 0x00) {

          patternFound = true;
          break;
        }

        if (payLoad[i] == 0x4C &&
            payLoad[i + 1] == 0x00 &&
            payLoad[i + 2] == 0x12 &&
            payLoad[i + 3] == 0x19) {

          patternFound = true;
          break;
        }
      }
    }

    // Not an AirTag advertisement.
    if (!patternFound) {
      return;
    }

    // --------------------------------------------------------
    // There is deliberately NO MAC deduplication here.
    // Every matching advertisement received by the callback is
    // recorded to Serial and, when available, to the SD card.
    // --------------------------------------------------------

    observationCount++;

    String macAddress =
        advertisedDevice.getAddress().toString().c_str();

    macAddress.toUpperCase();

    int rssi = advertisedDevice.getRSSI();

    char timestamp[32];
    bool timestampValid =
        timeSynchronized &&
        getCurrentTimestamp(timestamp, sizeof(timestamp));

    // --------------------------------------------------------
    // Keep the existing Serial observation output.
    // --------------------------------------------------------

    Serial.println();
    Serial.println("========================================");

    Serial.print("AirTag observation #");
    Serial.println(observationCount);

    Serial.print("Time:        ");

    if (timestampValid) {
      Serial.println(timestamp);
    } else {
      Serial.println("ERROR: System time is not available.");
    }

    Serial.print("MAC Address: ");
    Serial.println(macAddress);

    Serial.print("RSSI:        ");
    Serial.print(rssi);
    Serial.println(" dBm");

    Serial.print("Payload Len: ");
    Serial.println(payLoadLength);

    Serial.print("Payload:     ");

    for (size_t i = 0; i < payLoadLength; i++) {

      if (payLoad[i] < 0x10) {
        Serial.print("0");
      }

      Serial.print(payLoad[i], HEX);
      Serial.print(" ");
    }

    Serial.println();

    // Append the same complete observation to the daily CSV.
    if (timestampValid) {
      appendObservationToSD(
          timestamp,
          macAddress,
          rssi,
          payLoad,
          payLoadLength);
    } else {
      Serial.println("SD log skipped: timestamp is unavailable.");
    }

    Serial.println("========================================");
  }
};

// ============================================================
// Setup
// ============================================================

void setup() {

  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("========================================");
  Serial.println("AirTag Raw Logger V3");
  Serial.println("Arizona local time: UTC-7");
  Serial.println("SD logging: /AirTagLog/YYYY-MM-DD.csv");
  Serial.println("========================================");

  // Get accurate Arizona local date/time first.
  bool timeValid = initializeTime();

  if (!timeValid) {

    Serial.println();
    Serial.println("WARNING:");
    Serial.println("System clock was not synchronized.");
    Serial.println("BLE scanning will continue.");
    Serial.println("SD rows require a valid timestamp.");
    Serial.println();
  }

  // Initialize the onboard FREENOVE SD card.
  sdAvailable = initializeSDCard();

  // ----------------------------------------------------------
  // Initialize BLE.
  // ----------------------------------------------------------

  Serial.println("Initializing BLE...");

  BLEDevice::init("");

  pBLEScan = BLEDevice::getScan();

  // TRUE  = request callbacks for duplicate advertisements.
  // FALSE = keep the raw payload intact and do not let the BLE
  //         library parse/accumulate it across duplicate callbacks.
  //
  // We perform the AirTag signature matching ourselves below.
  pBLEScan->setAdvertisedDeviceCallbacks(
      new MyAdvertisedDeviceCallbacks(),
      true,
      false);

  // Keep the working V2 scan configuration.
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);

  Serial.println("BLE initialized.");
  Serial.println();
  Serial.println("Scanning for AirTags...");
  Serial.println();
}

// ============================================================
// Main loop
// ============================================================

void loop() {

  // Start only on a wall-clock schedule mark.
  waitUntilNextClockMark();

  Serial.println("Starting 10-second BLE scan at the clock mark...");

  // Scan for ten seconds.
  pBLEScan->start(SCAN_DURATION_SECONDS, false);

  // Release scan results.
  pBLEScan->clearResults();

  Serial.println("Scan complete. Waiting for the next clock mark...");
}
