/*
 * The automatic minimum size of a row item (CSS min-width: auto): when a row has to shrink, an item does not go below its min-content
 * width: the longest word of its text, the sum of the min-content widths of a nested row, the widest of a column, the width of a picture
 * (capped by a width of the item's own); items that clip, scroll, are lists or have a min-width of their own shrink as before, and a row
 * with room does not look at any of it. Expected widths come from the text measurer, wrapped at every opportunity. Needs a display.
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
#define LONGWORD "internationalization"
#define SENTENCE "aaa bbb ccc ddd eee fff ggg hhh"
#define ROWW(w, ...) gp_div(gp_flex_row(), gp_w(w), gp_items_start(), ##__VA_ARGS__)

static gp_image g_pic;

static gp_node *render(void *self, gp_ctx *cx) {
    return gp_div(
        gp_size_full(), gp_overflow_y_scroll(), gp_items_start(),
        /* 0: a word that cannot break keeps its width, the sentence gives way */ ROWW(150, gp_text(LONGWORD), gp_text(SENTENCE)),
        /* 1: ... unless it may shrink (min_w 0) */ ROWW(150, gp_text(LONGWORD, gp_min_w(0)), gp_text(SENTENCE)),
        /* 2: ... or clips */ ROWW(150, gp_div(gp_overflow_hidden(), gp_text(LONGWORD)), gp_text(SENTENCE)),
        /* 3: a nested row adds up */ ROWW(60, gp_div(gp_flex_row(), gp_gap(10), gp_text("foo"), gp_text("barbaz")), gp_text(SENTENCE, gp_flex_none())),
        /* 4: a nested column takes the widest */ ROWW(60, gp_div(gp_flex_col(), gp_text("foo"), gp_text("barbazqux")), gp_text(SENTENCE, gp_flex_none())),
        /* 5: padding and a border count */ ROWW(60, gp_div(gp_px(7), gp_border(2), gp_text("word")), gp_text(SENTENCE, gp_flex_none())),
        /* 6: a width of its own is a cap: 300 wide, shrinks to the word, not below it */ ROWW(120, gp_div(gp_w(300), gp_text(LONGWORD)), gp_div(gp_w(300), gp_text(LONGWORD))),
        /* 7: ... and the cap is the width when that is smaller than the word */ ROWW(100, gp_div(gp_w(40), gp_text(LONGWORD)), gp_div(gp_w(100))),
        /* 8: room to spare: nothing is measured, widths are the natural ones */ ROWW(600, gp_text(LONGWORD), gp_text(SENTENCE)),
        /* 9: a picture does not shrink below its pixels */ ROWW(60, gp_img(g_pic), gp_div(gp_w(40), gp_h(10), gp_bg(RED))),
        /* 10: no shrink factor, no minimum needed: it keeps its basis */ ROWW(150, gp_text(LONGWORD, gp_flex_none()), gp_text(SENTENCE)),
        /* 11: an empty item can shrink to nothing */ ROWW(50, gp_div(gp_w(40), gp_h(10), gp_bg(RED)), gp_div(gp_w(40), gp_h(10), gp_bg(RED))),
        /* 12: an absolute child is not part of it */ ROWW(60, gp_div(gp_text("ab"), gp_div(gp_absolute(), gp_w(200), gp_h(5))), gp_text(SENTENCE, gp_flex_none())),
        /* 13: a list or a scroller has no width to keep */ ROWW(60, gp_div(gp_overflow_x_scroll(), gp_text(LONGWORD)), gp_text(SENTENCE, gp_flex_none())),
        /* 14: margins of the nested items count */ ROWW(60, gp_div(gp_flex_row(), gp_text("foo", gp_mx(6)), gp_text("bar")), gp_text(SENTENCE, gp_flex_none())),
        /* 15: a wrapping row inside takes the widest */ ROWW(60, gp_div(gp_flex_row(), gp_flex_wrap(), gp_text("foo"), gp_text("barbazqux")), gp_text(SENTENCE, gp_flex_none())),
        /* 16: a column does not shrink along its own axis before the row asks (nested text wraps at its word) */ ROWW(150, gp_div(gp_text(SENTENCE)), gp_text(LONGWORD)),
        /* 17: a min-content that is wider than the row: the item overflows, the neighbour is squeezed to its own minimum */
        ROWW(100, gp_text(LONGWORD), gp_text("squeezed words here")),
        /* 18: whitespace pre does not break */ ROWW(60, gp_text("one two three", gp_whitespace_pre()), gp_text(SENTENCE, gp_flex_none())),
        /* 19: a nested item's max width limits what it asks for */ ROWW(60, gp_div(gp_flex_row(), gp_text(LONGWORD, gp_max_w(30))), gp_text(SENTENCE, gp_flex_none())),
        /* 20: ... and its min width raises it */ ROWW(60, gp_div(gp_flex_row(), gp_div(gp_min_w(90), gp_text("ab"))), gp_text(SENTENCE, gp_flex_none())),
        /* 21: a nested item with a width asks for that */ ROWW(60, gp_div(gp_flex_row(), gp_div(gp_w(70), gp_text(LONGWORD))), gp_text(SENTENCE, gp_flex_none())),
        /* 22: padding and a border on the text itself count */ ROWW(60, gp_text(LONGWORD, gp_px(5), gp_border(1)), gp_text(SENTENCE, gp_flex_none())),
        /* 23: a basis below the word is raised to it, with room to spare */ ROWW(600, gp_text(LONGWORD, gp_flex_basis(20)), gp_text("tail")),
        /* 24: in a wrapping row an item on its own line keeps its word */ gp_div(gp_flex_row(), gp_flex_wrap(), gp_w(100), gp_items_start(), gp_text(LONGWORD), gp_text("ab")),
        /* 25: ... and the line after it still wraps */ gp_div(gp_flex_row(), gp_flex_wrap(), gp_w(100), gp_items_start(), gp_text("ab"), gp_text("cd"), gp_text(LONGWORD)),
        /* 26: ... and the room it takes is taken from what grows */ ROWW(300, gp_text(LONGWORD, gp_flex_basis(20)), gp_div(gp_flex_grow(), gp_h(10))),
        /* 27: a width of its own caps it, basis or not */ ROWW(600, gp_text(LONGWORD, gp_w(40), gp_flex_basis(20)), gp_text("tail")));
}

