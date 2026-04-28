/**
 * @file elm327_spp.h
 * @brief BT-Classic SPP transport stub for ELM327 v1.5 (25K80) dongles.
 * @version 1.0
 * @date 2026-04-28
 * @copyright Copyright (c) Tuya Inc.
 *
 * v1.8 — STUB ONLY. The 25K80-based ELM327 v1.5 family (the cheap,
 * iconic blue-LED OBD-II dongle) speaks Bluetooth Classic SPP with
 * legacy PIN pairing (1234 or 0000). Real SPP master support on T5AI
 * needs the BK7258 vendor SDK functions:
 *
 *   - bk_dm_spp_init / bk_dm_spp_register_callback
 *   - bk_dm_spp_connect (master-mode socket on RFCOMM channel 1)
 *   - bk_dm_spp_send
 *   - bk_bt_gap_set_pin / bk_bt_gap_pin_reply (legacy pairing)
 *
 * These are NOT exposed by the Tuya TKL Bluetooth abstraction today —
 * tkl_bluetooth_bredr.h only surfaces audio sink (A2DP/HFP/AVRCP) for
 * the speaker reference applications. Wiring SPP up means bypassing
 * TKL and pulling in the platform SDK directly, which deserves its
 * own milestone (v1.9).
 *
 * For v1.8 we still expose the OBD_IO_T contract via obd_io_spp() so:
 *   - The bt-mode menu can offer "BT Classic SPP" as an option.
 *   - The session machine surfaces a clean OPRT_NOT_SUPPORTED that
 *     the UI can translate into "BT Classic not yet supported on
 *     T5AI — use a BLE 4.0 ELM327 for now".
 *
 * The stub never touches the BT stack, so toggling between BLE and
 * SPP from the menu is safe and reversible.
 */
#ifndef __ELM327_SPP_H__
#define __ELM327_SPP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "obd_io.h"

#ifdef __cplusplus
}
#endif

#endif /* __ELM327_SPP_H__ */
