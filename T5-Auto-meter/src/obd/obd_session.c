/**
 * @file obd_session.c
 * @brief OBD-II polling session built on top of elm327_ble + obd_pid.
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 *
 * State machine:
 *
 *   OFF
 *     -> SCAN  (start BLE scan)
 *   SCAN
 *     -> LINKED  (ELM_BLE_EV_CONNECTED)
 *   LINKED
 *     -> READY   (AT init sequence finishes)
 *   READY
 *     -> LINK_LOST (ELM_BLE_EV_DISCONNECTED)
 *   LINK_LOST
 *     -> SCAN    (after grace period)
 *
 * The task owns a small command queue (init-AT then a round-robin PID loop).
 * Each command waits for the line ending in '>' or APP_OBD_RX_TIMEOUT_MS.
 */
#include "obd_session.h"
#include "obd_pid.h"
#include "obd_io.h"
#include "elm327_ble.h"
#include "elm327_spp.h"
#include "app_config.h"
#include "app_kv.h"
#include "app_metric.h"
#include "tal_api.h"
#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define OBD_TASK_STACK            4096
#define OBD_TASK_PRIO             THREAD_PRIO_3
#define OBD_LOG(fmt, ...)         PR_DEBUG("[obd-ses] " fmt, ##__VA_ARGS__)
#define OBD_WARN(fmt, ...)        PR_WARN("[obd-ses] " fmt, ##__VA_ARGS__)
/* v1.8 — bumped queue depth (16→32) and per-line size (96→128) so the
 * ELM327 v1.5 25K80 can stream multi-frame ISO 15765-2 responses
 * (e.g. VIN, DTC list) without dropping intermediate "0: ..." lines.
 * 32 entries × 128 B = 4 KiB headroom — fine for the obd thread. */
#define OBD_RX_QUEUE_DEPTH        32
#define OBD_RX_LINE_MAX           128
#define OBD_INIT_FAIL_LIMIT       3
#define OBD_RX_AGG_MAX            512    /**< max bytes for multi-frame aggregate */

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef struct {
    char text[OBD_RX_LINE_MAX];
} OBD_RX_LINE_T;

typedef struct {
    THREAD_HANDLE        task;
    SEM_HANDLE           rx_sem;
    MUTEX_HANDLE         lock;
    QUEUE_HANDLE         rx_q;

    BOOL_T               run;
    BOOL_T               rescan_req;
    OBD_SES_STATE_E      state;
    OBD_SES_STATE_CB     state_cb;

    const OBD_IO_T      *io;            /**< current transport backend (BLE/SPP) */
    BOOL_T               io_unsupported; /**< latched if backend init returned NOT_SUPPORTED */

    const OBD_PID_DESC_T *poll_list[APP_METRIC_COUNT];
    int                  poll_count;
    int                  poll_idx;

    int                  init_fails;
} OBD_SES_T;

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC OBD_SES_T s_ses = {0};

/* ---------------------------------------------------------------------------
 * Backend resolution
 * --------------------------------------------------------------------------- */
/**
 * @brief Resolve a backend from the persisted bt_mode value.
 *
 * Used both at session start and on a runtime mode-switch from the
 * menu. Unknown values fall back to BLE so a stale prefs blob from
 * an older firmware never bricks the OBD path.
 *
 * @param[in] mode persisted enum
 * @return non-NULL backend vtable
 */
const OBD_IO_T *obd_io_for_mode(OBD_BT_MODE_E mode)
{
    switch (mode) {
    case OBD_BT_MODE_SPP:
        return obd_io_spp();
    case OBD_BT_MODE_BLE:
    default:
        return obd_io_ble();
    }
}

/* ---------------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------------- */
STATIC VOID_T __session_task(VOID_T *arg);

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------- */
/**
 * @brief Set state and notify upper layer.
 */
STATIC VOID_T __set_state(OBD_SES_STATE_E st)
{
    if (s_ses.state == st) {
        return;
    }
    OBD_LOG("state %d -> %d", s_ses.state, st);
    s_ses.state = st;
    if (s_ses.state_cb) {
        s_ses.state_cb(st);
    }
}

