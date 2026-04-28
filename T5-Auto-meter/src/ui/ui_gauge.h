/**
 * @file ui_gauge.h
 * @brief Reusable circular pointer gauge widget for the 466x466 round AMOLED.
 * @version 1.8
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 *
 * Layout (radius numbers are pixels; centre is the screen centre):
 *   - Outer decorative arc       : r = 220
 *   - Major/minor ticks          : r = 198..216 (inner end at 198)
 *   - Numeric tick labels (32pt) : r = 150  (manually placed lv_label
 *                                            objects; lv_scale's hardcoded
 *                                            15 px label-gap forces overlap
 *                                            with the tick line at large
 *                                            font sizes, so we draw our own.)
 *   - Needle pivot               : centre (233, 233)
 *   - Needle silhouette          : SINGLE LINE design (v1.8) — uniform W=14
 *                                  from BACK behind the pivot to TIP, with
 *                                  round_end=1 producing a half-disc cap of
 *                                  radius W/2 = 7 at the tip. ONE primitive,
 *                                  no cutters, no overlapping triangles.
 *                                  The user explicitly OK'd dropping the
 *                                  taper because v1.6/v1.7 multi-primitive
 *                                  approaches kept producing residual
 *                                  artefacts (internal black lines or
 *                                  black-then-red afterimages) — a single
 *                                  primitive trivially has no interior
 *                                  boundaries and no AABB-invalidation
 *                                  edge cases.
 *   - Hub                        : 6-layer "metallic dome" cap stacked
 *                                  centre-aligned (radii 30 / 26 / 22 /
 *                                  18 / 14 / 8). Outermost is the brand
 *                                  red rim, then a graded grey ramp
 *                                  (#1A1A1A → #353535 → #505050 → #7A7A7A)
 *                                  with the inner #7A as a soft specular
 *                                  highlight, finally the near-black face
 *                                  #202020 covers the needle's flat back.
 *   - Title / value / unit       : ALL stacked in the lower hemisphere;
 *                                  title at montserrat_28 / offset -150,
 *                                  value at montserrat_48 / offset -83,
 *                                  unit at montserrat_22 / offset -41.
 *
 * Lifecycle of the needle visibility
 * ----------------------------------
 *  The needle is drawn by a custom event handler that consults
 *  g->needle_visible. While that flag is FALSE the handler returns without
 *  painting, regardless of LVGL's hidden flag, so a render done between
 *  create() and sweep() cannot silhouette a stale MIN-angle needle on the
 *  boot screen.
 *  After ui_gauge_sweep() finishes the needle stays VISIBLE at MIN
 *  (= 135°, bottom-left) — that's the resting pose. v1.8 also forces a
 *  full-needle invalidate at sweep END so any partial AABB residual from
 *  the last sweep frame is cleanly repainted at MIN (this fixes the
 *  "0刻度残留三角块" bug). The first call to ui_gauge_set_value() simply
 *  updates needle_target_x10; the persistent tracker timer then GLIDES
 *  the needle from MIN to the live value at the same speed as any
 *  subsequent update.
 *
 * Animation is a persistent tracker timer at 200 Hz (period = 5 ms),
 * 2× LVGL's 100 Hz refresh — every refresh consumes the latest two
 * tracker steps' worth of dirty regions, so visible motion always
 * tracks at the panel rate even when the data refresh tick (30 Hz)
 * drops a target update.
 *
 * Performance: the tracker invalidates the union of the LAST FOUR
 * prev_dirty AABBs plus the new one (~30 kpx total per frame, ~5 small
 * rectangles, with a 16 px AA-halo PAD so the round_end half-disc
 * stays strictly inside even at peak sweep velocity). Total fill rate
 * during the boot sweep is well under the SW rasterizer's headroom,
 * so the visible sweep tracks at the full LVGL refresh rate (no frame
 * drops, no residual ghost paint).
 *
 * Heap behaviour
 * --------------
 *  v1.8 fixes a slow performance degradation under repeated KEY-cycling.
 *  Earlier versions called __labels_clear / __labels_build inside
 *  ui_gauge_set_def, which delete-and-recreate up to 12 lv_label objects
 *  on every gauge swap. LVGL's label create path allocates style transition
 *  records that the immediate delete cannot fully reclaim back to the heap
 *  arena (the freed blocks become small free-list entries), so cycling
 *  N times leaks ~N × (label-overhead) of fragmented heap until the LVGL
 *  task starts thrashing the allocator and frame rate falls off a cliff.
 *  v1.8 pre-creates UI_GAUGE_MAX_LABELS labels once in ui_gauge_create()
 *  (kept hidden) and ui_gauge_set_def() now JUST UPDATES text/position/
 *  visibility — zero per-cycle alloc, zero per-cycle free, infinite-cycle
 *  stable.
 */
