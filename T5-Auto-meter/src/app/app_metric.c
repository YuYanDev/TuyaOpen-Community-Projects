/**
 * @file app_metric.c
 * @brief Implementation of the central metric bus.
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 */
#include "app_metric.h"
#include "tal_api.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC APP_METRIC_BUS_T s_bus;
STATIC MUTEX_HANDLE     s_lock = NULL;
STATIC BOOL_T           s_inited = FALSE;

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
/**
 * @brief Initialize metric bus & its mutex. Idempotent.
 * @return OPRT_OK on success, error code on failure.
 */
OPERATE_RET app_metric_init(VOID_T)
{
    if (s_inited) {
        return OPRT_OK;
    }
    memset(&s_bus, 0, sizeof(s_bus));
    s_bus.source = APP_DATA_SRC_NONE;
    OPERATE_RET rt = tal_mutex_create_init(&s_lock);
    if (rt != OPRT_OK) {
        return rt;
    }
    s_inited = TRUE;
    return OPRT_OK;
}

/**
 * @brief Mark current data source on the bus.
 * @param[in] src new source identifier
 * @return none
 */
void app_metric_set_source(APP_DATA_SRC_E src)
{
    if (!s_inited) {
        return;
    }
    tal_mutex_lock(s_lock);
    s_bus.source = src;
    tal_mutex_unlock(s_lock);
}

/**
 * @brief Get current data source.
 * @return APP_DATA_SRC_E
 */
APP_DATA_SRC_E app_metric_get_source(VOID_T)
{
    if (!s_inited) {
        return APP_DATA_SRC_NONE;
    }
    APP_DATA_SRC_E src;
    tal_mutex_lock(s_lock);
    src = s_bus.source;
    tal_mutex_unlock(s_lock);
    return src;
}

/**
 * @brief Atomic snapshot copy of the bus.
 * @param[out] out caller buffer
 * @return OPRT_OK on success
 */
OPERATE_RET app_metric_snapshot(APP_METRIC_BUS_T *out)
{
    if (out == NULL) {
        return OPRT_INVALID_PARM;
    }
    if (!s_inited) {
        return OPRT_NOT_FOUND;
    }
    tal_mutex_lock(s_lock);
    memcpy(out, &s_bus, sizeof(*out));
    tal_mutex_unlock(s_lock);
    return OPRT_OK;
}

/**
 * @brief Update a single metric channel and stamp it.
 * @param[in] m metric id
 * @param[in] value scaled value (semantics per metric)
 * @return OPRT_OK on success
 */
OPERATE_RET app_metric_set(APP_METRIC_E m, int32_t value)
{
    if (!s_inited) {
        return OPRT_NOT_FOUND;
    }
    if (m >= APP_METRIC_COUNT) {
        return OPRT_INVALID_PARM;
    }
    tal_mutex_lock(s_lock);
    switch (m) {
    case APP_METRIC_WATER_TEMP:
        s_bus.ect_c10 = value;
        break;
    case APP_METRIC_OIL_TEMP:
        s_bus.oil_c10 = value;
        break;
    case APP_METRIC_INTAKE_TEMP:
        s_bus.iat_c10 = value;
        break;
    case APP_METRIC_FUEL_LEVEL:
        s_bus.fuel_pct10 = value;
        break;
    case APP_METRIC_OIL_PRESSURE:
        s_bus.oil_kpa10 = value;
        break;
    case APP_METRIC_VOLTAGE:
        s_bus.voltage_mv = value;
        break;
    case APP_METRIC_BOOST:
        s_bus.boost_kpa10 = value;
        break;
    default:
        break;
    }
    s_bus.valid[m] = TRUE;
    s_bus.last_update_ms[m] = tal_system_get_millisecond();
    tal_mutex_unlock(s_lock);
    return OPRT_OK;
}

/**
 * @brief Update raw IMU triplet + orient-projected forward/lateral pair.
 */
OPERATE_RET app_metric_set_imu(int32_t gx_mg, int32_t gy_mg, int32_t gz_mg,
                               int32_t gfwd_mg, int32_t glat_mg,
                               int32_t roll_deg10)
{
    if (!s_inited) {
        return OPRT_NOT_FOUND;
    }
    tal_mutex_lock(s_lock);
    s_bus.g_x_mg = gx_mg;
    s_bus.g_y_mg = gy_mg;
    s_bus.g_z_mg = gz_mg;
    s_bus.g_fwd_mg = gfwd_mg;
    s_bus.g_lat_mg = glat_mg;
    s_bus.roll_deg10 = roll_deg10;
    s_bus.valid[APP_METRIC_G_FORCE] = TRUE;
    s_bus.last_update_ms[APP_METRIC_G_FORCE] = tal_system_get_millisecond();
    tal_mutex_unlock(s_lock);
    return OPRT_OK;
}

/**
 * @brief Invalidate all OBD-sourced metrics.
 * @note IMU metrics are preserved because they don't depend on OBD link.
 */
void app_metric_invalidate_obd(VOID_T)
{
    if (!s_inited) {
        return;
    }
    tal_mutex_lock(s_lock);
    s_bus.valid[APP_METRIC_WATER_TEMP] = FALSE;
    s_bus.valid[APP_METRIC_OIL_TEMP] = FALSE;
    s_bus.valid[APP_METRIC_INTAKE_TEMP] = FALSE;
    s_bus.valid[APP_METRIC_FUEL_LEVEL] = FALSE;
    s_bus.valid[APP_METRIC_OIL_PRESSURE] = FALSE;
    s_bus.valid[APP_METRIC_VOLTAGE] = FALSE;
    s_bus.valid[APP_METRIC_BOOST] = FALSE;
    tal_mutex_unlock(s_lock);
}
