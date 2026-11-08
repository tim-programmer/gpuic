/*
 * State styles (gp_hover_style, gp_active_style, gp_focus_style) from frames read back from the renderer: what the state sets (background, a
 * gradient, border color, opacity, shadow, rounded corners) is painted only while the state holds and is gone after; hover then pressed then
 * focus stack; gp_hover_bg still wins; what would resize or move the element is ignored (the layout does not change); an element inside a cached
 * view and a child of a hovered parent behave; the state is looked up per frame, so moving the pointer is enough. Needs a display, at scale 1.
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
#define GREEN gp_rgb(0x00ff00)
#define BLUE gp_rgb(0x0000ff)
#define WHITE gp_rgb(0xffffff)
#define BLACK gp_rgb(0x000000)

#define BOX(...) gp_div(gp_w(80), gp_h(50), gp_flex_none(), gp_bg(FILL), __VA_ARGS__)

static gp_focus g_focus, g_focus2;
static gp_entity g_cached;

static gp_node *cached_render(void *self, gp_ctx *cx) {
    return BOX(gp_hover_style(gp_group(gp_bg(RED))), NULL);
}

static gp_node *render(void *self, gp_ctx *cx) {
    const gp_color_stop stops[2] = { { RED, 0 }, { BLUE, 1 } };
    return gp_div(
        gp_size_full(), gp_flex_row(), gp_flex_wrap(), gp_align_content_start(), gp_items_start(), gp_gap(30), gp_p(30), gp_bg(BLACK),
        /* 0: hover background */ BOX(gp_hover_style(gp_group(gp_bg(RED)))),
        /* 1: hover border color */ BOX(gp_border(6), gp_border_color(WHITE), gp_hover_style(gp_group(gp_border_color(GREEN)))),
        /* 2: hover opacity */ BOX(gp_hover_style(gp_group(gp_opacity(0.5f)))),
        /* 3: hover shadow */ BOX(gp_hover_style(gp_group(gp_shadow(0, 0, 0, GREEN)))),
        /* 4: hover corners */ BOX(gp_hover_style(gp_group(gp_rounded(20)))),
        /* 5: pressed */ BOX(gp_active_style(gp_group(gp_bg(GREEN)))),
        /* 6: focus */ BOX(gp_track_focus(g_focus), gp_focus_style(gp_group(gp_bg(BLUE)))),
        /* 7: hover, then pressed on top of it */ BOX(gp_hover_style(gp_group(gp_bg(RED), gp_opacity(0.5f))), gp_active_style(gp_group(gp_bg(BLUE)))),
        /* 8: gp_hover_bg wins */ BOX(gp_hover_bg(GREEN), gp_hover_style(gp_group(gp_bg(RED)))),
        /* 9: layout properties are ignored */ BOX(gp_hover_style(gp_group(gp_w(200), gp_h(120), gp_p(30), gp_bg(RED)))),
        /* 10: inside a cached view */ gp_child_view_cached(g_cached),
        /* 11: a gradient */ BOX(gp_hover_style(gp_group(gp_bg_linear_gradient(90, stops, 2)))),
        /* 12: a parent's state does not reach its children */ BOX(gp_hover_style(gp_group(gp_bg(RED))), gp_div(gp_w(20), gp_h(20), gp_bg(GREEN))),
        /* 13: nothing set: the same */ BOX(gp_hover_style(gp_group(gp_cursor_pointer()))),
        /* 14: one side's color */ BOX(gp_border(6), gp_border_color(WHITE), gp_hover_style(gp_group(gp_border_color_top(RED)))),
        /* 15: dashes */ BOX(gp_border(6), gp_border_color(WHITE), gp_hover_style(gp_group(gp_border_dashed()))),
        /* 16: one corner (the top left) */ BOX(gp_hover_style(gp_group(gp_rounded_corners(20, 0, 0, 0)))),
        /* 17: a gradient border */ BOX(gp_border(6), gp_border_color(WHITE), gp_hover_style(gp_group(gp_border_linear_gradient(90, stops, 2)))),
        /* 18: a shadow that shows: the box is smaller than its shadow's spread, offset */ BOX(gp_hover_style(gp_group(gp_shadow(14, 0, 0, GREEN)))),
        /* 19: a color over a gradient background */ BOX(gp_bg_linear_gradient(90, stops, 2), gp_hover_style(gp_group(gp_bg(GREEN)))),
        /* 20: a border color over side colors */ BOX(gp_border(6), gp_border_color_top(RED), gp_hover_style(gp_group(gp_border_color(GREEN)))),
        /* 21: focused, not hovered: the hover style stays out */ BOX(gp_track_focus(g_focus2), gp_hover_style(gp_group(gp_bg(RED))), gp_focus_style(gp_group(gp_opacity(0.5f)))));
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
static const gp_el *box(int i) { return win()->tree->children[i]; }
static void at(int i, float *cx, float *cy) { *cx = box(i)->pos.x + box(i)->size.w / 2; *cy = box(i)->pos.y + box(i)->size.h / 2; }
static const uint8_t *px_at(const uint8_t *px, int x, int y) { return px + ((size_t)y * (size_t)app->capture_w + (size_t)x) * 3; }
static bool is_rgb(const uint8_t *p, int r, int g, int b, int tol) { return abs(p[0] - r) <= tol && abs(p[1] - g) <= tol && abs(p[2] - b) <= tol; }
static bool is_color(const uint8_t *p, gp_color c, int tol) { return is_rgb(p, (int)lroundf(c.r * 255), (int)lroundf(c.g * 255), (int)lroundf(c.b * 255), tol); }

