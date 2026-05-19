#pragma once
#include <Arduino.h>

struct UsageData {
    float session_pct;       // 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until session resets
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets
    char status[16];         // "allowed" or "limited"
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse

    // Extra-usage spend / monthly limit from /api/oauth/usage.
    // Negative spend means "not provided" (token lacks scope, API down,
    // etc.) — UI shows dashes in that case. Currency is the 3-letter
    // code (e.g. "EUR", "USD") returned by Anthropic. Display as a
    // suffix because the bundled fonts only cover ASCII glyphs.
    float extra_usage_amount;
    float extra_budget_amount;
    char  extra_currency[8];
};
