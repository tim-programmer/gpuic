/*
 * 3D contexts (gp_preserve_3d), from frames read back from the renderer: a plane nearer the viewer is drawn over one painted after it; two planes that go through
 * each other each show where they are the nearer (cut along the line where they meet), whichever is first; a perspective reaches grandchildren through an element
 * that preserves 3D (its quad checked against CSS's matrices) and not through one that does not; an overflow clip flattens; a click lands on the nearest plane where
 * it is. Needs a display, at scale 1.
 */
#include "../src/internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...)                                \
    do {                                                \
        if(!(cond)) {                                   \
            failures++;                                 \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while(0)

static gp_app* app;
static gp_co* co_;
static gp_window* win(void)
{
    return app->windows[0];
}

#define BLACK gp_rgb(0x000000)
#define RED   gp_rgb(0xff0000)
#define BLUE  gp_rgb(0x0000ff)
#define GREEN gp_rgb(0x00ff00)

static int g_clicks[4];
static void on_click(void* self, gp_ctx* cx, const gp_mouse_event* ev, void* data)
{
    g_clicks[(intptr_t)data]++;
}
#define ABS(x, y, w, h, ...) gp_div(gp_absolute(), gp_left(x), gp_top(y), gp_w(w), gp_h(h), __VA_ARGS__)

/* A: red, nearer (z 20) but first, and blue over its lower right quarter: in a context red is on top, flat (or flattened by a clip) blue is */
static gp_node* overlap(float x, gp_node* flatten, gp_node* p3)
{
    return ABS(x, 20, 100, 100, p3, flatten, ABS(0, 0, 80, 80, gp_bg(RED), gp_translate_z(20), gp_on_click_data(on_click, (void*)0)), ABS(20, 20, 80, 80, gp_bg(BLUE), gp_on_click_data(on_click, (void*)1)));
}
/* B: red flat and blue turned 45 degrees about y through it: blue is nearer on the left of the middle, red on the right */
static gp_node* crossing(float x, bool blue_first)
{
    gp_node* red = ABS(0, 0, 100, 100, gp_bg(RED), gp_on_click_data(on_click, (void*)2));
    gp_node* blue = ABS(0, 0, 100, 100, gp_bg(BLUE), gp_rotate_y(45), gp_on_click_data(on_click, (void*)3));
    return ABS(x, 160, 100, 100, gp_preserve_3d(), blue_first ? blue : red, blue_first ? red : blue);
}
/* C: a perspective on the grandparent: through a child that preserves 3D the grandchild is in it, else only foreshortened */
static gp_node* grand(float x, bool p3)
{
    return ABS(x, 300, 200, 160, gp_perspective(300), gp_div(gp_absolute(), gp_left(0), gp_top(0), gp_size_full(), p3 ? gp_preserve_3d() : NULL, ABS(50, 50, 100, 60, gp_bg(GREEN), gp_rotate_y(40))));
}

static gp_node* render(void* self, gp_ctx* cx)
{
    return gp_div(gp_size_full(), gp_bg(BLACK), overlap(20, NULL, gp_preserve_3d()), overlap(160, NULL, NULL), overlap(300, gp_overflow_hidden(), gp_preserve_3d()), crossing(20, false), crossing(160, true), grand(300, true), grand(540, false));
}

/* ---- the oracle: CSS's matrices, 4 x 4, applied to column vectors */
typedef struct
{
    double m[4][4];
} M4;
static M4 m4_id(void)
{
    M4 r = {
        { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } }
    };
    return r;
}
static M4 m4_mul(M4 a, M4 b)
{
    M4 r;
    for(int i = 0; i < 4; i++) {
        for(int j = 0; j < 4; j++) {
            r.m[i][j] = 0;
            for(int k = 0; k < 4; k++) {
                r.m[i][j] += a.m[i][k] * b.m[k][j];
            }
        }
    }
    return r;
}
static M4 m4_t(double x, double y)
{
    M4 r = m4_id();
    r.m[0][3] = x;
    r.m[1][3] = y;
    return r;
}
static M4 m4_ry(double deg)
{
    double a = deg * M_PI / 180;
    M4 r = m4_id();
    r.m[0][0] = cos(a);
    r.m[0][2] = sin(a);
    r.m[2][0] = -sin(a);
    r.m[2][2] = cos(a);
    return r;
}
static M4 m4_p(double d)
{
    M4 r = m4_id();
    r.m[3][2] = -1 / d;
    return r;
}
static gp_point proj(M4 m, double x, double y)
{
    double o[4];
    for(int i = 0; i < 4; i++) {
        o[i] = m.m[i][0] * x + m.m[i][1] * y + m.m[i][3];
    }
    return (gp_point){ (float)(o[0] / o[3]), (float)(o[1] / o[3]) };
}

static bool wait_draws(unsigned n)
{
    unsigned long target = app->stat_draws + n;
    for(int i = 0; i < 1000 && app->stat_draws < target; i++) {
        if(i > 5) {
            win()->dirty_paint = true;
        }
        gp_sleep(co_, 0.004);
    }
    return app->stat_draws >= target;
}
static uint8_t* frame(void)
{
    gp_debug_capture_next(app);
    for(int i = 0; i < 1000 && !app->capture_px; i++) {
        win()->dirty_paint = true;
        gp_sleep(co_, 0.01);
    }
    return app->capture_px;
}
static const uint8_t* pix(const uint8_t* f, float x, float y)
{
    static uint8_t o[4][3];
    static int k;
    uint8_t* p = o[k++ & 3];
    int ix = (int)floorf(x), iy = (int)floorf(y);
    if(!f || ix < 0 || iy < 0 || ix >= app->capture_w || iy >= app->capture_h) {
        memset(p, 0, 3);
        return p;
    }
    memcpy(p, f + ((size_t)iy * (size_t)app->capture_w + (size_t)ix) * 3, 3);
    return p;
}
static bool rgb(const uint8_t* p, int r, int g, int b)
{
    return abs(p[0] - r) <= 3 && abs(p[1] - g) <= 3 && abs(p[2] - b) <= 3;
}
#define P3(p) (p)[0], (p)[1], (p)[2]

/* the box (x, y, w, h) through m: 3 px inside each edge's middle is green, 3 px outside black */
static void check_quad(const uint8_t* f, M4 m, double x, double y, double w, double h, const char* what)
{
    gp_point q[4] = { proj(m, x, y), proj(m, x + w, y), proj(m, x + w, y + h), proj(m, x, y + h) };
    gp_point c = { (q[0].x + q[1].x + q[2].x + q[3].x) / 4, (q[0].y + q[1].y + q[2].y + q[3].y) / 4 };
    for(int e = 0; e < 4; e++) {
        gp_point a = q[e], b = q[(e + 1) % 4], mid = { (a.x + b.x) / 2, (a.y + b.y) / 2 };
        float dx = c.x - mid.x, dy = c.y - mid.y, l = hypotf(dx, dy);
        const uint8_t *in = pix(f, mid.x + dx / l * 3, mid.y + dy / l * 3), *out = pix(f, mid.x - dx / l * 3, mid.y - dy / l * 3);
        CHECK(rgb(in, 0, 255, 0) && rgb(out, 0, 0, 0), "%s: edge %d: inside %d %d %d, outside %d %d %d", what, e, P3(in), P3(out));
    }
}

static void click(float x, float y)
{
    gp_simulate_mouse(app, GP_SIM_DOWN, x, y, 1, 0);
    gp_simulate_mouse(app, GP_SIM_UP, x, y, 1, 0);
    gp_sleep(co_, 0.03);
    wait_draws(1);
}

static void run(void)
{
    wait_draws(3);
    const uint8_t* f = frame();
    CHECK(f != NULL, "a frame");

    const uint8_t* p = pix(f, 90, 90);
    CHECK(rgb(p, 255, 0, 0), "A: in a context the nearer red is over the blue painted after it: %d %d %d", P3(p));
    p = pix(f, 230, 90);
    CHECK(rgb(p, 0, 0, 255), "A, flat: blue, painted after, is over red: %d %d %d", P3(p));
    p = pix(f, 370, 90);
    CHECK(rgb(p, 0, 0, 255), "A, with an overflow clip (which flattens): blue over red: %d %d %d", P3(p));

    for(int k = 0; k < 2; k++) {
        float x0 = k ? 160.f : 20.f;
        const uint8_t *l = pix(f, x0 + 30, 210), *r = pix(f, x0 + 70, 210);
        CHECK(rgb(l, 0, 0, 255) && rgb(r, 255, 0, 0), "B (%s first): blue on the left of where they meet, red on the right: %d %d %d / %d %d %d", k ? "blue" : "red", P3(l), P3(r));
        l = pix(f, x0 + 47, 210), r = pix(f, x0 + 53, 210);
        CHECK(rgb(l, 0, 0, 255) && rgb(r, 255, 0, 0), "B (%s first): ... up to the line: %d %d %d / %d %d %d", k ? "blue" : "red", P3(l), P3(r));
    }

    /* C: the grandchild's box (absolute 350, 350, 100 x 60; its origin its middle, the perspective's the grandparent's middle: both 400, 380) */
    M4 turn = m4_mul(m4_t(400, 380), m4_mul(m4_ry(40), m4_t(-400, -380)));
    check_quad(f, m4_mul(m4_t(400, 380), m4_mul(m4_p(300), m4_mul(m4_ry(40), m4_t(-400, -380)))), 350, 350, 100, 60, "C: through a child that preserves 3D, in the grandparent's perspective");
    M4 flat = m4_mul(m4_t(240, 0), m4_mul(turn, m4_t(-240, 0)));
    check_quad(f, flat, 590, 350, 100, 60, "C, flat: only foreshortened");

    click(90, 90);
    CHECK(g_clicks[0] == 1 && g_clicks[1] == 0, "A: a click where red is nearer is red's (%d), not blue's (%d)", g_clicks[0], g_clicks[1]);
    click(50, 210);
    click(90, 210);
    CHECK(g_clicks[3] == 1 && g_clicks[2] == 1, "B: left of the line blue's (%d), right of it red's (%d)", g_clicks[3], g_clicks[2]);
}

static void driver(gp_co* co, void* self, gp_ctx* cx, void* data)
{
    co_ = co;
    run();
    gp_quit(app);
}

static void ready(gp_app* a, void* userdata)
{
    app = a;
    gp_entity root = gp_view_new(app, 1, &(char){ 0 }, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "preserve3d_test", .width = 800, .height = 480 }, root);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void)
{
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "preserve3d_test: %d FAILURE(S)\n" : "preserve3d_test: all checks passed\n", failures);
    return failures != 0;
}
