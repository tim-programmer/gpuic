/*
 * Offscreen layers, from frames read back from the renderer: an opacity fades an element and what is in it as one thing (a child over its parent's
 * background is not mixed with it); groups nest; a translucent stroke that crosses itself is no darker at the crossing, nor an opaque one at an
 * opacity; a group under a rotation covers its rotated corners; a clip around a group still cuts it; deeper nesting than the renderer keeps layers
 * for still multiplies out right; a shadow does not show through its box; text in a layer is not LCD-antialiased (and outside one still is); an
 * element with one thing to paint takes no layer. Needs a display, at scale 1.
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

#define BLACK gp_rgb(0x000000)
#define RED gp_rgb(0xff0000)
#define GREEN gp_rgb(0x00ff00)
#define BLUE gp_rgb(0x0000ff)
#define WHITE gp_rgb(0xffffff)
#define FILL gp_rgb(0x4080c0)

static const gp_point cross1[] = { { 300, 20 }, { 380, 100 }, { 380, 20 }, { 300, 100 } };      /* crosses itself at (340, 60) */
static const gp_point cross2[] = { { 300, 120 }, { 380, 200 }, { 380, 120 }, { 300, 200 } };    /* ... at (340, 160) */
static const gp_point straight[] = { { 300, 230 }, { 330, 232 }, { 360, 230 }, { 400, 230 } };  /* never near itself: no layer */
static const gp_point hairpin[] = { { 300, 260 }, { 380, 260 }, { 380, 264 }, { 300, 264 } };   /* back along itself 4 px away, 10 wide: a layer */

static gp_entity g_root, g_boxview, g_textview;
static int g_phase;   /* 1: row 9 is put in a group, and the child of row 10 moves */

static gp_node *box_render(void *self, gp_ctx *cx) { return gp_div(gp_w(60), gp_h(20), gp_bg(BLUE)); }
static gp_node *text_render(void *self, gp_ctx *cx) { return gp_text("Kept"); }

static gp_node *deep(int k) {
    if (k == 0) return gp_div(gp_w(40), gp_h(20), gp_bg(WHITE));
    return gp_div(gp_opacity(0.9f), deep(k - 1));
}

