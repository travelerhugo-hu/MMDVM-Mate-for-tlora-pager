#include "ui.h"

#include <LilyGoLib.h>
#include <lvgl.h>
#include <WiFi.h>

#include "app_bus.h"
#include "audio_player.h"
#include "callsign.h"
#include "history.h"
#include "hose_client.h"
#include "settings.h"
#include "wifi_time.h"
#include <ctype.h>      // tolower() for the Number/Symbol (123) mode

// ---------------------------------------------------------------------------
// Palette. Dark, low-blue, high-contrast - this thing gets stared at in a car
// at night as often as at a desk.
// ---------------------------------------------------------------------------
#define C_BG        0x0D1117
#define C_PANEL     0x161B22
#define C_PANEL2    0x1C2129
#define C_BORDER    0x30363D
#define C_TEXT      0xC9D1D9
#define C_DIM       0x6E7681
#define C_ACCENT    0x58A6FF
#define C_OK        0x3FB950
#define C_WARN      0xD29922
#define C_ERR       0xF85149

#define CALL_PANEL_Y  BANNER_TOP_H
#define CALL_PANEL_H  90
#define HIST_PANEL_Y  (CALL_PANEL_Y + CALL_PANEL_H)
#define HIST_PANEL_H  (SCR_H - HIST_PANEL_Y - BANNER_BOTTOM_H)
#define HIST_ROW_H    18
#define HIST_VISIBLE  4

// ---------------------------------------------------------------------------
// Settings model
// ---------------------------------------------------------------------------
enum SetRow : uint8_t {
    ROW_SSID,
    ROW_PASS,
    ROW_TG,
    ROW_VOL,
    ROW_BL,
    ROW_SLEEP,
    ROW_APPLY,
    ROW_POWER,
    ROW_COUNT,
};

static const char *const SET_LABELS[ROW_COUNT] = {
    "Wi-Fi Network",
    "Wi-Fi Password",
    "Talkgroup",
    "Volume",
    "Backlight",
    "Screen Off",
    "Apply & Reconnect",
    "Power Off",
};

#define SET_ROW_H       26
#define SET_VISIBLE     6

enum SetMode : uint8_t {
    SM_NAV,          ///< moving the selection bar
    SM_VALUE,        ///< adjusting a numeric/enum value in place
    SM_TEXT,         ///< typing into the overlay editor
    SM_SCAN,         ///< Wi-Fi scan in flight
    SM_PICK,         ///< choosing from the scan result list
    SM_POWER,        ///< "press enter again to power off"
};

// ---------------------------------------------------------------------------
// Widget handles
// ---------------------------------------------------------------------------
static lv_obj_t *scr_main, *scr_set;

static lv_obj_t *lbl_link, *lbl_clock, *lbl_batt;
static lv_obj_t *lbl_tg, *lbl_call, *lbl_sub, *bar_vu;
static lv_obj_t *hist_call[HIST_VISIBLE], *hist_time[HIST_VISIBLE];
static lv_obj_t *lbl_hint, *lbl_vol;

static lv_obj_t *set_row[ROW_COUNT], *set_name[ROW_COUNT], *set_val[ROW_COUNT];
static lv_obj_t *lbl_set_clock, *lbl_set_hint;
static lv_obj_t *ovl, *ovl_title, *ovl_body, *ovl_hint;

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------
static bool      in_settings   = false;
static SetMode   set_mode      = SM_NAV;
static uint8_t   set_sel       = 0;
static uint8_t   set_top       = 0;      // first visible settings row
static bool      wifi_dirty    = false;  // credentials changed, needs reconnect

static uint32_t  cur_dmr       = 0;      // 0 == channel idle
static char      cur_call[CALLSIGN_MAX];
static char      cur_name[TA_NAME_MAX];
// Not called "link": <unistd.h> is pulled in transitively by Arduino.h and
// declares link(2), so a file-scope variable of that name is a redeclaration.
static LinkState link_state    = LINK_WIFI_DOWN;

static uint8_t   hist_top      = 0;      // scroll offset in the history list
static uint32_t  last_slow_ms  = 0;
static uint32_t  vu_decay_ms   = 0;

// text editor
static char      edit_buf[WIFI_PASS_MAX];
static uint8_t   edit_len      = 0;
static bool      edit_digits   = false;   // true for numeric-only fields (Talkgroup)
static SetRow    edit_row      = ROW_SSID;

// bottom-bar hint: live shortcut legend shown on the main screen
static const char SHORTCUT_HINT[] = "S settings   L backlight   Q/E volume";

// Number/Symbol input mode (firmware-owned, toggled by the orange key). The
// board has no dedicated digit row, so in 123 mode a letter key emits the
// digit/symbol printed on its face. The orange key emits KB_SYM_SENTINEL from
// the keyboard driver and the firmware flips this flag.
static bool g_symbol = false;

// Transient bottom-bar override (L = backlight level, orange = 123 mode) that
// auto-reverts to the shortcut legend after 5 seconds - see ui_tick().
static uint32_t hint_override_ms = 0;
static bool     hint_overridden   = false;

