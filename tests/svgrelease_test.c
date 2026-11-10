/*
 * gp_svg_release renders again only the views that show the released drawing: two cached views, each with a drawing of its own, and a third with none; releasing one
 * drawing renders its view again and leaves the others as they were (their render functions are not called). Needs a display.
 */
#include <gpuic/gpuic.h>

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
static gp_svg g_svg[2];
static int g_renders[3];
static gp_entity g_views[3];

typedef struct { int k; } V;

static gp_node *shows(void *self, gp_ctx *cx) {
    V *v = self;
    g_renders[v->k]++;
    if (v->k == 2) return gp_div(gp_w(40), gp_h(40), gp_bg(gp_rgb(0x336699)));
    return gp_svg_img(g_svg[v->k], gp_w(40), gp_h(40));
}

static gp_node *root(void *self, gp_ctx *cx) {
    return gp_div(gp_size_full(), gp_flex_row(), gp_gap(10), gp_p(10), gp_child_view_cached(g_views[0]), gp_child_view_cached(g_views[1]),
                  gp_child_view_cached(g_views[2]));
}

static void wait_draws(unsigned n) {
    gp_stats s;
    gp_app_stats(app, &s);
    unsigned long target = s.draws + n;
    for (int i = 0; i < 1000; i++) {
        gp_app_stats(app, &s);
        if (s.draws >= target) return;
        if (i > 5) gp_app_request_paint(app);
        gp_sleep(co_, 0.004);
    }
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    wait_draws(3);
    int r0[3];
    memcpy(r0, g_renders, sizeof r0);
    CHECK(r0[0] >= 1 && r0[1] >= 1 && r0[2] >= 1, "every view rendered: %d %d %d", r0[0], r0[1], r0[2]);
    gp_svg_release(app, g_svg[0]);
    wait_draws(2);
    CHECK(g_renders[0] == r0[0] + 1, "the view showing the released drawing rendered again: %d (was %d)", g_renders[0], r0[0]);
    CHECK(g_renders[1] == r0[1] && g_renders[2] == r0[2], "the others did not: %d %d (were %d %d)", g_renders[1], g_renders[2], r0[1], r0[2]);
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    const char *doc = "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10'><rect width='10' height='10' fill='red'/></svg>";
    g_svg[0] = gp_svg_load_memory(app, doc, strlen(doc));
    g_svg[1] = gp_svg_load_memory(app, doc, strlen(doc));
    for (int k = 0; k < 3; k++) g_views[k] = gp_view_new(app, sizeof(V), &(V){ k }, shows, NULL);
    gp_entity r = gp_view_new(app, 1, &(char){ 0 }, root, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "svgrelease_test", .width = 300, .height = 100 }, r);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "svgrelease_test: %d FAILURE(S)\n" : "svgrelease_test: all checks passed\n", failures);
    return failures != 0;
}
