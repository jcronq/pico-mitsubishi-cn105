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
//     GET  /diag    -> one-shot link diagnostics
//     POST /rpc     -> JSON-RPC-style probes (see handleRpc)
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

// ---------- CN105 link diagnostics ----------
//
// The serial link to the indoor unit and the web server share one thread, so
// these helpers always release the UART before touching pin modes: the core
// panics if setTX/setRX is called while the port is running.

static const uint8_t CN105_CONNECT[8] = {0xfc, 0x5a, 0x01, 0x30, 0x02, 0xca, 0x01, 0xa8};

static String toHex(const uint8_t *b, size_t n) {
  String s;
  char t[4];
  for (size_t i = 0; i < n; i++) {
    snprintf(t, sizeof(t), "%02x", b[i]);
    if (i) s += ' ';
    s += t;
  }
  return s;
}

static size_t fromHex(const String &in, uint8_t *out, size_t max) {
  size_t n = 0;
  int hi = -1;
  for (size_t i = 0; i < in.length() && n < max; i++) {
    char c = in[i];
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else continue;
    if (hi < 0) hi = v;
    else { out[n++] = (uint8_t)((hi << 4) | v); hi = -1; }
  }
  return n;
}

static uint32_t frameConfig(const String &f) {
  if (f == "8N1") return SERIAL_8N1;
  if (f == "8O1") return SERIAL_8O1;
  return SERIAL_8E1;
}

static void uartRelease() {
  cn105.end();
  delay(20);
}

static void uartOpen(int baud, const String &frame) {
  cn105.begin(baud, frameConfig(frame));   // pins are fixed at construction
  delay(20);
}

// Describe a received buffer against Mitsubishi framing.
static void classifyRx(JsonObject o, const uint8_t *buf, size_t n,
                       const uint8_t *sent, size_t sentLen) {
  o["rx_bytes"] = (int)n;
  o["hex"] = toHex(buf, n > 64 ? 64 : n);
  bool fcFirst = (n > 0 && buf[0] == 0xfc);
  o["starts_with_fc"] = fcFirst;
  int fcCount = 0;
  for (size_t i = 0; i < n; i++) if (buf[i] == 0xfc) fcCount++;
  o["fc_count"] = fcCount;
  if (n >= 5 && fcFirst) {
    o["type_byte"] = buf[1];
    const char *label = "unknown";
    if (buf[1] == 0x7a)      label = "CONNECT_ACK";
    else if (buf[1] == 0x62) label = "DATA";
    else if (buf[1] == 0x61) label = "UPDATE_ACK";
    o["type"] = label;
    o["header_ok"] = (buf[2] == 0x01 && buf[3] == 0x30);
    o["data_len"] = buf[4];
  }
  bool echo = (sent != nullptr && sentLen > 0 && n >= sentLen);
  if (echo) for (size_t i = 0; i < sentLen; i++) if (buf[i] != sent[i]) { echo = false; break; }
  o["echo_of_sent"] = echo;
}

// Send `sent` (optional), then collect whatever comes back for listenMs.
static void uartProbe(JsonObject o, int baud, const String &frame,
                      const uint8_t *sent, size_t sentLen, int listenMs) {
  o["baud"] = baud;
  o["frame"] = frame;
  uartRelease();
  uartOpen(baud, frame);
  while (cn105.available()) cn105.read();
  if (sent && sentLen) {
    o["sent_hex"] = toHex(sent, sentLen);
    cn105.write(sent, sentLen);
    cn105.flush();
  }
  static uint8_t buf[256];
  size_t n = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < (unsigned long)listenMs && n < sizeof(buf)) {
    while (cn105.available() && n < sizeof(buf)) buf[n++] = (uint8_t)cn105.read();
    delay(2);
  }
  classifyRx(o, buf, n, sent, sentLen);
  uartRelease();
}

