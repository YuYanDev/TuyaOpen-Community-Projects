/**
 * @file ui_gforce.c
 * @brief Implementation of the GoPro-style G-force target reticle.
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 *
 * Coordinate system (vehicle frame as projected by the IMU sampler):
 *  - +x  → lateral (right-of-driver)  → ball moves RIGHT on screen
 *  - +y  → forward (nose-of-car)      → ball moves UP    on screen
 *
 *  As of v1.8 the IMU sampler picks up the menu-selected mounting
 *  orientation (face-up / face-user 0/90/180/270°) and projects the
 *  live IMU body-frame vector onto the vehicle's forward and lateral
 *  axes before publishing on the metric bus. ui.c then feeds those
 *  two scalars (g_lat_mg, g_fwd_mg) directly into ui_gforce_set_xy(),
 *  so a positive +y on the reticle is ALWAYS "nose-of-car" regardless
 *  of which way the device is physically mounted. Static gravity bias
 *  is still zeroed out by "Calibrate G", per orientation.
 *
 * Why a 60 Hz EMA tracker instead of lv_anim
 * ------------------------------------------
 *  The IMU pushes ~50 Hz updates and the user can move the ball
 *  considerably between any two adjacent samples. Per-call lv_anim
 *  would either snap (duration=0) or constantly cancel-and-restart
 *  (duration>0), neither of which feels good. A persistent 60 Hz
 *  tracker that EMAs (bx, by) toward (tx, ty) with a velocity cap
 *  gives the same buttery feel as the gauge needle.
 */
#include "ui_gforce.h"
#include "ui_theme.h"
#include "app_config.h"
#include "tal_api.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
/* v1.8 — 3-ring layout with the OUTER ring at 1.5g instead of 1.0g.
 *
 * Rationale (user feedback): "G值的的表盘刻度过大，其实很多时候，用户不
 * 可能超过1.5个G的". A typical street car peaks at ~0.8 g hard braking
 * and ~1.0 g cornering on warm tyres; 2 g is the realm of track-day
 * driving with sticky compounds. 1.5 g full-scale is a far better
 * match for everyday use, gives a finer per-pixel resolution, and
 * means the ball rarely clips the outer ring.
 *
 * The ring radii are spaced equally on the screen (0.5g / 1.0g / 1.5g
 * = 1/3, 2/3, 3/3 of OUTER_R) so the user can visually estimate the
 * G value to the nearest 0.25 g without doing maths in their head.
 * The 4th 0.25g ring from the v1.7 layout is dropped — it added
 * visual noise without adding precision (the eye can already split
 * each 0.5g sector in half with no help from a tick mark).
 */
#define GFORCE_OUTER_R          200             /* px, ring radius for 1.50 g */
#define GFORCE_RING_R0           67             /* 0.50 g (≈ OUTER_R / 3) */
#define GFORCE_RING_R1          133             /* 1.00 g (≈ 2·OUTER_R / 3) */
#define GFORCE_RING_R2          200             /* 1.50 g (= OUTER_R) */
#define GFORCE_FULL_SCALE_MG   1500             /* outer ring = 1.50 g exactly */

#define GFORCE_CROSS_LEN        (GFORCE_OUTER_R * 2 - 16)
#define GFORCE_CROSS_W          1

#define GFORCE_CENTRE_R         5               /* fixed red zero marker */
#define GFORCE_BALL_R           14              /* live ball — 28 px diameter */

/* v1.8.2: tracker upgraded from ~60 Hz to ~100 Hz (matches LVGL's
 * 10 ms refresh budget). At 50 Hz IMU sample rate × 100 Hz tracker,
 * each new (gx, gy) target gets ~2 tracker steps, which the EMA
 * smooths into a continuous trajectory rather than a discrete step
 * (the user's "在两次数据中间需要插值" feedback). VEL cap reduced
 * proportionally so peak max angular velocity stays equivalent. */
