#include "ui.h"
#include "splash.h"
#include <lvgl.h>
#include "logo.h"
#include "icons.h"
#include "display_cfg.h"
#include "clawd_config.h"

// Custom fonts (scaled for 314 PPI, ~1.9x from original 165 PPI)
LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_mono_32);
LV_FONT_DECLARE(font_mono_18);
LV_FONT_DECLARE(font_styrene_16);

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

// ---- Layout constants for 480x320 LANDSCAPE (3.5" IPS, flat corners) ----
// Top band holds the 80px logo (left), title (center), battery (right).
// Usage screen places the two stat panels side-by-side so the big % stays large.
#define SCR_W         480
#define SCR_H         320
#define MARGIN        16
#define TITLE_Y       16            // title top within the top band
#define TOPBAND       88            // height reserved for logo/title/battery
#define CONTENT_Y     92            // panels start just below the top band
#define CONTENT_W     (SCR_W - 2 * MARGIN)   // 448
#define USAGE_GAP     14            // gap between the two side-by-side panels
#define USAGE_PANEL_W ((CONTENT_W - USAGE_GAP) / 2)  // 217
#define USAGE_PANEL_H 190           // 92 -> 282, leaves room for the anim line

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

// ---- Status-board screen widgets ----
// A fixed grid of pre-created dot+label cells (like the usage screen pre-creates
// its widgets); update only sets color/text/visibility — never creates objects
// or parses arrays at runtime.
#define SB_CELLS 24
static lv_obj_t* status_container;
static lv_obj_t* lbl_status_title;
static lv_obj_t* sb_dot[SB_CELLS];
static lv_obj_t* sb_cell_lbl[SB_CELLS];
static lv_obj_t* lbl_status_lf;     // bottom "last fail" line

// ---- Corner clock (host-formatted "8:23am · 04", one label per screen) ----
static lv_obj_t* lbl_usage_clock;
static lv_obj_t* lbl_status_clock;
static lv_obj_t* lbl_status_sum;    // top-right "N up · M down" summary

// ---- Herd screen widgets (herdr agents; same pre-created-grid pattern) ----
// 3 columns (wider than the status board's 4) so repo-name labels fit;
// 6 rows before the bottom line -> 18 cells.
#define HD_CELLS 18
static lv_obj_t* herd_container;
static lv_obj_t* lbl_herd_title;
static lv_obj_t* hd_dot[HD_CELLS];
static lv_obj_t* hd_cell_lbl[HD_CELLS];
static lv_obj_t* hd_hit[HD_CELLS];  // transparent tap zones (tap-to-focus)

// ---- Remote screen widgets (nav + shortcut buttons) ----
#define RM_SHORTCUTS 4
#define RM_ARM_MS 4000
static lv_obj_t* remote_container;
static lv_obj_t* rm_sc_btn[RM_SHORTCUTS];
static lv_obj_t* rm_sc_lbl[RM_SHORTCUTS];
static lv_obj_t* lbl_remote_focus;
static lv_obj_t* lbl_remote_clock;
static int      rm_armed = -1;       // shortcut awaiting its confirm tap
static uint32_t rm_armed_ms = 0;
static bool have_remote = false;

// "+ CLI" project picker: while active, the four big buttons become project
// choices (3 dirs from the daemon + "New tmp/"); times out back to shortcuts.
#define RM_PICKER_MS 5000
static lv_obj_t* rm_cli_btn = NULL;
static bool     rm_picker = false;
static uint32_t rm_picker_ms = 0;
static char     rm_sc_text[RM_SHORTCUTS][16];  // shortcut labels (restore)
static char     rm_pk_text[3][16];             // picker project labels
static lv_obj_t* lbl_herd_sum;      // top-right "N working · M blocked"
static lv_obj_t* lbl_herd_focus;    // bottom-left "focus: <label>"
static lv_obj_t* lbl_herd_clock;

// ---- Logo (shared, on top) ----
static lv_obj_t* logo_img;

// ---- Shared ----
static lv_image_dsc_t logo_dsc;
static screen_t current_screen = SCREEN_USAGE;

// ---- Screen rotation (Usage <-> Status) ----
#define ROTATE_MS 30000
static bool have_usage = false;
static bool have_status = false;
static bool have_herd = false;
static uint32_t last_rotate_ms = 0;

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

