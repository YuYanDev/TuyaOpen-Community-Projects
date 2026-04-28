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
 *   MAIN          : single full-screen gauge
 *      └─> MENU    on PWR short press (toggle)
 *
 *   MENU          : touch-only menu (Mock toggle, brightness, forget, close)
 *      └─> MAIN/WAIT_LINK on PWR short press (toggle) or "Close" item
 *
 * Button mapping (final):
 *   PWR short  : toggle menu (any state except BOOT_SWEEP)
 *   PWR long3s : graceful shutdown
 *   KEY short  : switch to next enabled gauge (MAIN / WAIT_LINK only)
 *
 * Inside the menu, only the touchscreen is used to manipulate items; the
 * KEY button is intentionally inert to keep "KEY = switch gauge" as a
 * single-purpose mental model.
 *
 * @note All LVGL access goes through lv_vendor_disp_lock/unlock in callers.
 *       The UI refresh timer runs in the LVGL task itself, so its callback
 *       does not need additional locking.
 */
#include "ui.h"
#include "ui_gauge.h"
#include "ui_gforce.h"
#include "ui_theme.h"
#include "app_config.h"
#include "app_metric.h"
#include "app_kv.h"
#include "app_i18n.h"
#include "app_mock.h"
#include "obd_session.h"
#include "sensor_imu.h"
#include "lv_vendor.h"
#include "lvgl.h"
#include "tal_api.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
/* UI data-refresh period.
 *
 * v1.8 used 100 ms (10 Hz). With the tracker at 125 Hz / τ ≈ 65 ms,
 * the needle reached ≥85% of each new target within one refresh
 * period — visible as a discrete "lurch" at every tick rather than
 * smooth motion. The user reported "在两次数据中间需要插值".
 *
 * v1.8.2 drops to 33 ms (~30 Hz). The 200 Hz tracker now sees a new
 * target ~6 times per τ, so the EMA traces a low-pass version of the
 * input sequence as a continuous curve. The center value text label
 * also updates at 30 Hz so digit changes feel instant rather than
 * deliberate.
 *
 * Cost: __refresh_timer_cb runs 3× more often. The body is cheap
 * (one app_metric_snapshot + one set_value), so this stays well under
 * the LVGL task budget. */
#define UI_REFRESH_PERIOD_MS    33
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
    MENU_GCAL,           /**< Calibrate G zero (per-orientation static bias) */
    MENU_ORIENT,         /**< Cycle device mounting orientation (5 presets) */
    MENU_BT_MODE,        /**< Toggle OBD transport: BLE 4.0 / BT-Classic SPP */
    MENU_BT_PAIR,        /**< Trigger SPP legacy PIN pairing (1234 / 0000) */
    MENU_LANG,           /**< Cycle UI language (English / 中文) */
    MENU_FORGET,
    MENU_CLOSE,
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

STATIC lv_obj_t     *s_menu_root = NULL;
STATIC lv_obj_t     *s_menu_title = NULL;
STATIC lv_obj_t     *s_menu_items[MENU_COUNT]  = {NULL};
STATIC lv_obj_t     *s_menu_labels[MENU_COUNT] = {NULL};

STATIC UI_GAUGE_T    s_gauge;                 /* dial gauge for non-G metrics */
STATIC UI_GFORCE_T   s_gforce;                /* GoPro-style G target reticle */
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
STATIC VOID_T __menu_item_clicked(lv_event_t *e);
STATIC VOID_T __apply_current_gauge(BOOL_T animate_in);
STATIC int    __next_enabled_gauge(int from);
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
        /* v1.8: feed off the orient-projected axes so the magnitude is
         * truly vehicle-centric — independent of how the device is
         * physically mounted. Sign convention: forward (+) is "nose
         * accelerating" (i.e. the gas pedal direction), so a hard
         * brake reads negative on the dial. */
        int32_t gfwd = bus->g_fwd_mg;
        int32_t glat = bus->g_lat_mg;
        int32_t af = (gfwd < 0) ? -gfwd : gfwd;
        int32_t al = (glat < 0) ? -glat : glat;
        int32_t mag = (af > al) ? af : al;
        raw = (gfwd < 0) ? -mag : mag;
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
 * For all dial metrics this reconfigures range/title/unit/ticks in-place
 * via ui_gauge_set_def() (much cheaper than destroy+create). For the G
 * metric we hide the dial and show the GoPro target reticle instead.
 *
 * @param[in] animate_in TRUE to smoothly slew the needle to the live value.
 * @return none
 */
