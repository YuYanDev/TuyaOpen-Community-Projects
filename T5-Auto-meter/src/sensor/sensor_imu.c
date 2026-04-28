/**
 * @file sensor_imu.c
 * @brief IMU sampler task — converts accel readings into G/roll updates.
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 */
#include "sensor_imu.h"
#include "qmi8658.h"
#include "app_config.h"
#include "app_metric.h"
#include "app_kv.h"
#include "tal_api.h"

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define IMU_TASK_STACK     2048
#define IMU_TASK_PRIO      THREAD_PRIO_3
#define IMU_PERIOD_MS      20      /* ~50 Hz */
#define IMU_FILTER_ALPHA   16      /* divisor: smoothing 1/16 step per sample */

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
/* A 3D unit vector in the IMU body frame, encoded as int8_t per axis with
 * values in {-1, 0, +1}. Used to project the live (post zero-cal) accel
 * onto the vehicle's forward / lateral axes for the picked mounting pose. */
typedef struct {
    int8_t fwd[3];
    int8_t lat[3];
} G_ORIENT_AXES_T;

/* Per-orientation projection LUT.
 *
 * IMU body convention (matches the Waveshare schematic):
 *   +X = screen-right,  +Y = screen-up,  +Z = out-of-screen toward user.
 *
 * Vehicle convention:
 *   forward = nose direction of the car (+ braking gives -fwd, + accel
 *             gives +fwd in the published metric).
 *   lateral = right-of-driver direction (+ left turn gives +lat, since
 *             the car body accelerates to the left of the driver in a
 *             left turn — but the *reticle* on screen is "where the
 *             ball flies to", which is opposite to the felt direction).
 *
 * For face-up (chip flat, screen up), USB is assumed pointing toward
 * the rear of the car so vehicle-forward is +Y in IMU body.
 *
 * For face-user XX° (screen face-user, dashboard mount), windshield is
 * away from the user so vehicle-forward is -Z. The lateral axis
 * follows the screen rotation. */
STATIC const G_ORIENT_AXES_T k_orient_axes[APP_G_ORIENT_COUNT] = {
    [APP_G_ORIENT_FACE_UP]  = { .fwd = { 0,  1,  0}, .lat = { 1,  0,  0} },
    [APP_G_ORIENT_USER_0]   = { .fwd = { 0,  0, -1}, .lat = { 1,  0,  0} },
    [APP_G_ORIENT_USER_90]  = { .fwd = { 0,  0, -1}, .lat = { 0, -1,  0} },
    [APP_G_ORIENT_USER_180] = { .fwd = { 0,  0, -1}, .lat = {-1,  0,  0} },
    [APP_G_ORIENT_USER_270] = { .fwd = { 0,  0, -1}, .lat = { 0,  1,  0} },
};

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC THREAD_HANDLE   s_task = NULL;
STATIC volatile BOOL_T s_running = FALSE;

/* The latest filtered triplet (kept up to date by __imu_task) so the
 * calibration request can grab it without crossing tasks. */
STATIC volatile int32_t s_last_lp_x = 0;
STATIC volatile int32_t s_last_lp_y = 0;
STATIC volatile int32_t s_last_lp_z = 1000;
STATIC volatile BOOL_T  s_have_sample = FALSE;

/* Active calibration offsets — subtracted from each sample before publish.
 * Loaded from KV at IMU start, updated by sensor_imu_calibrate_zero(). */
STATIC volatile int32_t s_off_x = 0;
STATIC volatile int32_t s_off_y = 0;
STATIC volatile int32_t s_off_z = 0;
STATIC volatile BOOL_T  s_off_valid = FALSE;

/* Cross-task hand-shake flags. Written from any task, processed on the
 * IMU sample loop. Single-writer-per-flag pattern, so a bare volatile is
 * sufficient on Cortex-M's word-aligned 8/32-bit accesses. */
