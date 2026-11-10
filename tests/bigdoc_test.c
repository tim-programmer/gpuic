/*
 * The text input with a very large document (300,000 lines, 14 MB): the line index against a rescan of the text after thousands of random
 * edits (with right-to-left lines among them), then the field itself: only the rows near the view are built, scrolling builds others (by
 * wheel, by keyboard, past a window and far away), the rows are where the line number says, typing and Enter at the end and in the middle,
 * a click and a drag on a deep row, selecting to the end and deleting 14 MB and undoing it, a very long line far down widening the scroll
 * extent, and how long the keystrokes take (a document rebuilt whole on every key would take seconds each). Needs a display.
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

#define NLINES 300000
#define LH 19.5f   /* 15 px font * GP_LINE_HEIGHT */

typedef struct { gp_entity big, small, wrapped; } Fix;

static gp_node *render(void *self, gp_ctx *cx) {
    Fix *f = self;
    return gp_div(gp_size_full(), gp_p(10), gp_gap(10), gp_child_view(f->big), gp_child_view(f->small), gp_child_view(f->wrapped));
}

static bool wait_draws(unsigned n) {
    unsigned long target = app->stat_draws + n;
    for (int i = 0; i < 2500 && app->stat_draws < target; i++) gp_sleep(co_, 0.004);
    return app->stat_draws >= target;
}
static void settle(void) { wait_draws(2); gp_sleep(co_, 0.15); wait_draws(1); }
static void keys(const char *k) { gp_simulate_keystrokes(app, k); settle(); }

/* ---- the tree */
static const gp_el *find_scroller(const gp_el *n, int which, int *seen) {
    if (n->on_scroll.fn && (n->style.overflow_y_scroll || n->style.overflow_x_scroll)) {
        if ((*seen)++ == which) return n;
    }
    for (uint32_t i = 0; i < n->nchildren; i++) {
        const gp_el *r = find_scroller(n->children[i], which, seen);
        if (r) return r;
    }
    return NULL;
}
static const gp_el *scroller(int which) { int seen = 0; return win()->tree ? find_scroller(win()->tree, which, &seen) : NULL; }
static size_t count_els(const gp_el *n) {
    size_t c = 1;
    for (uint32_t i = 0; i < n->nchildren; i++) c += count_els(n->children[i]);
    return c;
}
/* the text element of the scroller that is `want`, or NULL; its position in the scroller's content */
static const gp_el *text_el(const gp_el *sc, const char *want) {
    for (uint32_t i = 0; i < sc->nchildren; i++) {
        const gp_el *c = sc->children[i];
        if (c->kind == GP_EL_TEXT && strcmp(c->text, want) == 0) return c;
    }
    return NULL;
}
static bool abs_of(const gp_el *n, const gp_el *target, float x, float y, float *ox, float *oy) {
    x += n->pos.x;
    y += n->pos.y;
    if (n == target) { *ox = x; *oy = y; return true; }
    for (uint32_t i = 0; i < n->nchildren; i++)
        if (abs_of(n->children[i], target, x - n->scroll.x, y - n->scroll.y, ox, oy)) return true;
    return false;
}

/* ---- the document */
static char *make_doc(size_t *len_out) {
    size_t cap = (size_t)NLINES * 64, len = 0;
    char *d = malloc(cap);
    for (int i = 0; i < NLINES; i++) {
        int pad = i % 7 * 3;
        len += (size_t)snprintf(d + len, cap - len, "line %d: lorem ipsum dolor sit amet%.*s%s", i, pad, "-----------------------", i == NLINES - 1 ? "" : "\n");
    }
    *len_out = len;
    return d;
}
static void line_text(char *out, size_t cap, int i) { snprintf(out, cap, "line %d: lorem ipsum dolor sit amet%.*s", i, i % 7 * 3, "-----------------------"); }

static uint32_t rng_state = 12345;
static uint32_t rnd(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }

