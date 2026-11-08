/*
 * Paint-time transforms (gp_rotate, gp_scale, gp_scale_xy, gp_translate, gp_transform_origin), from frames read back from the renderer: a box turned a
 * quarter lands exactly where geometry puts it; at 45 degrees every pixel of the diamond agrees with an independent supersampled coverage; scale and
 * translate move and size it about the right point; origin moves the pivot; nested transforms compose; text turns with its box; a click and a
 * hover land on what is seen (and not on where the box was), with the position in the element's own coordinates; an overflow clip follows a transform
 * that keeps the axes; a cached view under a turning parent is the frame a full paint gives; and gp_with_animation can turn something (the clock is
 * jumped). Needs a display, at scale 1.
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

#define BG 0x101018
#define RED gp_rgb(0xff0000)
#define GREEN gp_rgb(0x00ff00)
#define BLUE gp_rgb(0x4040ff)

typedef struct { int clicks; gp_point local; int click_id; } State;
static State *st;
static float g_angle;
static bool g_anim2_on;     /* the cell with the cached view turns by animation (the cached view itself is not rebuilt) */
static int64_t g_skew;      /* the jumped clock: only ever forward (a frame already asked for would wait for the old time to come round again) */
static void jump_by(double seconds) {
    g_skew += (int64_t)(seconds * 1e9);
    gp_debug_clock_skew(g_skew);
}
static bool g_anim_on;      /* the animated box is there only for its own check: it would turn between two frames */       /* the cached-view scene's parent angle */
static gp_entity g_cached;

static void on_click(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    st->clicks++;
    st->local = ev->local;
    st->click_id = (int)(intptr_t)data;
}

static gp_node *spin(float t, void *data) { return gp_group(gp_rotate(t * 360.f)); }

static gp_node *cached_row(void *self, gp_ctx *cx) {
    return gp_div(gp_flex_row(), gp_gap(6), gp_w(110), gp_h(30), gp_items_center(), gp_bg(gp_rgb(0x203040)),
                  gp_div(gp_w(18), gp_h(18), gp_rounded(4), gp_bg(RED)), gp_text("spin", gp_text_color(gp_rgb(0xffffff))));
}

/* every scene is a box of its own at a known place: the cells are 200 x 150 */
#define CELL(x, y, ...) gp_div(gp_absolute(), gp_left(x), gp_top(y), gp_w(200), gp_h(150), __VA_ARGS__)
#define BOX(w, h, ...) gp_div(gp_w(w), gp_h(h), gp_bg(RED), ##__VA_ARGS__)

