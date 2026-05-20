#pragma once
#include <lvgl.h>

// Design tokens — single source of truth for UI colors. Dark and light
// variants of the Anthropic-inspired palette. The active palette is
// chosen at boot from a persisted preference (theme.cpp).
//
// THEME_X are variables (not #defines) so they can be set at boot. LVGL
// stores style values at set-style time, so changing these at runtime
// won't recolour existing widgets — switching themes does an esp_restart().
extern lv_color_t THEME_BG;
extern lv_color_t THEME_PANEL;
extern lv_color_t THEME_TEXT;
extern lv_color_t THEME_DIM;
extern lv_color_t THEME_ACCENT;
extern lv_color_t THEME_GREEN;
extern lv_color_t THEME_AMBER;
extern lv_color_t THEME_RED;
extern lv_color_t THEME_BAR_BG;

// Read the persisted theme choice from NVS and populate the THEME_X
// globals. MUST be called from setup() before ui_init().
void theme_init(void);

// Flip the persisted theme choice and reboot so the new palette
// applies cleanly to every widget.
void theme_toggle_and_reboot(void);
