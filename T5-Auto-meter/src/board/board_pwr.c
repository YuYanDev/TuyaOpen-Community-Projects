/**
 * @file board_pwr.c
 * @brief Power latch and shutdown helpers.
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 */
#include "board_pwr.h"
#include "app_config.h"
#include "tal_api.h"
#include "tkl_gpio.h"

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
/**
 * @brief Latch the power-enable line HIGH.
 * @return OPRT_OK on success
 * @note The board's __board_register_button() also drives this pin HIGH when
 *       BUTTON_NAME_2 is enabled; this implementation is idempotent.
 */
OPERATE_RET board_pwr_latch(VOID_T)
{
    TUYA_GPIO_BASE_CFG_T cfg = {
        .mode = TUYA_GPIO_PUSH_PULL,
        .direct = TUYA_GPIO_OUTPUT,
        .level = TUYA_GPIO_LEVEL_HIGH,
    };
    OPERATE_RET rt = tkl_gpio_init(APP_PIN_PWR_EN, &cfg);
    if (rt != OPRT_OK) {
        PR_ERR("pwr_en init failed: %d", rt);
        return rt;
    }
    tkl_gpio_write(APP_PIN_PWR_EN, TUYA_GPIO_LEVEL_HIGH);
    return OPRT_OK;
}

/**
 * @brief Drive the power-enable line LOW.
 * @return none — control never returns once power is cut.
 */
void board_pwr_shutdown(VOID_T)
{
    PR_NOTICE("board_pwr_shutdown: cutting PWR_EN");
    tkl_gpio_write(APP_PIN_PWR_EN, TUYA_GPIO_LEVEL_LOW);
    /* Wait for regulator collapse — if user is on USB the device may still
     * stay powered, so keep an idle loop to prevent runaway. */
    while (1) {
        tal_system_sleep(1000);
    }
}
