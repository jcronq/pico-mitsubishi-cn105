"""Thin MQTT wrapper. Caches the latest retained state and publishes commands."""

from __future__ import annotations

import json
import logging
import threading
from dataclasses import dataclass, field
from typing import Any

import paho.mqtt.client as mqtt

log = logging.getLogger(__name__)


@dataclass
class MqttConfig:
    host: str
    port: int = 1883
    username: str = ""
    password: str = ""
    topic_base: str = "cozyhem/climate/barn_minisplit"
    client_id: str = "mitsubishi-mcp"


@dataclass
class HeatPumpState:
    power: str = "UNKNOWN"           # "ON" | "OFF"
    mode: str = "UNKNOWN"            # "HEAT" | "COOL" | "FAN" | "DRY" | "AUTO"
    temperature: float = 0.0         # setpoint, celsius
    fan: str = "UNKNOWN"             # "AUTO" | "QUIET" | "1".."4"
    vane: str = "UNKNOWN"            # "AUTO" | "1".."5" | "SWING"
    widevane: str = "UNKNOWN"
    room_temp: float = 0.0           # sensed room temperature
    operating: bool = False
    available: bool = False          # LWT online/offline
    raw: dict[str, Any] = field(default_factory=dict)


class HeatPumpMqtt:
    def __init__(self, config: MqttConfig):
        self.config = config
        self.state = HeatPumpState()
        self._state_lock = threading.Lock()
        self._ready_event = threading.Event()

        self.client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id=config.client_id,
        )
        if config.username:
            self.client.username_pw_set(config.username, config.password)
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message

    # ---------- lifecycle ----------

    def start(self) -> None:
        self.client.connect(self.config.host, self.config.port, keepalive=60)
        self.client.loop_start()

    def stop(self) -> None:
        self.client.loop_stop()
        self.client.disconnect()

    def wait_ready(self, timeout: float = 5.0) -> bool:
        """Block until the first state message arrives (or timeout)."""
        return self._ready_event.wait(timeout)

    # ---------- callbacks ----------

    def _on_connect(self, client, userdata, flags, reason_code, properties=None):
        base = self.config.topic_base
        client.subscribe(f"{base}/state")
        client.subscribe(f"{base}/available")
        log.info("MQTT connected to %s:%d, subscribed under %s/*",
                 self.config.host, self.config.port, base)

    def _on_message(self, client, userdata, msg):
        payload = msg.payload.decode(errors="replace")
        if msg.topic.endswith("/state"):
            try:
                data = json.loads(payload)
            except json.JSONDecodeError as e:
                log.warning("state parse error: %s payload=%r", e, payload[:200])
                return
            with self._state_lock:
                self.state.power       = data.get("power", "UNKNOWN")
                self.state.mode        = data.get("mode", "UNKNOWN")
                self.state.temperature = float(data.get("temperature", 0))
                self.state.fan         = data.get("fan", "UNKNOWN")
                self.state.vane        = data.get("vane", "UNKNOWN")
                self.state.widevane    = data.get("widevane", "UNKNOWN")
                self.state.room_temp   = float(data.get("room_temp", 0))
                self.state.operating   = bool(data.get("operating", False))
                self.state.raw         = data
            self._ready_event.set()
        elif msg.topic.endswith("/available"):
            with self._state_lock:
                self.state.available = (payload.strip() == "online")

    # ---------- read ----------

    def snapshot(self) -> HeatPumpState:
        """Thread-safe copy of the cached state."""
        with self._state_lock:
            return HeatPumpState(
                power=self.state.power,
                mode=self.state.mode,
                temperature=self.state.temperature,
                fan=self.state.fan,
                vane=self.state.vane,
                widevane=self.state.widevane,
                room_temp=self.state.room_temp,
                operating=self.state.operating,
                available=self.state.available,
                raw=dict(self.state.raw),
            )

    # ---------- write ----------

    def command(self, **fields: Any) -> None:
        """Publish a command. Only pass the fields you want to change."""
        topic = f"{self.config.topic_base}/cmd"
        payload = json.dumps(fields)
        result = self.client.publish(topic, payload, qos=1)
        result.wait_for_publish(timeout=5)
        log.info("cmd %s %s", topic, payload)
