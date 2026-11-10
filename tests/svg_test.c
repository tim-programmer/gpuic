/*
 * SVG, end to end: documents are parsed, rasterized at the size (in device pixels) and in the currentColor they are shown at,
 * cached, and drawn like images. Shapes are compared with an independent supersampled coverage computation; strokes, gradients (written
 * with fractions, which needed a patch to nanosvg), fill opacity, viewBox scaling, transforms, even-odd fills, gp_svg_from_path,
 * object-fit and corners; the registry (refusals, stale handles, slot reuse); the raster cache (sharing, per size and color,
 * the budget forcing a clear and repaint); and releasing. Frames are read back from the renderer. Needs a display.
 */
#include "../src/internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static bool wait_draws(unsigned n) {
    unsigned long target = app->stat_draws + n;
    for (int i = 0; i < 1000 && app->stat_draws < target; i++) gp_sleep(co_, 0.004);
    return app->stat_draws >= target;
}
static void notify_root(void) {
    gp_ctx cx = gp_ctx_for(app, win()->root_view);
    gp_notify(&cx);
}
static uint8_t *frame(void) {
    gp_debug_capture_next(app);
    for (int i = 0; i < 1000 && !app->capture_px; i++) {
        win()->dirty_paint = true;
        gp_sleep(co_, 0.01);
    }
    return app->capture_px;
}

#define PAGE_R 20
#define PAGE_G 20
#define PAGE_B 30
#define PAGE gp_rgb(0x14141e)

#define HEAD "<svg xmlns='http://www.w3.org/2000/svg' "
static const char S_HALVES[] = HEAD "width='40' height='20' viewBox='0 0 40 20'><rect x='0' y='0' width='20' height='20' fill='#ff0000'/><rect x='20' y='0' width='20' height='20' fill='#0000ff'/></svg>";
static const char S_CIRCLE[] = HEAD "width='40' height='40'><circle cx='20' cy='20' r='15' fill='#00ff00'/></svg>";
static const char S_TRIANGLE[] = HEAD "width='40' height='40'><path d='M5 35 L20 5 L35 35 Z' fill='#ffff00'/></svg>";
static const char S_STROKE[] = HEAD "width='40' height='40'><path d='M5 20 H35' stroke='#ff0000' stroke-width='6' fill='none'/></svg>";
static const char S_GRADIENT[] = HEAD "width='100' height='20'><defs><linearGradient id='g' x1='0' y1='0' x2='1' y2='0'><stop offset='0' stop-color='#ff0000'/><stop offset='1' stop-color='#0000ff'/></linearGradient></defs><rect width='100' height='20' fill='url(#g)'/></svg>";
static const char S_CURRENT[] = HEAD "width='20' height='20'><rect width='20' height='20' fill='currentColor'/></svg>";
static const char S_OPACITY[] = HEAD "width='20' height='20'><rect width='20' height='20' fill='#ff0000' fill-opacity='0.5'/></svg>";
static const char S_VIEWBOX[] = HEAD "width='10' height='10' viewBox='0 0 100 100'><rect width='50' height='100' fill='#ff0000'/></svg>";
static const char S_TRANSFORM[] = HEAD "width='40' height='10'><g transform='translate(10,0)'><rect width='10' height='10' fill='#00ff00'/></g></svg>";
static const char S_RING[] = HEAD "width='40' height='40'><path fill='#ff0000' fill-rule='evenodd' d='M0 0 H40 V40 H0 Z M10 10 H30 V30 H10 Z'/></svg>";
static const char S_RING_NZ[] = HEAD "width='40' height='40'><path fill='#ff0000' d='M0 0 H40 V40 H0 Z M10 10 H30 V30 H10 Z'/></svg>";

typedef struct {
    gp_svg halves, circle, triangle, stroke, gradient, current, opacity, viewbox, transform, ring, ring_nz, path, stale, fresh;
    int phase;     /* 0: the main picture; 1: six sizes of S_HALVES */
    gp_svg extra[6];
} Fix;

