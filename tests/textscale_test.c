/*
 * Text under a scale is made of glyphs rasterized for the size it has on the screen, not of the bitmaps of its own size stretched: a text of size 20 in a parent under
 * gp_scale(z) against the same text of size 20 z set where the first is seen. Checked three ways: the layout box of the text is the same at every zoom (nothing reflows);
 * the glyph instances in the scene are the cache's entries for the size on the screen, each at its origin taken through the transform and snapped to the pixel grid, with
 * no transform of their own; and the frame read back shows the same ink as the reference text (which stretched bitmaps do not). Then a cached view under a scale that
 * changes: painted again for the new size without its render function being called; and turned text (a scale and a rotation), which stays under its transform but with
 * bitmaps of the size on the screen. Needs a display.
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

#define TXT "Hamburg quiz 123"
#define SIZE 20.f
#define HALF 460   /* the references are this far to the right of what they are compared with */
enum { NZ = 5 };
static const float zs[NZ] = { 1.f, 2.f, 0.5f, 1.3f, 2.5f };
static const float tops[NZ] = { 0.f, 40.f, 110.f, 150.f, 210.f };
#define CACHED_TOP 300.f
#define TURNED_TOP 400.f

static gp_app *app;
static gp_co *co_;
static gp_entity root_view, inner_view;
static float zc = 1.f;   /* the scale the cached view is under */
static int inner_renders;

static gp_window *win(void) { return app->windows[0]; }

static gp_node *inner(void *self, gp_ctx *cx) {
    inner_renders++;
    return gp_text("Cached view", gp_text_size(SIZE), gp_absolute(), gp_left(10), gp_top(5));
}

static gp_node *render(void *self, gp_ctx *cx) {
    static gp_node *kids[2 * NZ];
    for (int i = 0; i < NZ; i++) {
        float z = zs[i];
        kids[2 * i] = gp_div(gp_absolute(), gp_left(0), gp_top(tops[i]), gp_w(300), gp_h(30), gp_scale(z), gp_transform_origin(0, 0),
                             gp_text(TXT, gp_text_size(SIZE), gp_absolute(), gp_left(10), gp_top(5)));
        kids[2 * i + 1] = gp_text(TXT, gp_text_size(SIZE * z), gp_absolute(), gp_left(HALF + 10 * z), gp_top(tops[i] + 5 * z));
    }
    return gp_div(
        gp_size_full(), gp_bg(gp_rgb(0x000000)), gp_text_color(gp_rgb(0xffffff)),
        gp_children(kids, 2 * NZ),
        gp_div(gp_absolute(), gp_left(0), gp_top(CACHED_TOP), gp_w(300), gp_h(30), gp_scale(zc), gp_transform_origin(0, 0), gp_child_view_cached(inner_view)),
        gp_text("Cached view", gp_text_size(SIZE * 2), gp_absolute(), gp_left(HALF + 20), gp_top(CACHED_TOP + 10)),
        gp_div(gp_absolute(), gp_left(40), gp_top(TURNED_TOP), gp_w(300), gp_h(30), gp_scale(2), gp_rotate(20), gp_transform_origin(0, 0),
               gp_text("Turned", gp_text_size(SIZE), gp_absolute(), gp_left(10), gp_top(5))));
}

static uint8_t *frame(void) {
    gp_debug_capture_next(app);
    for (int i = 0; i < 1000 && !app->capture_px; i++) {
        win()->dirty_paint = true;
        gp_sleep(co_, 0.01);
    }
    return app->capture_px;
}

static const gp_el *found[16];
static int nfound;
static void find_texts(const gp_el *n, const char *text) {
    if (n->kind == GP_EL_TEXT && n->text && !strcmp(n->text, text) && nfound < 16) found[nfound++] = n;
    for (uint32_t i = 0; i < n->nchildren; i++) find_texts(n->children[i], text);
}

static float quarter(float px) { return floorf(px * 4.f + 0.5f) * 0.25f; }

/* The glyphs of `text` (size SIZE, its element's box at (10, 5) in a parent at (px0, py0) scaled z about that corner) as the scene must hold them: the cache's entry for
 * the size on the screen, at the origin the transform gives, snapped as text under no transform is; counts the ones that are there so, without a transform. */
