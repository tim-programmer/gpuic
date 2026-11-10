/*
 * Images: decoding (PNG, JPEG, GIF, BMP, raw RGBA; refusing garbage and absurd sizes), the registry (stale handles, slot
 * reuse), sizing by aspect ratio, every object-fit mode compared pixel by pixel with an independent computation, rounded
 * corners, padding and border, opacity, premultiplied alpha, smoothing vs pixelated, mipmapped minification, painter's
 * order against other elements, a released image, asynchronous loading and cancelling it, and what happens to textures.
 * Frames are read back from the GL renderer. Needs a display.
 */
#include "../src/internal.h"
#include "image_fixtures.h"

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

typedef struct {
    gp_image png, jpg, gif, bmp, checker, solid, half;   /* half: left half opaque red, right half transparent */
    gp_image stale;                                      /* a handle whose slot was reused by `fresh` */
    gp_image fresh;
} Fix;

static gp_node *render(void *self, gp_ctx *cx) {
    Fix *f = self;
    gp_node *colA = gp_div(
        gp_flex_col(), gp_gap(4), gp_items_start(),
        gp_img(f->png),
        gp_img(f->png, gp_w(80), gp_h(60)),
        gp_img(f->png, gp_w(80), gp_h(60), gp_image_pixelated()),
        gp_img(f->png, gp_w(160)),
        gp_img(f->png, gp_h(30)),
        gp_img(f->png, gp_w(60), gp_p(5)),
        gp_div(gp_flex_col(), gp_w(120), gp_img(f->png)));
    gp_node *colB = gp_div(
        gp_flex_col(), gp_gap(4), gp_items_start(),
        gp_img(f->png, gp_w(100), gp_h(40), gp_object_fit(GP_FIT_FILL)),
        gp_img(f->png, gp_w(100), gp_h(40), gp_object_fit(GP_FIT_CONTAIN)),
        gp_img(f->png, gp_w(100), gp_h(40), gp_object_fit(GP_FIT_COVER)),
        gp_img(f->png, gp_w(100), gp_h(40), gp_object_fit(GP_FIT_NONE)),
        gp_img(f->jpg, gp_w(16), gp_h(16), gp_object_fit(GP_FIT_SCALE_DOWN)),
        gp_img(f->jpg, gp_w(100), gp_h(100), gp_object_fit(GP_FIT_SCALE_DOWN)));
    gp_node *colC = gp_div(
        gp_flex_col(), gp_gap(4), gp_items_start(),
        gp_img(f->solid, gp_size_px(40), gp_object_fit(GP_FIT_COVER), gp_rounded(20)),
        gp_img(f->png, gp_w(40), gp_h(30), gp_p(5), gp_border(2), gp_border_color(gp_rgb(0xffffff))),
        gp_img(f->png, gp_w(80), gp_h(60), gp_opacity(0.5f)),
        gp_img(f->jpg),
        gp_img(f->jpg, gp_size_px(8)),
        gp_img(f->checker, gp_size_px(64)),
        gp_img(f->gif, gp_size_px(40), gp_image_pixelated()),
        gp_img(f->bmp, gp_w(40), gp_h(20), gp_image_pixelated()),
        gp_img(f->stale, gp_size_px(30)),
        gp_div(gp_size_px(40), gp_bg(gp_rgb(0xffff00)),
               gp_img(f->half, gp_absolute(), gp_left(0), gp_top(0), gp_size_px(40)),
               gp_div(gp_absolute(), gp_left(0), gp_top(0), gp_size_px(40), gp_bg(gp_rgba(0x0000ff80)))));
    gp_node *colD = gp_div(
        gp_flex_col(), gp_gap(4), gp_items_start(),
        gp_img(f->png, gp_w(80), gp_h(60), gp_image_grayscale(1)),
        gp_img(f->png, gp_w(80), gp_h(60), gp_image_grayscale(0.5f)),
        gp_img(f->png, gp_w(80), gp_h(60), gp_image_brightness(0.5f)),
        gp_img(f->png, gp_w(80), gp_h(60), gp_image_contrast(0)),
        gp_img(f->png, gp_w(80), gp_h(60), gp_image_saturate(0)),
        gp_img(f->png, gp_w(80), gp_h(60), gp_image_tint(gp_rgb(0x0000ff))),
        gp_img(f->png, gp_w(80), gp_h(60), gp_image_tint(gp_rgb(0x808080))),
        gp_img(f->png, gp_w(80), gp_h(60), gp_image_blur(12)),
        gp_img(f->jpg, gp_image_brightness(0.5f)));
    return gp_div(gp_size_full(), gp_flex_row(), gp_gap(12), gp_p(6), gp_bg(PAGE), colA, colB, colC, colD);
}