/**
 * @brief Drain any stale lines waiting in the queue.
 */
STATIC VOID_T __rx_drain(VOID_T)
{
    OBD_RX_LINE_T tmp;
    while (tal_queue_fetch(s_ses.rx_q, &tmp, 0) == OPRT_OK) {
        /* discard */
    }
}

/**
 * @brief Wait for one received line up to timeout_ms.
 * @return OPRT_OK on line, OPRT_TIMEOUT otherwise.
 */
STATIC OPERATE_RET __rx_wait(OBD_RX_LINE_T *out, uint32_t timeout_ms)
{
    return tal_queue_fetch(s_ses.rx_q, out, timeout_ms);
}

/**
 * @brief Strip a single ISO 15765-2 multi-frame line prefix in place.
 *
 * Multi-frame responses arrive as e.g. "0: 49 02 01 31 47 31",
 * "1: 4A 43 35 39 34 34 41", … The leading "n: " is a frame index,
 * not part of the actual hex payload. Our hex tokenizer in obd_pid
 * happily skips spaces, but that "N:" looks like an extra hex byte.
 * Easier to just chop the prefix here and concatenate the bare hex
 * tokens before handing the aggregate up to obd_pid_parse().
 *
 * @param[in,out] line caller-owned, zero-terminated; modified in place
 * @return none
 */
STATIC VOID_T __strip_iso15765_prefix(char *line)
{
    if (line == NULL || line[0] == '\0') {
        return;
    }
    /* Pattern: ^[0-9A-F]: ?  (one hex digit, colon, optional space) */
    char c0 = line[0];
    BOOL_T is_hex = (c0 >= '0' && c0 <= '9') ||
                    (c0 >= 'A' && c0 <= 'F') ||
                    (c0 >= 'a' && c0 <= 'f');
    if (!is_hex || line[1] != ':') {
        return;
    }
    int skip = 2;
    if (line[skip] == ' ') {
        skip++;
    }
    /* memmove handles the overlap safely. */
    size_t n = strlen(line);
    if ((size_t)skip > n) {
        line[0] = '\0';
    } else {
        memmove(line, line + skip, n - skip + 1);
    }
}

/**
 * @brief Send a single AT or PID command and wait for the '>' prompt.
 *
 * Aggregates ALL non-prompt non-echo lines into out_line, separated
 * by single spaces, with any ISO 15765-2 "n: " frame prefix stripped.
 * For typical Mode 01 single-frame responses this just copies the
 * one data line through unchanged. For long responses (VIN, DTC,
 * multi-frame mode 09) the upper-layer parser sees a single flat
 * hex stream and only the leading length byte (e.g. "014") is
 * leftover noise — obd_pid_parse already tolerates that by scanning
 * for "4X PP" before consuming bytes.
 *
 * @param[in]  cmd       null-terminated AT/PID command (no CR)
 * @param[out] out_line  optional aggregated response buffer
 * @param[in]  out_cap   sizeof(out_line)
 * @param[in]  timeout_ms total wait budget
 * @return OPRT_OK if at least one data line arrived, OPRT_TIMEOUT otherwise.
 */