STATIC volatile BOOL_T s_calibrate_request = FALSE;
STATIC volatile BOOL_T s_clear_calib_request = FALSE;

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
/**
 * @brief Approximate atan2 in 0.1 degrees using small-angle table.
 *        Returns range -1800 .. 1800 (i.e. -180.0° .. 180.0°).
 *
 * @note Accuracy ≈ ±0.5 deg, fully sufficient for vehicle attitude UI.
 */
STATIC int32_t __atan2_deg10(int32_t y, int32_t x)
{
    /* Use abs/quadrant trick + a 16-step lookup.  This is far cheaper than
     * importing libm just to render a roll arc. */
    static const int16_t k_atan_lut[17] = {
        0,    36,   72,   107,  141,  175,  209,  241,
        272,  302,  331,  358,  385,  410,  434,  457,  478
    };
    int sign = 1;
    int swap = 0;
    if (y < 0) { sign = -sign; y = -y; }
    int xs = 1;
    if (x < 0) { xs = -1; x = -x; }
    int32_t base = 0;
    int32_t a = y, b = x;
    if (a > b) {
        swap = 1;
        int32_t t = a; a = b; b = t;
    }
    if (b == 0) {
        return 0;
    }
    int idx = (int)((a * 16 + b / 2) / b);
    if (idx > 16) idx = 16;
    if (idx < 0)  idx = 0;
    base = k_atan_lut[idx];
    if (swap) {
        base = 900 - base;
    }
    if (xs < 0) {
        base = 1800 - base;
    }
    if (sign < 0) {
        base = -base;
    }
    return base;
}

/**
 * @brief IMU sampler thread body.
 *
 * Pipeline per tick:
 *   1) Read raw mg from QMI8658 (~50 Hz).
 *   2) Run a 1/IMU_FILTER_ALPHA exponential low-pass.
 *   3) Cache the filtered triplet in s_last_lp_* for cross-task readers.
 *   4) Service hand-shake flags:
 *        - s_calibrate_request   : snapshot lp_* into s_off_* and persist.
 *        - s_clear_calib_request : drop the calibration.
 *   5) Subtract s_off_* (if active) and publish on the metric bus.
 *
 * Calibration is intentionally serviced HERE, not in the requester
 * thread, so the calibration capture and the next "applied" sample are
 * sequenced from the same writer — no cross-task race on s_off_*.
 */
