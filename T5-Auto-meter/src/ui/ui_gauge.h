/**
 * @file ui_gauge.h
 * @brief Reusable circular pointer gauge widget for the 466x466 round AMOLED.
 * @version 1.1
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 *
 * Layout (radius numbers are pixels; centre is the screen centre):
 *   - Outer decorative arc       : r = 220
 *   - Major/minor ticks          : r = 195..215
 *   - Numeric tick labels        : r = 165
 *   - Needle pivot               : centre (233, 233)
 *   - Needle length              : 175 px
 *   - Hub                        : Ø 36 filled circle (covers needle base)
 *   - Title label                : top of dial
 *   - Big readout (live value)   : centred just below centre
 *   - Unit label                 : below readout
 *
 * The needle is a custom-drawn tapered teardrop polygon (wide base, narrow
 * rounded tip) anchored exactly at the pivot. Animation is performed on the
 * needle angle (in 0.1°) instead of the value, which avoids per-frame
 * value→angle conversions and gives perfectly linear rotation motion.
 */
#ifndef __UI_GAUGE_H__
#define __UI_GAUGE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include "tuya_cloud_types.h"

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef struct {
    lv_obj_t  *root;
    lv_obj_t  *arc;
    lv_obj_t  *scale;
    lv_obj_t  *needle;        /**< custom-drawn tapered needle (transparent obj) */
    lv_obj_t  *hub;
    lv_obj_t  *label_title;
    lv_obj_t  *label_value;
    lv_obj_t  *label_unit;

    int32_t    val_min;
    int32_t    val_max;
    int32_t    val_curr;          /**< latest displayed value (raw user units) */
    int32_t    needle_angle_x10;  /**< current needle angle in 0.1° (LVGL trigo: 0=3 o'clock, +cw) */
    lv_anim_t  anim;
} UI_GAUGE_T;

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */
/**
 * @brief Create a gauge filling the parent screen.
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
                            uint8_t tick_major);

/**
 * @brief Reconfigure an existing gauge (range / title / unit / ticks)
 *        without destroying the LVGL widgets. Avoids the destroy+create
 *        thrash on KEY-press gauge cycling.
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
                             uint8_t tick_major);

/**
 * @brief Animate the needle from its current angle to a new value.
 *        Durations < 500 ms use a linear path (smooth continuous tracking),
 *        durations >= 500 ms use ease-in-out (dramatic intro/handover).
 * @param[in,out] g gauge
 * @param[in] value target value (clamped to [val_min, val_max])
 * @param[in] duration_ms animation duration; 0 = snap
 * @return none
 */
void ui_gauge_set_value(UI_GAUGE_T *g, int32_t value, uint32_t duration_ms);

/**
 * @brief Sweep the needle from min to max and back to min for the boot animation.
 * @param[in,out] g gauge
 * @param[in] duration_ms total sweep duration
 * @return none
 */
void ui_gauge_sweep(UI_GAUGE_T *g, uint32_t duration_ms);

/**
 * @brief Set the readable value text (independent of the needle).
 * @param[in,out] g gauge
 * @param[in] text text to display (may be NULL or empty for "—")
 * @return none
 */
void ui_gauge_set_value_text(UI_GAUGE_T *g, const char *text);

/**
 * @brief Set the title displayed at the top of the dial.
 * @param[in,out] g gauge
 * @param[in] title null-terminated string
 * @return none
 */
void ui_gauge_set_title(UI_GAUGE_T *g, const char *title);

/**
 * @brief Show/hide the gauge.
 * @param[in,out] g gauge
 * @param[in] visible TRUE to show
 * @return none
 */
void ui_gauge_set_visible(UI_GAUGE_T *g, BOOL_T visible);

/**
 * @brief Tear down the LVGL objects and zero descriptor.
 * @param[in,out] g gauge
 * @return none
 */
void ui_gauge_destroy(UI_GAUGE_T *g);

#ifdef __cplusplus
}
#endif

#endif /* __UI_GAUGE_H__ */
