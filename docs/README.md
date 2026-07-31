# Design and Implementation of an IoT-Enabled Water Monitoring and Automated Irrigation System for Green Farming

## Project Overview

| Field | Details |
|---|---|
| **Title** | IoT-Enabled Water Monitoring & Automated Irrigation System for Green Farming |
| **Objectives** | Smart agriculture using real-time environmental monitoring and automated control |
| **Microcontroller** | ESP32 (240 MHz dual-core, Wi-Fi/BT) |
| **Sensors** | DHT22, Soil Moisture (Capacitive), DS3231 RTC |
| **Actuator** | 12V Water Pump via Relay Module |
| **Interface** | Real-time WebSocket Web Dashboard |

---

## 1.5 Project Objectives

1. **Design** a smart agriculture monitoring system using a microcontroller and environmental sensors for real-time farm data acquisition.
2. **Create** an automated irrigation and alert mechanism that responds to changes in soil moisture and environmental conditions.
3. **Implement** a real-time monitoring interface that enables users to view farm conditions and support informed decision-making.

---

## Hardware Components

| Component | Model | Purpose |
|---|---|---|
| Microcontroller | **ESP32 Dev Module** | Central processing, Wi-Fi, WebSocket server |
| Temperature & Humidity | **DHT22 (AM2302)** | Reads ambient temperature (−40–80°C) and humidity (0–100%) |
| Soil Moisture | **Capacitive Soil Sensor v1.2** | Reads volumetric water content of soil |
| Real-Time Clock | **DS3231 RTC Module** | Accurate timestamping (I2C) |
| Water Pump | **12V DC Submersible Pump** | Delivers irrigation water |
| Relay Module | **5V 1-Channel Relay** | ESP32-controlled switch for 12V pump |
| Power Supply | **12V 2A DC Adapter** | Powers pump; Buck converter for 5V/3.3V rails |
| Resistor | 10kΩ pull-up | DHT22 data line |

---

## 2. System Architecture

```
╔══════════════════════════════════════════════════════════════════╗
║                    SMART FARM IoT SYSTEM                         ║
╠══════════════════════════════════════════════════════════════════╣
║                                                                   ║
║  ┌──────────────┐   ADC (GPIO34)   ┌─────────────────────────┐  ║
║  │ Soil Moisture│─────────────────►│                         │  ║
║  │   Sensor     │                  │       ESP32             │  ║
║  └──────────────┘                  │   (Dual-Core 240MHz)    │  ║
║                                    │                         │  ║
║  ┌──────────────┐   GPIO4 (1-wire) │  ┌─────────────────┐   │  ║
║  │    DHT22     │─────────────────►│  │  Sensor Manager │   │  ║
║  │  Temp & Hum  │                  │  │  WebSocket Srv  │   │  ║
║  └──────────────┘                  │  │  HTTP Server    │   │  ║
║                                    │  │  Auto Irrigator │   │  ║
║  ┌──────────────┐   I2C (21/22)    │  │  Alert Engine   │   │  ║
║  │  DS3231 RTC  │◄────────────────►│  └─────────────────┘   │  ║
║  │  Clock Mod.  │                  │                         │  ║
║  └──────────────┘                  │  WiFi 802.11 b/g/n      │  ║
║                                    └──────────┬──────────────┘  ║
║  ┌──────────────┐   GPIO26 (relay)             │ WebSocket :81   ║
║  │ 5V Relay Mod │◄────────────────────────────►│ HTTP      :80   ║
║  └──────┬───────┘                              │                  ║
║         │ 12V switched                    ┌────▼────────────┐    ║
║  ┌──────▼───────┐                         │  Web Dashboard  │    ║
║  │  12V Pump    │                         │  (Any Browser)  │    ║
║  └──────────────┘                         └─────────────────┘    ║
╚══════════════════════════════════════════════════════════════════╝
```

---

## 3. GPIO Pin Mapping (ESP32)

