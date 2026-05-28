// ============================================================
// Battery Voltage Monitor v2.0 — Device 2
// XIAO ESP32-C6 + INA260 + MQTT + OTA
// ============================================================
// Everything calculated on the ESP32:
//   - Voltage (from INA260)
//   - SOC (voltage lookup, 3 chemistries)
//   - Voltage trend (V/hr, rolling 30-min buffer)
//   - Trend label (Charging/Stable/Discharging)
//   - Battery low alert (SOC < 70%)
//   - Chemistry selector (MQTT Select, changeable from HA)
//
// Zero HA configuration needed — just MQTT integration.
// All sensors auto-discover under one device.
//
// Wiring:
//   XIAO D4 (GPIO22) → INA260 SDA
//   XIAO D5 (GPIO23) → INA260 SCL
//   XIAO 3V3         → INA260 Vcc
//   BFF terminal -   → INA260 GND
//   BFF terminal +   → INA260 Vin+ (jumpered to Vin-)
//   Battery 12V      → BFF DC jack
// ============================================================

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include <Wire.h>
#include <Adafruit_INA260.h>
#include <Preferences.h>
#include "secrets.h"

// ---------- Hardware pins ----------
#define I2C_SDA    SDA          // GPIO22 (D4 on silk)
#define I2C_SCL    SCL          // GPIO23 (D5 on silk)
#define LED_PIN    LED_BUILTIN  // GPIO15

// ---------- INA260 ----------
#define INA260_ADDR  0x40

// ---------- Timing ----------
#define REPORT_INTERVAL_MS   60000   // publish every 60s
#define TREND_BUFFER_SIZE    30      // 30 samples = 30 minutes at 60s interval

// ---------- MQTT topics ----------
#define MQTT_PREFIX  "homeassistant"
#define DEVICE_ID    "battery_monitor_2"

// State topics
static const char* T_STATE      = MQTT_PREFIX "/sensor/" DEVICE_ID "/state";
static const char* T_AVAIL      = MQTT_PREFIX "/sensor/" DEVICE_ID "/availability";

// Discovery topics
static const char* T_DISC_VOLT  = MQTT_PREFIX "/sensor/" DEVICE_ID "_voltage/config";
static const char* T_DISC_SOC   = MQTT_PREFIX "/sensor/" DEVICE_ID "_soc/config";
static const char* T_DISC_TREND = MQTT_PREFIX "/sensor/" DEVICE_ID "_trend/config";
static const char* T_DISC_LABEL = MQTT_PREFIX "/sensor/" DEVICE_ID "_trend_label/config";
static const char* T_DISC_LOW   = MQTT_PREFIX "/binary_sensor/" DEVICE_ID "_low/config";
static const char* T_DISC_CHEM  = MQTT_PREFIX "/select/" DEVICE_ID "_chemistry/config";

// Select command/state topics
static const char* T_CHEM_CMD   = MQTT_PREFIX "/select/" DEVICE_ID "_chemistry/set";
static const char* T_CHEM_STATE = MQTT_PREFIX "/select/" DEVICE_ID "_chemistry/state";

// ---------- Chemistry types ----------
enum Chemistry { CHEM_FLOODED = 0, CHEM_AGM = 1, CHEM_LIFEPO4 = 2 };
static const char* CHEM_NAMES[] = { "Lead-Acid (Flooded)", "Lead-Acid (AGM)", "LiFePO4" };
static const int NUM_CHEMS = 3;

// ---------- SOC lookup tables [voltage_mV, soc_percent] ----------
// Lead-Acid Flooded (12V, 6-cell)
static const int16_t SOC_FLOODED[][2] = {
  {10500,  0}, {11310,  0}, {11510,  5}, {11660, 10},
  {11810, 20}, {11960, 30}, {12060, 40}, {12170, 50},
  {12280, 60}, {12370, 70}, {12480, 80}, {12580, 90},
  {12730,100}, {13200,100}
};
static const int SOC_FLOODED_LEN = sizeof(SOC_FLOODED) / sizeof(SOC_FLOODED[0]);

