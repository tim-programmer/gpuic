/*
 * Stroked curves (gp_curve, gp_polyline), from frames read back from the renderer and from simulated pointer events. Pixels against independent geometry (the exact capsule,
 * the flat-capped slab, a dense sampling of the Bezier, the miter written out as a polygon): straight lines, S curves, a loop, a point, widths from 1 to 24, both caps, both
 * joins and the miter limit, a degenerate or absurd input; that a translucent stroke is blended once everywhere (every pixel inside is the same blend, joints included, and no
 * pixel is darker anywhere) and that an edge pixel is partly covered and a far pixel is untouched; clicks, presses, drags, hover and the cursor only on the stroke (and what is under the
 * box gets the rest); a click that goes to a box above it; a rotated or scaled ancestor and an overflow clip (pixels and pointer); and a view whose cable moves between frames, against
 * a neighbour that must not be built or painted again. Needs a display, at scale 1.
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
            if (failures++ < 50) {                                                     \
                printf("FAIL %s:%d: ", __FILE__, __LINE__);                            \
                printf(__VA_ARGS__);                                                   \
                printf("\n");                                                          \
            }                                                                          \
        }                                                                              \
    } while (0)

static gp_app *app;
static gp_co *co_;
static gp_window *win(void) { return app->windows[0]; }

#define BGHEX 0x101018
#define RED gp_rgb(0xff0000)
#define GREEN gp_rgb(0x00ff00)
#define BLUE gp_rgb(0x4060ff)

typedef struct { int clicks, downs, drags, ups; int last_id; gp_point last_local; } State;
static State st;
static gp_entity cable_a, cable_b;   /* two cached views holding one cable each */
static gp_point g_move_a[4] = { { 40, 560 }, { 90, 500 }, { 140, 620 }, { 190, 560 } };
static gp_point g_move_b[4] = { { 260, 560 }, { 300, 500 }, { 340, 620 }, { 380, 560 } };
static int renders_a, renders_b;

static void on_click(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) { st.clicks++; st.last_id = (int)(intptr_t)data; st.last_local = ev->local; }
static void on_down(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) { st.downs++; st.last_id = (int)(intptr_t)data; }
static void on_drag(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) { st.drags++; }
static void on_up(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) { st.ups++; }

static gp_node *view_a(void *self, gp_ctx *cx) { renders_a++; return gp_curve(g_move_a[0], g_move_a[1], g_move_a[2], g_move_a[3], gp_stroke_width(8), gp_stroke_color(RED)); }
static gp_node *view_b(void *self, gp_ctx *cx) { renders_b++; return gp_curve(g_move_b[0], g_move_b[1], g_move_b[2], g_move_b[3], gp_stroke_width(8), gp_stroke_color(GREEN)); }

static const gp_point zig[] = { { 640, 20 }, { 680, 90 }, { 710, 25 }, { 728, 95 }, { 750, 20 }, { 760, 80 } };
static const gp_point wl1[] = { { 470, 390 }, { 470, 470 } }, wl2[] = { { 490, 390 }, { 490, 470 } }, wl3[] = { { 510, 390 }, { 510, 470 } }, wl5[] = { { 540, 390 }, { 540, 470 } }, wl24[] = { { 580, 390 }, { 580, 470 } };
static const gp_point opl[] = { { 10, 30 }, { 90, 30 } }, hvl[] = { { 620, 420 }, { 740, 420 } };
static const gp_point rev[] = { { 440, 60 }, { 500, 60 }, { 440, 60 } };
static const gp_point elbow[] = { { 450.5f, 100.5f }, { 490.5f, 100.5f }, { 490.5f, 140.5f } };
static const gp_point negw[] = { { 560, 520 }, { 640, 520 } }, zerow[] = { { 560, 560 }, { 640, 560 } };
static const gp_point dot_b[] = { { 600, 290 } }, dot_r[] = { { 650, 290 } };
static const gp_point hline[] = { { 600, 130.25f }, { 700, 130.25f } };
static const gp_point vee[] = { { 200, 110 }, { 240, 40 }, { 280, 110 } };
static const gp_point spike[] = { { 330, 30 }, { 400, 60 }, { 330, 90 } };   /* an angle of about 46 degrees; with a limit of 1.2 a miter falls back to round */
static const gp_point none1[] = { { 520, 40 } };
static const gp_point same2[] = { { 560, 40 }, { 560, 40 } };
static const gp_point bad[] = { { 600, 60 }, { NAN, 3 }, { 620, 90 } };

static const gp_point lit0[] = { { 20, 170 }, { 60, 150 }, { 100, 190 }, { 140, 150 }, { 220, 190 } };
static const gp_point lit1[] = { { 360, 270 }, { 440, 270 } };
static const gp_point lit2[] = { { 460, 270 }, { 540, 270 } };
static const gp_point lit3[] = { { 20, 330 }, { 140, 330 } };
static const gp_point lit4[] = { { 10, 30 }, { 90, 30 } };
static const gp_point lit5[] = { { 20, 30 }, { 80, 30 } };
static const gp_point lit6[] = { { -20, 30 }, { 160, 30 } };

