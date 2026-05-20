"""OpenAI / Codex provider.

Polls the OpenAI usage endpoint for today's tokens + estimated cost. The
endpoint isn't part of the public API surface — some org tiers don't
expose it — so on failure we fall back to the rate-limit headers
returned by `GET /v1/models`.

Pattern ported from AiMetr's `src/providers/openai.js` and
`daemon/multi_provider_daemon.py:223-279`.
"""
from __future__ import annotations

import os
import time

import httpx

ID = "openai"

# Default daily organisation token limit when the API doesn't expose one.
# Same heuristic as AiMetr.
ORG_DAILY_LIMIT = 10_000_000

USAGE_URL  = "https://api.openai.com/v1/usage"
MODELS_URL = "https://api.openai.com/v1/models"


def _log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] [openai] {msg}", flush=True)


def _api_key(cfg: dict) -> str | None:
    k = (cfg or {}).get("api_key") or ""
    if isinstance(k, str) and len(k) > 10:
        return k
    env = os.environ.get("OPENAI_API_KEY")
    if env and len(env) > 10:
        return env
    return None


def _seconds_until_utc_midnight() -> int:
    now = time.gmtime()
    secs_today = now.tm_hour * 3600 + now.tm_min * 60 + now.tm_sec
    return max(0, 86400 - secs_today)


def _parse_reset_duration(s: str) -> int:
    """Parse OpenAI's `x-ratelimit-reset-tokens` (e.g. '1m30s') into ms."""
    ms = 0
    import re
    for n, unit in re.findall(r"(\d+)([hms])", s or ""):
        v = int(n)
        if unit == "h": ms += v * 3_600_000
        elif unit == "m": ms += v * 60_000
        else: ms += v * 1_000
    return ms or 60_000


async def _from_usage_api(http: httpx.AsyncClient, key: str) -> dict | None:
    today = time.strftime("%Y-%m-%d", time.gmtime())
    try:
        resp = await http.get(
            USAGE_URL,
            headers={"Authorization": f"Bearer {key}"},
            params={"date": today},
            timeout=10.0,
        )
    except httpx.HTTPError as e:
        _log(f"usage endpoint error: {e}")
        return None
    if resp.status_code != 200:
        return None
    body = resp.json()
    data = body.get("data", []) or []
    total_ctx = sum(d.get("n_context_tokens_total", 0)   for d in data)
    total_gen = sum(d.get("n_generated_tokens_total", 0) for d in data)
    total = total_ctx + total_gen

    # Cost estimate — uses gpt-4o pricing as the AiMetr default ($2.50 in,
    # $10.00 out per 1M tokens). Inaccurate when other models are used,
    # but order-of-magnitude correct.
    cost = (total_ctx / 1e6) * 2.50 + (total_gen / 1e6) * 10.00

    pct = min(100, int(total * 100 / ORG_DAILY_LIMIT)) if ORG_DAILY_LIMIT else 0
    return {
        "id":  ID,
        "ok":  True,
        "s":   pct,
        "sr":  _seconds_until_utc_midnight() // 60,
        "w":   0,
        "wr":  -1,
        "st":  "allowed",
        "cost": round(cost, 4),
    }


async def _from_models_api(http: httpx.AsyncClient, key: str) -> dict | None:
    try:
        resp = await http.get(MODELS_URL,
                              headers={"Authorization": f"Bearer {key}"},
                              timeout=8.0)
    except httpx.HTTPError as e:
        return {"id": ID, "ok": False, "error": f"models endpoint error: {e}"}
    if resp.status_code == 401:
        return {"id": ID, "ok": False, "error": "Invalid API key"}
    if resp.status_code != 200:
        return {"id": ID, "ok": False, "error": f"models HTTP {resp.status_code}"}
    remaining = int(resp.headers.get("x-ratelimit-remaining-tokens", "0") or 0)
    limit     = int(resp.headers.get("x-ratelimit-limit-tokens",     "90000") or 90000)
    reset     = resp.headers.get("x-ratelimit-reset-tokens", "")
    reset_ms  = _parse_reset_duration(reset) if reset else 60_000
    used = max(0, limit - remaining)
    pct  = int(used * 100 / limit) if limit > 0 else 0
    return {
        "id":  ID,
        "ok":  True,
        "s":   pct,
        "sr":  max(0, reset_ms // 60_000),
        "w":   0,
        "wr":  -1,
        "st":  "limited" if pct >= 100 else "allowed",
    }


async def fetch(http: httpx.AsyncClient, cfg: dict) -> dict:
    key = _api_key(cfg)
    if not key:
        return {"id": ID, "ok": False, "error": "No OpenAI API key"}
    result = await _from_usage_api(http, key)
    if result is not None and result.get("ok"):
        return result
    return await _from_models_api(http, key)