static gp_el *box(int i) { return win()->tree->children[i]; }
static gp_el *kid(int i, int j) { return box(i)->children[j]; }
#define X(i, j) (kid(i, j)->pos.x)
#define W(i, j) (kid(i, j)->size.w)
static bool near(float a, float b) { return fabsf(a - b) < 0.51f; }

/* the widest word of `text` at the style `el` has: what min-content means */
static float word_w(const gp_el *el) {
    gp_size t = gp_text_measure_dir(app->text, GP_TEXT_SEL(el->ts), el->ts.size, el->text, 0, el->style.whitespace_pre, el->ts.dir, NULL, NULL, NULL);
    return ceilf(t.w);
}
static float natural_w(const gp_el *el) {
    gp_size t = gp_text_measure_dir(app->text, GP_TEXT_SEL(el->ts), el->ts.size, el->text, -1, el->style.whitespace_pre, el->ts.dir, NULL, NULL, NULL);
    return ceilf(t.w);
}

static void run(void) {
    wait_draws(2);
    CHECK(win()->tree->nchildren == 28, "%u scenarios", win()->tree->nchildren);
    if (win()->tree->nchildren != 28) return;
    const float lw = word_w(kid(0, 0)), ln = natural_w(kid(0, 0));
    CHECK(lw == ln && lw > 100, "the long word is a single word, %g wide", lw);
    const float sw = word_w(kid(0, 1)), sn = natural_w(kid(0, 1));
    CHECK(sw < 40 && sn > 150, "the sentence breaks into short words (%g) and is long in one line (%g)", sw, sn);

    /* the long word keeps its width and the sentence gives way (to what is left, but not below its widest word) */
    CHECK(near(W(0, 0), lw) && near(X(0, 1), lw) && near(W(0, 1), sw), "0: the word keeps %g (got %g), the sentence %g (the widest word of it is %g) from x %g", lw, W(0, 0), W(0, 1), sw, X(0, 1));
    CHECK(W(1, 0) < lw - 20, "1: with min_w 0 the word shrinks: %g of %g", W(1, 0), lw);
    CHECK(W(2, 0) < lw - 20, "2: with overflow hidden it shrinks: %g of %g", W(2, 0), lw);

    /* nested */
    const float foo = word_w(kid(3, 0)->children[0]), baz = word_w(kid(3, 0)->children[1]);
    CHECK(near(W(3, 0), foo + 10 + baz), "3: a row of foo and barbaz keeps their sum and the gap: %g, wanted %g", W(3, 0), foo + 10 + baz);
    const float f4 = word_w(kid(4, 0)->children[0]), q4 = word_w(kid(4, 0)->children[1]);
    CHECK(near(W(4, 0), fmaxf(f4, q4)), "4: a column keeps its widest: %g, wanted %g", W(4, 0), fmaxf(f4, q4));
    const float w5 = word_w(kid(5, 0)->children[0]) + 14 + 4;
    CHECK(near(W(5, 0), w5), "5: padding 7 and border 2 count: %g, wanted %g", W(5, 0), w5);

    /* a width of its own */
    CHECK(near(W(6, 0), fmaxf(lw, 60)) && near(W(6, 1), fmaxf(lw, 60)), "6: 300 wide items shrink to the word (%g) and no further: %g %g", lw, W(6, 0), W(6, 1));
    CHECK(near(W(7, 0), 40), "7: an item 40 wide holding a longer word is not squeezed below its 40: %g", W(7, 0));

    CHECK(near(W(8, 0), lw) && near(W(8, 1), natural_w(kid(8, 1))), "8: with room everything is natural: %g, %g", W(8, 0), W(8, 1));
    CHECK(near(W(9, 0), 50) && near(W(9, 1), 10), "9: a 50 px picture keeps its pixels: %g, the box gives way: %g", W(9, 0), W(9, 1));
    CHECK(near(W(10, 0), lw), "10: flex none keeps the word: %g", W(10, 0));
    CHECK(near(W(11, 0), 25) && near(W(11, 1), 25), "11: empty boxes of 40 shrink to 25 each: %g %g", W(11, 0), W(11, 1));
    CHECK(near(W(12, 0), word_w(kid(12, 0)->children[0])), "12: an absolute child adds nothing: %g", W(12, 0));
    CHECK(W(13, 0) < 20, "13: a scroller gives way completely: %g", W(13, 0));
    const float f14 = word_w(kid(14, 0)->children[0]), b14 = word_w(kid(14, 0)->children[1]);
    CHECK(near(W(14, 0), f14 + 12 + b14), "14: margins of the nested items count: %g, wanted %g", W(14, 0), f14 + 12 + b14);
    const float f15 = word_w(kid(15, 0)->children[0]), q15 = word_w(kid(15, 0)->children[1]);
    CHECK(near(W(15, 0), fmaxf(f15, q15)), "15: a wrapping row inside takes the widest: %g, wanted %g", W(15, 0), fmaxf(f15, q15));
    CHECK(W(16, 0) >= word_w(kid(16, 0)->children[0]) - 0.5f && near(W(16, 1), lw), "16: %g %g", W(16, 0), W(16, 1));
    const float s17 = word_w(kid(17, 1));
    CHECK(near(W(17, 0), lw) && W(17, 1) >= s17 - 0.5f && near(X(17, 1), lw), "17: the word keeps %g (got %g), the neighbour %g from x %g", lw, W(17, 0), W(17, 1), X(17, 1));
    CHECK(near(W(18, 0), natural_w(kid(18, 0))), "18: preformatted text keeps its line: %g", W(18, 0));
    CHECK(near(W(19, 0), 30), "19: a max width of 30 on the nested word: %g", W(19, 0));
    CHECK(near(W(20, 0), 90), "20: a min width of 90 on the nested box: %g", W(20, 0));
    CHECK(near(W(23, 0), lw) && near(X(23, 1), lw), "23: a basis of 20 under a word of %g: %g, the next at %g", lw, W(23, 0), X(23, 1));
    CHECK(near(W(24, 0), lw) && kid(24, 1)->pos.y > 10, "24: the word on its own line keeps %g (got %g), the next item is below at y %g", lw, W(24, 0), kid(24, 1)->pos.y);
    CHECK(near(kid(25, 0)->pos.y, 0) && near(kid(25, 1)->pos.y, 0) && kid(25, 2)->pos.y > 10 && near(W(25, 2), lw), "25: ab and cd share a line, the word wraps to the next at its width (y %g, w %g)", kid(25, 2)->pos.y, W(25, 2));
    CHECK(near(W(26, 1), 300 - lw), "26: what grows gets 300 - %g: %g", lw, W(26, 1));
    CHECK(near(W(27, 0), 40), "27: the basis 20 is raised to the width 40, not to the word: %g", W(27, 0));
    CHECK(near(W(22, 0), lw + 12), "22: the word and its padding 5 and border 1: %g, wanted %g", W(22, 0), lw + 12);
    CHECK(near(W(21, 0), 70), "21: a nested item 70 wide asks for 70: %g", W(21, 0));
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    run();
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    static unsigned char px[50 * 4 * 4];
    memset(px, 200, sizeof px);
    g_pic = gp_image_from_rgba(app, 50, 4, px);
    gp_entity v = gp_view_new(app, 1, &(char){ 0 }, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "minwidth_test", .width = 640, .height = 900 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "minwidth_test: %d FAILURE(S)\n" : "minwidth_test: all checks passed\n", failures);
    return failures != 0;
}