STATIC VOID_T __apply_current_gauge(BOOL_T animate_in)
{
    const UI_GAUGE_DEF_T *d = &s_gauge_defs[s_curr_gauge_idx];

    BOOL_T is_gforce = (d->metric == APP_METRIC_G_FORCE);

    if (is_gforce) {
        ui_gauge_set_visible(&s_gauge, FALSE);
        ui_gforce_set_visible(&s_gforce, TRUE);
        ui_gforce_set_uncalibrated_hint(&s_gforce,
                                        sensor_imu_calibration_active() ? FALSE : TRUE);
        return;
    }

    /* Non-G dial gauge */
    ui_gforce_set_visible(&s_gforce, FALSE);
    ui_gauge_set_visible(&s_gauge, TRUE);
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
    lv_obj_set_style_text_color(s_overlay_label, UI_COLOR_TEXT, 0);
    /* font_default tracks the active language: Montserrat 16 for EN,
     * project-bundled NotoSansSC subset (lv_font_zh_16) for ZH. We
     * keep the same 16-px size for both so the metrics align after a
     * lang flip — switching between Latin Montserrat and CJK Noto at
     * different sizes would re-flow the overlay every time. */
    lv_obj_set_style_text_font(s_overlay_label, app_i18n_font_default(), 0);
    lv_label_set_text(s_overlay_label, app_i18n_get(STR_OVL_CONNECTING));
    lv_obj_align(s_overlay_label, LV_ALIGN_CENTER, 0, 30);

    s_overlay_dots = lv_label_create(s_overlay);
    lv_obj_set_style_text_color(s_overlay_dots, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_overlay_dots, app_i18n_font_default(), 0);
    lv_label_set_text(s_overlay_dots, app_i18n_get(STR_OVL_SEARCHING));
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
    const char *hint = app_i18n_get(STR_OVL_HINT);
    APP_STR_E hdr_id = STR_OVL_CONNECTING;
    switch (st) {
    case OBD_SES_OFF:        hdr_id = STR_OVL_OBD_OFF;     break;
    case OBD_SES_SCAN:       hdr_id = STR_OVL_CONNECTING;  break;
    case OBD_SES_LINKED:     hdr_id = STR_OVL_CONFIGURING; break;
    case OBD_SES_LINK_LOST:  hdr_id = STR_OVL_LINK_LOST;   break;
    default: return;
    }
    lv_label_set_text(s_overlay_label, app_i18n_get(hdr_id));
    lv_label_set_text(s_overlay_dots,  hint);
}

/* ---------------------------------------------------------------------------
 * Menu (touchscreen-only; KEY/PWR don't navigate inside it)
 * --------------------------------------------------------------------------- */
/**
 * @brief LV_EVENT_CLICKED handler shared by all menu rows. The row index
 *        was stored in user_data when the row was created.
 *
 * @param[in] e LVGL event handle
 * @return none
 * @note Runs in LVGL task context (touch input is dispatched there) so no
 *       additional locking is required when calling app_kv / obd_session.
 */