#define GFORCE_TRACK_PERIOD_MS  10              /* ~100 Hz */
#define GFORCE_TRACK_ALPHA       4              /* shift: smoothing 1/16 step */
#define GFORCE_MAX_VEL_MG       50              /* mg / tick = ~5000 mg/s cap */

#define GFORCE_BALL_PAD          6              /* AA halo */

#define GFORCE_LOG(fmt, ...)    PR_DEBUG("[gforce] " fmt, ##__VA_ARGS__)

/* ---------------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------------- */
STATIC VOID_T __track_timer_cb(lv_timer_t *t);
STATIC VOID_T __sweep_anim_cb(VOID_T *var, int32_t v);
STATIC VOID_T __sweep_ready_cb(lv_anim_t *a);
STATIC VOID_T __ball_apply_pos(UI_GFORCE_T *g);
STATIC VOID_T __value_label_update(UI_GFORCE_T *g);

/* ---------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------------- */
/**
 * @brief Map (mg, mg) → screen offset in pixels around centre, clipped to
 *        the outer ring so the ball never escapes the dial.
 *
 * @param[in] gx_mg X in milli-g
 * @param[in] gy_mg Y in milli-g
 * @param[out] ox screen X offset (px, signed)
 * @param[out] oy screen Y offset (px, signed)
 * @return none
 */
STATIC VOID_T __mg_to_px(int32_t gx_mg, int32_t gy_mg,
                         int32_t *ox, int32_t *oy)
{
    /* Linear: GFORCE_FULL_SCALE_MG (= 1500, i.e. 1.50 g, v1.8) maps to
     * OUTER_R px on screen. Smaller magnitudes scale linearly. */
    int64_t px = (int64_t)gx_mg * GFORCE_OUTER_R / GFORCE_FULL_SCALE_MG;
    int64_t py = (int64_t)gy_mg * GFORCE_OUTER_R / GFORCE_FULL_SCALE_MG;

    /* Clip onto the OUTER_R - BALL_R circle so the ball stays inside the
     * dial. We use a coarse hypot via squared comparison + sqrt only when
     * needed — over-shoots are rare except during the boot sweep. */
    int32_t r_max = GFORCE_OUTER_R - GFORCE_BALL_R - 1;
    int64_t r2    = px * px + py * py;
    int64_t rmax2 = (int64_t)r_max * r_max;
    if (r2 > rmax2 && r2 > 0) {
        /* Integer Newton's method for sqrt(r2). For r2 ≤ 200^2 ≈ 4·10^4
         * — five iterations are plenty. */
        int64_t s = 1;
        while (s * s < r2) {
            s <<= 1;
        }
        int64_t lo = s >> 1, hi = s;
        while (lo + 1 < hi) {
            int64_t m = (lo + hi) >> 1;
            if (m * m <= r2) lo = m; else hi = m;
        }
        int64_t mag = lo;
        if (mag > 0) {
            px = px * r_max / mag;
            py = py * r_max / mag;
        }
    }

    /* Screen Y points DOWN, but +gy means "up on the dial" — flip sign so a
     * forward acceleration (+gy) shows as the ball moving UP on screen. */
    *ox = (int32_t)px;
    *oy = (int32_t)(-py);
}

/**
 * @brief Reposition the ball obj to the current (bx, by).
 */
STATIC VOID_T __ball_apply_pos(UI_GFORCE_T *g)
{
    if (g->ball == NULL) {
        return;
    }
    int32_t ox = 0, oy = 0;
    __mg_to_px(g->bx_mg, g->by_mg, &ox, &oy);
    lv_obj_align(g->ball, LV_ALIGN_CENTER, ox, oy);
}

/**
 * @brief Refresh the headline magnitude + axis breakdown labels.
 */
