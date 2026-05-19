#pragma once

#include <Arduino_GFX_Library.h>
#include <TouchDrvCSTXXX.hpp>
#include <XPowersLib.h>
#include <SensorQMI8658.hpp>
#include <Wire.h>

// ---- Display resolution ----
#define LCD_WIDTH   480
#define LCD_HEIGHT  480

// ============================================================================
// Pin mapping — selected per-board via build flags (CLAWD_BOARD_S3 / _C6)
// ============================================================================

#if defined(CLAWD_BOARD_S3)

// ---- ESP32-S3 (Waveshare ESP32-S3-Touch-AMOLED-2.16) ----
// QSPI display pins (CO5300)
#define LCD_CS      12
#define LCD_SCLK    38
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_RESET   2

// Touch pins (CST9220 via I2C)
#define IIC_SDA     15
#define IIC_SCL     14
#define TP_INT      11
#define TP_RST      2    // shared with LCD_RESET

// User buttons
#define BTN_LEFT_GPIO   0    // BOOT
#define BTN_RIGHT_GPIO  18

#elif defined(CLAWD_BOARD_C6)

// ---- ESP32-C6 (Waveshare ESP32-C6-Touch-AMOLED-2.16) ----
// Sources:
//   ESP32-C6-Touch-AMOLED-2.16-main/02_Example/XiaoZhi-v2.2.5/main/boards/
//     waveshare/esp32-c6-touch-amoled-2.16/config.h
//   claude-desktop-buddy-esp32-main/src/boards/board_waveshare_esp32c6_touch_amoled_2_16.h
//
// IMPORTANT HARDWARE DIFFERENCES vs S3 2.16 (not just pins!):
//
//  * Display IC is SH8601, NOT CO5300. The Arduino_CO5300 driver does NOT
//    work on this board. Use Arduino_SH8601 + a vendor init sequence,
//    otherwise the panel stays dark. See the buddy repo's display.cpp
//    sh8601_vendor_init() for the exact command list.
//
//  * Per-strip draw16bitRGBBitmap() calls leave the panel BLACK on this
//    board revision — CS toggles between calls confuse SH8601. Workarounds
//    used in the wild:
//      - ESP-IDF esp_lcd panel driver (vendor LVGL demo path), or
//      - Arduino_Canvas with one-shot flush (needs 460KB, won't fit), or
//      - Streamed QSPI push holding CS asserted across all rows.
//    Our existing my_flush_cb in main.cpp does per-strip draws — IT WILL
//    NEED REWRITING for C6.
//
//  * LCD_RESET is NC. Panel reset is via AXP2101 ALDO3 power-cycle.
//    Pass GFX_NOT_DEFINED (-1) to the driver constructor.
//
//  * Touch IC is CST9217 (still CST92xx family, same driver works).
//
//  * Three physical buttons exist, but mapping differs:
//      KEY1 (PWR silkscreen) = GPIO 18, ACTIVE-HIGH (via BSS138 inverter)
//                              — also wired to AXP PWRON
//      KEY2 (IO10 silkscreen) = GPIO 10, active-low
//      BOOT                   = GPIO 9,  active-low
//    Our app's "left" (Space) maps best to BOOT (GPIO 9).
//    "Right" (Shift+Tab) maps to KEY2 (GPIO 10).
//    The middle "cycle screens" action stays on the AXP PWR button (KEY1).
//
//  * No PSRAM — already handled via CLAWD_NO_PSRAM.

// QSPI display pins (SH8601 — note: NOT CO5300)
#define LCD_CS      15
#define LCD_SCLK    0
#define LCD_SDIO0   1
#define LCD_SDIO1   2
#define LCD_SDIO2   3
#define LCD_SDIO3   4
#define LCD_RESET   -1   // NC; reset via AXP ALDO3 power-cycle

// Touch pins (CST9217 via I2C)
#define IIC_SDA     8
#define IIC_SCL     7
#define TP_INT      5
#define TP_RST      11

// User buttons. BOOT is the natural fit for our "left" action.
// KEY2 is on the side; we use it for the "right" action.
#define BTN_LEFT_GPIO       9     // BOOT, active-low
#define BTN_RIGHT_GPIO      10    // KEY2, active-low
#define BTN_THIRD_GPIO      18    // KEY1 (PWR), ACTIVE-HIGH — not currently
                                  // read by main.cpp; PWR press is handled
                                  // via the AXP PMU IRQ path in power.cpp.

#define CLAWD_DISPLAY_SH8601 1    // tells main.cpp to use Arduino_SH8601

#else
#error "Define CLAWD_BOARD_S3 or CLAWD_BOARD_C6 in platformio.ini build_flags"
#endif

// ---- I2C device addresses (same on both boards) ----
#define CST9220_ADDR 0x5A
#define AXP2101_ADDR 0x34

// ---- Global hardware objects (defined in main.cpp) ----
extern Arduino_DataBus *bus;
#if defined(CLAWD_DISPLAY_SH8601)
extern Arduino_SH8601  *gfx;
#else
extern Arduino_CO5300  *gfx;
#endif
extern TouchDrvCST92xx touch;
extern XPowersPMU pmu;
extern SensorQMI8658 imu;
