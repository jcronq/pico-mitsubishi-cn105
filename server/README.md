# mitsubishi-mcp

MCP server exposing the Pico-W-flashed Mitsubishi mini-split as a set of tools
callable by any MCP client (Alice, Claude Desktop, etc.).

The server does not talk to the heat pump directly — it rides the MQTT
interface the Pico firmware already publishes:

    client → MCP tool call → this server → MQTT publish → Pico W → CN105 → unit

## Install

Local dev:

    cd server
    pip install -e .

Docker:

    docker build -t mitsubishi-mcp .

## Configure

All config comes from env vars. See `config.example.env` for the full list.
The minimum is `MITSUBISHI_MQTT_HOST`.

## Run

Standalone (reads/writes stdio — meant to be launched by an MCP client):

    export $(cat .env | xargs) && mitsubishi-mcp

Or via Docker, as launched by Claude Desktop or similar:

    docker run --rm -i \
      --env-file .env \
      mitsubishi-mcp

## Claude Desktop config

Add to your `claude_desktop_config.json`:

    {
      "mcpServers": {
        "mitsubishi-barn": {
          "command": "docker",
          "args": ["run", "--rm", "-i", "--env-file", "/path/to/.env", "mitsubishi-mcp"]
        }
      }
    }

Or if installed locally:

    {
      "mcpServers": {
        "mitsubishi-barn": {
          "command": "mitsubishi-mcp",
          "env": {
            "MITSUBISHI_MQTT_HOST": "10.20.30.xxx",
            "MITSUBISHI_MQTT_TOPIC_BASE": "cozyhem/climate/barn_minisplit"
          }
        }
      }
    }

## Tools

| Tool              | Args                                 | Effect                             |
|-------------------|--------------------------------------|------------------------------------|
| `get_status`      | —                                    | Current state (power/mode/temp/fan/vane/room_temp/operating/available) |
| `set_power`       | `power: "ON" \| "OFF"`               | Turn the unit on or off            |
| `set_mode`        | `mode: HEAT\|COOL\|FAN\|DRY\|AUTO`   | Change operating mode              |
| `set_temperature` | `celsius: 16..31`                    | Set target setpoint                |
| `set_fan_speed`   | `fan: AUTO\|QUIET\|1..4`             | Set fan speed                      |
| `set_vane`        | `vane: AUTO\|1..5\|SWING`            | Set vertical vane position         |
| `set_wide_vane`   | `widevane: <<\|<\|\|\|>\|>>\|<>\|SWING` | Set horizontal vane position   |

## Notes

- Commands are published to `<topic_base>/cmd` as a JSON object. The Pico
  firmware applies the fields and the unit's reply flows back on
  `<topic_base>/state`, which the server caches for `get_status`.
- `get_status` returns the latest cached state — it does not block on a fresh
  round-trip. If the Pico has never published state, the fields read
  `"UNKNOWN"` / `0`.
- The server stays connected to the broker for the lifetime of the MCP
  session; disconnect is graceful on client exit.