STATIC VOID_T __menu_item_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= MENU_COUNT) {
        return;
    }

    switch ((UI_MENU_E)idx) {
    case MENU_MOCK: {
        const APP_PREFS_T *p = app_kv_prefs();
        BOOL_T en = (p && p->mock_enabled) ? FALSE : TRUE;
        app_kv_set_mock_enabled(en);
        ui_on_mock_changed(en);
    } break;
    case MENU_BRIGHT: {
        const APP_PREFS_T *p = app_kv_prefs();
        uint8_t b = p ? p->brightness_pct : 50;
        b = (b >= 100) ? 20 : (uint8_t)(b + 20);
        app_kv_set_brightness(b);
    } break;
    case MENU_GCAL: {
        /* Capture the current static gravity vector as the new zero. The
         * IMU sample loop services the request on its next 20 ms tick
         * and persists into KV — see sensor_imu_calibrate_zero(). */
        OPERATE_RET rt = sensor_imu_calibrate_zero();
        if (rt != OPRT_OK) {
            UI_LOG("calibrate-zero rejected (rt=%d) — IMU not running", rt);
        }
    } break;
    case MENU_ORIENT: {
        /* Cycle through the 5 mounting orientations. The IMU sampler
         * picks up the new value on its next 20 ms tick (it reads the
         * KV pointer each iteration), so the GoPro ball will start
         * responding to the new fwd/lat axes immediately. The user
         * should re-tap "Calibrate G" after switching pose so static
         * gravity bias is zeroed for the new orientation. */
        const APP_PREFS_T *p = app_kv_prefs();
        uint8_t cur = p ? p->g_orient : (uint8_t)APP_G_ORIENT_FACE_UP;
        uint8_t nxt = (uint8_t)((cur + 1) % (uint8_t)APP_G_ORIENT_COUNT);
        app_kv_set_g_orient((APP_G_ORIENT_E)nxt);
    } break;
    case MENU_BT_MODE: {
        /* Toggle BLE ↔ SPP and ask the OBD session to re-pick the
         * backend on its next iteration. For v1.8 the SPP backend is
         * a stub that surfaces NOT_SUPPORTED to the overlay, so the
         * user can experiment with the toggle freely without
         * "bricking" their OBD link — flipping back to BLE always
         * restores the working transport. */
        const APP_PREFS_T *p = app_kv_prefs();
        uint8_t cur = p ? p->bt_mode : (uint8_t)OBD_BT_MODE_BLE;
        uint8_t nxt = (uint8_t)((cur + 1) % (uint8_t)OBD_BT_MODE_COUNT);
        app_kv_set_bt_mode((OBD_BT_MODE_E)nxt);
        obd_session_rescan();
    } break;
    case MENU_BT_PAIR: {
        /* SPP legacy PIN entry. The v1.8 stub backend can't actually
         * pair, but the menu row still triggers a rescan so the
         * upcoming v1.9 implementation will Just Work without UI
         * changes. Users on BLE see a "not applicable" hint. */
        const APP_PREFS_T *p = app_kv_prefs();
        if (p && p->bt_mode == (uint8_t)OBD_BT_MODE_SPP) {
            obd_session_rescan();
        } else {
            UI_LOG("BT pair: ignored (current backend is BLE, no PIN needed)");
        }
    } break;
    case MENU_LANG: {
        /* Cycle EN <-> ZH. v1.8.1 uses a single unified font
         * (lv_font_zh_16, NotoSansSC subset) for both languages
         * because the MENU_LANG row deliberately shows the OTHER
         * language's label and we can't dispatch font per-row
         * cleanly — see app_i18n_font_default() for the rationale.
         * The font reflow loop below is now a defensive no-op (the
         * font already covers both scripts) but kept for symmetry
         * with future per-language-font experiments. */
        APP_LANG_E cur = app_i18n_lang();
        APP_LANG_E nxt = (cur == APP_LANG_EN) ? APP_LANG_ZH : APP_LANG_EN;
        app_i18n_set_lang(nxt);
        const lv_font_t *f = app_i18n_font_default();
        if (s_menu_title) {
            lv_obj_set_style_text_font(s_menu_title, f, 0);
        }
        for (int j = 0; j < MENU_COUNT; j++) {
            if (s_menu_labels[j]) {
                lv_obj_set_style_text_font(s_menu_labels[j], f, 0);
            }
        }
        if (s_overlay_label) {
            lv_obj_set_style_text_font(s_overlay_label, f, 0);
        }
        if (s_overlay_dots) {
            lv_obj_set_style_text_font(s_overlay_dots, f, 0);
        }
    } break;
    case MENU_FORGET:
        app_kv_clear_bound_addr();
        obd_session_rescan();
        break;
    case MENU_CLOSE:
        ui_toggle_menu();   /* close + restore live state */
        return;
    default:
        break;
    }
    __menu_redraw();
    obd_session_refresh_poll_list();
}

/**
 * @brief Build the menu screen (initially hidden). Each row is a clickable
 *        rounded card with a centred label.
 *
 * @return none
 */