| GPIO | Pin Label | Connected To | Direction |
|---|---|---|---|
| **GPIO 4** | D4 | DHT22 Data | Input |
| **GPIO 21** | SDA | DS3231 SDA (I2C) | Bidirectional |
| **GPIO 22** | SCL | DS3231 SCL (I2C) | Bidirectional |
| **GPIO 26** | D26 | Relay IN (Pump) | Output |
| **GPIO 34** | VP | Soil Moisture AO | Input (ADC1) |
| **GPIO 2** | LED | Built-in LED | Output |
| **3.3V** | 3V3 | DHT22 VCC, RTC VCC, Sensor VCC | Power |
| **GND** | GND | All component GNDs | Ground |

> ⚠️ **Note**: GPIO34 is input-only; do not use as output. Use a 10kΩ pull-up resistor on DHT22 data line.

---

## 4. Wiring Diagram (Schematic Description)

### DHT22 Connection
```
DHT22 Pin 1 (VCC)  ──── 3.3V
DHT22 Pin 2 (DATA) ──── GPIO4 ──── 10kΩ ──── 3.3V  (pull-up)
DHT22 Pin 4 (GND)  ──── GND
```

### DS3231 RTC Connection (I2C)
```
DS3231 VCC ──── 3.3V
DS3231 GND ──── GND
DS3231 SDA ──── GPIO21 (ESP32 SDA)
DS3231 SCL ──── GPIO22 (ESP32 SCL)
```

### Soil Moisture Sensor Connection
```
Soil Sensor VCC ──── 3.3V
Soil Sensor GND ──── GND
Soil Sensor AO  ──── GPIO34 (ADC input)
```

### Relay & Pump Connection
```
Relay VCC  ──── 5V (from buck converter)
Relay GND  ──── GND
Relay IN   ──── GPIO26

Relay COM  ──── 12V PSU (+)
Relay NO   ──── Pump (+)   ← Normally Open: pump OFF when relay unenergized
Pump (-)   ──── 12V PSU (-)
```

> ⚠️ **Safety**: Use a flyback diode (1N4007) across pump terminals to suppress back-EMF.

### SD Card Module Connection (SPI)
```
SD VCC  ──── 5V (or 3.3V depending on module)
SD GND  ──── GND
SD CS   ──── GPIO5
SD MOSI ──── GPIO23
SD MISO ──── GPIO19
SD SCK  ──── GPIO18
```

### Horizontal Float Sensor Connection
```
Sensor Leg 1 ──── GPIO27 (Internal pull-up enabled in firmware)
Sensor Leg 2 ──── GND
```

---

## 5. Software Architecture

### Firmware Modules (`smart_farm.ino`)

| Module | Function |
|---|---|
| `readSensors()` | Reads DHT22, averages 10 ADC samples for soil, gets RTC time |
| `evaluateAlerts()` | Compares readings against thresholds, populates alert array |
| `autoIrrigationLogic()` | Turns pump ON below 30% moisture, OFF above 70% |
| `handlePumpSafety()` | Force-stops pump after 5 min; enforces 1 min cooldown |
| `broadcastState()` | Serializes state to JSON and broadcasts to all WS clients |
| `onWebSocketEvent()` | Parses incoming commands (pump_on/off, set_auto) |
| `setupHTTPRoutes()` | Serves `/api/status` and health endpoints |

### Dashboard Modules (`app.js`)

| Module | Function |
|---|---|
| `connectWebSocket()` | Manages WS connection with auto-reconnect |
| `processData()` | Dispatches incoming data to all UI update functions |
| `updateSensorCards()` | Updates value displays, ring gauge, progress bars |
| `updateAlerts()` | Renders alert items dynamically |
| `updateCharts()` | Pushes data into Chart.js line/bar charts |
| `appendLog()` | Inserts rows into session log table |
| `startDemo()` | Generates realistic simulated data when ESP32 unreachable |
| `sendPumpOn/Off()` | Sends WS commands to ESP32 |
| `setMode()` | Switches between Auto and Manual irrigation modes |

---

## 6. Irrigation Logic Flowchart