enum { A_NATURAL, A_SMOOTH, A_PIXEL, A_W160, A_H30, A_PAD, A_STRETCH,
       B_FILL, B_CONTAIN, B_COVER, B_NONE, B_SD_SMALL, B_SD_BIG,
       C_ROUND, C_BORDER, C_OPACITY, C_JPG, C_JPG8, C_CHECKER, C_GIF, C_BMP, C_STALE, C_OVERLAY,
       D_GRAY, D_GRAY_HALF, D_BRIGHT, D_CONTRAST0, D_SAT0, D_TINT_BLUE, D_TINT_GRAY, D_BLUR, D_JPG_BRIGHT, NIMG };

static const gp_el *imgs[NIMG + 4];
static int nimgs;
static void collect(const gp_el *n) {
    if (n->kind == GP_EL_IMAGE && nimgs < NIMG + 4) imgs[nimgs++] = n;
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
    abs_pos(win()->tree, imgs[i], -win()->tree->pos.x, -win()->tree->pos.y, &ax, &ay);
    float s = win()->scale;
    return (Box){ (int)lroundf(ax * s), (int)lroundf(ay * s), (int)lroundf(imgs[i]->size.w * s), (int)lroundf(imgs[i]->size.h * s) };
}

static const uint8_t *px_at(const uint8_t *px, int pw, int x, int y) { return px + ((size_t)y * (size_t)pw + (size_t)x) * 3; }
static bool near_rgb(const uint8_t *p, int r, int g, int b, int tol) { return abs(p[0] - r) <= tol && abs(p[1] - g) <= tol && abs(p[2] - b) <= tol; }
static bool is_page(const uint8_t *p) { return near_rgb(p, PAGE_R, PAGE_G, PAGE_B, 2); }

/* what the 8x6 test picture shows at source coordinates (u, v); the white quadrant is half transparent over the page */
static void png_color(float u, float v, int out[3]) {
    bool left = u < 4, top = v < 3;
    if (left && top) { out[0] = 255; out[1] = 0; out[2] = 0; }
    else if (!left && top) { out[0] = 0; out[1] = 255; out[2] = 0; }
    else if (left) { out[0] = 0; out[1] = 0; out[2] = 255; }
    else { out[0] = (255 + PAGE_R) / 2; out[1] = (255 + PAGE_G) / 2; out[2] = (255 + PAGE_B) / 2 + 0; }
}

/* object-fit written out again: where the picture lands in a content box, and so what each pixel should show */
static int check_fit(const uint8_t *px, int pw, Box b, int fit, int *worst_x, int *worst_y) {
    float cw = (float)b.w, ch = (float)b.h, iw = 8, ih = 6, sw, sh;
    if (fit == GP_FIT_CONTAIN) { float k = fminf(cw / iw, ch / ih); sw = iw * k; sh = ih * k; }
    else if (fit == GP_FIT_COVER) { float k = fmaxf(cw / iw, ch / ih); sw = iw * k; sh = ih * k; }
    else if (fit == GP_FIT_NONE) { sw = iw; sh = ih; }
    else { sw = cw; sh = ch; }
    float rx = (cw - sw) / 2, ry = (ch - sh) / 2;
    int bad = 0;
    for (int y = 0; y < b.h; y++)
        for (int x = 0; x < b.w; x++) {
            float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
            const uint8_t *p = px_at(px, pw, b.x0 + x, b.y0 + y);
            bool inside = fx >= rx + 0.5f && fx <= rx + sw - 0.5f && fy >= ry + 0.5f && fy <= ry + sh - 0.5f;
            bool outside = fx < rx - 0.5f || fx > rx + sw + 0.5f || fy < ry - 0.5f || fy > ry + sh + 0.5f;
            if (outside) {
                if (!is_page(p)) { bad++; *worst_x = x; *worst_y = y; }
                continue;
            }
            if (!inside) continue;   /* the edge pixels: partly covered */
            float u = (fx - rx) / sw * iw, v = (fy - ry) / sh * ih;
            if (fabsf(u - 4) < 0.8f || fabsf(v - 3) < 0.8f) continue;   /* bilinear blending across the quadrant edges */
            int want[3];
            png_color(u, v, want);
            if (!near_rgb(p, want[0], want[1], want[2], 4)) { bad++; *worst_x = x; *worst_y = y; }
        }
    return bad;
}

