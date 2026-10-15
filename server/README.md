# mitsubishi-mcp

MCP server exposing the Pico-W-flashed Mitsubishi mini-split as a set of tools
callable by any MCP client (Alice, Claude Desktop, etc.).

The server talks REST to the firmware's HTTP API — no broker required:

    client -> MCP tool call -> this server -> HTTP GET/POST -> Pico W -> CN105 -> unit

## Install

Local dev:

    cd server
    pip install -e .

Docker:

    docker build -t mitsubishi-mcp .

## Configure

All config comes from env vars. See `config.example.env`. The minimum is `MITSUBISHI_URL` (defaults to `http://barn-minisplit.local` so an untouched install works if your resolver handles .local).

## Run

Standalone (reads/writes stdio — meant to be launched by an MCP client):

    export $(cat .env | xargs) && mitsubishi-mcp

Or via Docker, as launched by Claude Desktop or similar:

    docker run --rm -i --env-file .env mitsubishi-mcp

## Claude Desktop config

Add to your `claude_desktop_config.json`:

    {
      "mcpServers": {
        "mitsubishi-barn": {
          "command": "mitsubishi-mcp",
          "env": {
            "MITSUBISHI_URL": "http://barn-minisplit.local"
          }
        }
      }
    }

## Tools

| Tool              | Args                                   | Effect                             |
|-------------------|----------------------------------------|------------------------------------|
| `get_status`      | —                                      | Current state via `GET /status`    |
| `get_info`        | —                                      | Device identity via `GET /info`    |
| `set_power`       | `power: "ON" \| "OFF"`                 | Turn the unit on or off            |
| `set_mode`        | `mode: HEAT\|COOL\|FAN\|DRY\|AUTO`     | Change operating mode              |
| `set_temperature` | `celsius: 16..31`                      | Set target setpoint                |
| `set_fan_speed`   | `fan: AUTO\|QUIET\|1..4`               | Set fan speed                      |
| `set_vane`        | `vane: AUTO\|1..5\|SWING`              | Set vertical vane position         |
| `set_wide_vane`   | `widevane: <<\|<\|\|\|>\|>>\|<>\|SWING` | Set horizontal vane position   |

## Notes

- Every call hits the firmware directly over HTTP. The firmware's `/command` endpoint echoes the new state back, which the server returns to the client — so `set_*` tools double as `get_status` on success.
- No broker, no retained state. If the Pico is offline, calls fail fast and the server returns an error JSON to the MCP client.
