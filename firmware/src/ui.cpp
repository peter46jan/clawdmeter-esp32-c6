#include "ui.h"
#include "splash.h"
#include "ble.h"
#include <lvgl.h>
#include "logo.h"
#include "icons.h"
#include "display_cfg.h"

// Custom fonts (scaled for 314 PPI, ~1.9x from original 165 PPI)
LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_mono_32);

// Anthropic brand palette — design tokens live in theme.h
#include "theme.h"
#define COL_BG        THEME_BG
#define COL_PANEL     THEME_PANEL
#define COL_TEXT      THEME_TEXT
#define COL_DIM       THEME_DIM
#define COL_ACCENT    THEME_ACCENT
#define COL_GREEN     THEME_GREEN
#define COL_AMBER     THEME_AMBER
#define COL_RED       THEME_RED
#define COL_BAR_BG    THEME_BAR_BG

// ---- Layout constants for 480x480 (scaled for 2.16" high-DPI + rounded corners) ----
#define SCR_W         480
#define SCR_H         480
#define MARGIN        20    // wider margin for rounded display corners
#define TITLE_Y       30
#define CONTENT_Y     100
#define CONTENT_W     (SCR_W - 2 * MARGIN)   // 440

// ---- Usage screen widgets ----
static lv_obj_t* usage_container;
static lv_obj_t* lbl_title;
static lv_obj_t* bar_session;
static lv_obj_t* lbl_session_pct;
static lv_obj_t* lbl_session_label;
static lv_obj_t* lbl_session_reset;
static lv_obj_t* bar_weekly;
static lv_obj_t* lbl_weekly_pct;
static lv_obj_t* lbl_weekly_label;
static lv_obj_t* lbl_weekly_reset;
static lv_obj_t* lbl_anim;
static lv_obj_t* lbl_provider_id;  // top-centre tag: CLAUDE / OPENAI / DEEPSEEK

// Multi-provider rotation state.
static UsageData last_usage = {};      // snapshot of the last ui_update
static uint8_t   active_provider_idx = 0;
static uint32_t  last_rotate_ms = 0;
#define PROVIDER_ROTATE_INTERVAL_MS 10000

// ---- Bluetooth screen widgets ----
static lv_obj_t* ble_container;
static lv_obj_t* lbl_ble_status;
static lv_obj_t* lbl_ble_device;
static lv_obj_t* lbl_ble_mac;

// ---- Pomodoro screen widgets ----
static lv_obj_t* pomo_container;
static lv_obj_t* pomo_arc;
static lv_obj_t* pomo_time_label;
static lv_obj_t* pomo_label;        // "Focus" / "Done!"

// Pomodoro state machine. start_ms is millis() at session start; the
// session ends at start_ms + pomo_duration_ms. DONE_HOLD_MS keeps the
// "Done!" message on-screen briefly after typing "/clear\n" so the user
// gets a visual confirmation.
enum pomo_state_t { POMO_IDLE, POMO_RUNNING, POMO_DONE };
static pomo_state_t pomo_state = POMO_IDLE;
static uint32_t     pomo_start_ms    = 0;
static uint32_t     pomo_duration_ms = 25UL * 60UL * 1000UL;
static uint32_t     pomo_done_ms     = 0;
static screen_t     pomo_return_to   = SCREEN_USAGE;

#define POMO_DONE_HOLD_MS  4000UL

// ---- Details screen widgets ----
// Pared-down layout: title + big amount + budget subtitle + bar + percent.
// Uptime / last-update / reset countdowns were removed at user request.
static lv_obj_t* details_container;
static lv_obj_t* lbl_extra_amount;   // big: "12.34"
static lv_obj_t* lbl_extra_currency; // small ISO suffix next to amount: "EUR"
static lv_obj_t* lbl_extra_budget;   // small: "of 50.00"
static lv_obj_t* lbl_extra_pct;      // big: "72%"
static lv_obj_t* bar_extra_usage;

// ---- Battery indicator (shared, on top) ----
static lv_obj_t* battery_img;
static lv_obj_t* logo_img;
static lv_image_dsc_t battery_dscs[5];  // empty, low, medium, full, charging

// ---- Shared ----
static lv_image_dsc_t logo_dsc;
static screen_t current_screen = SCREEN_USAGE;

// Animation state
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;
#define ANIM_MSG_MS     4000

static const char* const spinner_frames[] = {
    "\xC2\xB7", "\xE2\x9C\xBB", "\xE2\x9C\xBD",
    "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2",
};
#define SPINNER_COUNT 6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1))  // 10: ping-pong 0..5..0