static gp_node *render(void *self, gp_ctx *cx) {
    return gp_div(
        gp_size_full(), gp_bg(gp_rgb(BGHEX)),
        /* row 1 (y 0..120): a line with round caps, one with butt caps, an S, a loop */
        gp_curve(gp_xy(20, 20), gp_xy(60, 20), gp_xy(140, 20), gp_xy(180, 100), gp_stroke_width(12), gp_stroke_color(RED)),   /* 0: S, round */
        gp_curve(gp_xy(20, 20), gp_xy(20, 20), gp_xy(20, 20), gp_xy(20, 20), gp_stroke_width(1), gp_stroke_color(RED)),       /* 1: a point 1 px wide (at 20,20: overdrawn by 0, a dot) */
        gp_polyline(zig, 6, gp_stroke_width(5), gp_stroke_color(BLUE), gp_stroke_cap_butt()),                    /* 2: zig-zag, round joins, butt ends */
        gp_polyline(vee, 3, gp_stroke_width(14), gp_stroke_color(RED), gp_stroke_join_miter()),                             /* 3: V, miter */
        gp_polyline(spike, 3, gp_stroke_width(14), gp_stroke_color(RED), gp_stroke_join_miter(), gp_miter_limit(1.2f)),     /* 4: the same, the miter too long: round */
        gp_polyline(none1, 1, gp_stroke_width(10), gp_stroke_color(GREEN)),                                                 /* 5: one point: a dot */
        gp_polyline(same2, 2, gp_stroke_width(10), gp_stroke_color(GREEN), gp_stroke_cap_butt()),                           /* 6: a zero-length polyline with flat caps: nothing */
        gp_polyline(hline, 2, gp_stroke_width(4), gp_stroke_color(RED), gp_stroke_cap_butt()),   /* 7b: a straight edge between pixel rows */
        gp_polyline(wl1, 2, gp_stroke_width(1), gp_stroke_color(RED)), gp_polyline(wl2, 2, gp_stroke_width(2), gp_stroke_color(RED)), gp_polyline(wl3, 2, gp_stroke_width(3), gp_stroke_color(RED)),
        gp_polyline(wl5, 2, gp_stroke_width(5), gp_stroke_color(RED)), gp_polyline(wl24, 2, gp_stroke_width(24), gp_stroke_color(RED)),
        gp_div(gp_absolute(), gp_left(620), gp_top(330), gp_w(100), gp_h(60), gp_opacity(0.5f), gp_polyline(opl, 2, gp_stroke_width(20), gp_stroke_color(RED))),
        gp_polyline(hvl, 2, gp_stroke_width(10), gp_stroke_color(RED), gp_hover_style(gp_group(gp_stroke_color(GREEN)))),
        gp_polyline(rev, 3, gp_stroke_width(16), gp_stroke_color(gp_rgba(0xff8000a0))),                                       /* 7c: a turn right back on itself, translucent */
        gp_polyline(elbow, 3, gp_stroke_width(16), gp_stroke_color(gp_rgba(0xff8000a0))),   /* 7d: pixel centers exactly on the plane that divides the two pieces */
        gp_polyline(negw, 2, gp_stroke_width(-8), gp_stroke_color(RED), gp_on_click_data(on_click, (void *)(intptr_t)13)),     /* a width below zero is no width */
        gp_polyline(zerow, 2, gp_stroke_width(0), gp_stroke_color(RED), gp_on_click_data(on_click, (void *)(intptr_t)14)),
        gp_curve(gp_xy(560, 600), gp_xy(NAN, 600), gp_xy(620, 600), gp_xy(660, 600), gp_stroke_width(10), gp_stroke_color(RED)),   /* a Bezier with a point that is not a number is nothing */
        gp_polyline(dot_b, 1, gp_stroke_width(20), gp_stroke_color(RED), gp_stroke_cap_butt(), gp_on_click_data(on_click, (void *)(intptr_t)11)),
        gp_polyline(dot_r, 1, gp_stroke_width(20), gp_stroke_color(RED), gp_on_click_data(on_click, (void *)(intptr_t)12)),
        gp_polyline(bad, 3, gp_stroke_width(6), gp_stroke_color(RED)),                                                      /* 7: a point that is not a number ends it */
        /* row 2 (y 140..): translucent cable over a box; widths; the loop */
        gp_div(gp_absolute(), gp_left(20), gp_top(150), gp_w(200), gp_h(40), gp_bg(gp_rgb(0x2040c0))),                      /* 8: a box */
        gp_polyline(lit0, 5, gp_stroke_width(16), gp_stroke_color(gp_rgba(0xff8000a0))),   /* 9: translucent over the box */
        gp_curve(gp_xy(260, 200), gp_xy(380, 120), gp_xy(220, 120), gp_xy(260, 200), gp_stroke_width(6), gp_stroke_color(GREEN)),   /* 10: a loop: p0 == p3 */
        gp_curve(gp_xy(400, 150), gp_xy(460, 150), gp_xy(500, 210), gp_xy(560, 210), gp_stroke_width(24), gp_stroke_color(BLUE), gp_stroke_cap_butt()),   /* 11: wide, flat */
        /* row 3 (y 230..): hit testing */
        gp_div(gp_absolute(), gp_left(20), gp_top(240), gp_w(120), gp_h(60), gp_bg(gp_rgb(0x602020)), gp_on_click_data(on_click, (void *)(intptr_t)1)),   /* 12: a box under the cable's bounding box */
        gp_curve(gp_xy(20, 240), gp_xy(60, 240), gp_xy(100, 300), gp_xy(140, 300), gp_stroke_width(10), gp_stroke_color(RED),
                 gp_on_click_data(on_click, (void *)(intptr_t)2), gp_on_mouse_down(on_down), gp_on_drag(on_drag), gp_on_mouse_up(on_up), gp_hover_bg(GREEN), gp_cursor_pointer()),   /* 13: the clickable cable */
        gp_curve(gp_xy(200, 240), gp_xy(240, 240), gp_xy(280, 300), gp_xy(320, 300), gp_stroke_width(4), gp_stroke_color(RED), gp_hit_tolerance(8),
                 gp_on_click_data(on_click, (void *)(intptr_t)3)),   /* 14: thin, with a generous tolerance */
        gp_polyline(lit1, 2, gp_stroke_width(12), gp_stroke_color(RED), gp_stroke_cap_butt(), gp_on_click_data(on_click, (void *)(intptr_t)4)),   /* 15: flat ends */
        gp_polyline(lit2, 2, gp_stroke_width(12), gp_stroke_color(RED), gp_on_click_data(on_click, (void *)(intptr_t)5)),   /* 16: round ends */
        /* a box above a cable (later in the tree): the click goes to the box */
        gp_polyline(lit3, 2, gp_stroke_width(14), gp_stroke_color(RED), gp_on_click_data(on_click, (void *)(intptr_t)6)),   /* 17 */
        gp_div(gp_absolute(), gp_left(60), gp_top(320), gp_w(30), gp_h(30), gp_bg(gp_rgb(0x808080)), gp_on_click_data(on_click, (void *)(intptr_t)7)),   /* 18 */
        /* row 4: a rotated and a scaled ancestor, an overflow clip */
        gp_div(gp_absolute(), gp_left(200), gp_top(320), gp_w(100), gp_h(60), gp_rotate(90),
               gp_polyline(lit4, 2, gp_stroke_width(10), gp_stroke_color(RED), gp_on_click_data(on_click, (void *)(intptr_t)8))),   /* 19: turned a quarter */
        gp_div(gp_absolute(), gp_left(340), gp_top(320), gp_w(100), gp_h(60), gp_scale(2),
               gp_polyline(lit5, 2, gp_stroke_width(6), gp_stroke_color(GREEN), gp_on_click_data(on_click, (void *)(intptr_t)9))),   /* 20: twice as big */
        gp_div(gp_absolute(), gp_left(20), gp_top(400), gp_w(120), gp_h(60), gp_overflow_hidden(),
               gp_polyline(lit6, 2, gp_stroke_width(20), gp_stroke_color(RED), gp_on_click_data(on_click, (void *)(intptr_t)10))),   /* 21: cut by its parent */
        /* row 5: two cached views with a cable each */
        gp_child_view_cached(cable_a),
        gp_child_view_cached(cable_b));
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
static const uint8_t *px(const uint8_t *f, int x, int y) { return f + ((size_t)y * (size_t)app->capture_w + (size_t)x) * 3; }
/* how much of the stroke's color covers the pixel, from the red channel (the colors used here are red, or a color with red 0) */
static float red_cov(const uint8_t *p) { return fminf(fmaxf(((float)p[0] - 16.f) / 239.f, 0.f), 1.f); }
static float green_cov(const uint8_t *p) { return fminf(fmaxf(((float)p[1] - 16.f) / 239.f, 0.f), 1.f); }
static float blue_cov(const uint8_t *p) { return fminf(fmaxf(((float)p[2] - 24.f) / (255.f - 24.f), 0.f), 1.f); }

/* ---- oracles */

static float seg_dist(float x, float y, float ax, float ay, float bx, float by, float *t_out) {
    float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy, t = l2 > 0 ? ((x - ax) * dx + (y - ay) * dy) / l2 : 0;
    if (t_out) *t_out = t;
    t = fminf(fmaxf(t, 0.f), 1.f);
    return hypotf(x - (ax + t * dx), y - (ay + t * dy));
}
typedef struct { const gp_point *p; int n; float hw; bool butt; bool miter; float limit; } Stroke;

/* is the point on the stroke? round or flat caps; round joins, or the miter as a polygon written out (not the shader's formula) */
static bool on_stroke(const Stroke *s, float x, float y) {
    for (int i = 0; i + 1 < s->n; i++) {
        float t, d = seg_dist(x, y, s->p[i].x, s->p[i].y, s->p[i + 1].x, s->p[i + 1].y, &t);
        if (s->butt && (t < 0 || t > 1)) continue;   /* a flat stroke is only its slabs, and the wedge of the joints (below) */
        if (d <= s->hw) return true;
    }
    if (s->butt) {   /* round joins of a flat-ended stroke: the disc at a vertex, on the outer side only (ahead of the way in, behind the way out) */
        for (int i = 1; i + 1 < s->n; i++) {
            gp_point a = s->p[i - 1], v = s->p[i], b = s->p[i + 1];
            float d1x = v.x - a.x, d1y = v.y - a.y, d2x = b.x - v.x, d2y = b.y - v.y;
            float px_ = x - v.x, py_ = y - v.y;
            if (hypotf(px_, py_) <= s->hw && px_ * d1x + py_ * d1y >= 0 && px_ * d2x + py_ * d2y <= 0) return true;
        }
    }
    if (s->miter) {   /* the outer wedge at each joint: v, v + hw n1, the tip, v + hw n2 (n1, n2 on the outer side) */
        for (int i = 1; i + 1 < s->n; i++) {
            gp_point a = s->p[i - 1], v = s->p[i], b = s->p[i + 1];
            float d1x = v.x - a.x, d1y = v.y - a.y, l1 = hypotf(d1x, d1y), d2x = b.x - v.x, d2y = b.y - v.y, l2 = hypotf(d2x, d2y);
            d1x /= l1; d1y /= l1; d2x /= l2; d2y /= l2;
            float cross = d1x * d2y - d1y * d2x;
            float sgn = cross > 0 ? -1.f : 1.f;   /* the outer side is opposite the turn */
            gp_point n1 = { -d1y * sgn, d1x * sgn }, n2 = { -d2y * sgn, d2x * sgn };
            float mx = n1.x + n2.x, my = n1.y + n2.y, ml = hypotf(mx, my);
            if (ml < 1e-3f) continue;
            float cosh_ = (n1.x * mx + n1.y * my) / ml;   /* cos of half the angle between the normals */
            if (1.f / cosh_ > s->limit) continue;       /* too long a miter: round (nothing more than the discs already tested) */
            gp_point tip = { v.x + mx / ml * s->hw / cosh_, v.y + my / ml * s->hw / cosh_ };
            gp_point q0 = v, q1 = { v.x + n1.x * s->hw, v.y + n1.y * s->hw }, q2 = tip, q3 = { v.x + n2.x * s->hw, v.y + n2.y * s->hw };
            gp_point poly[4] = { q0, q1, q2, q3 };
            bool in = false;
            for (int k = 0, j = 3; k < 4; j = k++)
                if ((poly[k].y > y) != (poly[j].y > y) && x < (poly[j].x - poly[k].x) * (y - poly[k].y) / (poly[j].y - poly[k].y) + poly[k].x) in = !in;
            if (in) return true;
        }
    }
    if (s->n == 1 && !s->butt) return hypotf(x - s->p[0].x, y - s->p[0].y) <= s->hw;
    return false;
}

static float stroke_cov(const Stroke *s, int x, int y) {
    /* far from the centerline: nothing; well inside a round-capped, round-joined stroke: all (the supersampling is for the edges) */
    float dmin = INFINITY, cx = (float)x + 0.5f, cy = (float)y + 0.5f;
    for (int i = 0; i + 1 < s->n; i++) dmin = fminf(dmin, seg_dist(cx, cy, s->p[i].x, s->p[i].y, s->p[i + 1].x, s->p[i + 1].y, NULL));
    if (s->n == 1) dmin = hypotf(cx - s->p[0].x, cy - s->p[0].y);
    if (dmin > s->hw * (s->miter ? fmaxf(1.f, s->limit) : 1.f) + 1.5f) return 0.f;
    if (!s->butt && !s->miter && dmin < s->hw - 1.5f) return 1.f;
    int in = 0;
    for (int j = 0; j < 8; j++)
        for (int i = 0; i < 8; i++) in += on_stroke(s, (float)x + ((float)i + 0.5f) / 8.f, (float)y + ((float)j + 0.5f) / 8.f);
    return (float)in / 64.f;
}

/* the dense polyline of a Bezier, to be the oracle of its stroke */
static int bezier_dense(const gp_point *c, gp_point *out, int n) {
    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)(n - 1), u = 1 - t;
        out[i] = (gp_point){ u * u * u * c[0].x + 3 * u * u * t * c[1].x + 3 * u * t * t * c[2].x + t * t * t * c[3].x,
                             u * u * u * c[0].y + 3 * u * u * t * c[1].y + 3 * u * t * t * c[2].y + t * t * t * c[3].y };
    }
    return n;
}

