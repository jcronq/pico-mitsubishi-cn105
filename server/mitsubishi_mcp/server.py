"""MCP server exposing mini-split tools over stdio."""

from __future__ import annotations

import json
import logging
import os
from dataclasses import asdict
from typing import Literal

from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp.types import TextContent, Tool

from .mqtt_client import HeatPumpMqtt, MqttConfig

log = logging.getLogger(__name__)

Mode = Literal["HEAT", "COOL", "FAN", "DRY", "AUTO"]
Power = Literal["ON", "OFF"]
Fan = Literal["AUTO", "QUIET", "1", "2", "3", "4"]
Vane = Literal["AUTO", "1", "2", "3", "4", "5", "SWING"]
WideVane = Literal["<<", "<", "|", ">", ">>", "<>", "SWING"]


def build_server(mqtt_client: HeatPumpMqtt) -> Server:
    srv = Server("mitsubishi-mcp")

    @srv.list_tools()
    async def list_tools() -> list[Tool]:
        return [
            Tool(
                name="get_status",
                description="Current state of the mini-split: power, mode, setpoint, fan, vane, room temperature, operating flag.",
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
            snap = mqtt_client.snapshot()
            return [TextContent(type="text", text=json.dumps(asdict(snap), indent=2))]

        if name == "set_power":
            mqtt_client.command(power=arguments["power"])
            return [TextContent(type="text", text=f"power -> {arguments['power']}")]

        if name == "set_mode":
            mqtt_client.command(mode=arguments["mode"])
            return [TextContent(type="text", text=f"mode -> {arguments['mode']}")]

        if name == "set_temperature":
            mqtt_client.command(temperature=float(arguments["celsius"]))
            return [TextContent(type="text", text=f"temperature -> {arguments['celsius']}C")]

        if name == "set_fan_speed":
            mqtt_client.command(fan=arguments["fan"])
            return [TextContent(type="text", text=f"fan -> {arguments['fan']}")]

        if name == "set_vane":
            mqtt_client.command(vane=arguments["vane"])
            return [TextContent(type="text", text=f"vane -> {arguments['vane']}")]

        if name == "set_wide_vane":
            mqtt_client.command(widevane=arguments["widevane"])
            return [TextContent(type="text", text=f"widevane -> {arguments['widevane']}")]

        raise ValueError(f"unknown tool: {name}")

    return srv


def config_from_env() -> MqttConfig:
    return MqttConfig(
        host=os.environ["MITSUBISHI_MQTT_HOST"],
        port=int(os.environ.get("MITSUBISHI_MQTT_PORT", "1883")),
        username=os.environ.get("MITSUBISHI_MQTT_USERNAME", ""),
        password=os.environ.get("MITSUBISHI_MQTT_PASSWORD", ""),
        topic_base=os.environ.get("MITSUBISHI_MQTT_TOPIC_BASE", "cozyhem/climate/barn_minisplit"),
        client_id=os.environ.get("MITSUBISHI_MQTT_CLIENT_ID", "mitsubishi-mcp"),
    )


async def run() -> None:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s %(message)s",
    )
    mqtt_client = HeatPumpMqtt(config_from_env())
    mqtt_client.start()
    mqtt_client.wait_ready(timeout=5.0)   # best-effort warm-up

    srv = build_server(mqtt_client)
    try:
        async with stdio_server() as (read, write):
            await srv.run(read, write, srv.create_initialization_options())
    finally:
        mqtt_client.stop()
