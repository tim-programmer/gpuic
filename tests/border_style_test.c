/*
 * Border colors per side, dashed and dotted borders, and gradient borders, from frames read back from the renderer. Per-side colors against the
 * geometry CSS describes (each side is the trapezoid between the outer box and the inner one: the diagonals run from the outer corners to the inner
 * ones, with unequal widths too), pixel by pixel; dashes against the exact coverage of each pixel by the dash intervals (a 2-width dash and a 1-width gap),
 * dots against a supersampled circle, the gaps showing the fill; a gradient border against the same gradient as a background on a twin box (the ring is where
 * the gradient shows); and who wins between gp_border_color and the side colors and the gradient. Needs a display, at scale 1.
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

#define FILL gp_rgb(0x182030)
#define RED gp_rgb(0xff0000)
#define GREEN gp_rgb(0x00ff00)
#define BLUE gp_rgb(0x0000ff)
#define YELLOW gp_rgb(0xffff00)
#define WHITE gp_rgb(0xffffff)

#define BOX(...) gp_div(gp_w(100), gp_h(60), gp_flex_none(), gp_bg(FILL), __VA_ARGS__)

static gp_node *render(void *self, gp_ctx *cx) {
    const gp_color_stop stops[2] = { { RED, 0 }, { BLUE, 1 } };
    return gp_div(
        gp_size_full(), gp_flex_row(), gp_flex_wrap(), gp_align_content_start(), gp_items_start(), gp_gap(10), gp_p(10), gp_bg(gp_rgb(0x000000)),
        /* 0: four colors, equal widths */
        BOX(gp_border(8), gp_border_color_top(RED), gp_border_color_right(GREEN), gp_border_color_bottom(BLUE), gp_border_color_left(YELLOW)),
        /* 1: four colors, four widths */
        BOX(gp_border_top(12), gp_border_right(6), gp_border_bottom(10), gp_border_left(4), gp_border_color_top(RED), gp_border_color_right(GREEN), gp_border_color_bottom(BLUE), gp_border_color_left(YELLOW)),
        /* 2: dashed */
        BOX(gp_border(4), gp_border_color(WHITE), gp_border_dashed(), NULL, NULL),
        /* 3: dotted */
        BOX(gp_border(6), gp_border_color(WHITE), gp_border_dotted(), NULL, NULL),
        /* 4: the last one wins: a side, then all */
        BOX(gp_border(6), gp_border_color_top(RED), gp_border_color(BLUE), NULL, NULL),
        /* 5: all, then a side */
        BOX(gp_border(6), gp_border_color(BLUE), gp_border_color_top(RED), NULL, NULL),
        /* 6: x and y */
        BOX(gp_border(6), gp_border_color_x(GREEN), gp_border_color_y(YELLOW), NULL, NULL),
        /* 7: dashes in side colors */
        BOX(gp_border(4), gp_border_color_top(RED), gp_border_color_left(GREEN), gp_border_color_bottom(BLUE), gp_border_color_right(YELLOW), gp_border_dashed()),
        /* 8, 9: a linear gradient border, and the same gradient as a background of a twin */
        BOX(gp_border(6), gp_border_linear_gradient(90, stops, 2), NULL, NULL, NULL),
        gp_div(gp_w(100), gp_h(60), gp_flex_none(), gp_bg_linear_gradient(90, stops, 2)),
        /* 10, 11: radial */
        BOX(gp_border(6), gp_border_radial_gradient(0.3f, 0.6f, stops, 2), NULL, NULL, NULL),
        gp_div(gp_w(100), gp_h(60), gp_flex_none(), gp_bg_radial_gradient(0.3f, 0.6f, stops, 2)),
        /* 12, 13: conic */
        BOX(gp_border(6), gp_border_conic_gradient(0.5f, 0.5f, 30, stops, 2), NULL, NULL, NULL),
        gp_div(gp_w(100), gp_h(60), gp_flex_none(), gp_bg_conic_gradient(0.5f, 0.5f, 30, stops, 2)),
        /* 14: a gradient, then a color: the color; 15: dashed gradient border (the gaps show the fill) */
        BOX(gp_border(6), gp_border_linear_gradient(90, stops, 2), gp_border_color(WHITE), NULL, NULL),
        BOX(gp_border(6), gp_border_linear_gradient(90, stops, 2), gp_border_dashed(), NULL, NULL),
        /* 16: no color at all: nothing is drawn of the border (the fill stays) */
        BOX(gp_border(6), NULL, NULL, NULL, NULL));
}

