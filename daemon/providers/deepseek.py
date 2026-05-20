"""DeepSeek provider.

Pulls `GET /user/balance` and derives spend as `(granted + topped_up) -
total_balance`. No rate-limit concept exposed via the API, so we don't
populate session/weekly windows — just the cost.

Pattern ported from AiMetr's `src/providers/deepseek.js` and
`daemon/multi_provider_daemon.py:282-329`.
"""
from __future__ import annotations

import os
import time

import httpx

ID = "deepseek"
BALANCE_URL = "https://api.deepseek.com/user/balance"


def _log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] [deepseek] {msg}", flush=True)


def _api_key(cfg: dict) -> str | None:
    k = (cfg or {}).get("api_key") or ""
    if isinstance(k, str) and len(k) > 10:
        return k
    env = os.environ.get("DEEPSEEK_API_KEY")
    if env and len(env) > 10:
        return env
    return None


async def fetch(http: httpx.AsyncClient, cfg: dict) -> dict:
    key = _api_key(cfg)
    if not key:
        return {"id": ID, "ok": False, "error": "No DeepSeek API key"}

    try:
        resp = await http.get(
            BALANCE_URL,
            headers={"Authorization": f"Bearer {key}",
                     "Content-Type": "application/json"},
            timeout=10.0,
        )
    except httpx.HTTPError as e:
        return {"id": ID, "ok": False, "error": f"balance call failed: {e}"}

    if resp.status_code == 401:
        return {"id": ID, "ok": False, "error": "Invalid API key"}
    if resp.status_code != 200:
        return {"id": ID, "ok": False, "error": f"HTTP {resp.status_code}"}

    try:
        body = resp.json()
    except ValueError:
        return {"id": ID, "ok": False, "error": "non-JSON response"}

    is_available = body.get("is_available", False)
    infos = body.get("balance_infos", []) or []
    # Prefer USD; fall back to whatever currency the account uses.
    info = next((i for i in infos if i.get("currency") == "USD"), None) \
           or (infos[0] if infos else {})

    def _f(key: str) -> float:
        v = info.get(key, 0)
        try:
            return float(v or 0)
        except (TypeError, ValueError):
            return 0.0

    total    = _f("total_balance")
    granted  = _f("granted_balance")
    topped   = _f("topped_up_balance")

    # Original credit = granted + topped_up if known; else a soft estimate.
    orig = (granted + topped) if (granted + topped) > 0 else (total + 5.0)
    used = max(0.0, orig - total) if orig > 0 else 0.0
    pct  = min(100, int((used / orig) * 100)) if orig > 0 else 0

    return {
        "id":  ID,
        "ok":  bool(is_available),
        "s":   pct,
        "sr":  -1,
        "w":   0,
        "wr":  -1,
        "st":  "allowed" if is_available else "limited",
        "cost": round(used, 4),
    }
