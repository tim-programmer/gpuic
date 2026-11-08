/*
 * A rack: 40 panels (a title, a few knobs) and N cables, every one a cached view of its own, in a window of 1400 x 900. Measures the cost of a draw (the numbers GPUIC_STATS=1 prints,
 * but per phase of the run) when nothing changed, when a knob turns, when one cable moves, and when both do. Not a test: `cables_bench [cables]` (default 100) prints a table.
 * Draws wait for the display's vertical blank; the numbers are the CPU time each phase of a draw takes, not the frame time.
 */
#include "../src/internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NPANELS 40
#define MAXCABLES 400

static gp_app *app;
static gp_co *co_;
static int ncables = 100;

typedef struct { gp_point p[4]; int hue; } Cable;
typedef struct { float value; int id; } Panel;
static gp_entity cable_e[MAXCABLES], panel_e[NPANELS];

static gp_node *cable_view(void *self, gp_ctx *cx) {
    Cable *c = self;
    static const uint32_t pal[6] = { 0xf38ba8, 0xa6e3a1, 0x89b4fa, 0xf9e2af, 0xcba6f7, 0x94e2d5 };
    return gp_curve(c->p[0], c->p[1], c->p[2], c->p[3], gp_stroke_width(5), gp_stroke_color(gp_rgb(pal[c->hue % 6])), gp_hit_tolerance(4), gp_hover_bg(gp_rgb(0xffffff)));
}

static gp_node *knob(float v) {
    return gp_div(gp_w(26), gp_h(26), gp_rounded(13), gp_bg(gp_rgb(0x45475a)), gp_border(2), gp_border_color(gp_rgb(0x89b4fa)),
                  gp_div(gp_absolute(), gp_left(11), gp_top(2), gp_w(3), gp_h(9), gp_rotate(v * 270.f - 135.f), gp_bg(gp_rgb(0xcdd6f4))));
}

static gp_node *panel_view(void *self, gp_ctx *cx) {
    Panel *p = self;
    return gp_div(gp_w(160), gp_h(100), gp_p(8), gp_gap(6), gp_rounded(6), gp_bg(gp_rgb(0x313244)),
                  gp_text(gp_fmt("Module %d", p->id), gp_text_size(13), gp_text_color(gp_rgb(0xcdd6f4))),
                  gp_div(gp_flex_row(), gp_gap(8), knob(p->value), knob(0.3f), knob(0.8f)),
                  gp_text(gp_fmt("%.2f", (double)p->value), gp_text_size(11), gp_text_color(gp_rgb(0x9399b2))));
}

static gp_node *root_view(void *self, gp_ctx *cx) {
    gp_node *panels[NPANELS], *cables[MAXCABLES];
    for (int i = 0; i < NPANELS; i++) panels[i] = gp_child_view_cached(panel_e[i]);
    for (int i = 0; i < ncables; i++) cables[i] = gp_child_view_cached(cable_e[i]);
    return gp_div(gp_size_full(), gp_bg(gp_rgb(0x1e1e2e)), gp_div(gp_flex_row(), gp_flex_wrap(), gp_gap(10), gp_p(10), gp_children(panels, NPANELS)),
                  gp_children(cables, (size_t)ncables));
}

static unsigned long draws(void) { return app->stat_draws; }
static bool next_draw(void) {
    unsigned long target = draws() + 1;
    for (int i = 0; i < 2000 && draws() < target; i++) gp_sleep(co_, 0.001);
    return draws() >= target;
}

typedef struct { bool knob, cable; } Mode;
static unsigned rng = 12345;
static float frand(void) { rng = rng * 1664525u + 1013904223u; return (float)(rng >> 8) / 16777216.f; }

static void measure(const char *name, Mode m, int frames) {
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    int64_t b0 = app->stat_build_ns, l0 = app->stat_layout_ns, p0 = app->stat_paint_ns, s0 = app->stat_submit_ns, i0 = app->stat_instances;
    unsigned long d0 = draws(), v0 = app->stat_view_renders, a0 = app->stat_atlas_resets;
    for (int f = 0; f < frames; f++) {
        float t = (float)f / 30.f;
        if (m.knob) {
            Panel *p = gp_entity_get(app, panel_e[0]);
            p->value = 0.5f + 0.5f * sinf(t);
            gp_ctx c = gp_ctx_for(app, panel_e[0]);
            gp_notify(&c);
        }
        if (m.cable && ncables) {
            Cable *c = gp_entity_get(app, cable_e[0]);
            c->p[3] = (gp_point){ 900 + 200 * cosf(t), 600 + 150 * sinf(t) };
            c->p[2] = (gp_point){ 700 + 50 * sinf(t * 2), 700 };
            gp_ctx c2 = gp_ctx_for(app, cable_e[0]);
            gp_notify(&c2);
        }
        if (!m.knob && !m.cable) app->windows[0]->dirty_paint = true;
        if (!next_draw()) { printf("  (no draw)\n"); break; }
    }
    (void)cx;
    unsigned long n = draws() - d0;
    if (!n) return;
    double dn = (double)n;
    printf("%-26s %4lu draws  build %.3f  layout %.3f  paint %.3f  GL submit %.3f  total %.3f ms/draw;  %5ld instances; %.2f view renders/draw; atlas resets %lu\n", name, n,
           (double)(app->stat_build_ns - b0) / 1e6 / dn, (double)(app->stat_layout_ns - l0) / 1e6 / dn, (double)(app->stat_paint_ns - p0) / 1e6 / dn,
           (double)(app->stat_submit_ns - s0) / 1e6 / dn,
           (double)((app->stat_build_ns - b0) + (app->stat_layout_ns - l0) + (app->stat_paint_ns - p0) + (app->stat_submit_ns - s0)) / 1e6 / dn,
           (long)((app->stat_instances - i0) / (int64_t)n), (double)(app->stat_view_renders - v0) / dn, app->stat_atlas_resets - a0);
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    for (int i = 0; i < 40; i++) next_draw();
    printf("%d cables, %d panels, window 1400 x 900 (all of them cached views)\n", ncables, NPANELS);
    measure("nothing changes", (Mode){ false, false }, 200);
    measure("one knob turns", (Mode){ true, false }, 200);
    measure("one cable moves", (Mode){ false, true }, 200);
    measure("one of each", (Mode){ true, true }, 200);
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    /* (the panel and the cable are views with their own state) */
    for (int i = 0; i < NPANELS; i++) panel_e[i] = gp_view_new(app, sizeof(Panel), &(Panel){ 0.5f, i }, panel_view, NULL);
    for (int i = 0; i < ncables; i++) {
        Cable c;
        for (int k = 0; k < 4; k++) c.p[k] = (gp_point){ 50 + frand() * 1300, 150 + frand() * 700 };
        c.p[1] = (gp_point){ c.p[0].x + 100, c.p[0].y + 80 };
        c.p[2] = (gp_point){ c.p[3].x - 100, c.p[3].y + 80 };
        c.hue = i;
        cable_e[i] = gp_view_new(app, sizeof c, &c, cable_view, NULL);
    }
    gp_entity v = gp_view_new(app, 1, &(char){ 0 }, root_view, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "cables_bench", .width = 1400, .height = 900 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(int argc, char **argv) {
    if (argc > 1) ncables = atoi(argv[1]) > MAXCABLES ? MAXCABLES : atoi(argv[1]);
    return gp_application_run(ready, NULL);
}
