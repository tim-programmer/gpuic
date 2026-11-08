/*
 * Gradients, end to end: every pixel of a gradient box is compared with the color the CSS definition gives (the angle's
 * gradient line, premultiplied interpolation), computed here independently of the renderer, from frames read back from
 * the GL renderer. Also: a fade to transparent over a background, rounded corners and borders, opacity, the hover
 * color replacing a gradient, a gradient in a cached view, and the ramp table filling up (the frame is cleared and
 * painted again). Needs a display.
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
            if (failures++ < 30) {                                                     \
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

#define PAGE gp_rgb(0x14141e)   /* 20 20 30 */
#define RED    gp_rgb(0xf04050)
#define BLUE   gp_rgb(0x3060f0)
#define GREEN  gp_rgb(0x40e070)
#define YELLOW gp_rgb(0xf0d040)
#define HOVER  gp_rgb(0x00ff80)
#define BW 260
#define BH 40

enum { B_LINEAR90, B_UP, B_DIAG3, B_RADIAL, B_RADIAL_OFF, B_HARD, B_FADE, B_ROUND, B_OPACITY, B_ANGLE37, B_HOVER, B_LAST, B_REPEAT, B_REPEAT_OFF, B_REPEAT_RAD, B_CONIC, B_CONIC_FROM, NBOX };

typedef struct { int many; int calls; gp_entity kid; } Fix;   /* many: 0 the boxes, 1 and 2 two sets of 100 gradients that share no ramp */

static gp_node *kid_render(void *self, gp_ctx *cx) {
    return gp_div(gp_w(BW), gp_h(BH), gp_bg_gradient(90, gp_stop(RED, 0), gp_stop(BLUE, 1)));
}

