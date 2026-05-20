#pragma once
#include <Arduino.h>

#define CLAWD_MAX_PROVIDERS    3
#define CLAWD_PROVIDER_ID_LEN  12
#define CLAWD_STATUS_LEN       16
#define CLAWD_CURRENCY_LEN     8

// Per-provider snapshot. The Usage screen rotates through this array.
struct ProviderData {
    char  id[CLAWD_PROVIDER_ID_LEN];   // "claude" / "openai" / "deepseek"
    float session_pct;                  // primary % (0..100)
    int   session_reset_mins;           // -1 if N/A
    float weekly_pct;                   // secondary % (0 if N/A)
    int   weekly_reset_mins;            // -1 if N/A
    char  status[CLAWD_STATUS_LEN];     // "allowed" / "limited" / "unknown"
    float cost_usd;                     // -1 if not provided
    bool  ok;
};

struct UsageData {
    // ── Legacy "primary provider" fields, kept so splash, Details and
    // existing UI code don't have to change. These mirror the Claude
    // provider (if present) — or the first provider when Claude isn't
    // enabled. parse_json() keeps both shapes in sync.
    float session_pct;
    int   session_reset_mins;
    float weekly_pct;
    int   weekly_reset_mins;
    char  status[CLAWD_STATUS_LEN];     // "allowed" / "limited"
    bool  ok;
    bool  valid;
    float extra_usage_amount;
    float extra_budget_amount;
    char  extra_currency[CLAWD_CURRENCY_LEN];

    // ── Multi-provider array (v2 payload).
    ProviderData providers[CLAWD_MAX_PROVIDERS];
    uint8_t      provider_count;
};