```
START
  │
  ▼
[Read Sensors every 5s]
  │
  ▼
[Auto Mode?]
  ├── NO  → [Manual Control via Dashboard]
  │
  └── YES
        │
        ▼
   [Soil < 30%?]
        ├── YES → [Cooldown expired?]
        │           ├── YES → [PUMP ON] → Monitor
        │           └── NO  → Wait
        │
        └── NO
              │
              ▼
         [Soil ≥ 70%?]
              ├── YES → [PUMP OFF] → Set cooldown
              └── NO  → Keep current state
                          │
                          ▼
                   [Pump ON > 5min?] → [SAFETY SHUTOFF]
```

---

## 7. Alert Thresholds

| Condition | Threshold | Alert Message |
|---|---|---|
| Drought Risk | Soil < 30% | `DROUGHT RISK: Soil moisture critically low` |
| Heat Stress | Temp > 35°C | `HEAT STRESS: Temperature exceeds safe limit` |
| Frost Warning | Temp < 4°C | `FROST WARNING: Temperature near freezing` |
| Low Humidity | Humidity < 30% | `LOW HUMIDITY: Increased evaporation rate` |
| Disease Risk | Humidity > 90% | `DISEASE RISK: Fungal risk elevated` |
| Pump Safety | Pump ON > 4.5 min | `PUMP WARNING: Approaching max run time` |

---

## 8. Installation & Setup

### Step 1: Arduino IDE Setup
1. Install **Arduino IDE 2.x**
2. Add ESP32 board package:
   - File → Preferences → Additional Board Manager URLs:
   - `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
3. Tools → Board → Boards Manager → Install **esp32 by Espressif Systems**

### Step 2: Install Libraries
Open Arduino IDE → Tools → Manage Libraries, install:

| Library | Author |
|---|---|
| `DHT sensor library` | Adafruit |
| `Adafruit Unified Sensor` | Adafruit |
| `RTClib` | Adafruit |
| `WebSockets` | Markus Sattler (arduinoWebSockets) |
| `ArduinoJson` | Benoit Blanchon |

### Step 3: Configure Firmware
Edit `firmware/config.h`:
```cpp
#define WIFI_SSID     "YourNetworkName"
#define WIFI_PASSWORD "YourPassword"
```

### Step 4: Calibrate Soil Sensor
1. Read `soilRaw` value in Serial Monitor with sensor in **open air** → set as `SOIL_DRY_VALUE`
2. Submerge sensor in water → read value → set as `SOIL_WET_VALUE`

### Step 5: Upload & Get IP
1. Select: Tools → Board → **ESP32 Dev Module**
2. Upload `smart_farm.ino`
3. Open Serial Monitor (115200 baud)
4. Note the IP address shown: `[WiFi] IP Address: 192.168.x.x`

### Step 6: Configure Dashboard
Edit `dashboard/app.js`:
```js
const CONFIG = {
  ESP32_IP: '192.168.x.x',   // ← Your ESP32 IP
  WS_PORT:  81,
  ...
};
```

### Step 7: Open Dashboard
Open `dashboard/index.html` in a browser, or serve it:
```bash
cd dashboard
python3 -m http.server 8080
# Then visit: http://localhost:8080
```

---

## 9. Project File Structure

```
smart_farm/
├── firmware/
│   ├── smart_farm.ino       # Main Arduino sketch
│   └── config.h             # Configuration & pin definitions
├── dashboard/
│   ├── index.html           # Web dashboard UI
│   ├── style.css            # Dark agri-tech theme styles
│   └── app.js               # WebSocket client & Chart.js logic
└── docs/
    └── README.md            # This file