static void expect_glyphs(const gp_el *n, const char *text, float px0, float py0, float z, int *hit_out, int *want_out) {
    gp_text_system *ts = app->text;
    int64_t sel = GP_TEXT_SEL(n->ts);
    gp_glyph_pos g[64];
    float width, asc, desc;
    int ng = gp_text_shape(ts, sel, SIZE, text, strlen(text), g, 64, &width);
    gp_text_metrics(ts, sel, SIZE, &asc, &desc);
    float scale = win()->scale, lh = SIZE * GP_LINE_HEIGHT, px = quarter(SIZE * scale * z);
    float baseline = 5.f + (lh - (asc + desc)) / 2 + asc;   /* in the scaled parent */
    const gp_scene *sc = &win()->scene;
    int hit = 0, want = 0;
    for (int k = 0; k < ng; k++) {
        float ox = px0 + z * (10.f + g[k].x), oy = py0 + z * (baseline + g[k].y);
        gp_glyph_entry e = { 0 };
        float gx = 0, gy = 0;
        bool here = false;
        /* (the origin is computed in floats here and there: when it falls on the edge of a sub-pixel step either neighbour is right) */
        for (int d = -1; d <= 1 && !here; d++) {
            int pixel, bin;
            gp_text_snap_x(ts, g[k].font, px, ox * scale + (float)d * 0.01f, &pixel, &bin);
            e = gp_text_glyph_at(ts, g[k].font, g[k].glyph, px, bin);
            if (!e.w) break;
            gx = ((float)pixel + e.left) / scale;
            gy = (roundf(oy * scale) - e.top) / scale;
            for (size_t m = 0; m < sc->len && !here; m++) {
                const gp_instance *in = &sc->items[m];
                here = in->kind == GP_PRIM_GLYPH && fabsf(in->uv[0] - e.u / GP_ATLAS_SIZE) < 1e-6f && fabsf(in->uv[1] - e.v / GP_ATLAS_SIZE) < 1e-6f &&
                       fabsf(in->bounds[0] - gx) < 0.01f && fabsf(in->bounds[1] - gy) < 1.01f / scale && fabsf(in->bounds[2] - e.w / scale) < 0.01f &&
                       in->xf0[0] == 0.f && in->xf0[1] == 0.f && in->xf0[2] == 0.f && in->xf0[3] == 0.f && in->xf1[0] == 0.f && in->xf1[1] == 0.f;
            }
        }
        if (!e.w) continue;
        want++;
        if (here) hit++;
    }
    *hit_out = hit;
    *want_out = want;
}

static const uint8_t *px_at(const uint8_t *f, int x, int y) { return f + ((size_t)y * (size_t)app->capture_w + (size_t)x) * 3; }

/* the box of the ink (green channel over 40) in a part of the frame */
static bool ink_box(const uint8_t *f, int x0, int y0, int x1, int y1, int b[4]) {
    b[0] = x1; b[1] = y1; b[2] = x0 - 1; b[3] = y0 - 1;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            if (px_at(f, x, y)[1] > 40) {
                if (x < b[0]) b[0] = x;
                if (y < b[1]) b[1] = y;
                if (x > b[2]) b[2] = x;
                if (y > b[3]) b[3] = y;
            }
    return b[2] >= b[0];
}

/* The ink in rows [y0, y1) of the left part against the same rows HALF to the right: pixels that differ by more than 48 of 255, out of the pixels with ink in either.
 * False when one has no ink or their boxes are not the same to a pixel. (Measured: under 3 % differ when the glyphs are made for the size, a glyph now and then a
 * pixel aside where its origin rounds the other way; 35 % and more when the bitmaps of the small size are stretched. The checks allow 10 %.) */
static bool compare(const uint8_t *f, float fy0, float fy1, int *differ, int *inked) {
    float s = win()->scale;
    int half = (int)lroundf(HALF * s), y0 = (int)lroundf(fy0 * s), y1 = (int)lroundf(fy1 * s);
    if (y1 > app->capture_h) y1 = app->capture_h;
    int a[4], b[4];
    *differ = *inked = 0;
    if (!ink_box(f, 0, y0, half, y1, a) || !ink_box(f, half, y0, app->capture_w < 2 * half ? app->capture_w : 2 * half, y1, b)) return false;
    bool same_box = abs(a[0] - (b[0] - half)) <= 1 && abs(a[1] - b[1]) <= 1 && abs(a[2] - (b[2] - half)) <= 1 && abs(a[3] - b[3]) <= 1;
    int dx = b[0] - a[0], dy = b[1] - a[1];
    for (int y = a[1]; y <= a[3]; y++)
        for (int x = a[0]; x <= a[2]; x++) {
            if (x + dx >= app->capture_w || y + dy >= app->capture_h) continue;
            int p = px_at(f, x, y)[1], q = px_at(f, x + dx, y + dy)[1];
            if (p > 40 || q > 40) (*inked)++;
            if (abs(p - q) > 48) (*differ)++;
        }
    return same_box;
}

