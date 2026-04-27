/**
 * @file ui_gauge.c
 * @brief Reusable round gauge widget with a custom tapered teardrop needle.
 * @version 1.1
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 *
 * Design notes
 * ------------
 *  - The needle is rendered by a transparent overlay obj that handles
 *    LV_EVENT_DRAW_MAIN itself and draws two triangles + a circular tip
 *    cap directly onto the layer. This guarantees the needle's pivot is
 *    exactly the obj centre (= dial centre on screen), so there is never
 *    a "phantom origin" away from the centre.
 *
 *  - The animation drives the needle angle (in 0.1°) directly, not the
 *    user-units value. That avoids per-frame value→angle math and gives
 *    perfectly linear rotation, which is what your eyes expect from an
 *    analog gauge.
 *
 *  - For continuous tracking (refresh tick @ 100 ms), `ui_gauge_set_value`
 *    uses a short linear-path animation (<= refresh period * 1.5) and a
 *    1° angular dead-band, so the needle glides without restart-stutter.
 *
 *  - Long durations (boot sweep, first sample after BLE link) get the
 *    classic ease-in-out path for a more "mechanical" feel.
 */
#include "ui_gauge.h"
#include "ui_theme.h"
#include "app_config.h"
#include "tal_api.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define GAUGE_OUTER_R          220
#define GAUGE_TICK_LEN_MAJOR   18
#define GAUGE_TICK_LEN_MINOR   8

/* Needle geometry (local frame: +x along needle, +y perpendicular) */
#define GAUGE_NEEDLE_LEN       175    /**< pivot to tip distance */
#define GAUGE_NEEDLE_BACK      6      /**< tail length behind pivot (under hub) */
#define GAUGE_NEEDLE_BASE_W    14     /**< base (near hub) width */
#define GAUGE_NEEDLE_TIP_W     4      /**< tip width before the round cap */
#define GAUGE_NEEDLE_CAP_R     3      /**< rounded tip cap radius */
#define GAUGE_NEEDLE_PAD       12     /**< invalidation safety margin */

/* Hub diameter chosen large enough to cover the needle base + back tail. */
#define GAUGE_HUB_R            18
#define GAUGE_LABEL_PAD        45
#define GAUGE_ANGLE_RANGE      270    /**< degrees swept by the dial */
#define GAUGE_ANGLE_ROTATION   135    /**< 0° tick at 7:30 position */

/* Angle range in 0.1° (LVGL trig system: 0 = 3 o'clock, +deg = clockwise). */
#define GAUGE_ANGLE_X10_MIN    (GAUGE_ANGLE_ROTATION * 10)
#define GAUGE_ANGLE_X10_MAX    ((GAUGE_ANGLE_ROTATION + GAUGE_ANGLE_RANGE) * 10)

/* Tracking thresholds */
#define GAUGE_DEADBAND_X10     8      /**< skip set_value if |Δangle| < 0.8° */
#define GAUGE_TRACK_DUR_MS     180    /**< default tracking slew */
#define GAUGE_EASE_THRESH_MS   500    /**< >= this duration → ease_in_out */

/* Bounding square that always contains the rotated needle (some safety pad). */
#define GAUGE_NEEDLE_BOX       ((GAUGE_NEEDLE_LEN + GAUGE_NEEDLE_CAP_R + \
                                 GAUGE_NEEDLE_PAD) * 2)

/* ---------------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------------- */
STATIC VOID_T  __anim_angle_cb(VOID_T *obj, int32_t a_x10);
STATIC VOID_T  __needle_draw_event_cb(lv_event_t *e);
STATIC int32_t __value_to_angle_x10(const UI_GAUGE_T *g, int32_t value);
STATIC int32_t __clamp(int32_t v, int32_t lo, int32_t hi);
STATIC VOID_T  __scale_apply_ticks(lv_obj_t *scale, uint8_t tick_major);

/* ---------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------------- */
/**
 * @brief Clamp helper.
 * @param[in] v input
 * @param[in] lo lower bound
 * @param[in] hi upper bound
 * @return clamped value
 */
