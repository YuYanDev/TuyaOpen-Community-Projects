/**
 * @file app_i18n.h
 * @brief Lightweight string-table translator for the menu / overlay (EN / 中文).
 * @version 1.0
 * @date 2026-04-28
 * @copyright Copyright (c) Tuya Inc.
 *
 * Why a hand-rolled table?
 * ------------------------
 * The visible UI string surface is tiny (the boot/wait overlay copy
 * plus 8 menu rows), so the cost-benefit of a gettext-class
 * implementation is poor. A flat APP_STR_E enum + a `[lang][id]`
 * pointer table gives:
 *
 *   - O(1) lookups at the call site (`app_i18n_get(STR_BT_MODE_LBL)`).
 *   - Zero RAM footprint — both languages live in flash.
 *   - Compile-time enforcement that every new ID has both
 *     translations, by writing the ZH and EN tables side-by-side and
 *     letting `_Static_assert(ARRAY_SIZE == APP_STR_COUNT)` catch
 *     missing entries.
 *
 * Font handling
 * -------------
 *  - English uses LVGL's Montserrat family (we already have 16/22/28
 *    enabled in app_default.config).
 *  - Simplified Chinese uses LVGL's bundled SimSun 16-px CJK font
 *    (CONFIG_LV_FONT_SIMSUN_16_CJK=y, enabled in v1.8).
 *
 * Only one CJK glyph size ships with LVGL by default, so when the
 * language is set to Chinese the menu/overlay drop down to that
 * single 16-px size for the duration; the gauge dial labels stay
 * Latin (numbers + units like °C/g/V) and use Montserrat regardless.
 *
 * Persistence
 * -----------
 *  The active language is stored in APP_PREFS_T::lang and survives
 *  reboot. ui.c calls app_i18n_set_lang() from the menu click and
 *  re-renders the visible label set so the change is immediate.
 */
#ifndef __APP_I18N_H__
#define __APP_I18N_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "tuya_cloud_types.h"
#include "lvgl.h"

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
/**
 * @brief Supported display languages.
 *
 * Persisted as APP_PREFS_T::lang. Order MUST match the language
 * dimension of the s_strings table in app_i18n.c.
 */
typedef enum {
    APP_LANG_EN = 0,
    APP_LANG_ZH = 1,
    APP_LANG_COUNT
} APP_LANG_E;

/**
 * @brief All translatable string IDs.
 *
 * Add new IDs at the end of the enum so existing prefs blobs that
 * persist a string-id index (currently we don't, but we may in the
 * future for a "last-shown overlay tip" feature) stay valid. Every
 * new ID must be paired with a row in BOTH language tables in
 * app_i18n.c — the file's _Static_assert catches a missing one at
 * compile time.
 */
typedef enum {
    /* Menu rows (label only — value is composed at runtime) */
    STR_MENU_TITLE = 0,
    STR_MENU_MOCK,             /**< "Mock Mode" / "模拟模式" */
    STR_MENU_BRIGHT,           /**< "Brightness" / "亮度" */
    STR_MENU_GCAL,             /**< "Calibrate G" / "G 值校准" */
    STR_MENU_GCAL_HINT_SAVED,  /**< "(saved)" / "(已保存)" */
    STR_MENU_GCAL_HINT_TAP,    /**< "(tap to zero)" / "(点击归零)" */
    STR_MENU_ORIENT,           /**< "Orient:" / "朝向：" */
    STR_ORIENT_FACE_UP,        /**< "Face up" / "屏幕朝上" */
    STR_ORIENT_USER_0,         /**< "User 0°" / "朝用户 0°" */
    STR_ORIENT_USER_90,        /**< "User 90°" / "朝用户 90°" */
    STR_ORIENT_USER_180,       /**< "User 180°" / "朝用户 180°" */
    STR_ORIENT_USER_270,       /**< "User 270°" / "朝用户 270°" */
    STR_MENU_BT_MODE,          /**< "BT:" / "蓝牙：" */
    STR_BT_STUB_SUFFIX,        /**< " (stub)" / "（占位）" */
    STR_MENU_BT_PAIR_SPP,      /**< "Pair OBD (1234/0000)" / "配对（1234/0000）" */
    STR_MENU_BT_PAIR_BLE,      /**< "Pair OBD (BLE: n/a)" / "配对（BLE 无需）" */
    STR_MENU_FORGET,           /**< "Forget Adapter" / "忘记适配器" */
    STR_MENU_FORGET_SAVED,     /**< "(saved)" / "(已保存)" */
    STR_MENU_FORGET_NONE,      /**< "(none)" / "(无)" */
    STR_MENU_LANG,             /**< "Language: EN" / "语言：中文".
                                    NOTE: ui.c renders this row by hand (it
                                    shows the OTHER language's label so the
                                    user can see what they'll switch TO,
                                    e.g. "语言：中文" while running in EN).
                                    The strings here are kept in sync as a
                                    fallback in case a future caller wants
                                    to display "current language" instead. */
    STR_MENU_CLOSE,            /**< "Close" / "关闭" */

    /* Boot / wait-link overlay copy */
    STR_OVL_OBD_OFF,           /**< "OBD off" / "OBD 关闭" */
    STR_OVL_CONNECTING,        /**< "Connecting OBD" / "正在连接 OBD" */
    STR_OVL_CONFIGURING,       /**< "Configuring" / "初始化中" */
    STR_OVL_LINK_LOST,         /**< "Link lost" / "连接断开" */
    STR_OVL_HINT,              /**< "Press PWR for menu  ·  KEY: switch gauge" / 中文同义 */
    STR_OVL_SEARCHING,         /**< "Searching for ELM327 BLE…" / "正在搜索 ELM327 …" */

    APP_STR_COUNT
} APP_STR_E;

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */
/**
 * @brief Initialise the i18n module — pulls the active language from KV.
 *
 * Idempotent. Safe to call before / after lv_init() — only state we
 * touch is a static enum.
 *
 * @return OPRT_OK on success
 */
OPERATE_RET app_i18n_init(VOID_T);

/**
 * @brief Get the currently active language.
 * @return one of APP_LANG_E
 */
APP_LANG_E app_i18n_lang(VOID_T);

/**
 * @brief Switch language and persist the choice.
 *
 * Caller is responsible for re-rendering any visible labels — this
 * function does not call into LVGL. ui.c hooks the menu click to
 * call us followed by a menu redraw + a font reflow on the overlay.
 *
 * @param[in] lang one of APP_LANG_E
 * @return OPRT_OK on success
 */
OPERATE_RET app_i18n_set_lang(APP_LANG_E lang);

/**
 * @brief Translate a string ID to the active language.
 *
 * Returns a pointer to a constant flash string — never NULL. Unknown
 * IDs (defensive against forward-compat prefs) fall back to the EN
 * table; if even EN is missing (shouldn't happen due to compile-time
 * checks) returns "?".
 *
 * @param[in] id one of APP_STR_E
 * @return non-NULL pointer to a UTF-8 string
 */
const char *app_i18n_get(APP_STR_E id);

/**
 * @brief Get the recommended default LVGL font for the active language.
 *
 * EN  → &lv_font_montserrat_16
 * ZH  → &lv_font_simsun_16_cjk
 *
 * The menu rows and overlay use this so a language flip immediately
 * picks up CJK glyph coverage. The gauge dial labels are pure Latin
 * (numbers, °C, g, kPa, V) so they stay on Montserrat regardless.
 *
 * @return non-NULL font pointer
 */
const lv_font_t *app_i18n_font_default(VOID_T);

#ifdef __cplusplus
}
#endif

#endif /* __APP_I18N_H__ */
