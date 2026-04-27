/**
 * @file tuya_main.c
 * @brief Application entry point: bring up board, peripherals, OBD, UI.
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 *
 * Boot order:
 *   1. tal_log + board_register_hardware
 *   2. board_pwr_latch        (keep power on while running)
 *   3. board_io_init          (I2C bus mutex)
 *   4. lv_vendor_init/start   (LVGL on top of CO5300 + CST92xx)
 *   5. ui_init                (creates gauges, overlay, menu, refresh timer)
 *   6. board_btn_init         (KEY/PWR -> ui_*)
 *   7. app_kv_init / app_metric_init
 *   8. sensor_imu_start       (QMI8658)
 *   9. obd_session_start      (BLE central + ELM327 + PID poller)
 *  10. (optional) app_mock_start when prefs.mock_enabled
 *
 * @note Button callbacks fire from the TDL button task; we wrap LVGL access
 *       in lv_vendor_disp_lock/unlock from those callbacks.
 */
#include "tuya_cloud_types.h"
#include "tal_api.h"
#include "tkl_output.h"
#include "lv_vendor.h"
#include "board_com_api.h"

#include "app_config.h"
#include "app_metric.h"
#include "app_kv.h"
#include "app_mock.h"
#include "board_io.h"
#include "board_pwr.h"
#include "board_btn.h"
#include "sensor_imu.h"
#include "obd_session.h"
#include "ui.h"

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define APP_LOG(fmt, ...)       PR_NOTICE("[app] " fmt, ##__VA_ARGS__)

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC THREAD_HANDLE s_app_thread = NULL;

/* ---------------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------------- */
STATIC VOID_T __on_btn_evt(BOARD_BTN_E btn, BOARD_BTN_EVT_E evt);
STATIC VOID_T __on_obd_state(OBD_SES_STATE_E st);

/* ---------------------------------------------------------------------------
 * Internal callbacks
 * --------------------------------------------------------------------------- */
/**
 * @brief Translate hardware button events to UI commands.
 *
 * KEY short      : next gauge or menu cursor down
 * KEY long 1s    : (reserved)
 * PWR short      : in MENU activate item
 * PWR long 1s    : open / close menu
 * PWR long 3s    : graceful shutdown (handled by board layer)
 */
STATIC VOID_T __on_btn_evt(BOARD_BTN_E btn, BOARD_BTN_EVT_E evt)
{
    APP_LOG("btn=%d evt=%d", btn, evt);

    lv_vendor_disp_lock();
    if (btn == BOARD_BTN_KEY) {
        if (evt == BOARD_BTN_EV_SHORT || evt == BOARD_BTN_EV_DOUBLE) {
            ui_show_next_gauge();
        }
    } else if (btn == BOARD_BTN_PWR) {
        if (evt == BOARD_BTN_EV_SHORT) {
            ui_handle_pwr_short();
        } else if (evt == BOARD_BTN_EV_LONG_1S) {
            ui_toggle_menu();
        } else if (evt == BOARD_BTN_EV_LONG_3S) {
            APP_LOG("user requested shutdown");
            board_pwr_shutdown();
        }
    }
    lv_vendor_disp_unlock();
}

/**
 * @brief Forward OBD session state into the UI under the LVGL lock.
 */
STATIC VOID_T __on_obd_state(OBD_SES_STATE_E st)
{
    APP_LOG("obd state=%d", st);
    lv_vendor_disp_lock();
    ui_on_obd_state(st);
    lv_vendor_disp_unlock();
}

/* ---------------------------------------------------------------------------
 * Boot
 * --------------------------------------------------------------------------- */
/**
 * @brief Bring up the application stack in the right order.
 */
STATIC VOID_T __user_main(VOID_T)
{
    tal_log_init(TAL_LOG_LEVEL_DEBUG, 4096, (TAL_LOG_OUTPUT_CB)tkl_log_output);

    APP_LOG("T5-Auto-meter starting");
    APP_LOG("  Project version : %s", PROJECT_VERSION);
    APP_LOG("  Compile time    : %s", __DATE__);
    APP_LOG("  TuyaOpen ver    : %s", OPEN_VERSION);
    APP_LOG("  Platform board  : %s", PLATFORM_BOARD);

    /* Board */
    OPERATE_RET rt = board_register_hardware();
    if (rt != OPRT_OK) {
        APP_LOG("board_register_hardware failed=%d", rt);
    }
    board_pwr_latch();
    board_io_init();

    /* Persistent prefs / metric bus */
    app_kv_init();
    app_metric_init();

    /* LVGL */
    lv_vendor_init(DISPLAY_NAME);
    lv_vendor_disp_lock();
    ui_init();
    lv_vendor_disp_unlock();
    lv_vendor_start(5, 1024 * 8);

    /* IMU sampler */
    sensor_imu_start();

    /* OBD session */
    obd_session_start(__on_obd_state);

    /* Optional mock task */
    const APP_PREFS_T *p = app_kv_prefs();
    if (p && p->mock_enabled) {
        app_mock_start();
    }

    /* Buttons last so their callbacks see a built UI */
    board_btn_init();
    board_btn_set_cb(__on_btn_evt);

    APP_LOG("boot complete");
}

/**
 * @brief Application thread entry; runs __user_main once.
 */
STATIC VOID_T __app_thread(VOID_T *arg)
{
    (VOID_T)arg;
    __user_main();
    tal_thread_delete(s_app_thread);
    s_app_thread = NULL;
}

/* ---------------------------------------------------------------------------
 * Entry
 * --------------------------------------------------------------------------- */
/**
 * @brief Tuya app entry – called by the SDK once OS is up.
 * @return none
 */
void tuya_app_main(void)
{
    THREAD_CFG_T cfg = {
        .thrdname  = "auto_meter",
        .stackDepth = 1024 * 4,
        .priority  = THREAD_PRIO_1,
    };
    tal_thread_create_and_start(&s_app_thread, NULL, NULL,
                                __app_thread, NULL, &cfg);
}