// Is the line actively driven, floating, or held at ground?
// PIO software serial probe: unlike the hardware UART, PIO can put TX and RX on
// ANY pins, so this can test the GP4/GP5 orientation reversed without rewiring.
static void pioProbe(JsonObject o, int txPin, int rxPin, int baud,
                     const String &frame, const uint8_t *sent, size_t sentLen,
                     int listenMs, bool invert) {
  o["tx_gpio"]  = txPin;
  o["rx_gpio"]  = rxPin;
  o["baud"]     = baud;
  o["frame"]    = frame;
  o["inverted"] = invert;
  uartRelease();   // hand GP4/GP5 back from the hardware UART before PIO takes them
  SerialPIO *sp = new SerialPIO(txPin, rxPin, 64);
  if (invert) sp->setInverted(true, true);
  sp->begin(baud, frameConfig(frame));
  delay(10);
  while (sp->available()) sp->read();
  if (sent && sentLen) {
    o["sent_hex"] = toHex(sent, sentLen);
    sp->write(sent, sentLen);
    sp->flush();
  }
  static uint8_t buf[256];
  size_t n = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < (unsigned long)listenMs && n < sizeof(buf)) {
    while (sp->available() && n < sizeof(buf)) buf[n++] = (uint8_t)sp->read();
    delay(2);
  }
  o["rx_overflow"] = sp->overflow();
  classifyRx(o, buf, n, sent, sentLen);
  sp->end();
  delete sp;
  delay(10);
}

static void lineProbe(JsonObject o, int pin) {
  o["gpio"] = pin;
  pinMode(pin, INPUT);          delay(5); int np = digitalRead(pin);
  pinMode(pin, INPUT_PULLDOWN); delay(5); int pd = digitalRead(pin);
  pinMode(pin, INPUT_PULLUP);   delay(5); int pu = digitalRead(pin);
  pinMode(pin, INPUT);
  o["no_pull"] = np;
  o["with_pulldown"] = pd;
  o["with_pullup"] = pu;
  const char *v;
  if (pd == 1 && pu == 1)      v = "driven_high: something actively holds it high, which is what an idle 5V TTL transmitter looks like";
  else if (pd == 0 && pu == 0) v = "driven_low: held at ground - shifter dead/unpowered, or the wire lands on GND";
  else                         v = "floating: nothing drives it - not connected, shifter unpowered, or landed on the unit's RX input instead of its TX output";
  o["verdict"] = v;
}

