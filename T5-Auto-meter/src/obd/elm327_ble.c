/**
 * @file elm327_ble.c
 * @brief BLE central transport that exposes ELM327 over a transparent line pipe.
 * @version 1.1
 * @date 2026-04-28
 * @copyright Copyright (c) Tuya Inc.
 *
 * Supported adapter families (BLE 4.0):
 *   - HM-10 / JDY-08 / TI CC2541   : service 0xFFE0, char 0xFFE1 (WriteNoRsp + Notify)
 *   - Vgate / OBDLink LX style     : service 0xFFF0, 0xFFF1=Notify, 0xFFF2=WriteNoRsp
 *   - Veepeak alt name / 18F0      : service 0x18F0, 0x2AF0=Notify, 0x2AF1=WriteNoRsp
 *
 * Classic Bluetooth (SPP) ELM327 v1.5 dongles are NOT served by this
 * file — the SPP backend is in elm327_spp.c (v1.8 stub).
 *
 * v1.8 — also exports the OBD_IO_T BLE backend instance via
 * obd_io_ble(). The vtable forwards to the same internal state, so
 * both the legacy direct API and the unified vtable can coexist.
 */
#include "elm327_ble.h"
#include "obd_io.h"
#include "app_config.h"
#include "tal_api.h"
#include "tkl_bluetooth.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define ELM_LINE_BUF_SIZE       256
#define ELM_LOG(fmt, ...)       PR_DEBUG("[elm-ble] " fmt, ##__VA_ARGS__)
#define ELM_WARN(fmt, ...)      PR_WARN("[elm-ble] " fmt, ##__VA_ARGS__)

#define ELM_UUID_HM10_SVC       0xFFE0
#define ELM_UUID_HM10_CHR       0xFFE1
#define ELM_UUID_VGATE_SVC      0xFFF0
#define ELM_UUID_VGATE_NTF      0xFFF1
#define ELM_UUID_VGATE_WR       0xFFF2
#define ELM_UUID_VEEPEAK_SVC    0x18F0
#define ELM_UUID_VEEPEAK_NTF    0x2AF0
#define ELM_UUID_VEEPEAK_WR     0x2AF1

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef enum {
    LST_IDLE = 0,
    LST_SCANNING,
    LST_CONNECTING,
    LST_DISCOVERING_SVC,
    LST_DISCOVERING_CHR,
    LST_DISCOVERING_DESC,
    LST_ENABLING_NOTIFY,
    LST_READY,
} ELM_LINK_STATE_E;

typedef struct {
    OBD_IO_CB            cb;
    ELM_LINK_STATE_E     state;
    MUTEX_HANDLE         lock;

    uint16_t             conn_handle;
    TKL_BLE_GAP_ADDR_T   peer_addr;
    int8_t               peer_rssi;

    uint16_t             svc_start;
    uint16_t             svc_end;
    uint16_t             write_handle;
    uint16_t             notify_handle;
    uint16_t             cccd_handle;
    uint16_t             vendor;     /* 0=hm10, 1=vgate, 2=veepeak */

    uint8_t              prefer_addr[6];
    BOOL_T               has_prefer;

    char                 line_buf[ELM_LINE_BUF_SIZE];
    uint16_t             line_len;
} ELM_CTX_T;

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC ELM_CTX_T s_ctx = {0};

/* ---------------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------------- */
STATIC VOID_T __on_gap_evt(TKL_BLE_GAP_PARAMS_EVT_T *evt);
STATIC VOID_T __on_gatt_evt(TKL_BLE_GATT_PARAMS_EVT_T *evt);

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------- */
/**
 * @brief Notify the application layer of an event.
 * @param[in] type event type
 * @param[in] line optional line buffer (only for OBD_IO_EV_RX_LINE)
 * @return none
 */
STATIC VOID_T __emit(OBD_IO_EVENT_E type, const char *line)
{
    if (s_ctx.cb == NULL) {
        return;
    }
    OBD_IO_EVENT_T e = {
        .type = type,
        .rssi = s_ctx.peer_rssi,
        .line = line,
    };
    memcpy(e.peer_addr, s_ctx.peer_addr.addr, 6);
    s_ctx.cb(&e);
}

/**
 * @brief Match advertisement payload against known ELM327-class signatures.
 *
 * Scans both the AD-types 0x09 (full local name), 0x08 (short local name)
 * and 0x02..0x07 (UUID lists) for hints. Returns TRUE when the device looks
 * like an ELM327 BLE adapter.
 */
