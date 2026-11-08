/*
 * Touch: a tap is a click, a drag scrolls the content with the finger at once (even with smooth scrolling on), a flick keeps going and slows
 * with a time constant of 0.35 s (a flick of v px/s travels v * 0.35 px: checked against that, in one big step of the clock and in many
 * small ones, which agree because each step is exact), it stops at the end of the content, a new touch or a wheel turn stops it, a finger that
 * stopped before it lifted or moved slowly does not fling, and what the innermost scroller cannot take goes on to the one around it. Times
 * of the finger events are given by the test (gp_simulate_touch), the clock of the fling is jumped. Needs a display.
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
static gp_window *win(void) { return app->windows[0]; }

typedef struct { gp_scroll sa, so, si, sc; int clicks, last_row; } Fix;
#define RED gp_rgb(0xc03030)
#define BLUE gp_rgb(0x3030c0)

static void on_row(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    Fix *f = self;
    f->clicks++;
    f->last_row = (int)(intptr_t)data;
}

static gp_node *render(void *self, gp_ctx *cx) {
    Fix *f = self;
    gp_node **a = gp_frame_alloc(40 * sizeof *a), **c = gp_frame_alloc(40 * sizeof *c);
    for (int i = 0; i < 40; i++) {
        a[i] = gp_div(gp_h(50), gp_flex_none(), gp_bg(i % 2 ? RED : BLUE), gp_on_click_data(on_row, (void *)(intptr_t)i));
        c[i] = gp_div(gp_w(50), gp_flex_none(), gp_bg(i % 2 ? RED : BLUE));
    }
    return gp_div(
        gp_size_full(), gp_flex_row(), gp_items_start(), gp_gap(20), gp_p(10),
        gp_div(gp_w(100), gp_h(200), gp_overflow_y_scroll(), gp_track_scroll(f->sa), gp_children(a, 40)),
        gp_div(gp_w(100), gp_h(200), gp_overflow_y_scroll(), gp_track_scroll(f->so),
               gp_div(gp_w(100), gp_h(100), gp_flex_none(), gp_overflow_y_scroll(), gp_track_scroll(f->si), gp_div(gp_h(150), gp_flex_none(), gp_bg(RED))),
               gp_div(gp_h(2000), gp_flex_none(), gp_bg(BLUE))),
        gp_div(gp_w(200), gp_h(60), gp_flex_row(), gp_overflow_x_scroll(), gp_track_scroll(f->sc), gp_children(c, 40)));
}

static int64_t skew;
static void jump(double s) { skew += (int64_t)(s * 1e9); gp_debug_clock_skew(skew); }
static bool wait_draws(unsigned n) {
    unsigned long target = app->stat_draws + n;
    for (int i = 0; i < 125 && app->stat_draws < target; i++) gp_sleep(co_, 0.004);   /* (half a second at most: a frame may not be wanted) */
    return app->stat_draws >= target;
}
static gp_ctx cx_;
static float offy(gp_scroll s) { return gp_scroll_offset(&cx_, s).y; }
static float offx(gp_scroll s) { return gp_scroll_offset(&cx_, s).x; }
static bool near(float a, float b, float tol) { return fabsf(a - b) <= tol; }
static void touch(int phase, float x, float y, double t) { gp_simulate_touch(app, phase, x, y, t); }

/* a finger moving along y from y0 to y1 in `n` steps of 10 ms, starting at time t0; returns the time of the last sample */
static double drag(float x, float y0, float y1, int n, double t0) {
    touch(GP_TOUCH_DOWN, x, y0, t0);
    for (int i = 1; i <= n; i++) touch(GP_TOUCH_MOVE, x, y0 + (y1 - y0) * (float)i / (float)n, t0 + 0.01 * i);
    return t0 + 0.01 * n;
}
static void reset(Fix *f) {
    gp_scroll_to(&cx_, f->sa, 0, 0);
    gp_scroll_to(&cx_, f->so, 0, 0);
    gp_scroll_to(&cx_, f->si, 0, 0);
    gp_scroll_to(&cx_, f->sc, 0, 0);
    wait_draws(2);
}

