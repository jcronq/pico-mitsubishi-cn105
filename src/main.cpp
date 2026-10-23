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
//   CN105 pin 1 (+12V)          : leave disconnected
//   CN105 pin 2 (GND)           : Pico GND + shifter GND (common ground)
//   CN105 pin 3 (+5V)           : Pico VSYS + shifter HV rail
//   CN105 pin 4 (unit TX, 5V)   : --[level shifter]-- Pico GP4 (PIO RX)
//   CN105 pin 5 (unit RX, 5V)   : --[level shifter]-- Pico GP5 (PIO TX)
//
//   The data pins run STRAIGHT THROUGH (pin 4 -> GP4, pin 5 -> GP5), so the
//   Pico's roles are TX=GP5 / RX=GP4. Verified empirically 2026-10-22: that
//   orientation at 2400 8E1 returns a checksum-valid CONNECT_ACK
//   (fc 7a 01 30 01 00 54); the reverse, and 8N1 either way, return zero bytes.
//
//   If you instead cross the data pins (pin 4 -> GP5, pin 5 -> GP4), the roles
//   become TX=GP4 / RX=GP5 and the hardware UART1 can be used instead of PIO.
//   Either build works, but the firmware must match the wiring.
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

// Upper bound on how long POST /command waits for the unit to confirm the new
// settings before echoing them back. On timeout the echo lags by one sync and
// GET /status catches up within about a second.
//
// Sizing: one settings round trip costs ~2.1 s. canSend(isInfo=true) gates on
// PACKET_INFO_INTERVAL_MS (2000) since the last send, then canRead() gates on
// PACKET_SENT_INTERVAL_MS (1000) after the request goes out. A second attempt
// would complete at ~4.1 s, and SerialPIO pushes that out another ~85 ms
// because writes block until the 22-byte packet has clocked out at 2400 baud
// and lastSend is stamped afterwards. 4000 would send the retry and then break
// before its reply could be read, so this is 5000.
//
// Note this is not a hard bound on handler latency: the check happens once per
// iteration and hp.sync() can itself enter a ~6 s hp.connect() if the link has
// gone stale, so worst-case latency is nearer 10 s.
#define CMD_CONFIRM_MS 5000

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

// Open the CN105 link. Always go through here, never hp.connect() directly.
//
// SerialPIO::begin() is NOT idempotent. Unlike SerialUART::begin(), which opens
// with "if (_running) { end(); }", it has no such guard and never calls end():
// every call claims two fresh state machines through pio_claim_unused_sm() and
// abandons the previous pair, because pio_sm_unclaim() appears only in end().
// HeatPump::connect() calls begin() unconditionally, so each reconnect would
// leak 2 of the 7 state machines available to us (the CYW43 WiFi driver holds
// one of the 8), and the abandoned RX machine stays enabled with its FIFO
// interrupt armed while _pioSP[] no longer routes it to a handler.
//
// retry=false matters too: on failure HeatPump::connect() flips its bitrate
// member between 2400 and 9600 and recurses, which is both a second unpaired
// begin() and a port left at the wrong baud for the following attempt.
static bool cn105Connect() {
  cn105.end();
  delay(20);
  return hp.connect(&cn105, false);
}

// ---------- HTTP handlers ----------

