/*
 * Skew transforms, scrollers inside transformed elements, and the text color of a state style, from frames read back and simulated input: a skewed box's edges against
 * the sheared rectangle and clicks on either side of them; the wheel over a scroller in a turned parent (where it is seen, not where its box would be); a scrollbar thumb in
 * a doubled parent pressed and dragged where it is seen, by the distance as the scroller sees it; and a hover style's text color reaching the text in the element, the text
 * of a cached view inside it, and not a child with a color of its own. Needs a display, at scale 1.
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
            failures++;                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                \
            printf(__VA_ARGS__);                                                       \
            printf("\n");                                                              \
        }                                                                              \
    } while (0)

static gp_app *app;
static gp_co *co_;
static gp_scroll sc_turned, sc_scaled;
static gp_entity inner_view;
static int clicks;
static void on_click(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) { clicks++; }

static gp_node *rows(void) {
    static gp_node *r[2][20];
    static int k;
    k ^= 1;   /* (gp_children keeps the array until its element is made, and both scrollers' arguments may be evaluated first) */
    for (int i = 0; i < 20; i++) r[k][i] = gp_div(gp_h(40), gp_flex_none(), gp_bg(i % 2 ? gp_rgb(0x303030) : gp_rgb(0x505050)));
    return gp_children(r[k], 20);
}
static gp_node *inner(void *self, gp_ctx *cx) { return gp_text("World", gp_text_size(24)); }

static gp_node *render(void *self, gp_ctx *cx) {
    return gp_div(
        gp_size_full(), gp_bg(gp_rgb(0x000000)),
        gp_div(gp_absolute(), gp_left(40), gp_top(40), gp_w(100), gp_h(60), gp_bg(gp_rgb(0xff0000)), gp_skew(30, 0), gp_on_click(on_click)),
        gp_div(gp_absolute(), gp_left(250), gp_top(50), gp_w(200), gp_h(100), gp_rotate(90),
               gp_div(gp_size_full(), gp_overflow_y_scroll(), gp_track_scroll(sc_turned), gp_flex_col(), rows())),
        gp_div(gp_absolute(), gp_left(40), gp_top(200), gp_w(100), gp_h(100), gp_scale(2),
               gp_div(gp_size_full(), gp_overflow_y_scroll(), gp_track_scroll(sc_scaled), gp_flex_col(), rows())),
        gp_div(gp_absolute(), gp_left(260), gp_top(260), gp_w(220), gp_h(120), gp_flex_col(), gp_text_color(gp_rgb(0xffffff)),
               gp_hover_style(gp_group(gp_text_color(gp_rgb(0x00ff00)))),
               gp_text("Hello", gp_text_size(24)),
               gp_text("Red", gp_text_size(24), gp_text_color(gp_rgb(0xff0000))),
               gp_child_view_cached(inner_view)));
}

static bool wait_draws(unsigned n) {
    unsigned long target = app->stat_draws + n;
    for (int i = 0; i < 1000 && app->stat_draws < target; i++) gp_sleep(co_, 0.004);
    return app->stat_draws >= target;
}
static uint8_t *frame(void) {
    gp_debug_capture_next(app);
    for (int i = 0; i < 1000 && !app->capture_px; i++) {
        app->windows[0]->dirty_paint = true;
        gp_sleep(co_, 0.01);
    }
    return app->capture_px;
}
static const uint8_t *px(const uint8_t *f, int x, int y) { return f + ((size_t)y * (size_t)app->capture_w + (size_t)x) * 3; }

/* light pixels in a band, by which channel stands out: white (all), green (only green), red (only red) */
static void inks(const uint8_t *f, int x0, int y0, int x1, int y1, int *white, int *green, int *red) {
    *white = *green = *red = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            const uint8_t *p = px(f, x, y);
            if (p[0] > 150 && p[1] > 150 && p[2] > 150) (*white)++;
            else if (p[1] > 150 && p[0] < 60 && p[2] < 60) (*green)++;
            else if (p[0] > 150 && p[1] < 60 && p[2] < 60) (*red)++;
        }
}

