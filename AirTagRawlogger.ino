// AirTag Raw Logger V8
//
// Based on:
// Matthew KuKanich - ESP32-AirTag-Scanner
// https://github.com/MatthewKuKanich/ESP32-AirTag-Scanner
//
// V8 changes:
//   - Keeps the V2 Wi-Fi/NTP Arizona local-time behavior.
//   - Adds SD card CSV logging using SDMMC 1-bit mode.
//   - Creates /AirTagLog/YYYY-MM-DD.csv for each local calendar day.
//   - Writes the CSV header when a new daily file is created.
//   - Logs every matching AirTag advertisement to Serial and SD.
//   - Falls back to LittleFS pending CSV files when SD is unavailable.
//   - Replays pending LittleFS rows to SD on the next boot with SD present.
//   - Keeps duplicate BLE callbacks enabled.
//   - Does not deduplicate by MAC address.
//   - Does not auto-format LittleFS.
//   - Retries NTP synchronization before giving up.
//   - Waits for a fresh SNTP synchronization callback instead of accepting
//     a retained deep-sleep clock as newly synchronized.
//   - Forces immediate NTP correction rather than gradual clock adjustment.
//   - Production schedule: 120-second scan at every 15-minute clock mark
//     (:00, :15, :30, and :45).
//   - Deep-sleeps between scheduled scans.
//   - Unmounts storage before sleep and remounts it after timer wake.
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
#include "esp_sntp.h"
#include "secrets.h"

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

#include "FS.h"
#include "SD_MMC.h"
#include "LittleFS.h"
#include "esp_sleep.h"

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
//   Production schedule: CLOCK_MARK_MINUTES = 15,
//   CLOCK_MARK_SECOND = 0, and 120-second scans.
const uint8_t CLOCK_MARK_MINUTES = 15;
const uint8_t CLOCK_MARK_SECOND = 0;
const uint32_t SCAN_DURATION_SECONDS = 120;

const uint8_t NTP_SYNC_ATTEMPTS = 3;
const uint32_t NTP_ATTEMPT_TIMEOUT_MS = 15000;

BLEScan* pBLEScan;
unsigned long observationCount = 0;
bool wakeFromTimer = false;
volatile bool ntpSyncReceived = false;

// This callback is invoked only when SNTP receives a fresh time update.
// Do not print from the SNTP task; just set a flag for initializeTime().
void onNtpTimeSync(struct timeval* tv) {
  (void)tv;
  ntpSyncReceived = true;
}

// ============================================================
// SD card configuration
// ============================================================

const int SD_CLK_PIN = 39;
const int SD_CMD_PIN = 38;
const int SD_DATA0_PIN = 40;

const char* SD_MOUNT_POINT = "/sdcard";
const char* SD_LOG_DIRECTORY = "/AirTagLog";
const char* LITTLEFS_PENDING_DIRECTORY = "/AirTagPending";
const char* CSV_HEADER = "timestamp,mac,rssi,payload_length,payload";