/* the pixel at the middle of box i, with the pointer at (mx, my) (or away from the window), after the frame has been drawn */
static uint8_t g_px[3];
static const uint8_t *sample(int i, float mx, float my, int dx, int dy) {
    gp_simulate_mouse(app, GP_SIM_MOVE, mx, my, 0, 0);
    gp_sleep(co_, 0.02);
    uint8_t *f = frame();
    if (!f) return NULL;
    float cx, cy;
    at(i, &cx, &cy);
    memcpy(g_px, px_at(f, (int)cx + dx, (int)cy + dy), 3);
    return g_px;
}
#define AWAY 2000.f, 2000.f

static void run(void) {
    wait_draws(2);
    CHECK(win()->tree->nchildren == 22, "%u boxes", win()->tree->nchildren);
    if (win()->tree->nchildren != 22) return;
    float cx, cy;
    const uint8_t *p;

    /* 0: background */
    at(0, &cx, &cy);
    p = sample(0, AWAY, 0, 0);
    CHECK(p && is_color(p, FILL, 2), "0: at rest the fill: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);
    p = sample(0, cx, cy, 0, 0);
    CHECK(p && is_color(p, RED, 2), "0: hovered, red: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);
    p = sample(0, AWAY, 0, 0);
    CHECK(p && is_color(p, FILL, 2), "0: gone with the pointer: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);

    /* 1: the border turns green, the inside stays */
    at(1, &cx, &cy);
    int bx = -(int)box(1)->size.w / 2 + 3;
    p = sample(1, AWAY, bx, 0);
    CHECK(p && is_color(p, WHITE, 2), "1: border at rest white: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);
    p = sample(1, cx, cy, bx, 0);
    CHECK(p && is_color(p, GREEN, 2), "1: hovered, green border: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);
    p = sample(1, cx, cy, 0, 0);
    CHECK(p && is_color(p, FILL, 2), "1: the inside stays: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);

    /* 2: half opaque over black */
    at(2, &cx, &cy);
    p = sample(2, cx, cy, 0, 0);
    CHECK(p && is_rgb(p, 0x18 / 2, 0x20 / 2, 0x30 / 2, 3), "2: opacity .5 over black: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);

    /* 3: a hard shadow the size of the box, offset 0: it is under the box, not seen; offset it with a second look just outside is not needed: the fill stays */
    at(3, &cx, &cy);
    p = sample(3, cx, cy, 0, 0);
    CHECK(p && is_color(p, FILL, 2), "3: the box stays on top of its shadow: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);

    /* 4: a corner turns round: the pixel in the very corner shows what is behind */
    at(4, &cx, &cy);
    int ccx = -(int)box(4)->size.w / 2 + 1, ccy = -(int)box(4)->size.h / 2 + 1;
    p = sample(4, AWAY, ccx, ccy);
    CHECK(p && is_color(p, FILL, 2), "4: the corner is square at rest: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);
    p = sample(4, cx, cy, ccx, ccy);
    CHECK(p && is_color(p, BLACK, 6), "4: and round when hovered: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);

    /* 5: pressed */
    at(5, &cx, &cy);
    p = sample(5, cx, cy, 0, 0);
    CHECK(p && is_color(p, FILL, 2), "5: hovered but not pressed: the fill: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);
    gp_simulate_mouse(app, GP_SIM_DOWN, cx, cy, 1, 0);
    gp_sleep(co_, 0.02);
    uint8_t *f = frame();
    CHECK(f && is_color(px_at(f, (int)cx, (int)cy), GREEN, 2), "5: pressed: green");
    gp_simulate_mouse(app, GP_SIM_UP, cx, cy, 1, 0);
    gp_sleep(co_, 0.02);
    f = frame();
    CHECK(f && is_color(px_at(f, (int)cx, (int)cy), FILL, 2), "5: released: the fill again");

    /* 6: focus */
    at(6, &cx, &cy);
    p = sample(6, AWAY, 0, 0);
    CHECK(p && is_color(p, FILL, 2), "6: not focused: the fill");
    gp_ctx c = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_focus_set(&c, g_focus);
    gp_sleep(co_, 0.02);
    p = sample(6, AWAY, 0, 0);
    CHECK(p && is_color(p, BLUE, 2), "6: focused: blue: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);
    gp_focus_set(&c, (gp_focus){ 0 });
    gp_sleep(co_, 0.02);
    p = sample(6, AWAY, 0, 0);
    CHECK(p && is_color(p, FILL, 2), "6: and not again");

    /* 7: hover (red at half opacity over black), then pressed: blue at half opacity (the hover's opacity stays) */
    at(7, &cx, &cy);
    p = sample(7, cx, cy, 0, 0);
    CHECK(p && is_rgb(p, 128, 0, 0, 3), "7: hovered: red at .5: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);
    gp_simulate_mouse(app, GP_SIM_DOWN, cx, cy, 1, 0);
    gp_sleep(co_, 0.02);
    f = frame();
    CHECK(f && is_rgb(px_at(f, (int)cx, (int)cy), 0, 0, 128, 3), "7: pressed: blue, still at .5");
    gp_simulate_mouse(app, GP_SIM_UP, cx, cy, 1, 0);
    gp_sleep(co_, 0.02);

    /* 8: gp_hover_bg wins */
    at(8, &cx, &cy);
    p = sample(8, cx, cy, 0, 0);
    CHECK(p && is_color(p, GREEN, 2), "8: gp_hover_bg over the state style: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);

    /* 9: the size does not change, the background does */
    at(9, &cx, &cy);
    float w0 = box(9)->size.w, h0 = box(9)->size.h;
    p = sample(9, cx, cy, 0, 0);
    CHECK(p && is_color(p, RED, 2) && box(9)->size.w == w0 && box(9)->size.h == h0 && w0 == 80 && h0 == 50,
          "9: hovered, the background changes and the box stays %g x %g (was %g x %g)", box(9)->size.w, box(9)->size.h, w0, h0);

    /* 10: in a cached view */
    at(10, &cx, &cy);
    p = sample(10, AWAY, 0, 0);
    CHECK(p && is_color(p, FILL, 2), "10: cached, at rest");
    p = sample(10, cx, cy, 0, 0);
    CHECK(p && is_color(p, RED, 2), "10: cached, hovered: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);
    p = sample(10, AWAY, 0, 0);
    CHECK(p && is_color(p, FILL, 2), "10: cached, and back");

    /* 11: a gradient: red at the left, blue at the right */
    at(11, &cx, &cy);
    int gx = (int)box(11)->size.w / 2 - 4;
    p = sample(11, cx, cy, -gx, 0);
    CHECK(p && p[0] > 200 && p[2] < 60, "11: gradient, left is red: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);
    p = sample(11, cx, cy, gx, 0);
    CHECK(p && p[2] > 200 && p[0] < 60, "11: gradient, right is blue: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);

    /* 12: the child keeps its own color, the parent changes around it */
    at(12, &cx, &cy);
    p = sample(12, cx, cy, -30, -15);
    CHECK(p && is_color(p, GREEN, 2), "12: the child stays green: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);
    p = sample(12, cx, cy, 30, 0);
    CHECK(p && is_color(p, RED, 2), "12: the parent is red around it: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);

    /* 13: a style that sets nothing paintable changes nothing */
    at(13, &cx, &cy);
    p = sample(13, cx, cy, 0, 0);
    CHECK(p && is_color(p, FILL, 2), "13: nothing to paint: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);

    /* 14: only the top turns red */
    at(14, &cx, &cy);
    int tx = 0, ty = -(int)box(14)->size.h / 2 + 3, lx = -(int)box(14)->size.w / 2 + 3;
    p = sample(14, cx, cy, tx, ty);
    CHECK(p && is_color(p, RED, 2), "14: the top border is red: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);
    p = sample(14, cx, cy, lx, 0);
    CHECK(p && is_color(p, WHITE, 2), "14: the left stays white: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);
    p = sample(14, AWAY, tx, ty);
    CHECK(p && is_color(p, WHITE, 2), "14: at rest the top is white");

    /* 15: dashed: a dash covers x 0..12 of the top border, the gap 12..18 (period 3 widths of 6) */
    at(15, &cx, &cy);
    int dx_gap = -(int)box(15)->size.w / 2 + 15, dx_dash = -(int)box(15)->size.w / 2 + 6;
    p = sample(15, AWAY, dx_gap, ty);
    CHECK(p && is_color(p, WHITE, 2), "15: solid at rest, also where the gap will be");
    p = sample(15, cx, cy, dx_dash, ty);
    CHECK(p && is_color(p, WHITE, 2), "15: hovered, the dash is white: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);
    p = sample(15, cx, cy, dx_gap, ty);
    CHECK(p && is_color(p, FILL, 2), "15: hovered, the gap shows the fill: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);

    /* 16: only the top left corner is round */
    at(16, &cx, &cy);
    int hx = -(int)box(16)->size.w / 2 + 1, hy = -(int)box(16)->size.h / 2 + 1;
    p = sample(16, cx, cy, hx, hy);
    CHECK(p && is_color(p, BLACK, 6), "16: the top left corner is round: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);
    p = sample(16, cx, cy, -hx, hy);
    CHECK(p && is_color(p, FILL, 2), "16: the top right is not: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);

    /* 17: the border becomes a gradient: red at the left end, blue at the right end of the top side */
    at(17, &cx, &cy);
    p = sample(17, cx, cy, -(int)box(17)->size.w / 2 + 8, ty);
    CHECK(p && p[0] > 200 && p[2] < 60, "17: gradient border, left red: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);
    p = sample(17, cx, cy, (int)box(17)->size.w / 2 - 8, ty);
    CHECK(p && p[2] > 200 && p[0] < 60, "17: gradient border, right blue: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);

    /* 18: the shadow is 14 px to the right of the box: a pixel there is green while hovered, black at rest */
    at(18, &cx, &cy);
    int sx = (int)box(18)->size.w / 2 + 7;
    p = sample(18, AWAY, sx, 0);
    CHECK(p && is_color(p, BLACK, 2), "18: no shadow at rest: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);
    p = sample(18, cx, cy, sx, 0);
    CHECK(p && is_color(p, GREEN, 6), "18: the shadow beside the hovered box: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);

    /* 19: the gradient is gone under the color */
    at(19, &cx, &cy);
    p = sample(19, cx, cy, -(int)box(19)->size.w / 2 + 6, 0);
    CHECK(p && is_color(p, GREEN, 2), "19: a color replaces the gradient: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);

    /* 20: all four sides turn green, the red top too */
    at(20, &cx, &cy);
    p = sample(20, AWAY, 0, ty);
    CHECK(p && is_color(p, RED, 2), "20: the top is red at rest: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);
    p = sample(20, cx, cy, 0, ty);
    CHECK(p && is_color(p, GREEN, 2), "20: hovered, the top is green too: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);

    /* 21: focus alone brings the focus style and not the hover style */
    at(21, &cx, &cy);
    gp_focus_set(&c, g_focus2);
    gp_sleep(co_, 0.02);
    p = sample(21, AWAY, 0, 0);
    CHECK(p && is_rgb(p, 0x18 / 2, 0x20 / 2, 0x30 / 2, 3), "21: focused: half opaque, not red: %d %d %d", p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1);
    gp_focus_set(&c, (gp_focus){ 0 });
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
    gp_open_window(app, &(gp_window_options){ .title = "state_style_test", .width = 560, .height = 540 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "state_style_test: %d FAILURE(S)\n" : "state_style_test: all checks passed\n", failures);
    return failures != 0;
}
