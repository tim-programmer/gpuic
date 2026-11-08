/*
 * Auto margins and wrapping flex lines, checked on the geometry layout produces (positions and sizes read from the laid-out tree):
 * auto margins sharing the free space on the main axis, overriding gp_justify, absorbing the cross space (centering, pushing, no
 * stretch), being cleared by a number after them and zero without free space; wrapped rows and columns breaking by base size and gap,
 * growing, justifying and stretching per line, with padding, auto-sized, with auto margins in a line, and scrolling across.
 * Needs a display (a window to lay out in).
 */
#include "../src/internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...)                                                               \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            if (failures++ < 60) {                                                     \
                printf("FAIL %s:%d: ", __FILE__, __LINE__);                            \
                printf(__VA_ARGS__);                                                   \
                printf("\n");                                                          \
            }                                                                          \
        }                                                                              \
    } while (0)

static gp_app *app;
static gp_co *co_;
static gp_window *win(void) { return app->windows[0]; }
static bool wait_draws(unsigned n) {
    unsigned long target = app->stat_draws + n;
    for (int i = 0; i < 1000 && app->stat_draws < target; i++) gp_sleep(co_, 0.004);
    return app->stat_draws >= target;
}

#define RED gp_rgb(0xff0000)
#define BOX(w, h, ...) gp_div(gp_w(w), gp_h(h), gp_bg(RED), ##__VA_ARGS__)
#define ROW(w, h, ...) gp_div(gp_flex_row(), gp_w(w), gp_h(h), ##__VA_ARGS__)
#define ROWA(w, ...) gp_div(gp_flex_row(), gp_w(w), ##__VA_ARGS__)
#define B3 BOX(30, 10), BOX(30, 10), BOX(30, 10)