STATIC OPERATE_RET __send_and_wait(const char *cmd, char *out_line,
                                   size_t out_cap, uint32_t timeout_ms)
{
    OBD_RX_LINE_T rx;
    __rx_drain();
    if (s_ses.io == NULL) {
        return OPRT_RESOURCE_NOT_READY;
    }
    OPERATE_RET rt = s_ses.io->send(cmd);
    if (rt != OPRT_OK) {
        return rt;
    }
    BOOL_T got_data = FALSE;
    if (out_line && out_cap) {
        out_line[0] = '\0';
    }
    size_t out_len = 0;
    uint64_t t_end = tal_system_get_millisecond() + timeout_ms;
    while (1) {
        uint64_t now = tal_system_get_millisecond();
        if (now >= t_end) {
            return got_data ? OPRT_OK : OPRT_TIMEOUT;
        }
        rt = __rx_wait(&rx, (uint32_t)(t_end - now));
        if (rt != OPRT_OK) {
            return got_data ? OPRT_OK : OPRT_TIMEOUT;
        }

        /* Drop echoes: ELM may echo the command (when ATE not yet applied) */
        if (strcmp(rx.text, cmd) == 0) {
            continue;
        }
        if (rx.text[0] == '>' && rx.text[1] == '\0') {
            return got_data ? OPRT_OK : OPRT_TIMEOUT;
        }

        /* Strip multi-frame "n: " prefix (no-op for single-frame). */
        __strip_iso15765_prefix(rx.text);
        if (rx.text[0] == '\0') {
            continue;
        }

        if (out_line && out_cap) {
            size_t add = strlen(rx.text);
            /* Reserve a separator byte if we already have content. */
            size_t need = (out_len > 0 ? 1 : 0) + add;
            if (out_len + need + 1 < out_cap) {
                if (out_len > 0) {
                    out_line[out_len++] = ' ';
                }
                memcpy(out_line + out_len, rx.text, add);
                out_len += add;
                out_line[out_len] = '\0';
            }
            /* If overflowing, silently truncate — better to deliver the
             * first frames than fail the whole parse. */
        }
        got_data = TRUE;
    }
}

/* ---------------------------------------------------------------------------
 * BLE callback
 * --------------------------------------------------------------------------- */
/**
 * @brief Receive transport events from any OBD_IO_T backend.
 *
 * Backend-agnostic — same handler dispatches BLE notify lines and
 * (eventually) SPP byte-stream lines once the v1.9 SPP backend lands.
 * Forwards RX lines into the session queue and updates the session
 * state machine on connect/disconnect.
 */
STATIC VOID_T __on_io_evt(const OBD_IO_EVENT_T *e)
{
    if (e == NULL) {
        return;
    }
    switch (e->type) {
    case OBD_IO_EV_RX_LINE: {
        if (e->line == NULL) {
            return;
        }
        OBD_RX_LINE_T r;
        size_t n = strlen(e->line);
        if (n >= sizeof(r.text)) {
            n = sizeof(r.text) - 1;
        }
        memcpy(r.text, e->line, n);
        r.text[n] = '\0';
        tal_queue_post(s_ses.rx_q, &r, 0);
    } break;

    case OBD_IO_EV_CONNECTED:
        OBD_LOG("io pipe up");
        if (s_ses.state == OBD_SES_SCAN) {
            __set_state(OBD_SES_LINKED);
        }
        if (s_ses.rx_sem) {
            tal_semaphore_post(s_ses.rx_sem);
        }
        break;

    case OBD_IO_EV_DISCONNECTED:
        OBD_LOG("io pipe down");
        app_metric_invalidate_obd();
        if (s_ses.state == OBD_SES_LINKED || s_ses.state == OBD_SES_READY) {
            __set_state(OBD_SES_LINK_LOST);
        }
        if (s_ses.rx_sem) {
            tal_semaphore_post(s_ses.rx_sem);
        }
        break;

    default:
        break;
    }
}

/* ---------------------------------------------------------------------------
 * State actions
 * --------------------------------------------------------------------------- */
/**
 * @brief Whether a `0100` response looks like a real Mode 01 ack.
 *
 * Used to distinguish a successful protocol probe from "SEARCHING…",
 * "NO DATA" or empty replies that an ATSP0 auto-search may produce
 * on cars where the dongle picks the wrong CAN protocol. We accept
 * any line that contains "41 00" (Mode 01 ack with PID 0x00) since
 * that's the universal "supported PIDs" reply.
 */
STATIC BOOL_T __looks_like_0100_ok(const char *rsp)
{
    if (rsp == NULL || rsp[0] == '\0') {
        return FALSE;
    }
    if (strstr(rsp, "NO DATA") || strstr(rsp, "no data") ||
        strstr(rsp, "STOPPED") || strstr(rsp, "ERROR") ||
        strstr(rsp, "SEARCHING") || strstr(rsp, "?") == rsp) {
        return FALSE;
    }
    return (strstr(rsp, "41 00") != NULL ||
            strstr(rsp, "4100") != NULL) ? TRUE : FALSE;
}

