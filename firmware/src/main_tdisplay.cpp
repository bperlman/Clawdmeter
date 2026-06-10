// Firmware entry point for LilyGO T-Display S3 (1.9" 170×320 ST7789).
//
// This file replaces main.cpp when CLAWDMETER_BOARD_TDISPLAY_S3 is selected
// in platformio.ini. The T-Display has no touch, no PMU, no IMU, no
// battery — so all rotation, touch, and power code is gone. The BLE,
// splash, and usage-rate modules are reused unchanged.

#include <Arduino.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include "display_cfg_tdisplay.h"
#include "data.h"
#include "ui.h"
#include "ble.h"
#include "splash.h"
#include "usage_rate.h"

// ---- Display objects (declared extern in display_cfg_tdisplay.h) ----
Arduino_DataBus *bus = new Arduino_ESP32LCD8(
    LCD_DC, LCD_CS, LCD_WR, LCD_RD,
    LCD_D0, LCD_D1, LCD_D2, LCD_D3,
    LCD_D4, LCD_D5, LCD_D6, LCD_D7);
// Panel is 170 wide × 320 tall natively. Rotation=1 puts USB-C on the
// right edge in landscape (which puts GPIO 0 on the top edge, GPIO 14 on
// the bottom — that's how we map the buttons). IPS=true. The 35-px column
// offsets center the 170-wide visible window inside the ST7789's 240-wide
// line buffer; Arduino_GFX swaps them automatically for landscape rotation.
Arduino_GFX *gfx = new Arduino_ST7789(
    bus, LCD_RST, 1 /* rotation */, true /* IPS */,
    170, 320,
    35, 0, 35, 0);

static UsageData usage = {};

// ---- LVGL draw buffers (PSRAM-backed, partial render, 40-line strips) ----
#define BUF_LINES 40
static uint16_t *buf1 = nullptr;
static uint16_t *buf2 = nullptr;

static uint32_t my_tick(void) { return millis(); }

static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*)px_map, w, h);
    lv_display_flush_ready(disp);
}

// ---- Serial-screenshot command (same protocol as AMOLED firmware) ----
#define CMD_BUF_SIZE 64
static char cmd_buf[CMD_BUF_SIZE];
static int  cmd_pos = 0;