static bool wait_draws(unsigned n) {
    unsigned long target = app->stat_draws + n;
    for (int i = 0; i < 1000 && app->stat_draws < target; i++) gp_sleep(co_, 0.004);
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

typedef struct { int x0, y0; } Org;
static Org org_of(int i) {
    const gp_el *b = win()->tree->children[i];
    return (Org){ (int)lroundf(win()->tree->pos.x * 0 + b->pos.x), (int)lroundf(b->pos.y) };
}
static const uint8_t *px_at(const uint8_t *px, int pw, Org o, int x, int y) { return px + ((size_t)(o.y0 + y) * (size_t)pw + (size_t)(o.x0 + x)) * 3; }
static bool is_rgb(const uint8_t *p, int r, int g, int b, int tol) { return abs(p[0] - r) <= tol && abs(p[1] - g) <= tol && abs(p[2] - b) <= tol; }
static bool is_color(const uint8_t *p, gp_color c, int tol) { return is_rgb(p, (int)lroundf(c.r * 255), (int)lroundf(c.g * 255), (int)lroundf(c.b * 255), tol); }

/* --- CSS's geometry of a border with four colors: which side's trapezoid holds the point (x, y) of a W x H box, or -1 inside */
static bool in_poly(const float (*v)[2], int n, float x, float y) {
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++)
        if ((v[i][1] > y) != (v[j][1] > y) && x < (v[j][0] - v[i][0]) * (y - v[i][1]) / (v[j][1] - v[i][1]) + v[i][0]) in = !in;
    return in;
}
/* bw: top, right, bottom, left. Returns the side (0 top, 1 right, 2 bottom, 3 left) whose trapezoid holds the point, -1 inside the padding box (*alt: a second one, on a diagonal) */
static int side_of(float W, float H, const float bw[4], float x, float y, int *alt) {
    float t = bw[0], r = bw[1], b = bw[2], l = bw[3];
    float top[4][2] = { { 0, 0 }, { W, 0 }, { W - r, t }, { l, t } };
    float right[4][2] = { { W, 0 }, { W, H }, { W - r, H - b }, { W - r, t } };
    float bottom[4][2] = { { 0, H }, { W, H }, { W - r, H - b }, { l, H - b } };
    float left[4][2] = { { 0, 0 }, { 0, H }, { l, H - b }, { l, t } };
    *alt = -1;
    if (x > l && x < W - r && y > t && y < H - b) return -1;
    int found = -1;
    const float (*polys[4])[2] = { top, right, bottom, left };
    for (int s = 0; s < 4; s++) {
        bool in = in_poly(polys[s], 4, x, y);
        /* distance to the polygon's two diagonal edges: near one means the neighbour may own the pixel */
        if (in) { if (found < 0) found = s; else *alt = s; }
    }
    return found;
}

static void test_sides(const uint8_t *px, int pw, int idx, const float bw[4], const char *name) {
    Org o = org_of(idx);
    const gp_color col[4] = { RED, GREEN, BLUE, YELLOW };
    long checked = 0, bad = 0;
    for (int y = 0; y < 60; y++)
        for (int x = 0; x < 100; x++) {
            int alt;
            float cx = (float)x + 0.5f, cy = (float)y + 0.5f;
            int s = side_of(100, 60, bw, cx, cy, &alt);
            const uint8_t *p = px_at(px, pw, o, x, y);
            if (s < 0) { if (!is_rgb(p, 0x18, 0x20, 0x30, 2)) { bad++; } checked++; continue; }
            /* near a diagonal (the mitre between two sides) the two colors are blended over a pixel across it: half and half on the line itself
             * (whichever way a renderer rounds a pixel centre that lies on it), all one side a pixel away */
            bool ok = is_color(p, col[s], 2);
            if (!ok) {
                float t = bw[0], r = bw[1], b = bw[2], l = bw[3];
                float d[4] = { t > 0 ? cy / t : 1e9f, r > 0 ? (100 - cx) / r : 1e9f, b > 0 ? (60 - cy) / b : 1e9f, l > 0 ? cx / l : 1e9f };   /* in from each side, in its widths */
                float g[4][2] = { { 0, t > 0 ? 1 / t : 0 }, { r > 0 ? -1 / r : 0, 0 }, { 0, b > 0 ? -1 / b : 0 }, { l > 0 ? 1 / l : 0, 0 } };
                int sa = 0, sb = 1;
                if (d[sb] < d[sa]) { sa = 1; sb = 0; }
                for (int i = 2; i < 4; i++) { if (d[i] < d[sa]) { sb = sa; sa = i; } else if (d[i] < d[sb]) sb = i; }
                float e = (d[sb] - d[sa]) / hypotf(g[sb][0] - g[sa][0], g[sb][1] - g[sa][1]);   /* how far from the mitre, in pixels */
                float k = fminf(fmaxf(0.5f + e, 0.f), 1.f);
                gp_color m = { k * col[sa].r + (1 - k) * col[sb].r, k * col[sa].g + (1 - k) * col[sb].g, k * col[sa].b + (1 - k) * col[sb].b, 1 };
                ok = e < 1.5f && is_color(p, m, 4);
            }
            checked++;
            if (!ok) { if (bad < 3) printf("  %s: pixel (%d,%d) is %d %d %d, side %d\n", name, x, y, p[0], p[1], p[2], s); bad++; }
        }
    CHECK(bad == 0 && checked == 6000, "%s: %ld of %ld pixels are not the side the geometry says", name, bad, checked);
}