```

---

## 10. Verification & Testing

| Test | Method | Expected Result |
|---|---|---|
| Sensor readings | Serial Monitor at 115200 baud | Values print every 5s |
| WebSocket | Browser DevTools → Network → WS | JSON frames arriving every 5s |
| Auto irrigation | Dry the sensor until < 30% | Pump activates automatically |
| Pump shutoff | Let soil reach 70% | Pump deactivates automatically |
| Safety shutoff | Hold pump ON for 5 min | Pump auto-stops |
| Dashboard demo | Open with default ESP32_IP | Simulated data appears immediately |
| Manual control | Click ON/OFF buttons | Pump state changes instantly |
| Alerts | Apply heat source near DHT22 | Alert appears in dashboard |

---

## 11. Power Budget

| Component | Voltage | Current | Power |
|---|---|---|---|
| ESP32 (WiFi active) | 3.3V | ~250 mA | 0.83 W |
| DHT22 | 3.3V | ~2.5 mA | 0.01 W |
| DS3231 | 3.3V | ~2 mA | 0.01 W |
| Soil Sensor | 3.3V | ~5 mA | 0.02 W |
| Relay Module | 5V | ~70 mA | 0.35 W |
| 12V Pump | 12V | ~500 mA | 6 W |
| **Total (pump ON)** | | | **~7.2 W** |
| **Total (pump OFF)** | | | **~1.2 W** |

---

## 12. New Components Added

### Horizontal Float Sensor (Water Tank Level)

| Detail | Value |
|---|---|
| **Type** | Magnetic horizontal float switch |
| **GPIO** | GPIO27 (`FLOAT_SENSOR_PIN` in config.h) |
| **Logic** | `INPUT_PULLUP` — float UP = LOW = tank FULL |
| **Behaviour** | Blocks pump activation when tank empty; triggers emergency shutoff if tank empties mid-cycle |
| **Alert** | `TANK EMPTY: Water reservoir low — refill required` |

**Wiring:**
```
Float Sensor Terminal 1 ──── GPIO27
Float Sensor Terminal 2 ──── GND
(Internal pull-up resistor used — no external resistor needed)
```

> ⚠️ Some float sensors have normally-open (NO) vs normally-closed (NC) variants. Adjust `FLOAT_TANK_FULL_LEVEL` in `config.h` between `LOW` and `HIGH` to match yours.

---

### SD Card Module (Optional — Graceful Fallback)

| Detail | Value |
|---|---|
| **Interface** | SPI (HSPI) |
| **CS Pin** | GPIO5 |
| **MOSI** | GPIO23 |
| **MISO** | GPIO19 |
| **SCK** | GPIO18 |
| **Log File** | `/farmlog.csv` on SD root |
| **Log Interval** | Every 30 seconds |
| **Fallback** | If SD absent: `[SD] Not found — SD logging disabled. System continues normally.` |

**CSV Log Format:**
```csv
Date,Time,Soil_Moisture_%,Temperature_C,Humidity_%,Tank_Level,Pump_State,Mode
2026-07-30,20:45:00,45,24.5,62.0,FULL,OFF,AUTO
2026-07-30,20:45:30,44,24.6,61.8,FULL,OFF,AUTO
```

**Graceful Fallback Behaviour:**
- SD card **not inserted** → `sdAvailable = false` → all sensors, pump, WiFi, WebSocket run normally
- SD card **inserted later** → requires reboot to detect (SD.begin() runs only at startup)
- SD status visible in dashboard header badge: `SD Offline` / `SD Ready` / `SD Logging`

---

## 13. ☁️ Cloud Backend Storage Recommendations

These are cloud platforms you can integrate with for storing sensor readings remotely, enabling access from anywhere and long-term historical analysis.

| Platform | Best For | Free Tier | ESP32 Library | Difficulty |
|---|---|---|---|---|
| **Firebase Realtime Database** | Real-time sync, mobile apps | 1 GB storage, 10 GB/month transfer | `Firebase-ESP-Client` | ⭐⭐ Medium |
| **ThingSpeak** (MathWorks) | IoT dashboards, MATLAB analysis | 3 channels, 8 fields each | HTTP GET (no library needed) | ⭐ Easy |
| **Adafruit IO** | Quick IoT prototyping | 10 feeds, 30 data pts/min | `Adafruit_MQTT_Library` | ⭐ Easy |
| **InfluxDB Cloud + Grafana** | Time-series analytics, pro dashboards | 30-day retention, 5 GB writes | `InfluxDB-Client-for-Arduino` | ⭐⭐⭐ Advanced |
| **AWS IoT Core** | Production-scale, device fleet | 2M messages/month (12 months) | `aws-iot-device-sdk-embedded-C` | ⭐⭐⭐ Advanced |
| **Blynk IoT** | Mobile app dashboard | 2 devices, basic widgets | `Blynk` library | ⭐ Easy |

### 🏆 Recommended: **ThingSpeak** (Easiest to Start)

ThingSpeak requires zero backend setup — just register and get an API key:

```cpp
// Add to smart_farm.ino — send data every 15+ seconds
#include <HTTPClient.h>