STATIC int32_t __clamp(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

/**
 * @brief Map a user-units value to the LVGL trigo angle in 0.1° units.
 * @param[in] g gauge
 * @param[in] value raw user-units value
 * @return angle in 0.1°
 */
STATIC int32_t __value_to_angle_x10(const UI_GAUGE_T *g, int32_t value)
{
    if (g->val_max <= g->val_min) {
        return GAUGE_ANGLE_X10_MIN;
    }
    int32_t v = __clamp(value, g->val_min, g->val_max);
    int64_t span = (int64_t)(GAUGE_ANGLE_X10_MAX - GAUGE_ANGLE_X10_MIN);
    int64_t num  = span * (int64_t)(v - g->val_min);
    return GAUGE_ANGLE_X10_MIN + (int32_t)(num / (int64_t)(g->val_max - g->val_min));
}

/**
 * @brief LVGL animation step: store new angle and request a redraw.
 * @param[in] obj gauge handle (passed via lv_anim_set_var)
 * @param[in] a_x10 current angle in 0.1°
 * @return none
 */
STATIC VOID_T __anim_angle_cb(VOID_T *obj, int32_t a_x10)
{
    UI_GAUGE_T *g = (UI_GAUGE_T *)obj;
    if (g == NULL || g->needle == NULL) {
        return;
    }
    g->needle_angle_x10 = a_x10;
    lv_obj_invalidate(g->needle);
}

/**
 * @brief LV_EVENT_DRAW_MAIN handler for the needle overlay.
 *
 * Draws a tapered (wide base → narrow tip) teardrop using two triangles
 * plus a small circular cap at the tip for an anti-aliased rounded look.
 * The pivot is exactly the obj centre, which sits over the dial centre.
 *
 * @param[in] e LVGL event
 * @return none
 */
STATIC VOID_T __needle_draw_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) {
        return;
    }
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (obj == NULL) {
        return;
    }
    UI_GAUGE_T *g = (UI_GAUGE_T *)lv_obj_get_user_data(obj);
    if (g == NULL) {
        return;
    }
    lv_layer_t *layer = lv_event_get_layer(e);
    if (layer == NULL) {
        return;
    }

    /* Pivot = obj geometric centre (in screen-absolute coords) */
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    int32_t cx = (coords.x1 + coords.x2) / 2;
    int32_t cy = (coords.y1 + coords.y2) / 2;

    /* Q15 sin/cos. lv_trigo_sin returns sin(angle) * LV_TRIGO_SIN_MAX(=32768) */
    int16_t a_deg = (int16_t)(g->needle_angle_x10 / 10);
    int32_t s = lv_trigo_sin(a_deg);
    int32_t c = lv_trigo_sin((int16_t)(a_deg + 90));

    /* Trapezoid corners in needle-local frame:
     *   p0 (back-bottom) ──────────── p3 (tip-top)
     *   p1 (back-top)    ──────────── p2 (tip-bottom)
     * The back tail (-BACK..0) is hidden under the hub circle.
     */
    static const int32_t LX[4] = {
        -GAUGE_NEEDLE_BACK,
        -GAUGE_NEEDLE_BACK,
         GAUGE_NEEDLE_LEN,
         GAUGE_NEEDLE_LEN,
    };
    static const int32_t LY[4] = {
        -GAUGE_NEEDLE_BASE_W / 2,
         GAUGE_NEEDLE_BASE_W / 2,
         GAUGE_NEEDLE_TIP_W  / 2,
        -GAUGE_NEEDLE_TIP_W  / 2,
    };

    lv_point_precise_t pts[4];
    int i;
    for (i = 0; i < 4; i++) {
        int32_t rx = (LX[i] * c - LY[i] * s) >> 15;
        int32_t ry = (LX[i] * s + LY[i] * c) >> 15;
        pts[i].x = (lv_value_precise_t)(cx + rx);
        pts[i].y = (lv_value_precise_t)(cy + ry);
    }

    /* Two triangles share the diagonal p0→p2 to avoid a 1-pixel gap. */
    lv_draw_triangle_dsc_t tri;
    lv_draw_triangle_dsc_init(&tri);
    tri.bg_color = UI_COLOR_PRIMARY;
    tri.bg_opa   = LV_OPA_COVER;

    tri.p[0] = pts[0]; tri.p[1] = pts[1]; tri.p[2] = pts[2];
    lv_draw_triangle(layer, &tri);

    tri.p[0] = pts[0]; tri.p[1] = pts[2]; tri.p[2] = pts[3];
    lv_draw_triangle(layer, &tri);

    /* Rounded tip cap: a small filled circle straddling the tip edge. */
    int32_t tip_x = cx + ((GAUGE_NEEDLE_LEN * c) >> 15);
    int32_t tip_y = cy + ((GAUGE_NEEDLE_LEN * s) >> 15);
    lv_draw_rect_dsc_t cap;
    lv_draw_rect_dsc_init(&cap);
    cap.bg_color = UI_COLOR_PRIMARY;
    cap.bg_opa   = LV_OPA_COVER;
    cap.radius   = LV_RADIUS_CIRCLE;
    lv_area_t cap_a = {
        .x1 = tip_x - GAUGE_NEEDLE_CAP_R,
        .y1 = tip_y - GAUGE_NEEDLE_CAP_R,
        .x2 = tip_x + GAUGE_NEEDLE_CAP_R,
        .y2 = tip_y + GAUGE_NEEDLE_CAP_R,
    };
    lv_draw_rect(layer, &cap, &cap_a);
}

