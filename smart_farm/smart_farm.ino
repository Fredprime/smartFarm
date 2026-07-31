// ============================================================
//  smart_farm.ino — Main Firmware
//  Project:  IoT-Enabled Water Monitoring & Automated Irrigation
//  Microcontroller: ESP32
//
//  Components:
//    - DHT22              : Temperature & Humidity sensor
//    - Soil Moisture      : Capacitive analog sensor
//    - DS3231 RTC         : Real-Time Clock (I2C)
//    - Float Sensor       : Horizontal tank water-level switch
//    - SD Card Module     : SPI CSV data logger (OPTIONAL — graceful fallback)
//    - 12V Pump           : Via relay module on PUMP_RELAY_PIN
//    - ESP32              : WiFi + WebSocket server + HTTP server
//
//  Dependencies (install via Arduino Library Manager):
//    - DHT sensor library by Adafruit
//    - Adafruit Unified Sensor
//    - RTClib by Adafruit
//    - WebSockets by Markus Sattler (arduinoWebSockets)
//    - ArduinoJson by Benoit Blanchon
//    - SD (built-in ESP32 Arduino core — no extra install needed)
//
//  Board: "ESP32 Dev Module" in Arduino IDE
// ============================================================

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <DHT.h>
#include <Wire.h>
#include <RTClib.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <SD.h>
#include "config.h"

// ----------------------------------------------------------
// Object Instantiation
// ----------------------------------------------------------
DHT dht(DHT_PIN, DHT_TYPE);
RTC_DS3231    rtc;
WebServer     httpServer(HTTP_PORT);
WebSocketsServer webSocket(WS_PORT);

// ----------------------------------------------------------
// Global State
// ----------------------------------------------------------
struct SensorData {
  float   temperature;        // °C
  float   humidity;           // %
  int     soilMoisture;       // % (0 = dry, 100 = wet)
  int     soilRaw;            // Raw ADC value
  bool    tankFull;           // true = water in tank (float sensor HIGH)
  bool    pumpState;          // true = ON
  bool    autoMode;           // true = automatic irrigation
  String  timestamp;          // HH:MM:SS from RTC
  String  datestamp;          // YYYY-MM-DD from RTC
  String  alerts[10];         // Active alert messages (increased for new alerts)
  int     alertCount;
  bool    sdAvailable;        // true = SD card mounted and ready
  bool    sdLogging;          // true = SD is actively writing
};

SensorData state;

// ----------------------------------------------------------
// Runtime-Adjustable Thresholds
//   Initialized from config.h defaults.
//   Updated live via WebSocket 'set_thresholds' command.
//   No reboot required — changes take effect on next cycle.
// ----------------------------------------------------------
int           rt_moistureLow    = MOISTURE_LOW_THRESHOLD;
int           rt_moistureHigh   = MOISTURE_HIGH_THRESHOLD;
float         rt_tempHeat       = TEMP_HEAT_STRESS;
float         rt_tempFrost      = TEMP_FROST_WARN;
float         rt_humidLow       = HUMIDITY_LOW_WARN;
float         rt_humidHigh      = HUMIDITY_HIGH_WARN;
unsigned long rt_pumpMaxMs      = PUMP_MAX_ON_TIME_MS;
unsigned long rt_pumpCoolMs     = PUMP_COOLDOWN_MS;
unsigned long rt_sensorInterval = SENSOR_READ_INTERVAL_MS;
unsigned long rt_sdLogInterval  = SD_LOG_INTERVAL_MS;
bool          rt_tankGuardEnabled = true; // Bypasses float switch if disabled

// Pump safety timers
unsigned long pumpOnTime     = 0;       // Millis when pump turned ON
unsigned long pumpOffTime    = 0;       // Millis when pump turned OFF
bool          pumpCooldown   = false;
bool          pumpForcedOn   = false;   // True when started via manual force-override

// Sensor read timer
unsigned long lastSensorRead = 0;

// WebSocket broadcast timer
unsigned long lastBroadcast  = 0;

// SD Card log timer
unsigned long lastSDLog      = 0;

