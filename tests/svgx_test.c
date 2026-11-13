/*
 * SVG <use>, <symbol>, clip-path and mask (svgx.c): documents are drawn by gp_svg_render and the pixels compared with what the shapes
 * should cover (a supersampled coverage computed here for circles, exact squares otherwise): where a <use> puts its copy (x, y, a transform
 * before them, a fill it passes down, nested and symbol uses with a viewBox), that a document that points at itself or doubles at
 * every step ends, that a clip leaves exactly the clip shape's coverage (in the clipped element's own coordinates, through a transform, a union
 * of shapes, a use inside the clip, written in style), that what is drawn after a clipped element covers it and what is drawn before is
 * covered, a mask's luminance and alpha, a gradient mask, and that a clip or mask it cannot use is ignored. No display needed.
 */
#include "../src/internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;
#define CHECK(cond, ...)                                                               \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            if (failures++ < 50) {                                                     \
                printf("FAIL %s:%d: ", __FILE__, __LINE__);                            \
                printf(__VA_ARGS__);                                                   \
                printf("\n");                                                          \
            }                                                                          \
        }                                                                              \
    } while (0)

#define HEAD "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' width='40' height='40' viewBox='0 0 40 40'>"
#define TAIL "</svg>"

static int W = 40;
static gp_text_system *g_ts;
static bool g_ts_has_font;
static uint8_t *draw(const char *doc) {
    uint8_t *px = gp_svg_render(doc, strlen(doc), 0x000000, W, W, 40.f, g_ts);
    return px;
}
#define P(px, x, y) ((px) + ((size_t)(y) * (size_t)W + (size_t)(x)) * 4)
static bool is_clear(const uint8_t *p) { return p[3] < 8; }
static bool is_color(const uint8_t *p, int r, int g, int b) { return p[3] > 247 && abs(p[0] - r) < 8 && abs(p[1] - g) < 8 && abs(p[2] - b) < 8; }
static bool is_red(const uint8_t *p) { return is_color(p, 255, 0, 0); }
static bool is_blue(const uint8_t *p) { return is_color(p, 0, 0, 255); }
static bool is_green(const uint8_t *p) { return is_color(p, 0, 255, 0); }

/* pictures that should be the same: the clipped or masked shape against the clip shape drawn directly (same rasterizer, so the edges agree to a few bytes, which also
 * checks nothing is lost or doubled where the layers are put together) */
static int differ(const uint8_t *a, const uint8_t *b, int tol) {
    int bad = 0;
    for (size_t i = 0; i < (size_t)W * (size_t)W; i++)
        for (int c = 0; c < 4; c++)
            if (abs((int)a[i * 4 + c] - (int)b[i * 4 + c]) > tol) { bad++; break; }
    return bad;
}
static int vs_direct(const uint8_t *px, const char *reference_doc, int tol) {
    uint8_t *ref = draw(reference_doc);
    int bad = ref && px ? differ(px, ref, tol) : 99999;
    free(ref);
    return bad;
}

static double now(void) { return (double)gp_now_ns() * 1e-9; }

static void test_use(void) {
    uint8_t *px = draw(HEAD "<defs><rect id='r' width='10' height='10' fill='#f00'/></defs><use href='#r' x='5' y='3'/>" TAIL);
    CHECK(px && is_red(P(px, 10, 8)) && is_red(P(px, 5, 3)) && is_red(P(px, 14, 12)), "use: the square is where x and y put it");
    CHECK(px && is_clear(P(px, 2, 2)) && is_clear(P(px, 16, 8)) && is_clear(P(px, 10, 14)), "use: and nowhere else (the one in defs is not drawn)");
    free(px);

    px = draw(HEAD "<defs><rect id='r' width='10' height='10'/></defs><use xlink:href='#r' fill='#00f' x='20' y='20'/>" TAIL);
    CHECK(px && is_blue(P(px, 25, 25)), "use: xlink:href, and the fill of the use passes to a shape that sets none");
    free(px);

    px = draw(HEAD "<defs><rect id='r' width='10' height='10' fill='#f00'/></defs><use href='#r' fill='#00f' x='20' y='20'/>" TAIL);
    CHECK(px && is_red(P(px, 25, 25)), "use: a fill the shape sets itself stays");
    free(px);

    px = draw(HEAD "<defs><rect id='r' width='10' height='10' fill='#f00'/></defs><use href='#r' x='10' transform='translate(5 0)'/>" TAIL);
    CHECK(px && is_red(P(px, 20, 5)) && is_clear(P(px, 12, 5)) && is_clear(P(px, 27, 5)), "use: the transform comes before x (the square is at 15..25)");
    free(px);

    px = draw(HEAD "<defs><rect id='r' width='10' height='10' fill='#f00'/><g id='g2'><use href='#r' x='10'/><use href='#r' x='20'/></g></defs><use href='#g2' y='5'/>" TAIL);
    CHECK(px && is_red(P(px, 15, 10)) && is_red(P(px, 25, 10)) && is_clear(P(px, 5, 10)) && is_clear(P(px, 35, 10)) && is_clear(P(px, 15, 2)), "use: a use of a group of uses");
    free(px);

    px = draw(HEAD "<symbol id='s' viewBox='0 0 10 10'><rect width='10' height='10' fill='#0f0'/></symbol><use href='#s' width='20' height='20' x='10' y='10'/>" TAIL);
    CHECK(px && is_green(P(px, 12, 12)) && is_green(P(px, 27, 27)) && is_clear(P(px, 5, 5)) && is_clear(P(px, 32, 32)), "use: a symbol is scaled by its viewBox into the width and height (10..30)");
    free(px);

    px = draw(HEAD "<defs><rect id='r' width='5' height='5' fill='#f00'/></defs><use href='#r' x='5' transform='scale(2)'/>" TAIL);
    CHECK(px && is_red(P(px, 12, 3)) && is_red(P(px, 18, 8)) && is_clear(P(px, 6, 3)) && is_clear(P(px, 22, 3)), "use: x is inside the transform (scale 2 then 5: the square is at 10..20)");
    free(px);

    px = draw(HEAD "<symbol id='s' viewBox='0 0 10 20'><rect width='10' height='20' fill='#0f0'/></symbol><use href='#s' width='20' height='20'/>" TAIL);
    CHECK(px && is_green(P(px, 10, 10)) && is_green(P(px, 6, 2)) && is_clear(P(px, 2, 10)) && is_clear(P(px, 18, 10)), "use: a tall symbol in a square is kept in proportion (5..15)");
    free(px);

    px = draw(HEAD "<symbol id='s' viewBox='0 0 10 5'><rect width='10' height='5' fill='#0f0'/></symbol><use href='#s' width='20' height='20'/>" TAIL);
    CHECK(px && is_green(P(px, 10, 10)) && is_clear(P(px, 10, 2)) && is_clear(P(px, 10, 18)), "use: a symbol is kept in proportion and centered (20 x 10, in the middle of 20 x 20)");
    free(px);

    px = draw(HEAD "<defs><rect id='r' width='10' height='10' fill='#f00'/></defs><use href='#nothing'/><use/><use href='#r' x='30'/>" TAIL);
    CHECK(px && is_red(P(px, 35, 5)) && is_clear(P(px, 5, 5)), "use: a use of nothing draws nothing and the others still do");
    free(px);

    /* a document that points at itself, and one that doubles at each step, end quickly */
    double t0 = now();
    px = draw(HEAD "<g id='a'><use href='#a'/></g><use href='#a'/><rect width='5' height='5' fill='#f00'/>" TAIL);
    CHECK(px && is_red(P(px, 2, 2)), "use: a cycle ends and the rest is drawn");
    free(px);
    px = draw(HEAD "<g id='a'><rect width='3' height='3'/><use href='#a'/><use href='#a'/><use href='#a'/><use href='#a'/><use href='#a'/><use href='#a'/></g><use href='#a' fill='#f00'/>" TAIL);
    free(px);
    CHECK(now() - t0 < 2.0, "use: a cycle and a bomb take %.2f s", now() - t0);
}

