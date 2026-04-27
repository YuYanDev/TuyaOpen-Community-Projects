/**
 * @file board_pwr.h
 * @brief Power latch and shutdown helpers (GPIO 19 self-hold).
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __BOARD_PWR_H__
#define __BOARD_PWR_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "tuya_cloud_types.h"

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */
/**
 * @brief Latch the power-enable line HIGH so the device stays alive after the
 *        user releases the PWR button.
 * @return OPRT_OK on success
 * @note Must be called as early as possible after boot.
 */
OPERATE_RET board_pwr_latch(VOID_T);

/**
 * @brief Drive the power-enable line LOW so the regulator turns off.
 * @return none — control never returns once power is cut.
 */
void board_pwr_shutdown(VOID_T);

#ifdef __cplusplus
}
#endif

#endif /* __BOARD_PWR_H__ */
