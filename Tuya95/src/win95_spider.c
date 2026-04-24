/**
 * @file win95_spider.c
 * @brief Win95 Spider Solitaire — 1 suit, 10 columns, 104 cards.
 *
 * Rules:
 *  - 104 cards (2 decks, spades only in 1-suit mode)
 *  - 10 tableau columns: cols 0-3 get 6 cards each, cols 4-9 get 5 each (54 total)
 *  - 50 remaining cards in the stock (5 deals of 10)
 *  - Top card of each column is face-up; the rest are face-down
 *  - Move a sequence of face-up cards (descending ranks, same suit) onto a
 *    card one rank higher.  Moving to empty column is also allowed.
 *  - When a full K→A sequence (13 cards) forms, it auto-removes.
 *  - "Deal" button places one card face-up on each non-empty column from stock
 *    (or on all 10 if all are empty).
 *
 * Display:
 *  - Full 480×320 window
 *  - Card: 40×60 px.  Face-down: gray.  Face-up: white with rank/suit text.
 *  - Columns laid out with 4px gap between, top cards overlapping 14px each.
 *  - Click a face-up card to select it (and the movable sequence above it);
 *    click a destination card or column base to attempt the move.
 *  - Selected cards: highlighted (yellow tint)
 */
#include "win95_spider.h"
#include "win95_desktop.h"
#include "bios_simulator.h"
#include "tal_api.h"
#include "lv_vendor.h"

#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Constants
 * --------------------------------------------------------------------------- */
#define N_COLS      10
#define N_CARDS     104
#define RANK_MAX    13    /* A=1 … K=13 */

#define CARD_W      40
#define CARD_H      54
#define CARD_OVERLAP 14   /* vertical overlap for face-up cards */
#define CARD_BACK_OVERLAP 8  /* tighter overlap for face-down cards */
#define COL_X0      4     /* left edge of first column */
#define COL_Y0      38    /* top of tableau area (below toolbar) */
#define COL_GAP     4     /* horizontal gap between columns */
#define STOCK_X     (480 - 4 - CARD_W)  /* stock pile display position */
#define STOCK_Y     4

#define SP_W        BIOS_SCREEN_WIDTH
#define SP_H        BIOS_SCREEN_HEIGHT

/* ---------------------------------------------------------------------------
 * Card data
 * --------------------------------------------------------------------------- */
typedef struct {
    UINT8_T rank;      /* 1=A, 2–10, 11=J, 12=Q, 13=K */
    UINT8_T face_up;
    UINT8_T removed;   /* part of completed sequence */
} CARD_T;

/* ---------------------------------------------------------------------------
 * Column: stack of card indices */
typedef struct {
    UINT8_T cards[N_CARDS];   /* card indices */
    INT32_T n;                 /* number of cards in this column */
} COL_T;

/* ---------------------------------------------------------------------------
 * Stock pile */
typedef struct {
    UINT8_T cards[N_CARDS];
    INT32_T n;
} STOCK_T;

/* ---------------------------------------------------------------------------
 * Module context */
typedef struct {
    lv_obj_t  *screen;
    lv_obj_t  *score_lbl;
    lv_obj_t  *status_lbl;
    lv_obj_t  *stock_obj;    /* clickable area for stock */

    /* Game state */
    CARD_T     deck[N_CARDS];
    COL_T      cols[N_COLS];
    STOCK_T    stock;
    INT32_T    score;
    INT32_T    completed;    /* number of completed sets removed */
    UINT32_T   rand_seed;

    /* Selection state */
    INT32_T    sel_col;      /* -1 = no selection */
    INT32_T    sel_card_n;   /* how many cards selected (from top of col downward) */

    /* Completed sets overlay (8 possible, one per removed sequence) */
    lv_obj_t  *done_lbl;
} SPIDER_CTX_T;

STATIC SPIDER_CTX_T s_sp;

/* ---------------------------------------------------------------------------
 * Random */
STATIC UINT32_T __sp_rand(VOID_T)
{
    s_sp.rand_seed = s_sp.rand_seed * 1664525u + 1013904223u;
    return s_sp.rand_seed >> 1;
}

/* ---------------------------------------------------------------------------
 * Rank/suit helpers */
STATIC CONST CHAR_T *__rank_str(INT32_T r)
{
    STATIC CONST CHAR_T *s[] = {"?","A","2","3","4","5","6","7","8","9","10","J","Q","K"};
    if (r < 1 || r > 13) return "?";
    return s[r];
}

/* ---------------------------------------------------------------------------
 * Shuffle the 104 cards (1-suit = all spades, ranks 1-13 × 8) */
