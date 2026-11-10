/*
 * Fill rules, markers and backdrop filters, from frames read back from the renderer: a star drawn in one line filled non-zero (the middle in) and even-odd (out), a square
 * with a square inside going round the same way and the other way, the bridge that joins a hole to its outline leaving no seam, the pointer by the rule; circle, square,
 * diamond and bar markers where they belong and turned along the line, a translucent stroke and its marker blended once, a marker hit; a backdrop blur at an edge under a
 * panel and sharp beside it, a backdrop grayscale, a rounded panel's corner, the panel's own background over the result, and its opacity fading the result. Needs a display,
 * at scale 1.
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

static gp_point star_nz[5], star_eo[5];
/* a square (0..100) and a square inside it (25..75), joined by a line out and back (which cancels); the inside one the same way round or the other */
static gp_point holes_same[11], holes_other[11], holes_eo[11];
static const gp_point mk_line[] = { { 20, 330 }, { 120, 330 }, { 120, 390 } };
static const gp_point bar_line[] = { { 160, 330 }, { 260, 330 } };
static const gp_point trans_line[] = { { 300, 330 }, { 400, 330 } };
/* two boxes joined along the top, which goes out and back over itself between them (over part of it only: the right box is wider): the part between is not inside */
/* a box with a spur that runs back along part of its own top edge and returns: the spur is nothing, the box all there */
static const gp_point spur[] = { { 220, 160 }, { 400, 160 }, { 300, 160 }, { 400, 160 }, { 400, 200 }, { 220, 200 } };
static const gp_point notch[] = { { 20, 160 }, { 200, 160 }, { 200, 200 }, { 80, 200 }, { 80, 160 }, { 50, 160 }, { 50, 200 }, { 20, 200 } };
static int clicks[4];
static void on_click(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) { clicks[(intptr_t)data]++; }

static void make_points(void) {
    for (int k = 0; k < 5; k++) {
        float a = (-90.f + 144.f * (float)k) * (float)M_PI / 180.f;
        star_nz[k] = (gp_point){ 60 + 50 * cosf(a), 70 + 50 * sinf(a) };
        star_eo[k] = (gp_point){ 180 + 50 * cosf(a), 70 + 50 * sinf(a) };
    }
    const gp_point outer[] = { { 0, 0 }, { 100, 0 }, { 100, 100 }, { 0, 100 }, { 0, 0 } };
    const gp_point in_same[] = { { 25, 25 }, { 75, 25 }, { 75, 75 }, { 25, 75 }, { 25, 25 } };
    const gp_point in_other[] = { { 25, 25 }, { 25, 75 }, { 75, 75 }, { 75, 25 }, { 25, 25 } };
    for (int i = 0; i < 5; i++) {
        holes_same[i] = (gp_point){ outer[i].x + 260, outer[i].y + 20 };
        holes_other[i] = (gp_point){ outer[i].x + 380, outer[i].y + 20 };
        holes_eo[i] = (gp_point){ outer[i].x + 500, outer[i].y + 20 };
        holes_same[5 + i] = (gp_point){ in_same[i].x + 260, in_same[i].y + 20 };
        holes_other[5 + i] = (gp_point){ in_other[i].x + 380, in_other[i].y + 20 };
        holes_eo[5 + i] = (gp_point){ in_same[i].x + 500, in_same[i].y + 20 };
    }
    holes_same[10] = holes_same[0];   /* (back out to the corner: the line in and the line out cancel) */
    holes_other[10] = holes_other[0];
    holes_eo[10] = holes_eo[0];
}

#define ABS(x, y, w, h, ...) gp_div(gp_absolute(), gp_left(x), gp_top(y), gp_w(w), gp_h(h), __VA_ARGS__)

