/**
 * @file board_io.c
 * @brief Board I/O helpers implementation.
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 */
#include "board_io.h"
#include "app_config.h"
#include "tal_api.h"
#include "tkl_gpio.h"
#include "tkl_adc.h"

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC MUTEX_HANDLE s_i2c_mutex = NULL;
STATIC BOOL_T       s_inited = FALSE;
STATIC BOOL_T       s_adc_inited = FALSE;
STATIC uint32_t     s_last_bat_mv = 0;

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
/**
 * @brief Initialize board-level I/O mutex (I2C only).
 * @return OPRT_OK on success
 */
OPERATE_RET board_io_init(VOID_T)
{
    if (s_inited) {
        return OPRT_OK;
    }

    OPERATE_RET rt = tal_mutex_create_init(&s_i2c_mutex);
    if (rt != OPRT_OK) {
        return rt;
    }

    /* Charging detect input */
    TUYA_GPIO_BASE_CFG_T chrg_cfg = {
        .mode = TUYA_GPIO_PULLUP,
        .direct = TUYA_GPIO_INPUT,
        .level = TUYA_GPIO_LEVEL_HIGH,
    };
    tkl_gpio_init(APP_PIN_BAT_CHRG, &chrg_cfg);

    s_inited = TRUE;
    return OPRT_OK;
}

/**
 * @brief Acquire I2C bus mutex.
 */
OPERATE_RET board_io_i2c_lock(VOID_T)
{
    if (s_i2c_mutex == NULL) {
        return OPRT_OK;
    }
    return tal_mutex_lock(s_i2c_mutex);
}

/**
 * @brief Release I2C bus mutex.
 */
OPERATE_RET board_io_i2c_unlock(VOID_T)
{
    if (s_i2c_mutex == NULL) {
        return OPRT_OK;
    }
    return tal_mutex_unlock(s_i2c_mutex);
}

/**
 * @brief Read battery voltage in millivolts.
 * @note ADC reading scaled by APP_BAT_ADC_RATIO_NUM/APP_BAT_ADC_RATIO_DEN to compensate divider.
 */
OPERATE_RET board_io_read_battery_mv(uint32_t *mv)
{
    if (mv == NULL) {
        return OPRT_INVALID_PARM;
    }

    if (!s_adc_inited) {
        TUYA_ADC_BASE_CFG_T adc_cfg = {
            .ch_list = { .data = (1 << APP_BAT_ADC_CHANNEL) },
            .ch_nums = 1,
            .freq = 25000,
            .type = TUYA_ADC_INNER_SAMPLE_VOL,
            .mode = TUYA_ADC_CONTINUOUS,
            .conv_cnt = 1,
        };
        if (tkl_adc_init(0, &adc_cfg) == OPRT_OK) {
            s_adc_inited = TRUE;
        } else {
            *mv = s_last_bat_mv;
            return OPRT_COM_ERROR;
        }
    }

    int32_t raw_mv = 0;
    if (tkl_adc_read_voltage(0, &raw_mv, 1) != OPRT_OK || raw_mv < 0) {
        *mv = s_last_bat_mv;
        return OPRT_COM_ERROR;
    }

    /* Scale according to resistor divider */
    uint32_t scaled = ((uint32_t)raw_mv * APP_BAT_ADC_RATIO_NUM) / APP_BAT_ADC_RATIO_DEN;
    s_last_bat_mv = scaled;
    *mv = scaled;
    return OPRT_OK;
}

/**
 * @brief Read whether battery is charging.
 */
BOOL_T board_io_is_charging(VOID_T)
{
    TUYA_GPIO_LEVEL_E lv = TUYA_GPIO_LEVEL_HIGH;
    if (tkl_gpio_read(APP_PIN_BAT_CHRG, &lv) != OPRT_OK) {
        return FALSE;
    }
    return (lv == TUYA_GPIO_LEVEL_LOW) ? TRUE : FALSE;
}
