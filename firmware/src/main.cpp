#include <Arduino.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include "display_cfg.h"
#include "data.h"
#include "ui.h"
#include "power.h"
#include "imu.h"
#include "splash.h"
#include "usage_rate.h"
#include "sound.h"

// ---- Hardware objects ----
// ST7796 over standard 4-wire SPI; the driver does HW rotation, so passing
// LCD_ROTATION here makes gfx report 480x320 landscape directly (no Canvas).
Arduino_DataBus *bus = new Arduino_ESP32SPI(
    LCD_SPI_DC, LCD_SPI_CS, LCD_SPI_SCK, LCD_SPI_MOSI, LCD_SPI_MISO);
Arduino_GFX *gfx = new Arduino_ST7796(
    bus, LCD_RST, LCD_ROTATION, true /* IPS */, PANEL_W, PANEL_H);
TouchDrvFT6X36 touch;
TCA9554 tca(TCA9554_ADDR);
XPowersPMU pmu;
SensorQMI8658 imu;

static UsageData usage = {};

// ---- Touch shared state (FT6336, polled once per loop; no INT line used) ----
static bool     touch_pressed = false;
static uint16_t touch_x = 0;
static uint16_t touch_y = 0;
static bool     touch_ok = false;

static void touch_read() {
    if (!touch_ok) return;
    int16_t tx[1], ty[1];
    if (touch.getPoint(tx, ty, 1) > 0) {
        touch_pressed = true;
        touch_x = (uint16_t)tx[0];
        touch_y = (uint16_t)ty[0];
    } else {
        touch_pressed = false;
    }
}

// ---- LVGL draw buffers (PSRAM-backed, partial render) ----
#define BUF_LINES 40
static uint16_t *buf1 = nullptr;
static uint16_t *buf2 = nullptr;

// LVGL tick callback
static uint32_t my_tick(void) {
    return millis();
}

// LVGL flush callback — ST7796 driver writes the strip straight to the panel
// over SPI (no Canvas/framebuffer needed; the driver owns rotation).
static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*)px_map, w, h);
    lv_display_flush_ready(disp);
}

// LVGL touch callback
static void my_touch_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    if (touch_pressed) {
        data->point.x = touch_x;
        data->point.y = touch_y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// Parse a JSON line into UsageData
static bool parse_json(const char* json, UsageData* out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }

    out->session_pct = doc["s"] | 0.0f;
    out->session_reset_mins = doc["sr"] | -1;
    out->weekly_pct = doc["w"] | 0.0f;
    out->weekly_reset_mins = doc["wr"] | -1;
    strlcpy(out->status, doc["st"] | "unknown", sizeof(out->status));
    out->ok = doc["ok"] | false;
    out->valid = true;
    return true;
}

// Parse + apply a usage JSON payload (pushed every ~60s over USB serial).
// Silent by design — routine polls must NOT beep. Only plays an alert on a
// *transition* into a not-OK rate-limit status (a real, infrequent event).
static bool apply_usage_json(const char* json) {
    if (!parse_json(json, &usage)) return false;
    int g_before = usage_rate_group();
    usage_rate_sample(usage.session_pct);
    int g_after = usage_rate_group();
    if (g_after != g_before) {
        Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
            g_before, g_after, usage.session_pct);
        if (splash_is_active()) splash_pick_for_current_rate();
    }
    ui_update(&usage);

    bool ok_now = usage.ok && (strcmp(usage.status, "allowed") == 0 ||
                               strcmp(usage.status, "ok") == 0 ||
                               strcmp(usage.status, "unknown") == 0);
    static bool was_ok = true;
    if (was_ok && !ok_now) sound_alert();   // only on the OK -> not-OK edge
    was_ok = ok_now;
    return true;
}

// Handle a Claude-session event line: {"ev":"task-complete"} etc.
// Mirrors the game-sounds plugin's 5 categories — distinct sound + a
// brief colored screen banner. Returns false if `ev` is missing.
static bool handle_event_json(const char* json) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;
    const char* ev = doc["ev"] | (const char*)nullptr;
    if (!ev) return false;

    const char* label;
    unsigned long color;
    if (strcmp(ev, "session-start") == 0)       { label = "Session started"; color = 0x788c5d; }
    else if (strcmp(ev, "task-acknowledge") == 0){ label = "Working..."; color = 0x4a6b8a; }
    else if (strcmp(ev, "task-complete") == 0)   { label = "Done";            color = 0x788c5d; }
    else if (strcmp(ev, "error") == 0)           { label = "Error";           color = 0xc0392b; }
    else if (strcmp(ev, "permission") == 0)      { label = "Needs you";       color = 0xd97757; }
    else                                         { label = ev;                color = 0x1f1f1e; }

    ui_flash_event(label, color);
    sound_event(ev);
    return true;
}

