/*
 * Cached views that paint nothing. A view whose render returns an empty div is cached and reused as empty (its paint is
 * reused: paint_hits grows), and a view that switches between drawing something and drawing nothing paints what it draws
 * each time. The first frames had a memcpy to the empty view's never-allocated copy (NULL, length 0): undefined behavior,
 * which a build with -fsanitize=undefined reports. Needs a display.
 */
#include "../src/internal.h"   /* (gp_debug_capture_next: the frame read back) */

#include <stdio.h>
#include <stdlib.h>

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
static gp_entity g_empty, g_toggle, g_root;
static bool g_show;   /* what g_toggle draws: a red box, or nothing */

static gp_node *empty_view(void *self, gp_ctx *cx) { return gp_div(); }
static gp_node *toggle_view(void *self, gp_ctx *cx) {
    return g_show ? gp_div(gp_w(60), gp_h(60), gp_bg(gp_rgb(0xff0000))) : gp_div();
}
static gp_node *root(void *self, gp_ctx *cx) {
    /* (the two cached views first, so that the first one paints into a scene with nothing in it yet) */
    return gp_div(gp_size_full(), gp_child_view_cached(g_empty), gp_child_view_cached(g_toggle));
}

static gp_stats stats(void) {
    gp_stats s;
    gp_app_stats(app, &s);
    return s;
}

static void wait_draws(unsigned n) {
    unsigned long target = stats().draws + n;
    for (int i = 0; i < 1000 && stats().draws < target; i++) {
        if (i > 5) gp_app_request_paint(app);
        gp_sleep(co_, 0.004);
    }
}

/* the red box's pixel, in the frame drawn next */
static bool red_shows(void) {
    gp_debug_capture_next(app);
    gp_app_request_paint(app);
    wait_draws(2);
    const uint8_t *px = app->capture_px;
    int w = app->capture_w, h = app->capture_h;
    if (!px || w < 30 || h < 30) return false;
    const uint8_t *p = px + ((size_t)30 * (size_t)w + 30) * 3;   /* (the box is 60 x 60 at the top left: 30,30 is in it at any scale) */
    return p[0] > 200 && p[1] < 60 && p[2] < 60;
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    wait_draws(3);
    CHECK(!red_shows(), "nothing drawn at first");

    gp_stats s0 = stats();
    gp_app_request_paint(app);
    wait_draws(4);
    gp_stats s1 = stats();
    CHECK(s1.paint_hits >= s0.paint_hits + 4, "the empty views are reused, painting again: paint hits %lu -> %lu", s0.paint_hits, s1.paint_hits);
    CHECK(s1.paint_misses == s0.paint_misses, "and nothing is painted anew: misses %lu -> %lu", s0.paint_misses, s1.paint_misses);

    for (int round = 0; round < 2; round++) {
        g_show = true;
        gp_ctx c = gp_ctx_for(app, g_toggle);
        gp_notify(&c);
        wait_draws(2);
        CHECK(red_shows(), "round %d: the view that drew nothing now draws its box", round);
        g_show = false;
        gp_notify(&c);
        wait_draws(2);
        CHECK(!red_shows(), "round %d: and then nothing again", round);
    }
    s0 = stats();
    gp_app_request_paint(app);
    wait_draws(3);
    s1 = stats();
    CHECK(s1.paint_hits > s0.paint_hits && s1.paint_misses == s0.paint_misses, "empty again, reused again: hits %lu -> %lu, misses %lu -> %lu",
          s0.paint_hits, s1.paint_hits, s0.paint_misses, s1.paint_misses);
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    g_empty = gp_view_new(app, 1, &(char){ 0 }, empty_view, NULL);
    g_toggle = gp_view_new(app, 1, &(char){ 0 }, toggle_view, NULL);
    g_root = gp_view_new(app, 1, &(char){ 0 }, root, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "emptyview_test", .width = 120, .height = 100 }, g_root);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "emptyview_test: %d FAILURE(S)\n" : "emptyview_test: all checks passed\n", failures);
    return failures != 0;
}