STATIC VOID_T __sp_shuffle(VOID_T)
{
    /* Init deck: ranks 1-13 repeated 8 times */
    for (INT32_T i = 0; i < N_CARDS; i++) {
        s_sp.deck[i].rank    = (UINT8_T)((i % RANK_MAX) + 1);
        s_sp.deck[i].face_up = 0;
        s_sp.deck[i].removed = 0;
    }
    /* Fisher-Yates shuffle */
    for (INT32_T i = N_CARDS - 1; i > 0; i--) {
        INT32_T j = (INT32_T)(__sp_rand() % (UINT32_T)(i + 1));
        CARD_T tmp = s_sp.deck[i];
        s_sp.deck[i] = s_sp.deck[j];
        s_sp.deck[j] = tmp;
    }
}

/* ---------------------------------------------------------------------------
 * Deal initial tableau + stock */
STATIC VOID_T __sp_deal_initial(VOID_T)
{
    __sp_shuffle();

    INT32_T card_idx = 0;
    /* Cols 0-3: 6 cards each; cols 4-9: 5 cards each */
    for (INT32_T c = 0; c < N_COLS; c++) {
        s_sp.cols[c].n = 0;
        INT32_T n = (c < 4) ? 6 : 5;
        for (INT32_T k = 0; k < n; k++) {
            s_sp.cols[c].cards[s_sp.cols[c].n++] = (UINT8_T)card_idx;
            card_idx++;
        }
        /* Flip top card face-up */
        INT32_T top = s_sp.cols[c].n - 1;
        s_sp.deck[s_sp.cols[c].cards[top]].face_up = 1;
    }
    /* Remaining 50 cards go to stock */
    s_sp.stock.n = 0;
    for (; card_idx < N_CARDS; card_idx++) {
        s_sp.stock.cards[s_sp.stock.n++] = (UINT8_T)card_idx;
    }
}

/* ---------------------------------------------------------------------------
 * Check for and remove completed K→A sequences from the top of a column */
STATIC VOID_T __sp_check_complete(INT32_T col)
{
    COL_T *c = &s_sp.cols[col];
    if (c->n < RANK_MAX) return;

    /* Find the topmost K in face-up cards */
    INT32_T k_pos = -1;
    for (INT32_T i = c->n - RANK_MAX; i <= c->n - RANK_MAX; i++) {
        if (i < 0) continue;
        if (!s_sp.deck[c->cards[i]].face_up) continue;
        if (s_sp.deck[c->cards[i]].rank != RANK_MAX) continue;
        /* Check K A→ full K downto A sequence */
        BOOL_T ok = TRUE;
        for (INT32_T j = 0; j < RANK_MAX; j++) {
            INT32_T ci = i + j;
            if (ci >= c->n) { ok = FALSE; break; }
            if (!s_sp.deck[c->cards[ci]].face_up) { ok = FALSE; break; }
            if (s_sp.deck[c->cards[ci]].rank != (UINT8_T)(RANK_MAX - j)) { ok = FALSE; break; }
        }
        if (ok) {
            k_pos = i;
            break;
        }
    }
    if (k_pos < 0) return;

    /* Remove RANK_MAX cards from position k_pos */
    for (INT32_T j = 0; j < RANK_MAX; j++) {
        s_sp.deck[c->cards[k_pos + j]].removed = 1;
    }
    /* Shift remaining cards down */
    INT32_T new_n = k_pos;
    c->n = new_n;

    /* Flip new top card face-up if needed */
    if (c->n > 0) {
        s_sp.deck[c->cards[c->n - 1]].face_up = 1;
    }

    s_sp.completed++;
    s_sp.score += 100;
}

/* ---------------------------------------------------------------------------
 * Can a sequence starting at card_start (length seq_len) in from_col
 * be moved to to_col? */
STATIC BOOL_T __sp_can_move(INT32_T from_col, INT32_T seq_bottom, INT32_T to_col)
{
    COL_T *from = &s_sp.cols[from_col];
    /* seq_bottom is the index of the lowest rank card in the sequence (top of move) */
    INT32_T rank_bottom = (INT32_T)s_sp.deck[from->cards[seq_bottom]].rank;

    COL_T *to = &s_sp.cols[to_col];
    if (to->n == 0) return TRUE;   /* empty column accepts anything */

    INT32_T rank_top_dest = (INT32_T)s_sp.deck[to->cards[to->n - 1]].rank;
    return (rank_top_dest == rank_bottom + 1) ? TRUE : FALSE;
}

/* ---------------------------------------------------------------------------
 * Find the bottom of the movable sequence at top of col.
 * Returns the index within col->cards[] of the deepest card in the sequence.
 * A "movable sequence" is a run of face-up cards with consecutively decreasing ranks. */