static void test_clip(void) {
    const char *clip = "<defs><clipPath id='c'><circle cx='20' cy='20' r='10'/></clipPath></defs>";
    char doc[1024];
    snprintf(doc, sizeof doc, HEAD "%s<rect width='40' height='40' fill='#f00' clip-path='url(#c)'/>" TAIL, clip);
    uint8_t *px = draw(doc);
    CHECK(px && is_red(P(px, 20, 20)) && is_clear(P(px, 2, 2)) && is_clear(P(px, 20, 5)) && is_red(P(px, 20, 12)), "clip: red inside the circle, nothing outside");
    int bad = vs_direct(px, HEAD "<circle cx='20' cy='20' r='10' fill='#f00'/>" TAIL, 3);
    CHECK(bad == 0, "clip: %d pixels differ from the circle drawn directly", bad);
    free(px);

    int save = W;
    W = 80;   /* at twice the size */
    px = draw(doc);
    bad = vs_direct(px, HEAD "<circle cx='20' cy='20' r='10' fill='#f00'/>" TAIL, 3);
    CHECK(bad == 0, "clip at 2x: %d pixels differ", bad);
    free(px);
    W = save;

    /* the order of drawing: what is drawn after covers the clipped element, what is before is covered by it */
    snprintf(doc, sizeof doc, HEAD "%s<rect width='40' height='40' fill='#f00' clip-path='url(#c)'/><rect x='15' y='15' width='10' height='10' fill='#00f'/>" TAIL, clip);
    px = draw(doc);
    CHECK(px && is_blue(P(px, 20, 20)) && is_red(P(px, 20, 12)), "clip: a shape after it covers it");
    free(px);
    snprintf(doc, sizeof doc, HEAD "%s<rect x='15' y='15' width='10' height='10' fill='#00f'/><rect width='40' height='40' fill='#f00' clip-path='url(#c)'/>" TAIL, clip);
    px = draw(doc);
    CHECK(px && is_red(P(px, 20, 20)), "clip: one before it is covered by it");
    free(px);
    snprintf(doc, sizeof doc, HEAD "%s<rect width='6' height='6' fill='#0f0'/><rect width='40' height='40' fill='#f00' clip-path='url(#c)'/><rect x='34' y='34' width='6' height='6' fill='#00f'/>" TAIL, clip);
    px = draw(doc);
    CHECK(px && is_green(P(px, 2, 2)) && is_blue(P(px, 37, 37)) && is_red(P(px, 20, 20)) && is_clear(P(px, 2, 37)), "clip: plain shapes before and after it keep their places");
    free(px);
    snprintf(doc, sizeof doc, HEAD "%s<rect width='40' height='40' fill='#0f0'/><rect width='40' height='40' fill='#f00' clip-path='url(#c)'/><rect x='15' y='15' width='10' height='10' fill='#00f'/>" TAIL, clip);
    px = draw(doc);
    CHECK(px && is_blue(P(px, 20, 20)) && is_red(P(px, 20, 12)) && is_green(P(px, 3, 3)), "clip: a shape after it is above it even when a plain one came before (green, red circle, blue)");
    free(px);

    /* a clipped group with a transform: the clip is in the group's own coordinates, moved with it */
    snprintf(doc, sizeof doc, HEAD "%s<g transform='translate(10 0)' clip-path='url(#c)'><rect width='40' height='40' fill='#f00'/></g>" TAIL, clip);
    px = draw(doc);
    CHECK(px && is_red(P(px, 30, 20)) && is_clear(P(px, 17, 20)) && is_clear(P(px, 12, 20)), "clip: through a group's transform the circle is at 30, 20");
    bad = vs_direct(px, HEAD "<g transform='translate(10 0)'><circle cx='20' cy='20' r='10' fill='#f00'/></g>" TAIL, 3);
    CHECK(bad == 0, "clip with a transform: %d pixels differ from the moved circle", bad);
    free(px);

    /* an ancestor's transform too, and the element's own */
    snprintf(doc, sizeof doc, HEAD "%s<g transform='translate(10 0)'><rect transform='translate(0 5)' width='40' height='30' fill='#f00' clip-path='url(#c)'/></g>" TAIL, clip);
    px = draw(doc);
    CHECK(px && is_red(P(px, 30, 25)) && is_clear(P(px, 30, 12)) && is_clear(P(px, 17, 25)), "clip: an ancestor's transform and the element's own (the circle is at 30, 25)");
    bad = vs_direct(px, HEAD "<circle cx='30' cy='25' r='10' fill='#f00'/>" TAIL, 3);
    CHECK(bad == 0, "clip with two transforms: %d pixels differ from the circle at 30, 25", bad);
    free(px);

    /* a union of two shapes, one of them through a use */
    px = draw(HEAD "<defs><circle id='k' cx='10' cy='20' r='8'/><clipPath id='u'><use href='#k'/><circle cx='30' cy='20' r='8'/></clipPath></defs><rect width='40' height='40' fill='#f00' clip-path='url(#u)'/>" TAIL);
    CHECK(px && is_red(P(px, 10, 20)) && is_red(P(px, 30, 20)) && is_clear(P(px, 20, 20)) && is_clear(P(px, 10, 2)), "clip: two shapes (one a use) clip to their union");
    free(px);

    /* clip written in style; clip with a stroke and a fill that mean nothing; clip-rule */
    snprintf(doc, sizeof doc, HEAD "%s<rect width='40' height='40' fill='#f00' style='clip-path:url(#c)'/>" TAIL, clip);
    px = draw(doc);
    CHECK(px && is_red(P(px, 20, 20)) && is_clear(P(px, 2, 2)), "clip: in the style attribute");
    free(px);
    px = draw(HEAD "<defs><clipPath id='d'><circle cx='20' cy='20' r='10' fill='none' stroke='#fff' stroke-width='6' opacity='0.1'/></clipPath></defs><rect width='40' height='40' fill='#f00' clip-path='url(#d)'/>" TAIL);
    CHECK(px && is_red(P(px, 20, 20)) && is_clear(P(px, 20, 8)) && is_clear(P(px, 20, 4)), "clip: the clip shape's own paint and stroke do not matter, only its area");
    free(px);
    px = draw(HEAD "<defs><clipPath id='e'><path clip-rule='evenodd' d='M5 5H35V35H5Z M15 15H25V25H15Z'/></clipPath></defs><rect width='40' height='40' fill='#f00' clip-path='url(#e)'/>" TAIL);
    CHECK(px && is_red(P(px, 8, 8)) && is_clear(P(px, 20, 20)) && is_clear(P(px, 2, 2)), "clip: clip-rule evenodd leaves a hole");
    free(px);

    /* what cannot be used is ignored: the shape is drawn whole */
    px = draw(HEAD "<defs><clipPath id='b' clipPathUnits='objectBoundingBox'><circle cx='.5' cy='.5' r='.2'/></clipPath></defs><rect width='40' height='40' fill='#f00' clip-path='url(#b)'/>" TAIL);
    CHECK(px && is_red(P(px, 2, 2)) && is_red(P(px, 37, 37)), "clip: objectBoundingBox units are not supported: no clip");
    free(px);
    px = draw(HEAD "<rect width='40' height='40' fill='#f00' clip-path='url(#missing)'/>" TAIL);
    CHECK(px && is_red(P(px, 2, 2)), "clip: a clip that is not there: no clip");
    free(px);
    px = draw(HEAD "<defs><linearGradient id='g'><stop offset='0' stop-color='#00f'/></linearGradient></defs><rect width='40' height='40' fill='#f00' clip-path='url(#g)'/>" TAIL);
    CHECK(px && is_red(P(px, 2, 2)), "clip: a reference to something that is no clip path: no clip");
    free(px);
    px = draw(HEAD "<defs><mask id='m'><rect width='10' height='10' fill='#fff'/></mask></defs><rect width='40' height='40' fill='#f00' clip-path='url(#m)'/>" TAIL);
    CHECK(px && is_red(P(px, 30, 30)), "clip: a mask named as a clip path is no clip");
    free(px);

    /* a clipPath nobody uses is not drawn as a shape */
    px = draw(HEAD "<clipPath id='z'><rect width='40' height='40' fill='#f00'/></clipPath><rect x='10' y='10' width='4' height='4' fill='#00f'/>" TAIL);
    CHECK(px && is_clear(P(px, 2, 2)) && is_blue(P(px, 12, 12)), "clip: an unused clipPath draws nothing");
    free(px);

    /* a gradient defined in defs still paints a clipped shape */
    px = draw(HEAD "<defs><linearGradient id='g' x1='0' x2='1' y1='0' y2='0'><stop offset='0' stop-color='#f00'/><stop offset='1' stop-color='#00f'/></linearGradient>" "<clipPath id='c'><rect width='40' height='20'/></clipPath></defs><rect width='40' height='40' fill='url(#g)' clip-path='url(#c)'/>" TAIL);
    CHECK(px && P(px, 3, 10)[0] > 200 && P(px, 36, 10)[2] > 200 && is_clear(P(px, 20, 30)), "clip: a gradient fill through a clip");
    free(px);
}