// Connected WebSocket clients count
int connectedClients = 0;

// ----------------------------------------------------------
// Alert System
// ----------------------------------------------------------
void clearAlerts() {
  state.alertCount = 0;
  for (int i = 0; i < 8; i++) state.alerts[i] = "";
}

void addAlert(const String& msg) {
  if (state.alertCount < 8) {
    state.alerts[state.alertCount++] = msg;
  }
}

void evaluateAlerts() {
  clearAlerts();
  
  if (state.soilMoisture < rt_moistureLow)
    addAlert("DROUGHT RISK: Soil moisture critically low (" + String(state.soilMoisture) + "%)");

  if (state.temperature > rt_tempHeat)
    addAlert("HEAT STRESS: Temperature " + String(state.temperature, 1) + "°C exceeds safe limit");

  if (state.temperature < rt_tempFrost)
    addAlert("FROST WARNING: Temperature " + String(state.temperature, 1) + "°C near freezing");

  if (state.humidity < rt_humidLow)
    addAlert("LOW HUMIDITY: " + String(state.humidity, 1) + "% — increased evaporation rate");

  if (state.humidity > rt_humidHigh)
    addAlert("DISEASE RISK: Humidity " + String(state.humidity, 1) + "% — fungal risk elevated");

  if (state.pumpState && (millis() - pumpOnTime > rt_pumpMaxMs - 30000))
    addAlert("PUMP WARNING: Pump approaching maximum run time limit");

  // Float sensor alerts
  if (!state.tankFull)
    addAlert("TANK EMPTY: Water reservoir low — refill required");
}

// ----------------------------------------------------------
// Pump Control
// ----------------------------------------------------------
void setPump(bool on, bool forceOverride = false) {
  if (on == state.pumpState) return; // No change

  if (on) {
    // --- Safety: Block pump if tank is empty (skipped on manual force override) ---
    if (rt_tankGuardEnabled && !state.tankFull && !forceOverride) {
      Serial.println("[PUMP] BLOCKED — tank empty (float sensor)");
      return;
    }
    // Check cooldown
    if (pumpCooldown && (millis() - pumpOffTime < rt_pumpCoolMs)) {
      Serial.printf("[PUMP] Cooldown active — %lu s remaining\n",
        (rt_pumpCoolMs - (millis() - pumpOffTime)) / 1000);
      return;
    }
    pumpOnTime   = millis();
    pumpCooldown = false;
    pumpForcedOn = forceOverride; // Set AFTER all early-return checks
    Serial.printf("[PUMP] ON%s\n", forceOverride ? " (forced — tank guard bypassed)" : "");
  } else {
    pumpOffTime  = millis();
    pumpCooldown = true;
    pumpForcedOn = false; // Clear forced flag when pump stops
    Serial.println("[PUMP] OFF");
  }

  state.pumpState = on;
  // Relay logic: active-LOW relay means LOW = ON
  if (PUMP_ACTIVE_LOW) {
    digitalWrite(PUMP_RELAY_PIN, on ? LOW : HIGH);
  } else {
    digitalWrite(PUMP_RELAY_PIN, on ? HIGH : LOW);
  }
}

void handlePumpSafety() {
  // Hard safety: auto shutoff after max run time (applies in ALL modes)
  if (state.pumpState && (millis() - pumpOnTime > rt_pumpMaxMs)) {
    Serial.println("[PUMP] Safety shutoff — max run time exceeded");
    setPump(false);
    return;
  }
  // Emergency shutoff if tank runs dry while pump is running.
  // Only active in AUTO mode — in MANUAL mode the user is in full control.
  // Alerts are always shown regardless; this only controls the pump relay.
  if (rt_tankGuardEnabled && state.autoMode && state.pumpState && !state.tankFull) {
    Serial.println("[PUMP] Emergency shutoff — tank empty (auto mode guard)");
    setPump(false);
  }
}

