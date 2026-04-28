/**
 * @file sensor_imu.h
 * @brief IMU sampler task that fans QMI8658 readings into the metric bus.
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __SENSOR_IMU_H__
#define __SENSOR_IMU_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "tuya_cloud_types.h"

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */
/**
 * @brief Initialise QMI8658 hardware and start sampler thread (50 Hz).
 * @return OPRT_OK on success, OPRT_COM_ERROR if device probe failed.
 * @note Failure does NOT abort boot: the rest of the app keeps running and
 *       the G-force gauge simply stays invalid.
 */
OPERATE_RET sensor_imu_start(VOID_T);

/**
 * @brief Capture the device's CURRENT static gravity vector and persist it
 *        as the G-force zero calibration. Subsequent IMU samples have
 *        the offsets subtracted before being pushed to the metric bus, so
 *        the GoPro target reticle reads (0, 0) when the user is at rest
 *        — irrespective of mounting orientation (vertical, slanted,
 *        rotated 90°).
 *
 * @return OPRT_OK on success, OPRT_COM_ERROR if no IMU sample is available.
 * @note Safe to call from any task. The actual capture happens on the next
 *       IMU sample tick (≤ 20 ms) and persistence is performed on the IMU
 *       task to avoid cross-task state corruption.
 */
OPERATE_RET sensor_imu_calibrate_zero(VOID_T);

/**
 * @brief Discard the user-supplied G zero calibration.
 * @return OPRT_OK on success
 */
OPERATE_RET sensor_imu_clear_calibration(VOID_T);

/**
 * @brief Whether a user-supplied G calibration is currently active.
 */
BOOL_T sensor_imu_calibration_active(VOID_T);

#ifdef __cplusplus
}
#endif

#endif /* __SENSOR_IMU_H__ */