static gp_node *render(void *self, gp_ctx *cx) {
    return gp_div(
        gp_size_full(), gp_flex_col(), gp_items_start(), gp_gap(10), gp_p(10), gp_bg(BLACK),
        /* 0: a group: blue over red, at .5 */
        gp_div(gp_w(100), gp_h(30), gp_flex_none(), gp_bg(RED), gp_opacity(0.5f), gp_div(gp_w(50), gp_h(30), gp_bg(BLUE))),
        /* 1: nested: .5 around (.5 green around white) */
        gp_div(gp_w(100), gp_h(30), gp_flex_none(), gp_opacity(0.5f),
               gp_div(gp_w(100), gp_h(30), gp_bg(GREEN), gp_opacity(0.5f), gp_div(gp_w(50), gp_h(30), gp_bg(WHITE)))),
        /* 2: turned 45 degrees: red at .5 with blue in the middle */
        gp_div(gp_w(60), gp_h(60), gp_flex_none(), gp_ml(20), gp_bg(RED), gp_opacity(0.5f), gp_rotate(45), gp_p(20), gp_div(gp_w(20), gp_h(20), gp_bg(BLUE))),
        /* 3: a clip around a group that is wider than it */
        gp_div(gp_w(100), gp_h(30), gp_flex_none(), gp_overflow_hidden(),
               gp_div(gp_w(200), gp_h(30), gp_flex_none(), gp_bg(RED), gp_opacity(0.5f), gp_div(gp_w(20), gp_h(30), gp_bg(BLUE)))),
        /* 4: ten groups in one another, each at .9 */
        deep(10),
        /* 5: a shadow under its box, at .5 */
        gp_div(gp_w(100), gp_h(30), gp_flex_none(), gp_bg(FILL), gp_shadow(0, 0, 0, GREEN), gp_opacity(0.5f)),
        /* 6: text outside a layer and inside one */
        gp_div(gp_flex_row(), gp_gap(20), gp_text("Out"), gp_div(gp_opacity(0.5f), gp_text("In"))),
        /* 7: one thing to paint: no layer */
        gp_div(gp_w(30), gp_h(20), gp_flex_none(), gp_bg(RED), gp_opacity(0.5f)),
        /* 8: a cached view alone in a group (what it painted is used again: the layer's region has to hold it all the same) */
        gp_div(gp_flex_none(), gp_opacity(0.5f), gp_child_view_cached(g_boxview)),
        /* 9: a cached view's text, outside any layer, then (phase 1) in a group */
        g_phase ? gp_div(gp_opacity(0.5f), gp_child_view_cached(g_textview)) : gp_div(gp_child_view_cached(g_textview)),
        /* 10: a group whose child moves (phase 1): where it was is cleared in the layer */
        gp_div(gp_w(100), gp_h(20), gp_flex_none(), gp_opacity(0.5f), gp_flex_row(), gp_div(gp_w(30), gp_h(20), gp_bg(WHITE), gp_ml(g_phase ? 60 : 0))),
        /* 11: a group with nothing to paint: no layer */
        gp_div(gp_flex_none(), gp_opacity(0.5f), gp_div(gp_w(10), gp_h(10))),
        /* the strokes, absolute in the root: translucent; opaque at an opacity */
        gp_polyline(cross1, 4, gp_stroke_width(10), gp_stroke_color(gp_rgba(0xffffff80))),
        gp_polyline(cross2, 4, gp_stroke_width(10), gp_stroke_color(WHITE), gp_opacity(0.5f)),
        gp_polyline(straight, 4, gp_stroke_width(6), gp_stroke_color(gp_rgba(0xffffff80))),
        gp_polyline(hairpin, 4, gp_stroke_width(10), gp_stroke_color(gp_rgba(0xffffff80))));
}

