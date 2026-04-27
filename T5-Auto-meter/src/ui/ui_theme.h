/**
 * @file ui_theme.h
 * @brief Shared colors, fonts and gauge defaults.
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __UI_THEME_H__
#define __UI_THEME_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define UI_COLOR_BG          lv_color_hex(0x000000)
#define UI_COLOR_PRIMARY     lv_color_hex(0xE53935) /* needle red */
#define UI_COLOR_ACCENT      lv_color_hex(0xFFB300) /* warning amber */
#define UI_COLOR_TICK        lv_color_hex(0xC8C8C8)
#define UI_COLOR_TICK_DIM    lv_color_hex(0x606060)
#define UI_COLOR_TEXT        lv_color_hex(0xF5F5F5)
#define UI_COLOR_TEXT_DIM    lv_color_hex(0x888888)
#define UI_COLOR_ARC         lv_color_hex(0x303030)
#define UI_COLOR_HUB         lv_color_hex(0x202020)
#define UI_COLOR_OK          lv_color_hex(0x43A047)
#define UI_COLOR_BAD         lv_color_hex(0xE53935)

/* Sweep range (LVGL angle in 0.1°, 0=north). 270° span centred on bottom. */
#define UI_GAUGE_ANGLE_MIN   (-1350)  /* -135° */
#define UI_GAUGE_ANGLE_MAX   ( 1350)  /*  135° */

#ifdef __cplusplus
}
#endif

#endif /* __UI_THEME_H__ */
