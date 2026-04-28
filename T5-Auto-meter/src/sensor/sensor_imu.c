/**
 * @file sensor_imu.c
 * @brief IMU sampler task — high-pass dynamic acceleration on the QMI8658.
 * @version 1.1
 * @date 2026-04-28
 * @copyright Copyright (c) Tuya Inc.
 *
 * Why a high-pass instead of a one-shot static-cal subtraction
 * ------------------------------------------------------------
 *  v1.0..v1.8 captured the gravity vector once at user-pressed
 *  "calibrate" time and subtracted that constant offset from every
 *  later sample. That works ONLY while the device stays in the
 *  exact same pose as at calibration time — tilt the screen by 10°
 *  and the gravity vector projects differently across body axes,
 *  the constant offset no longer cancels gravity, and the residual
 *  reads as a steady, never-zeroing "G" deflection. The user
 *  reported exactly this behaviour ("我歪一下屏幕G值就变了，回不去")
 *  — by the textbook accelerometer-physics definition the metric is
 *  no longer a G value, it's tilt.
 *
 *  v1.1 replaces the one-shot offset with a **continuous slow EMA
 *  gravity tracker** that reflows alongside the user's pose:
 *
 *     fast  lp_xyz = lp + (raw - lp)/16        # τ ≈ 320 ms @ 50 Hz
 *     slow  grav   = grav + (lp - grav)/1024   # τ ≈ 20.5 s   @ 50 Hz
 *     dynamic     = lp - grav
 *
 *  The "fast / slow EMA difference" is the textbook 1-pole high-
 *  pass filter — DC content (any sustained acceleration vector,
 *  i.e. gravity in whatever orientation) flows into `grav` and
 *  cancels out of `dynamic`. Static device → `dynamic` ≈ 0 in any
 *  orientation. Pulse-style accelerations (braking 1 s, cornering
 *  2 s) survive nearly intact (≈ 5–10 % decay over a 2 s sustained
 *  input at τ=20 s), which matches what a vehicle G-meter wants.
 *
 *  τ_slow = 20 s is chosen by:
 *   - α = 1 / 1024 → cheap shift, no divide.
 *   - 5 s would eat too much of a sustained brake / accel.
 *   - 60 s would leave the convergence visibly slow when the user
 *     re-positions the device (e.g. picks it up after a stop, the
 *     G readout shows "phantom" residual for ~10 s).
 *
 * Bootstrap seed
 * --------------
 *  KV `g_offset_mg` is repurposed: instead of "the constant offset
 *  to subtract" it now stores "the last known gravity vector" so
 *  on next boot the slow EMA starts already converged. If KV has
 *  no saved seed, the first sample is snapped into `grav` so the
 *  first ~20 ms after start aren't reading 1 g of bias before the
 *  EMA had a chance to converge.
 *
 *  The user's "Calibrate G" / "Reset Zero" menu button forces
 *  `grav := lp` immediately (i.e. seeds the EMA to zero out *now*)
 *  AND persists the new seed to KV — useful when the user rolls
 *  to a parking spot and wants the meter to read 0 g without
 *  waiting for the slow EMA's natural convergence.
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
#define IMU_TASK_STACK         2048
#define IMU_TASK_PRIO          THREAD_PRIO_3
#define IMU_PERIOD_MS          20    /**< 50 Hz sampling */
#define IMU_FILTER_ALPHA       16    /**< fast LPF divisor; τ ≈ 320 ms @ 50 Hz */
#define IMU_GRAV_ALPHA_SHIFT   10    /**< slow EMA shift; α=1/1024, τ ≈ 20 s @ 50 Hz */

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

/* The latest slow EMA gravity-tracker triplet. Persisted as the KV
 * seed at "calibrate" time so the next boot's EMA is already pointed
 * at the right gravity vector. `s_grav_inited` flips to TRUE the first
 * time we either seed from KV or snap to the first IMU sample. */
STATIC volatile int32_t s_grav_x = 0;
STATIC volatile int32_t s_grav_y = 0;
STATIC volatile int32_t s_grav_z = 1000;
STATIC volatile BOOL_T  s_grav_inited = FALSE;

/* Whether the active gravity seed came from a saved KV value (i.e.
 * the user has at some point pressed "Reset Zero" on this device).
 * The menu uses this to decide between "(saved)" and "(tap to zero)"
 * hint text — see sensor_imu_calibration_active(). */
STATIC volatile BOOL_T  s_grav_seeded_from_kv = FALSE;

/* Cross-task hand-shake flags. Written from any task, processed on the
 * IMU sample loop. Single-writer-per-flag pattern, so a bare volatile is
 * sufficient on Cortex-M's word-aligned 8/32-bit accesses. */
STATIC volatile BOOL_T s_calibrate_request   = FALSE;  /**< snap grav := lp */
STATIC volatile BOOL_T s_clear_calib_request = FALSE;  /**< drop the KV seed */

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
 *   2) Fast LPF: lp += (raw - lp) / 16    — denoises ADC jitter.
 *   3) Slow EMA: grav += (lp - grav) / 1024 — tracks current gravity
 *      vector across pose changes (τ ≈ 20 s at 50 Hz).
 *   4) Service hand-shake flags:
 *        - s_calibrate_request   : snap grav := lp now AND persist.
 *        - s_clear_calib_request : forget the KV seed; tracker keeps
 *                                  running off whatever it has.
 *   5) dyn = lp - grav            — the high-pass output, what we ship.
 *   6) Project dyn onto vehicle fwd / lat using the user-picked
 *      mounting-pose preset, then publish.
 *
 * Why we update everything HERE rather than in the requester thread:
 * the IMU task is the SOLE writer to grav_*, so cross-task atomicity
 * reduces to volatile semantics and `s_calibrate_request` is just a
 * one-shot "do this on next tick" handoff.
 */
