/*
 * The extras of stroked curves, from frames read back from the renderer and simulated clicks: dashes (every pixel of a dashed line against the pattern, with an offset, an odd
 * pattern, round caps; a click in a gap), a gradient along the stroke (the color against the distance along a straight line and round a corner; a hover color replaces it),
 * arrowheads (the triangle's pixels, the stroke stopping at its base, a translucent stroke and head blended once across the seam, a Bezier's head pointing along the tangent,
 * clicks on the head), fills (every pixel of a concave outline and of a star against even-odd containment, a translucent fill blended once across the diagonals inside it,
 * a closed stroke's join where it meets its start, clicks inside), and text along a path (its ink on the right side of a horizontal and a vertical line, centered). Needs a
 * display, at scale 1.
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

#define BG 0x101018
#define RED gp_rgb(0xff0000)
#define GREEN gp_rgb(0x00ff00)
#define BLUE gp_rgb(0x0000ff)
#define TRANS gp_rgba(0xff800080)

static int clicks, last_id;
static void on_click(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) { clicks++; last_id = (int)(intptr_t)data; }
#define CLICK(id) gp_on_click_data(on_click, (void *)(intptr_t)(id))

static const gp_point dash1[] = { { 20, 30 }, { 320, 30 } }, dash2[] = { { 20, 60 }, { 320, 60 } }, dash3[] = { { 20, 90 }, { 320, 90 } }, dash4[] = { { 20, 120 }, { 320, 120 } };
static const gp_point grad1[] = { { 20, 160 }, { 320, 160 } }, grad2[] = { { 20, 190 }, { 170, 190 }, { 170, 340 } };
static const gp_point arr1[] = { { 400, 40 }, { 600, 40 } }, arr2[] = { { 400.5f, 80 }, { 600.5f, 80 } };   /* (the heads' bases on pixel centers: x 420.5 and 580.5) */
static const gp_point arr3[] = { { 20, 365 }, { 200, 365 } };
static const gp_point ell[] = { { 220, 380 }, { 320, 380 }, { 320, 420 }, { 260, 420 }, { 260, 480 }, { 220, 480 } };
static gp_point star[10];
static const gp_point sq[] = { { 400, 380 }, { 480, 380 }, { 480, 460 }, { 400, 460 } };
static const gp_point txt1[] = { { 400, 300 }, { 700, 300 } }, txt2[] = { { 730, 220 }, { 730, 540 } };

static gp_node *render(void *self, gp_ctx *cx) {
    return gp_div(
        gp_size_full(), gp_bg(gp_rgb(BG)),
        gp_polyline(dash1, 2, gp_stroke_width(10), gp_stroke_color(RED), gp_stroke_cap_butt(), gp_stroke_dashes(20, 10), CLICK(1)),
        gp_polyline(dash2, 2, gp_stroke_width(10), gp_stroke_color(RED), gp_stroke_cap_butt(), gp_stroke_dashes(20, 10), gp_stroke_dash_offset(5)),
        gp_polyline(dash3, 2, gp_stroke_width(10), gp_stroke_color(RED), gp_stroke_cap_butt(), gp_stroke_dashes(10)),
        gp_polyline(dash4, 2, gp_stroke_width(6), gp_stroke_color(RED), gp_stroke_dashes(20, 10)),
        gp_polyline(grad1, 2, gp_stroke_width(10), gp_stroke_cap_butt(), gp_stroke_gradient(gp_stop(RED, 0), gp_stop(BLUE, 1)), gp_hover_bg(GREEN), CLICK(2)),
        gp_polyline(grad2, 3, gp_stroke_width(10), gp_stroke_cap_butt(), gp_stroke_gradient(gp_stop(RED, 0), gp_stop(BLUE, 1))),
        gp_polyline(arr1, 2, gp_stroke_width(4), gp_stroke_color(RED), gp_arrow_end(20), CLICK(3)),
        gp_polyline(arr3, 2, gp_stroke_width(4), gp_stroke_gradient(gp_stop(RED, 0), gp_stop(BLUE, 1)), gp_arrow_end(16)),
        gp_polyline(arr2, 2, gp_stroke_width(6), gp_stroke_color(TRANS), gp_arrow_start(20), gp_arrow_end(20)),
        gp_curve(gp_xy(400, 150), gp_xy(450, 100), gp_xy(550, 200), gp_xy(600, 150), gp_stroke_width(4), gp_stroke_color(RED), gp_arrow_end(24)),
        gp_polyline(ell, 6, gp_stroke_width(0), gp_fill_color(gp_rgba(0x00ff0080)), CLICK(4)),
        gp_polyline(star, 10, gp_stroke_width(0), gp_fill_color(GREEN)),
        gp_polyline(sq, 4, gp_closed(), gp_stroke_width(6), gp_stroke_color(TRANS), gp_fill_color(gp_rgb(0x2040c0))),
        gp_polyline(txt1, 2, gp_stroke_width(0), gp_path_text("HHHH"), gp_text_size(20), gp_text_color(gp_rgb(0xffffff))),
        gp_polyline(txt2, 2, gp_stroke_width(0), gp_path_text("HHHH"), gp_text_size(20), gp_text_color(gp_rgb(0xffffff)), gp_text_center()));
}

