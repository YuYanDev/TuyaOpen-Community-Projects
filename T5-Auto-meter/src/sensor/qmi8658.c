/**
 * @file qmi8658.c
 * @brief Minimal QMI8658 driver implementation.
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 */
#include "qmi8658.h"
#include "board_io.h"
#include "tal_api.h"
#include "tkl_i2c.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Macros — register map (subset of QMI8658 datasheet)
 * --------------------------------------------------------------------------- */
#define REG_WHO_AM_I    0x00
#define REG_REVISION_ID 0x01
#define REG_CTRL1       0x02
#define REG_CTRL2       0x03
#define REG_CTRL3       0x04
#define REG_CTRL5       0x06
#define REG_CTRL7       0x08
#define REG_CTRL8       0x09
#define REG_CTRL9       0x0A
#define REG_RESET       0x60
#define REG_TEMP_L      0x33
#define REG_AX_L        0x35
#define REG_GX_L        0x3B

/* CTRL register values */
#define CTRL2_ACC_4G_235HZ    0x25  /* aFS = ±4g, aODR = 235 Hz */
#define CTRL3_GYR_512DPS_235  0x55  /* gFS = ±512dps, gODR = 235 Hz */
#define CTRL7_EN_BOTH         0x03  /* enable both accel & gyro */
#define CTRL1_DEFAULT         0x60  /* address auto-increment, INT1 push-pull */

/* Sensitivity / scale factors  */
#define ACCEL_LSB_PER_G       8192  /* ±4g full scale → 32768/4 */
#define GYRO_LSB_PER_DPS      64    /* ±512dps full scale → 32768/512 */
#define TEMP_LSB_PER_DEG      256   /* per datasheet */

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC uint8_t s_port = 0;
STATIC uint8_t s_addr = QMI8658_I2C_ADDR_HIGH;
STATIC BOOL_T  s_inited = FALSE;

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
/**
 * @brief Write a single register.
 */
STATIC OPERATE_RET __reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    OPERATE_RET rt;
    board_io_i2c_lock();
    rt = tkl_i2c_master_send(s_port, s_addr, buf, sizeof(buf), FALSE);
    board_io_i2c_unlock();
    return rt;
}

/**
 * @brief Read a contiguous register block.
 */
STATIC OPERATE_RET __reg_read(uint8_t reg, uint8_t *buf, uint16_t len)
{
    if (buf == NULL || len == 0) {
        return OPRT_INVALID_PARM;
    }
    OPERATE_RET rt;
    board_io_i2c_lock();
    rt = tkl_i2c_master_send(s_port, s_addr, &reg, 1, TRUE);
    if (rt == OPRT_OK) {
        rt = tkl_i2c_master_receive(s_port, s_addr, buf, len, FALSE);
    }
    board_io_i2c_unlock();
    return rt;
}

/**
 * @brief Probe & configure for production use.
 */
OPERATE_RET qmi8658_init(uint8_t i2c_port, uint8_t i2c_addr)
{
    s_port = i2c_port;
    s_addr = i2c_addr;
    s_inited = FALSE;

    uint8_t whoami = 0;
    OPERATE_RET rt = __reg_read(REG_WHO_AM_I, &whoami, 1);
    if (rt != OPRT_OK || whoami != QMI8658_WHO_AM_I_VAL) {
        PR_ERR("qmi8658 whoami=0x%02x rt=%d (expected 0x%02x)",
               whoami, rt, QMI8658_WHO_AM_I_VAL);
        return OPRT_COM_ERROR;
    }

    /* Soft reset */
    __reg_write(REG_RESET, 0xB0);
    tal_system_sleep(10);

    /* Configuration sequence */
    if (__reg_write(REG_CTRL1, CTRL1_DEFAULT)        != OPRT_OK ||
        __reg_write(REG_CTRL2, CTRL2_ACC_4G_235HZ)   != OPRT_OK ||
        __reg_write(REG_CTRL3, CTRL3_GYR_512DPS_235) != OPRT_OK ||
        __reg_write(REG_CTRL5, 0x00)                 != OPRT_OK ||
        __reg_write(REG_CTRL7, CTRL7_EN_BOTH)        != OPRT_OK) {
        PR_ERR("qmi8658 ctrl write failed");
        return OPRT_COM_ERROR;
    }

    s_inited = TRUE;
    PR_NOTICE("qmi8658 ready (port=%u addr=0x%02x)", (unsigned)i2c_port, (unsigned)i2c_addr);
    return OPRT_OK;
}

/**
 * @brief Read sensor data and convert to engineering units.
 */
OPERATE_RET qmi8658_read(QMI8658_DATA_T *out)
{
    if (out == NULL) {
        return OPRT_INVALID_PARM;
    }
    if (!s_inited) {
        return OPRT_NOT_FOUND;
    }

    uint8_t buf[12];
    OPERATE_RET rt = __reg_read(REG_AX_L, buf, sizeof(buf));
    if (rt != OPRT_OK) {
        return rt;
    }

    QMI8658_RAW_T raw;
    raw.ax = (int16_t)((buf[1] << 8) | buf[0]);
    raw.ay = (int16_t)((buf[3] << 8) | buf[2]);
    raw.az = (int16_t)((buf[5] << 8) | buf[4]);
    raw.gx = (int16_t)((buf[7] << 8) | buf[6]);
    raw.gy = (int16_t)((buf[9] << 8) | buf[8]);
    raw.gz = (int16_t)((buf[11] << 8) | buf[10]);

    uint8_t temp_buf[2];
    if (__reg_read(REG_TEMP_L, temp_buf, sizeof(temp_buf)) == OPRT_OK) {
        raw.temp = (int16_t)((temp_buf[1] << 8) | temp_buf[0]);
    } else {
        raw.temp = 0;
    }

    out->ax_mg    = ((int32_t)raw.ax * 1000) / ACCEL_LSB_PER_G;
    out->ay_mg    = ((int32_t)raw.ay * 1000) / ACCEL_LSB_PER_G;
    out->az_mg    = ((int32_t)raw.az * 1000) / ACCEL_LSB_PER_G;
    out->gx_dps10 = ((int32_t)raw.gx * 10) / GYRO_LSB_PER_DPS;
    out->gy_dps10 = ((int32_t)raw.gy * 10) / GYRO_LSB_PER_DPS;
    out->gz_dps10 = ((int32_t)raw.gz * 10) / GYRO_LSB_PER_DPS;
    out->temp_c10 = ((int32_t)raw.temp * 10) / TEMP_LSB_PER_DEG;
    return OPRT_OK;
}
