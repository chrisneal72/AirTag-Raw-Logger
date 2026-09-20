// AirTag Raw Logger V2
//
// Based on:
// Matthew KuKanich - ESP32-AirTag-Scanner
// https://github.com/MatthewKuKanich/ESP32-AirTag-Scanner
//
// V2 changes:
//   - Connects to Wi-Fi during startup
//   - Synchronizes system clock using NTP
//   - Uses Arizona local time for timestamps
//   - Prints real date/time with every observation
//   - Uses gettimeofday() for actual wall-clock milliseconds
//   - Disconnects and turns off Wi-Fi after time synchronizationcpp
//   - Logs EVERY matching AirTag advertisement
//   - No application-level MAC deduplication
//   - Still Serial output only
//   - SD logging and scheduled sleep will be added later
//
// ESP32-WROOM / ESP32-S3

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <sys/time.h>

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>


// ============================================================
// Wi-Fi configuration
// ============================================================

const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";


// ============================================================
// NTP / Time configuration
// ============================================================

// Arizona local time is UTC-7.
// Arizona does not observe daylight saving time.
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.nist.gov";

const long GMT_OFFSET_SEC = -7 * 3600;
const int DAYLIGHT_OFFSET_SEC = 0;


// ============================================================
// BLE configuration
// ============================================================

int scanTime = 1;

BLEScan* pBLEScan;

unsigned long observationCount = 0;


// ============================================================
// Print current system time
// ============================================================

void printCurrentTime() {

  struct timeval tv;

  if (gettimeofday(&tv, nullptr) != 0) {
    Serial.println("ERROR: System time is not available.");
    return;
  }

  struct tm timeinfo;

  if (!localtime_r(&tv.tv_sec, &timeinfo)) {
    Serial.println("ERROR: Local time conversion failed.");
    return;
  }

  char timeString[32];

  strftime(
    timeString,
    sizeof(timeString),
    "%Y-%m-%d %H:%M:%S",
    &timeinfo
  );

  Serial.print(timeString);

  // tv_usec contains the actual microseconds from the
  // synchronized system clock.
  //
  // We only display milliseconds.

  int milliseconds = tv.tv_usec / 1000;

  Serial.print(".");

  if (milliseconds < 100) {
    Serial.print("0");
  }

  if (milliseconds < 10) {
    Serial.print("0");
  }

  Serial.print(milliseconds);
}


// ============================================================
// Connect to Wi-Fi and synchronize time
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
    NTP_SERVER_2
  );

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

  // We don't need Wi-Fi during BLE scanning.
  Serial.println("Disconnecting Wi-Fi...");

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  Serial.println("Wi-Fi disabled.");

  return true;
}


// ============================================================
// BLE advertisement callback
// ============================================================

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {

    void onResult(BLEAdvertisedDevice advertisedDevice) {

      // Get raw advertisement payload
      uint8_t* payLoad = advertisedDevice.getPayload();
      size_t payLoadLength = advertisedDevice.getPayloadLength();


      // --------------------------------------------------------
      // Look for AirTag / Find My advertisement signatures.
      //
      // Matthew's original scanner searches for:
      //
      //   1E FF 4C 00
      //
      // and:
      //
      //   4C 00 12 19
      //
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
      // IMPORTANT:
      //
      // There is deliberately NO MAC deduplication here.
      //
      // Every matching advertisement received by the callback
      // is recorded.
      // --------------------------------------------------------

      observationCount++;

      String macAddress =
          advertisedDevice.getAddress().toString().c_str();

      macAddress.toUpperCase();

      int rssi = advertisedDevice.getRSSI();


      // --------------------------------------------------------
      // Print observation
      // --------------------------------------------------------

      Serial.println();
      Serial.println("========================================");

      Serial.print("AirTag observation #");
      Serial.println(observationCount);

      Serial.print("Time:        ");
      printCurrentTime();
      Serial.println();

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
  Serial.println("AirTag Raw Logger V2");
  Serial.println("Arizona local time: UTC-7");
  Serial.println("========================================");


  // ----------------------------------------------------------
  // Get accurate Arizona local date/time first.
  // ----------------------------------------------------------

  bool timeValid = initializeTime();

  if (!timeValid) {

    Serial.println();
    Serial.println("WARNING:");
    Serial.println("System clock was not synchronized.");
    Serial.println("BLE scanning will continue.");
    Serial.println();
  }


  // ----------------------------------------------------------
  // Initialize BLE
  // ----------------------------------------------------------

  Serial.println("Initializing BLE...");

  BLEDevice::init("");

  pBLEScan = BLEDevice::getScan();

  // TRUE = request callbacks for duplicate advertisements.
  //
  // This is important for our experiment.
  pBLEScan->setAdvertisedDeviceCallbacks(
      new MyAdvertisedDeviceCallbacks(),
      true
  );

  // Keep Matthew's original scan configuration.
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

  // Scan for one second.

  BLEScanResults foundDevicesScan =
      pBLEScan->start(scanTime, false);

  // Release scan results.

  pBLEScan->clearResults();

  delay(50);
}