// Either payload (usage or status) may carry the clock; update both screens'
// corner labels so whichever is visible stays current.
static void set_clock_labels(const char* clk) {
    if (!clk || !clk[0]) return;
    lv_label_set_text(lbl_usage_clock, clk);
    lv_label_set_text(lbl_status_clock, clk);
    lv_label_set_text(lbl_herd_clock, clk);
    lv_label_set_text(lbl_remote_clock, clk);
}

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
static void rotate_step(bool include_remote);

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


// ======== Usage Screen (480x320 landscape, side-by-side panels) ========

// One Session/Weekly panel, stacked vertically inside a tall narrow column:
// pill on top, big % below it, progress bar, reset label.
static void make_usage_panel(lv_obj_t* parent, int x, int y, const char* pill_text,
                             lv_obj_t** out_pct, lv_obj_t** out_pill,
                             lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, x, y, USAGE_PANEL_W, USAGE_PANEL_H);
    // Inner content width = panel width minus make_panel's 16px L/R padding.
    const int inner_w = USAGE_PANEL_W - 32;

    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_LEFT, 0, 0);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, &font_styrene_48, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 52);

    *out_bar = make_bar(panel, 0, 116, inner_w, 20);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    // styrene_20: "Resets in 18h 36m" at 24px overflowed the 185px inner width
    lv_obj_set_style_text_font(*out_reset, &font_styrene_20, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, 146);
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
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, TITLE_Y);

    make_usage_panel(usage_container, MARGIN, CONTENT_Y, "Current",
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset);
    make_usage_panel(usage_container, MARGIN + USAGE_PANEL_W + USAGE_GAP, CONTENT_Y, "Weekly",
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset);

    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, &font_mono_18, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -10);

    lbl_usage_clock = lv_label_create(usage_container);
    lv_label_set_text(lbl_usage_clock, "");
    lv_obj_set_style_text_font(lbl_usage_clock, &font_mono_18, 0);
    lv_obj_set_style_text_color(lbl_usage_clock, COL_DIM, 0);
    lv_obj_align(lbl_usage_clock, LV_ALIGN_BOTTOM_RIGHT, -MARGIN, -10);
}

