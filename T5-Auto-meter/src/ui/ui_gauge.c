/**
 * @file ui_gauge.c
 * @brief Reusable round gauge with a SINGLE-LINE rounded-cap needle,
 *        layered metallic hub and AABB dirty-area invalidation.
 * @version 1.8
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 *
 * Why a persistent tracker instead of per-call lv_anim
 * ----------------------------------------------------
 *  We run a single 200 Hz tracker timer in the LVGL task context
 *  (v1.8.2 — 2× LVGL's 100 Hz refresh, so every render consumes the
 *  latest two tracker steps' worth of angular delta). Each tick it
 *  glides `needle_angle_x10` toward `needle_target_x10` using an
 *  exponential step capped to a maximum velocity, with a 1-LSB
 *  minimum so it never stalls in the EMA's quantised tail. With the
 *  UI data-refresh tick at 30 Hz, that's ~6 tracker steps per data
 *  update — the EMA traces a low-pass curve through the data sequence
 *  rather than stepping discretely (the user's "在两次数据中间需要
 *  插值" feedback).
 *
 * Needle silhouette — single line (v1.8)
 * --------------------------------------
 *  v1.6 used 4 same-colour overlapping primitives (body + tip + 2 fill
 *  triangles); seams between adjacent same-colour primitives left
 *  sub-pixel hairlines. v1.7 tried "BODY-PLUS-CUTTERS" (one body line +
 *  two BG-coloured triangles cutting away the outer wedges into a
 *  taper); the cutter approach still produced visible AABB residual
 *  artefacts during slow rotation — BG triangles painted at frame N
 *  and the body line at frame N-1 had different rotated bboxes, so on
 *  some sub-pixel transitions the cutter would clear a wedge of pixels
 *  the body had previously painted, leaving black-triangle-then-red-line
 *  afterimages.
 *
 *  v1.8 follows the user's explicit guidance ("实在难实现就换成一根的吧"
 *  — "if it's that hard, just use a single line"): a single
 *  lv_draw_line, W=14, BACK → TIP, round_end=1. ONE primitive. The line
 *  has only its own outer red→bg AA edges (which the SW rasteriser
 *  handles cleanly) and a half-disc cap of radius W/2=7 at the tip. With
 *  one primitive there are zero interior boundaries, zero same-colour
 *  seams, zero overlapping rotated bboxes — by construction there can
 *  never be an internal artefact. The 60/40 taper from earlier versions
 *  is dropped in exchange for guaranteed clean rendering.
 *
 * Boot sweep — same dirty cadence as the steady-state tracker
 * -----------------------------------------------------------
 *  Earlier versions drove the boot sweep with lv_anim, which fires
 *  the angle update once per LVGL refresh tick (16-33 ms). At sweep
 *  mid-arc the eased curve hits ~1640°/s, so each tick advanced the
 *  needle ~10° — the consecutive AABBs sat far enough apart that
 *  LVGL couldn't coalesce them, and the SW rasterizer paid for 6
 *  mostly-disjoint rectangles every frame. The user reported "卡，远
 *  没有切屏丝滑" — and the contrast was exact: KEY-cycling uses the
 *  5 ms tracker timer where consecutive ticks differ by <1° and the
 *  invalidates collapse to a single tight region per refresh.
 *
 *  v1.8.4 ports the sweep onto the SAME 5 ms timer slot. Angle is
 *  computed via a fixed-point smoothstep (3t² − 2t³), peak per-tick
 *  delta drops to ~3°, and 6 ticks of dirty regions per LVGL refresh
 *  overlap heavily — coalesces back to ~1.5× single-AABB cost. Sweep
 *  now perceptually matches the steady-state glide. Once the timer's
 *  elapsed >= total it snaps angle to exact MIN and hands off to
 *  __sweep_finish() which restores LVGL refresh, drops the MIN-pose
 *  anchor, and re-seeds the dirty ring for the tracker.
 *
 *  An explicit lv_obj_invalidate(needle) at sweep end forces the
 *  resting-MIN pose to be pixel-clean (the smoothstep can leave a
 *  fractional-degree residual that the per-tick AABB chain may not
 *  perfectly cover at the AA halo).
 *
 * Heap stability under repeated KEY-cycling (v1.8)
 * ------------------------------------------------
 *  v1.5..v1.7 called __labels_clear (delete N lv_label objs) and then
 *  __labels_build (re-create N lv_label objs) on every gauge swap inside
 *  ui_gauge_set_def. Each create/delete pair leaves a few small fragmented
 *  free-list entries in the LVGL/TKL heap — over many KEY presses the
 *  arena thrashes, allocator latency climbs, and the user sees frame-rate
 *  collapse. v1.8 restructures the labels to be CREATE-ONCE-IN-PLACE:
 *
 *    - ui_gauge_create(): pre-creates UI_GAUGE_MAX_LABELS = 12 labels,
 *      all hidden by default.
 *    - ui_gauge_set_def(): __labels_apply() walks the 12 slots, sets
 *      text+position+visibility on slots 0..tick_major-1, hides
 *      slots tick_major..11. Zero alloc, zero free, zero per-cycle
 *      heap traffic.
 *    - ui_gauge_destroy(): deletes the labels (called once at teardown,
 *      not per cycle).
 *
 *  Net: the user can KEY-cycle indefinitely without performance decay.
 *
 * Layered metallic hub
 * --------------------
 *  Six concentric circles stacked centre-aligned, each subsequent layer
 *  smaller than the previous. Outer to inner:
 *      r = 30  red rim          UI_COLOR_PRIMARY  (#E53935)
 *      r = 26  very dark grey   #1A1A1A
 *      r = 22  dark grey        #353535
 *      r = 18  mid grey         #505050
 *      r = 14  specular hi-light #7A7A7A
 *      r =  8  near-black face  #202020 (UI_COLOR_HUB)
 *
 * Per-frame redraw — rotated AABB invalidation
 * --------------------------------------------
 *  The tracker computes the rotated AABB of the single needle line
 *  (LEN+W/2 along x, ±W/2 across) plus a 10 px PAD that covers AA halo
 *  and the round_end half-disc, and invalidates the UNION of the
 *  previous frame's AABB and the new one. Sweep frames use the same
 *  AABB-union machinery, plus the v1.8 sweep-end full invalidate.
 *
 * Visibility lifecycle
 * --------------------
 *  __needle_draw_event_cb returns immediately while g->needle_visible
 *  is FALSE — guards against a stray invalidate painting a stale MIN
 *  silhouette before the boot sweep starts. The flag flips to TRUE
 *  inside ui_gauge_sweep() and stays TRUE thereafter.
 */
#include "ui_gauge.h"
#include "ui_theme.h"
#include "app_config.h"
#include "tal_api.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define GAUGE_OUTER_R          220
#define GAUGE_TICK_LEN_MAJOR   18
#define GAUGE_TICK_LEN_MINOR   8

/* Needle geometry — single-line design (v1.8). Local frame: pivot at
 * origin, +x toward tip.
 *
 *       ┌──────── body W=14 ───────┐━━╮
 *       │                          │ cap r=W/2=7  (round_end=1)
 *       └────── pivot at 0,0 ──────┘━━╯
 *      -BACK                      TIP
 *
 * - Body / taper / tip : a single lv_draw_line, p1=(-BACK,0), p2=(TIP,0),
 *           width=W=14, round_start=0, round_end=1. The line is rendered
 *           as a uniform-W=14 strip with a half-disc of radius W/2=7
 *           extending past TIP. There is no internal seam, no overlapping
 *           primitive — by construction. Trades the old 60/40 taper for
 *           guaranteed seam-free rendering.
 * - Back  : flat (round_start=0). Tucked under the Ø16 hub face — the
 *           furthest back corner is at radius √(BACK²+(W/2)²) ≈ 15.65 < 16
 *           so the flat back disappears inside the hub disc.
 *
 * AABB extent: rectangle [-BACK..LEN+W/2] × [-W/2..+W/2], + a uniform
 * 10 px PAD to absorb the line's AA halo (~1 px) plus the round_end's
 * sub-pixel rasterisation tail under arbitrary rotation. PAD=10 leaves
 * comfortable 2-3 px headroom so the per-frame swept band ALWAYS
 * strictly contains every pixel the SW renderer touched on the previous
 * frame — eliminates "trailing strip" residue at slow rotation.
 */
