#!/usr/bin/env python3
"""Clawdmeter multi-provider usage daemon (BLE, macOS).

Polls one or more LLM providers (Claude, OpenAI, DeepSeek) in parallel
and sends a compact JSON payload to the Clawdmeter device over a custom
GATT service. Provider behaviour lives in `providers/` — this file only
loads config, schedules polls, and handles BLE.

The filename is preserved from the single-provider days so existing
LaunchAgent plists keep working.
"""
from __future__ import annotations

import asyncio
import json
import re
import signal
import sys
import time
from pathlib import Path

import httpx
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

from providers import PROVIDERS

# ── Constants ─────────────────────────────────────────────────────────────────

DEVICE_NAME   = "Claude Controller"
SERVICE_UUID  = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID  = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"

DEFAULT_POLL_INTERVAL = 60
TICK         = 5
SCAN_TIMEOUT = 8.0

SAVED_ADDR_FILE = Path.home() / ".config" / "claude-usage-monitor" / "ble-address"
CONFIG_PATHS = [
    Path.home() / ".clawdmeter.json",
    Path(__file__).resolve().parent / "config.json",
]

# Backoff schedule per provider (seconds). After N consecutive failures
# we sleep BACKOFF[min(N, len-1)] before retrying that one provider.
BACKOFF = [0, 10, 30, 60, 120, 300]


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


# ── Config ────────────────────────────────────────────────────────────────────

def load_config() -> dict:
    """Read config.json from one of the known locations. Claude defaults
    to enabled (preserves the pre-multi-provider behaviour)."""
    cfg: dict = {
        "claude":   {"enabled": True},
        "openai":   {"enabled": False},
        "deepseek": {"enabled": False},
        "poll_interval": DEFAULT_POLL_INTERVAL,
    }
    for path in CONFIG_PATHS:
        if path.is_file():
            try:
                user_cfg = json.loads(path.read_text())
                # Shallow merge per top-level key.
                for k, v in user_cfg.items():
                    if isinstance(v, dict) and isinstance(cfg.get(k), dict):
                        cfg[k] = {**cfg[k], **v}
                    else:
                        cfg[k] = v
                log(f"Loaded config: {path}")
                break
            except (OSError, json.JSONDecodeError) as e:
                log(f"Config load failed ({path}): {e}")
    return cfg


# ── BLE address cache ─────────────────────────────────────────────────────────

def load_cached_address() -> str | None:
    if not SAVED_ADDR_FILE.exists():
        return None
    addr = SAVED_ADDR_FILE.read_text().strip()
    if re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", addr) or re.fullmatch(
        r"[0-9A-Fa-f]{8}-(?:[0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}", addr
    ):
        return addr
    return None


def save_address(addr: str) -> None:
    SAVED_ADDR_FILE.parent.mkdir(parents=True, exist_ok=True)
    SAVED_ADDR_FILE.write_text(addr)


