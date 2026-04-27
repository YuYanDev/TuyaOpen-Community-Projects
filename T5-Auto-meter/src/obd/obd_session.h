/**
 * @file obd_session.h
 * @brief OBD-II polling session (ELM327 init + round-robin PID poll).
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __OBD_SESSION_H__
#define __OBD_SESSION_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "tuya_cloud_types.h"
#include "app_metric.h"

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef enum {
    OBD_SES_OFF = 0,
    OBD_SES_SCAN,            /**< scanning BLE for adapter */
    OBD_SES_LINKED,          /**< BLE pipe up, AT init in progress */
    OBD_SES_READY,           /**< AT done, polling PIDs */
    OBD_SES_LINK_LOST,       /**< pipe dropped, will re-scan */
} OBD_SES_STATE_E;

typedef void (*OBD_SES_STATE_CB)(OBD_SES_STATE_E st);

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */
/**
 * @brief Start the OBD session task. Idempotent.
 * @param[in] state_cb optional state-change callback (can be NULL)
 * @return OPRT_OK on success
 */
OPERATE_RET obd_session_start(OBD_SES_STATE_CB state_cb);

/**
 * @brief Stop the OBD session, disconnect any open BLE link.
 * @return OPRT_OK on success
 */
OPERATE_RET obd_session_stop(VOID_T);

/**
 * @brief Re-trigger BLE scanning.
 * @return OPRT_OK on success
 */
OPERATE_RET obd_session_rescan(VOID_T);

/**
 * @brief Refresh the polling PID list from the current preferences.
 * @return OPRT_OK on success
 */
OPERATE_RET obd_session_refresh_poll_list(VOID_T);

/**
 * @brief Get current session state.
 */
OBD_SES_STATE_E obd_session_state(VOID_T);

#ifdef __cplusplus
}
#endif

#endif /* __OBD_SESSION_H__ */
