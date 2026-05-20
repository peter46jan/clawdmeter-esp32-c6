"""Multi-provider polling for Clawdmeter.

Each provider module exports an async `fetch(cfg, http)` coroutine that
returns a normalised dict (or `None` when disabled / unconfigured). The
main daemon polls all enabled providers in parallel and packs them into
a single BLE payload.

Normalised result shape:
    {
        "id":  "claude" | "openai" | "deepseek",
        "ok":  bool,
        "s":   int,   # primary % used (0..100, session/today)
        "sr":  int,   # primary reset in minutes (-1 if N/A)
        "w":   int,   # secondary % used (0 if N/A)
        "wr":  int,   # secondary reset minutes (-1 if N/A)
        "st":  str,   # "allowed" / "limited" / "unknown"
        # Optional, only present when the provider has them:
        "eu":  float, # extra-usage amount (currency units, Claude only)
        "em":  float, # budget (Claude only)
        "cu":  str,   # ISO currency code (Claude only)
        "cost": float, # USD spent today / month-to-date
    }

On failure: {"id": "...", "ok": False, "error": "..."}.
"""

from . import claude, openai, deepseek

# Registration order is also the on-device rotation order.
PROVIDERS = [claude, openai, deepseek]