STATIC BOOL_T __adv_looks_like_elm327(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0) {
        return FALSE;
    }
    uint16_t i = 0;
    while (i + 1 < len) {
        uint8_t fl = data[i];
        if (fl == 0 || (i + fl) >= len) {
            break;
        }
        uint8_t type = data[i + 1];
        const uint8_t *payload = &data[i + 2];
        uint8_t plen = fl - 1;

        if (type == 0x08 || type == 0x09) {
            /* Local name */
            const char *names[] = {"OBDII", "OBD2", "VLINK", "VGATE",
                                   "VEEPEAK", "ICAR", "ELM", "BTOBD"};
            for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
                size_t nl = strlen(names[k]);
                if (plen >= nl) {
                    char tmp[32] = {0};
                    size_t cp = (plen < sizeof(tmp) - 1) ? plen : sizeof(tmp) - 1;
                    memcpy(tmp, payload, cp);
                    for (size_t z = 0; z < cp; z++) {
                        tmp[z] = (char)toupper((unsigned char)tmp[z]);
                    }
                    if (strstr(tmp, names[k])) {
                        return TRUE;
                    }
                }
            }
        } else if (type == 0x02 || type == 0x03) {
            /* Incomplete/Complete list of 16-bit UUIDs */
            for (uint8_t k = 0; k + 1 < plen; k += 2) {
                uint16_t u = payload[k] | (payload[k + 1] << 8);
                if (u == ELM_UUID_HM10_SVC || u == ELM_UUID_VGATE_SVC ||
                    u == ELM_UUID_VEEPEAK_SVC) {
                    return TRUE;
                }
            }
        }
        i += fl + 1;
    }
    return FALSE;
}

/**
 * @brief Push received bytes into line buffer, emit on terminator.
 *
 * ELM327 lines end with \r and the prompt '>' marks end of response. We treat
 * either as a line break to keep upper-layer parsing simple.
 */
STATIC VOID_T __feed_rx(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\r' || c == '\n' || c == '>') {
            if (s_ctx.line_len > 0) {
                s_ctx.line_buf[s_ctx.line_len] = '\0';
                __emit(OBD_IO_EV_RX_LINE, s_ctx.line_buf);
                s_ctx.line_len = 0;
            }
            if (c == '>') {
                /* Forward an empty marker so consumer can detect end-of-frame */
                static const char prompt[2] = {'>', '\0'};
                __emit(OBD_IO_EV_RX_LINE, prompt);
            }
        } else if ((size_t)s_ctx.line_len + 1 < sizeof(s_ctx.line_buf)) {
            s_ctx.line_buf[s_ctx.line_len++] = c;
        } else {
            /* Overflow safety: drop and reset */
            ELM_WARN("rx overflow");
            s_ctx.line_len = 0;
        }
    }
}

/**
 * @brief Convert a 16-bit UUID from a TKL discovery record (any width).
 */
STATIC uint16_t __uuid16_from(const TKL_BLE_UUID_T *u)
{
    if (u == NULL) {
        return 0;
    }
    if (u->uuid_type == TKL_BLE_UUID_TYPE_16) {
        return u->uuid.uuid16;
    }
    if (u->uuid_type == TKL_BLE_UUID_TYPE_32) {
        return (uint16_t)u->uuid.uuid32;
    }
    /* 128-bit UUIDs: bytes [12..13] hold the 16-bit alias for SIG-derived
     * UUIDs (Bluetooth base UUID layout, little-endian). */
    return (uint16_t)(u->uuid.uuid128[12] | (u->uuid.uuid128[13] << 8));
}

/* ---------------------------------------------------------------------------
 * GAP callback
 * --------------------------------------------------------------------------- */
/**
 * @brief Handle GAP events from the BLE stack.
 */