void sendToThingSpeak() {
  if (WiFi.status() != WL_CONNECTED) return;

  String url = "https://api.thingspeak.com/update?api_key=YOUR_KEY"
               "&field1=" + String(state.soilMoisture) +
               "&field2=" + String(state.temperature, 1) +
               "&field3=" + String(state.humidity, 1) +
               "&field4=" + String(state.tankFull ? 1 : 0) +
               "&field5=" + String(state.pumpState ? 1 : 0);

  HTTPClient http;
  http.begin(url);
  int code = http.GET();
  Serial.printf("[ThingSpeak] Response: %d\\n", code);
  http.end();
}
```

### 🥇 Recommended: **Firebase** (Best for Dashboards + Apps)

Firebase keeps data in sync across devices in real-time — perfect for a web or mobile app:

```cpp
// Using Firebase-ESP-Client library
#include <Firebase_ESP_Client.h>

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig fbConfig;

void setupFirebase() {
  fbConfig.host = "YOUR-PROJECT.firebaseio.com";
  fbConfig.signer.tokens.legacy_token = "YOUR-SECRET-TOKEN";
  Firebase.begin(&fbConfig, &auth);
}

void sendToFirebase() {
  Firebase.RTDB.setFloat(&fbdo, "/farm/temperature",   state.temperature);
  Firebase.RTDB.setFloat(&fbdo, "/farm/humidity",      state.humidity);
  Firebase.RTDB.setInt  (&fbdo, "/farm/soilMoisture",  state.soilMoisture);
  Firebase.RTDB.setBool (&fbdo, "/farm/tankFull",      state.tankFull);
  Firebase.RTDB.setBool (&fbdo, "/farm/pumpState",     state.pumpState);
  Firebase.RTDB.setString(&fbdo, "/farm/timestamp",    state.timestamp);
}
```

---

## 14. Updated Component List

| Component | Model | Purpose |
|---|---|---|
| Microcontroller | **ESP32 Dev Module** | Central processing, Wi-Fi, WebSocket server |
| Temperature & Humidity | **DHT22 (AM2302)** | Ambient temperature & humidity |
| Soil Moisture | **Capacitive Sensor v1.2** | Volumetric water content |
| Real-Time Clock | **DS3231 RTC Module** | Accurate timestamping (I2C) |
| **Tank Level** | **Horizontal Float Switch** | Detects if water reservoir has water |
| **Data Logger** | **SPI SD Card Module** | Optional CSV logging to microSD |
| Water Pump | **12V DC Submersible Pump** | Delivers irrigation water |
| Relay Module | **5V 1-Channel Relay** | ESP32-controlled pump switch |
| Power Supply | **12V 2A DC Adapter** | Powers pump + buck converters |

---

## 15. Technologies Used

- **ESP32 Arduino Framework** — Firmware development
- **DHT Sensor Library** — DHT22 temperature/humidity reading
- **RTClib** — DS3231 real-time clock
- **SD (built-in)** — MicroSD card CSV logging
- **arduinoWebSockets** — Bidirectional real-time communication
- **ArduinoJson** — JSON serialization/deserialization
- **HTML5 / CSS3 / JavaScript** — Web dashboard frontend
- **Chart.js v4** — Real-time live charts
- **Google Fonts (Outfit + JetBrains Mono)** — Typography
- **WebSocket API** — Browser real-time communication

---

*Smart Agriculture Project — IoT-Enabled Water Monitoring & Automated Irrigation System for Green Farming*
