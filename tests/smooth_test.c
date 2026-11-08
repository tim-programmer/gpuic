/*
 * Smooth wheel scrolling: off by default (the offset jumps), with it on a tick moves the target and the offset glides, ticks add up,
 * chaining to the scroller above is decided on the targets, other ways of setting an offset end the glide, the glide ends at the target
 * and then asks for no more frames. Time is jumped with gp_debug_clock_skew, and the glide is made so long (100 s) that the real time
 * a test takes is nothing next to it. Needs a display.
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

typedef struct { gp_scroll v, so, si, l; bool few; } Fix;   /* few: the first scroller has 4 rows instead of 10 */
#define RED gp_rgb(0xc03030)
#define BLUE gp_rgb(0x3030c0)

static gp_node *row(void *self, gp_ctx *cx, size_t i, void *data) { return gp_div(gp_h(20), gp_bg(i % 2 ? RED : BLUE)); }

static gp_node *render(void *self, gp_ctx *cx) {
    Fix *f = self;
    gp_node **v = gp_frame_alloc(10 * sizeof *v);
    for (int i = 0; i < 10; i++) v[i] = gp_div(gp_h(100), gp_flex_none(), gp_bg(i % 2 ? RED : BLUE));
    return gp_div(
        gp_size_full(), gp_flex_row(), gp_items_start(), gp_gap(20), gp_p(10), gp_bg(gp_rgb(0x202030)),
        gp_div(gp_w(100), gp_h(200), gp_overflow_y_scroll(), gp_track_scroll(f->v), gp_children(v, f->few ? 4 : 10)),
        gp_div(gp_w(100), gp_h(200), gp_overflow_y_scroll(), gp_track_scroll(f->so),
               gp_div(gp_w(100), gp_h(100), gp_flex_none(), gp_overflow_y_scroll(), gp_track_scroll(f->si),
                      gp_div(gp_h(150), gp_flex_none(), gp_bg(RED))),
               gp_div(gp_h(1000), gp_flex_none(), gp_bg(BLUE))),
        gp_uniform_list(5000, 20, row, NULL, gp_w(100), gp_h(200), gp_track_scroll(f->l)));
}

static gp_ctx cx_;
static int64_t skew;
static void jump(double seconds) {
    skew += (int64_t)(seconds * 1e9);
    gp_debug_clock_skew(skew);
}
static bool wait_draws(unsigned n) {
    unsigned long target = app->stat_draws + n;
    for (int i = 0; i < 1000 && app->stat_draws < target; i++) gp_sleep(co_, 0.004);
    return app->stat_draws >= target;
}
static float offy(gp_scroll s) { return gp_scroll_offset(&cx_, s).y; }
static bool near(float a, float b, float tol) { return fabsf(a - b) <= tol; }
static void wheel(float x, float y, float dy) { gp_scroll_wheel(win(), (gp_point){ x, y }, 0, dy); }
/* over the column / the outer scroller / the list (the root's children), `dx` px in and `dy` down */
static gp_point over(int child, float dx, float dy) {
    const gp_el *c = win()->tree->children[child];
    return (gp_point){ win()->tree->pos.x + c->pos.x + dx, win()->tree->pos.y + c->pos.y + dy };
}
static void wheel_at(int child, float dx, float dy, float amount) { gp_point p = over(child, dx, dy); wheel(p.x, p.y, amount); }

