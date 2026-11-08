/*
 * gp_on_wheel: the innermost element under the pointer that has a handler gets the wheel, before any scroller, and uses it up; gp_propagate passes it to the next handler under the
 * pointer and, after the last, to scrolling; the event carries the pointer (window and element coordinates, through a transform too), the turn in pixels the way scroll offsets
 * grow, shift + wheel as dx, and the modifiers; on a curve only the stroke counts. Needs a display.
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
typedef struct { gp_scroll sc; } Fix;
static gp_ctx cx_;

#define MAXC 16
static struct { int id; gp_wheel_event ev; } calls[MAXC];
static int ncalls;
static bool propagate_ids[16];
static void on_wheel(void *self, gp_ctx *cx, const gp_wheel_event *ev, void *data) {
    int id = (int)(intptr_t)data;
    if (ncalls < MAXC) { calls[ncalls].id = id; calls[ncalls].ev = *ev; }
    ncalls++;
    if (propagate_ids[id]) gp_propagate(cx);
}
#define WH(id) gp_on_wheel_data(on_wheel, (void *)(intptr_t)(id))

static const gp_point cable[] = { { 300, 40 }, { 380, 40 } };

static gp_node *render(void *self, gp_ctx *cx) {
    Fix *f = self;
    return gp_div(
        gp_size_full(), gp_bg(gp_rgb(0x101018)),
        /* a scroller with three kinds of content */
        gp_div(gp_absolute(), gp_left(10), gp_top(10), gp_w(120), gp_h(200), gp_overflow_y_scroll(), gp_track_scroll(f->sc),
               gp_div(gp_h(60), gp_flex_none(), gp_bg(gp_rgb(0x305030)), WH(1)),                /* uses it up */
               gp_div(gp_h(60), gp_flex_none(), gp_bg(gp_rgb(0x503030)), WH(2)),                /* passes it on */
               gp_div(gp_h(1000), gp_flex_none(), gp_bg(gp_rgb(0x303050)))),                     /* no handler */
        /* nested handlers, the inner passing it on, the outer using it up */
        gp_div(gp_absolute(), gp_left(150), gp_top(10), gp_w(120), gp_h(100), gp_bg(gp_rgb(0x404040)), WH(3),
               gp_div(gp_absolute(), gp_left(20), gp_top(20), gp_w(60), gp_h(40), gp_bg(gp_rgb(0x808080)), WH(4))),
        /* a handler that lets everything through to the scroller around it, and a scroller with a handler of its own on top */
        /* a cable over a box that has a handler */
        gp_div(gp_absolute(), gp_left(290), gp_top(20), gp_w(100), gp_h(40), gp_bg(gp_rgb(0x404060)), WH(6)),
        gp_polyline(cable, 2, gp_stroke_width(8), gp_stroke_color(gp_rgb(0xff0000)), WH(5)),
        /* under a quarter turn */
        gp_div(gp_absolute(), gp_left(150), gp_top(150), gp_w(100), gp_h(40), gp_rotate(90), gp_bg(gp_rgb(0x205050)), WH(7)));
}

static void settle(void) { gp_sleep(co_, 0.08); }
static void wheel(float x, float y, float dx, float dy, uint32_t mods) {
    ncalls = 0;
    gp_simulate_wheel(app, x, y, dx, dy, mods);
    settle();
}
static bool near(float a, float b) { return fabsf(a - b) < 0.01f; }