static void test_mask(void) {
    char doc[1024];
    const char *shape = "<rect width='40' height='40' fill='#f00' mask='url(#m)'/>";
    snprintf(doc, sizeof doc, HEAD "<defs><mask id='m'><rect width='40' height='40' fill='#fff'/></mask></defs>%s" TAIL, shape);
    uint8_t *px = draw(doc);
    CHECK(px && is_red(P(px, 20, 20)), "mask: white shows everything");
    free(px);
    snprintf(doc, sizeof doc, HEAD "<defs><mask id='m'><rect width='40' height='40' fill='#000'/></mask></defs>%s" TAIL, shape);
    px = draw(doc);
    CHECK(px && is_clear(P(px, 20, 20)), "mask: black hides everything");
    free(px);
    snprintf(doc, sizeof doc, HEAD "<defs><mask id='m'><rect width='40' height='40'/></mask></defs>%s" TAIL, shape);
    px = draw(doc);
    CHECK(px && is_clear(P(px, 20, 20)), "mask: a shape with no fill is black (the default): hides");
    free(px);
    snprintf(doc, sizeof doc, HEAD "<defs><mask id='m'><rect width='40' height='40' fill='#808080'/></mask></defs>%s" TAIL, shape);
    px = draw(doc);
    CHECK(px && abs(P(px, 20, 20)[3] - 128) < 8, "mask: mid gray is half (%d)", px ? P(px, 20, 20)[3] : -1);
    free(px);
    snprintf(doc, sizeof doc, HEAD "<defs><mask id='m'><rect width='40' height='40' fill='#fff' fill-opacity='0.25'/></mask></defs>%s" TAIL, shape);
    px = draw(doc);
    CHECK(px && abs(P(px, 20, 20)[3] - 64) < 8, "mask: the mask's alpha counts (%d)", px ? P(px, 20, 20)[3] : -1);
    free(px);
    snprintf(doc, sizeof doc, HEAD "<defs><mask id='m'><circle cx='20' cy='20' r='10' fill='#fff'/></mask></defs>%s" TAIL, shape);
    px = draw(doc);
    CHECK(px && is_red(P(px, 20, 20)) && is_clear(P(px, 2, 2)), "mask: a white circle");
    int bad = vs_direct(px, HEAD "<circle cx='20' cy='20' r='10' fill='#f00'/>" TAIL, 3);
    CHECK(bad == 0, "mask: %d pixels differ from the circle drawn directly", bad);
    free(px);
    /* a gradient mask: opaque at the left, gone at the right */
    snprintf(doc, sizeof doc,
             HEAD "<defs><linearGradient id='l' x1='0' x2='1' y1='0' y2='0'><stop offset='0' stop-color='#fff'/><stop offset='1' stop-color='#000'/></linearGradient>"
                  "<mask id='m'><rect width='40' height='40' fill='url(#l)'/></mask></defs>%s" TAIL, shape);
    px = draw(doc);
    bool ok = px != NULL;
    for (int x = 2; ok && x < 38; x += 4) {
        float want = 1.f - ((float)x + 0.5f) / 40.f;
        ok = fabsf((float)P(px, x, 20)[3] / 255.f - want) < 0.07f;
    }
    CHECK(ok, "mask: a white to black gradient is a ramp of alpha (%d at x=4, %d at x=20, %d at x=36)", px ? P(px, 4, 20)[3] : -1, px ? P(px, 20, 20)[3] : -1, px ? P(px, 36, 20)[3] : -1);
    free(px);
    /* a clip and a mask on the same element; a mask on a group with a transform */
    px = draw(HEAD "<defs><clipPath id='c'><rect width='20' height='40'/></clipPath><mask id='m'><rect width='40' height='20' fill='#fff'/></mask></defs>"
                   "<rect width='40' height='40' fill='#f00' clip-path='url(#c)' mask='url(#m)'/>" TAIL);
    CHECK(px && is_red(P(px, 10, 10)) && is_clear(P(px, 30, 10)) && is_clear(P(px, 10, 30)), "clip and mask together: the overlap (the top left quarter)");
    free(px);
    px = draw(HEAD "<defs><mask id='m'><rect width='20' height='20' fill='#fff'/></mask></defs><g transform='translate(20 20)' mask='url(#m)'><rect width='40' height='40' fill='#f00'/></g>" TAIL);
    CHECK(px && is_red(P(px, 30, 30)) && is_clear(P(px, 10, 10)) && is_clear(P(px, 5, 30)), "mask: in the coordinates of a transformed group (20..40)");
    free(px);
    /* a mask that is not there */
    px = draw(HEAD "<rect width='40' height='40' fill='#f00' mask='url(#nope)'/>" TAIL);
    CHECK(px && is_red(P(px, 20, 20)), "mask: one that is not there: no mask");
    free(px);
}

/* the box of the ink in a picture (alpha above 40), as left, top, right, bottom pixel edges; false if there is none */
static bool ink_box(const uint8_t *px, int *l, int *t, int *r, int *b) {
    int x0 = W, y0 = W, x1 = -1, y1 = -1;
    for (int y = 0; y < W; y++)
        for (int x = 0; x < W; x++)
            if (P(px, x, y)[3] > 40) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
    *l = x0; *t = y0; *r = x1 + 1; *b = y1 + 1;
    return x1 >= 0;
}
static long ink_sum(const uint8_t *px) {
    long s = 0;
    for (size_t i = 0; i < (size_t)W * (size_t)W; i++) s += px[i * 4 + 3];
    return s;
}

