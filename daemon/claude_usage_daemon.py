#!/usr/bin/env python3
"""Claude Usage Tracker Daemon (BLE) — macOS port of claude-usage-daemon.sh.

Polls Claude API rate-limit headers and writes a JSON payload to the
ESP32 "Claude Controller" peripheral over a custom GATT service. Uses
bleak (CoreBluetooth backend on macOS).
"""

import asyncio
import getpass
import json
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

import httpx
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

DEVICE_NAME = "Claude Controller"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"

POLL_INTERVAL = 60
TICK = 5
SCAN_TIMEOUT = 8.0

# macOS: token lives in Keychain (service "Claude Code-credentials").
# Linux: token lives in ~/.claude/.credentials.json.
KEYCHAIN_SERVICE = "Claude Code-credentials"
CREDENTIALS_PATH = Path.home() / ".claude" / ".credentials.json"
SAVED_ADDR_FILE = Path.home() / ".config" / "claude-usage-monitor" / "ble-address"

# Override the OAuth-reported Extra-usage limit (USD). Most users leave
# this unset — the value from the API is what claude.ai shows.
BUDGET_OVERRIDE_USD = os.environ.get("CLAWDMETER_BUDGET_USD")
# OAuth usage endpoint isn't free to call — cache.
OAUTH_USAGE_CACHE_SECS = 60

API_URL = "https://api.anthropic.com/v1/messages"
OAUTH_USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
API_HEADERS_TEMPLATE = {
    "anthropic-version": "2023-06-01",
    "anthropic-beta": "oauth-2025-04-20",
    "Content-Type": "application/json",
    "User-Agent": "claude-code/2.1.5",
}
API_BODY = {
    "model": "claude-haiku-4-5-20251001",
    "max_tokens": 1,
    "messages": [{"role": "user", "content": "hi"}],
}

# (timestamp, payload dict) — populated by fetch_oauth_usage.
_oauth_usage_cache: tuple[float, dict] | None = None


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def _extract_access_token(blob: str) -> str | None:
    """Pull the accessToken out of a credentials blob.

    Claude Code stores credentials as a JSON object; the blob may also be
    nested ({"claudeAiOauth": {"accessToken": "..."}}). Fall back to a
    regex match so unexpected shapes still work, and finally treat the
    blob as a raw token if nothing else matches.
    """
    blob = blob.strip()
    if not blob:
        return None
    try:
        data = json.loads(blob)
    except json.JSONDecodeError:
        data = None
    if isinstance(data, dict):
        # direct: {"accessToken": "..."}
        if isinstance(data.get("accessToken"), str):
            return data["accessToken"]
        # nested: {"claudeAiOauth": {"accessToken": "..."}}
        for v in data.values():
            if isinstance(v, dict) and isinstance(v.get("accessToken"), str):
                return v["accessToken"]
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
    if m:
        return m.group(1)
    # Raw token (no JSON wrapper) — must look plausible (sk-ant-... etc.)
    if re.fullmatch(r"[A-Za-z0-9_\-.~+/=]{20,}", blob):
        return blob
    return None


def _read_token_keychain() -> str | None:
    try:
        out = subprocess.run(
            [
                "security",
                "find-generic-password",
                "-s",
                KEYCHAIN_SERVICE,
                "-a",
                getpass.getuser(),
                "-w",
            ],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except subprocess.CalledProcessError as e:
        log(f"Keychain read failed (rc={e.returncode}): {e.stderr.strip()}")
        return None
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        log(f"Keychain access error: {e}")
        return None
    return _extract_access_token(out.stdout)


def _read_token_file() -> str | None:
    try:
        raw = CREDENTIALS_PATH.read_text()
    except OSError as e:
        log(f"Error reading credentials: {e}")
        return None
    return _extract_access_token(raw)


def read_token() -> str | None:
    if sys.platform == "darwin":
        return _read_token_keychain()
    return _read_token_file()


def load_cached_address() -> str | None:
    if not SAVED_ADDR_FILE.exists():
        return None
    addr = SAVED_ADDR_FILE.read_text().strip()
    # Accept both Linux MAC (AA:BB:CC:DD:EE:FF) and macOS CoreBluetooth UUID
    # (E621E1F8-C36C-495A-93FC-0C247A3E6E5F).
    if re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", addr) or re.fullmatch(
        r"[0-9A-Fa-f]{8}-(?:[0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}", addr
    ):
        return addr
    log("Cached address malformed, discarding")
    SAVED_ADDR_FILE.unlink(missing_ok=True)
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


async def fetch_oauth_usage(token: str) -> dict | None:
    """Query the OAuth usage endpoint for windowed utilization + extra usage.

    Uses the same OAuth Bearer token Claude Code already stores. The
    endpoint requires the `user:profile` scope on the token — CLI tokens
    that only have `user:inference` will 401/403 here. Result is cached
    briefly so the daemon doesn't hammer the API.

    Response shape (defensive parse — Anthropic has shifted field names
    between betas):
        {
          "five_hour":  { "utilization": 0.42, "resets_at": "..." },
          "seven_day":  { ... },
          "extra_usage": {
              "spend"|"amount"|"used":  35.97,
              "limit"|"max"|"cap":      50.00,
              ...
          }
        }
    """
    global _oauth_usage_cache
    now = time.time()
    if _oauth_usage_cache and (now - _oauth_usage_cache[0]) < OAUTH_USAGE_CACHE_SECS:
        return _oauth_usage_cache[1]

    headers = {
        "Authorization": f"Bearer {token}",
        "anthropic-beta": "oauth-2025-04-20",
        "anthropic-version": "2023-06-01",
    }
    try:
        async with httpx.AsyncClient(timeout=15.0) as http:
            resp = await http.get(OAUTH_USAGE_URL, headers=headers)
    except httpx.HTTPError as e:
        log(f"oauth/usage call failed: {e}")
        return None

    if resp.status_code == 401 or resp.status_code == 403:
        log(f"oauth/usage HTTP {resp.status_code} — token likely lacks user:profile scope")
        return None
    if resp.status_code >= 400:
        log(f"oauth/usage HTTP {resp.status_code}: {resp.text[:200]}")
        return None

    try:
        body = resp.json()
    except ValueError:
        log("oauth/usage returned non-JSON")
        return None

    # Diagnostic: log top-level keys once per cache window so we know what
    # the endpoint actually returns. The shape is undocumented and has
    # shifted between betas.
    if not _oauth_usage_cache:
        log(f"oauth/usage top-level keys: {sorted(body.keys()) if isinstance(body, dict) else type(body).__name__}")

    _oauth_usage_cache = (now, body)
    return body


