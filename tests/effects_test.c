/*
 * Filters, blend modes and masks on elements (gp_filter_*, gp_blend_mode, gp_mask), from frames read back from the renderer: every color filter against CSS's
 * formula, two filters in either order, a blur's spread and a drop shadow that follows the shape and not the box; every blend mode against an independent
 * implementation of the W3C formulas, a translucent one, and one inside a group (it blends with the group, not the window); a gradient mask, a mask with a
 * hole, a luminance mask, a mask after a blur (the blur does not leak past it), a turned masked element; a filter in a hover style. Needs a display, at scale 1.
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
            if (failures++ < 60) {                                                     \
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
#define WHITE gp_rgb(0xffffff)
#define RED gp_rgb(0xff0000)
#define GREEN gp_rgb(0x00ff00)
#define FILL gp_rgb(0x4080c0)
#define BACK gp_rgb(0x3366cc)   /* the backdrop of the blend modes: .2 .4 .8 */
#define SRC gp_rgb(0xcc9933)    /* their source: .8 .6 .2 */

#define CELL(...) gp_div(gp_w(30), gp_h(20), gp_flex_none(), __VA_ARGS__)

/* a row of cells; the test finds cell k of row r */
static gp_node *filters_row(void) {
    return gp_div(gp_flex_row(), gp_gap(4),
        /* 0 */ CELL(gp_bg(RED), gp_filter_grayscale(1)),
        /* 1 */ CELL(gp_bg(FILL), gp_filter_invert(1)),
        /* 2 */ CELL(gp_bg(WHITE), gp_filter_brightness(0.5f)),
        /* 3 */ CELL(gp_bg(WHITE), gp_filter_sepia(1)),
        /* 4 */ CELL(gp_bg(RED), gp_filter_hue_rotate(180)),
        /* 5 */ CELL(gp_bg(gp_rgb(0x606060)), gp_filter_contrast(2)),
        /* 6 */ CELL(gp_bg(RED), gp_filter_saturate(0)),
        /* 7 */ CELL(gp_bg(WHITE), gp_filter_opacity(0.5f)),
        /* 8 */ CELL(gp_bg(RED), gp_filter_grayscale(1), gp_filter_brightness(2)),
        /* 9 */ CELL(gp_bg(RED), gp_filter_brightness(2), gp_filter_grayscale(1)),
        /* 10 */ CELL(gp_bg(FILL), gp_filter_saturate(0.5f)),
        /* 11 */ CELL(gp_bg(gp_rgba(0x4080c080)), gp_filter_invert(1)));
}

static gp_node *blend_row(void) {
    gp_node *cells[16];
    for (int m = 0; m < 16; m++) cells[m] = CELL(gp_bg(BACK), gp_div(gp_size_full(), gp_bg(SRC), gp_blend_mode(m)));
    gp_node **list = gp_frame_alloc(sizeof cells);
    memcpy(list, cells, sizeof cells);
    return gp_div(gp_flex_row(), gp_gap(4), gp_children(list, 16));
}

static gp_entity g_root;
static int g_phase;   /* 1: the hole of the second mask moves right */