STATIC VOID_T __value_label_update(UI_GFORCE_T *g)
{
    if (g->lbl_value == NULL) {
        return;
    }
    int32_t ax = (g->tx_mg < 0) ? -g->tx_mg : g->tx_mg;
    int32_t ay = (g->ty_mg < 0) ? -g->ty_mg : g->ty_mg;
    /* mag^2 in (mg)^2 — peak at 32g would be 32000^2 ≈ 10^9, fits in int64. */
    int64_t mag2 = (int64_t)ax * ax + (int64_t)ay * ay;
    int64_t mag  = 0;
    if (mag2 > 0) {
        int64_t s = 1;
        while (s * s < mag2) s <<= 1;
        int64_t lo = s >> 1, hi = s;
        while (lo + 1 < hi) {
            int64_t m = (lo + hi) >> 1;
            if (m * m <= mag2) lo = m; else hi = m;
        }
        mag = lo;
    }
    char buf[48];
    /* Magnitude as N.NN g */
    int mg_int = (int)(mag / 1000);
    int mg_frac = (int)((mag % 1000) / 10);    /* 0.00..0.99 */
    lv_snprintf(buf, sizeof(buf), "%d.%02d g", mg_int, mg_frac < 0 ? -mg_frac : mg_frac);
    lv_label_set_text(g->lbl_value, buf);

    if (g->lbl_axis) {
        /* tx = lateral (right-of-driver), ty = forward (nose-of-car).
         * v1.8 — labels match the FWD/BACK/L/R cardinal markers around
         * the ring rather than X/Y so the user never has to mentally
         * remap "+X is which way?" while watching the ball move. */
        int xi = (int)(g->tx_mg / 1000);
        int xf = (int)((g->tx_mg < 0 ? -g->tx_mg : g->tx_mg) % 1000) / 10;
        int yi = (int)(g->ty_mg / 1000);
        int yf = (int)((g->ty_mg < 0 ? -g->ty_mg : g->ty_mg) % 1000) / 10;
        lv_snprintf(buf, sizeof(buf),
                    "FWD %s%d.%02d  LAT %s%d.%02d",
                    g->ty_mg < 0 ? "-" : "+", yi < 0 ? -yi : yi, yf,
                    g->tx_mg < 0 ? "-" : "+", xi < 0 ? -xi : xi, xf);
        lv_label_set_text(g->lbl_axis, buf);
    }
}

/**
 * @brief Apply an EMA + velocity-cap step from (bx,by) toward (tx,ty).
 *
 * Returns TRUE if the position changed enough to require a redraw.
 */
STATIC BOOL_T __track_step(UI_GFORCE_T *g)
{
    int32_t dx = g->tx_mg - g->bx_mg;
    int32_t dy = g->ty_mg - g->by_mg;
    if (dx == 0 && dy == 0) {
        return FALSE;
    }

    int32_t sx = dx >> GFORCE_TRACK_ALPHA;
    int32_t sy = dy >> GFORCE_TRACK_ALPHA;
    /* 1-LSB minimum so we always crawl out of the EMA tail. */
    if (sx == 0 && dx != 0) sx = (dx > 0) ? 1 : -1;
    if (sy == 0 && dy != 0) sy = (dy > 0) ? 1 : -1;

    /* Velocity cap so violent jolts don't teleport the ball (looks bad). */
    if (sx >  GFORCE_MAX_VEL_MG) sx =  GFORCE_MAX_VEL_MG;
    if (sx < -GFORCE_MAX_VEL_MG) sx = -GFORCE_MAX_VEL_MG;
    if (sy >  GFORCE_MAX_VEL_MG) sy =  GFORCE_MAX_VEL_MG;
    if (sy < -GFORCE_MAX_VEL_MG) sy = -GFORCE_MAX_VEL_MG;

    g->bx_mg += sx;
    g->by_mg += sy;
    return TRUE;
}

/**
 * @brief 60 Hz tracker.
 */
