"""Anthropic Claude provider.

Reuses our existing OAuth flow: minimal `POST /v1/messages` for rate-limit
headers, plus `GET /api/oauth/usage` for the Extra-usage spend + budget.
Token comes from macOS Keychain (`Claude Code-credentials`) on darwin or
`~/.claude/.credentials.json` elsewhere — same as before.
"""
from __future__ import annotations

import getpass
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

import httpx

ID = "claude"

KEYCHAIN_SERVICE = "Claude Code-credentials"
CREDENTIALS_PATH = Path.home() / ".claude" / ".credentials.json"

API_URL         = "https://api.anthropic.com/v1/messages"
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

OAUTH_USAGE_CACHE_SECS = 60
_oauth_usage_cache: tuple[float, dict] | None = None

# Override the OAuth-reported limit (rare; most users leave this unset).
BUDGET_OVERRIDE_USD = os.environ.get("CLAWDMETER_BUDGET_USD")


def _log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] [claude] {msg}", flush=True)


# ── Token lookup ──────────────────────────────────────────────────────────────

def _extract_access_token(blob: str) -> str | None:
    blob = blob.strip()
    if not blob:
        return None
    try:
        data = json.loads(blob)
    except json.JSONDecodeError:
        data = None
    if isinstance(data, dict):
        if isinstance(data.get("accessToken"), str):
            return data["accessToken"]
        for v in data.values():
            if isinstance(v, dict) and isinstance(v.get("accessToken"), str):
                return v["accessToken"]
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
    if m:
        return m.group(1)
    if re.fullmatch(r"[A-Za-z0-9_\-.~+/=]{20,}", blob):
        return blob
    return None


def _read_token_keychain() -> str | None:
    try:
        out = subprocess.run(
            ["security", "find-generic-password",
             "-s", KEYCHAIN_SERVICE, "-a", getpass.getuser(), "-w"],
            check=True, capture_output=True, text=True, timeout=10,
        )
    except (subprocess.CalledProcessError, FileNotFoundError, subprocess.TimeoutExpired) as e:
        _log(f"Keychain access error: {e}")
        return None
    return _extract_access_token(out.stdout)


def _read_token_file() -> str | None:
    try:
        raw = CREDENTIALS_PATH.read_text()
    except OSError:
        return None
    return _extract_access_token(raw)


def read_token() -> str | None:
    if sys.platform == "darwin":
        return _read_token_keychain()
    return _read_token_file()


# ── OAuth usage endpoint (Extra usage / budget) ───────────────────────────────

async def _fetch_oauth_usage(http: httpx.AsyncClient, token: str) -> dict | None:
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
        resp = await http.get(OAUTH_USAGE_URL, headers=headers, timeout=15.0)
    except httpx.HTTPError as e:
        _log(f"oauth/usage call failed: {e}")
        return None
    if resp.status_code in (401, 403):
        _log(f"oauth/usage HTTP {resp.status_code} — token lacks user:profile scope")
        return None
    if resp.status_code >= 400:
        _log(f"oauth/usage HTTP {resp.status_code}: {resp.text[:200]}")
        return None
    try:
        body = resp.json()
    except ValueError:
        _log("oauth/usage returned non-JSON")
        return None
    _oauth_usage_cache = (now, body)
    return body


def _extract_extra_usage(body: dict) -> tuple[float, float, str] | None:
    eu = body.get("extra_usage")
    if not isinstance(eu, dict):
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
        _log(f"extra_usage shape unrecognised: {json.dumps(eu)[:200]}")
        return None

    spend = round(spend_cents / 100.0, 2)
    limit = round(limit_cents / 100.0, 2)
    if BUDGET_OVERRIDE_USD:
        try:
            limit = float(BUDGET_OVERRIDE_USD)
        except ValueError:
            pass
    return (spend, limit, currency)


# ── Public fetch ──────────────────────────────────────────────────────────────

async def fetch(http: httpx.AsyncClient, cfg: dict) -> dict:
    """Return a normalised Claude usage dict, or {ok: False, error: ...}."""
    token = read_token()
    if not token:
        return {"id": ID, "ok": False, "error": "no Claude token"}

    headers = dict(API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"
    try:
        resp = await http.post(API_URL, headers=headers, json=API_BODY, timeout=20.0)
    except httpx.HTTPError as e:
        return {"id": ID, "ok": False, "error": f"API call failed: {e}"}
    if resp.status_code >= 400:
        return {"id": ID, "ok": False,
                "error": f"API HTTP {resp.status_code}: {resp.text[:80]}"}

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

    result = {
        "id":  ID,
        "ok":  True,
        "s":   pct(hdr("anthropic-ratelimit-unified-5h-utilization")),
        "sr":  reset_minutes(hdr("anthropic-ratelimit-unified-5h-reset")),
        "w":   pct(hdr("anthropic-ratelimit-unified-7d-utilization")),
        "wr":  reset_minutes(hdr("anthropic-ratelimit-unified-7d-reset")),
        "st":  hdr("anthropic-ratelimit-unified-5h-status", "unknown"),
    }

    usage_body = await _fetch_oauth_usage(http, token)
    if usage_body is not None:
        eu = _extract_extra_usage(usage_body)
        if eu is not None:
            result["eu"] = round(eu[0], 2)
            result["em"] = round(eu[1], 2)
            result["cu"] = eu[2]
    return result