#define GAUGE_NEEDLE_LEN       180   /**< pivot → tip centre */
#define GAUGE_NEEDLE_BACK      14    /**< tail behind pivot (under hub) */
#define GAUGE_NEEDLE_W         14    /**< uniform line width */
/* AABB safety margin (px). v1.8.2 widened from 10 → 16 so the swept
 * band strictly covers the next-frame pose's AA halo at peak sweep
 * angular velocity (~7°/frame at the ease-in-out apex, ~22 px tip
 * displacement per frame), eliminating sub-pixel residue without
 * resorting to full-obj invalidation per frame. */
#define GAUGE_NEEDLE_PAD       16
/* Dirty-AABB ring depth. Ring of 4 covers buffer-alternation drift
 * up to 4 frames (~40 ms at LVGL's 10 ms refresh) — comfortably
 * absorbs LVGL load skew on T5AI under OBD+IMU task contention.
 * v1.8 used 2 (theory: dual-buffer alternation is strict); reality
 * showed the alternation slips occasionally under load, leaking
 * frame N-3's paint through. */
#define GAUGE_DIRTY_RING       4

/* Six-layer metallic hub. Radii decrease ~4 px per ring so each layer
 * is a clean 4-px-wide annulus visible to the eye. */
#define GAUGE_HUB_R_RIM        30    /**< Ø60 red rim */
#define GAUGE_HUB_R_DIM1       26    /**< very dark grey  #1A1A1A */
#define GAUGE_HUB_R_DIM2       22    /**< dark grey       #353535 */
#define GAUGE_HUB_R_MID        18    /**< mid grey        #505050 */
#define GAUGE_HUB_R_HI         14    /**< specular hi-light #7A7A7A */
#define GAUGE_HUB_R_FACE        8    /**< near-black face #202020 (UI_COLOR_HUB) */

/* Hub palette (kept local to this file — they're a sub-graphic of the
 * needle module and not reused elsewhere). */
#define GAUGE_HUB_C_DIM1       lv_color_hex(0x1A1A1A)
#define GAUGE_HUB_C_DIM2       lv_color_hex(0x353535)
#define GAUGE_HUB_C_MID        lv_color_hex(0x505050)
#define GAUGE_HUB_C_HI         lv_color_hex(0x7A7A7A)

/* Manually-placed dial digit radius, measured from the centre of the
 * panel to the centre of each label rectangle. With the major tick inner
 * end at radius 198 and the largest expected digit string ("1500") taking
 * ~60 px wide at 32 pt, the closest the label rectangle's outer edge can
 * get to the tick line at any orientation is:
 *     radius 198 − (radius 150 + half_width 30)  =  18 px gap
 * which is the breathing room the user asked for. */
#define GAUGE_LBL_RADIUS       150
#define GAUGE_ANGLE_RANGE      270   /**< degrees swept by the dial */
#define GAUGE_ANGLE_ROTATION   135   /**< 0° tick at 7:30 position */

/* Angle range in 0.1° (LVGL trig system: 0 = 3 o'clock, +deg = clockwise). */
#define GAUGE_ANGLE_X10_MIN    (GAUGE_ANGLE_ROTATION * 10)
#define GAUGE_ANGLE_X10_MAX    ((GAUGE_ANGLE_ROTATION + GAUGE_ANGLE_RANGE) * 10)

/* Persistent tracker. v1.8.2 raised from 125 Hz → 200 Hz (8 ms → 5 ms)
 * so the tracker fires twice per LVGL refresh (LVGL's
 * LV_DEF_REFR_PERIOD = 10 ms = 100 Hz). At 200 Hz tick × 30 Hz data
 * refresh, every data update goes through ~6 tracker steps, smoothing
 * the angular delta into a low-pass curve rather than a discrete step
 * — the visible result is the user's "两次数据中间插值"  ask for
 * mid-data smoothness. α and velocity cap are halved from v1.8 to
 * keep τ unchanged (~65 ms) so the perceived response time stays the
 * same, only the smoothness improves.
 *
 * Math:
 *   τ = -dt / ln(1 - α)
 *   v1.8 :  dt = 8 ms, α = 0.12 → τ ≈ 62.6 ms,  v_max = 6°/8ms = 750°/s
 *   v1.8.2: dt = 5 ms, α = 0.075→ τ ≈ 64.3 ms, v_max = 4°/5ms = 800°/s   */
#define GAUGE_TRACK_PERIOD_MS  5      /**< 200 Hz tracker (2× LVGL refresh) */
#define GAUGE_TRACK_ALPHA_X1000 75    /**< 7.5 %/frame ≈ τ ≈ 64 ms @ 200 Hz */
#define GAUGE_TRACK_VEL_X10    40     /**< max 4°/frame ≈ 800°/s catch-up */
#define GAUGE_DEADBAND_X10     2      /**< snap when |err| < 0.2° */

/* MIN-pose anchor lifetime during the boot sweep.
 * The anchor only matters while the needle is still close to MIN
 * (where dual-buffer alternation drift could orphan its silhouette).
 * Once the sweep has carried the needle far enough away that the
 * 4-deep prev_dirty ring fully scrolls past MIN, the anchor invalidate
 * is pure overhead. 100 ms covers the worst-case dual-buffer
 * alternation drift we measure (~50 ms under BLE-write storms) plus
 * the eased ramp's slow-start dwell near MIN — at 167 Hz refresh
 * (TURBO) and 200 Hz tracker that's 16-20 sweep-ticks of belt-and-
 * braces coverage. After that we drop the anchor and let the 4-deep
 * prev_dirty ring carry the load alone, which frees ~25% of the SW
 * rasterizer budget for the higher-frequency refresh. */
#define GAUGE_SWEEP_ANCHOR_WINDOW_MS 100

/* LVGL display refresh period — TUYA T5AI ships with LV_DEF_REFR_PERIOD
 * = 10 ms (100 Hz). v1.8.6 pushed sweep to 8 ms (125 Hz); v1.8.7
 * pushes further to 6 ms (~167 Hz) for the smoothness the user is
 * still asking for. Why this specific ceiling:
 *
 *   - 5 ms (200 Hz) was v1.8.3's catastrophe: SW rasterizer can't
 *     finish a 5-AABB frame in 5 ms while BLE characteristic-write
 *     storms compete for the CM33; lv_timer_handler() falls behind,
 *     real fps collapses ("比上一版还烂").
 *   - 6 ms (167 Hz) leaves ~1 ms of safety margin above the
 *     measured 5 ms rasterizer baseline at our 5-AABB footprint,
 *     which the BLE/IMU interrupt service routines comfortably fit
 *     into. We additionally tighten the anchor window from 150 ms
 *     to 100 ms (see GAUGE_SWEEP_ANCHOR_WINDOW_MS) so the post-100ms
 *     mid-arc only carries 4 AABBs, dropping ~20% of the rasterizer
 *     load — that's the ~1 ms we steal back to feed the 6 ms refresh.
 *   - Mid-arc per-frame angle delta drops from 4.92° → 3.69°, the
 *     eye stops resolving frame edges entirely.
 *
 * Steady-state stays at 100 Hz default — the EMA tracker is gentle
 * enough that 100 Hz looks perfect there, and we save ~40% of the
 * SW rasterizer cycles for BLE/IMU/data-path work. */
#define GAUGE_REFR_PERIOD_TURBO_MS 6

/* The needle obj's bounding box: just big enough to contain the rotated
 * needle at any angle — a square with side = 2*(LEN + small pad). The
 * AABB invalidation logic does the per-frame fine-grained dirty area. */
#define GAUGE_NEEDLE_BOX       ((GAUGE_NEEDLE_LEN + GAUGE_NEEDLE_PAD) * 2)

/* ---------------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------------- */
STATIC VOID_T  __sweep_timer_cb(lv_timer_t *t);
STATIC VOID_T  __sweep_finish(UI_GAUGE_T *g);
STATIC VOID_T  __track_timer_cb(lv_timer_t *t);
STATIC VOID_T  __needle_draw_event_cb(lv_event_t *e);
STATIC int32_t __value_to_angle_x10(const UI_GAUGE_T *g, int32_t value);
STATIC int32_t __clamp(int32_t v, int32_t lo, int32_t hi);
STATIC VOID_T  __scale_apply_ticks(lv_obj_t *scale, uint8_t tick_major);
STATIC int32_t __sweep_ease_in_out_x1000(uint32_t t_ms, uint32_t period_ms);
STATIC VOID_T  __display_refr_period_set(uint32_t period_ms);
STATIC uint32_t __display_refr_period_get(VOID_T);