typedef struct { bool called; gp_image image; int calls; gp_ctx cx; } Done;
static void on_done(void *self, gp_ctx *cx, gp_image im, void *data) {
    Done *d = data;
    d->called = true;
    d->calls++;
    d->image = im;
    d->cx = *cx;
}

static void api_tests(Fix *fx) {
    int w = 0, h = 0;
    gp_gfx *g = app->gfx;
    int base = gp_gfx_image_count(g);

    gp_image png = gp_image_load_memory(app, fx_png, sizeof fx_png);
    CHECK(png.generation && gp_image_size(app, png, &w, &h) && w == 8 && h == 6, "PNG: %dx%d", w, h);
    gp_image jpg = gp_image_load_memory(app, fx_jpg, sizeof fx_jpg);
    CHECK(jpg.generation && gp_image_size(app, jpg, &w, &h) && w == 32 && h == 32, "JPEG: %dx%d", w, h);
    gp_image gif = gp_image_load_memory(app, fx_gif, sizeof fx_gif);
    CHECK(gif.generation && gp_image_size(app, gif, &w, &h) && w == 4 && h == 4, "GIF: %dx%d", w, h);
    gp_image bmp = gp_image_load_memory(app, fx_bmp, sizeof fx_bmp);
    CHECK(bmp.generation && gp_image_size(app, bmp, &w, &h) && w == 4 && h == 2, "BMP: %dx%d", w, h);
    CHECK(gp_gfx_image_count(g) == base + 4, "four images registered (%d)", gp_gfx_image_count(g) - base);

    unsigned char junk[64];
    for (int i = 0; i < 64; i++) junk[i] = (unsigned char)(i * 37 + 11);
    CHECK(!gp_image_load_memory(app, junk, sizeof junk).generation, "garbage is refused");
    CHECK(!gp_image_load_memory(app, NULL, 10).generation && !gp_image_load_memory(app, fx_png, 0).generation, "no data is refused");
    CHECK(!gp_image_load_memory(app, fx_png, 40).generation, "a truncated PNG is refused");
    CHECK(!gp_image_load_memory(app, fx_huge, sizeof fx_huge).generation, "a PNG that claims 16000 x 16000 pixels is refused before anything is allocated");
    CHECK(!gp_image_load_file(app, "/nonexistent/picture.png").generation && !gp_image_load_file(app, NULL).generation, "a missing file");
    CHECK(gp_gfx_image_count(g) == base + 4, "and none of that registered anything");
    unsigned char px[2 * 2 * 4] = { 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 0 };
    CHECK(!gp_image_from_rgba(app, 0, 2, px).generation && !gp_image_from_rgba(app, 2, -1, px).generation && !gp_image_from_rgba(app, 2, 2, NULL).generation &&
              !gp_image_from_rgba(app, 20000, 2, px).generation,
          "from_rgba refuses empty, negative, absurd and missing");
    CHECK(!gp_image_size(app, GP_IMAGE_NULL, &w, &h), "the null image has no size");

    /* stale handles: a slot that is reused is a new image */
    gp_image a = gp_image_from_rgba(app, 2, 2, px);
    gp_image_release(app, a);
    CHECK(!gp_image_size(app, a, &w, &h), "a released image has no size");
    gp_image b = gp_image_from_rgba(app, 3, 1, px);
    CHECK(b.index == a.index && b.generation != a.generation, "the slot is reused with a new generation (%u/%u vs %u/%u)", a.index, a.generation, b.index, b.generation);
    CHECK(!gp_image_size(app, a, &w, &h) && gp_image_size(app, b, &w, &h) && w == 3 && h == 1, "the old handle does not see the new image");
    gp_image_release(app, a);   /* again: harmless */
    CHECK(gp_image_size(app, b, &w, &h), "releasing a stale handle leaves the new image alone");
    gp_image_release(app, b);
    gp_image_release(app, GP_IMAGE_NULL);
    CHECK(gp_gfx_image_count(g) == base + 4, "everything made was released (%d)", gp_gfx_image_count(g) - base);

    fx->png = png;
    fx->jpg = jpg;
    fx->gif = gif;
    fx->bmp = bmp;
    /* pictures made in code */
    unsigned char *chk = malloc(512 * 512 * 4);
    for (int y = 0; y < 512; y++)
        for (int x = 0; x < 512; x++) {
            unsigned char v = (x + y) & 1 ? 255 : 0;
            unsigned char *p = chk + ((size_t)y * 512 + (size_t)x) * 4;
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    fx->checker = gp_image_from_rgba(app, 512, 512, chk);
    free(chk);
    unsigned char green[4 * 4 * 4];
    for (int i = 0; i < 16; i++) { green[i * 4] = 0; green[i * 4 + 1] = 200; green[i * 4 + 2] = 0; green[i * 4 + 3] = 255; }
    fx->solid = gp_image_from_rgba(app, 4, 4, green);
    unsigned char half[8 * 8 * 4];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            unsigned char *p = half + ((size_t)y * 8 + (size_t)x) * 4;
            p[0] = x < 4 ? 255 : 0; p[1] = 0; p[2] = 0; p[3] = x < 4 ? 255 : 0;
        }
    fx->half = gp_image_from_rgba(app, 8, 8, half);
    /* a stale handle that an element will hold while its slot belongs to another image */
    gp_image old = gp_image_from_rgba(app, 4, 4, green);
    gp_image_release(app, old);
    unsigned char blue[4 * 4 * 4];
    for (int i = 0; i < 16; i++) { blue[i * 4] = 0; blue[i * 4 + 1] = 0; blue[i * 4 + 2] = 255; blue[i * 4 + 3] = 255; }
    fx->fresh = gp_image_from_rgba(app, 4, 4, blue);   /* not the same pixels: if the element drew this one it would show */
    CHECK(fx->fresh.index == old.index && fx->fresh.generation != old.generation, "the stale test's setup: slot reused");
    fx->stale = old;
}

