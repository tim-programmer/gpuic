/*
 * Per-corner radii and per-side borders, from frames read back from the renderer: every corner against a supersampled
 * rounded-rectangle computed here (CSS's radius scaling included), "the last one wins" between gp_rounded and the single-corner
 * calls and between gp_border and the single sides, borders on some sides only, their effect on layout (they are padding), padding and
 * borders in any order, and corners on a gradient, a border and an image. Needs a display.
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

#define PAGE gp_rgb(0x14141e)
#define RED gp_rgb(0xff0000)
#define BLUE gp_rgb(0x0000ff)
#define WHITE gp_rgb(0xffffff)

typedef struct { gp_image solid; } Fix;

static gp_node *render(void *self, gp_ctx *cx) {
    Fix *f = self;
    gp_node *a = gp_div(
        gp_flex_col(), gp_gap(6), gp_items_start(),
        gp_div(gp_size_px(60), gp_bg(RED), gp_rounded_corners(20, 0, 10, 5)),                  /* 0: all four different */
        gp_div(gp_size_px(60), gp_bg(RED), gp_rounded(6), gp_rounded_top(20)),                  /* 1: top 20, bottom 6 */
        gp_div(gp_size_px(60), gp_bg(RED), gp_rounded_top(20), gp_rounded(6)),                  /* 2: gp_rounded afterwards: all 6 */
        gp_div(gp_w(60), gp_h(30), gp_bg(RED), gp_rounded_corners(40, 40, 40, 40)),             /* 3: too big: scaled to fit */
        gp_div(gp_size_px(60), gp_bg_gradient(0, gp_stop(RED, 0), gp_stop(BLUE, 1)), gp_rounded_corners(30, 0, 0, 30)),   /* 4: a gradient */
        gp_div(gp_size_px(60), gp_bg(RED), gp_rounded_left(12), gp_rounded_right(2)),           /* 5 */
        gp_img(f->solid, gp_size_px(40), gp_rounded_corners(20, 0, 0, 0)));                     /* 6: an image */
    gp_node *b = gp_div(
        gp_flex_col(), gp_gap(6), gp_items_start(),
        gp_div(gp_w(80), gp_h(50), gp_bg(BLUE), gp_border_color(WHITE), gp_border_left(2), gp_border_top(6), gp_border_right(10)),   /* 7 */
        gp_div(gp_w(80), gp_h(50), gp_bg(BLUE), gp_border_color(WHITE), gp_border(3), gp_border_left(10)),                          /* 8: left 10, others 3 */
        gp_div(gp_w(80), gp_h(50), gp_bg(BLUE), gp_border_color(WHITE), gp_border_left(10), gp_border(3)),                          /* 9: all 3 */
        gp_div(gp_border_left(8), gp_border_y(3), gp_p(4), gp_bg(BLUE), gp_div(gp_size_px(20), gp_bg(RED))),                       /* 10: sizes from borders and padding */
        gp_div(gp_p(4), gp_border_left(8), gp_bg(BLUE), gp_div(gp_size_px(20), gp_bg(RED))),                                       /* 11: the other order */
        gp_div(gp_border_left(8), gp_p(4), gp_bg(BLUE), gp_div(gp_size_px(20), gp_bg(RED))),                                       /* 12 */
        gp_div(gp_w(80), gp_h(50), gp_bg(BLUE), gp_border_color(WHITE), gp_rounded(16), gp_border_x(4)),                           /* 13: corners and sides */
        gp_div(gp_w(80), gp_h(50), gp_bg(BLUE), gp_border_color(WHITE), gp_rounded(20), gp_border(5)),                             /* 14: rounded, uniform border */
        gp_div(gp_w(80), gp_h(50), gp_bg(BLUE), gp_border_color(WHITE), gp_rounded_corners(24, 4, 12, 0), gp_border(5)),           /* 15: four radii, uniform border */
        gp_div(gp_w(80), gp_h(50), gp_bg(BLUE), gp_border_color(WHITE), gp_rounded(20), gp_border_right(10)),                       /* 16: a border on one side of a rounded box */
        gp_div(gp_border(3), gp_border_left(10), gp_p(2), gp_bg(BLUE), gp_div(gp_size_px(20), gp_bg(RED))),                         /* 17: sides and the rest of the uniform border, with padding */
        gp_div(gp_p(5), gp_bg(BLUE), gp_div(gp_size_px(20), gp_bg(RED))),                                                           /* 18: padding alone */
        gp_img(f->solid, gp_size_px(60), gp_rounded(20), gp_border(6), gp_border_color(WHITE)));                                    /* 19: a rounded image in a border: its radius is 14 */
    return gp_div(gp_size_full(), gp_flex_row(), gp_gap(12), gp_p(6), gp_bg(PAGE), a, b);
}

