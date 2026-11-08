/*
 * Clicking a scrollbar's track pages toward the click: a column, a row, a bordered one, a gp_uniform_list (offsets with a base),
 * the clamp at both ends, pressing on the thumb still dragging, a press on the track not starting a drag, and a click beside the
 * strip still reaching the content. Needs a display.
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

typedef struct { gp_scroll v, h, b, l, vl; int clicks; } Fix;

#define RED gp_rgb(0xc03030)
#define BLUE gp_rgb(0x3030c0)

static void on_click(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) { ((Fix *)self)->clicks++; }
static gp_node *row(void *self, gp_ctx *cx, size_t i, void *data) {
    return gp_div(gp_h(20), gp_bg(i % 2 ? RED : BLUE));
}

static gp_node *render(void *self, gp_ctx *cx) {
    Fix *f = self;
    gp_node **v = gp_frame_alloc(10 * sizeof *v), **h = gp_frame_alloc(10 * sizeof *h), **b = gp_frame_alloc(10 * sizeof *b);
    for (int i = 0; i < 10; i++) {
        v[i] = gp_div(gp_h(100), gp_flex_none(), gp_bg(i % 2 ? RED : BLUE), gp_on_click(on_click));
        h[i] = gp_div(gp_w(100), gp_flex_none(), gp_bg(i % 2 ? RED : BLUE));
        b[i] = gp_div(gp_h(100), gp_flex_none(), gp_bg(i % 2 ? RED : BLUE));
    }
    return gp_div(
        gp_size_full(), gp_flex_row(), gp_items_start(), gp_gap(20), gp_p(10), gp_bg(gp_rgb(0x202030)),
        gp_div(gp_w(100), gp_h(200), gp_overflow_y_scroll(), gp_track_scroll(f->v), gp_children(v, 10)),
        gp_div(gp_w(200), gp_h(100), gp_flex_row(), gp_overflow_x_scroll(), gp_track_scroll(f->h), gp_children(h, 10)),
        gp_div(gp_w(100), gp_h(200), gp_border(4), gp_border_color(RED), gp_overflow_y_scroll(), gp_track_scroll(f->b), gp_children(b, 10)),
        gp_uniform_list(5000, 20, row, NULL, gp_w(100), gp_h(200), gp_track_scroll(f->l)),
        gp_list(200000, 20, row, NULL, gp_w(100), gp_h(200), gp_track_scroll(f->vl)));
}

static gp_ctx cx_;
static void settle(void) { gp_sleep(co_, 0.15); }
static float offy(gp_scroll s) { return gp_scroll_offset(&cx_, s).y; }
static float offx(gp_scroll s) { return gp_scroll_offset(&cx_, s).x; }
static bool near(float a, float b) { return fabsf(a - b) < 0.6f; }

/* the thumb of scroller `i` (a child of the root) as the window shows it */
static bool thumb(int i, int axis, gp_thumb *t) {
    const gp_el *n = win()->tree->children[i];
    return gp_scroll_thumb(n, axis, win()->tree->pos.x + n->pos.x, win()->tree->pos.y + n->pos.y, t);
}
static int64_t skew;
static void jump(double seconds) {
    skew += (int64_t)(seconds * 1e9);
    gp_debug_clock_skew(skew);
}
static void tick(void) { gp_sleep(co_, 0.02); }
static void click(float x, float y) {
    gp_simulate_mouse(app, GP_SIM_MOVE, x, y, 0, 0);
    gp_simulate_mouse(app, GP_SIM_DOWN, x, y, 1, 0);
    gp_simulate_mouse(app, GP_SIM_UP, x, y, 1, 0);
    settle();
}

