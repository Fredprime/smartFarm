// ============================================================
//  config.h — Smart Farm IoT System Configuration
//  Project:  IoT-Enabled Water Monitoring & Automated Irrigation
//  Author:   Smart Agriculture Project
//  MCU:      ESP32
//
//  Components:
//    - DHT22              : Temperature & Humidity
//    - Soil Moisture      : Capacitive Analog Sensor
//    - DS3231 RTC         : I2C Real-Time Clock
//    - Float Sensor       : Horizontal tank-level switch
//    - SD Card Module     : SPI data logger (optional — graceful fallback)
//    - 12V Pump + Relay   : Automated irrigation
// ============================================================

#ifndef CONFIG_H
#define CONFIG_H

// ----------------------------------------------------------
// WiFi Credentials
// ----------------------------------------------------------
// Copy secrets.example.h to secrets.h and enter the local network
// credentials there. secrets.h is excluded from version control.
#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"
#endif

// ----------------------------------------------------------
// WebSocket Server
// ----------------------------------------------------------
#define WS_PORT         81          // WebSocket port
#define HTTP_PORT       80          // HTTP dashboard port

// ----------------------------------------------------------
// GPIO Pin Definitions
// ----------------------------------------------------------

// DHT22 Temperature & Humidity Sensor
#define DHT_PIN         4           // GPIO4
#define DHT_TYPE        DHT22

// Soil Moisture Sensor (Analog)
#define SOIL_MOISTURE_PIN   34      // GPIO34 (ADC1_CH6 — input only)

// Relay for 12V Water Pump (active LOW)
#define PUMP_RELAY_PIN  26          // GPIO26
#define PUMP_ACTIVE_LOW true        // Most relay modules are active-LOW

// Status LED
#define LED_PIN         2           // Built-in LED on most ESP32 boards

// DS3231 RTC (I2C)
#define RTC_SDA_PIN     21          // GPIO21 — I2C SDA
#define RTC_SCL_PIN     22          // GPIO22 — I2C SCL

// ----------------------------------------------------------
// Horizontal Float Sensor (Tank Water Level)
//   Wiring: one leg to GPIO, other leg to GND.
//   Internal pull-up enabled. Float UP (tank has water) = LOW
//   Float DOWN (tank empty) = HIGH  (switch open)
//   Set FLOAT_TANK_FULL_LEVEL to LOW or HIGH to match your sensor.
// ----------------------------------------------------------
#define FLOAT_SENSOR_PIN       27          // GPIO27
#define FLOAT_TANK_FULL_LEVEL  LOW         // Signal level when tank has water

// ----------------------------------------------------------
// SD Card Module (SPI) — OPTIONAL
//   The system continues operating if SD card is absent.
//   Standard ESP32 SPI2 (HSPI) default pins:
// ----------------------------------------------------------
#define SD_CS_PIN    5           // GPIO5  — Chip Select
#define SD_MOSI_PIN  23          // GPIO23 — MOSI
#define SD_MISO_PIN  19          // GPIO19 — MISO
#define SD_SCK_PIN   18          // GPIO18 — Clock
#define SD_LOG_FILENAME  "/farmlog.csv"    // Log file on SD card
#define SD_LOG_INTERVAL_MS  30000          // Write to SD every 30 seconds

// ----------------------------------------------------------
// Soil Moisture Sensor Calibration
//   Adjust these values based on your specific sensor:
//   - DRY_VALUE  = ADC reading in open air (sensor dry)
//   - WET_VALUE  = ADC reading fully submerged in water
// ----------------------------------------------------------
#define SOIL_DRY_VALUE  3900        // Raw ADC when completely dry
#define SOIL_WET_VALUE   800        // Raw ADC when in water

// ----------------------------------------------------------
// Irrigation Thresholds
// ----------------------------------------------------------
#define MOISTURE_LOW_THRESHOLD   30   // % — turn pump ON  below this
#define MOISTURE_HIGH_THRESHOLD  70   // % — turn pump OFF above this

// Temperature alert threshold (°C)
#define TEMP_HEAT_STRESS   35.0f      // Above this = heat stress alert
#define TEMP_FROST_WARN    4.0f       // Below this = frost warning

// Humidity alert threshold (%)
#define HUMIDITY_LOW_WARN  30.0f      // Below this = low humidity alert
#define HUMIDITY_HIGH_WARN 90.0f      // Above this = disease risk alert

// ----------------------------------------------------------
// System Timing
// ----------------------------------------------------------
#define SENSOR_READ_INTERVAL_MS   5000   // Read sensors every 5 seconds
#define PUMP_MAX_ON_TIME_MS    300000    // Safety: auto-stop pump after 5 min
#define PUMP_COOLDOWN_MS        60000    // Minimum off time between pump cycles

// ----------------------------------------------------------
// Auto / Manual Mode
// ----------------------------------------------------------
#define DEFAULT_AUTO_MODE false         // Start in MANUAL mode (safer for commissioning)

// ----------------------------------------------------------
// MQTT Broker Configuration (Remote Cross-Network Control)
// ----------------------------------------------------------
#define MQTT_BROKER             "broker.hivemq.com"
#define MQTT_PORT               1883
#define MQTT_CLIENT_ID          "SmartFarm_ESP32"
#define MQTT_TOPIC_TELEMETRY    "smartfarm/telemetry"
#define MQTT_TOPIC_CONTROL      "smartfarm/control"
#define MQTT_RECONNECT_MS       5000

// ----------------------------------------------------------
// Supabase Direct REST API (Cloud Database Logging)
// ----------------------------------------------------------
#define SUPABASE_ENABLED        true
#define SUPABASE_URL            "https://exnhqpzlkucjiubvsabx.supabase.co"
#define SUPABASE_ANON_KEY       "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImV4bmhxcHpsa3Vjaml1YnZzYWJ4Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODU0OTgxOTYsImV4cCI6MjEwMTA3NDE5Nn0.LQFvw98M2cN4Ojf7LoUp2kJR7bMoCGICcdSmM9xlW9g"
#define SUPABASE_POST_INTERVAL_MS 60000 // Send data to Supabase every 60 seconds

#endif // CONFIG_H
