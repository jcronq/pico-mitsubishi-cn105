# pico-mitsubishi-cn105

Raspberry Pi Pico W firmware for controlling a Mitsubishi mini-split via its
built-in CN105 serial port. Full bidirectional control (mode, setpoint, fan,
vane, room-temp readback) over local MQTT. No cloud.

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
| 3         | RX (to unit, 5V TTL)   | Pico `GP4` through level shifter |
| 4         | TX (from unit, 5V TTL) | Pico `GP5` through level shifter |
| 5         | GND        | Pico `GND`                            |

The level shifter is mandatory — CN105 is 5V TTL and Pico GPIO is 3.3V-tolerant only.

## Build

PlatformIO is used; install the extension in VS Code or the CLI (`pip install platformio`).

1. Clone this repo.
2. `cp include/secrets.h.example include/secrets.h` and fill in WiFi + MQTT.
3. (Optional) Edit `include/config.h` — device id, friendly name, UART pin assignments, model string.
4. Hold the BOOTSEL button on the Pico W, plug into USB, run `pio run -t upload`.

After the first flash, subsequent uploads happen without BOOTSEL via `picotool` (platformio handles it).

## MQTT topics

Topic prefix (`TOPIC_BASE` in `config.h`) defaults to `cozyhem/climate/<device_id>`.

| Topic                              | Direction | Format                                           |
|------------------------------------|-----------|--------------------------------------------------|
| `cozyhem/climate/<id>/state`       | pub       | JSON: `{power, mode, temperature, fan, vane, widevane, room_temp, operating}` |
| `cozyhem/climate/<id>/cmd`         | sub       | JSON subset of the state fields (send only what you want to change) |
| `cozyhem/climate/<id>/available`   | pub       | `online` / `offline` (last-will)                 |
| `homeassistant/climate/<id>/config`| pub (boot)| Home Assistant MQTT discovery payload            |

### Example commands

Set the mode to heat and the setpoint to 20°C:

    { "mode": "HEAT", "temperature": 20 }

Turn the unit off:

    { "power": "OFF" }

Set the fan to QUIET and the vane to SWING:

    { "fan": "QUIET", "vane": "SWING" }

## Home Assistant / CozyHem auto-discovery

On MQTT connect the firmware publishes a retained HA MQTT discovery payload to
`homeassistant/climate/<device_id>/config`. CozyHem (configured with the
standard HA discovery prefix) picks the entity up automatically — no manual
YAML.

## Serial debug

USB CDC runs at 115200 baud. Boot messages, WiFi link state, MQTT connect
events, and command parse errors print here. Attach `pio device monitor` to
watch.

## Credits

- [SwiCago/HeatPump](https://github.com/SwiCago/HeatPump) — Arduino library implementing the Mitsubishi CN105 protocol.
- [mlesniew/PicoMQTT](https://github.com/mlesniew/PicoMQTT) — Lightweight MQTT client with RP2040 support.
- [earlephilhower/arduino-pico](https://github.com/earlephilhower/arduino-pico) — Pico Arduino core.

## License

MIT. See [LICENSE](LICENSE).