static gp_node *render(void *self, gp_ctx *cx) {
    const gp_color clear = { 1, 1, 1, 0 };
    return gp_div(
        gp_size_full(), gp_flex_col(), gp_items_start(), gp_gap(12), gp_p(10), gp_bg(BLACK),
        /* 0 */ filters_row(),
        /* 1 */ blend_row(),
        /* 2: a translucent multiply; multiply in a group over black (it sees the group's empty layer, not the black); a blur; a drop shadow of a circle */
        gp_div(gp_flex_row(), gp_gap(20), gp_items_start(),
               CELL(gp_bg(BACK), gp_div(gp_size_full(), gp_bg(gp_rgba(0xcc993380)), gp_blend_mode(GP_BLEND_MULTIPLY))),
               CELL(gp_opacity(0.999f), gp_div(gp_size_full(), gp_bg(RED), gp_blend_mode(GP_BLEND_MULTIPLY))),
               CELL(gp_opacity(0.999f), gp_bg(gp_rgba(0x0000ff80)), gp_div(gp_size_full(), gp_bg(gp_rgba(0xff000080)), gp_blend_mode(GP_BLEND_MULTIPLY))),
               gp_div(gp_w(20), gp_h(20), gp_flex_none(), gp_ml(10), gp_bg(WHITE), gp_filter_blur(4)),
               gp_div(gp_w(20), gp_h(20), gp_flex_none(), gp_ml(10), gp_bg(WHITE), gp_rounded_full(), gp_filter_drop_shadow(10, 5, 0, GREEN)),
               gp_div(gp_w(20), gp_h(20), gp_flex_none(), gp_ml(10), gp_bg(WHITE), gp_rounded_full(), gp_filter_drop_shadow(10, 5, 8, GREEN))),
        /* 3: masks: a gradient left to right; a hole (only the middle shows); luminance of a gray; a blur cut by its mask; a turned one */
        gp_div(gp_flex_row(), gp_gap(20), gp_items_start(),
               gp_div(gp_w(100), gp_h(20), gp_flex_none(), gp_bg(WHITE), gp_mask(gp_div(gp_size_full(), gp_bg_gradient(90, gp_stop(WHITE, 0), gp_stop(clear, 1))))),
               gp_div(gp_w(40), gp_h(40), gp_flex_none(), gp_bg(WHITE), gp_mask(gp_div(gp_size_full(), gp_p(10), gp_div(gp_w(20), gp_h(20), gp_ml(g_phase ? 10 : 0), gp_bg(RED))))),
               gp_div(gp_w(30), gp_h(20), gp_flex_none(), gp_bg(WHITE), gp_mask_luminance(), gp_mask(gp_div(gp_size_full(), gp_bg(gp_rgb(0x808080))))),
               gp_div(gp_w(40), gp_h(40), gp_flex_none(), gp_p(10), gp_filter_blur(6), gp_mask(gp_div(gp_size_full(), gp_bg(WHITE))), gp_div(gp_size_full(), gp_bg(WHITE))),
               gp_div(gp_w(40), gp_h(40), gp_flex_none(), gp_ml(10), gp_mt(10), gp_bg(WHITE), gp_rotate(90),
                      gp_mask(gp_div(gp_size_full(), gp_div(gp_w(20), gp_h(40), gp_bg(WHITE)))))),
        /* 4: grayscale on hover; a blur in a clip (it spreads to the clip and not past it) */
        gp_div(gp_flex_row(), gp_gap(30),
               CELL(gp_bg(RED), gp_hover_style(gp_group(gp_filter_grayscale(1)))),
               gp_div(gp_w(40), gp_h(40), gp_flex_none(), gp_overflow_hidden(), gp_p(10), gp_div(gp_size_full(), gp_bg(WHITE), gp_filter_blur(6)))));
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
static gp_point at(int r, int k, float dx, float dy) {
    gp_el *rw = row(r), *c = rw->children[k];
    return (gp_point){ rw->pos.x + c->pos.x + dx, rw->pos.y + c->pos.y + dy };
}
static uint8_t g_px[4][3];   /* (a few at once: two read one after the other are both kept) */
static int g_pxi;
static const uint8_t *pix(const uint8_t *f, gp_point p) {
    uint8_t *o = g_px[g_pxi++ & 3];
    memcpy(o, f + ((size_t)(int)p.y * (size_t)app->capture_w + (size_t)(int)p.x) * 3, 3);
    return o;
}
static bool rgb(const uint8_t *p, int r, int g, int b, int tol) { return abs(p[0] - r) <= tol && abs(p[1] - g) <= tol && abs(p[2] - b) <= tol; }
#define P3(p) (p)[0], (p)[1], (p)[2]
static int b8(float v) { v = v < 0 ? 0 : v > 1 ? 1 : v; return (int)lroundf(v * 255.f); }

/* ---- the W3C blend formulas, written again here, in double */
static double sep(int m, double b, double s) {
    switch (m) {
    case 1: return b * s;
    case 2: return b + s - b * s;
    case 3: return b <= 0.5 ? 2 * b * s : 1 - 2 * (1 - b) * (1 - s);        /* overlay: screen / multiply by the backdrop */
    case 4: return b < s ? b : s;
    case 5: return b > s ? b : s;
    case 6: return b == 0 ? 0 : s == 1 ? 1 : fmin(1, b / (1 - s));
    case 7: return b == 1 ? 1 : s == 0 ? 0 : 1 - fmin(1, (1 - b) / s);
    case 8: return s <= 0.5 ? 2 * b * s : 1 - 2 * (1 - b) * (1 - s);
    case 9: {
        if (s <= 0.5) return b - (1 - 2 * s) * b * (1 - b);
        double d = b <= 0.25 ? ((16 * b - 12) * b + 4) * b : sqrt(b);
        return b + (2 * s - 1) * (d - b);
    }
    case 10: return fabs(b - s);
    case 11: return b + s - 2 * b * s;
    }
    return s;
}
static double L(const double *c) { return 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2]; }
static void clipc(double *c) {
    double l = L(c), n = fmin(c[0], fmin(c[1], c[2])), x = fmax(c[0], fmax(c[1], c[2]));
    for (int i = 0; i < 3; i++) {
        if (n < 0) c[i] = l + (c[i] - l) * l / (l - n);
    }
    for (int i = 0; i < 3; i++) {
        if (x > 1) c[i] = l + (c[i] - l) * (1 - l) / (x - l);
    }
}
static void setlum(double *c, double l) {
    double d = l - L(c);
    for (int i = 0; i < 3; i++) c[i] += d;
    clipc(c);
}
static double S(const double *c) { return fmax(c[0], fmax(c[1], c[2])) - fmin(c[0], fmin(c[1], c[2])); }
static void setsat(double *c, double s) {   /* the spec's: the largest to s, the smallest to 0, the middle in proportion */
    int imax = 0, imin = 0;
    for (int i = 1; i < 3; i++) { if (c[i] > c[imax]) imax = i; if (c[i] < c[imin]) imin = i; }
    if (imax == imin) { c[0] = c[1] = c[2] = 0; return; }
    int imid = 3 - imax - imin;
    c[imid] = (c[imid] - c[imin]) * s / (c[imax] - c[imin]);
    c[imax] = s;
    c[imin] = 0;
}
static void blend(int m, const double *b, const double *s, double *out) {
    double t[3];
    switch (m) {
    case 12: memcpy(t, s, sizeof t); setsat(t, S(b)); setlum(t, L(b)); break;
    case 13: memcpy(t, b, sizeof t); setsat(t, S(s)); setlum(t, L(b)); break;
    case 14: memcpy(t, s, sizeof t); setlum(t, L(b)); break;
    case 15: memcpy(t, b, sizeof t); setlum(t, L(s)); break;
    default: for (int i = 0; i < 3; i++) t[i] = sep(m, b[i], s[i]);
    }
    memcpy(out, t, sizeof t);
}