/* the elements in order: the 7 on the left, then the 7 on the right */
static const gp_el *els[24];
static void collect(const gp_el *col, int *n) {
    for (uint32_t i = 0; i < col->nchildren; i++) els[(*n)++] = col->children[i];
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
    float ax, ay;
    abs_pos(win()->tree, els[i], -win()->tree->pos.x, -win()->tree->pos.y, &ax, &ay);
    return (Box){ (int)lroundf(ax), (int)lroundf(ay), (int)lroundf(els[i]->size.w), (int)lroundf(els[i]->size.h) };
}
static const uint8_t *px_at(const uint8_t *px, int pw, int x, int y) { return px + ((size_t)y * (size_t)pw + (size_t)x) * 3; }
static bool near_rgb(const uint8_t *p, int r, int g, int b, int tol) { return abs(p[0] - r) <= tol && abs(p[1] - g) <= tol && abs(p[2] - b) <= tol; }
static bool is_page(const uint8_t *p) { return near_rgb(p, 20, 20, 30, 2); }

/* a rounded rectangle w x h with radii (top-left, top-right, bottom-right, bottom-left): is (x, y) inside? */
static bool in_rr(float x, float y, float w, float h, const float r[4]) {
    if (x < 0 || y < 0 || x > w || y > h) return false;
    float cx, cy, rr;
    if (x < r[0] && y < r[0]) { cx = r[0]; cy = r[0]; rr = r[0]; }
    else if (x > w - r[1] && y < r[1]) { cx = w - r[1]; cy = r[1]; rr = r[1]; }
    else if (x > w - r[2] && y > h - r[2]) { cx = w - r[2]; cy = h - r[2]; rr = r[2]; }
    else if (x < r[3] && y > h - r[3]) { cx = r[3]; cy = h - r[3]; rr = r[3]; }
    else return true;
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= rr * rr;
}
static float rr_coverage(int px, int py, float w, float h, const float r[4]) {
    int in = 0;
    for (int sy = 0; sy < 8; sy++)
        for (int sx = 0; sx < 8; sx++) in += in_rr((float)px + (sx + 0.5f) / 8, (float)py + (sy + 0.5f) / 8, w, h, r);
    return (float)in / 64.f;
}

/* compares an element painted in a flat color with the rounded rectangle of those radii; returns pixels off, and how many are on the edge */
static int check_rr(const uint8_t *px, int pw, Box b, const float r[4], const int fill[3], int *edge) {
    int bad = 0;
    *edge = 0;
    for (int y = 0; y < b.h; y++)
        for (int x = 0; x < b.w; x++) {
            float c = rr_coverage(x, y, (float)b.w, (float)b.h, r);
            bool solid = c == 1.f || c == 0.f;
            for (int d = 0; d < 4 && solid; d++) solid = rr_coverage(x + (d == 0) - (d == 1), y + (d == 2) - (d == 3), (float)b.w, (float)b.h, r) == c;
            const uint8_t *p = px_at(px, pw, b.x0 + x, b.y0 + y);
            if (solid) {
                if (c == 1.f ? !near_rgb(p, fill[0], fill[1], fill[2], 3) : !is_page(p)) bad++;
            } else {
                int want[3] = { (int)lroundf((float)fill[0] * c + 20 * (1 - c)), (int)lroundf((float)fill[1] * c + 20 * (1 - c)), (int)lroundf((float)fill[2] * c + 30 * (1 - c)) };
                if (!near_rgb(p, want[0], want[1], want[2], 70)) bad++;
                if (!is_page(p) && !near_rgb(p, fill[0], fill[1], fill[2], 3)) (*edge)++;
            }
        }
    return bad;
}

