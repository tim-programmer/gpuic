/*
 * State styles that lay out (gp_hover_style, gp_active_style, gp_focus_style setting sizes, padding, fonts): the element is laid out in the
 * state and its neighbours move; it goes back when the state ends; text inside takes a font size from its parent's state; pressed and focus
 * work the same; an element inside a cached view; an animation on the same element keeps running under the state; the frame that shows the
 * state is laid out for it already, and a click lands on the grown box; a style that moves its element off the pointer does not have the
 * window drawing for ever. Needs a display, at scale 1.
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

#define FILL gp_rgb(0x182030)
#define RED gp_rgb(0xff0000)
#define BLACK gp_rgb(0x000000)

#define BOX(...) gp_div(gp_w(80), gp_h(40), gp_flex_none(), gp_bg(FILL), __VA_ARGS__)

static gp_focus g_focus, g_focus2;
static gp_entity g_cached;
static int g_clicks_a0;
static const char *g_anim_id = "lstate-grow";   /* (a new id starts the animation again) */

/* the state is on an element inside the view's root, which is the same size whatever is in it: only the inside is laid out again */
static gp_node *cached_render(void *self, gp_ctx *cx) {
    return gp_div(gp_w(200), gp_h(50), gp_flex_none(), gp_items_start(), BOX(gp_hover_style(gp_group(gp_w(120)))));
}

static void on_a0(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) { g_clicks_a0++; }

static gp_node *grow_w(float t, void *data) { return gp_w(40 + 40 * t); }

static gp_node *render(void *self, gp_ctx *cx) {
    return gp_div(
        gp_size_full(), gp_flex_col(), gp_items_start(), gp_gap(10), gp_p(20), gp_bg(BLACK),
        /* 0: a box grows, its neighbour moves */
        gp_div(gp_flex_row(), gp_items_start(), gp_gap(10),
               BOX(gp_hover_style(gp_group(gp_w(160), gp_h(60), gp_bg(RED))), gp_on_click(on_a0)),
               gp_div(gp_w(80), gp_h(40), gp_flex_none(), gp_bg(FILL))),
        /* 1: padding: the child moves in */
        gp_div(gp_p(4), gp_bg(FILL), gp_hover_style(gp_group(gp_p(20))), gp_div(gp_w(30), gp_h(30), gp_bg(RED))),
        /* 2: a font size, inherited by the text inside */
        gp_div(gp_text_size(14), gp_hover_style(gp_group(gp_text_size(28))), gp_text("Hello")),
        /* 3: pressed */
        BOX(gp_active_style(gp_group(gp_h(80)))),
        /* 4: focus */
        BOX(gp_track_focus(g_focus), gp_focus_style(gp_group(gp_w(150)))),
        /* 5: in a cached view */
        gp_child_view_cached(g_cached),
        /* 6: an animation (the width) and a state (the height) together */
        gp_div(gp_w(40), gp_h(40), gp_flex_none(), gp_bg(FILL), gp_hover_style(gp_group(gp_h(70))),
               gp_with_animation(g_anim_id, (gp_animation){ .duration = 0.6 }, grow_w, NULL)),
        /* 7: a box that shrinks away from the pointer when hovered */
        BOX(gp_hover_style(gp_group(gp_w(10)))),
        /* 8: hover (the height) then pressed (the width) */
        BOX(gp_hover_style(gp_group(gp_h(60))), gp_active_style(gp_group(gp_w(140)))),
        /* 9: hover and focus set the width: focus wins */
        BOX(gp_track_focus(g_focus2), gp_hover_style(gp_group(gp_w(100))), gp_focus_style(gp_group(gp_w(150)))),
        /* 10: three in one another, all hovered at once */
        gp_div(gp_p(2), gp_bg(FILL), gp_hover_style(gp_group(gp_p(6))),
               gp_div(gp_p(2), gp_bg(BLACK), gp_hover_style(gp_group(gp_p(8))), BOX(gp_hover_style(gp_group(gp_w(120)))))));
}

