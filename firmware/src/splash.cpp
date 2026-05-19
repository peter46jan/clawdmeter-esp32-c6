#include "splash.h"
#include "splash_animations.h"
#include "theme.h"
#include "usage_rate.h"
#include <Arduino.h>
#include <string.h>
#include <esp_heap_caps.h>

// The splash is a 20x20 pixel-art animation. The original implementation
// allocated a full 480x480 RGB565 canvas (~460 KB, PSRAM) and CPU-upscaled
// every frame by 24x. That doesn't fit on the C6 (no PSRAM), and is wasteful
// on the S3 too.
//
// We now back the splash with a tiny 20x20 RGB565 image (800 bytes) and let
// LVGL scale it to 480x480 at draw time with antialiasing disabled, which
// gives crisp nearest-neighbour pixels. The same 480x480 output reaches the
// CO5300 via the existing strip-render + rotation path in my_flush_cb.

#define GRID            20
#define DISPLAY_PX      480
// LVGL 9 scale factor: 256 == 1x. We want 24x -> 6144.
#define SPLASH_ZOOM     (256 * (DISPLAY_PX / GRID))

#define COL_EMPTY       0x0000  // true black

LV_FONT_DECLARE(font_styrene_28);

static lv_obj_t      *splash_container = NULL;
static lv_obj_t      *image_obj        = NULL;
static lv_obj_t      *label_status     = NULL;
static uint16_t      *image_buf        = NULL;  // 20x20 RGB565 (internal SRAM)
static lv_image_dsc_t image_dsc;

static uint16_t cur_anim = 0;
static uint16_t cur_frame = 0;
static uint32_t frame_started_ms = 0;
static uint32_t last_pick_ms = 0;
static bool     active = false;

#define SPLASH_ROTATE_INTERVAL_MS 20000

#define GROUP_COUNT 4
#define GROUP_MAX   4
static int8_t  group_lists[GROUP_COUNT][GROUP_MAX];
static uint8_t group_size[GROUP_COUNT] = {0};
static uint8_t group_rotation[GROUP_COUNT] = {0};

static const char* GROUP_NAMES[GROUP_COUNT][GROUP_MAX] = {
    { "expression sleep", "idle breathe", "idle blink", "expression wink" },
    { "idle look around", "work think", "work coding", NULL },
    { "dance sway", "expression surprise", "dance bounce", NULL },
    { "dance bounce dj", "dance sway dj", "dance djmix", NULL },
};

static void resolve_group_lists(void) {
    for (int g = 0; g < GROUP_COUNT; g++) {
        group_size[g] = 0;
        for (int s = 0; s < GROUP_MAX; s++) {
            group_lists[g][s] = -1;
            const char* want = GROUP_NAMES[g][s];
            if (!want) continue;
            for (int i = 0; i < SPLASH_ANIM_COUNT; i++) {
                if (strcmp(splash_anims[i].name, want) == 0) {
                    group_lists[g][group_size[g]++] = (int8_t)i;
                    break;
                }
            }
        }
    }
}

static void render_frame(const uint8_t *cells, const uint16_t *palette) {
    for (int i = 0; i < GRID * GRID; i++) {
        uint8_t code = cells[i];
        image_buf[i] = (palette && code < SPLASH_PALETTE_SIZE) ? palette[code] : COL_EMPTY;
    }
    if (image_obj) lv_obj_invalidate(image_obj);
}

static void show_placeholder() {
    for (int i = 0; i < GRID * GRID; i++) image_buf[i] = COL_EMPTY;
    if (image_obj) lv_obj_invalidate(image_obj);
    if (label_status) lv_obj_clear_flag(label_status, LV_OBJ_FLAG_HIDDEN);
}