// Per-frame hold time. Modeled on Claude Code's spinner (Cavalry triangle
// oscillator, range 0..5, period 5s) — turn-around frames (0 and 5) appear
// once per cycle, middle frames twice, so 0/5 read as held longer.
static const uint16_t spinner_ms[SPINNER_COUNT] = {
    260, 130, 130, 130, 130, 260,
};

static const char* const anim_messages[] = {
    "Accomplishing", "Elucidating", "Perusing",
    "Actioning", "Enchanting", "Philosophising",
    "Actualizing", "Envisioning", "Pondering",
    "Baking", "Finagling", "Pontificating",
    "Booping", "Flibbertigibbeting", "Processing",
    "Brewing", "Forging", "Puttering",
    "Calculating", "Forming", "Puzzling",
    "Cerebrating", "Frolicking", "Reticulating",
    "Channelling", "Generating", "Ruminating",
    "Churning", "Germinating", "Scheming",
    "Clauding", "Hatching", "Schlepping",
    "Coalescing", "Herding", "Shimmying",
    "Cogitating", "Honking", "Shucking",
    "Combobulating", "Hustling", "Simmering",
    "Computing", "Ideating", "Smooshing",
    "Concocting", "Imagining", "Spelunking",
    "Conjuring", "Incubating", "Spinning",
    "Considering", "Inferring", "Stewing",
    "Contemplating", "Jiving", "Sussing",
    "Cooking", "Manifesting", "Synthesizing",
    "Crafting", "Marinating", "Thinking",
    "Creating", "Meandering", "Tinkering",
    "Crunching", "Moseying", "Transmuting",
    "Deciphering", "Mulling", "Unfurling",
    "Deliberating", "Mustering", "Unravelling",
    "Determining", "Musing", "Vibing",
    "Discombobulating", "Noodling", "Wandering",
    "Divining", "Percolating", "Whirring",
    "Doing", "Wibbling",
    "Effecting", "Wizarding",
    "Working", "Wrangling",
};
#define ANIM_MSG_COUNT (sizeof(anim_messages) / sizeof(anim_messages[0]))

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "Resets in %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "Resets in %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

// Forward decls — callbacks defined near ui_show_screen below
static void global_click_cb(lv_event_t* e);
static void ble_reset_click_cb(lv_event_t* e);

// Set when ui_touch_tick recognises a swipe. Causes the click handler to
// ignore the next CLICKED event so the splash-toggle doesn't fight the
// swipe-driven screen switch.
static uint32_t suppress_click_until_ms = 0;

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, 16, 0);
    lv_obj_set_style_pad_right(panel, 16, 0);
    lv_obj_set_style_pad_top(panel, 12, 0);
    lv_obj_set_style_pad_bottom(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    // Bubble click events up to the screen / usage_container so a tap anywhere
    // on the panel fires the global click handler.
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    return panel;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    return bar;
}

static void init_icon_dsc(lv_image_dsc_t* dsc, int w, int h, const uint16_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.stride = w * 2;
    dsc->data = (const uint8_t*)data;
    dsc->data_size = w * h * 2;
}

// RGB565A8: planar — w*h RGB565 pixels followed by w*h alpha bytes.
// Stride is RGB565-only (w*2); LVGL infers alpha plane location from header.
static void init_icon_dsc_rgb565a8(lv_image_dsc_t* dsc, int w, int h, const uint8_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = data;
    dsc->data_size = w * h * 3;
}

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl, 18, 0);
    lv_obj_set_style_pad_right(lbl, 18, 0);
    lv_obj_set_style_pad_top(lbl, 6, 0);
    lv_obj_set_style_pad_bottom(lbl, 6, 0);
    return lbl;
}

// ---- Battery icon initialization ----
static void init_battery_icons(void) {
    init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_data);
    init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_W, ICON_BATTERY_LOW_H, icon_battery_low_data);
    init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_W, ICON_BATTERY_MEDIUM_H, icon_battery_medium_data);
    init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_W, ICON_BATTERY_FULL_H, icon_battery_full_data);
    init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_W, ICON_BATTERY_CHARGING_H, icon_battery_charging_data);
}

// ======== Usage Screen (480x480) ========

#define PANEL_H     150
#define PANEL_GAP   16

// One Session/Weekly panel: big % label, pill on the right, bar, reset label.
// Pill y=1: symmetric inside the panel — panel-outer-top → pill-top equals
// pill-bottom → bar-top (pill height 42 + panel pad_top 12 + bar y=56).
static void make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                             lv_obj_t** out_pct, lv_obj_t** out_pill,
                             lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, MARGIN, y, CONTENT_W, PANEL_H);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, &font_styrene_48, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, 56, CONTENT_W - 32, 24);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, &font_styrene_28, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, 94);
}