static void test_text(void) {
    gp_glyph_pos g[4];
    if (!g_ts || gp_text_shape(g_ts, 400, 16.f, "H", 1, g, 4, NULL) < 1 || g[0].glyph == 0) {
        printf("SKIP: no font to draw SVG text with\n");
        return;
    }
    int l, t, r, b;
    uint8_t *px = draw(HEAD "<text x='5' y='30' font-size='12' fill='#f00'>HHH</text>" TAIL);
    bool ok = px && ink_box(px, &l, &t, &r, &b);
    CHECK(ok, "text: something is drawn");
    if (ok) {
        float w = 0;
        gp_text_shape(g_ts, 400, 12.f, "HHH", 3, NULL, 0, &w);
        CHECK(abs(l - 5) <= 3 && abs((r - l) - (int)w) <= 5, "text: starts near x = 5 (%d) and is as wide as it shapes (%d, shaped %.1f)", l, r - l, (double)w);
        CHECK(b >= 28 && b <= 32 && t >= 19 && t <= 24, "text: sits on its baseline at y = 30 (bottom %d) and is a capital's height (top %d)", b, t);
        int cx = (l + r) / 2, cy = (t + b) / 2;
        const uint8_t *stem = P(px, l + 2, cy);
        CHECK(is_red(stem) || (stem[3] > 200 && stem[0] > 200 && stem[1] < 40), "text: painted with the fill (%d %d %d %d)", stem[0], stem[1], stem[2], stem[3]);
        (void)cx;
    }
    free(px);

    /* the size: twice the font size is twice the height */
    uint8_t *a = draw(HEAD "<text x='2' y='38' font-size='10'>H</text>" TAIL);
    uint8_t *c = draw(HEAD "<text x='2' y='38' font-size='20'>H</text>" TAIL);
    int l2, t2, r2, b2;
    if (a && c && ink_box(a, &l, &t, &r, &b) && ink_box(c, &l2, &t2, &r2, &b2)) {
        float ratio = (float)(b2 - t2) / (float)(b - t);
        CHECK(ratio > 1.8f && ratio < 2.2f, "text: font-size 20 is twice as tall as 10 (%d against %d)", b2 - t2, b - t);
    } else {
        CHECK(false, "text: sizes drawn");
    }
    free(a);
    free(c);

    /* the anchor: the text ends, or is centered, at x */
    uint8_t *s0 = draw(HEAD "<text x='20' y='30' font-size='16'>HHHH</text>" TAIL);
    uint8_t *s1 = draw(HEAD "<text x='20' y='30' font-size='16' text-anchor='middle'>HHHH</text>" TAIL);
    uint8_t *s2 = draw(HEAD "<text x='20' y='30' font-size='16' text-anchor='end'>HHHH</text>" TAIL);
    if (s0 && s1 && s2) {
        int a0, a1, a2, b0, b1, b2, t_, bb;
        ink_box(s0, &a0, &t_, &b0, &bb);
        ink_box(s1, &a1, &t_, &b1, &bb);
        ink_box(s2, &a2, &t_, &b2, &bb);
        CHECK(abs(a0 - 20) <= 3, "text: start anchor: ink starts at %d (x = 20)", a0);
        CHECK(abs((a1 + b1) / 2 - 20) <= 3, "text: middle anchor: ink is centered on %d (x = 20)", (a1 + b1) / 2);
        CHECK(abs(b2 - 20) <= 3, "text: end anchor: ink ends at %d (x = 20)", b2);
    } else {
        CHECK(false, "text: anchors drawn");
    }
    free(s0);
    free(s1);
    free(s2);

    /* bold has more ink; italic is the same ink slanted (the box is wider) */
    uint8_t *n = draw(HEAD "<text x='2' y='30' font-size='24'>Ho</text>" TAIL);
    uint8_t *bo = draw(HEAD "<text x='2' y='30' font-size='24' font-weight='bold'>Ho</text>" TAIL);
    CHECK(n && bo && ink_sum(bo) > ink_sum(n) * 105 / 100, "text: bold has more ink (%ld against %ld)", bo ? ink_sum(bo) : -1, n ? ink_sum(n) : -1);
    free(n);
    free(bo);

    /* italic is slanted (a wider box), a family changes the glyphs (when the system has another to give) */
    uint8_t *up = draw(HEAD "<text x='4' y='30' font-size='16'>HHH</text>" TAIL);
    uint8_t *it = draw(HEAD "<text x='4' y='30' font-size='16' font-style='italic'>HHH</text>" TAIL);
    if (up && it) {
        int ul, ut, ur, ub, il, it_, ir, ib;
        ink_box(up, &ul, &ut, &ur, &ub);
        ink_box(it, &il, &it_, &ir, &ib);
        CHECK(differ(up, it, 8) > 20, "text: italic is not upright (%d pixels differ)", differ(up, it, 8));
    }
    free(up);
    free(it);
    {
        float wm = 0, ws = 0;
        gp_text_shape(g_ts, GP_FONT_SEL(gp_text_family(g_ts, "monospace"), 400), 16.f, "iiii", 4, NULL, 0, &wm);
        gp_text_shape(g_ts, GP_FONT_SEL(gp_text_family(g_ts, "sans-serif"), 400), 16.f, "iiii", 4, NULL, 0, &ws);
        if (wm > ws + 4.f) {
            uint8_t *fm = draw(HEAD "<text x='2' y='30' font-size='16' font-family='monospace'>iiii</text>" TAIL);
            uint8_t *fs = draw(HEAD "<text x='2' y='30' font-size='16' font-family='sans-serif'>iiii</text>" TAIL);
            int a0, a1, b0, b1, c0, c1;
            if (fm && fs && ink_box(fm, &a0, &c0, &b0, &c1) && ink_box(fs, &a1, &c0, &b1, &c1)) CHECK((b0 - a0) > (b1 - a1) + 3, "text: font-family monospace is wider than sans-serif (%d, %d)", b0 - a0, b1 - a1);
            else CHECK(false, "text: families drawn");
            free(fm);
            free(fs);
        }
    }

    /* tspans: their own fill and position, and dx / dy */
    uint8_t *ts = draw(HEAD "<text y='20' font-size='14'><tspan x='2' fill='#f00'>HH</tspan><tspan x='22' fill='#00f'>HH</tspan></text>" TAIL);
    if (ts) {
        bool red = false, blue = false;
        for (int y = 5; y < 22; y++)
            for (int x = 0; x < 20; x++) red |= is_red(P(ts, x, y));
        for (int y = 5; y < 22; y++)
            for (int x = 20; x < 40; x++) blue |= is_blue(P(ts, x, y));
        CHECK(red && blue, "text: two tspans with their own fills and x");
    }
    free(ts);
    uint8_t *d0 = draw(HEAD "<text x='5' y='20' font-size='14'>H</text>" TAIL);
    uint8_t *d1 = draw(HEAD "<text x='5' y='20' dx='10' dy='12' font-size='14'>H</text>" TAIL);
    if (d0 && d1) {
        ink_box(d0, &l, &t, &r, &b);
        ink_box(d1, &l2, &t2, &r2, &b2);
        CHECK(l2 - l == 10 && t2 - t == 12, "text: dx and dy move it (%d, %d)", l2 - l, t2 - t);
    }
    free(d0);
    free(d1);

    /* the pen goes on across tspans without an x; a tspan with an x starts there; dx adds a gap; font-size in pt */
    {
        uint8_t *pn = draw(HEAD "<text x='2' y='30' font-size='12'><tspan fill='#f00'>HH</tspan><tspan fill='#00f'>HH</tspan></text>" TAIL);
        uint8_t *dxg = draw(HEAD "<text x='2' y='30' font-size='12' fill='#f00'>H<tspan dx='10' fill='#00f'>H</tspan></text>" TAIL);
        uint8_t *ab = draw(HEAD "<text y='30' font-size='12'><tspan x='2' fill='#f00'>HH</tspan><tspan x='28' fill='#00f'>HH</tspan></text>" TAIL);
        int rl = 99, rr = -1, bl = 99, br = -1;
        for (int pass = 0; pass < 3; pass++) {
            uint8_t *im = pass == 0 ? pn : pass == 1 ? dxg : ab;
            rl = 99; rr = -1; bl = 99; br = -1;
            for (int y = 0; im && y < W; y++)
                for (int x = 0; x < W; x++) {
                    const uint8_t *p = P(im, x, y);
                    if (p[3] > 200 && p[0] > 200 && p[2] < 40) { if (x < rl) rl = x; if (x > rr) rr = x; }
                    if (p[3] > 200 && p[2] > 200 && p[0] < 40) { if (x < bl) bl = x; if (x > br) br = x; }
                }
            if (pass == 0) CHECK(rr > 0 && bl > rr - 1 && bl < rr + 6, "text: the second tspan continues where the first ended (red ends %d, blue starts %d)", rr, bl);
            if (pass == 1) CHECK(rr > 0 && bl >= rr + 9 && bl <= rr + 16, "text: dx = 10 leaves a gap of about 10 (plus the letters side bearings) (red ends %d, blue starts %d)", rr, bl);
            if (pass == 2) CHECK(rl > 0 && bl >= 28 && bl <= 31 && rl <= 4, "text: a tspan with x = 28 starts there (%d), the first at 2 (%d)", bl, rl);
        }
        free(pn);
        free(dxg);
        free(ab);
        uint8_t *pt = draw(HEAD "<text x='2' y='30' font-size='12pt'>H</text>" TAIL);
        uint8_t *px16 = draw(HEAD "<text x='2' y='30' font-size='16'>H</text>" TAIL);
        CHECK(pt && px16 && differ(pt, px16, 0) == 0, "text: 12pt is 16px");
        free(pt);
        free(px16);
    }

    /* entities and white space */
    uint8_t *e1 = draw(HEAD "<text x='2' y='30' font-size='16'>&amp;&#65;&#x42;   x\n  y</text>" TAIL);
    uint8_t *e2 = draw(HEAD "<text x='2' y='30' font-size='16'>&amp;AB x y</text>" TAIL);
    CHECK(e1 && e2 && differ(e1, e2, 0) == 0, "text: entities are decoded and white space is one space");
    free(e1);
    free(e2);

    /* text is a shape: a clip cuts it, a use copies it, a transform moves it */
    uint8_t *cl = draw(HEAD "<defs><clipPath id='c'><rect width='20' height='40'/></clipPath></defs><text x='2' y='30' font-size='24' fill='#f00' clip-path='url(#c)'>HHHH</text>" TAIL);
    if (cl) {
        bool left = false, right = false;
        for (int y = 0; y < W; y++) {
            for (int x = 0; x < 20; x++) left |= P(cl, x, y)[3] > 200;
            for (int x = 20; x < 40; x++) right |= P(cl, x, y)[3] > 8;
        }
        CHECK(left && !right, "text: a clip cuts it at x = 20");
    }
    free(cl);
    uint8_t *u1 = draw(HEAD "<defs><text id='t' font-size='14'>H</text></defs><use href='#t' x='20' y='30' fill='#00f'/>" TAIL);
    uint8_t *u2 = draw(HEAD "<text x='20' y='30' font-size='14' fill='#00f'>H</text>" TAIL);
    CHECK(u1 && u2 && ink_sum(u1) > 0 && differ(u1, u2, 2) == 0, "text: a use of a text is the text there");
    free(u1);
    free(u2);
    uint8_t *tr1 = draw(HEAD "<g transform='translate(10 5)'><text x='2' y='20' font-size='14'>H</text></g>" TAIL);
    uint8_t *tr2 = draw(HEAD "<text x='12' y='25' font-size='14'>H</text>" TAIL);
    CHECK(tr1 && tr2 && differ(tr1, tr2, 2) == 0, "text: inside a transformed group");
    free(tr1);
    free(tr2);

    /* the font properties come from the group above, a style attribute too */
    uint8_t *i1 = draw(HEAD "<g font-size='20'><text x='2' y='30'>H</text></g>" TAIL);
    uint8_t *i2 = draw(HEAD "<text x='2' y='30' style='font-size:20px'>H</text>" TAIL);
    uint8_t *i3 = draw(HEAD "<text x='2' y='30' font-size='20'>H</text>" TAIL);
    CHECK(i1 && i2 && i3 && differ(i1, i3, 0) == 0 && differ(i2, i3, 0) == 0, "text: font-size from an ancestor and from style");
    free(i1);
    free(i2);
    free(i3);

    /* without a text system the text is not drawn (and nothing breaks) */
    gp_text_system *saved = g_ts;
    g_ts = NULL;
    uint8_t *no = draw(HEAD "<text x='2' y='30' font-size='24'>HHH</text><rect width='5' height='5' fill='#f00'/>" TAIL);
    CHECK(no && is_red(P(no, 2, 2)) && is_clear(P(no, 8, 22)), "text: without a text system only the shapes are drawn");
    free(no);
    g_ts = saved;
}

