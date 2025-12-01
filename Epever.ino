#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <ModbusMaster.h>
#include <ArduinoJson.h>
#include "time.h" // Added for Real Time
#include "config.h"

// Firebase Addons
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

FirebaseData fbDO;   // Database (Live Data)
FirebaseData fbHist; // Database (History Data)
FirebaseData fbOTA;  // OTA
FirebaseAuth auth;
FirebaseConfig config;
ModbusMaster node;

// --- FIRMWARE INFO ---
const char* FIRMWARE_VERSION = "1.0.7";
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 25200; // WIB (UTC+7) = 7 * 3600
const int   DAYLIGHT_OFFSET_SEC = 0;

unsigned long lastUpdate = 0;
const int UPDATE_INTERVAL = 5000; // Update Live Data every 5s

unsigned long lastHistoryUpdate = 0;
const int HISTORY_INTERVAL = 15 * 60 * 1000; // Save History every 15 mins

// --- EPEVER PIN CONFIG ---
#define RS485_DE_RE_PIN 2     
#define RX_PIN 16
#define TX_PIN 17

// --- REGISTERS ---
// Live Data
const uint16_t REG_PV_VOLTAGE      = 0x3100;
const uint16_t REG_PV_CURRENT      = 0x3101; 
const uint16_t REG_PV_POWER_L      = 0x3102;
const uint16_t REG_BAT_VOLTAGE     = 0x3104; 
const uint16_t REG_CHARGE_CURRENT  = 0x3105;
const uint16_t REG_LOAD_VOLTAGE    = 0x310C; 
const uint16_t REG_LOAD_CURRENT    = 0x310D;
const uint16_t REG_LOAD_POWER_L    = 0x310E; 
const uint16_t REG_TEMP_CTRL       = 0x3111;
const uint16_t REG_BAT_SOC         = 0x311A;
const uint16_t REG_STATUS          = 0x3201;
const uint16_t REG_DAILY_ENERGY_L  = 0x330C;

struct EpeverData {
  bool connected; // Modbus connection status
  float pvVolt, pvAmps, pvPower;
  float battVolt, chgAmps, battSOC;
  float loadVolt, loadAmps, loadPower;
  float temp, dailyEnergy;
  uint16_t status;
};
EpeverData live;

bool isOnline = false; // Device online status

// Get Unix Timestamp
time_t getTimestamp() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    return 0;
  }
  time_t now;
  time(&now);
  return now;
}

void preTransmission() { digitalWrite(RS485_DE_RE_PIN, HIGH); delay(1); }
void postTransmission() { digitalWrite(RS485_DE_RE_PIN, LOW); delay(1); }

// --- CALLBACK OTA ---
void fcsDownloadCallback(FCS_DownloadStatusInfo info) {
    if (info.status == fb_esp_fcs_download_status_init) {
        Serial.println("Downloading Firmware...");
    } else if (info.status == fb_esp_fcs_download_status_download) {
        Serial.printf("Progress: %d%%\n", info.progress);
    } else if (info.status == fb_esp_fcs_download_status_complete) {
        Serial.println("Download Success! Restarting...");
        delay(2000);
        ESP.restart();
    } else if (info.status == fb_esp_fcs_download_status_error) {
        Serial.printf("Download Failed: %s\n", info.errorMsg.c_str());
    }
}

void setup() {
  Serial.begin(115200);

  // --- WIFI CONNECTION ---
  unsigned long startWifiAttemptTime = millis();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
    // Cek apakah sudah melebihi waktu tunggu maksimal (30 detik)
    if (millis() - startWifiAttemptTime > 900000) { // 15 menit
      Serial.println("\n\nGagal terhubung ke WiFi dalam 15 menit.");
      Serial.println("Mematikan perangkat (Deep Sleep)...");
      esp_deep_sleep_start(); // OPSI 1: Matikan selamanya (sampai tombol RESET ditekan manual)
      // OPSI 2: Tidur sebentar lalu coba lagi (misal tidur 1 jam)
      // esp_sleep_enable_timer_wakeup(3600ULL * 1000000ULL); // 3600 detik = 1 jam
      // esp_deep_sleep_start();
    }
  }
  Serial.println(" Connected!");
  
  pinMode(RS485_DE_RE_PIN, OUTPUT); digitalWrite(RS485_DE_RE_PIN, LOW);
  Serial2.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);
  node.begin(1, Serial2);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);

  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;

  // Init Time (NTP)
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  config.token_status_callback = tokenStatusCallback; 

  config.timeout.wifiReconnect = 10 * 1000;
  config.timeout.socketConnection = 60 * 1000;
  config.timeout.sslHandshake = 60 * 1000;
  config.timeout.serverResponse = 60 * 1000;
  config.timeout.rtdbKeepAlive = 45 * 1000;

  fbDO.setBSSLBufferSize(2048, 1024);
  fbOTA.setBSSLBufferSize(4096, 1024);  
  fbOTA.setResponseSize(2048);
  
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  if (Firebase.ready()) {
    Serial.println("Firebase Realtime Database Connected");
    isOnline = true;
    if (!Firebase.RTDB.setBool(&fbDO, "/epever/is_online", isOnline)) {
      isOnline = false;
    }
  } else {
    Serial.printf("RTDB Connection Failed: %s\n", fbDO.errorReason().c_str());
  }

  checkFirmware();
}

