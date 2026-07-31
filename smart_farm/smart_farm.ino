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
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include "config.h"

// ----------------------------------------------------------
// Object Instantiation
// ----------------------------------------------------------
DHT dht(DHT_PIN, DHT_TYPE);
RTC_DS3231    rtc;
WebServer     httpServer(HTTP_PORT);
WebSocketsServer webSocket(WS_PORT);

// MQTT Client (Remote Cross-Network Control)
WiFiClient   mqttWifiClient;
PubSubClient mqttClient(mqttWifiClient);

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
  String json = buildJSON();
  if (connectedClients > 0) {
    webSocket.broadcastTXT(json);
  }
  if (mqttClient.connected()) {
    mqttClient.publish(MQTT_TOPIC_TELEMETRY, json.c_str(), false);
  }
}

// ----------------------------------------------------------
// Supabase Direct REST API (Cloud Database Logging)
// ----------------------------------------------------------
void postToSupabase() {
#if defined(SUPABASE_ENABLED) && SUPABASE_ENABLED
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure(); // Skip certificate validation for convenience

  HTTPClient http;
  String url = String(SUPABASE_URL) + "/rest/v1/sensor_readings";
  if (http.begin(client, url)) {
    http.addHeader("Content-Type", "application/json");
    http.addHeader("apikey", SUPABASE_ANON_KEY);
    http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
    http.addHeader("Prefer", "return=minimal");

    StaticJsonDocument<512> doc;
    doc["temperature"]   = round(state.temperature * 10.0) / 10.0;
    doc["humidity"]      = round(state.humidity * 10.0) / 10.0;
    doc["soil_moisture"] = state.soilMoisture;
    doc["soil_raw"]      = state.soilRaw;
    doc["tank_full"]     = state.tankFull;
    doc["pump_state"]    = state.pumpState;
    doc["auto_mode"]     = state.autoMode;

    String json;
    serializeJson(doc, json);

    int httpCode = http.POST(json);
    if (httpCode == 201 || httpCode == 200 || httpCode == 204) {
      Serial.println("[SUPABASE] Telemetry logged to cloud database!");
    } else {
      Serial.printf("[SUPABASE] POST failed, HTTP status: %d\n", httpCode);
    }
    http.end();
  }
#endif
}

// ----------------------------------------------------------
// Command Processor (Shared by WebSockets and MQTT)
// ----------------------------------------------------------
void processCommandJSON(StaticJsonDocument<384>& cmd) {
  String action = cmd["action"] | "";

  if (action == "pump_on") {
    state.autoMode = false;   // Switch to manual
    bool force = cmd.containsKey("force") && cmd["force"].as<bool>();
    setPump(true, force);
    Serial.printf("[CMD] Manual pump ON%s\n", force ? " (FORCE — tank guard bypassed)" : "");
  } else if (action == "pump_off") {
    setPump(false);
    Serial.println("[CMD] Manual pump OFF");
  } else if (action == "set_auto") {
    bool newAutoMode = cmd["value"].as<bool>();
    if (!newAutoMode && state.autoMode && state.pumpState) {
      Serial.println("[CMD] Switched to MANUAL — stopping auto-run pump");
      setPump(false);
    }
    state.autoMode = newAutoMode;
    Serial.printf("[CMD] Mode: %s\n", state.autoMode ? "AUTO" : "MANUAL");
  } else if (action == "set_thresholds") {
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
    Serial.println("[CMD] Unknown action: " + action);
  }

  broadcastState();
}