STATIC INT32_T __sp_seq_start(INT32_T col)
{
    COL_T *c = &s_sp.cols[col];
    if (c->n == 0) return -1;
    INT32_T start = c->n - 1;
    while (start > 0) {
        if (!s_sp.deck[c->cards[start - 1]].face_up) break;
        if ((INT32_T)s_sp.deck[c->cards[start - 1]].rank !=
            (INT32_T)s_sp.deck[c->cards[start]].rank + 1) break;
        start--;
    }
    return start;
}

/* ---------------------------------------------------------------------------
 * Execute a move: move cards from from_col[seq_start..n-1] to to_col */
STATIC VOID_T __sp_move(INT32_T from_col, INT32_T seq_start, INT32_T to_col)
{
    COL_T *from = &s_sp.cols[from_col];
    COL_T *to   = &s_sp.cols[to_col];

    INT32_T n_move = from->n - seq_start;
    for (INT32_T i = 0; i < n_move; i++) {
        to->cards[to->n++] = from->cards[seq_start + i];
    }
    from->n = seq_start;

    /* Flip new top of from column */
    if (from->n > 0 && !s_sp.deck[from->cards[from->n - 1]].face_up) {
        s_sp.deck[from->cards[from->n - 1]].face_up = 1;
        s_sp.score += 5;
    }

    s_sp.score -= 1;   /* small penalty per move */
    __sp_check_complete(to_col);
}

/* ---------------------------------------------------------------------------
 * Deal from stock: add one card to each column */
STATIC VOID_T __sp_deal_stock(VOID_T)
{
    if (s_sp.stock.n < N_COLS) return;
    for (INT32_T c = 0; c < N_COLS; c++) {
        UINT8_T ci = s_sp.stock.cards[--s_sp.stock.n];
        /* Flip previous top face-up if needed */
        if (s_sp.cols[c].n > 0) {
            /* ensure previous top stays face-up */
        }
        s_sp.deck[ci].face_up = 1;
        s_sp.cols[c].cards[s_sp.cols[c].n++] = ci;
        __sp_check_complete(c);
    }
}

/* ---------------------------------------------------------------------------
 * Render: paint one card object with rank/suit text */
STATIC VOID_T __paint_card_obj(lv_obj_t *obj, INT32_T rank, BOOL_T face_up, BOOL_T selected)
{
    if (!obj) return;
    lv_obj_clean(obj);

    UINT32_T bg;
    if (!face_up) {
        bg = 0x004488;   /* face-down: dark blue */
    } else if (selected) {
        bg = 0xFFFFAA;   /* selected: yellow tint */
    } else {
        bg = 0xFFFFFF;   /* face-up: white */
    }
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);

    if (!face_up) {
        /* Draw small pattern on back */
        lv_obj_t *lbl = lv_label_create(obj);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x336699), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
        lv_label_set_text(lbl, "##\n##");
        lv_obj_center(lbl);
        return;
    }

    /* Face-up: show rank + spade */
    CHAR_T buf[8];
    snprintf(buf, sizeof(buf), "%s\n%s", __rank_str(rank), "S");
    lv_obj_t *lbl = lv_label_create(obj);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
    lv_label_set_text(lbl, buf);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 2, 1);
}

/* ---------------------------------------------------------------------------
 * Rebuild all card widgets from game state */
STATIC VOID_T __sp_render(VOID_T);

/* ---------------------------------------------------------------------------
 * Click on a card/column */
typedef struct {
    INT32_T col;
    INT32_T card_in_col;   /* index within col->cards */
} CARD_LOC_T;

STATIC VOID_T __card_click_cb(lv_event_t *e)
{
    CARD_LOC_T *loc = (CARD_LOC_T *)lv_event_get_user_data(e);
    if (!loc) return;

    INT32_T col = loc->col;
    INT32_T pos = loc->card_in_col;

    if (s_sp.sel_col >= 0) {
        /* Second click: attempt move */
        INT32_T from = s_sp.sel_col;
        COL_T *fc = &s_sp.cols[from];
        INT32_T seq_start = __sp_seq_start(from);
        if (seq_start < 0) { s_sp.sel_col = -1; __sp_render(); return; }

        if (col != from && __sp_can_move(from, seq_start, col)) {
            __sp_move(from, seq_start, col);
            s_sp.sel_col = -1;
        } else {
            /* Re-select from the new column if it's a valid sequence start */
            if (s_sp.deck[s_sp.cols[col].cards[pos]].face_up) {
                s_sp.sel_col = col;
            } else {
                s_sp.sel_col = -1;
            }
        }
    } else {
        /* First click: select */
        if (s_sp.deck[s_sp.cols[col].cards[pos]].face_up) {
            s_sp.sel_col = col;
        }
    }

    /* Update score label */
    if (s_sp.score_lbl) {
        CHAR_T buf[32];
        snprintf(buf, sizeof(buf), "Score: %d  Sets: %d",
                 (int)s_sp.score, (int)s_sp.completed);
        lv_label_set_text(s_sp.score_lbl, buf);
    }

    if (s_sp.completed >= 8) {
        if (s_sp.status_lbl) lv_label_set_text(s_sp.status_lbl, "You Win!");
        s_sp.sel_col = -1;
    }

    __sp_render();
    lv_free(loc);   /* was allocated per render */
}