void handleStatus() {
  JsonDocument doc;
  writeStateInto(doc.to<JsonObject>());
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// Value domains accepted by SwiCago's setters. Validation matters because every
// string setter falls back to MAP[0] on an unrecognised value instead of
// failing: a lowercase {"power":"on"} would set POWER_MAP[0] == "OFF" and turn
// the unit off while returning 200. Reject rather than guess.
static const char *const POWER_VALUES[]    = {"ON", "OFF"};
static const char *const MODE_VALUES[]     = {"HEAT", "COOL", "FAN", "DRY", "AUTO"};
static const char *const FAN_VALUES[]      = {"AUTO", "QUIET", "1", "2", "3", "4"};
static const char *const VANE_VALUES[]     = {"AUTO", "1", "2", "3", "4", "5", "SWING"};
static const char *const WIDEVANE_VALUES[] = {"<<", "<", "|", ">", ">>", "<>", "SWING"};

template <size_t N>
static bool inDomain(const char *v, const char *const (&list)[N]) {
  for (size_t i = 0; i < N; i++) if (!strcmp(v, list[i])) return true;
  return false;
}

static void sendError(int code, const String &msg) {
  JsonDocument e;
  e["error"] = msg;
  String body;
  serializeJson(e, body);
  server.send(code, "application/json", body);
}

void handleCommand() {
  if (!server.hasArg("plain")) {
    sendError(400, "missing body");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    sendError(400, String("parse: ") + err.c_str());
    return;
  }

  // Validate everything before applying anything, so a request is all-or-nothing.
  JsonObject req = doc.as<JsonObject>();
  for (JsonPair kv : req) {
    const char *k = kv.key().c_str();
    bool known = false;
    for (const char *c : {"power", "mode", "temperature", "fan", "vane", "widevane"}) {
      if (!strcmp(k, c)) { known = true; break; }
    }
    if (!known) {
      sendError(400, String("unknown field: ") + k);
      return;
    }
    if (!strcmp(k, "temperature")) {
      if (!kv.value().is<float>()) {
        sendError(400, "temperature must be a number");
        return;
      }
      float t = kv.value().as<float>();
      if (t < 10.0f || t > 31.0f) {
        sendError(400, "temperature out of range (10..31)");
        return;
      }
      continue;
    }
    if (!kv.value().is<const char *>()) {
      sendError(400, String(k) + " must be a string");
      return;
    }
    const char *v = kv.value().as<const char *>();
    bool valid = !strcmp(k, "power")    ? inDomain(v, POWER_VALUES)
               : !strcmp(k, "mode")     ? inDomain(v, MODE_VALUES)
               : !strcmp(k, "fan")      ? inDomain(v, FAN_VALUES)
               : !strcmp(k, "vane")     ? inDomain(v, VANE_VALUES)
               :                          inDomain(v, WIDEVANE_VALUES);
    if (!valid) {
      sendError(400, String("invalid value for ") + k + ": " + v);
      return;
    }
  }

  if (req.size() == 0) {
    sendError(400, "no fields to apply");
    return;
  }

  if (!hp.isConnected()) {
    sendError(503, "CN105 link not connected");
    return;
  }

  if (req["power"].is<const char *>())       hp.setPowerSetting(req["power"].as<const char *>());
  if (req["mode"].is<const char *>())        hp.setModeSetting(req["mode"].as<const char *>());
  if (req["temperature"].is<float>())        hp.setTemperature(req["temperature"].as<float>());
  if (req["fan"].is<const char *>())         hp.setFanSpeed(req["fan"].as<const char *>());
  if (req["vane"].is<const char *>())        hp.setVaneSetting(req["vane"].as<const char *>());
  if (req["widevane"].is<const char *>())    hp.setWideVaneSetting(req["widevane"].as<const char *>());

  // Drop any reply still in flight from loop()'s periodic sync. readPacket()
  // consumes exactly one packet and never drains, so a stale info response left
  // in the queue would be eaten by update() below, which would then miss its own
  // ack and push confirmation a full round trip past the budget.
  while (cn105.available()) cn105.read();

  hp.update();

  // Echo the state the unit reports, not the state from before the command.
  //
  // Two things conspire against a naive read. hp.update() refreshes
  // currentSettings only when autoUpdate is enabled, which it is not. And
  // hp.sync() is a single-step state machine: one call does exactly ONE of
  // connect / readPacket / send-info-request, so a single call can never
  // complete a request-response round trip. Pump it until the unit confirms
  // the fields we actually applied.
  JsonDocument out;
  unsigned long t0 = millis();
  for (;;) {
    // Ask for SETTINGS specifically. The default packet type makes
    // createInfoPacket() rotate infoMode through six request types (settings,
    // room temp, timers, status, ...), so a plain sync() has roughly a 1-in-6
    // chance of returning the settings packet this echo needs. Passing an
    // explicit type also leaves loop()'s own rotation undisturbed.
    hp.sync(hp.RQST_PKT_SETTINGS);
    writeStateInto(out.to<JsonObject>());
    bool confirmed = true;
    for (JsonPair kv : req) {
      if (out[kv.key().c_str()] != kv.value()) { confirmed = false; break; }
    }
    if (confirmed || millis() - t0 > CMD_CONFIRM_MS) break;
    delay(25);
  }
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

  connectWifi();
  startMdns();

  server.on("/status",  HTTP_GET,  handleStatus);
  server.on("/command", HTTP_POST, handleCommand);
  server.on("/info",    HTTP_GET,  handleInfo);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.printf("HTTP server up on port %d\n", HTTP_PORT);

  // Seed the retry clock from this attempt, not from boot, so a failure here
  // retries CN105_RETRY_MS later rather than at millis()==CN105_RETRY_MS.
  lastConnectAttempt = millis();
  cn105Connect();
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
    cn105Connect();
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
