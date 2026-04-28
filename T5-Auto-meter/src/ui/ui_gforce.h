/**
 * @file ui_gforce.h
 * @brief GoPro-style G-force target reticle widget for the round AMOLED.
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 *
 * Visual layout (centred on the 466x466 panel):
 *   - 3 concentric thin rings at 0.50 / 1.00 / 1.50 g (rim) — v1.8.
 *   - Crosshair: thin vertical + horizontal grey lines through the centre.
 *   - 4 cardinal labels FWD / BACK / L / R outside the outer ring.
 *   - A small static red dot at the centre (the calibrated zero).
 *   - A larger red ball that GLIDES toward the live (gx, gy) sample,
 *     pinned inside the outer ring (clipped to the radius circle).
 *   - A live readout below the rings: "0.42 g  (FWD +0.27 LAT +0.32)".
 *
 * As of v1.8 the (gx, gy) fed to ui_gforce_set_xy() are vehicle-frame
 * (lateral, forward) milli-g — projected by sensor_imu.c from the
 * raw IMU body frame using the menu-selected mounting orientation.
 * The widget itself is orientation-agnostic; +x is "right of driver"
 * on screen and +y is "nose of car" on screen, always.
 *
 * Ball motion uses the same 60 Hz EMA tracker idiom as the gauge
 * widget: a persistent lv_timer glides the ball position toward the
 * latest target with a velocity cap, so the ball doesn't snap and
 * doesn't lag (no per-call lv_anim restarts even when the metric bus
 * pushes 50 Hz IMU updates).
 *
 * Boot sweep
 * ----------
 *  ui_gforce_sweep() animates the ball from centre out to the rim and
 *  back, mirroring the dial sweep used by ui_gauge — gives a consistent
 *  "self-test" feel for both gauge types.
 */
#ifndef __UI_GFORCE_H__
#define __UI_GFORCE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include "tuya_cloud_types.h"

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef struct {
    lv_obj_t   *root;
    lv_obj_t   *ring[3];        /**< 0.50 / 1.00 / 1.50 g rings (v1.8) */
    lv_obj_t   *cross_h;        /**< horizontal cross-hair line */
    lv_obj_t   *cross_v;        /**< vertical cross-hair line */
    lv_obj_t   *centre_dot;     /**< tiny red marker at the calibrated zero */
    lv_obj_t   *ball;           /**< the moving G ball */
    lv_obj_t   *lbl_n;
    lv_obj_t   *lbl_s;
    lv_obj_t   *lbl_e;
    lv_obj_t   *lbl_w;
    lv_obj_t   *lbl_value;      /**< headline magnitude readout */
    lv_obj_t   *lbl_axis;       /**< per-axis breakdown (X/Y) */
    lv_obj_t   *lbl_cal_hint;   /**< small "uncalibrated" hint when off */

    int32_t     tx_mg;          /**< target X in milli-g (post-cal) */
    int32_t     ty_mg;          /**< target Y in milli-g (post-cal) */
    int32_t     bx_mg;          /**< current ball X (smoothed) */
    int32_t     by_mg;          /**< current ball Y (smoothed) */
    BOOL_T      data_valid;
    BOOL_T      sweep_running;

    lv_timer_t *track_timer;    /**< 60 Hz tracker that glides ball to target */
    lv_anim_t   sweep_anim;
} UI_GFORCE_T;

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */
/**
 * @brief Create the G-force target reticle.
 * @param[in,out] g caller-owned descriptor (must be cleared)
 * @param[in] parent LVGL parent (the screen itself)
 * @return OPRT_OK on success
 */
OPERATE_RET ui_gforce_create(UI_GFORCE_T *g, lv_obj_t *parent);

/**
 * @brief Push a new (gx, gy) target. The ball glides smoothly toward it.
 * @param[in,out] g widget
 * @param[in] gx_mg X axis in milli-g (right positive)
 * @param[in] gy_mg Y axis in milli-g (forward/up positive in display)
 * @return none
 */
void ui_gforce_set_xy(UI_GFORCE_T *g, int32_t gx_mg, int32_t gy_mg);

/**
 * @brief Boot self-test sweep.
 * @param[in,out] g widget
 * @param[in] duration_ms total animation length
 * @return none
 */
void ui_gforce_sweep(UI_GFORCE_T *g, uint32_t duration_ms);

/**
 * @brief Set whether the calibration hint label is shown.
 * @param[in,out] g widget
 * @param[in] uncalibrated TRUE if the user has NOT calibrated yet
 * @return none
 */
void ui_gforce_set_uncalibrated_hint(UI_GFORCE_T *g, BOOL_T uncalibrated);

/**
 * @brief Show / hide the widget root.
 * @param[in,out] g widget
 * @param[in] visible TRUE to show
 * @return none
 */
void ui_gforce_set_visible(UI_GFORCE_T *g, BOOL_T visible);

/**
 * @brief Tear down the widget.
 * @param[in,out] g widget
 * @return none
 */
void ui_gforce_destroy(UI_GFORCE_T *g);

#ifdef __cplusplus
}
#endif

#endif /* __UI_GFORCE_H__ */
