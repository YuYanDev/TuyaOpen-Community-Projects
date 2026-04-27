/**
 * @file board_btn.c
 * @brief Dual-button event broker built on TDL button manager.
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 */
#include "board_btn.h"
#include "tdl_button_manage.h"
#include "tal_api.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define BTN_LONG_START_MS    1200
#define BTN_LONG_HOLD_MS     100
#define BTN_LONG_3S_MS       3000

/* Names must match Kconfig BUTTON_NAME / BUTTON_NAME_2 */
#ifndef BUTTON_NAME
#define BUTTON_NAME "key_btn"
#endif
#ifndef BUTTON_NAME_2
#define BUTTON_NAME_2 "pwr_btn"
#endif

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef struct {
    TDL_BUTTON_HANDLE handle;
    char              name[20];
    BOARD_BTN_E       id;
    BOOL_T            long_started;
    BOOL_T            long_3s_fired;
    uint32_t          long_hold_ms;
} BTN_NODE_T;

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC BTN_NODE_T       s_btns[BOARD_BTN_COUNT];
STATIC BOARD_BTN_EVT_CB s_user_cb = NULL;
STATIC BOOL_T           s_inited = FALSE;

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
/**
 * @brief Find internal button node by string name.
 * @return pointer or NULL
 */
STATIC BTN_NODE_T *__find_by_name(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (int i = 0; i < BOARD_BTN_COUNT; i++) {
        if (strncmp(s_btns[i].name, name, sizeof(s_btns[i].name)) == 0) {
            return &s_btns[i];
        }
    }
    return NULL;
}

/**
 * @brief Dispatch a high-level event to user callback.
 */
STATIC void __emit(BOARD_BTN_E id, BOARD_BTN_EVT_E ev)
{
    BOARD_BTN_EVT_CB cb = s_user_cb;
    if (cb) {
        cb(id, ev);
    }
}

/**
 * @brief TDL event hook — translates raw events to BOARD_BTN_EVT_E.
 */
STATIC void __on_btn_event(char *name, TDL_BUTTON_TOUCH_EVENT_E event, void *argc)
{
    (void)argc;
    BTN_NODE_T *n = __find_by_name(name);
    if (n == NULL) {
        return;
    }

    switch (event) {
    case TDL_BUTTON_PRESS_DOWN:
        n->long_started = FALSE;
        n->long_3s_fired = FALSE;
        n->long_hold_ms = 0;
        break;

    case TDL_BUTTON_PRESS_SINGLE_CLICK:
        __emit(n->id, BOARD_BTN_EV_SHORT);
        break;

    case TDL_BUTTON_PRESS_DOUBLE_CLICK:
        __emit(n->id, BOARD_BTN_EV_DOUBLE);
        break;

    case TDL_BUTTON_LONG_PRESS_START:
        n->long_started = TRUE;
        n->long_hold_ms = BTN_LONG_START_MS;
        break;

    case TDL_BUTTON_LONG_PRESS_HOLD:
        if (n->long_started) {
            n->long_hold_ms += BTN_LONG_HOLD_MS;
            if (!n->long_3s_fired && n->long_hold_ms >= BTN_LONG_3S_MS) {
                n->long_3s_fired = TRUE;
                __emit(n->id, BOARD_BTN_EV_LONG_3S);
            }
        }
        break;

    case TDL_BUTTON_PRESS_UP:
        if (n->long_started && !n->long_3s_fired) {
            __emit(n->id, BOARD_BTN_EV_LONG_1S);
        }
        n->long_started = FALSE;
        n->long_3s_fired = FALSE;
        n->long_hold_ms = 0;
        break;

    default:
        break;
    }
}

/**
 * @brief Bind a single button to TDL with our timing & cb.
 */
STATIC OPERATE_RET __bind(BOARD_BTN_E id, const char *name)
{
    BTN_NODE_T *n = &s_btns[id];
    memset(n, 0, sizeof(*n));
    n->id = id;
    snprintf(n->name, sizeof(n->name), "%s", name);

    TDL_BUTTON_CFG_T cfg = {
        .long_start_valid_time   = BTN_LONG_START_MS,
        .long_keep_timer         = BTN_LONG_HOLD_MS,
        .button_debounce_time    = 50,
        .button_repeat_valid_count = 2,
        .button_repeat_valid_time  = 350,
    };

    OPERATE_RET rt = tdl_button_create((char *)name, &cfg, &n->handle);
    if (rt != OPRT_OK || n->handle == NULL) {
        PR_ERR("tdl_button_create(%s) failed: %d", name, rt);
        return rt;
    }

    tdl_button_event_register(n->handle, TDL_BUTTON_PRESS_DOWN,        __on_btn_event);
    tdl_button_event_register(n->handle, TDL_BUTTON_PRESS_UP,          __on_btn_event);
    tdl_button_event_register(n->handle, TDL_BUTTON_PRESS_SINGLE_CLICK,__on_btn_event);
    tdl_button_event_register(n->handle, TDL_BUTTON_PRESS_DOUBLE_CLICK,__on_btn_event);
    tdl_button_event_register(n->handle, TDL_BUTTON_LONG_PRESS_START,  __on_btn_event);
    tdl_button_event_register(n->handle, TDL_BUTTON_LONG_PRESS_HOLD,   __on_btn_event);
    return OPRT_OK;
}

/**
 * @brief Initialize the dual-button system.
 */
OPERATE_RET board_btn_init(VOID_T)
{
    if (s_inited) {
        return OPRT_OK;
    }

    OPERATE_RET rt = __bind(BOARD_BTN_KEY, BUTTON_NAME);
    if (rt != OPRT_OK) {
        PR_WARN("KEY button bind skipped (rt=%d)", rt);
    }

    rt = __bind(BOARD_BTN_PWR, BUTTON_NAME_2);
    if (rt != OPRT_OK) {
        PR_WARN("PWR button bind skipped (rt=%d)", rt);
    }

    s_inited = TRUE;
    return OPRT_OK;
}

/**
 * @brief Register user-level event callback.
 */
OPERATE_RET board_btn_set_cb(BOARD_BTN_EVT_CB cb)
{
    s_user_cb = cb;
    return OPRT_OK;
}
