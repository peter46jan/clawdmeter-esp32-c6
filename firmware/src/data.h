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

    // Month-to-date API spend via Anthropic Admin API cost_report.
    // < 0 means "not provided" (no admin key configured).
    float extra_usage_usd;
    float extra_budget_usd;
};
