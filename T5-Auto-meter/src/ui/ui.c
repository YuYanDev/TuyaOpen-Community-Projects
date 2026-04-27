/**
 * @file ui.c
 * @brief Top-level UI: boot sweep -> BLE wait -> live gauges, plus menu.
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 *
 * UI state machine (driven by OBD session state and button events):
 *
 *   BOOT_SWEEP    : full-circle needle sweep on every gauge for ~1.1 s
 *      └─> WAIT_LINK if no OBD link & not mock mode
 *      └─> MAIN     directly if mock mode is enabled
 *
 *   WAIT_LINK     : "Waiting for OBD adapter" spinner overlay
 *      └─> MAIN    when obd_session reports READY (smooth pointer transition)
 *
 *   MAIN          : single full-screen gauge, KEY cycles, PWR-long opens menu
 *      └─> MENU    on PWR long press
 *
 *   MENU          : navigable list (Mock toggle, gauges, brightness, forget)
 *      └─> MAIN    on PWR long press / "Back" item
 *
 * @note All LVGL access goes through lv_vendor_disp_lock/unlock in callers.
 *       The UI refresh timer runs in the LVGL task itself, so its callback
 *       does not need additional locking.
 */
#include "ui.h"
#include "ui_gauge.h"
#include "ui_theme.h"
#include "app_config.h"
#include "app_metric.h"
#include "app_kv.h"
#include "app_mock.h"
#include "obd_session.h"
#include "lv_vendor.h"
#include "lvgl.h"
#include "tal_api.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define UI_REFRESH_PERIOD_MS    100
#define UI_NEEDLE_TRACK_MS      180   /**< slew during continuous tracking */
#define UI_NEEDLE_INTRO_MS      900   /**< slew on first sample after link/mock */
#define UI_BOOT_SWEEP_MS        APP_BOOT_SWEEP_MS
#define UI_BOOT_HOLD_MS         (UI_BOOT_SWEEP_MS + 100) /**< don't promote out
                                                              of BOOT_SWEEP until
                                                              the sweep is done */
#define UI_LINK_HINT_PULSE_MS   650
#define UI_LOG(fmt, ...)        PR_DEBUG("[ui] " fmt, ##__VA_ARGS__)

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef struct {
    APP_METRIC_E metric;
    const char  *title;
    const char  *unit;
    int32_t      v_min;       /* gauge dial min in user units */
    int32_t      v_max;       /* gauge dial max in user units */
    uint8_t      ticks;       /* major tick count */
    int32_t      scale_div;   /* divide bus value by this for user units */
} UI_GAUGE_DEF_T;

typedef enum {
    MENU_MOCK = 0,
    MENU_BRIGHT,
    MENU_GAUGES,
    MENU_FORGET,
    MENU_BACK,
    MENU_COUNT
} UI_MENU_E;

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC UI_STATE_E    s_state = UI_STATE_BOOT_SWEEP;

STATIC lv_obj_t     *s_screen = NULL;
STATIC lv_obj_t     *s_overlay = NULL;        /* BLE waiting overlay */
STATIC lv_obj_t     *s_overlay_label = NULL;
STATIC lv_obj_t     *s_overlay_dots = NULL;
STATIC lv_obj_t     *s_overlay_spinner = NULL;
STATIC lv_obj_t     *s_status_bar = NULL;     /* small status row top of dial */
STATIC lv_obj_t     *s_lbl_bt = NULL;         /* BT indicator */
STATIC lv_obj_t     *s_lbl_src = NULL;        /* MOCK / OBD tag */

STATIC lv_obj_t     *s_menu_root = NULL;
STATIC lv_obj_t     *s_menu_items[MENU_COUNT] = {NULL};
STATIC int           s_menu_cursor = 0;