/* ---- reading frames */

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
static float red_cov(const uint8_t *p) { return fminf(fmaxf(((float)p[0] - 16.f) / 239.f, 0.f), 1.f); }
static float green_cov(const uint8_t *p) { return fminf(fmaxf(((float)p[1] - 16.f) / 239.f, 0.f), 1.f); }
static bool is_bg(const uint8_t *p) { return abs(p[0] - 0x10) <= 1 && abs(p[1] - 0x10) <= 1 && abs(p[2] - 0x18) <= 1; }
static int cdiff(const uint8_t *a, const uint8_t *b) { return abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]); }

static void click_at(float x, float y) {
    gp_simulate_mouse(app, GP_SIM_MOVE, x, y, 0, 0);
    gp_simulate_mouse(app, GP_SIM_DOWN, x, y, 1, 0);
    gp_simulate_mouse(app, GP_SIM_UP, x, y, 1, 0);
    gp_sleep(co_, 0.03);
}
static void away(void) { gp_simulate_mouse(app, GP_SIM_MOVE, 770, 5, 0, 0); gp_sleep(co_, 0.03); }

/* even-odd containment, and the distance to the outline */
static bool inside(const gp_point *p, int n, float x, float y) {
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++)
        if ((p[i].y > y) != (p[j].y > y) && x < (p[j].x - p[i].x) * (y - p[i].y) / (p[j].y - p[i].y) + p[i].x) in = !in;
    return in;
}
static float edge_dist(const gp_point *p, int n, float x, float y) {
    float best = INFINITY;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        float dx = p[i].x - p[j].x, dy = p[i].y - p[j].y, l2 = dx * dx + dy * dy, t = l2 > 0 ? ((x - p[j].x) * dx + (y - p[j].y) * dy) / l2 : 0;
        t = fminf(fmaxf(t, 0.f), 1.f);
        best = fminf(best, hypotf(x - (p[j].x + t * dx), y - (p[j].y + t * dy)));
    }
    return best;
}

/* the ink of light pixels in a rectangle: its bounds and how many */
static int ink(const uint8_t *f, int x0, int y0, int x1, int y1, int *bx0, int *by0, int *bx1, int *by1) {
    int n = 0;
    *bx0 = *by0 = 1 << 30;
    *bx1 = *by1 = -1;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            if (px(f, x, y)[0] > 128 && px(f, x, y)[1] > 128 && px(f, x, y)[2] > 128) {
                n++;
                if (x < *bx0) *bx0 = x;
                if (x > *bx1) *bx1 = x;
                if (y < *by0) *by0 = y;
                if (y > *by1) *by1 = y;
            }
    return n;
}

