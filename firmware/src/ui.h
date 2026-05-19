#pragma once
#include "data.h"

enum screen_t {
    SCREEN_SPLASH,
    SCREEN_USAGE,
    SCREEN_COUNT,
};

void ui_init(void);
void ui_update(const UsageData* data);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);

// Brief event banner for Claude-session events. color_hex is 0xRRGGBB.
// Auto-dismisses; call ui_event_tick() every loop to handle the timeout.
void ui_flash_event(const char* label, unsigned long color_hex);
void ui_event_tick(void);