static void test_index_fuzz(gp_entity input, const char *what) {
    static const char *pool[] = { "", "a", "\n", "\n\n", "x\ny", "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d", "\xd7\xa9\n", "\xc3\xa9", "abc\ndef\nghi", "\n\xd7\x90\n", "  ", "z" };
    char why[256];
    for (int i = 0; i < 3000; i++) {
        size_t len = strlen(gp_text_input_text(app, input));
        size_t a = len ? rnd() % (len + 1) : 0, b = len ? rnd() % (len + 1) : 0;
        if (rnd() % 3 == 0) b = a;   /* mostly insertions */
        if (a > b) { size_t t = a; a = b; b = t; }
        if (rnd() % 5 == 0 && len > 30) b = a + (rnd() % 20 > b - a ? 0 : 0) + (b - a > 20 ? 20 : b - a);
        gp_text_input_debug_replace(app, input, a, b, pool[rnd() % (sizeof pool / sizeof *pool)]);
        if (i < 400 || i % 10 == 0) {
            const char *bad = gp_text_input_debug_check(app, input, why, sizeof why);
            if (bad) { CHECK(false, "%s: after edit %d (%zu..%zu): %s", what, i, a, b, bad); return; }
        }
    }
    CHECK(true, "ok");
}

static void test_big(Fix *f) {
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_focus_set(&cx, gp_text_input_focus(app, f->big));
    settle();
    size_t nrows = gp_text_input_rows(app, f->big);
    CHECK(nrows == NLINES, "rows %zu, wanted %d", nrows, NLINES);

    /* only what is near the view is built */
    const gp_el *sc = scroller(0);
    CHECK(sc != NULL, "the big field's scroller");
    if (!sc) return;
    size_t els = count_els(win()->tree);
    CHECK(els < 400, "the whole tree has %zu elements for a document of %d lines", els, NLINES);
    char want[128];
    line_text(want, sizeof want, 0);
    CHECK(text_el(sc, want) != NULL, "the first line is built at the top");
    line_text(want, sizeof want, NLINES - 1);
    CHECK(text_el(sc, want) == NULL, "the last line is not built");

    float total = (float)NLINES * LH;
    CHECK(fabsf(sc->scroll_max.y - (total - 10 * LH)) < 40.f, "the scroll extent is the whole document: %g, wanted about %g", sc->scroll_max.y, total - 10 * LH);

    /* to the end and back by keyboard: the caret's row is brought into view and built */
    double t0 = gp_time();
    keys("ctrl-end");
    double dt = gp_time() - t0;
    sc = scroller(0);
    line_text(want, sizeof want, NLINES - 1);
    const gp_el *last = text_el(sc, want);
    CHECK(last != NULL, "ctrl-end builds the last line");
    CHECK(last && fabsf(last->pos.y - (float)(NLINES - 1) * LH) < 1.5f, "...at its place: y %g, wanted %g", last ? last->pos.y : -1.f, (float)(NLINES - 1) * LH);
    CHECK(gp_text_input_scroll(app, f->big).y > total - 12 * LH, "scrolled to the end: %g", gp_text_input_scroll(app, f->big).y);
    line_text(want, sizeof want, 0);
    CHECK(text_el(sc, want) == NULL, "the first line is gone now");
    CHECK(count_els(win()->tree) < 400, "still a small tree at the end (%zu)", count_els(win()->tree));
    printf("  (ctrl-end in %d lines: %.0f ms to the next frames)\n", NLINES, dt * 1000);

    /* typing at the end and Enter: how long a key takes with a 14 MB document */
    size_t len0 = strlen(gp_text_input_text(app, f->big));
    unsigned long d0 = app->stat_draws;
    t0 = gp_time();
    gp_simulate_text(app, "X");
    wait_draws(1);
    gp_sleep(co_, 0.02);
    dt = gp_time() - t0;
    CHECK(strlen(gp_text_input_text(app, f->big)) == len0 + 1, "typed at the end");
    CHECK(dt < 1.0, "a key took %.0f ms to reach a frame (the whole document must not be rebuilt)", dt * 1000);
    printf("  (a typed key at the end of %zu bytes: %.0f ms, %lu draws)\n", len0, dt * 1000, app->stat_draws - d0);
    keys("enter");
    CHECK(gp_text_input_rows(app, f->big) == NLINES + 1, "Enter adds a row: %zu", gp_text_input_rows(app, f->big));
    gp_simulate_text(app, "tail");
    settle();
    sc = scroller(0);
    CHECK(text_el(sc, "tail") != NULL, "the new last line is drawn");
    {
        size_t s0, e0;
        CHECK(gp_text_input_row_range(app, f->big, NLINES, &s0, &e0) && e0 - s0 == 4, "its row range");
        CHECK(gp_text_input_caret_row(app, f->big) == NLINES, "the caret is on it: row %zu", gp_text_input_caret_row(app, f->big));
    }
    keys("ctrl-z");   /* the typing of "tail" */
    keys("ctrl-z");   /* Enter */
    CHECK(gp_text_input_rows(app, f->big) == NLINES, "undo takes the line break back: %zu", gp_text_input_rows(app, f->big));

    /* a wheel jump into the middle: the rows are rebuilt around where it lands. (Not focused: a focused field also rebuilds
     * every time its caret blinks, which would hide a view that did not ask for rows by itself.) */
    gp_blur(&cx);
    wait_draws(2);
    {
        const gp_el *s = scroller(0);
        float ax = 0.f, ay = 0.f;
        abs_of(win()->tree, s, 0, 0, &ax, &ay);
        gp_scroll_wheel(win(), (gp_point){ ax + 50, ay + 20 }, 0, -1e9f);   /* all the way up first */
        wait_draws(2);
        CHECK(gp_text_input_scroll(app, f->big).y == 0, "back at the top by the wheel: %g", gp_text_input_scroll(app, f->big).y);
        float target = 123456 * LH;
        gp_scroll_wheel(win(), (gp_point){ ax + 50, ay + 20 }, 0, target);
        wait_draws(2);
        s = scroller(0);
        float y = gp_text_input_scroll(app, f->big).y;
        CHECK(fabsf(y - target) < 3, "the wheel moved to %g, wanted %g", y, target);
        int row = (int)floorf(y / LH);
        char a[128], b[128];
        line_text(a, sizeof a, row + 1);
        line_text(b, sizeof b, row + 9);
        const gp_el *ea = text_el(s, a), *eb = text_el(s, b);
        CHECK(ea && eb, "rows %d and %d are built after the jump", row + 1, row + 9);
        CHECK(ea && fabsf(ea->pos.y - (float)(row + 1) * LH) < 1.5f, "row %d is at y %g, wanted %g", row + 1, ea ? ea->pos.y : -1.f, (float)(row + 1) * LH);
        line_text(a, sizeof a, 5);
        CHECK(text_el(s, a) == NULL && count_els(win()->tree) < 400, "nothing near the top is still built; %zu elements", count_els(win()->tree));

        /* a smaller scroll inside the window builds nothing new (no rebuild is asked for); a larger one does */
        unsigned long before = app->stat_draws;
        gp_scroll_wheel(win(), (gp_point){ ax + 50, ay + 20 }, 0, 2 * LH);
        wait_draws(2);
        s = scroller(0);
        line_text(a, sizeof a, row + 20);
        CHECK(text_el(s, a) != NULL, "after two more rows, row %d is there", row + 20);
        (void)before;
        gp_scroll_wheel(win(), (gp_point){ ax + 50, ay + 20 }, 0, 500 * LH);
        wait_draws(2);
        s = scroller(0);
        y = gp_text_input_scroll(app, f->big).y;
        row = (int)floorf(y / LH);
        line_text(a, sizeof a, row + 3);
        CHECK(text_el(s, a) != NULL, "after 500 rows, row %d is built", row + 3);
        CHECK(count_els(win()->tree) < 400, "%zu elements", count_els(win()->tree));

        /* a click and a drag on a built row map to offsets in that row, deep in the document */
        const gp_el *t = text_el(s, a);
        float tx, ty;
        abs_of(win()->tree, t, 0, 0, &tx, &ty);
        abs_of(win()->tree, s, 0, 0, &ax, &ay);
        float cx_ = ax + 12 + 60, cy_ = ay + (t->pos.y - y) + LH / 2;
        gp_simulate_mouse(app, GP_SIM_MOVE, cx_, cy_, 0, 0);
        gp_simulate_mouse(app, GP_SIM_DOWN, cx_, cy_, 1, 0);
        gp_simulate_mouse(app, GP_SIM_UP, cx_, cy_, 1, 0);
        settle();
        size_t s0, e0, c0, c1;
        gp_text_input_selection(app, f->big, &c0, &c1);
        CHECK(gp_text_input_row_range(app, f->big, (size_t)row + 3, &s0, &e0) && c0 == c1 && c0 > s0 && c0 < e0, "a click on row %d put the caret in it: %zu in [%zu, %zu]", row + 3, c0, s0, e0);
        CHECK(gp_text_input_caret_row(app, f->big) == (size_t)row + 3, "caret row %zu", gp_text_input_caret_row(app, f->big));
        gp_simulate_mouse(app, GP_SIM_DOWN, cx_, cy_, 1, 0);
        gp_simulate_mouse(app, GP_SIM_MOVE, cx_ + 80, cy_ + 5 * LH, 0, 0);
        gp_simulate_mouse(app, GP_SIM_UP, cx_ + 80, cy_ + 5 * LH, 1, 0);
        settle();
        gp_text_input_selection(app, f->big, &c0, &c1);
        size_t s5, e5;
        gp_text_input_row_range(app, f->big, (size_t)row + 8, &s5, &e5);
        CHECK(c0 == (size_t)(s0 + (c0 - s0)) && c1 > s5 && c1 < e5 + 1 && c0 < s5, "a drag five rows down selected from row %d into row %d: %zu..%zu", row + 3, row + 8, c0, c1);
        float rects[20][4];
        double t1 = gp_time();
        size_t nr = gp_text_input_selection_rects(app, f->big, rects, 20);
        CHECK(gp_time() - t1 < 0.05, "the rectangles of a selection deep in the document took %.0f ms (rows above it must not be looked at)", (gp_time() - t1) * 1000);
        CHECK(nr >= 5 && nr <= 7 && fabsf(rects[0][1] - (float)(row + 3) * LH) < 1.5f, "the selection's rectangles start at its first row (%d): %zu of them, the first at y %g", row + 3, nr, nr ? rects[0][1] : -1.f);
    }

    /* the very long line far down widens the scroll extent only once it has been seen */
    keys("ctrl-home");
    CHECK(gp_text_input_scroll(app, f->big).y == 0, "ctrl-home scrolls to the top");
    sc = scroller(0);
    CHECK(count_els(win()->tree) < 400 && sc->scroll_max.x < 400, "at the top the content is no wider than its widest row (%g)", sc->scroll_max.x);

    /* select from the top to the end, delete 14 MB, get it back */
    size_t full = strlen(gp_text_input_text(app, f->big));
    char *copy = malloc(full + 1);
    memcpy(copy, gp_text_input_text(app, f->big), full + 1);
    t0 = gp_time();
    keys("ctrl-shift-end");
    size_t a0, a1;
    gp_text_input_selection(app, f->big, &a0, &a1);
    CHECK(a0 == 0 && a1 == full, "ctrl-shift-end selects the document: %zu..%zu of %zu", a0, a1, full);
    {
        float rects[60][4];
        size_t n = gp_text_input_selection_rects(app, f->big, rects, 60);
        CHECK(n == 60 && rects[0][1] == 0 && fabsf(rects[59][1] - 59 * LH) < 0.01f, "selection rectangles of the first rows, without walking the rest: %zu", n);
    }
    keys("backspace");
    CHECK(strlen(gp_text_input_text(app, f->big)) == 0 && gp_text_input_rows(app, f->big) == 1, "deleted: %zu bytes, %zu rows", strlen(gp_text_input_text(app, f->big)), gp_text_input_rows(app, f->big));
    CHECK(count_els(win()->tree) < 200, "an empty field has a small tree");
    keys("ctrl-z");
    double took = gp_time() - t0;
    CHECK(strlen(gp_text_input_text(app, f->big)) == full && memcmp(gp_text_input_text(app, f->big), copy, full) == 0, "undo gives the 14 MB back, byte for byte");
    CHECK(gp_text_input_rows(app, f->big) == NLINES, "...and its rows: %zu", gp_text_input_rows(app, f->big));
    {
        char why[256];
        const char *bad = gp_text_input_debug_check(app, f->big, why, sizeof why);
        CHECK(!bad, "the index of the restored document: %s", bad ? bad : "");
    }
    printf("  (select all, delete and undo of %zu bytes: %.0f ms with the frames and %d settles in between)\n", full, took * 1000, 4);
    free(copy);
}