static void run(Fix *f) {
    settle();
    cx_ = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_thumb t;

    /* the column: content 1000, view 200, max 800, a page is 175 */
    CHECK(thumb(0, 1, &t), "the column has a thumb");
    float x = t.x + t.w / 2;
    click(x, t.track_start + t.track_len - 3);
    CHECK(near(offy(f->v), 175), "a press on the track below the thumb pages down: %g", offy(f->v));
    click(x, t.track_start + t.track_len - 3);
    CHECK(near(offy(f->v), 350), "...again: %g", offy(f->v));
    thumb(0, 1, &t);
    click(x, t.track_start + 1);
    CHECK(near(offy(f->v), 175), "above the thumb pages up: %g", offy(f->v));
    for (int i = 0; i < 6; i++) { thumb(0, 1, &t); click(x, t.track_start + t.track_len - 1); }
    CHECK(near(offy(f->v), 800), "paging stops at the end: %g", offy(f->v));
    thumb(0, 1, &t);
    click(x, t.track_start + t.track_len - 1);
    CHECK(near(offy(f->v), 800), "...and a press below the thumb at the end does nothing: %g", offy(f->v));
    for (int i = 0; i < 8; i++) { thumb(0, 1, &t); click(x, t.track_start + 1); }
    CHECK(near(offy(f->v), 0), "and at the start: %g", offy(f->v));

    /* a press on the thumb drags (it does not page); a press on the track does not start a drag */
    thumb(0, 1, &t);
    float ty = t.y + t.h / 2;
    gp_simulate_mouse(app, GP_SIM_MOVE, x, ty, 0, 0);
    gp_simulate_mouse(app, GP_SIM_DOWN, x, ty, 1, 0);
    gp_simulate_mouse(app, GP_SIM_MOVE, x, ty + 30, 0, 0);
    gp_simulate_mouse(app, GP_SIM_UP, x, ty + 30, 1, 0);
    settle();
    float dragged = offy(f->v);
    CHECK(dragged > 0 && dragged != 175, "a press on the thumb drags: %g", dragged);
    gp_scroll_to(&cx_, f->v, 0, 0);
    settle();
    thumb(0, 1, &t);
    float below = t.track_start + t.track_len - 3;
    gp_simulate_mouse(app, GP_SIM_MOVE, x, below, 0, 0);
    gp_simulate_mouse(app, GP_SIM_DOWN, x, below, 1, 0);
    gp_simulate_mouse(app, GP_SIM_MOVE, x, below - 60, 0, 0);
    settle();
    gp_simulate_mouse(app, GP_SIM_UP, x, below - 60, 1, 0);
    settle();
    CHECK(near(offy(f->v), 175), "dragging after a press on the track does not move the content further: %g", offy(f->v));

    /* a click beside the strip reaches the content */
    gp_scroll_to(&cx_, f->v, 0, 0);
    settle();
    f->clicks = 0;
    click(win()->tree->children[0]->pos.x + 80, 10 + 100);
    CHECK(f->clicks == 1 && near(offy(f->v), 0), "a click on the content is a click (%d clicks, offset %g)", f->clicks, offy(f->v));

    /* held, a press on the track goes on paging (after 0.4 s, then every 0.1 s) until it is released, the end, or the thumb is under the pointer.
     * Time is the clock jumped plus a little real time (tick), and nothing here waits for a draw: only the repeat itself would cause one. */
    gp_scroll_to(&cx_, f->v, 0, 0);
    settle();
    thumb(0, 1, &t);
    float far = t.track_start + t.track_len - 3;
    gp_simulate_mouse(app, GP_SIM_MOVE, x, far, 0, 0);
    gp_simulate_mouse(app, GP_SIM_DOWN, x, far, 1, 0);
    tick();
    CHECK(near(offy(f->v), 175), "held on the track: the first page at once: %g", offy(f->v));
    jump(0.2);
    tick();
    CHECK(near(offy(f->v), 175), "...and no second one before 0.4 s: %g", offy(f->v));
    jump(0.2);
    tick();
    CHECK(near(offy(f->v), 350), "...then one: %g", offy(f->v));
    jump(0.03);
    tick();
    CHECK(near(offy(f->v), 350), "...and the next after 0.1 s, not before: %g", offy(f->v));
    jump(0.06);
    tick();
    CHECK(near(offy(f->v), 525), "...it comes: %g", offy(f->v));
    gp_simulate_mouse(app, GP_SIM_UP, x, far, 1, 0);
    tick();
    unsigned long dr = app->stat_draws;
    gp_sleep(co_, 0.3);
    CHECK(app->stat_draws == dr, "released: nothing is woken afterwards, the repeat's timer is gone (%lu draws in 0.3 s)", app->stat_draws - dr);
    jump(2);
    tick();
    CHECK(near(offy(f->v), 525), "...and no more pages: %g", offy(f->v));

    /* the end of the content stops it, and then nothing is asked for */
    gp_simulate_mouse(app, GP_SIM_MOVE, x, far, 0, 0);
    gp_simulate_mouse(app, GP_SIM_DOWN, x, far, 1, 0);
    tick();
    gp_simulate_mouse(app, GP_SIM_MOVE, x, t.track_start + t.track_len + 25, 0, 0);   /* held, and the pointer slides below the track: the thumb will never reach it */
    for (int i = 0; i < 4; i++) { jump(i ? 0.11 : 0.45); tick(); }
    CHECK(near(offy(f->v), 800), "paging stops at the end: %g", offy(f->v));
    jump(0.5);
    tick();
    unsigned long d0 = app->stat_draws;
    gp_sleep(co_, 0.3);
    CHECK(app->stat_draws - d0 <= 1, "...and then asks for nothing (%lu draws in 0.3 s)", app->stat_draws - d0);
    gp_simulate_mouse(app, GP_SIM_UP, x, far, 1, 0);
    settle();

    /* a press right below the thumb: one page brings the thumb under the pointer, which ends it */
    gp_scroll_to(&cx_, f->v, 0, 0);
    settle();
    thumb(0, 1, &t);
    float near_y = t.y + t.h + 2;
    gp_simulate_mouse(app, GP_SIM_MOVE, x, near_y, 0, 0);
    gp_simulate_mouse(app, GP_SIM_DOWN, x, near_y, 1, 0);
    tick();
    jump(0.5);
    tick();
    CHECK(near(offy(f->v), 175), "when the thumb is under the pointer paging stops: %g", offy(f->v));
    jump(0.11);
    tick();
    jump(0.11);
    tick();
    CHECK(near(offy(f->v), 175), "...for good: %g", offy(f->v));
    gp_simulate_mouse(app, GP_SIM_UP, x, near_y, 1, 0);
    settle();

    /* held above the thumb it pages up */
    gp_scroll_to(&cx_, f->v, 0, 800);
    settle();
    thumb(0, 1, &t);
    float up_y = t.track_start + 1;
    gp_simulate_mouse(app, GP_SIM_MOVE, x, up_y, 0, 0);
    gp_simulate_mouse(app, GP_SIM_DOWN, x, up_y, 1, 0);
    tick();
    CHECK(near(offy(f->v), 625), "a press above the thumb pages up: %g", offy(f->v));
    jump(0.45);
    tick();
    CHECK(near(offy(f->v), 450), "...and goes on upward: %g", offy(f->v));
    gp_simulate_mouse(app, GP_SIM_UP, x, up_y, 1, 0);
    settle();
    gp_debug_clock_skew(0);
    skew = 0;

    /* the row: content 1000, view 200: a page is 175 */
    CHECK(thumb(1, 0, &t), "the row has a thumb");
    float hy = t.y + t.h / 2;
    click(t.track_start + t.track_len - 3, hy);
    CHECK(near(offx(f->h), 175), "a row pages right: %g", offx(f->h));
    thumb(1, 0, &t);
    click(t.track_start + 1, hy);
    CHECK(near(offx(f->h), 0), "and left: %g", offx(f->h));

    /* bordered: the view is 192 high */
    CHECK(thumb(2, 1, &t), "the bordered column has a thumb");
    click(t.x + t.w / 2, t.track_start + t.track_len - 3);
    CHECK(near(offy(f->b), 168), "a border is not part of the page (0.875 of 192): %g", offy(f->b));

    /* the list: 5000 rows of 20 */
    CHECK(thumb(3, 1, &t), "the list has a thumb");
    float lx = t.x + t.w / 2;
    click(lx, t.track_start + t.track_len - 3);
    CHECK(near(offy(f->l), 175), "a list pages too: %g", offy(f->l));
    thumb(3, 1, &t);
    click(lx, t.track_start + t.track_len - 3);
    CHECK(near(offy(f->l), 350), "...again: %g", offy(f->l));
    gp_scroll_to(&cx_, f->l, 0, 50000);
    settle();
    thumb(3, 1, &t);
    click(lx, t.track_start + 1);
    CHECK(near(offy(f->l), 49825), "...and back, far from the start: %g", offy(f->l));

    /* a gp_list with millions of pixels keeps its offset as a base and a small remainder: the page is still 175 */
    gp_scroll_to(&cx_, f->vl, 0, 3000000);
    settle();
    CHECK(thumb(4, 1, &t), "the long list has a thumb");
    float vx = t.x + t.w / 2;
    click(vx, t.track_start + 1);
    CHECK(near(offy(f->vl), 3000000 - 175), "a long gp_list pages up from deep inside: %g", offy(f->vl));
    thumb(4, 1, &t);
    click(vx, t.track_start + t.track_len - 1);
    CHECK(near(offy(f->vl), 3000000), "...and down: %g", offy(f->vl));
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
    init.v = gp_scroll_new(app); init.h = gp_scroll_new(app); init.b = gp_scroll_new(app); init.l = gp_scroll_new(app); init.vl = gp_scroll_new(app);
    gp_entity v = gp_view_new(app, sizeof init, &init, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "track_test", .width = 740, .height = 260 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "track_test: %d FAILURE(S)\n" : "track_test: all checks passed\n", failures);
    return failures != 0;
}