/* the flattening the library makes of a Bezier at a quarter of a pixel (what is drawn is a stroke of this polyline) */
static int flat_of(const gp_point *c, bool butt, gp_point *out, int cap) {
    static gp_el el;
    memset(&el, 0, sizeof el);
    el.kind = GP_EL_CURVE;
    el.curve_pts = (gp_point *)c;
    el.curve_n = 4;
    el.style.stroke_cap = butt ? 1 : 0;
    gp_arena a = { .chunk_size = 4096 };
    gp_point *pts;
    size_t m = gp_curve_flatten(&el, 0.25f, &a, &pts);
    if (m > (size_t)cap) m = (size_t)cap;
    memcpy(out, pts, m * sizeof *out);
    gp_arena_free(&a);
    return (int)m;
}

typedef float (*cov_fn)(const uint8_t *);

/* compare the pixels of a region with the oracle: how many differ by more than `tol`, and the worst */
static int region_diff(const uint8_t *f, const Stroke *s, cov_fn cf, int x0, int y0, int x1, int y1, float tol, float *worst) {
    int bad = 0;
    float w = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float e = fabsf(stroke_cov(s, x, y) - cf(px(f, x, y)));
            if (e > w) w = e;
            if (e > tol && !bad++ && getenv("CURVE_DEBUG")) printf("  first bad pixel (%d, %d): oracle %.2f, got %.2f\n", x, y, (double)stroke_cov(s, x, y), (double)cf(px(f, x, y)));
            else if (e > tol && getenv("CURVE_DEBUG") && bad < 12) printf("  bad (%d, %d): oracle %.2f got %.2f\n", x, y, (double)stroke_cov(s, x, y), (double)cf(px(f, x, y)));
        }
    if (worst) *worst = w;
    return bad;
}