/* ---------------------------------------------------------------------------
 * Stock click */
STATIC VOID_T __stock_click_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (s_sp.stock.n < N_COLS) {
        if (s_sp.status_lbl) lv_label_set_text(s_sp.status_lbl, "Stock empty!");
        return;
    }
    s_sp.sel_col = -1;
    __sp_deal_stock();
    if (s_sp.score_lbl) {
        CHAR_T buf[32];
        snprintf(buf, sizeof(buf), "Score: %d  Sets: %d",
                 (int)s_sp.score, (int)s_sp.completed);
        lv_label_set_text(s_sp.score_lbl, buf);
    }
    __sp_render();
}

/* ---------------------------------------------------------------------------
 * Close */
STATIC VOID_T __sp_close_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (s_sp.screen) { lv_obj_delete(s_sp.screen); }
    memset(&s_sp, 0, sizeof(s_sp));
}

/* ---------------------------------------------------------------------------
 * Full render: remove old card objects and re-create them */
STATIC lv_obj_t *s_card_area = NULL;  /* child of screen for card widgets */

STATIC VOID_T __sp_render(VOID_T)
{
    if (!s_card_area) return;
    lv_obj_clean(s_card_area);

    INT32_T col_stride = (SP_W - COL_X0 - 4) / N_COLS;

    for (INT32_T c = 0; c < N_COLS; c++) {
        INT32_T cx = COL_X0 + c * col_stride;
        COL_T *col = &s_sp.cols[c];

        /* Empty column placeholder */
        lv_obj_t *placeholder = lv_obj_create(s_card_area);
        lv_obj_remove_style_all(placeholder);
        lv_obj_set_size(placeholder, CARD_W, CARD_H);
        lv_obj_set_pos(placeholder, cx, COL_Y0);
        lv_obj_set_style_border_color(placeholder, lv_color_hex(WIN95_COLOR_SHADOW), 0);
        lv_obj_set_style_border_width(placeholder, 1, 0);
        lv_obj_set_style_bg_opa(placeholder, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(placeholder, 2, 0);
        lv_obj_clear_flag(placeholder, LV_OBJ_FLAG_SCROLLABLE);
        /* Empty column is also clickable as a drop target */
        if (col->n == 0) {
            CARD_LOC_T *loc = (CARD_LOC_T *)lv_malloc(sizeof(CARD_LOC_T));
            if (loc) { loc->col = c; loc->card_in_col = -1; }
            lv_obj_add_flag(placeholder, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(placeholder, __card_click_cb, LV_EVENT_CLICKED, loc);
        }

        /* Card stack */
        INT32_T face_down_count = 0;
        for (INT32_T k = 0; k < col->n; k++) {
            if (!s_sp.deck[col->cards[k]].face_up) face_down_count++;
            else break;
        }

        INT32_T y = COL_Y0;
        for (INT32_T k = 0; k < col->n; k++) {
            UINT8_T ci = col->cards[k];
            BOOL_T fu = (BOOL_T)s_sp.deck[ci].face_up;
            BOOL_T sel = (s_sp.sel_col == c) &&
                          (k >= __sp_seq_start(c));

            lv_obj_t *card = lv_obj_create(s_card_area);
            lv_obj_remove_style_all(card);
            lv_obj_set_size(card, CARD_W, CARD_H);
            lv_obj_set_pos(card, cx, y);
            lv_obj_set_style_radius(card, 2, 0);
            lv_obj_set_style_border_color(card, lv_color_hex(0x555555), 0);
            lv_obj_set_style_border_width(card, 1, 0);
            lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
            __paint_card_obj(card, (INT32_T)s_sp.deck[ci].rank, fu, sel);

            if (fu) {
                CARD_LOC_T *loc = (CARD_LOC_T *)lv_malloc(sizeof(CARD_LOC_T));
                if (loc) { loc->col = c; loc->card_in_col = k; }
                lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(card, __card_click_cb, LV_EVENT_CLICKED, loc);
            }

            /* Advance y by overlap amount */
            if (!fu) y += CARD_BACK_OVERLAP;
            else y += CARD_OVERLAP;
        }
    }

    /* Stock pile indicator */
    INT32_T deals = s_sp.stock.n / N_COLS;
    CHAR_T sbuf[16];
    snprintf(sbuf, sizeof(sbuf), "[%d]", (int)deals);

    lv_obj_t *stockobj = lv_obj_create(s_card_area);
    lv_obj_remove_style_all(stockobj);
    lv_obj_set_size(stockobj, CARD_W, CARD_H);
    lv_obj_set_pos(stockobj, STOCK_X, STOCK_Y);
    lv_obj_set_style_bg_color(stockobj, lv_color_hex(
        s_sp.stock.n >= N_COLS ? 0x004488 : 0x444444), 0);
    lv_obj_set_style_bg_opa(stockobj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(stockobj, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_border_width(stockobj, 1, 0);
    lv_obj_set_style_radius(stockobj, 2, 0);
    lv_obj_clear_flag(stockobj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(stockobj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(stockobj, __stock_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *slbl = lv_label_create(stockobj);
    lv_obj_set_style_text_color(slbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(slbl, &lv_font_unscii_8, 0);
    lv_label_set_text(slbl, sbuf);
    lv_obj_center(slbl);
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */
VOID_T win95_spider_open(lv_obj_t *parent)
{
    if (s_sp.screen) {
        lv_obj_delete(s_sp.screen);
        memset(&s_sp, 0, sizeof(s_sp));
    }

    s_sp.rand_seed = (UINT32_T)tal_time_get_posix();
    s_sp.score     = 500;
    s_sp.sel_col   = -1;
    __sp_deal_initial();

    /* Full-screen window */
    lv_obj_t *scr = lv_obj_create(parent);
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, SP_W, SP_H);
    lv_obj_set_pos(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x006600), 0);  /* green baize */
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(scr);
    s_sp.screen = scr;

    /* Toolbar: close + score */
    lv_obj_t *tb = lv_obj_create(scr);
    lv_obj_remove_style_all(tb);
    lv_obj_set_size(tb, SP_W - 4, 18);
    lv_obj_set_pos(tb, 2, 2);
    lv_obj_set_style_bg_color(tb, lv_color_hex(WIN95_COLOR_TITLEBAR), 0);
    lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tb, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tl = lv_label_create(tb);
    lv_obj_set_style_text_color(tl, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_text_font(tl, &lv_font_unscii_8, 0);
    lv_label_set_text(tl, "Spider Solitaire");
    lv_obj_align(tl, LV_ALIGN_LEFT_MID, 4, 0);

    s_sp.score_lbl = lv_label_create(tb);
    lv_obj_set_style_text_color(s_sp.score_lbl, lv_color_hex(0xFFFF88), 0);
    lv_obj_set_style_text_font(s_sp.score_lbl, &lv_font_unscii_8, 0);
    lv_label_set_text(s_sp.score_lbl, "Score: 500  Sets: 0");
    lv_obj_align(s_sp.score_lbl, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *xb = lv_btn_create(tb);
    lv_obj_set_size(xb, 14, 14);
    lv_obj_align(xb, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(xb, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_radius(xb, 0, 0);
    lv_obj_set_style_pad_all(xb, 0, 0);
    lv_obj_add_event_cb(xb, __sp_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(xb);
    lv_obj_set_style_text_color(xl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(xl, &lv_font_unscii_8, 0);
    lv_label_set_text(xl, "x");
    lv_obj_center(xl);

    /* Status label */
    s_sp.status_lbl = lv_label_create(scr);
    lv_obj_set_style_text_color(s_sp.status_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_sp.status_lbl, &lv_font_unscii_8, 0);
    lv_label_set_text(s_sp.status_lbl, "Click stock [N] to deal. Click card to select, click target to move.");
    lv_obj_set_pos(s_sp.status_lbl, 4, SP_H - 14);
    lv_obj_set_width(s_sp.status_lbl, SP_W - CARD_W - 10);

    /* Card area (full screen minus toolbar) */
    lv_obj_t *ca = lv_obj_create(scr);
    lv_obj_remove_style_all(ca);
    lv_obj_set_size(ca, SP_W, SP_H - 20);
    lv_obj_set_pos(ca, 0, 20);
    lv_obj_set_style_bg_opa(ca, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(ca, LV_OBJ_FLAG_SCROLLABLE);
    s_card_area = ca;

    __sp_render();
}
