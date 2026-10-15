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

| CN105 pin | Signal     | Pico W pin                            |
|-----------|------------|---------------------------------------|
| 1         | +12V       | leave disconnected                    |
| 2         | +5V        | Pico `VSYS` (powers the regulator)    |
| 3         | TX from unit (5V TTL) | Pico `GP5` through level shifter |
| 4         | RX to unit (5V TTL)   | Pico `GP4` through level shifter |
| 5         | GND        | Pico `GND`                            |

The level shifter is mandatory — CN105 is 5V TTL and Pico GPIO is 3.3V-tolerant only.

## Build

PlatformIO is used; install the extension in VS Code or the CLI (`pip install platformio`).

1. Clone this repo.
2. `cp include/secrets.h.example include/secrets.h` and fill in your WiFi SSID + password.
3. (Optional) Edit `include/config.h` — device id, friendly name, UART pin assignments, model string, mDNS hostname.
4. Hold the BOOTSEL button on the Pico W, plug into USB, run `pio run -t upload`.

After the first flash, subsequent uploads happen without BOOTSEL via `picotool` (platformio handles it).

## HTTP API

The firmware advertises itself on mDNS as `barn-minisplit.local` (configurable via `MDNS_HOSTNAME` in `config.h`) on port 80.

| Method | Path       | Description                                                     |
|--------|------------|-----------------------------------------------------------------|
| GET    | `/status`  | Current state as JSON: `{power, mode, temperature, fan, vane, widevane, room_temp, operating, connected}` |
| POST   | `/command` | Apply a subset of state fields. Echoes the new state back.      |
| GET    | `/info`    | Device identity: `{device_id, friendly, manufacturer, model, mdns, ip, connected}` |

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

USB CDC runs at 115200 baud. Boot messages, WiFi link state, and command parse errors print here. Attach `pio device monitor` to watch.

## MCP server

A Python MCP server that wraps the HTTP interface lives at [`server/`](server/). It exposes tools like `get_status`, `set_power`, `set_mode`, `set_temperature`, and `set_fan_speed` for any MCP client (Alice, Claude Desktop, custom integrations). The server talks REST to the Pico — it does not need a broker. See [`server/README.md`](server/README.md) for install, config, and the full tool reference.

## Credits

- [SwiCago/HeatPump](https://github.com/SwiCago/HeatPump) — Arduino library implementing the Mitsubishi CN105 protocol.
- [earlephilhower/arduino-pico](https://github.com/earlephilhower/arduino-pico) — Pico Arduino core with WebServer and LEAmDNS.

## License

MIT. See [LICENSE](LICENSE).