/**
 * @brief Run ELM327 reset/init AT command sequence.
 *
 * Common prefix:
 *   ATZ        - reset (long timeout, may answer "ELM327 v1.5")
 *   ATE0       - echo off
 *   ATL0       - linefeeds off
 *   ATS0       - spaces off (we tolerate spaces in parser anyway)
 *   ATH0       - headers off
 *
 * Protocol probe with fallback:
 *   1) ATSP0 + 0100  → primary, auto-detect any J1979 protocol.
 *   2) If 0100 looks empty / SEARCHING / NO_DATA, retry with ATSP6 +
 *      0100. Most modern OBD-II vehicles use ISO 15765-4 CAN 11-bit
 *      500 kbps which is protocol 6, so this is the most common
 *      workaround for ATSP0 timing out on stubborn dongles
 *      (ELM327 v1.5 25K80 clones often need the explicit hint).
 *
 * @return OPRT_OK once 0100 acks, OPRT_TIMEOUT/error otherwise.
 */
STATIC OPERATE_RET __init_elm327(VOID_T)
{
    static const struct {
        const char *cmd;
        uint32_t    timeout;
    } prefix_seq[] = {
        {"ATZ",   2500},
        {"ATE0",   600},
        {"ATL0",   600},
        {"ATS0",   600},
        {"ATH0",   600},
    };

    char rsp[OBD_RX_AGG_MAX];
    for (size_t i = 0; i < sizeof(prefix_seq) / sizeof(prefix_seq[0]); i++) {
        rsp[0] = '\0';
        OPERATE_RET rt = __send_and_wait(prefix_seq[i].cmd, rsp, sizeof(rsp),
                                         prefix_seq[i].timeout);
        OBD_LOG("init %s -> rt=%d rsp='%s'", prefix_seq[i].cmd, rt, rsp);
        if (rt != OPRT_OK) {
            return rt;
        }
        tal_system_sleep(40);
    }

    /* Attempt 1: ATSP0 (auto) + 0100 */
    rsp[0] = '\0';
    OPERATE_RET rt = __send_and_wait("ATSP0", rsp, sizeof(rsp), 800);
    OBD_LOG("init ATSP0 -> rt=%d rsp='%s'", rt, rsp);
    if (rt != OPRT_OK) {
        return rt;
    }
    tal_system_sleep(40);

    rsp[0] = '\0';
    rt = __send_and_wait("0100", rsp, sizeof(rsp), 4000);
    OBD_LOG("init 0100[ATSP0] -> rt=%d rsp='%s'", rt, rsp);
    if (rt == OPRT_OK && __looks_like_0100_ok(rsp)) {
        return OPRT_OK;
    }

    /* Attempt 2: ATSP6 (CAN 11-bit 500 kbps) + 0100 */
    OBD_WARN("ATSP0 probe failed — falling back to ATSP6");
    rsp[0] = '\0';
    rt = __send_and_wait("ATSP6", rsp, sizeof(rsp), 800);
    OBD_LOG("init ATSP6 -> rt=%d rsp='%s'", rt, rsp);
    if (rt != OPRT_OK) {
        return rt;
    }
    tal_system_sleep(40);

    rsp[0] = '\0';
    rt = __send_and_wait("0100", rsp, sizeof(rsp), 4000);
    OBD_LOG("init 0100[ATSP6] -> rt=%d rsp='%s'", rt, rsp);
    if (rt == OPRT_OK && __looks_like_0100_ok(rsp)) {
        return OPRT_OK;
    }
    return (rt == OPRT_OK) ? OPRT_NOT_FOUND : rt;
}

/**
 * @brief Send one PID, parse the response, push into metric bus.
 */
