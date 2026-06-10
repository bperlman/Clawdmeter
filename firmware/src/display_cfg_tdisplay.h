#pragma once

#include <Arduino_GFX_Library.h>

// ============================================================================
// LilyGO T-Display S3 — 1.9" 170×320 ST7789 IPS, 8-bit i80 parallel
//
// We run the panel in landscape (320×170) with rotation = 3. With USB-C on
// the left edge in landscape, GPIO 14 lands on the top edge and GPIO 0 on
// the bottom edge — that's the "Top button / Bottom button" mapping in
// main_tdisplay.cpp.
// ============================================================================

#define LCD_WIDTH   320
#define LCD_HEIGHT  170

// ST7789 controller is 240×320; the 170-wide visible window is centered
// inside the controller's 240-wide line buffer with a 35-pixel offset on
// each side. In landscape rotation this becomes the y-offset.
#define LCD_OFFSET_LANDSCAPE_X  0
#define LCD_OFFSET_LANDSCAPE_Y  35

// ---- Power-on (panel VCC enable). MUST be driven HIGH before bringing the
// ST7789 out of reset, otherwise the panel stays dark.
#define LCD_PWR_EN  15

// ---- Backlight (PWM-capable). HIGH = on.
#define LCD_BL      38

// ---- ST7789 control lines (8-bit i80 parallel via ESP32-S3 LCD_CAM)
#define LCD_RST     5
#define LCD_CS      6
#define LCD_DC      7
#define LCD_WR      8
#define LCD_RD      9   // tie high; not used for write-only operation

// ---- 8-bit parallel data lines
#define LCD_D0      39
#define LCD_D1      40
#define LCD_D2      41
#define LCD_D3      42
#define LCD_D4      45
#define LCD_D5      46
#define LCD_D6      47
#define LCD_D7      48

// ---- Physical buttons (landscape orientation: top/bottom)
#define BTN_TOP     14
#define BTN_BOTTOM  0    // BOOT key

// ---- Global display object (defined in main_tdisplay.cpp)
extern Arduino_DataBus *bus;
extern Arduino_GFX *gfx;