static gp_node *render(void *self, gp_ctx *cx) {
    return gp_div(
        gp_size_full(), gp_bg(gp_rgb(BG)),
        /* 0: a quarter turn */ CELL(0, 0, gp_div(gp_absolute(), gp_left(70), gp_top(60), BOX(60, 20, gp_rotate(90)))),
        /* 1: 45 degrees */ CELL(200, 0, gp_div(gp_absolute(), gp_left(70), gp_top(50), BOX(60, 40, gp_rotate(45)))),
        /* 2: scale about the middle */ CELL(400, 0, gp_div(gp_absolute(), gp_left(80), gp_top(60), BOX(40, 20, gp_scale(2)))),
        /* 3: scale_xy and translate */ CELL(0, 150, gp_div(gp_absolute(), gp_left(80), gp_top(60), BOX(40, 20, gp_scale_xy(2, 0.5f), gp_translate(30, 10)))),
        /* 4: origin at the corner */ CELL(200, 150, gp_div(gp_absolute(), gp_left(100), gp_top(60), BOX(60, 20, gp_transform_origin(0, 0), gp_rotate(90)))),
        /* 5: nested: a quarter turn of a box holding a box that is turned a quarter */
        CELL(400, 150, gp_div(gp_absolute(), gp_left(60), gp_top(50), gp_w(80), gp_h(40), gp_rotate(90),
                              gp_div(gp_w(80), gp_h(40), gp_bg(gp_rgb(0x305030)),
                                     gp_div(gp_absolute(), gp_left(0), gp_top(0), BOX(30, 10, gp_rotate(90)))))),
        /* 6: text turned a quarter */ CELL(0, 300, gp_div(gp_absolute(), gp_left(50), gp_top(60), gp_w(100), gp_h(24), gp_rotate(90),
                                                         gp_text("Rotated text", gp_text_color(gp_rgb(0xffffff))))),
        /* 7: the same, not turned (the reference for its ink) */ CELL(200, 300, gp_div(gp_absolute(), gp_left(50), gp_top(60), gp_w(100), gp_h(24),
                                                         gp_text("Rotated text", gp_text_color(gp_rgb(0xffffff))))),
        /* 8: a click and a hover on a turned box */ CELL(400, 300, gp_div(gp_absolute(), gp_left(70), gp_top(60), BOX(60, 20, gp_rotate(90), gp_on_click_data(on_click, (void *)(intptr_t)8), gp_hover_bg(GREEN)))),
        /* 9: an overflow clip on a turned box (a quarter): the child is cut to the turned box */
        CELL(0, 450, gp_div(gp_absolute(), gp_left(70), gp_top(60), gp_w(60), gp_h(20), gp_overflow_hidden(), gp_rotate(90), gp_bg(gp_rgb(0x305030)),
                            gp_div(gp_absolute(), gp_left(-20), gp_top(-20), gp_w(100), gp_h(60), gp_bg(BLUE), gp_on_click_data(on_click, (void *)(intptr_t)9)))),
        /* 10: a cached view under a parent that turns */ CELL(200, 450, gp_div(gp_absolute(), gp_left(40), gp_top(60), gp_w(110), gp_h(30), g_anim2_on ? gp_with_animation("spin2", (gp_animation){ .duration = 10000 }, spin, NULL) : gp_rotate(g_angle), gp_child_view_cached(g_cached))),
        /* 12: origin at the top right corner */ CELL(0, 600, gp_div(gp_absolute(), gp_left(100), gp_top(60), BOX(60, 20, gp_transform_origin(1, 0), gp_rotate(90)))),
        /* 11: animated */ CELL(400, 450, gp_div(gp_absolute(), gp_left(70), gp_top(60), gp_w(60), gp_h(20),
                                                  g_anim_on ? gp_with_animation("spin", (gp_animation){ .duration = 100 }, spin, NULL) : NULL, gp_bg(RED))));
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
static bool is_c(const uint8_t *p, int r, int g, int b) { return abs(p[0] - r) < 10 && abs(p[1] - g) < 10 && abs(p[2] - b) < 10; }
static bool is_red(const uint8_t *p) { return is_c(p, 255, 0, 0); }
static bool is_bg(const uint8_t *p) { return is_c(p, 0x10, 0x10, 0x18); }
/* how much of a red box covers the pixel (the background is nearly black) */
static float red_cov(const uint8_t *p) { return fminf(fmaxf(((float)p[0] - 16.f) / 239.f, 0.f), 1.f); }

/* the mean absolute difference of the red coverage from a supersampled one of a box w x h whose middle is (cx, cy) in a frame, turned by `deg` about its middle */
static int box_mismatch(const uint8_t *f, float cx, float cy, float w, float h, float deg, float sx, float sy, int x0, int y0, int x1, int y1, float tol) {
    float r = deg * (float)M_PI / 180.f, cs = cosf(r), sn = sinf(r);
    int bad = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            int in = 0;
            for (int j = 0; j < 8; j++)
                for (int i = 0; i < 8; i++) {
                    float fx = (float)x + ((float)i + 0.5f) / 8.f - cx, fy = (float)y + ((float)j + 0.5f) / 8.f - cy;
                    float lx = (fx * cs + fy * sn) / sx, ly = (-fx * sn + fy * cs) / sy;   /* back through the rotation and the scale */
                    in += fabsf(lx) <= w / 2 && fabsf(ly) <= h / 2;
                }
            float cov = (float)in / 64.f;
            if (fabsf(cov - red_cov(px(f, x, y))) > tol) bad++;
        }
    return bad;
}

