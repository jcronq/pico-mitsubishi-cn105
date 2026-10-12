// Pico W firmware for Mitsubishi mini-split control via CN105.
//
// Architecture:
//   SwiCago/HeatPump drives the CN105 serial protocol on UART1.
//   PicoMQTT publishes state and subscribes to command topics.
//   Home Assistant MQTT auto-discovery payload is sent once at boot so
//   CozyHem (or any HA-compatible broker consumer) picks the device up.
//
// Wiring (Pico W -> CN105 via BSS138 or similar bidirectional level shifter):
//   Pico GP4  (UART1 TX, 3V3)  --[level shifter]--  CN105 pin 3 (RX to unit, 5V)
//   Pico GP5  (UART1 RX, 3V3)  --[level shifter]--  CN105 pin 4 (TX from unit, 5V)
//   Pico GND                                        CN105 pin 5 (GND)
//   Pico VSYS or VIN                                CN105 pin 2 (+5V, feeds the regulator)
//   CN105 pin 1 (+12V) left disconnected.

#include <Arduino.h>
#include <WiFi.h>
#include <HeatPump.h>
#include <PicoMQTT.h>
#include <ArduinoJson.h>

#include "secrets.h"
#include "config.h"

HeatPump hp;
PicoMQTT::Client mqtt(MQTT_HOST, MQTT_PORT, DEVICE_ID, MQTT_USERNAME, MQTT_PASSWORD);

unsigned long lastSync = 0;
unsigned long lastWifiCheck = 0;

// ---------- state publishing ----------

void publishState() {
  JsonDocument doc;
  heatpumpSettings s = hp.getSettings();
  heatpumpStatus st = hp.getStatus();

  doc["power"]       = s.power;              // "ON" / "OFF"
  doc["mode"]        = s.mode;               // "HEAT" / "COOL" / "FAN" / "DRY" / "AUTO"
  doc["temperature"] = s.temperature;        // target setpoint, celsius
  doc["fan"]         = s.fan;                // "AUTO" / "QUIET" / "1".."4"
  doc["vane"]        = s.vane;               // "AUTO" / "1".."5" / "SWING"
  doc["widevane"]    = s.wideVane;           // "<<" / "<" / "|" / ">" / ">>" / "<>" / "SWING"
  doc["room_temp"]   = st.roomTemperature;   // sensed temp from the unit's thermistor
  doc["operating"]   = st.operating;         // true if actively heating/cooling

  char buf[512];
  size_t len = serializeJson(doc, buf, sizeof(buf));
  mqtt.publish(TOPIC_STATE, buf, len, true /*retain*/);
}

// ---------- command handler ----------

void onCommand(const char *topic, const char *payload) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("cmd parse error: %s\n", err.c_str());
    return;
  }

  if (doc["power"].is<const char*>())       hp.setPowerSetting(doc["power"].as<const char*>());
  if (doc["mode"].is<const char*>())        hp.setModeSetting(doc["mode"].as<const char*>());
  if (doc["temperature"].is<float>())       hp.setTemperature(doc["temperature"].as<float>());
  if (doc["fan"].is<const char*>())         hp.setFanSpeed(doc["fan"].as<const char*>());
  if (doc["vane"].is<const char*>())        hp.setVaneSetting(doc["vane"].as<const char*>());
  if (doc["widevane"].is<const char*>())    hp.setWideVaneSetting(doc["widevane"].as<const char*>());

  hp.update();  // push settings to the unit
}

// ---------- Home Assistant MQTT discovery ----------

void publishDiscovery() {
  JsonDocument doc;
  doc["name"]                = FRIENDLY_NAME;
  doc["unique_id"]           = DEVICE_ID;
  doc["mode_cmd_t"]          = TOPIC_CMD;
  doc["mode_cmd_tpl"]        = "{\"mode\":\"{{ value }}\"}";
  doc["mode_stat_t"]         = TOPIC_STATE;
  doc["mode_stat_tpl"]       = "{{ value_json.mode }}";
  doc["temp_cmd_t"]          = TOPIC_CMD;
  doc["temp_cmd_tpl"]        = "{\"temperature\":{{ value }}}";
  doc["temp_stat_t"]         = TOPIC_STATE;
  doc["temp_stat_tpl"]       = "{{ value_json.temperature }}";
  doc["curr_temp_t"]         = TOPIC_STATE;
  doc["curr_temp_tpl"]       = "{{ value_json.room_temp }}";
  doc["fan_mode_cmd_t"]      = TOPIC_CMD;
  doc["fan_mode_cmd_tpl"]    = "{\"fan\":\"{{ value }}\"}";
  doc["fan_mode_stat_t"]     = TOPIC_STATE;
  doc["fan_mode_stat_tpl"]   = "{{ value_json.fan }}";
  doc["avty_t"]              = TOPIC_AVAIL;
  doc["pl_avail"]            = "online";
  doc["pl_not_avail"]        = "offline";
  doc["modes"][0]            = "off";
  doc["modes"][1]            = "heat";
  doc["modes"][2]            = "cool";
  doc["modes"][3]            = "dry";
  doc["modes"][4]            = "fan_only";
  doc["modes"][5]            = "auto";
  doc["min_temp"]            = 16;
  doc["max_temp"]            = 31;
  doc["temp_step"]           = 1;

  JsonObject dev = doc["device"].to<JsonObject>();
  dev["identifiers"][0]      = DEVICE_ID;
  dev["name"]                = FRIENDLY_NAME;
  dev["manufacturer"]        = MANUFACTURER;
  dev["model"]               = MODEL;

  char topic[128];
  snprintf(topic, sizeof(topic), "%s/climate/%s/config", HA_DISCOVERY_PREFIX, DEVICE_ID);

  char buf[1024];
  size_t len = serializeJson(doc, buf, sizeof(buf));
  mqtt.publish(topic, buf, len, true /*retain*/);
}

// ---------- SwiCago callbacks ----------

void onSettingsChanged() {
  publishState();
}

void onRoomTempChanged(float temp) {
  publishState();
}

// ---------- lifecycle ----------

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi connecting");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi up: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("WiFi connect timeout, will retry");
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\npico-mitsubishi-cn105 boot");

  // UART1 on GP4/GP5 for CN105.
  Serial1.setTX(CN105_TX_PIN);
  Serial1.setRX(CN105_RX_PIN);

  connectWifi();

  // MQTT setup. PicoMQTT's Client is non-blocking.
  mqtt.will.topic = TOPIC_AVAIL;
  mqtt.will.payload = "offline";
  mqtt.will.retain = true;

  mqtt.subscribe(TOPIC_CMD, onCommand);

  mqtt.connected_callback = []() {
    Serial.println("MQTT up");
    mqtt.publish(TOPIC_AVAIL, "online", true /*retain*/);
    publishDiscovery();
    publishState();
  };

  // SwiCago HeatPump setup.
  hp.setSettingsChangedCallback(onSettingsChanged);
  hp.setRoomTempChangedCallback(onRoomTempChanged);
  hp.connect(&Serial1);

  mqtt.begin();
}

void loop() {
  mqtt.loop();
  hp.sync();

  // Lazy WiFi re-connect if link dropped.
  if (millis() - lastWifiCheck > WIFI_RETRY_MS) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi dropped, reconnecting");
      connectWifi();
    }
  }
}