void autoIrrigationLogic() {
  if (!state.autoMode) return;

  if (!state.pumpState && state.soilMoisture < rt_moistureLow) {
    Serial.println("[AUTO] Moisture low — starting irrigation");
    setPump(true);
  } else if (state.pumpState && state.soilMoisture >= rt_moistureHigh) {
    Serial.println("[AUTO] Moisture sufficient — stopping irrigation");
    setPump(false);
  }
}

// ----------------------------------------------------------
// Sensor Reading
// ----------------------------------------------------------
int readSoilMoisture() {
  // Average multiple readings for stability
  long sum = 0;
  const int samples = 10;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(SOIL_MOISTURE_PIN);
    delay(5);
  }
  int raw = sum / samples;
  state.soilRaw = raw;

  // Map to 0–100% (constrain for out-of-range values)
  int pct = map(raw, SOIL_DRY_VALUE, SOIL_WET_VALUE, 0, 100);
  return constrain(pct, 0, 100);
}

void readSensors() {
  // --- DHT22 ---
  float temp = dht.readTemperature();
  float hum  = dht.readHumidity();

  if (!isnan(temp)) state.temperature = temp;
  else              Serial.println("[DHT22] Read error — temperature");

  if (!isnan(hum))  state.humidity = hum;
  else              Serial.println("[DHT22] Read error — humidity");

  // --- Soil Moisture ---
  state.soilMoisture = readSoilMoisture();

  // --- Float Sensor (Tank Level) ---
  //   Using INPUT_PULLUP: float closed (water present) = LOW = tank FULL
  int floatReading = digitalRead(FLOAT_SENSOR_PIN);
  state.tankFull   = (floatReading == FLOAT_TANK_FULL_LEVEL);

  // --- RTC ---
  DateTime now = rtc.now();
  char timeBuf[12];
  char dateBuf[12];
  sprintf(timeBuf, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  sprintf(dateBuf, "%04d-%02d-%02d", now.year(), now.month(), now.day());
  state.timestamp = String(timeBuf);
  state.datestamp = String(dateBuf);

  Serial.printf("[SENSORS] Temp=%.1f°C  Hum=%.1f%%  Soil=%d%% (raw=%d)  Tank=%s  SD=%s  Time=%s\n",
    state.temperature, state.humidity,
    state.soilMoisture, state.soilRaw,
    state.tankFull ? "FULL" : "EMPTY",
    state.sdAvailable ? (state.sdLogging ? "logging" : "ready") : "offline",
    state.timestamp.c_str());
}

// ----------------------------------------------------------
// JSON Broadcast via WebSocket
// ----------------------------------------------------------
String buildJSON() {
  StaticJsonDocument<600> doc;
  doc["temperature"]   = round(state.temperature * 10.0) / 10.0;
  doc["humidity"]      = round(state.humidity * 10.0) / 10.0;
  doc["soilMoisture"]  = state.soilMoisture;
  doc["soilRaw"]       = state.soilRaw;
  doc["tankFull"]      = state.tankFull;       // Float sensor
  doc["pumpState"]     = state.pumpState;
  doc["autoMode"]      = state.autoMode;
  doc["timestamp"]     = state.timestamp;
  doc["datestamp"]     = state.datestamp;
  doc["alertCount"]    = state.alertCount;
  doc["sdAvailable"]   = state.sdAvailable;    // SD card status
  doc["sdLogging"]     = state.sdLogging;

  JsonArray alerts = doc.createNestedArray("alerts");
  for (int i = 0; i < state.alertCount; i++) {
    alerts.add(state.alerts[i]);
  }

  String json;
  serializeJson(doc, json);
  return json;
}

void broadcastState() {
  if (connectedClients == 0) return;
  String json = buildJSON();
  webSocket.broadcastTXT(json);
}

// ----------------------------------------------------------
// WebSocket Event Handler
// ----------------------------------------------------------
void onWebSocketEvent(uint8_t clientId, WStype_t type,
                      uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      connectedClients++;
      Serial.printf("[WS] Client #%d connected. Total: %d\n", clientId, connectedClients);
      // Send current state immediately to new client
      { String json = buildJSON(); webSocket.sendTXT(clientId, json); }
      break;

    case WStype_DISCONNECTED:
      if (connectedClients > 0) connectedClients--;
      Serial.printf("[WS] Client #%d disconnected. Total: %d\n", clientId, connectedClients);
      break;

    case WStype_TEXT: {
      // Parse incoming command JSON
      StaticJsonDocument<384> cmd;
      DeserializationError err = deserializeJson(cmd, payload, length);
      if (err) { Serial.println("[WS] JSON parse error"); break; }

      // Command: { "action": "pump_on" }
      // Command: { "action": "pump_off" }
      // Command: { "action": "set_auto", "value": true/false }
      String action = cmd["action"].as<String>();

      if (action == "pump_on") {
        state.autoMode = false;   // Switch to manual
        {
          bool force = cmd.containsKey("force") && cmd["force"].as<bool>();
          setPump(true, force);
          Serial.printf("[CMD] Manual pump ON%s\n", force ? " (FORCE — tank guard bypassed)" : "");
        }
      } else if (action == "pump_off") {
        setPump(false);
        Serial.println("[CMD] Manual pump OFF");
      } else if (action == "set_auto") {
        bool newAutoMode = cmd["value"].as<bool>();
        // Switching TO manual: stop pump if auto had it running
        if (!newAutoMode && state.autoMode && state.pumpState) {
          Serial.println("[CMD] Switched to MANUAL — stopping auto-run pump");
          setPump(false);
        }
        state.autoMode = newAutoMode;
        Serial.printf("[CMD] Mode: %s\n", state.autoMode ? "AUTO" : "MANUAL");

      } else if (action == "set_thresholds") {
        // ------ Update runtime KPI thresholds from dashboard settings ------
        if (cmd.containsKey("moisture_low"))      rt_moistureLow    = cmd["moisture_low"].as<int>();
        if (cmd.containsKey("moisture_high"))     rt_moistureHigh   = cmd["moisture_high"].as<int>();
        if (cmd.containsKey("temp_heat"))         rt_tempHeat       = cmd["temp_heat"].as<float>();
        if (cmd.containsKey("temp_frost"))        rt_tempFrost      = cmd["temp_frost"].as<float>();
        if (cmd.containsKey("humid_low"))         rt_humidLow       = cmd["humid_low"].as<float>();
        if (cmd.containsKey("humid_high"))        rt_humidHigh      = cmd["humid_high"].as<float>();
        if (cmd.containsKey("pump_max_min"))      rt_pumpMaxMs      = (unsigned long)cmd["pump_max_min"].as<int>() * 60000UL;
        if (cmd.containsKey("pump_cool_min"))     rt_pumpCoolMs     = (unsigned long)cmd["pump_cool_min"].as<int>() * 60000UL;
        if (cmd.containsKey("sensor_interval_s")) rt_sensorInterval = (unsigned long)cmd["sensor_interval_s"].as<int>() * 1000UL;
        if (cmd.containsKey("sd_interval_s"))     rt_sdLogInterval  = (unsigned long)cmd["sd_interval_s"].as<int>() * 1000UL;
        if (cmd.containsKey("tank_guard"))        rt_tankGuardEnabled = cmd["tank_guard"].as<bool>();

        Serial.printf("[THRESHOLDS] Updated: soil=%d%%/%d%% temp=%.1f/%.1f hum=%.1f/%.1f pumpMax=%lums cool=%lums guard=%s\n",
          rt_moistureLow, rt_moistureHigh,
          rt_tempHeat, rt_tempFrost,
          rt_humidLow, rt_humidHigh,
          rt_pumpMaxMs, rt_pumpCoolMs,
          rt_tankGuardEnabled ? "ON" : "OFF");

      } else {
        Serial.println("[WS] Unknown command: " + action);
      }
      // Broadcast updated state
      broadcastState();
      break;
    }

    default:
      break;
  }
}

