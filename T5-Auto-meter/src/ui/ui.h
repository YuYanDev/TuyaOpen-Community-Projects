/**
 * @file ui.h
 * @brief Top-level UI manager: boot animation, BLE waiting overlay, gauges, menu.
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __UI_H__
#define __UI_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "tuya_cloud_types.h"
#include "obd_session.h"
#include "app_metric.h"

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef enum {
    UI_STATE_BOOT_SWEEP = 0,
    UI_STATE_WAIT_LINK,
    UI_STATE_MAIN,
    UI_STATE_MENU,
} UI_STATE_E;

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */
/**
 * @brief Build the LVGL UI hierarchy and start the refresh timer.
 *        Must be called inside lv_vendor_disp_lock/unlock by the caller.
 * @return OPRT_OK on success
 */
OPERATE_RET ui_init(VOID_T);

/**
 * @brief Inform the UI about an OBD session state change.
 * @param[in] st new session state
 * @return none
 */
void ui_on_obd_state(OBD_SES_STATE_E st);

/**
 * @brief Show the next enabled gauge (driven by KEY short press).
 *        Inert in MENU and BOOT_SWEEP states.
 * @return none
 */
void ui_show_next_gauge(VOID_T);

/**
 * @brief Toggle the menu open/closed (driven by PWR short press).
 *        Ignored during BOOT_SWEEP so the boot animation can finish.
 * @return none
 */
void ui_toggle_menu(VOID_T);

/**
 * @brief Notify UI that mock-mode preference changed; the UI may need to
 *        update state and (re)trigger a smooth needle transition.
 * @param[in] enabled new mock pref
 * @return none
 */
void ui_on_mock_changed(BOOL_T enabled);

/**
 * @brief Get current high-level UI state (mostly for diagnostics/tests).
 */
UI_STATE_E ui_state(VOID_T);

#ifdef __cplusplus
}
#endif

#endif /* __UI_H__ */