static gp_node *render(void *self, gp_ctx *cx) {
    return gp_div(
        gp_size_full(), gp_bg(BLACK),
        /* fill rules */
        gp_polyline(star_nz, 5, gp_closed(), gp_fill_color(WHITE), gp_stroke_width(0), gp_on_click_data(on_click, (void *)0)),
        gp_polyline(star_eo, 5, gp_closed(), gp_fill_color(WHITE), gp_stroke_width(0), gp_fill_evenodd(), gp_on_click_data(on_click, (void *)1)),
        gp_polyline(holes_same, 11, gp_closed(), gp_fill_color(WHITE), gp_stroke_width(0)),
        gp_polyline(holes_other, 11, gp_closed(), gp_fill_color(WHITE), gp_stroke_width(0)),
        gp_polyline(holes_eo, 11, gp_closed(), gp_fill_color(WHITE), gp_stroke_width(0), gp_fill_evenodd()),
        /* markers */
        gp_polyline(mk_line, 3, gp_stroke_width(2), gp_stroke_color(RED), gp_marker_start(GP_MARKER_CIRCLE, 12), gp_marker_end(GP_MARKER_SQUARE, 12),
                    gp_marker_mid(GP_MARKER_DIAMOND, 12), gp_on_click_data(on_click, (void *)2)),
        gp_polyline(bar_line, 2, gp_stroke_width(4), gp_stroke_color(RED), gp_marker_end(GP_MARKER_BAR, 20)),
        gp_polyline(trans_line, 2, gp_stroke_width(4), gp_stroke_color(WHITE), gp_opacity(0.5f), gp_marker_start(GP_MARKER_CIRCLE, 16), gp_marker_end(GP_MARKER_SQUARE, 16)),
        gp_polyline(notch, 8, gp_closed(), gp_fill_color(WHITE), gp_stroke_width(0)),
        gp_polyline(spur, 6, gp_closed(), gp_fill_color(WHITE), gp_stroke_width(0)),
        /* backdrops: a white box under half a blurring panel */
        ABS(20, 440, 40, 40, gp_bg(WHITE)),
        ABS(40, 440, 60, 40, gp_backdrop_filter(gp_group(gp_filter_blur(4)))),
        /* red under a graying panel */
        ABS(140, 440, 60, 40, gp_bg(RED)),
        ABS(160, 440, 60, 40, gp_backdrop_filter(gp_group(gp_filter_grayscale(1)))),
        /* white under a rounded inverting panel, one with a half red background of its own, one at half opacity */
        ABS(260, 430, 340, 60, gp_bg(WHITE)),
        ABS(270, 440, 60, 40, gp_rounded(14), gp_backdrop_filter(gp_group(gp_filter_invert(1)))),
        ABS(350, 440, 60, 40, gp_bg(gp_rgba(0xff000080)), gp_backdrop_filter(gp_group(gp_filter_invert(1)))),
        ABS(430, 440, 60, 40, gp_opacity(0.5f), gp_backdrop_filter(gp_group(gp_filter_invert(1)))),
        /* two white rows along the window's top, under a blurring panel: past the edge is the edge again, not nothing */
        ABS(0, 0, 640, 2, gp_bg(WHITE)),
        ABS(560, 0, 60, 14, gp_backdrop_filter(gp_group(gp_filter_blur(3)))),
        /* white just left of a blurring panel, black inside its edge: the blur reads the white past the panel's edge */
        ABS(20, 492, 20, 20, gp_bg(WHITE)),
        ABS(40, 492, 40, 20, gp_backdrop_filter(gp_group(gp_filter_blur(4)))));
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
static uint8_t g_px[4][3];
static int g_pxi;
static const uint8_t *pix(const uint8_t *f, float x, float y) {
    uint8_t *o = g_px[g_pxi++ & 3];
    memcpy(o, f + ((size_t)(int)y * (size_t)app->capture_w + (size_t)(int)x) * 3, 3);
    return o;
}
static bool rgb(const uint8_t *p, int r, int g, int b, int tol) { return abs(p[0] - r) <= tol && abs(p[1] - g) <= tol && abs(p[2] - b) <= tol; }
#define P3(p) (p)[0], (p)[1], (p)[2]
static void click(float x, float y) {
    gp_simulate_mouse(app, GP_SIM_DOWN, x, y, 1, 0);
    gp_simulate_mouse(app, GP_SIM_UP, x, y, 1, 0);
    gp_sleep(co_, 0.03);
    wait_draws(1);
}

static void run(void) {
    wait_draws(3);
    uint8_t *f = frame();
    if (!f) { CHECK(0, "no frame"); return; }
    const uint8_t *p, *q;

    /* ---- the stars: a tip filled by both rules; the middle (wound twice) by non-zero only */
    p = pix(f, 60, 70);
    q = pix(f, 180, 70);
    CHECK(rgb(p, 255, 255, 255, 1), "a star non-zero: the middle is in: %d %d %d", P3(p));
    CHECK(rgb(q, 0, 0, 0, 1), "a star even-odd: the middle is out: %d %d %d", P3(q));
    p = pix(f, 60, 26);
    q = pix(f, 180, 26);
    CHECK(rgb(p, 255, 255, 255, 2) && rgb(q, 255, 255, 255, 2), "the stars' top tips: %d %d %d / %d %d %d", P3(p), P3(q));
    p = pix(f, 60 + 30, 70 - 30);
    CHECK(rgb(p, 0, 0, 0, 1), "between two tips nothing: %d %d %d", P3(p));

    /* ---- a square in a square: going round the same way, non-zero fills it (wound twice); the other way it is a hole; even-odd a hole either way */
    p = pix(f, 260 + 50, 20 + 50);
    q = pix(f, 380 + 50, 20 + 50);
    const uint8_t *r = pix(f, 500 + 50, 20 + 50);
    CHECK(rgb(p, 255, 255, 255, 1), "the inner square the same way round, non-zero: in: %d %d %d", P3(p));
    CHECK(rgb(q, 0, 0, 0, 1), "the other way round: a hole: %d %d %d", P3(q));
    CHECK(rgb(r, 0, 0, 0, 1), "even-odd: a hole: %d %d %d", P3(r));
    /* the ring around each hole is filled all over, with no seam where the hole was joined to it (any line from the hole to a corner of the outside) */
    int seams = 0;
    for (int b = 0; b < 2; b++) {
        float x0 = b ? 500 : 380;
        for (int k = 0; k < 400; k++) {
            float t = (float)k / 400.f;   /* the ring: x in 0..25 and 75..100 of the square, or y the same */
            float px = x0 + 2 + 96 * t, py = 20 + 12;
            if (!rgb(pix(f, px, py), 255, 255, 255, 0)) seams++;
            py = 20 + 88;
            if (!rgb(pix(f, px, py), 255, 255, 255, 0)) seams++;
            px = x0 + 88; py = 20 + 2 + 96 * t;
            if (!rgb(pix(f, px, py), 255, 255, 255, 0)) seams++;
            px = x0 + 12;
            if (!rgb(pix(f, px, py), 255, 255, 255, 0)) seams++;
        }
        /* and along every diagonal from the hole's corners to the outside's, where a bridge would go */
        const float hc[4][2] = { { 75, 25 }, { 75, 75 }, { 25, 75 }, { 25, 25 } }, oc[4][2] = { { 100, 0 }, { 100, 100 }, { 0, 100 }, { 0, 0 } };
        for (int c = 0; c < 4; c++)
            for (int k = 2; k < 30; k++) {
                float t = (float)k / 32.f;
                float px = x0 + hc[c][0] + (oc[c][0] - hc[c][0]) * t, py = 20 + hc[c][1] + (oc[c][1] - hc[c][1]) * t;
                if (!rgb(pix(f, px, py), 255, 255, 255, 0)) seams++;
            }
    }
    CHECK(seams == 0, "%d pixels of the rings around the holes are not white", seams);

    /* ---- the notch: both boxes, nothing between */
    p = pix(f, 24, 164);
    q = pix(f, 150, 180);
    r = pix(f, 65, 180);
    CHECK(rgb(p, 255, 255, 255, 1) && rgb(q, 255, 255, 255, 1) && rgb(r, 0, 0, 0, 1), "the notch: %d %d %d, %d %d %d, between %d %d %d", P3(p), P3(q), P3(r));
    p = pix(f, 45, 196);
    CHECK(rgb(p, 255, 255, 255, 1), "the notch's left box, its other corner: %d %d %d", P3(p));
    int spur_bad = 0;
    for (int yy = 162; yy < 199; yy += 3)
        for (int xx = 222; xx < 399; xx += 3) spur_bad += !rgb(pix(f, (float)xx, (float)yy), 255, 255, 255, 1);
    p = pix(f, 300, 157);
    CHECK(spur_bad == 0 && rgb(p, 0, 0, 0, 1), "a box with a spur along its edge: %d pixels inside not white; above it %d %d %d", spur_bad, P3(p));

    /* ---- markers: a circle at the start (off the stroke line, inside the circle: red; past it: black) */
    p = pix(f, 20, 330 + 4);
    q = pix(f, 20, 330 + 8);
    CHECK(rgb(p, 255, 0, 0, 2) && rgb(q, 0, 0, 0, 2), "the circle marker: %d %d %d inside, %d %d %d outside", P3(p), P3(q));
    p = pix(f, 120 + 5, 390 + 5);
    q = pix(f, 120 + 8, 390);
    CHECK(rgb(p, 255, 0, 0, 2) && rgb(q, 0, 0, 0, 2), "the square marker, along the last piece (down): %d %d %d in its corner, %d %d %d past it", P3(p), P3(q));
    /* the diamond at the corner, turned half way between right and down: a square on the axes, half 4.24 */
    p = pix(f, 120 + 3.5f, 330 - 3.5f);
    q = pix(f, 120 + 6, 330 - 1);
    CHECK(rgb(p, 255, 0, 0, 2) && rgb(q, 0, 0, 0, 2), "the diamond marker at the corner: %d %d %d in, %d %d %d out", P3(p), P3(q));
    p = pix(f, 260 - 1, 330 + 8);
    q = pix(f, 260 - 1, 330 + 12);
    CHECK(rgb(p, 255, 0, 0, 2) && rgb(q, 0, 0, 0, 2), "the bar across the end: %d %d %d on it, %d %d %d past it", P3(p), P3(q));
    p = pix(f, 300, 330);
    q = pix(f, 300, 330 + 6);
    CHECK(rgb(p, 128, 128, 128, 3) && rgb(q, 128, 128, 128, 3), "a translucent stroke under its marker: once %d %d %d, the marker alone %d %d %d", P3(p), P3(q));
    p = pix(f, 400, 330);
    q = pix(f, 400 + 6, 330 - 6);
    CHECK(rgb(p, 128, 128, 128, 3) && rgb(q, 128, 128, 128, 3), "a translucent stroke under a square marker: %d %d %d, the marker alone %d %d %d", P3(p), P3(q));

    /* ---- backdrops: under the panel the box's edge (x 60) is blurred; beside the panel it is sharp */
    p = pix(f, 60, 460);
    q = pix(f, 64, 460);
    CHECK(p[0] > 100 && p[0] < 160 && q[0] > 5 && q[0] < 80, "a backdrop blur at the edge: %d, 4 px past it %d", p[0], q[0]);
    p = pix(f, 38, 460);
    CHECK(rgb(p, 255, 255, 255, 1), "beside the panel the box is as it was: %d %d %d", P3(p));
    p = pix(f, 41, 502);
    CHECK(p[0] > 45 && p[0] < 130, "inside the panel's edge the blur reads the white past it: %d", p[0]);
    p = pix(f, 590, 4);
    CHECK(p[0] > 42 && p[0] < 70, "a blur at the window's top edge (the white rows go on above it): %d", p[0]);
    p = pix(f, 90, 460);
    CHECK(p[0] < 3, "the panel far from the box: %d", p[0]);
    p = pix(f, 190, 460);
    q = pix(f, 150, 460);
    CHECK(rgb(p, 54, 54, 54, 2) && rgb(q, 255, 0, 0, 1), "a backdrop grayscale: %d %d %d under it, %d %d %d beside", P3(p), P3(q));
    p = pix(f, 300, 460);
    q = pix(f, 271, 441);
    CHECK(rgb(p, 0, 0, 0, 1) && rgb(q, 255, 255, 255, 2), "a rounded inverting panel: %d %d %d inside, %d %d %d in its cut corner", P3(p), P3(q));
    p = pix(f, 380, 460);
    CHECK(rgb(p, 128, 0, 0, 3), "the panel's half red over its inverted backdrop: %d %d %d", P3(p));
    p = pix(f, 460, 460);
    CHECK(rgb(p, 128, 128, 128, 3), "the inverted backdrop at the panel's opacity: %d %d %d", P3(p));

    /* ---- the pointer: the even-odd star's middle is not it; the non-zero one's is; a marker off the line is the stroke's */
    click(180, 70);
    click(60, 70);
    click(20, 330 + 4);
    CHECK(clicks[0] == 1 && clicks[1] == 0 && clicks[2] == 1, "clicks: non-zero star %d, even-odd star %d, the marker %d", clicks[0], clicks[1], clicks[2]);
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    run();
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    make_points();
    gp_entity v = gp_view_new(app, 1, &(char){ 0 }, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "shapes_test", .width = 640, .height = 520 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "shapes_test: %d FAILURE(S)\n" : "shapes_test: all checks passed\n", failures);
    return failures != 0;
}