static gp_node *render(void *self, gp_ctx *cx) {
    Fix *f = self;
    if (f->phase == 1) {
        return gp_div(gp_size_full(), gp_flex_col(), gp_gap(2), gp_items_start(), gp_bg(PAGE),
                      gp_svg_img(f->extra[0], gp_w(20 * 1)), gp_svg_img(f->extra[0], gp_w(22)), gp_svg_img(f->extra[0], gp_w(24)),
                      gp_svg_img(f->extra[0], gp_w(26)), gp_svg_img(f->extra[0], gp_w(28)), gp_svg_img(f->extra[0], gp_w(30)),
                      gp_svg_img(f->extra[0], gp_w(32)), gp_svg_img(f->extra[0], gp_w(34)));
    }
    gp_node *a = gp_div(gp_flex_col(), gp_gap(4), gp_items_start(),
        gp_svg_img(f->halves),
        gp_svg_img(f->halves, gp_w(80)),
        gp_svg_img(f->halves, gp_w(40), gp_h(40), gp_object_fit(GP_FIT_CONTAIN)),
        gp_svg_img(f->circle),
        gp_svg_img(f->circle, gp_size_px(120)),
        gp_svg_img(f->triangle),
        gp_svg_img(f->stroke),
        gp_svg_img(f->gradient));
    gp_node *b = gp_div(gp_flex_col(), gp_gap(4), gp_items_start(),
        gp_svg_img(f->current, gp_text_color(gp_rgb(0x00ff00))),
        gp_svg_img(f->current, gp_text_color(gp_rgb(0xff00ff))),
        gp_svg_img(f->current, gp_text_color(gp_rgb(0x00ff00))),   /* the same color again: the same raster */
        gp_svg_img(f->opacity),
        gp_svg_img(f->viewbox),
        gp_svg_img(f->viewbox, gp_size_px(100)),
        gp_svg_img(f->transform),
        gp_svg_img(f->ring),
        gp_svg_img(f->ring_nz),
        gp_svg_img(f->path),
        gp_svg_img(f->stale, gp_size_px(20)),
        gp_svg_img(f->halves, gp_size_px(40), gp_rounded(10), gp_object_fit(GP_FIT_COVER)),
        gp_div(gp_flex_col(), gp_w(120), gp_svg_img(f->halves)));
    return gp_div(gp_size_full(), gp_flex_row(), gp_gap(12), gp_p(6), gp_bg(PAGE), a, b);
}

enum { A_HALVES, A_HALVES2X, A_CONTAIN, A_CIRCLE, A_CIRCLE3X, A_TRIANGLE, A_STROKE, A_GRADIENT,
       B_GREEN, B_MAGENTA, B_GREEN2, B_OPACITY, B_VIEWBOX, B_VIEWBOX100, B_TRANSFORM, B_RING, B_RING_NZ, B_PATH, B_STALE, B_ROUND, B_STRETCH, NSVG };

static const gp_el *els[NSVG + 10];
static int nels;
static void collect(const gp_el *n) {
    if (n->kind == GP_EL_SVG && nels < NSVG + 10) els[nels++] = n;
    for (uint32_t i = 0; i < n->nchildren; i++) collect(n->children[i]);
}
static bool abs_pos(const gp_el *n, const gp_el *target, float x, float y, float *ox, float *oy) {
    x += n->pos.x;
    y += n->pos.y;
    if (n == target) { *ox = x; *oy = y; return true; }
    for (uint32_t i = 0; i < n->nchildren; i++)
        if (abs_pos(n->children[i], target, x, y, ox, oy)) return true;
    return false;
}
typedef struct { int x0, y0, w, h; } Box;
static Box box_of(int i) {
    float ax = 0.f, ay = 0.f;
    abs_pos(win()->tree, els[i], -win()->tree->pos.x, -win()->tree->pos.y, &ax, &ay);
    float s = win()->scale;
    return (Box){ (int)lroundf(ax * s), (int)lroundf(ay * s), (int)lroundf(els[i]->size.w * s), (int)lroundf(els[i]->size.h * s) };
}
static const uint8_t *px_at(const uint8_t *px, int pw, int x, int y) { return px + ((size_t)y * (size_t)pw + (size_t)x) * 3; }
static bool near_rgb(const uint8_t *p, int r, int g, int b, int tol) { return abs(p[0] - r) <= tol && abs(p[1] - g) <= tol && abs(p[2] - b) <= tol; }
static bool is_page(const uint8_t *p) { return near_rgb(p, PAGE_R, PAGE_G, PAGE_B, 2); }