// ----------------------------------------------------------
// HTTP Server: Serve dashboard and API
// ----------------------------------------------------------
void setupHTTPRoutes() {
  // Serve the main dashboard HTML (stored in SPIFFS or inline)
  httpServer.on("/", HTTP_GET, []() {
    httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    // If not using SPIFFS, respond with a redirect hint
    httpServer.send(200, "text/html",
      "<html><head><meta http-equiv='refresh' content='0; url=http://" +
      WiFi.localIP().toString() +
      "/dashboard'></head></html>");
  });

  // JSON API endpoint for initial data fetch
  httpServer.on("/api/status", HTTP_GET, []() {
    httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    httpServer.sendHeader("Content-Type", "application/json");
    httpServer.send(200, "application/json", buildJSON());
  });

  // Health check
  httpServer.on("/api/health", HTTP_GET, []() {
    httpServer.send(200, "text/plain", "OK");
  });

  httpServer.begin();
  Serial.println("[HTTP] Server started on port " + String(HTTP_PORT));
}

// ----------------------------------------------------------
// SD Card — Optional Graceful Init & CSV Logger
//   If SD card is absent or fails to mount, sdAvailable = false
//   and ALL other system functions continue normally.
// ----------------------------------------------------------
void initSDCard() {
  Serial.println("[SD] Initialising SD card module...");

  // Use custom SPI pins defined in config.h
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

  if (!SD.begin(SD_CS_PIN)) {
    // SD card not present or wiring issue — continue without it
    state.sdAvailable = false;
    state.sdLogging   = false;
    Serial.println("[SD] Not found or mount failed — SD logging disabled");
    Serial.println("[SD] System continues normally without SD card.");
    return;
  }

  state.sdAvailable = true;
  state.sdLogging   = false;

  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
  Serial.printf("[SD] Mounted successfully. Card size: %lluMB\n", cardSize);

  // Write CSV header if file doesn't exist yet
  if (!SD.exists(SD_LOG_FILENAME)) {
    File f = SD.open(SD_LOG_FILENAME, FILE_WRITE);
    if (f) {
      f.println("Date,Time,Soil_Moisture_%,Temperature_C,Humidity_%,Tank_Level,Pump_State,Mode");
      f.close();
      Serial.printf("[SD] Created log file: %s\n", SD_LOG_FILENAME);
    } else {
      Serial.println("[SD] Failed to create log file");
    }
  } else {
    Serial.printf("[SD] Appending to existing log: %s\n", SD_LOG_FILENAME);
  }
}

