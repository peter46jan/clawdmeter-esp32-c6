#pragma once
#include <stdint.h>
#include <lvgl.h>

// Initialize splash module. Creates a 20x20 RGB565 LVGL image inside
// `parent` and scales it 24x to fill 480x480 (no PSRAM required).
void splash_init(lv_obj_t *parent);

// Advance animation frame if hold time elapsed. Call from main loop.
void splash_tick(void);

// Cycle to the next animation in the catalog.
void splash_next(void);

// Show/hide the splash container.
void splash_show(void);
void splash_hide(void);

// Pick the next animation matching the current usage-rate group.
// Called automatically by splash_show(); also exposed so other modules can
// trigger a re-pick when the rate group changes mid-display.
void splash_pick_for_current_rate(void);

// True when splash is currently rendering (used to gate re-picks).
bool splash_is_active(void);

// Tell the splash module the current Extra-usage spend percentage so it
// can pick mood-matching animations once spend climbs past 50% (focus),
// 80% (worried), or 95% (panic). Pass -1 when no spend data is available
// to fall back to the rate-driven selection.
void splash_set_spend_pct(int pct);

// Root container (so ui.cpp can attach a click event).
lv_obj_t* splash_get_root(void);