// Wi-Fi picker
#define PICK_MAX 12
static char      pick_ssid[PICK_MAX][WIFI_SSID_MAX];
static int8_t    pick_rssi[PICK_MAX];
static uint8_t   pick_count    = 0;
static uint8_t   pick_sel      = 0;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static lv_obj_t *mk_panel(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                          const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

/// Montserrat has no bullet glyph, so masked passwords use asterisks and are
/// capped so a 60-character PSK does not overflow the row.
static void mask_secret(char *out, size_t sz, const char *src)
{
    size_t n = strlen(src);
    if (n == 0) { strlcpy(out, "not set", sz); return; }
    if (n > 10) n = 10;
    if (n > sz - 1) n = sz - 1;
    memset(out, '*', n);
    out[n] = '\0';
}

static void fmt_hms(char *buf, size_t sz, time_t t)
{
    if (t == 0) { strlcpy(buf, "--:--:--", sz); return; }
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(buf, sz, "%H:%M:%S", &tmv);
}



// ---------------------------------------------------------------------------
// Main screen construction
// ---------------------------------------------------------------------------
static void build_main()
{
    scr_main = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_main, lv_color_hex(C_BG), 0);
    lv_obj_remove_flag(scr_main, LV_OBJ_FLAG_SCROLLABLE);

    // --- top banner --------------------------------------------------------
    lv_obj_t *top = mk_panel(scr_main, 0, 0, SCR_W, BANNER_TOP_H, C_PANEL);

    lbl_link = mk_label(top, &lv_font_montserrat_14, C_ERR, LV_SYMBOL_WIFI " OFFLINE");
    lv_obj_align(lbl_link, LV_ALIGN_LEFT_MID, 8, 0);

    lbl_clock = mk_label(top, &lv_font_montserrat_16, C_TEXT, "--:--:-- UTC");
    lv_obj_align(lbl_clock, LV_ALIGN_CENTER, 0, 0);

    lbl_batt = mk_label(top, &lv_font_montserrat_14, C_TEXT, LV_SYMBOL_BATTERY_FULL " --%");
    lv_obj_align(lbl_batt, LV_ALIGN_RIGHT_MID, -8, 0);

    // --- call panel --------------------------------------------------------
    lv_obj_t *cp = mk_panel(scr_main, 0, CALL_PANEL_Y, SCR_W, CALL_PANEL_H, C_BG);

    lbl_tg = mk_label(cp, &lv_font_montserrat_20, C_ACCENT, "TG ----");
    lv_obj_align(lbl_tg, LV_ALIGN_TOP_LEFT, 10, 4);

    bar_vu = lv_bar_create(cp);
    lv_obj_set_size(bar_vu, 150, 8);
    lv_obj_align(bar_vu, LV_ALIGN_TOP_RIGHT, -10, 12);
    lv_bar_set_range(bar_vu, 0, 100);
    lv_bar_set_value(bar_vu, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar_vu, 2, 0);
    lv_obj_set_style_bg_color(bar_vu, lv_color_hex(C_PANEL2), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_vu, lv_color_hex(C_OK), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_vu, 2, LV_PART_INDICATOR);

    lbl_call = mk_label(cp, &lv_font_montserrat_40, C_DIM, "FREE");
    lv_obj_align(lbl_call, LV_ALIGN_CENTER, 0, 6);

    lbl_sub = mk_label(cp, &lv_font_montserrat_14, C_DIM, "no traffic");
    lv_obj_align(lbl_sub, LV_ALIGN_BOTTOM_MID, 0, -2);

    // --- history -----------------------------------------------------------
    lv_obj_t *hp = mk_panel(scr_main, 0, HIST_PANEL_Y, SCR_W, HIST_PANEL_H, C_PANEL);
    lv_obj_set_style_border_side(hp, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(hp, lv_color_hex(C_BORDER), 0);
    lv_obj_set_style_border_width(hp, 1, 0);

    for (uint8_t i = 0; i < HIST_VISIBLE; ++i) {
        int y = 2 + i * HIST_ROW_H;
        hist_call[i] = mk_label(hp, &lv_font_montserrat_14, C_TEXT, "");
        lv_obj_align(hist_call[i], LV_ALIGN_TOP_LEFT, 10, y);

        hist_time[i] = mk_label(hp, &lv_font_montserrat_14, C_DIM, "");
        lv_obj_align(hist_time[i], LV_ALIGN_TOP_RIGHT, -10, y);
    }

    // --- bottom banner -----------------------------------------------------
    lv_obj_t *bot = mk_panel(scr_main, 0, SCR_H - BANNER_BOTTOM_H, SCR_W, BANNER_BOTTOM_H, C_PANEL2);

    lbl_hint = mk_label(bot, &lv_font_montserrat_12, C_DIM, SHORTCUT_HINT);
    lv_obj_align(lbl_hint, LV_ALIGN_LEFT_MID, 8, 0);

    lbl_vol = mk_label(bot, &lv_font_montserrat_12, C_TEXT, LV_SYMBOL_VOLUME_MAX " --%");
    lv_obj_align(lbl_vol, LV_ALIGN_RIGHT_MID, -8, 0);
}

// ---------------------------------------------------------------------------
// Settings screen construction
// ---------------------------------------------------------------------------
static void build_settings()
{
    scr_set = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_set, lv_color_hex(C_BG), 0);
    lv_obj_remove_flag(scr_set, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *top = mk_panel(scr_set, 0, 0, SCR_W, BANNER_TOP_H, C_PANEL);
    lv_obj_t *t = mk_label(top, &lv_font_montserrat_16, C_ACCENT, LV_SYMBOL_SETTINGS "  SETTINGS");
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 8, 0);
    lbl_set_clock = mk_label(top, &lv_font_montserrat_14, C_DIM, "--:--:-- UTC");
    lv_obj_align(lbl_set_clock, LV_ALIGN_RIGHT_MID, -8, 0);

    for (uint8_t i = 0; i < ROW_COUNT; ++i) {
        set_row[i] = mk_panel(scr_set, 0, BANNER_TOP_H + i * SET_ROW_H,
                              SCR_W, SET_ROW_H, C_BG);
        set_name[i] = mk_label(set_row[i], &lv_font_montserrat_14, C_TEXT, SET_LABELS[i]);
        lv_obj_align(set_name[i], LV_ALIGN_LEFT_MID, 14, 0);
        set_val[i] = mk_label(set_row[i], &lv_font_montserrat_14, C_ACCENT, "");
        lv_obj_align(set_val[i], LV_ALIGN_RIGHT_MID, -14, 0);
    }

    lv_obj_t *bot = mk_panel(scr_set, 0, SCR_H - BANNER_BOTTOM_H, SCR_W, BANNER_BOTTOM_H, C_PANEL2);
    lbl_set_hint = mk_label(bot, &lv_font_montserrat_12, C_DIM,
                            "Q/E move   ENTER select   BKSP back");
    lv_obj_align(lbl_set_hint, LV_ALIGN_LEFT_MID, 8, 0);

    // --- modal overlay (text editor / network picker) ----------------------
    ovl = mk_panel(scr_set, 40, 34, SCR_W - 80, SCR_H - 68, C_PANEL);
    lv_obj_set_style_border_width(ovl, 1, 0);
    lv_obj_set_style_border_color(ovl, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_radius(ovl, 4, 0);
    lv_obj_add_flag(ovl, LV_OBJ_FLAG_HIDDEN);

    ovl_title = mk_label(ovl, &lv_font_montserrat_14, C_ACCENT, "");
    lv_obj_align(ovl_title, LV_ALIGN_TOP_LEFT, 12, 8);

    ovl_body = mk_label(ovl, &lv_font_montserrat_16, C_TEXT, "");
    lv_obj_set_width(ovl_body, SCR_W - 104);
    lv_label_set_long_mode(ovl_body, LV_LABEL_LONG_WRAP);
    lv_obj_align(ovl_body, LV_ALIGN_TOP_LEFT, 12, 32);

    ovl_hint = mk_label(ovl, &lv_font_montserrat_12, C_DIM, "");
    lv_obj_align(ovl_hint, LV_ALIGN_BOTTOM_LEFT, 12, -8);
}

// ---------------------------------------------------------------------------
// Main screen refresh
// ---------------------------------------------------------------------------
static void refresh_link()
{
    const char *txt;
    uint32_t col;
    switch (link_state) {
    case LINK_HOSE_UP:         txt = LV_SYMBOL_WIFI " HOSELINE"; col = C_OK;     break;
    case LINK_HOSE_CONNECTING: txt = LV_SYMBOL_WIFI " LINKING";  col = C_WARN;   break;
    case LINK_WIFI_UP:         txt = LV_SYMBOL_WIFI " WI-FI";    col = C_WARN;   break;
    case LINK_HOSE_DOWN:       txt = LV_SYMBOL_WIFI " NO HOSE";  col = C_ERR;    break;
    default:                   txt = LV_SYMBOL_WARNING " OFFLINE"; col = C_ERR;  break;
    }
    lv_label_set_text(lbl_link, txt);
    lv_obj_set_style_text_color(lbl_link, lv_color_hex(col), 0);
}

static void refresh_call()
{
    if (cur_dmr == 0) {
        lv_label_set_text(lbl_call, "FREE");
        lv_obj_set_style_text_color(lbl_call, lv_color_hex(C_DIM), 0);
        lv_label_set_text(lbl_sub, "no traffic");
        lv_obj_set_style_text_color(lbl_sub, lv_color_hex(C_DIM), 0);
        lv_bar_set_value(bar_vu, 0, LV_ANIM_OFF);
    } else {
        char id[20];
        if (cur_call[0]) {
            lv_label_set_text(lbl_call, cur_call);
        } else {
            snprintf(id, sizeof(id), "%lu", (unsigned long)cur_dmr);
            lv_label_set_text(lbl_call, id);
        }
        lv_obj_set_style_text_color(lbl_call, lv_color_hex(C_OK), 0);

        if (cur_name[0]) {
            snprintf(id, sizeof(id), "%lu", (unsigned long)cur_dmr);
            static char sub[TA_NAME_MAX + 24];
            snprintf(sub, sizeof(sub), "%s  -  %s", cur_name, id);
            lv_label_set_text(lbl_sub, sub);
        } else {
            lv_label_set_text_fmt(lbl_sub, "DMR ID %lu", (unsigned long)cur_dmr);
        }
        lv_obj_set_style_text_color(lbl_sub, lv_color_hex(C_TEXT), 0);
    }
    lv_obj_align(lbl_call, LV_ALIGN_CENTER, 0, 6);
    lv_obj_align(lbl_sub, LV_ALIGN_BOTTOM_MID, 0, -2);
}

static void refresh_history()
{
    uint8_t n = history_count();
    if (hist_top > 0 && hist_top + HIST_VISIBLE > n) {
        hist_top = (n > HIST_VISIBLE) ? (uint8_t)(n - HIST_VISIBLE) : 0;
    }

    for (uint8_t i = 0; i < HIST_VISIBLE; ++i) {
        uint8_t idx = hist_top + i;
        if (idx >= n) {
            lv_label_set_text(hist_call[i], "");
            lv_label_set_text(hist_time[i], "");
            continue;
        }
        const QsoEntry &e = history_at(idx);

        char left[CALLSIGN_MAX + 16];
        if (e.call[0]) {
            snprintf(left, sizeof(left), "%s", e.call);
        } else {
            snprintf(left, sizeof(left), "#%lu", (unsigned long)e.dmr_id);
        }
        lv_label_set_text(hist_call[i], left);
        lv_obj_set_style_text_color(hist_call[i],
                                    lv_color_hex(e.dmr_id == cur_dmr ? C_OK : C_TEXT), 0);

        char right[32], hms[12];
        fmt_hms(hms, sizeof(hms), e.last_heard);
        snprintf(right, sizeof(right), "TG %lu   %s", (unsigned long)e.talkgroup, hms);
        lv_label_set_text(hist_time[i], right);
        lv_obj_align(hist_time[i], LV_ALIGN_TOP_RIGHT, -10, 2 + i * HIST_ROW_H);
    }
}

static void refresh_volume()
{
    lv_label_set_text_fmt(lbl_vol, "%s %u%%",
                          g_settings.volume ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE,
                          g_settings.volume);
    lv_obj_align(lbl_vol, LV_ALIGN_RIGHT_MID, -8, 0);
}

static void refresh_tg()
{
    lv_label_set_text_fmt(lbl_tg, "TG %lu", (unsigned long)g_settings.talkgroup);
}

// ---------------------------------------------------------------------------
// Settings screen refresh
// ---------------------------------------------------------------------------
static void set_value_text(SetRow r, char *out, size_t sz)
{
    switch (r) {
    case ROW_SSID:
        strlcpy(out, g_settings.wifi_ssid[0] ? g_settings.wifi_ssid : "not set", sz);
        break;
    case ROW_PASS:
        mask_secret(out, sz, g_settings.wifi_pass);
        break;
    case ROW_TG:
        snprintf(out, sz, "%lu", (unsigned long)g_settings.talkgroup);
        break;
    case ROW_VOL:
        snprintf(out, sz, "%u %%", g_settings.volume);
        break;
    case ROW_BL:
        snprintf(out, sz, "%u %%", BL_PERCENTS[g_settings.bl_index]);
        break;
    case ROW_SLEEP:
        strlcpy(out, SLEEP_LABELS[g_settings.sleep_index], sz);
        break;
    case ROW_APPLY:
        strlcpy(out, wifi_dirty ? "pending" : "", sz);
        break;
    case ROW_POWER:
        strlcpy(out, LV_SYMBOL_POWER, sz);
        break;
    default:
        out[0] = '\0';
        break;
    }
}

static void refresh_settings_rows()
{
    // Keep the selection inside the visible window.
    if (set_sel < set_top)                    set_top = set_sel;
    if (set_sel >= set_top + SET_VISIBLE)     set_top = set_sel - SET_VISIBLE + 1;

    for (uint8_t i = 0; i < ROW_COUNT; ++i) {
        bool visible = (i >= set_top) && (i < set_top + SET_VISIBLE);
        if (!visible) {
            lv_obj_add_flag(set_row[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(set_row[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(set_row[i], BANNER_TOP_H + (i - set_top) * SET_ROW_H);

        bool sel  = (i == set_sel);
        bool edit = sel && set_mode == SM_VALUE;

        uint32_t bg   = sel ? (edit ? C_ACCENT : C_PANEL2) : C_BG;
        uint32_t name = sel ? (edit ? C_BG : C_TEXT) : C_DIM;
        uint32_t val  = edit ? C_BG : C_ACCENT;

        if (i == ROW_POWER) val = edit ? C_BG : C_ERR;

        lv_obj_set_style_bg_color(set_row[i], lv_color_hex(bg), 0);
        lv_obj_set_style_text_color(set_name[i], lv_color_hex(name), 0);
        lv_obj_set_style_text_color(set_val[i], lv_color_hex(val), 0);

        char v[48];
        set_value_text((SetRow)i, v, sizeof(v));
        lv_label_set_text(set_val[i], v);
        lv_obj_align(set_val[i], LV_ALIGN_RIGHT_MID, -14, 0);
    }
}

static void refresh_settings_hint()
{
    const char *h;
    switch (set_mode) {
    case SM_VALUE: h = "Q/E adjust   ENTER done   BKSP done";        break;
    case SM_TEXT:  h = "type   ENTER save   BKSP delete";            break;
    case SM_SCAN:  h = "scanning...";                                break;
    case SM_PICK:  h = "Q/E move   ENTER pick   BKSP cancel";        break;
    case SM_POWER: h = "ENTER to power off   BKSP to cancel";        break;
    default:       h = "Q/E move   ENTER select   S/BKSP back";      break;
    }
    lv_label_set_text(lbl_set_hint, h);
}

// Forward declaration (defined further below, after the overlay helpers).
static void overlay_show_text(void);

/// Translate a character-mode key into its symbol-map (123) equivalent. The
/// T-LoRa-Pager prints digits on the symbol layer, so in 123 mode a letter key
/// emits the digit/symbol printed on its face. Mirrors the board keymap.
static char sym_translate(char c)
{
    switch (c) {
    case 'q': return '1'; case 'w': return '2'; case 'e': return '3';
    case 'r': return '4'; case 't': return '5'; case 'y': return '6';
    case 'u': return '7'; case 'i': return '8'; case 'o': return '9';
    case 'p': return '0';
    case 'a': return '*'; case 's': return '/'; case 'd': return '+';
    case 'f': return '-'; case 'g': return '='; case 'h': return ':';
    case 'j': return '\''; case 'k': return '"'; case 'l': return '@';
    case 'z': return '_'; case 'x': return '$'; case 'c': return ';';
    case 'v': return '?'; case 'b': return '!'; case 'n': return ',';
    case 'm': return '.';
    default:  return c;
    }
}

/// Show a transient message on the main-screen bottom bar; reverts to the
/// shortcut legend after 5 s (handled in ui_tick()).
static void set_hint_override(const char *msg)
{
    lv_label_set_text(lbl_hint, msg);
    hint_override_ms = millis();
    hint_overridden  = true;
}

/// Orange key: toggle Number/Symbol input mode. Refreshes whichever hint is
/// currently visible.
static void toggle_symbol_mode()
{
    g_symbol = !g_symbol;
    if (in_settings && set_mode == SM_TEXT) {
        overlay_show_text();   // refresh the editor hint (abc <-> 123)
    } else {
        char buf[40];
        snprintf(buf, sizeof(buf), "Symbol: %s", g_symbol ? "123" : "abc");
        set_hint_override(buf); // auto-reverts to shortcut legend after 5 s
    }
}

// ---------------------------------------------------------------------------
// Overlay (text editor + Wi-Fi picker)
// ---------------------------------------------------------------------------
static void overlay_hide()
{
    lv_obj_add_flag(ovl, LV_OBJ_FLAG_HIDDEN);
}

static void overlay_show_text()
{
    lv_obj_remove_flag(ovl, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(ovl_title, SET_LABELS[edit_row]);

    // A trailing block makes the caret position obvious without a real cursor.
    char shown[WIFI_PASS_MAX + 4];
    snprintf(shown, sizeof(shown), "%s_", edit_buf);
    lv_label_set_text(ovl_body, shown);
    lv_obj_set_style_text_color(ovl_body, lv_color_hex(C_TEXT), 0);

    if (edit_digits) {
        lv_label_set_text(ovl_hint,
            "123 mode - digits only - orange toggles 123/abc - ENTER save, BKSP delete");
    } else {
        lv_label_set_text(ovl_hint,
            g_symbol
                ? "123 mode - orange: abc - ENTER save, BKSP delete"
                : "abc mode - orange: 123 - ENTER save, BKSP delete");
    }
}

static void overlay_show_pick()
{
    lv_obj_remove_flag(ovl, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(ovl_title, "Nearby networks");

    if (pick_count == 0) {
        lv_label_set_text(ovl_body, "no networks found\n\nENTER to type the SSID by hand");
        lv_label_set_text(ovl_hint, "ENTER manual entry   BKSP cancel");
        return;
    }

    // Six lines fit in the overlay; window them around the selection.
    static char body[6 * (WIFI_SSID_MAX + 12) + 40];
    body[0] = '\0';
    uint8_t first = 0;
    if (pick_sel >= 5) first = pick_sel - 4;
    uint8_t last = first + 5;
    if (last > pick_count) last = pick_count;

    for (uint8_t i = first; i < last; ++i) {
        char line[WIFI_SSID_MAX + 16];
        snprintf(line, sizeof(line), "%s %s  %d dBm\n",
                 i == pick_sel ? ">" : " ", pick_ssid[i], pick_rssi[i]);
        strlcat(body, line, sizeof(body));
    }
    strlcat(body, pick_sel == pick_count ? "> [type manually]" : "  [type manually]",
            sizeof(body));

    lv_label_set_text(ovl_body, body);
    lv_label_set_text(ovl_hint, "Q/E move   ENTER pick   BKSP cancel");
}

static void overlay_show_power()
{
    lv_obj_remove_flag(ovl, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(ovl_title, LV_SYMBOL_POWER "  Power off");
    lv_label_set_text(ovl_body, "The receiver will stop and the\nscreen will go dark.\n\n"
                                "Press ENTER again to confirm.");
    lv_obj_set_style_text_color(ovl_body, lv_color_hex(C_WARN), 0);
    lv_label_set_text(ovl_hint, "ENTER confirm   BKSP cancel");
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
static void apply_backlight()
{
    instance.setBrightness(settings_backlight_level());
}

static void apply_volume()
{
    audio_set_volume(g_settings.volume);
}

static void apply_wifi_if_needed()
{
    if (!wifi_dirty) return;
    wifi_dirty = false;
    log_i("ui: reconnecting with new credentials");
    wifi_disconnect();
    wifi_begin_connect();
    hose_request_reconnect();
}

static void enter_settings()
{
    in_settings = true;
    set_mode    = SM_NAV;
    set_sel     = 0;
    set_top     = 0;
    overlay_hide();
    refresh_settings_rows();
    refresh_settings_hint();
    lv_screen_load(scr_set);
}

static void leave_settings()
{
    in_settings = false;
    set_mode    = SM_NAV;
    overlay_hide();
    settings_save();
    history_save();
    apply_wifi_if_needed();
    refresh_tg();
    refresh_volume();
    refresh_history();
    lv_screen_load(scr_main);
}

static void power_off()
{
    settings_save();
    history_save();
    audio_set_volume(0);
    // Fade out instead of cutting the backlight: the charge pump clicks
    // audibly if it is switched off at full current.
    instance.decrementBrightness(0, 5, false);
    instance.ppm.shutdown();
    // shutdown() does not return on battery; on USB power it does, so park.
    for (;;) delay(1000);
}

static void start_wifi_scan()
{
    set_mode   = SM_SCAN;
    pick_count = 0;
    pick_sel   = 0;
    WiFi.scanDelete();
    WiFi.scanNetworks(true /* async */, false /* hidden */);
    lv_obj_remove_flag(ovl, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(ovl_title, "Wi-Fi");
    lv_label_set_text(ovl_body, "Scanning for networks...");
    lv_obj_set_style_text_color(ovl_body, lv_color_hex(C_TEXT), 0);
    lv_label_set_text(ovl_hint, "BKSP cancel");
    refresh_settings_hint();
}

static void begin_text_edit(SetRow row, bool digits, const char *initial)
{
    edit_row    = row;
    edit_digits = digits;
    // Numeric fields (e.g. Talkgroup) start in 123 mode so digits are
    // immediately typeable; alpha fields start in abc mode.
    g_symbol    = digits;
    strlcpy(edit_buf, initial ? initial : "", sizeof(edit_buf));
    edit_len    = strlen(edit_buf);
    set_mode    = SM_TEXT;
    overlay_show_text();
    refresh_settings_hint();
}

static void commit_text_edit()
{
    switch (edit_row) {
    case ROW_SSID:
        strlcpy(g_settings.wifi_ssid, edit_buf, sizeof(g_settings.wifi_ssid));
        wifi_dirty = true;
        break;
    case ROW_PASS:
        strlcpy(g_settings.wifi_pass, edit_buf, sizeof(g_settings.wifi_pass));
        wifi_dirty = true;
        break;
    case ROW_TG: {
        uint32_t tg = strtoul(edit_buf, nullptr, 10);
        // The Hoseline front end refuses anything <= 90 and so does the server
        // in practice; silently keeping the old value is friendlier than
        // subscribing to something that will never deliver a frame.
        if (tg >= HOSE_TG_MIN) {
            g_settings.talkgroup = tg;
            hose_set_talkgroup(tg);
            audio_flush();
            cur_dmr = 0;
            cur_call[0] = cur_name[0] = '\0';
            refresh_tg();
            refresh_call();
        }
        break;
    }
    default:
        break;
    }
    settings_save();
    set_mode = SM_NAV;
    overlay_hide();
    refresh_settings_rows();
    refresh_settings_hint();
}

/// One step of a value row. `dir` is +1 or -1.
static void bump_value(int dir)
{
    switch (set_sel) {
    // NOTE: ROW_TG is edited as a digits-only text field (see activate_row ->
    // begin_text_edit(ROW_TG, true, ...)), so it never reaches this stepper.
    case ROW_VOL: {
        int v = (int)g_settings.volume + dir * 5;
        if (v < 0)   v = 0;
        if (v > 100) v = 100;
        g_settings.volume = (uint8_t)v;
        apply_volume();
        refresh_volume();
        break;
    }
    case ROW_BL: {
        int b = (int)g_settings.bl_index + dir;
        if (b < 0) b = 0;
        if (b > 3) b = 3;
        g_settings.bl_index = (uint8_t)b;
        apply_backlight();
        break;
    }
    case ROW_SLEEP: {
        int s = (int)g_settings.sleep_index + dir;
        if (s < 0) s = 0;
        if (s > 4) s = 4;
        g_settings.sleep_index = (uint8_t)s;
        break;
    }
    default:
        return;
    }
    refresh_settings_rows();
}

/// ENTER on the currently selected settings row.
static void activate_row()
{
    switch (set_sel) {
    case ROW_SSID:
        start_wifi_scan();
        break;
    case ROW_PASS:
        begin_text_edit(ROW_PASS, false, g_settings.wifi_pass);
        break;
    case ROW_TG: {
        char cur[12];
        snprintf(cur, sizeof(cur), "%lu", (unsigned long)g_settings.talkgroup);
        begin_text_edit(ROW_TG, true, cur);
        break;
    }
    case ROW_VOL:
    case ROW_BL:
    case ROW_SLEEP:
        set_mode = SM_VALUE;
        refresh_settings_rows();
        refresh_settings_hint();
        break;
    case ROW_APPLY:
        settings_save();
        apply_wifi_if_needed();
        hose_set_talkgroup(g_settings.talkgroup);
        refresh_settings_rows();
        break;
    case ROW_POWER:
        set_mode = SM_POWER;
        overlay_show_power();
        refresh_settings_hint();
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Wi-Fi scan completion
// ---------------------------------------------------------------------------
static void poll_wifi_scan()
{
    if (set_mode != SM_SCAN) return;

    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;

    pick_count = 0;
    if (n > 0) {
        for (int i = 0; i < n && pick_count < PICK_MAX; ++i) {
            String ssid = WiFi.SSID(i);
            if (ssid.length() == 0) continue;

            bool dup = false;
            for (uint8_t j = 0; j < pick_count; ++j) {
                if (ssid.equals(pick_ssid[j])) { dup = true; break; }
            }
            if (dup) continue;                       // same SSID, several APs

            strlcpy(pick_ssid[pick_count], ssid.c_str(), WIFI_SSID_MAX);
            pick_rssi[pick_count] = (int8_t)WiFi.RSSI(i);
            pick_count++;
        }
    }
    WiFi.scanDelete();

    pick_sel = 0;
    set_mode = SM_PICK;
    overlay_show_pick();
    refresh_settings_hint();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void ui_begin()
{
    cur_call[0] = cur_name[0] = '\0';

    build_main();
    build_settings();

    refresh_tg();
    refresh_volume();
    refresh_call();
    refresh_history();
    refresh_link();

    lv_screen_load(scr_main);
}

void ui_tick()
{
    // Revert a transient bottom-bar message (backlight level / 123 mode) back
    // to the shortcut legend 5 s after it was shown.
    if (hint_overridden && (millis() - hint_override_ms >= 5000)) {
        lv_label_set_text(lbl_hint, SHORTCUT_HINT);
        hint_overridden = false;
    }

    poll_wifi_scan();

    // Everything below is once a second; the clock is the fastest thing here.
    uint32_t now = millis();
    if (now - last_slow_ms < 500) {
        // VU decay still needs sub-second resolution.
        if (cur_dmr && vu_decay_ms && now - vu_decay_ms > 400) {
            lv_bar_set_value(bar_vu, 0, LV_ANIM_OFF);
            vu_decay_ms = 0;
        }
        return;
    }
    last_slow_ms = now;

    char hms[16], line[24];
    utc_clock_string(hms, sizeof(hms));
    snprintf(line, sizeof(line), "%s UTC", hms);
    lv_label_set_text(in_settings ? lbl_set_clock : lbl_clock, line);

    static uint32_t last_batt = 0;
    if (now - last_batt > 10000 || last_batt == 0) {
        last_batt = now;
        instance.gauge.refresh();
        int soc = instance.gauge.getStateOfCharge();
        if (soc < 0)   soc = 0;
        if (soc > 100) soc = 100;
        bool charging = instance.gauge.getCurrent() > 0;
        lv_label_set_text_fmt(lbl_batt, "%s %d%%",
                              charging ? LV_SYMBOL_CHARGE : LV_SYMBOL_BATTERY_FULL, soc);
        lv_obj_set_style_text_color(lbl_batt,
                                    lv_color_hex(soc <= 15 && !charging ? C_ERR : C_TEXT), 0);
        lv_obj_align(lbl_batt, LV_ALIGN_RIGHT_MID, -8, 0);
    }
}

void ui_handle_event(const UiEvent &ev)
{
    switch (ev.type) {
    case EV_LINK_STATE:
        link_state = (LinkState)ev.link;
        refresh_link();
        break;

    case EV_CALL_START:
        cur_dmr = ev.dmr_id;
        strlcpy(cur_call, ev.call, sizeof(cur_call));
        cur_name[0] = '\0';
        history_note(ev.dmr_id, ev.talkgroup, ev.call);
        hist_top = 0;                       // a new caller belongs on screen
        refresh_call();
        refresh_history();
        break;

    case EV_CALL_END:
        cur_dmr = 0;
        cur_call[0] = cur_name[0] = '\0';
        refresh_call();
        refresh_history();
        break;

    case EV_TALKER_ALIAS:
        if (cur_dmr == 0) break;
        if (ev.call[0]) strlcpy(cur_call, ev.call, sizeof(cur_call));
        if (ev.name[0]) strlcpy(cur_name, ev.name, sizeof(cur_name));
        history_resolve(cur_dmr, ev.call);
        refresh_call();
        refresh_history();
        break;

    case EV_CALLSIGN_RESOLVED:
        if (ev.dmr_id == cur_dmr && ev.call[0] && !cur_call[0]) {
            strlcpy(cur_call, ev.call, sizeof(cur_call));
            refresh_call();
        }
        if (history_resolve(ev.dmr_id, ev.call)) refresh_history();
        break;

    case EV_VU_METER: {
        // CALL_METER is dBm-ish on a roughly -40..0 scale. Clamp and map.
        int pct = (int)((ev.vu_db + 40.0f) * 2.5f);
        if (pct < 0)   pct = 0;
        if (pct > 100) pct = 100;
        lv_bar_set_value(bar_vu, pct, LV_ANIM_OFF);
        vu_decay_ms = millis();
        break;
    }

    case EV_ROTARY:
        ui_handle_rotary((RotaryCode)ev.code);
        break;

    default:
        break;
    }
}

// --- input -----------------------------------------------------------------

static void key_main(char c)
{
    switch (c) {
    case 's': case 'S':
        enter_settings();
        break;

    case 'l': case 'L': {
        g_settings.bl_index = (uint8_t)((g_settings.bl_index + 1) % 4);
        apply_backlight();
        settings_save();
        char bh[32];
        snprintf(bh, sizeof(bh), "backlight %u%%", BL_PERCENTS[g_settings.bl_index]);
        set_hint_override(bh);   // auto-reverts to shortcut legend after 5 s
        break;
    }

    case 'q': case 'Q':
        if (g_settings.volume >= 5) g_settings.volume -= 5; else g_settings.volume = 0;
        apply_volume();
        refresh_volume();
        settings_save();
        break;

    case 'e': case 'E':
        g_settings.volume = (uint8_t)(g_settings.volume <= 95 ? g_settings.volume + 5 : 100);
        apply_volume();
        refresh_volume();
        settings_save();
        break;

    default:
        break;
    }
}

static void key_settings(char c)
{
    switch (set_mode) {

    case SM_NAV:
        if (c == 'q' || c == 'Q') {
            set_sel = (uint8_t)((set_sel + ROW_COUNT - 1) % ROW_COUNT);
            refresh_settings_rows();
        } else if (c == 'e' || c == 'E') {
            set_sel = (uint8_t)((set_sel + 1) % ROW_COUNT);
            refresh_settings_rows();
        } else if (c == '\n') {
            activate_row();
        } else if (c == '\b' || c == 's' || c == 'S') {
            leave_settings();
        }
        break;

    case SM_VALUE:
        if (c == 'q' || c == 'Q')      bump_value(-1);
        else if (c == 'e' || c == 'E') bump_value(+1);
        else if (c == '\n' || c == '\b') {
            set_mode = SM_NAV;
            settings_save();
            refresh_settings_rows();
            refresh_settings_hint();
        }
        break;

    case SM_TEXT:
        if (c == '\n') {
            commit_text_edit();
        } else if (c == '\b') {
            if (edit_len > 0) {
                edit_buf[--edit_len] = '\0';
                overlay_show_text();
            } else {
                set_mode = SM_NAV;             // empty + backspace == cancel
                overlay_hide();
                refresh_settings_hint();
            }
        } else if (c >= 0x20 && c < 0x7F) {
            // Apply the active input mode to the keystroke. The board has no
            // dedicated digit row, so in 123 mode a letter key emits the
            // digit/symbol printed on its face.
            char tc = g_symbol ? sym_translate((char)tolower((unsigned char)c)) : c;
            bool digits_only = edit_digits;
            if (digits_only && (tc < '0' || tc > '9')) break;
            size_t cap = (edit_row == ROW_PASS) ? WIFI_PASS_MAX
                       : (edit_row == ROW_SSID) ? WIFI_SSID_MAX
                                                : 8;
            if (edit_len + 1 < cap) {
                edit_buf[edit_len++] = tc;
                edit_buf[edit_len]   = '\0';
                overlay_show_text();
            }
        }
        break;

    case SM_SCAN:
        if (c == '\b') {
            WiFi.scanDelete();
            set_mode = SM_NAV;
            overlay_hide();
            refresh_settings_hint();
        }
        break;

    case SM_PICK:
        if (c == 'q' || c == 'Q') {
            pick_sel = (uint8_t)(pick_sel == 0 ? pick_count : pick_sel - 1);
            overlay_show_pick();
        } else if (c == 'e' || c == 'E') {
            pick_sel = (uint8_t)(pick_sel >= pick_count ? 0 : pick_sel + 1);
            overlay_show_pick();
        } else if (c == '\n') {
            if (pick_sel < pick_count) {
                strlcpy(g_settings.wifi_ssid, pick_ssid[pick_sel],
                        sizeof(g_settings.wifi_ssid));
                wifi_dirty = true;
                settings_save();
                set_mode = SM_NAV;
                overlay_hide();
                refresh_settings_rows();
                refresh_settings_hint();
                // Picking a network almost always means the password is next.
                set_sel = ROW_PASS;
                refresh_settings_rows();
            } else {
                begin_text_edit(ROW_SSID, false, g_settings.wifi_ssid);
            }
        } else if (c == '\b') {
            set_mode = SM_NAV;
            overlay_hide();
            refresh_settings_hint();
        }
        break;

    case SM_POWER:
        if (c == '\n') {
            power_off();
        } else if (c == '\b' || c == 's' || c == 'S') {
            set_mode = SM_NAV;
            overlay_hide();
            refresh_settings_hint();
        }
        break;
    }
}

void ui_handle_key(char c)
{
    if (c == '\0') return;
    if (c == KB_SYM_SENTINEL) { toggle_symbol_mode(); return; }  // orange key
    if (in_settings) key_settings(c);
    else             key_main(c);
}

void ui_handle_rotary(RotaryCode code)
{
    if (!in_settings) {
        uint8_t n = history_count();
        switch (code) {
        case ROT_CW:
            if (n > HIST_VISIBLE && hist_top + HIST_VISIBLE < n) hist_top++;
            refresh_history();
            break;
        case ROT_CCW:
            if (hist_top > 0) hist_top--;
            refresh_history();
            break;
        case ROT_CLICK:
            enter_settings();
            break;
        }
        return;
    }

    // In settings the encoder is just a nicer Q/E.
    switch (code) {
    case ROT_CCW:  ui_handle_key('q'); break;
    case ROT_CW:   ui_handle_key('e'); break;
    case ROT_CLICK: ui_handle_key('\n'); break;
    }
}

bool ui_in_settings()
{
    return in_settings;
}

bool ui_capturing_text()
{
    return in_settings && set_mode == SM_TEXT;
}
