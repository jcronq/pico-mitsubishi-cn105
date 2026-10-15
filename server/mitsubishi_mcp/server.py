"""MCP server exposing mini-split tools over stdio."""

from __future__ import annotations

import json
import logging
import os
from dataclasses import asdict

from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp.types import TextContent, Tool

from .http_client import HeatPumpHttp, HttpConfig

log = logging.getLogger(__name__)


def build_server(client: HeatPumpHttp) -> Server:
    srv = Server("mitsubishi-mcp")

    @srv.list_tools()
    async def list_tools() -> list[Tool]:
        return [
            Tool(
                name="get_status",
                description="Current state of the mini-split: power, mode, setpoint, fan, vane, room temperature, operating flag, connected flag.",
                inputSchema={"type": "object", "properties": {}, "required": []},
            ),
            Tool(
                name="get_info",
                description="Device identity: device_id, friendly name, manufacturer, model, mDNS name, IP, connected flag.",
                inputSchema={"type": "object", "properties": {}, "required": []},
            ),
            Tool(
                name="set_power",
                description="Turn the unit on or off.",
                inputSchema={
                    "type": "object",
                    "properties": {"power": {"type": "string", "enum": ["ON", "OFF"]}},
                    "required": ["power"],
                },
            ),
            Tool(
                name="set_mode",
                description="Set the operating mode. HEAT/COOL/FAN/DRY/AUTO. Turning the unit on is separate — call set_power(ON) first if it is off.",
                inputSchema={
                    "type": "object",
                    "properties": {"mode": {"type": "string", "enum": ["HEAT", "COOL", "FAN", "DRY", "AUTO"]}},
                    "required": ["mode"],
                },
            ),
            Tool(
                name="set_temperature",
                description="Set the target temperature in Celsius. Valid range 16-31.",
                inputSchema={
                    "type": "object",
                    "properties": {"celsius": {"type": "number", "minimum": 16, "maximum": 31}},
                    "required": ["celsius"],
                },
            ),
            Tool(
                name="set_fan_speed",
                description="Set the fan speed. AUTO, QUIET, or 1..4.",
                inputSchema={
                    "type": "object",
                    "properties": {"fan": {"type": "string", "enum": ["AUTO", "QUIET", "1", "2", "3", "4"]}},
                    "required": ["fan"],
                },
            ),
            Tool(
                name="set_vane",
                description="Set the vertical vane position. AUTO, 1..5 (1=most upward, 5=most downward), or SWING.",
                inputSchema={
                    "type": "object",
                    "properties": {"vane": {"type": "string", "enum": ["AUTO", "1", "2", "3", "4", "5", "SWING"]}},
                    "required": ["vane"],
                },
            ),
            Tool(
                name="set_wide_vane",
                description="Set the horizontal (wide) vane position. << most-left, >> most-right, | center, <> split, SWING.",
                inputSchema={
                    "type": "object",
                    "properties": {"widevane": {"type": "string", "enum": ["<<", "<", "|", ">", ">>", "<>", "SWING"]}},
                    "required": ["widevane"],
                },
            ),
        ]

    @srv.call_tool()
    async def call_tool(name: str, arguments: dict) -> list[TextContent]:
        if name == "get_status":
            snap = client.snapshot()
            return [TextContent(type="text", text=json.dumps(asdict(snap), indent=2))]

        if name == "get_info":
            info = client.info()
            return [TextContent(type="text", text=json.dumps(info, indent=2))]

        if name == "set_power":
            result = client.command(power=arguments["power"])
            return [TextContent(type="text", text=json.dumps(result, indent=2))]

        if name == "set_mode":
            result = client.command(mode=arguments["mode"])
            return [TextContent(type="text", text=json.dumps(result, indent=2))]

        if name == "set_temperature":
            result = client.command(temperature=float(arguments["celsius"]))
            return [TextContent(type="text", text=json.dumps(result, indent=2))]

        if name == "set_fan_speed":
            result = client.command(fan=arguments["fan"])
            return [TextContent(type="text", text=json.dumps(result, indent=2))]

        if name == "set_vane":
            result = client.command(vane=arguments["vane"])
            return [TextContent(type="text", text=json.dumps(result, indent=2))]

        if name == "set_wide_vane":
            result = client.command(widevane=arguments["widevane"])
            return [TextContent(type="text", text=json.dumps(result, indent=2))]

        raise ValueError(f"unknown tool: {name}")

    return srv


def config_from_env() -> HttpConfig:
    return HttpConfig(
        base_url=os.environ.get("MITSUBISHI_URL", "http://barn-minisplit.local"),
        timeout=float(os.environ.get("MITSUBISHI_TIMEOUT", "5.0")),
    )


async def run() -> None:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s %(message)s",
    )
    client = HeatPumpHttp(config_from_env())

    srv = build_server(client)
    async with stdio_server() as (read, write):
        await srv.run(read, write, srv.create_initialization_options())
