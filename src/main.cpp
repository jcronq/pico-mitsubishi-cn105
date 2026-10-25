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
//     GET  /info    -> identity plus diagnostics (uptime, reset reason, RSSI, ...)
//     GET  /events  -> in-RAM ring buffer of recent commands, unit-side settings
//                      changes and CN105 link transitions (lost on reboot)
//   The RP2040 hardware watchdog reboots the board if loop() wedges.
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
#include <hardware/watchdog.h>

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

// ---------- watchdog ----------
//
// The RP2040 watchdog maxes out at ~8.38 s (the SDK doubles the count to work
// around erratum RP2040-E1), and several paths here block for longer than that
// in total, so the timeout alone cannot cover them. Instead the dog is fed at
// every point that starts a bounded wait:
//   - top of loop()
//   - each iteration of the WiFi connect wait (WiFi.begin() itself would block
//     for up to 2 x 15 s, so connectWifi() uses beginNoBlock() and waits itself)
//   - the start of every CN105 connect attempt, via HeatPump's onConnect
//     callback. That covers hp.connect() calls made from inside hp.sync() as
//     well as ours; each attempt is one delay(2000) plus a ~1 s read wait.
//   - before hp.update() and on every pass of /command's confirm loop
// The longest unfed stretch is then about 5 s (WebServer's HTTP_MAX_POST_WAIT
// for a slow client body, or its HTTP_MAX_SEND_WAIT on the reply), which is
// comfortably inside WDT_TIMEOUT_MS.
#define WDT_TIMEOUT_MS 8000

static inline void feedWatchdog() { rp2040.wdt_reset(); }

// ---------- diagnostics ----------

// millis() wraps after ~49.7 days; keep a 64-bit uptime so event timestamps
// stay monotonic across the wrap. Must be called more often than every 49 days,
// which loop() does.
static uint64_t uptimeMs() {
  static uint32_t last = 0, wraps = 0;
  uint32_t now = millis();
  if (now < last) wraps++;
  last = now;
  return ((uint64_t)wraps << 32) | now;
}

// Boot counter kept in watchdog scratch registers 0/1. Scratch survives
// watchdog and soft resets but not a power cycle, so this counts boots since
// the last power-on. The SDK reserves scratch[4..7] for its own reboot magic.
#define BOOT_MAGIC 0xC105B007u
static uint32_t bootCount = 0;
static const char *resetReason = "unknown";

static void captureBootInfo() {
  // Read the reason before wdt_begin(): watchdog_enable() rewrites scratch[4],
  // which getResetReason() consults.
  switch (rp2040.getResetReason()) {
    case RP2040::PWRON_RESET:    resetReason = "power_on"; break;
    case RP2040::RUN_PIN_RESET:  resetReason = "run_pin"; break;
    case RP2040::SOFT_RESET:     resetReason = "soft"; break;
    case RP2040::WDT_RESET:      resetReason = "watchdog"; break;
    case RP2040::DEBUG_RESET:    resetReason = "debug"; break;
    case RP2040::GLITCH_RESET:   resetReason = "glitch"; break;
    case RP2040::BROWNOUT_RESET: resetReason = "brownout"; break;
    default:                     resetReason = "unknown"; break;
  }
  if (watchdog_hw->scratch[0] != BOOT_MAGIC) {
    watchdog_hw->scratch[0] = BOOT_MAGIC;
    watchdog_hw->scratch[1] = 0;
  }
  bootCount = ++watchdog_hw->scratch[1];
}

static uint32_t cn105ConnectAttempts = 0;  // every hp.connect(), incl. ones from inside hp.sync()
static uint32_t cn105LinkUps = 0;          // down->up transitions of hp.isConnected()
static bool lastLinkUp = false;

