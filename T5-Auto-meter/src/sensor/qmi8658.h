/**
 * @file qmi8658.h
 * @brief Minimal QMI8658 6-axis IMU driver over Tuya tkl_i2c.
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 *
 * @note Register layout follows the QST Corporation QMI8658 datasheet rev 1.07.
 *       Compared to the 01_Factory_Firmware vendor driver, this version skips
 *       run-time range / ODR setters and uses a single tested configuration:
 *       accel ±4 g, gyro ±512 dps, ODR = 235 Hz on both, low-power FIFO bypass.
 */
#ifndef __QMI8658_H__
#define __QMI8658_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "tuya_cloud_types.h"

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define QMI8658_I2C_ADDR_LOW         0x6A
#define QMI8658_I2C_ADDR_HIGH        0x6B
#define QMI8658_WHO_AM_I_VAL         0x05

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef struct {
    int16_t ax;     /**< raw accel X */
    int16_t ay;     /**< raw accel Y */
    int16_t az;     /**< raw accel Z */
    int16_t gx;     /**< raw gyro X */
    int16_t gy;     /**< raw gyro Y */
    int16_t gz;     /**< raw gyro Z */
    int16_t temp;   /**< chip temperature, 0.1 °C units */
} QMI8658_RAW_T;

typedef struct {
    int32_t ax_mg;       /**< accel X in milli-g */
    int32_t ay_mg;       /**< accel Y in milli-g */
    int32_t az_mg;       /**< accel Z in milli-g */
    int32_t gx_dps10;    /**< gyro X in 0.1 dps */
    int32_t gy_dps10;    /**< gyro Y in 0.1 dps */
    int32_t gz_dps10;    /**< gyro Z in 0.1 dps */
    int32_t temp_c10;    /**< temperature in 0.1 °C */
} QMI8658_DATA_T;

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */
/**
 * @brief Probe the device and bring it out of soft-reset into a fixed
 *        production configuration.
 * @param[in] i2c_port Tuya I2C port index (e.g. TUYA_I2C_NUM_0)
 * @param[in] i2c_addr 7-bit slave address (0x6A or 0x6B)
 * @return OPRT_OK on success, error code on failure.
 * @note Caller is responsible for tkl_i2c_init() — typically the touch driver
 *       has already done it.
 */
OPERATE_RET qmi8658_init(uint8_t i2c_port, uint8_t i2c_addr);

/**
 * @brief Sample the device and convert to engineering units.
 * @param[out] out caller buffer
 * @return OPRT_OK on success
 */
OPERATE_RET qmi8658_read(QMI8658_DATA_T *out);

#ifdef __cplusplus
}
#endif

#endif /* __QMI8658_H__ */