// Lead-Acid AGM
static const int16_t SOC_AGM[][2] = {
  {10500,  0}, {11510,  0}, {11660,  5}, {11810, 10},
  {11960, 20}, {12060, 30}, {12110, 40}, {12200, 50},
  {12320, 60}, {12420, 70}, {12500, 80}, {12620, 90},
  {12800,100}, {13200,100}
};
static const int SOC_AGM_LEN = sizeof(SOC_AGM) / sizeof(SOC_AGM[0]);

// LiFePO4 (4S, 12.8V nominal)
static const int16_t SOC_LFP[][2] = {
  {10000,  0}, {11200,  0}, {12000,  5}, {12800, 10},
  {13000, 20}, {13100, 30}, {13200, 50}, {13280, 70},
  {13300, 80}, {13350, 90}, {13400, 95}, {13600,100},
  {14600,100}
};
static const int SOC_LFP_LEN = sizeof(SOC_LFP) / sizeof(SOC_LFP[0]);

// ---------- Objects ----------
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);
Adafruit_INA260 ina260;
Preferences  prefs;

// ---------- State ----------
bool          sensorOK     = false;
bool          discovSent   = false;
unsigned long lastReport   = 0;
Chemistry     chemistry    = CHEM_FLOODED;

// Trend buffer (circular, stores voltage in mV)
int32_t  trendBuf[TREND_BUFFER_SIZE];
int      trendHead  = 0;
int      trendCount = 0;

// Last published values (for change detection)
float    lastVoltage  = 0;
int      lastSOC      = -1;
float    lastTrend    = 0;
bool     lastLow      = false;
String   lastLabel    = "";

// ========================  LED  ========================

void ledOn()  { digitalWrite(LED_PIN, HIGH); }
void ledOff() { digitalWrite(LED_PIN, LOW);  }

// ========================  SOC lookup  ========================

int calcSOC(int32_t voltage_mV) {
  const int16_t (*table)[2];
  int len;

  switch (chemistry) {
    case CHEM_AGM:     table = SOC_AGM;     len = SOC_AGM_LEN;     break;
    case CHEM_LIFEPO4: table = SOC_LFP;     len = SOC_LFP_LEN;     break;
    default:           table = SOC_FLOODED;  len = SOC_FLOODED_LEN;  break;
  }

  if (voltage_mV <= table[0][0]) return table[0][1];
  if (voltage_mV >= table[len-1][0]) return table[len-1][1];

  for (int i = 0; i < len - 1; i++) {
    if (voltage_mV >= table[i][0] && voltage_mV < table[i+1][0]) {
      float frac = (float)(voltage_mV - table[i][0]) /
                   (float)(table[i+1][0] - table[i][0]);
      return (int)(table[i][1] + frac * (table[i+1][1] - table[i][1]) + 0.5f);
    }
  }
  return 0;
}

// ========================  Trend  ========================

void trendPush(int32_t voltage_mV) {
  trendBuf[trendHead] = voltage_mV;
  trendHead = (trendHead + 1) % TREND_BUFFER_SIZE;
  if (trendCount < TREND_BUFFER_SIZE) trendCount++;
}

// Returns trend in V/hr
float calcTrend() {
  if (trendCount < 2) return 0.0;

  // Average of oldest half vs newest half
  int half = trendCount / 2;
  float oldAvg = 0, newAvg = 0;

  for (int i = 0; i < half; i++) {
    int oldIdx = (trendHead - trendCount + i + TREND_BUFFER_SIZE) % TREND_BUFFER_SIZE;
    int newIdx = (trendHead - half + i + TREND_BUFFER_SIZE) % TREND_BUFFER_SIZE;
    oldAvg += trendBuf[oldIdx];
    newAvg += trendBuf[newIdx];
  }
  oldAvg /= half;
  newAvg /= half;

  // Time span: half * interval = minutes between the two halves
  float minutesBetween = half * (REPORT_INTERVAL_MS / 60000.0);
  if (minutesBetween < 0.1) return 0.0;

  float delta_mV = newAvg - oldAvg;
  float vPerHour = (delta_mV / minutesBetween) * 60.0 / 1000.0;  // mV/min → V/hr

  return vPerHour;
}