STATIC VOID_T __build_menu(VOID_T)
{
    s_menu_root = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_menu_root);
    lv_obj_set_size(s_menu_root, APP_LCD_WIDTH, APP_LCD_HEIGHT);
    lv_obj_center(s_menu_root);
    lv_obj_set_style_bg_color(s_menu_root, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_menu_root, LV_OPA_COVER, 0);
    /* v1.8 — enable vertical touch-scroll. With 8 rows (P5C added BT
     * mode + BT pair) we exceed the round display's inscribed safe
     * area, so we rely on the user dragging the list. The scrollbar
     * is left at LVGL defaults (auto-hide) which feels native on a
     * round AMOLED. */
    lv_obj_add_flag(s_menu_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_menu_root, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_menu_root, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_menu_root, LV_OBJ_FLAG_HIDDEN);

    s_menu_title = lv_label_create(s_menu_root);
    lv_obj_set_style_text_color(s_menu_title, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_menu_title, app_i18n_font_default(), 0);
    lv_label_set_text(s_menu_title, app_i18n_get(STR_MENU_TITLE));
    lv_obj_align(s_menu_title, LV_ALIGN_TOP_MID, 0, 30);

    /* Round display: 8 rows (Mock / Bright / GCal / Orient / BT mode /
     * BT pair / Forget / Close). Even the first 6 fit the inscribed
     * 233 px circle by themselves; the remaining 2 spill below the
     * visible area and become reachable by dragging the list up.
     * ROW_W is shrunk to 290 px so the rounded corners land inside
     * the rim at the top and bottom edges of the visible portion. */
    static const int ROW_W = 290;
    static const int ROW_H = 44;
    static const int ROW_GAP = 6;
    int y = 70;
    int i;
    for (i = 0; i < MENU_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(s_menu_root);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, ROW_W, ROW_H);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_set_style_bg_color(row, UI_COLOR_HUB, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 16, 0);
        lv_obj_set_style_pad_all(row, 8, 0);
        lv_obj_set_style_border_color(row, UI_COLOR_TICK_DIM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        /* Touch feedback: brighten on press */
        lv_obj_set_style_bg_color(row, UI_COLOR_PRIMARY, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(row, UI_COLOR_PRIMARY, LV_STATE_PRESSED);

        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(row);
        lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT, 0);
        /* Font is sourced from i18n so a language flip immediately
         * picks up the right glyph table (Montserrat 16 for Latin,
         * lv_font_zh_16 — NotoSansSC subset — for Chinese). Same
         * metrics for both, so the row layout doesn't shift. */
        lv_obj_set_style_text_font(lbl, app_i18n_font_default(), 0);
        lv_label_set_text(lbl, "");
        lv_obj_center(lbl);

        s_menu_items[i]  = row;
        s_menu_labels[i] = lbl;

        lv_obj_add_event_cb(row, __menu_item_clicked,
                            LV_EVENT_CLICKED, (void *)(intptr_t)i);

        y += ROW_H + ROW_GAP;
    }
}

/**
 * @brief Re-render the menu item labels with the current preferences.
 *
 * Pulls every visible string through app_i18n_get() so a Chinese
 * lang flip via MENU_LANG instantly retranslates the entire menu.
 * The runtime values (ON/OFF, brightness %, …) stay Latin to match
 * the dial labels and avoid the CJK font having to cover digits
 * outside the project-bundled NotoSansSC subset's char list.
 *
 * @return none
 */
