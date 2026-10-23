# pico-mitsubishi-cn105

Raspberry Pi Pico W firmware for controlling a Mitsubishi mini-split via its
built-in CN105 serial port. Full bidirectional control (mode, setpoint, fan,
vane, room-temp readback) over a local HTTP API. No cloud.

## Hardware

- Raspberry Pi Pico W (RP2040 + CYW43439)
- JST PA 5-pin plug: housing `PAP-05V-S`, contacts `SPHD-002T-P0.5` (26AWG stranded recommended). CN105 is JST PA — JST PH shares the 2.0mm pitch but will not clip.
- Bidirectional 3.3V/5V level shifter (BSS138 breakout, TXS0108E, or equivalent)
- 26AWG stranded wire (five conductors, 20–50cm)
- ENGINEER PA-09 crimper for the SPHD contacts

### Wiring

CN105 pin numbering with the connector latch down, pins facing you, left to right:

| CN105 pin | Signal | Pico W pin |
|-----------|--------|------------|
| 1         | +12V (measured 12.5-15V on some units) | **leave disconnected** |
| 2         | GND | Pico `GND` (and level-shifter GND) |
| 3         | +5V | Pico `VSYS` (and level-shifter HV supply) |
| 4         | TX from unit (5V TTL) | Pico `GP4` = serial **RX**, through level shifter |
| 5         | RX to unit (5V TTL) | Pico `GP5` = serial **TX**, through level shifter |

The level shifter is mandatory — CN105 is 5V TTL and Pico GPIO is 3.3V-tolerant only.

Note the pin roles: **TX is GP5 and RX is GP4.** This matters more than it looks,
because it is why the firmware uses `SerialPIO` (PIO software serial) instead of the
hardware UART. RP2040 UART1 permits TX only on GPIO `{4, 8, 20, 24}`, so GP5 can
never be a hardware TX pin. PIO has no such restriction. Setting `CN105_TX_PIN 5`
with a hardware `Serial2` would call `panic()` at boot and halt the chip silently —
no serial output, no WiFi, nothing on the network.

Verified on a MSZ-FH15NA, 2026-10-22: with TX=GP5 / RX=GP4 at 2400 8E1, sending the
CONNECT packet `fc 5a 01 30 02 ca 01 a8` returns a checksum-valid `CONNECT_ACK`:

    fc 7a 01 30 01 00 54

The reverse orientation (TX=GP4 / RX=GP5), and 8N1 in either orientation, return
zero bytes.

**Verify the pinout before connecting anything.** An earlier revision of this table
was wrong: it listed +5V on pin 2, the data lines on pins 3/4, and GND on pin 5. That
puts `VSYS` on the unit's ground and the Pico's ground on a data line. It destroyed one
indoor-unit control board.

Pin 2 and pin 3 cannot be distinguished with a voltmeter: an idle 5V TTL data line also
reads about 5V. Identify them by counting positions with the connector latch down, or by
loading the pin -- pin 3 holds up, a data line collapses.

**Never connect USB and the JST at the same time.** CN105 pin 3 feeds `VSYS` directly,
and USB `VBUS` reaches `VSYS` through a Schottky diode, so the two 5V sources sit on the
same node with no isolation. Unplug the JST before plugging in USB, and unplug USB before
reconnecting the JST. The board runs entirely on CN105 power — USB is only needed for
flashing.