/* ---------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------------- */
/**
 * @brief Clamp helper.
 * @param[in] v input
 * @param[in] lo lower bound
 * @param[in] hi upper bound
 * @return clamped value
 */
STATIC int32_t __clamp(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

/**
 * @brief Set the LVGL default-display refresh timer period.
 *
 * Wraps lv_display_get_refr_timer + lv_timer_set_period so callers
 * don't need the timer handle. Used to push refresh from the 100 Hz
 * default to 125 Hz during the boot sweep (see GAUGE_REFR_PERIOD_*
 * docs) and back when the sweep ends.
 *
 * @param[in] period_ms new refresh period in milliseconds
 * @return none
 * @note No-op if the default display or its refresh timer is NULL —
 *       happens transiently before lv_init / display registration in
 *       weird boot orders, and isn't fatal there.
 */
STATIC VOID_T __display_refr_period_set(uint32_t period_ms)
{
    lv_display_t *disp = lv_display_get_default();
    if (disp == NULL) {
        return;
    }
    lv_timer_t *t = lv_display_get_refr_timer(disp);
    if (t == NULL) {
        return;
    }
    lv_timer_set_period(t, period_ms);
}

/**
 * @brief Read the current LVGL default-display refresh timer period.
 *
 * Used by the sweep entry point to snapshot the steady-state period
 * (whatever LVGL's lv_conf.h LV_DEF_REFR_PERIOD happens to be — 10 ms
 * on T5AI today, but other platforms or future config changes may
 * pick a different baseline) so __sweep_finish() can restore it
 * exactly. Hard-coding 10 ms here would silently break that.
 *
 * @return current refresh period in ms, or LV_DEF_REFR_PERIOD if
 *         the display/timer is unavailable.
 * @note LVGL v9 doesn't expose lv_timer_get_period(), but the period
 *       field of lv_timer_t is a public struct member (see
 *       lv_timer.h: `uint32_t period; / **< How often... * /`), so
 *       reading it directly is sanctioned and stable across patch
 *       releases.
 */
STATIC uint32_t __display_refr_period_get(VOID_T)
{
    lv_display_t *disp = lv_display_get_default();
    if (disp == NULL) {
        return LV_DEF_REFR_PERIOD;
    }
    lv_timer_t *t = lv_display_get_refr_timer(disp);
    if (t == NULL) {
        return LV_DEF_REFR_PERIOD;
    }
    return t->period;
}

/**
 * @brief Map a user-units value to the LVGL trigo angle in 0.1° units.
 * @param[in] g gauge
 * @param[in] value raw user-units value
 * @return angle in 0.1°
 */
STATIC int32_t __value_to_angle_x10(const UI_GAUGE_T *g, int32_t value)
{
    if (g->val_max <= g->val_min) {
        return GAUGE_ANGLE_X10_MIN;
    }
    int32_t v = __clamp(value, g->val_min, g->val_max);
    int64_t span = (int64_t)(GAUGE_ANGLE_X10_MAX - GAUGE_ANGLE_X10_MIN);
    int64_t num  = span * (int64_t)(v - g->val_min);
    return GAUGE_ANGLE_X10_MIN + (int32_t)(num / (int64_t)(g->val_max - g->val_min));
}

/**
 * @brief Compute the axis-aligned bounding box of the needle hairpin at
 *        a given angle, in screen-absolute pixel coordinates.
 *
 * The v1.7 hairpin's local-frame bounding rectangle is
 *     [-BACK ..  LEN + TIP_W/2]  ×  [-W/2 .. +W/2]
 * because:
 *   - body line covers [-BACK..LEN] × [-W/2..+W/2]
 *   - cutter triangles are nested INSIDE the body line's bbox
 *   - tip cap (radius TIP_W/2 half-disc) extends to LEN + TIP_W/2 in +x,
 *     and ±TIP_W/2 in y (already inside ±W/2)
 * We rotate the rectangle's 4 corners and take their screen-AABB. The
 * (now 10 px) GAUGE_NEEDLE_PAD margin strictly contains every pixel the
 * SW renderer can touch including the AA halo of the body line's flat
 * edges and the cutter triangles' diagonal red→bg transition.
 *
 * @param[in] g gauge (needle obj coords are read for the pivot)
 * @param[in] angle_x10 needle angle in 0.1°
 * @param[out] out AABB in screen-absolute coords
 * @return none
 */
STATIC VOID_T __needle_compute_aabb(const UI_GAUGE_T *g,
                                    int32_t angle_x10, lv_area_t *out)
{
    lv_area_t coords;
    lv_obj_get_coords(g->needle, &coords);
    int32_t cx = (coords.x1 + coords.x2) / 2;
    int32_t cy = (coords.y1 + coords.y2) / 2;

    int16_t a = (int16_t)(angle_x10 / 10);
    int32_t s = lv_trigo_sin(a);
    int32_t c = lv_trigo_sin((int16_t)(a + 90));

    /* Conservative AABB rectangle in the local needle frame: the single
     * line's bounding box extended by W/2 in +x for the round_end half-
     * disc cap. y range is ±W/2 because the line is W-wide everywhere. */
    static const int32_t LX[4] = {
        -GAUGE_NEEDLE_BACK,                                 -GAUGE_NEEDLE_BACK,
         GAUGE_NEEDLE_LEN + GAUGE_NEEDLE_W / 2,
         GAUGE_NEEDLE_LEN + GAUGE_NEEDLE_W / 2,
    };
    static const int32_t LY[4] = {
        -GAUGE_NEEDLE_W / 2,  GAUGE_NEEDLE_W / 2,
        -GAUGE_NEEDLE_W / 2,  GAUGE_NEEDLE_W / 2,
    };

    int32_t xmin = INT32_MAX, xmax = INT32_MIN;
    int32_t ymin = INT32_MAX, ymax = INT32_MIN;
    int i;
    for (i = 0; i < 4; i++) {
        int32_t rx = (LX[i] * c - LY[i] * s) >> 15;
        int32_t ry = (LX[i] * s + LY[i] * c) >> 15;
        int32_t x = cx + rx;
        int32_t y = cy + ry;
        if (x < xmin) { xmin = x; }
        if (x > xmax) { xmax = x; }
        if (y < ymin) { ymin = y; }
        if (y > ymax) { ymax = y; }
    }

    out->x1 = xmin - GAUGE_NEEDLE_PAD;
    out->y1 = ymin - GAUGE_NEEDLE_PAD;
    out->x2 = xmax + GAUGE_NEEDLE_PAD;
    out->y2 = ymax + GAUGE_NEEDLE_PAD;
}

/**
 * @brief Seed all 4 ring slots with a single AABB. Used at sweep
 *        start / sweep end / set_def — whenever the tracker is
 *        "starting fresh" and there's no real previous-frame trail.
 *
 * @param[in,out] g gauge
 * @param[in] seed AABB to copy into every ring slot
 * @return none
 */
STATIC VOID_T __needle_dirty_ring_seed(UI_GAUGE_T *g, const lv_area_t *seed)
{
    int i;
    for (i = 0; i < GAUGE_DIRTY_RING; i++) {
        g->prev_dirty[i] = *seed;
    }
    g->prev_dirty_idx = 0;
}

/**
 * @brief Mark the LAST 4 frames' AABBs + the current frame's AABB
 *        dirty, then rotate the new AABB into the ring.
 *
 * Why ring of 4 and not 2 (v1.8) or full-obj (v1.8.1)?
 * ----------------------------------------------------
 *  CONFIG_ENABLE_LVGL_DUAL_DISP_BUFF=y on T5AI gives us two
 *  framebuffers. Theory says strict alternation A/B/A/B/... means a
 *  ring of 2 covers both buffers' previous paint. Practice says
 *  alternation drifts under load (BLE characteristic-write storm
 *  during ELM327 init, IMU sample contention, heap thrash) — the
 *  same buffer can be rendered twice in a row, or one buffer can
 *  skip a render cycle entirely. With a 2-prev ring this leaves
 *  N-3's paint stale on whichever buffer drifted, which the user
 *  sees as the "残留" at the dial-MIN scale during the boot sweep.
 *
 *  v1.8.1 brute-forced the issue with `lv_obj_invalidate(needle)`
 *  every sweep frame (full 380×380 obj area = 144 kpx fill). That
 *  guarantees clean buffers but stalls the SW rasterizer below the
 *  LVGL refresh budget — the user reported "动画非常卡，帧率需要翻
 *  4倍".
 *
 *  v1.8.2 (this version) — ring of 4 small AABBs (each ~6 kpx) plus
 *  the new one = ~30 kpx fill total per frame. 4 frames ≈ 40 ms of
 *  history at 100 Hz LVGL refresh, more than the worst-case load
 *  skew we observe. Rasterizer overhead drops 5× vs full-obj while
 *  still completely eliminating the residue.
 *
 *  LVGL coalesces overlapping dirty rectangles internally when the
 *  motion is slow (so 4 mostly-overlapping AABBs collapse to ≈ 1
 *  during steady tracking) — the cost only goes up during fast
 *  sweep frames where each AABB sits next to the previous, which
 *  is exactly when we need the full breadcrumb trail.
 *
 * @param[in,out] g gauge
 * @return none
 */
STATIC VOID_T __needle_invalidate_swept(UI_GAUGE_T *g)
{
    lv_area_t new_area;
    __needle_compute_aabb(g, g->needle_angle_x10, &new_area);

    int i;
    for (i = 0; i < GAUGE_DIRTY_RING; i++) {
        lv_obj_invalidate_area(g->root, &g->prev_dirty[i]);
    }
    lv_obj_invalidate_area(g->root, &new_area);

    /* Boot-sweep only: scrub the MIN pose every frame so dual-buffer
     * alternation drift can't leave the starting needle silhouette
     * dangling (user-reported "WATER TEMP 40 下面的区块有指针残留").
     *
     * The cache is built ONCE at sweep_start (in ui_gauge_sweep,
     * AFTER lv_obj_update_layout forces the needle obj's coords to
     * resolve), so this fast-path is just one cheap invalidate per
     * frame — same order of magnitude as one ring slot.
     *
     * We deliberately do NOT also scrub MAX: in practice only the
     * MIN end shows the residue, and adding a second far-apart small
     * AABB doubles the SW renderer's region dispatch cost during the
     * wide-span middle frames where MIN and the live AABB sit on
     * opposite sides of the dial. */
    if (g->sweep_anchor_armed) {
        lv_obj_invalidate_area(g->root, &g->sweep_anchor_dirty);
    }

    /* Ring-buffer write: overwrite the oldest slot, advance idx mod RING. */
    g->prev_dirty[g->prev_dirty_idx] = new_area;
    g->prev_dirty_idx = (uint8_t)((g->prev_dirty_idx + 1) % GAUGE_DIRTY_RING);
}

/**
 * @brief Persistent ~100 Hz tracker that glides the displayed angle toward
 *        the target with a velocity-capped exponential filter.
 *
 * @param[in] t LVGL timer handle (user data = UI_GAUGE_T*)
 * @return none
 *
 * @note Runs in LVGL task context; safe to call invalidate without locks.
 *       Disabled while the boot sweep animation is active to avoid the
 *       two writers fighting over needle_angle_x10.
 */
STATIC VOID_T __track_timer_cb(lv_timer_t *t)
{
    UI_GAUGE_T *g = (UI_GAUGE_T *)lv_timer_get_user_data(t);
    if (g == NULL || g->needle == NULL || g->sweep_running) {
        return;
    }

    int32_t err = g->needle_target_x10 - g->needle_angle_x10;
    int32_t abs_err = (err < 0) ? -err : err;

    if (abs_err < GAUGE_DEADBAND_X10) {
        /* Quietly snap to target if we're already inside the dead-band. */
        if (g->needle_angle_x10 != g->needle_target_x10) {
            g->needle_angle_x10 = g->needle_target_x10;
            __needle_invalidate_swept(g);
        }
        return;
    }

    /* Exponential step toward target, then velocity-cap so big jumps
     * (boot, BLE intro, gauge cycle) sweep at a constant max rate
     * instead of teleporting. v1.8.2 uses /1000 scaling because at
     * 200 Hz tick the per-frame α drops below 0.1 — /100 quantised
     * away ~30% of the EMA's signal and produced a visible "stair-
     * step" in the slow tail. /1000 keeps the integer math clean. */
    int32_t step = (err * GAUGE_TRACK_ALPHA_X1000) / 1000;
    if (step >  GAUGE_TRACK_VEL_X10) {
        step =  GAUGE_TRACK_VEL_X10;
    }
    if (step < -GAUGE_TRACK_VEL_X10) {
        step = -GAUGE_TRACK_VEL_X10;
    }
    /* Don't stall in the EMA's quantised tail. */
    if (step == 0 && err != 0) {
        step = (err > 0) ? 1 : -1;
    }

    g->needle_angle_x10 += step;
    __needle_invalidate_swept(g);
}

/**
 * @brief LVGL animation step for the boot sweep.
 *
 * Writes the current angle directly and reuses the steady-state AABB-
 * ring invalidation. The persistent tracker is paused via
 * g->sweep_running while this is active.
 *
 * History
 * -------
 *  v1.8   used the AABB ring with depth 2; the user reported residue
 *         at the dial-MIN position when the sweep moved away from the
 *         starting pose under load.
 *  v1.8.1 swapped to `lv_obj_invalidate(g->needle)` (full 380×380
 *         needle obj area) every sweep frame. This eliminated the
 *         residue but costs ~144 kpx of fill per frame; the SW
 *         rasterizer fell behind the 100 Hz LVGL refresh budget and
 *         the user reported "动画非常卡，帧率需要翻4倍".
 *  v1.8.2 (this version) deepens the AABB ring to 4 frames and uses
 *         the same `__needle_invalidate_swept` for sweep AND tracker.
 *         Total per-frame fill ~30 kpx — well under the rasterizer's
 *         budget — and 40 ms of trail history covers the worst-case
 *         buffer-alternation drift we see on T5AI under load.
 *
 * @param[in] obj gauge handle (passed via lv_anim_set_var)
 * @param[in] a_x10 current angle in 0.1°
 * @return none
 */
/**
 * @brief Smoothstep ease-in-out, fixed-point.
 *
 * Returns 0..1000 representing 0.0..1.0 progress along a smoothstep
 * curve (3t² − 2t³). Used to drive the boot sweep's angle interpolation
 * with the same shape as LVGL's lv_anim_path_ease_in_out — but evaluated
 * once per 5 ms tracker tick instead of once per LVGL refresh, so the
 * resulting per-tick angle delta is small (<3°) and consecutive needle
 * AABBs overlap heavily. The SW rasterizer then coalesces 3-7 invalidate
 * calls between LVGL refreshes into a single mostly-overlapping dirty
 * region, dropping per-second fill from ~360 kpx (lv_anim @ 60 Hz, 6
 * disjoint AABBs/frame) to ~90 kpx (5 ms timer @ 30 Hz LVGL refresh,
 * coalesced). Same visual smoothness as the steady-state tracker.
 *
 * @param[in] t_ms      elapsed time in ms (clamped to [0, period_ms])
 * @param[in] period_ms half-leg duration in ms (e.g. 660 for a 1320 ms sweep)
 * @return progress in 0..1000
 */
STATIC int32_t __sweep_ease_in_out_x1000(uint32_t t_ms, uint32_t period_ms)
{
    if (period_ms == 0) {
        return 1000;
    }
    if (t_ms >= period_ms) {
        return 1000;
    }
    int64_t r = (int64_t)t_ms * 1000 / (int64_t)period_ms;  /* 0..1000 */
    int64_t r2 = (r * r) / 1000;
    int64_t r3 = (r2 * r) / 1000;
    int64_t out = 3 * r2 - 2 * r3;
    if (out < 0) {
        return 0;
    }
    if (out > 1000) {
        return 1000;
    }
    return (int32_t)out;
}

/**
 * @brief Per-tick boot sweep driver (5 ms tracker timer slot).
 *
 * Runs at GAUGE_TRACK_PERIOD_MS so its dirty-region trail merges with
 * the same 5 ms cadence as the steady-state tracker — see
 * __sweep_ease_in_out_x1000() for why this shape gives the user the
 * "as smooth as KEY-cycling" feel they asked for.
 *
 * Time domain: 0 .. sweep_total_ms. Half-point splits forward leg
 * (MIN→MAX) from the playback leg (MAX→MIN). When elapsed >= total,
 * we hand off to __sweep_finish() which restores the steady state.
 *
 * @param[in] t timer handle (user_data is the gauge)
 * @return none
 */
STATIC VOID_T __sweep_timer_cb(lv_timer_t *t)
{
    UI_GAUGE_T *g = (UI_GAUGE_T *)lv_timer_get_user_data(t);
    if (g == NULL || g->needle == NULL) {
        lv_timer_delete(t);
        return;
    }
    if (!g->sweep_running) {
        return;     /* finish-helper will tear the timer down */
    }

    uint32_t now      = lv_tick_get();
    uint32_t elapsed  = now - g->sweep_start_ms;
    uint32_t total    = g->sweep_total_ms;
    uint32_t half     = total / 2;
    int32_t  span_x10 = GAUGE_ANGLE_X10_MAX - GAUGE_ANGLE_X10_MIN;

    /* Disarm the MIN-pose anchor once we're past the buffer-drift
     * window — see GAUGE_SWEEP_ANCHOR_WINDOW_MS docs. By the time
     * we cross the window the needle is already a couple of degrees
     * off MIN and the prev_dirty ring fully scrubs the starting
     * pose; the anchor is just a mid-arc tax on the rasterizer,
     * costing us refresh-rate headroom. */
    if (g->sweep_anchor_armed && elapsed >= GAUGE_SWEEP_ANCHOR_WINDOW_MS) {
        g->sweep_anchor_armed = FALSE;
    }

    int32_t angle_x10;
    if (elapsed < half) {
        /* forward leg: MIN → MAX */
        int32_t p = __sweep_ease_in_out_x1000(elapsed, half);
        angle_x10 = GAUGE_ANGLE_X10_MIN + (int32_t)((int64_t)p * span_x10 / 1000);
    } else if (elapsed < total) {
        /* playback leg: MAX → MIN */
        int32_t p = __sweep_ease_in_out_x1000(elapsed - half, half);
        angle_x10 = GAUGE_ANGLE_X10_MAX - (int32_t)((int64_t)p * span_x10 / 1000);
    } else {
        goto natural_end;
    }

    if (angle_x10 != g->needle_angle_x10) {
        g->needle_angle_x10  = angle_x10;
        g->needle_target_x10 = angle_x10;   /* keep tracker target == current */
        __needle_invalidate_swept(g);
    }
    return;

natural_end:
    /* Snap to exact MIN (sub-tick error from the ease curve can leave
     * angle_x10 a few tenths of a degree off zero) before the finish
     * helper invalidates the resting pose. */
    g->needle_angle_x10  = GAUGE_ANGLE_X10_MIN;
    g->needle_target_x10 = GAUGE_ANGLE_X10_MIN;
    __sweep_finish(g);
}

/**
 * @brief Boot sweep teardown — invoked when the sweep timer reaches
 *        total duration OR when the sweep is aborted by ui_gauge_set_def().
 *
 * Both paths share the same final state: needle parked at MIN,
 * sweep_running cleared, anchor disarmed, LVGL refresh restored to
 * BASE, dirty ring re-seeded so the tracker resumes cleanly. Aborts
 * additionally need the timer torn down (the natural-finish path
 * tears down BEFORE the cb returns, see __sweep_timer_cb above).
 *
 * Note: v1.6 keeps the needle VISIBLE at the dial minimum (135° = SW)
 * after the sweep completes — that's the resting pose. The first call
 * to ui_gauge_set_value() then glides the needle from MIN to the live
 * value (the user explicitly wants to watch this transition).
 *
 * @param[in,out] g gauge
 * @return none
 */
STATIC VOID_T __sweep_finish(UI_GAUGE_T *g)
{
    if (g == NULL) {
        return;
    }
    g->sweep_running = FALSE;
    /* Caller decides the resting angle:
     *   __sweep_timer_cb (natural finish) snaps to exact MIN before
     *     calling us, so the resting pose is pixel-clean.
     *   ui_gauge_set_def (KEY-cycle abort) leaves whatever angle the
     *     needle was at when the user pressed KEY, then locks the
     *     tracker target to it for a smooth glide to the new live value.
     * Either way we just lock target == current here. */
    g->needle_target_x10 = g->needle_angle_x10;
    /* Drop the per-frame MIN anchor — steady-state tracker doesn't
     * need it (the 4-deep ring is sufficient at the gentle angular
     * velocities the EMA tracker produces). */
    g->sweep_anchor_armed = FALSE;

    /* Restore the LVGL refresh period that was active before sweep —
     * see GAUGE_REFR_PERIOD_TURBO_MS docs for why we don't hardcode
     * LV_DEF_REFR_PERIOD here. Snapshot is captured in ui_gauge_sweep
     * and stored on the gauge so dual-gauge layouts (ui_gforce + the
     * regular gauge) don't step on each other if both finish out of
     * order — each restores from its own snapshot. */
    if (g->sweep_saved_refr != 0) {
        __display_refr_period_set(g->sweep_saved_refr);
        g->sweep_saved_refr = 0;
    }

    if (g->sweep_timer != NULL) {
        lv_timer_delete(g->sweep_timer);
        g->sweep_timer = NULL;
    }

    /* Force a CLEAN repaint of the whole needle area when the sweep
     * completes. The per-tick AABB trail may have left a sub-pixel
     * smudge near MIN since the last tick can land on a slightly
     * different sub-angle than the resting pose. A one-shot full
     * needle invalidate makes the resting-MIN pose pixel-clean. */
    if (g->needle) {
        lv_obj_invalidate(g->needle);
        lv_area_t resting;
        __needle_compute_aabb(g, g->needle_angle_x10, &resting);
        __needle_dirty_ring_seed(g, &resting);
    }
}

/**
 * @brief Rotate a local-frame point (lx, ly) into screen-absolute (sx, sy).
 *
 * @param[in] cx pivot screen X
 * @param[in] cy pivot screen Y
 * @param[in] s sin(angle) * LV_TRIGO_SIN_MAX
 * @param[in] c cos(angle) * LV_TRIGO_SIN_MAX
 * @param[in] lx local X (along needle, +x = toward tip)
 * @param[in] ly local Y (perpendicular to needle, +y = right of needle)
 * @param[out] sx output screen X
 * @param[out] sy output screen Y
 * @return none
 */
STATIC inline VOID_T __rotate_local(int32_t cx, int32_t cy,
                                    int32_t s, int32_t c,
                                    int32_t lx, int32_t ly,
                                    int32_t *sx, int32_t *sy)
{
    *sx = cx + ((lx * c - ly * s) >> 15);
    *sy = cy + ((lx * s + ly * c) >> 15);
}

/**
 * @brief LV_EVENT_DRAW_MAIN handler for the needle overlay.
 *
 * v1.8: SINGLE LINE primitive — the entire needle is one lv_draw_line
 * with width=GAUGE_NEEDLE_W=14, round_start=0 (flat back tucked under
 * the hub) and round_end=1 (half-disc cap of radius W/2 at the tip).
 *
 * Rationale: with one primitive there are zero same-colour interior
 * boundaries, zero overlapping rotated sub-bboxes and zero AABB
 * residuals during slow rotation. The taper from earlier multi-
 * primitive designs is dropped on the user's explicit guidance —
 * "实在难实现就换成一根的吧".
 *
 * Visibility guard: returns immediately if g->needle_visible is FALSE
 * (between create() and the first ui_gauge_sweep()), so a stray invalidate
 * cannot paint a stale MIN-angle silhouette on the boot screen.
 *
 * @param[in] e LVGL event
 * @return none
 */
STATIC VOID_T __needle_draw_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) {
        return;
    }
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (obj == NULL) {
        return;
    }
    UI_GAUGE_T *g = (UI_GAUGE_T *)lv_obj_get_user_data(obj);
    if (g == NULL || !g->needle_visible) {
        return;
    }
    lv_layer_t *layer = lv_event_get_layer(e);
    if (layer == NULL) {
        return;
    }

    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    int32_t cx = (coords.x1 + coords.x2) / 2;
    int32_t cy = (coords.y1 + coords.y2) / 2;

    /* Q15 sin/cos. lv_trigo_sin returns sin(angle) * LV_TRIGO_SIN_MAX. */
    int16_t a_deg = (int16_t)(g->needle_angle_x10 / 10);
    int32_t s = lv_trigo_sin(a_deg);
    int32_t c = lv_trigo_sin((int16_t)(a_deg + 90));

    const int32_t BACK_X = -GAUGE_NEEDLE_BACK;
    const int32_t TIP_X  =  GAUGE_NEEDLE_LEN;

    int32_t p1x, p1y, p2x, p2y;
    __rotate_local(cx, cy, s, c, BACK_X, 0, &p1x, &p1y);
    __rotate_local(cx, cy, s, c, TIP_X,  0, &p2x, &p2y);

    lv_draw_line_dsc_t l;
    lv_draw_line_dsc_init(&l);
    l.color       = UI_COLOR_PRIMARY;
    l.opa         = LV_OPA_COVER;
    l.width       = GAUGE_NEEDLE_W;
    l.round_start = 0;     /* flat back is hidden under the hub face */
    l.round_end   = 1;     /* half-disc nose cap, radius=W/2=7 */
    l.p1.x = (lv_value_precise_t)p1x;
    l.p1.y = (lv_value_precise_t)p1y;
    l.p2.x = (lv_value_precise_t)p2x;
    l.p2.y = (lv_value_precise_t)p2y;
    lv_draw_line(layer, &l);
}