static void run(void) {
    wait_draws(2);
    away();
    uint8_t *f = frame();
    CHECK(f != NULL, "a frame");
    if (!f) return;

    /* ---- dashes: pixel centers are half a pixel from every dash end, so each pixel is wholly in or out */
    int bad = 0;
    for (int x = 15; x < 325; x++) {
        float s = (float)x + 0.5f - 20.f;
        bool on1 = s > 0 && s < 300 && fmodf(s, 30.f) < 20.f, on2 = s > 0 && s < 300 && fmodf(s + 5.f, 30.f) < 20.f, on3 = s > 0 && s < 300 && fmodf(s, 20.f) < 10.f;
        float c1 = red_cov(px(f, x, 30)), c2 = red_cov(px(f, x, 60)), c3 = red_cov(px(f, x, 90));
        if (fabsf(c1 - (on1 ? 1.f : 0.f)) > 0.03f || fabsf(c2 - (on2 ? 1.f : 0.f)) > 0.03f || fabsf(c3 - (on3 ? 1.f : 0.f)) > 0.03f) {
            if (bad++ < 5) printf("  dashes at x %d: %.2f %.2f %.2f, wanted %d %d %d\n", x, c1, c2, c3, on1, on2, on3);
        }
        if (!is_bg(px(f, x, 30 - 6)) || !is_bg(px(f, x, 30 + 5))) bad++;   /* (nothing above or below: the dashes are 10 px thick, 25 .. 35) */
    }
    CHECK(bad == 0, "dashed lines: every pixel as the pattern says, with an offset of 5 and with an odd pattern (%d wrong)", bad);
    /* round caps: a dash reaches 3 px past its end (half of 6) along the line, and the middle of a 10 px gap is clear */
    CHECK(red_cov(px(f, 41, 120)) > 0.9f && red_cov(px(f, 42, 120)) > 0.3f, "a round-capped dash goes on past its end (%.2f %.2f)", red_cov(px(f, 41, 120)), red_cov(px(f, 42, 120)));
    CHECK(is_bg(px(f, 44, 120)) && is_bg(px(f, 45, 120)), "and the middle of the gap is clear");
    clicks = 0;
    click_at(45, 30);
    CHECK(clicks == 1 && last_id == 1, "a click in a gap is on the stroke (%d, id %d)", clicks, last_id);

    /* ---- a gradient along the stroke: red to blue by distance */
    bad = 0;
    for (int x = 22; x < 318; x++) {
        float t = ((float)x + 0.5f - 20.f) / 300.f;
        const uint8_t *p = px(f, x, 160);
        if (abs(p[0] - (int)lroundf(255.f * (1.f - t))) > 4 || abs(p[2] - (int)lroundf(255.f * t)) > 4 || p[1] > 2) {
            if (bad++ < 5) printf("  gradient at x %d: %d %d %d, wanted %.0f 0 %.0f\n", x, p[0], p[1], p[2], 255.f * (1.f - t), 255.f * t);
        }
    }
    CHECK(bad == 0, "a stroke gradient follows the distance along a straight line (%d wrong)", bad);
    struct { int x, y; float t; } along[] = { { 60, 190, 40.5f / 300 }, { 160, 190, 140.5f / 300 }, { 170, 230, 190.5f / 300 }, { 170, 330, 290.5f / 300 } };
    for (size_t i = 0; i < sizeof along / sizeof *along; i++) {
        const uint8_t *p = px(f, along[i].x, along[i].y);
        float t = along[i].t;
        CHECK(abs(p[0] - (int)lroundf(255.f * (1.f - t))) <= 5 && abs(p[2] - (int)lroundf(255.f * t)) <= 5, "round a corner too, at (%d, %d): %d %d %d, wanted %.0f, %.0f",
              along[i].x, along[i].y, p[0], p[1], p[2], 255.f * (1.f - t), 255.f * t);
    }
    bad = 0;
    for (int y = 184; y < 196; y++)
        for (int x = 160; x < 177; x++) {   /* the joint: no pixel of another color, and none missing near the corner */
            const uint8_t *p = px(f, x, y);
            if (p[1] > 17 || (p[0] + p[2] < 200 && hypotf((float)x + 0.5f - 170.f, (float)y + 0.5f - 190.f) < 4.5f)) { if (bad++ < 4) printf("  joint (%d, %d): %d %d %d\n", x, y, p[0], p[1], p[2]); }
        }
    CHECK(bad == 0, "and the joint is filled with the colors there (%d odd pixels)", bad);
    gp_simulate_mouse(app, GP_SIM_MOVE, 100, 160, 0, 0);
    gp_sleep(co_, 0.05);
    f = frame();
    CHECK(f && green_cov(px(f, 100, 160)) > 0.95f && red_cov(px(f, 100, 160)) < 0.05f, "a hover color replaces the gradient");
    away();
    f = frame();
    CHECK(f && red_cov(px(f, 100, 160)) > 0.5f, "and it comes back");

    /* ---- arrowheads */
    bad = 0;
    for (int y = 25; y < 56; y++)
        for (int x = 560; x < 606; x++) {
            float cx = (float)x + 0.5f, cy = (float)y + 0.5f;
            float hw_head = cx >= 580.f && cx <= 600.f ? (600.f - cx) * 0.5f : -1.f;   /* the head: tip (600,40), base 580 from y 30 to 50 */
            bool in = (cx < 580.f && fabsf(cy - 40.f) < 2.f) || fabsf(cy - 40.f) < hw_head;
            static const gp_point outline[] = { { 560, 38 }, { 580, 38 }, { 580, 30 }, { 600, 40 }, { 580, 50 }, { 580, 42 }, { 560, 42 } };
            float edge = INFINITY;
            for (int k = 0; k + 1 < 7; k++) edge = fminf(edge, edge_dist(outline + k, 2, cx, cy));
            if (edge < 1.f) continue;   /* (pixels the edge passes through are partly covered) */
            float c = red_cov(px(f, x, y));
            if (fabsf(c - (in ? 1.f : 0.f)) > 0.05f && bad++ < 5) printf("  arrow at (%d, %d): %.2f, wanted %d\n", x, y, c, in);
        }
    CHECK(bad == 0, "an arrowhead: a triangle 20 long and 20 wide at the end, the stroke up to its base (%d wrong)", bad);
    const uint8_t *ref = px(f, 500, 80);
    CHECK(!is_bg(ref) && red_cov(ref) < 0.95f, "a translucent stroke is translucent (%d %d %d)", ref[0], ref[1], ref[2]);
    bad = 0;
    for (int x = 404; x < 597; x++)
        if (cdiff(px(f, x, 80), ref) > 3 && bad++ < 5) printf("  translucent arrow line at x %d: %d %d %d vs %d %d %d\n", x, px(f, x, 80)[0], px(f, x, 80)[1], px(f, x, 80)[2], ref[0], ref[1], ref[2]);
    CHECK(bad == 0, "a translucent stroke and its heads blend once: the same color from tip to tip, across both seams (%d differ)", bad);
    bad = 0;
    for (int y = 70; y < 91; y++)
        for (int x = 405; x < 596; x++) {
            const uint8_t *p = px(f, x, y);
            if (!is_bg(p) && cdiff(p, ref) > 3 && !(fabsf((float)y + 0.5f - 80.f) > 2.f)) bad++;   /* within the stroke's band: all one color */
            if (cdiff(p, ref) <= 3 || is_bg(p)) continue;
            if (p[0] > ref[0] + 3) bad++;   /* nowhere brighter than once */
        }
    CHECK(bad == 0, "and no pixel near the seams is blended twice (%d)", bad);
    /* the Bezier's head points from the curve 24 back along it (the middle of its base, where the stroke stops) to the tip */
    {
        gp_point bz[4] = { { 400, 150 }, { 450, 100 }, { 550, 200 }, { 600, 150 } }, prev = bz[3], base = bz[3];
        float run = 0;
        for (int i = 1; i <= 20000 && run < 24.f; i++) {
            float t = 1.f - (float)i / 20000.f, u = 1.f - t;
            gp_point q = { u * u * u * bz[0].x + 3 * u * u * t * bz[1].x + 3 * u * t * t * bz[2].x + t * t * t * bz[3].x,
                           u * u * u * bz[0].y + 3 * u * u * t * bz[1].y + 3 * u * t * t * bz[2].y + t * t * t * bz[3].y };
            run += hypotf(q.x - prev.x, q.y - prev.y);
            prev = base = q;
        }
        float dx = 600 - base.x, dy = 150 - base.y, l = hypotf(dx, dy);
        dx /= l; dy /= l;
        float mx = 600 - dx * l * 0.5f, my = 150 - dy * l * 0.5f;   /* halfway: 6 px wide each side */
        CHECK(red_cov(px(f, (int)(mx + dy * 4), (int)(my - dx * 4))) > 0.8f && red_cov(px(f, (int)(mx - dy * 4), (int)(my + dx * 4))) > 0.8f,
              "a Bezier's arrowhead: 4 px either side of its middle is covered");
        CHECK(is_bg(px(f, (int)(mx + dy * 9), (int)(my - dx * 9))) && is_bg(px(f, (int)(mx - dy * 9), (int)(my + dx * 9))), "9 px either side is not");
        CHECK(is_bg(px(f, 604, 146)), "and nothing past the tip");
    }
    {
        const uint8_t *h = px(f, 194, 365);
        CHECK(h[2] > 230 && h[0] < 25, "a gradient stroke's arrowhead at the end is the end's color (%d %d %d)", h[0], h[1], h[2]);
    }
    clicks = 0;
    click_at(587, 45);   /* in the head, 5 px off the line: beyond the stroke's 2 */
    CHECK(clicks == 1 && last_id == 3, "a click on the arrowhead lands (%d)", clicks);
    clicks = 0;
    click_at(595, 46);
    CHECK(clicks == 0, "beside it does not (%d)", clicks);

    /* ---- fills */
    away();
    f = frame();
    const uint8_t *fref = px(f, 240, 400);
    CHECK(green_cov(fref) > 0.3f && green_cov(fref) < 0.7f, "a translucent fill (%d %d %d)", fref[0], fref[1], fref[2]);
    bad = 0;
    for (int y = 370; y < 490; y++)
        for (int x = 210; x < 330; x++) {
            bool in = inside(ell, 6, (float)x + 0.5f, (float)y + 0.5f);
            const uint8_t *p = px(f, x, y);
            if (in ? cdiff(p, fref) > 2 : !is_bg(p)) { if (bad++ < 5) printf("  L at (%d, %d): %d %d %d, inside %d\n", x, y, p[0], p[1], p[2], in); }
        }
    CHECK(bad == 0, "a concave outline: every pixel inside is the one blend (no seams along the diagonals), every pixel outside untouched (%d wrong)", bad);
    bad = 0;
    for (int y = 380; y < 500; y++)
        for (int x = 500; x < 620; x++) {
            float cx = (float)x + 0.5f, cy = (float)y + 0.5f;
            if (edge_dist(star, 10, cx, cy) < 0.75f) continue;
            float c = green_cov(px(f, x, y));
            if (fabsf(c - (inside(star, 10, cx, cy) ? 1.f : 0.f)) > 0.05f && bad++ < 5) printf("  star at (%d, %d): %.2f\n", x, y, c);
        }
    CHECK(bad == 0, "a star (ten corners, five of them inward): inside filled, outside not (%d wrong)", bad);
    /* the closed square: its stroke goes round the corner at its first point as at the others, over the fill inside and the background outside */
    const uint8_t *outer = px(f, 440, 378), *inner = px(f, 440, 381);
    CHECK(cdiff(outer, inner) > 20, "a closed stroke over a fill: the outer half is over the background, the inner over the fill");
    CHECK(cdiff(px(f, 398, 420), outer) <= 3 && cdiff(px(f, 401, 420), inner) <= 3, "on the side that closes it as well");
    CHECK(cdiff(px(f, 398, 378), outer) <= 3 && cdiff(px(f, 481, 461), outer) <= 3, "its corners are joined: the first point as the third (%d %d %d)", px(f, 398, 378)[0], px(f, 398, 378)[1], px(f, 398, 378)[2]);
    CHECK(cdiff(px(f, 400, 378), outer) <= 3 && cdiff(px(f, 401, 377), outer) <= 3, "and blended once there: the last side does not run on into the first (%d %d %d)",
          px(f, 400, 378)[0], px(f, 400, 378)[1], px(f, 400, 378)[2]);
    if (getenv("DUMP")) for (int y = 372; y < 392; y++) { for (int x = 392; x < 412; x++) printf("%3d ", px(f, x, y)[0]); printf("  %d\n", y); }
    if (getenv("DUMP")) for (int y = 430; y < 446; y++) { for (int x = 560; x < 580; x++) printf("%3d ", px(f, x, y)[1]); printf("  %d\n", y); }
    CHECK(cdiff(px(f, 440, 420), px(f, 420, 400)) <= 2 && px(f, 440, 420)[2] > 0xb0, "and the fill inside");
    clicks = 0;
    click_at(240, 460);
    CHECK(clicks == 1 && last_id == 4, "a click inside a fill lands (%d)", clicks);
    clicks = 0;
    click_at(290, 450);
    CHECK(clicks == 0, "in the notch of the L it does not (%d)", clicks);

    /* ---- text along a path */
    away();
    f = frame();
    int x0, y0, x1, y1;
    int n = ink(f, 380, 260, 720, 330, &x0, &y0, &x1, &y1);
    CHECK(n > 40 && x0 >= 399 && x0 <= 404 && y1 <= 300 && y0 >= 282 && x1 < 470, "text along a horizontal line sits on it from its start (%d px, %d..%d x %d..%d)", n, x0, x1, y0, y1);
    int n2 = ink(f, 700, 210, 780, 550, &x0, &y0, &x1, &y1);
    CHECK(n2 > 40 && x0 >= 730 && x1 <= 750 && y1 - y0 > 2 * (x1 - x0), "along a line drawn downwards it is turned a quarter and stands to the left of travel (%d px, x %d..%d, y %d..%d)", n2, x0, x1, y0, y1);
    CHECK(abs((y0 + y1) / 2 - 380) <= 4, "gp_text_center centers it along the length (middle %d, wanted 380)", (y0 + y1) / 2);
    CHECK(abs(n - n2) < n / 5, "and it is the same text (%d and %d light pixels)", n, n2);
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    run();
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    for (int i = 0; i < 10; i++) {
        float r = i % 2 ? 22.f : 55.f, ang = (float)i * (float)M_PI / 5.f - (float)M_PI / 2.f;
        star[i] = (gp_point){ 560.f + r * cosf(ang), 440.f + r * sinf(ang) };
    }
    gp_entity v = gp_view_new(app, 1, &(char){ 0 }, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "stroke_test", .width = 780, .height = 560 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "stroke_test: %d FAILURE(S)\n" : "stroke_test: all checks passed\n", failures);
    return failures != 0;
}
