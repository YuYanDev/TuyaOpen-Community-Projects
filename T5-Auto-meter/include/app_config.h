/**
 * @file app_config.h
 * @brief Single source of truth for board pinout & global configuration.
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __APP_CONFIG_H__
#define __APP_CONFIG_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "tuya_cloud_types.h"
#include "tkl_gpio.h"

/* ---------------------------------------------------------------------------
 * Display geometry
 * --------------------------------------------------------------------------- */
#define APP_LCD_WIDTH               466
#define APP_LCD_HEIGHT              466
#define APP_LCD_CX                  (APP_LCD_WIDTH / 2)
#define APP_LCD_CY                  (APP_LCD_HEIGHT / 2)

/* ---------------------------------------------------------------------------
 * Pinout (mirrors WAVESHARE_T5AI_TOUCH_AMOLED_1_75/board_com_api.c)
 * --------------------------------------------------------------------------- */
#define APP_PIN_BTN_PWR             TUYA_GPIO_NUM_18
#define APP_PIN_BTN_KEY             TUYA_GPIO_NUM_12
#define APP_PIN_PWR_EN              TUYA_GPIO_NUM_19

#define APP_I2C_PORT                TUYA_I2C_NUM_0
#define APP_PIN_I2C_SCL             TUYA_GPIO_NUM_20
#define APP_PIN_I2C_SDA             TUYA_GPIO_NUM_21

#define APP_QMI8658_I2C_ADDR        0x6B

#define APP_PIN_BAT_CHRG            TUYA_GPIO_NUM_30
#define APP_PIN_BAT_ADC             TUYA_GPIO_NUM_13
#define APP_BAT_ADC_CHANNEL         15
#define APP_BAT_ADC_RATIO_NUM       251
#define APP_BAT_ADC_RATIO_DEN       51

/* ---------------------------------------------------------------------------
 * Application-level constants
 * --------------------------------------------------------------------------- */
#define APP_KV_NS                   "auto_meter"
#define APP_DEFAULT_BRIGHTNESS_PCT  60

/* Boot animation timing (ms) */
#define APP_BOOT_LOGO_FADE_MS       400
#define APP_BOOT_SWEEP_MS           1320
#define APP_BOOT_HOLD_MS            300
#define APP_BOOT_BLE_HINT_MS        500

/* OBD polling */
#define APP_OBD_POLL_PERIOD_MS      250
#define APP_OBD_RX_TIMEOUT_MS       1500
#define APP_BLE_SCAN_TIMEOUT_MS     30000

#ifdef __cplusplus
}
#endif

#endif /* __APP_CONFIG_H__ */
