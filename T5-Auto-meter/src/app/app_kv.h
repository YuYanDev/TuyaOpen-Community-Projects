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
#include "obd_io.h"

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

/* Mounting orientation of the device (and therefore the QMI8658 IMU
 * inside it) relative to the vehicle frame. The user picks one from
 * the menu after physically installing the device in their car. The
 * IMU sampler uses this to project the (post zero-cal) live
 * acceleration vector onto the vehicle's forward & lateral axes
 * before publishing on the metric bus.
 *
 * Convention (matches the Waveshare T5AI-Touch-AMOLED-1.75 schematic):
 *   IMU +X  → screen-right when face-user 0°
 *   IMU +Y  → screen-up    when face-user 0°
 *   IMU +Z  → out-of-screen toward the user when face-user
 *
 * The dashboard mounting assumption is "screen face-user, windshield
 * is in front of the driver, so vehicle-forward is OUT of the screen
 * away from the user — i.e. -Z in IMU body frame for any face-user
 * rotation". For face-up (screen flat, parallel to ground), we assume
 * the user installed the device with the USB connector toward the
 * REAR of the vehicle, so vehicle-forward is +Y in IMU body frame.
 *
 * If the user has a different mount convention they can re-pick the
 * orientation menu item — the math is consistent for any of the 5
 * presets — or rotate the device to one of the supported poses.
 */
typedef enum {
    APP_G_ORIENT_FACE_UP   = 0,   /**< Chip flat, screen up, USB toward car rear */
    APP_G_ORIENT_USER_0    = 1,   /**< Screen face-user, USB at bottom */
    APP_G_ORIENT_USER_90   = 2,   /**< Screen face-user, rotated 90° CW (USB right) */
    APP_G_ORIENT_USER_180  = 3,   /**< Screen face-user, rotated 180° (USB top) */
    APP_G_ORIENT_USER_270  = 4,   /**< Screen face-user, rotated 270° CW (USB left) */
    APP_G_ORIENT_COUNT
} APP_G_ORIENT_E;

typedef struct {
    uint32_t gauge_enabled_mask;   /**< bitmask of APP_METRIC_E that are enabled */
    uint8_t  current_gauge;        /**< APP_METRIC_E currently shown */
    uint8_t  brightness_pct;       /**< 0..100 */
    uint8_t  unit_temp;            /**< APP_UNIT_TEMP_E */
    uint8_t  unit_press;           /**< APP_UNIT_PRESS_E */
    uint8_t  mock_enabled;         /**< 0/1 */
    uint8_t  bound_addr_valid;     /**< 0/1 */
    uint8_t  bound_addr[6];        /**< last connected ELM327 BLE MAC */
    /* G-force zero calibration. The IMU task subtracts these offsets from
     * the live (lp_x, lp_y, lp_z) samples before publishing them on the
     * metric bus so the GoPro-style target reticle reads (0,0) when the
     * device is sitting still in any installation orientation. Captured
     * by the menu's "Calibrate G" item. Range ±32 g is far more than the
     * 1 g of static gravity bias we ever need to cancel. */
    int16_t  g_offset_mg[3];       /**< [0]=X, [1]=Y, [2]=Z, in milli-g */
    uint8_t  g_offset_valid;       /**< 0=defaults (0,0,0), 1=user-calibrated */
    uint8_t  g_orient;             /**< APP_G_ORIENT_E, default = APP_G_ORIENT_FACE_UP */
    uint8_t  bt_mode;              /**< OBD_BT_MODE_E, default = OBD_BT_MODE_BLE */
    uint8_t  lang;                 /**< APP_LANG_E (0=EN, 1=ZH), default = EN */
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

/**
 * @brief Persist a fresh G-force zero-calibration vector.
 *
 * The IMU sampler captures the device's static gravity vector (filtered
 * lp_x/y/z when the user holds it still) and writes it here through the
 * menu "Calibrate G" item. After this call the GoPro target reticle
 * reads (0,0) for the current orientation regardless of mounting angle.
 *
 * @param[in] x_mg X axis offset in milli-g
 * @param[in] y_mg Y axis offset in milli-g
 * @param[in] z_mg Z axis offset in milli-g
 * @return OPRT_OK on success
 */
OPERATE_RET app_kv_set_g_offset(int16_t x_mg, int16_t y_mg, int16_t z_mg);

/**
 * @brief Discard the user-supplied G zero calibration (revert to factory).
 * @return OPRT_OK on success
 */
OPERATE_RET app_kv_clear_g_offset(VOID_T);

/**
 * @brief Set the device mounting orientation (face-up / face-user 0/90/180/270°).
 *
 * The IMU sampler uses this to project the live acceleration vector
 * onto the vehicle's forward & lateral axes. Persisted to KV so the
 * choice survives reboot. Picking a new orientation does NOT
 * automatically re-calibrate zero — the user should rest the device,
 * then trigger "Calibrate G" once for the static gravity bias of the
 * new pose.
 *
 * @param[in] orient one of APP_G_ORIENT_E (clamped to valid range)
 * @return OPRT_OK on success
 */
OPERATE_RET app_kv_set_g_orient(APP_G_ORIENT_E orient);

/**
 * @brief Set the OBD-II Bluetooth backend mode (BLE or SPP).
 *
 * The OBD session reads this on startup and on each rescan to pick a
 * transport backend. Switching modes triggers a backend teardown +
 * re-init on the next rescan, so the user should call
 * `obd_session_rescan()` after changing this. v1.8 ships SPP as a
 * stub that returns OPRT_NOT_SUPPORTED — the choice is still
 * persisted so it activates automatically once the v1.9 BK7258 SPP
 * backend is wired in.
 *
 * @param[in] mode one of OBD_BT_MODE_E
 * @return OPRT_OK on success
 */
OPERATE_RET app_kv_set_bt_mode(OBD_BT_MODE_E mode);

/**
 * @brief Set the active UI language.
 *
 * The lang enum (APP_LANG_E) is declared in app_i18n.h. We accept it
 * as a plain uint8_t here to avoid pulling app_i18n.h into app_kv.h
 * (would create a circular include since app_i18n.c persists via
 * this setter).
 *
 * @param[in] lang one of APP_LANG_E (cast to uint8_t at the call site)
 * @return OPRT_OK on success, OPRT_INVALID_PARM if out of range (>= 2)
 */
OPERATE_RET app_kv_set_lang(uint8_t lang);

#ifdef __cplusplus
}
#endif

#endif /* __APP_KV_H__ */