// Serial command buffer
#define CMD_BUF_SIZE 256   // holds the usage JSON line pushed over serial
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_pos = 0;

static void send_screenshot() {
    const uint32_t w = LCD_WIDTH, h = LCD_HEIGHT;
    const uint32_t row_bytes = w * 2;
    const uint32_t buf_size = row_bytes * h;
    uint8_t* sbuf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!sbuf) {
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_RGB565, row_bytes, sbuf, buf_size);

    lv_result_t res = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &draw_buf);
    if (res != LV_RESULT_OK) {
        heap_caps_free(sbuf);
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    Serial.printf("SCREENSHOT_START %lu %lu %lu\n", (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");

    heap_caps_free(sbuf);
}

static void check_serial_cmd() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            if (strcmp(cmd_buf, "screenshot") == 0) {
                send_screenshot();
            } else if (cmd_buf[0] == '{') {
                // {"ev":...} = session event; otherwise a usage payload.
                if (handle_event_json(cmd_buf)) {
                    Serial.println("EVENT_OK");
                } else {
                    Serial.println(apply_usage_json(cmd_buf) ? "USAGE_OK" : "USAGE_ERR");
                }
            }
            cmd_pos = 0;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("{\"ready\":true}");

    // Init I2C (shared by touch + TCA9554 + PMU + IMU)
    Wire.begin(IIC_SDA, IIC_SCL);

    // Init PMU FIRST — it enables the AXP2101 power rails that feed the LCD
    // panel. The panel must be powered before its reset pulse / gfx->begin().
    power_init();
    delay(100);   // let the LCD rails stabilize

    // TCA9554 drives the LCD reset/enable line — mandatory before gfx->begin()
    tca.begin();
    tca.pinMode1(TCA_LCD_RST_CH, OUTPUT);
    tca.write1(TCA_LCD_RST_CH, 1); delay(10);
    tca.write1(TCA_LCD_RST_CH, 0); delay(10);
    tca.write1(TCA_LCD_RST_CH, 1); delay(200);

    // Init display (ST7796 over SPI; driver does the landscape HW rotation).
    if (!gfx->begin()) {
        Serial.println("gfx->begin() failed!");
    }
    gfx->fillScreen(0x0000);

    // Backlight on (active high)
    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, HIGH);

    // Init IMU (present on the bus; accelerometer is read but not used for
    // rotation — orientation is fixed landscape on this board)
    imu_init();

    // Init touch (FocalTech FT6336 @ 0x38, shared I2C bus).
    touch_ok = touch.begin(Wire, FT6X36_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
    if (touch_ok) {
        // FT6336 reports coords in native portrait (320x480). We run the
        // display rotated to 480x320 landscape, so remap to match.
        touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
        touch.setSwapXY(true);
        touch.setMirrorXY(false, true);
        Serial.println("FT6336 touch init OK");
    } else {
        Serial.println("FT6336 touch init FAILED");
    }

    // Init LVGL
    lv_init();
    lv_tick_set_cb(my_tick);

    // Allocate PSRAM-backed partial render buffers
    buf1 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);
    buf2 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);

    lv_display_t* disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, LCD_WIDTH * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);

    // Init ES8311 audio (event sounds)
    sound_init();

    // Build dashboard
    ui_init();

    // Show initial battery status
    ui_update_battery(power_battery_pct(), power_is_charging());

    // Boot straight to the Usage dashboard (no buttons; tap the screen to
    // toggle the splash animation).
    ui_show_screen(SCREEN_USAGE);

    Serial.println("Dashboard ready, waiting for usage data on USB serial...");
}

void loop() {
    touch_read();
    lv_timer_handler();
    ui_tick_anim();
    ui_event_tick();
    power_tick();
    imu_tick();
    splash_tick();

    // Update battery indicator
    static int last_pct = -2;
    static bool last_charging = false;
    int pct = power_battery_pct();
    bool charging = power_is_charging();
    if (pct != last_pct || charging != last_charging) {
        last_pct = pct;
        last_charging = charging;
        ui_update_battery(pct, charging);
    }

    // Usage payloads + session events arrive here over USB serial.
    check_serial_cmd();

    delay(5);
}
