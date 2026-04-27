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

#ifdef __cplusplus
}
#endif

#endif /* __SENSOR_IMU_H__ */