static bool wait_draws(unsigned n) {
    unsigned long target = app->stat_draws + n;
    for (int i = 0; i < 1000 && app->stat_draws < target; i++) {
        if (i > 5) win()->dirty_paint = true;
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
static const uint8_t *px(const uint8_t *f, float x, float y) { return f + ((size_t)(int)y * (size_t)app->capture_w + (size_t)(int)x) * 3; }
static bool rgb(const uint8_t *p, int r, int g, int b, int tol) { return abs(p[0] - r) <= tol && abs(p[1] - g) <= tol && abs(p[2] - b) <= tol; }
#define P3(p) (p)[0], (p)[1], (p)[2]

static void run(void) {
    gp_fonts_set_antialiasing(app, GP_AA_LCD_RGB);
    win()->dirty_render = true;
    wait_draws(3);
    CHECK(win()->tree->nchildren == 16, "%u children", win()->tree->nchildren);
    if (win()->tree->nchildren != 16) return;
    uint8_t *f = frame();
    if (!f) { CHECK(0, "no frame"); return; }
    const uint8_t *p;

    /* 0: where blue covers red the group is blue at .5; red alone, red at .5 (one at a time would give 64 0 128 over the red) */
    gp_el *e = row(0);
    p = px(f, e->pos.x + 25, e->pos.y + 15);
    CHECK(rgb(p, 0, 0, 128, 3), "0: blue over red, faded as one: %d %d %d", P3(p));
    p = px(f, e->pos.x + 75, e->pos.y + 15);
    CHECK(rgb(p, 128, 0, 0, 3), "0: red alone: %d %d %d", P3(p));

    /* 1: white in the inner group: .5 x .5 of white; green alone: .25 of green */
    e = row(1);
    p = px(f, e->pos.x + 25, e->pos.y + 15);
    CHECK(rgb(p, 64, 64, 64, 3), "1: white through two groups: %d %d %d", P3(p));
    p = px(f, e->pos.x + 75, e->pos.y + 15);
    CHECK(rgb(p, 0, 64, 0, 3), "1: green through two groups: %d %d %d", P3(p));

    /* 2: the middle is blue at .5; a point inside the turned square but outside the box it had (its top corner) is red at .5; outside, black */
    e = row(2);
    float cx = e->pos.x + 30, cy = e->pos.y + 30;
    p = px(f, cx, cy);
    CHECK(rgb(p, 0, 0, 128, 3), "2: the middle of the turned group: %d %d %d", P3(p));
    p = px(f, cx, cy - 38);   /* (the corner reaches 42.4 above the middle) */
    CHECK(rgb(p, 128, 0, 0, 3), "2: its top corner, above the box it had: %d %d %d", P3(p));
    p = px(f, cx - 38, cy - 38);
    CHECK(rgb(p, 0, 0, 0, 3), "2: beside the corner, nothing: %d %d %d", P3(p));

    /* 3: inside the clip the group shows; past it (where the group's own box goes on) nothing */
    e = row(3);
    p = px(f, e->pos.x + 50, e->pos.y + 15);
    CHECK(rgb(p, 128, 0, 0, 3), "3: inside the clip: %d %d %d", P3(p));
    p = px(f, e->pos.x + 150, e->pos.y + 15);
    CHECK(rgb(p, 0, 0, 0, 3), "3: past the clip: %d %d %d", P3(p));

    /* 4: .9 ten times over white: 89 (more groups than layers kept: the rest multiply out) */
    e = row(4);
    p = px(f, e->pos.x + 20, e->pos.y + 10);
    CHECK(rgb(p, 89, 89, 89, 3), "4: ten groups at .9: %d %d %d", P3(p));

    /* 5: the box hides its shadow and fades as one: the fill at .5, no green */
    e = row(5);
    p = px(f, e->pos.x + 50, e->pos.y + 15);
    CHECK(rgb(p, 0x40 / 2, 0x80 / 2, 0xc0 / 2, 3), "5: the box over its shadow at .5: %d %d %d", P3(p));

    /* 7: no layer for one thing: the frame is the same */
    e = row(7);
    p = px(f, e->pos.x + 15, e->pos.y + 10);
    CHECK(rgb(p, 128, 0, 0, 3), "7: a box alone at .5: %d %d %d", P3(p));

    /* the strokes: the crossing is as bright as the rest of the stroke */
    p = px(f, 340, 60);
    const uint8_t *q = px(f, 320, 40);
    CHECK(rgb(p, 128, 128, 128, 3) && rgb(q, 128, 128, 128, 3), "a translucent stroke: crossing %d %d %d, elsewhere %d %d %d", P3(p), P3(q));
    p = px(f, 340, 160);
    q = px(f, 320, 140);
    CHECK(rgb(p, 128, 128, 128, 3) && rgb(q, 128, 128, 128, 3), "an opaque stroke at .5: crossing %d %d %d, elsewhere %d %d %d", P3(p), P3(q));
    p = px(f, 330, 231);
    q = px(f, 315, 231);
    CHECK(rgb(p, 128, 128, 128, 3) && rgb(q, 128, 128, 128, 3), "a translucent stroke drawn without a layer, at its joints: %d %d %d, %d %d %d", P3(p), P3(q));
    p = px(f, 340, 262);
    CHECK(rgb(p, 128, 128, 128, 3), "a hairpin's two legs over each other, once: %d %d %d", P3(p));

    /* the scene: which layers there are, and the kind of glyph each text was drawn with */
    const gp_scene *sc = &win()->scene;
    int begins = 0, ends = 0, depth = 0, maxdepth = 0, lcd_out = 0, gray_in = 0, lcd_in = 0;
    bool in_text_layer = false;
    for (size_t i = 0; i < sc->len; i++) {
        int k = (int)sc->items[i].kind;
        if (k == GP_PRIM_LAYER_BEGIN) { begins++; depth++; if (depth > maxdepth) maxdepth = depth; }
        if (k == GP_PRIM_LAYER_END) { ends++; depth--; }
        if (k == GP_PRIM_LCD_GLYPH) { if (depth) lcd_in++; else lcd_out++; }
        if (k == GP_PRIM_GLYPH && depth) { gray_in++; in_text_layer = true; }
    }
    /* 0, 1 (two), 2, 3, 4 (eight of ten), 5, 6, 8, 10, one for each crossing stroke and the hairpin; none for 7, 9, 11 or the straight stroke */
    CHECK(begins == 20 && ends == 20 && depth == 0, "%d layers begun, %d ended, want 20", begins, ends);
    CHECK(maxdepth == GP_MAX_LAYERS, "layers nest %d deep, want %d", maxdepth, GP_MAX_LAYERS);
    CHECK(lcd_out > 0 && gray_in > 0 && lcd_in == 0 && in_text_layer, "glyphs: %d LCD outside a layer, %d gray and %d LCD in one", lcd_out, gray_in, lcd_in);
    /* every BEGIN's region is whole device pixels and holds its END's */
    for (size_t i = 0; i < sc->len; i++) {
        const gp_instance *in = &sc->items[i];
        if ((int)in->kind != GP_PRIM_LAYER_BEGIN) continue;
        CHECK(in->bounds[2] > 0 && in->bounds[3] > 0 && in->bounds[0] == floorf(in->bounds[0]) && in->bounds[1] == floorf(in->bounds[1]) &&
                  in->bounds[2] == floorf(in->bounds[2]) && in->bounds[3] == floorf(in->bounds[3]),
              "a layer's region %g %g %g %g", in->bounds[0], in->bounds[1], in->bounds[2], in->bounds[3]);
    }


    /* 8: the cached box in the group (painted before, used again): blue at .5 */
    e = row(8);
    p = px(f, e->pos.x + 30, e->pos.y + 10);
    CHECK(rgb(p, 0, 0, 128, 3), "8: a cached view in a group: %d %d %d", P3(p));

    /* phase 1: 9 goes into a group (its view is not built again: its painted copy must not be used, it has LCD glyphs), 10's child moves */
    int lcd_before = 0;
    for (size_t i = 0; i < sc->len; i++) lcd_before += (int)sc->items[i].kind == GP_PRIM_LCD_GLYPH;
    unsigned long hits0 = app->stat_paint_hits;
    g_phase = 1;
    gp_ctx rc = gp_ctx_for(app, g_root);
    gp_notify(&rc);
    wait_draws(2);
    f = frame();
    if (!f) return;
    CHECK(app->stat_paint_hits > hits0, "the cached views were used again");
    int lcd_in9 = 0, lcd_after = 0;
    depth = 0;
    for (size_t i = 0; i < sc->len; i++) {
        int k = (int)sc->items[i].kind;
        if (k == GP_PRIM_LAYER_BEGIN) depth++;
        if (k == GP_PRIM_LAYER_END) depth--;
        if (k == GP_PRIM_LCD_GLYPH) { lcd_after++; if (depth) lcd_in9++; }
    }
    CHECK(lcd_in9 == 0 && lcd_after < lcd_before, "9: in a group its text is gray: %d LCD glyphs in layers (%d before, %d now)", lcd_in9, lcd_before, lcd_after);
    e = row(10);
    p = px(f, e->pos.x + 15, e->pos.y + 10);
    q = px(f, e->pos.x + 75, e->pos.y + 10);
    CHECK(rgb(p, 0, 0, 0, 3) && rgb(q, 128, 128, 128, 3), "10: the child moved: where it was %d %d %d, where it is %d %d %d", P3(p), P3(q));

    gp_fonts_set_antialiasing(app, GP_AA_AUTO);
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    run();
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    g_boxview = gp_view_new(app, 1, &(char){ 0 }, box_render, NULL);
    g_textview = gp_view_new(app, 1, &(char){ 0 }, text_render, NULL);
    g_root = gp_view_new(app, 1, &(char){ 0 }, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "layer_test", .width = 420, .height = 620 }, g_root);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "layer_test: %d FAILURE(S)\n" : "layer_test: all checks passed\n", failures);
    return failures != 0;
}
