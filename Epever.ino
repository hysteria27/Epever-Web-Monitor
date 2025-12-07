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
const char* FIRMWARE_VERSION = "1.2.2";
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
const uint16_t REG_BATTERY_STATUS     = 0x3200;  
const uint16_t REG_CHARGING_STATUS    = 0x3201;  
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
  float battVolt, chgAmps, chgPower, soc;
  float loadVolt, loadAmps, loadPower;
  float dailyEnergy, monthlyEnergy, yearlyEnergy, totalEnergy;
  uint16_t battTemp, deviceTemp, powerTemp;
  uint16_t battState, chargingState;

  uint16_t battType, battCap, tempComp;
  float hVoltDisc, chgLimitVolt, overVoltRec;
  float eqVolt, boostVolt, floatVolt;
  float boostRecVolt, lowVoltRec, underVoltRecover;
  float underVoltWarn, lowVoltDisc, dischLimitVolt;
};
EpeverData epever;

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
  Firebase.RTDB.setJSON(&fbDO, "epever/firmware_info", &json);
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
  static uint8_t step = 0;
  uint16_t reg = 0x3100;
  int result;
  switch (step) {
    case 0:
      reg = 0x3100;
      result = node.readInputRegisters(reg, 8);
      if (result == node.ku8MBSuccess) {
        epever.pvVolt   = node.getResponseBuffer(0) / 100.0f;
        epever.pvAmps   = node.getResponseBuffer(1) / 100.0f;
        epever.pvPower  = ((uint32_t)node.getResponseBuffer(2) | ((uint32_t)node.getResponseBuffer(3) << 16)) / 100.0f;
        epever.battVolt = node.getResponseBuffer(4) / 100.0f;
        epever.chgAmps  = node.getResponseBuffer(5) / 100.0f;
        epever.chgPower = ((uint32_t)node.getResponseBuffer(6) | ((uint32_t)node.getResponseBuffer(7) << 16)) / 100.0f;
      } else WebSerial.println("Error reading registers at 0x" + String(reg, HEX) + ": " + String(result));
      step++;
    break;

    case 1: {
      reg = 0x310C;
      result = node.readInputRegisters(reg, 4);
      if (result == node.ku8MBSuccess) {
        epever.loadVolt    = node.getResponseBuffer(0) / 100.0f;
        epever.loadAmps    = node.getResponseBuffer(1) / 100.0f;
        epever.loadPower   = ((uint32_t)node.getResponseBuffer(2) | ((uint32_t)node.getResponseBuffer(3) << 16)) / 100.0f;
      } else WebSerial.println("Error reading registers at 0x" + String(reg, HEX) + ": " + String(result));
      step++;
    } break;

    case 2: {
      result = node.readInputRegisters(REG_BAT_SOC, 1); // REG 0x311A
      if (result == node.ku8MBSuccess) {
        epever.soc = node.getResponseBuffer(0);
      } else WebSerial.println("Error reading registers at 0x" + String(REG_BAT_SOC, HEX) + ": " + String(result));
      step++;
    } break;

    case 3: {
      result = node.readInputRegisters(REG_BATTERY_STATUS, 2); // REG 0x3200-0x3201
      if (result == node.ku8MBSuccess) {
        epever.battState      = node.getResponseBuffer(0);
        epever.chargingState  = node.getResponseBuffer(1);
        epever.connected = true;
      } else {
        epever.connected = false;
        WebSerial.println("Error reading registers at 0x" + String(REG_BATTERY_STATUS, HEX) + ": " + String(result));
      }
      step++;
    } break;

    case 4: {
      reg = 0x330C; // Daily Energy Low Reg
      result = node.readInputRegisters(reg, 4);
      if (result == node.ku8MBSuccess) {
        epever.dailyEnergy    = ((uint32_t)node.getResponseBuffer(0) | ((uint32_t)node.getResponseBuffer(1) << 16)) / 100.0f;
        epever.monthlyEnergy  = ((uint32_t)node.getResponseBuffer(2) | ((uint32_t)node.getResponseBuffer(3) << 16)) / 100.0f;
      } else WebSerial.println("Error reading registers at 0x" + String(reg, HEX) + ": " + String(result));
      step++;
    } break;

    default: step = 0;
  }
}