STATIC OPERATE_RET __poll_one(const OBD_PID_DESC_T *desc)
{
    if (desc == NULL) {
        return OPRT_INVALID_PARM;
    }
    /* Mode 01 PIDs are single-frame, but the aggregate buffer also
     * absorbs adapters that prepend "SEARCHING…" then a real reply. */
    char rsp[OBD_RX_AGG_MAX] = {0};
    OPERATE_RET rt = __send_and_wait(desc->cmd, rsp, sizeof(rsp),
                                     APP_OBD_RX_TIMEOUT_MS);
    if (rt != OPRT_OK || rsp[0] == '\0') {
        return rt;
    }
    int32_t val = 0;
    rt = obd_pid_parse(desc, rsp, &val);
    if (rt != OPRT_OK) {
        OBD_WARN("parse fail '%s' rt=%d", rsp, rt);
        return rt;
    }
    app_metric_set(desc->metric, val);
    return OPRT_OK;
}

/* ---------------------------------------------------------------------------
 * Task loop
 * --------------------------------------------------------------------------- */
/**
 * @brief OBD session task body.
 */
STATIC VOID_T __session_task(VOID_T *arg)
{
    (VOID_T)arg;
    OBD_LOG("task running");

    /* Pick the transport backend per the persisted bt_mode and bring
     * it up. If init returns OPRT_NOT_SUPPORTED (e.g. the SPP stub on
     * v1.8) we latch io_unsupported so the rest of the loop skips
     * scan/connect cycling — the menu can offer to switch to BLE,
     * and the WAIT_LINK overlay can show a clear "BT Classic not yet
     * supported" hint instead of looping forever. */
    const APP_PREFS_T *prefs = app_kv_prefs();
    OBD_BT_MODE_E bt_mode = prefs
        ? (OBD_BT_MODE_E)prefs->bt_mode
        : OBD_BT_MODE_BLE;
    s_ses.io = obd_io_for_mode(bt_mode);
    OPERATE_RET rt = s_ses.io->init(__on_io_evt);
    if (rt == OPRT_NOT_SUPPORTED) {
        OBD_WARN("backend '%s' not supported on this platform — idle",
                 s_ses.io->name);
        s_ses.io_unsupported = TRUE;
    } else if (rt != OPRT_OK) {
        OBD_WARN("backend '%s' init failed (rt=%d)", s_ses.io->name, rt);
    }
    obd_session_refresh_poll_list();

    BOOL_T have_addr = (prefs && prefs->bound_addr_valid) ? TRUE : FALSE;
    if (!s_ses.io_unsupported) {
        s_ses.io->scan_start(have_addr ? prefs->bound_addr : NULL);
    }
    __set_state(OBD_SES_SCAN);

    while (s_ses.run) {
        switch (s_ses.state) {
        case OBD_SES_SCAN:
            tal_system_sleep(200);
            break;

        case OBD_SES_LINKED:
            if (__init_elm327() == OPRT_OK) {
                s_ses.init_fails = 0;
                obd_session_refresh_poll_list();
                __set_state(OBD_SES_READY);
            } else {
                s_ses.init_fails++;
                OBD_WARN("init failed (%d/%d)", s_ses.init_fails, OBD_INIT_FAIL_LIMIT);
                if (s_ses.init_fails >= OBD_INIT_FAIL_LIMIT) {
                    s_ses.init_fails = 0;
                    if (s_ses.io) {
                        s_ses.io->disconnect();
                    }
                }
                tal_system_sleep(500);
            }
            break;

        case OBD_SES_READY: {
            if (s_ses.poll_count == 0) {
                tal_system_sleep(APP_OBD_POLL_PERIOD_MS);
                break;
            }
            const OBD_PID_DESC_T *desc = s_ses.poll_list[s_ses.poll_idx];
            s_ses.poll_idx = (s_ses.poll_idx + 1) % s_ses.poll_count;
            __poll_one(desc);
            tal_system_sleep(APP_OBD_POLL_PERIOD_MS);
        } break;

        case OBD_SES_LINK_LOST:
            tal_system_sleep(800);
            if (s_ses.io && !s_ses.io_unsupported) {
                s_ses.io->scan_start(NULL);
            }
            __set_state(OBD_SES_SCAN);
            break;

        case OBD_SES_OFF:
        default:
            tal_system_sleep(500);
            break;
        }

        if (s_ses.rescan_req) {
            s_ses.rescan_req = FALSE;

            /* Honour any bt_mode change made through the menu. If the
             * persisted mode no longer matches our active backend we
             * tear the old one down and bring the new one up. The
             * backend's own scan_start() pumps the cycle. */
            const APP_PREFS_T *cur_prefs = app_kv_prefs();
            OBD_BT_MODE_E want_mode = cur_prefs
                ? (OBD_BT_MODE_E)cur_prefs->bt_mode
                : OBD_BT_MODE_BLE;
            const OBD_IO_T *want_io = obd_io_for_mode(want_mode);
            if (want_io != s_ses.io) {
                if (s_ses.io && !s_ses.io_unsupported) {
                    s_ses.io->disconnect();
                    s_ses.io->scan_stop();
                }
                s_ses.io = want_io;
                s_ses.io_unsupported = FALSE;
                OPERATE_RET rti = s_ses.io->init(__on_io_evt);
                if (rti == OPRT_NOT_SUPPORTED) {
                    OBD_WARN("backend '%s' not supported", s_ses.io->name);
                    s_ses.io_unsupported = TRUE;
                }
            }

            if (s_ses.io && !s_ses.io_unsupported) {
                s_ses.io->disconnect();
                s_ses.io->scan_stop();
                s_ses.io->scan_start(NULL);
            }
            __set_state(OBD_SES_SCAN);
        }
    }

    OBD_LOG("task exit");
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */
/**
 * @brief Start the OBD session task.
 */
OPERATE_RET obd_session_start(OBD_SES_STATE_CB state_cb)
{
    if (s_ses.task != NULL) {
        s_ses.state_cb = state_cb;
        return OPRT_OK;
    }
    memset(&s_ses, 0, sizeof(s_ses));
    s_ses.state_cb = state_cb;
    s_ses.run = TRUE;

    OPERATE_RET rt = tal_mutex_create_init(&s_ses.lock);
    if (rt != OPRT_OK) {
        return rt;
    }
    rt = tal_semaphore_create_init(&s_ses.rx_sem, 0, 16);
    if (rt != OPRT_OK) {
        return rt;
    }
    rt = tal_queue_create_init(&s_ses.rx_q, sizeof(OBD_RX_LINE_T),
                               OBD_RX_QUEUE_DEPTH);
    if (rt != OPRT_OK) {
        return rt;
    }

    THREAD_CFG_T cfg = {
        .thrdname = "obd_ses",
        .stackDepth = OBD_TASK_STACK,
        .priority = OBD_TASK_PRIO,
    };
    rt = tal_thread_create_and_start(&s_ses.task, NULL, NULL,
                                     __session_task, NULL, &cfg);
    return rt;
}

/**
 * @brief Stop the session.
 */
OPERATE_RET obd_session_stop(VOID_T)
{
    s_ses.run = FALSE;
    if (s_ses.io) {
        s_ses.io->disconnect();
        s_ses.io->scan_stop();
    }
    return OPRT_OK;
}

/**
 * @brief Trigger an immediate rescan.
 */
OPERATE_RET obd_session_rescan(VOID_T)
{
    s_ses.rescan_req = TRUE;
    return OPRT_OK;
}

/**
 * @brief Re-build the poll list from current prefs.
 */
OPERATE_RET obd_session_refresh_poll_list(VOID_T)
{
    const APP_PREFS_T *p = app_kv_prefs();
    uint32_t mask = p ? p->gauge_enabled_mask : 0;
    s_ses.poll_count = obd_pid_build_poll_list(mask, s_ses.poll_list,
                                               APP_METRIC_COUNT);
    s_ses.poll_idx = 0;
    OBD_LOG("poll list refreshed: %d entries (mask=0x%08x)",
            s_ses.poll_count, mask);
    return OPRT_OK;
}

/**
 * @brief Get state.
 */
OBD_SES_STATE_E obd_session_state(VOID_T)
{
    return s_ses.state;
}

/**
 * @brief Snapshot of active backend name + supported flag.
 */
void obd_session_io_status(const char **out_name, BOOL_T *out_unsupported)
{
    if (out_name) {
        *out_name = (s_ses.io && s_ses.io->name) ? s_ses.io->name : "—";
    }
    if (out_unsupported) {
        *out_unsupported = s_ses.io_unsupported;
    }
}