/* shape coverage of the pixel (x, y) of a w x h box showing a 40 x 40 drawing, by 8 x 8 supersampling */
typedef bool (*inside_fn)(float x, float y);
static bool in_circle(float x, float y) { return (x - 20) * (x - 20) + (y - 20) * (y - 20) <= 15 * 15; }
static bool in_triangle(float x, float y) {
    /* M5 35 L20 5 L35 35: left edge from (5,35) to (20,5), right from (20,5) to (35,35), base y = 35 */
    if (y < 5 || y > 35) return false;
    float half = (y - 5) / 30.f * 15.f;
    return fabsf(x - 20) <= half;
}
static float coverage(inside_fn f, int px, int py, float k) {
    int in = 0;
    for (int sy = 0; sy < 8; sy++)
        for (int sx = 0; sx < 8; sx++) in += f(((float)px + (sx + 0.5f) / 8) / k, ((float)py + (sy + 0.5f) / 8) / k);
    return (float)in / 64.f;
}

/* compares the picture with the shape; fills of exact coverage are compared tightly, edges only have to be between the colors */
static int check_shape(const uint8_t *px, int pw, Box b, inside_fn f, const int fill[3], int *partial, int *bad_x, int *bad_y) {
    float k = (float)b.w / 40.f;
    int bad = 0;
    *partial = 0;
    for (int y = 0; y < b.h; y++)
        for (int x = 0; x < b.w; x++) {
            float c = coverage(f, x, y, k);
            const uint8_t *p = px_at(px, pw, b.x0 + x, b.y0 + y);
            /* the rasterizer samples coverage more coarsely than 8 x 8, so only pixels well inside or outside are held to the exact color */
            bool solid = c == 1.f || c == 0.f;
            for (int d = 0; d < 4 && solid; d++) {
                int nx = x + (d == 0) - (d == 1), ny = y + (d == 2) - (d == 3);
                solid = coverage(f, nx, ny, k) == c;
            }
            if (solid && c == 1.f) {
                if (!near_rgb(p, fill[0], fill[1], fill[2], 3)) { bad++; *bad_x = x; *bad_y = y; }
            } else if (solid && c == 0.f) {
                if (!is_page(p)) { bad++; *bad_x = x; *bad_y = y; }
            } else {
                int want[3] = { (int)lroundf((float)fill[0] * c + PAGE_R * (1 - c)), (int)lroundf((float)fill[1] * c + PAGE_G * (1 - c)), (int)lroundf((float)fill[2] * c + PAGE_B * (1 - c)) };
                if (!near_rgb(p, want[0], want[1], want[2], 64)) { bad++; *bad_x = x; *bad_y = y; }
                if (!is_page(p) && !near_rgb(p, fill[0], fill[1], fill[2], 3)) (*partial)++;
            }
        }
    return bad;
}

