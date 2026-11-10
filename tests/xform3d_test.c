/*
 * 3D transforms (gp_rotate_x / _y, gp_translate_z, gp_transform_perspective, gp_perspective on a parent, gp_backface_hidden) and keys (gp_key), from frames read back
 * from the renderer: each box is checked against its corners projected by CSS's matrices, worked out again here (just inside every edge is the box, just outside it is
 * not); a turn without perspective only foreshortens; a move toward the viewer under a parent's perspective magnifies; a card's hidden back face is not drawn or hit; the
 * pointer lands where the box is seen; text in perspective is drawn; a box behind the viewer is not drawn. A keyed scroller keeps its offset when a sibling comes before it,
 * and one known by its place does not. Needs a display, at scale 1.
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
#define RED gp_rgb(0xff0000)
#define BLUE gp_rgb(0x0000ff)
#define GREEN gp_rgb(0x00ff00)
#define WHITE gp_rgb(0xffffff)

static gp_entity g_root;
static int g_extra;            /* 1: a sibling before the scrollers */
static int g_clicks[3];
static void on_click(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) { g_clicks[(intptr_t)data]++; }

static const gp_point sq[] = { { 10, 10 }, { 90, 10 }, { 90, 50 }, { 60, 30 }, { 10, 50 } };   /* a concave outline: three triangles, two shared edges */
#define ABS(x, y, w, h, ...) gp_div(gp_absolute(), gp_left(x), gp_top(y), gp_w(w), gp_h(h), __VA_ARGS__)

static gp_node *rows(void) {
    static gp_node *list[2][20];
    static int which;
    gp_node **l = list[which ^= 1];
    for (int i = 0; i < 20; i++) l[i] = gp_div(gp_h(20), gp_flex_none(), gp_bg(i % 2 ? WHITE : RED));
    return gp_children(l, 20);
}

static gp_node *render(void *self, gp_ctx *cx) {
    return gp_div(
        gp_size_full(), gp_bg(BLACK),
        /* A: rotate_y(60), no perspective: half as wide */
        ABS(20, 20, 100, 60, gp_bg(RED), gp_rotate_y(60)),
        /* B: its own perspective (200) and rotate_y(40) */
        ABS(180, 20, 100, 60, gp_bg(RED), gp_transform_perspective(200), gp_rotate_y(40), gp_on_click_data(on_click, (void *)0)),
        /* C: a parent with a perspective (300, seen from its top left), a child turned about x */
        ABS(340, 20, 200, 120, gp_perspective(300), gp_perspective_origin(0, 0), gp_div(gp_absolute(), gp_left(40), gp_top(20), gp_w(100), gp_h(60), gp_bg(GREEN), gp_rotate_x(50))),
        /* D: a move 100 toward the viewer under a perspective of 300: 1.5 times as big about the parent's middle */
        ABS(580, 20, 200, 160, gp_perspective(300), gp_div(gp_absolute(), gp_left(70), gp_top(60), gp_w(60), gp_h(40), gp_bg(BLUE), gp_translate_z(100))),
        /* E: a card: the back (blue) under, the front turned half round with its back hidden (and a child, also not drawn) */
        ABS(20, 200, 100, 60, gp_bg(BLUE), gp_on_click_data(on_click, (void *)1)),
        ABS(20, 200, 100, 60, gp_bg(RED), gp_rotate_y(180), gp_backface_hidden(), gp_on_click_data(on_click, (void *)2), gp_div(gp_w(20), gp_h(20), gp_bg(WHITE))),
        /* F: text in perspective */
        ABS(180, 200, 200, 60, gp_transform_perspective(300), gp_rotate_y(30), gp_text("Perspective", gp_text_size(28), gp_text_color(WHITE))),
        /* G: behind the viewer: not drawn */
        ABS(420, 200, 100, 60, gp_perspective(300), gp_div(gp_size_full(), gp_bg(WHITE), gp_translate_z(400))),
        /* I: a group in perspective (a layer: its region must hold what perspective makes of it) */
        ABS(560, 200, 100, 60, gp_opacity(0.5f), gp_transform_perspective(150), gp_rotate_y(-50), gp_bg(WHITE), gp_div(gp_w(20), gp_h(20), gp_bg(WHITE))),
        /* J: a fill in perspective: no seam between its triangles */
        ABS(680, 200, 100, 60, gp_transform_perspective(150), gp_rotate_x(40), gp_polyline(sq, 5, gp_closed(), gp_fill_color(WHITE), gp_stroke_width(0))),
        /* K: a gradient in strong perspective: inside one quad, what is where depends on the interpolation being perspective-correct */
        ABS(580, 380, 200, 60, gp_transform_perspective(150), gp_rotate_y(55), gp_bg_gradient(90, gp_stop(BLACK, 0), gp_stop(WHITE, 1))),
        /* H: two scrollers, one keyed, one known by its place, after a box that comes and goes */
        ABS(20, 320, 400, 120, gp_flex_row(), gp_gap(10),
            g_extra ? gp_div(gp_w(40), gp_h(40), gp_flex_none(), gp_bg(WHITE)) : NULL,
            gp_div(gp_w(100), gp_h(100), gp_flex_none(), gp_flex_col(), gp_overflow_y_scroll(), gp_key("keyed"), rows()),
            gp_div(gp_w(100), gp_h(100), gp_flex_none(), gp_flex_col(), gp_overflow_y_scroll(), rows())));
}

