/*
 * Painting skips what the clip leaves out (paint.c, from layout's ink). Whatever it skips must not have shown: a frame painted with culling must be
 * the frame painted without it, to the byte. The traps: text given a height its wrapped lines run past (its box above the scroller's top, its lines
 * inside), and a focused box just below the scroller's bottom whose focus style's glow reaches into it. Needs a display, at scale 1.
 */
#include "../src/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...)                                \
    do {                                                \
        if(!(cond)) {                                   \
            failures++;                                 \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while(0)

static gp_app* app;
static gp_co* co_;
static gp_scroll g_scroll;
static gp_focus g_focus;

static gp_node* view(void* self, gp_ctx* cx)
{
    return gp_div(gp_size_full(), gp_bg(gp_rgb(0x101018)), gp_div(gp_flex_col(), gp_h(150), gp_w_full(), gp_flex_none(), gp_overflow_y_scroll(), gp_track_scroll(g_scroll), gp_text("text whose wrapped lines run on well past the height of twenty pixels it was given, six lines or so of it in all", gp_h(20), gp_w(200), gp_flex_none(), gp_text_color(gp_rgb(0xffffff))), gp_div(gp_h(160), gp_flex_none()), gp_div(gp_track_focus(g_focus), gp_w(120), gp_h(20), gp_flex_none(), gp_bg(gp_rgb(0x335533)), gp_focus_style(gp_group(gp_shadow(0, 0, 24, gp_rgb(0xffcc00))))), gp_div(gp_h(400), gp_flex_none())));
}

static uint8_t* frame(void)
{
    gp_debug_capture_next(app);
    for(int i = 0; i < 1000 && !app->capture_px; i++) {
        app->windows[0]->dirty_paint = true;
        gp_sleep(co_, 0.01);
    }
    return app->capture_px;
}

/* bytes that differ between the frame painted with culling and without (-1: no frame) */
static long culled_vs_all(void)
{
    uint8_t* a = frame();
    if(!a) {
        return -1;
    }
    size_t n = (size_t)app->capture_w * (size_t)app->capture_h * 3;
    uint8_t* copy = malloc(n);
    memcpy(copy, a, n);
    gp_paint_set_culling(false);
    uint8_t* b = frame();
    gp_paint_set_culling(true);
    long diff = -1;
    if(b) {
        diff = 0;
        for(size_t i = 0; i < n; i++) {
            diff += copy[i] != b[i];
        }
    }
    free(copy);
    return diff;
}

static void driver(gp_co* co, void* self, gp_ctx* cx, void* data)
{
    co_ = co;
    gp_sleep(co, 0.1);
    gp_ctx c = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_focus_set(&c, g_focus);
    const float at[] = { 0, 30, 60, 90 }; /* 0: the focused box 30 px below the bottom, its glow inside; 60: the text's box above the top, its lines inside */
    for(int i = 0; i < 4; i++) {
        gp_scroll_to(&c, g_scroll, 0, at[i]);
        gp_sleep(co, 0.05);
        long diff = culled_vs_all();
        CHECK(diff == 0, "scrolled to %g: the frame painted with culling differs from one without in %ld bytes", (double)at[i], diff);
    }
    gp_quit(app);
}

static void ready(gp_app* a, void* userdata)
{
    app = a;
    g_scroll = gp_scroll_new(app);
    g_focus = gp_focus_new(app);
    gp_entity v = gp_view_new(app, 1, &(char){ 0 }, view, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "cull_test", .width = 300, .height = 200 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void)
{
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "cull_test: %d FAILURE(S)\n" : "cull_test: all checks passed\n", failures);
    return failures != 0;
}
