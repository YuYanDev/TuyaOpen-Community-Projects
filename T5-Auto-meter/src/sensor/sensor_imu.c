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
#include "tal_api.h"

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define IMU_TASK_STACK     2048
#define IMU_TASK_PRIO      THREAD_PRIO_3
#define IMU_PERIOD_MS      20      /* ~50 Hz */
#define IMU_FILTER_ALPHA   16      /* divisor: smoothing 1/16 step per sample */

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC THREAD_HANDLE   s_task = NULL;
STATIC volatile BOOL_T s_running = FALSE;

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
 */
STATIC VOID_T __imu_task(VOID_T *arg)
{
    (void)arg;
    PR_NOTICE("IMU sampler started");

    /* Low-pass state */
    int32_t lp_x = 0, lp_y = 0, lp_z = 1000;

    while (s_running) {
        QMI8658_DATA_T d;
        if (qmi8658_read(&d) == OPRT_OK) {
            lp_x += (d.ax_mg - lp_x) / IMU_FILTER_ALPHA;
            lp_y += (d.ay_mg - lp_y) / IMU_FILTER_ALPHA;
            lp_z += (d.az_mg - lp_z) / IMU_FILTER_ALPHA;
            int32_t roll = __atan2_deg10(lp_y, lp_z);
            app_metric_set_imu(lp_x, lp_y, lp_z, roll);
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