Bus framing is 2400 baud, 8 data bits, EVEN parity, 1 stop bit (some indoor units use
9600; SwiCago's library retries at the other rate automatically).

Pinout confirmed against the ESPHome `mitsubishi_cn105` component, casualhacker.net's
CN105 connector writeup, and the Serin Labs CN105 wiring guide.

## Build

PlatformIO is used; install the extension in VS Code or the CLI (`pip install platformio`).

1. Clone this repo.
2. `cp include/secrets.h.example include/secrets.h` and fill in your WiFi SSID + password.
3. (Optional) Edit `include/config.h` — device id, friendly name, serial pin assignments, model string, mDNS hostname.
4. `pio run` to build.
5. Unplug the JST, then hold BOOTSEL, plug into USB, and release. The `RPI-RP2`
   volume appears.
6. Copy the firmware onto it:

       cp .pio/build/pico_w/firmware.uf2 /Volumes/RPI-RP2/

   `cp: fcopyfile failed: Device not configured` on the last block is **normal** — the
   bootloader reboots as soon as it has the final block. Confirm success by checking that
   `RPI-RP2` has disappeared.
7. Unplug USB, reconnect the JST.

**`pio run -t upload` does not work on this setup.** It fails with
`No accessible RP2040/RP2350 devices in BOOTSEL mode` (Error 249); copying the UF2 by hand
is the reliable path. Nor can BOOTSEL be triggered in software: `picotool reboot -f -u` and
the 1200-baud touch both fail against this firmware, so **every** reflash needs the physical
button, not just the first.

## HTTP API

The firmware advertises itself on mDNS as `barn-minisplit.local` (configurable via `MDNS_HOSTNAME` in `config.h`) on port 80.

| Method | Path       | Description                                                     |
|--------|------------|-----------------------------------------------------------------|
| GET    | `/status`  | Current state as JSON: `{power, mode, temperature, fan, vane, widevane, room_temp, operating, connected}` |
| POST   | `/command` | Apply a subset of state fields. Echoes the new state back.      |
| GET    | `/info`    | Device identity: `{device_id, friendly, manufacturer, model, mdns, ip, connected}` |

The firmware deliberately exposes nothing else. A `/diag` endpoint and a `/rpc` probe
interface were used to diagnose the CN105 link (see [Troubleshooting](#troubleshooting))
and then removed, because they drove the serial port and toggled GPIO with no
authentication — fine on a trusted LAN, not something to leave running.

### Example

Fetch state:

    curl http://barn-minisplit.local/status

Turn on heat at 20°C:

    curl -X POST -H 'Content-Type: application/json' \
         -d '{"power":"ON","mode":"HEAT","temperature":20}' \
         http://barn-minisplit.local/command

Turn the unit off:

    curl -X POST -H 'Content-Type: application/json' \
         -d '{"power":"OFF"}' \
         http://barn-minisplit.local/command

Set the fan to QUIET and the vane to SWING:

    curl -X POST -H 'Content-Type: application/json' \
         -d '{"fan":"QUIET","vane":"SWING"}' \
         http://barn-minisplit.local/command

`POST /command` takes roughly 6-8 seconds to return. `HeatPump::update()` gates sending
on a 2 s interval and then waits up to 1 s to read the acknowledgement, and the handler
then calls `hp.sync()` so the echoed state reflects the command rather than the state
before it. The setpoint itself lands on the unit about a second after the write.

Field domains (all strings except `temperature` which is a float):

- `power`: `"ON"` | `"OFF"`
- `mode`: `"HEAT"` | `"COOL"` | `"FAN"` | `"DRY"` | `"AUTO"`
- `temperature`: 16..31 (°C)
- `fan`: `"AUTO"` | `"QUIET"` | `"1"` .. `"4"`
- `vane`: `"AUTO"` | `"1"` .. `"5"` | `"SWING"`
- `widevane`: `"<<"` | `"<"` | `"|"` | `">"` | `">>"` | `"<>"` | `"SWING"`

## CozyHem integration

The device presents as a REST endpoint that matches the shape of CozyHem's existing adapters (Hue, Anthem). Add a Mitsubishi adapter in `cozyhem-engine/backend/` that polls `GET /status` on an interval and routes CozyHem commands to `POST /command`. mDNS resolution means no static IP reservation is required, though a DHCP reservation is nicer for monitoring.

## Serial debug

USB CDC runs at 115200 baud. Boot messages, WiFi link state, and command parse errors print
here. Attach `pio device monitor --baud 115200` to watch.

Two caveats. The boot banner is printed within a second or two of reset, so a monitor
attached after the fact will have missed it — and since the JST must be off for USB to be
connected, **serial debugging cannot observe a live CN105 link at all**. Anything about the
link itself has to be diagnosed over WiFi (see [Diagnostic tooling](#diagnostic-tooling)).
Expected boot sequence:

    pico-mitsubishi-cn105 boot (HTTP mode)
    WiFi connecting....
    WiFi up: 10.20.30.100
    mDNS: barn-minisplit.local -> 10.20.30.100
    HTTP server up on port 80

## MCP server

A Python MCP server that wraps the HTTP interface lives at [`server/`](server/). It exposes tools like `get_status`, `set_power`, `set_mode`, `set_temperature`, and `set_fan_speed` for any MCP client (Alice, Claude Desktop, custom integrations). The server talks REST to the Pico — it does not need a broker. See [`server/README.md`](server/README.md) for install, config, and the full tool reference.

## Troubleshooting

### `/status` returns `"connected": false`

`connected` is `hp.isConnected()` — the **CN105 serial link**, not WiFi. If the device
answers HTTP at all, WiFi is fine; these are two independent connections, and only the
serial one is in question.

Check in this order.

**1. Pin roles.** This is the most likely cause on a fresh build, and the symptom is
exactly `connected: false` forever with no other sign of trouble. The link needs
**TX=GP5, RX=GP4**. Reversing them produces zero received bytes, silently. Remember that
TX on GP5 requires `SerialPIO`; a hardware `Serial2` will `panic()` at boot instead.

**2. Framing.** CN105 is 2400 baud 8**E**1. With EVEN parity the unit replies; with 8N1 it
returns nothing at all, in either pin orientation. Some indoor units run at 9600, and
`HeatPump::connect()` flips between 2400 and 9600 on retry by itself.

**3. Electrical.** If neither orientation nor either baud produces a single byte, the
problem is physical: confirm the level shifter's HV side is actually powered from CN105
pin 3, and check continuity on pins 4 and 5.

A valid reply to the CONNECT packet `fc 5a 01 30 02 ca 01 a8` looks like:

    fc 7a 01 30 01 00 54

`0xfc` start, `0x7a` = CONNECT_ACK, then `01 30` header. The checksum is
`(0xfc - sum(all preceding bytes, including the leading 0xfc)) & 0xff`, which comes to
`0x54` here — worth computing by hand once to confirm a reply is real and not line noise.

### Diagnostic tooling

The `/diag` and `/rpc` endpoints used to find the above are preserved in commit
`72f1f02` and were removed in the commit that follows it. Restore that file temporarily if
a link ever needs diagnosing again. `/diag` returned, in one call: CONNECT probes across
{2400, 9600} x {8E1, 8N1}; a passive listen; the same CONNECT with the TX/RX roles reversed
over PIO software serial; and a TX/RX short test. `/rpc` exposed `sys.info`, `line.probe`,
`line.short_test`, `uart.probe`, `uart.listen`, `pio.probe`, `hp.connect`, `hp.sync` and
`hp.status` for interactive probing.

Two lessons from that exercise are worth keeping:

- **Reading the line state with internal pulls proves nothing on a BSS138 breakout.** The
  board has 10k pull-ups to each rail and the RP2040's internal pulldown is only ~50-80k,
  so the pin reads high even with the JST physically unplugged. Count received bytes
  instead.
- **Always take a control reading with the unit disconnected.** The reversed-orientation
  probe only became meaningful once the disconnected baseline showed what "nothing
  attached" looks like.

### HTTP requests time out or are dropped

If most requests get no response while some succeed, the main loop is being starved by the
CN105 link rather than by anything in the network stack.

`hp.sync()` is **not** non-blocking, despite how it is usually described. Whenever the link
is down it calls `connect()`, and `connect()` costs about 6 seconds — two `delay(2000)`
calls plus two roughly 1-second waits. Calling it once per second against a dead link
leaves almost no time for `server.handleClient()`. Measured on a build that did exactly
that: 44 of 180 requests answered over 15 minutes, about 24%.

`loop()` therefore syncs only while `hp.isConnected()` and rate-limits reconnection to one
attempt every `CN105_RETRY_MS` (30 s). With that in place the same poll answers 12 of 12.
Keep this in mind before moving `hp.sync()` or shortening the retry interval.

### Nothing enumerates over USB at all

No `RPI-RP2` volume, no tty, and no USB attach events point at the **cable** before the
board. Many micro-USB cables are power-only. Two boards failing identically on one cable
is evidence about the cable, not the boards.

If a flash succeeds but no `/dev/tty.usbmodem*` appears, picotool's soft "reboot to
application" can leave USB half-initialised; a physical power cycle brings the CDC port up.

## Known issues

- `platformio.ini` points `platform` at an unpinned git URL
  (`https://github.com/maxgerhardt/platform-raspberrypi.git`), so builds are not
  reproducible over time. This firmware was built against core `6.1.0-4-gfd65f6d4`.
  Pin it to a release tag before relying on it.

## Credits

- [SwiCago/HeatPump](https://github.com/SwiCago/HeatPump) — Arduino library implementing the Mitsubishi CN105 protocol.
- [earlephilhower/arduino-pico](https://github.com/earlephilhower/arduino-pico) — Pico Arduino core with WebServer and LEAmDNS.

## License

MIT. See [LICENSE](LICENSE).