// /command is refused until the unit has sent at least one SETTINGS packet.
// Until then HeatPump's tempMode is false and currentSettings is unpopulated,
// so setTemperature(10) misses TEMP_MAP (16..31) and falls back to TEMP_MAP[0]
// = 31 degC, and refreshing wantedSettings from currentSettings would copy
// null pointers.
static bool gotSettings = false;

// Commands are attributed to settings changes observed while one is in flight.
static bool inCommand = false;
static uint64_t lastCommandMs = 0;
static bool haveLastCommand = false;

struct LastCommand {
  uint64_t ts;
  char ip[16];
  int status;
  char body[160];
} lastCommand;

// ---------- event ring ----------
//
// Small in-RAM log so "who changed the setpoint?" can be answered from the
// device itself. Lost on reboot; the boot entry plus reset_reason tells you
// that happened.
#define EVENT_RING_SIZE 32
#define EVENT_DETAIL_LEN 192

enum EventKind : uint8_t { EV_BOOT, EV_COMMAND, EV_SETTINGS_CHANGE, EV_LINK_UP, EV_LINK_DOWN };
static const char *const EVENT_KIND_NAMES[] = {"boot", "command", "settings_change", "link_up", "link_down"};

struct Event {
  uint64_t ts;
  EventKind kind;
  int16_t status;            // HTTP status for commands, else 0
  char ip[16];               // client IP for commands, else ""
  char detail[EVENT_DETAIL_LEN];  // compact JSON
};

static Event events[EVENT_RING_SIZE];
static uint32_t eventSeq = 0;  // total events ever recorded; ring index = seq % size

static void recordEvent(EventKind kind, const char *detail, const char *ip = "", int status = 0) {
  Event &e = events[eventSeq % EVENT_RING_SIZE];
  e.ts = uptimeMs();
  e.kind = kind;
  e.status = status;
  strlcpy(e.ip, ip, sizeof(e.ip));
  strlcpy(e.detail, detail, sizeof(e.detail));
  eventSeq++;
}

// Previous settings, for computing old->new diffs in the change callback.
static heatpumpSettings prevSettings;
static bool havePrevSettings = false;

static void onSettingsChanged() {
  heatpumpSettings s = hp.getSettings();
  JsonDocument d;
  if (!havePrevSettings) {
    d["initial"] = true;
    d["power"] = s.power; d["mode"] = s.mode; d["temperature"] = s.temperature;
    d["fan"] = s.fan; d["vane"] = s.vane; d["widevane"] = s.wideVane;
  } else {
    // Pointer compares are what the library itself uses: every string field
    // points into one of its static *_MAP tables.
    auto diffStr = [&](const char *k, const char *o, const char *n) {
      if (o != n) { JsonArray a = d[k].to<JsonArray>(); a.add(o); a.add(n); }
    };
    diffStr("power", prevSettings.power, s.power);
    diffStr("mode", prevSettings.mode, s.mode);
    if (prevSettings.temperature != s.temperature) {
      JsonArray a = d["temperature"].to<JsonArray>();
      a.add(prevSettings.temperature); a.add(s.temperature);
    }
    diffStr("fan", prevSettings.fan, s.fan);
    diffStr("vane", prevSettings.vane, s.vane);
    diffStr("widevane", prevSettings.wideVane, s.wideVane);
    if (d.size() == 0) d["other"] = true;  // e.g. iSee bit only
  }
  // during_command: seen while POST /command was being handled. Otherwise
  // ms_since_command says how long after the last /command this arrived; a
  // change with no recent command came from the remote or the unit itself.
  d["during_command"] = inCommand;
  if (haveLastCommand) d["ms_since_command"] = (uint64_t)(uptimeMs() - lastCommandMs);
  char buf[EVENT_DETAIL_LEN];
  serializeJson(d, buf, sizeof(buf));
  recordEvent(EV_SETTINGS_CHANGE, buf);
  Serial.printf("settings change: %s\n", buf);
  prevSettings = s;
  havePrevSettings = true;
}