static void run(Fix *f) {
    gp_sleep(co_, 0.15);
    cx_ = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_set_smooth_scrolling(app, 0);

    wheel(60, 40, 0, -1, 0);   /* one notch down over the first box */
    CHECK(ncalls == 1 && calls[0].id == 1, "a handler under the pointer gets the wheel (%d calls, id %d)", ncalls, ncalls ? calls[0].id : -1);
    CHECK(near(calls[0].ev.dy, 40) && near(calls[0].ev.dx, 0), "one notch down is dy = +40 (%g, %g)", (double)calls[0].ev.dy, (double)calls[0].ev.dx);
    CHECK(near(calls[0].ev.position.x, 60) && near(calls[0].ev.position.y, 40) && near(calls[0].ev.local.x, 50) && near(calls[0].ev.local.y, 30), "at (60, 40), (50, 30) in a box at (10, 10): %g, %g", (double)calls[0].ev.local.x, (double)calls[0].ev.local.y);
    CHECK(near(gp_scroll_offset(&cx_, f->sc).y, 0), "and it is used up: the scroller around it did not move (%g)", (double)gp_scroll_offset(&cx_, f->sc).y);

    propagate_ids[2] = true;
    wheel(60, 100, 0, -1, 0);   /* the second box, which passes it on: the scroller takes it */
    CHECK(ncalls == 1 && calls[0].id == 2, "the second handler is called (%d)", ncalls);
    CHECK(near(gp_scroll_offset(&cx_, f->sc).y, 40), "gp_propagate lets the scroller have it (%g)", (double)gp_scroll_offset(&cx_, f->sc).y);
    gp_scroll_to(&cx_, f->sc, 0, 0);
    settle();

    propagate_ids[2] = false;
    wheel(60, 100, 0, -1, 0);
    CHECK(ncalls == 1 && near(gp_scroll_offset(&cx_, f->sc).y, 0), "without gp_propagate it does not (%g)", (double)gp_scroll_offset(&cx_, f->sc).y);

    wheel(60, 180, 0, -1, 0);   /* the rest of the scroller has no handler */
    CHECK(ncalls == 0 && near(gp_scroll_offset(&cx_, f->sc).y, 40), "over content with no handler the scroller scrolls as before (%g, %d calls)", (double)gp_scroll_offset(&cx_, f->sc).y, ncalls);
    gp_scroll_to(&cx_, f->sc, 0, 0);
    settle();

    /* nested: the inner passes it on to the outer */
    propagate_ids[4] = true;
    wheel(190, 50, 0, 2, GP_MOD_CTRL);
    CHECK(ncalls == 2 && calls[0].id == 4 && calls[1].id == 3, "the innermost first, then the one around it (%d calls: %d, %d)", ncalls, ncalls > 0 ? calls[0].id : -1, ncalls > 1 ? calls[1].id : -1);
    CHECK(ncalls == 2 && near(calls[0].ev.dy, -80) && calls[0].ev.mods == GP_MOD_CTRL && calls[1].ev.mods == GP_MOD_CTRL, "two notches up is dy = -80 with the modifiers (%g, mods %u)", (double)calls[0].ev.dy, calls[0].ev.mods);
    CHECK(ncalls == 2 && near(calls[0].ev.local.x, 20) && near(calls[0].ev.local.y, 20) && near(calls[1].ev.local.x, 40) && near(calls[1].ev.local.y, 40), "each gets the pointer in its own coordinates ((%g, %g) and (%g, %g))", (double)calls[0].ev.local.x, (double)calls[0].ev.local.y, (double)calls[1].ev.local.x, (double)calls[1].ev.local.y);
    propagate_ids[4] = false;
    wheel(190, 50, 0, 1, 0);
    CHECK(ncalls == 1 && calls[0].id == 4, "when the inner one uses it up the outer one does not hear it (%d)", ncalls);
    wheel(160, 20, 0, 1, 0);
    CHECK(ncalls == 1 && calls[0].id == 3, "outside the inner box the outer one gets it (%d)", ncalls ? calls[0].id : -1);

    /* shift + wheel is horizontal: dx */
    wheel(160, 20, 0, -1, GP_MOD_SHIFT);
    CHECK(ncalls == 1 && near(calls[0].ev.dx, 40) && near(calls[0].ev.dy, 0) && calls[0].ev.mods == GP_MOD_SHIFT, "shift + a notch down is dx = +40 (%g, %g, mods %u)", (double)calls[0].ev.dx, (double)calls[0].ev.dy, calls[0].ev.mods);
    wheel(160, 20, 1, 0, 0);
    CHECK(ncalls == 1 && near(calls[0].ev.dx, 40) && near(calls[0].ev.dy, 0), "a sideways notch is dx = +40 (%g)", (double)calls[0].ev.dx);

    /* a cable: only its stroke, the box under the rest of its bounding box */
    wheel(340, 40, 0, -1, 0);
    CHECK(ncalls == 1 && calls[0].id == 5, "on the stroke: the cable's handler (%d)", ncalls ? calls[0].id : -1);
    CHECK(ncalls == 1 && near(calls[0].ev.local.x, 40 + 4) && near(calls[0].ev.local.y, 4 + 0), "(local in the cable's box: %g, %g)", (double)calls[0].ev.local.x, (double)calls[0].ev.local.y);
    wheel(340, 50, 0, -1, 0);   /* 10 px off the 8 px stroke, inside its box? the box is 80 x 8 + 0: off it altogether: the box underneath */
    CHECK(ncalls == 1 && calls[0].id == 6, "off the stroke: what is under it (%d)", ncalls ? calls[0].id : -1);
    wheel(340, 43, 0, -1, 0);   /* 3 px from the center line: on the stroke */
    CHECK(ncalls == 1 && calls[0].id == 5, "3 px from the center line of an 8 px stroke is on it (%d)", ncalls ? calls[0].id : -1);
    wheel(340, 46, 0, -1, 0);   /* 6 px: past half the width (4) */
    CHECK(ncalls == 1 && calls[0].id == 6, "6 px from it is not (%d)", ncalls ? calls[0].id : -1);

    /* a quarter turn: the 100 x 40 box at (150, 150) is turned about its middle (200, 170) into 40 x 100 at (180, 120) */
    wheel(200, 125, 0, -1, 0);
    CHECK(ncalls == 1 && calls[0].id == 7, "the turned box gets the wheel where it is (%d)", ncalls ? calls[0].id : -1);
    CHECK(ncalls == 1 && near(calls[0].ev.local.x, 5) && near(calls[0].ev.local.y, 20), "its own coordinates: (5, 20) (%g, %g)", ncalls ? (double)calls[0].ev.local.x : 0., ncalls ? (double)calls[0].ev.local.y : 0.);
    wheel(160, 170, 0, -1, 0);   /* where its unturned box would have been */
    CHECK(ncalls == 0, "and not where it was (%d)", ncalls);
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    run(data);
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    gp_entity v = gp_view_new(app, sizeof(Fix), &(Fix){ gp_scroll_new(app) }, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "wheel_test", .width = 420, .height = 260 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, gp_entity_get(app, v));
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "wheel_test: %d FAILURE(S)\n" : "wheel_test: all checks passed\n", failures);
    return failures != 0;
}
