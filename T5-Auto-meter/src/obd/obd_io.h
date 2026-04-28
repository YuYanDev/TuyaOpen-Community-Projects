/**
 * @file obd_io.h
 * @brief Backend-agnostic OBD-II transport vtable (BLE 4.0 GATT or BT-Classic SPP).
 * @version 1.0
 * @date 2026-04-28
 * @copyright Copyright (c) Tuya Inc.
 *
 * Why a vtable?
 * -------------
 * ELM327 v1.5 dongles split into two physical-layer families:
 *
 *   - BLE 4.0 GATT  (newer "Mini OBD" / HM-10 / JDY-08 / Vgate iCar Pro
 *                    BLE / OBDLink LX BT). The chip exposes a write +
 *                    notify characteristic pair and the device pairs
 *                    Just-Works (no PIN). This is what `elm327_ble.c`
 *                    drives, and it works through the public Tuya TKL
 *                    BLE central API today.
 *
 *   - Bluetooth Classic SPP  (the original 25K80-based "ELM327 v1.5"
 *                    dongles, mass-produced as the iCar v2/v3, the
 *                    blue-LED MINI OBD-II, and most generic eBay
 *                    clones). These present a Serial Port Profile
 *                    socket and pair with a legacy PIN (1234 or 0000).
 *                    On T5AI this requires the BK7258 vendor SDK
 *                    `bk_dm_spp_*` + `bk_bt_gap_set_pin_*` APIs that
 *                    Tuya TKL does not yet expose.
 *
 * Both backends speak the same upper-layer protocol (ASCII AT/PID
 * commands terminated by '\r', responses terminated by '\r' lines and
 * a '>' prompt). So the upper-half session machine is identical; only
 * the link layer differs. We hide that difference behind a tiny vtable
 * that exposes init/scan/disconnect/send/is_connected. obd_session
 * picks one at startup based on the user's menu selection.
 *
 * For v1.8 the SPP backend ships as a STUB that always returns
 * OPRT_NOT_SUPPORTED — see elm327_spp.c. Picking it from the menu
 * surfaces a clear "BT Classic not yet supported on T5AI" overlay so
 * the user knows their 25K80 dongle won't work yet (full BK7258 SDK
 * SPP integration is planned for v1.9).
 */
#ifndef __OBD_IO_H__
#define __OBD_IO_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "tuya_cloud_types.h"

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
/**
 * @brief Bluetooth backend selector persisted in prefs (`bt_mode`).
 */
typedef enum {
    OBD_BT_MODE_BLE = 0,    /**< BLE 4.0 GATT (default, works today) */
    OBD_BT_MODE_SPP = 1,    /**< BT-Classic SPP (ELM327 v1.5 25K80; v1.9 stub) */
    OBD_BT_MODE_COUNT
} OBD_BT_MODE_E;

/**
 * @brief Common event surface emitted by every transport backend.
 *
 * Both BLE and SPP backends translate their stack-native events into
 * this unified shape before invoking the registered OBD_IO_CB. The
 * upper layer (obd_session) only sees this shape, so a backend swap
 * costs zero code in the session machine.
 */
typedef enum {
    OBD_IO_EV_SCAN_STARTED = 0,
    OBD_IO_EV_SCAN_TIMEOUT,
    OBD_IO_EV_DEVICE_FOUND,    /**< adapter visible; UI may show RSSI */
    OBD_IO_EV_CONNECTING,
    OBD_IO_EV_PAIR_REQUEST,    /**< SPP only: legacy PIN dialog (1234 / 0000) */
    OBD_IO_EV_CONNECTED,       /**< writeable pipe ready */
    OBD_IO_EV_DISCONNECTED,
    OBD_IO_EV_RX_LINE          /**< complete '>' or '\r' terminated line */
} OBD_IO_EVENT_E;

/**
 * @brief Transport event payload.
 *
 * @note `line` is borrowed for the duration of the callback; backends
 *       must not free it underneath the session, and the session must
 *       not retain the pointer past callback return (it's pushed into
 *       a copy queue).
 */
typedef struct {
    OBD_IO_EVENT_E type;
    uint8_t        peer_addr[6];   /**< MAC of the adapter being acted on */
    const char    *line;           /**< only valid for OBD_IO_EV_RX_LINE */
    int            rssi;
} OBD_IO_EVENT_T;

typedef void (*OBD_IO_CB)(const OBD_IO_EVENT_T *evt);

/**
 * @brief Pure-virtual transport interface.
 *
 * Each backend exposes a single static instance via obd_io_ble() /
 * obd_io_spp(); the session holds a `const OBD_IO_T *` and calls
 * through it. Function semantics are identical for every backend:
 *
 *   - init       : register cb, initialise the underlying stack
 *   - scan_start : start a 30 s discovery (preferred_addr is optional)
 *   - scan_stop  : abort discovery
 *   - disconnect : drop any open pipe (idempotent)
 *   - send       : send a CR-terminated command (driver appends '\r')
 *   - is_connected : TRUE if writeable pipe is open
 *
 * Return OPRT_NOT_SUPPORTED from any function for backends that the
 * platform doesn't currently support — obd_session will surface this
 * to the menu/overlay rather than treat it as a transient failure.
 */
typedef struct {
    const char *name;       /**< human-readable, "BLE" / "SPP", for logs/UI */
    OPERATE_RET (*init)(OBD_IO_CB cb);
    OPERATE_RET (*scan_start)(const uint8_t *preferred_addr);
    OPERATE_RET (*scan_stop)(VOID_T);
    OPERATE_RET (*disconnect)(VOID_T);
    OPERATE_RET (*send)(const char *str);
    BOOL_T      (*is_connected)(VOID_T);
} OBD_IO_T;

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */
/**
 * @brief Get the BLE 4.0 GATT backend instance.
 *
 * Implemented in elm327_ble.c. Always available.
 *
 * @return non-NULL pointer to the static vtable
 */
const OBD_IO_T *obd_io_ble(VOID_T);

/**
 * @brief Get the BT-Classic SPP backend instance.
 *
 * Implemented in elm327_spp.c. For v1.8 this is a stub: every method
 * returns OPRT_NOT_SUPPORTED. Wired up so the menu can offer SPP and
 * the session machine can surface the platform limitation cleanly.
 *
 * @return non-NULL pointer to the static vtable (returns OPRT_NOT_SUPPORTED on call)
 */
const OBD_IO_T *obd_io_spp(VOID_T);

/**
 * @brief Resolve a backend pointer from the persisted bt_mode enum.
 * @param[in] mode OBD_BT_MODE_BLE or OBD_BT_MODE_SPP (clamped to BLE for unknowns)
 * @return non-NULL backend vtable
 */
const OBD_IO_T *obd_io_for_mode(OBD_BT_MODE_E mode);

#ifdef __cplusplus
}
#endif

#endif /* __OBD_IO_H__ */