STATIC VOID_T __imu_task(VOID_T *arg)
{
    (void)arg;
    PR_NOTICE("IMU sampler started");

    /* Pull any persisted calibration into the active offsets. */
    const APP_PREFS_T *p = app_kv_prefs();
    if (p && p->g_offset_valid) {
        s_off_x = (int32_t)p->g_offset_mg[0];
        s_off_y = (int32_t)p->g_offset_mg[1];
        s_off_z = (int32_t)p->g_offset_mg[2];
        s_off_valid = TRUE;
        PR_NOTICE("IMU loaded zero-cal: x=%d y=%d z=%d",
                  (int)s_off_x, (int)s_off_y, (int)s_off_z);
    }

    int32_t lp_x = 0, lp_y = 0, lp_z = 1000;

    while (s_running) {
        QMI8658_DATA_T d;
        if (qmi8658_read(&d) == OPRT_OK) {
            lp_x += (d.ax_mg - lp_x) / IMU_FILTER_ALPHA;
            lp_y += (d.ay_mg - lp_y) / IMU_FILTER_ALPHA;
            lp_z += (d.az_mg - lp_z) / IMU_FILTER_ALPHA;
            s_last_lp_x = lp_x;
            s_last_lp_y = lp_y;
            s_last_lp_z = lp_z;
            s_have_sample = TRUE;

            if (s_calibrate_request) {
                s_off_x = lp_x;
                s_off_y = lp_y;
                s_off_z = lp_z;
                s_off_valid = TRUE;
                s_calibrate_request = FALSE;
                /* Persist on the IMU thread — flash write may take a few ms,
                 * which is fine: we'll just skip 1-2 sample periods. */
                app_kv_set_g_offset((int16_t)lp_x, (int16_t)lp_y, (int16_t)lp_z);
                PR_NOTICE("IMU zero-cal captured: x=%d y=%d z=%d",
                          (int)lp_x, (int)lp_y, (int)lp_z);
            }
            if (s_clear_calib_request) {
                s_off_x = 0;
                s_off_y = 0;
                s_off_z = 0;
                s_off_valid = FALSE;
                s_clear_calib_request = FALSE;
                app_kv_clear_g_offset();
                PR_NOTICE("IMU zero-cal cleared");
            }

            int32_t out_x = lp_x - s_off_x;
            int32_t out_y = lp_y - s_off_y;
            int32_t out_z = lp_z - s_off_z;

            /* Pick up the user-selected mounting orientation each tick.
             * The KV pointer is stable and reads are unsynchronized
             * single-byte loads, so this is race-free w.r.t. the menu
             * task that updates s_prefs.g_orient. */
            APP_G_ORIENT_E orient = APP_G_ORIENT_FACE_UP;
            if (p) {
                uint8_t o = p->g_orient;
                if (o < (uint8_t)APP_G_ORIENT_COUNT) {
                    orient = (APP_G_ORIENT_E)o;
                }
            }
            const G_ORIENT_AXES_T *ax = &k_orient_axes[orient];
            int32_t out_fwd = out_x * ax->fwd[0] + out_y * ax->fwd[1] + out_z * ax->fwd[2];
            int32_t out_lat = out_x * ax->lat[0] + out_y * ax->lat[1] + out_z * ax->lat[2];

            int32_t roll = __atan2_deg10(lp_y, lp_z);
            app_metric_set_imu(out_x, out_y, out_z, out_fwd, out_lat, roll);
        }
        tal_system_sleep(IMU_PERIOD_MS);
    }
    PR_NOTICE("IMU sampler stopped");
    s_task = NULL;
}

/**
 * @brief Initialise QMI8658 and spawn sampler thread.
 */
OPERATE_RET sensor_imu_start(VOID_T)
{
    if (s_running) {
        return OPRT_OK;
    }

    OPERATE_RET rt = qmi8658_init(APP_I2C_PORT, APP_QMI8658_I2C_ADDR);
    if (rt != OPRT_OK) {
        PR_WARN("qmi8658 init failed (rt=%d) — IMU disabled", rt);
        return rt;
    }

    s_running = TRUE;
    THREAD_CFG_T cfg = {
        .stackDepth = IMU_TASK_STACK,
        .priority = IMU_TASK_PRIO,
        .thrdname = "app_imu"
    };
    rt = tal_thread_create_and_start(&s_task, NULL, NULL, __imu_task, NULL, &cfg);
    if (rt != OPRT_OK) {
        s_running = FALSE;
        return rt;
    }
    return OPRT_OK;
}

/**
 * @brief Capture & persist current static gravity vector as zero-cal.
 *
 * Posts a request flag that the IMU sample loop services on its next
 * tick — see __imu_task() for the full pipeline. Returns OPRT_COM_ERROR
 * if the IMU has not yet produced a sample (first ~20 ms after start).
 */
OPERATE_RET sensor_imu_calibrate_zero(VOID_T)
{
    if (!s_running) {
        return OPRT_COM_ERROR;
    }
    if (!s_have_sample) {
        return OPRT_COM_ERROR;
    }
    s_calibrate_request = TRUE;
    return OPRT_OK;
}

/**
 * @brief Discard the current zero calibration.
 */
OPERATE_RET sensor_imu_clear_calibration(VOID_T)
{
    if (!s_running) {
        return OPRT_COM_ERROR;
    }
    s_clear_calib_request = TRUE;
    return OPRT_OK;
}

/**
 * @brief Whether a user-supplied calibration is currently active.
 */
BOOL_T sensor_imu_calibration_active(VOID_T)
{
    return s_off_valid ? TRUE : FALSE;
}