/**
 * @brief (Re)apply tick configuration to an existing scale.
 * @param[in] scale lv_scale handle
 * @param[in] tick_major number of major ticks (>= 2)
 * @return none
 */
STATIC VOID_T __scale_apply_ticks(lv_obj_t *scale, uint8_t tick_major)
{
    int32_t majors = (tick_major < 2) ? 2 : tick_major;
    int32_t minors_per_major = 4;
    int32_t total_ticks = (majors - 1) * minors_per_major + 1;
    lv_scale_set_total_tick_count(scale, total_ticks);
    lv_scale_set_major_tick_every(scale, minors_per_major);
}

/**
 * @brief Pre-create UI_GAUGE_MAX_LABELS dial digit labels (all hidden).
 *
 * v1.8 change: labels are no longer destroyed and recreated when the
 * gauge range/tick count changes (KEY-cycling). Instead we create the
 * MAX number of slots once at create time and __labels_apply() below
 * just rewrites text + position + visibility on subsequent set_def
 * calls. This eliminates a slow per-cycle alloc/free cycle in the LVGL
 * heap (each lv_label_create touches several small allocations whose
 * fragments add up over hundreds of KEY presses, eventually causing
 * frame-rate collapse — exactly the "memory leak" the user reported).
 *
 * @param[in,out] g gauge
 * @return none
 */
