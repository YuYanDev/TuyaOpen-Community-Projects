/**
 * @file app_metric.h
 * @brief Central metric bus shared by data producers (OBD/IMU/Mock) and consumers (UI).
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __APP_METRIC_H__
#define __APP_METRIC_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "tuya_cloud_types.h"

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef enum {
    APP_METRIC_WATER_TEMP = 0, /**< Engine coolant temp, OBD PID 0x05, °C */
    APP_METRIC_OIL_TEMP,       /**< Engine oil temp, OBD PID 0x5C, °C */
    APP_METRIC_INTAKE_TEMP,    /**< Intake air temp, OBD PID 0x0F, °C */
    APP_METRIC_FUEL_LEVEL,     /**< Fuel level, OBD PID 0x2F, % */
    APP_METRIC_OIL_PRESSURE,   /**< Oil pressure, OEM specific (Mode 22), kPa — usually N/A */
    APP_METRIC_VOLTAGE,        /**< Module voltage, OBD PID 0x42, mV */
    APP_METRIC_BOOST,          /**< Boost = MAP - BARO, kPa (signed) */
    APP_METRIC_G_FORCE,        /**< 2D G-force from QMI8658, scaled millig (read paired x/y) */
    APP_METRIC_COUNT
} APP_METRIC_E;

typedef enum {
    APP_DATA_SRC_NONE = 0,
    APP_DATA_SRC_OBD,
    APP_DATA_SRC_IMU,
    APP_DATA_SRC_MOCK
} APP_DATA_SRC_E;

typedef struct {
    int32_t  ect_c10;          /**< coolant in 0.1 °C */
    int32_t  oil_c10;          /**< oil temp in 0.1 °C */
    int32_t  iat_c10;          /**< intake temp in 0.1 °C */
    int32_t  fuel_pct10;       /**< fuel level in 0.1 % */
    int32_t  oil_kpa10;        /**< oil pressure in 0.1 kPa */
    int32_t  voltage_mv;       /**< module voltage in mV */
    int32_t  boost_kpa10;      /**< boost in 0.1 kPa, signed */
    int32_t  g_x_mg;           /**< raw accel X in IMU body frame (post zero-cal), milli-g */
    int32_t  g_y_mg;           /**< raw accel Y in IMU body frame (post zero-cal), milli-g */
    int32_t  g_z_mg;           /**< raw accel Z in IMU body frame (post zero-cal), milli-g */
    int32_t  g_fwd_mg;         /**< vehicle forward axis acceleration, milli-g (orient-projected) */
    int32_t  g_lat_mg;         /**< vehicle lateral (right) axis acceleration, milli-g (orient-projected) */
    int32_t  roll_deg10;       /**< roll angle in 0.1 ° */

    BOOL_T   valid[APP_METRIC_COUNT];
    uint32_t last_update_ms[APP_METRIC_COUNT];
    APP_DATA_SRC_E source;
} APP_METRIC_BUS_T;

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */
/**
 * @brief Initialize metric bus & its mutex. Idempotent.
 * @return OPRT_OK on success, error code on failure.
 */
OPERATE_RET app_metric_init(VOID_T);

/**
 * @brief Mark current data source on the bus.
 * @param[in] src new source identifier
 * @return none
 */
void app_metric_set_source(APP_DATA_SRC_E src);

/**
 * @brief Get current data source (without locking).
 * @return APP_DATA_SRC_E
 */
APP_DATA_SRC_E app_metric_get_source(VOID_T);

/**
 * @brief Atomic snapshot copy of the bus into caller buffer.
 * @param[out] out caller buffer
 * @return OPRT_OK on success, error code on failure.
 */
OPERATE_RET app_metric_snapshot(APP_METRIC_BUS_T *out);

/**
 * @brief Update a single metric channel and stamp it.
 * @param[in] m metric id
 * @param[in] value scaled int (semantic depends on metric, see APP_METRIC_BUS_T)
 * @return OPRT_OK on success
 */
OPERATE_RET app_metric_set(APP_METRIC_E m, int32_t value);

/**
 * @brief Update G-force triplet + orient-projected forward/lateral at once (IMU writer).
 *
 * The first three arguments are the raw post-zero-cal IMU body-frame
 * components (kept for diagnostics / future logging). `gfwd_mg` and
 * `glat_mg` are the projection of that vector onto the vehicle's
 * forward (positive = nose-forward acceleration / braking-negative)
 * and lateral (positive = right turn) axes — the GoPro reticle and
 * any future longitudinal/lateral display reads these.
 *
 * @param[in] gx_mg     raw IMU X, milli-g
 * @param[in] gy_mg     raw IMU Y, milli-g
 * @param[in] gz_mg     raw IMU Z, milli-g
 * @param[in] gfwd_mg   projected forward axis, milli-g
 * @param[in] glat_mg   projected lateral (right) axis, milli-g
 * @param[in] roll_deg10 roll angle in 0.1 °
 * @return OPRT_OK on success
 */
OPERATE_RET app_metric_set_imu(int32_t gx_mg, int32_t gy_mg, int32_t gz_mg,
                               int32_t gfwd_mg, int32_t glat_mg,
                               int32_t roll_deg10);

/**
 * @brief Invalidate all OBD metrics (e.g. on disconnect).
 * @return none
 */
void app_metric_invalidate_obd(VOID_T);

#ifdef __cplusplus
}
#endif

#endif /* __APP_METRIC_H__ */