STATIC UI_GAUGE_T    s_gauge;                 /* single live gauge */
STATIC int           s_curr_gauge_idx = 0;    /* index into s_gauge_defs */
STATIC BOOL_T        s_first_value_after_link = TRUE;
STATIC uint32_t      s_boot_elapsed_ms = 0;   /* time since BOOT_SWEEP entered */

STATIC lv_timer_t   *s_refresh_timer = NULL;
STATIC OBD_SES_STATE_E s_obd_state = OBD_SES_OFF;

STATIC const UI_GAUGE_DEF_T s_gauge_defs[APP_METRIC_COUNT] = {
    [APP_METRIC_WATER_TEMP]  = { APP_METRIC_WATER_TEMP,  "WATER",  "°C",   40, 130, 7,  10 },
    [APP_METRIC_OIL_TEMP]    = { APP_METRIC_OIL_TEMP,    "OIL T",  "°C",   40, 150, 7,  10 },
    [APP_METRIC_INTAKE_TEMP] = { APP_METRIC_INTAKE_TEMP, "IAT",    "°C",  -20, 100, 7,  10 },
    [APP_METRIC_FUEL_LEVEL]  = { APP_METRIC_FUEL_LEVEL,  "FUEL",   "%",     0, 100, 6,  10 },
    [APP_METRIC_OIL_PRESSURE]= { APP_METRIC_OIL_PRESSURE,"OIL P",  "kPa",   0, 800, 9,  10 },
    [APP_METRIC_VOLTAGE]     = { APP_METRIC_VOLTAGE,     "VOLT",   "V",     8,  16, 9, 1000 },
    [APP_METRIC_BOOST]       = { APP_METRIC_BOOST,       "BOOST",  "kPa", -100, 250, 8,  10 },
    [APP_METRIC_G_FORCE]     = { APP_METRIC_G_FORCE,     "G",      "g",  -200, 200, 9,  100 },
};

/* ---------------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------------- */
STATIC VOID_T __refresh_timer_cb(lv_timer_t *t);
STATIC VOID_T __build_overlay(VOID_T);
STATIC VOID_T __build_menu(VOID_T);
STATIC VOID_T __overlay_show(BOOL_T show);
STATIC VOID_T __overlay_set_state_text(OBD_SES_STATE_E st);
STATIC VOID_T __menu_show(BOOL_T show);
STATIC VOID_T __menu_redraw(VOID_T);
STATIC VOID_T __apply_current_gauge(BOOL_T animate_in);
STATIC int   __next_enabled_gauge(int from);
STATIC UI_STATE_E __compute_live_state(VOID_T);
STATIC VOID_T __enter_live_state(UI_STATE_E target);

/* ---------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------------- */
/**
 * @brief Compute the "live" state given current OBD link + mock pref.
 *
 * MAIN  if mock is on or OBD session is READY; otherwise WAIT_LINK.
 * Used when leaving MENU / promoting from BOOT_SWEEP, so any one entry
 * point makes the same decision.
 *
 * @return UI_STATE_MAIN or UI_STATE_WAIT_LINK
 */
STATIC UI_STATE_E __compute_live_state(VOID_T)
{
    const APP_PREFS_T *p = app_kv_prefs();
    BOOL_T mock_on = (p && p->mock_enabled) ? TRUE : FALSE;
    if (mock_on || s_obd_state == OBD_SES_READY) {
        return UI_STATE_MAIN;
    }
    return UI_STATE_WAIT_LINK;
}

/**
 * @brief Move into a "live" (non-menu, non-boot) state and update overlay.
 *
 * Centralises the overlay show/hide + first-sample flag so that any code
 * path (boot promote, menu close, mock toggle, OBD state change) ends up
 * with consistent visuals. Also re-arms the smooth needle intro animation
 * when entering MAIN from a non-MAIN state.
 *
 * @param[in] target UI_STATE_MAIN or UI_STATE_WAIT_LINK
 * @return none
 */
