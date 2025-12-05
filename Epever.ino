#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <ModbusMaster.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <WebSerial.h>
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
const char* FIRMWARE_VERSION = "1.1.0";
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 25200; // WIB (UTC+7) = 7 * 3600
const int   DAYLIGHT_OFFSET_SEC = 0;

unsigned long lastUpdate = 0;
const int UPDATE_INTERVAL = 5000; // Update Live Data every 5s
unsigned long lastOTACheck = 0;
const int OTA_CHECK_INTERVAL = 60000; // Check OTA every 60s
unsigned long lastHistoryUpdate = 0;
const int HISTORY_INTERVAL = 15 * 60 * 1000; // Save History every 15 mins

// --- REGISTERS ---
// Real-time Data (Read Only)
const uint16_t REG_PV_VOLTAGE         = 0x3100;
const uint16_t REG_PV_CURRENT         = 0x3101; 
const uint16_t REG_PV_POWER_L         = 0x3102;
const uint16_t REG_BAT_VOLTAGE        = 0x3104; 
const uint16_t REG_CHARGE_CURRENT     = 0x3105;
const uint16_t REG_LOAD_VOLTAGE       = 0x310C; 
const uint16_t REG_LOAD_CURRENT       = 0x310D;
const uint16_t REG_LOAD_POWER_L       = 0x310E; 
const uint16_t REG_TEMP_CTRL          = 0x3111;
const uint16_t REG_BAT_SOC            = 0x311A;
const uint16_t REG_STATUS             = 0x3201;  // Charging Equip. Status
const uint16_t REG_DAILY_ENERGY_L     = 0x330C;
const uint16_t REG_MAX_BATTERY_VOLT   = 0x3302;
// Parameter Settings (Read/Write)
const uint16_t REG_BATTERY_TYPE         = 0x9000;
const uint16_t REG_BATTERY_CAPACITY     = 0x9001;
const uint16_t REG_TEMP_COMPENSATION    = 0x9002;
const uint16_t REG_H_VOLT_DISCONNECT    = 0x9003;
const uint16_t REEG_CHARGING_LIMIT_VOLT = 0x9004;
const uint16_t REG_OVERVOLT_RECONNECT   = 0x9005;
const uint16_t REG_EQ_VOLTAGE           = 0x9006;
const uint16_t REG_BOOST_VOLTAGE        = 0x9007;
const uint16_t REG_FLOAT_VOLTAGE        = 0x9008;
const uint16_t REG_BOOST_RECONNECT_VOLT = 0x9009;
const uint16_t REG_LOW_VOLT_RECONNECT   = 0x900A;
const uint16_t REG_UNDER_VOLT_RECOVER   = 0x900B;
const uint16_t REG_UNDER_VOLT_WARNING   = 0x900C;
const uint16_t REG_LOW_VOLT_DISCONNECT  = 0x900D;
const uint16_t REG_DISCHARGE_LIMIT_VOLT = 0x900E;

struct EpeverData {
  bool connected;                             // Flag modbus connection status
  float pvVolt, pvAmps, pvPower;
  float battVolt, chgAmps, chgPower, battSOC;
  float loadVolt, loadAmps, loadPower;
  float temp, dailyEnergy;
  uint16_t status;
};
EpeverData live;

AsyncWebServer webLog(8632);