/* a Bezier's stroke against (1) the exact stroke of the polyline the library flattens it to, to within .14 of coverage, and (2) that polyline within a quarter pixel of the curve
 * (and the stroke of the dense sampling of the true curve within a third of coverage of what is drawn) */
static void check_bezier(const char *what, const uint8_t *f, const gp_point *c, float hw, bool butt, cov_fn cf, int x0, int y0, int x1, int y1) {
    gp_point flat[600], dense[600];
    int m = flat_of(c, butt, flat, 600);
    bezier_dense(c, dense, 600);
    float worst;
    Stroke s = { flat, m, hw, butt, false, 0 };
    int bad = region_diff(f, &s, cf, x0, y0, x1, y1, 0.14f, &worst);
    CHECK(bad == 0, "%s: %d pixels differ from the exact stroke of its %d-piece flattening by more than .14 (worst %.2f)", what, bad, m - 1, (double)worst);
    float dev = 0;
    for (int i = 0; i < 600; i++) {
        float d = INFINITY;
        for (int j = 0; j + 1 < m; j++) d = fminf(d, seg_dist(dense[i].x, dense[i].y, flat[j].x, flat[j].y, flat[j + 1].x, flat[j + 1].y, NULL));
        if (d > dev) dev = d;
    }
    CHECK(dev <= 0.26f, "%s: the flattening strays %.3f px from the curve (more than a quarter pixel)", what, (double)dev);
    Stroke sd = { dense, 600, hw, butt, false, 0 };
    bad = region_diff(f, &sd, cf, x0, y0, x1, y1, 0.34f, &worst);
    CHECK(bad == 0, "%s: %d pixels differ from the stroke of the true curve by more than a third of coverage (worst %.2f)", what, bad, (double)worst);
}

/* is there a curve element in the tree whose box is w x h? */
static bool has_box(const gp_el *n, float w, float h) {
    if (n->kind == GP_EL_CURVE && fabsf(n->size.w - w) < 0.01f && fabsf(n->size.h - h) < 0.01f) return true;
    for (uint32_t i = 0; i < n->nchildren; i++)
        if (has_box(n->children[i], w, h)) return true;
    return false;
}