static void run(void) {
    wait_draws(3);
    CHECK(win()->tree->nchildren == 5, "%u rows", win()->tree->nchildren);
    if (win()->tree->nchildren != 5) return;
    uint8_t *f = frame();
    if (!f) { CHECK(0, "no frame"); return; }
    const uint8_t *p;
#define MID(r, k) at(r, k, 15, 10)

    /* ---- filters: each against CSS's matrix for it */
    p = pix(f, MID(0, 0));
    CHECK(rgb(p, 54, 54, 54, 2), "grayscale(1) of red: %d %d %d", P3(p));
    p = pix(f, MID(0, 1));
    CHECK(rgb(p, 0xbf, 0x7f, 0x3f, 2), "invert(1): %d %d %d", P3(p));
    p = pix(f, MID(0, 2));
    CHECK(rgb(p, 128, 128, 128, 2), "brightness(.5) of white: %d %d %d", P3(p));
    p = pix(f, MID(0, 3));
    CHECK(rgb(p, 255, 255, b8(0.272f + 0.534f + 0.131f), 2), "sepia(1) of white: %d %d %d", P3(p));
    p = pix(f, MID(0, 4));
    CHECK(rgb(p, 0, b8(0.426f), b8(0.426f), 2), "hue-rotate(180) of red: %d %d %d", P3(p));
    p = pix(f, MID(0, 5));
    CHECK(rgb(p, b8(2 * 0x60 / 255.f - 0.5f), b8(2 * 0x60 / 255.f - 0.5f), b8(2 * 0x60 / 255.f - 0.5f), 2), "contrast(2) of 0x60: %d %d %d", P3(p));
    p = pix(f, MID(0, 6));
    CHECK(rgb(p, 54, 54, 54, 2), "saturate(0) of red: %d %d %d", P3(p));
    p = pix(f, MID(0, 7));
    CHECK(rgb(p, 128, 128, 128, 2), "opacity(.5) of white over black: %d %d %d", P3(p));
    p = pix(f, MID(0, 8));
    CHECK(rgb(p, 108, 108, 108, 3), "grayscale then brightness(2): %d %d %d", P3(p));
    p = pix(f, MID(0, 9));
    CHECK(rgb(p, 54, 54, 54, 2), "brightness(2) then grayscale (red stays 1): %d %d %d", P3(p));
    {   /* saturate(.5) of FILL: CSS's matrix, every coefficient */
        const float v = 0.5f, c0[3] = { 0x40 / 255.f, 0x80 / 255.f, 0xc0 / 255.f };
        const float m[3][3] = { { 0.213f + 0.787f * v, 0.715f - 0.715f * v, 0.072f - 0.072f * v },
                                { 0.213f - 0.213f * v, 0.715f + 0.285f * v, 0.072f - 0.072f * v },
                                { 0.213f - 0.213f * v, 0.715f - 0.715f * v, 0.072f + 0.928f * v } };
        int e[3];
        for (int i = 0; i < 3; i++) e[i] = b8(m[i][0] * c0[0] + m[i][1] * c0[1] + m[i][2] * c0[2]);
        p = pix(f, MID(0, 10));
        CHECK(rgb(p, e[0], e[1], e[2], 2), "saturate(.5) of 0x4080c0: %d %d %d, want %d %d %d", P3(p), e[0], e[1], e[2]);
    }

    {   /* invert(1) of a half-transparent color: the straight color inverted, still half transparent, over black */
        const float a = 0x80 / 255.f;
        p = pix(f, MID(0, 11));
        CHECK(rgb(p, b8(a * 0xbf / 255.f), b8(a * 0x7f / 255.f), b8(a * 0x3f / 255.f), 2), "invert(1) of a translucent color: %d %d %d", P3(p));
    }

    /* ---- blend modes: each cell is SRC blended over BACK, both opaque: exactly the formula */
    const double B[3] = { 0x33 / 255.0, 0x66 / 255.0, 0xcc / 255.0 }, Sc[3] = { 0xcc / 255.0, 0x99 / 255.0, 0x33 / 255.0 };
    for (int m = 0; m < 16; m++) {
        double o[3];
        blend(m, B, Sc, o);
        p = pix(f, MID(1, m));
        CHECK(rgb(p, b8((float)o[0]), b8((float)o[1]), b8((float)o[2]), 2), "blend mode %d: %d %d %d, want %d %d %d", m, P3(p), b8((float)o[0]), b8((float)o[1]),
              b8((float)o[2]));
    }
    /* half-opaque multiply: half the backdrop, half the product */
    {
        double o[3];
        double a = 0x80 / 255.0;
        blend(1, B, Sc, o);
        p = pix(f, MID(2, 0));
        CHECK(rgb(p, b8((float)((1 - a) * B[0] + a * o[0])), b8((float)((1 - a) * B[1] + a * o[1])), b8((float)((1 - a) * B[2] + a * o[2])), 2),
              "a translucent multiply: %d %d %d", P3(p));
    }
    p = pix(f, MID(2, 1));
    CHECK(rgb(p, 255, 0, 0, 2), "multiply in a group sees the group, not the black under it: %d %d %d", P3(p));
    {   /* half red multiplied over half blue in a group: (.25, 0, .25) at alpha .75, over black */
        const double sa = 0x80 / 255.0, da = 0x80 / 255.0;
        double r = sa * (1 - da), b = da * (1 - sa);
        p = pix(f, MID(2, 2));
        CHECK(rgb(p, b8((float)r), 0, b8((float)b), 2), "a translucent multiply over a translucent backdrop: %d %d %d, want %d 0 %d", P3(p), b8((float)r), b8((float)b));
    }

    /* ---- a blur of a white 20 x 20 box, sigma 4: nearly white in the middle, half at the edge, a little 4 px outside, nothing 14 px out */
    gp_point c = at(2, 3, 10, 10);
    p = pix(f, c);
    CHECK(p[0] >= 235 && p[0] <= 252, "blur: the middle %d", p[0]);
    p = pix(f, (gp_point){ c.x + 10, c.y });
    CHECK(p[0] >= 110 && p[0] <= 140, "blur: the edge %d", p[0]);
    p = pix(f, (gp_point){ c.x + 14, c.y });
    CHECK(p[0] >= 15 && p[0] <= 60, "blur: 4 px out %d", p[0]);
    p = pix(f, (gp_point){ c.x + 24, c.y });
    CHECK(p[0] <= 2, "blur: 14 px out %d", p[0]);
    p = pix(f, (gp_point){ c.x, c.y + 10 });
    CHECK(p[0] >= 110 && p[0] <= 140, "blur: the bottom edge %d", p[0]);
    p = pix(f, (gp_point){ c.x, c.y - 14 });
    CHECK(p[0] >= 15 && p[0] <= 60, "blur: 4 px above %d", p[0]);
    p = pix(f, (gp_point){ c.x + 10, c.y + 10 });
    CHECK(p[0] >= 50 && p[0] <= 75, "blur: the corner (a quarter) %d", p[0]);

    /* ---- a drop shadow of a circle, moved 10 right and 5 down: green where the moved circle is and the circle is not; black at the corner of the moved
     * box (a shadow of the box would be green there) */
    c = at(2, 4, 10, 10);   /* the circle's middle */
    p = pix(f, (gp_point){ c.x + 10, c.y + 5 });
    CHECK(rgb(p, 0, 255, 0, 3), "drop shadow at the moved middle: %d %d %d", P3(p));
    p = pix(f, (gp_point){ c.x + 18, c.y + 5 });
    CHECK(rgb(p, 0, 255, 0, 3), "drop shadow past the box: %d %d %d", P3(p));
    p = pix(f, (gp_point){ c.x + 18, c.y + 13 });
    CHECK(rgb(p, 0, 0, 0, 3), "drop shadow: the moved box's corner is not in it: %d %d %d", P3(p));
    p = pix(f, c);
    CHECK(rgb(p, 255, 255, 255, 2), "drop shadow: the circle on top: %d %d %d", P3(p));
    /* a blurred one: soft, green at less than full past the circle's moved edge */
    c = at(2, 5, 10, 10);
    p = pix(f, (gp_point){ c.x + 10, c.y + 5 });
    CHECK(p[1] > 100 && p[1] < 250 && p[0] < 3, "a blurred drop shadow at the moved middle: %d %d %d", P3(p));

    /* ---- masks */
    gp_point m0 = at(3, 0, 0, 10);
    p = pix(f, (gp_point){ m0.x + 2, m0.y });
    CHECK(p[0] > 240, "gradient mask: the left %d", p[0]);
    p = pix(f, (gp_point){ m0.x + 50, m0.y });
    CHECK(p[0] > 110 && p[0] < 145, "gradient mask: the middle %d", p[0]);
    p = pix(f, (gp_point){ m0.x + 98, m0.y });
    CHECK(p[0] < 10, "gradient mask: the right %d", p[0]);
    gp_point m1 = at(3, 1, 0, 0);
    p = pix(f, (gp_point){ m1.x + 20, m1.y + 20 });
    const uint8_t *q = pix(f, (gp_point){ m1.x + 5, m1.y + 5 });
    CHECK(p[0] == 255 && p[1] == 255 && p[2] == 255, "a mask with a hole: the middle shows, white (the mask's red is not seen): %d %d %d", P3(p));
    q = pix(f, (gp_point){ m1.x + 5, m1.y + 5 });
    CHECK(rgb(q, 0, 0, 0, 1), "a mask with a hole: the edge does not: %d %d %d", P3(q));
    p = pix(f, at(3, 2, 15, 10));
    CHECK(rgb(p, 128, 128, 128, 3), "a luminance mask of 0x80: %d %d %d", P3(p));
    gp_point m3 = at(3, 3, 0, 0);
    p = pix(f, (gp_point){ m3.x + 20, m3.y + 20 });
    CHECK(p[0] > 60, "a blurred, masked box: the middle %d", p[0]);
    p = pix(f, (gp_point){ m3.x + 2, m3.y + 20 });
    q = pix(f, (gp_point){ m3.x - 3, m3.y + 20 });
    CHECK(p[0] > 3 && q[0] == 0, "a blurred, masked box: inside the mask %d, past it nothing: %d", p[0], q[0]);
    /* turned a quarter: the mask's left half becomes the top half */
    gp_point m4 = at(3, 4, 0, 0);
    p = pix(f, (gp_point){ m4.x + 20, m4.y + 10 });
    q = pix(f, (gp_point){ m4.x + 20, m4.y + 30 });
    CHECK(rgb(p, 255, 255, 255, 2) && rgb(q, 0, 0, 0, 2), "a turned mask: the top %d %d %d, the bottom %d %d %d", P3(p), P3(q));

    /* ---- a blur in a clip: up to the clip's edge, not past it */
    gp_point k = at(4, 1, 0, 0);
    p = pix(f, (gp_point){ k.x + 5, k.y + 20 });
    q = pix(f, (gp_point){ k.x - 3, k.y + 20 });
    CHECK(p[0] > 15 && q[0] == 0, "a blur in a clip: inside it %d, past it %d", p[0], q[0]);

    /* ---- a filter in a hover style */
    gp_point h = at(4, 0, 15, 10);
    p = pix(f, h);
    CHECK(rgb(p, 255, 0, 0, 2), "at rest red: %d %d %d", P3(p));
    gp_simulate_mouse(app, GP_SIM_MOVE, h.x, h.y, 0, 0);
    gp_sleep(co_, 0.03);
    f = frame();
    if (!f) return;
    p = pix(f, h);
    CHECK(rgb(p, 54, 54, 54, 2), "hovered gray: %d %d %d", P3(p));

    /* ---- the mask's hole moves 10 right: where it was (its left part) is masked now */
    g_phase = 1;
    gp_ctx rc = gp_ctx_for(app, g_root);
    gp_notify(&rc);
    wait_draws(2);
    f = frame();
    if (!f) return;
    m1 = at(3, 1, 0, 0);
    p = pix(f, (gp_point){ m1.x + 13, m1.y + 20 });
    q = pix(f, (gp_point){ m1.x + 33, m1.y + 20 });
    CHECK(rgb(p, 0, 0, 0, 1) && rgb(q, 255, 255, 255, 1), "the hole moved: where it was %d %d %d, where it is %d %d %d", P3(p), P3(q));
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    run();
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    g_root = gp_view_new(app, 1, &(char){ 0 }, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "effects_test", .width = 600, .height = 360 }, g_root);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "effects_test: %d FAILURE(S)\n" : "effects_test: all checks passed\n", failures);
    return failures != 0;
}