STATIC VOID_T __enter_live_state(UI_STATE_E target)
{
    if (target != UI_STATE_MAIN && target != UI_STATE_WAIT_LINK) {
        return;
    }
    UI_STATE_E prev = s_state;
    s_state = target;
    if (target == UI_STATE_MAIN) {
        if (prev != UI_STATE_MAIN) {
            s_first_value_after_link = TRUE;
        }
        __overlay_show(FALSE);
    } else {
        __overlay_set_state_text(s_obd_state);
        __overlay_show(TRUE);
    }
}

/**
 * @brief Get the user-units value for a metric (after divisor).
 *        Returns FALSE if the channel has no fresh data.
 */
STATIC BOOL_T __metric_user_value(const APP_METRIC_BUS_T *bus,
                                  const UI_GAUGE_DEF_T *def,
                                  int32_t *out_int, char *out_text, size_t cap)
{
    if (bus == NULL || def == NULL || !bus->valid[def->metric]) {
        if (out_text && cap) {
            snprintf(out_text, cap, "—");
        }
        return FALSE;
    }
    int32_t raw = 0;
    switch (def->metric) {
    case APP_METRIC_WATER_TEMP:   raw = bus->ect_c10;     break;
    case APP_METRIC_OIL_TEMP:     raw = bus->oil_c10;     break;
    case APP_METRIC_INTAKE_TEMP:  raw = bus->iat_c10;     break;
    case APP_METRIC_FUEL_LEVEL:   raw = bus->fuel_pct10;  break;
    case APP_METRIC_OIL_PRESSURE: raw = bus->oil_kpa10;   break;
    case APP_METRIC_VOLTAGE:      raw = bus->voltage_mv;  break;
    case APP_METRIC_BOOST:        raw = bus->boost_kpa10; break;
    case APP_METRIC_G_FORCE: {
        int32_t gx = bus->g_x_mg;
        int32_t gy = bus->g_y_mg;
        /* magnitude in milli-g, sign by larger axis */
        int32_t ax = (gx < 0) ? -gx : gx;
        int32_t ay = (gy < 0) ? -gy : gy;
        int32_t mag = (ax > ay) ? ax : ay;
        raw = (gy < 0) ? -mag : mag;
    } break;
    default: raw = 0; break;
    }
    int32_t user = raw / (def->scale_div ? def->scale_div : 1);
    if (out_int) {
        *out_int = user;
    }
    if (out_text && cap) {
        if (def->scale_div >= 100) {
            int frac = (raw < 0 ? -raw : raw) % def->scale_div;
            int frac2 = frac * 100 / def->scale_div;
            snprintf(out_text, cap, "%d.%02d", user, frac2);
        } else if (def->scale_div >= 10) {
            int frac = (raw < 0 ? -raw : raw) % def->scale_div;
            int frac1 = frac * 10 / def->scale_div;
            snprintf(out_text, cap, "%d.%d", user, frac1);
        } else {
            snprintf(out_text, cap, "%d", user);
        }
    }
    return TRUE;
}

/**
 * @brief Find next enabled gauge index starting after 'from' (wrap).
 */
STATIC int __next_enabled_gauge(int from)
{
    const APP_PREFS_T *p = app_kv_prefs();
    if (p == NULL) {
        return from;
    }
    for (int i = 1; i <= APP_METRIC_COUNT; i++) {
        int idx = (from + i) % APP_METRIC_COUNT;
        if (p->gauge_enabled_mask & (1u << idx)) {
            return idx;
        }
    }
    return from;
}

/**
 * @brief (Re)apply the current gauge config to the live widget.
 *
 * Reconfigures range/title/unit/ticks in-place via ui_gauge_set_def, which
 * is much cheaper than destroy+create and avoids a perceptible flash when
 * the user cycles gauges with the KEY button.
 *
 * @param[in] animate_in TRUE to smoothly slew the needle to the live value.
 * @return none
 */