const char* trendLabel(float vPerHour) {
  if (vPerHour >  0.10) return "Charging fast";
  if (vPerHour >  0.02) return "Charging";
  if (vPerHour < -0.10) return "Discharging fast";
  if (vPerHour < -0.02) return "Discharging";
  return "Stable";
}

// ========================  WiFi  ========================

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.printf("[WiFi] Connecting to %s ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("battery-monitor-2");
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    digitalWrite(LED_PIN, attempts % 2);
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected — IP %s\n", WiFi.localIP().toString().c_str());
    ledOn();
  } else {
    Serial.println("\n[WiFi] FAILED — will retry");
    ledOff();
  }
}

// ========================  MQTT  ========================

// Shared device JSON block for all discovery messages
String deviceBlock() {
  String d = "\"device\":{";
  d += "\"identifiers\":[\"" + String(DEVICE_ID) + "\"],";
  d += "\"name\":\"Battery Monitor 2\",";
  d += "\"model\":\"XIAO ESP32-C6 + INA260\",";
  d += "\"manufacturer\":\"DIY\",";
  d += "\"sw_version\":\"2.0\"";
  d += "}";
  return d;
}

void publishDiscovery() {
  String avail = "\"availability_topic\":\"" + String(T_AVAIL) + "\"";
  String state = "\"state_topic\":\"" + String(T_STATE) + "\"";
  String dev   = deviceBlock();

  // --- Voltage ---
  {
    String p = "{\"name\":\"Battery Voltage\",\"unique_id\":\"bat_mon_2_v\",";
    p += state + "," + avail + ",";
    p += "\"device_class\":\"voltage\",\"unit_of_measurement\":\"V\",";
    p += "\"value_template\":\"{{ value_json.voltage }}\",";
    p += "\"state_class\":\"measurement\",\"expire_after\":180,";
    p += "\"icon\":\"mdi:flash\"," + dev + "}";
    mqtt.publish(T_DISC_VOLT, p.c_str(), true);
  }

  // --- SOC ---
  {
    String p = "{\"name\":\"Battery SOC\",\"unique_id\":\"bat_mon_2_soc\",";
    p += state + "," + avail + ",";
    p += "\"device_class\":\"battery\",\"unit_of_measurement\":\"%\",";
    p += "\"value_template\":\"{{ value_json.soc }}\",";
    p += "\"state_class\":\"measurement\",\"expire_after\":180,";
    p += dev + "}";
    mqtt.publish(T_DISC_SOC, p.c_str(), true);
  }

  // --- Voltage Trend ---
  {
    String p = "{\"name\":\"Voltage Trend\",\"unique_id\":\"bat_mon_2_trend\",";
    p += state + "," + avail + ",";
    p += "\"unit_of_measurement\":\"V/hr\",";
    p += "\"value_template\":\"{{ value_json.trend }}\",";
    p += "\"state_class\":\"measurement\",\"expire_after\":180,";
    p += "\"icon\":\"mdi:chart-line\"," + dev + "}";
    mqtt.publish(T_DISC_TREND, p.c_str(), true);
  }

  // --- Trend Label ---
  {
    String p = "{\"name\":\"Battery Trend\",\"unique_id\":\"bat_mon_2_label\",";
    p += state + "," + avail + ",";
    p += "\"value_template\":\"{{ value_json.trend_label }}\",";
    p += "\"expire_after\":180,";
    p += "\"icon\":\"mdi:trending-neutral\"," + dev + "}";
    mqtt.publish(T_DISC_LABEL, p.c_str(), true);
  }

  // --- Battery Condition (binary sensor) ---
  {
    String p = "{\"name\":\"Battery Condition\",\"unique_id\":\"bat_mon_2_low\",";
    p += state + "," + avail + ",";
    p += "\"device_class\":\"battery\",";
    p += "\"value_template\":\"{{ value_json.low }}\",";
    p += "\"payload_on\":\"ON\",\"payload_off\":\"OFF\",";
    p += "\"expire_after\":180," + dev + "}";
    mqtt.publish(T_DISC_LOW, p.c_str(), true);
  }

  // --- Chemistry Select ---
  {
    String p = "{\"name\":\"Battery Chemistry\",\"unique_id\":\"bat_mon_2_chem\",";
    p += "\"command_topic\":\"" + String(T_CHEM_CMD) + "\",";
    p += "\"state_topic\":\"" + String(T_CHEM_STATE) + "\",";
    p += avail + ",";
    p += "\"options\":[\"Lead-Acid (Flooded)\",\"Lead-Acid (AGM)\",\"LiFePO4\"],";
    p += "\"icon\":\"mdi:battery-heart-variant\"," + dev + "}";
    mqtt.publish(T_DISC_CHEM, p.c_str(), true);
  }

  Serial.println("[MQTT] Discovery published (6 entities)");
  discovSent = true;
}

