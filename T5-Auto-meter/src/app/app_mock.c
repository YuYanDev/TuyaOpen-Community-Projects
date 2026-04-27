/**
 * @file app_mock.c
 * @brief Mock data generator producing slowly varying plausible OBD telemetry.
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 */
#include "app_mock.h"
#include "app_metric.h"
#include "tal_api.h"

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define MOCK_TASK_STACK     2048
#define MOCK_TASK_PRIO      THREAD_PRIO_3
#define MOCK_TICK_MS        100

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC THREAD_HANDLE s_task = NULL;
STATIC volatile BOOL_T s_running = FALSE;

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
/**
 * @brief Map a sine-like wave [-1024..1024] from a uint32 phase 0..2047.
 * @param[in] phase 0..2047
 * @return scaled sine value in [-1024, 1024]
 */
STATIC int32_t __sine1024(uint32_t phase)
{
    static const int16_t k_sin[64] = {
        0,    100,  200,  297,  392,  483,  569,  650,  724,  791,  851,  903,  946,  980,  1004, 1019,
        1024, 1019, 1004, 980,  946,  903,  851,  791,  724,  650,  569,  483,  392,  297,  200,  100,
        0,   -100, -200, -297, -392, -483, -569, -650, -724, -791, -851, -903, -946, -980, -1004,-1019,
        -1024,-1019,-1004,-980, -946, -903, -851, -791, -724, -650, -569, -483, -392, -297, -200, -100
    };
    phase &= 0x3F; /* 64 entries cover full cycle for our coarse mock */
    return k_sin[phase];
}

/**
 * @brief Mock task body. Generates varying values for all metrics.
 */
STATIC VOID_T __mock_task(VOID_T *arg)
{
    (void)arg;
    uint32_t tick = 0;
    PR_NOTICE("mock generator started");
    app_metric_set_source(APP_DATA_SRC_MOCK);
    while (s_running) {
        /* Coolant: 60..95 °C with slow ramp + tiny ripple */
        int32_t ramp = (int32_t)(tick % 600) / 6; /* 0..99 */
        int32_t ect = 600 + ramp * 4 + __sine1024(tick / 4) / 80;
        if (ect > 950) ect = 950 - (ect - 950);
        app_metric_set(APP_METRIC_WATER_TEMP, ect);

        /* Oil temp: tracks coolant + offset */
        app_metric_set(APP_METRIC_OIL_TEMP, ect + 80 + __sine1024(tick / 8) / 100);

        /* Intake: 20..45 °C */
        app_metric_set(APP_METRIC_INTAKE_TEMP, 250 + __sine1024(tick / 6) / 8);

        /* Fuel level: slow drain 80% -> 20% */
        int32_t fuel = 800 - (int32_t)((tick / 30) % 600);
        app_metric_set(APP_METRIC_FUEL_LEVEL, fuel);

        /* Voltage: 13800 ± 600 mV */
        app_metric_set(APP_METRIC_VOLTAGE, 13800 + __sine1024(tick / 5) / 2);

        /* Boost: -30..+150 kPa, breathing */
        app_metric_set(APP_METRIC_BOOST, __sine1024(tick / 3) * 15 / 10);

        /* Fake oil pressure: 200..600 kPa */
        app_metric_set(APP_METRIC_OIL_PRESSURE, 4000 + __sine1024(tick / 6) * 2);

        tick++;
        tal_system_sleep(MOCK_TICK_MS);
    }
    PR_NOTICE("mock generator stopped");
    s_task = NULL;
}

/**
 * @brief Start the mock generator task. Idempotent.
 */
OPERATE_RET app_mock_start(VOID_T)
{
    if (s_running) {
        return OPRT_OK;
    }
    s_running = TRUE;
    THREAD_CFG_T cfg = {
        .stackDepth = MOCK_TASK_STACK,
        .priority = MOCK_TASK_PRIO,
        .thrdname = "app_mock"
    };
    OPERATE_RET rt = tal_thread_create_and_start(&s_task, NULL, NULL, __mock_task, NULL, &cfg);
    if (rt != OPRT_OK) {
        s_running = FALSE;
        return rt;
    }
    return OPRT_OK;
}

/**
 * @brief Stop the mock generator task.
 */
OPERATE_RET app_mock_stop(VOID_T)
{
    if (!s_running) {
        return OPRT_OK;
    }
    s_running = FALSE;
    /* Let the task observe flag and exit naturally; allow up to 300ms */
    for (int i = 0; i < 30 && s_task != NULL; i++) {
        tal_system_sleep(10);
    }
    return OPRT_OK;
}

/**
 * @brief Whether mock task is currently running.
 */
BOOL_T app_mock_is_running(VOID_T)
{
    return s_running;
}