STATIC VOID_T __apply_current_gauge(BOOL_T animate_in)
{
    const UI_GAUGE_DEF_T *d = &s_gauge_defs[s_curr_gauge_idx];
    if (s_gauge.root == NULL) {
        return;
    }

    ui_gauge_set_def(&s_gauge, d->title, d->unit,
                     d->v_min, d->v_max, d->ticks);

    if (!animate_in) {
        return;
    }
    APP_METRIC_BUS_T bus;
    if (app_metric_snapshot(&bus) != OPRT_OK) {
        return;
    }
    int32_t user_v = d->v_min;
    char txt[16] = {0};
    if (__metric_user_value(&bus, d, &user_v, txt, sizeof(txt))) {
        ui_gauge_set_value_text(&s_gauge, txt);
        ui_gauge_set_value(&s_gauge, user_v, UI_NEEDLE_INTRO_MS);
    } else {
        ui_gauge_set_value_text(&s_gauge, "—");
    }
}

/* ---------------------------------------------------------------------------
 * Overlay
 * --------------------------------------------------------------------------- */
/**
 * @brief Build the BLE wait overlay (initially hidden).
 */
STATIC VOID_T __build_overlay(VOID_T)
{
    s_overlay = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, APP_LCD_WIDTH, APP_LCD_HEIGHT);
    lv_obj_center(s_overlay);
    lv_obj_set_style_bg_color(s_overlay, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_70, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

    s_overlay_spinner = lv_spinner_create(s_overlay);
    lv_obj_set_size(s_overlay_spinner, 90, 90);
    lv_spinner_set_anim_params(s_overlay_spinner, 1500, 80);
    lv_obj_align(s_overlay_spinner, LV_ALIGN_CENTER, 0, -50);
    lv_obj_set_style_arc_color(s_overlay_spinner, UI_COLOR_ARC, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_overlay_spinner, UI_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_overlay_spinner, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_overlay_spinner, 6, LV_PART_INDICATOR);

    s_overlay_label = lv_label_create(s_overlay);
    lv_label_set_text(s_overlay_label, "Connecting OBD");
    lv_obj_set_style_text_color(s_overlay_label, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_overlay_label, &lv_font_montserrat_28, 0);
    lv_obj_align(s_overlay_label, LV_ALIGN_CENTER, 0, 30);

    s_overlay_dots = lv_label_create(s_overlay);
    lv_label_set_text(s_overlay_dots, "Searching for ELM327 BLE…");
    lv_obj_set_style_text_color(s_overlay_dots, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_overlay_dots, &lv_font_montserrat_16, 0);
    lv_obj_align(s_overlay_dots, LV_ALIGN_CENTER, 0, 70);
}

/**
 * @brief Show / hide the BLE wait overlay.
 */
