/**
 * @file ble_compat.c
 * @brief Weak fall-back stubs for BLE GATT-client (central) APIs.
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 *
 * Some Tuya hardware platforms (e.g. T5AI / BK7258 in its default profile)
 * ship a BLE host that only exposes the *peripheral* / *observer* halves of
 * the TKL Bluetooth API.  The advertising and scanning entry points (used to
 * detect ELM327 adapters) are present, but the GATT-client half — connect,
 * service / characteristic discovery, write / read — is gated by the
 * `TY_HS_BLE_ROLE_CENTRAL` macro inside the NimBLE host and therefore is
 * never linked into the final image.
 *
 * Without those symbols the application fails to link.  This file provides
 * weak fall-back implementations that report `OPRT_NOT_SUPPORTED` so the
 * firmware can still be flashed and run.  When the real strong definitions
 * are present (after enabling central in the platform port) they win during
 * link, and the rest of the OBD stack works unchanged.
 *
 * Effect at runtime when only stubs are linked:
 *   - `elm327_ble_scan_start()` still reports adapters via adv reports.
 *   - `elm327_ble_init()` succeeds (stack init + GAP/GATT callback register
 *     are available in the strong build).
 *   - `elm327_ble.c` will fail to actually open the GATT pipe; the OBD
 *     session never reaches `OBD_SES_READY`.  The user can switch to mock
 *     mode from the menu to drive the gauges with synthetic data.
 *
 * @attention The function signatures must mirror those declared in
 *            `tkl_bluetooth.h` exactly, otherwise the linker will silently
 *            shadow the real symbols.
 */
#include "tuya_cloud_types.h"
#include "tuya_error_code.h"
#include "tkl_bluetooth.h"

/* ---------------------------------------------------------------------------
 * Weak fall-back stubs (return OPRT_NOT_SUPPORTED)
 * --------------------------------------------------------------------------- */
/**
 * @brief Weak stub for `tkl_ble_gap_connect`. Reports "not supported".
 * @return OPRT_NOT_SUPPORTED always.
 */
__attribute__((weak)) OPERATE_RET
tkl_ble_gap_connect(TKL_BLE_GAP_ADDR_T const *p_peer_addr,
                    TKL_BLE_GAP_SCAN_PARAMS_T const *p_scan_params,
                    TKL_BLE_GAP_CONN_PARAMS_T const *p_conn_params)
{
    (void)p_peer_addr;
    (void)p_scan_params;
    (void)p_conn_params;
    return OPRT_NOT_SUPPORTED;
}

/**
 * @brief Weak stub for `tkl_ble_gattc_all_service_discovery`.
 * @return OPRT_NOT_SUPPORTED always.
 */
__attribute__((weak)) OPERATE_RET
tkl_ble_gattc_all_service_discovery(uint16_t conn_handle)
{
    (void)conn_handle;
    return OPRT_NOT_SUPPORTED;
}

/**
 * @brief Weak stub for `tkl_ble_gattc_all_char_discovery`.
 * @return OPRT_NOT_SUPPORTED always.
 */
__attribute__((weak)) OPERATE_RET
tkl_ble_gattc_all_char_discovery(uint16_t conn_handle,
                                 uint16_t start_handle,
                                 uint16_t end_handle)
{
    (void)conn_handle;
    (void)start_handle;
    (void)end_handle;
    return OPRT_NOT_SUPPORTED;
}

/**
 * @brief Weak stub for `tkl_ble_gattc_char_desc_discovery`.
 * @return OPRT_NOT_SUPPORTED always.
 */
__attribute__((weak)) OPERATE_RET
tkl_ble_gattc_char_desc_discovery(uint16_t conn_handle,
                                  uint16_t start_handle,
                                  uint16_t end_handle)
{
    (void)conn_handle;
    (void)start_handle;
    (void)end_handle;
    return OPRT_NOT_SUPPORTED;
}

/**
 * @brief Weak stub for `tkl_ble_gattc_write_without_rsp`.
 * @return OPRT_NOT_SUPPORTED always.
 */
__attribute__((weak)) OPERATE_RET
tkl_ble_gattc_write_without_rsp(uint16_t conn_handle, uint16_t char_handle,
                                uint8_t *p_data, uint16_t length)
{
    (void)conn_handle;
    (void)char_handle;
    (void)p_data;
    (void)length;
    return OPRT_NOT_SUPPORTED;
}

/**
 * @brief Weak stub for `tkl_ble_gattc_write`.
 * @return OPRT_NOT_SUPPORTED always.
 */
__attribute__((weak)) OPERATE_RET
tkl_ble_gattc_write(uint16_t conn_handle, uint16_t char_handle,
                    uint8_t *p_data, uint16_t length)
{
    (void)conn_handle;
    (void)char_handle;
    (void)p_data;
    (void)length;
    return OPRT_NOT_SUPPORTED;
}

/**
 * @brief Weak stub for `tkl_ble_gattc_read`.
 * @return OPRT_NOT_SUPPORTED always.
 */
__attribute__((weak)) OPERATE_RET
tkl_ble_gattc_read(uint16_t conn_handle, uint16_t char_handle)
{
    (void)conn_handle;
    (void)char_handle;
    return OPRT_NOT_SUPPORTED;
}