static void run(Fix *fx) {
    wait_draws(2);
    int n = 0;
    CHECK(win()->tree->nchildren == 2, "two columns");
    collect(win()->tree->children[0], &n);
    collect(win()->tree->children[1], &n);
    CHECK(n == 20, "found %d boxes, wanted 20", n);
    if (n != 20) return;
    uint8_t *px = frame();
    CHECK(px != NULL, "no frame");
    if (!px) return;
    int pw = app->capture_w, edge = 0, bad;
    static const int red[3] = { 255, 0, 0 };

    static const float r0[4] = { 20, 0, 10, 5 }, r1[4] = { 20, 20, 6, 6 }, r2[4] = { 6, 6, 6, 6 }, r3[4] = { 15, 15, 15, 15 }, r5[4] = { 12, 2, 2, 12 };
    bad = check_rr(px, pw, box_of(0), r0, red, &edge);
    CHECK(bad == 0 && edge > 15, "four different corners: %d pixels off, %d edge pixels", bad, edge);
    bad = check_rr(px, pw, box_of(1), r1, red, &edge);
    CHECK(bad == 0 && edge > 20, "gp_rounded then gp_rounded_top: %d pixels off", bad);
    bad = check_rr(px, pw, box_of(2), r2, red, &edge);
    CHECK(bad == 0, "gp_rounded_top then gp_rounded: all six, the later one wins (%d pixels off)", bad);
    bad = check_rr(px, pw, box_of(3), r3, red, &edge);
    CHECK(bad == 0 && edge > 15, "radii that do not fit are scaled together: 40 on a 60x30 box is 15 (%d pixels off)", bad);
    bad = check_rr(px, pw, box_of(5), r5, red, &edge);
    CHECK(bad == 0, "gp_rounded_left and _right: %d pixels off", bad);
    {   /* a gradient follows the corners: the cut corners show the page, the others the gradient */
        Box b = box_of(4);
        CHECK(is_page(px_at(px, pw, b.x0, b.y0)) && is_page(px_at(px, pw, b.x0, b.y0 + b.h - 1)) && !is_page(px_at(px, pw, b.x0 + b.w - 1, b.y0)) && !is_page(px_at(px, pw, b.x0 + b.w - 1, b.y0 + b.h - 1)),
              "gradient: corners 30,0,0,30");
    }
    {   /* an image: only the top-left is cut */
        Box b = box_of(6);
        CHECK(is_page(px_at(px, pw, b.x0, b.y0)) && near_rgb(px_at(px, pw, b.x0 + 39, b.y0), 0, 200, 0, 3) && near_rgb(px_at(px, pw, b.x0 + 39, b.y0 + 39), 0, 200, 0, 3) && near_rgb(px_at(px, pw, b.x0, b.y0 + 39), 0, 200, 0, 3),
              "an image rounded at the top-left only");
    }

    /* borders on some sides: 7 has left 2, top 6, right 10, bottom none */
    Box s = box_of(7);
    int mid = s.y0 + 25, cx = s.x0 + 40;
    CHECK(near_rgb(px_at(px, pw, s.x0, mid), 255, 255, 255, 2) && near_rgb(px_at(px, pw, s.x0 + 1, mid), 255, 255, 255, 2) && near_rgb(px_at(px, pw, s.x0 + 2, mid), 0, 0, 255, 3),
          "left border 2 wide");
    CHECK(near_rgb(px_at(px, pw, cx, s.y0), 255, 255, 255, 2) && near_rgb(px_at(px, pw, cx, s.y0 + 5), 255, 255, 255, 2) && near_rgb(px_at(px, pw, cx, s.y0 + 6), 0, 0, 255, 3),
          "top border 6 high");
    CHECK(near_rgb(px_at(px, pw, s.x0 + 79, mid), 255, 255, 255, 2) && near_rgb(px_at(px, pw, s.x0 + 70, mid), 255, 255, 255, 2) && near_rgb(px_at(px, pw, s.x0 + 69, mid), 0, 0, 255, 3),
          "right border 10 wide");
    CHECK(near_rgb(px_at(px, pw, cx, s.y0 + 49), 0, 0, 255, 3) && near_rgb(px_at(px, pw, cx, s.y0 + 48), 0, 0, 255, 3), "and none at the bottom");
    /* gp_border then gp_border_left: left 10, the others 3; the reverse: all 3 */
    Box s8 = box_of(8), s9 = box_of(9);
    CHECK(near_rgb(px_at(px, pw, s8.x0 + 9, s8.y0 + 25), 255, 255, 255, 2) && near_rgb(px_at(px, pw, s8.x0 + 10, s8.y0 + 25), 0, 0, 255, 3) && near_rgb(px_at(px, pw, s8.x0 + 40, s8.y0 + 2), 255, 255, 255, 2) &&
              near_rgb(px_at(px, pw, s8.x0 + 40, s8.y0 + 3), 0, 0, 255, 3) && near_rgb(px_at(px, pw, s8.x0 + 77, s8.y0 + 25), 255, 255, 255, 2) && near_rgb(px_at(px, pw, s8.x0 + 76, s8.y0 + 25), 0, 0, 255, 3),
          "gp_border(3) then gp_border_left(10)");
    CHECK(near_rgb(px_at(px, pw, s9.x0 + 2, s9.y0 + 25), 255, 255, 255, 2) && near_rgb(px_at(px, pw, s9.x0 + 3, s9.y0 + 25), 0, 0, 255, 3), "gp_border_left(10) then gp_border(3): all three, the later wins");

    /* layout: a border on a side is padding */
    CHECK(els[10]->size.w == 8 + 4 + 20 + 4 && els[10]->size.h == 3 + 4 + 20 + 4 + 3, "sizes follow borders and padding: %.1fx%.1f", els[10]->size.w, els[10]->size.h);
    CHECK(els[10]->children[0]->pos.x == 12 && els[10]->children[0]->pos.y == 7, "the child sits inside both: %.1f,%.1f", els[10]->children[0]->pos.x, els[10]->children[0]->pos.y);
    CHECK(els[11]->size.w == 8 + 4 + 20 + 4 && els[11]->size.h == 4 + 20 + 4 && els[12]->size.w == els[11]->size.w && els[12]->size.h == els[11]->size.h &&
              els[12]->children[0]->pos.x == els[11]->children[0]->pos.x,
          "padding and a side border in either order: the same box (%.1fx%.1f | %.1fx%.1f)", els[11]->size.w, els[11]->size.h, els[12]->size.w, els[12]->size.h);
    {   /* and drawn: the border band is 8 wide, the padding 4 (blue), the child red */
        Box b = box_of(11);
        CHECK(near_rgb(px_at(px, pw, b.x0 + 5, b.y0 + 12), 0, 0, 255, 3) && near_rgb(px_at(px, pw, b.x0 + 14, b.y0 + 14), 255, 0, 0, 3) && near_rgb(px_at(px, pw, b.x0 + 11, b.y0 + 14), 0, 0, 255, 3),
              "the content is where the border and padding leave it");
    }

    /* corners and sides together: the corner is cut; along the straight parts the left strip is white, the top edge is not */
    Box c = box_of(13);
    CHECK(is_page(px_at(px, pw, c.x0, c.y0)) && near_rgb(px_at(px, pw, c.x0, c.y0 + 25), 255, 255, 255, 3) && near_rgb(px_at(px, pw, c.x0 + 3, c.y0 + 25), 255, 255, 255, 3) &&
              near_rgb(px_at(px, pw, c.x0 + 5, c.y0 + 25), 0, 0, 255, 3) && near_rgb(px_at(px, pw, c.x0 + 40, c.y0), 0, 0, 255, 3) && near_rgb(px_at(px, pw, c.x0 + 40, c.y0 + 25), 0, 0, 255, 3),
          "rounded with left and right borders: the straight part of the top has none");
    /* a border on rounded corners: the ring is the box less the padding box, whose corners are the radii less the average of the two
     * borders at each corner. (Four cases: uniform with one radius, uniform with four, and a border on one side only.) */
    static const struct { int el; float outer[4]; float ox, oy, dw, dh; float inner[4]; } cases[] = {
        { 14, { 20, 20, 20, 20 }, 5, 5, 10, 10, { 15, 15, 15, 15 } },
        { 15, { 24, 4, 12, 0 }, 5, 5, 10, 10, { 19, 0, 7, 0 } },
        { 16, { 20, 20, 20, 20 }, 0, 0, 10, 0, { 20, 15, 15, 20 } },   /* a border on the right only: the corners on that side are reduced by half of it */
    };
    for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
        Box b = box_of(cases[k].el);
        int off = 0, edge_px = 0, ring = 0;
        float iw = (float)b.w - cases[k].dw, ih = (float)b.h - cases[k].dh;
        for (int y = 0; y < b.h; y++)
            for (int x = 0; x < b.w; x++) {
                float co = rr_coverage(x, y, (float)b.w, (float)b.h, cases[k].outer);
                int in = 0;
                for (int sy = 0; sy < 8; sy++)
                    for (int sx = 0; sx < 8; sx++) in += in_rr((float)x + (sx + 0.5f) / 8 - cases[k].ox, (float)y + (sy + 0.5f) / 8 - cases[k].oy, iw, ih, cases[k].inner);
                float ci = (float)in / 64.f;
                const uint8_t *p = px_at(px, pw, b.x0 + x, b.y0 + y);
                bool solid = (co == 1.f || co == 0.f) && (ci == 1.f || ci == 0.f);
                for (int d = 0; d < 4 && solid; d++) {
                    int nx = x + (d == 0) - (d == 1), ny = y + (d == 2) - (d == 3);
                    int in2 = 0;
                    for (int sy = 0; sy < 8; sy++)
                        for (int sx = 0; sx < 8; sx++) in2 += in_rr((float)nx + (sx + 0.5f) / 8 - cases[k].ox, (float)ny + (sy + 0.5f) / 8 - cases[k].oy, iw, ih, cases[k].inner);
                    solid = rr_coverage(nx, ny, (float)b.w, (float)b.h, cases[k].outer) == co && (float)in2 / 64.f == ci;
                }
                float white = 255 * (1 - ci);   /* the ring is white, the inside blue; the page shows outside the box */
                int want[3] = { (int)lroundf(white * co + 20 * (1 - co)), (int)lroundf(white * co + 20 * (1 - co)), (int)lroundf((white + 255 * ci) * co + 30 * (1 - co)) };
                if (solid) {
                    if (!near_rgb(p, want[0], want[1], want[2], 3)) off++;
                } else if (!near_rgb(p, want[0], want[1], want[2], 80)) {
                    off++;
                } else {
                    edge_px++;
                }
                if (solid && co == 1.f && ci == 0.f) ring++;
            }
        CHECK(off == 0 && edge_px > 30 && ring > 60, "bordered rounded box %d: %d pixels off, %d edge pixels, %d ring pixels", cases[k].el, off, edge_px, ring);
    }

    /* layout: a uniform border with one side changed, and plain padding */
    CHECK(els[17]->size.w == 10 + 2 + 20 + 2 + 3 && els[17]->size.h == 3 + 2 + 20 + 2 + 3, "gp_border(3) with gp_border_left(10) and padding 2: %.1fx%.1f", els[17]->size.w, els[17]->size.h);
    CHECK(els[18]->size.w == 30 && els[18]->size.h == 30 && els[18]->children[0]->pos.x == 5, "padding alone: 30x30, the child at 5 (%.1fx%.1f)", els[18]->size.w, els[18]->size.h);
    {   /* a rounded image inside a border: its radius is the element's less the border, so the picture fills the corner of the ring's inside */
        Box b = box_of(19);
        CHECK(near_rgb(px_at(px, pw, b.x0 + 11, b.y0 + 11), 0, 200, 0, 3) && near_rgb(px_at(px, pw, b.x0 + 2, b.y0 + 30), 255, 255, 255, 3) && is_page(px_at(px, pw, b.x0, b.y0)),
              "a rounded image in a border: the corner of its inside is the picture (%d,%d,%d)", px_at(px, pw, b.x0 + 11, b.y0 + 11)[0], px_at(px, pw, b.x0 + 11, b.y0 + 11)[1], px_at(px, pw, b.x0 + 11, b.y0 + 11)[2]);
    }
    (void)fx;
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    Fix *fx = gp_entity_get(app, win()->root_view);
    run(fx);
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    unsigned char green[4 * 4 * 4];
    for (int i = 0; i < 16; i++) { green[i * 4] = 0; green[i * 4 + 1] = 200; green[i * 4 + 2] = 0; green[i * 4 + 3] = 255; }
    gp_image solid = gp_image_from_rgba(app, 4, 4, green);
    gp_entity v = gp_view_new(app, sizeof(Fix), &(Fix){ .solid = solid }, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "box_test", .width = 260, .height = 820 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "box_test: %d FAILURE(S)\n" : "box_test: all checks passed\n", failures);
    return failures != 0;
}