STATIC VOID_T __imu_task(VOID_T *arg)
{
    (void)arg;
    PR_NOTICE("IMU sampler started");

    /* Seed the slow gravity tracker from KV if the user has previously
     * hit "Reset Zero" on this device — avoids a full ~τ_slow=20 s
     * convergence period after every cold boot. If KV has nothing,
     * we'll snap on the first sample below. */
    const APP_PREFS_T *p = app_kv_prefs();
    if (p && p->g_offset_valid) {
        s_grav_x = (int32_t)p->g_offset_mg[0];
        s_grav_y = (int32_t)p->g_offset_mg[1];
        s_grav_z = (int32_t)p->g_offset_mg[2];
        s_grav_inited        = TRUE;
        s_grav_seeded_from_kv = TRUE;
        PR_NOTICE("IMU loaded gravity seed: x=%d y=%d z=%d",
                  (int)s_grav_x, (int)s_grav_y, (int)s_grav_z);
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

            /* First-sample bootstrap: if KV had no seed, snap the slow
             * tracker to the very first lp_* so we don't read 1 g of
             * apparent gravity for the first ~τ seconds. */
            if (!s_grav_inited) {
                s_grav_x = lp_x;
                s_grav_y = lp_y;
                s_grav_z = lp_z;
                s_grav_inited = TRUE;
            }

            if (s_calibrate_request) {
                /* User pressed "Reset Zero" — force the slow tracker
                 * to converge instantly and persist as next-boot seed. */
                s_grav_x = lp_x;
                s_grav_y = lp_y;
                s_grav_z = lp_z;
                s_grav_seeded_from_kv = TRUE;
                s_calibrate_request   = FALSE;
                /* Flash write may take a few ms — that's fine, we'll
                 * just skip 1-2 sample periods. */
                app_kv_set_g_offset((int16_t)lp_x, (int16_t)lp_y, (int16_t)lp_z);
                PR_NOTICE("IMU gravity snapped: x=%d y=%d z=%d",
                          (int)lp_x, (int)lp_y, (int)lp_z);
            } else {
                /* Slow tracker on the fast LPF output. With α=1/1024
                 * at 50 Hz we get τ ≈ 20.5 s — DC content (gravity in
                 * any orientation) flows in, dynamic accel survives. */
                s_grav_x += (lp_x - s_grav_x) >> IMU_GRAV_ALPHA_SHIFT;
                s_grav_y += (lp_y - s_grav_y) >> IMU_GRAV_ALPHA_SHIFT;
                s_grav_z += (lp_z - s_grav_z) >> IMU_GRAV_ALPHA_SHIFT;
            }

            if (s_clear_calib_request) {
                /* User asked to forget the persisted seed. We don't
                 * touch the running grav_* — the EMA keeps tracking,
                 * we just clear KV so the next boot starts unseeded. */
                s_grav_seeded_from_kv = FALSE;
                s_clear_calib_request = FALSE;
                app_kv_clear_g_offset();
                PR_NOTICE("IMU gravity seed cleared");
            }

            /* High-pass: dynamic acceleration with gravity removed. */
            int32_t dyn_x = lp_x - s_grav_x;
            int32_t dyn_y = lp_y - s_grav_y;
            int32_t dyn_z = lp_z - s_grav_z;

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
            int32_t out_fwd = dyn_x * ax->fwd[0] + dyn_y * ax->fwd[1] + dyn_z * ax->fwd[2];
            int32_t out_lat = dyn_x * ax->lat[0] + dyn_y * ax->lat[1] + dyn_z * ax->lat[2];

            /* Roll attitude is computed from the lp (not dyn) signal:
             * roll is by definition the orientation of the gravity
             * vector relative to the device, which is what lp tracks. */
            int32_t roll = __atan2_deg10(lp_y, lp_z);
            /* Publish the dyn body-frame triplet as g_x/y/z for any
             * future logging / diagnostics consumer; the UI reads
             * g_fwd_mg / g_lat_mg. */
            app_metric_set_imu(dyn_x, dyn_y, dyn_z, out_fwd, out_lat, roll);
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
 * @brief Force the slow gravity tracker to snap to the current sample.
 *
 * Equivalent of "tap to zero now": instead of waiting τ ≈ 20 s for the
 * slow EMA to converge after a pose change, this seeds it instantly
 * (grav := lp) and persists the new seed to KV so the next boot
 * starts already converged. Posts a flag that the IMU sample loop
 * services on its next tick. Returns OPRT_COM_ERROR if the IMU has
 * not yet produced a sample (first ~20 ms after start).
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
 * @brief Forget the persisted gravity seed (KV only).
 *
 * The running slow EMA keeps tracking — there's nothing meaningful
 * to "uncalibrate" on a continuous tracker — we just clear the
 * stored bootstrap seed so the next boot starts from the first
 * sample. UI surfaces this as the menu's "Forget" affordance.
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
 * @brief Whether a persisted gravity seed exists in KV.
 *
 * The semantics flipped slightly in v1.1 — there is no longer a
 * "static offset is active" mode, the slow EMA is always live —
 * but the function still answers the question the menu wants:
 * "do we have a saved seed (and therefore should we show '(saved)'
 * vs '(tap to zero)' next to the calibrate row)?".
 */
BOOL_T sensor_imu_calibration_active(VOID_T)
{
    return s_grav_seeded_from_kv ? TRUE : FALSE;
}