// ----------------------------------------------------------
// MQTT Connection & Callback (Remote Cross-Network Control)
// ----------------------------------------------------------
void connectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;
  Serial.printf("[MQTT] Connecting to %s:%d as %s...\n", MQTT_BROKER, MQTT_PORT, MQTT_CLIENT_ID);
  if (mqttClient.connect(MQTT_CLIENT_ID)) {
    Serial.println("[MQTT] Connected successfully!");
    mqttClient.subscribe(MQTT_TOPIC_CONTROL);
    Serial.println("[MQTT] Subscribed to topic: " + String(MQTT_TOPIC_CONTROL));
  } else {
    Serial.printf("[MQTT] Connection failed, state=%d\n", mqttClient.state());
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.printf("[MQTT] Message on %s (%u bytes)\n", topic, length);
  StaticJsonDocument<384> cmd;
  DeserializationError err = deserializeJson(cmd, payload, length);
  if (!err) {
    processCommandJSON(cmd);
  } else {
    Serial.println("[MQTT] JSON parse error");
  }
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
      { String json = buildJSON(); webSocket.sendTXT(clientId, json); }
      break;

    case WStype_DISCONNECTED:
      if (connectedClients > 0) connectedClients--;
      Serial.printf("[WS] Client #%d disconnected. Total: %d\n", clientId, connectedClients);
      break;

    case WStype_TEXT: {
      StaticJsonDocument<384> cmd;
      DeserializationError err = deserializeJson(cmd, payload, length);
      if (!err) {
        processCommandJSON(cmd);
      } else {
        Serial.println("[WS] JSON parse error");
      }
      break;
    }

    default:
      break;
  }
}