void splash_init(lv_obj_t *parent) {
    // 20x20 RGB565 = 800 bytes — comfortably in internal SRAM on either board.
    image_buf = (uint16_t*)heap_caps_malloc(GRID * GRID * 2, MALLOC_CAP_INTERNAL);
    if (!image_buf) {
        Serial.println("splash: failed to alloc image buffer");
        return;
    }
    memset(image_buf, 0, GRID * GRID * 2);

    memset(&image_dsc, 0, sizeof(image_dsc));
    image_dsc.header.magic    = LV_IMAGE_HEADER_MAGIC;
    image_dsc.header.cf       = LV_COLOR_FORMAT_RGB565;
    image_dsc.header.w        = GRID;
    image_dsc.header.h        = GRID;
    image_dsc.header.stride   = GRID * 2;
    image_dsc.data_size       = GRID * GRID * 2;
    image_dsc.data            = (const uint8_t*)image_buf;

    splash_container = lv_obj_create(parent);
    lv_obj_set_size(splash_container, DISPLAY_PX, DISPLAY_PX);
    lv_obj_set_pos(splash_container, 0, 0);
    lv_obj_set_style_bg_color(splash_container, THEME_BG, 0);
    lv_obj_set_style_bg_opa(splash_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(splash_container, 0, 0);
    lv_obj_set_style_pad_all(splash_container, 0, 0);
    lv_obj_clear_flag(splash_container, LV_OBJ_FLAG_SCROLLABLE);

    image_obj = lv_image_create(splash_container);
    lv_image_set_src(image_obj, &image_dsc);
    lv_image_set_antialias(image_obj, false);
    // Anchor scale around top-left so the 20x20 pixels map cleanly onto 480x480.
    lv_image_set_pivot(image_obj, 0, 0);
    lv_image_set_scale(image_obj, SPLASH_ZOOM);
    lv_obj_set_pos(image_obj, 0, 0);

    label_status = lv_label_create(splash_container);
    lv_label_set_text(label_status,
        "no animations loaded\n\n"
        "run tools/scrape_claudepix.js\n"
        "then tools/convert_to_c.js");
    lv_obj_set_style_text_font(label_status, &font_styrene_28, 0);
    lv_obj_set_style_text_color(label_status, lv_color_hex(0xb0aea5), 0);
    lv_obj_set_style_text_align(label_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label_status);

    resolve_group_lists();

    if (SPLASH_ANIM_COUNT == 0) {
        show_placeholder();
    } else {
        lv_obj_add_flag(label_status, LV_OBJ_FLAG_HIDDEN);
        const splash_anim_def_t *a = &splash_anims[0];
        render_frame(a->frames[0], a->palette);
        frame_started_ms = millis();
    }

    lv_obj_add_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
}

void splash_tick(void) {
    if (!active || SPLASH_ANIM_COUNT == 0) return;

    if (millis() - last_pick_ms >= SPLASH_ROTATE_INTERVAL_MS) {
        splash_pick_for_current_rate();
    }

    const splash_anim_def_t *a = &splash_anims[cur_anim];
    if (a->frame_count == 0) return;

    uint16_t hold = a->holds[cur_frame];
    if (millis() - frame_started_ms >= hold) {
        cur_frame = (cur_frame + 1) % a->frame_count;
        frame_started_ms = millis();
        render_frame(a->frames[cur_frame], a->palette);
    }
}

void splash_next(void) {
    if (SPLASH_ANIM_COUNT == 0) return;
    cur_anim = (cur_anim + 1) % SPLASH_ANIM_COUNT;
    cur_frame = 0;
    frame_started_ms = millis();
    last_pick_ms = frame_started_ms;
    const splash_anim_def_t *a = &splash_anims[cur_anim];
    render_frame(a->frames[0], a->palette);
    Serial.printf("splash: -> %s\n", a->name);
}

void splash_pick_for_current_rate(void) {
    if (SPLASH_ANIM_COUNT == 0) return;
    int g = usage_rate_group();
    if (g < 0 || g >= GROUP_COUNT) g = 0;
    if (group_size[g] == 0) return;

    uint8_t slot = group_rotation[g] % group_size[g];
    group_rotation[g]++;
    int8_t idx = group_lists[g][slot];
    if (idx < 0) return;

    cur_anim = (uint16_t)idx;
    cur_frame = 0;
    frame_started_ms = millis();
    last_pick_ms = frame_started_ms;
    const splash_anim_def_t *a = &splash_anims[cur_anim];
    render_frame(a->frames[0], a->palette);
}

bool splash_is_active(void) { return active; }

void splash_show(void) {
    splash_pick_for_current_rate();
    if (splash_container) lv_obj_clear_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
    active = true;
}

void splash_hide(void) {
    if (splash_container) lv_obj_add_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
    active = false;
}

lv_obj_t* splash_get_root(void) {
    return splash_container;
}
