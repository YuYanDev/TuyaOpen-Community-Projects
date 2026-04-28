/**
 * @file elm327_spp.c
 * @brief BT-Classic SPP transport stub (every method returns OPRT_NOT_SUPPORTED).
 * @version 1.0
 * @date 2026-04-28
 * @copyright Copyright (c) Tuya Inc.
 *
 * See elm327_spp.h for the rationale behind shipping a stub in v1.8.
 * The real implementation is planned for v1.9 once the BK7258 vendor
 * SDK SPP master + legacy-PIN APIs are integrated.
 *
 * Implementation notes for v1.9 (pre-flighted here so we don't lose
 * the design context between revisions):
 *
 *  1. Initialise BR/EDR by calling `bk_bt_gap_init()` (BK7258 SDK)
 *     and registering a GAP callback that handles:
 *      - INQUIRY_RES   → emit OBD_IO_EV_DEVICE_FOUND when the BD_ADDR
 *                        matches the bound peer or the name contains
 *                        "OBDII"/"OBD"/"V-LINK"/"ICAR"/"VGATE".
 *      - PIN_REQ       → emit OBD_IO_EV_PAIR_REQUEST and call
 *                        `bk_bt_gap_pin_reply()` with "1234" first,
 *                        fall back to "0000" on rejection.
 *      - AUTH_CMPL     → continue to SPP connect step.
 *
 *  2. Initialise SPP by calling `bk_dm_spp_init()` and registering a
 *     callback that handles:
 *      - SPP_OPEN      → emit OBD_IO_EV_CONNECTED.
 *      - SPP_DATA_IND  → feed the byte stream into the same line
 *                        buffer machinery as elm327_ble.c::__feed_rx
 *                        (factor that helper out into a shared file
 *                        before v1.9 to avoid duplication).
 *      - SPP_CLOSE     → emit OBD_IO_EV_DISCONNECTED.
 *
 *  3. Connect with `bk_dm_spp_connect(BD_ADDR, RFCOMM_CHANNEL=1)` —
 *     the standard channel for ELM327 clones. Channel 0 is GAP and
 *     channel 1 is what every 25K80 firmware uses.
 *
 *  4. Send via `bk_dm_spp_send(handle, buf, len)` with the same CR
 *     terminator convention as the BLE backend.
 *
 *  5. Pin the bt_mode flag through KV so reboots remember the choice;
 *     prompt the user once for the legacy PIN if neither 1234 nor
 *     0000 work (the stub UI should already reserve a string for
 *     that prompt — see ui.c MENU_BT_PAIR).
 */
#include "elm327_spp.h"
#include "tal_api.h"
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define ELM_SPP_LOG(fmt, ...)   PR_DEBUG("[elm-spp] " fmt, ##__VA_ARGS__)

/* ---------------------------------------------------------------------------
 * Stub trampolines
 * --------------------------------------------------------------------------- */
/**
 * @brief Stub init — log intent, do not touch the BT stack.
 *
 * Returning OPRT_NOT_SUPPORTED here is what tells the session machine
 * (and therefore the UI overlay) to surface a "BT Classic not yet
 * supported on T5AI" message instead of looping in scan retries.
 */
STATIC OPERATE_RET __spp_init(OBD_IO_CB cb)
{
    (void)cb;
    ELM_SPP_LOG("init: stub — full SPP backend lands in v1.9 (see header)");
    return OPRT_NOT_SUPPORTED;
}

/**
 * @brief Stub scan_start — never enters BR/EDR inquiry.
 */
STATIC OPERATE_RET __spp_scan_start(const uint8_t *preferred_addr)
{
    (void)preferred_addr;
    ELM_SPP_LOG("scan_start: stub");
    return OPRT_NOT_SUPPORTED;
}

/**
 * @brief Stub scan_stop — no inquiry to cancel.
 */
STATIC OPERATE_RET __spp_scan_stop(VOID_T)
{
    return OPRT_OK;     /* idempotent — return OK so callers can always invoke it */
}

/**
 * @brief Stub disconnect — no link to drop.
 */
STATIC OPERATE_RET __spp_disconnect(VOID_T)
{
    return OPRT_OK;
}

/**
 * @brief Stub send — refuse with NOT_SUPPORTED so callers don't busy-loop.
 */
STATIC OPERATE_RET __spp_send(const char *str)
{
    (void)str;
    return OPRT_NOT_SUPPORTED;
}

/**
 * @brief Stub is_connected — always FALSE.
 */
STATIC BOOL_T __spp_is_connected(VOID_T)
{
    return FALSE;
}

/* ---------------------------------------------------------------------------
 * OBD_IO_T vtable export
 * --------------------------------------------------------------------------- */
/**
 * @brief Static SPP backend instance (stub).
 */
STATIC const OBD_IO_T s_io_spp = {
    .name         = "SPP",
    .init         = __spp_init,
    .scan_start   = __spp_scan_start,
    .scan_stop    = __spp_scan_stop,
    .disconnect   = __spp_disconnect,
    .send         = __spp_send,
    .is_connected = __spp_is_connected,
};

/**
 * @brief Get the SPP backend vtable instance.
 * @return non-NULL pointer to the stub vtable
 */
const OBD_IO_T *obd_io_spp(VOID_T)
{
    return &s_io_spp;
}
