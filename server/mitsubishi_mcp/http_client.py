"""Thin HTTP wrapper around the Pico firmware's REST API."""

from __future__ import annotations

import logging
from dataclasses import dataclass, field
from typing import Any

import requests

log = logging.getLogger(__name__)


@dataclass
class HttpConfig:
    base_url: str              # e.g. "http://barn-minisplit.local"
    timeout: float = 5.0


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
    connected: bool = False          # heatpump serial connect state
    raw: dict[str, Any] = field(default_factory=dict)


class HeatPumpHttp:
    def __init__(self, config: HttpConfig):
        self.config = config
        self._session = requests.Session()

    def _url(self, path: str) -> str:
        return f"{self.config.base_url.rstrip('/')}{path}"

    # ---------- read ----------

    def snapshot(self) -> HeatPumpState:
        """Fetch /status and return a parsed state object."""
        try:
            r = self._session.get(self._url("/status"), timeout=self.config.timeout)
            r.raise_for_status()
            data = r.json()
        except requests.RequestException as e:
            log.warning("status fetch failed: %s", e)
            return HeatPumpState()

        return HeatPumpState(
            power=str(data.get("power", "UNKNOWN")),
            mode=str(data.get("mode", "UNKNOWN")),
            temperature=float(data.get("temperature", 0) or 0),
            fan=str(data.get("fan", "UNKNOWN")),
            vane=str(data.get("vane", "UNKNOWN")),
            widevane=str(data.get("widevane", "UNKNOWN")),
            room_temp=float(data.get("room_temp", 0) or 0),
            operating=bool(data.get("operating", False)),
            connected=bool(data.get("connected", False)),
            raw=data,
        )

    def info(self) -> dict[str, Any]:
        """Fetch /info for device identity + mDNS/IP info."""
        try:
            r = self._session.get(self._url("/info"), timeout=self.config.timeout)
            r.raise_for_status()
            return r.json()
        except requests.RequestException as e:
            log.warning("info fetch failed: %s", e)
            return {}

    # ---------- write ----------

    def command(self, **fields: Any) -> dict[str, Any]:
        """POST /command with the given field subset. Returns the Pico's echoed new state."""
        payload = {k: v for k, v in fields.items() if v is not None}
        try:
            r = self._session.post(
                self._url("/command"),
                json=payload,
                timeout=self.config.timeout,
            )
            r.raise_for_status()
            return r.json()
        except requests.RequestException as e:
            log.warning("command POST failed: %s payload=%r", e, payload)
            return {"error": str(e)}