static void click_at(float x, float y) {
    gp_simulate_mouse(app, GP_SIM_MOVE, x, y, 0, 0);
    gp_simulate_mouse(app, GP_SIM_DOWN, x, y, 1, 0);
    gp_simulate_mouse(app, GP_SIM_UP, x, y, 1, 0);
    gp_sleep(co_, 0.03);
}
static void away(void) { gp_simulate_mouse(app, GP_SIM_MOVE, 635, 635, 0, 0); gp_sleep(co_, 0.03); }

static void run(void) {
    wait_draws(2);
    away();
    uint8_t *f = frame();
    CHECK(f != NULL, "a frame");
    if (!f) return;
    float worst;
    int bad;
    if (getenv("CURVE_DUMP")) {
        FILE *fp = fopen(getenv("CURVE_DUMP"), "wb");
        if (fp) {
            fprintf(fp, "P6\n%d %d\n255\n", app->capture_w, app->capture_h);
            fwrite(f, 3, (size_t)app->capture_w * (size_t)app->capture_h, fp);
            fclose(fp);
        }
    }

    /* 0: an S curve, round caps, 12 wide */
    {
        gp_point c[4] = { { 20, 20 }, { 60, 20 }, { 140, 20 }, { 180, 100 } };
        check_bezier("the S curve", f, c, 6, false, red_cov, 8, 8, 192, 118);
    }
    /* 2: the zig-zag with flat ends and round joins */
    {
        Stroke s = { zig, 6, 2.5f, true, false, 0 };
        bad = region_diff(f, &s, blue_cov, 20, 10, 165, 105, 0.14f, &worst);
        CHECK(bad == 0, "a polyline with round joins and flat ends: %d pixels differ (worst %.2f)", bad, (double)worst);
    }
    /* 3: a V with a miter join: the exact polygon */
    {
        Stroke s = { vee, 3, 7, false, true, 4 };
        bad = region_diff(f, &s, red_cov, 190, 25, 295, 125, 0.14f, &worst);
        CHECK(bad == 0, "a miter join: %d pixels differ from the polygon (worst %.2f)", bad, (double)worst);
        CHECK(red_cov(px(f, 240, 31)) > 0.9f, "the miter tip is filled (above the vertex at 240, 40: %.2f)", (double)red_cov(px(f, 240, 31)));
    }
    /* 4: the same too sharp for its limit: round: no spike beyond the round join */
    {
        Stroke s = { spike, 3, 7, false, false, 0 };
        bad = region_diff(f, &s, red_cov, 320, 15, 420, 105, 0.14f, &worst);
        CHECK(bad == 0, "a miter past its limit falls back to a round join: %d pixels differ (worst %.2f)", bad, (double)worst);
    }
    /* 5, 6, 7: a dot, nothing, and a point that is not a number */
    {
        Stroke s = { none1, 1, 5, false, false, 0 };
        bad = region_diff(f, &s, green_cov, 510, 25, 536, 56, 0.14f, &worst);
        CHECK(bad == 0, "one point is a dot of the stroke's width: %d pixels differ (worst %.2f)", bad, (double)worst);
        CHECK(green_cov(px(f, 560, 40)) < 0.02f && green_cov(px(f, 555, 40)) < 0.02f, "a zero-length polyline with flat ends draws nothing");
        CHECK(red_cov(px(f, 600, 60)) > 0.9f && red_cov(px(f, 610, 75)) < 0.02f && red_cov(px(f, 620, 90)) < 0.02f, "a point that is not a number ends the stroke there: only the first point is left, a dot (and no crash)");
    }
    /* 9: translucent over a box: one blend everywhere */
    {
        float zz[5][2] = { { 20, 170 }, { 60, 150 }, { 100, 190 }, { 140, 150 }, { 220, 190 } };
        gp_point pts[5];
        for (int i = 0; i < 5; i++) pts[i] = (gp_point){ zz[i][0], zz[i][1] };
        Stroke s = { pts, 5, 8, false, false, 0 };
        int holes = 0, doubled = 0, outside = 0, inside = 0;
        for (int y = 130; y < 210; y++)
            for (int x = 8; x < 240; x++) {
                float d = INFINITY;
                for (int i = 0; i + 1 < 5; i++) d = fminf(d, seg_dist((float)x + 0.5f, (float)y + 0.5f, pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y, NULL));
                bool on_box = x >= 20 && x < 220 && y >= 150 && y < 190;
                const uint8_t *p = px(f, x, y);
                /* the color #ff8000 at alpha 160/255 over the box (0x2040c0) or over the background */
                float a = 160.f / 255.f;
                float bgc[3] = { on_box ? 0x20 : 0x10, on_box ? 0x40 : 0x10, on_box ? 0xc0 : 0x18 };
                float want[3] = { 255 * a + bgc[0] * (1 - a), 128 * a + bgc[1] * (1 - a), 0 * a + bgc[2] * (1 - a) };
                if (d < 7.0f) {   /* well inside the stroke */
                    inside++;
                    for (int c = 0; c < 3; c++) if (fabsf((float)p[c] - want[c]) > 2.5f) { holes++; break; }
                } else if (d > 9.5f) {   /* well outside it: the background, or the box, untouched */
                    outside++;
                    for (int c = 0; c < 3; c++) if (fabsf((float)p[c] - bgc[c]) > 0.5f) { doubled++; break; }
                }
            }
        CHECK(inside > 1500 && holes == 0, "a translucent stroke is the same blend at all %d pixels inside it, joints included (%d differ)", inside, holes);
        CHECK(outside > 5000 && doubled == 0, "and the pixels far from it are untouched (%d of %d differ)", doubled, outside);
        (void)s;
    }
    /* 10: a loop with p0 == p3, and 11: wide with flat caps */
    {
        gp_point c[4] = { { 260, 200 }, { 380, 120 }, { 220, 120 }, { 260, 200 } };
        check_bezier("a loop (p0 == p3)", f, c, 3, false, green_cov, 238, 110, 340, 196);   /* (not the point where the two ends meet and overlap: their anti-aliased edges blend twice there, see the README) */
        gp_point c2[4] = { { 400, 150 }, { 460, 150 }, { 500, 210 }, { 560, 210 } };
        check_bezier("a wide stroke with flat caps", f, c2, 12, true, blue_cov, 385, 135, 575, 225);
    }
    /* anti-aliasing: a horizontal stroke from y 128.25 to 132.25: its edge rows are .75 and .25 covered, the middle ones full, the rows beyond untouched */
    {
        float want[7] = { 0, 0.75f, 1, 1, 1, 0.25f, 0 };
        for (int i = 0; i < 7; i++) {
            float c = red_cov(px(f, 650, 127 + i));
            CHECK(fabsf(c - want[i]) < 0.04f, "row %d of a straight edge is %.2f covered (wanted %.2f)", 127 + i, (double)c, (double)want[i]);
        }
        const uint8_t *far = px(f, 650, 120);
        CHECK(far[0] == 0x10 && far[1] == 0x10 && far[2] == 0x18, "a pixel far from the stroke is the background exactly");
    }

    /* widths from 1 to 24, each against the exact stroke (a line of width 1 on a pixel line is half a pixel each side) */
    {
        const gp_point *ls[5] = { wl1, wl2, wl3, wl5, wl24 };
        float ws[5] = { 1, 2, 3, 5, 24 };
        for (int i = 0; i < 5; i++) {
            Stroke s = { ls[i], 2, ws[i] / 2, false, false, 0 };
            int cx = (int)ls[i][0].x;
            bad = region_diff(f, &s, red_cov, cx - 16, 380, cx + 16, 480, 0.1f, &worst);
            if (i == 4) bad = region_diff(f, &s, red_cov, cx - 16, 380, cx + 16, 480, 0.1f, &worst);
            CHECK(bad == 0, "a stroke %g px wide: %d pixels differ from the exact one (worst %.2f)", (double)ws[i], bad, (double)worst);
        }
    }
    /* opacity of an ancestor: half the color over the background, once */
    CHECK(px(f, 670, 360)[0] >= 132 && px(f, 670, 360)[0] <= 139 && px(f, 670, 360)[1] >= 6 && px(f, 670, 360)[1] <= 10, "under an ancestor at half opacity the stroke is half its color (%d, %d, %d)", px(f, 670, 360)[0], px(f, 670, 360)[1], px(f, 670, 360)[2]);

    /* a polyline that goes back along itself, translucent: one blend over all of it (the cut at the turn is square), and the round end past the turn */
    {
        int inside = 0, wrong = 0;
        for (int y = 50; y < 71; y++)
            for (int x = 430; x < 515; x++) {
                float d = fabsf((float)y + 0.5f - 60.f);
                bool on_line = x >= 441 && x <= 499, in_cap = (float)x + 0.5f > 500.f && hypotf((float)x + 0.5f - 500.f, d) < 7.f;
                const uint8_t *p = px(f, x, y);
                if ((on_line && d < 7.f) || in_cap) {
                    inside++;
                    float a = 160.f / 255.f;
                    if (fabsf((float)p[0] - (255 * a + 16 * (1 - a))) > 2.5f || fabsf((float)p[1] - (128 * a + 16 * (1 - a))) > 2.5f) wrong++;
                }
            }
        CHECK(inside > 800 && wrong == 0, "a stroke that doubles back is one blend everywhere (%d of %d pixels are not)", wrong, inside);
    }

    /* an elbow on half-pixel coordinates: the pixels whose centers are exactly on the line between the two pieces belong to one of them, not both and not neither */
    {
        int checked = 0, wrong = 0;
        float a = 160.f / 255.f;
        for (int t = -5; t <= 5; t++) {
            const uint8_t *p = px(f, 490 + t, 100 - t);
            checked++;
            if (fabsf((float)p[0] - (255 * a + 16 * (1 - a))) > 2.5f || fabsf((float)p[1] - (128 * a + 16 * (1 - a))) > 2.5f) { wrong++; if (getenv("CURVE_DEBUG")) printf("  t=%d: %d %d %d\n", t, p[0], p[1], p[2]); }
        }
        CHECK(wrong == 0, "on the line between two pieces each pixel is blended once (%d of %d are not)", wrong, checked);
    }

    CHECK(red_cov(px(f, 560, 600)) < 0.02f && red_cov(px(f, 620, 600)) < 0.02f && red_cov(px(f, 660, 600)) < 0.02f, "a Bezier with a point that is not a number is not drawn at all (not as the points that are left)");
    /* no width, no stroke, nothing to click */
    CHECK(red_cov(px(f, 600, 520)) < 0.02f && red_cov(px(f, 600, 560)) < 0.02f, "a width of zero or less draws nothing");
    memset(&st, 0, sizeof st);
    click_at(600, 520);
    click_at(600, 560);
    CHECK(st.clicks == 0, "and cannot be clicked (%d)", st.clicks);

    /* the box of a curve is the bounding box of its points padded by half the width, plus the hit tolerance, plus more for a miter join (up to the limit times the half width) */
    CHECK(has_box(win()->tree, 160 + 12, 80 + 12), "the S curve's box: its points (all four, 160 x 80) and 6 px on every side");
    CHECK(has_box(win()->tree, 120 + 2 * 10, 60 + 2 * 10), "a thin curve with a tolerance of 8: half its width (2) and the tolerance on every side (%s)", "140 x 80");
    CHECK(has_box(win()->tree, 80 + 2 * 28, 70 + 2 * 28), "a miter join reserves up to the limit (4) times the half width (7) all round (136 x 126)");
    CHECK(has_box(win()->tree, 0 + 2 * 5, 0 + 2 * 5), "a polyline of one point is the dot's box (10 x 10)");

    /* ---- hit testing */
    memset(&st, 0, sizeof st);
    click_at(80, 270);   /* on the clickable cable (the middle of its S: (80, 270)) */
    CHECK(st.clicks == 1 && st.last_id == 2, "a click on the stroke registers on the cable (clicks %d, id %d)", st.clicks, st.last_id);
    CHECK(st.downs == 1 && st.ups == 1, "a press and a release registered (%d, %d)", st.downs, st.ups);
    st.clicks = 0;
    click_at(50, 285);   /* inside the cable's box, off the stroke, over the box below: the box gets it */
    CHECK(st.clicks == 1 && st.last_id == 1, "a click inside the cable's box but off the stroke reaches the box under it (clicks %d, id %d)", st.clicks, st.last_id);
    st.clicks = 0;
    click_at(30, 250);   /* inside the cable's box, over the box below, near the start: the stroke is at (20..40, 240..) hmm: this is on the cable? */
    st.clicks = 0;
    click_at(130, 240);   /* off the cable, inside its box, above the box's top (the box is at y 240..300): over the box */
    CHECK(st.clicks == 1 && st.last_id == 1, "another point in the box that is off the stroke goes to what is under it (id %d)", st.last_id);
    /* the tolerance: the thin cable is 4 wide with a tolerance of 8: a point 9 px from the center line registers, 11 does not */
    st.clicks = 0;
    click_at(260, 270 - 9);   /* (the curve slopes 3:4 there: the distance across is 0.8 of the distance up) */
    {
        /* the cable at x = 260 is near y = 270 (the middle of its curve: (260, 270)) */
        CHECK(st.clicks == 1 && st.last_id == 3, "a point 9 px above a thin stroke (7 across) with a tolerance of 8 registers (clicks %d)", st.clicks);
    }
    st.clicks = 0;
    click_at(260, 270 - 14);
    CHECK(st.clicks == 0, "and 14 px above it (11 across, past half the width and the tolerance, 10) does not (%d)", st.clicks);
    /* the tolerance reaches past the end of a thin stroke, outside the box a bare stroke would have */
    st.clicks = 0;
    click_at(195, 240);   /* 5 px before the start (200, 240) of a 4 px wide stroke with a tolerance of 8 */
    CHECK(st.clicks == 1 && st.last_id == 3, "the tolerance reaches past the end of the stroke (%d)", st.clicks);
    /* flat ends do not reach past the end, round ones do */
    st.clicks = 0;
    click_at(355, 270);   /* 5 px before the flat end (360) */
    CHECK(st.clicks == 0, "beyond a flat end is not on the stroke (%d)", st.clicks);
    click_at(445, 270);
    CHECK(st.clicks == 0, "beyond the other flat end either");
    click_at(400, 270);
    CHECK(st.clicks == 1 && st.last_id == 4, "on a flat-ended stroke it registers (%d)", st.last_id);
    st.clicks = 0;
    click_at(457, 270);   /* 3 px before the round end (460): within the half width (6) of the end point */
    CHECK(st.clicks == 1 && st.last_id == 5, "within the radius of a round end it registers (%d)", st.last_id);
    st.clicks = 0;
    click_at(468 + 20, 270);
    st.clicks = 0;
    /* one point: a dot if the ends are round, nothing to click if they are flat */
    st.clicks = 0;
    click_at(600, 290);
    CHECK(st.clicks == 0, "a one-point polyline with flat ends is nothing to click (%d)", st.clicks);
    click_at(650, 290);
    CHECK(st.clicks == 1 && st.last_id == 12, "and with round ends it is a dot that takes the click (%d)", st.clicks);
    st.clicks = 0;
    /* z-order: a box later in the tree is above the cable, the click goes to the box */
    click_at(70, 330);
    CHECK(st.clicks == 1 && st.last_id == 7, "a box above the cable takes the click (id %d)", st.last_id);
    st.clicks = 0;
    click_at(110, 330);
    CHECK(st.clicks == 1 && st.last_id == 6, "and the cable takes the click where the box is not (id %d)", st.last_id);

    /* drag: a press on the stroke, a move, a release */
    st.drags = st.ups = 0;
    gp_simulate_mouse(app, GP_SIM_MOVE, 80, 270, 0, 0);
    gp_simulate_mouse(app, GP_SIM_DOWN, 80, 270, 1, 0);
    gp_simulate_mouse(app, GP_SIM_MOVE, 120, 280, 0, 0);
    gp_simulate_mouse(app, GP_SIM_MOVE, 160, 290, 0, 0);
    gp_simulate_mouse(app, GP_SIM_UP, 160, 290, 1, 0);
    gp_sleep(co_, 0.03);
    CHECK(st.drags >= 2 && st.ups == 1, "a drag started on the stroke reports its moves (%d) and its end (%d)", st.drags, st.ups);
    st.downs = st.drags = 0;
    gp_simulate_mouse(app, GP_SIM_MOVE, 50, 285, 0, 0);
    gp_simulate_mouse(app, GP_SIM_DOWN, 50, 285, 1, 0);
    gp_simulate_mouse(app, GP_SIM_MOVE, 90, 285, 0, 0);
    gp_simulate_mouse(app, GP_SIM_UP, 90, 285, 1, 0);
    gp_sleep(co_, 0.03);
    CHECK(st.downs == 0 && st.drags == 0, "a press beside the stroke is not the cable's (%d, %d)", st.downs, st.drags);

    /* hover and cursor: only on the stroke */
    gp_simulate_mouse(app, GP_SIM_MOVE, 80, 270, 0, 0);
    gp_sleep(co_, 0.05);
    f = frame();
    CHECK(f && green_cov(px(f, 80, 270)) > 0.9f && red_cov(px(f, 80, 270)) < 0.1f, "hovered: the cable is green");
    CHECK(win()->cursor == GP_CURSOR_POINTER, "the pointer cursor is on the stroke (%d)", win()->cursor);
    gp_simulate_mouse(app, GP_SIM_MOVE, 50, 285, 0, 0);
    gp_sleep(co_, 0.05);
    f = frame();
    CHECK(f && green_cov(px(f, 80, 270)) < 0.1f && red_cov(px(f, 80, 270)) > 0.9f, "the pointer in the box but off the stroke: not hovered (red again)");
    CHECK(win()->cursor == GP_CURSOR_DEFAULT, "and the cursor is the arrow (%d)", win()->cursor);
    away();

    /* a hover style that changes the stroke color: only when the pointer is on the stroke */
    gp_simulate_mouse(app, GP_SIM_MOVE, 680, 420, 0, 0);
    gp_sleep(co_, 0.05);
    f = frame();
    CHECK(f && green_cov(px(f, 680, 420)) > 0.9f && red_cov(px(f, 680, 420)) < 0.1f, "gp_hover_style(gp_stroke_color(..)) changes the color under the pointer");
    gp_simulate_mouse(app, GP_SIM_MOVE, 680, 431, 0, 0);   /* 11 px beside a 10 px stroke: off it */
    gp_sleep(co_, 0.05);
    f = frame();
    CHECK(f && red_cov(px(f, 680, 420)) > 0.9f, "and not beside it");
    away();

    /* ---- an ancestor's transform: pixels and pointer */
    f = frame();
    /* 19: a 100 x 60 box at (200,320), turned a quarter about its middle (250, 350): the horizontal stroke (10..90, y 30) becomes vertical at x 250.. (from y 310 to y 390) */
    CHECK(f && red_cov(px(f, 250, 320)) > 0.9f && red_cov(px(f, 250, 380)) > 0.9f && red_cov(px(f, 230, 350)) < 0.05f, "a stroke under a turned ancestor is turned: vertical at x = 250");
    st.clicks = 0;
    click_at(250, 330);
    CHECK(st.clicks == 1 && st.last_id == 8, "a click lands on the turned stroke (%d)", st.clicks);
    st.clicks = 0;
    click_at(232, 350);   /* where the unturned stroke's middle would have been (x 210..290 at y 350): off the turned stroke */
    CHECK(st.clicks == 0, "and not where it was (%d)", st.clicks);
    /* 20: scaled by 2 about the middle of its box (390, 350): the stroke from (20,30) to (80,30), 6 wide, is a stroke from (340+..): x 370-... */
    {
        float cx = 390, cy = 350;   /* the box's middle */
        float x0 = cx + (340 + 20 - cx) * 2, x1 = cx + (340 + 80 - cx) * 2, yy = cy + (350 - 20 - cy) * 0 + (320 + 30 - cy) * 2;   /* y: the stroke at layout y = 320 + 30 */
        (void)yy;
        float ly = cy + (350 - cy) * 2;   /* layout y 350 -> 350 */
        CHECK(f && green_cov(px(f, (int)((x0 + x1) / 2), (int)ly)) > 0.9f, "a stroke under a scaled ancestor is scaled (green at the middle)");
        int thick = 0;
        for (int y = (int)ly - 12; y < (int)ly + 12; y++) thick += green_cov(px(f, (int)((x0 + x1) / 2), y)) > 0.5f;
        CHECK(thick >= 11 && thick <= 13, "twice as thick: 6 became 12 (%d px)", thick);
        st.clicks = 0;
        click_at((x0 + x1) / 2, ly + 5);
        CHECK(st.clicks == 1 && st.last_id == 9, "a click 5 px from the middle of the scaled stroke lands (6 px away: inside its 12 px)");
        st.clicks = 0;
        click_at((x0 + x1) / 2, ly + 9);
        CHECK(st.clicks == 0, "9 px away is off it (%d)", st.clicks);
    }
    /* 21: cut by its parent's overflow: a 120 x 60 box at (20,400): the stroke from -20 to 160 is cut at x 20 and 140 */
    CHECK(f && red_cov(px(f, 80, 430)) > 0.9f && red_cov(px(f, 15, 430)) < 0.02f && red_cov(px(f, 150, 430)) < 0.02f, "an overflow clip above cuts the stroke");
    st.clicks = 0;
    click_at(80, 430);
    CHECK(st.clicks == 1 && st.last_id == 10, "a click on the part inside the clip lands");
    st.clicks = 0;
    click_at(150, 430);
    CHECK(st.clicks == 0, "and on the part that is cut away does not (%d)", st.clicks);

    /* ---- a cable that moves: its view is built and painted again, its neighbour's is not */
    away();
    f = frame();
    CHECK(f && red_cov(px(f, 115, 560)) > 0.9f, "the cable of view A is drawn where its points are (red at (115, 560))");
    int ra = renders_a, rb = renders_b;
    unsigned long hits0 = app->stat_paint_hits, miss0 = app->stat_paint_misses;
    for (int i = 0; i < 4; i++) g_move_a[i].y += 30;
    gp_ctx cx = gp_ctx_for(app, cable_a);
    gp_notify(&cx);
    gp_sleep(co_, 0.08);
    f = frame();
    CHECK(renders_a == ra + 1 && renders_b == rb, "moving one cable renders its view once and no other (A %d -> %d, B %d -> %d)", ra, renders_a, rb, renders_b);
    CHECK(app->stat_paint_hits > hits0, "the neighbouring cable is painted from its kept copy (%lu reuses)", app->stat_paint_hits - hits0);
    CHECK(app->stat_paint_misses - miss0 <= 4, "and little is painted again (%lu)", app->stat_paint_misses - miss0);
    check_bezier("the moved cable", f, g_move_a, 4, false, red_cov, 30, 520, 205, 655);
    check_bezier("the neighbour's cable", f, g_move_b, 4, false, green_cov, 250, 485, 395, 635);
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    run();
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    cable_a = gp_view_new(app, 1, &(char){ 0 }, view_a, NULL);
    cable_b = gp_view_new(app, 1, &(char){ 0 }, view_b, NULL);
    gp_entity v = gp_view_new(app, 1, &(char){ 0 }, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "curve_test", .width = 780, .height = 700 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "curve_test: %d FAILURE(S)\n" : "curve_test: all checks passed\n", failures);
    return failures != 0;
}