STATIC VOID_T __labels_create_all(UI_GAUGE_T *g)
{
    int i;
    for (i = 0; i < UI_GAUGE_MAX_LABELS; i++) {
        lv_obj_t *lbl = lv_label_create(g->root);
        lv_label_set_text(lbl, "");
        lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_32, 0);
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        g->labels[i] = lbl;
    }
    g->label_count = 0;
}

/**
 * @brief Update the pre-created labels for a new (range, tick_major)
 *        configuration. Zero alloc, zero free.
 *
 * Slots [0..tick_major-1] get the right value text, polar-coordinates
 * position on GAUGE_LBL_RADIUS, and are made visible. Slots
 * [tick_major..MAX-1] are hidden. Replaces the legacy delete-and-
 * recreate __labels_build path.
 *
 * @param[in,out] g gauge
 * @param[in] val_min lower bound of the dial range
 * @param[in] val_max upper bound of the dial range
 * @param[in] tick_major number of major ticks (clamped to [2 .. MAX])
 * @return none
 */
STATIC VOID_T __labels_apply(UI_GAUGE_T *g, int32_t val_min, int32_t val_max,
                             uint8_t tick_major)
{
    int32_t majors = (tick_major < 2) ? 2 :
                     (tick_major > UI_GAUGE_MAX_LABELS) ? UI_GAUGE_MAX_LABELS :
                     tick_major;
    g->label_count = (uint8_t)majors;

    int32_t span_x10 = GAUGE_ANGLE_X10_MAX - GAUGE_ANGLE_X10_MIN;
    int32_t i;
    for (i = 0; i < UI_GAUGE_MAX_LABELS; i++) {
        if (g->labels[i] == NULL) {
            continue;
        }
        if (i >= majors) {
            lv_obj_add_flag(g->labels[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        int32_t a_x10 = GAUGE_ANGLE_X10_MIN +
                        (int32_t)(((int64_t)i * span_x10) / (majors - 1));
        int16_t a_deg = (int16_t)(a_x10 / 10);
        int32_t s = lv_trigo_sin(a_deg);
        int32_t c = lv_trigo_sin((int16_t)(a_deg + 90));
        int32_t lx = (GAUGE_LBL_RADIUS * c) >> 15;
        int32_t ly = (GAUGE_LBL_RADIUS * s) >> 15;

        int32_t v = val_min +
                    (int32_t)(((int64_t)i * (val_max - val_min)) / (majors - 1));

        char buf[12];
        lv_snprintf(buf, sizeof(buf), "%" LV_PRId32, v);
        lv_label_set_text(g->labels[i], buf);
        lv_obj_align(g->labels[i], LV_ALIGN_CENTER, lx, ly);
        lv_obj_clear_flag(g->labels[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */
/**
 * @brief Create a circular gauge.
 * @param[in,out] g caller-owned descriptor (must be cleared)
 * @param[in] parent LVGL parent (the screen itself)
 * @param[in] title gauge title (e.g. "WATER TEMP")
 * @param[in] unit unit string (e.g. "°C")
 * @param[in] val_min lower bound of the dial range
 * @param[in] val_max upper bound of the dial range
 * @param[in] tick_major number of major ticks
 * @return OPRT_OK on success
 */
OPERATE_RET ui_gauge_create(UI_GAUGE_T *g, lv_obj_t *parent,
                            const char *title, const char *unit,
                            int32_t val_min, int32_t val_max,
                            uint8_t tick_major)
{
    if (g == NULL || parent == NULL || val_max <= val_min) {
        return OPRT_INVALID_PARM;
    }
    memset(g, 0, sizeof(*g));
    g->val_min = val_min;
    g->val_max = val_max;
    g->val_curr = val_min;
    g->needle_angle_x10  = GAUGE_ANGLE_X10_MIN;
    g->needle_target_x10 = GAUGE_ANGLE_X10_MIN;
    g->sweep_running     = FALSE;
    g->needle_visible    = FALSE;  /* gate the drawer until ui_gauge_sweep() */
    g->data_valid        = FALSE;

    /* Root container fills the screen */
    g->root = lv_obj_create(parent);
    lv_obj_remove_style_all(g->root);
    lv_obj_set_size(g->root, APP_LCD_WIDTH, APP_LCD_HEIGHT);
    lv_obj_center(g->root);
    lv_obj_set_style_bg_color(g->root, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(g->root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g->root, LV_OBJ_FLAG_SCROLLABLE);

    /* Decorative outer arc */
    g->arc = lv_arc_create(g->root);
    lv_obj_set_size(g->arc, GAUGE_OUTER_R * 2, GAUGE_OUTER_R * 2);
    lv_obj_center(g->arc);
    lv_obj_remove_style(g->arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(g->arc, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_bg_angles(g->arc, GAUGE_ANGLE_ROTATION,
                          GAUGE_ANGLE_ROTATION + GAUGE_ANGLE_RANGE);
    lv_arc_set_value(g->arc, 0);
    lv_arc_set_range(g->arc, 0, 100);
    lv_obj_set_style_arc_color(g->arc, UI_COLOR_ARC, LV_PART_MAIN);
    lv_obj_set_style_arc_color(g->arc, UI_COLOR_ARC, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(g->arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g->arc, 6, LV_PART_INDICATOR);

    /* Tick scale: ticks only. Built-in labels are disabled and replaced
     * by __labels_create_all() / __labels_apply() below — see those
     * function comments for why. */
    g->scale = lv_scale_create(g->root);
    lv_obj_set_size(g->scale, GAUGE_OUTER_R * 2 - 8, GAUGE_OUTER_R * 2 - 8);
    lv_obj_center(g->scale);
    lv_scale_set_mode(g->scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_angle_range(g->scale, GAUGE_ANGLE_RANGE);
    lv_scale_set_rotation(g->scale, GAUGE_ANGLE_ROTATION);
    lv_scale_set_range(g->scale, val_min, val_max);
    __scale_apply_ticks(g->scale, tick_major);
    lv_scale_set_label_show(g->scale, false);
    lv_obj_set_style_line_color(g->scale, UI_COLOR_TICK_DIM, LV_PART_ITEMS);
    lv_obj_set_style_line_width(g->scale, 2, LV_PART_ITEMS);
    lv_obj_set_style_length(g->scale, GAUGE_TICK_LEN_MINOR, LV_PART_ITEMS);
    lv_obj_set_style_line_color(g->scale, UI_COLOR_TICK, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(g->scale, 4, LV_PART_INDICATOR);
    lv_obj_set_style_length(g->scale, GAUGE_TICK_LEN_MAJOR, LV_PART_INDICATOR);

    /* Manually-placed dial digits (replaces the scale's built-in labels).
     * v1.8: pre-create the MAX number of label slots once and just rewrite
     * them on subsequent set_def() calls, see __labels_create_all() docs. */
    __labels_create_all(g);
    __labels_apply(g, val_min, val_max, tick_major);

    /* Needle: transparent overlay obj that owns LV_EVENT_DRAW_MAIN.
     * Box is centred on the screen and just large enough to contain
     * the rotated needle. Created HIDDEN — see file header
     * "Visibility lifecycle" for the why. */
    g->needle = lv_obj_create(g->root);
    lv_obj_remove_style_all(g->needle);
    lv_obj_set_size(g->needle, GAUGE_NEEDLE_BOX, GAUGE_NEEDLE_BOX);
    lv_obj_center(g->needle);
    lv_obj_clear_flag(g->needle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g->needle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g->needle, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_user_data(g->needle, g);
    lv_obj_add_event_cb(g->needle, __needle_draw_event_cb,
                        LV_EVENT_DRAW_MAIN, NULL);

    /* Layered metallic hub — six stacked filled circles. They sit ABOVE
     * the needle in the z-order so the needle's flat back is hidden
     * under the inner face. The radii decrease ~4 px per layer so each
     * layer is a 4-px-wide annulus visible as a distinct ring. The two
     * darkest greys sandwich a brighter mid grey to simulate a domed
     * metal cap with subtle specular hi-light. */
    struct __HUB_LAYER_S {
        lv_obj_t  **out;
        int32_t     radius;
        lv_color_t  color;
    };
    struct __HUB_LAYER_S hub_layers[] = {
        { &g->hub_l0, GAUGE_HUB_R_RIM,  UI_COLOR_PRIMARY }, /* red rim */
        { &g->hub_l1, GAUGE_HUB_R_DIM1, GAUGE_HUB_C_DIM1 },
        { &g->hub_l2, GAUGE_HUB_R_DIM2, GAUGE_HUB_C_DIM2 },
        { &g->hub_l3, GAUGE_HUB_R_MID,  GAUGE_HUB_C_MID  },
        { &g->hub_l4, GAUGE_HUB_R_HI,   GAUGE_HUB_C_HI   }, /* hi-light */
        { &g->hub_l5, GAUGE_HUB_R_FACE, UI_COLOR_HUB     }, /* near-black */
    };
    size_t hub_i;
    for (hub_i = 0; hub_i < sizeof(hub_layers) / sizeof(hub_layers[0]); hub_i++) {
        lv_obj_t *layer = lv_obj_create(g->root);
        lv_obj_remove_style_all(layer);
        lv_obj_set_size(layer,
                        hub_layers[hub_i].radius * 2,
                        hub_layers[hub_i].radius * 2);
        lv_obj_center(layer);
        lv_obj_set_style_radius(layer, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(layer, hub_layers[hub_i].color, 0);
        lv_obj_set_style_bg_opa(layer, LV_OPA_COVER, 0);
        lv_obj_clear_flag(layer, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(layer, LV_OBJ_FLAG_CLICKABLE);
        *hub_layers[hub_i].out = layer;
    }

    /* Title (e.g. VOLT / BAT / BOOST). v1.7 bumps the size from
     * montserrat_22 to montserrat_28 (+27 % type) AND drops the BOTTOM
     * offset from -195 to -150 (= 45 px DOWN, in the 40-60 px range the
     * user asked for). At -195 with the montserrat_22 line-box the
     * title top sat at y≈243 — INSIDE the Ø60 hub rim (rim bottom at
     * y=263). Now the title's top is at y≈280, leaving ~17 px clearance
     * to the hub's bottom and still ~7 px above the 48-pt value
     * readout below. The whole text stack (title / value / unit) stays
     * in the lower hemisphere, just better spaced. */
    g->label_title = lv_label_create(g->root);
    lv_label_set_text(g->label_title, title ? title : "");
    lv_obj_set_style_text_color(g->label_title, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(g->label_title, &lv_font_montserrat_28, 0);
    lv_obj_align(g->label_title, LV_ALIGN_BOTTOM_MID, 0, -150);

    /* Live readout — moved DOWN by 47 px (≈ 10 % of 466 px panel).
     *
     * Placeholder uses ASCII "--" (two hyphens) rather than U+2014
     * EM-DASH because `lv_font_montserrat_48` ships with only the
     * 0x20..0x7F range and would render U+2014 as a tofu box during
     * the boot sweep — the user's "自检的时候，值是个方块" report. */
    g->label_value = lv_label_create(g->root);
    lv_label_set_text(g->label_value, "--");
    lv_obj_set_style_text_color(g->label_value, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(g->label_value, &lv_font_montserrat_48, 0);
    lv_obj_align(g->label_value, LV_ALIGN_BOTTOM_MID, 0, -83);

    /* Unit — moved DOWN by 47 px to keep the value/unit pair together. */
    g->label_unit = lv_label_create(g->root);
    lv_label_set_text(g->label_unit, unit ? unit : "");
    lv_obj_set_style_text_color(g->label_unit, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(g->label_unit, &lv_font_montserrat_22, 0);
    lv_obj_align(g->label_unit, LV_ALIGN_BOTTOM_MID, 0, -41);

    /* Persistent tracker — runs forever, paused by the sweep_running flag. */
    g->track_timer = lv_timer_create(__track_timer_cb,
                                     GAUGE_TRACK_PERIOD_MS, g);

    return OPRT_OK;
}

/**
 * @brief Reconfigure an existing gauge in-place.
 * @param[in,out] g gauge
 * @param[in] title gauge title (NULL leaves unchanged)
 * @param[in] unit unit string (NULL leaves unchanged)
 * @param[in] val_min lower bound of the dial range
 * @param[in] val_max upper bound of the dial range
 * @param[in] tick_major number of major ticks
 * @return OPRT_OK on success
 */
OPERATE_RET ui_gauge_set_def(UI_GAUGE_T *g,
                             const char *title, const char *unit,
                             int32_t val_min, int32_t val_max,
                             uint8_t tick_major)
{
    if (g == NULL || g->scale == NULL || val_max <= val_min) {
        return OPRT_INVALID_PARM;
    }
    /* Cancel any in-flight sweep before changing the range. __sweep_finish
     * tears down the timer, drops the anchor, restores LVGL refresh to
     * BASE, and re-seeds the dirty ring — exactly the cleanup needed
     * when KEY-cycle interrupts a sweep. Branch only when actually
     * sweeping so we don't churn refresh-period or full-needle
     * invalidate on plain range-changes during steady-state. */
    if (g->sweep_running) {
        __sweep_finish(g);
    }

    g->val_min = val_min;
    g->val_max = val_max;
    g->val_curr = val_min;
    /* IMPORTANT: do NOT reset data_valid here. KEY-cycling gauges
     * mid-flight should keep the existing needle visible and let the
     * tracker glide smoothly from its current angle to the next
     * gauge's live value. Resetting data_valid would cause the next
     * set_value() to teleport, which the user explicitly disliked. */
    /* Keep the visible angle where it is so the user doesn't see a snap when
     * cycling gauges. The next set_value call will update the target and the
     * tracker will glide smoothly from here to the new live position. */
    g->needle_target_x10 = g->needle_angle_x10;

    lv_scale_set_range(g->scale, val_min, val_max);
    __scale_apply_ticks(g->scale, tick_major);
    /* v1.8 — reuse the pre-created label slots, no alloc/free here. */
    __labels_apply(g, val_min, val_max, tick_major);

    if (title && g->label_title) {
        lv_label_set_text(g->label_title, title);
    }
    if (unit && g->label_unit) {
        lv_label_set_text(g->label_unit, unit);
    }
    if (g->label_value) {
        lv_label_set_text(g->label_value, "--");
    }
    /* Full redraw + sync the dirty-area ring so the first tracker tick
     * after a gauge swap doesn't accidentally invalidate a stale area. */
    lv_obj_invalidate(g->needle);
    {
        lv_area_t fresh;
        __needle_compute_aabb(g, g->needle_angle_x10, &fresh);
        __needle_dirty_ring_seed(g, &fresh);
    }
    return OPRT_OK;
}

/**
 * @brief Update the needle target. The persistent tracker glides toward
 *        it at 200 Hz; calling this every refresh tick is safe.
 *
 *  v1.6 behaviour change: the FIRST call after create()/sweep() does NOT
 *  snap. We just update needle_target_x10 and let the tracker glide
 *  from the resting MIN angle — so the user watches the needle slide
 *  off the bottom-left into the live value, which is the "slow intro"
 *  they asked for in problem 3 of the latest feedback.
 *
 *  The data_valid flag still flips on the first call so callers (e.g.
 *  ui_gauge_set_def during gauge cycling) can tell whether a meaningful
 *  reading has been delivered.
 *
 * @param[in,out] g gauge
 * @param[in] value target value (clamped to [val_min, val_max])
 * @param[in] duration_ms 0 = snap immediately. > 0 is a hint and is
 *                        currently ignored — the tracker dictates motion.
 * @return none
 */
void ui_gauge_set_value(UI_GAUGE_T *g, int32_t value, uint32_t duration_ms)
{
    if (g == NULL || g->needle == NULL) {
        return;
    }
    int32_t target = __clamp(value, g->val_min, g->val_max);
    int32_t target_a = __value_to_angle_x10(g, target);
    g->val_curr = target;
    g->needle_target_x10 = target_a;

    if (!g->data_valid) {
        g->data_valid = TRUE;
        /* No snap. The tracker is already running (sweep_running = FALSE
         * by now since the boot sweep completes before the first sample
         * arrives) and will glide needle_angle_x10 from MIN toward
         * target_a at the velocity-capped EMA rate. */
        return;
    }

    if (duration_ms == 0) {
        if (g->needle_angle_x10 != target_a) {
            g->needle_angle_x10 = target_a;
            __needle_invalidate_swept(g);
        }
    }
    /* duration_ms > 0: tracker handles the glide. */
}

/**
 * @brief Sweep needle min->max->min for the boot animation.
 * @param[in,out] g gauge
 * @param[in] duration_ms total sweep duration (one full out+back cycle)
 * @return none
 */
void ui_gauge_sweep(UI_GAUGE_T *g, uint32_t duration_ms)
{
    if (g == NULL || g->needle == NULL) {
        return;
    }
    if (duration_ms < 200) {
        duration_ms = 200;
    }

    /* Cancel any prior sweep timer (defensive — set_def usually does this
     * via __sweep_finish, but the user may legitimately call sweep()
     * twice back-to-back during init reordering). */
    if (g->sweep_timer != NULL) {
        lv_timer_delete(g->sweep_timer);
        g->sweep_timer = NULL;
    }

    g->sweep_running     = TRUE;
    g->sweep_start_ms    = lv_tick_get();
    g->sweep_total_ms    = duration_ms;
    g->needle_angle_x10  = GAUGE_ANGLE_X10_MIN;
    g->needle_target_x10 = GAUGE_ANGLE_X10_MIN;
    /* Flip the render-time visibility gate so the drawer starts painting
     * the needle. From this point on it stays TRUE for the gauge's
     * lifetime — after the sweep ends the needle parks at MIN, and the
     * first set_value() glides it to the live value (no snap, no hide). */
    g->needle_visible = TRUE;
    /* Defensive: clear the LVGL hidden flag in case anything toggled it. */
    if (lv_obj_has_flag(g->needle, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(g->needle, LV_OBJ_FLAG_HIDDEN);
    }
    /* One-shot full needle redraw at sweep start so the very first
     * frame is guaranteed clean on BOTH framebuffers (the dirty-area
     * ring may carry stale slots from a prior tracker run, or be
     * uninitialised on first boot — neither of which we want LVGL to
     * paint OVER). Per-frame fine-grained AABB invalidation kicks in
     * on every subsequent sweep tick (see __sweep_timer_cb), so this
     * heavy 380×380 fill happens exactly once.
     *
     * We also seed all 4 ring slots with the MIN AABB so the first
     * tick's __needle_invalidate_swept() can build its swept-band
     * union with no "phantom" empty rectangles in the trail. */
    /* Force-resolve LVGL's deferred layout for the gauge subtree BEFORE
     * we read needle obj coords. LVGL v9 only computes obj->coords
     * during the refresh phase of lv_timer_handler() — anim/timer
     * callbacks (which is where __needle_invalidate_swept reads
     * coords) run BEFORE that, so without this forced update, both a
     * sweep_start cache AND a first-frame lazy build would land on
     * stale (≈0) coords and the resulting AABB would scrub the wrong
     * pixels. lv_obj_update_layout walks the subtree synchronously
     * and writes final coords, making lv_obj_get_coords reliable
     * from this line onwards. */
    lv_obj_update_layout(g->root);

    lv_obj_invalidate(g->needle);
    {
        lv_area_t initial;
        __needle_compute_aabb(g, g->needle_angle_x10, &initial);
        __needle_dirty_ring_seed(g, &initial);

        /* Cache the MIN-pose AABB once. Layout is now resolved
         * (lv_obj_update_layout above), so coords are reliable.
         * __needle_invalidate_swept replays this cache on every
         * sweep frame. Cleared in __sweep_finish() (which runs on
         * both natural-finish via __sweep_timer_cb and KEY-cycle
         * abort via ui_gauge_set_def). */
        g->sweep_anchor_dirty = initial;
        g->sweep_anchor_armed = TRUE;
    }

    /* Push LVGL refresh from its baseline (100 Hz default on T5AI) to
     * 125 Hz (8 ms) for the sweep duration — drops mid-arc per-frame
     * angle delta from ~6.15° to ~4.92°, which is the "more
     * interpolation between frames" the user is asking for. Saved
     * snapshot is restored in __sweep_finish() (both natural-finish
     * and KEY-cycle abort paths). See GAUGE_REFR_PERIOD_TURBO_MS
     * docs for why 8 ms specifically. */
    g->sweep_saved_refr = __display_refr_period_get();
    __display_refr_period_set(GAUGE_REFR_PERIOD_TURBO_MS);

    /* Spawn the 5 ms sweep driver. LVGL refresh @ 125 Hz (8 ms)
     * paired with this 200 Hz timer means each refresh sees ~1.6
     * angle updates — consecutive AABBs heavily overlap and the
     * SW rasterizer paints a single small dirty region per refresh,
     * matching the steady-state tracker's behaviour. Timer
     * self-tears-down via __sweep_finish() when elapsed >=
     * sweep_total_ms. */
    g->sweep_timer = lv_timer_create(__sweep_timer_cb,
                                     GAUGE_TRACK_PERIOD_MS, g);
}

/**
 * @brief Set the centre value text.
 * @param[in,out] g gauge
 * @param[in] text text to display (NULL or empty becomes "—")
 * @return none
 */
void ui_gauge_set_value_text(UI_GAUGE_T *g, const char *text)
{
    if (g == NULL || g->label_value == NULL) {
        return;
    }
    lv_label_set_text(g->label_value, (text && *text) ? text : "--");
}

/**
 * @brief Update the title label.
 * @param[in,out] g gauge
 * @param[in] title null-terminated string
 * @return none
 */
void ui_gauge_set_title(UI_GAUGE_T *g, const char *title)
{
    if (g == NULL || g->label_title == NULL) {
        return;
    }
    lv_label_set_text(g->label_title, title ? title : "");
}

/**
 * @brief Show/hide the gauge root.
 * @param[in,out] g gauge
 * @param[in] visible TRUE to show
 * @return none
 */
void ui_gauge_set_visible(UI_GAUGE_T *g, BOOL_T visible)
{
    if (g == NULL || g->root == NULL) {
        return;
    }
    if (visible) {
        lv_obj_clear_flag(g->root, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g->root, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief Tear down the gauge.
 * @param[in,out] g gauge
 * @return none
 */
void ui_gauge_destroy(UI_GAUGE_T *g)
{
    if (g == NULL) {
        return;
    }
    if (g->track_timer) {
        lv_timer_delete(g->track_timer);
        g->track_timer = NULL;
    }
    if (g->sweep_timer) {
        lv_timer_delete(g->sweep_timer);
        g->sweep_timer = NULL;
    }
    /* Children get cleaned up when g->root is deleted; we just null the
     * label pointers explicitly so any further access is defensive. */
    int i;
    for (i = 0; i < UI_GAUGE_MAX_LABELS; i++) {
        g->labels[i] = NULL;
    }
    g->label_count = 0;
    if (g->root) {
        lv_obj_delete(g->root);
    }
    memset(g, 0, sizeof(*g));
}