STATIC VOID_T __track_timer_cb(lv_timer_t *t)
{
    UI_GFORCE_T *g = (UI_GFORCE_T *)lv_timer_get_user_data(t);
    if (g == NULL || g->root == NULL || g->ball == NULL) {
        return;
    }
    if (g->sweep_running) {
        return;
    }
    if (__track_step(g)) {
        __ball_apply_pos(g);
    }
}

/* ---------------------------------------------------------------------------
 * Boot sweep
 * --------------------------------------------------------------------------- */
/**
 * @brief Sweep animation interpolator — sends the ball from centre out
 *        along the +x axis to the rim and back to centre.
 */
STATIC VOID_T __sweep_anim_cb(VOID_T *var, int32_t v)
{
    UI_GFORCE_T *g = (UI_GFORCE_T *)var;
    if (g == NULL || g->ball == NULL) {
        return;
    }
    /* v: 0..1000..0 over the full duration (lv_anim playback) */
    g->bx_mg = v;            /* 0 .. 1000 mg */
    g->by_mg = 0;
    __ball_apply_pos(g);
}

STATIC VOID_T __sweep_ready_cb(lv_anim_t *a)
{
    UI_GFORCE_T *g = (UI_GFORCE_T *)a->var;
    if (g == NULL) {
        return;
    }
    g->sweep_running = FALSE;
    g->bx_mg = 0;
    g->by_mg = 0;
    __ball_apply_pos(g);
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */
/**
 * @brief Create the G-force target reticle.
 */
OPERATE_RET ui_gforce_create(UI_GFORCE_T *g, lv_obj_t *parent)
{
    if (g == NULL || parent == NULL) {
        return OPRT_INVALID_PARM;
    }
    memset(g, 0, sizeof(*g));

    g->root = lv_obj_create(parent);
    lv_obj_remove_style_all(g->root);
    lv_obj_set_size(g->root, APP_LCD_WIDTH, APP_LCD_HEIGHT);
    lv_obj_center(g->root);
    lv_obj_set_style_bg_color(g->root, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(g->root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(g->root, LV_OBJ_FLAG_CLICKABLE);

    /* Concentric rings — 3 rings at 0.5 / 1.0 / 1.5 g (v1.8). The outer
     * ring is drawn brighter and thicker so the user can spot the
     * "full-scale" boundary at a glance during cornering. */
    static const int32_t k_radii[3] = {
        GFORCE_RING_R0, GFORCE_RING_R1, GFORCE_RING_R2
    };
    int i;
    for (i = 0; i < 3; i++) {
        lv_obj_t *r = lv_obj_create(g->root);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, k_radii[i] * 2, k_radii[i] * 2);
        lv_obj_center(r);
        lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(r,
            (i == 2) ? UI_COLOR_TICK : UI_COLOR_TICK_DIM, 0);
        lv_obj_set_style_border_width(r, (i == 2) ? 2 : 1, 0);
        lv_obj_set_style_border_opa(r, LV_OPA_COVER, 0);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);
        g->ring[i] = r;
    }

    /* Cross-hair: two thin filled rectangles. */
    g->cross_h = lv_obj_create(g->root);
    lv_obj_remove_style_all(g->cross_h);
    lv_obj_set_size(g->cross_h, GFORCE_CROSS_LEN, GFORCE_CROSS_W);
    lv_obj_center(g->cross_h);
    lv_obj_set_style_bg_color(g->cross_h, UI_COLOR_TICK_DIM, 0);
    lv_obj_set_style_bg_opa(g->cross_h, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g->cross_h, LV_OBJ_FLAG_CLICKABLE);

    g->cross_v = lv_obj_create(g->root);
    lv_obj_remove_style_all(g->cross_v);
    lv_obj_set_size(g->cross_v, GFORCE_CROSS_W, GFORCE_CROSS_LEN);
    lv_obj_center(g->cross_v);
    lv_obj_set_style_bg_color(g->cross_v, UI_COLOR_TICK_DIM, 0);
    lv_obj_set_style_bg_opa(g->cross_v, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g->cross_v, LV_OBJ_FLAG_CLICKABLE);

    /* Cardinal labels just outside the outer ring. */
    struct __LABEL_DEF_S {
        lv_obj_t **slot;
        const char *txt;
        int32_t dx;
        int32_t dy;
    };
    struct __LABEL_DEF_S k_labels[] = {
        { &g->lbl_n, "FWD",  0,                     -(GFORCE_OUTER_R + 22) },
        { &g->lbl_s, "BACK", 0,                      (GFORCE_OUTER_R + 22) },
        { &g->lbl_e, "R",    (GFORCE_OUTER_R + 22),  0 },
        { &g->lbl_w, "L",   -(GFORCE_OUTER_R + 22),  0 },
    };
    size_t li;
    for (li = 0; li < sizeof(k_labels) / sizeof(k_labels[0]); li++) {
        lv_obj_t *lbl = lv_label_create(g->root);
        lv_label_set_text(lbl, k_labels[li].txt);
        lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_22, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, k_labels[li].dx, k_labels[li].dy);
        *k_labels[li].slot = lbl;
    }

    /* Static centre dot — the calibrated zero marker. */
    g->centre_dot = lv_obj_create(g->root);
    lv_obj_remove_style_all(g->centre_dot);
    lv_obj_set_size(g->centre_dot, GFORCE_CENTRE_R * 2, GFORCE_CENTRE_R * 2);
    lv_obj_center(g->centre_dot);
    lv_obj_set_style_radius(g->centre_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g->centre_dot, UI_COLOR_PRIMARY, 0);
    lv_obj_set_style_bg_opa(g->centre_dot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g->centre_dot, LV_OBJ_FLAG_CLICKABLE);

    /* Live ball — the moving G dot. Bigger than the centre marker, plus a
     * thin white ring so the ball stays visible when it rolls over the
     * crosshair lines. */
    g->ball = lv_obj_create(g->root);
    lv_obj_remove_style_all(g->ball);
    lv_obj_set_size(g->ball, GFORCE_BALL_R * 2, GFORCE_BALL_R * 2);
    lv_obj_center(g->ball);
    lv_obj_set_style_radius(g->ball, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g->ball, UI_COLOR_PRIMARY, 0);
    lv_obj_set_style_bg_opa(g->ball, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g->ball, UI_COLOR_TEXT, 0);
    lv_obj_set_style_border_width(g->ball, 2, 0);
    lv_obj_set_style_border_opa(g->ball, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g->ball, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g->ball, LV_OBJ_FLAG_SCROLLABLE);

    /* Headline value (large) + per-axis breakdown (small) below the rings. */
    g->lbl_value = lv_label_create(g->root);
    lv_label_set_text(g->lbl_value, "0.00 g");
    lv_obj_set_style_text_color(g->lbl_value, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(g->lbl_value, &lv_font_montserrat_28, 0);
    lv_obj_align(g->lbl_value, LV_ALIGN_BOTTOM_MID, 0, -50);

    g->lbl_axis = lv_label_create(g->root);
    lv_label_set_text(g->lbl_axis, "FWD +0.00  LAT +0.00");
    lv_obj_set_style_text_color(g->lbl_axis, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(g->lbl_axis, &lv_font_montserrat_16, 0);
    lv_obj_align(g->lbl_axis, LV_ALIGN_BOTTOM_MID, 0, -22);

    /* Calibration hint (top of screen, hidden by default). */
    g->lbl_cal_hint = lv_label_create(g->root);
    lv_label_set_text(g->lbl_cal_hint, "Press PWR → Calibrate G");
    lv_obj_set_style_text_color(g->lbl_cal_hint, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(g->lbl_cal_hint, &lv_font_montserrat_16, 0);
    lv_obj_align(g->lbl_cal_hint, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_add_flag(g->lbl_cal_hint, LV_OBJ_FLAG_HIDDEN);

    g->tx_mg = 0;
    g->ty_mg = 0;
    g->bx_mg = 0;
    g->by_mg = 0;
    g->data_valid = FALSE;
    g->sweep_running = FALSE;

    g->track_timer = lv_timer_create(__track_timer_cb,
                                     GFORCE_TRACK_PERIOD_MS, g);
    if (g->track_timer == NULL) {
        ui_gforce_destroy(g);
        return OPRT_MALLOC_FAILED;
    }
    lv_timer_set_user_data(g->track_timer, g);

    __ball_apply_pos(g);
    __value_label_update(g);
    return OPRT_OK;
}

/**
 * @brief Push a new live (gx, gy) target.
 */
void ui_gforce_set_xy(UI_GFORCE_T *g, int32_t gx_mg, int32_t gy_mg)
{
    if (g == NULL) {
        return;
    }
    /* Clamp to a sane range so a temporary IMU glitch (e.g. ±10g spike)
     * doesn't blow up the EMA with a long catch-up. */
    if (gx_mg >  4000) gx_mg =  4000;
    if (gx_mg < -4000) gx_mg = -4000;
    if (gy_mg >  4000) gy_mg =  4000;
    if (gy_mg < -4000) gy_mg = -4000;

    g->tx_mg = gx_mg;
    g->ty_mg = gy_mg;
    if (!g->data_valid) {
        /* First sample after the widget appears: snap to the live value
         * so the ball doesn't crawl from 0,0 over a second. The user
         * already sat through the boot sweep — they want the live state. */
        g->bx_mg = gx_mg;
        g->by_mg = gy_mg;
        g->data_valid = TRUE;
        __ball_apply_pos(g);
    }
    __value_label_update(g);
}

/**
 * @brief Boot self-test: ball flies to the rim and back.
 */
void ui_gforce_sweep(UI_GFORCE_T *g, uint32_t duration_ms)
{
    if (g == NULL || g->root == NULL) {
        return;
    }
    g->sweep_running = TRUE;
    g->bx_mg = 0;
    g->by_mg = 0;
    __ball_apply_pos(g);

    lv_anim_init(&g->sweep_anim);
    lv_anim_set_var(&g->sweep_anim, g);
    lv_anim_set_exec_cb(&g->sweep_anim, (lv_anim_exec_xcb_t)__sweep_anim_cb);
    lv_anim_set_values(&g->sweep_anim, 0, GFORCE_FULL_SCALE_MG);
    lv_anim_set_time(&g->sweep_anim, duration_ms / 2);
    lv_anim_set_playback_time(&g->sweep_anim, duration_ms / 2);
    lv_anim_set_path_cb(&g->sweep_anim, lv_anim_path_ease_in_out);
    lv_anim_set_ready_cb(&g->sweep_anim, __sweep_ready_cb);
    lv_anim_start(&g->sweep_anim);
}

/**
 * @brief Toggle the "uncalibrated" hint.
 */
void ui_gforce_set_uncalibrated_hint(UI_GFORCE_T *g, BOOL_T uncalibrated)
{
    if (g == NULL || g->lbl_cal_hint == NULL) {
        return;
    }
    if (uncalibrated) {
        lv_obj_clear_flag(g->lbl_cal_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g->lbl_cal_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief Show / hide the widget root.
 */
void ui_gforce_set_visible(UI_GFORCE_T *g, BOOL_T visible)
{
    if (g == NULL || g->root == NULL) {
        return;
    }
    if (visible) {
        lv_obj_clear_flag(g->root, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g->root, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief Tear down.
 */
void ui_gforce_destroy(UI_GFORCE_T *g)
{
    if (g == NULL) {
        return;
    }
    if (g->track_timer) {
        lv_timer_delete(g->track_timer);
        g->track_timer = NULL;
    }
    lv_anim_delete(g, __sweep_anim_cb);
    if (g->root) {
        lv_obj_delete(g->root);
    }
    memset(g, 0, sizeof(*g));
}