STATIC VOID_T __menu_redraw(VOID_T)
{
    if (s_menu_root == NULL) {
        return;
    }
    const APP_PREFS_T *p = app_kv_prefs();

    if (s_menu_title) {
        lv_label_set_text(s_menu_title, app_i18n_get(STR_MENU_TITLE));
    }

    char buf[80];
    int i;
    for (i = 0; i < MENU_COUNT; i++) {
        if (s_menu_labels[i] == NULL) {
            continue;
        }
        switch ((UI_MENU_E)i) {
        case MENU_MOCK:
            snprintf(buf, sizeof(buf), "%s%s",
                     app_i18n_get(STR_MENU_MOCK),
                     (p && p->mock_enabled) ? "ON" : "OFF");
            break;
        case MENU_BRIGHT:
            snprintf(buf, sizeof(buf), "%s%d%%",
                     app_i18n_get(STR_MENU_BRIGHT),
                     p ? p->brightness_pct : 0);
            break;
        case MENU_GCAL: {
            BOOL_T cal_on = sensor_imu_calibration_active();
            snprintf(buf, sizeof(buf), "%s%s",
                     app_i18n_get(STR_MENU_GCAL),
                     app_i18n_get(cal_on ? STR_MENU_GCAL_HINT_SAVED
                                         : STR_MENU_GCAL_HINT_TAP));
        } break;
        case MENU_ORIENT: {
            static const APP_STR_E k_orient_ids[APP_G_ORIENT_COUNT] = {
                [APP_G_ORIENT_FACE_UP]  = STR_ORIENT_FACE_UP,
                [APP_G_ORIENT_USER_0]   = STR_ORIENT_USER_0,
                [APP_G_ORIENT_USER_90]  = STR_ORIENT_USER_90,
                [APP_G_ORIENT_USER_180] = STR_ORIENT_USER_180,
                [APP_G_ORIENT_USER_270] = STR_ORIENT_USER_270,
            };
            uint8_t o = p ? p->g_orient : (uint8_t)APP_G_ORIENT_FACE_UP;
            if (o >= APP_G_ORIENT_COUNT) {
                o = (uint8_t)APP_G_ORIENT_FACE_UP;
            }
            snprintf(buf, sizeof(buf), "%s%s",
                     app_i18n_get(STR_MENU_ORIENT),
                     app_i18n_get(k_orient_ids[o]));
        } break;
        case MENU_BT_MODE: {
            const char *io_name = "—";
            BOOL_T io_unsupported = FALSE;
            obd_session_io_status(&io_name, &io_unsupported);
            uint8_t m = p ? p->bt_mode : (uint8_t)OBD_BT_MODE_BLE;
            const char *want = (m == (uint8_t)OBD_BT_MODE_SPP) ? "SPP" : "BLE";
            snprintf(buf, sizeof(buf), "%s%s%s",
                     app_i18n_get(STR_MENU_BT_MODE),
                     want,
                     io_unsupported ? app_i18n_get(STR_BT_STUB_SUFFIX) : "");
        } break;
        case MENU_BT_PAIR: {
            uint8_t m = p ? p->bt_mode : (uint8_t)OBD_BT_MODE_BLE;
            snprintf(buf, sizeof(buf), "%s",
                     app_i18n_get((m == (uint8_t)OBD_BT_MODE_SPP)
                                       ? STR_MENU_BT_PAIR_SPP
                                       : STR_MENU_BT_PAIR_BLE));
        } break;
        case MENU_LANG: {
            /* Show the OTHER language's name in its own script so the
             * action of tapping the row is obvious: tapping "中文"
             * switches to Chinese, tapping "EN" switches to English.
             * (More natural UX than showing the current language.) */
            APP_LANG_E cur = app_i18n_lang();
            const char *next_label = (cur == APP_LANG_EN)
                ? "\xE8\xAF\xAD\xE8\xA8\x80\xEF\xBC\x9A\xE4\xB8\xAD\xE6\x96\x87"  /* 语言：中文 */
                : "Language: EN";
            snprintf(buf, sizeof(buf), "%s", next_label);
        } break;
        case MENU_FORGET:
            snprintf(buf, sizeof(buf), "%s%s",
                     app_i18n_get(STR_MENU_FORGET),
                     app_i18n_get((p && p->bound_addr_valid)
                                       ? STR_MENU_FORGET_SAVED
                                       : STR_MENU_FORGET_NONE));
            break;
        case MENU_CLOSE:
            snprintf(buf, sizeof(buf), "%s", app_i18n_get(STR_MENU_CLOSE));
            break;
        default:
            buf[0] = '\0';
            break;
        }
        lv_label_set_text(s_menu_labels[i], buf);
    }
}

/**
 * @brief Show / hide the menu.
 *
 * @param[in] show TRUE to show
 * @return none
 */