def _extract_extra_usage(body: dict) -> tuple[float, float, str] | None:
    """Pull (spend, limit, currency_code) out of an oauth/usage payload.

    Observed shape (May 2026, claude.ai EU):
        "extra_usage": {
            "is_enabled": true,
            "monthly_limit": 5000,      // cents in local currency
            "used_credits": 3597.0,     // cents
            "utilization": 71.94,
            "currency": "EUR",
            "disabled_reason": null
        }

    Anthropic has shipped multiple naming variants in beta, so we look at
    a small list of plausible field names. Amounts are always interpreted
    as cents → divided by 100.
    """
    eu = body.get("extra_usage")
    if eu is None:
        log("oauth/usage: no extra_usage field in response")
        return None
    if not isinstance(eu, dict):
        log(f"oauth/usage: extra_usage is {type(eu).__name__}, not object: {eu!r}")
        return None

    def _num(*keys: str) -> float | None:
        for k in keys:
            v = eu.get(k)
            if isinstance(v, (int, float)):
                return float(v)
            if isinstance(v, str):
                try:
                    return float(v)
                except ValueError:
                    pass
        return None

    spend_cents = _num("used_credits", "spend", "amount", "used", "spent", "current_usage", "cost")
    limit_cents = _num("monthly_limit", "limit", "max", "cap", "spend_limit", "budget")
    currency = eu.get("currency") if isinstance(eu.get("currency"), str) else "USD"

    if spend_cents is None or limit_cents is None:
        log(f"extra_usage shape unrecognised: {json.dumps(eu)[:200]}")
        return None

    spend = round(spend_cents / 100.0, 2)
    limit = round(limit_cents / 100.0, 2)

    if BUDGET_OVERRIDE_USD:
        try:
            limit = float(BUDGET_OVERRIDE_USD)
        except ValueError:
            pass

    return (spend, limit, currency)


async def poll_api(token: str) -> dict | None:
    headers = dict(API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.post(API_URL, headers=headers, json=API_BODY)
    except httpx.HTTPError as e:
        log(f"API call failed: {e}")
        return None
    if resp.status_code >= 400:
        log(f"API HTTP {resp.status_code}: {resp.text[:200]}")
        return None

    def hdr(name: str, default: str = "0") -> str:
        return resp.headers.get(name, default)

    now = time.time()

    def reset_minutes(reset_ts: str) -> int:
        try:
            r = float(reset_ts)
        except ValueError:
            return 0
        mins = (r - now) / 60.0
        return int(round(mins)) if mins > 0 else 0

    def pct(util: str) -> int:
        try:
            return int(round(float(util) * 100))
        except ValueError:
            return 0

    payload = {
        "s": pct(hdr("anthropic-ratelimit-unified-5h-utilization")),
        "sr": reset_minutes(hdr("anthropic-ratelimit-unified-5h-reset")),
        "w": pct(hdr("anthropic-ratelimit-unified-7d-utilization")),
        "wr": reset_minutes(hdr("anthropic-ratelimit-unified-7d-reset")),
        "st": hdr("anthropic-ratelimit-unified-5h-status", "unknown"),
        "ok": True,
    }

    # Optional: extra-usage spend + limit via the OAuth usage endpoint
    # (same Bearer token, no separate admin key needed). Gracefully
    # omitted if the token lacks the user:profile scope or the field
    # shape changes — firmware treats missing fields as "no data".
    usage_body = await fetch_oauth_usage(token)
    if usage_body is not None:
        eu = _extract_extra_usage(usage_body)
        if eu is not None:
            payload["eu"] = round(eu[0], 2)
            payload["em"] = round(eu[1], 2)
            payload["cu"] = eu[2]   # currency code, e.g. "EUR" / "USD"

    return payload


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


async def connect_and_run(address: str, stop_event: asyncio.Event) -> bool:
    """Connect to a known address and poll until disconnected or stopped.

    Returns True if the connection was used successfully (so the caller
    keeps the cached address), False if the connection failed and the
    cache should be invalidated.
    """
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

    last_poll = 0.0
    used_successfully = False
    try:
        while client.is_connected and not stop_event.is_set():
            now = time.time()
            elapsed = now - last_poll
            if session.refresh_requested.is_set() or elapsed >= POLL_INTERVAL:
                session.refresh_requested.clear()
                token = read_token()
                if not token:
                    log("No token; skipping poll")
                else:
                    payload = await poll_api(token)
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

    log("=== Claude Usage Tracker Daemon (BLE, macOS) ===")
    log(f"Poll interval: {POLL_INTERVAL}s")

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

        ok = await connect_and_run(address, stop_event)
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