async def scan_for_device() -> str | None:
    log(f"Scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
    devices = await BleakScanner.discover(timeout=SCAN_TIMEOUT)
    for d in devices:
        if d.name == DEVICE_NAME:
            log(f"Found: {d.address}")
            return d.address
    return None


# ── Polling all enabled providers ─────────────────────────────────────────────

# (provider_module, consecutive_failure_count, next_attempt_epoch)
_provider_state: dict[str, dict] = {}


def _enabled_providers(cfg: dict) -> list:
    out = []
    for mod in PROVIDERS:
        sub = cfg.get(mod.ID, {})
        if sub.get("enabled"):
            out.append(mod)
    return out


async def _fetch_one(mod, http: httpx.AsyncClient, cfg: dict) -> dict:
    state = _provider_state.setdefault(mod.ID, {"fails": 0, "next": 0.0})
    now = time.time()
    if now < state["next"]:
        return {"id": mod.ID, "ok": False, "error": "backing off"}
    try:
        result = await mod.fetch(http, cfg.get(mod.ID, {}))
    except Exception as e:
        result = {"id": mod.ID, "ok": False, "error": f"unexpected: {e}"}
    if result.get("ok"):
        state["fails"] = 0
        state["next"]  = 0.0
    else:
        state["fails"] += 1
        delay = BACKOFF[min(state["fails"], len(BACKOFF) - 1)]
        state["next"] = time.time() + delay
        log(f"[{mod.ID}] {result.get('error','failed')} (retry in {delay}s)")
    return result


async def poll_all(cfg: dict) -> dict | None:
    """Return the v2 BLE payload, or None if every provider failed."""
    enabled = _enabled_providers(cfg)
    if not enabled:
        log("No providers enabled — nothing to send")
        return None

    async with httpx.AsyncClient() as http:
        results = await asyncio.gather(*(_fetch_one(m, http, cfg) for m in enabled))

    ok_results = [r for r in results if r.get("ok")]
    if not ok_results:
        return None

    return {"v": 2, "p": ok_results}


# ── BLE session ───────────────────────────────────────────────────────────────

class Session:
    def __init__(self, client: BleakClient) -> None:
        self.client = client
        self.refresh_requested = asyncio.Event()

    def _on_refresh(self, _char, _data: bytearray) -> None:
        log("Refresh requested by device")
        self.refresh_requested.set()

    async def setup_refresh_subscription(self) -> None:
        try:
            await self.client.start_notify(REQ_CHAR_UUID, self._on_refresh)
        except (BleakError, ValueError) as e:
            log(f"Refresh subscription unavailable: {e}")

    async def write_payload(self, payload: dict) -> bool:
        data = json.dumps(payload, separators=(",", ":")).encode()
        log(f"Sending: {data.decode()}")
        try:
            await self.client.write_gatt_char(RX_CHAR_UUID, data, response=False)
            return True
        except BleakError as e:
            log(f"Write failed: {e}")
            return False


async def connect_and_run(address: str, cfg: dict, stop_event: asyncio.Event) -> bool:
    log(f"Connecting to {address}...")
    client = BleakClient(address)
    try:
        await client.connect()
    except (BleakError, asyncio.TimeoutError) as e:
        log(f"Connection failed: {e}")
        return False

    if not client.is_connected:
        log("Connection failed (no error but not connected)")
        return False

    log("Connected")
    session = Session(client)
    await session.setup_refresh_subscription()

    poll_interval = int(cfg.get("poll_interval", DEFAULT_POLL_INTERVAL))
    last_poll = 0.0
    used_successfully = False
    try:
        while client.is_connected and not stop_event.is_set():
            now = time.time()
            elapsed = now - last_poll
            if session.refresh_requested.is_set() or elapsed >= poll_interval:
                session.refresh_requested.clear()
                payload = await poll_all(cfg)
                if payload is not None:
                    if await session.write_payload(payload):
                        last_poll = time.time()
                        used_successfully = True

            try:
                await asyncio.wait_for(session.refresh_requested.wait(), timeout=TICK)
            except asyncio.TimeoutError:
                pass
    finally:
        try:
            await client.disconnect()
        except BleakError:
            pass

    log("Device disconnected" if not stop_event.is_set() else "Stopping")
    return used_successfully


# ── Main ──────────────────────────────────────────────────────────────────────

async def main() -> None:
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    def _stop(*_args: object) -> None:
        log("Daemon stopping")
        stop_event.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _stop)
        except NotImplementedError:
            signal.signal(sig, _stop)

    log("=== Clawdmeter Multi-Provider Daemon (BLE, macOS) ===")
    cfg = load_config()
    enabled_ids = [m.ID for m in _enabled_providers(cfg)]
    log(f"Poll interval: {cfg.get('poll_interval', DEFAULT_POLL_INTERVAL)}s; enabled: {enabled_ids}")

    backoff = 1
    while not stop_event.is_set():
        address = load_cached_address()
        if not address:
            address = await scan_for_device()
            if address:
                save_address(address)
            else:
                log(f"Device not found, retrying in {backoff}s...")
                try:
                    await asyncio.wait_for(stop_event.wait(), timeout=backoff)
                except asyncio.TimeoutError:
                    pass
                backoff = min(backoff * 2, 60)
                continue

        ok = await connect_and_run(address, cfg, stop_event)
        if not ok:
            log("Invalidating cached address")
            SAVED_ADDR_FILE.unlink(missing_ok=True)
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
        else:
            backoff = 1


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