static gp_node *render(void *self, gp_ctx *cx) {
    return gp_div(
        gp_size_full(), gp_overflow_y_scroll(), gp_items_start(),
        /* 0 */ ROW(200, 40, BOX(20, 10), BOX(20, 10), BOX(20, 10, gp_ml_auto())),
        /* 1 */ ROW(200, 40, BOX(20, 10), BOX(20, 10, gp_mx_auto()), BOX(20, 10)),
        /* 2 */ ROW(200, 40, BOX(20, 10, gp_mr_auto()), BOX(20, 10), BOX(20, 10)),
        /* 3 */ ROW(200, 40, gp_justify_center(), BOX(20, 10), BOX(20, 10), BOX(20, 10, gp_ml_auto())),
        /* 4 */ ROW(200, 40, BOX(60, 10), BOX(70, 10), BOX(70, 10, gp_ml_auto())),
        /* 5 */ ROW(200, 40, gp_items_start(), BOX(20, 10, gp_my_auto()), BOX(20, 10, gp_mt_auto()), BOX(20, 10, gp_mb_auto()), BOX(20, 10, gp_mt_auto(), gp_mt(5))),
        /* 6 */ ROW(200, 40, gp_div(gp_w(20), gp_div(gp_h(10), gp_bg(RED)), gp_my_auto()), gp_div(gp_w(20), gp_div(gp_h(10), gp_bg(RED)))),
        /* 7 */ gp_div(gp_w(100), gp_h(200), BOX(20, 20, gp_mx_auto()), BOX(20, 20, gp_mt_auto()), BOX(20, 20, gp_ml_auto())),
        /* 8 */ ROW(200, 40, BOX(20, 10, gp_ml_auto(), gp_ml(5)), BOX(20, 10, gp_ml(5), gp_ml_auto())),
        /* 9 */ ROW(200, 40, BOX(20, 10, gp_flex_grow()), BOX(20, 10, gp_ml_auto())),
        /* 10: wrapping, no gap */ ROWA(100, gp_flex_wrap(), B3, B3, BOX(30, 10), BOX(30, 10)),
        /* 11: gap both ways */ ROWA(100, gp_flex_wrap(), gp_gap(5), B3, B3, BOX(30, 10), BOX(30, 10)),
        /* 12: grow per line */ ROWA(100, gp_flex_wrap(), BOX(40, 10, gp_flex_grow()), BOX(40, 10, gp_flex_grow()), BOX(40, 10, gp_flex_grow())),
        /* 13: justify per line */ ROWA(100, gp_flex_wrap(), gp_justify_center(), B3, BOX(30, 10)),
        /* 14: line heights, stretch */ ROWA(100, gp_flex_wrap(), BOX(30, 10), BOX(30, 30), BOX(30, 10),
                                           gp_div(gp_w(50), gp_bg(RED)), gp_div(gp_w(50), gp_bg(RED), gp_div(gp_h(8)))),
        /* 15: a column wraps by height */ gp_div(gp_flex_wrap(), gp_h(100), BOX(20, 30), BOX(20, 30), BOX(20, 30), BOX(20, 30), BOX(20, 30)),
        /* 16: padding and border */ ROWA(124, gp_flex_wrap(), gp_p(10), gp_border(2), B3, BOX(30, 10)),
        /* 17: unsized, not stretched */ gp_div(gp_w(100), gp_items_start(), gp_div(gp_flex_row(), gp_flex_wrap(), B3, BOX(30, 10))),
        /* 18: auto margin in a line */ ROWA(100, gp_flex_wrap(), BOX(30, 10), BOX(30, 10), BOX(30, 10, gp_ml_auto()), BOX(30, 10), BOX(30, 10, gp_ml_auto())),
        /* 19: wrapping scroller */ ROW(100, 25, gp_flex_wrap(), gp_overflow_y_scroll(), B3, B3, BOX(30, 10)),
        /* 20: empty */ ROWA(100, gp_flex_wrap()), /* 21: single */ ROWA(100, gp_flex_wrap(), BOX(30, 10)),
        /* 22: margins and wrapping */ ROWA(130, gp_flex_wrap(), BOX(40, 10, gp_m(5)), BOX(40, 10, gp_m(5)), BOX(40, 10, gp_m(5))),
        /* 23: wrap off by default */ ROW(100, 20, B3, BOX(30, 10)),
        /* 24: an item wider than the line has a line of its own, shrunk to it */ ROWA(100, gp_flex_wrap(), BOX(30, 10), BOX(150, 10), BOX(30, 10)),
        /* 25: unsized, one wide item */ gp_div(gp_w(100), gp_items_start(), gp_div(gp_flex_row(), gp_flex_wrap(), BOX(150, 10))),
        /* 26: absolute children */ ROWA(100, gp_flex_wrap(), gp_align_content_start(), gp_h(60), BOX(30, 10), BOX(90, 10, gp_absolute(), gp_right(4), gp_bottom(6)), BOX(30, 10), BOX(30, 10), BOX(30, 10)),
        /* 27: a scroll offset survives */ ROW(100, 25, gp_flex_wrap(), gp_overflow_y_scroll(), B3, B3, BOX(30, 10)),
        /* 28: auto after a number: the number is gone */ ROW(200, 40, BOX(20, 10, gp_ml(10), gp_mx_auto())),
        /* 29: the gap counts in breaking */ ROWA(95, gp_flex_wrap(), gp_gap(5), B3),
        /* 30: flex-basis is the size that breaks lines */ ROWA(100, gp_flex_wrap(), BOX(10, 10, gp_flex_basis(40)), BOX(10, 10, gp_flex_basis(40)), BOX(10, 10, gp_flex_basis(40))),
        /* 31: ...clamped by min and max */ ROWA(100, gp_flex_wrap(), BOX(10, 10, gp_flex_basis(20), gp_min_w(60)), BOX(10, 10, gp_flex_basis(20), gp_min_w(60)),
                                                BOX(10, 10, gp_flex_basis(80), gp_max_w(40)), BOX(10, 10, gp_flex_basis(80), gp_max_w(40)), BOX(10, 10, gp_flex_basis(80), gp_max_w(40))),
        /* 32: no room across, no auto margin */ ROW(200, 40, BOX(20, 60, gp_my_auto()), BOX(20, 60, gp_mt_auto()), BOX(20, 60, gp_mb_auto())),
        /* 33: a scroller along the main axis does not wrap */ ROW(100, 20, gp_flex_wrap(), gp_overflow_x_scroll(), B3, B3),
        /* 34: right to left */ ROW(200, 40, gp_text_dir_rtl(), BOX(20, 10), BOX(30, 10), BOX(40, 10)),
        /* 35: ...left and right margins stay where they are */ ROW(200, 40, gp_text_dir_rtl(), BOX(20, 10, gp_ml(8)), BOX(20, 10, gp_mr(6))),
        /* 36: ...end is the left */ ROW(200, 40, gp_text_dir_rtl(), gp_justify_end(), BOX(20, 10), BOX(30, 10)),
        /* 37: ...padding is physical */ ROW(200, 40, gp_text_dir_rtl(), gp_pr(30), gp_pl(5), BOX(20, 10)),
        /* 38: a column's start is the right */ gp_div(gp_w(100), gp_h(100), gp_text_dir_rtl(), gp_items_start(), BOX(20, 10), BOX(30, 10, gp_mr(5))),
        /* 39: ...and its end the left */ gp_div(gp_w(100), gp_h(100), gp_text_dir_rtl(), gp_items_end(), BOX(20, 10, gp_ml(7))),
        /* 40: auto margins are physical */ ROW(200, 40, gp_text_dir_rtl(), BOX(20, 10), BOX(20, 10, gp_ml_auto())),
        /* 41 */ ROW(200, 40, gp_text_dir_rtl(), BOX(20, 10, gp_mr_auto()), BOX(20, 10)),
        /* 42: wrapped rows */ ROWA(100, gp_text_dir_rtl(), gp_flex_wrap(), B3, B3, BOX(30, 10)),
        /* 43: wrapped columns stack from the right */ gp_div(gp_text_dir_rtl(), gp_flex_wrap(), gp_h(100), BOX(20, 30), BOX(20, 30), BOX(20, 30), BOX(20, 30), BOX(20, 30)),
        /* 44: a left-to-right child inside */ ROW(200, 40, gp_text_dir_rtl(), gp_div(gp_flex_row(), gp_w(100), gp_h(20), gp_text_dir_ltr(), BOX(20, 10), BOX(20, 10))),
        /* 45: a row scroller is not mirrored */ ROW(100, 20, gp_text_dir_rtl(), gp_overflow_x_scroll(), B3, B3),
        /* 46: align-content, the default: stretch */ ROWA(100, gp_h(100), gp_flex_wrap(), B3, B3, BOX(30, 10)),
        /* 47 */ ROWA(100, gp_h(100), gp_flex_wrap(), gp_align_content_start(), B3, B3, BOX(30, 10)),
        /* 48 */ ROWA(100, gp_h(100), gp_flex_wrap(), gp_align_content_center(), B3, B3, BOX(30, 10)),
        /* 49 */ ROWA(100, gp_h(100), gp_flex_wrap(), gp_align_content_end(), B3, B3, BOX(30, 10)),
        /* 50 */ ROWA(100, gp_h(100), gp_flex_wrap(), gp_align_content_between(), B3, B3, BOX(30, 10)),
        /* 51 */ ROWA(100, gp_h(100), gp_flex_wrap(), gp_align_content_around(), B3, B3, BOX(30, 10)),
        /* 52 */ ROWA(100, gp_h(100), gp_flex_wrap(), gp_align_content_evenly(), B3, B3, BOX(30, 10)),
        /* 53: with a gap */ ROWA(100, gp_h(100), gp_flex_wrap(), gp_gap(5), gp_align_content_between(), B3, B3, BOX(30, 10)),
        /* 54: a stretched item fills its stretched line */ ROWA(100, gp_h(100), gp_flex_wrap(), BOX(30, 10), gp_div(gp_w(30), gp_bg(RED)), BOX(30, 10)),
        /* 55: columns */ gp_div(gp_flex_wrap(), gp_w(100), gp_h(100), gp_align_content_end(), BOX(20, 30), BOX(20, 30), BOX(20, 30), BOX(20, 30), BOX(20, 30)),
        /* 56: ...right to left, where end is the left */ gp_div(gp_flex_wrap(), gp_w(100), gp_h(100), gp_text_dir_rtl(), gp_align_content_end(), BOX(20, 30), BOX(20, 30), BOX(20, 30), BOX(20, 30), BOX(20, 30)),
        /* 57: no room: nothing moves */ ROWA(100, gp_h(20), gp_flex_wrap(), gp_align_content_end(), B3, B3, BOX(30, 10)));
}