static void send_screenshot(void) {
    const uint32_t w = LCD_WIDTH, h = LCD_HEIGHT;
    const uint32_t row_bytes = w * 2;
    const uint32_t buf_size = row_bytes * h;
    uint8_t* sbuf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!sbuf) sbuf = (uint8_t*)malloc(buf_size);
    if (!sbuf) { Serial.println("SCREENSHOT_ERR"); return; }

    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_RGB565, row_bytes, sbuf, buf_size);

    lv_result_t res = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &draw_buf);
    if (res != LV_RESULT_OK) {
        free(sbuf);
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    Serial.printf("SCREENSHOT_START %lu %lu %lu\n",
                  (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");

    free(sbuf);
}

static void check_serial_cmd(void) {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            if (strcmp(cmd_buf, "screenshot") == 0) send_screenshot();
            cmd_pos = 0;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

static bool parse_json(const char* json, UsageData* out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) { Serial.printf("JSON parse error: %s\n", err.c_str()); return false; }

    out->session_pct        = doc["s"]  | 0.0f;
    out->session_reset_mins = doc["sr"] | -1;
    out->weekly_pct         = doc["w"]  | 0.0f;
    out->weekly_reset_mins  = doc["wr"] | -1;
    strlcpy(out->status, doc["st"] | "unknown", sizeof(out->status));
    out->ok    = doc["ok"] | false;
    out->valid = true;
    return true;
}

// ============================================================================
// Two-button handler with short/long-press detection.
//
//   Top button (GPIO 0)
//     short tap            → HID Space (Claude Code voice toggle)
//     long  (≥ 600 ms)     → cycle screen
//
//   Bottom button (GPIO 14)
//     short tap            → HID Shift+Tab (Claude Code mode toggle)
//     long  (≥ 600 ms)     → toggle Splash overlay (or next animation if on splash)
//
//   Both held ≥ 2000 ms    → ble_clear_bonds() (forget BLE host pairing)
//
// Both-held suppresses individual long/short events so the user doesn't
// trigger a screen-cycle on the way to the BLE reset.
// ============================================================================

#define LONG_PRESS_MS  600
#define BOTH_RESET_MS  2000
#define HID_KEY_SPACE  0x2C
#define HID_KEY_TAB    0x2B
#define HID_MOD_LSHIFT 0x02

struct Btn {
    uint8_t pin;
    bool    was_down;
    uint32_t down_ms;
    bool    long_fired;
    bool    suppress;        // set when both-held; clears on release
};

static Btn btn_top    = { BTN_TOP,    false, 0, false, false };
static Btn btn_bottom = { BTN_BOTTOM, false, 0, false, false };

static uint32_t both_down_start = 0;
static bool     both_reset_fired = false;

static void send_tap(uint8_t key, uint8_t mod) {
    ble_keyboard_press(key, mod);
    delay(20);
    ble_keyboard_release();
}

static void top_short(void) { send_tap(HID_KEY_SPACE, 0); }
static void top_long(void)  { ui_cycle_screen(); }
static void bot_short(void) { send_tap(HID_KEY_TAB, HID_MOD_LSHIFT); }
static void bot_long(void) {
    if (ui_get_current_screen() == SCREEN_SPLASH) splash_next();
    else                                          ui_toggle_splash();
}

static void poll_button(Btn& b, void (*on_short)(), void (*on_long)()) {
    bool now_down = (digitalRead(b.pin) == LOW);
    uint32_t now  = millis();

    if (now_down && !b.was_down) {
        b.down_ms = now;
        b.long_fired = false;
    } else if (now_down && b.was_down) {
        if (!b.long_fired && !b.suppress && (now - b.down_ms) >= LONG_PRESS_MS) {
            on_long();
            b.long_fired = true;
        }
    } else if (!now_down && b.was_down) {
        if (!b.long_fired && !b.suppress) {
            on_short();
        }
        b.suppress = false;
    }
    b.was_down = now_down;
}

static void poll_buttons(void) {
    bool top    = (digitalRead(BTN_TOP)    == LOW);
    bool bottom = (digitalRead(BTN_BOTTOM) == LOW);
    uint32_t now = millis();

    if (top && bottom) {
        if (both_down_start == 0) both_down_start = now;
        // Suppress individual fire-on-release once both are down.
        btn_top.suppress    = true;
        btn_bottom.suppress = true;
        if (!both_reset_fired && (now - both_down_start) >= BOTH_RESET_MS) {
            Serial.println("buttons: both held — clearing BLE bonds");
            ble_clear_bonds();
            both_reset_fired = true;
        }
    } else {
        both_down_start  = 0;
        both_reset_fired = false;
    }

    poll_button(btn_top,    top_short, top_long);
    poll_button(btn_bottom, bot_short, bot_long);
}

// ============================================================================

void setup(void) {
    Serial.begin(115200);
    delay(300);
    Serial.println("{\"ready\":true}");

    // 1) Power the panel on. The T-Display S3 gates LCD VCC behind GPIO 15
    //    and the backlight behind GPIO 38. Both must be HIGH before the
    //    ST7789 begin() sequence or the screen stays black.
    pinMode(LCD_PWR_EN, OUTPUT);
    digitalWrite(LCD_PWR_EN, HIGH);
    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, HIGH);
    delay(50);

    // 2) Init display.
    if (!gfx->begin()) {
        Serial.println("gfx->begin() failed");
    }
    gfx->fillScreen(0x0000);

    // 3) Init LVGL.
    lv_init();
    lv_tick_set_cb(my_tick);

    buf1 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);
    buf2 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);
    if (!buf1) buf1 = (uint16_t*)malloc(LCD_WIDTH * BUF_LINES * 2);
    if (!buf2) buf2 = (uint16_t*)malloc(LCD_WIDTH * BUF_LINES * 2);

    lv_display_t* disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, LCD_WIDTH * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    // 4) BLE.
    ble_init();

    // 5) Buttons (both pulled up to 3V3, active LOW).
    pinMode(BTN_TOP,    INPUT_PULLUP);
    pinMode(BTN_BOTTOM, INPUT_PULLUP);

    // 6) Build UI and show initial state.
    ui_init();
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());
    ui_show_screen(SCREEN_SPLASH);

    Serial.println("Dashboard ready, waiting for data on BLE...");
}

static ble_state_t last_ble_state = BLE_STATE_INIT;

void loop(void) {
    lv_timer_handler();
    ui_tick_anim();
    ble_tick();
    splash_tick();
    poll_buttons();

    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }

    check_serial_cmd();

    if (ble_has_data()) {
        if (parse_json(ble_get_data(), &usage)) {
            int g_before = usage_rate_group();
            usage_rate_sample(usage.session_pct);
            int g_after = usage_rate_group();
            if (g_after != g_before) {
                Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
                              g_before, g_after, usage.session_pct);
                if (splash_is_active()) splash_pick_for_current_rate();
            }
            ui_update(&usage);
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }

    delay(5);
}