static void init_usage_screen(lv_obj_t* scr) {
    usage_container = lv_obj_create(scr);
    lv_obj_set_size(usage_container, SCR_W, SCR_H);
    lv_obj_set_pos(usage_container, 0, 0);
    lv_obj_set_style_bg_opa(usage_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_container, 0, 0);
    lv_obj_set_style_pad_all(usage_container, 0, 0);
    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(usage_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lbl_title = lv_label_create(usage_container);
    lv_label_set_text(lbl_title, "Usage");
    lv_obj_set_style_text_font(lbl_title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 16, TITLE_Y);

    // Provider tag was originally a separate label below the title — it
    // bumped into the first panel. The provider name now goes into the
    // title itself via render_usage_for (e.g. "Claude" / "OpenAI"),
    // re-using the same Tiempos 56 slot. lbl_provider_id is kept as a
    // hidden no-op placeholder so existing references don't NPE.
    lbl_provider_id = lv_label_create(usage_container);
    lv_obj_add_flag(lbl_provider_id, LV_OBJ_FLAG_HIDDEN);

    make_usage_panel(usage_container, CONTENT_Y, "Current",
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset);
    make_usage_panel(usage_container, CONTENT_Y + PANEL_H + PANEL_GAP, "Weekly",
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset);

    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, &font_mono_32, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -15);
}

// ======== Details Screen (480x480) — swipe target from Usage ========
//
// One job: show "Extra usage" — what you've spent this month against the
// monthly limit Anthropic shows in console.anthropic.com.
//
// Layout:
//   title "Extra usage" (Tiempos 56, top centred)
//   amount       (Tiempos 56)  — big number, e.g. "12.34"
//   currency     (Styrene 28)  — ISO code suffix, e.g. "EUR"
//   "of 50.00"   (Styrene 28)  — dim budget reference
//   bar          full-width, taller than the usage screen bars
//   "72%"        (Styrene 48)  — bottom, dim accent

