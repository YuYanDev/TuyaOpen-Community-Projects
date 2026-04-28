/**
 * @file lv_font_zh_16.h
 * @brief Project-bundled 16-px Simplified Chinese font (NotoSansSC subset).
 * @version 1.0
 * @date 2026-04-28
 * @copyright NotoSansSC is © Adobe / Google, OFL 1.1 — see fonts/OFL.txt.
 *
 * Why we ship our own glyph table
 * -------------------------------
 * LVGL ships exactly one Simplified-Chinese-looking font:
 *   `lv_font_simsun_16_cjk` (CONFIG_LV_FONT_SIMSUN_16_CJK=y).
 * Its `--symbols` list however is a chat-bot vocabulary that mixes
 * Japanese / Traditional Chinese / a few Simplified glyphs. Crucially
 * the **menu vocabulary we use** ("菜单 / 蓝牙 / 配对 / 切换仪表 / 校准 /
 * 屏幕 / 朝向 / 关闭 / 语言 / 连接 / 搜索 / 适配器 / 已保存 / 无 / 归零 …")
 * is mostly NOT covered — the missing glyphs come out as the empty
 * "tofu" rectangle the user reported.
 *
 * Generation
 * ----------
 *  Generated once by `lv_font_conv` (npx lv_font_conv@1.5.3) from
 *  Google's NotoSansSC-Regular.ttf (OFL 1.1) and committed as a static
 *  C source under `src/ui/fonts/lv_font_zh_16.c`. The exact glyph set
 *  (62 CJK ideograms + ASCII 0x20-0x7F + ° + 【】（）：) is a strict
 *  superset of every translated string in `app_i18n.c`. If a future
 *  string adds a new ideogram, the build will SILENTLY fall through to
 *  a missing-glyph "?" — re-run the generator command listed at the top
 *  of the .c file with the expanded char list.
 *
 *  The font is BPP=4 (16 grey levels) so anti-aliased CJK strokes look
 *  clean on the AMOLED at 16 px. Total flash cost ≈ 91 KiB.
 *
 * Font metrics
 * ------------
 *  Line height  20 px, base line 5 — matches Montserrat 16 closely so
 *  it can be substituted in place inside menu rows without re-laying
 *  the geometry out.
 */
#ifndef __LV_FONT_ZH_16_H__
#define __LV_FONT_ZH_16_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

/* ---------------------------------------------------------------------------
 * Public font symbol
 * --------------------------------------------------------------------------- */
extern const lv_font_t lv_font_zh_16;

#ifdef __cplusplus
}
#endif
#endif /* __LV_FONT_ZH_16_H__ */