static gp_node *render(void *self, gp_ctx *cx) {
    Fix *f = self;
    if (f->many) {   /* 100 gradients of one row of 3 px each: two sets together are more than the ramp table holds */
        gp_node *rows[100];
        int set = f->many - 1;
        for (int i = 0; i < 100; i++)
            rows[i] = gp_div(gp_w(BW), gp_h(3), gp_flex_shrink_0(),
                             gp_bg_gradient(90, gp_stop(gp_rgb((uint32_t)((i + 40) << 16 | (100 + set * 50) << 8 | 30)), 0), gp_stop(gp_rgb((uint32_t)(10 << 16 | 60 << 8 | (200 - i))), 1)));
        return gp_div(gp_size_full(), gp_flex_col(), gp_bg(PAGE), gp_children(rows, 100));
    }
    return gp_div(
        gp_size_full(), gp_flex_col(), gp_gap(6), gp_p(10), gp_bg(PAGE),
        gp_div(gp_w(BW), gp_h(BH), gp_bg_gradient(90, gp_stop(RED, 0), gp_stop(BLUE, 1))),
        gp_div(gp_w(BW), gp_h(BH), gp_bg_gradient(0, gp_stop(RED, 0), gp_stop(YELLOW, 1))),
        gp_div(gp_w(BW), gp_h(BH), gp_bg_gradient(135, gp_stop(RED, 0), gp_stop(GREEN, 0.4f), gp_stop(BLUE, 1))),
        gp_div(gp_w(BW), gp_h(BH), gp_bg_radial(0.5f, 0.5f, gp_stop(gp_rgb(0xffffff), 0), gp_stop(gp_rgb(0x000000), 1))),
        gp_div(gp_w(BW), gp_h(BH), gp_bg_radial(0.25f, 0.75f, gp_stop(YELLOW, 0), gp_stop(BLUE, 0.5f), gp_stop(RED, 1))),
        gp_div(gp_w(BW), gp_h(BH), gp_bg_gradient(90, gp_stop(RED, 0), gp_stop(RED, 0.3f), gp_stop(GREEN, 0.3f), gp_stop(GREEN, 0.7f), gp_stop(BLUE, 0.7f), gp_stop(BLUE, 1))),
        gp_div(gp_w(BW), gp_h(BH), gp_bg_gradient(90, gp_stop(gp_rgba(0xf0405000), 0), gp_stop(gp_rgba(0x3060f0ff), 1))),
        gp_div(gp_w(BW), gp_h(BH), gp_rounded(16), gp_border(3), gp_border_color(gp_rgb(0xffffff)), gp_bg_gradient(0, gp_stop(GREEN, 0), gp_stop(BLUE, 1))),
        gp_div(gp_w(BW), gp_h(BH), gp_opacity(0.5f), gp_bg_gradient(90, gp_stop(RED, 0), gp_stop(BLUE, 1))),
        gp_div(gp_w(BW), gp_h(BH), gp_bg_gradient(37, gp_stop(YELLOW, 0), gp_stop(BLUE, 1))),
        gp_div(gp_w(BW), gp_h(BH), gp_bg_gradient(90, gp_stop(RED, 0), gp_stop(BLUE, 1)), gp_hover_bg(HOVER)),
        gp_div(gp_w(BW), gp_h(BH), gp_bg_gradient(90, gp_stop(RED, 0), gp_stop(BLUE, 1)), gp_bg(gp_rgb(0x808080))),   /* the later background wins */
        gp_div(gp_w(BW), gp_h(BH), gp_bg_repeating(90, gp_stop(RED, 0), gp_stop(BLUE, 0.25f))),
        gp_div(gp_w(BW), gp_h(BH), gp_bg_repeating(90, gp_stop(YELLOW, 0.1f), gp_stop(GREEN, 0.35f))),
        gp_div(gp_w(BW), gp_h(BH), gp_bg_repeating_radial(0.5f, 0.5f, gp_stop(gp_rgb(0xffffff), 0), gp_stop(gp_rgb(0x000000), 0.1f))),
        gp_div(gp_w(BW), gp_h(BH), gp_bg_conic(0.5f, 0.5f, 0, gp_stop(RED, 0), gp_stop(YELLOW, 0.25f), gp_stop(BLUE, 0.5f), gp_stop(RED, 1))),
        gp_div(gp_w(BW), gp_h(BH), gp_bg_conic(0.3f, 0.6f, 90, gp_stop(GREEN, 0), gp_stop(BLUE, 1))),
        gp_child_view_cached(f->kid));
}

/* ---- the reference: CSS, written out again */

typedef struct { gp_color_stop s[8]; int n; int kind; bool repeat; float angle, cx, cy; } Ref;   /* kind: 0 linear, 1 radial, 2 conic */

static void ref_premult(const Ref *r, float t, float out[4]) {
    const gp_color_stop *s = r->s;
    int n = r->n;
    gp_color c0, c1;
    float u;
    if (t <= s[0].at) { c0 = c1 = s[0].color; u = 0; }
    else if (t >= s[n - 1].at) { c0 = c1 = s[n - 1].color; u = 0; }
    else {
        int k = 1;
        while (t > s[k].at) k++;
        c0 = s[k - 1].color; c1 = s[k].color;
        u = (t - s[k - 1].at) / (s[k].at - s[k - 1].at);
    }
    float a[4] = { c0.r * c0.a, c0.g * c0.a, c0.b * c0.a, c0.a }, b[4] = { c1.r * c1.a, c1.g * c1.a, c1.b * c1.a, c1.a };
    for (int i = 0; i < 4; i++) out[i] = a[i] + (b[i] - a[i]) * u;
}

static float ref_t(const Ref *r, float lx, float ly, float w, float h) {
    if (r->kind == 2) {   /* conic: clockwise from up, around the center */
        float ox = (r->cx - 0.5f) * w, oy = (r->cy - 0.5f) * h;
        float t = (atan2f(lx - ox, -(ly - oy)) - r->angle * (float)M_PI / 180.f) / (2.f * (float)M_PI);
        return t - floorf(t);
    }
    if (r->kind == 0) {
        float a = r->angle * (float)M_PI / 180.f, dx = sinf(a), dy = -cosf(a);
        float len = fabsf(w * dx) + fabsf(h * dy);   /* CSS: the line that reaches the corners */
        return (lx * dx + ly * dy) / len + 0.5f;
    }
    float ox = (r->cx - 0.5f) * w, oy = (r->cy - 0.5f) * h;
    float far = hypotf(fmaxf(r->cx, 1 - r->cx) * w, fmaxf(r->cy, 1 - r->cy) * h);
    return hypotf(lx - ox, ly - oy) / far;
}