STATIC VOID_T __on_gap_evt(TKL_BLE_GAP_PARAMS_EVT_T *evt)
{
    if (evt == NULL) {
        return;
    }
    switch (evt->type) {
    case TKL_BLE_EVT_STACK_INIT:
        ELM_LOG("stack init result=%d", evt->result);
        break;

    case TKL_BLE_GAP_EVT_ADV_REPORT: {
        if (s_ctx.state != LST_SCANNING) {
            return;
        }
        TKL_BLE_GAP_ADV_REPORT_T *r = &evt->gap_event.adv_report;
        BOOL_T match = FALSE;
        if (s_ctx.has_prefer &&
            memcmp(r->peer_addr.addr, s_ctx.prefer_addr, 6) == 0) {
            match = TRUE;
        } else if (!s_ctx.has_prefer &&
                   __adv_looks_like_elm327(r->data.p_data, r->data.length)) {
            match = TRUE;
        }
        if (!match) {
            return;
        }
        memcpy(&s_ctx.peer_addr, &r->peer_addr, sizeof(TKL_BLE_GAP_ADDR_T));
        s_ctx.peer_rssi = r->rssi;
        ELM_LOG("found adapter rssi=%d", r->rssi);
        __emit(OBD_IO_EV_DEVICE_FOUND, NULL);

        tkl_ble_gap_scan_stop();

        TKL_BLE_GAP_SCAN_PARAMS_T sp = {
            .extended = 0, .active = 1,
            .scan_phys = TKL_BLE_GAP_PHY_1MBPS,
            .interval = 0x0050, .window = 0x0030, .timeout = 0,
            .scan_channel_map = 0x07,
        };
        TKL_BLE_GAP_CONN_PARAMS_T cp = {
            .conn_interval_min = 0x0018,
            .conn_interval_max = 0x0028,
            .conn_latency = 0,
            .conn_sup_timeout = 0x01F4,
            .connection_timeout = 5000,
        };
        s_ctx.state = LST_CONNECTING;
        __emit(OBD_IO_EV_CONNECTING, NULL);
        if (tkl_ble_gap_connect(&s_ctx.peer_addr, &sp, &cp) != OPRT_OK) {
            ELM_WARN("connect kickoff failed");
            s_ctx.state = LST_IDLE;
            __emit(OBD_IO_EV_DISCONNECTED, NULL);
        }
    } break;

    case TKL_BLE_GAP_EVT_CONNECT:
        ELM_LOG("link up conn=%u", evt->conn_handle);
        s_ctx.conn_handle = evt->conn_handle;
        s_ctx.state = LST_DISCOVERING_SVC;
        tkl_ble_gattc_exchange_mtu_request(s_ctx.conn_handle, 247);
        tkl_ble_gattc_all_service_discovery(s_ctx.conn_handle);
        break;

    case TKL_BLE_GAP_EVT_DISCONNECT:
        ELM_LOG("link down reason=0x%02x", evt->gap_event.disconnect.reason);
        s_ctx.state = LST_IDLE;
        s_ctx.conn_handle = TKL_BLE_GATT_INVALID_HANDLE;
        s_ctx.write_handle = 0;
        s_ctx.notify_handle = 0;
        s_ctx.cccd_handle = 0;
        s_ctx.line_len = 0;
        __emit(OBD_IO_EV_DISCONNECTED, NULL);
        break;

    default:
        break;
    }
}

/* ---------------------------------------------------------------------------
 * GATT callback
 * --------------------------------------------------------------------------- */
/**
 * @brief Handle GATT events for service/char/desc discovery and notifications.
 */