STATIC VOID_T __menu_show(BOOL_T show)
{
    if (s_menu_root == NULL) {
        return;
    }
    if (show) {
        __menu_redraw();
        lv_obj_clear_flag(s_menu_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_menu_root);
    } else {
        lv_obj_add_flag(s_menu_root, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------------------------------------------------------------------------
 * Refresh timer
 * --------------------------------------------------------------------------- */
/**
 * @brief Periodic UI refresh (LVGL task context).
 *
 * 1. Live-update the current gauge needle/value (with smooth slew)
 * 2. Auto-promote BOOT_SWEEP -> WAIT_LINK / MAIN
 *
 * The previous status bar (BT icon + MOCK tag at the top) was removed —
 * those small white/yellow labels at the 11/1 o'clock dial corners
 * looked like artefacts from a few feet away. BLE state is conveyed via
 * the wait-link overlay; data source is conveyed via the menu.
 */
STATIC VOID_T __refresh_timer_cb(lv_timer_t *t)
{
    (void)t;

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

    /* Main: refresh the live widget. The G metric drives the GoPro target
     * reticle directly with the (post-calibration) (gx, gy); every other
     * metric goes through the dial gauge. */
    if (s_state == UI_STATE_MAIN && have_bus) {
        const UI_GAUGE_DEF_T *d = &s_gauge_defs[s_curr_gauge_idx];
        if (d->metric == APP_METRIC_G_FORCE && s_gforce.root) {
            if (bus.valid[APP_METRIC_G_FORCE]) {
                /* Feed the GoPro reticle the orient-projected vehicle
                 * axes (lat = right-of-driver, fwd = nose-of-car). The
                 * IMU sampler does the projection per the user's
                 * mounting selection, so the ball indicates car-frame
                 * acceleration regardless of physical orientation. */
                ui_gforce_set_xy(&s_gforce, bus.g_lat_mg, bus.g_fwd_mg);
                s_first_value_after_link = FALSE;
            }
            ui_gforce_set_uncalibrated_hint(&s_gforce,
                                            sensor_imu_calibration_active() ? FALSE : TRUE);
        } else if (s_gauge.root) {
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

    rt = ui_gforce_create(&s_gforce, s_screen);
    if (rt != OPRT_OK) {
        ui_gauge_destroy(&s_gauge);
        return rt;
    }
    /* Both widgets are full-screen siblings; we only ever show one of them
     * at a time. The other is hidden so its layer doesn't waste blits. */
    if (d->metric == APP_METRIC_G_FORCE) {
        ui_gauge_set_visible(&s_gauge, FALSE);
        ui_gforce_set_visible(&s_gforce, TRUE);
    } else {
        ui_gauge_set_visible(&s_gauge, TRUE);
        ui_gforce_set_visible(&s_gforce, FALSE);
    }

    __build_overlay();
    __build_menu();

    /* Boot animation: sweep both widgets so cycling to the hidden one
     * later doesn't show an awkward "still at zero" first frame. */
    ui_gauge_sweep(&s_gauge, UI_BOOT_SWEEP_MS);
    ui_gforce_sweep(&s_gforce, UI_BOOT_SWEEP_MS);

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
 * @brief KEY handler — cycle to the next enabled gauge.
 *
 * Allowed in MAIN (animates to the live value) and WAIT_LINK (just
 * reconfigures the dial title/range — no live value to animate to).
 * Inert in MENU (menu is touchscreen-only) and BOOT_SWEEP (let the boot
 * animation play out cleanly).
 *
 * @return none
 */
void ui_show_next_gauge(VOID_T)
{
    if (s_state != UI_STATE_MAIN && s_state != UI_STATE_WAIT_LINK) {
        return;
    }
    s_curr_gauge_idx = __next_enabled_gauge(s_curr_gauge_idx);
    app_kv_set_current_gauge((APP_METRIC_E)s_curr_gauge_idx);
    __apply_current_gauge(s_state == UI_STATE_MAIN);
}

/**
 * @brief PWR short-press handler — toggle the menu open/closed.
 *
 * Reachable from any "live" state (MAIN or WAIT_LINK) so the user can
 * always reach Mock Mode / rescan, even with a stuck BLE link. Ignored
 * during BOOT_SWEEP so the boot animation can play out. On close the
 * right live state (MAIN or WAIT_LINK) is recomputed.
 *
 * @return none
 */
void ui_toggle_menu(VOID_T)
{
    if (s_state == UI_STATE_MENU) {
        __menu_show(FALSE);
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