static void shortTest(JsonObject o) {
  uartRelease();
  pinMode(CN105_RX_PIN, INPUT);
  pinMode(CN105_TX_PIN, OUTPUT);
  digitalWrite(CN105_TX_PIN, LOW);  delay(10); int lo = digitalRead(CN105_RX_PIN);
  digitalWrite(CN105_TX_PIN, HIGH); delay(10); int hi = digitalRead(CN105_RX_PIN);
  pinMode(CN105_TX_PIN, INPUT);
  o["rx_when_tx_low"] = lo;
  o["rx_when_tx_high"] = hi;
  o["shorted"] = (lo == 0 && hi == 1);
  o["note"] = "shorted=true means TX and RX are tied together: crossed shifter channels or a bridge";
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

void handleDiag() {
  JsonDocument doc;
  doc["uptime_ms"]          = millis();
  doc["ip"]                 = WiFi.localIP().toString();
  doc["rssi"]               = WiFi.RSSI();
  doc["hp_connected_before"] = hp.isConnected();
  doc["tx_gpio"]            = CN105_TX_PIN;
  doc["rx_gpio"]            = CN105_RX_PIN;
  doc["connect_packet"]     = toHex(CN105_CONNECT, 8);

  uartRelease();
  lineProbe(doc["rx_line"].to<JsonObject>(), CN105_RX_PIN);
  lineProbe(doc["tx_line"].to<JsonObject>(), CN105_TX_PIN);
  shortTest(doc["tx_rx_short"].to<JsonObject>());

  JsonArray probes = doc["probes"].to<JsonArray>();
  const int bauds[2]     = {2400, 9600};
  const char *frames[2]  = {"8E1", "8N1"};
  for (int b = 0; b < 2; b++)
    for (int f = 0; f < 2; f++)
      uartProbe(probes.add<JsonObject>(), bauds[b], frames[f], CN105_CONNECT, 8, 1500);

  uartProbe(doc["passive_listen"].to<JsonObject>(), 2400, "8E1", nullptr, 0, 3000);

  // Same CONNECT with the pin roles reversed, as a standing regression check.
  // Entry 0 is the configured (working) orientation; entries 1-2 are reversed
  // and are expected to stay at rx_bytes=0. If that ever inverts, the data
  // wires moved.
  JsonArray swaps = doc["swap_test"].to<JsonArray>();
  pioProbe(swaps.add<JsonObject>(), CN105_TX_PIN, CN105_RX_PIN, 2400, "8E1",
           CN105_CONNECT, 8, 1500, false);
  pioProbe(swaps.add<JsonObject>(), CN105_RX_PIN, CN105_TX_PIN, 2400, "8E1",
           CN105_CONNECT, 8, 1500, false);
  pioProbe(swaps.add<JsonObject>(), CN105_RX_PIN, CN105_TX_PIN, 2400, "8N1",
           CN105_CONNECT, 8, 1500, false);

  doc["note"] = "UART is closed after /diag; POST /rpc {\"method\":\"hp.connect\"} or wait for the auto-retry";
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleRpc() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"missing body\"}");
    return;
  }
  JsonDocument req;
  if (deserializeJson(req, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }

  String m = String((const char *)(req["method"] | ""));
  JsonVariant p = req["params"];

  JsonDocument res;
  res["method"] = m;
  JsonObject r = res["result"].to<JsonObject>();

  if (m == "sys.info") {
    r["uptime_ms"]    = millis();
    r["ip"]           = WiFi.localIP().toString();
    r["rssi"]         = WiFi.RSSI();
    r["hp_connected"] = hp.isConnected();
    r["tx_gpio"]      = CN105_TX_PIN;
    r["rx_gpio"]      = CN105_RX_PIN;
    r["free_heap"]    = rp2040.getFreeHeap();
  } else if (m == "line.probe") {
    int pin = p["pin"] | CN105_RX_PIN;
    if (pin != CN105_TX_PIN && pin != CN105_RX_PIN) {
      res["error"] = "pin restricted to the two CN105 data pins";
    } else {
      uartRelease();
      lineProbe(r, pin);
    }
  } else if (m == "line.short_test") {
    shortTest(r);
  } else if (m == "uart.probe" || m == "uart.listen") {
    int baud = p["baud"] | 2400;
    String frame = String((const char *)(p["frame"] | "8E1"));
    int ms = p["listen_ms"] | (m == "uart.probe" ? 1500 : 3000);
    if (ms > 10000) ms = 10000;
    uint8_t sbuf[64];
    size_t sn = 0;
    if (m == "uart.probe") {
      String sh = String((const char *)(p["send_hex"] | "connect"));
      if (sh == "connect") { memcpy(sbuf, CN105_CONNECT, 8); sn = 8; }
      else if (sh.length()) sn = fromHex(sh, sbuf, sizeof(sbuf));
    }
    uartProbe(r, baud, frame, sn ? sbuf : nullptr, sn, ms);
  } else if (m == "pio.probe") {
    int txPin = p["tx"] | CN105_TX_PIN;
    int rxPin = p["rx"] | CN105_RX_PIN;
    bool okPins = (txPin == CN105_TX_PIN || txPin == CN105_RX_PIN) &&
                  (rxPin == CN105_TX_PIN || rxPin == CN105_RX_PIN) &&
                  txPin != rxPin;
    if (!okPins) {
      res["error"] = "tx/rx restricted to the two CN105 data pins, and must differ";
    } else {
      int baud = p["baud"] | 2400;
      String frame = String((const char *)(p["frame"] | "8E1"));
      int ms = p["listen_ms"] | 1500;
      if (ms > 10000) ms = 10000;
      bool invert = p["invert"] | false;
      uint8_t sbuf[64];
      size_t sn = 0;
      String sh = String((const char *)(p["send_hex"] | "connect"));
      if (sh == "connect") { memcpy(sbuf, CN105_CONNECT, 8); sn = 8; }
      else if (sh.length()) sn = fromHex(sh, sbuf, sizeof(sbuf));
      pioProbe(r, txPin, rxPin, baud, frame, sn ? sbuf : nullptr, sn, ms, invert);
    }
  } else if (m == "hp.connect") {
    uartRelease();
    bool ok = hp.connect(&cn105);
    r["connect_returned"] = ok;
    r["is_connected"]     = hp.isConnected();
  } else if (m == "hp.sync") {
    hp.sync();
    writeStateInto(r);
  } else if (m == "hp.status") {
    writeStateInto(r);
  } else {
    res["error"] = "unknown method";
    JsonArray ms2 = res["methods"].to<JsonArray>();
    ms2.add("sys.info");
    ms2.add("line.probe");
    ms2.add("line.short_test");
    ms2.add("uart.probe");
    ms2.add("uart.listen");
    ms2.add("pio.probe");
    ms2.add("hp.connect");
    ms2.add("hp.sync");
    ms2.add("hp.status");
  }

  String out;
  serializeJson(res, out);
  server.send(200, "application/json", out);
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
  server.on("/diag",    HTTP_GET,  handleDiag);
  server.on("/rpc",     HTTP_POST, handleRpc);
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