/* ---- the oracle: CSS's matrices, 4 x 4, row-major, applied to column vectors */
typedef struct { double m[4][4]; } M4;
static M4 m4_id(void) { M4 r = { { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } } }; return r; }
static M4 m4_mul(M4 a, M4 b) {
    M4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) { r.m[i][j] = 0; for (int k = 0; k < 4; k++) r.m[i][j] += a.m[i][k] * b.m[k][j]; }
    return r;
}
static M4 m4_t(double x, double y, double z) { M4 r = m4_id(); r.m[0][3] = x; r.m[1][3] = y; r.m[2][3] = z; return r; }
static M4 m4_rx(double deg) { double a = deg * M_PI / 180; M4 r = m4_id(); r.m[1][1] = cos(a); r.m[1][2] = -sin(a); r.m[2][1] = sin(a); r.m[2][2] = cos(a); return r; }
static M4 m4_ry(double deg) { double a = deg * M_PI / 180; M4 r = m4_id(); r.m[0][0] = cos(a); r.m[0][2] = sin(a); r.m[2][0] = -sin(a); r.m[2][2] = cos(a); return r; }
static M4 m4_p(double d) { M4 r = m4_id(); r.m[3][2] = -1 / d; return r; }
static gp_point proj(M4 m, double x, double y) {
    double v[4] = { x, y, 0, 1 }, o[4];
    for (int i = 0; i < 4; i++) o[i] = m.m[i][0] * v[0] + m.m[i][1] * v[1] + m.m[i][2] * v[2] + m.m[i][3] * v[3];
    return (gp_point){ (float)(o[0] / o[3]), (float)(o[1] / o[3]) };
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
    int ix = (int)floorf(x), iy = (int)floorf(y);
    if (ix < 0 || iy < 0 || ix >= app->capture_w || iy >= app->capture_h) { memset(o, 0, 3); return o; }
    memcpy(o, f + ((size_t)iy * (size_t)app->capture_w + (size_t)ix) * 3, 3);
    return o;
}
static bool rgb(const uint8_t *p, int r, int g, int b, int tol) { return abs(p[0] - r) <= tol && abs(p[1] - g) <= tol && abs(p[2] - b) <= tol; }
#define P3(p) (p)[0], (p)[1], (p)[2]