typedef struct { int x0, y0, x1, y1; } Box;
static Box box_of(int i) {
    const gp_el *n = win()->tree->children[i];
    float s = win()->scale;
    return (Box){ (int)lroundf(n->pos.x * s), (int)lroundf(n->pos.y * s), (int)lroundf((n->pos.x + n->size.w) * s), (int)lroundf((n->pos.y + n->size.h) * s) };
}

/* the largest difference over the sample points (one in `step` pixels, `inset` away from the edge, skipping `skip_t` near stops) */
static int compare_box(const uint8_t *px, int pw, int i, const Ref *r, float opacity, int inset, const float *avoid, int navoid, int *worst_x, int *worst_y) {
    Box b = box_of(i);
    float w = (float)(b.x1 - b.x0), h = (float)(b.y1 - b.y0);
    int worst = 0;
    static const float bg[3] = { 20, 20, 30 };
    for (int y = b.y0 + inset; y < b.y1 - inset; y += 3)
        for (int x = b.x0 + inset; x < b.x1 - inset; x += 3) {
            float lx = (float)(x - b.x0) + 0.5f - w / 2, ly = (float)(y - b.y0) + 0.5f - h / 2;
            float t = ref_t(r, lx, ly, w, h);
            bool near_stop = false;
            if (r->kind == 2) {   /* not at the seam or the middle, where the angle is not defined */
                float ox = (r->cx - 0.5f) * w, oy = (r->cy - 0.5f) * h;
                if (hypotf(lx - ox, ly - oy) < 5.f || t < 0.012f || t > 0.988f) continue;
            }
            if (r->repeat) {   /* the period's ends are where the pattern jumps; between them it is the stops, as if they were laid again */
                float first = r->s[0].at, span = r->s[r->n - 1].at - first, u = (t - first) / span, f = u - floorf(u);
                if (f < 0.03f || f > 0.97f) continue;
                t = first + f * span;
            }
            for (int k = 0; k < navoid; k++) near_stop |= fabsf(t - avoid[k]) < 4.f / w + 0.004f;
            if (near_stop) continue;
            float c[4];
            ref_premult(r, t < 0 ? 0 : t > 1 ? 1 : t, c);
            const uint8_t *p = px + ((size_t)y * (size_t)pw + (size_t)x) * 3;
            for (int k = 0; k < 3; k++) {
                float want = (c[k] * opacity + bg[k] / 255.f * (1.f - c[3] * opacity)) * 255.f;
                int d = abs((int)lroundf(want) - (int)p[k]);
                if (d > worst) { worst = d; *worst_x = x - b.x0; *worst_y = y - b.y0; }
            }
        }
    return worst;
}

static Ref two(gp_color a, gp_color b, float angle) {
    Ref r = { .n = 2, .angle = angle };
    r.s[0] = (gp_color_stop){ a, 0 };
    r.s[1] = (gp_color_stop){ b, 1 };
    return r;
}