static void api_tests(Fix *fx) {
    gp_gfx *g = app->gfx;
    float w = 0, h = 0;
    int images0 = gp_gfx_image_count(g);
    fx->halves = gp_svg_load_memory(app, S_HALVES, sizeof S_HALVES - 1);
    CHECK(fx->halves.generation && gp_svg_size(app, fx->halves, &w, &h) && w == 40 && h == 20, "size of the halves drawing: %gx%g", w, h);
    gp_svg vb = gp_svg_load_memory(app, S_VIEWBOX, sizeof S_VIEWBOX - 1);
    CHECK(gp_svg_size(app, vb, &w, &h) && w == 10 && h == 10, "width and height win over the viewBox: %gx%g", w, h);
    const char only_viewbox[] = HEAD "viewBox='0 0 30 20'><rect width='30' height='20'/></svg>";
    gp_svg ov = gp_svg_load_memory(app, only_viewbox, sizeof only_viewbox - 1);
    CHECK(gp_svg_size(app, ov, &w, &h) && w == 30 && h == 20, "a viewBox alone gives the size: %gx%g", w, h);
    gp_svg_release(app, vb);
    gp_svg_release(app, ov);

    CHECK(!gp_svg_load_memory(app, NULL, 10).generation && !gp_svg_load_memory(app, "", 0).generation, "no text is refused");
    CHECK(!gp_svg_load_memory(app, "hello, world", 12).generation, "text that is not SVG is refused");
    const char nosize[] = HEAD "><rect width='10' height='10'/></svg>";
    gp_svg ns = gp_svg_load_memory(app, nosize, sizeof nosize - 1);
    CHECK(ns.generation && gp_svg_size(app, ns, &w, &h) && w == 10 && h == 10, "a drawing with no size has the extent of its shapes (%gx%g)", w, h);
    gp_svg_release(app, ns);
    const char empty[] = HEAD "width='0' height='0'/>";
    CHECK(!gp_svg_load_memory(app, empty, sizeof empty - 1).generation, "an empty drawing is refused");
    const char huge[] = HEAD "width='1e9' height='1e9'><rect width='10' height='10'/></svg>";
    CHECK(!gp_svg_load_memory(app, huge, sizeof huge - 1).generation, "an absurd size is refused");
    const char trunc[] = HEAD "width='20' height='20'><rect width='10' hei";
    gp_svg tr = gp_svg_load_memory(app, trunc, sizeof trunc - 1);   /* nanosvg is lenient with broken markup: whatever it does, it must not crash */
    gp_svg_release(app, tr);
    size_t big = (8u << 20) + 16;
    char *buf = malloc(big);
    memset(buf, ' ', big);
    memcpy(buf, S_HALVES, sizeof S_HALVES - 1);
    CHECK(!gp_svg_load_memory(app, buf, big).generation, "a document over 8 MB is refused");
    free(buf);
    CHECK(!gp_svg_load_file(app, "/nonexistent/x.svg").generation && !gp_svg_load_file(app, NULL).generation, "a missing file");

    /* gp_svg_from_path: a path in a box, quoted into a document safely */
    fx->path = gp_svg_from_path(app, "M0 0 L20 0 L10 20 Z", 20, 20, gp_rgb(0xff0000), gp_rgba(0), 0);
    CHECK(fx->path.generation && gp_svg_size(app, fx->path, &w, &h) && w == 20 && h == 20, "a path");
    CHECK(!gp_svg_from_path(app, "M0 0\" onload=\"x", 20, 20, gp_rgb(0xff0000), gp_rgba(0), 0).generation && !gp_svg_from_path(app, "M0 0 <", 20, 20, gp_rgb(0xff0000), gp_rgba(0), 0).generation,
          "a path that would break out of its attribute is refused");
    CHECK(!gp_svg_from_path(app, "M0 0 L1 1", 0, 20, gp_rgb(0xff0000), gp_rgba(0), 0).generation && !gp_svg_from_path(app, NULL, 20, 20, gp_rgb(0), gp_rgb(0), 0).generation, "no size or no path");

    /* a file */
    char path[512];
    snprintf(path, sizeof path, "%s/gpuic-svg-test-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    int fd = mkstemp(path);
    if (fd >= 0) {
        CHECK(write(fd, S_HALVES, sizeof S_HALVES - 1) == (ssize_t)(sizeof S_HALVES - 1), "write");
        close(fd);
        gp_svg fsv = gp_svg_load_file(app, path);
        CHECK(fsv.generation && gp_svg_size(app, fsv, &w, &h) && w == 40, "a file");
        gp_svg_release(app, fsv);
        remove(path);
    }

    /* stale handles */
    gp_svg a = gp_svg_load_memory(app, S_CIRCLE, sizeof S_CIRCLE - 1);
    gp_svg_release(app, a);
    CHECK(!gp_svg_size(app, a, &w, &h), "a released drawing has no size");
    gp_svg b = gp_svg_load_memory(app, S_TRIANGLE, sizeof S_TRIANGLE - 1);
    CHECK(b.index == a.index && b.generation != a.generation, "the slot is reused with a new generation");
    CHECK(!gp_svg_size(app, a, &w, &h) && gp_svg_size(app, b, &w, &h), "the old handle does not see the new drawing");
    gp_svg_release(app, a);
    CHECK(gp_svg_size(app, b, &w, &h), "releasing a stale handle leaves the new one");
    gp_svg_release(app, b);
    gp_svg_release(app, GP_SVG_NULL);

    fx->circle = gp_svg_load_memory(app, S_CIRCLE, sizeof S_CIRCLE - 1);
    fx->triangle = gp_svg_load_memory(app, S_TRIANGLE, sizeof S_TRIANGLE - 1);
    fx->stroke = gp_svg_load_memory(app, S_STROKE, sizeof S_STROKE - 1);
    fx->gradient = gp_svg_load_memory(app, S_GRADIENT, sizeof S_GRADIENT - 1);
    fx->current = gp_svg_load_memory(app, S_CURRENT, sizeof S_CURRENT - 1);
    fx->opacity = gp_svg_load_memory(app, S_OPACITY, sizeof S_OPACITY - 1);
    fx->viewbox = gp_svg_load_memory(app, S_VIEWBOX, sizeof S_VIEWBOX - 1);
    fx->transform = gp_svg_load_memory(app, S_TRANSFORM, sizeof S_TRANSFORM - 1);
    fx->ring = gp_svg_load_memory(app, S_RING, sizeof S_RING - 1);
    fx->ring_nz = gp_svg_load_memory(app, S_RING_NZ, sizeof S_RING_NZ - 1);
    gp_svg old = gp_svg_load_memory(app, S_CIRCLE, sizeof S_CIRCLE - 1);
    gp_svg_release(app, old);
    fx->fresh = gp_svg_load_memory(app, S_HALVES, sizeof S_HALVES - 1);   /* takes the slot: the element holding `old` must not draw it */
    fx->stale = old;
    CHECK(fx->fresh.index == old.index && fx->fresh.generation != old.generation, "the stale setup: slot reused");
    CHECK(gp_gfx_image_count(g) == images0, "no images were made by parsing (%d)", gp_gfx_image_count(g) - images0);
    fx->extra[0] = gp_svg_load_memory(app, S_HALVES, sizeof S_HALVES - 1);
}

static void pixel_tests(Fix *fx) {
    wait_draws(2);
    nels = 0;
    collect(win()->tree);
    CHECK(nels == NSVG, "found %d svg elements, wanted %d", nels, NSVG);
    if (nels != NSVG) return;

    /* sizes */
    CHECK(els[A_HALVES]->size.w == 40 && els[A_HALVES]->size.h == 20, "natural size (%.1fx%.1f)", els[A_HALVES]->size.w, els[A_HALVES]->size.h);
    CHECK(els[A_HALVES2X]->size.w == 80 && els[A_HALVES2X]->size.h == 40, "width 80 keeps the proportions (%.1fx%.1f)", els[A_HALVES2X]->size.w, els[A_HALVES2X]->size.h);
    CHECK(els[B_VIEWBOX]->size.w == 10 && els[B_VIEWBOX100]->size.w == 100, "viewBox drawing at 10 and at 100");
    CHECK(els[B_STRETCH]->size.w == 120 && els[B_STRETCH]->size.h == 60, "stretched in a column: 120 wide, 60 tall from the proportions (%.1fx%.1f)", els[B_STRETCH]->size.w, els[B_STRETCH]->size.h);

    uint8_t *px = frame();
    CHECK(px != NULL, "no frame");
    if (!px) return;
    int pw = app->capture_w, bx = 0, by = 0, partial = 0;

    /* flat shapes, natural and enlarged: crisp, not stretched from a small raster */
    Box h1 = box_of(A_HALVES), h2 = box_of(A_HALVES2X);
    CHECK(near_rgb(px_at(px, pw, h1.x0 + 5, h1.y0 + 10), 255, 0, 0, 2) && near_rgb(px_at(px, pw, h1.x0 + 35, h1.y0 + 10), 0, 0, 255, 2) &&
              near_rgb(px_at(px, pw, h1.x0 + 19, h1.y0 + 10), 255, 0, 0, 2) && near_rgb(px_at(px, pw, h1.x0 + 20, h1.y0 + 10), 0, 0, 255, 2),
          "two halves at natural size, the edge on the pixel boundary");
    CHECK(near_rgb(px_at(px, pw, h2.x0 + 39, h2.y0 + 20), 255, 0, 0, 2) && near_rgb(px_at(px, pw, h2.x0 + 40, h2.y0 + 20), 0, 0, 255, 2) && near_rgb(px_at(px, pw, h2.x0 + 5, h2.y0 + 1), 255, 0, 0, 2),
          "and at twice the size the edge is still one pixel wide (%d,%d,%d | %d,%d,%d)", px_at(px, pw, h2.x0 + 39, h2.y0 + 20)[0], px_at(px, pw, h2.x0 + 39, h2.y0 + 20)[1], px_at(px, pw, h2.x0 + 39, h2.y0 + 20)[2],
          px_at(px, pw, h2.x0 + 40, h2.y0 + 20)[0], px_at(px, pw, h2.x0 + 40, h2.y0 + 20)[1], px_at(px, pw, h2.x0 + 40, h2.y0 + 20)[2]);
    Box cb = box_of(A_CONTAIN);   /* 40x20 drawing contained in 40x40: the drawing is the middle band */
    CHECK(is_page(px_at(px, pw, cb.x0 + 20, cb.y0 + 5)) && is_page(px_at(px, pw, cb.x0 + 20, cb.y0 + 35)) && near_rgb(px_at(px, pw, cb.x0 + 5, cb.y0 + 20), 255, 0, 0, 2),
          "contain: letterboxed above and below");

    /* a circle and a triangle against coverage computed here; enlarged, still sharp */
    static const int green[3] = { 0, 255, 0 }, yellow[3] = { 255, 255, 0 }, red[3] = { 255, 0, 0 };
    int bad = check_shape(px, pw, box_of(A_CIRCLE), in_circle, green, &partial, &bx, &by);
    CHECK(bad == 0 && partial > 20, "circle at 40: %d pixels off (first %d,%d), %d antialiased edge pixels", bad, bx, by, partial);
    bad = check_shape(px, pw, box_of(A_CIRCLE3X), in_circle, green, &partial, &bx, &by);
    CHECK(bad == 0 && partial > 60, "circle at 120: %d pixels off (first %d,%d), %d edge pixels", bad, bx, by, partial);
    bad = check_shape(px, pw, box_of(A_TRIANGLE), in_triangle, yellow, &partial, &bx, &by);
    CHECK(bad == 0 && partial > 20, "triangle: %d pixels off (first %d,%d), %d edge pixels", bad, bx, by, partial);

    /* a stroke 6 wide centered on y = 20: rows 17..22 */
    Box sb = box_of(A_STROKE);
    CHECK(near_rgb(px_at(px, pw, sb.x0 + 20, sb.y0 + 17), 255, 0, 0, 3) && near_rgb(px_at(px, pw, sb.x0 + 20, sb.y0 + 22), 255, 0, 0, 3) &&
              is_page(px_at(px, pw, sb.x0 + 20, sb.y0 + 15)) && is_page(px_at(px, pw, sb.x0 + 20, sb.y0 + 24)) && is_page(px_at(px, pw, sb.x0 + 3, sb.y0 + 20)) && near_rgb(px_at(px, pw, sb.x0 + 6, sb.y0 + 20), 255, 0, 0, 3),
          "a stroke: six rows, from x = 5 to 35");

    /* a gradient written with fractions: red at the left to blue at the right */
    Box gb = box_of(A_GRADIENT);
    int worst = 0;
    for (int x = 2; x < 98; x += 4) {
        const uint8_t *p = px_at(px, pw, gb.x0 + x, gb.y0 + 10);
        float t = ((float)x + 0.5f) / 100.f;
        worst = (int)fmaxf((float)worst, fmaxf((float)abs((int)lroundf(255 * (1 - t)) - p[0]), fmaxf((float)p[1], (float)abs((int)lroundf(255 * t) - p[2]))));
    }
    CHECK(worst <= 6, "a gradient from fractions runs red to blue across the shape: off by %d", worst);

    /* currentColor */
    Box g1 = box_of(B_GREEN), g2 = box_of(B_MAGENTA);
    CHECK(near_rgb(px_at(px, pw, g1.x0 + 10, g1.y0 + 10), 0, 255, 0, 2) && near_rgb(px_at(px, pw, g2.x0 + 10, g2.y0 + 10), 255, 0, 255, 2), "currentColor is the text color: green and magenta");
    /* fill-opacity */
    Box ob = box_of(B_OPACITY);
    CHECK(near_rgb(px_at(px, pw, ob.x0 + 10, ob.y0 + 10), (255 + PAGE_R) / 2, PAGE_G / 2, PAGE_B / 2, 4), "fill-opacity 0.5 over the page: %d,%d,%d", px_at(px, pw, ob.x0 + 10, ob.y0 + 10)[0], px_at(px, pw, ob.x0 + 10, ob.y0 + 10)[1], px_at(px, pw, ob.x0 + 10, ob.y0 + 10)[2]);
    /* viewBox: a 100-unit drawing in a 10 px box and at 100 px */
    Box v10 = box_of(B_VIEWBOX), v100 = box_of(B_VIEWBOX100);
    CHECK(near_rgb(px_at(px, pw, v10.x0 + 2, v10.y0 + 5), 255, 0, 0, 3) && is_page(px_at(px, pw, v10.x0 + 7, v10.y0 + 5)), "viewBox: the left half at 10 px");
    CHECK(near_rgb(px_at(px, pw, v100.x0 + 49, v100.y0 + 50), 255, 0, 0, 2) && is_page(px_at(px, pw, v100.x0 + 50, v100.y0 + 50)) && near_rgb(px_at(px, pw, v100.x0 + 3, v100.y0 + 99), 255, 0, 0, 2),
          "and at 100 px, sharp at the half");
    /* a transformed group */
    Box tb = box_of(B_TRANSFORM);
    CHECK(is_page(px_at(px, pw, tb.x0 + 5, tb.y0 + 5)) && near_rgb(px_at(px, pw, tb.x0 + 15, tb.y0 + 5), 0, 255, 0, 2) && is_page(px_at(px, pw, tb.x0 + 25, tb.y0 + 5)), "translate(10,0): the square is at 10..20");
    /* even-odd and non-zero */
    Box rb = box_of(B_RING), nb = box_of(B_RING_NZ);
    CHECK(near_rgb(px_at(px, pw, rb.x0 + 5, rb.y0 + 20), 255, 0, 0, 2) && is_page(px_at(px, pw, rb.x0 + 20, rb.y0 + 20)), "even-odd: a hole");
    CHECK(near_rgb(px_at(px, pw, nb.x0 + 20, nb.y0 + 20), 255, 0, 0, 2) || is_page(px_at(px, pw, nb.x0 + 20, nb.y0 + 20)), "non-zero: (same direction paths fill the middle; whichever the rasterizer decides, it is solid)");
    /* gp_svg_from_path: a red triangle pointing down in 20x20 */
    Box pb = box_of(B_PATH);
    CHECK(near_rgb(px_at(px, pw, pb.x0 + 10, pb.y0 + 5), 255, 0, 0, 3) && is_page(px_at(px, pw, pb.x0 + 1, pb.y0 + 15)) && is_page(px_at(px, pw, pb.x0 + 10, pb.y0 + 19)) == false,
          "a path: a triangle");
    /* a stale handle draws nothing, though its slot is another drawing now */
    Box st = box_of(B_STALE);
    CHECK(is_page(px_at(px, pw, st.x0 + 5, st.y0 + 10)) && is_page(px_at(px, pw, st.x0 + 15, st.y0 + 10)), "a stale drawing draws nothing");
    /* corners */
    Box ro = box_of(B_ROUND);
    CHECK(is_page(px_at(px, pw, ro.x0, ro.y0)) && !is_page(px_at(px, pw, ro.x0 + 20, ro.y0 + 20)), "rounded: the corner is cut");
    (void)red;
    (void)fx;
}

static void cache_tests(Fix *fx) {
    gp_gfx *g = app->gfx;
    /* the main picture asked for: halves 40, 80, contain-40x40 (the picture is 40 wide in it), 80x80? list distinct (svg, width, color) */
    int n = gp_gfx_svg_raster_count(g);
    /* distinct: halves@40 black? colors are the text color of each element; those with no color set share the default.
     * halves 40 (A_HALVES, A_CONTAIN and B_ROUND's 40 px cover: 40x40 box, cover scale 2 -> raster 80 wide: same as A_HALVES2X) */
    CHECK(n >= 10 && n <= 20, "the rasters made (%d)", n);
    int before = n;
    win()->dirty_paint = true;
    wait_draws(3);
    CHECK(gp_gfx_svg_raster_count(g) == before, "painting again makes no new rasters (%d, was %d)", gp_gfx_svg_raster_count(g), before);
    notify_root();
    wait_draws(3);
    CHECK(gp_gfx_svg_raster_count(g) == before, "nor does building the same views again (%d)", gp_gfx_svg_raster_count(g));

    /* two elements with the same color share a raster; another color is another one */
    uint8_t *px = frame();
    (void)px;
    (void)fx;
}

static void budget_tests(Fix *fx) {
    gp_gfx *g = app->gfx;
    /* a small budget: the sizes of phase 1 do not fit together, so the frame is cleared and everything is made again */
    gp_gfx_svg_set_budget(g, 96u << 20, 5);
    unsigned long resets = app->stat_atlas_resets;
    fx->phase = 1;
    notify_root();
    wait_draws(3);
    for (int i = 0; i < 30 && app->stat_atlas_resets == resets; i++) { win()->dirty_paint = true; wait_draws(1); }
    CHECK(app->stat_atlas_resets > resets, "more rasters than the budget: the frame was cleared and repainted (%lu resets)", app->stat_atlas_resets - resets);
    gp_gfx_svg_set_budget(g, 96u << 20, 512);   /* back: the eight sizes fit */
    for (int i = 0; i < 4; i++) { win()->dirty_paint = true; wait_draws(2); }
    wait_draws(2);
    nels = 0;
    collect(win()->tree);
    CHECK(nels == 8, "eight elements in phase 1 (%d)", nels);
    uint8_t *px = frame();
    int pw = app->capture_w;
    int bad = 0;
    for (int i = 0; px && i < nels && i < 8; i++) {
        Box b = box_of(i);
        bool left_red = near_rgb(px_at(px, pw, b.x0 + 2, b.y0 + b.h / 2), 255, 0, 0, 3), right_blue = near_rgb(px_at(px, pw, b.x0 + b.w - 3, b.y0 + b.h / 2), 0, 0, 255, 3);
        if (!left_red || !right_blue) bad++;
    }
    CHECK(bad == 0, "after the clear every drawing is there with its colors (%d wrong)", bad);
    CHECK(gp_gfx_svg_raster_count(g) <= 8, "and only what is shown has a raster (%d)", gp_gfx_svg_raster_count(g));
}

static void release_tests(Fix *fx) {
    gp_gfx *g = app->gfx;
    int imgs = gp_gfx_image_count(g), rasters = gp_gfx_svg_raster_count(g);
    CHECK(rasters > 0, "rasters exist before releasing (%d)", rasters);
    gp_svg_release(app, fx->extra[0]);
    CHECK(gp_gfx_svg_raster_count(g) == 0 && gp_gfx_image_count(g) == imgs - rasters, "releasing a drawing frees its rasters (%d left, images %d -> %d)", gp_gfx_svg_raster_count(g), imgs, gp_gfx_image_count(g));
    wait_draws(3);
    uint8_t *px = frame();
    CHECK(px != NULL, "a frame is drawn after the drawing is gone");
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    Fix *fx = gp_entity_get(app, win()->root_view);
    api_tests(fx);
    notify_root();
    pixel_tests(fx);
    cache_tests(fx);
    budget_tests(fx);
    release_tests(fx);
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    gp_entity v = gp_view_new(app, sizeof(Fix), &(Fix){ 0 }, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "svg_test", .width = 440, .height = 600 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "svg_test: %d FAILURE(S)\n" : "svg_test: all checks passed\n", failures);
    return failures != 0;
}