/* the coverage of the pixel [x, x + 1] by dashes [k * per, k * per + dash] */
static double dash_cov(double x, double per, double dash) {
    double c = 0;
    for (int k = (int)floor(x / per) - 1; k <= (int)floor((x + 1) / per) + 1; k++) {
        double a = fmax(x, k * per), b = fmin(x + 1, k * per + dash);
        if (b > a) c += b - a;
    }
    return c;
}

static void test_dashes(const uint8_t *px, int pw) {
    Org o = org_of(2);
    long checked = 0, bad = 0, lit = 0, gaps = 0;
    /* the top side, rows 0..3 (the width is 4), away from the corners; the left side too, down it */
    for (int y = 0; y < 4; y++)
        for (int x = 14; x < 86; x++) {
            double c = dash_cov(x, 12, 8), want = 0x18 + (255 - 0x18) * c;
            const uint8_t *p = px_at(px, pw, o, x, y);
            checked++;
            if (fabs(p[0] - want) > 4) bad++;
            if (c == 1) lit++;
            if (c == 0) gaps++;
        }
    for (int x = 0; x < 4; x++)
        for (int y = 14; y < 46; y++) {
            double c = dash_cov(y, 12, 8), want = 0x18 + (255 - 0x18) * c;
            const uint8_t *p = px_at(px, pw, o, x, y);
            checked++;
            if (fabs(p[0] - want) > 4) bad++;
        }
    CHECK(bad == 0 && lit > 100 && gaps > 50, "dashes: %ld of %ld pixels differ from the exact coverage (%ld lit, %ld in gaps)", bad, checked, lit, gaps);
    /* the gaps show the fill, the inside is the fill */
    const uint8_t *g = px_at(px, pw, o, 20, 1);   /* x = 20.5: 20..24 is a gap in a 12 period of 8 and 4 (8..12, 20..24) */
    CHECK(is_rgb(g, 0x18, 0x20, 0x30, 2) && is_rgb(px_at(px, pw, o, 50, 30), 0x18, 0x20, 0x30, 2), "the gaps show the fill (%d %d %d)", g[0], g[1], g[2]);
}

static void test_dots(const uint8_t *px, int pw) {
    Org o = org_of(3);
    /* a width of 6: dots of diameter 6 centred at x = 6 + 12 k, y = 3; supersampled */
    long checked = 0, bad = 0, full = 0;
    double worst = 0;
    for (int y = 0; y < 6; y++)
        for (int x = 18; x < 84; x++) {
            int in = 0;
            for (int sy = 0; sy < 8; sy++)
                for (int sx = 0; sx < 8; sx++) {
                    double xx = x + (sx + 0.5) / 8, yy = y + (sy + 0.5) / 8;
                    double cxk = (floor(xx / 12) + 0.5) * 12;
                    if ((xx - cxk) * (xx - cxk) + (yy - 3) * (yy - 3) <= 9) in++;
                }
            double c = in / 64.0, want = 0x18 + (255 - 0x18) * c;
            const uint8_t *p = px_at(px, pw, o, x, y);
            double err = fabs(p[0] - want);
            worst = fmax(worst, err);
            checked++;
            if (err > 30) bad++;
            if (c == 1) full++;
        }
    CHECK(bad == 0 && full > 30, "dots: %ld of %ld pixels are far from a supersampled circle (worst %.0f levels, %ld solid)", bad, checked, worst, full);
    CHECK(is_rgb(px_at(px, pw, o, 12, 3), 0x18, 0x20, 0x30, 40) && px_at(px, pw, o, 12, 3)[0] < 0x70, "between two dots is the fill");
    CHECK(is_rgb(px_at(px, pw, o, 30, 3), 255, 255, 255, 3), "the middle of a dot is the color");
}