static void run(Fix *f) {
    wait_draws(2);
    cx_ = gp_ctx_for(app, GP_ENTITY_NULL);

    /* off: the offset jumps */
    CHECK(app->smooth_scroll_s == 0, "off by default");
    wheel_at(0, 20, 20, 100);
    CHECK(near(offy(f->v), 100, 0.5f), "without smooth scrolling the offset jumps: %g", offy(f->v));
    gp_scroll_to(&cx_, f->v, 0, 0);
    wait_draws(2);

    /* on, a very long glide so that real time does not matter */
    gp_set_smooth_scrolling(app, 100);
    wheel_at(0, 20, 20, 100);
    wait_draws(2);
    CHECK(offy(f->v) > 0 && offy(f->v) < 5, "a tick starts a glide: %g", offy(f->v));
    wheel_at(0, 20, 20, 100);
    jump(25);   /* one time constant (100 s / 4): the offset is 1 - 1/e of the way to the target, 200 */
    wait_draws(2);
    CHECK(near(offy(f->v), 200 * (1 - expf(-1.f)), 2.0f), "ticks add to the target, and a time constant covers 63%%: %g", offy(f->v));
    jump(300);
    wait_draws(3);
    CHECK(near(offy(f->v), 200, 0.01f), "the glide ends at the target: %g", offy(f->v));
    unsigned long d0 = app->stat_draws;
    gp_sleep(co_, 0.3);
    CHECK(app->stat_draws - d0 <= 1, "no more frames once there (%lu draws in 0.3 s)", app->stat_draws - d0);

    /* a short glide in real time ends exactly at the target and goes quiet */
    gp_set_smooth_scrolling(app, 0.1f);
    wheel_at(0, 20, 20, -100);
    gp_sleep(co_, 0.9);
    CHECK(near(offy(f->v), 100, 0.01f), "a short glide arrives, exactly: %g", offy(f->v));
    d0 = app->stat_draws;
    gp_sleep(co_, 0.3);
    CHECK(app->stat_draws - d0 <= 1, "...and then asks for no frames (%lu draws in 0.3 s)", app->stat_draws - d0);
    gp_set_smooth_scrolling(app, 100);
    wheel_at(0, 20, 20, 100);   /* back to the target 200 */
    wait_draws(2);
    jump(1000);
    wait_draws(3);

    /* the end is a limit of the target, and the way back */
    wheel_at(0, 20, 20, 5000);
    wait_draws(2);   /* a glide's first step is a nominal 1/60 s; the jump comes after it */
    CHECK(offy(f->v) < 210, "a new glide starts slowly, whatever time passed since the last: %g", offy(f->v));
    jump(1000);
    wait_draws(3);
    CHECK(near(offy(f->v), 800, 0.01f), "the target stops at the end: %g", offy(f->v));
    wheel_at(0, 20, 20, -100);
    wait_draws(2);
    jump(1000);
    wait_draws(3);
    CHECK(near(offy(f->v), 700, 0.01f), "and ticks go back too: %g", offy(f->v));

    /* chaining decides on targets: the inner scroller (50 to go) takes the first tick, the outer the second, at once */
    wheel_at(1, 20, 20, 100);
    wheel_at(1, 20, 20, 100);
    wait_draws(2);
    jump(1000);
    wait_draws(3);
    CHECK(near(offy(f->si), 50, 0.01f) && near(offy(f->so), 100, 0.01f), "the second tick finds the inner target at its end and goes on to the outer: inner %g, outer %g", offy(f->si), offy(f->so));

    /* content that shrinks under a glide: the target is held to the new end */
    gp_scroll_to(&cx_, f->v, 0, 0);
    wait_draws(2);
    wheel_at(0, 20, 20, 5000);
    wait_draws(2);
    f->few = true;
    win()->dirty_render = true;
    wait_draws(2);
    jump(1000);
    wait_draws(3);
    CHECK(near(offy(f->v), 200, 0.01f), "the target follows the content when it shrinks: %g", offy(f->v));
    f->few = false;
    win()->dirty_render = true;
    wait_draws(2);

    /* other ways of setting an offset end the glide where it is */
    gp_scroll_to(&cx_, f->v, 0, 0);
    wait_draws(2);
    wheel_at(0, 20, 20, 100);
    gp_scroll_to(&cx_, f->v, 0, 300);
    wait_draws(2);
    jump(1000);
    wait_draws(3);
    CHECK(near(offy(f->v), 300, 0.01f), "gp_scroll_to ends a glide: %g", offy(f->v));

    /* grabbing the thumb ends a glide too */
    gp_scroll_to(&cx_, f->v, 0, 0);
    wait_draws(2);
    wheel_at(0, 20, 20, 400);
    wait_draws(2);
    {
        const gp_el *n = win()->tree->children[0];
        gp_thumb t;
        CHECK(gp_scroll_thumb(n, 1, win()->tree->pos.x + n->pos.x, win()->tree->pos.y + n->pos.y, &t), "thumb");
        float x = t.x + t.w / 2, y = t.y + 3;
        gp_simulate_mouse(app, GP_SIM_MOVE, x, y, 0, 0);
        gp_simulate_mouse(app, GP_SIM_DOWN, x, y, 1, 0);
        gp_simulate_mouse(app, GP_SIM_MOVE, x, y + 20, 0, 0);
        gp_simulate_mouse(app, GP_SIM_UP, x, y + 20, 1, 0);
        wait_draws(2);
        float at = offy(f->v);
        jump(1000);
        wait_draws(3);
        CHECK(at > 0 && near(offy(f->v), at, 0.5f), "dragging the thumb ends the glide where the drag put the offset (%g, then %g)", at, offy(f->v));
    }

    /* a list: the rows follow */
    wheel_at(2, 20, 20, 100);
    wait_draws(2);
    jump(1000);
    wait_draws(3);
    CHECK(near(offy(f->l), 100, 0.01f), "a list glides too: %g", offy(f->l));
    CHECK(win()->tree->children[2]->list_first == 5, "...and has built the rows for where it is (first row %zu)", win()->tree->children[2]->list_first);

    /* back to off: a tick jumps again */
    gp_set_smooth_scrolling(app, 0);
    gp_scroll_to(&cx_, f->v, 0, 0);
    wait_draws(2);
    wheel_at(0, 20, 20, 60);
    CHECK(near(offy(f->v), 60, 0.5f), "turned off again: %g", offy(f->v));
    gp_debug_clock_skew(0);
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    Fix *fx = gp_entity_get(app, win()->root_view);
    run(fx);
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    Fix init;
    memset(&init, 0, sizeof init);
    init.v = gp_scroll_new(app); init.so = gp_scroll_new(app); init.si = gp_scroll_new(app); init.l = gp_scroll_new(app);
    gp_entity v = gp_view_new(app, sizeof init, &init, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "smooth_test", .width = 520, .height = 260 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "smooth_test: %d FAILURE(S)\n" : "smooth_test: all checks passed\n", failures);
    return failures != 0;
}