static void pixel_tests(Fix *fx) {
    wait_draws(2);
    nimgs = 0;
    collect(win()->tree);
    CHECK(nimgs == NIMG, "found %d image elements, wanted %d", nimgs, NIMG);
    if (nimgs != NIMG) return;

    /* sizes */
    CHECK(imgs[A_NATURAL]->size.w == 8 && imgs[A_NATURAL]->size.h == 6, "natural size 8x6 (%.1fx%.1f)", imgs[A_NATURAL]->size.w, imgs[A_NATURAL]->size.h);
    CHECK(imgs[A_W160]->size.w == 160 && imgs[A_W160]->size.h == 120, "width 160 keeps the proportions: height %.1f", imgs[A_W160]->size.h);
    CHECK(imgs[A_H30]->size.w == 40 && imgs[A_H30]->size.h == 30, "height 30: width %.1f", imgs[A_H30]->size.w);
    CHECK(imgs[A_PAD]->size.w == 60 && fabsf(imgs[A_PAD]->size.h - 47.5f) < 0.01f, "width 60 with padding 5: the content is 50 wide, 37.5 tall, plus the padding: %.2f", imgs[A_PAD]->size.h);
    CHECK(imgs[A_STRETCH]->size.w == 120 && imgs[A_STRETCH]->size.h == 90, "stretched in a column: 120 wide, height from the proportions (%.1fx%.1f)", imgs[A_STRETCH]->size.w, imgs[A_STRETCH]->size.h);
    CHECK(imgs[C_JPG]->size.w == 32 && imgs[C_JPG]->size.h == 32, "a JPEG's own size");
    CHECK(imgs[C_STALE]->size.w == 30 && imgs[C_STALE]->size.h == 30, "a stale image keeps the size the style gives it");

    uint8_t *px = frame();
    CHECK(px != NULL, "no frame");
    if (!px) return;
    int pw = app->capture_w, wx = 0, wy = 0;

    /* natural size: the four quadrants, one pixel each at least */
    Box b = box_of(A_NATURAL);
    CHECK(near_rgb(px_at(px, pw, b.x0 + 1, b.y0 + 1), 255, 0, 0, 3) && near_rgb(px_at(px, pw, b.x0 + 6, b.y0 + 1), 0, 255, 0, 3) &&
              near_rgb(px_at(px, pw, b.x0 + 1, b.y0 + 4), 0, 0, 255, 3),
          "8x6 at natural size: the quadrants' colors");
    /* premultiplied alpha: white at 50% over the page is (137,137,142), not a darkened or a brighter white */
    CHECK(near_rgb(px_at(px, pw, b.x0 + 6, b.y0 + 4), 137, 137, 142, 3), "half transparent white over the page: %d,%d,%d", px_at(px, pw, b.x0 + 6, b.y0 + 4)[0], px_at(px, pw, b.x0 + 6, b.y0 + 4)[1], px_at(px, pw, b.x0 + 6, b.y0 + 4)[2]);

    /* magnified: smooth blends across an edge, pixelated does not */
    Box s = box_of(A_SMOOTH), q = box_of(A_PIXEL);
    const uint8_t *sl = px_at(px, pw, s.x0 + 39, s.y0 + 10), *sr = px_at(px, pw, s.x0 + 40, s.y0 + 10);
    const uint8_t *ql = px_at(px, pw, q.x0 + 39, q.y0 + 10), *qr = px_at(px, pw, q.x0 + 40, q.y0 + 10);
    CHECK(sl[1] > 20 && sr[0] > 20 && sl[0] < 240, "smoothed: a blend across the red/green edge (%d,%d,%d | %d,%d,%d)", sl[0], sl[1], sl[2], sr[0], sr[1], sr[2]);
    CHECK(near_rgb(ql, 255, 0, 0, 2) && near_rgb(qr, 0, 255, 0, 2), "pixelated: no blend (%d,%d,%d | %d,%d,%d)", ql[0], ql[1], ql[2], qr[0], qr[1], qr[2]);

    /* object-fit, against the independent computation */
    static const int fits[4] = { B_FILL, B_CONTAIN, B_COVER, B_NONE };
    static const int modes[4] = { GP_FIT_FILL, GP_FIT_CONTAIN, GP_FIT_COVER, GP_FIT_NONE };
    static const char *names[4] = { "fill", "contain", "cover", "none" };
    for (int i = 0; i < 4; i++) {
        int bad = check_fit(px, pw, box_of(fits[i]), modes[i], &wx, &wy);
        CHECK(bad == 0, "object-fit %s: %d pixels differ from the expected picture (first at %d,%d)", names[i], bad, wx, wy);
    }
    /* scale-down: the smaller of none and contain */
    Box sdb = box_of(B_SD_SMALL);   /* 16x16 box, a 32x32 picture: shrunk to fit, so it fills the box */
    CHECK(!is_page(px_at(px, pw, sdb.x0 + 1, sdb.y0 + 1)) && !is_page(px_at(px, pw, sdb.x0 + 14, sdb.y0 + 14)), "scale-down of a picture that does not fit: it is shrunk to the box");
    Box sdbig = box_of(B_SD_BIG);   /* 100x100 box: the 32x32 picture stays 32x32 in the middle */
    CHECK(is_page(px_at(px, pw, sdbig.x0 + 5, sdbig.y0 + 5)) && !is_page(px_at(px, pw, sdbig.x0 + 50, sdbig.y0 + 50)) && is_page(px_at(px, pw, sdbig.x0 + 90, sdbig.y0 + 50)),
          "scale-down of a picture that fits: natural size, centered");
    const uint8_t *edge_l = px_at(px, pw, sdbig.x0 + 35, sdbig.y0 + 50), *edge_r = px_at(px, pw, sdbig.x0 + 64, sdbig.y0 + 50);
    CHECK(!is_page(edge_l) && !is_page(edge_r) && is_page(px_at(px, pw, sdbig.x0 + 32, sdbig.y0 + 50)) && is_page(px_at(px, pw, sdbig.x0 + 68, sdbig.y0 + 50)),
          "its edges: 32 pixels wide, at 34..66");

    /* rounded corners: the corner shows the page, the middle the picture */
    Box rb = box_of(C_ROUND);
    CHECK(is_page(px_at(px, pw, rb.x0, rb.y0)) && is_page(px_at(px, pw, rb.x0 + 39, rb.y0 + 39)) && near_rgb(px_at(px, pw, rb.x0 + 20, rb.y0 + 20), 0, 200, 0, 3),
          "rounded: corners are cut (%d,%d,%d), the middle is the picture", px_at(px, pw, rb.x0, rb.y0)[0], px_at(px, pw, rb.x0, rb.y0)[1], px_at(px, pw, rb.x0, rb.y0)[2]);
    /* padding and border */
    Box bb = box_of(C_BORDER);
    CHECK(near_rgb(px_at(px, pw, bb.x0 + 1, bb.y0 + 15), 255, 255, 255, 3) && is_page(px_at(px, pw, bb.x0 + 4, bb.y0 + 15)) &&
              near_rgb(px_at(px, pw, bb.x0 + 9, bb.y0 + 9), 255, 0, 0, 4) && near_rgb(px_at(px, pw, bb.x0 + 36, bb.y0 + 28), 255, 255, 255, 3),
          "border ring, padding, then the picture inside both");
    CHECK(is_page(px_at(px, pw, bb.x0 + 35, bb.y0 + 15)) && is_page(px_at(px, pw, bb.x0 + 20, bb.y0 + 25)) && is_page(px_at(px, pw, bb.x0 + 20, bb.y0 + 4)),
          "padding on the right, bottom and top as well: the picture is the 26x16 inside them");
    /* opacity */
    Box ob = box_of(C_OPACITY);
    const uint8_t *op = px_at(px, pw, ob.x0 + 10, ob.y0 + 10);
    CHECK(near_rgb(op, (255 + PAGE_R) / 2, PAGE_G / 2, PAGE_B / 2, 3), "opacity 0.5 over the page: %d,%d,%d", op[0], op[1], op[2]);

    /* JPEG: red at the left, blue at the right, purple in the middle */
    Box jb = box_of(C_JPG);
    const uint8_t *jl = px_at(px, pw, jb.x0 + 1, jb.y0 + 16), *jm = px_at(px, pw, jb.x0 + 16, jb.y0 + 16), *jr = px_at(px, pw, jb.x0 + 30, jb.y0 + 16);
    CHECK(jl[0] > 215 && jl[2] < 40 && jr[2] > 215 && jr[0] < 40 && abs(jm[0] - 128) < 25 && abs(jm[2] - 128) < 25, "JPEG: %d,%d,%d | %d,%d,%d | %d,%d,%d", jl[0], jl[1], jl[2], jm[0], jm[1], jm[2], jr[0], jr[1], jr[2]);
    Box j8 = box_of(C_JPG8);
    const uint8_t *js = px_at(px, pw, j8.x0 + 4, j8.y0 + 4);
    CHECK(abs(js[0] - 128) < 30 && abs(js[2] - 128) < 30, "the JPEG shown at 8x8 is its average, purple: %d,%d,%d", js[0], js[1], js[2]);
    /* GIF and BMP */
    Box gb = box_of(C_GIF), bmb = box_of(C_BMP);
    CHECK(near_rgb(px_at(px, pw, gb.x0 + 5, gb.y0 + 20), 255, 255, 0, 3) && near_rgb(px_at(px, pw, gb.x0 + 35, gb.y0 + 20), 255, 0, 255, 3), "GIF: yellow | magenta");
    CHECK(near_rgb(px_at(px, pw, bmb.x0 + 5, bmb.y0 + 10), 0, 255, 255, 3) && near_rgb(px_at(px, pw, bmb.x0 + 35, bmb.y0 + 10), 255, 128, 0, 3), "BMP: cyan | orange");

    /* mipmaps: a one pixel checkerboard shown at an eighth is gray, evenly, not black and white dots */
    Box cb = box_of(C_CHECKER);
    double sum = 0, sum2 = 0;
    int n = 0;
    for (int y = 2; y < cb.h - 2; y++)
        for (int x = 2; x < cb.w - 2; x++) {
            double v = px_at(px, pw, cb.x0 + x, cb.y0 + y)[1];
            sum += v; sum2 += v * v; n++;
        }
    double mean = sum / n, sd = sqrt(sum2 / n - mean * mean);
    CHECK(fabs(mean - 127.5) < 12 && sd < 20, "a minified checkerboard: mean %.1f, spread %.1f (about 127 and under 20 with mipmaps)", mean, sd);

    /* filters, against CSS's formulas: the quadrants are red, green and blue (top-left, top-right, bottom-left) */
    {
        struct { int el; int tl[3], tr[3], bl[3]; const char *what; } f[] = {
            { D_GRAY, { 54, 54, 54 }, { 182, 182, 182 }, { 18, 18, 18 }, "grayscale(1)" },
            { D_GRAY_HALF, { 155, 27, 27 }, { 91, 218, 91 }, { 9, 9, 137 }, "grayscale(0.5)" },
            { D_BRIGHT, { 128, 0, 0 }, { 0, 128, 0 }, { 0, 0, 128 }, "brightness(0.5)" },
            { D_CONTRAST0, { 128, 128, 128 }, { 128, 128, 128 }, { 128, 128, 128 }, "contrast(0)" },
            { D_SAT0, { 54, 54, 54 }, { 182, 182, 182 }, { 18, 18, 18 }, "saturate(0)" },
            { D_TINT_BLUE, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 255 }, "tint blue" },
            { D_TINT_GRAY, { 128, 0, 0 }, { 0, 128, 0 }, { 0, 0, 128 }, "tint gray" },
        };
        for (size_t i = 0; i < sizeof f / sizeof *f; i++) {
            Box fb = box_of(f[i].el);
            const uint8_t *a1 = px_at(px, pw, fb.x0 + 10, fb.y0 + 10), *a2 = px_at(px, pw, fb.x0 + 70, fb.y0 + 10), *a3 = px_at(px, pw, fb.x0 + 10, fb.y0 + 50);
            CHECK(near_rgb(a1, f[i].tl[0], f[i].tl[1], f[i].tl[2], 3) && near_rgb(a2, f[i].tr[0], f[i].tr[1], f[i].tr[2], 3) && near_rgb(a3, f[i].bl[0], f[i].bl[1], f[i].bl[2], 3),
                  "%s: %d,%d,%d | %d,%d,%d | %d,%d,%d", f[i].what, a1[0], a1[1], a1[2], a2[0], a2[1], a2[2], a3[0], a3[1], a3[2]);
        }
        {   /* brightness multiplies: a mid tone goes to half, where an offset would take it to black (primaries cannot tell the two apart) */
            Box jb = box_of(C_JPG), db = box_of(D_JPG_BRIGHT);
            const uint8_t *orig = px_at(px, pw, jb.x0 + 16, jb.y0 + 16), *dim = px_at(px, pw, db.x0 + 16, db.y0 + 16);
            CHECK(abs(dim[0] - orig[0] / 2) <= 3 && abs(dim[2] - orig[2] / 2) <= 3 && orig[0] > 100, "brightness(0.5) halves a mid tone: %d,%d,%d -> %d,%d,%d", orig[0], orig[1], orig[2], dim[0], dim[1], dim[2]);
        }
        Box bl = box_of(D_BLUR), plain = box_of(A_SMOOTH);
        /* the 10x magnified edge is already a bilinear ramp 10 px wide; a blur of 12 reaches further: 10 px left of the edge, red only if sharp */
        const uint8_t *edge = px_at(px, pw, bl.x0 + 30, bl.y0 + 10), *far = px_at(px, pw, bl.x0 + 3, bl.y0 + 10), *corner = px_at(px, pw, bl.x0, bl.y0), *sharp = px_at(px, pw, plain.x0 + 30, plain.y0 + 10);
        CHECK(edge[1] > 15 && edge[0] < 245 && far[0] > 235 && far[1] < 20 && near_rgb(corner, 255, 0, 0, 25) && sharp[1] < 3,
              "blur 12: soft 10 px before the red/green edge (%d,%d,%d), red far from it (%d,%d,%d), the corner not darkened (%d,%d,%d)", edge[0], edge[1], edge[2], far[0], far[1], far[2], corner[0], corner[1], corner[2]);
        CHECK(edge[1] > sharp[1] + 12, "and softer than the unblurred picture at the same place (%d vs %d green)", edge[1], sharp[1]);
        int step = 0;   /* a blurred picture is still smoothly magnified, not blocks of nearest-neighbor texels */
        for (int x = 0; x + 1 < 80; x++) {
            int d = abs((int)px_at(px, pw, bl.x0 + x, bl.y0 + 10)[1] - (int)px_at(px, pw, bl.x0 + x + 1, bl.y0 + 10)[1]);
            if (d > step) step = d;
        }
        CHECK(step <= 40, "a blurred picture is magnified smoothly: the largest step between neighbors is %d", step);
    }

    /* a stale handle draws nothing, though its slot holds another picture now */
    Box st = box_of(C_STALE);
    CHECK(is_page(px_at(px, pw, st.x0 + 15, st.y0 + 15)) && is_page(px_at(px, pw, st.x0 + 2, st.y0 + 2)), "a stale image draws nothing (%d,%d,%d)", px_at(px, pw, st.x0 + 15, st.y0 + 15)[0], px_at(px, pw, st.x0 + 15, st.y0 + 15)[1], px_at(px, pw, st.x0 + 15, st.y0 + 15)[2]);

    /* painter's order: yellow box, then an image, then a half transparent blue box over both */
    Box ov = box_of(C_OVERLAY);
    const uint8_t *over_img = px_at(px, pw, ov.x0 + 10, ov.y0 + 20), *over_bg = px_at(px, pw, ov.x0 + 30, ov.y0 + 20);
    CHECK(near_rgb(over_img, 128, 0, 128, 4), "over the opaque half of the image: red under blue: %d,%d,%d", over_img[0], over_img[1], over_img[2]);
    CHECK(near_rgb(over_bg, 128, 128, 128, 4), "over the transparent half: the yellow behind, under the blue: %d,%d,%d", over_bg[0], over_bg[1], over_bg[2]);
}