bool sdAvailable = false;
bool littleFsAvailable = false;
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

  struct tm timeinfo;
  bool ntpSynchronized = false;

  // getLocalTime() only verifies that the stored year is plausible. After
  // deep sleep, that can be an old but valid clock value. The callback below
  // is the proof that a fresh NTP response was actually received.
  esp_sntp_set_time_sync_notification_cb(onNtpTimeSync);
  esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);

  for (uint8_t attempt = 1;
       attempt <= NTP_SYNC_ATTEMPTS;
       attempt++) {

    Serial.print("NTP attempt ");
    Serial.print(attempt);
    Serial.print("/");
    Serial.println(NTP_SYNC_ATTEMPTS);

    ntpSyncReceived = false;

    configTime(
        GMT_OFFSET_SEC,
        DAYLIGHT_OFFSET_SEC,
        NTP_SERVER_1,
        NTP_SERVER_2);

    uint32_t waitStart = millis();

    while (!ntpSyncReceived &&
           (millis() - waitStart < NTP_ATTEMPT_TIMEOUT_MS)) {
      delay(50);
    }

    if (ntpSyncReceived && getLocalTime(&timeinfo, 1000)) {
      ntpSynchronized = true;
      Serial.println("Fresh NTP response received.");
      break;
    }

    Serial.println("NTP attempt failed.");

    if (attempt < NTP_SYNC_ATTEMPTS) {
      delay(1000);
    }
  }

  if (!ntpSynchronized) {

    Serial.println("NTP synchronization failed after all retries.");

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

String makeDailyFilePath(
    const char* directory,
    const char* timestamp) {

  // The timestamp begins with YYYY-MM-DD.
  String path = directory;
  path += "/";

  for (size_t i = 0; i < 10; i++) {
    path += timestamp[i];
  }

  path += ".csv";

  return path;
}

bool ensureCsvFile(fs::FS& filesystem, const String& path) {

  if (filesystem.exists(path.c_str())) {
    return true;
  }

  File file = filesystem.open(path.c_str(), FILE_WRITE);

  if (!file) {
    return false;
  }

  file.println(CSV_HEADER);
  file.close();

  return true;
}

bool appendCsvRecord(
    fs::FS& filesystem,
    const String& path,
    const String& record) {

  if (!ensureCsvFile(filesystem, path)) {
    return false;
  }

  File file = filesystem.open(path.c_str(), FILE_APPEND);

  if (!file) {
    return false;
  }

  file.print(record);
  bool writeSucceeded = file.getWriteError() == 0;
  file.close();

  return writeSucceeded;
}

String buildCsvRecord(
    const char* timestamp,
    const String& macAddress,
    int rssi,
    const uint8_t* payload,
    size_t payloadLength) {

  String record;
  record.reserve(64 + macAddress.length() + payloadLength * 3);

  record += timestamp;
  record += ",";
  record += macAddress;
  record += ",";
  record += String(rssi);
  record += ",";
  record += String(payloadLength);
  record += ",";

  // Store the complete raw advertisement payload as uppercase hex bytes.
  for (size_t i = 0; i < payloadLength; i++) {

    char byteHex[3];
    snprintf(byteHex, sizeof(byteHex), "%02X", payload[i]);
    record += byteHex;

    if (i + 1 < payloadLength) {
      record += " ";
    }
  }

  record += "\n";

  return record;
}

bool initializeLittleFS() {

  Serial.println();
  Serial.println("========================================");
  Serial.println("Initializing LittleFS fallback");
  Serial.println("========================================");

  // Do not auto-format the filesystem in the main logger.
  if (!LittleFS.begin(false)) {

    Serial.println("ERROR: LittleFS mount failed.");
    Serial.println("LittleFS fallback will be disabled.");

    return false;
  }

  Serial.println("LittleFS mounted successfully.");

  Serial.print("Total LittleFS space: ");
  Serial.print(LittleFS.totalBytes());
  Serial.println(" bytes");

  Serial.print("Used LittleFS space: ");
  Serial.print(LittleFS.usedBytes());
  Serial.println(" bytes");

  if (!LittleFS.exists(LITTLEFS_PENDING_DIRECTORY)) {

    if (!LittleFS.mkdir(LITTLEFS_PENDING_DIRECTORY)) {

      Serial.println("ERROR: Could not create LittleFS pending directory.");
      Serial.println("LittleFS fallback will be disabled.");

      LittleFS.end();
      return false;
    }
  }

  Serial.println("LittleFS fallback ready.");

  return true;
}

bool flushPendingFileToSD(const String& pendingPath) {

  if (!sdAvailable || !littleFsAvailable) {
    return false;
  }

  int lastSlash = pendingPath.lastIndexOf('/');

  if (lastSlash < 0) {
    return false;
  }

  String fileName = pendingPath.substring(lastSlash + 1);
  String sdPath = String(SD_LOG_DIRECTORY) + "/" + fileName;

  if (!ensureCsvFile(SD_MMC, sdPath)) {

    Serial.print("ERROR: Could not prepare SD file for pending data: ");
    Serial.println(sdPath);
    return false;
  }

  File pendingFile = LittleFS.open(pendingPath.c_str(), FILE_READ);
  File sdFile = SD_MMC.open(sdPath.c_str(), FILE_APPEND);

  if (!pendingFile || !sdFile) {

    if (pendingFile) {
      pendingFile.close();
    }

    if (sdFile) {
      sdFile.close();
    }

    return false;
  }

  // Pending files have the same header as the SD CSV. Skip that first line
  // while replaying the data rows.
  bool firstLine = true;
  while (pendingFile.available()) {

    String line = pendingFile.readStringUntil('\n');

    if (firstLine) {
      firstLine = false;
      continue;
    }

    if (line.endsWith("\r")) {
      line.remove(line.length() - 1);
    }

    if (line.length() == 0) {
      continue;
    }

    sdFile.print(line);
    sdFile.print("\n");
  }

  bool replaySucceeded = sdFile.getWriteError() == 0;

  pendingFile.close();
  sdFile.close();

  if (!replaySucceeded) {
    return false;
  }

  if (!LittleFS.remove(pendingPath.c_str())) {

    Serial.print("ERROR: Could not remove replayed pending file: ");
    Serial.println(pendingPath);
    return false;
  }

  return true;
}

void flushPendingLittleFS() {

  if (!sdAvailable || !littleFsAvailable) {
    return;
  }

  File pendingDirectory = LittleFS.open(LITTLEFS_PENDING_DIRECTORY);

  if (!pendingDirectory || !pendingDirectory.isDirectory()) {

    Serial.println("ERROR: Could not open LittleFS pending directory.");
    return;
  }

  File entry = pendingDirectory.openNextFile();

  while (entry) {

    String pendingPath = entry.name();

    // Some LittleFS implementations return only the entry name from
    // openNextFile(), while others return the full path. Normalize both
    // forms before opening the pending file.
    if (!pendingPath.startsWith(LITTLEFS_PENDING_DIRECTORY)) {

      if (!pendingPath.startsWith("/")) {
        pendingPath = "/" + pendingPath;
      }

      pendingPath = String(LITTLEFS_PENDING_DIRECTORY) + pendingPath;
    }

    bool isPendingCsv =
        !entry.isDirectory() && pendingPath.endsWith(".csv");

    entry.close();

    if (isPendingCsv) {

      Serial.print("Replaying LittleFS pending file: ");
      Serial.println(pendingPath);

      if (flushPendingFileToSD(pendingPath)) {
        Serial.println("Pending file replayed and removed successfully.");
      } else {
        Serial.println("Pending file replay failed; keeping it on LittleFS.");
      }
    }

    entry = pendingDirectory.openNextFile();
  }

  pendingDirectory.close();
}

void logObservationToStorage(
    const char* timestamp,
    const String& macAddress,
    int rssi,
    const uint8_t* payload,
    size_t payloadLength) {

  if (timestamp == nullptr || strlen(timestamp) < 10) {

    Serial.println("Storage log skipped: timestamp is unavailable.");
    return;
  }

  String record = buildCsvRecord(
      timestamp,
      macAddress,
      rssi,
      payload,
      payloadLength);

  if (sdAvailable) {

    String sdPath = makeDailyFilePath(SD_LOG_DIRECTORY, timestamp);

    if (appendCsvRecord(SD_MMC, sdPath, record)) {
      return;
    }

    Serial.println("WARNING: SD append failed; switching to LittleFS fallback.");
    SD_MMC.end();
    sdAvailable = false;
  }

  if (littleFsAvailable) {

    String pendingPath = makeDailyFilePath(
        LITTLEFS_PENDING_DIRECTORY,
        timestamp);

    if (appendCsvRecord(LittleFS, pendingPath, record)) {
      Serial.print("Stored observation in LittleFS pending file: ");
      Serial.println(pendingPath);
      return;
    }

    Serial.println("ERROR: LittleFS fallback append failed.");
    return;
  }

  Serial.println("ERROR: Neither SD nor LittleFS is available for this row.");
}

// ============================================================
// Wall-clock scheduling
// ============================================================

void unmountStorageBeforeSleep() {

  // All observation files are closed immediately after each write.
  // Explicitly end both filesystems before deep sleep anyway.
  if (sdAvailable) {
    Serial.println("Unmounting SD card...");
    SD_MMC.end();
    sdAvailable = false;
  }

  if (littleFsAvailable) {
    Serial.println("Unmounting LittleFS...");
    LittleFS.end();
    littleFsAvailable = false;
  }
}

// Put the ESP32 into deep sleep until the next wall-clock mark.
// With CLOCK_MARK_MINUTES = 15 and CLOCK_MARK_SECOND = 0,
// this means the next HH:00:00, HH:15:00, HH:30:00, or HH:45:00.
void deepSleepUntilNextClockMark() {

  if (!timeSynchronized) {

    Serial.println("Clock unavailable; rebooting in 30 seconds to retry NTP.");
    Serial.flush();
    delay(30000UL);
    ESP.restart();
    return;
  }

  struct timeval tv;

  if (gettimeofday(&tv, nullptr) != 0) {

    Serial.println("Clock read failed; rebooting to retry NTP.");
    Serial.flush();
    delay(1000);
    ESP.restart();
    return;
  }

  struct tm timeinfo;

  if (!localtime_r(&tv.tv_sec, &timeinfo)) {

    Serial.println("Local clock conversion failed; rebooting to retry NTP.");
    Serial.flush();
    delay(1000);
    ESP.restart();
    return;
  }

  uint32_t markLengthSeconds = CLOCK_MARK_MINUTES * 60UL;
  uint32_t secondsIntoHour =
      static_cast<uint32_t>(timeinfo.tm_min) * 60UL +
      static_cast<uint32_t>(timeinfo.tm_sec);

  uint32_t currentMarkBlock =
      (secondsIntoHour / markLengthSeconds) * markLengthSeconds;

  uint32_t nextMarkSeconds =
      currentMarkBlock + CLOCK_MARK_SECOND;

  // Always choose a future mark. Timer wakeups bypass this function and
  // begin scanning immediately after startup initialization.
  if (nextMarkSeconds <= secondsIntoHour) {
    nextMarkSeconds += markLengthSeconds;
  }

  uint32_t secondsUntilMark = nextMarkSeconds - secondsIntoHour;
  uint64_t microsecondsUntilMark =
      static_cast<uint64_t>(secondsUntilMark) * 1000000ULL -
      static_cast<uint64_t>(tv.tv_usec);

  Serial.print("Deep sleeping until next clock mark in ");
  Serial.print(
      static_cast<unsigned long>(microsecondsUntilMark / 1000000ULL));
  Serial.println(" seconds...");

  unmountStorageBeforeSleep();

  esp_sleep_enable_timer_wakeup(microsecondsUntilMark);

  Serial.flush();
  delay(50);
  esp_deep_sleep_start();
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

    // Store the same complete observation. SD is primary; LittleFS is the
    // pending fallback when SD is unavailable.
    if (timestampValid) {
      logObservationToStorage(
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

  wakeFromTimer =
      esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;

  Serial.println();
  Serial.println("========================================");
  Serial.println("AirTag Raw Logger V8");
  Serial.println("Arizona local time: UTC-7");
  Serial.println("SD logging: /AirTagLog/YYYY-MM-DD.csv");
  Serial.println("LittleFS fallback: /AirTagPending/YYYY-MM-DD.csv");
  Serial.println("Schedule: every 15 minutes at :00/:15/:30/:45, 120-second scan");
  Serial.println("========================================");

  if (wakeFromTimer) {
    Serial.println("Wake reason: deep-sleep timer");
  }

  // Get accurate Arizona local date/time first.
  bool timeValid = initializeTime();

  if (!timeValid) {

    Serial.println();
    Serial.println("WARNING:");
    Serial.println("System clock was not synchronized.");
    Serial.println("BLE scanning will wait for a valid clock.");
    Serial.println("SD rows require a valid timestamp.");
    Serial.println();
  }

  // Initialize the onboard FREENOVE SD card.
  sdAvailable = initializeSDCard();

  // Initialize internal flash fallback without auto-formatting.
  littleFsAvailable = initializeLittleFS();

  // If SD is available again after a previous outage, replay pending rows.
  if (sdAvailable && littleFsAvailable) {
    flushPendingLittleFS();
  }

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
  Serial.println("Waiting for scheduled scan...");
  Serial.println();
}

// ============================================================
// Main loop
// ============================================================

void loop() {

  if (wakeFromTimer) {

    // A timer wake corresponds to the scheduled mark. Startup work such as
    // NTP synchronization may have taken a few seconds, so scan immediately
    // instead of waiting for the next mark and skipping this cycle.
    wakeFromTimer = false;
    Serial.println("Timer wake complete; starting scheduled scan now.");

  } else {

    // Initial power-on: sleep until the first wall-clock mark.
    deepSleepUntilNextClockMark();
  }

  Serial.println("Starting 120-second BLE scan at the clock mark...");

  // Scan for ten seconds.
  pBLEScan->start(SCAN_DURATION_SECONDS, false);

  // Release scan results.
  pBLEScan->clearResults();

  Serial.println("Scan complete.");

  // Close storage and deep sleep until the next wall-clock mark.
  deepSleepUntilNextClockMark();
}