static bool wait_draws(unsigned n) {
    unsigned long target = app->stat_draws + n;
    for (int i = 0; i < 1000 && app->stat_draws < target; i++) {
        if (i > 5) win()->dirty_paint = true;   /* (nothing changed: a frame anyway, which is all the waiting is for) */
        gp_sleep(co_, 0.004);
    }
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
static gp_el *row(int i) { return win()->tree->children[i]; }
static void move(float x, float y) {
    gp_simulate_mouse(app, GP_SIM_MOVE, x, y, 0, 0);
    gp_sleep(co_, 0.02);
    wait_draws(1);
}
static const uint8_t *px_at(const uint8_t *px, int x, int y) { return px + ((size_t)y * (size_t)app->capture_w + (size_t)x) * 3; }
static bool is_color(const uint8_t *p, gp_color c, int tol) {
    return abs(p[0] - (int)lroundf(c.r * 255)) <= tol && abs(p[1] - (int)lroundf(c.g * 255)) <= tol && abs(p[2] - (int)lroundf(c.b * 255)) <= tol;
}
#define AWAY 2000.f, 2000.f

static void run(void) {
    wait_draws(2);
    gp_sleep(co_, 0.8);   /* the animation of 6 is over: no frames are drawn but for what the test does */
    wait_draws(1);
    CHECK(win()->tree->nchildren == 11, "%u rows", win()->tree->nchildren);
    if (win()->tree->nchildren != 11) return;

    /* 0: the box grows to 160 x 60, its neighbour moves right by 80, the row grows */
    gp_el *r0 = row(0), *a0 = r0->children[0], *a1 = r0->children[1];
    float x0 = r0->pos.x + a0->pos.x, y0 = r0->pos.y + a0->pos.y;
    float a1x = a1->pos.x, r1y = row(1)->pos.y;
    CHECK(a0->size.w == 80 && a0->size.h == 40 && a1x == 90, "0: at rest %g x %g, the neighbour at %g", a0->size.w, a0->size.h, a1x);
    /* the first frame drawn after the pointer came is laid out for the state already: red where only the grown box reaches */
    /* (frame by frame, until one shows the hover: the move is an event, which may come after the next frame) */
    gp_debug_capture_next(app);
    gp_simulate_mouse(app, GP_SIM_MOVE, x0 + 40, y0 + 20, 0, 0);
    bool shown = false;
    for (int k = 0; k < 50 && !shown; k++) {
        for (int i = 0; i < 1000 && !app->capture_px; i++) {
            if (i > 25) win()->dirty_paint = true;
            gp_sleep(co_, 0.002);
        }
        if (!app->capture_px) break;
        if (is_color(px_at(app->capture_px, (int)(x0 + 40), (int)(y0 + 20)), RED, 2)) {
            shown = true;
            CHECK(is_color(px_at(app->capture_px, (int)(x0 + 150), (int)(y0 + 50)), RED, 2), "0: the first frame that shows the hover shows the grown box");
        } else {
            gp_debug_capture_next(app);
        }
    }
    CHECK(shown, "0: a frame shows the hover");
    move(x0 + 40, y0 + 20);
    r0 = row(0); a0 = r0->children[0]; a1 = r0->children[1];
    CHECK(a0->size.w == 160 && a0->size.h == 60, "0: hovered, %g x %g", a0->size.w, a0->size.h);
    CHECK(a1->pos.x == 170, "0: the neighbour moved to %g", a1->pos.x);
    CHECK(row(1)->pos.y == r1y + 20, "0: the next row moved down to %g (was %g)", row(1)->pos.y, r1y);
    /* a click there, where at rest there is nothing, lands on the grown box (the pointer is still over it) */
    move(x0 + 150, y0 + 50);
    gp_simulate_mouse(app, GP_SIM_DOWN, x0 + 150, y0 + 50, 1, 0);
    gp_simulate_mouse(app, GP_SIM_UP, x0 + 150, y0 + 50, 1, 0);
    gp_sleep(co_, 0.03);
    wait_draws(1);
    CHECK(g_clicks_a0 == 1, "0: the click landed on the grown box (%d clicks)", g_clicks_a0);
    move(AWAY);
    r0 = row(0); a0 = r0->children[0]; a1 = r0->children[1];
    CHECK(a0->size.w == 80 && a0->size.h == 40 && a1->pos.x == 90 && row(1)->pos.y == r1y, "0: back at rest: %g x %g, neighbour at %g", a0->size.w, a0->size.h,
          a1->pos.x);
    uint8_t *f = frame();
    CHECK(f && is_color(px_at(f, (int)(x0 + 150), (int)(y0 + 50)), BLACK, 2), "0: and painted so");

    /* 1: the padding: the child at (4, 4), then (20, 20); the box 38 then 70 */
    gp_el *b = row(1);
    CHECK(b->children[0]->pos.x == 4 && b->children[0]->pos.y == 4 && b->size.w == 38, "1: at rest the child at %g %g, box %g", b->children[0]->pos.x,
          b->children[0]->pos.y, b->size.w);
    move(b->pos.x + 19, b->pos.y + 19);
    b = row(1);
    CHECK(b->children[0]->pos.x == 20 && b->children[0]->pos.y == 20 && b->size.w == 70 && b->size.h == 70, "1: hovered the child at %g %g, box %g x %g",
          b->children[0]->pos.x, b->children[0]->pos.y, b->size.w, b->size.h);
    move(AWAY);
    CHECK(row(1)->children[0]->pos.x == 4 && row(1)->size.w == 38, "1: back at rest");

    /* 2: the text inside is laid out at the parent's state size */
    gp_el *t = row(2)->children[0];
    float tw = t->size.w, th = t->size.h;
    CHECK(t->ts.size == 14, "2: at rest the text is %g px", t->ts.size);
    move(row(2)->pos.x + 5, row(2)->pos.y + th / 2);
    t = row(2)->children[0];
    CHECK(t->ts.size == 28 && t->size.w > tw * 1.6f && t->size.h > th * 1.6f, "2: hovered the text is %g px, %g x %g (was %g x %g)", t->ts.size, t->size.w,
          t->size.h, tw, th);
    move(AWAY);
    t = row(2)->children[0];
    CHECK(t->ts.size == 14 && t->size.w == tw && t->size.h == th, "2: back at rest: %g px, %g x %g", t->ts.size, t->size.w, t->size.h);

    /* 3: pressed: 80 high while the button is down over it, 40 after */
    gp_el *d = row(3);
    float dx = d->pos.x + 40, dy = d->pos.y + 20;
    move(dx, dy);
    CHECK(row(3)->size.h == 40, "3: hovered, not pressed: %g", row(3)->size.h);
    gp_simulate_mouse(app, GP_SIM_DOWN, dx, dy, 1, 0);
    gp_sleep(co_, 0.02);
    wait_draws(1);
    CHECK(row(3)->size.h == 80, "3: pressed: %g", row(3)->size.h);
    gp_simulate_mouse(app, GP_SIM_UP, dx, dy, 1, 0);
    gp_sleep(co_, 0.02);
    wait_draws(1);
    CHECK(row(3)->size.h == 40, "3: released: %g", row(3)->size.h);
    move(AWAY);

    /* 4: focus, with the pointer away */
    gp_ctx c = gp_ctx_for(app, GP_ENTITY_NULL);
    CHECK(row(4)->size.w == 80, "4: not focused: %g", row(4)->size.w);
    gp_focus_set(&c, g_focus);
    gp_sleep(co_, 0.02);
    wait_draws(1);
    CHECK(row(4)->size.w == 150, "4: focused: %g", row(4)->size.w);
    gp_focus_set(&c, (gp_focus){ 0 });
    gp_sleep(co_, 0.02);
    wait_draws(1);
    CHECK(row(4)->size.w == 80, "4: and not: %g", row(4)->size.w);

    /* 5: in a cached view, twice (the view is not built again: the same elements go into the state and out) */
    for (int k = 0; k < 2; k++) {
        gp_el *e = row(5);
        move(e->pos.x + 40, e->pos.y + 20);
        CHECK(row(5)->children[0]->size.w == 120 && row(5)->size.w == 200, "5.%d: cached, hovered: %g in %g", k, row(5)->children[0]->size.w, row(5)->size.w);
        move(AWAY);
        CHECK(row(5)->children[0]->size.w == 80, "5.%d: cached, back: %g", k, row(5)->children[0]->size.w);
    }

    /* 6: hovered while the width animates: the height is the state's all along, the width the animation's */
    g_anim_id = "lstate-grow-again";
    gp_app_notify(app);
    wait_draws(1);
    gp_el *g = row(6);
    CHECK(g->size.w < 80, "6: the animation runs again: %g", g->size.w);
    move(g->pos.x + 10, g->pos.y + 10);
    float last_w = 0;
    bool h_held = true;
    for (int i = 0; i < 6; i++) {
        wait_draws(1);
        h_held &= row(6)->size.h == 70;
        last_w = row(6)->size.w;
    }
    CHECK(h_held, "6: the state's height holds while the animation runs: %g", row(6)->size.h);
    gp_sleep(co_, 0.7);
    wait_draws(2);
    CHECK(row(6)->size.w == 80 && row(6)->size.h == 70, "6: the animation ended at 80 wide (was %g), still 70 high: %g x %g", last_w, row(6)->size.w,
          row(6)->size.h);
    move(AWAY);
    CHECK(row(6)->size.w == 80 && row(6)->size.h == 40, "6: back at rest: %g x %g", row(6)->size.w, row(6)->size.h);

    /* 7: shrinking off the pointer: the window settles, it does not keep drawing */
    gp_el *h = row(7);
    move(h->pos.x + 60, h->pos.y + 20);
    gp_sleep(co_, 0.1);
    unsigned long d0 = app->stat_draws;
    gp_sleep(co_, 0.3);
    CHECK(app->stat_draws - d0 <= 1, "7: %lu draws while the pointer stood still", app->stat_draws - d0);
    move(AWAY);
    CHECK(row(7)->size.w == 80, "7: back at rest once the pointer left: %g", row(7)->size.w);

    /* 8: hovered 80 x 60; pressed as well 140 x 60 */
    gp_el *e8 = row(8);
    float ex = e8->pos.x + 20, ey = e8->pos.y + 20;
    move(ex, ey);
    CHECK(row(8)->size.w == 80 && row(8)->size.h == 60, "8: hovered: %g x %g", row(8)->size.w, row(8)->size.h);
    gp_simulate_mouse(app, GP_SIM_DOWN, ex, ey, 1, 0);
    gp_sleep(co_, 0.02);
    wait_draws(1);
    CHECK(row(8)->size.w == 140 && row(8)->size.h == 60, "8: pressed: %g x %g", row(8)->size.w, row(8)->size.h);
    gp_simulate_mouse(app, GP_SIM_UP, ex, ey, 1, 0);
    gp_sleep(co_, 0.02);
    move(AWAY);
    CHECK(row(8)->size.w == 80 && row(8)->size.h == 40, "8: at rest: %g x %g", row(8)->size.w, row(8)->size.h);

    /* 9: hovered 100; focused and hovered 150 */
    gp_el *e9 = row(9);
    move(e9->pos.x + 20, e9->pos.y + 20);
    CHECK(row(9)->size.w == 100, "9: hovered: %g", row(9)->size.w);
    gp_focus_set(&c, g_focus2);
    gp_sleep(co_, 0.02);
    wait_draws(1);
    CHECK(row(9)->size.w == 150, "9: focused and hovered, focus wins: %g", row(9)->size.w);
    move(AWAY);
    CHECK(row(9)->size.w == 150, "9: focused: %g", row(9)->size.w);

    /* 10: with 9 still focused (four elements in states at once): the inner box 120 wide, 8 and 6 of padding around it */
    gp_el *e10 = row(10);
    move(e10->pos.x + 40, e10->pos.y + 20);   /* (well inside the inner box: the paddings around it push it 12 px right and down) */
    e10 = row(10);
    gp_el *mid = e10->children[0], *in = mid->children[0];
    CHECK(in->size.w == 120 && mid->size.w == 136 && e10->size.w == 148 && row(9)->size.w == 150, "10: all in their states: %g %g %g (and 9 at %g)", in->size.w,
          mid->size.w, e10->size.w, row(9)->size.w);
    move(AWAY);
    gp_focus_set(&c, (gp_focus){ 0 });
    gp_sleep(co_, 0.02);
    wait_draws(1);
    e10 = row(10);
    CHECK(e10->size.w == 88 && row(9)->size.w == 80, "10: at rest: %g, 9 at %g", e10->size.w, row(9)->size.w);
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    run();
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    g_focus = gp_focus_new(app);
    g_focus2 = gp_focus_new(app);
    g_cached = gp_view_new(app, 1, &(char){ 0 }, cached_render, NULL);
    gp_entity v = gp_view_new(app, 1, &(char){ 0 }, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "lstate_test", .width = 400, .height = 760 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "lstate_test: %d FAILURE(S)\n" : "lstate_test: all checks passed\n", failures);
    return failures != 0;
}
