/**
 * @file elm327_ble.h
 * @brief Minimal BLE central transport for ELM327 OBD-II adapters.
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 *
 * @note Only BLE 4.0 (HM-10/JDY-08 style 0xFFE0/0xFFE1, OBDLink/Vgate
 *       0xFFF0/0xFFF1+0xFFF2 style) is supported. Classic Bluetooth SPP is
 *       NOT supported.
 */
#ifndef __ELM327_BLE_H__
#define __ELM327_BLE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "tuya_cloud_types.h"

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef enum {
    ELM_BLE_EV_SCAN_STARTED = 0,
    ELM_BLE_EV_SCAN_TIMEOUT,
    ELM_BLE_EV_DEVICE_FOUND,
    ELM_BLE_EV_CONNECTING,
    ELM_BLE_EV_CONNECTED,        /**< writeable & subscribed pipe ready */
    ELM_BLE_EV_DISCONNECTED,
    ELM_BLE_EV_RX_LINE           /**< complete '>' or '\r' terminated line */
} ELM_BLE_EVENT_E;

typedef struct {
    ELM_BLE_EVENT_E type;
    uint8_t  peer_addr[6];
    char    *line;               /**< only valid for ELM_BLE_EV_RX_LINE */
    int      rssi;
} ELM_BLE_EVENT_T;

typedef void (*ELM_BLE_CB)(const ELM_BLE_EVENT_T *evt);

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */
/**
 * @brief Initialise the BLE stack in central mode and prepare internal state.
 * @param[in] cb event callback (called from BLE stack thread context — keep
 *               work brief, push longer work onto your own thread)
 * @return OPRT_OK on success
 */
OPERATE_RET elm327_ble_init(ELM_BLE_CB cb);

/**
 * @brief Start scanning for ELM327 BLE adapters.
 * @param[in] preferred_addr optional bound address (NULL to scan freely)
 * @return OPRT_OK on success
 */
OPERATE_RET elm327_ble_scan_start(const uint8_t *preferred_addr);

/**
 * @brief Stop scanning explicitly.
 * @return OPRT_OK on success
 */
OPERATE_RET elm327_ble_scan_stop(VOID_T);

/**
 * @brief Disconnect any active link.
 * @return OPRT_OK on success
 */
OPERATE_RET elm327_ble_disconnect(VOID_T);

/**
 * @brief Send a raw ASCII command. The driver appends a CR ('\r') as ELM327 expects.
 * @param[in] str null-terminated ASCII string (no trailing CR)
 * @return OPRT_OK on success, OPRT_NOT_FOUND if not connected
 */
OPERATE_RET elm327_ble_send(const char *str);

/**
 * @brief Whether a writeable pipe is currently open.
 * @return TRUE if connected
 */
BOOL_T elm327_ble_is_connected(VOID_T);

#ifdef __cplusplus
}
#endif

#endif /* __ELM327_BLE_H__ */