static void lifecycle_tests(Fix *fx) {
    gp_gfx *g = app->gfx;
    uint32_t dead[8];
    /* an image that was drawn has a texture; releasing it queues the texture, and the next frame deletes it */
    gp_gfx_image *im = gp_gfx_image_get(g, fx->checker.index, fx->checker.generation);
    CHECK(im && im->gl_tex != 0, "a drawn image has its texture");
    gp_image_release(app, fx->checker);
    fx->checker = GP_IMAGE_NULL;
    wait_draws(2);
    CHECK(gp_gfx_take_dead_textures(g, dead, 8) == 0, "after the next frame no dead texture is left for the renderer");
    uint8_t *px = frame();
    nimgs = 0;
    collect(win()->tree);
    CHECK(px != NULL && nimgs == NIMG, "the frame after releasing an image still draws (%d images)", nimgs);
    if (px && nimgs == NIMG) {
        Box b = box_of(C_CHECKER);
        CHECK(is_page(px_at(px, app->capture_w, b.x0 + 20, b.y0 + 20)), "and shows nothing where it was");
        Box o = box_of(A_NATURAL);
        CHECK(near_rgb(px_at(px, app->capture_w, o.x0 + 1, o.y0 + 1), 255, 0, 0, 3), "while the others are intact");
    }
}

