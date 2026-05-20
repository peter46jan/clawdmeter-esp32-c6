#include "theme.h"
#include <Arduino.h>
#include <Preferences.h>

// Theme variables — initial values are the dark palette so anything
// that reads them before theme_init() runs still gets sensible colours.
lv_color_t THEME_BG;
lv_color_t THEME_PANEL;
lv_color_t THEME_TEXT;
lv_color_t THEME_DIM;
lv_color_t THEME_ACCENT;
lv_color_t THEME_GREEN;
lv_color_t THEME_AMBER;
lv_color_t THEME_RED;
lv_color_t THEME_BAR_BG;

static const char* PREF_NS  = "clawd";
static const char* PREF_KEY = "light";

static void apply_dark(void) {
    THEME_BG     = lv_color_hex(0x000000);
    THEME_PANEL  = lv_color_hex(0x1f1f1e);
    THEME_TEXT   = lv_color_hex(0xfaf9f5);
    THEME_DIM    = lv_color_hex(0xb0aea5);
    THEME_ACCENT = lv_color_hex(0xd97757);
    THEME_GREEN  = lv_color_hex(0x788c5d);
    THEME_AMBER  = lv_color_hex(0xd97757);
    THEME_RED    = lv_color_hex(0xc0392b);
    THEME_BAR_BG = lv_color_hex(0x2a2a28);
}

static void apply_light(void) {
    // Inverted Anthropic-ish palette: warm off-white background, dark
    // ink text. Accent terra-cotta stays the same so the percentage
    // bars keep their identity in both themes.
    THEME_BG     = lv_color_hex(0xfaf9f5);
    THEME_PANEL  = lv_color_hex(0xebe9e0);
    THEME_TEXT   = lv_color_hex(0x1f1f1e);
    THEME_DIM    = lv_color_hex(0x77756e);
    THEME_ACCENT = lv_color_hex(0xd97757);
    THEME_GREEN  = lv_color_hex(0x788c5d);
    THEME_AMBER  = lv_color_hex(0xd97757);
    THEME_RED    = lv_color_hex(0xc0392b);
    THEME_BAR_BG = lv_color_hex(0xd6d3c8);
}

void theme_init(void) {
    Preferences p;
    bool light = false;
    if (p.begin(PREF_NS, /*readOnly=*/true)) {
        light = p.getBool(PREF_KEY, false);
        p.end();
    }
    if (light) apply_light();
    else       apply_dark();
}

void theme_toggle_and_reboot(void) {
    Preferences p;
    if (!p.begin(PREF_NS, /*readOnly=*/false)) {
        Serial.println("theme: NVS open failed, not toggling");
        return;
    }
    bool current = p.getBool(PREF_KEY, false);
    p.putBool(PREF_KEY, !current);
    p.end();
    Serial.printf("theme: switching to %s, rebooting\n", current ? "dark" : "light");
    delay(100);  // let serial flush
    ESP.restart();
}