// ======== Status-board Screen (480x320 landscape) ========
// Four plain static labels (no recolor, no per-item object churn -> crash-safe):
// a title, a summary line, a RED label listing whatever needs attention, and a
// Grid of dot+label cells (4 columns), pre-created here and only updated
// (color/text/visibility) at runtime — no object creation or array parsing
// in the hot path. Plus a bottom "last fail" line.
static void init_status_screen(lv_obj_t* scr) {
    status_container = lv_obj_create(scr);
    lv_obj_set_size(status_container, SCR_W, SCR_H);
    lv_obj_set_pos(status_container, 0, 0);
    lv_obj_set_style_bg_opa(status_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_container, 0, 0);
    lv_obj_set_style_pad_all(status_container, 0, 0);
    lv_obj_clear_flag(status_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(status_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lbl_status_title = lv_label_create(status_container);
    lv_label_set_text(lbl_status_title, "Web Status");
    lv_obj_set_style_text_font(lbl_status_title, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_status_title, COL_TEXT, 0);
    // Left-aligned: the centered title collided with the top-right summary
    lv_obj_align(lbl_status_title, LV_ALIGN_TOP_LEFT, MARGIN, 8);

    const int COLS = 4, CW = 116, X0 = MARGIN, Y0 = 52, RH = 40;
    for (int i = 0; i < SB_CELLS; i++) {
        int x = X0 + (i % COLS) * CW;
        int y = Y0 + (i / COLS) * RH;

        sb_dot[i] = lv_obj_create(status_container);
        lv_obj_set_size(sb_dot[i], 16, 16);
        lv_obj_set_pos(sb_dot[i], x, y + 3);
        lv_obj_set_style_radius(sb_dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(sb_dot[i], 0, 0);
        lv_obj_set_style_bg_color(sb_dot[i], COL_GREEN, 0);
        lv_obj_clear_flag(sb_dot[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(sb_dot[i], LV_OBJ_FLAG_HIDDEN);

        sb_cell_lbl[i] = lv_label_create(status_container);
        lv_obj_set_pos(sb_cell_lbl[i], x + 22, y + 1);
        lv_obj_set_style_text_font(sb_cell_lbl[i], &font_styrene_16, 0);
        lv_obj_set_style_text_color(sb_cell_lbl[i], COL_DIM, 0);
        lv_label_set_text(sb_cell_lbl[i], "");
        lv_obj_add_flag(sb_cell_lbl[i], LV_OBJ_FLAG_HIDDEN);
    }

    lbl_status_lf = lv_label_create(status_container);
    lv_obj_set_pos(lbl_status_lf, X0, 292);
    // Cap the width so a long "last fail" line can't run under the corner
    // clock; overflow is elided with "..." by LVGL.
    lv_obj_set_width(lbl_status_lf, 330);
    lv_label_set_long_mode(lbl_status_lf, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(lbl_status_lf, &font_mono_18, 0);
    lv_obj_set_style_text_color(lbl_status_lf, COL_AMBER, 0);
    lv_label_set_text(lbl_status_lf, "waiting for status...");

    lbl_status_clock = lv_label_create(status_container);
    lv_label_set_text(lbl_status_clock, "");
    lv_obj_set_style_text_font(lbl_status_clock, &font_mono_18, 0);
    lv_obj_set_style_text_color(lbl_status_clock, COL_DIM, 0);
    lv_obj_align(lbl_status_clock, LV_ALIGN_BOTTOM_RIGHT, -MARGIN, -8);

    lbl_status_sum = lv_label_create(status_container);
    lv_label_set_text(lbl_status_sum, "");
    // mono_18: styrene_16 is ASCII-only and the summary uses U+00B7 "·"
    lv_obj_set_style_text_font(lbl_status_sum, &font_mono_18, 0);
    lv_obj_set_style_text_color(lbl_status_sum, COL_DIM, 0);
    lv_obj_align(lbl_status_sum, LV_ALIGN_TOP_RIGHT, -MARGIN, 16);
}

// ======== Herd Screen (480x320 landscape) ========
// herdr agent grid — same crash-safe pre-created dot+label cells as the
// status board. Dot: dim=idle, amber=working, red=blocked. "*" prefix on the
// label marks the focused pane.
//
// Tap-to-focus: each cell has a transparent hit zone that sends {"btn":N}
// up the serial link; the daemon maps N to the pane and runs
// `herdr agent focus`. The zones do NOT bubble, so a tap on an agent focuses
// it while a tap on empty space still advances the screen.
static void herd_cell_click_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    Serial.printf("{\"btn\":%d}\n", idx);
}

static void init_herd_screen(lv_obj_t* scr) {
    herd_container = lv_obj_create(scr);
    lv_obj_set_size(herd_container, SCR_W, SCR_H);
    lv_obj_set_pos(herd_container, 0, 0);
    lv_obj_set_style_bg_opa(herd_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(herd_container, 0, 0);
    lv_obj_set_style_pad_all(herd_container, 0, 0);
    lv_obj_clear_flag(herd_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(herd_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lbl_herd_title = lv_label_create(herd_container);
    lv_label_set_text(lbl_herd_title, "Herd");
    lv_obj_set_style_text_font(lbl_herd_title, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_herd_title, COL_TEXT, 0);
    lv_obj_align(lbl_herd_title, LV_ALIGN_TOP_LEFT, MARGIN, 8);

    const int COLS = 3, CW = 149, X0 = MARGIN, Y0 = 52, RH = 40;
    for (int i = 0; i < HD_CELLS; i++) {
        int x = X0 + (i % COLS) * CW;
        int y = Y0 + (i / COLS) * RH;

        hd_dot[i] = lv_obj_create(herd_container);
        lv_obj_set_size(hd_dot[i], 16, 16);
        lv_obj_set_pos(hd_dot[i], x, y + 3);
        lv_obj_set_style_radius(hd_dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(hd_dot[i], 0, 0);
        lv_obj_set_style_bg_color(hd_dot[i], COL_DIM, 0);
        lv_obj_clear_flag(hd_dot[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(hd_dot[i], LV_OBJ_FLAG_HIDDEN);

        hd_cell_lbl[i] = lv_label_create(herd_container);
        lv_obj_set_pos(hd_cell_lbl[i], x + 22, y + 1);
        lv_obj_set_style_text_font(hd_cell_lbl[i], &font_styrene_16, 0);
        lv_obj_set_style_text_color(hd_cell_lbl[i], COL_DIM, 0);
        lv_label_set_text(hd_cell_lbl[i], "");
        lv_obj_add_flag(hd_cell_lbl[i], LV_OBJ_FLAG_HIDDEN);

        // Transparent tap zone covering the whole cell, on top of dot+label.
        // No EVENT_BUBBLE: its click must not also trigger screen rotation.
        hd_hit[i] = lv_obj_create(herd_container);
        lv_obj_set_pos(hd_hit[i], x - 4, y - 4);
        lv_obj_set_size(hd_hit[i], CW - 6, RH);
        lv_obj_set_style_bg_opa(hd_hit[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(hd_hit[i], 0, 0);
        lv_obj_clear_flag(hd_hit[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(hd_hit[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(hd_hit[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(hd_hit[i], herd_cell_click_cb, LV_EVENT_CLICKED,
                            (void*)(intptr_t)i);
    }

    lbl_herd_focus = lv_label_create(herd_container);
    lv_obj_set_pos(lbl_herd_focus, X0, 292);
    lv_obj_set_width(lbl_herd_focus, 330);
    lv_label_set_long_mode(lbl_herd_focus, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(lbl_herd_focus, &font_mono_18, 0);
    lv_obj_set_style_text_color(lbl_herd_focus, COL_DIM, 0);
    lv_label_set_text(lbl_herd_focus, "waiting for herd...");

    lbl_herd_clock = lv_label_create(herd_container);
    lv_label_set_text(lbl_herd_clock, "");
    lv_obj_set_style_text_font(lbl_herd_clock, &font_mono_18, 0);
    lv_obj_set_style_text_color(lbl_herd_clock, COL_DIM, 0);
    lv_obj_align(lbl_herd_clock, LV_ALIGN_BOTTOM_RIGHT, -MARGIN, -8);

    lbl_herd_sum = lv_label_create(herd_container);
    lv_label_set_text(lbl_herd_sum, "");
    lv_obj_set_style_text_font(lbl_herd_sum, &font_mono_18, 0);
    lv_obj_set_style_text_color(lbl_herd_sum, COL_DIM, 0);
    lv_obj_align(lbl_herd_sum, LV_ALIGN_TOP_RIGHT, -MARGIN, 16);
}

// ======== Remote Screen (480x320 landscape) ========
// Navigation (prev/next terminal, new tab) fires immediately; the four
// shortcut buttons ARM on the first tap (blue banner "Tap again") and only
// SEND {"act":"sc<i>"} on a second tap within RM_ARM_MS — a stray finger
// must not type "commit and push" into an agent. Button labels come from the
// daemon ({"rm":1,"b0":...}, sourced from ~/.config/clawd/shortcuts.json) so
// the device always shows what would actually be sent.

static void rm_set_armed(int idx) {
    if (rm_armed >= 0)
        lv_obj_set_style_border_width(rm_sc_btn[rm_armed], 0, 0);
    rm_armed = idx;
    rm_armed_ms = lv_tick_get();
    if (idx >= 0) {
        lv_obj_set_style_border_color(rm_sc_btn[idx], COL_ACCENT, 0);
        lv_obj_set_style_border_width(rm_sc_btn[idx], 3, 0);
    }
}

static void rm_exit_picker(void) {
    if (!rm_picker) return;
    rm_picker = false;
    if (rm_cli_btn) lv_obj_set_style_border_width(rm_cli_btn, 0, 0);
    for (int i = 0; i < RM_SHORTCUTS; i++)
        lv_label_set_text(rm_sc_lbl[i], rm_sc_text[i][0] ? rm_sc_text[i] : "-");
}

static void rm_enter_picker(void) {
    rm_set_armed(-1);
    rm_picker = true;
    rm_picker_ms = lv_tick_get();
    if (rm_cli_btn) {
        lv_obj_set_style_border_color(rm_cli_btn, COL_ACCENT, 0);
        lv_obj_set_style_border_width(rm_cli_btn, 3, 0);
    }
    for (int i = 0; i < 3; i++)
        lv_label_set_text(rm_sc_lbl[i], rm_pk_text[i][0] ? rm_pk_text[i] : "-");
    lv_label_set_text(rm_sc_lbl[3], "New tmp/");
    ui_flash_event("CLI where? (+ CLI again = here)", 0x4a6b8a);
}

void ui_remote_enter_picker(void) {   // also reachable via {"pk":1} for QA
    rm_enter_picker();
}

static void rm_nav_click_cb(lv_event_t* e) {
    const char* act = (const char*)lv_event_get_user_data(e);
    if (strcmp(act, "cli") == 0) {
        // First tap opens the project picker; second tap = "spawn here"
        if (!rm_picker) {
            rm_enter_picker();
        } else {
            rm_exit_picker();
            Serial.printf("{\"act\":\"clihere\"}\n");
            ui_flash_event("CLI: here", 0x788c5d);
        }
        return;
    }
    Serial.printf("{\"act\":\"%s\"}\n", act);
}

static void rm_sc_click_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    static char msg[40];
    if (rm_picker) {
        // Picker mode: big buttons are project choices, single tap spawns
        if (idx < 3 && !rm_pk_text[idx][0]) { rm_exit_picker(); return; }
        snprintf(msg, sizeof(msg), "CLI: %s", lv_label_get_text(rm_sc_lbl[idx]));
        rm_exit_picker();
        Serial.printf(idx < 3 ? "{\"act\":\"cli%d\"}\n" : "{\"act\":\"clitmp\"}\n", idx);
        ui_flash_event(msg, 0x788c5d);
        return;
    }
    if (rm_armed == idx && lv_tick_get() - rm_armed_ms < RM_ARM_MS) {
        Serial.printf("{\"act\":\"sc%d\"}\n", idx);
        rm_set_armed(-1);
        snprintf(msg, sizeof(msg), "Sent: %s", lv_label_get_text(rm_sc_lbl[idx]));
        ui_flash_event(msg, 0x788c5d);
    } else {
        rm_set_armed(idx);
        snprintf(msg, sizeof(msg), "Tap again: %s", lv_label_get_text(rm_sc_lbl[idx]));
        ui_flash_event(msg, 0x4a6b8a);
    }
}

static lv_obj_t* make_remote_btn(lv_obj_t* parent, int x, int y, int w, int h,
                                 const char* text, lv_obj_t** out_lbl,
                                 lv_event_cb_t cb, void* user_data) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);   // no bubble: taps stay here
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &font_styrene_20, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_center(lbl);
    if (out_lbl) *out_lbl = lbl;
    return btn;
}

static void init_remote_screen(lv_obj_t* scr) {
    remote_container = lv_obj_create(scr);
    lv_obj_set_size(remote_container, SCR_W, SCR_H);
    lv_obj_set_pos(remote_container, 0, 0);
    lv_obj_set_style_bg_opa(remote_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(remote_container, 0, 0);
    lv_obj_set_style_pad_all(remote_container, 0, 0);
    lv_obj_clear_flag(remote_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(remote_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* title = lv_label_create(remote_container);
    lv_label_set_text(title, "Remote");
    lv_obj_set_style_text_font(title, &font_styrene_28, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, MARGIN, 8);

    // Nav row: immediate actions ("cli" = new tab running claude in
    // bypass-permissions mode — the user's usual way to spawn an agent)
    const int NAV_W = (CONTENT_W - 3 * 12) / 4, NAV_H = 48, NAV_Y = 52;
    static const char* const nav_acts[] = {"prev", "next", "tab", "cli"};
    static const char* const nav_lbls[] = {"< Prev", "Next >", "+ Tab", "+ CLI"};
    for (int i = 0; i < 4; i++) {
        lv_obj_t* b = make_remote_btn(remote_container, MARGIN + i * (NAV_W + 12),
                                      NAV_Y, NAV_W, NAV_H, nav_lbls[i], NULL,
                                      rm_nav_click_cb, (void*)nav_acts[i]);
        if (i == 3) rm_cli_btn = b;   // picker highlights this while active
    }

    // Shortcut grid 2x2: arm/confirm actions (labels filled by the daemon)
    const int SC_W = (CONTENT_W - 14) / 2, SC_H = 64;
    for (int i = 0; i < RM_SHORTCUTS; i++) {
        int x = MARGIN + (i % 2) * (SC_W + 14);
        int y = 116 + (i / 2) * (SC_H + 12);
        rm_sc_btn[i] = make_remote_btn(remote_container, x, y, SC_W, SC_H,
                                       "...", &rm_sc_lbl[i],
                                       rm_sc_click_cb, (void*)(intptr_t)i);
    }

    lbl_remote_focus = lv_label_create(remote_container);
    lv_obj_set_pos(lbl_remote_focus, MARGIN, 292);
    lv_obj_set_width(lbl_remote_focus, 330);
    lv_label_set_long_mode(lbl_remote_focus, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(lbl_remote_focus, &font_mono_18, 0);
    lv_obj_set_style_text_color(lbl_remote_focus, COL_DIM, 0);
    lv_label_set_text(lbl_remote_focus, "");

    lbl_remote_clock = lv_label_create(remote_container);
    lv_label_set_text(lbl_remote_clock, "");
    lv_obj_set_style_text_font(lbl_remote_clock, &font_mono_18, 0);
    lv_obj_set_style_text_color(lbl_remote_clock, COL_DIM, 0);
    lv_obj_align(lbl_remote_clock, LV_ALIGN_BOTTOM_RIGHT, -MARGIN, -8);
}

// ======== Bluetooth Screen (480x320 landscape) ========

// ======== Public API ========

void ui_init(void) {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Logo (shared, always visible, on top of all containers)
    // Logo is RGB565A8 (planar: w*h RGB565 then w*h alpha) so it composites
    // cleanly against whatever bg is behind it.
    init_icon_dsc_rgb565a8(&logo_dsc, LOGO_WIDTH, LOGO_HEIGHT, logo_data);

    init_usage_screen(scr);
    init_status_screen(scr);
    init_herd_screen(scr);
    init_remote_screen(scr);
    splash_init(scr);

    // Splash is touch-toggled — tap anywhere on the splash dismisses it
    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    // Logo on top of all containers (inset for rounded corners)
    logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, &logo_dsc);
    lv_obj_set_pos(logo_img, MARGIN, TITLE_Y - 10);
}

void ui_update(const UsageData* data) {
    if (!data->valid) return;
    have_usage = true;

    int s_pct = (int)(data->session_pct + 0.5f);

    // Usage screen
    lv_label_set_text_fmt(lbl_session_pct, "%d%%", s_pct);
    lv_bar_set_value(bar_session, s_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_session, pct_color(data->session_pct), LV_PART_INDICATOR);

    char buf[48];
    format_reset_time(data->session_reset_mins, buf, sizeof(buf));
    lv_label_set_text(lbl_session_reset, buf);

    int w_pct = (int)(data->weekly_pct + 0.5f);
    lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", w_pct);
    lv_bar_set_value(bar_weekly, w_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_weekly, pct_color(data->weekly_pct), LV_PART_INDICATOR);

    format_reset_time(data->weekly_reset_mins, buf, sizeof(buf));
    lv_label_set_text(lbl_weekly_reset, buf);

    set_clock_labels(data->clk);
}

// Update the dot grid: set each cell's color (green/red/grey) + name + show it;
// hide unused cells. Only touches pre-created widgets — no allocation here.
void ui_update_status(const StatusData* data) {
    if (!data->valid) return;
    have_status = true;
    for (int i = 0; i < SB_CELLS; i++) {
        if (i < data->count) {
            lv_color_t c = data->items[i].state == 1 ? COL_GREEN
                         : data->items[i].state == 0 ? COL_RED : COL_DIM;
            lv_obj_set_style_bg_color(sb_dot[i], c, 0);
            lv_obj_clear_flag(sb_dot[i], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(sb_cell_lbl[i], data->items[i].name);
            lv_obj_clear_flag(sb_cell_lbl[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(sb_dot[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(sb_cell_lbl[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_label_set_text(lbl_status_lf, data->lastfail[0] ? data->lastfail : "all systems OK");

    // Top-right summary, counted from the states we just applied. Green when
    // everything is up, red the moment anything is down (readable across the
    // desk without reading dot labels).
    int up = 0, down = 0, unk = 0;
    for (int i = 0; i < data->count; i++) {
        if      (data->items[i].state == 1) up++;
        else if (data->items[i].state == 0) down++;
        else                                unk++;
    }
    static char sum[40];
    if (unk > 0)
        snprintf(sum, sizeof(sum), "%d up \xC2\xB7 %d down \xC2\xB7 %d ?", up, down, unk);
    else
        snprintf(sum, sizeof(sum), "%d up \xC2\xB7 %d down", up, down);
    lv_label_set_text(lbl_status_sum, sum);
    lv_obj_set_style_text_color(lbl_status_sum, down > 0 ? COL_RED : COL_GREEN, 0);
    lv_obj_set_style_text_color(lbl_status_title, down > 0 ? COL_RED : COL_TEXT, 0);

    set_clock_labels(data->clk);
}

// Update the herd grid — dim=idle(0), amber=working(1), red=blocked(2).
void ui_update_herd(const HerdData* data) {
    if (!data->valid) return;
    have_herd = true;
    int working = 0, blocked = 0;
    for (int i = 0; i < HD_CELLS; i++) {
        if (i < data->count) {
            uint8_t st = data->items[i].state;
            lv_color_t c = st == 2 ? COL_RED
                         : st == 1 ? COL_AMBER : COL_DIM;
            if (st == 1) working++;
            if (st == 2) blocked++;
            lv_obj_set_style_bg_color(hd_dot[i], c, 0);
            lv_obj_clear_flag(hd_dot[i], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(hd_cell_lbl[i], data->items[i].name);
            // Label color mirrors urgency: blocked=red (matches dot),
            // focused pane ("*" prefix)=accent orange + underline,
            // working=bright, idle=dim.
            bool focused = data->items[i].name[0] == '*';
            lv_obj_set_style_text_color(hd_cell_lbl[i],
                st == 2   ? COL_RED
                : focused ? COL_ACCENT
                : st == 1 ? COL_TEXT : COL_DIM, 0);
            lv_obj_set_style_text_decor(hd_cell_lbl[i],
                focused ? LV_TEXT_DECOR_UNDERLINE : LV_TEXT_DECOR_NONE, 0);
            lv_obj_clear_flag(hd_cell_lbl[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(hd_hit[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(hd_dot[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(hd_cell_lbl[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(hd_hit[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_label_set_text(lbl_herd_sum, data->sum);
    lv_obj_set_style_text_color(lbl_herd_sum,
        blocked > 0 ? COL_RED : working > 0 ? COL_AMBER : COL_DIM, 0);
    lv_obj_set_style_text_color(lbl_herd_title,
        blocked > 0 ? COL_RED : COL_TEXT, 0);
    lv_label_set_text(lbl_herd_focus, data->focus[0] ? data->focus : "");
    // The Remote screen shows the same focus line: it says which pane the
    // nav / shortcut buttons will act on.
    lv_label_set_text(lbl_remote_focus, data->focus[0] ? data->focus : "");
    set_clock_labels(data->clk);
}

void ui_update_remote(const char* b0, const char* b1,
                      const char* b2, const char* b3,
                      const char* p0, const char* p1, const char* p2) {
    const char* b[RM_SHORTCUTS] = {b0, b1, b2, b3};
    const char* p[3] = {p0, p1, p2};
    for (int i = 0; i < RM_SHORTCUTS; i++)
        strlcpy(rm_sc_text[i], b[i] ? b[i] : "", sizeof(rm_sc_text[i]));
    for (int i = 0; i < 3; i++)
        strlcpy(rm_pk_text[i], p[i] ? p[i] : "", sizeof(rm_pk_text[i]));
    if (!rm_picker)   // don't overwrite the project choices mid-pick
        for (int i = 0; i < RM_SHORTCUTS; i++)
            lv_label_set_text(rm_sc_lbl[i], rm_sc_text[i][0] ? rm_sc_text[i] : "-");
    have_remote = true;
}

void ui_remote_tick(void) {
    if (rm_armed >= 0 && lv_tick_get() - rm_armed_ms >= RM_ARM_MS)
        rm_set_armed(-1);
    if (rm_picker && lv_tick_get() - rm_picker_ms >= RM_PICKER_MS)
        rm_exit_picker();
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

// A tap anywhere advances to the next info screen (Usage <-> Status), or
// toggles the splash when the status screen is disabled.
static void global_click_cb(lv_event_t* e) {
    (void)e;
#if CLAWD_STATUS_SCREEN
    rotate_step(true);   // manual taps cycle through the Remote screen too
#else
    ui_toggle_splash();
#endif
}

void ui_show_screen(screen_t screen) {
    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(herd_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(remote_container, LV_OBJ_FLAG_HIDDEN);
    splash_hide();
    rm_set_armed(-1);   // leaving/entering a screen disarms any pending send
    rm_exit_picker();

    switch (screen) {
    case SCREEN_SPLASH:     splash_show(); break;
    case SCREEN_USAGE:      lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_STATUS:     lv_obj_clear_flag(status_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_HERD:       lv_obj_clear_flag(herd_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_REMOTE:     lv_obj_clear_flag(remote_container, LV_OBJ_FLAG_HIDDEN); break;
    default: break;
    }

    // Hide the logo overlay everywhere except the usage screen (splash needs a
    // clean canvas; the grid screens have their own left-aligned titles where
    // the logo would overlap).
    if (logo_img) {
        if (screen == SCREEN_USAGE)
            lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    }

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    // Any explicit switch (tap, first data, event-driven show) earns a full
    // rotation interval before auto-rotation moves on.
    last_rotate_ms = lv_tick_get();
}

void ui_toggle_splash(void) {
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

// Advance to the next screen, skipping screens whose data never arrived
// (e.g. herdr not running). The Remote control screen is reachable only by
// MANUAL taps — auto-rotation skips it (a control surface carries no info,
// and rotating onto it wastes a 30s slot; rotating off it after a dwell is
// fine and is how you leave it without tapping through).
static void rotate_step(bool include_remote) {
    static const screen_t order[] = {SCREEN_USAGE, SCREEN_STATUS,
                                     SCREEN_HERD, SCREEN_REMOTE};
    const bool have[] = {have_usage, have_status, have_herd,
                         include_remote && have_remote};
    int cur = 0;
    for (int i = 0; i < 4; i++)
        if (order[i] == current_screen) cur = i;
    for (int k = 1; k <= 4; k++) {
        int n = (cur + k) % 4;
        if (have[n] || k == 4) {   // k==4: nothing has data yet — just advance
            ui_show_screen(order[n]);
            break;
        }
    }
    last_rotate_ms = lv_tick_get();
}

void ui_rotate_next(void) {   // auto-rotation path
    rotate_step(false);
}

// Called every loop; auto-rotates once at least two datasets have arrived.
void ui_tick_rotate(void) {
    int n = (have_usage ? 1 : 0) + (have_status ? 1 : 0) + (have_herd ? 1 : 0);
    if (n < 2) return;
    if (current_screen == SCREEN_SPLASH) return;
    if (lv_tick_get() - last_rotate_ms >= ROTATE_MS) ui_rotate_next();
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

// ======== Event banner (Claude-session events) ========
// Floats on the top layer so it appears over any screen incl. the splash.

static lv_obj_t* ev_banner = nullptr;
static lv_obj_t* ev_label  = nullptr;
static uint32_t  ev_shown_ms = 0;
static bool      ev_visible  = false;
#define EV_BANNER_MS 2200

static void ev_banner_ensure(void) {
    if (ev_banner) return;
    ev_banner = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ev_banner, 480, 60);
    lv_obj_align(ev_banner, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(ev_banner, 0, 0);
    lv_obj_set_style_border_width(ev_banner, 0, 0);
    lv_obj_set_style_pad_all(ev_banner, 0, 0);
    lv_obj_set_style_bg_opa(ev_banner, LV_OPA_COVER, 0);
    lv_obj_clear_flag(ev_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ev_banner, LV_OBJ_FLAG_HIDDEN);

    ev_label = lv_label_create(ev_banner);
    lv_obj_set_style_text_font(ev_label, &font_styrene_28, 0);
    lv_obj_set_style_text_color(ev_label, lv_color_hex(0xffffff), 0);
    lv_obj_center(ev_label);
}

void ui_flash_event(const char* label, unsigned long color_hex) {
    ev_banner_ensure();
    lv_obj_set_style_bg_color(ev_banner, lv_color_hex((uint32_t)color_hex), 0);
    lv_label_set_text(ev_label, label ? label : "Event");
    lv_obj_clear_flag(ev_banner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ev_banner);
    ev_shown_ms = lv_tick_get();
    ev_visible  = true;
}

void ui_event_tick(void) {
    if (ev_visible && (lv_tick_get() - ev_shown_ms) >= EV_BANNER_MS) {
        if (ev_banner) lv_obj_add_flag(ev_banner, LV_OBJ_FLAG_HIDDEN);
        ev_visible = false;
    }
}