void checkFirmware() {
  if (!Firebase.ready()) return;
  FirebaseJson json;
  json.set("firmware_version", FIRMWARE_VERSION);
  json.set("firmware_date", __DATE__ " " __TIME__);
  json.set("chip_model", ESP.getChipModel());
  json.set("free_space", (int)ESP.getFreeSketchSpace());
  Firebase.RTDB.setJSON(&fbDO, "/epever/firmware_info", &json);
}

void checkOTA() {
  if (!Firebase.ready()) return;
  // Use getBool with a smaller timeout if possible or check infrequently
  if (Firebase.RTDB.getBool(&fbDO, "epever/firmware_info/ota_trigger")) {
    if (fbDO.boolData() == true) {
      Serial.println("\n--- OTA TRIGGER RECEIVED ---");
      Firebase.RTDB.setBool(&fbDO, "epever/firmware_info/ota_trigger", false);
      if (!Firebase.Storage.downloadOTA(&fbOTA, STORAGE_BUCKET_ID, "firmware.bin", fcsDownloadCallback)) {
        Serial.println(fbOTA.errorReason());
      }
    }
  }
}

void readSensors() {
  uint8_t result;

  result = node.readInputRegisters(REG_STATUS, 1);
  if (result == node.ku8MBSuccess) {
    live.status = node.getResponseBuffer(0);
    live.connected = true;
  } else {
    live.status = 0;
    live.connected = false;
    return; // Skip reading other registers if not connected
  }
  
  node.readInputRegisters(REG_PV_VOLTAGE, 2);
  live.pvVolt = node.getResponseBuffer(0) / 100.0f;
  live.pvAmps = node.getResponseBuffer(1) / 100.0f;
  delay(5);

  node.readInputRegisters(REG_PV_POWER_L, 2);
  live.pvPower = ((uint32_t)node.getResponseBuffer(0) | ((uint32_t)node.getResponseBuffer(1) << 16)) / 100.0f;
  delay(5);

  node.readInputRegisters(REG_BAT_VOLTAGE, 2);
  live.battVolt = node.getResponseBuffer(0) / 100.0f;
  live.chgAmps = node.getResponseBuffer(1) / 100.0f;
  delay(5);

  node.readInputRegisters(REG_LOAD_VOLTAGE, 4);
  live.loadVolt = node.getResponseBuffer(0) / 100.0f;
  live.loadAmps = node.getResponseBuffer(1) / 100.0f;
  live.loadPower = ((uint32_t)node.getResponseBuffer(2) | ((uint32_t)node.getResponseBuffer(3) << 16)) / 100.0f;
  delay(5);

  node.readInputRegisters(REG_TEMP_CTRL, 1);
  live.temp = node.getResponseBuffer(0) / 100.0f;
  delay(5);

  node.readInputRegisters(REG_BAT_SOC, 1);
  live.battSOC = node.getResponseBuffer(0);
  delay(5);

  node.readInputRegisters(REG_DAILY_ENERGY_L, 2);
  live.dailyEnergy = ((uint32_t)node.getResponseBuffer(0) | ((uint32_t)node.getResponseBuffer(1) << 16)) / 100.0f;
  delay(5);
}

void loop() {
  checkOTA();

  // 1. Live Data Update (Fast)
  if (millis() - lastUpdate > UPDATE_INTERVAL) {
    lastUpdate = millis();
    readSensors();
    
    if (Firebase.ready()) {
      time_t now = getTimestamp();
      
      FirebaseJson json;
      json.set("isConnected", live.connected);  // Modbus Connection Status
      json.set("pv/volt", live.pvVolt);
      json.set("pv/amps", live.pvAmps);
      json.set("pv/power", live.pvPower); 
      json.set("batt/volt", live.battVolt); 
      json.set("batt/amps", live.chgAmps);
      json.set("batt/soc", live.battSOC); 
      json.set("load/power", live.loadPower); 
      json.set("load/amps", live.loadAmps);
      json.set("temp", live.temp); 
      json.set("daily_kwh", live.dailyEnergy); 
      json.set("status_code", live.status);
      
      // Use Real Timestamp if available, else millis
      if(now > 10000) json.set("timestamp", (int)now);
      else json.set("timestamp", millis());
      
      Firebase.RTDB.setJSON(&fbDO, "/epever/live", &json);
    }
  }

  // 2. History Data Update (Slow - Every 15 mins)
  if (millis() - lastHistoryUpdate > HISTORY_INTERVAL) {
    lastHistoryUpdate = millis();
    
    if (Firebase.ready()) {
      time_t now = getTimestamp();
      if (now > 10000) {
        FirebaseJson hist;
        hist.set("hStamp", (int)now); // Timestamp
        hist.set("hCCode", live.status); // Status Code
        hist.set("hPWatt", live.pvPower); // Power
        hist.set("hBVolt", live.battVolt); // Battery
        hist.set("hBSOC", live.battSOC);  // SOC
        hist.set("hDayWatt", live.dailyEnergy); // Daily Energy
        
        // Push adds a new unique node to the list
        Firebase.RTDB.pushJSON(&fbHist, "/epever/history", &hist);
        Serial.println("History Log Pushed");
      }
    }
  }
}