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
#include "elm327_ble.h"
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
#define OBD_RX_QUEUE_DEPTH        16
#define OBD_RX_LINE_MAX           96
#define OBD_INIT_FAIL_LIMIT       3

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
 * @brief Send a single AT or PID command and wait for the '>' prompt.
 *
 * Captures the most recent non-prompt non-echo line into out_line (optional)
 * because that's the actual response.
 */
STATIC OPERATE_RET __send_and_wait(const char *cmd, char *out_line,
                                   size_t out_cap, uint32_t timeout_ms)
{
    OBD_RX_LINE_T rx;
    __rx_drain();
    OPERATE_RET rt = elm327_ble_send(cmd);
    if (rt != OPRT_OK) {
        return rt;
    }
    BOOL_T got_data = FALSE;
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
        if (out_line && out_cap) {
            strncpy(out_line, rx.text, out_cap - 1);
            out_line[out_cap - 1] = '\0';
        }
        got_data = TRUE;
    }
}

/* ---------------------------------------------------------------------------
 * BLE callback
 * --------------------------------------------------------------------------- */
/**
 * @brief Receive ELM BLE events (from BLE stack thread).
 *        Forwards RX lines into the session queue and posts a semaphore.
 */
STATIC VOID_T __on_ble_evt(const ELM_BLE_EVENT_T *e)
{
    if (e == NULL) {
        return;
    }
    switch (e->type) {
    case ELM_BLE_EV_RX_LINE: {
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

    case ELM_BLE_EV_CONNECTED:
        OBD_LOG("ble pipe up");
        if (s_ses.state == OBD_SES_SCAN) {
            __set_state(OBD_SES_LINKED);
        }
        if (s_ses.rx_sem) {
            tal_semaphore_post(s_ses.rx_sem);
        }
        break;

    case ELM_BLE_EV_DISCONNECTED:
        OBD_LOG("ble pipe down");
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
 * @brief Run ELM327 reset/init AT command sequence.
 *
 * Sequence:
 *   ATZ        - reset (long timeout, may answer "ELM327 v1.5")
 *   ATE0       - echo off
 *   ATL0       - linefeeds off
 *   ATS0       - spaces off (we tolerate spaces in parser anyway)
 *   ATH0       - headers off
 *   ATSP0      - automatic protocol
 *   0100       - dummy PID to lock the protocol
 */
STATIC OPERATE_RET __init_elm327(VOID_T)
{
    static const struct {
        const char *cmd;
        uint32_t    timeout;
    } seq[] = {
        {"ATZ",   2500},
        {"ATE0",   600},
        {"ATL0",   600},
        {"ATS0",   600},
        {"ATH0",   600},
        {"ATSP0",  800},
        {"0100",  4000},
    };

    char rsp[OBD_RX_LINE_MAX];
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        rsp[0] = '\0';
        OPERATE_RET rt = __send_and_wait(seq[i].cmd, rsp, sizeof(rsp),
                                         seq[i].timeout);
        OBD_LOG("init %s -> rt=%d rsp='%s'", seq[i].cmd, rt, rsp);
        if (rt != OPRT_OK) {
            return rt;
        }
        tal_system_sleep(40);
    }
    return OPRT_OK;
}

/**
 * @brief Send one PID, parse the response, push into metric bus.
 */
STATIC OPERATE_RET __poll_one(const OBD_PID_DESC_T *desc)
{
    if (desc == NULL) {
        return OPRT_INVALID_PARM;
    }
    char rsp[OBD_RX_LINE_MAX] = {0};
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

    /* Initial scan */
    elm327_ble_init(__on_ble_evt);
    obd_session_refresh_poll_list();

    const APP_PREFS_T *prefs = app_kv_prefs();
    BOOL_T have_addr = (prefs && prefs->bound_addr_valid) ? TRUE : FALSE;
    elm327_ble_scan_start(have_addr ? prefs->bound_addr : NULL);
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
                    elm327_ble_disconnect();
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
            elm327_ble_scan_start(NULL);
            __set_state(OBD_SES_SCAN);
            break;

        case OBD_SES_OFF:
        default:
            tal_system_sleep(500);
            break;
        }

        if (s_ses.rescan_req) {
            s_ses.rescan_req = FALSE;
            elm327_ble_disconnect();
            elm327_ble_scan_stop();
            elm327_ble_scan_start(NULL);
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
    elm327_ble_disconnect();
    elm327_ble_scan_stop();
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