/* through the registry the way the app draws it: the text system the gfx was given is the one the text is drawn with, and the raster is cached */
static void test_registry(void) {
    gp_gfx *g = gp_gfx_new();
    gp_text_set_gfx(g_ts, g);
    const char *doc = HEAD "<defs><rect id='r' width='10' height='10' fill='#f00'/></defs><use href='#r' x='5' y='5'/>" "<text x='20' y='34' font-size='12' fill='#00f'>HH</text>" TAIL;
    gp_svg s = gp_gfx_svg_add(g, strdup(doc), strlen(doc), 40.f, 40.f);
    gp_image im = gp_gfx_svg_raster(g, s, 40, 0);
    gp_gfx_image *img = gp_gfx_image_get(g, im.index, im.generation);
    CHECK(img && img->w == 40 && is_red(img->pixels + ((size_t)10 * 40 + 10) * 4), "registry: the use is drawn");
    bool blue = false;
    for (int y = 20; img && y < 36; y++)
        for (int x = 20; x < 40; x++) blue |= is_blue(img->pixels + ((size_t)y * 40 + (size_t)x) * 4);
    gp_gfx_image *again = gp_gfx_image_get(g, im.index, im.generation);
    CHECK(blue || !g_ts_has_font, "registry: the text is drawn with the text system the gfx has");
    CHECK(again == img && gp_gfx_svg_raster(g, s, 40, 0).index == im.index, "registry: the raster is cached");
    gp_text_set_gfx(g_ts, NULL);
    gp_gfx_free(g);
}