void logToSD() {
  if (!state.sdAvailable) return;  // SD absent — skip silently

  File f = SD.open(SD_LOG_FILENAME, FILE_APPEND);
  if (!f) {
    Serial.println("[SD] Failed to open log file for writing");
    state.sdLogging = false;
    return;
  }

  // CSV row: Date,Time,Soil%,Temp,Hum,Tank,Pump,Mode
  f.printf("%s,%s,%d,%.1f,%.1f,%s,%s,%s\n",
    state.datestamp.c_str(),
    state.timestamp.c_str(),
    state.soilMoisture,
    state.temperature,
    state.humidity,
    state.tankFull ? "FULL" : "EMPTY",
    state.pumpState ? "ON"   : "OFF",
    state.autoMode  ? "AUTO" : "MANUAL"
  );

  f.close();
  state.sdLogging = true;
  Serial.printf("[SD] Logged row at %s %s\n",
    state.datestamp.c_str(), state.timestamp.c_str());
}

// ----------------------------------------------------------
// WiFi Connection
// ----------------------------------------------------------
void connectWiFi() {
  Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 30) {
    delay(500);
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected!");
    Serial.print("[WiFi] IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.printf("[WiFi] Dashboard: http://%s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[WiFi] WebSocket: ws://%s:%d\n", WiFi.localIP().toString().c_str(), WS_PORT);
  } else {
    Serial.println("\n[WiFi] Connection FAILED — operating offline");
    // Could start AP mode here for local configuration
    WiFi.mode(WIFI_AP);
    WiFi.softAP("SmartFarm_Setup", "farm12345");
    Serial.printf("[WiFi] AP mode: connect to 'SmartFarm_Setup' -> %s\n",
                  WiFi.softAPIP().toString().c_str());
  }
}