static void init_details_screen(lv_obj_t* scr) {
    details_container = lv_obj_create(scr);
    lv_obj_set_size(details_container, SCR_W, SCR_H);
    lv_obj_set_pos(details_container, 0, 0);
    lv_obj_set_style_bg_opa(details_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(details_container, 0, 0);
    lv_obj_set_style_pad_all(details_container, 0, 0);
    lv_obj_clear_flag(details_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(details_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    // Title
    lv_obj_t* title = lv_label_create(details_container);
    lv_label_set_text(title, "Extra usage");
    lv_obj_set_style_text_font(title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, TITLE_Y);

    // Single content panel takes most of the screen, centered.
    const int PANEL_TOP    = 130;
    const int PANEL_HEIGHT = 320;
    lv_obj_t* p = make_panel(details_container, MARGIN, PANEL_TOP, CONTENT_W, PANEL_HEIGHT);
    lv_obj_set_style_pad_all(p, 24, 0);

    // Big amount, right-aligned to leave room for the currency suffix.
    lbl_extra_amount = lv_label_create(p);
    lv_label_set_text(lbl_extra_amount, "---");
    lv_obj_set_style_text_font(lbl_extra_amount, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(lbl_extra_amount, COL_TEXT, 0);
    lv_obj_align(lbl_extra_amount, LV_ALIGN_TOP_MID, -32, 12);

    // Currency code immediately after the amount, baseline-aligned visually.
    lbl_extra_currency = lv_label_create(p);
    lv_label_set_text(lbl_extra_currency, "");
    lv_obj_set_style_text_font(lbl_extra_currency, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_extra_currency, COL_DIM, 0);
    lv_obj_align_to(lbl_extra_currency, lbl_extra_amount, LV_ALIGN_OUT_RIGHT_BOTTOM, 10, -8);

    // "of 50.00" subtitle, centred under the amount.
    lbl_extra_budget = lv_label_create(p);
    lv_label_set_text(lbl_extra_budget, "");
    lv_obj_set_style_text_font(lbl_extra_budget, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_extra_budget, COL_DIM, 0);
    lv_obj_align(lbl_extra_budget, LV_ALIGN_TOP_MID, 0, 94);

    // Beefy progress bar (taller than the usage-screen bars so it reads
    // as the main thing on this screen).
    bar_extra_usage = make_bar(p, 0, 160, CONTENT_W - 48, 32);

    // Big "72%" label under the bar — same dim-accent treatment as the
    // budget line so the eye lands on the amount first.
    lbl_extra_pct = lv_label_create(p);
    lv_label_set_text(lbl_extra_pct, "---");
    lv_obj_set_style_text_font(lbl_extra_pct, &font_styrene_48, 0);
    lv_obj_set_style_text_color(lbl_extra_pct, COL_DIM, 0);
    lv_obj_align(lbl_extra_pct, LV_ALIGN_TOP_MID, 0, 210);
}

// ======== Pomodoro Screen (480x480) — long-press right to start ========

static void init_pomodoro_screen(lv_obj_t* scr) {
    pomo_container = lv_obj_create(scr);
    lv_obj_set_size(pomo_container, SCR_W, SCR_H);
    lv_obj_set_pos(pomo_container, 0, 0);
    lv_obj_set_style_bg_color(pomo_container, COL_BG, 0);
    lv_obj_set_style_bg_opa(pomo_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pomo_container, 0, 0);
    lv_obj_set_style_pad_all(pomo_container, 0, 0);
    lv_obj_clear_flag(pomo_container, LV_OBJ_FLAG_SCROLLABLE);

    // Big arc — 360 px diameter, centered. Background ring is dim,
    // indicator uses the accent colour. Sweep starts at the top.
    pomo_arc = lv_arc_create(pomo_container);
    lv_obj_set_size(pomo_arc, 360, 360);
    lv_obj_center(pomo_arc);
    lv_arc_set_rotation(pomo_arc, 270);
    lv_arc_set_bg_angles(pomo_arc, 0, 360);
    lv_arc_set_range(pomo_arc, 0, 1000);
    lv_arc_set_value(pomo_arc, 0);
    lv_obj_remove_style(pomo_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(pomo_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(pomo_arc, 18, LV_PART_MAIN);
    lv_obj_set_style_arc_width(pomo_arc, 18, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(pomo_arc, COL_BAR_BG,  LV_PART_MAIN);
    lv_obj_set_style_arc_color(pomo_arc, COL_ACCENT,  LV_PART_INDICATOR);

    // Time remaining "MM:SS" inside the arc.
    pomo_time_label = lv_label_create(pomo_container);
    lv_label_set_text(pomo_time_label, "25:00");
    lv_obj_set_style_text_font(pomo_time_label, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(pomo_time_label, COL_TEXT, 0);
    lv_obj_align(pomo_time_label, LV_ALIGN_CENTER, 0, -8);

    // Subtitle below the time — switches between "Focus" and "Done!".
    pomo_label = lv_label_create(pomo_container);
    lv_label_set_text(pomo_label, "Focus");
    lv_obj_set_style_text_font(pomo_label, &font_styrene_28, 0);
    lv_obj_set_style_text_color(pomo_label, COL_DIM, 0);
    lv_obj_align(pomo_label, LV_ALIGN_CENTER, 0, 56);

    lv_obj_add_flag(pomo_container, LV_OBJ_FLAG_HIDDEN);
}

void ui_pomodoro_start(uint32_t duration_ms) {
    if (pomo_state == POMO_RUNNING) return;  // already going
    pomo_state       = POMO_RUNNING;
    pomo_start_ms    = lv_tick_get();
    pomo_duration_ms = duration_ms;
    pomo_return_to   = (current_screen == SCREEN_SPLASH) ? SCREEN_USAGE : current_screen;
    lv_label_set_text(pomo_label, "Focus");
    lv_obj_set_style_text_color(pomo_label, COL_DIM, 0);
    lv_arc_set_value(pomo_arc, 0);

    // Seed the time label with the initial MM:SS so the screen doesn't
    // show stale text in the first frame before the tick runs.
    uint32_t total_s = (duration_ms + 999) / 1000;
    char buf[8];
    snprintf(buf, sizeof(buf), "%02lu:%02lu",
             (unsigned long)(total_s / 60),
             (unsigned long)(total_s % 60));
    lv_label_set_text(pomo_time_label, buf);

    ui_show_screen(SCREEN_POMODORO);
}

void ui_pomodoro_cancel(void) {
    if (pomo_state == POMO_IDLE) return;
    pomo_state = POMO_IDLE;
    ui_show_screen(pomo_return_to);
}

bool ui_pomodoro_active(void) {
    return pomo_state != POMO_IDLE;
}

void ui_pomodoro_tick(void) {
    if (pomo_state == POMO_IDLE) return;

    uint32_t now = lv_tick_get();

    if (pomo_state == POMO_RUNNING) {
        uint32_t elapsed = now - pomo_start_ms;
        if (elapsed >= pomo_duration_ms) {
            // Session complete. Flash the panel and type "/clear\n" so
            // the user lands in a fresh Claude conversation.
            pomo_state = POMO_DONE;
            pomo_done_ms = now;
            lv_arc_set_value(pomo_arc, 1000);
            lv_label_set_text(pomo_time_label, "00:00");
            lv_label_set_text(pomo_label, "Done!");
            lv_obj_set_style_text_color(pomo_label, COL_ACCENT, 0);
            gfx->setBrightness(255);          // attention flash
            ble_type_string("/clear\n");
            return;
        }
        // Remaining time + arc progress.
        uint32_t remaining_s = (pomo_duration_ms - elapsed + 999) / 1000;
        uint32_t mm = remaining_s / 60;
        uint32_t ss = remaining_s % 60;
        char buf[8];
        snprintf(buf, sizeof(buf), "%02lu:%02lu",
                 (unsigned long)mm, (unsigned long)ss);
        lv_label_set_text(pomo_time_label, buf);
        // Arc fills as time progresses — visually obvious how far in we are.
        uint32_t progress = (uint32_t)((uint64_t)elapsed * 1000 / pomo_duration_ms);
        lv_arc_set_value(pomo_arc, (int32_t)progress);
        return;
    }

    if (pomo_state == POMO_DONE && (now - pomo_done_ms) >= POMO_DONE_HOLD_MS) {
        // Done message has been shown long enough — restore brightness
        // and pop back to the screen the user came from.
        gfx->setBrightness(200);
        pomo_state = POMO_IDLE;
        ui_show_screen(pomo_return_to);
    }
}

// ======== Bluetooth Screen (480x480) ========

static void init_bluetooth_screen(lv_obj_t* scr) {
    ble_container = lv_obj_create(scr);
    lv_obj_set_size(ble_container, SCR_W, SCR_H);
    lv_obj_set_pos(ble_container, 0, 0);
    lv_obj_set_style_bg_opa(ble_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ble_container, 0, 0);
    lv_obj_set_style_pad_all(ble_container, 0, 0);
    lv_obj_clear_flag(ble_container, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t* lbl_ble_title = lv_label_create(ble_container);
    lv_label_set_text(lbl_ble_title, "Bluetooth");
    lv_obj_set_style_text_font(lbl_ble_title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(lbl_ble_title, COL_TEXT, 0);
    lv_obj_align(lbl_ble_title, LV_ALIGN_TOP_MID, 16, TITLE_Y);

    // Info panel (taller for 480x480)
    lv_obj_t* p_info = make_panel(ble_container, MARGIN, CONTENT_Y, CONTENT_W, 160);

    // Bluetooth icon + status row
    static lv_image_dsc_t icon_bt_dsc;
    init_icon_dsc(&icon_bt_dsc, ICON_BLUETOOTH_W, ICON_BLUETOOTH_H, icon_bluetooth_data);

    lv_obj_t* bt_img = lv_image_create(p_info);
    lv_image_set_src(bt_img, &icon_bt_dsc);
    lv_obj_set_pos(bt_img, 0, 0);

    lbl_ble_status = lv_label_create(p_info);
    lv_label_set_text(lbl_ble_status, "Initializing...");
    lv_obj_set_style_text_font(lbl_ble_status, &font_styrene_48, 0);
    lv_obj_set_style_text_color(lbl_ble_status, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_status, 56, 2);

    lbl_ble_device = lv_label_create(p_info);
    lv_label_set_text(lbl_ble_device, "Device: ---");
    lv_obj_set_style_text_font(lbl_ble_device, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_ble_device, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_device, 0, 64);

    lbl_ble_mac = lv_label_create(p_info);
    lv_label_set_text(lbl_ble_mac, "Address: ---");
    lv_obj_set_style_text_font(lbl_ble_mac, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_ble_mac, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_mac, 0, 100);

    // Reset Bluetooth tap zone with trash icon
    int reset_y = CONTENT_Y + 160 + 16;
    lv_obj_t* reset_zone = lv_obj_create(ble_container);
    lv_obj_set_pos(reset_zone, MARGIN, reset_y);
    lv_obj_set_size(reset_zone, CONTENT_W, 110);
    lv_obj_set_style_bg_color(reset_zone, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(reset_zone, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(reset_zone, 8, 0);
    lv_obj_set_style_border_width(reset_zone, 0, 0);
    lv_obj_set_style_pad_column(reset_zone, 14, 0);
    lv_obj_set_flex_flow(reset_zone, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(reset_zone, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(reset_zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(reset_zone, ble_reset_click_cb, LV_EVENT_CLICKED, NULL);

    static lv_image_dsc_t icon_trash_dsc;
    init_icon_dsc(&icon_trash_dsc, ICON_TRASH2_W, ICON_TRASH2_H, icon_trash2_data);
    lv_obj_t* trash_img = lv_image_create(reset_zone);
    lv_image_set_src(trash_img, &icon_trash_dsc);

    lv_obj_t* reset_lbl = lv_label_create(reset_zone);
    lv_label_set_text(reset_lbl, "Reset Bluetooth");
    lv_obj_set_style_text_font(reset_lbl, &font_styrene_28, 0);
    lv_obj_set_style_text_color(reset_lbl, COL_DIM, 0);

    // Attribution
    lv_obj_t* lbl_credit = lv_label_create(ble_container);
    lv_label_set_text(lbl_credit, "Built by @hermannbjorgvin");
    lv_obj_set_style_text_font(lbl_credit, &font_styrene_24, 0);
    lv_obj_set_style_text_color(lbl_credit, COL_DIM, 0);
    lv_obj_align(lbl_credit, LV_ALIGN_BOTTOM_MID, 0, -46);

    lv_obj_t* lbl_credit2 = lv_label_create(ble_container);
    lv_label_set_text(lbl_credit2, "Clawd animation by @amaanbuilds");
    lv_obj_set_style_text_font(lbl_credit2, &font_styrene_20, 0);
    lv_obj_set_style_text_color(lbl_credit2, COL_DIM, 0);
    lv_obj_align(lbl_credit2, LV_ALIGN_BOTTOM_MID, 0, -20);

    // Start hidden
    lv_obj_add_flag(ble_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Public API ========

void ui_init(void) {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Logo (shared, always visible, on top of all containers)
    // Logo is RGB565A8 (planar: w*h RGB565 then w*h alpha) so it composites
    // cleanly against whatever bg is behind it.
    init_icon_dsc_rgb565a8(&logo_dsc, LOGO_WIDTH, LOGO_HEIGHT, logo_data);

    // Initialize battery icon descriptors
    init_battery_icons();

    init_usage_screen(scr);
    init_details_screen(scr);
    init_bluetooth_screen(scr);
    init_pomodoro_screen(scr);
    splash_init(scr);

    // Note: swipe gestures are detected in ui_touch_tick (called from
    // main loop) rather than via LVGL's gesture events — the CST92xx
    // sample rate is too low to reliably trigger LVGL's gesture
    // recogniser, and a missed gesture falls through as a click which
    // toggles the splash.

    // Splash is touch-toggled — tap anywhere on the splash dismisses it
    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    // Logo on top of all containers (inset for rounded corners)
    logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, &logo_dsc);
    lv_obj_set_pos(logo_img, MARGIN, TITLE_Y - 10);

    // Battery indicator on top of all containers (upper-right, inset)
    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    lv_obj_set_pos(battery_img, SCR_W - 48 - MARGIN, TITLE_Y);
}

// Render one provider's session+weekly into the Usage screen widgets.
// Used both by ui_update (initial fill) and ui_tick_provider_rotate.
static void render_usage_for(const ProviderData* p) {
    int s_pct = (int)(p->session_pct + 0.5f);
    lv_label_set_text_fmt(lbl_session_pct, "%d%%", s_pct);
    lv_bar_set_value(bar_session, s_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_session, pct_color(p->session_pct), LV_PART_INDICATOR);

    char buf[48];
    format_reset_time(p->session_reset_mins, buf, sizeof(buf));
    lv_label_set_text(lbl_session_reset, buf);

    int w_pct = (int)(p->weekly_pct + 0.5f);
    if (w_pct > 0 || p->weekly_reset_mins > 0) {
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", w_pct);
        lv_bar_set_value(bar_weekly, w_pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(bar_weekly, pct_color(p->weekly_pct), LV_PART_INDICATOR);
        format_reset_time(p->weekly_reset_mins, buf, sizeof(buf));
        lv_label_set_text(lbl_weekly_reset, buf);
    } else {
        // Provider has no secondary window — show dashes and an empty bar.
        lv_label_set_text(lbl_weekly_pct,   "---%");
        lv_bar_set_value(bar_weekly, 0, LV_ANIM_OFF);
        lv_label_set_text(lbl_weekly_reset, "");
    }

    // Re-skin the screen title with the provider name when rotating
    // between multiple providers; keep the generic "Usage" otherwise.
    if (last_usage.provider_count > 1) {
        char title[16];
        if      (strcmp(p->id, "claude")   == 0) strlcpy(title, "Claude",   sizeof(title));
        else if (strcmp(p->id, "openai")   == 0) strlcpy(title, "OpenAI",   sizeof(title));
        else if (strcmp(p->id, "deepseek") == 0) strlcpy(title, "DeepSeek", sizeof(title));
        else                                      strlcpy(title, p->id,     sizeof(title));
        lv_label_set_text(lbl_title, title);
    } else {
        lv_label_set_text(lbl_title, "Usage");
    }
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 16, TITLE_Y);
}

void ui_update(const UsageData* data) {
    if (!data->valid) return;

    last_usage = *data;       // snapshot for the rotation tick
    last_rotate_ms = lv_tick_get();
    active_provider_idx = 0;

    // Render the primary (Claude when present) by default — same look as
    // before for single-provider setups.
    if (data->provider_count > 0) {
        render_usage_for(&data->providers[0]);
    }

    // Extra usage from /api/oauth/usage. Spend < 0 means the daemon
    // couldn't fetch it (token scope, API change, etc.) — show dashes.
    //
    // We format into stack buffers first and call lv_label_set_text. The
    // LVGL-internal printf path (set_text_fmt) had a crash with "%s" on
    // C6 — see commit history. libc snprintf is fine.
    if (data->extra_budget_amount > 0.0f && data->extra_usage_amount >= 0.0f) {
        float used = data->extra_usage_amount;
        float bud  = data->extra_budget_amount;
        int pct = (int)((used / bud) * 100.0f + 0.5f);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;

        char cur[8];
        strlcpy(cur, data->extra_currency, sizeof(cur));

        char amount[16];
        snprintf(amount, sizeof(amount), "%.2f", used);
        lv_label_set_text(lbl_extra_amount, amount);

        lv_label_set_text(lbl_extra_currency, cur);

        char budline[32];
        snprintf(budline, sizeof(budline), "of %.2f", bud);
        lv_label_set_text(lbl_extra_budget, budline);

        char pctbuf[8];
        snprintf(pctbuf, sizeof(pctbuf), "%d%%", pct);
        lv_label_set_text(lbl_extra_pct, pctbuf);

        lv_bar_set_value(bar_extra_usage, pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(bar_extra_usage, pct_color((float)pct), LV_PART_INDICATOR);

        // Re-centre the amount + currency pair after the text width changed.
        lv_obj_update_layout(lbl_extra_amount);
        lv_obj_align_to(lbl_extra_currency, lbl_extra_amount,
                        LV_ALIGN_OUT_RIGHT_BOTTOM, 10, -8);
    } else {
        lv_label_set_text(lbl_extra_amount, "---");
        lv_label_set_text(lbl_extra_currency, "");
        lv_label_set_text(lbl_extra_budget, "no data yet");
        lv_label_set_text(lbl_extra_pct, "");
        lv_bar_set_value(bar_extra_usage, 0, LV_ANIM_OFF);
    }
}

void ui_tick_provider_rotate(void) {
    if (current_screen != SCREEN_USAGE) return;
    if (last_usage.provider_count <= 1) return;
    uint32_t now = lv_tick_get();
    if (now - last_rotate_ms < PROVIDER_ROTATE_INTERVAL_MS) return;

    // Pick the next provider in array order that reported ok.
    for (uint8_t step = 1; step <= last_usage.provider_count; step++) {
        uint8_t next = (active_provider_idx + step) % last_usage.provider_count;
        if (last_usage.providers[next].ok) {
            active_provider_idx = next;
            last_rotate_ms = now;
            render_usage_for(&last_usage.providers[next]);
            return;
        }
    }
    last_rotate_ms = now;   // nothing to rotate to — try again next interval
}

void ui_tick_anim(void) {
    if (current_screen != SCREEN_USAGE) return;

    uint32_t now = lv_tick_get();

    if (now - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = now;
    }

    if (now - anim_last_ms >= spinner_ms[anim_spinner_idx]) {
        anim_last_ms = now;
        anim_phase = (anim_phase + 1) % SPINNER_PHASES;
        anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                        : (SPINNER_PHASES - anim_phase);

        static char buf[80];
        snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
                 spinner_frames[anim_spinner_idx],
                 anim_messages[anim_msg_idx]);
        lv_label_set_text(lbl_anim, buf);
    }
}

static screen_t prev_non_splash_screen = SCREEN_USAGE;
// Hide the battery indicator on the splash and pomodoro screens — the
// icon is visually noisy over the pixel-art animations and competes with
// the timer arc for attention.
static void apply_battery_visibility(void) {
    if (!battery_img) return;
    if (current_screen == SCREEN_SPLASH || current_screen == SCREEN_POMODORO)
        lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
}

// LVGL handles click debouncing internally. Screen-level handler fires when
// no child consumed the event (children only consume if they have their own
// event callback, e.g. the Reset Bluetooth zone). On BT screen we skip the
// splash toggle so only the reset zone is interactive there.
static void global_click_cb(lv_event_t* e) {
    (void)e;
    if (ui_get_current_screen() == SCREEN_BLUETOOTH) return;
    // A swipe just happened — eat the spurious tap so we don't immediately
    // bounce to the splash.
    if ((int32_t)(lv_tick_get() - suppress_click_until_ms) < 0) return;
    ui_toggle_splash();
}

void ui_touch_tick(bool pressed, int x, int y) {
    static bool was_pressed = false;
    static int start_x = 0, start_y = 0;
    static int last_x = 0, last_y = 0;
    static uint32_t start_ms = 0;
    static bool swipe_fired = false;

    uint32_t now = lv_tick_get();

    if (pressed && !was_pressed) {
        // Press start
        start_x = last_x = x;
        start_y = last_y = y;
        start_ms = now;
        swipe_fired = false;
    } else if (pressed) {
        // Drag — check live for a horizontal swipe so we can fire as soon
        // as the threshold is crossed (don't wait for release).
        last_x = x;
        last_y = y;
        if (!swipe_fired) {
            int dx = x - start_x;
            int dy = y - start_y;
            if (abs(dx) > 70 && abs(dy) < 80 && (now - start_ms) < 1000) {
                screen_t cur = current_screen;
                if (dx < 0 && cur == SCREEN_USAGE) {
                    ui_show_screen(SCREEN_DETAILS);
                    swipe_fired = true;
                } else if (dx > 0 && cur == SCREEN_DETAILS) {
                    ui_show_screen(SCREEN_USAGE);
                    swipe_fired = true;
                }
                if (swipe_fired) suppress_click_until_ms = now + 400;
            }
        }
    } else if (!pressed && was_pressed && swipe_fired) {
        // Release after a swipe — keep the click-suppress window alive a
        // bit longer in case LVGL queues the release event late.
        suppress_click_until_ms = now + 400;
    }
    was_pressed = pressed;
}

static void ble_reset_click_cb(lv_event_t* e) {
    (void)e;
    ble_clear_bonds();
}

void ui_show_screen(screen_t screen) {
    lv_obj_add_flag(usage_container,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(details_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ble_container,     LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(pomo_container,    LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:     splash_show(); break;
    case SCREEN_USAGE:      lv_obj_clear_flag(usage_container,   LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_DETAILS:    lv_obj_clear_flag(details_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_BLUETOOTH:  lv_obj_clear_flag(ble_container,     LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_POMODORO:   lv_obj_clear_flag(pomo_container,    LV_OBJ_FLAG_HIDDEN); break;
    default: break;
    }

    // Hide the logo overlay on the splash and pomodoro screens.
    if (logo_img) {
        if (screen == SCREEN_SPLASH || screen == SCREEN_POMODORO)
            lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    }

    // Don't memoise SPLASH or POMODORO as the "previous" screen — those
    // are transient overlays. PWR / splash-toggle should always return to
    // the regular Usage/Details/Bluetooth flow.
    if (screen != SCREEN_SPLASH && screen != SCREEN_POMODORO)
        prev_non_splash_screen = screen;
    current_screen = screen;
    apply_battery_visibility();
}

void ui_cycle_screen(void) {
    // PWR button: cycles between Usage/Details and Bluetooth, preserving
    // which "data" screen the user last viewed.
    screen_t next;
    if (current_screen == SCREEN_BLUETOOTH) {
        next = (prev_non_splash_screen == SCREEN_DETAILS) ? SCREEN_DETAILS : SCREEN_USAGE;
    } else {
        next = SCREEN_BLUETOOTH;
    }
    ui_show_screen(next);
}

void ui_toggle_splash(void) {
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    switch (state) {
    case BLE_STATE_CONNECTED:
        lv_label_set_text(lbl_ble_status, "Connected");
        lv_obj_set_style_text_color(lbl_ble_status, COL_GREEN, 0);
        break;
    case BLE_STATE_ADVERTISING:
        lv_label_set_text(lbl_ble_status, "Advertising...");
        lv_obj_set_style_text_color(lbl_ble_status, COL_AMBER, 0);
        break;
    case BLE_STATE_DISCONNECTED:
        lv_label_set_text(lbl_ble_status, "Disconnected");
        lv_obj_set_style_text_color(lbl_ble_status, COL_RED, 0);
        break;
    default:
        lv_label_set_text(lbl_ble_status, "Initializing...");
        lv_obj_set_style_text_color(lbl_ble_status, COL_DIM, 0);
        break;
    }

    if (name) {
        static char nbuf[48];
        snprintf(nbuf, sizeof(nbuf), "Device: %s", name);
        lv_label_set_text(lbl_ble_device, nbuf);
    }
    if (mac) {
        static char mbuf[48];
        snprintf(mbuf, sizeof(mbuf), "Address: %s", mac);
        lv_label_set_text(lbl_ble_mac, mbuf);
    }
}

void ui_update_battery(int percent, bool charging) {
    int idx;
    if (charging) {
        idx = 4;  // charging icon
    } else if (percent < 0) {
        idx = 0;  // no battery / unknown
    } else if (percent <= 10) {
        idx = 0;  // empty
    } else if (percent <= 35) {
        idx = 1;  // low
    } else if (percent <= 75) {
        idx = 2;  // medium
    } else {
        idx = 3;  // full
    }
    lv_image_set_src(battery_img, &battery_dscs[idx]);
    apply_battery_visibility();
}
