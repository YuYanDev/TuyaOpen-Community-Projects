/**
 * @file elm327_ble.h
 * @brief BLE 4.0 GATT central transport for ELM327 OBD-II adapters.
 * @version 1.1
 * @date 2026-04-28
 * @copyright Copyright (c) Tuya Inc.
 *
 * @note v1.8 — this file now exposes BOTH the legacy direct API (kept
 *       so existing callers continue to compile) AND the unified
 *       OBD_IO_T vtable instance via obd_io_ble(). New callers should
 *       only consume the vtable; the direct functions are retained
 *       to keep the migration patch small.
 *
 * Adapters covered:
 *   - HM-10 / JDY-08 style (0xFFE0 / 0xFFE1 single-pipe characteristic)
 *   - OBDLink LX BT / Vgate iCar Pro BLE (0xFFF0 / 0xFFF1+0xFFF2 split)
 *
 * Bluetooth Classic SPP (the original ELM327 v1.5 25K80 dongles) is
 * NOT served by this file — see elm327_spp.c (v1.8 stub).
 */
#ifndef __ELM327_BLE_H__
#define __ELM327_BLE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "tuya_cloud_types.h"
#include "obd_io.h"

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */
/**
 * @brief Initialise the BLE stack in central mode and prepare internal state.
 * @param[in] cb event callback (called from BLE stack thread context — keep
 *               work brief, push longer work onto your own thread)
 * @return OPRT_OK on success
 */
OPERATE_RET elm327_ble_init(OBD_IO_CB cb);

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
