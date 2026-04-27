/**
 * @file board_io.h
 * @brief Board I/O helpers (I2C bus mutex, ADC battery, GPIO).
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __BOARD_IO_H__
#define __BOARD_IO_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "tuya_cloud_types.h"

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */
/**
 * @brief Initialize board-level I/O mutex used for I2C access.
 * @return OPRT_OK on success
 * @note Touch driver already calls tkl_i2c_init() during display registration;
 *       this module only adds a mutex for cross-driver coordination.
 */
OPERATE_RET board_io_init(VOID_T);

/**
 * @brief Acquire I2C bus mutex.
 * @return OPRT_OK on success
 */
OPERATE_RET board_io_i2c_lock(VOID_T);

/**
 * @brief Release I2C bus mutex.
 * @return OPRT_OK on success
 */
OPERATE_RET board_io_i2c_unlock(VOID_T);

/**
 * @brief Read battery voltage in millivolts.
 * @param[out] mv pointer to receive voltage value
 * @return OPRT_OK on success
 * @note Returns last good reading on transient ADC failure.
 */
OPERATE_RET board_io_read_battery_mv(uint32_t *mv);

/**
 * @brief Read whether battery is charging.
 * @return TRUE if charging
 */
BOOL_T board_io_is_charging(VOID_T);

#ifdef __cplusplus
}
#endif

#endif /* __BOARD_IO_H__ */