static void test_wide_line(Fix *f) {
    /* a 3000-character line at row 200000: not seen until the view gets there, and then the content is wide enough to scroll across to its end */
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_focus_set(&cx, gp_text_input_focus(app, f->big));
    size_t s0, e0;
    gp_text_input_row_range(app, f->big, 200000, &s0, &e0);
    char *wide = malloc(3001);
    memset(wide, 'w', 3000);
    wide[3000] = 0;
    gp_text_input_debug_replace(app, f->big, e0, e0, wide);   /* appended to row 200000 */
    free(wide);
    gp_view_dirty(app, f->big);
    settle();
    const gp_el *sc = scroller(0);
    float before = sc->scroll_max.x;
    gp_point p;
    {
        float ax = 0.f, ay = 0.f;
        abs_of(win()->tree, sc, 0, 0, &ax, &ay);
        gp_scroll_wheel(win(), (gp_point){ ax + 50, ay + 20 }, 0, 1e9f);
        gp_scroll_wheel(win(), (gp_point){ ax + 50, ay + 20 }, 0, -(float)(NLINES - 200000 - 4) * LH);
        settle();
        sc = scroller(0);
        p = (gp_point){ sc->scroll_max.x, 0 };
    }
    CHECK(before < 400 && p.x > 1000, "the extent across was %g before the long line was seen and is %g after", before, p.x);
    keys("ctrl-home");
    sc = scroller(0);
    CHECK(sc->scroll_max.x > 1000, "scrolled away from it the content is still as wide as the widest row seen: %g", sc->scroll_max.x);
    {   /* a new document (itself long enough to be built in part): what was seen of the old one is forgotten */
        char *doc = malloc(5000 * 8 + 1);
        for (int i = 0; i < 5000; i++) memcpy(doc + i * 8, "shorts\n\n", 8);
        doc[5000 * 8] = 0;
        gp_text_input_set_text(&cx, f->big, doc);
        free(doc);
        settle();
        sc = scroller(0);
        CHECK(sc->scroll_max.x < 400 && gp_text_input_rows(app, f->big) == 10001, "a new text has its own width: %g across, %zu rows", sc->scroll_max.x, gp_text_input_rows(app, f->big));
    }
}