static long ink(const uint8_t *f, int x0, int y0, int x1, int y1, int *bx0, int *by0, int *bx1, int *by1) {
    long n = 0;
    *bx0 = x1; *by0 = y1; *bx1 = x0; *by1 = y0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            const uint8_t *p = px(f, x, y);
            if (p[0] > 120 && p[1] > 120 && p[2] > 120) {
                n++;
                if (x < *bx0) *bx0 = x;
                if (y < *by0) *by0 = y;
                if (x > *bx1) *bx1 = x;
                if (y > *by1) *by1 = y;
            }
        }
    return n;
}

static void dump(const char *name) {
    if (!getenv("TRANSFORM_DUMP") || !app->capture_px) return;
    char path[200];
    snprintf(path, sizeof path, "/tmp/claude-1000/xf_%s.ppm", name);
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    fprintf(fp, "P6\n%d %d\n255\n", app->capture_w, app->capture_h);
    fwrite(app->capture_px, 3, (size_t)app->capture_w * (size_t)app->capture_h, fp);
    fclose(fp);
}

static void click_at(float x, float y) {
    gp_simulate_mouse(app, GP_SIM_MOVE, x, y, 0, 0);
    gp_simulate_mouse(app, GP_SIM_DOWN, x, y, 1, 0);
    gp_simulate_mouse(app, GP_SIM_UP, x, y, 1, 0);
    gp_sleep(co_, 0.03);
}