/* the gaussian blur of a box [x0, x1] x [y0, y1] of alpha 1, at the middle of pixel (x, y), by the integral of the gaussian (an independent oracle) */
static float blurred_box(float x0, float x1, float y0, float y1, float sd, int x, int y, float scale) {
    float s = sd * scale * 1.41421356f;
    float fx = 0.5f * (erff(((float)x + 0.5f - x0 * scale) / s) - erff(((float)x + 0.5f - x1 * scale) / s));
    float fy = 0.5f * (erff(((float)y + 0.5f - y0 * scale) / s) - erff(((float)y + 0.5f - y1 * scale) / s));
    return fx * fy;
}

static float srgb_of_linear(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.f / 2.4f) - 0.055f; }

#define FILTER(body) HEAD "<defs><filter id='f' color-interpolation-filters='sRGB'>" body "</filter></defs>"
#define RECT10 "<rect x='15' y='15' width='10' height='10' fill='#f00' filter='url(#f)'/>"

static void test_filter(void) {
    uint8_t *px = draw(FILTER("<feOffset dx='5' dy='3'/>") RECT10 TAIL);
    CHECK(px && is_red(P(px, 22, 21)) && is_red(P(px, 29, 27)) && is_clear(P(px, 16, 16)) && is_clear(P(px, 33, 21)) && is_clear(P(px, 22, 15)), "filter: feOffset moves the shape (5, 3)");
    free(px);

    /* a blur against the integral of the gaussian */
    px = draw(FILTER("<feGaussianBlur stdDeviation='2'/>") RECT10 TAIL);
    int bad = 0;
    float worst = 0;
    for (int y = 5; y < 35 && px; y++)
        for (int x = 5; x < 35; x++) {
            float want = blurred_box(15, 25, 15, 25, 2, x, y, 1.f), got = (float)P(px, x, y)[3] / 255.f;
            if (fabsf(want - got) > 0.03f) bad++;
            worst = fmaxf(worst, fabsf(want - got));
        }
    CHECK(px && bad == 0, "filter: feGaussianBlur 2 against the integral of the gaussian: %d pixels more than 3%% off (worst %.3f)", bad, (double)worst);
    CHECK(px && P(px, 20, 20)[0] > 245 && P(px, 20, 20)[1] < 8, "filter: a blurred red box is still red inside");
    CHECK(px && P(px, 15, 20)[3] > 100 && P(px, 15, 20)[3] < 155, "filter: its edge is half (%d)", px ? P(px, 15, 20)[3] : -1);
    free(px);

    /* at twice the size the blur is twice as wide in pixels */
    int save = W;
    W = 80;
    px = draw(FILTER("<feGaussianBlur stdDeviation='2'/>") RECT10 TAIL);
    bad = 0;
    for (int y = 10; y < 70 && px; y++)
        for (int x = 10; x < 70; x++)
            if (fabsf(blurred_box(15, 25, 15, 25, 2, x, y, 2.f) - (float)P(px, x, y)[3] / 255.f) > 0.03f) bad++;
    CHECK(px && bad == 0, "filter: at 2x the blur is 4 px wide: %d pixels off", bad);
    free(px);
    W = save;

    /* x and y deviations */
    px = draw(FILTER("<feGaussianBlur stdDeviation='4 0'/>") RECT10 TAIL);
    CHECK(px && P(px, 12, 20)[3] > 20 && P(px, 20, 12)[3] < 4 && P(px, 20, 15)[3] > 190, "filter: stdDeviation 4 0 blurs across only");
    free(px);

    /* a drop shadow: the shape on top, a black half-opaque blurred copy below it, moved */
    px = draw(HEAD "<defs><filter id='f' color-interpolation-filters='sRGB'><feDropShadow dx='6' dy='4' stdDeviation='2' flood-color='#000' flood-opacity='0.5'/></filter></defs>" RECT10 TAIL);
    bad = 0;
    for (int y = 5; px && y < 38; y++)
        for (int x = 5; x < 38; x++) {
            float sa = 0.5f * blurred_box(21, 31, 19, 29, 2, x, y, 1.f);   /* the shadow's alpha */
            bool inside = x >= 15 && x < 25 && y >= 15 && y < 25;
            float want = inside ? 1.f : sa;
            if (fabsf(want - (float)P(px, x, y)[3] / 255.f) > 0.04f) bad++;
        }
    CHECK(px && bad == 0, "filter: feDropShadow: %d pixels differ from the shape over a half-opaque blurred shadow", bad);
    CHECK(px && is_red(P(px, 20, 20)) && P(px, 27, 24)[3] > 100 && P(px, 27, 24)[0] < 8, "filter: the shape on top, the shadow black beside it");
    free(px);

    /* the same built by hand from primitives, with named results: blur -> offset -> flood -> in -> merge */
    uint8_t *hand = draw(HEAD "<defs><filter id='f' color-interpolation-filters='sRGB'>"
                         "<feGaussianBlur in='SourceAlpha' stdDeviation='2' result='b'/><feOffset in='b' dx='6' dy='4' result='o'/>"
                         "<feFlood flood-color='#000' flood-opacity='0.5' result='c'/><feComposite in='c' in2='o' operator='in' result='s'/>"
                         "<feMerge><feMergeNode in='s'/><feMergeNode in='SourceGraphic'/></feMerge></filter></defs>" RECT10 TAIL);
    px = draw(HEAD "<defs><filter id='f' color-interpolation-filters='sRGB'><feDropShadow dx='6' dy='4' stdDeviation='2' flood-color='#000' flood-opacity='0.5'/></filter></defs>" RECT10 TAIL);
    CHECK(px && hand && differ(px, hand, 3) == 0, "filter: feDropShadow is blur, offset, flood, composite in, merge (%d pixels differ)", px && hand ? differ(px, hand, 3) : -1);
    free(px);
    free(hand);

    /* flood and composite in: a blue copy of the shape's outline */
    px = draw(FILTER("<feFlood flood-color='#00f'/><feComposite in2='SourceAlpha' operator='in'/>") RECT10 TAIL);
    CHECK(px && is_blue(P(px, 20, 20)) && is_clear(P(px, 10, 10)) && is_clear(P(px, 30, 20)), "filter: flood in SourceAlpha is a blue shape");
    free(px);
    px = draw(FILTER("<feFlood flood-color='#0f0' flood-opacity='0.5'/>") RECT10 TAIL);
    CHECK(px && abs(P(px, 5, 5)[3] - 128) < 3 && abs(P(px, 5, 5)[1] - 128) < 3 && P(px, 5, 5)[0] == 0, "filter: a flood fills the whole picture, here half-opaque green (%d %d %d %d)", px ? P(px, 5, 5)[0] : -1, px ? P(px, 5, 5)[1] : -1, px ? P(px, 5, 5)[2] : -1, px ? P(px, 5, 5)[3] : -1);
    free(px);

    /* the composite operators on two shapes (a red box and a blue flood of the left half): over, in, out, atop, xor, arithmetic */
    const char *ops[] = { "over", "in", "out", "atop", "xor" };
    for (int o = 0; o < 5; o++) {
        char doc[900];
        snprintf(doc, sizeof doc, HEAD "<defs><filter id='f' color-interpolation-filters='sRGB'><feFlood flood-color='#00f' result='fl'/>"
                                      "<feOffset in='SourceGraphic' dx='5' result='sh'/><feComposite in='sh' in2='SourceGraphic' operator='%s'/></filter></defs>" RECT10 TAIL, ops[o]);
        px = draw(doc);
        /* sh is the box moved 5 right (20..30), SourceGraphic is 15..25: a = sh, b = source */
        bool in_a = false, in_b = false;
        int ok = px != NULL;
        for (int x = 12; px && x < 34; x++) {
            in_a = x >= 20 && x < 30;
            in_b = x >= 15 && x < 25;
            float alpha = (float)P(px, x, 20)[3] / 255.f, want = 0;
            if (o == 0) want = in_a || in_b;
            else if (o == 1) want = in_a && in_b;
            else if (o == 2) want = in_a && !in_b;
            else if (o == 3) want = in_a && in_b ? 1.f : in_b && !in_a ? 1.f : 0.f;   /* atop: a where b is, plus b where a is not (b has a = 1 there) */
            else want = in_a != in_b;
            if (o == 3) want = in_b ? 1.f : 0.f;
            if (fabsf(alpha - want) > 0.03f) ok = 0;
        }
        CHECK(ok, "filter: feComposite operator %s", ops[o]);
        free(px);
    }
    {   /* operators that care which is which, with two colors: a blue flood (everywhere) and the red box */
        struct { const char *op; bool blue_in, red_in, blue_out, red_out; } cases[] = {
            /* where: inside the box, outside it */
            { "atop", true, false, false, false },   /* blue where the box is, nothing else */
            { "out", false, false, true, false },    /* blue where the box is not */
            { "in", true, false, false, false },
            { "xor", false, false, true, false },    /* blue outside, nothing inside (both opaque there) */
        };
        for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
            char doc[900];
            snprintf(doc, sizeof doc, HEAD "<defs><filter id='f' color-interpolation-filters='sRGB'><feFlood flood-color='#00f' result='fl'/>"
                                          "<feComposite in='fl' in2='SourceGraphic' operator='%s'/></filter></defs>" RECT10 TAIL, cases[k].op);
            px = draw(doc);
            bool inside_blue = px && is_blue(P(px, 20, 20)), outside_blue = px && is_blue(P(px, 5, 5));
            bool inside_red = px && is_red(P(px, 20, 20)), outside_clear = px && is_clear(P(px, 5, 5));
            bool inside_clear = px && is_clear(P(px, 20, 20));
            bool ok = (cases[k].blue_in ? inside_blue : inside_clear) && (cases[k].blue_out ? outside_blue : outside_clear) && !inside_red;
            CHECK(ok, "filter: feComposite %s of a blue flood and the red box (inside %d %d %d %d, outside %d %d %d %d)", cases[k].op,
                  px ? P(px, 20, 20)[0] : -1, px ? P(px, 20, 20)[1] : -1, px ? P(px, 20, 20)[2] : -1, px ? P(px, 20, 20)[3] : -1,
                  px ? P(px, 5, 5)[0] : -1, px ? P(px, 5, 5)[1] : -1, px ? P(px, 5, 5)[2] : -1, px ? P(px, 5, 5)[3] : -1);
            free(px);
        }
        px = draw(FILTER("<feFlood flood-color='#00f' result='fl'/><feComposite in='SourceGraphic' in2='fl' operator='arithmetic' k1='0' k2='1' k3='0' k4='0'/>") RECT10 TAIL);
        CHECK(px && is_red(P(px, 20, 20)) && is_clear(P(px, 5, 5)), "filter: arithmetic k2 weighs the first input (the red box only)");
        free(px);
        px = draw(FILTER("<feFlood flood-color='#00f' result='fl'/><feComposite in='SourceGraphic' in2='fl' operator='arithmetic' k1='0' k2='0' k3='1' k4='0'/>") RECT10 TAIL);
        CHECK(px && is_blue(P(px, 5, 5)) && is_blue(P(px, 20, 20)), "filter: arithmetic k3 weighs the second (the blue flood only)");
        free(px);
        /* SourceAlpha is the shape's silhouette in black; the default of feDropShadow is a 2 px move (and a 2 px blur, here 0) in black */
        px = draw(FILTER("<feMerge><feMergeNode in='SourceAlpha'/></feMerge>") RECT10 TAIL);
        CHECK(px && P(px, 20, 20)[3] == 255 && P(px, 20, 20)[0] == 0 && P(px, 20, 20)[1] == 0 && P(px, 20, 20)[2] == 0 && is_clear(P(px, 5, 5)), "filter: SourceAlpha is the shape in black");
        free(px);
        px = draw(FILTER("<feDropShadow stdDeviation='0'/>") RECT10 TAIL);
        CHECK(px && is_red(P(px, 20, 20)) && P(px, 26, 26)[3] > 250 && P(px, 26, 26)[0] == 0 && is_clear(P(px, 14, 14)), "filter: feDropShadow with no offset given moves the shadow (2, 2)");
        free(px);
        /* colors survive the trip through linear light unchanged when nothing is done to them */
        px = draw(HEAD "<defs><filter id='f'><feFlood flood-color='#808080'/></filter></defs>" RECT10 TAIL);
        CHECK(px && abs(P(px, 5, 5)[0] - 128) <= 1 && abs(P(px, 5, 5)[1] - 128) <= 1, "filter: a gray flood in linear light comes out the same gray (%d)", px ? P(px, 5, 5)[0] : -1);
        free(px);
    }
    px = draw(FILTER("<feComposite in='SourceGraphic' in2='SourceGraphic' operator='arithmetic' k1='0' k2='0.5' k3='0' k4='0'/>") RECT10 TAIL);
    CHECK(px && abs(P(px, 20, 20)[3] - 128) < 3 && abs(P(px, 20, 20)[0] - 128) < 3 && P(px, 20, 20)[1] == 0, "filter: arithmetic k2 = 0.5 halves it (%d %d)", px ? P(px, 20, 20)[3] : -1, px ? P(px, 20, 20)[0] : -1);
    free(px);

    /* feBlend of two floods: multiply in sRGB */
    px = draw(FILTER("<feFlood flood-color='#ff8000' result='a'/><feFlood flood-color='#808080' result='b'/><feBlend in='a' in2='b' mode='multiply'/>") RECT10 TAIL);
    CHECK(px && abs(P(px, 3, 3)[0] - 128) <= 2 && abs(P(px, 3, 3)[1] - 64) <= 2 && P(px, 3, 3)[2] <= 2 && P(px, 3, 3)[3] == 255, "filter: feBlend multiply (%d %d %d)", px ? P(px, 3, 3)[0] : -1, px ? P(px, 3, 3)[1] : -1, px ? P(px, 3, 3)[2] : -1);
    free(px);
    px = draw(FILTER("<feFlood flood-color='#804000' result='a'/><feFlood flood-color='#404040' result='b'/><feBlend in='a' in2='b' mode='screen'/>") RECT10 TAIL);
    CHECK(px && abs(P(px, 3, 3)[0] - (int)lroundf(255 * (1 - (1 - 128 / 255.f) * (1 - 64 / 255.f)))) <= 2 && abs(P(px, 3, 3)[2] - 64) <= 2, "filter: feBlend screen");
    free(px);

    /* color matrices: saturate 0 makes a gray (sRGB: the luminance weights; in linear light the same weights in linear light) */
    px = draw(FILTER("<feColorMatrix type='saturate' values='0'/>") RECT10 TAIL);
    CHECK(px && abs(P(px, 20, 20)[0] - 54) <= 2 && abs(P(px, 20, 20)[1] - 54) <= 2 && abs(P(px, 20, 20)[2] - 54) <= 2, "filter: saturate 0 in sRGB is a gray of 0.213 (%d)", px ? P(px, 20, 20)[0] : -1);
    free(px);
    px = draw(HEAD "<defs><filter id='f'><feColorMatrix type='saturate' values='0'/></filter></defs>" RECT10 TAIL);
    int lin = (int)lroundf(255.f * srgb_of_linear(0.213f));
    CHECK(px && abs(P(px, 20, 20)[0] - lin) <= 2, "filter: the default is linearRGB: the gray is %d (got %d)", lin, px ? P(px, 20, 20)[0] : -1);
    free(px);
    px = draw(FILTER("<feColorMatrix type='matrix' values='0 0 1 0 0  0 1 0 0 0  1 0 0 0 0  0 0 0 1 0'/>") RECT10 TAIL);
    CHECK(px && is_blue(P(px, 20, 20)), "filter: a matrix that swaps red and blue");
    free(px);
    px = draw(FILTER("<feColorMatrix type='hueRotate' values='120'/>") RECT10 TAIL);
    CHECK(px && P(px, 20, 20)[1] > 100 && P(px, 20, 20)[0] < 20, "filter: hueRotate 120 turns red toward green (%d %d %d)", px ? P(px, 20, 20)[0] : -1, px ? P(px, 20, 20)[1] : -1, px ? P(px, 20, 20)[2] : -1);
    free(px);
    px = draw(FILTER("<feColorMatrix type='luminanceToAlpha'/>") RECT10 TAIL);
    CHECK(px && abs(P(px, 20, 20)[3] - (int)lroundf(0.2125f * 255)) <= 2 && P(px, 20, 20)[0] == 0, "filter: luminanceToAlpha makes the luminance the alpha (%d)", px ? P(px, 20, 20)[3] : -1);
    free(px);

    /* linearRGB against sRGB: a blur of two colors side by side mixes lighter in linear light */
    uint8_t *lr = draw(HEAD "<defs><filter id='f'><feGaussianBlur stdDeviation='3'/></filter></defs><g filter='url(#f)'><rect x='5' y='5' width='15' height='30' fill='#fff'/><rect x='20' y='5' width='15' height='30' fill='#000'/></g>" TAIL);
    uint8_t *sr = draw(HEAD "<defs><filter id='f' color-interpolation-filters='sRGB'><feGaussianBlur stdDeviation='3'/></filter></defs><g filter='url(#f)'><rect x='5' y='5' width='15' height='30' fill='#fff'/><rect x='20' y='5' width='15' height='30' fill='#000'/></g>" TAIL);
    CHECK(lr && sr && P(lr, 20, 20)[0] > P(sr, 20, 20)[0] + 20, "filter: blurring white next to black is lighter in the middle in linear light (%d against %d)", lr ? P(lr, 20, 20)[0] : -1, sr ? P(sr, 20, 20)[0] : -1);
    free(lr);
    free(sr);

    /* a filter on a group filters the group as a whole (the two boxes blur together) */
    px = draw(HEAD "<defs><filter id='f' color-interpolation-filters='sRGB'><feGaussianBlur stdDeviation='3'/></filter></defs><g filter='url(#f)'><rect x='5' y='15' width='10' height='10' fill='#f00'/><rect x='20' y='15' width='10' height='10' fill='#f00'/></g>" TAIL);
    CHECK(px && P(px, 17, 20)[3] > 90 && P(px, 17, 20)[3] < 200, "filter: on a group, the gap between two shapes is filled by both blurs (%d)", px ? P(px, 17, 20)[3] : -1);
    free(px);

    /* the order: filter, then clip: the blur is cut at the clip */
    px = draw(HEAD "<defs><filter id='f' color-interpolation-filters='sRGB'><feGaussianBlur stdDeviation='3'/></filter><clipPath id='c'><rect x='0' y='0' width='22' height='40'/></clipPath></defs>"
                   "<rect x='15' y='15' width='10' height='10' fill='#f00' filter='url(#f)' clip-path='url(#c)'/>" TAIL);
    CHECK(px && P(px, 21, 20)[3] > 100 && is_clear(P(px, 23, 20)) && is_clear(P(px, 27, 20)), "filter: the clip cuts the blurred shape at x = 22 (%d, %d)", px ? P(px, 21, 20)[3] : -1, px ? P(px, 23, 20)[3] : -1);
    free(px);

    /* what cannot be used passes through */
    uint8_t *plain = draw(HEAD "<rect x='15' y='15' width='10' height='10' fill='#f00'/>" TAIL);
    px = draw(FILTER("<feTurbulence baseFrequency='0.1'/>") RECT10 TAIL);
    CHECK(px && plain && differ(px, plain, 0) == 0, "filter: a primitive that is not supported passes its input on");
    free(px);
    px = draw(HEAD "<rect x='15' y='15' width='10' height='10' fill='#f00' filter='url(#nothing)'/>" TAIL);
    CHECK(px && plain && differ(px, plain, 0) == 0, "filter: a filter that is not there: drawn plain");
    free(px);
    px = draw(HEAD "<defs><clipPath id='c'><rect width='5' height='5'/></clipPath></defs><rect x='15' y='15' width='10' height='10' fill='#f00' filter='url(#c)'/>" TAIL);
    CHECK(px && plain && differ(px, plain, 0) == 0, "filter: a reference to something that is no filter: drawn plain");
    free(px);
    px = draw(HEAD "<defs><filter id='f'></filter></defs><rect x='15' y='15' width='10' height='10' fill='#f00' filter='url(#f)'/>" TAIL);
    CHECK(px && plain && differ(px, plain, 0) == 0, "filter: a filter with no primitives changes nothing");
    free(px);
    px = draw(HEAD "<defs><filter id='f' color-interpolation-filters='sRGB'><feOffset dx='5'/></filter></defs><rect x='15' y='15' width='10' height='10' fill='#f00' style='filter:url(#f)'/>" TAIL);
    CHECK(px && is_red(P(px, 27, 20)) && is_clear(P(px, 17, 20)), "filter: in the style attribute");
    free(px);
    free(plain);
}

