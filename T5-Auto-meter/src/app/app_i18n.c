/**
 * @file app_i18n.c
 * @brief Translator implementation — flat [lang][id] lookup table.
 * @version 1.0
 * @date 2026-04-28
 * @copyright Copyright (c) Tuya Inc.
 */
#include "app_i18n.h"
#include "app_kv.h"
#include "tal_api.h"

/* app_kv.c hard-codes the literal `2` as the APP_LANG_COUNT clamp boundary
 * (it can't include this header without creating a circular include —
 * see app_kv.h::app_kv_set_lang for the full rationale). If a third
 * language is ever added, the clamp in app_kv.c::app_kv_set_lang and
 * the sanitize step in app_kv.c::app_kv_init MUST be bumped in lock
 * step. This static_assert turns the silent breakage into a compile
 * error so future contributors get a loud nudge. */
_Static_assert(APP_LANG_COUNT == 2,
               "Update the hard-coded '2' clamp in app_kv.c app_kv_set_lang "
               "and the sanitize check in app_kv.c app_kv_init when adding "
               "a new language.");

/* ---------------------------------------------------------------------------
 * String tables
 * --------------------------------------------------------------------------- */
/* English (US) — index = APP_STR_E. */
STATIC const char *const k_strings_en[APP_STR_COUNT] = {
    [STR_MENU_TITLE]          = "MENU",
    [STR_MENU_MOCK]           = "Mock Mode  :  ",
    [STR_MENU_BRIGHT]         = "Brightness  :  ",
    [STR_MENU_GCAL]           = "Calibrate G  ",
    [STR_MENU_GCAL_HINT_SAVED]= "(saved)",
    [STR_MENU_GCAL_HINT_TAP]  = "(tap to zero)",
    [STR_MENU_ORIENT]         = "Orient: ",
    [STR_ORIENT_FACE_UP]      = "Face up",
    [STR_ORIENT_USER_0]       = "User 0\xC2\xB0",
    [STR_ORIENT_USER_90]      = "User 90\xC2\xB0",
    [STR_ORIENT_USER_180]     = "User 180\xC2\xB0",
    [STR_ORIENT_USER_270]     = "User 270\xC2\xB0",
    [STR_MENU_BT_MODE]        = "BT: ",
    [STR_BT_STUB_SUFFIX]      = " (stub)",
    [STR_MENU_BT_PAIR_SPP]    = "Pair OBD  (1234/0000)",
    [STR_MENU_BT_PAIR_BLE]    = "Pair OBD  (BLE: n/a)",
    [STR_MENU_FORGET]         = "Forget Adapter  ",
    [STR_MENU_FORGET_SAVED]   = "(saved)",
    [STR_MENU_FORGET_NONE]    = "(none)",
    [STR_MENU_LANG]           = "Language: EN",
    [STR_MENU_CLOSE]          = "Close",

    [STR_OVL_OBD_OFF]         = "OBD off",
    [STR_OVL_CONNECTING]      = "Connecting OBD",
    [STR_OVL_CONFIGURING]     = "Configuring",
    [STR_OVL_LINK_LOST]       = "Link lost",
    [STR_OVL_HINT]            = "Press PWR for menu  \xC2\xB7  KEY: switch gauge",
    [STR_OVL_SEARCHING]       = "Searching for ELM327 BLE\xE2\x80\xA6",
};

/* Simplified Chinese — UTF-8.  Note: every CJK glyph used here MUST
 * exist in lv_font_simsun_16_cjk's range (BMP basic CJK 0x4E00-0x9FFF
 * plus common punctuation). All entries below stick to that range. */