// ----------------------------------------------------------
// Setup
// ----------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("\n========================================");
  Serial.println("  Smart Farm IoT System — Booting...");
  Serial.println("========================================");

  // GPIO setup
  pinMode(PUMP_RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  // Ensure pump is OFF at startup (active-LOW relay = HIGH means OFF)
  digitalWrite(PUMP_RELAY_PIN, PUMP_ACTIVE_LOW ? HIGH : LOW);

  // Float sensor: internal pull-up — switch pulls to GND when float rises (tank full)
  pinMode(FLOAT_SENSOR_PIN, INPUT_PULLUP);
  Serial.println("[FLOAT] Tank level sensor initialized on GPIO" + String(FLOAT_SENSOR_PIN));

  // Initialize I2C for RTC
  Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);

  // Initialize RTC
  if (!rtc.begin()) {
    Serial.println("[RTC] DS3231 not found — using millis() fallback");
  } else {
    Serial.println("[RTC] DS3231 initialized");
    if (rtc.lostPower()) {
      Serial.println("[RTC] Lost power — setting compile time");
      // Set RTC to the time this sketch was compiled
      // In production, sync via NTP after WiFi connect
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
  }

  // Initialize DHT22
  dht.begin();
  Serial.println("[DHT22] Sensor initialized");

  // Configure ADC for soil moisture (12-bit, 0-4095)
  analogSetAttenuation(ADC_11db); // Full 0–3.3V range
  analogReadResolution(12);
  Serial.println("[SOIL] ADC configured");

  // Initialize state
  state.pumpState    = false;
  state.autoMode     = DEFAULT_AUTO_MODE;
  state.alertCount   = 0;
  state.tankFull     = true;    // Assume full until first read
  state.sdAvailable  = false;
  state.sdLogging    = false;

  // Initialize SD Card (optional — graceful fallback if absent)
  initSDCard();

  // Connect to WiFi
  connectWiFi();

  // Sync RTC with NTP if WiFi connected
  if (WiFi.status() == WL_CONNECTED) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    struct tm timeInfo;
    if (getLocalTime(&timeInfo, 5000)) {
      rtc.adjust(DateTime(
        timeInfo.tm_year + 1900,
        timeInfo.tm_mon + 1,
        timeInfo.tm_mday,
        timeInfo.tm_hour,
        timeInfo.tm_min,
        timeInfo.tm_sec
      ));
      Serial.println("[NTP] RTC synced with internet time");
    }
  }

  // Start WebSocket server
  webSocket.begin();
  webSocket.onEvent(onWebSocketEvent);
  Serial.println("[WS] WebSocket server started on port " + String(WS_PORT));

  // Start HTTP server
  setupHTTPRoutes();

  // First sensor read
  readSensors();

  // Blink LED to signal ready
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, HIGH); delay(200);
    digitalWrite(LED_PIN, LOW);  delay(200);
  }

  Serial.println("\n[SYSTEM] Ready! Monitoring farm conditions...\n");
}

// ----------------------------------------------------------
// Main Loop
// ----------------------------------------------------------
void loop() {
  // Handle WebSocket & HTTP events
  webSocket.loop();
  httpServer.handleClient();

  unsigned long now = millis();

  // --- Sensor Read Cycle ---
  if (now - lastSensorRead >= rt_sensorInterval) {
    lastSensorRead = now;

    readSensors();
    evaluateAlerts();
    handlePumpSafety();
    autoIrrigationLogic();

    // Broadcast to all WebSocket clients
    broadcastState();

    // Visual indication: blink LED when pump is running
    digitalWrite(LED_PIN, state.pumpState ? HIGH : LOW);
  }

  // --- SD Card Periodic Log (independent timer, uses runtime interval) ---
  if (now - lastSDLog >= rt_sdLogInterval) {
    lastSDLog = now;
    logToSD();  // No-op if SD not available
  }

  // --- WiFi Reconnect ---
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastReconnect = 0;
    if (now - lastReconnect > 30000) {
      lastReconnect = now;
      Serial.println("[WiFi] Reconnecting...");
      WiFi.reconnect();
    }
  }

  // Small yield to prevent watchdog reset
  delay(10);
}