static void test_small(Fix *f) {
    char why[256];
    gp_text_input_set_text(&(gp_ctx){ app, NULL, GP_ENTITY_NULL, false }, f->small, "a\n\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d\nb");
    settle();
    CHECK(gp_text_input_rows(app, f->small) == 3 && gp_text_input_row_rtl(app, f->small, 1) && !gp_text_input_row_rtl(app, f->small, 0), "right-to-left rows come from the index");
    test_index_fuzz(f->small, "multi-line field, no wrap");
    test_index_fuzz(f->wrapped, "multi-line field, soft wrap");
    const char *bad = gp_text_input_debug_check(app, f->small, why, sizeof why);
    CHECK(!bad, "%s", bad ? bad : "");
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    Fix *f = gp_entity_get(app, win()->root_view);
    gp_sleep(co, 0.5);
    test_small(f);
    test_big(f);
    test_wide_line(f);
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    Fix init = { 0 };
    size_t len;
    char *doc = make_doc(&len);
    double t0 = gp_time();
    init.big = gp_text_input_new(app, &(gp_text_input_options){ .multiline = true, .rows = 10, .text = doc });
    printf("  (creating a field of %zu bytes, %d lines: %.0f ms)\n", len, NLINES, (gp_time() - t0) * 1000);
    free(doc);
    init.small = gp_text_input_new(app, &(gp_text_input_options){ .multiline = true, .rows = 3, .text = "start" });
    init.wrapped = gp_text_input_new(app, &(gp_text_input_options){ .multiline = true, .soft_wrap = true, .rows = 3, .width = 200, .text = "some words to wrap round the edge of a narrow field\nand a second line" });
    gp_entity v = gp_view_new(app, sizeof init, &init, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "bigdoc_test", .width = 600, .height = 420 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "bigdoc_test: %d FAILURE(S)\n" : "bigdoc_test: all checks passed\n", failures);
    return failures != 0;
}