// Fires on every packet in and out. Used only to learn when the first SETTINGS
// reply (0x62 / data[0] == 0x02) has arrived; readPacket() calls this before it
// parses the packet, but nothing reads gotSettings until it has returned.
static void onPacket(byte *packet, unsigned int length, char *direction) {
  if (!gotSettings && length > 5 && !strcmp(direction, "packetRecv") &&
      packet[1] == 0x62 && packet[5] == 0x02) {
    gotSettings = true;
  }
}

static void onCn105Connect() {
  // Start of every connect attempt: feed the dog before its ~3 s of blocking.
  feedWatchdog();
  cn105ConnectAttempts++;
}

static void trackLink() {
  bool up = hp.isConnected();
  if (up == lastLinkUp) return;
  lastLinkUp = up;
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"connect_attempts\":%lu}", (unsigned long)cn105ConnectAttempts);
  if (up) {
    cn105LinkUps++;
    recordEvent(EV_LINK_UP, buf);
  } else {
    recordEvent(EV_LINK_DOWN, buf);
  }
}

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

void writeDiagInto(JsonObject doc) {
  doc["uptime_s"]              = uptimeMs() / 1000;
  doc["reset_reason"]          = resetReason;
  doc["boot_count"]            = bootCount;
  doc["wifi_rssi"]             = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  doc["cn105_connect_attempts"] = cn105ConnectAttempts;
  doc["cn105_link_ups"]        = cn105LinkUps;
  // Link-ups after the first one are reconnects.
  doc["cn105_reconnects"]      = cn105LinkUps > 0 ? cn105LinkUps - 1 : 0;
  doc["settings_received"]     = gotSettings;
  doc["free_heap"]             = rp2040.getFreeHeap();
  if (haveLastCommand) {
    JsonObject lc = doc["last_command"].to<JsonObject>();
    lc["ts_uptime"] = lastCommand.ts / 1000.0;
    lc["source_ip"] = lastCommand.ip;
    lc["status"]    = lastCommand.status;
    lc["body"]      = lastCommand.body;
  } else {
    doc["last_command"] = nullptr;
  }
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
  // Diagnostics are nested so they never collide with the state fields
  // clients diff on.
  writeDiagInto(doc["diag"].to<JsonObject>());
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleEvents() {
  JsonDocument doc;
  doc["uptime_s"] = uptimeMs() / 1000;
  doc["total"]    = eventSeq;  // > size of "events" means older ones were dropped
  JsonArray arr = doc["events"].to<JsonArray>();
  uint32_t n = eventSeq < EVENT_RING_SIZE ? eventSeq : EVENT_RING_SIZE;
  for (uint32_t i = eventSeq - n; i < eventSeq; i++) {  // oldest first
    const Event &e = events[i % EVENT_RING_SIZE];
    JsonObject o = arr.add<JsonObject>();
    o["seq"]       = i;
    o["ts_uptime"] = e.ts / 1000.0;
    o["kind"]      = EVENT_KIND_NAMES[e.kind];
    if (e.kind == EV_COMMAND) {
      o["source_ip"] = e.ip;
      o["status"]    = e.status;
    }
    // detail is JSON we produced; embed it as an object rather than a string.
    JsonDocument det;
    if (!deserializeJson(det, e.detail)) o["detail"] = det;
    else o["detail"] = e.detail;  // truncated command body: keep it as text
  }
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

// Every /command request, accepted or not, is recorded with its source IP so a
// setpoint change can be traced to whoever sent it.
static void recordCommand(int status, const String &note = String()) {
  String ip = server.client().remoteIP().toString();
  String body = server.hasArg("plain") ? server.arg("plain") : String();
  body.replace("\n", " ");
  body.replace("\r", " ");

  lastCommand.ts = uptimeMs();
  strlcpy(lastCommand.ip, ip.c_str(), sizeof(lastCommand.ip));
  lastCommand.status = status;
  strlcpy(lastCommand.body, body.c_str(), sizeof(lastCommand.body));
  lastCommandMs = lastCommand.ts;
  haveLastCommand = true;

  // Detail is the request body, plus the error for rejected requests. Built
  // with ArduinoJson so an arbitrary body cannot break the JSON.
  JsonDocument d;
  JsonDocument parsed;
  if (body.length() && !deserializeJson(parsed, body)) d["body"] = parsed;
  else d["body"] = body.substring(0, 120);
  if (note.length()) d["error"] = note;
  char buf[EVENT_DETAIL_LEN];
  size_t len = serializeJson(d, buf, sizeof(buf));
  if (len >= sizeof(buf) - 1) {  // truncated: fall back to raw text
    strlcpy(buf, body.c_str(), sizeof(buf));
  }
  recordEvent(EV_COMMAND, buf, ip.c_str(), status);
  Serial.printf("command from %s -> %d: %s\n", ip.c_str(), status, body.c_str());
}

static void sendError(int code, const String &msg) {
  recordCommand(code, msg);
  JsonDocument e;
  e["error"] = msg;
  String body;
  serializeJson(e, body);
  server.send(code, "application/json", body);
}

void handleCommand() {
  feedWatchdog();
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

  if (!gotSettings) {
    server.sendHeader("Retry-After", "5");
    sendError(503, "waiting for first settings packet from unit");
    return;
  }

  inCommand = true;

  // Start from what the unit is actually set to. HeatPump never resyncs
  // wantedSettings after the first settings packet (firstRun) unless
  // autoUpdate+externalUpdate are on, and createPacket() sends every field
  // where wantedSettings differs from currentSettings. So without this, a
  // setpoint changed on the handheld remote would be reverted by the next
  // unrelated command such as {"fan":"AUTO"}. After the refresh only the
  // fields in this request can differ, so only they go on the wire.
  hp.setSettings(hp.getSettings());

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

  feedWatchdog();
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
    feedWatchdog();
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
  inCommand = false;
  recordCommand(200);
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
  writeDiagInto(doc.as<JsonObject>());
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
  // beginNoBlock, not begin(): begin() waits up to 2 x 15 s internally with no
  // way to feed the watchdog. Same 30 s budget, fed every 250 ms.
  WiFi.beginNoBlock(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi connecting");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
    feedWatchdog();
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
  captureBootInfo();
  rp2040.wdt_begin(WDT_TIMEOUT_MS);
  Serial.println("\npico-mitsubishi-cn105 boot (HTTP mode)");
  Serial.printf("reset reason: %s, boot %lu since power-on\n",
                resetReason, (unsigned long)bootCount);
  {
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"reset_reason\":\"%s\",\"boot_count\":%lu}",
             resetReason, (unsigned long)bootCount);
    recordEvent(EV_BOOT, buf);
  }

  hp.setOnConnectCallback(onCn105Connect);
  hp.setSettingsChangedCallback(onSettingsChanged);
  hp.setPacketCallback(onPacket);

  connectWifi();
  startMdns();

  server.on("/status",  HTTP_GET,  handleStatus);
  server.on("/command", HTTP_POST, handleCommand);
  server.on("/info",    HTTP_GET,  handleInfo);
  server.on("/events",  HTTP_GET,  handleEvents);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.printf("HTTP server up on port %d\n", HTTP_PORT);

  // Seed the retry clock from this attempt, not from boot, so a failure here
  // retries CN105_RETRY_MS later rather than at millis()==CN105_RETRY_MS.
  lastConnectAttempt = millis();
  cn105Connect();
  trackLink();
}

void loop() {
  feedWatchdog();
  uptimeMs();  // keep the wrap counter current
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
  trackLink();

  if (millis() - lastWifiCheck > WIFI_RETRY_MS) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi dropped, reconnecting");
      connectWifi();
      startMdns();
    }
  }
}