#ifndef __UI_GAUGE_H__
#define __UI_GAUGE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include "tuya_cloud_types.h"

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
#define UI_GAUGE_MAX_LABELS 12

typedef struct {
    lv_obj_t   *root;
    lv_obj_t   *arc;
    lv_obj_t   *scale;
    lv_obj_t   *needle;        /**< transparent overlay; the needle is rendered
                                    by __needle_draw_event_cb (single line) */

    /* Layered "metallic dome" cap — 6 concentric circles stacked in z-order.
     * hub_l0 sits above the needle's flat back. The radii decrease from
     * 30 px (red rim) down to 8 px (near-black eye) so the user sees a
     * clear screwed-down cap with depth. */
    lv_obj_t   *hub_l0;        /**< r = 30 — brand red rim */
    lv_obj_t   *hub_l1;        /**< r = 26 — very dark grey #1A1A1A */
    lv_obj_t   *hub_l2;        /**< r = 22 — dark grey       #353535 */
    lv_obj_t   *hub_l3;        /**< r = 18 — mid grey        #505050 */
    lv_obj_t   *hub_l4;        /**< r = 14 — light specular  #7A7A7A */
    lv_obj_t   *hub_l5;        /**< r =  8 — near-black eye  #202020 */

    lv_obj_t   *label_title;
    lv_obj_t   *label_value;
    lv_obj_t   *label_unit;

    /* Manually placed dial digits — bypass lv_scale's hardcoded 15 px
     * label-gap so we can keep the digits well clear of the tick marks
     * even with a 32 pt font. labels[0..label_count-1] are CCW from the
     * dial minimum to maximum. */
    lv_obj_t   *labels[UI_GAUGE_MAX_LABELS];
    uint8_t     label_count;

    int32_t     val_min;
    int32_t     val_max;
    int32_t     val_curr;         /**< latest displayed value (raw user units) */
    int32_t     needle_angle_x10; /**< current needle angle in 0.1° (LVGL trigo) */
    int32_t     needle_target_x10;/**< desired needle angle in 0.1° (set by API) */

    /* Dirty AABB ring buffer covering the LAST FOUR frames. The LVGL
     * stack on T5AI is configured with CONFIG_ENABLE_LVGL_DUAL_DISP_BUFF=y
     * (full-screen alternate framebuffers).
     *
     * v1.8  used a 2-slot ring on the assumption "buffer alternation is
     *       strict, so 2 prev frames cover both buffers". Reality on
     *       T5AI: when the OBD task and IMU task both spike (BLE
     *       characteristic-write storm + IMU sample), LVGL refreshes
     *       can be skewed enough that the same buffer is rendered
     *       twice in a row, or the alternation slips. A 2-prev ring
     *       then leaves frame N-3 stale on the back buffer at frame
     *       N+1 — visible as the "残留" the user reported.
     *
     * v1.8.1 used `lv_obj_invalidate(g->needle)` (full 380×380 obj area)
     *       per sweep frame to brute-force the issue — guaranteed
     *       clean, but ~144 kpx of fill per frame stalled the SW
     *       rasterizer below the LVGL refresh budget so the user
     *       saw "动画非常卡，帧率需要翻4倍".
     *
     * v1.8.2 (current) — 4-slot ring. Each tracker / sweep frame issues
     *       lv_obj_invalidate_area for prev[0..3] PLUS the new AABB
     *       (5 small rectangles, ~30 kpx total fill). 4 frames of
     *       history covers buffer-alternation drift up to 30 ms, which
     *       comfortably absorbs all the load skew we see in practice.
     *       Slot rotation is round-robin (prev_dirty_idx mod 4).
     *
     * The PAD around each AABB was widened in step (v1.8.2 PAD = 16
     * vs v1.8 PAD = 10) so the per-frame swept band strictly covers
     * the next-frame pose's AA halo even at peak sweep velocity
     * (~7°/frame at the ease-in-out apex). */
    lv_area_t   prev_dirty[4];
    uint8_t     prev_dirty_idx;   /**< 0..3; next slot to overwrite */

    /* Boot-sweep MIN-pose anchor.
     * --------------------------
     * Problem: dual-buffer alternation drift can orphan the dial-MIN
     * pose's needle silhouette on whichever framebuffer skipped a
     * paint cycle (the user-reported "WATER TEMP 40 下面的区块有
     * 指针残留"). The 4-deep prev_dirty ring covers it for the first
     * ~40 ms but if alternation slips badly, MIN pixels can survive.
     *
     * Solution: every sweep frame, ALSO invalidate a cached MIN-pose
     * AABB so it gets repainted on whichever buffer LVGL renders
     * next. Cost: ~6 kpx fill per frame, same order as one ring slot.
     *
     * The cache is computed in ui_gauge_sweep() AFTER an explicit
     * lv_obj_update_layout() call — LVGL v9 defers layout to the
     * refresh phase, so reading needle obj coords before that returns
     * stale (≈0) values, and a lazy-build inside an anim callback
     * doesn't help (the anim cb still runs BEFORE refresh in the same
     * lv_timer_handler() pass). Force-layout is the only reliable
     * way to get correct cx/cy at sweep_start.
     *
     *   sweep_anchor_armed  : TRUE between ui_gauge_sweep() start and
     *                         __sweep_finish() (natural finish path
     *                         from __sweep_timer_cb, OR KEY-cycle
     *                         abort path from ui_gauge_set_def).
     *                         Steady-state tracker keeps it FALSE so
     *                         the per-frame cost is zero. */
    lv_area_t   sweep_anchor_dirty;
    BOOL_T      sweep_anchor_armed;

    lv_timer_t *track_timer;      /**< 200 Hz tracker that glides angle to target */
    lv_timer_t *sweep_timer;      /**< 200 Hz boot-sweep driver (replaces lv_anim
                                       so sweep walks the same low-jerk dirty path
                                       as the steady-state tracker — see file
                                       header §"Boot sweep"). NULL outside the
                                       sweep window. */
    uint32_t    sweep_start_ms;   /**< lv_tick_get() at sweep launch */
    uint32_t    sweep_total_ms;   /**< full MIN→MAX→MIN duration */
    uint32_t    sweep_saved_refr; /**< LVGL refresh period (ms) before
                                       sweep pushed it to TURBO; restored
                                       in __sweep_finish */
    BOOL_T      sweep_running;    /**< pause tracker while the sweep timer runs */
    BOOL_T      needle_visible;   /**< drawer-level paint gate. FALSE between
                                       create() and the first ui_gauge_sweep();
                                       TRUE thereafter. The needle obj's HIDDEN
                                       flag is too late — by the time we'd
                                       toggle it the screen may have already
                                       drawn one frame at MIN. */
    BOOL_T      data_valid;       /**< set by the first ui_gauge_set_value();
                                       used by ui_gauge_set_def() to know
                                       whether to (re)snap the needle when the
                                       gauge range changes mid-flight. */
} UI_GAUGE_T;

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */
/**
 * @brief Create a gauge filling the parent screen.
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
                            uint8_t tick_major);

/**
 * @brief Reconfigure an existing gauge (range / title / unit / ticks)
 *        without destroying the LVGL widgets. Avoids the destroy+create
 *        thrash on KEY-press gauge cycling.
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
                             uint8_t tick_major);

/**
 * @brief Update the needle target. The persistent tracker glides toward
 *        it at 200 Hz with a velocity-capped exponential filter, so
 *        calling this at the UI refresh tick (33 ms ≈ 30 Hz) updates
 *        the target ~6 times per τ — the EMA smoothly traces a low-
 *        pass version of the data sequence between consecutive ticks
 *        instead of "stepping" each tick (the user's "在两次数据中间需要
 *        插值" feedback). The first call after create()/sweep() does
 *        NOT snap — the tracker glides from the resting MIN angle to
 *        the live value at the same speed as any subsequent update,
 *        giving the user the slow "needle sliding off the minimum"
 *        intro they asked for.
 *
 * @param[in,out] g gauge
 * @param[in] value target value (clamped to [val_min, val_max])
 * @param[in] duration_ms 0 = snap immediately. > 0 is treated as a hint
 *                        and currently ignored (the tracker dictates the
 *                        actual slew speed for visual consistency).
 * @return none
 */