STATIC const char *const k_strings_zh[APP_STR_COUNT] = {
    [STR_MENU_TITLE]          = "\xE8\x8F\x9C\xE5\x8D\x95",                                   /* 菜单 */
    [STR_MENU_MOCK]           = "\xE6\xA8\xA1\xE6\x8B\x9F\xE6\xA8\xA1\xE5\xBC\x8F\xEF\xBC\x9A", /* 模拟模式： */
    [STR_MENU_BRIGHT]         = "\xE4\xBA\xAE\xE5\xBA\xA6\xEF\xBC\x9A",                         /* 亮度： */
    [STR_MENU_GCAL]           = "G \xE5\x80\xBC\xE6\xA0\xA1\xE5\x87\x86 ",                       /* G 值校准 */
    [STR_MENU_GCAL_HINT_SAVED]= "(\xE5\xB7\xB2\xE4\xBF\x9D\xE5\xAD\x98)",                       /* (已保存) */
    [STR_MENU_GCAL_HINT_TAP]  = "(\xE7\x82\xB9\xE5\x87\xBB\xE5\xBD\x92\xE9\x9B\xB6)",           /* (点击归零) */
    [STR_MENU_ORIENT]         = "\xE6\x9C\x9D\xE5\x90\x91\xEF\xBC\x9A",                          /* 朝向： */
    [STR_ORIENT_FACE_UP]      = "\xE5\xB1\x8F\xE5\xB9\x95\xE6\x9C\x9D\xE4\xB8\x8A",              /* 屏幕朝上 */
    [STR_ORIENT_USER_0]       = "\xE6\x9C\x9D\xE7\x94\xA8\xE6\x88\xB7 0\xC2\xB0",                /* 朝用户 0° */
    [STR_ORIENT_USER_90]      = "\xE6\x9C\x9D\xE7\x94\xA8\xE6\x88\xB7 90\xC2\xB0",               /* 朝用户 90° */
    [STR_ORIENT_USER_180]     = "\xE6\x9C\x9D\xE7\x94\xA8\xE6\x88\xB7 180\xC2\xB0",              /* 朝用户 180° */
    [STR_ORIENT_USER_270]     = "\xE6\x9C\x9D\xE7\x94\xA8\xE6\x88\xB7 270\xC2\xB0",              /* 朝用户 270° */
    [STR_MENU_BT_MODE]        = "\xE8\x93\x9D\xE7\x89\x99\xEF\xBC\x9A",                          /* 蓝牙： */
    [STR_BT_STUB_SUFFIX]      = "\xEF\xBC\x88\xE5\x8D\xA0\xE4\xBD\x8D\xEF\xBC\x89",              /* （占位） */
    /* GCC's \x escape is greedy — it eats every following hex digit it
     * can find, so "\xEF\xBC\x881234" becomes one giant out-of-range
     * escape. Splitting the string into adjacent literals stops the
     * escape at the byte we actually meant (0x88) and lets the ASCII
     * digits stand on their own. */
    [STR_MENU_BT_PAIR_SPP]    = "\xE9\x85\x8D\xE5\xAF\xB9 \xEF\xBC\x88" "1234/0000" "\xEF\xBC\x89",   /* 配对（1234/0000） */
    [STR_MENU_BT_PAIR_BLE]    = "\xE9\x85\x8D\xE5\xAF\xB9 \xEF\xBC\x88" "BLE " "\xE6\x97\xA0\xE9\x9C\x80\xEF\xBC\x89", /* 配对（BLE 无需） */
    [STR_MENU_FORGET]         = "\xE5\xBF\x98\xE8\xAE\xB0\xE9\x80\x82\xE9\x85\x8D\xE5\x99\xA8 ", /* 忘记适配器 */
    [STR_MENU_FORGET_SAVED]   = "(\xE5\xB7\xB2\xE4\xBF\x9D\xE5\xAD\x98)",                       /* (已保存) */
    [STR_MENU_FORGET_NONE]    = "(\xE6\x97\xA0)",                                               /* (无) */
    [STR_MENU_LANG]           = "\xE8\xAF\xAD\xE8\xA8\x80\xEF\xBC\x9A\xE4\xB8\xAD\xE6\x96\x87", /* 语言：中文 */
    [STR_MENU_CLOSE]          = "\xE5\x85\xB3\xE9\x97\xAD",                                     /* 关闭 */

    [STR_OVL_OBD_OFF]         = "OBD \xE5\x85\xB3\xE9\x97\xAD",                                 /* OBD 关闭 */
    [STR_OVL_CONNECTING]      = "\xE6\xAD\xA3\xE5\x9C\xA8\xE8\xBF\x9E\xE6\x8E\xA5 OBD",         /* 正在连接 OBD */
    [STR_OVL_CONFIGURING]     = "\xE5\x88\x9D\xE5\xA7\x8B\xE5\x8C\x96\xE4\xB8\xAD",             /* 初始化中 */
    [STR_OVL_LINK_LOST]       = "\xE8\xBF\x9E\xE6\x8E\xA5\xE6\x96\xAD\xE5\xBC\x80",             /* 连接断开 */
    [STR_OVL_HINT]            = "PWR \xE9\x94\xAE\xEF\xBC\x9A\xE8\x8F\x9C\xE5\x8D\x95  \xC2\xB7  KEY \xE9\x94\xAE\xEF\xBC\x9A\xE5\x88\x87\xE6\x8D\xA2\xE4\xBB\xAA\xE8\xA1\xA8", /* PWR 键：菜单 · KEY 键：切换仪表 */
    [STR_OVL_SEARCHING]       = "\xE6\xAD\xA3\xE5\x9C\xA8\xE6\x90\x9C\xE7\xB4\xA2 ELM327\xE2\x80\xA6", /* 正在搜索 ELM327… */
};