// --- Get Unix Timestamp ---
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
        WebSerial.println("Downloading Firmware...");
    } else if (info.status == fb_esp_fcs_download_status_download) {
        WebSerial.printf("Progress: %d%%\n", info.progress);
    } else if (info.status == fb_esp_fcs_download_status_complete) {
        WebSerial.println("Download Success! Restarting...");
        delay(2000);
        ESP.restart();
    } else if (info.status == fb_esp_fcs_download_status_error) {
        WebSerial.printf("Download Failed: %s\n", info.errorMsg.c_str());
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
    // Cek apakah sudah melebihi waktu tunggu maksimal
    if (millis() - startWifiAttemptTime > 900000) { // 15 menit
      Serial.println("\n\nGagal terhubung ke WiFi dalam 15 menit.");
      Serial.println("Mematikan perangkat (Deep Sleep)...");

      // Masuk ke mode deep sleep untuk menghemat daya
      esp_sleep_enable_timer_wakeup(3600ULL * 1000000ULL); // 3600 detik = 1 jam
      esp_deep_sleep_start();
    }
  }
  Serial.print("Web Logging started: ");
  Serial.print(WiFi.localIP());
  Serial.println(":8632/webserial");

  WebSerial.begin(&webLog);
  webLog.on("/", HTTP_GET, [](AsyncWebServerRequest *request){request->redirect("/webserial");});
  webLog.begin();
  WebSerial.println("WiFi Connected!");
  
  Serial2.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);
  pinMode(RS485_DE_RE_PIN, OUTPUT);
  digitalWrite(RS485_DE_RE_PIN, LOW);
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
  
  if (Firebase.RTDB.getBool(&fbDO, "epever/firmware_info/ota_trigger")) {
    if (fbDO.boolData() == true) {
      WebSerial.println("\n--- OTA TRIGGER RECEIVED ---");
      Firebase.RTDB.setBool(&fbDO, "epever/firmware_info/ota_trigger", false);
      if (!Firebase.Storage.downloadOTA(&fbOTA, STORAGE_BUCKET_ID, "firmware.bin", fcsDownloadCallback)) {
        WebSerial.println(fbOTA.errorReason());
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
  
  node.readInputRegisters(REG_PV_VOLTAGE, 1);
  live.pvVolt = node.getResponseBuffer(0) / 100.0f;
  delay(5);

  node.readInputRegisters(REG_PV_CURRENT, 1);
  live.pvAmps = node.getResponseBuffer(0) / 100.0f;
  delay(5);

  node.readInputRegisters(REG_PV_POWER_L, 2);
  live.pvPower = ((uint32_t)node.getResponseBuffer(0) | ((uint32_t)node.getResponseBuffer(1) << 16)) / 100.0f;
  delay(5);

  node.readInputRegisters(REG_BAT_VOLTAGE, 1);
  live.battVolt = node.getResponseBuffer(0) / 100.0f;
  delay(5);

  node.readInputRegisters(REG_CHARGE_CURRENT, 1);
  live.chgAmps = node.getResponseBuffer(0) / 100.0f;
  delay(5);

  node.readInputRegisters(REG_LOAD_VOLTAGE, 1);
  live.loadVolt = node.getResponseBuffer(0) / 100.0f;
  delay(5);

  node.readInputRegisters(REG_LOAD_CURRENT, 1);
  live.loadAmps = node.getResponseBuffer(0) / 100.0f;
  delay(5);

  node.readInputRegisters(REG_LOAD_POWER_L, 2);
  live.loadPower = ((uint32_t)node.getResponseBuffer(0) | ((uint32_t)node.getResponseBuffer(1) << 16)) / 100.0f;
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

void readSensors2() {
  static uint8_t step = 0;
  uint16_t reg = 0x3100;
  int result;
  switch (step) {
    case 0: // PV Voltage, Current, Power, Battery Voltage, Charge Current, Charge Power (8 Registers: V1)
      reg = 0x3100;
      result = node.readInputRegisters(reg, 8);
      if (result == node.ku8MBSuccess) {
        float pvVolt    = node.getResponseBuffer(0) / 100.0f;
        float pvAmp     = node.getResponseBuffer(1) / 100.0f;
        float pvWatt    = ((uint32_t)node.getResponseBuffer(2) | ((uint32_t)node.getResponseBuffer(3) << 16)) / 100.0f;
        float battVolt  = node.getResponseBuffer(4) / 100.0f;
        float chgAmp    = node.getResponseBuffer(5) / 100.0f;
        float chgWatt   = ((uint32_t)node.getResponseBuffer(6) | ((uint32_t)node.getResponseBuffer(7) << 16)) / 100.0f;

        // --- Print ke WebSerial dengan format String Concatenation ---
        WebSerial.println("PV Volt: "   + String(pvVolt)    + " V");
        WebSerial.println("PV Amp: "    + String(pvAmp)     + " A");
        WebSerial.println("PV Watt: "   + String(pvWatt)    + " W");
        WebSerial.println("Bat Volt: "  + String(battVolt)  + " V");
        WebSerial.println("Chg Amp: "   + String(chgAmp)    + " A");
        WebSerial.println("Chg Watt: "  + String(chgWatt)   + " W");

        /* // --- Simpan ke Struct 'live' (Sesuai kode Anda sebelumnya) ---
        live.pvVolt   = pvVolt;
        live.pvAmps   = pvAmp;
        live.pvPower  = pvWatt;
        live.battVolt = battVolt;
        live.chgAmps  = chgAmp;
        live.chgPower = chgWatt; */
      } else WebSerial.println("Error reading registers at 0x" + String(reg, HEX) + ": " + String(result));
      step++;
    break;

    case 1:
      reg = 0x310C;
      result = node.readInputRegisters(reg, 8);
      if (result == node.ku8MBSuccess) {
        float loadVolt    = node.getResponseBuffer(0) / 100.0f;
        float loadAmp     = node.getResponseBuffer(1) / 100.0f;
        float loadWatt    = ((uint32_t)node.getResponseBuffer(2) | ((uint32_t)node.getResponseBuffer(3) << 16)) / 100.0f;
        float battTemp    = node.getResponseBuffer(4) / 100.0f;
        float deviceTemp  = node.getResponseBuffer(5) / 100.0f;
        float powerTemp   = node.getResponseBuffer(6) / 100.0f;
        float soc         = node.getResponseBuffer(7) / 100.0f;

        // --- Print ke WebSerial dengan format String Concatenation ---
        WebSerial.println("Load Volt: "                   + String(loadVolt)    + " V");
        WebSerial.println("Load Amp: "                    + String(loadAmp)     + " A");
        WebSerial.println("Load Watt: "                   + String(loadWatt)    + " W");
        WebSerial.println("Battery Temperature: "         + String(battTemp)    + " °C");
        WebSerial.println("Device Temperature: "          + String(deviceTemp)  + " °C");
        WebSerial.println("Power Component Temperature: " + String(powerTemp)   + " °C");
        WebSerial.println("Battery SOC: "                 + String(soc)         + " %");
      } else WebSerial.println("Error reading registers at 0x" + String(reg, HEX) + ": " + String(result));
      step++;
    break;

    case 2:
      reg = 0x311D;
      result = node.readInputRegisters(reg, 1);
      if (result == node.ku8MBSuccess) {
        float ratedPower = node.getResponseBuffer(0) / 100.0f;
        WebSerial.println("Rated Power: " + String(ratedPower) + " V");
      } else WebSerial.println("Error reading registers at 0x" + String(reg, HEX) + ": " + String(result));
      step++;
    break;
  }
}

void loop() {
  unsigned long currentMillis = millis();

  // --- 1. Live Data Update (Fast) ---
  if (currentMillis - lastUpdate > UPDATE_INTERVAL) {
    lastUpdate = currentMillis;
    readSensors();
    readSensors2();
    
    if (Firebase.ready() && live.connected) {
      time_t now = getTimestamp();     
      
      Firebase.RTDB.setBool(&fbDO, "/epever/is_online", true);  // Set device online status 
      FirebaseJson json;
      if (now > 10000) {
        json.set("timestamp", (int)now);          // Timestamp
        json.set("isConnected", live.connected);  // Modbus Connection Status
        json.set("pv/volt", live.pvVolt);         // PV Voltage
        json.set("pv/amps", live.pvAmps);         // PV Current
        json.set("pv/power", live.pvPower);       // PV Power
        json.set("batt/volt", live.battVolt);     // Battery Voltage
        json.set("batt/amps", live.chgAmps);      // Charge Current
        json.set("batt/soc", live.battSOC);       // Battery SOC
        json.set("load/power", live.loadPower);   // Load Power
        json.set("load/amps", live.loadAmps);     // Load Current
        json.set("temp", live.temp);              // Temperature
        json.set("daily_kwh", live.dailyEnergy);  // Daily Energy
        json.set("status_code", live.status);     // Status Code
        
        Firebase.RTDB.setJSON(&fbDO, "/epever/live", &json);
      }
      WebSerial.println("Live Data Updated");
    } else WebSerial.println("Skipping Live Data Update - MODBUS Disconnected");
  }

  // --- 2. OTA CHECK (Low Priority: 60s) ---
  // Checking every 60s is plenty. It frees up bandwidth for the sensors.
  if (currentMillis - lastOTACheck > OTA_CHECK_INTERVAL) {
    lastOTACheck = currentMillis;
    checkOTA(); 
  }

  // --- 3. History Data Update (Slow - Every 15 mins) ---
  if (currentMillis - lastHistoryUpdate > HISTORY_INTERVAL) {
    lastHistoryUpdate = currentMillis;
    readSensors();
    
    if (Firebase.ready() && live.connected) {
      time_t now = getTimestamp();
      if (now > 10000) {
        FirebaseJson hist;
        hist.set("hStamp", (int)now);           // Timestamp
        hist.set("hCCode", live.status);        // Status Code
        hist.set("hPWatt", live.pvPower);       // Power
        hist.set("hBVolt", live.battVolt);      // Battery
        hist.set("hBSOC", live.battSOC);        // SOC
        hist.set("hDayWatt", live.dailyEnergy); // Daily Energy
        
        // Push adds a new unique node to the list
        Firebase.RTDB.pushJSON(&fbHist, "/epever/history", &hist);
        WebSerial.println("History Log Pushed");
      }
    } else WebSerial.println("Skipping History Log - MODBUS Disconnected");
  }
}