static void run(void) {
    gp_sleep(co_, 0.1);
    uint8_t *f = frame();
    CHECK(f != NULL, "a frame");
    if (!f) return;
    float s = win()->scale;

    /* ---- layout: the text takes the same room at every zoom */
    nfound = 0;
    find_texts(win()->tree, TXT);
    CHECK(nfound == 2 * NZ, "the texts are in the tree (%d)", nfound);
    if (nfound != 2 * NZ) return;
    for (int i = 1; i < NZ; i++)
        CHECK(found[2 * i]->size.w == found[0]->size.w && found[2 * i]->size.h == found[0]->size.h && found[2 * i]->pos.x == 10.f && found[2 * i]->pos.y == 5.f,
              "under a scale of %.1f the text's box is the one it has unscaled: %.2f x %.2f at (%.1f, %.1f), wanted %.2f x %.2f at (10, 5)", zs[i],
              found[2 * i]->size.w, found[2 * i]->size.h, found[2 * i]->pos.x, found[2 * i]->pos.y, found[0]->size.w, found[0]->size.h);

    /* ---- the scene: glyphs of the size on the screen, placed, with no transform; and the picture: the ink of the text set at that size */
    for (int i = 0; i < NZ; i++) {
        int hit, want;
        expect_glyphs(found[2 * i], TXT, 0.f, tops[i], zs[i], &hit, &want);
        CHECK(want >= 12 && hit == want, "scale %.1f: %d of %d glyphs are the cache's for %.2f px, where the transform puts their origins, untransformed", zs[i], hit, want,
              quarter(SIZE * s * zs[i]));
        int differ, inked;
        bool boxes = compare(f, tops[i], tops[i] + 35.f * zs[i], &differ, &inked);
        CHECK(boxes && inked > 100 && differ * 10 <= inked, "scale %.1f: the picture is the text set at size %.1f (boxes alike %d; %d of %d inked pixels differ)", zs[i],
              SIZE * zs[i], boxes, differ, inked);
    }

    /* ---- turned as well: still under its transform, but made of bitmaps of the size on the screen (a texel a pixel) */
    {
        const gp_scene *sc = &win()->scene;
        int turned = 0, right_size = 0;
        for (size_t m = 0; m < sc->len; m++) {
            const gp_instance *in = &sc->items[m];
            if (in->kind != GP_PRIM_GLYPH || (in->xf0[1] == 0.f && in->xf0[2] == 0.f)) continue;
            turned++;
            if (fabsf(in->bounds[2] * s * 2.f - in->uv[2] * GP_ATLAS_SIZE) < 0.01f && fabsf(in->bounds[3] * s * 2.f - in->uv[3] * GP_ATLAS_SIZE) < 0.01f) right_size++;
        }
        CHECK(turned == 6 && right_size == turned, "text scaled 2 and turned: %d glyphs under the transform, %d of them bitmaps of twice the size drawn at half", turned, right_size);
        int b[4];
        CHECK(ink_box(f, 0, (int)(TURNED_TOP * s), (int)(HALF * s), app->capture_h, b), "and it is drawn");
    }

    /* ---- a cached view: what was kept for one scale is not used under another, and its render function is not what makes it so */
    {
        int differ, inked;
        bool boxes = compare(f, CACHED_TOP, CACHED_TOP + 70.f, &differ, &inked);
        CHECK(!boxes, "at scale 1 the cached view's text is not the size of its reference (the check below can fail)");
        f = frame();   /* (kept by now) */
        int renders = inner_renders;
        unsigned long hits = app->stat_paint_hits;
        f = frame();
        CHECK(app->stat_paint_hits > hits, "the cached view's paint is reused while nothing changes (%lu, %lu)", hits, app->stat_paint_hits);
        zc = 2.f;
        gp_ctx cx = gp_ctx_for(app, root_view);
        gp_notify(&cx);
        gp_sleep(co_, 0.05);
        f = frame();
        CHECK(f != NULL, "a frame");
        if (!f) return;
        CHECK(inner_renders == renders, "the view inside was not rendered again (%d, %d)", renders, inner_renders);
        nfound = 0;
        find_texts(win()->tree, "Cached view");
        int hit = 0, want = 0;
        if (nfound == 2) expect_glyphs(found[0], "Cached view", 0.f, CACHED_TOP, 2.f, &hit, &want);
        CHECK(nfound == 2 && want >= 9 && hit == want, "under the new scale the cached view's glyphs are the ones for twice the size (%d of %d)", hit, want);
        boxes = compare(f, CACHED_TOP, CACHED_TOP + 70.f, &differ, &inked);
        CHECK(boxes && inked > 100 && differ * 10 <= inked, "and the picture is the text set at twice the size (boxes alike %d; %d of %d inked pixels differ)", boxes, differ, inked);
    }
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    run();
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    inner_view = gp_view_new(app, 1, &(char){ 0 }, inner, NULL);
    root_view = gp_view_new(app, 1, &(char){ 0 }, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "textscale_test", .width = 2 * HALF, .height = 520 }, root_view);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    setenv("GPUIC_LCD", "off", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "textscale_test: %d FAILURE(S)\n" : "textscale_test: all checks passed\n", failures);
    return failures != 0;
}