// Handle chemistry change from HA
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  if (String(topic) == T_CHEM_CMD) {
    Serial.printf("[MQTT] Chemistry set to: %s\n", msg.c_str());

    Chemistry newChem = CHEM_FLOODED;
    if (msg == "Lead-Acid (AGM)")   newChem = CHEM_AGM;
    if (msg == "LiFePO4")           newChem = CHEM_LIFEPO4;

    if (newChem != chemistry) {
      chemistry = newChem;
      // Save to NVS so it persists across reboots
      prefs.begin("batmon", false);
      prefs.putInt("chem", (int)chemistry);
      prefs.end();
      Serial.printf("[NVS] Chemistry saved: %s\n", CHEM_NAMES[chemistry]);
    }

    // Publish back to confirm
    mqtt.publish(T_CHEM_STATE, CHEM_NAMES[chemistry], true);
  }
}

void connectMQTT() {
  if (mqtt.connected()) return;

  Serial.print("[MQTT] Connecting... ");
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setBufferSize(600);
  mqtt.setCallback(mqttCallback);

  String clientId = "bat_mon_2_" + String((uint32_t)ESP.getEfuseMac(), HEX);

  bool ok;
  if (strlen(MQTT_USER) > 0) {
    ok = mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS,
                      T_AVAIL, 0, true, "offline");
  } else {
    ok = mqtt.connect(clientId.c_str(), NULL, NULL,
                      T_AVAIL, 0, true, "offline");
  }

  if (ok) {
    Serial.println("connected");
    mqtt.publish(T_AVAIL, "online", true);
    mqtt.subscribe(T_CHEM_CMD);

    if (!discovSent) publishDiscovery();

    // Publish current chemistry state
    mqtt.publish(T_CHEM_STATE, CHEM_NAMES[chemistry], true);
  } else {
    Serial.printf("failed (rc=%d)\n", mqtt.state());
  }
}

// ========================  INA260  ========================

bool initINA260() {
  Wire.begin(I2C_SDA, I2C_SCL);

  if (!ina260.begin(INA260_ADDR, &Wire)) {
    Serial.println("[INA260] NOT FOUND — check wiring:");
    Serial.println("         D4 (GPIO22) → SDA");
    Serial.println("         D5 (GPIO23) → SCL");
    Serial.println("         3V3 → Vcc, GND → GND");
    return false;
  }

  ina260.setAveragingCount(INA260_COUNT_64);
  ina260.setVoltageConversionTime(INA260_TIME_1_1_ms);
  ina260.setCurrentConversionTime(INA260_TIME_1_1_ms);
  ina260.setMode(INA260_MODE_CONTINUOUS);

  Serial.println("[INA260] OK — 64x averaging, continuous");
  Serial.printf("[INA260] I2C: SDA=GPIO%d (D4), SCL=GPIO%d (D5)\n", I2C_SDA, I2C_SCL);
  return true;
}

// ========================  OTA  ========================