static void test_plain_untouched(void) {
    /* a document with none of it goes through nanosvg as it always did */
    uint8_t *px = draw(HEAD "<rect width='10' height='10' fill='#f00'/>" TAIL);
    CHECK(px && is_red(P(px, 5, 5)) && is_clear(P(px, 20, 20)), "a plain document");
    free(px);
    /* the word is in an attribute value or a comment, not a use or a clip: still right */
    px = draw(HEAD "<!-- <use href='#x'/> --><rect width='10' height='10' fill='#f00'><title>mask and clip-path</title></rect>" TAIL);
    CHECK(px && is_red(P(px, 5, 5)) && is_clear(P(px, 20, 20)), "words in a comment and a title");
    free(px);
    px = draw("<?xml version='1.0'?><!DOCTYPE svg [<!ENTITY a 'b'>]>" HEAD "<defs><rect id='r' width='10' height='10' fill='#f00'/></defs><use href='#r'/>" TAIL);
    CHECK(px && is_red(P(px, 5, 5)), "a prolog and a doctype with a subset");
    free(px);
}

int main(void) {
    test_use();
    test_clip();
    test_mask();
    test_filter();
    g_ts = gp_text_system_new();
    { gp_glyph_pos gg[4]; g_ts_has_font = gp_text_shape(g_ts, 400, 16.f, "H", 1, gg, 4, NULL) >= 1 && gg[0].glyph != 0; }
    test_text();
    test_registry();
    test_plain_untouched();
    gp_text_system_free(g_ts);
    printf(failures ? "svgx_test: %d FAILURE(S)\n" : "svgx_test: all checks passed\n", failures);
    return failures != 0;
}