/* the quad q[0..4) (a box's corners through the oracle): 3 px inside each edge's middle is `c`, 3 px outside is black */
static void check_quad(const uint8_t *f, const gp_point q[4], gp_color c, const char *what) {
    gp_point m = { (q[0].x + q[1].x + q[2].x + q[3].x) / 4, (q[0].y + q[1].y + q[2].y + q[3].y) / 4 };
    int cr = (int)lroundf(c.r * 255), cg = (int)lroundf(c.g * 255), cb = (int)lroundf(c.b * 255);
    const uint8_t *p = pix(f, m.x, m.y);
    CHECK(rgb(p, cr, cg, cb, 2), "%s: the middle %d %d %d", what, P3(p));
    for (int e = 0; e < 4; e++) {
        gp_point a = q[e], b = q[(e + 1) % 4], mid = { (a.x + b.x) / 2, (a.y + b.y) / 2 };
        float dx = m.x - mid.x, dy = m.y - mid.y, l = hypotf(dx, dy);
        dx /= l; dy /= l;
        p = pix(f, mid.x + dx * 3, mid.y + dy * 3);
        const uint8_t *o = pix(f, mid.x - dx * 3, mid.y - dy * 3);
        CHECK(rgb(p, cr, cg, cb, 3), "%s: edge %d, inside: %d %d %d", what, e, P3(p));
        CHECK(rgb(o, 0, 0, 0, 3), "%s: edge %d, outside: %d %d %d", what, e, P3(o));
    }
}
static void corners(M4 m, double x, double y, double w, double h, gp_point q[4]) {
    q[0] = proj(m, x, y); q[1] = proj(m, x + w, y); q[2] = proj(m, x + w, y + h); q[3] = proj(m, x, y + h);
}

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
    gp_point q[4];
    const uint8_t *p;

    /* A: about its middle (70, 50) */
    corners(m4_mul(m4_t(70, 50, 0), m4_mul(m4_ry(60), m4_t(-70, -50, 0))), 20, 20, 100, 60, q);
    check_quad(f, q, RED, "A: rotate_y(60)");
    CHECK(fabsf(q[1].x - q[0].x - 50.f) < 0.01f, "A: the oracle says half as wide (%g)", q[1].x - q[0].x);

    /* B: perspective(200) after rotate_y(40), about (230, 50) */
    M4 mb = m4_mul(m4_t(230, 50, 0), m4_mul(m4_p(200), m4_mul(m4_ry(40), m4_t(-230, -50, 0))));
    corners(mb, 180, 20, 100, 60, q);
    check_quad(f, q, RED, "B: its own perspective");
    CHECK(fabsf((q[2].y - q[1].y) - (q[3].y - q[0].y)) > 5.f, "B: the oracle has one side taller than the other (%g, %g)", q[3].y - q[0].y, q[2].y - q[1].y);
    gp_point b_far = q[1];   /* the far side's top corner: smaller than the box was */

    /* C: the child's origin (430, 70); the parent's perspective origin its top left (340, 20) */
    M4 mc = m4_mul(m4_t(340, 20, 0), m4_mul(m4_p(300), m4_mul(m4_t(430 - 340, 70 - 20, 0), m4_mul(m4_rx(50), m4_t(-430, -70, 0)))));
    corners(mc, 380, 40, 100, 60, q);
    check_quad(f, q, GREEN, "C: a parent's perspective from its corner");

    /* D: the child's origin (680, 100) is the parent's middle: 1.5 times about it */
    M4 md = m4_mul(m4_t(680, 100, 0), m4_mul(m4_p(300), m4_mul(m4_t(0, 0, 100), m4_t(-680, -100, 0))));
    corners(md, 650, 80, 60, 40, q);
    CHECK(fabsf((q[1].x - q[0].x) - 90.f) < 0.01f, "D: the oracle has it 90 wide (%g)", q[1].x - q[0].x);
    check_quad(f, q, BLUE, "D: moved toward the viewer");

    /* E: the front is turned away and hidden: the back shows, the front's child too is not drawn */
    p = pix(f, 70, 230);
    const uint8_t *c = pix(f, 30, 210);
    CHECK(rgb(p, 0, 0, 255, 2) && rgb(c, 0, 0, 255, 2), "E: the hidden face is not drawn: %d %d %d, its child's place %d %d %d", P3(p), P3(c));

    /* F: text in perspective: some ink in its box, none past it */
    int ink = 0;
    for (int y = 200; y < 260; y++)
        for (int x = 180; x < 380; x++) ink += pix(f, (float)x, (float)y)[0] > 128;
    CHECK(ink > 200, "F: text in perspective has %d inked pixels", ink);

    /* G: behind the viewer: nothing in or around its box */
    int lit = 0;
    for (int y = 180; y < 300; y += 2)
        for (int x = 400; x < 560; x += 2) lit += pix(f, (float)x, (float)y)[0] > 10;
    CHECK(lit == 0, "G: %d lit pixels around a box behind the viewer", lit);

    /* I: the group's quad, at half opacity */
    {
        M4 mi = m4_mul(m4_t(610, 230, 0), m4_mul(m4_p(150), m4_mul(m4_ry(-50), m4_t(-610, -230, 0))));
        corners(mi, 560, 200, 100, 60, q);
        check_quad(f, q, (gp_color){ 0.5f, 0.5f, 0.5f, 1 }, "I: a group in perspective");
    }
    /* J: every pixel well inside the fill is white (a seam between triangles would show as a darker line) */
    {
        M4 mj = m4_mul(m4_t(730, 230, 0), m4_mul(m4_p(150), m4_mul(m4_rx(40), m4_t(-730, -230, 0))));
        int bad = 0, n = 0;
        for (int k = 0; k < 40; k++)
            for (int j = 0; j < 8; j++) {   /* across the band above the notch (y 12..28 of the outline), which the triangles' shared edges cross */
                double lx = 690 + 2 + 76 * k / 39.0, ly = 200 + 12 + 16 * j / 7.0;
                gp_point s0 = proj(mj, lx, ly);
                n++;
                bad += !rgb(pix(f, s0.x, s0.y), 255, 255, 255, 0);
            }
        CHECK(bad == 0, "J: %d of %d pixels inside a fill in perspective are not white", bad, n);
    }

    /* K: a quarter and three quarters of the way along: 64 and 191 (a gradient interpolated flat across the screen would not be) */
    {
        M4 mk = m4_mul(m4_t(680, 410, 0), m4_mul(m4_p(150), m4_mul(m4_ry(55), m4_t(-680, -410, 0))));
        for (int k = 1; k <= 3; k += 2) {
            gp_point s0 = proj(mk, 580 + 200 * k / 4.0, 410);
            p = pix(f, s0.x, s0.y);
            CHECK(abs(p[0] - (int)lround(255 * k / 4.0)) <= 8, "K: the gradient %d/4 along, where perspective puts it: %d", k, p[0]);
        }
    }

    /* the pointer: B is hit inside its seen quad and not where its box was but is no more (the far top corner); the card's hidden face is not hit */
    corners(mb, 180, 20, 100, 60, q);
    gp_point mid = { (q[0].x + q[2].x) / 2, (q[0].y + q[2].y) / 2 };
    click(mid.x, mid.y);
    click(279, 21);   /* inside the box it had, outside the far side's corner (b_far is lower and to the left) */
    CHECK(b_far.x < 276 && b_far.y > 24, "the oracle's far corner %g %g", b_far.x, b_far.y);
    CHECK(g_clicks[0] == 1, "B: %d clicks, want 1 (the middle, not the corner it no longer covers)", g_clicks[0]);
    click(70, 230);
    CHECK(g_clicks[1] == 1 && g_clicks[2] == 0, "E: the back %d, the hidden front %d", g_clicks[1], g_clicks[2]);

    /* H: both scrollers 40 down; then a box comes before them: the keyed one stays at 40, the other starts over */
    gp_ctx cx = gp_ctx_for(app, g_root);
    gp_scroll_wheel(win(), (gp_point){ 20 + 50, 370 }, 0, 40);
    gp_scroll_wheel(win(), (gp_point){ 20 + 110 + 50, 370 }, 0, 20);   /* (another amount: the keyed one must not take the other's place and its offset) */
    wait_draws(2);
    gp_el *h = win()->tree->children[11];
    float keyed_before = h->children[0]->scroll.y;
    CHECK(h->nchildren == 2 && keyed_before > 0 && h->children[1]->scroll.y > 0, "H: scrolled to %g and %g", keyed_before, h->nchildren > 1 ? h->children[1]->scroll.y : -1.f);
    float place_before = h->children[1]->scroll.y;
    g_extra = 1;
    gp_notify(&cx);
    wait_draws(2);
    h = win()->tree->children[11];
    CHECK(h->nchildren == 3 && h->children[1]->scroll.y == keyed_before, "H: the keyed scroller after a box came before it: %g (was %g)", h->children[1]->scroll.y, keyed_before);
    CHECK(h->nchildren == 3 && h->children[2]->scroll.y != place_before, "H: the one known by its place is another now: %g (was %g)", h->children[2]->scroll.y, place_before);
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    run();
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    g_root = gp_view_new(app, 1, &(char){ 0 }, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "xform3d_test", .width = 800, .height = 480 }, g_root);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "xform3d_test: %d FAILURE(S)\n" : "xform3d_test: all checks passed\n", failures);
    return failures != 0;
}