void setupOTA() {
  ArduinoOTA.setHostname("battery-monitor-2");
  //ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    Serial.println("[OTA] Update starting...");
    mqtt.publish(T_AVAIL, "offline", true);
    mqtt.disconnect();
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\n[OTA] Complete, rebooting...");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("[OTA] %u%%\r", progress / (total / 100));
    digitalWrite(LED_PIN, (progress / 5000) % 2);
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Error %u\n", error);
  });

  ArduinoOTA.begin();
  Serial.println("[OTA] Ready on port 3232");
}

// ========================  Read & Publish  ========================

void readAndPublish() {
  if (!sensorOK) return;

  // Read voltage
  int32_t voltage_mV = (int32_t)(ina260.readBusVoltage());  // returns float mV
  float voltage_V = voltage_mV / 1000.0;

  // Push to trend buffer
  trendPush(voltage_mV);

  // Calculate SOC
  int soc = calcSOC(voltage_mV);

  // Calculate trend
  float trend = calcTrend();
  // Round to 3 decimal places
  trend = ((int)(trend * 1000 + (trend >= 0 ? 0.5 : -0.5))) / 1000.0;

  const char* label = trendLabel(trend);
  bool low = (soc < 70);

  // Log
  Serial.printf("[READ] %.3f V | SOC %d%% | Trend %.3f V/hr (%s) | %s: %s\n",
                voltage_V, soc, trend, label,
                CHEM_NAMES[chemistry], low ? "LOW" : "OK");

  // Build JSON state payload (all sensors in one message)
  char payload[200];
  snprintf(payload, sizeof(payload),
    "{\"voltage\":%.3f,\"soc\":%d,\"trend\":%.3f,\"trend_label\":\"%s\",\"low\":\"%s\"}",
    voltage_V, soc, trend, label, low ? "ON" : "OFF");

  if (mqtt.connected()) {
    bool ok = mqtt.publish(T_STATE, payload);
    Serial.printf("[MQTT] Publish → %s\n", ok ? "OK" : "FAIL");
  }
}

// ========================  setup  ========================

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(LED_PIN, OUTPUT);
  ledOff();

  Serial.println();
  Serial.println("===================================");
  Serial.println("  Battery Voltage Monitor 2 v2.0");
  Serial.println("  XIAO ESP32-C6 + INA260 + MQTT");
  Serial.println("  All sensors on-board");
  Serial.println("===================================");

  // Load saved chemistry from NVS
  prefs.begin("batmon", true);  // read-only
  chemistry = (Chemistry)prefs.getInt("chem", CHEM_FLOODED);
  prefs.end();
  Serial.printf("[NVS] Chemistry: %s\n", CHEM_NAMES[chemistry]);

  // INA260
  sensorOK = initINA260();
  if (!sensorOK) {
    Serial.println("[FATAL] INA260 not found — retrying every 30s");
  }

  // WiFi
  connectWiFi();

  // OTA
  setupOTA();

  // MQTT
  connectMQTT();

  // Trigger first reading immediately
  lastReport = millis() - REPORT_INTERVAL_MS;

  Serial.println("[READY] All sensors publishing every 60s");
  Serial.printf("[READY] OTA: battery-monitor-2 | IP: %s\n",
                WiFi.localIP().toString().c_str());
}

// ========================  loop  ========================

void loop() {
  ArduinoOTA.handle();

  // Maintain WiFi
  if (WiFi.status() != WL_CONNECTED) {
    ledOff();
    connectWiFi();
  }

  // Maintain MQTT
  if (!mqtt.connected()) {
    connectMQTT();
  }
  mqtt.loop();

  // Retry sensor if failed at boot
  if (!sensorOK) {
    static unsigned long lastRetry = 0;
    if (millis() - lastRetry > 30000) {
      lastRetry = millis();
      Serial.println("[INA260] Retrying...");
      sensorOK = initINA260();
      if (sensorOK) ledOn();
    }
    return;
  }

  // LED solid when connected
  if (WiFi.status() == WL_CONNECTED && mqtt.connected()) {
    ledOn();
  }

  // Periodic reading
  if (millis() - lastReport >= REPORT_INTERVAL_MS) {
    lastReport = millis();
    readAndPublish();
  }
}
