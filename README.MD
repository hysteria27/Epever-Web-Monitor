# EPEVER Solar Web Monitor

A real-time solar monitoring system for EPEVER MPPT Charge Controllers (Tracer/Xtra series) using an ESP32 and Google Firebase. This project features a responsive web dashboard for live telemetry, historical data analysis, and Over-The-Air (OTA) firmware updates.

## 🚀 Features
- **Real-Time Monitoring**: View PV power, Battery Voltage, Charge Current, Load Power, and Controller Temperature updated every 5 seconds.
- **Live Dashboard**: Responsive web interface built with Tailwind CSS, featuring visual gauges for SOC and live power charts.
- **Historical Logging**: Automatically logs system data to Firebase Realtime Database every 15 minutes.
- **Data Analysis**: View intraday performance charts and download historical logs as CSV files.
- **OTA Firmware Updates**: Upload compiled .bin firmware files directly via the web dashboard to wirelessly update the ESP32.
- **Status Indicators**: Visual indicators for Wi-Fi, Firebase, and Modbus RS485 connection status.

## 🛠️ Tech Stack
**Hardware / Firmware**
- **Microcontroller**: ESP32
- **Communication**: Modbus RTU (RS485)
- **Framework**: Arduino
- **Key Libraries**:
    - `ModbusMaster` (Device communication)
    - `Firebase_ESP_Client` (Database & Auth)
    - `ArduinoJson` (Data parsing)
    - `WiFi.h` & `time.h`

## Web Interface
- **Frontend**: HTML5, JavaScript (Vanilla)
- **Styling**: Tailwind CSS (via CDN)
- **Visualization**: Chart.js (Live & History charts)
- **Icons**: Lucide Icons
- **Backend**: Firebase Realtime Database & Firebase Storage

## 📂 Project Structure
- `Epever.ino`: The main firmware logic. Handles Modbus polling, Wi-Fi connection, and data synchronization with Firebase.
- `public/index.html`: The client-side dashboard. Connects directly to Firebase to display data without a dedicated backend server.
- `config.h` (Gitignored): Stores sensitive credentials for the ESP32 (Wi-Fi, Firebase keys).
- `fbConfig.js` (Gitignored): Stores the Firebase Web SDK configuration.

## ⚙️ Configuration
## 1. ESP32 Firmware Setup
Create a `config.h` file in the Arduino project directory with the following credentials:
```c++
#define WIFI_SSID "YOUR_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

#define API_KEY "FIREBASE_WEB_API_KEY"
#define DATABASE_URL "https://your-project.firebaseio.com/"
#define STORAGE_BUCKET_ID "your-project.appspot.com"

// RS485 Pins (Customize based on your wiring)
#define RX_PIN 16
#define TX_PIN 17
#define RS485_DE_RE_PIN 4 
```
## 2. Web Dashboard Setup
Create a `fbConfig.js` file in the public/ folder:
```javascript
const firebaseConfig = {
  apiKey: "YOUR_API_KEY",
  authDomain: "your-project.firebaseapp.com",
  databaseURL: "https://your-project.firebaseio.com",
  projectId: "your-project",
  storageBucket: "your-project.appspot.com",
  messagingSenderId: "...",
  appId: "..."
};
```

## 📊 Data Flow
1. **Polling**: The ESP32 queries the EPEVER controller via RS485 every 5 seconds.
2. **Sync**:
    - **Live Data**: Pushed to /epever/live (overwritten every update).
    - **History**: Pushed to /epever/history (appended every 15 minutes).
3. **Visualization**: The web browser listens to changes in the Firebase Database and updates the DOM and Charts instantly.

## ☁️ OTA Updates
To update the device remotely:
1. Compile your code in Arduino IDE (`Sketch` -> `Export Compiled Binary`).
2. Go to the **OTA** tab in the Web Dashboard.
3. Drag and drop the `.bin` file.
4. The ESP32 will detect the update trigger, download the firmware, and restart automatically.