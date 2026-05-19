#pragma once
#include "data.h"
#include "ble.h"

enum screen_t {
    SCREEN_SPLASH,
    SCREEN_USAGE,
    SCREEN_DETAILS,
    SCREEN_BLUETOOTH,
    SCREEN_POMODORO,
    SCREEN_COUNT,
};

void ui_init(void);
void ui_update(const UsageData* data);
void ui_tick_anim(void);

// Touch-driven swipe detection. Call once per loop with the current
// touch state. Tracks press start, computes deltas, and fires a screen
// switch on a horizontal swipe. Also suppresses the next "click" so the
// splash toggle doesn't fight the swipe.
void ui_touch_tick(bool pressed, int x, int y);

// Pomodoro timer (25-minute focus session). Triggered by a long-press
// on the right side button. Shows a fullscreen arc countdown; when it
// finishes the screen flashes, types "/clear\n" over BLE HID, and
// returns to the previous screen.
void ui_pomodoro_start(void);     // begin a 25-minute session
void ui_pomodoro_cancel(void);    // abort early (PWR button)
void ui_pomodoro_tick(void);      // call once per loop
bool ui_pomodoro_active(void);    // for main.cpp to know whether to suppress other button actions
void ui_show_screen(screen_t screen);
void ui_cycle_screen(void);
void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);