static void run(void) {
    wait_draws(2);
    Fix *fx = gp_entity_get(app, win()->root_view);
    CHECK(win()->tree->nchildren == NBOX + 1, "found %u boxes", win()->tree->nchildren);
    if (win()->tree->nchildren != NBOX + 1) return;
    uint8_t *px = frame();
    CHECK(px != NULL, "no frame");
    if (!px) return;
    int pw = app->capture_w, wx = 0, wy = 0;
    const int tol = 3;

    Ref r = two(RED, BLUE, 90);
    int e = compare_box(px, pw, B_LINEAR90, &r, 1.f, 0, NULL, 0, &wx, &wy);
    CHECK(e <= tol, "90 deg red to blue: off by %d at %d,%d", e, wx, wy);
    {   /* no visible banding: neighbouring pixels differ by at most a couple of levels */
        Box b = box_of(B_LINEAR90);
        int step = 0;
        int y = (b.y0 + b.y1) / 2;
        for (int x = b.x0; x + 1 < b.x1; x++)
            for (int c = 0; c < 3; c++) step = abs((int)px[((size_t)y * (size_t)pw + (size_t)x) * 3 + (size_t)c] - (int)px[((size_t)y * (size_t)pw + (size_t)x + 1) * 3 + (size_t)c]) > step ?
                                             abs((int)px[((size_t)y * (size_t)pw + (size_t)x) * 3 + (size_t)c] - (int)px[((size_t)y * (size_t)pw + (size_t)x + 1) * 3 + (size_t)c]) : step;
        CHECK(step <= 3, "a smooth ramp: the largest step between neighbouring pixels is %d", step);
    }
    r = two(RED, YELLOW, 0);
    e = compare_box(px, pw, B_UP, &r, 1.f, 0, NULL, 0, &wx, &wy);
    CHECK(e <= tol, "0 deg (bottom to top): off by %d at %d,%d", e, wx, wy);
    {   /* and it really points up: red at the bottom, yellow at the top */
        Box b = box_of(B_UP);
        const uint8_t *top = px + ((size_t)(b.y0 + 1) * (size_t)pw + (size_t)(b.x0 + 100)) * 3, *bottom = px + ((size_t)(b.y1 - 2) * (size_t)pw + (size_t)(b.x0 + 100)) * 3;
        CHECK(top[1] > bottom[1] + 40, "0 deg: the top is the end of the gradient (%d vs %d green)", top[1], bottom[1]);
    }
    Ref three = { .n = 3, .angle = 135 };
    three.s[0] = (gp_color_stop){ RED, 0 }; three.s[1] = (gp_color_stop){ GREEN, 0.4f }; three.s[2] = (gp_color_stop){ BLUE, 1 };
    e = compare_box(px, pw, B_DIAG3, &three, 1.f, 0, NULL, 0, &wx, &wy);
    CHECK(e <= tol, "135 deg, three stops: off by %d at %d,%d", e, wx, wy);
    Ref r37 = two(YELLOW, BLUE, 37);
    e = compare_box(px, pw, B_ANGLE37, &r37, 1.f, 0, NULL, 0, &wx, &wy);
    CHECK(e <= tol, "37 deg: off by %d at %d,%d", e, wx, wy);
    Ref rad = { .n = 2, .kind = 1, .cx = 0.5f, .cy = 0.5f };
    rad.s[0] = (gp_color_stop){ gp_rgb(0xffffff), 0 }; rad.s[1] = (gp_color_stop){ gp_rgb(0x000000), 1 };
    e = compare_box(px, pw, B_RADIAL, &rad, 1.f, 0, NULL, 0, &wx, &wy);
    CHECK(e <= tol, "radial from the middle: off by %d at %d,%d", e, wx, wy);
    Ref off = { .n = 3, .kind = 1, .cx = 0.25f, .cy = 0.75f };
    off.s[0] = (gp_color_stop){ YELLOW, 0 }; off.s[1] = (gp_color_stop){ BLUE, 0.5f }; off.s[2] = (gp_color_stop){ RED, 1 };
    e = compare_box(px, pw, B_RADIAL_OFF, &off, 1.f, 0, NULL, 0, &wx, &wy);
    CHECK(e <= tol, "radial off center: off by %d at %d,%d", e, wx, wy);
    Ref hard = { .n = 6, .angle = 90 };
    hard.s[0] = (gp_color_stop){ RED, 0 }; hard.s[1] = (gp_color_stop){ RED, 0.3f }; hard.s[2] = (gp_color_stop){ GREEN, 0.3f };
    hard.s[3] = (gp_color_stop){ GREEN, 0.7f }; hard.s[4] = (gp_color_stop){ BLUE, 0.7f }; hard.s[5] = (gp_color_stop){ BLUE, 1 };
    static const float edges[2] = { 0.3f, 0.7f };
    e = compare_box(px, pw, B_HARD, &hard, 1.f, 0, edges, 2, &wx, &wy);
    CHECK(e <= tol, "hard stops: off by %d at %d,%d", e, wx, wy);
    {   /* and the edge is hard: within two pixels the color has changed completely */
        Box b = box_of(B_HARD);
        int edge = b.x0 + (int)lroundf(0.3f * (float)(b.x1 - b.x0)), y = (b.y0 + b.y1) / 2;
        const uint8_t *before = px + ((size_t)y * (size_t)pw + (size_t)(edge - 2)) * 3, *after = px + ((size_t)y * (size_t)pw + (size_t)(edge + 2)) * 3;
        CHECK(before[0] > 200 && before[1] < 100 && after[1] > 180 && after[0] < 100, "a hard stop is hard: %d,%d,%d then %d,%d,%d", before[0], before[1], before[2], after[0], after[1], after[2]);
    }
    /* a fade from transparent red to blue over the page: premultiplied, so it never goes through a darker or grayer middle */
    Ref fade = { .n = 2, .angle = 90 };
    fade.s[0] = (gp_color_stop){ gp_rgba(0xf0405000), 0 }; fade.s[1] = (gp_color_stop){ gp_rgba(0x3060f0ff), 1 };
    e = compare_box(px, pw, B_FADE, &fade, 1.f, 0, NULL, 0, &wx, &wy);
    CHECK(e <= tol, "fade from transparent: off by %d at %d,%d", e, wx, wy);
    e = compare_box(px, pw, B_OPACITY, &(Ref){ .n = 2, .angle = 90, .s = { { RED, 0 }, { BLUE, 1 } } }, 0.5f, 0, NULL, 0, &wx, &wy);
    CHECK(e <= tol, "element opacity 0.5: off by %d at %d,%d", e, wx, wy);

    /* rounded corners and a border: outside the corner the page shows, the ring is the border color, inside the gradient */
    {
        Box b = box_of(B_ROUND);
        const uint8_t *corner = px + ((size_t)(b.y0) * (size_t)pw + (size_t)b.x0) * 3, *ring = px + ((size_t)((b.y0 + b.y1) / 2) * (size_t)pw + (size_t)(b.x0 + 1)) * 3,
                      *inside = px + ((size_t)((b.y0 + b.y1) / 2) * (size_t)pw + (size_t)(b.x0 + 100)) * 3;
        CHECK(abs(corner[0] - 20) <= 2 && abs(corner[1] - 20) <= 2 && abs(corner[2] - 30) <= 2, "rounded: the corner pixel is the page (%d,%d,%d)", corner[0], corner[1], corner[2]);
        CHECK(ring[0] > 240 && ring[1] > 240 && ring[2] > 240, "the border is drawn over the gradient (%d,%d,%d)", ring[0], ring[1], ring[2]);
        Ref up = two(GREEN, BLUE, 0);
        float lx = 100.5f - (float)(b.x1 - b.x0) / 2, ly = (float)((b.y1 - b.y0) / 2) + 0.5f - (float)(b.y1 - b.y0) / 2;
        float c[4];
        ref_premult(&up, ref_t(&up, lx, ly, (float)(b.x1 - b.x0), (float)(b.y1 - b.y0)), c);
        CHECK(abs((int)lroundf(c[0] * 255) - inside[0]) <= tol && abs((int)lroundf(c[1] * 255) - inside[1]) <= tol && abs((int)lroundf(c[2] * 255) - inside[2]) <= tol,
              "inside the border it is the gradient: %d,%d,%d, wanted %.0f,%.0f,%.0f", inside[0], inside[1], inside[2], c[0] * 255, c[1] * 255, c[2] * 255);
    }

    {   /* a plain background declared after a gradient replaces it */
        Box b = box_of(B_LAST);
        const uint8_t *p = px + ((size_t)(b.y0 + 20) * (size_t)pw + (size_t)(b.x0 + 10)) * 3, *q = px + ((size_t)(b.y0 + 20) * (size_t)pw + (size_t)(b.x0 + 250)) * 3;
        CHECK(abs(p[0] - 128) <= 2 && abs(q[2] - 128) <= 2 && abs(p[1] - 128) <= 2, "a later gp_bg replaces the gradient (%d,%d,%d | %d,%d,%d)", p[0], p[1], p[2], q[0], q[1], q[2]);
    }

    {   /* repeating and conic gradients, against the same definition */
        Ref rep1 = { .n = 2, .repeat = true, .angle = 90 };
        rep1.s[0] = (gp_color_stop){ RED, 0 }; rep1.s[1] = (gp_color_stop){ BLUE, 0.25f };
        e = compare_box(px, pw, B_REPEAT, &rep1, 1.f, 0, NULL, 0, &wx, &wy);
        CHECK(e <= tol, "repeating 90 deg, four periods: off by %d at %d,%d", e, wx, wy);
        Box b = box_of(B_REPEAT);   /* the pattern jumps back at a quarter of the width, and the pixels either side are far apart */
        const uint8_t *before = px + ((size_t)(b.y0 + 20) * (size_t)pw + (size_t)(b.x0 + 63)) * 3, *after = px + ((size_t)(b.y0 + 20) * (size_t)pw + (size_t)(b.x0 + 67)) * 3;
        CHECK(before[2] > 180 && after[0] > 180, "it jumps from blue back to red at each period (%d,%d,%d | %d,%d,%d)", before[0], before[1], before[2], after[0], after[1], after[2]);
        Ref rep2 = { .n = 2, .repeat = true, .angle = 90 };
        rep2.s[0] = (gp_color_stop){ YELLOW, 0.1f }; rep2.s[1] = (gp_color_stop){ GREEN, 0.35f };
        e = compare_box(px, pw, B_REPEAT_OFF, &rep2, 1.f, 0, NULL, 0, &wx, &wy);
        CHECK(e <= tol, "repeating, starting at 0.1: the pattern goes on backwards too: off by %d at %d,%d", e, wx, wy);
        Ref rr = { .n = 2, .kind = 1, .repeat = true, .cx = 0.5f, .cy = 0.5f };
        rr.s[0] = (gp_color_stop){ gp_rgb(0xffffff), 0 }; rr.s[1] = (gp_color_stop){ gp_rgb(0x000000), 0.1f };
        e = compare_box(px, pw, B_REPEAT_RAD, &rr, 1.f, 0, NULL, 0, &wx, &wy);
        CHECK(e <= tol, "repeating radial: off by %d at %d,%d", e, wx, wy);
        Ref con = { .n = 4, .kind = 2, .cx = 0.5f, .cy = 0.5f, .angle = 0 };
        con.s[0] = (gp_color_stop){ RED, 0 }; con.s[1] = (gp_color_stop){ YELLOW, 0.25f }; con.s[2] = (gp_color_stop){ BLUE, 0.5f }; con.s[3] = (gp_color_stop){ RED, 1 };
        e = compare_box(px, pw, B_CONIC, &con, 1.f, 0, NULL, 0, &wx, &wy);
        CHECK(e <= tol, "conic: off by %d at %d,%d", e, wx, wy);
        Ref con2 = { .n = 2, .kind = 2, .cx = 0.3f, .cy = 0.6f, .angle = 90 };
        con2.s[0] = (gp_color_stop){ GREEN, 0 }; con2.s[1] = (gp_color_stop){ BLUE, 1 };
        e = compare_box(px, pw, B_CONIC_FROM, &con2, 1.f, 0, NULL, 0, &wx, &wy);
        CHECK(e <= tol, "conic from 90 deg around (0.3, 0.6): off by %d at %d,%d", e, wx, wy);
    }

    /* the hover color replaces the gradient and gives it back */
    {
        Box b = box_of(B_HOVER);
        gp_simulate_mouse(app, GP_SIM_MOVE, (float)(b.x0 + 50), (float)(b.y0 + 20), 0, 0);
        wait_draws(1);
        px = frame();
        const uint8_t *p = px + ((size_t)(b.y0 + 20) * (size_t)pw + (size_t)(b.x0 + 200)) * 3;
        CHECK(abs(p[0] - 0) <= 2 && abs(p[1] - 255) <= 2 && abs(p[2] - 128) <= 2, "hovered: the plain color (%d,%d,%d)", p[0], p[1], p[2]);
        gp_simulate_mouse(app, GP_SIM_MOVE, 5, 5, 0, 0);
        wait_draws(1);
        px = frame();
        p = px + ((size_t)(b.y0 + 20) * (size_t)pw + (size_t)(b.x0 + 200)) * 3;
        CHECK(p[2] > 150 && p[1] < 150, "not hovered: the gradient is back (%d,%d,%d)", p[0], p[1], p[2]);
    }

    /* a gradient in a cached view, and the view painted again from its copy */
    {
        px = frame();
        Ref kid = two(RED, BLUE, 90);
        e = compare_box(px, pw, NBOX, &kid, 1.f, 0, NULL, 0, &wx, &wy);
        CHECK(e <= tol, "in a cached view: off by %d at %d,%d", e, wx, wy);
        unsigned long hits = app->stat_paint_hits;
        win()->dirty_paint = true;
        wait_draws(2);
        px = frame();
        e = compare_box(px, pw, NBOX, &kid, 1.f, 0, NULL, 0, &wx, &wy);
        CHECK(e <= tol && app->stat_paint_hits > hits, "and from the reused copy: off by %d, %lu reuses", e, app->stat_paint_hits - hits);
    }

    /* more gradients over time than the table holds: the frame is cleared and painted again, and every row ends up right */
    {
        unsigned long resets = app->stat_atlas_resets;
        fx->many = 1;
        notify_root();
        wait_draws(3);
        CHECK(app->stat_atlas_resets == resets, "100 gradients fit (%lu resets)", app->stat_atlas_resets - resets);
        fx->many = 2;
        notify_root();
        wait_draws(3);
        for (int i = 0; i < 20 && app->stat_atlas_resets == resets; i++) { win()->dirty_paint = true; wait_draws(1); }
        CHECK(app->stat_atlas_resets > resets, "the ramp table filled up and was cleared (%lu resets)", app->stat_atlas_resets - resets);
        wait_draws(3);
        px = frame();
        pw = app->capture_w;
        int bad = 0, checked = 0;
        for (int i = 0; i < 100; i++) {
            float c0[3] = { (float)(i + 40), 150, 30 }, c1[3] = { 10, 60, (float)(200 - i) };
            const uint8_t *p = px + ((size_t)(i * 3 + 1) * (size_t)pw + 130) * 3;
            for (int k = 0; k < 3; k++) {
                float want = c0[k] + (c1[k] - c0[k]) * (130.5f / (float)BW);
                checked++;
                if (abs((int)lroundf(want) - (int)p[k]) > 4) bad++;
            }
        }
        CHECK(bad == 0, "after the reset every one of the %d gradients has its own colors (%d channel values off)", checked / 3, bad);
        fx->many = 0;
        notify_root();
        wait_draws(3);
    }
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    run();
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    gp_entity kid = gp_view_new(app, 0, NULL, kid_render, NULL);
    gp_entity v = gp_view_new(app, sizeof(Fix), &(Fix){ .kid = kid }, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "gradient_test", .width = 300, .height = 900 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "gradient_test: %d FAILURE(S)\n" : "gradient_test: all checks passed\n", failures);
    return failures != 0;
}