static void test_gradients(const uint8_t *px, int pw) {
    for (int k = 0; k < 3; k++) {
        Org a = org_of(8 + 2 * k), b = org_of(9 + 2 * k);
        long checked = 0, bad = 0, interior_bad = 0;
        for (int y = 0; y < 60; y++)
            for (int x = 0; x < 100; x++) {
                const uint8_t *p = px_at(px, pw, a, x, y), *q = px_at(px, pw, b, x, y);
                bool ring = x < 6 || y < 6 || x >= 94 || y >= 54;
                if (ring) {
                    checked++;
                    if (abs(p[0] - q[0]) > 3 || abs(p[1] - q[1]) > 3 || abs(p[2] - q[2]) > 3) bad++;
                } else if (!is_rgb(p, 0x18, 0x20, 0x30, 2)) interior_bad++;
            }
        const char *names[3] = { "linear", "radial", "conic" };
        CHECK(bad == 0 && interior_bad == 0 && checked == 100 * 60 - 88 * 48, "%s: %ld of %ld ring pixels differ from the same gradient as a background; %ld interior pixels are not the fill", names[k], bad, checked, interior_bad);
    }
    /* the colors really vary around the ring */
    Org a = org_of(8);
    CHECK(px_at(px, pw, a, 2, 30)[0] > 200 && px_at(px, pw, a, 97, 30)[2] > 200, "a linear gradient border runs red to blue across the box");
}

static void run(void) {
    wait_draws(2);
    if (win()->scale != 1.f) { printf("border_style_test: skipped at scale %g\n", win()->scale); return; }
    const uint8_t *px = frame();
    CHECK(px != NULL, "no frame");
    if (!px) return;
    int pw = app->capture_w;
    const float eq[4] = { 8, 8, 8, 8 }, uneq[4] = { 12, 6, 10, 4 };
    test_sides(px, pw, 0, eq, "four colors, equal widths");
    test_sides(px, pw, 1, uneq, "four colors, four widths");
    test_dashes(px, pw);
    test_dots(px, pw);
    test_gradients(px, pw);
    {   /* the last one wins */
        Org o4 = org_of(4), o5 = org_of(5), o6 = org_of(6);
        CHECK(is_color(px_at(px, pw, o4, 50, 2), BLUE, 2) && is_color(px_at(px, pw, o4, 2, 30), BLUE, 2), "a side color, then gp_border_color: all blue");
        CHECK(is_color(px_at(px, pw, o5, 50, 2), RED, 2) && is_color(px_at(px, pw, o5, 50, 57), BLUE, 2) && is_color(px_at(px, pw, o5, 2, 30), BLUE, 2), "gp_border_color, then a side: only the top is red");
        CHECK(is_color(px_at(px, pw, o6, 2, 30), GREEN, 2) && is_color(px_at(px, pw, o6, 97, 30), GREEN, 2) && is_color(px_at(px, pw, o6, 50, 2), YELLOW, 2) && is_color(px_at(px, pw, o6, 50, 57), YELLOW, 2), "_x and _y");
    }
    {   /* dashes take the side's color */
        Org o = org_of(7);
        CHECK(is_color(px_at(px, pw, o, 3, 1), RED, 3) || is_color(px_at(px, pw, o, 4, 1), RED, 3), "a dash on the top is red (%d %d %d)", px_at(px, pw, o, 3, 1)[0], px_at(px, pw, o, 3, 1)[1], px_at(px, pw, o, 3, 1)[2]);
        CHECK(is_color(px_at(px, pw, o, 1, 30 - 0), GREEN, 3) || is_color(px_at(px, pw, o, 1, 26), GREEN, 3) || is_color(px_at(px, pw, o, 1, 27), GREEN, 3) || is_color(px_at(px, pw, o, 1, 28), GREEN, 3), "a dash on the left is green");
    }
    {   /* a gradient, then a color: the color */
        Org o = org_of(14);
        CHECK(is_color(px_at(px, pw, o, 50, 2), WHITE, 2) && is_color(px_at(px, pw, o, 2, 30), WHITE, 2), "a gradient then gp_border_color: the color");
        /* a dashed gradient border: gaps show the fill */
        Org d = org_of(15);
        long gaps = 0, lit = 0;
        for (int x = 14; x < 86; x++) {
            const uint8_t *p = px_at(px, pw, d, x, 2);
            if (is_rgb(p, 0x18, 0x20, 0x30, 2)) gaps++;
            else if (p[0] > 100 || p[2] > 100) lit++;
        }
        CHECK(gaps > 15 && lit > 30, "a dashed gradient border: %ld fill pixels in the gaps, %ld lit", gaps, lit);
        Org n = org_of(16);
        CHECK(is_rgb(px_at(px, pw, n, 2, 30), 0x18, 0x20, 0x30, 2) && is_rgb(px_at(px, pw, n, 50, 30), 0x18, 0x20, 0x30, 2), "a border without a color draws nothing: the fill");
    }
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    run();
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    gp_open_window(app, &(gp_window_options){ .title = "border_style_test", .width = 560, .height = 380 }, gp_view_new(app, 1, NULL, render, NULL));
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "border_style_test: %d FAILURE(S)\n" : "border_style_test: all checks passed\n", failures);
    return failures != 0;
}