void ui_gauge_set_value(UI_GAUGE_T *g, int32_t value, uint32_t duration_ms);

/**
 * @brief Sweep the needle from min to max and back to min for the boot animation.
 * @param[in,out] g gauge
 * @param[in] duration_ms total sweep duration
 * @return none
 */
void ui_gauge_sweep(UI_GAUGE_T *g, uint32_t duration_ms);

/**
 * @brief Set the readable value text (independent of the needle).
 * @param[in,out] g gauge
 * @param[in] text text to display (may be NULL or empty for "—")
 * @return none
 */
void ui_gauge_set_value_text(UI_GAUGE_T *g, const char *text);

/**
 * @brief Set the title displayed below the hub.
 * @param[in,out] g gauge
 * @param[in] title null-terminated string
 * @return none
 */
void ui_gauge_set_title(UI_GAUGE_T *g, const char *title);

/**
 * @brief Show/hide the gauge.
 * @param[in,out] g gauge
 * @param[in] visible TRUE to show
 * @return none
 */
void ui_gauge_set_visible(UI_GAUGE_T *g, BOOL_T visible);

/**
 * @brief Tear down the LVGL objects and zero descriptor.
 * @param[in,out] g gauge
 * @return none
 */
void ui_gauge_destroy(UI_GAUGE_T *g);

#ifdef __cplusplus
}
#endif

#endif /* __UI_GAUGE_H__ */