void readParameters() {
  int result;
  result = node.readHoldingRegisters(0x9000, 15);
  if (result == node.ku8MBSuccess) {
    epever.battType          = node.getResponseBuffer(0);
    epever.battCap           = node.getResponseBuffer(1);
    epever.tempComp          = node.getResponseBuffer(2) / 100.0f;
    epever.hVoltDisc         = node.getResponseBuffer(3) / 100.0f;
    epever.chgLimitVolt      = node.getResponseBuffer(4) / 100.0f;
    epever.overVoltRec       = node.getResponseBuffer(5) / 100.0f;
    epever.eqVolt            = node.getResponseBuffer(6) / 100.0f;
    epever.boostVolt         = node.getResponseBuffer(7) / 100.0f;
    epever.floatVolt         = node.getResponseBuffer(8) / 100.0f;
    epever.boostRecVolt      = node.getResponseBuffer(9) / 100.0f;
    epever.lowVoltRec        = node.getResponseBuffer(10) / 100.0f;
    epever.underVoltRecover  = node.getResponseBuffer(11) / 100.0f;
    epever.underVoltWarn     = node.getResponseBuffer(12) / 100.0f;
    epever.lowVoltDisc       = node.getResponseBuffer(13) / 100.0f;
    epever.dischLimitVolt    = node.getResponseBuffer(14) / 100.0f;
  }
  delay(5);
}

void requestParameterHandler() {
  if (!Firebase.ready()) {
    WebSerial.println("Firebase not ready");
    return;
  } 

  bool isRequested = Firebase.RTDB.getBool(&fbDO, "epever/parameters/isRequested") ? fbDO.boolData() : false;
  if (isRequested && epever.connected) {
    readParameters();

    FirebaseJson param;
    param.set("batt_type",          epever.battType);
    param.set("batt_capacity",      epever.battCap);
    param.set("temp_compensation",  epever.tempComp);
    param.set("h_voltage_disconnect",   epever.hVoltDisc);
    param.set("charging_limit_voltage", epever.chgLimitVolt);
    param.set("overvoltage_reconnect",  epever.overVoltRec);
    param.set("equalization_voltage",   epever.eqVolt);
    param.set("boost_voltage",          epever.boostVolt);
    param.set("float_voltage",          epever.floatVolt);
    param.set("boost_reconnect_voltage",epever.boostRecVolt);
    param.set("low_voltage_reconnect",  epever.lowVoltRec);
    param.set("undervoltage_recover",   epever.underVoltRecover);
    param.set("undervoltage_warning",   epever.underVoltWarn);
    param.set("low_voltage_disconnect", epever.lowVoltDisc);
    param.set("discharge_limit_voltage",epever.dischLimitVolt);

    Firebase.RTDB.setJSON(&fbDO, "epever/parameters/data", &param);
    Firebase.RTDB.setBool(&fbDO, "epever/parameters/isRequested", false);

    WebSerial.println("Parameters read and sent to Firebase");
  }
}

void loop() {
  unsigned long currentMillis = millis();

  // --- 1. Live Data Update (Fast) ---
  if (currentMillis - lastUpdate > UPDATE_INTERVAL) {
    lastUpdate = currentMillis;
    readSensors();
    requestParameterHandler();
    
    if (Firebase.ready() && epever.connected) {
      time_t now = getTimestamp();     
      
      Firebase.RTDB.setBool(&fbDO, "epever/is_online", true);  // Set device online status 
      FirebaseJson json;
      if (now > 10000) {
        json.set("timestamp", (int)now);                // Timestamp
        json.set("isConnected", epever.connected);      // Modbus Connection Status
        json.set("pv/volt", epever.pvVolt);             // PV Voltage
        json.set("pv/amps", epever.pvAmps);             // PV Current
        json.set("pv/power", epever.pvPower);           // PV Power
        json.set("batt/volt", epever.battVolt);         // Battery Voltage
        json.set("batt/amps", epever.chgAmps);          // Charge Current
        json.set("batt/power", epever.chgPower);        // Charge Power
        json.set("batt/soc", epever.soc);               // Battery SOC
        json.set("load/power", epever.loadPower);       // Load Power
        json.set("load/amps", epever.loadAmps);         // Load Current
        json.set("temp", epever.deviceTemp);            // Temperature
        json.set("daily_kwh", epever.dailyEnergy);      // Daily Energy
        json.set("batt_state", epever.battState);       // Battery Status
        json.set("status_code", epever.chargingState);  // Status Code
        
        Firebase.RTDB.setJSON(&fbDO, "epever/live", &json);
      }
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
    
    if (Firebase.ready() && epever.connected) {
      time_t now = getTimestamp();
      if (now > 10000) {
        FirebaseJson hist;
        hist.set("hStamp",    (int)now);             // Timestamp
        hist.set("hCCode",    epever.chargingState); // Status Code
        hist.set("hPWatt",    epever.pvPower);       // Power
        hist.set("hBVolt",    epever.battVolt);      // Battery
        hist.set("hBSOC",     epever.soc);           // SOC
        hist.set("hDayWatt",  epever.dailyEnergy);   // Daily Energy
        
        // Push adds a new unique node to the list
        Firebase.RTDB.pushJSON(&fbHist, "epever/history", &hist);
        WebSerial.println("History Log Pushed");
      }
    } else WebSerial.println("Skipping History Log - MODBUS Disconnected");
  }
}