static void run(void) {
    wait_draws(2);
    gp_simulate_mouse(app, GP_SIM_MOVE, 590, 590, 0, 0);   /* the pointer away from everything */
    gp_sleep(co_, 0.03);
    uint8_t *f = frame();
    CHECK(f != NULL, "a frame");
    if (!f) return;
    dump("main");

    /* 0: 60 x 20 at (70,60) in the cell at (0,0): middle (100, 70); a quarter turn makes it 20 x 60: x 90..110, y 40..100 */
    CHECK(is_red(px(f, 100, 45)) && is_red(px(f, 100, 95)) && is_red(px(f, 91, 70)) && is_red(px(f, 108, 70)), "a quarter turn: red where the turned box is");
    CHECK(is_bg(px(f, 75, 70)) && is_bg(px(f, 125, 70)) && is_bg(px(f, 100, 35)) && is_bg(px(f, 100, 105)), "a quarter turn: nothing where the box was");
    CHECK(box_mismatch(f, 100, 70, 60, 20, 90, 1, 1, 60, 30, 140, 110, 0.06f) == 0, "a quarter turn is exact (%d pixels off)", box_mismatch(f, 100, 70, 60, 20, 90, 1, 1, 60, 30, 140, 110, 0.06f));

    /* 1: 60 x 40 at (70,50) in the cell at (200,0): middle (300, 70), turned 45 degrees */
    int bad = box_mismatch(f, 300, 70, 60, 40, 45, 1, 1, 240, 20, 360, 120, 0.09f);
    CHECK(bad <= 3, "45 degrees: %d pixels differ from the supersampled box", bad);
    CHECK(is_bg(px(f, 270, 52)) && is_red(px(f, 300, 70)), "45 degrees: the corner of the unturned box is empty");

    /* 2: 40 x 20 at (80,60) in the cell at (400,0): middle (500, 70), twice as big: 80 x 40 */
    bad = box_mismatch(f, 500, 70, 40, 20, 0, 2, 2, 440, 30, 560, 110, 0.06f);
    CHECK(bad == 0, "scale 2: %d pixels differ from an 80 x 40 box", bad);

    /* 3: 40 x 20 at (80,60) in the cell at (0,150): middle (100, 220), scaled 2 x 0.5 (80 x 10) and moved by (30, 10) */
    bad = box_mismatch(f, 130, 230, 40, 20, 0, 2, 0.5f, 70, 200, 200, 260, 0.45f);   /* (an edge across a stretched axis is softer than a pixel: see the README) */
    if (bad && getenv("TRANSFORM_DUMP")) {
        for (int y = 222; y < 238; y++) { for (int x = 86; x < 96; x++) fprintf(stderr, "%3d ", px(f, x, y)[0]); fprintf(stderr, "   y=%d\n", y); }
    }
    CHECK(bad == 0, "scale_xy and translate: %d pixels differ (80 x 10 about 130, 230)", bad);

    /* 4: 60 x 20 at (100,60) in the cell at (200,150), turned a quarter about its top left corner (300, 210): x 280..300, y 210..270 */
    CHECK(is_red(px(f, 290, 240)) && is_red(px(f, 282, 215)) && is_bg(px(f, 320, 215)) && is_bg(px(f, 290, 200)), "origin 0, 0: pivots about the corner (red at 290, 240; nothing at 320, 215)");

    /* 5: nested: 80 x 40 at (60,50) in the cell at (400,150): middle (500, 220); it is turned a quarter, the box inside it (30 x 10 at its top left) turned a quarter about its own middle too */
    CHECK(is_c(px(f, 500, 220), 0x30, 0x50, 0x30), "nested: the green box holds the middle");
    /* the outer box (80 x 40) turned 90 about (500,220): 40 x 80, x 480..520, y 180..260; its top left (460+... ) corner maps to its top right: (520, 180) */
    CHECK(is_c(px(f, 500, 255), 0x30, 0x50, 0x30) && is_bg(px(f, 530, 220)) && is_bg(px(f, 470, 220)), "nested: the outer box is turned a quarter (40 x 80)");
    /* the inner 30 x 10 box sits at the layout top left (460,200)-(490,210), its middle (475,205); the outer turn maps (dx,dy) -> (-dy, dx) about (500,220): (-25,-15) -> (15,-25): (515,195); its own turn makes it 10 x 30 then the outer turn makes it 30 x 10 again: x 500..530? */
    CHECK(is_red(px(f, 515, 195)), "nested: the inner box is where both turns put it (a red pixel at 515, 195)");

    /* 6, 7: text turned a quarter: its ink is the unturned ink with width and height swapped */
    int ax0, ay0, ax1, ay1, bx0, by0, bx1, by1;
    long turned = ink(f, 0, 300, 200, 450, &ax0, &ay0, &ax1, &ay1), plain = ink(f, 200, 300, 400, 450, &bx0, &by0, &bx1, &by1);
    CHECK(plain > 200 && turned > 200 && labs(turned - plain) * 100 < plain * 50, "turned text has the ink of the plain one (%ld, %ld pixels)", turned, plain);
    {   /* with LCD antialiasing on, turned text must not have colored fringes (its stripes are not the screen's) */
        long colored = 0;
        for (int y = 300; y < 450; y++)
            for (int x = 0; x < 200; x++) {
                const uint8_t *p = px(f, x, y);
                int mx = p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2]) : (p[1] > p[2] ? p[1] : p[2]), mn = p[0] < p[1] ? (p[0] < p[2] ? p[0] : p[2]) : (p[1] < p[2] ? p[1] : p[2]);
                colored += mx - mn > 40;
            }
        CHECK(colored == 0, "turned text has no color fringes (%ld colored pixels)", colored);
    }
    CHECK(abs((ax1 - ax0) - (by1 - by0)) <= 3 && abs((ay1 - ay0) - (bx1 - bx0)) <= 3, "turned text: its box is the plain one with width and height swapped (%d x %d against %d x %d)", ax1 - ax0, ay1 - ay0, bx1 - bx0, by1 - by0);

    /* 9: a clip on a turned box, 60 x 20 at (70,60) in the cell at (0,450): middle (100, 520), turned: 20 x 60, x 90..110, y 490..550; its child (blue, big) is cut to it */
    CHECK(is_c(px(f, 100, 495), 0x40, 0x40, 0xff) && is_c(px(f, 100, 545), 0x40, 0x40, 0xff) && is_bg(px(f, 80, 520)) && is_bg(px(f, 100, 480)) && is_bg(px(f, 100, 560)),
          "an overflow clip on a turned box is the turned box: blue inside 90..110 x 490..550, nothing outside");

    /* 8: click and hover on a turned box: 60 x 20 at (70,60) in the cell at (400,300): middle (500, 370), turned: 20 x 60 */
    st->clicks = 0;
    click_at(500, 345);   /* inside the turned box, outside the box where it was (it spans 470..530 x 360..380) */
    CHECK(st->clicks == 1 && st->click_id == 8, "a click on the turned box lands (%d)", st->clicks);
    /* the point is the turned box's (middle - 25 up): local in the 60 x 20 box: (5, 10) */
    CHECK(fabsf(st->local.x - 5.f) < 1.f && fabsf(st->local.y - 10.f) < 1.f, "its position is in the element's own coordinates: %.1f, %.1f (wanted 5, 10)", (double)st->local.x, (double)st->local.y);
    st->clicks = 0;
    click_at(475, 370);   /* inside the box where it was, outside where it is */
    CHECK(st->clicks == 0, "a click where the box was does nothing (%d)", st->clicks);
    gp_simulate_mouse(app, GP_SIM_MOVE, 500, 345, 0, 0);
    gp_sleep(co_, 0.03);
    f = frame();
    CHECK(f && is_c(px(f, 500, 345), 0, 255, 0) && is_red(px(f, 500, 395)) == false, "hover on the turned box: green where it is");
    CHECK(f && is_c(px(f, 500, 395), 0, 255, 0), "hover colors all of it");
    gp_simulate_mouse(app, GP_SIM_MOVE, 475, 370, 0, 0);
    gp_sleep(co_, 0.03);
    f = frame();
    CHECK(f && is_red(px(f, 500, 345)), "the pointer where the box was does not hover it (still red)");
    gp_simulate_mouse(app, GP_SIM_MOVE, 590, 590, 0, 0);
    gp_sleep(co_, 0.03);

    /* 10: a cached view under a parent that turns: the frame is the one a full paint gives, at several angles */
    float angles[] = { 0, 30, 90, 137.5f, 200 };
    for (size_t i = 0; i < sizeof angles / sizeof *angles; i++) {
        g_angle = angles[i];
        gp_app_notify(app);
        gp_sleep(co_, 0.05);
        uint8_t *a = frame();
        if (!a) continue;
        size_t n = (size_t)app->capture_w * (size_t)app->capture_h * 3;
        uint8_t *copy = malloc(n);
        memcpy(copy, a, n);
        gp_vviews *t = &win()->views;
        for (size_t k = 0; k < t->len; k++) t->v[k].chunk_valid = false;
        uint8_t *b = frame();
        long diff = b ? 0 : -1;
        for (size_t k = 0; b && k < n; k++) diff += copy[k] != b[k];
        if (diff && getenv("TRANSFORM_DUMP")) {
            int x0 = 9999, y0 = 9999, x1 = -1, y1 = -1;
            for (int yy = 0; yy < app->capture_h; yy++)
                for (int xx = 0; xx < app->capture_w; xx++) {
                    size_t q = ((size_t)yy * (size_t)app->capture_w + (size_t)xx) * 3;
                    if (copy[q] != b[q] || copy[q + 1] != b[q + 1] || copy[q + 2] != b[q + 2]) { if (xx < x0) x0 = xx; if (xx > x1) x1 = xx; if (yy < y0) y0 = yy; if (yy > y1) y1 = yy; }
                }
            fprintf(stderr, "angle %g: differing pixels in x %d..%d y %d..%d\n", (double)angles[i], x0, x1, y0, y1);
        }
        CHECK(diff == 0, "a cached view under a parent turned %.1f degrees: %ld bytes differ from a full paint", (double)angles[i], diff);
        free(copy);
    }

    /* 10 again, turned by an animation: the cached view is not rebuilt, only the angle above it changes */
    g_anim2_on = true;
    gp_app_notify(app);
    gp_sleep(co_, 0.05);
    for (int k = 0; k < 4; k++) {
        if (k) jump_by(1700);   /* of 10000 s: 61 degrees a step, and the real time between two frames is nothing */
        gp_sleep(co_, 0.08);
        uint8_t *a = frame();
        if (!a) continue;
        size_t n = (size_t)app->capture_w * (size_t)app->capture_h * 3;
        uint8_t *copy = malloc(n);
        memcpy(copy, a, n);
        gp_vviews *t = &win()->views;
        for (size_t q = 0; q < t->len; q++) t->v[q].chunk_valid = false;
        uint8_t *b = frame();
        long diff = b ? 0 : -1;   /* (the turn goes on in real time between the two frames: small differences at edges are that, a stale copy differs by a lot) */
        for (size_t q = 0; b && q < n; q++) diff += abs((int)copy[q] - (int)b[q]) > 90;
        CHECK(diff == 0, "a cached view under an animated turn (step %d): %ld bytes differ strongly from a full paint", k, diff);
        free(copy);
    }
    g_anim2_on = false;
    gp_app_notify(app);
    gp_sleep(co_, 0.05);

    /* 12: origin 1, 0: a 60 x 20 box at (100,60) in the cell at (0,600) turned a quarter about its top right corner (160, 660): x 140..160, y 600..660 */
    f = frame();
    CHECK(f && is_red(px(f, 150, 630)) && is_red(px(f, 142, 605)) && is_bg(px(f, 130, 630)) && is_bg(px(f, 150, 670)) && is_bg(px(f, 170, 630)), "origin 1, 0: pivots about the top right corner");

    /* 9: a click on the child of a clipped, turned box: inside the clip it lands, outside it (still on the child, which is bigger) it does not */
    st->clicks = 0;
    click_at(100, 495);
    CHECK(st->clicks == 1 && st->click_id == 9, "a click on the child inside the turned clip lands (%d)", st->clicks);
    st->clicks = 0;
    click_at(76, 520);
    CHECK(st->clicks == 0, "a click on the child outside the turned clip does not (%d)", st->clicks);

    g_anim_on = true;
    gp_app_notify(app);
    wait_draws(2);   /* (the animation starts when it is first laid out: before the clock is jumped) */
    /* 11: with_animation turns it: at t = 0.25 of 100 s (a quarter of a turn) the 60 x 20 box at (70,60) in the cell at (400,450) is 20 x 60 about (500, 520) */
    jump_by(25);   /* 25 s of 100: a quarter of a turn */
    gp_sleep(co_, 0.1);
    f = frame();
    dump("anim1");
    CHECK(f && is_red(px(f, 500, 495)) && is_red(px(f, 500, 545)) && is_bg(px(f, 475, 520)) && is_bg(px(f, 525, 520)), "animated: a quarter of a turn after 25 of 100 seconds");
    jump_by(25);   /* 50 s: half a turn, as it was, upside down */
    gp_sleep(co_, 0.1);
    f = frame();
    CHECK(f && is_red(px(f, 475, 520)) && is_red(px(f, 525, 520)) && is_bg(px(f, 500, 495)), "animated: half a turn after 50 seconds (as at the start)");
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    run();
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    st = calloc(1, sizeof *st);
    gp_fonts_set_antialiasing(app, GP_AA_LCD_RGB);   /* (turned text must not use it) */
    g_cached = gp_view_new(app, 1, &(char){ 0 }, cached_row, NULL);
    gp_entity v = gp_view_new(app, 1, &(char){ 0 }, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "transform_test", .width = 600, .height = 750 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "transform_test: %d FAILURE(S)\n" : "transform_test: all checks passed\n", failures);
    return failures != 0;
}