static void run(Fix *f) {
    wait_draws(2);
    cx_ = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_set_smooth_scrolling(app, 100);   /* touch must not glide: the content follows the finger */

    /* a tap is a click on what is under it; a few pixels of wobble are still a tap */
    f->clicks = 0;
    touch(GP_TOUCH_DOWN, 60, 80, 0);
    touch(GP_TOUCH_UP, 60, 80, 0.05);
    wait_draws(1);
    CHECK(f->clicks == 1 && f->last_row == 1, "a tap clicks the row under it (row 1 at y 80 of rows 50 high): %d clicks, row %d", f->clicks, f->last_row);
    touch(GP_TOUCH_DOWN, 60, 130, 1);
    touch(GP_TOUCH_MOVE, 63, 126, 1.01);
    touch(GP_TOUCH_UP, 63, 126, 1.05);
    wait_draws(1);
    CHECK(f->clicks == 2 && offy(f->sa) == 0, "a wobble of 5 px is still a tap (%d clicks, offset %g)", f->clicks, offy(f->sa));

    /* a slow drag: the content moves with the finger, at once, from where the finger was when it became a drag */
    touch(GP_TOUCH_DOWN, 60, 190, 2);
    touch(GP_TOUCH_MOVE, 60, 180, 2.01);   /* 10 px: over the slop, so a drag; this move itself does not scroll */
    CHECK(offy(f->sa) == 0, "the move that makes it a drag does not scroll (%g)", offy(f->sa));
    touch(GP_TOUCH_MOVE, 60, 140, 2.5);
    CHECK(near(offy(f->sa), 40, 0.01f), "a finger 40 px up: the content is 40 px further on, immediately, with smooth scrolling on: %g", offy(f->sa));
    touch(GP_TOUCH_MOVE, 60, 160, 3.0);
    CHECK(near(offy(f->sa), 20, 0.01f), "and back 20 px: %g", offy(f->sa));
    touch(GP_TOUCH_UP, 60, 160, 3.01);
    wait_draws(2);
    gp_sleep(co_, 0.2);
    CHECK(near(offy(f->sa), 20, 0.01f) && !win()->fling.active, "a slow drag does not fling (offset %g)", offy(f->sa));
    int clicks = f->clicks;
    CHECK(clicks == 2, "...and is not a click");
    touch(GP_TOUCH_MOVE, 60, 100, 3.5);   /* a move after the finger has lifted: nothing is touching */
    touch(GP_TOUCH_MOVE, 60, 40, 3.6);
    CHECK(near(offy(f->sa), 20, 0.01f), "moves without a finger down do nothing (%g)", offy(f->sa));

    /* a flick of 1000 px/s: 90 px of drag, then v * tau = 350 px more, in one big step of the clock */
    reset(f);
    double t = drag(60, 190, 90, 10, 10);
    touch(GP_TOUCH_UP, 60, 90, t + 0.005);
    CHECK(near(offy(f->sa), 90, 0.01f) && win()->fling.active, "a flick: dragged 90 px, flinging (%g, %d)", offy(f->sa), win()->fling.active);
    CHECK(near(win()->fling.vy, 1000, 5), "its speed is the finger's, against it: %g px/s", win()->fling.vy);
    wait_draws(2);   /* the first step is a nominal 1/60 s */
    jump(10);
    wait_draws(3);
    CHECK(near(offy(f->sa), 90 + 350, 3) && !win()->fling.active, "it travels v * tau = 350 px and stops: offset %g, wanted 440", offy(f->sa));
    {
        unsigned long d0 = app->stat_draws;
        gp_sleep(co_, 0.3);
        CHECK(app->stat_draws - d0 <= 1, "no frames once it has stopped (%lu)", app->stat_draws - d0);
    }

    /* the same flick in small steps of the clock lands in the same place: each step is exact */
    reset(f);
    t = drag(60, 190, 90, 10, 20);
    touch(GP_TOUCH_UP, 60, 90, t + 0.005);
    wait_draws(2);
    for (int i = 0; i < 40 && win()->fling.active; i++) { jump(0.05); wait_draws(1); }
    CHECK(near(offy(f->sa), 440, 4) && !win()->fling.active, "in steps of 50 ms: %g", offy(f->sa));

    /* a faster flick goes further, in proportion */
    reset(f);
    t = drag(60, 190, 10, 6, 30);   /* 180 px in 60 ms: 3000 px/s, 180 px of drag */
    touch(GP_TOUCH_UP, 60, 10, t + 0.005);
    wait_draws(2);
    jump(10);
    wait_draws(3);
    CHECK(near(offy(f->sa), 150 + 3000 * 0.35f, 6), "3000 px/s: offset %g, wanted %g (the first move only makes it a drag: 150 px of content)", offy(f->sa), 150 + 3000 * 0.35f);

    /* the speed is that of the last 100 ms, not the average of the drag: slow for a quarter of a second, then 1000 px/s */
    reset(f);
    touch(GP_TOUCH_DOWN, 60, 200, 45);
    for (int i = 1; i <= 50; i++) touch(GP_TOUCH_MOVE, 60, 200 - (float)i, 45 + 0.005 * i);            /* 200 px/s: 50 px */
    for (int i = 1; i <= 10; i++) touch(GP_TOUCH_MOVE, 60, 150 - 10 * (float)i, 45.25 + 0.01 * i);    /* 1000 px/s: 100 px */
    touch(GP_TOUCH_UP, 60, 50, 45.355);
    CHECK(win()->fling.active && near(win()->fling.vy, 1000, 60), "the flick at the end of a slow drag has the speed of the end: %g", win()->fling.vy);
    wait_draws(2);
    jump(10);
    wait_draws(3);
    CHECK(near(offy(f->sa), 150 - 10 + 350, 12), "and goes %g px, not what the average speed would give", offy(f->sa));

    /* a finger that stopped before it lifted, and one that moved slowly, do not fling */
    reset(f);
    t = drag(60, 190, 90, 10, 40);
    touch(GP_TOUCH_UP, 60, 90, t + 0.2);
    CHECK(!win()->fling.active && near(offy(f->sa), 90, 0.01f), "held still for 200 ms and then lifted: no fling");
    reset(f);
    t = drag(60, 190, 160, 30, 50);   /* 100 px/s */
    touch(GP_TOUCH_UP, 60, 160, t + 0.005);
    CHECK(!win()->fling.active, "100 px/s is below the threshold");

    /* a new touch and a wheel turn stop a fling */
    reset(f);
    t = drag(60, 190, 90, 10, 60);
    touch(GP_TOUCH_UP, 60, 90, t + 0.005);
    CHECK(win()->fling.active, "(flinging)");
    touch(GP_TOUCH_DOWN, 60, 100, t + 0.05);
    CHECK(!win()->fling.active, "a finger down stops it");
    touch(GP_TOUCH_UP, 60, 100, t + 0.06);
    reset(f);
    t = drag(60, 190, 90, 10, 70);
    touch(GP_TOUCH_UP, 60, 90, t + 0.005);
    gp_scroll_wheel(win(), (gp_point){ 60, 100 }, 0, 10);
    CHECK(!win()->fling.active, "a wheel turn stops it too");

    /* the end of the content stops it */
    reset(f);
    gp_scroll_to(&cx_, f->sa, 0, 1700);
    wait_draws(2);
    t = drag(60, 190, 90, 10, 80);
    touch(GP_TOUCH_UP, 60, 90, t + 0.005);
    wait_draws(2);
    jump(0.3);   /* (the speed is still about 400 px/s: it is the end that stops it) */
    wait_draws(3);
    CHECK(near(offy(f->sa), 1800, 0.01f) && !win()->fling.active, "a fling against the end ends there, at once: %g, active %d", offy(f->sa), win()->fling.active);

    /* sideways, in a row */
    reset(f);
    touch(GP_TOUCH_DOWN, 420, 40, 90);
    for (int i = 1; i <= 10; i++) touch(GP_TOUCH_MOVE, 420 - 10 * (float)i, 40, 90 + 0.01 * i);
    touch(GP_TOUCH_UP, 320, 40, 90.105);
    CHECK(near(offx(f->sc), 90, 0.01f) && offy(f->sc) == 0 && win()->fling.active, "a drag along a row scrolls it sideways (%g)", offx(f->sc));
    wait_draws(2);
    jump(10);
    wait_draws(3);
    CHECK(near(offx(f->sc), 440, 3), "and a flick goes on: %g", offx(f->sc));

    /* what the inner scroller cannot take goes to the one around it: the inner one has 50 px, then the outer moves */
    reset(f);
    t = drag(180, 105, 15, 10, 100);   /* inside the inner scroller (it is at y 10..110) */
    (void)t;
    touch(GP_TOUCH_UP, 180, 15, 100.105);
    wait_draws(2);
    jump(10);
    wait_draws(3);
    CHECK(near(offy(f->si), 50, 0.01f) && offy(f->so) > 300, "inner scroller to its end (%g), the rest to the outer one (%g)", offy(f->si), offy(f->so));

    gp_set_smooth_scrolling(app, 0);
    gp_debug_clock_skew(0);
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    run(gp_entity_get(app, win()->root_view));
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    Fix init;
    memset(&init, 0, sizeof init);
    init.sa = gp_scroll_new(app); init.so = gp_scroll_new(app); init.si = gp_scroll_new(app); init.sc = gp_scroll_new(app);
    gp_entity v = gp_view_new(app, sizeof init, &init, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "touch_test", .width = 500, .height = 260 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "touch_test: %d FAILURE(S)\n" : "touch_test: all checks passed\n", failures);
    return failures != 0;
}
