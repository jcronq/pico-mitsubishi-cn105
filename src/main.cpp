// Pico W firmware for Mitsubishi mini-split control via CN105.
//
// Architecture:
//   SwiCago/HeatPump drives the CN105 serial protocol. The port is SerialPIO
//   (PIO software serial), NOT the hardware UART: the working orientation is
//   TX=GP5 / RX=GP4, and RP2040 UART1 permits TX only on {4,8,20,24}, so GP5
//   can never be a hardware TX pin.
//   The Pico serves a small HTTP server:
//     GET  /status  -> JSON state (power, mode, setpoint, fan, vane, room temp, operating)
//     POST /command -> JSON subset of state fields to apply
//   mDNS advertises "barn-minisplit.local" so clients don't need a static IP.
//
// Wiring (Pico W -> CN105 via BSS138 or similar bidirectional level shifter):
//   Pico GP5  (PIO TX) --[level shifter]-- CN105 pin 5 (unit's RX input, 5V)
//   Pico GP4  (PIO RX) --[level shifter]-- CN105 pin 4 (unit's TX output, 5V)
//   Pico GND                               CN105 pin 2 (GND)
//   Pico VSYS                              CN105 pin 3 (+5V)
//   CN105 pin 1 (+12V) left disconnected.
//
//   Verified empirically 2026-10-22: TX=GP5 / RX=GP4 at 2400 8E1 returns a
//   checksum-valid CONNECT_ACK (fc 7a 01 30 01 00 54). The reverse orientation,
//   and 8N1 in either orientation, return zero bytes.
//
//   WARNING: do not power VSYS from USB while the JST is connected -- CN105
//   pin 3 and VSYS are both 5V sources with no isolation between them.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LEAmDNS.h>
#include <HeatPump.h>
#include <ArduinoJson.h>

#include "secrets.h"
#include "config.h"

// CN105 link runs on PIO software serial, not the hardware UART: the correct
// roles are TX=GP5 / RX=GP4, and RP2040 UART1 only permits TX on {4,8,20,24},
// so GP5 cannot be a hardware TX pin. PIO has no such pin restriction.
SerialPIO cn105(CN105_TX_PIN, CN105_RX_PIN, 64);
HeatPump hp;
WebServer server(HTTP_PORT);

unsigned long lastWifiCheck = 0;
unsigned long lastSync = 0;
unsigned long lastConnectAttempt = 0;

// While the CN105 link is down, hp.sync() calls hp.connect() on every pass and
// each connect() blocks ~6s (two delay(2000) plus two 1s read waits), which
// starves server.handleClient(). Retry on a slow timer instead.
#define CN105_RETRY_MS 30000

// ---------- state serialization ----------

void writeStateInto(JsonObject doc) {
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
  writeStateInto(doc.to<JsonObject>());
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

  // hp.update() only refreshes currentSettings when autoUpdate is enabled, which
  // it is not; without this sync the echo below returns the PRE-command state.
  hp.sync();

  // Echo the applied state back so the caller sees the effect.
  JsonDocument out;
  writeStateInto(out.to<JsonObject>());
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

  connectWifi();
  startMdns();

  server.on("/status",  HTTP_GET,  handleStatus);
  server.on("/command", HTTP_POST, handleCommand);
  server.on("/info",    HTTP_GET,  handleInfo);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.printf("HTTP server up on port %d\n", HTTP_PORT);

  hp.connect(&cn105);
}

void loop() {
  server.handleClient();
  MDNS.update();

  if (hp.isConnected()) {
    if (millis() - lastSync >= SYNC_INTERVAL_MS) {
      lastSync = millis();
      hp.sync();
    }
  } else if (millis() - lastConnectAttempt >= CN105_RETRY_MS) {
    lastConnectAttempt = millis();
    hp.connect(&cn105);
  }

  if (millis() - lastWifiCheck > WIFI_RETRY_MS) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi dropped, reconnecting");
      connectWifi();
      startMdns();
    }
  }
}
