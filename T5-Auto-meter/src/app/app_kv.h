/**
 * @file app_kv.h
 * @brief Persisted user preferences (selected gauges, brightness, units, mock flag).
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __APP_KV_H__
#define __APP_KV_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "tuya_cloud_types.h"
#include "app_metric.h"

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define APP_KV_GAUGE_BITS        (1u << APP_METRIC_COUNT)

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef enum {
    APP_UNIT_TEMP_C = 0,
    APP_UNIT_TEMP_F = 1
} APP_UNIT_TEMP_E;

typedef enum {
    APP_UNIT_PRESS_KPA = 0,
    APP_UNIT_PRESS_BAR = 1
} APP_UNIT_PRESS_E;

typedef struct {
    uint32_t gauge_enabled_mask;   /**< bitmask of APP_METRIC_E that are enabled */
    uint8_t  current_gauge;        /**< APP_METRIC_E currently shown */
    uint8_t  brightness_pct;       /**< 0..100 */
    uint8_t  unit_temp;            /**< APP_UNIT_TEMP_E */
    uint8_t  unit_press;           /**< APP_UNIT_PRESS_E */
    uint8_t  mock_enabled;         /**< 0/1 */
    uint8_t  bound_addr_valid;     /**< 0/1 */
    uint8_t  bound_addr[6];        /**< last connected ELM327 BLE MAC */
} APP_PREFS_T;

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */
/**
 * @brief Initialize KV system & load preferences. Falls back to defaults on missing.
 * @return OPRT_OK on success, error code on failure.
 * @note Calls tal_kv_init() once; safe to call multiple times.
 */
OPERATE_RET app_kv_init(VOID_T);

/**
 * @brief Get a const pointer to the in-memory preferences (read only).
 * @return pointer to APP_PREFS_T (never NULL after init)
 */
const APP_PREFS_T *app_kv_prefs(VOID_T);

/**
 * @brief Persist current preferences to KV storage.
 * @return OPRT_OK on success, error code on failure.
 */
OPERATE_RET app_kv_save(VOID_T);

/**
 * @brief Toggle whether a given gauge is enabled.
 * @param[in] m gauge metric id
 * @param[in] enabled TRUE/FALSE
 * @return OPRT_OK on success
 */
OPERATE_RET app_kv_set_gauge_enabled(APP_METRIC_E m, BOOL_T enabled);

/**
 * @brief Set current gauge selection.
 * @param[in] m metric id
 * @return OPRT_OK on success
 */
OPERATE_RET app_kv_set_current_gauge(APP_METRIC_E m);

/**
 * @brief Toggle mock mode flag.
 * @param[in] enabled TRUE/FALSE
 * @return OPRT_OK on success
 */
OPERATE_RET app_kv_set_mock_enabled(BOOL_T enabled);

/**
 * @brief Set brightness 0..100.
 * @param[in] pct percentage
 * @return OPRT_OK on success
 */
OPERATE_RET app_kv_set_brightness(uint8_t pct);

/**
 * @brief Save bound BLE peer address.
 * @param[in] addr 6-byte MAC pointer
 * @return OPRT_OK on success
 */
OPERATE_RET app_kv_set_bound_addr(const uint8_t *addr);

/**
 * @brief Forget bound device.
 * @return OPRT_OK on success
 */
OPERATE_RET app_kv_clear_bound_addr(VOID_T);

#ifdef __cplusplus
}
#endif

#endif /* __APP_KV_H__ */