static void async_tests(void) {
    char path[512];
    snprintf(path, sizeof path, "%s/gpuic-image-test-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    int fd = mkstemp(path);
    CHECK(fd >= 0, "temp file");
    if (fd < 0) return;
    CHECK(write(fd, fx_png, sizeof fx_png) == (ssize_t)sizeof fx_png, "write");
    close(fd);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    int base = gp_gfx_image_count(app->gfx), w = 0, h = 0;

    gp_image sync = gp_image_load_file(app, path);
    CHECK(sync.generation && gp_image_size(app, sync, &w, &h) && w == 8 && h == 6, "a file, synchronously");
    gp_image_release(app, sync);

    Done ok = { 0 };
    gp_task t = gp_image_load_file_async(&cx, path, on_done, &ok);
    CHECK(t.id != 0, "task started");
    for (int i = 0; i < 500 && !ok.called; i++) gp_sleep(co_, 0.01);
    CHECK(ok.called && ok.calls == 1 && ok.image.generation && gp_image_size(app, ok.image, &w, &h) && w == 8 && h == 6, "asynchronously: called back once with the image");
    CHECK(ok.cx.app == app, "with a ctx");
    gp_image_release(app, ok.image);

    Done bad = { 0 };
    gp_image_load_file_async(&cx, "/nonexistent/x.png", on_done, &bad);
    for (int i = 0; i < 500 && !bad.called; i++) gp_sleep(co_, 0.01);
    CHECK(bad.called && !bad.image.generation, "a missing file: called back with the null image");
    FILE *junk = fopen(path, "wb");
    fwrite("not a picture at all", 1, 20, junk);
    fclose(junk);
    Done garbage = { 0 };
    gp_image_load_file_async(&cx, path, on_done, &garbage);
    for (int i = 0; i < 500 && !garbage.called; i++) gp_sleep(co_, 0.01);
    CHECK(garbage.called && !garbage.image.generation, "garbage: called back with the null image");

    /* cancelling: no callback, and what the worker made is freed (ASan shows a leak) */
    FILE *again = fopen(path, "wb");
    fwrite(fx_png, 1, sizeof fx_png, again);
    fclose(again);
    Done cancelled = { 0 };
    gp_task c = gp_image_load_file_async(&cx, path, on_done, &cancelled);
    gp_cancel(app, c);
    gp_sleep(co_, 0.3);
    CHECK(!cancelled.called, "a cancelled load never calls back");
    CHECK(gp_gfx_image_count(app->gfx) == base, "and registers nothing (%d, was %d)", gp_gfx_image_count(app->gfx), base);
    remove(path);
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    Fix *fx = gp_entity_get(app, win()->root_view);
    api_tests(fx);
    notify_root();
    pixel_tests(fx);
    lifecycle_tests(fx);
    async_tests();
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    gp_entity v = gp_view_new(app, sizeof(Fix), &(Fix){ 0 }, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "image_test", .width = 760, .height = 600 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "image_test: %d FAILURE(S)\n" : "image_test: all checks passed\n", failures);
    return failures != 0;
}
