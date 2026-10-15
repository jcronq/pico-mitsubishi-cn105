// Pico W firmware for Mitsubishi mini-split control via CN105.
//
// Architecture:
//   SwiCago/HeatPump drives the CN105 serial protocol on UART1.
//   The Pico serves a small HTTP server with two endpoints:
//     GET  /status  -> JSON state (power, mode, setpoint, fan, vane, room temp, operating)
//     POST /command -> JSON subset of state fields to apply
//   mDNS advertises "barn-minisplit.local" so clients don't need a static IP.
//
// Wiring (Pico W -> CN105 via BSS138 or similar bidirectional level shifter):
//   CN105 pin 1 (+12V)           : leave disconnected
//   CN105 pin 2 (GND)            : Pico GND + shifter GND (common ground)
//   CN105 pin 3 (+5V)            : Pico VSYS + shifter HV rail
//   CN105 pin 4 (unit TX, 5V)    : --[shifter HV2/LV2]-- Pico GP5 (UART1 RX)
//   CN105 pin 5 (unit RX, 5V)    : --[shifter HV1/LV1]-- Pico GP4 (UART1 TX)

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LEAmDNS.h>
#include <HeatPump.h>
#include <ArduinoJson.h>

#include "secrets.h"
#include "config.h"

HeatPump hp;
WebServer server(HTTP_PORT);

unsigned long lastWifiCheck = 0;

// ---------- state serialization ----------

void writeStateJson(JsonDocument &doc) {
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
  doc["connected"]   = hp.isConnected();
}

// ---------- HTTP handlers ----------

void handleStatus() {
  JsonDocument doc;
  writeStateJson(doc);
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleCommand() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"missing body\"}");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    String msg = "{\"error\":\"parse: ";
    msg += err.c_str();
    msg += "\"}";
    server.send(400, "application/json", msg);
    return;
  }

  if (doc["power"].is<const char*>())      hp.setPowerSetting(doc["power"].as<const char*>());
  if (doc["mode"].is<const char*>())       hp.setModeSetting(doc["mode"].as<const char*>());
  if (doc["temperature"].is<float>())      hp.setTemperature(doc["temperature"].as<float>());
  if (doc["fan"].is<const char*>())        hp.setFanSpeed(doc["fan"].as<const char*>());
  if (doc["vane"].is<const char*>())       hp.setVaneSetting(doc["vane"].as<const char*>());
  if (doc["widevane"].is<const char*>())   hp.setWideVaneSetting(doc["widevane"].as<const char*>());

  hp.update();

  // Echo the applied state back so the caller sees the effect.
  JsonDocument out;
  writeStateJson(out);
  String body;
  serializeJson(out, body);
  server.send(200, "application/json", body);
}

void handleInfo() {
  JsonDocument doc;
  doc["device_id"]    = DEVICE_ID;
  doc["friendly"]     = FRIENDLY_NAME;
  doc["manufacturer"] = MANUFACTURER;
  doc["model"]        = MODEL;
  doc["mdns"]         = String(MDNS_HOSTNAME) + ".local";
  doc["ip"]           = WiFi.localIP().toString();
  doc["connected"]    = hp.isConnected();
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleNotFound() {
  server.send(404, "application/json", "{\"error\":\"not found\"}");
}

// ---------- lifecycle ----------

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(MDNS_HOSTNAME);
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

void startMdns() {
  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", HTTP_PORT);
    Serial.printf("mDNS: %s.local -> %s\n",
                  MDNS_HOSTNAME, WiFi.localIP().toString().c_str());
  } else {
    Serial.println("mDNS begin failed");
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\npico-mitsubishi-cn105 boot (HTTP mode)");

  // UART1 on GP4/GP5 for CN105.
  Serial1.setTX(CN105_TX_PIN);
  Serial1.setRX(CN105_RX_PIN);

  connectWifi();
  startMdns();

  server.on("/status",  HTTP_GET,  handleStatus);
  server.on("/command", HTTP_POST, handleCommand);
  server.on("/info",    HTTP_GET,  handleInfo);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.printf("HTTP server up on port %d\n", HTTP_PORT);

  hp.connect(&Serial1);
}

void loop() {
  server.handleClient();
  hp.sync();
  MDNS.update();

  if (millis() - lastWifiCheck > WIFI_RETRY_MS) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi dropped, reconnecting");
      connectWifi();
      startMdns();
    }
  }
}