/**
 * @brief (Re)apply tick configuration to an existing scale.
 * @param[in] scale lv_scale handle
 * @param[in] tick_major number of major ticks (>= 2)
 * @return none
 */
STATIC VOID_T __scale_apply_ticks(lv_obj_t *scale, uint8_t tick_major)
{
    int32_t majors = (tick_major < 2) ? 2 : tick_major;
    int32_t minors_per_major = 4;
    int32_t total_ticks = (majors - 1) * minors_per_major + 1;
    lv_scale_set_total_tick_count(scale, total_ticks);
    lv_scale_set_major_tick_every(scale, minors_per_major);
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */
/**
 * @brief Create a circular gauge.
 * @param[in,out] g caller-owned descriptor (must be cleared)
 * @param[in] parent LVGL parent (the screen itself)
 * @param[in] title gauge title (e.g. "WATER TEMP")
 * @param[in] unit unit string (e.g. "°C")
 * @param[in] val_min lower bound of the dial range
 * @param[in] val_max upper bound of the dial range
 * @param[in] tick_major number of major ticks
 * @return OPRT_OK on success
 */
OPERATE_RET ui_gauge_create(UI_GAUGE_T *g, lv_obj_t *parent,
                            const char *title, const char *unit,
                            int32_t val_min, int32_t val_max,
                            uint8_t tick_major)
{
    if (g == NULL || parent == NULL || val_max <= val_min) {
        return OPRT_INVALID_PARM;
    }
    memset(g, 0, sizeof(*g));
    g->val_min = val_min;
    g->val_max = val_max;
    g->val_curr = val_min;
    g->needle_angle_x10 = GAUGE_ANGLE_X10_MIN;

    /* Root container fills the screen */
    g->root = lv_obj_create(parent);
    lv_obj_remove_style_all(g->root);
    lv_obj_set_size(g->root, APP_LCD_WIDTH, APP_LCD_HEIGHT);
    lv_obj_center(g->root);
    lv_obj_set_style_bg_color(g->root, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(g->root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g->root, LV_OBJ_FLAG_SCROLLABLE);

    /* Decorative outer arc */
    g->arc = lv_arc_create(g->root);
    lv_obj_set_size(g->arc, GAUGE_OUTER_R * 2, GAUGE_OUTER_R * 2);
    lv_obj_center(g->arc);
    lv_obj_remove_style(g->arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(g->arc, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_bg_angles(g->arc, GAUGE_ANGLE_ROTATION,
                          GAUGE_ANGLE_ROTATION + GAUGE_ANGLE_RANGE);
    lv_arc_set_value(g->arc, 0);
    lv_arc_set_range(g->arc, 0, 100);
    lv_obj_set_style_arc_color(g->arc, UI_COLOR_ARC, LV_PART_MAIN);
    lv_obj_set_style_arc_color(g->arc, UI_COLOR_ARC, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(g->arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g->arc, 6, LV_PART_INDICATOR);

    /* Tick scale (numbers + ticks only; needle is drawn separately) */
    g->scale = lv_scale_create(g->root);
    lv_obj_set_size(g->scale, GAUGE_OUTER_R * 2 - 8, GAUGE_OUTER_R * 2 - 8);
    lv_obj_center(g->scale);
    lv_scale_set_mode(g->scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_angle_range(g->scale, GAUGE_ANGLE_RANGE);
    lv_scale_set_rotation(g->scale, GAUGE_ANGLE_ROTATION);
    lv_scale_set_range(g->scale, val_min, val_max);
    __scale_apply_ticks(g->scale, tick_major);
    lv_scale_set_label_show(g->scale, true);
    lv_obj_set_style_line_color(g->scale, UI_COLOR_TICK_DIM, LV_PART_ITEMS);
    lv_obj_set_style_line_width(g->scale, 2, LV_PART_ITEMS);
    lv_obj_set_style_length(g->scale, GAUGE_TICK_LEN_MINOR, LV_PART_ITEMS);
    lv_obj_set_style_line_color(g->scale, UI_COLOR_TICK, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(g->scale, 4, LV_PART_INDICATOR);
    lv_obj_set_style_length(g->scale, GAUGE_TICK_LEN_MAJOR, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(g->scale, UI_COLOR_TEXT, LV_PART_INDICATOR);
    lv_obj_set_style_text_font(g->scale, &lv_font_montserrat_22, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(g->scale, GAUGE_LABEL_PAD, LV_PART_INDICATOR);

    /* Needle: transparent overlay obj that owns LV_EVENT_DRAW_MAIN.
     * Box is centred on the screen and just large enough to contain
     * the rotated needle, so each invalidation only repaints that
     * area instead of the whole 466x466 screen. */
    g->needle = lv_obj_create(g->root);
    lv_obj_remove_style_all(g->needle);
    lv_obj_set_size(g->needle, GAUGE_NEEDLE_BOX, GAUGE_NEEDLE_BOX);
    lv_obj_center(g->needle);
    lv_obj_clear_flag(g->needle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g->needle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(g->needle, g);
    lv_obj_add_event_cb(g->needle, __needle_draw_event_cb,
                        LV_EVENT_DRAW_MAIN, NULL);

    /* Hub: filled circle that sits on top of the needle base for a
     * clean "no exposed back tail" look. Created AFTER the needle so
     * it draws on top in z-order. */
    g->hub = lv_obj_create(g->root);
    lv_obj_remove_style_all(g->hub);
    lv_obj_set_size(g->hub, GAUGE_HUB_R * 2, GAUGE_HUB_R * 2);
    lv_obj_center(g->hub);
    lv_obj_set_style_radius(g->hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g->hub, UI_COLOR_HUB, 0);
    lv_obj_set_style_bg_opa(g->hub, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g->hub, UI_COLOR_PRIMARY, 0);
    lv_obj_set_style_border_width(g->hub, 3, 0);

    /* Title */
    g->label_title = lv_label_create(g->root);
    lv_label_set_text(g->label_title, title ? title : "");
    lv_obj_set_style_text_color(g->label_title, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(g->label_title, &lv_font_montserrat_22, 0);
    lv_obj_align(g->label_title, LV_ALIGN_TOP_MID, 0, 60);

    /* Live readout */
    g->label_value = lv_label_create(g->root);
    lv_label_set_text(g->label_value, "—");
    lv_obj_set_style_text_color(g->label_value, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(g->label_value, &lv_font_montserrat_48, 0);
    lv_obj_align(g->label_value, LV_ALIGN_BOTTOM_MID, 0, -120);

    /* Unit */
    g->label_unit = lv_label_create(g->root);
    lv_label_set_text(g->label_unit, unit ? unit : "");
    lv_obj_set_style_text_color(g->label_unit, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(g->label_unit, &lv_font_montserrat_22, 0);
    lv_obj_align(g->label_unit, LV_ALIGN_BOTTOM_MID, 0, -80);

    return OPRT_OK;
}

/**
 * @brief Reconfigure an existing gauge in-place.
 * @param[in,out] g gauge
 * @param[in] title gauge title (NULL leaves unchanged)
 * @param[in] unit unit string (NULL leaves unchanged)
 * @param[in] val_min lower bound of the dial range
 * @param[in] val_max upper bound of the dial range
 * @param[in] tick_major number of major ticks
 * @return OPRT_OK on success
 */
OPERATE_RET ui_gauge_set_def(UI_GAUGE_T *g,
                             const char *title, const char *unit,
                             int32_t val_min, int32_t val_max,
                             uint8_t tick_major)
{
    if (g == NULL || g->scale == NULL || val_max <= val_min) {
        return OPRT_INVALID_PARM;
    }
    /* Cancel any in-flight needle animation before changing the range. */
    lv_anim_delete(g, __anim_angle_cb);

    g->val_min = val_min;
    g->val_max = val_max;
    g->val_curr = val_min;
    g->needle_angle_x10 = GAUGE_ANGLE_X10_MIN;

    lv_scale_set_range(g->scale, val_min, val_max);
    __scale_apply_ticks(g->scale, tick_major);

    if (title && g->label_title) {
        lv_label_set_text(g->label_title, title);
    }
    if (unit && g->label_unit) {
        lv_label_set_text(g->label_unit, unit);
    }
    if (g->label_value) {
        lv_label_set_text(g->label_value, "—");
    }
    lv_obj_invalidate(g->needle);
    return OPRT_OK;
}

/**
 * @brief Animate the needle to a new value.
 * @param[in,out] g gauge
 * @param[in] value target value (clamped to [val_min, val_max])
 * @param[in] duration_ms animation duration; 0 = snap
 * @return none
 *
 * @note Same-target re-entry is filtered by an angular dead-band so the
 *       refresh tick can call this every 100 ms without restart-stutter.
 */
void ui_gauge_set_value(UI_GAUGE_T *g, int32_t value, uint32_t duration_ms)
{
    if (g == NULL || g->needle == NULL) {
        return;
    }
    int32_t target = __clamp(value, g->val_min, g->val_max);
    int32_t target_a = __value_to_angle_x10(g, target);
    g->val_curr = target;

    /* Snap path */
    if (duration_ms == 0) {
        if (target_a != g->needle_angle_x10) {
            g->needle_angle_x10 = target_a;
            lv_obj_invalidate(g->needle);
        }
        return;
    }

    /* Dead-band: ignore micro-jitter for short slews. */
    int32_t delta = target_a - g->needle_angle_x10;
    if (delta < 0) {
        delta = -delta;
    }
    if (delta < GAUGE_DEADBAND_X10 && duration_ms < GAUGE_EASE_THRESH_MS) {
        return;
    }

    lv_anim_init(&g->anim);
    lv_anim_set_var(&g->anim, g);
    lv_anim_set_exec_cb(&g->anim, __anim_angle_cb);
    lv_anim_set_duration(&g->anim, duration_ms);
    lv_anim_set_path_cb(&g->anim,
                        (duration_ms >= GAUGE_EASE_THRESH_MS)
                            ? lv_anim_path_ease_in_out
                            : lv_anim_path_linear);
    lv_anim_set_values(&g->anim, g->needle_angle_x10, target_a);
    lv_anim_start(&g->anim);
}

/**
 * @brief Sweep needle min->max->min for the boot animation.
 * @param[in,out] g gauge
 * @param[in] duration_ms total sweep duration (one full out+back cycle)
 * @return none
 */
void ui_gauge_sweep(UI_GAUGE_T *g, uint32_t duration_ms)
{
    if (g == NULL || g->needle == NULL) {
        return;
    }
    if (duration_ms < 200) {
        duration_ms = 200;
    }

    /* Cancel any tracking animation that might race with the sweep. */
    lv_anim_delete(g, __anim_angle_cb);

    g->needle_angle_x10 = GAUGE_ANGLE_X10_MIN;
    lv_obj_invalidate(g->needle);

    lv_anim_init(&g->anim);
    lv_anim_set_var(&g->anim, g);
    lv_anim_set_exec_cb(&g->anim, __anim_angle_cb);
    lv_anim_set_duration(&g->anim, duration_ms / 2);
    lv_anim_set_playback_duration(&g->anim, duration_ms / 2);
    lv_anim_set_path_cb(&g->anim, lv_anim_path_ease_in_out);
    lv_anim_set_values(&g->anim, GAUGE_ANGLE_X10_MIN, GAUGE_ANGLE_X10_MAX);
    lv_anim_start(&g->anim);
}

/**
 * @brief Set the centre value text.
 * @param[in,out] g gauge
 * @param[in] text text to display (NULL or empty becomes "—")
 * @return none
 */
void ui_gauge_set_value_text(UI_GAUGE_T *g, const char *text)
{
    if (g == NULL || g->label_value == NULL) {
        return;
    }
    lv_label_set_text(g->label_value, (text && *text) ? text : "—");
}

/**
 * @brief Update the title label.
 * @param[in,out] g gauge
 * @param[in] title null-terminated string
 * @return none
 */
void ui_gauge_set_title(UI_GAUGE_T *g, const char *title)
{
    if (g == NULL || g->label_title == NULL) {
        return;
    }
    lv_label_set_text(g->label_title, title ? title : "");
}

/**
 * @brief Show/hide the gauge root.
 * @param[in,out] g gauge
 * @param[in] visible TRUE to show
 * @return none
 */
void ui_gauge_set_visible(UI_GAUGE_T *g, BOOL_T visible)
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
 * @brief Tear down the gauge.
 * @param[in,out] g gauge
 * @return none
 */
void ui_gauge_destroy(UI_GAUGE_T *g)
{
    if (g == NULL) {
        return;
    }
    lv_anim_delete(g, __anim_angle_cb);
    if (g->root) {
        lv_obj_delete(g->root);
    }
    memset(g, 0, sizeof(*g));
}
