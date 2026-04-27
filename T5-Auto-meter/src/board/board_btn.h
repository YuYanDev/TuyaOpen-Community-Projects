/**
 * @file board_btn.h
 * @brief Dual-button (KEY + PWR) registration with high-level event broker.
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __BOARD_BTN_H__
#define __BOARD_BTN_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "tuya_cloud_types.h"

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef enum {
    BOARD_BTN_KEY = 0,           /**< KEY button (GPIO 12) */
    BOARD_BTN_PWR,               /**< PWR button (GPIO 18) */
    BOARD_BTN_COUNT
} BOARD_BTN_E;

typedef enum {
    BOARD_BTN_EV_SHORT = 0,      /**< Short single click */
    BOARD_BTN_EV_DOUBLE,         /**< Double click */
    BOARD_BTN_EV_LONG_1S,        /**< Long press ≥ 1.2s released before 3s */
    BOARD_BTN_EV_LONG_3S         /**< Long press ≥ 3s — fires once at threshold */
} BOARD_BTN_EVT_E;

typedef void (*BOARD_BTN_EVT_CB)(BOARD_BTN_E btn, BOARD_BTN_EVT_E evt);

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */
/**
 * @brief Initialise dual-button system. Idempotent.
 * @return OPRT_OK on success
 * @note Must be called after board_register_hardware() so that GPIO buttons
 *       are already registered with the TDD layer.
 */
OPERATE_RET board_btn_init(VOID_T);

/**
 * @brief Set application-level event callback.
 * @param[in] cb callback function (NULL clears)
 * @return OPRT_OK on success
 * @note Callback fires from the TDL button task — do NOT call LVGL APIs
 *       directly without acquiring lv_vendor_disp_lock().
 */
OPERATE_RET board_btn_set_cb(BOARD_BTN_EVT_CB cb);

#ifdef __cplusplus
}
#endif

#endif /* __BOARD_BTN_H__ */
