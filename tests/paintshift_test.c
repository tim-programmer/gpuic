/*
 * A cached view that only moves (a row in a scroller that is scrolled by whole pixels) is not painted again: its instances are moved. The frame it
 * gives must be the one a full paint gives, to the byte (the same view painted from scratch, with the painted copies dropped, is the oracle), for
 * text, gradients, rounded boxes, shadows and borders; it must not be shifted by a fraction of a pixel (text would land on another subpixel position),
 * nor when something below it is clipped to a box of its own (that clip moves with it and is not the one it inherits); and it must be counted as
 * a reuse (stat_paint_shifts). Needs a display, at scale 1.
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
            if (failures++ < 40) {                                                     \
                printf("FAIL %s:%d: ", __FILE__, __LINE__);                            \
                printf(__VA_ARGS__);                                                   \
                printf("\n");                                                          \
            }                                                                          \
        }                                                                              \
    } while (0)

static gp_app *app;
static gp_co *co_;
static gp_scroll g_scroll;
static gp_entity rows[8];
static gp_window *win(void) { return app->windows[0]; }

enum { PLAIN, CLIPPED };

static gp_node *row_view(void *self, gp_ctx *cx) {
    int kind = *(int *)self;
    const gp_color_stop st[2] = { { gp_rgb(0xff4040), 0 }, { gp_rgb(0x4040ff), 1 } };
    gp_node *inner = kind == CLIPPED ? gp_div(gp_w(120), gp_h(20), gp_overflow_hidden(), gp_bg(gp_rgb(0x335533)), gp_text("clipped text that is longer than its box", gp_text_color(gp_rgb(0xffffff))))
                                     : gp_text("a row of plain text 0123", gp_text_color(gp_rgb(0xffffff)));
    return gp_div(gp_flex_row(), gp_gap(8), gp_w(420), gp_h(34), gp_flex_none(), gp_px(6), gp_items_center(), gp_bg(gp_rgb(0x20243a)), gp_border(1), gp_border_color(gp_rgb(0x555a80)),
                  gp_div(gp_w(24), gp_h(24), gp_rounded(6), gp_bg_linear_gradient(45, st, 2), gp_shadow(0, 2, 4, gp_rgba(0x00000080))),
                  inner,
                  gp_div(gp_w(16), gp_h(16), gp_rounded_full(), gp_border(2), gp_border_color(gp_rgb(0xffcc00)), gp_border_dashed()));
}

static gp_node *view(void *self, gp_ctx *cx) {
    gp_node *kids[8];
    for (int i = 0; i < 8; i++) kids[i] = gp_child_view_cached(rows[i]);
    return gp_div(gp_size_full(), gp_flex_col(), gp_bg(gp_rgb(0x101018)),
                  gp_div(gp_flex_col(), gp_h(150), gp_w_full(), gp_flex_none(), gp_overflow_scroll(), gp_track_scroll(g_scroll), gp_children(kids, 8)));
}

static bool wait_draws(unsigned n) {
    unsigned long target = app->stat_draws + n;
    for (int i = 0; i < 1000 && app->stat_draws < target; i++) gp_sleep(co_, 0.004);
    return app->stat_draws >= target;
}
static uint8_t *frame(void) {
    gp_debug_capture_next(app);
    for (int i = 0; i < 1000 && !app->capture_px; i++) {
        win()->dirty_paint = true;
        gp_sleep(co_, 0.01);
    }
    return app->capture_px;
}

static unsigned long base_shifts, base_misses;   /* the counters before the last scroll: the frames drawn on the way count too */

static void drop_painted_copies(void) {
    gp_vviews *t = &win()->views;
    for (size_t i = 0; i < t->len; i++) t->v[i].chunk_valid = false;
}

/* the frame as the cache gives it, and then as a full paint gives it: how many bytes differ (-1: no frame) */
static long cache_vs_fresh(unsigned long *shifts, unsigned long *misses) {
    uint8_t *a = frame();
    if (!a) return -1;
    *shifts = app->stat_paint_shifts - base_shifts;
    *misses = app->stat_paint_misses - base_misses;
    size_t n = (size_t)app->capture_w * (size_t)app->capture_h * 3;
    uint8_t *copy = malloc(n);
    memcpy(copy, a, n);
    drop_painted_copies();
    uint8_t *b = frame();
    long diff = -1;
    if (b) {
        diff = 0;
        for (size_t i = 0; i < n; i++) diff += copy[i] != b[i];
    }
    free(copy);
    return diff;
}