// ----------------------------------------------------------
// HTTP Server: Dashboard, API & WiFi Onboarding Portal
// ----------------------------------------------------------
void setupHTTPRoutes() {
  httpServer.on("/", HTTP_GET, []() {
    httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    httpServer.send(200, "text/html",
      "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>SmartFarm Controller</title><style>body{font-family:sans-serif;background:#020b05;color:#e2e8f0;padding:20px;text-align:center;}"
      "a{color:#22c55e;font-weight:bold;text-decoration:none;}.box{background:#0a1a0f;border:1px solid #16381e;padding:20px;border-radius:12px;max-width:400px;margin:30px auto;}</style></head>body>"
      "<div class='box'><h2>🌱 SmartFarm IoT Controller</h2>"
      "<p>Connected IP: " + WiFi.localIP().toString() + "</p>"
      "<p><a href='/wifi'>⚙️ Configure Wi-Fi Network</a></p>"
      "</div></body></html>");
  });

  // WiFi Onboarding Web Page (for connecting to new Wi-Fi network)
  httpServer.on("/wifi", HTTP_GET, []() {
    httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<title>SmartFarm Wi-Fi Setup</title><style>"
                  "body{font-family:sans-serif;background:#020b05;color:#e2e8f0;padding:20px;margin:0;}"
                  ".card{background:#0a1a0f;border:1px solid #16381e;padding:24px;border-radius:12px;max-width:400px;margin:20px auto;box-shadow:0 10px 25px rgba(0,0,0,0.5);}"
                  "h2{color:#22c55e;margin-top:0;}label{display:block;margin-top:12px;font-size:0.9rem;color:#94a3b8;}"
                  "input{width:100%;padding:10px;margin-top:6px;border-radius:6px;border:1px solid #1e293b;background:#0f172a;color:#fff;box-sizing:border-box;}"
                  "button{width:100%;padding:12px;margin-top:20px;border:none;border-radius:6px;background:#22c55e;color:#000;font-weight:bold;cursor:pointer;}"
                  ".note{font-size:0.8rem;color:#64748b;margin-top:14px;line-height:1.4;}"
                  "</style></head><body><div class='card'>"
                  "<h2>📶 Wi-Fi Onboarding</h2>"
                  "<p style='font-size:0.85rem;color:#cbd5e1;'>Connect SmartFarm ESP32 to your local Wi-Fi router.</p>"
                  "<form action='/api/wifi' method='POST'>"
                  "<label>Wi-Fi Network Name (SSID):</label>"
                  "<input type='text' name='ssid' placeholder='Enter SSID' required>"
                  "<label>Wi-Fi Password:</label>"
                  "<input type='password' name='pass' placeholder='Enter Password'>"
                  "<button type='submit'>Save &amp; Connect</button>"
                  "</form>"
                  "<div class='note'>Upon saving, ESP32 will reboot and connect to the new Wi-Fi network.</div>"
                  "</div></body></html>";
    httpServer.send(200, "text/html", html);
  });

  // Save new Wi-Fi credentials via Form or API
  httpServer.on("/api/wifi", HTTP_POST, []() {
    String newSsid = httpServer.arg("ssid");
    String newPass = httpServer.arg("pass");

    if (newSsid.length() > 0) {
      Preferences pref;
      pref.begin("smartfarm", false);
      pref.putString("ssid", newSsid);
      pref.putString("pass", newPass);
      pref.end();

      httpServer.send(200, "text/html",
        "<html><body style='background:#020b05;color:#22c55e;font-family:sans-serif;padding:30px;text-align:center;'>"
        "<h2>✅ Wi-Fi Credentials Saved!</h2>"
        "<p style='color:#e2e8f0;'>Connecting to <b>" + newSsid + "</b>... ESP32 is restarting.</p>"
        "</body></html>");
      delay(1500);
      ESP.restart();
    } else {
      httpServer.send(400, "text/plain", "Missing SSID");
    }
  });

  // Reset Wi-Fi credentials back to secrets.h default
  httpServer.on("/api/wifi/reset", HTTP_POST, []() {
    Preferences pref;
    pref.begin("smartfarm", false);
    pref.clear();
    pref.end();
    httpServer.send(200, "text/plain", "Wi-Fi credentials reset. Restarting...");
    delay(1000);
    ESP.restart();
  });

  // JSON API status endpoint
  httpServer.on("/api/status", HTTP_GET, []() {
    httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    httpServer.sendHeader("Content-Type", "application/json");
    httpServer.send(200, "application/json", buildJSON());
  });

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
  if (!state.sdAvailable) return;

  File f = SD.open(SD_LOG_FILENAME, FILE_APPEND);
  if (!f) {
    Serial.println("[SD] Failed to open log file for writing");
    state.sdLogging = false;
    return;
  }

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
// WiFi Connection & Onboarding AP Fallback
// ----------------------------------------------------------
void connectWiFi() {
  Preferences pref;
  pref.begin("smartfarm", true);
  String targetSsid = pref.getString("ssid", WIFI_SSID);
  String targetPass = pref.getString("pass", WIFI_PASSWORD);
  pref.end();

  Serial.printf("[WiFi] Connecting to network '%s'...", targetSsid.c_str());
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(targetSsid.c_str(), targetPass.c_str());

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 30) {
    delay(500);
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected successfully!");
    Serial.printf("[WiFi] IP Address: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[WiFi] Setup Portal: http://%s/wifi\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] Could not connect to router — Starting Access Point mode...");
    WiFi.softAP("SmartFarm_Setup", "farm12345");
    Serial.printf("[WiFi AP] Connect to Wi-Fi 'SmartFarm_Setup' (pass: farm12345) -> http://%s/wifi\n",
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

  // Initialize & Connect MQTT
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(768);
  if (WiFi.status() == WL_CONNECTED) {
    connectMQTT();
  }

  // Start HTTP server
  setupHTTPRoutes();

  // First sensor read & Supabase post
  readSensors();
  postToSupabase();

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

  // --- MQTT Reconnect & Event Loop ---
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      static unsigned long lastMqttAttempt = 0;
      if (now - lastMqttAttempt >= MQTT_RECONNECT_MS) {
        lastMqttAttempt = now;
        connectMQTT();
      }
    } else {
      mqttClient.loop();
    }
  }

  // --- Supabase Direct Cloud Log (every 60 seconds) ---
  static unsigned long lastSupabasePost = 0;
  if (now - lastSupabasePost >= SUPABASE_POST_INTERVAL_MS) {
    lastSupabasePost = now;
    postToSupabase();
  }

  // --- Sensor Read Cycle ---
  if (now - lastSensorRead >= rt_sensorInterval) {
    lastSensorRead = now;

    readSensors();
    evaluateAlerts();
    handlePumpSafety();
    autoIrrigationLogic();

    // Broadcast to all WebSocket and MQTT subscribers
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