STATIC VOID_T __overlay_show(BOOL_T show)
{
    if (s_overlay == NULL) {
        return;
    }
    if (show) {
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_overlay);
    } else {
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief Update the overlay sub-text based on the OBD session state.
 *        Always includes a hint so the user can reach the menu from
 *        WAIT_LINK to enable Mock Mode or trigger a rescan.
 */
STATIC VOID_T __overlay_set_state_text(OBD_SES_STATE_E st)
{
    if (s_overlay_label == NULL) {
        return;
    }
    static const char *HINT = "Hold PWR for menu  ·  KEY: switch gauge";
    switch (st) {
    case OBD_SES_OFF:
        lv_label_set_text(s_overlay_label, "OBD off");
        lv_label_set_text(s_overlay_dots,  HINT);
        break;
    case OBD_SES_SCAN:
        lv_label_set_text(s_overlay_label, "Connecting OBD");
        lv_label_set_text(s_overlay_dots,  HINT);
        break;
    case OBD_SES_LINKED:
        lv_label_set_text(s_overlay_label, "Configuring");
        lv_label_set_text(s_overlay_dots,  HINT);
        break;
    case OBD_SES_LINK_LOST:
        lv_label_set_text(s_overlay_label, "Link lost");
        lv_label_set_text(s_overlay_dots,  HINT);
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------------------
 * Menu
 * --------------------------------------------------------------------------- */
/**
 * @brief Build the menu screen (initially hidden).
 */
STATIC VOID_T __build_menu(VOID_T)
{
    s_menu_root = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_menu_root);
    lv_obj_set_size(s_menu_root, APP_LCD_WIDTH, APP_LCD_HEIGHT);
    lv_obj_center(s_menu_root);
    lv_obj_set_style_bg_color(s_menu_root, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_menu_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_menu_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_menu_root, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = lv_label_create(s_menu_root);
    lv_label_set_text(title, "MENU");
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 70);

    static const char *labels[MENU_COUNT] = {
        "Mock Mode",
        "Brightness +",
        "Cycle Gauges",
        "Forget Adapter",
        "Back",
    };
    int y = 150;
    for (int i = 0; i < MENU_COUNT; i++) {
        lv_obj_t *row = lv_label_create(s_menu_root);
        lv_label_set_text(row, labels[i]);
        lv_obj_set_style_text_font(row, &lv_font_montserrat_22, 0);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
        s_menu_items[i] = row;
        y += 38;
    }
}

/**
 * @brief Re-render menu items, highlighting the current cursor row.
 */
STATIC VOID_T __menu_redraw(VOID_T)
{
    if (s_menu_root == NULL) {
        return;
    }
    const APP_PREFS_T *p = app_kv_prefs();

    char buf[64];
    for (int i = 0; i < MENU_COUNT; i++) {
        if (s_menu_items[i] == NULL) {
            continue;
        }
        switch ((UI_MENU_E)i) {
        case MENU_MOCK:
            snprintf(buf, sizeof(buf), "Mock Mode : %s",
                     (p && p->mock_enabled) ? "ON" : "OFF");
            break;
        case MENU_BRIGHT:
            snprintf(buf, sizeof(buf), "Brightness : %d%%",
                     p ? p->brightness_pct : 0);
            break;
        case MENU_GAUGES: {
            int n = 0;
            if (p) {
                for (int k = 0; k < APP_METRIC_COUNT; k++) {
                    if (p->gauge_enabled_mask & (1u << k)) {
                        n++;
                    }
                }
            }
            snprintf(buf, sizeof(buf), "Gauges Enabled : %d/%d", n, APP_METRIC_COUNT);
        } break;
        case MENU_FORGET:
            snprintf(buf, sizeof(buf), "Forget Adapter : %s",
                     (p && p->bound_addr_valid) ? "saved" : "(none)");
            break;
        case MENU_BACK:
            snprintf(buf, sizeof(buf), "Back");
            break;
        default:
            buf[0] = '\0';
            break;
        }
        lv_label_set_text(s_menu_items[i], buf);
        lv_obj_set_style_text_color(s_menu_items[i],
            (i == s_menu_cursor) ? UI_COLOR_PRIMARY : UI_COLOR_TEXT_DIM, 0);
    }
}

/**
 * @brief Show / hide the menu.
 */
STATIC VOID_T __menu_show(BOOL_T show)
{
    if (s_menu_root == NULL) {
        return;
    }
    if (show) {
        s_menu_cursor = 0;
        __menu_redraw();
        lv_obj_clear_flag(s_menu_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_menu_root);
    } else {
        lv_obj_add_flag(s_menu_root, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------------------------------------------------------------------------
 * Status bar
 * --------------------------------------------------------------------------- */
/**
 * @brief Build a small status bar at the very top of the screen
 *        (BT icon + data source tag).
 */
STATIC VOID_T __build_status_bar(VOID_T)
{
    s_status_bar = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_status_bar);
    lv_obj_set_size(s_status_bar, APP_LCD_WIDTH, 40);
    lv_obj_align(s_status_bar, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_clear_flag(s_status_bar, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_bt = lv_label_create(s_status_bar);
    lv_label_set_text(s_lbl_bt, LV_SYMBOL_BLUETOOTH " --");
    lv_obj_set_style_text_color(s_lbl_bt, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_lbl_bt, &lv_font_montserrat_16, 0);
    lv_obj_align(s_lbl_bt, LV_ALIGN_LEFT_MID, 60, 0);

    s_lbl_src = lv_label_create(s_status_bar);
    lv_label_set_text(s_lbl_src, "—");
    lv_obj_set_style_text_color(s_lbl_src, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_lbl_src, &lv_font_montserrat_16, 0);
    lv_obj_align(s_lbl_src, LV_ALIGN_RIGHT_MID, -60, 0);
}

/**
 * @brief Update the BT status text and source tag.
 */
STATIC VOID_T __status_refresh(VOID_T)
{
    if (s_lbl_bt) {
        const char *txt = LV_SYMBOL_BLUETOOTH " --";
        switch (s_obd_state) {
        case OBD_SES_OFF:       txt = LV_SYMBOL_BLUETOOTH " off";   break;
        case OBD_SES_SCAN:      txt = LV_SYMBOL_BLUETOOTH " scan";  break;
        case OBD_SES_LINKED:    txt = LV_SYMBOL_BLUETOOTH " init";  break;
        case OBD_SES_READY:     txt = LV_SYMBOL_BLUETOOTH " ok";    break;
        case OBD_SES_LINK_LOST: txt = LV_SYMBOL_BLUETOOTH " lost";  break;
        }
        lv_label_set_text(s_lbl_bt, txt);
        lv_obj_set_style_text_color(s_lbl_bt,
            (s_obd_state == OBD_SES_READY) ? UI_COLOR_OK : UI_COLOR_TEXT_DIM, 0);
    }
    if (s_lbl_src) {
        APP_DATA_SRC_E src = app_metric_get_source();
        const char *t = "—";
        if (src == APP_DATA_SRC_OBD)  t = "OBD";
        if (src == APP_DATA_SRC_MOCK) t = "MOCK";
        if (src == APP_DATA_SRC_IMU)  t = "IMU";
        lv_label_set_text(s_lbl_src, t);
        lv_obj_set_style_text_color(s_lbl_src,
            (src == APP_DATA_SRC_OBD) ? UI_COLOR_OK : UI_COLOR_ACCENT, 0);
    }
}

/* ---------------------------------------------------------------------------
 * Refresh timer
 * --------------------------------------------------------------------------- */
/**
 * @brief Periodic UI refresh (LVGL task context).
 *
 * 1. Update status bar
 * 2. Live-update the current gauge needle/value (with smooth slew)
 * 3. Auto-promote BOOT_SWEEP -> WAIT_LINK / MAIN
 */
STATIC VOID_T __refresh_timer_cb(lv_timer_t *t)
{
    (void)t;
    __status_refresh();

    APP_METRIC_BUS_T bus;
    BOOL_T have_bus = (app_metric_snapshot(&bus) == OPRT_OK);

    /* Boot sweep: hold this state until the visual sweep has actually
     * had time to play out. Otherwise the very first tick (~100 ms after
     * ui_init) would snap the needle and cancel the sweep animation. */
    if (s_state == UI_STATE_BOOT_SWEEP) {
        s_boot_elapsed_ms += UI_REFRESH_PERIOD_MS;
        if (s_boot_elapsed_ms < UI_BOOT_HOLD_MS) {
            return;
        }
        __enter_live_state(__compute_live_state());
        return;
    }

    /* Wait link: keep overlay text fresh, promote on link/mock change. */
    if (s_state == UI_STATE_WAIT_LINK) {
        __overlay_set_state_text(s_obd_state);
        __enter_live_state(__compute_live_state());
        return;
    }

    /* Menu: don't touch the gauge, but do keep gauge def in sync if the
     * cursor was on the "Cycle Gauges" item etc. */
    if (s_state == UI_STATE_MENU) {
        return;
    }

    /* Main: refresh the live gauge */
    if (s_state == UI_STATE_MAIN && have_bus && s_gauge.root) {
        const UI_GAUGE_DEF_T *d = &s_gauge_defs[s_curr_gauge_idx];
        int32_t user_v = d->v_min;
        char txt[16] = {0};
        if (__metric_user_value(&bus, d, &user_v, txt, sizeof(txt))) {
            ui_gauge_set_value_text(&s_gauge, txt);
            uint32_t slew = s_first_value_after_link
                                ? UI_NEEDLE_INTRO_MS
                                : UI_NEEDLE_TRACK_MS;
            ui_gauge_set_value(&s_gauge, user_v, slew);
            s_first_value_after_link = FALSE;
        } else {
            ui_gauge_set_value_text(&s_gauge, "—");
        }
    }

    /* Lost link / mock turned off → bounce back to overlay. */
    if (s_state == UI_STATE_MAIN) {
        UI_STATE_E live = __compute_live_state();
        if (live == UI_STATE_WAIT_LINK) {
            __enter_live_state(live);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */
/**
 * @brief Build LVGL hierarchy + start refresh timer.
 */
OPERATE_RET ui_init(VOID_T)
{
    s_screen = lv_screen_active();
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_style_bg_color(s_screen, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Choose initial gauge based on prefs */
    const APP_PREFS_T *p = app_kv_prefs();
    if (p && p->current_gauge < APP_METRIC_COUNT &&
        (p->gauge_enabled_mask & (1u << p->current_gauge))) {
        s_curr_gauge_idx = p->current_gauge;
    } else {
        s_curr_gauge_idx = 0;
        s_curr_gauge_idx = __next_enabled_gauge(APP_METRIC_COUNT - 1);
    }

    const UI_GAUGE_DEF_T *d = &s_gauge_defs[s_curr_gauge_idx];
    OPERATE_RET rt = ui_gauge_create(&s_gauge, s_screen, d->title, d->unit,
                                     d->v_min, d->v_max, d->ticks);
    if (rt != OPRT_OK) {
        return rt;
    }

    __build_status_bar();
    __build_overlay();
    __build_menu();

    /* Boot animation */
    ui_gauge_sweep(&s_gauge, UI_BOOT_SWEEP_MS);

    s_state = UI_STATE_BOOT_SWEEP;
    s_boot_elapsed_ms = 0;
    s_refresh_timer = lv_timer_create(__refresh_timer_cb,
                                      UI_REFRESH_PERIOD_MS, NULL);
    return OPRT_OK;
}

/**
 * @brief Inform the UI of an OBD state change.
 *        @note must be called from LVGL task or with lv_vendor_disp_lock.
 *
 * Safe to call in any state. While the menu is open we just update the
 * cached state; the right "live" state is re-computed when the menu
 * closes. While in BOOT_SWEEP we let the boot-promote logic handle it.
 */
void ui_on_obd_state(OBD_SES_STATE_E st)
{
    s_obd_state = st;
    if (s_state == UI_STATE_MENU || s_state == UI_STATE_BOOT_SWEEP) {
        return;
    }
    if (s_state == UI_STATE_WAIT_LINK) {
        __overlay_set_state_text(st);
    }
    __enter_live_state(__compute_live_state());
}

/**
 * @brief KEY handler — cycle gauges, or move cursor in MENU.
 *
 * Allowed in MAIN (animate to live value), WAIT_LINK (preview which
 * gauge will appear when OBD comes online), and MENU (cursor down).
 * Ignored during BOOT_SWEEP to keep the boot animation clean.
 */
void ui_show_next_gauge(VOID_T)
{
    if (s_state == UI_STATE_MENU) {
        s_menu_cursor = (s_menu_cursor + 1) % MENU_COUNT;
        __menu_redraw();
        return;
    }
    if (s_state != UI_STATE_MAIN && s_state != UI_STATE_WAIT_LINK) {
        return;
    }
    s_curr_gauge_idx = __next_enabled_gauge(s_curr_gauge_idx);
    app_kv_set_current_gauge((APP_METRIC_E)s_curr_gauge_idx);
    /* In WAIT_LINK there's no live data yet, so don't try to animate-in:
     * just reconfigure the dial so the user sees the new title/range
     * once the overlay clears. */
    __apply_current_gauge(s_state == UI_STATE_MAIN);
}

/**
 * @brief Toggle the menu open/closed.
 *
 * Reachable from any "live" state (MAIN or WAIT_LINK), so the user can
 * always get to Mock Mode / rescan even when the BLE link is stuck.
 * On close the right live state (MAIN or WAIT_LINK) is recomputed.
 */
void ui_toggle_menu(VOID_T)
{
    if (s_state == UI_STATE_MENU) {
        __menu_show(FALSE);
        /* Pick the right live state based on current OBD/mock; this also
         * decides if the overlay should reappear and re-arms the smooth
         * intro animation when we end up in MAIN. */
        __enter_live_state(__compute_live_state());
        return;
    }
    if (s_state == UI_STATE_BOOT_SWEEP) {
        return;     /* let the sweep finish first */
    }
    /* Hide the overlay so the menu shows on a clean background. */
    __overlay_show(FALSE);
    s_state = UI_STATE_MENU;
    __menu_show(TRUE);
}

/**
 * @brief PWR-short handler: in MENU activates the highlighted row.
 *        In MAIN/WAIT_LINK it's a no-op (PWR-long is the menu trigger).
 */
void ui_handle_pwr_short(VOID_T)
{
    if (s_state != UI_STATE_MENU) {
        return;
    }
    switch ((UI_MENU_E)s_menu_cursor) {
    case MENU_MOCK: {
        const APP_PREFS_T *p = app_kv_prefs();
        BOOL_T en = (p && p->mock_enabled) ? FALSE : TRUE;
        app_kv_set_mock_enabled(en);
        ui_on_mock_changed(en);
    } break;
    case MENU_BRIGHT: {
        const APP_PREFS_T *p = app_kv_prefs();
        uint8_t b = p ? p->brightness_pct : 50;
        b = (b >= 100) ? 20 : (b + 20);
        app_kv_set_brightness(b);
    } break;
    case MENU_GAUGES: {
        /* Cursor advance to next item; toggling individual gauges
         * is done via the KEY button while highlighted. */
        s_menu_cursor = (s_menu_cursor + 1) % MENU_COUNT;
    } break;
    case MENU_FORGET:
        app_kv_clear_bound_addr();
        obd_session_rescan();
        break;
    case MENU_BACK:
        ui_toggle_menu();   /* close menu, restore live state */
        return;
    default:
        break;
    }
    __menu_redraw();
    obd_session_refresh_poll_list();
}

/**
 * @brief Mock pref changed: kick the mock task and update the live state.
 *
 * Safe in any state — when we're inside the MENU we just keep the cached
 * OBD state and update the mock task; the next ui_toggle_menu() close
 * will move us into MAIN (mock on) or WAIT_LINK (mock off, no link).
 */
void ui_on_mock_changed(BOOL_T enabled)
{
    if (enabled) {
        app_mock_start();
    } else {
        app_mock_stop();
        app_metric_invalidate_obd();
    }
    if (s_state == UI_STATE_MENU || s_state == UI_STATE_BOOT_SWEEP) {
        return;
    }
    __enter_live_state(__compute_live_state());
}

/**
 * @brief Get state.
 */
UI_STATE_E ui_state(VOID_T)
{
    return s_state;
}