static gp_el *box(int i) { return win()->tree->children[i]; }
static gp_el *kid(int i, int j) { return box(i)->children[j]; }
#define X(i, j) (kid(i, j)->pos.x)
#define Y(i, j) (kid(i, j)->pos.y)
#define W(i, j) (kid(i, j)->size.w)
#define H(i, j) (kid(i, j)->size.h)
static bool near(float a, float b) { return fabsf(a - b) < 0.51f; }
#define AT(i, j, x, y) (near(X(i, j), (x)) && near(Y(i, j), (y)))

static void run(void) {
    wait_draws(2);
    CHECK(win()->tree->nchildren == 58, "%u scenarios", win()->tree->nchildren);
    if (win()->tree->nchildren != 58) return;

    /* auto margins, main axis */
    CHECK(AT(0, 0, 0, 0) && AT(0, 1, 20, 0) && AT(0, 2, 180, 0), "ml_auto on the last pushes it to the end: %g %g %g", X(0, 0), X(0, 1), X(0, 2));
    CHECK(AT(1, 0, 0, 0) && AT(1, 1, 90, 0) && AT(1, 2, 180, 0), "mx_auto centers in the free space: %g %g %g", X(1, 0), X(1, 1), X(1, 2));
    CHECK(AT(2, 0, 0, 0) && AT(2, 1, 160, 0) && AT(2, 2, 180, 0), "mr_auto on the first pushes the rest to the end: %g %g %g", X(2, 0), X(2, 1), X(2, 2));
    CHECK(AT(3, 0, 0, 0) && AT(3, 2, 180, 0), "auto margins leave nothing for justify: %g %g", X(3, 0), X(3, 2));
    CHECK(AT(4, 0, 0, 0) && AT(4, 1, 60, 0) && AT(4, 2, 130, 0) && near(W(4, 2), 70), "no free space, no auto margin: %g %g w %g", X(4, 1), X(4, 2), W(4, 2));
    CHECK(AT(8, 0, 5, 0) && AT(8, 1, 180, 0), "a number after auto clears it, auto after a number replaces it: %g %g", X(8, 0), X(8, 1));
    CHECK(near(W(9, 0), 180) && AT(9, 1, 180, 0), "grow takes the space before auto margins see it: w %g, x %g", W(9, 0), X(9, 1));

    /* across */
    CHECK(AT(5, 0, 0, 15) && AT(5, 1, 20, 30) && AT(5, 2, 40, 0), "my_auto centers, mt_auto ends, mb_auto starts: y %g %g %g", Y(5, 0), Y(5, 1), Y(5, 2));
    CHECK(AT(5, 3, 60, 5), "a number after mt_auto is the margin: y %g", Y(5, 3));
    CHECK(near(H(6, 0), 10) && near(Y(6, 0), 15), "an auto margin stops the stretch: h %g y %g", H(6, 0), Y(6, 0));
    CHECK(near(H(6, 1), 40) && near(Y(6, 1), 0), "the next is stretched: h %g", H(6, 1));
    CHECK(AT(7, 0, 40, 0) && AT(7, 1, 0, 160) && AT(7, 2, 80, 180), "column: x %g %g %g, y %g %g %g", X(7, 0), X(7, 1), X(7, 2), Y(7, 0), Y(7, 1), Y(7, 2));

    /* wrapping */
    const float lx[8] = { 0, 30, 60, 0, 30, 60, 0, 30 }, ly[8] = { 0, 0, 0, 10, 10, 10, 20, 20 };
    for (int j = 0; j < 8; j++) CHECK(AT(10, j, lx[j], ly[j]), "wrap %d at %g,%g wanted %g,%g", j, X(10, j), Y(10, j), lx[j], ly[j]);
    CHECK(near(box(10)->size.h, 30) && near(box(10)->size.w, 100), "an unsized wrapped container is as tall as its lines: %g x %g", box(10)->size.w, box(10)->size.h);
    for (int j = 0; j < 8; j++) CHECK(AT(11, j, (j % 3) * 35, (j / 3) * 15), "gap wrap %d at %g,%g", j, X(11, j), Y(11, j));
    CHECK(near(box(11)->size.h, 40), "gaps between lines: %g", box(11)->size.h);
    CHECK(near(W(12, 0), 50) && near(W(12, 1), 50) && near(W(12, 2), 100) && AT(12, 1, 50, 0) && AT(12, 2, 0, 10), "grow per line: %g %g %g", W(12, 0), W(12, 1), W(12, 2));
    CHECK(AT(13, 0, 5, 0) && AT(13, 2, 65, 0) && AT(13, 3, 35, 10), "justify per line: %g %g %g | %g", X(13, 0), X(13, 1), X(13, 2), X(13, 3));
    CHECK(AT(14, 0, 0, 0) && AT(14, 1, 30, 0) && AT(14, 2, 60, 0), "line 1: %g %g %g", X(14, 0), X(14, 1), X(14, 2));
    CHECK(AT(14, 3, 0, 30) && AT(14, 4, 50, 30), "line 2 starts below the tallest of line 1: %g,%g %g,%g", X(14, 3), Y(14, 3), X(14, 4), Y(14, 4));
    CHECK(near(H(14, 3), 8) && near(H(14, 4), 8) && near(box(14)->size.h, 38), "a stretched item fills its line: %g %g, container %g", H(14, 3), H(14, 4), box(14)->size.h);
    for (int j = 0; j < 5; j++) CHECK(AT(15, j, (j / 3) * 20, (j % 3) * 30), "column wrap %d at %g,%g", j, X(15, j), Y(15, j));
    CHECK(near(box(15)->size.w, 40), "column width from its columns: %g", box(15)->size.w);
    for (int j = 0; j < 4; j++) CHECK(AT(16, j, 12 + (j % 3) * 30, 12 + (j / 3) * 10), "padding and border offset the lines: %d at %g,%g", j, X(16, j), Y(16, j));
    CHECK(near(box(16)->size.h, 44), "...and add to the size: %g", box(16)->size.h);
    CHECK(near(box(17)->children[0]->size.w, 90) && near(box(17)->children[0]->size.h, 20), "unsized: as wide as the widest line (%g x %g)", box(17)->children[0]->size.w, box(17)->children[0]->size.h);
    CHECK(AT(18, 2, 70, 0) && AT(18, 3, 0, 10) && AT(18, 4, 70, 10), "auto margins per line: %g %g %g", X(18, 2), X(18, 3), X(18, 4));
    CHECK(near(box(19)->scroll_max.y, 5) && near(box(19)->scroll_max.x, 0), "a wrapped scroller scrolls across: %g %g", box(19)->scroll_max.y, box(19)->scroll_max.x);
    CHECK(near(box(20)->size.h, 0) && box(20)->nchildren == 0, "nothing to wrap");
    CHECK(AT(21, 0, 0, 0) && near(box(21)->size.h, 10), "a single child: %g", box(21)->size.h);
    CHECK(AT(22, 0, 5, 5) && AT(22, 1, 55, 5) && AT(22, 2, 5, 25), "margins count in the breaking: %g,%g %g,%g %g,%g", X(22, 0), Y(22, 0), X(22, 1), Y(22, 1), X(22, 2), Y(22, 2));
    CHECK(AT(24, 0, 0, 0) && AT(24, 1, 0, 10) && near(W(24, 1), 100) && AT(24, 2, 0, 20) && near(box(24)->size.h, 30), "an item wider than the line: %g,%g %g,%g w %g, then %g,%g, height %g", X(24, 0), Y(24, 0), X(24, 1), Y(24, 1), W(24, 1), X(24, 2), Y(24, 2), box(24)->size.h);
    CHECK(near(box(25)->children[0]->size.w, 100) && near(kid(25, 0)->children[0]->size.w, 100), "unsized, one wide item: the container is as wide as there is room and the item shrinks to it (%g, %g)", box(25)->children[0]->size.w, kid(25, 0)->children[0]->size.w);
    CHECK(AT(26, 1, 6, 44) && near(box(26)->size.h, 60) && AT(26, 2, 30, 0) && AT(26, 3, 60, 0) && AT(26, 4, 0, 10), "an absolute child is placed by its insets and takes no part in the lines: %g,%g; then %g,%g %g,%g %g,%g", X(26, 1), Y(26, 1), X(26, 2), Y(26, 2), X(26, 3), Y(26, 3), X(26, 4), Y(26, 4));
    {   /* a scroller wrapping across keeps its offset when it is laid out again */
        gp_scroll_state *st = gp_scroll_state_get(&win()->scrolls, box(27)->scroll_key, false);
        CHECK(st != NULL && near(box(27)->scroll_max.y, 5), "scroll state of the wrapping scroller (max %g)", box(27)->scroll_max.y);
        if (st) {
            st->y = 3;
            win()->dirty_layout = true;
            win()->dirty_paint = true;
            wait_draws(2);
            CHECK(near(box(27)->scroll.y, 3) && near(st->y, 3), "its offset survives layout: %g, state %g", box(27)->scroll.y, st->y);
        }
    }
    CHECK(AT(28, 0, 90, 0), "an auto margin replaces the number set before it: x %g", X(28, 0));
    CHECK(AT(32, 0, 0, 0) && AT(32, 1, 20, 0) && AT(32, 2, 40, 0), "an item taller than the line: auto margins are zero: y %g %g %g", Y(32, 0), Y(32, 1), Y(32, 2));
    CHECK(AT(29, 0, 0, 0) && AT(29, 1, 35, 0) && AT(29, 2, 0, 15), "the gap counts when breaking (3 x 30 and 2 x 5 do not fit 95): %g,%g %g,%g %g,%g", X(29, 0), Y(29, 0), X(29, 1), Y(29, 1), X(29, 2), Y(29, 2));
    CHECK(AT(30, 0, 0, 0) && AT(30, 1, 40, 0) && AT(30, 2, 0, 10) && near(W(30, 0), 40), "flex-basis breaks the lines, not the width: %g,%g %g,%g %g,%g w %g", X(30, 0), Y(30, 0), X(30, 1), Y(30, 1), X(30, 2), Y(30, 2), W(30, 0));
    CHECK(AT(31, 0, 0, 0) && AT(31, 1, 0, 10) && AT(31, 2, 60, 10) && AT(31, 3, 0, 20) && AT(31, 4, 40, 20),
          "min and max clamp the size that breaks lines: %g,%g %g,%g %g,%g %g,%g %g,%g", X(31, 0), Y(31, 0), X(31, 1), Y(31, 1), X(31, 2), Y(31, 2), X(31, 3), Y(31, 3), X(31, 4), Y(31, 4));
    CHECK(AT(33, 5, 150, 0) && near(box(33)->scroll_max.x, 80), "a scroller along the main axis keeps one line: x %g, scroll %g", X(33, 5), box(33)->scroll_max.x);
    /* right to left */
    CHECK(AT(34, 0, 180, 0) && AT(34, 1, 150, 0) && AT(34, 2, 110, 0), "rtl row from the right: %g %g %g", X(34, 0), X(34, 1), X(34, 2));
    CHECK(AT(35, 0, 180, 0) && AT(35, 1, 146, 0), "rtl margins are physical: %g %g", X(35, 0), X(35, 1));
    CHECK(AT(36, 0, 30, 0) && AT(36, 1, 0, 0), "rtl justify end is the left: %g %g", X(36, 0), X(36, 1));
    CHECK(AT(37, 0, 150, 0), "rtl padding is physical (30 on the right): %g", X(37, 0));
    CHECK(AT(38, 0, 80, 0) && AT(38, 1, 65, 10), "rtl column, items start: %g %g", X(38, 0), X(38, 1));
    CHECK(AT(39, 0, 7, 0), "rtl column, items end: %g", X(39, 0));
    CHECK(AT(40, 0, 180, 0) && AT(40, 1, 160, 0), "rtl ml_auto is the physical left, beside the item: %g %g", X(40, 0), X(40, 1));
    CHECK(AT(41, 0, 20, 0) && AT(41, 1, 0, 0), "rtl mr_auto is the start: %g %g", X(41, 0), X(41, 1));
    for (int j = 0; j < 7; j++) CHECK(AT(42, j, 70 - (j % 3) * 30, (j / 3) * 10), "rtl wrap %d at %g,%g", j, X(42, j), Y(42, j));
    for (int j = 0; j < 5; j++) CHECK(AT(43, j, (j / 3) ? 0 : 20, (j % 3) * 30), "rtl column wrap %d at %g,%g", j, X(43, j), Y(43, j));
    CHECK(AT(44, 0, 100, 0) && near(kid(44, 0)->children[1]->pos.x, 20), "ltr inside rtl: the box is at the right, its own items from the left: %g, %g", X(44, 0), kid(44, 0)->children[1]->pos.x);
    CHECK(AT(45, 5, 150, 0) && near(box(45)->scroll_max.x, 80), "rtl row scroller is left alone: %g, scroll %g", X(45, 5), box(45)->scroll_max.x);
    {   /* align-content: three lines of 10 in a box 100 high; the first item of each line is 0, 3, 6 */
        const float expect[7][3] = { { 0, 100.f / 3, 200.f / 3 }, { 0, 10, 20 }, { 35, 45, 55 }, { 70, 80, 90 }, { 0, 45, 90 },
                                     { 35.f / 3, 45, 90 - 35.f / 3 }, { 17.5f, 45, 72.5f } };
        const char *name[7] = { "stretch", "start", "center", "end", "between", "around", "evenly" };
        for (int k = 0; k < 7; k++)
            CHECK(near(Y(46 + k, 0), expect[k][0]) && near(Y(46 + k, 3), expect[k][1]) && near(Y(46 + k, 6), expect[k][2]) && near(X(46 + k, 3), 0),
                  "align-content %s: lines at %g %g %g, wanted %g %g %g", name[k], Y(46 + k, 0), Y(46 + k, 3), Y(46 + k, 6), expect[k][0], expect[k][1], expect[k][2]);
    }
    CHECK(near(Y(53, 0), 0) && near(Y(53, 3), 45) && near(Y(53, 6), 90), "align-content between with a gap: %g %g %g", Y(53, 0), Y(53, 3), Y(53, 6));
    CHECK(near(H(54, 1), 100) && near(H(54, 0), 10), "a stretched item fills a stretched line (%g), fixed ones do not (%g)", H(54, 1), H(54, 0));
    CHECK(AT(55, 0, 60, 0) && AT(55, 3, 80, 0), "align-content end on columns: %g, %g", X(55, 0), X(55, 3));
    CHECK(AT(56, 0, 20, 0) && AT(56, 3, 0, 0), "...right to left, the first column is the right one of the two at the left: %g, %g", X(56, 0), X(56, 3));
    CHECK(AT(57, 0, 0, 0) && AT(57, 3, 0, 10) && AT(57, 6, 0, 20), "without room the lines stay: %g %g %g", Y(57, 0), Y(57, 3), Y(57, 6));
    CHECK(near(X(23, 3), 75) && near(W(23, 3), 25) && near(Y(23, 3), 0), "without gp_flex_wrap everything is on one line (shrunk): x %g w %g y %g", X(23, 3), W(23, 3), Y(23, 3));
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    run();
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    gp_entity v = gp_view_new(app, 1, &(char){ 0 }, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "flex_test", .width = 640, .height = 800 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "flex_test: %d FAILURE(S)\n" : "flex_test: all checks passed\n", failures);
    return failures != 0;
}