STATIC VOID_T __on_gatt_evt(TKL_BLE_GATT_PARAMS_EVT_T *evt)
{
    if (evt == NULL) {
        return;
    }

    switch (evt->type) {
    case TKL_BLE_GATT_EVT_PRIM_SEV_DISCOVERY: {
        TKL_BLE_GATT_SVC_DISC_TYPE_T *svc = &evt->gatt_event.svc_disc;
        ELM_LOG("svc count=%u", svc->svc_num);
        for (uint8_t i = 0; i < svc->svc_num; i++) {
            uint16_t u = __uuid16_from(&svc->services[i].uuid);
            if (u == ELM_UUID_HM10_SVC) {
                s_ctx.vendor = 0;
            } else if (u == ELM_UUID_VGATE_SVC) {
                s_ctx.vendor = 1;
            } else if (u == ELM_UUID_VEEPEAK_SVC) {
                s_ctx.vendor = 2;
            } else {
                continue;
            }
            s_ctx.svc_start = svc->services[i].start_handle;
            s_ctx.svc_end = svc->services[i].end_handle;
            ELM_LOG("match svc 0x%04x [0x%04x..0x%04x]", u,
                    s_ctx.svc_start, s_ctx.svc_end);
            s_ctx.state = LST_DISCOVERING_CHR;
            tkl_ble_gattc_all_char_discovery(s_ctx.conn_handle,
                                             s_ctx.svc_start, s_ctx.svc_end);
            return;
        }
        ELM_WARN("no compatible service found, dropping link");
        tkl_ble_gap_disconnect(s_ctx.conn_handle,
                               TKL_BLE_HCI_LOCAL_HOST_TERMINATED_CONNECTION);
    } break;

    case TKL_BLE_GATT_EVT_CHAR_DISCOVERY: {
        TKL_BLE_GATT_CHAR_DISC_TYPE_T *cd = &evt->gatt_event.char_disc;
        for (uint8_t i = 0; i < cd->char_num; i++) {
            uint16_t u = __uuid16_from(&cd->characteristics[i].uuid);
            uint16_t h = cd->characteristics[i].handle;
            if (s_ctx.vendor == 0 && u == ELM_UUID_HM10_CHR) {
                s_ctx.write_handle = h;
                s_ctx.notify_handle = h;
            } else if (s_ctx.vendor == 1) {
                if (u == ELM_UUID_VGATE_NTF) {
                    s_ctx.notify_handle = h;
                } else if (u == ELM_UUID_VGATE_WR) {
                    s_ctx.write_handle = h;
                }
            } else if (s_ctx.vendor == 2) {
                if (u == ELM_UUID_VEEPEAK_NTF) {
                    s_ctx.notify_handle = h;
                } else if (u == ELM_UUID_VEEPEAK_WR) {
                    s_ctx.write_handle = h;
                }
            }
        }
        ELM_LOG("chr write=0x%04x notify=0x%04x", s_ctx.write_handle,
                s_ctx.notify_handle);
        if (s_ctx.notify_handle == 0 || s_ctx.write_handle == 0) {
            ELM_WARN("missing chars, dropping link");
            tkl_ble_gap_disconnect(s_ctx.conn_handle,
                                   TKL_BLE_HCI_LOCAL_HOST_TERMINATED_CONNECTION);
            return;
        }
        s_ctx.state = LST_DISCOVERING_DESC;
        /* The CCCD lives just after the notify char value handle */
        tkl_ble_gattc_char_desc_discovery(s_ctx.conn_handle,
                                          s_ctx.notify_handle,
                                          s_ctx.notify_handle + 4);
    } break;

    case TKL_BLE_GATT_EVT_CHAR_DESC_DISCOVERY: {
        s_ctx.cccd_handle = evt->gatt_event.desc_disc.cccd_handle;
        ELM_LOG("cccd=0x%04x", s_ctx.cccd_handle);
        if (s_ctx.cccd_handle == 0) {
            /* Fallback: assume value+1 */
            s_ctx.cccd_handle = (uint16_t)(s_ctx.notify_handle + 1);
        }
        uint8_t en[2] = {0x01, 0x00};
        s_ctx.state = LST_ENABLING_NOTIFY;
        if (tkl_ble_gattc_write(s_ctx.conn_handle, s_ctx.cccd_handle, en, 2) != OPRT_OK) {
            ELM_WARN("cccd write fail, fall back to write_no_rsp");
            tkl_ble_gattc_write_without_rsp(s_ctx.conn_handle, s_ctx.cccd_handle, en, 2);
        }
        s_ctx.state = LST_READY;
        __emit(OBD_IO_EV_CONNECTED, NULL);
    } break;

    case TKL_BLE_GATT_EVT_NOTIFY_INDICATE_RX: {
        TKL_BLE_DATA_REPORT_T *r = &evt->gatt_event.data_report;
        if (r->report.p_data && r->report.length) {
            __feed_rx(r->report.p_data, r->report.length);
        }
    } break;

    case TKL_BLE_GATT_EVT_MTU_RSP:
        ELM_LOG("mtu negotiated %u", evt->gatt_event.exchange_mtu);
        break;

    default:
        break;
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */
/**
 * @brief Init BLE stack and prepare context.
 */
OPERATE_RET elm327_ble_init(OBD_IO_CB cb)
{
    if (s_ctx.lock != NULL) {
        s_ctx.cb = cb;
        return OPRT_OK;
    }
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.cb = cb;
    s_ctx.conn_handle = TKL_BLE_GATT_INVALID_HANDLE;
    OPERATE_RET rt = tal_mutex_create_init(&s_ctx.lock);
    if (rt != OPRT_OK) {
        return rt;
    }

    rt = tkl_ble_stack_init(TKL_BLE_ROLE_CLIENT);
    if (rt != OPRT_OK) {
        ELM_WARN("stack init failed=%d", rt);
        return rt;
    }
    tkl_ble_gap_callback_register(__on_gap_evt);
    tkl_ble_gatt_callback_register(__on_gatt_evt);
    return OPRT_OK;
}

/**
 * @brief Start scan for ELM327 adapters.
 */
OPERATE_RET elm327_ble_scan_start(const uint8_t *preferred_addr)
{
    if (s_ctx.lock == NULL) {
        return OPRT_RESOURCE_NOT_READY;
    }
    if (preferred_addr) {
        memcpy(s_ctx.prefer_addr, preferred_addr, 6);
        s_ctx.has_prefer = TRUE;
    } else {
        s_ctx.has_prefer = FALSE;
    }
    s_ctx.state = LST_SCANNING;

    TKL_BLE_GAP_SCAN_PARAMS_T sp = {
        .extended = 0,
        .active = 1,
        .scan_phys = TKL_BLE_GAP_PHY_1MBPS,
        .interval = 0x00A0,   /* 100 ms */
        .window   = 0x0050,   /* 50 ms */
        .timeout  = APP_BLE_SCAN_TIMEOUT_MS / 10,
        .scan_channel_map = 0x07,
    };
    OPERATE_RET rt = tkl_ble_gap_scan_start(&sp);
    if (rt == OPRT_OK) {
        __emit(OBD_IO_EV_SCAN_STARTED, NULL);
    } else {
        s_ctx.state = LST_IDLE;
    }
    return rt;
}

/**
 * @brief Stop scanning explicitly.
 */
OPERATE_RET elm327_ble_scan_stop(VOID_T)
{
    if (s_ctx.state == LST_SCANNING) {
        s_ctx.state = LST_IDLE;
    }
    return tkl_ble_gap_scan_stop();
}

/**
 * @brief Disconnect from peer.
 */
OPERATE_RET elm327_ble_disconnect(VOID_T)
{
    if (s_ctx.conn_handle == TKL_BLE_GATT_INVALID_HANDLE) {
        return OPRT_OK;
    }
    return tkl_ble_gap_disconnect(s_ctx.conn_handle,
                                  TKL_BLE_HCI_LOCAL_HOST_TERMINATED_CONNECTION);
}

/**
 * @brief Send a command. Appends a CR.
 */
OPERATE_RET elm327_ble_send(const char *str)
{
    if (str == NULL) {
        return OPRT_INVALID_PARM;
    }
    if (s_ctx.state != LST_READY || s_ctx.write_handle == 0) {
        return OPRT_NOT_FOUND;
    }
    size_t n = strlen(str);
    if (n == 0 || n > 32) {
        return OPRT_INVALID_PARM;
    }
    uint8_t buf[40];
    memcpy(buf, str, n);
    buf[n++] = '\r';
    return tkl_ble_gattc_write_without_rsp(s_ctx.conn_handle,
                                           s_ctx.write_handle, buf, (uint16_t)n);
}

/**
 * @brief Whether the GATT pipe is open.
 */
BOOL_T elm327_ble_is_connected(VOID_T)
{
    return (s_ctx.state == LST_READY) ? TRUE : FALSE;
}

/* ---------------------------------------------------------------------------
 * OBD_IO_T vtable export
 * --------------------------------------------------------------------------- */
/**
 * @brief Static BLE backend instance for the unified OBD_IO_T vtable.
 *
 * Each method is a thin trampoline to the matching elm327_ble_*
 * function so existing direct-call sites continue to work while new
 * callers (obd_session) speak only via the vtable.
 */
STATIC const OBD_IO_T s_io_ble = {
    .name         = "BLE",
    .init         = elm327_ble_init,
    .scan_start   = elm327_ble_scan_start,
    .scan_stop    = elm327_ble_scan_stop,
    .disconnect   = elm327_ble_disconnect,
    .send         = elm327_ble_send,
    .is_connected = elm327_ble_is_connected,
};

/**
 * @brief Get the BLE backend vtable instance.
 * @return non-NULL pointer to s_io_ble
 */
const OBD_IO_T *obd_io_ble(VOID_T)
{
    return &s_io_ble;
}