STATIC const char *const *const k_tables[APP_LANG_COUNT] = {
    [APP_LANG_EN] = k_strings_en,
    [APP_LANG_ZH] = k_strings_zh,
};

/* Compile-time guard the header advertises: every language table must
 * have AT LEAST APP_STR_COUNT slots. This catches a half-finished
 * translation that drops the table size — the runtime EN fallback in
 * app_i18n_get() still handles individual NULL entries inside the
 * table for incremental wiring. */
_Static_assert(sizeof(k_strings_en) / sizeof(k_strings_en[0]) == APP_STR_COUNT,
               "k_strings_en must define every APP_STR_E slot.");
_Static_assert(sizeof(k_strings_zh) / sizeof(k_strings_zh[0]) == APP_STR_COUNT,
               "k_strings_zh must define every APP_STR_E slot.");

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC APP_LANG_E s_lang = APP_LANG_EN;
STATIC BOOL_T     s_inited = FALSE;

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
/**
 * @brief Pull the persisted language out of KV (idempotent).
 */
OPERATE_RET app_i18n_init(VOID_T)
{
    if (s_inited) {
        return OPRT_OK;
    }
    const APP_PREFS_T *p = app_kv_prefs();
    if (p) {
        uint8_t l = p->lang;
        if (l < (uint8_t)APP_LANG_COUNT) {
            s_lang = (APP_LANG_E)l;
        }
    }
    s_inited = TRUE;
    return OPRT_OK;
}

/**
 * @brief Get the active language.
 */
APP_LANG_E app_i18n_lang(VOID_T)
{
    if (!s_inited) {
        app_i18n_init();
    }
    return s_lang;
}

/**
 * @brief Set & persist the language.
 */
OPERATE_RET app_i18n_set_lang(APP_LANG_E lang)
{
    if ((unsigned)lang >= (unsigned)APP_LANG_COUNT) {
        return OPRT_INVALID_PARM;
    }
    if (!s_inited) {
        app_i18n_init();
    }
    s_lang = lang;
    return app_kv_set_lang(lang);
}

/**
 * @brief Translate a string id with EN fallback.
 */
const char *app_i18n_get(APP_STR_E id)
{
    if ((unsigned)id >= (unsigned)APP_STR_COUNT) {
        return "?";
    }
    if (!s_inited) {
        app_i18n_init();
    }
    const char *s = k_tables[s_lang][id];
    if (s == NULL) {
        s = k_tables[APP_LANG_EN][id];
    }
    return s ? s : "?";
}

/**
 * @brief Pick a default font matching the active language's script.
 *
 * For EN we keep Montserrat 16 — same metrics as the menu used
 * before v1.8. For ZH we drop to LVGL's bundled SimSun 16 px CJK so
 * Chinese glyphs render. (LVGL ships exactly one CJK glyph size by
 * default; if higher resolutions are needed they have to be
 * generated with lv_font_conv and added to the build.)
 */
const lv_font_t *app_i18n_font_default(VOID_T)
{
    if (!s_inited) {
        app_i18n_init();
    }
#if LV_FONT_SIMSUN_16_CJK
    if (s_lang == APP_LANG_ZH) {
        return &lv_font_simsun_16_cjk;
    }
#endif
    return &lv_font_montserrat_16;
}