static void scroll_to(float x, float y) {
    base_shifts = app->stat_paint_shifts;
    base_misses = app->stat_paint_misses;
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_scroll_to(&cx, g_scroll, x, y);
    gp_sleep(co_, 0.05);
}

static void run(void) {
    wait_draws(2);
    unsigned long shifts, misses;
    long diff = cache_vs_fresh(&shifts, &misses);   /* the first frames: everything painted, copies made, then reused */
    CHECK(diff == 0, "from the start the cached frame is the fresh one (%ld bytes differ)", diff);
    diff = cache_vs_fresh(&shifts, &misses);
    CHECK(diff == 0, "at rest the cached frame is the fresh one (%ld bytes differ)", diff);

    for (int k = 0; k < 2; k++) {
        const char *what = k == 0 ? "plain rows" : "rows with a clipped box";
        if (k == 1) {   /* the same, with every row holding an element clipped to its own box */
            for (int i = 0; i < 8; i++) *(int *)gp_entity_get(app, rows[i]) = CLIPPED;
            gp_app_notify(app);
            wait_draws(2);
            scroll_to(0, 0);
            frame();
            drop_painted_copies();
            frame();
        }
        /* painted copies exist now: scroll by whole pixels */
        for (int step = 1; step <= 5; step++) {
            float sx = (float)(step * 5), sy = (float)(step * 7);
            scroll_to(sx, sy);
            diff = cache_vs_fresh(&shifts, &misses);
            CHECK(diff == 0, "%s, scrolled to %g, %g: %ld bytes differ from a full paint", what, (double)sx, (double)sy, diff);
            if (k == 0) CHECK(shifts >= 3, "%s: the rows are moved, not painted again (%lu shifted, %lu painted)", what, shifts, misses);
            else CHECK(shifts == 0 && misses >= 3, "%s: a clip of its own below: painted again, not moved (%lu shifted, %lu painted)", what, shifts, misses);
            frame();   /* leaves the copies made again by the full paint */
        }
        /* along one axis only: the other stays where it was */
        scroll_to(30, 35);
        frame();
        scroll_to(40, 35);
        diff = cache_vs_fresh(&shifts, &misses);
        CHECK(diff == 0, "%s, moved sideways only: %ld bytes differ", what, diff);
        scroll_to(40, 50);
        diff = cache_vs_fresh(&shifts, &misses);
        CHECK(diff == 0, "%s, moved down only: %ld bytes differ", what, diff);
        /* a fraction of a pixel is not moved: its glyphs would sit on other subpixel positions */
        scroll_to(10.5f, 14);
        frame();
        diff = cache_vs_fresh(&shifts, &misses);
        CHECK(diff == 0, "%s, scrolled to 10.5, 14: %ld bytes differ", what, diff);
        scroll_to(10, 20.25f);
        diff = cache_vs_fresh(&shifts, &misses);
        CHECK(diff == 0 && shifts == 0, "%s, scrolled by a quarter pixel down: painted, never moved (%lu shifted, %ld differ)", what, shifts, diff);
        scroll_to(14.75f, 20.25f);
        diff = cache_vs_fresh(&shifts, &misses);
        CHECK(diff == 0 && shifts == 0, "%s, a fraction sideways too (%lu shifted, %ld differ)", what, shifts, diff);
        /* whole pixels from a place that is itself a fraction: the glyphs' subpixel positions stay what they were, so it can be moved */
        frame();
        scroll_to(16.75f, 23.25f);
        diff = cache_vs_fresh(&shifts, &misses);
        CHECK(diff == 0, "%s, moved by whole pixels from fractional places: %ld bytes differ", what, diff);
        if (k == 0) CHECK(shifts >= 3, "%s: ... and moved, not painted (%lu shifted)", what, shifts);
        frame();
        /* ... but a whole pixel one way and a fraction the other is painted */
        scroll_to(18.75f, 24.5f);
        diff = cache_vs_fresh(&shifts, &misses);
        CHECK(diff == 0 && shifts == 0, "%s, a whole pixel sideways and a fraction down (%lu shifted, %ld differ)", what, shifts, diff);
        scroll_to(0, 0);
        frame();
    }
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    run();
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    g_scroll = gp_scroll_new(app);
    for (int i = 0; i < 8; i++) {
        int init = PLAIN;
        rows[i] = gp_view_new(app, sizeof init, &init, row_view, NULL);
    }
    gp_entity v = gp_view_new(app, 1, &(char){ 0 }, view, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "paintshift_test", .width = 300, .height = 200 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "paintshift_test: %d FAILURE(S)\n" : "paintshift_test: all checks passed\n", failures);
    return failures != 0;
}