static void run(void) {
    wait_draws(2);
    gp_simulate_mouse(app, GP_SIM_MOVE, 5, 395, 0, 0);
    gp_sleep(co_, 0.03);
    uint8_t *f = frame();
    CHECK(f != NULL, "a frame");
    if (!f) return;

    /* ---- skew(30, 0) about the middle (y 70): a row at y is moved by tan 30 * (y - 70) */
    const float k = tanf(30.f * (float)M_PI / 180.f);
    int bad = 0;
    for (int y = 42; y < 99; y++) {
        float shift = k * ((float)y + 0.5f - 70.f), l = 40.f + shift, r = 140.f + shift;
        for (int x = 0; x < 200; x++) {
            float c = (float)x + 0.5f;
            if (fabsf(c - l) < 1.2f || fabsf(c - r) < 1.2f) continue;   /* (the edge pixels) */
            bool in = c > l && c < r;
            bool red = px(f, x, y)[0] > 200;
            if (red != in && bad++ < 4) printf("  skew at (%d, %d): red %d, wanted %d\n", x, y, red, in);
        }
    }
    CHECK(bad == 0, "a box skewed 30 degrees along x is the sheared rectangle (%d wrong)", bad);
    clicks = 0;
    gp_simulate_mouse(app, GP_SIM_MOVE, 30, 45, 0, 0);
    gp_simulate_mouse(app, GP_SIM_DOWN, 30, 45, 1, 0);
    gp_simulate_mouse(app, GP_SIM_UP, 30, 45, 1, 0);
    gp_sleep(co_, 0.03);
    CHECK(clicks == 1, "a click on the sheared part outside the box's own place lands (%d)", clicks);
    clicks = 0;
    gp_simulate_mouse(app, GP_SIM_MOVE, 45, 95, 0, 0);
    gp_simulate_mouse(app, GP_SIM_DOWN, 45, 95, 1, 0);
    gp_simulate_mouse(app, GP_SIM_UP, 45, 95, 1, 0);
    gp_sleep(co_, 0.03);
    CHECK(clicks == 0, "and in the box's own place, where the shear moved it away, it does not (%d)", clicks);

    /* ---- the wheel over a scroller in a parent turned a quarter: 200 x 100 at (250, 50) is seen as 100 x 200 at (300, 0) */
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_simulate_wheel(app, 310, 20, 0, -1, 0);
    gp_sleep(co_, 0.05);
    gp_point o = gp_scroll_offset(&cx, sc_turned);
    CHECK(o.y > 10.f, "the wheel where the turned scroller is seen scrolls it (%.1f)", o.y);
    float before = o.y;
    gp_simulate_wheel(app, 260, 60, 0, -1, 0);
    gp_sleep(co_, 0.05);
    o = gp_scroll_offset(&cx, sc_turned);
    CHECK(o.y == before, "and where its box would be without the turn it does not (%.1f, %.1f)", before, o.y);

    /* ---- a thumb in a parent scaled 2x about its middle (90, 250): layout x 132..140 is seen at 174..190, the thumb (100 tall of 800: 12.5 px) at y 150 .. 175 */
    gp_simulate_mouse(app, GP_SIM_MOVE, 184, 158, 0, 0);
    gp_simulate_mouse(app, GP_SIM_DOWN, 184, 158, 1, 0);
    gp_simulate_mouse(app, GP_SIM_MOVE, 184, 208, 1, 0);   /* 50 px on screen is 25 in the scroller */
    gp_sleep(co_, 0.05);
    float o1 = gp_scroll_offset(&cx, sc_scaled).y;
    gp_simulate_mouse(app, GP_SIM_MOVE, 184, 228, 1, 0);   /* 20 more on screen: 10 in the scroller */
    gp_sleep(co_, 0.05);
    float o2 = gp_scroll_offset(&cx, sc_scaled).y;
    /* thumb and track in the scroller's own units: the drag moved the thumb 25 of (track - thumb) there; unscaled it would have moved it 50, and the offset twice as far */
    CHECK(o1 > 100.f && o1 < 350.f, "a press where the doubled thumb is seen takes it, and 50 px on screen drags it 25 of its own (offset %.1f)", o1);
    CHECK(fabsf(o2 - o1 * 35.f / 25.f) < 3.f, "the offset goes with the drag as the scroller sees it: 35 / 25 of that after 20 px more (%.1f, wanted %.1f)", o2, o1 * 35.f / 25.f);
    gp_simulate_mouse(app, GP_SIM_UP, 184, 208, 1, 0);

    /* ---- a hover style's text color */
    gp_simulate_mouse(app, GP_SIM_MOVE, 5, 395, 0, 0);
    gp_sleep(co_, 0.05);
    f = frame();
    int w0, g0, r0;
    inks(f, 260, 260, 480, 380, &w0, &g0, &r0);
    CHECK(w0 > 100 && g0 < 5 && r0 > 50, "before: white text and red text (%d white, %d green, %d red)", w0, g0, r0);
    gp_simulate_mouse(app, GP_SIM_MOVE, 470, 370, 0, 0);
    gp_sleep(co_, 0.05);
    f = frame();
    int w1, g1, r1;
    inks(f, 260, 260, 480, 380, &w1, &g1, &r1);
    CHECK(w1 < 5 && g1 > 100 && abs(r1 - r0) < 5, "hovered: the text turns green, the cached view's too, and the red child stays red (%d white, %d green, %d red)", w1, g1, r1);
    int wv, gv, rv;
    inks(f, 260, 320, 480, 380, &wv, &gv, &rv);
    CHECK(wv < 5 && gv > 30, "the text of the cached view inside it is green (%d white, %d green)", wv, gv);
    gp_simulate_mouse(app, GP_SIM_MOVE, 5, 395, 0, 0);
    gp_sleep(co_, 0.05);
    f = frame();
    int w2, g2, r2;
    inks(f, 260, 260, 480, 380, &w2, &g2, &r2);
    CHECK(abs(w2 - w0) < 5 && g2 < 5, "and back when the pointer leaves (%d white, %d green)", w2, g2);
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    run();
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    sc_turned = gp_scroll_new(app);
    sc_scaled = gp_scroll_new(app);
    inner_view = gp_view_new(app, 1, &(char){ 0 }, inner, NULL);
    gp_entity v = gp_view_new(app, 1, &(char){ 0 }, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "xform2_test", .width = 500, .height = 400 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "xform2_test: %d FAILURE(S)\n" : "xform2_test: all checks passed\n", failures);
    return failures != 0;
}
