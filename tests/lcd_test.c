/*
 * LCD antialiasing, end to end: the pixels the GL renderer produces for text in each mode. Needs a display.
 *
 * Black text on white must be neutral gray in grayscale mode and carry color fringes in the LCD modes (with the stripes
 * the other way round for BGR); the same amount of ink either way; colored text on a colored background must stay between
 * the two colors in every channel (per-channel blending, not just a tint); and fading the text (opacity) keeps all of
 * that. Frames are read back from the renderer (gp_debug_capture_next), not drawn from the scene.
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

#define BG    gp_rgb(0x28507a)   /* 40 80 122 */
#define INK   gp_rgb(0xe6b43c)   /* 230 180 60 */

static gp_node *render(void *self, gp_ctx *cx) {
    return gp_div(
        gp_size_full(), gp_flex_col(), gp_items_start(), gp_gap(10), gp_p(10), gp_bg(BG),
        gp_div(gp_bg(gp_rgb(0xffffff)), gp_p(6), gp_text("Hamburgefonstiv Illumination", gp_text_size(15), gp_text_color(gp_rgb(0x000000)))),
        gp_div(gp_p(4), gp_text("Illl", gp_text_size(80), gp_text_color(INK))),
        gp_div(gp_bg(gp_rgb(0xffffff)), gp_p(6), gp_text("Hamburgefonstiv Illumination", gp_text_size(15), gp_text_color(gp_rgb(0x000000)), gp_opacity(0.5f))));
}

static const gp_el *texts[8];
static int ntexts;
static void collect(const gp_el *n) {
    if (n->kind == GP_EL_TEXT && ntexts < 8) texts[ntexts++] = n;
    for (uint32_t i = 0; i < n->nchildren; i++) collect(n->children[i]);
}
static bool abs_pos(const gp_el *n, const gp_el *target, float x, float y, float *ox, float *oy) {
    x += n->pos.x;
    y += n->pos.y;
    if (n == target) { *ox = x; *oy = y; return true; }
    for (uint32_t i = 0; i < n->nchildren; i++)
        if (abs_pos(n->children[i], target, x, y, ox, oy)) return true;
    return false;
}

typedef struct { int x0, y0, x1, y1; } Box;
static Box box_of(int i) {
    float ax = 0.f, ay = 0.f;
    abs_pos(win()->tree, texts[i], -win()->tree->pos.x, -win()->tree->pos.y, &ax, &ay);
    float s = win()->scale;
    return (Box){ (int)floorf(ax * s), (int)floorf(ay * s), (int)ceilf((ax + texts[i]->size.w) * s), (int)ceilf((ay + texts[i]->size.h) * s) };
}

/* the frame as the renderer drew it, for the mode in force */
static uint8_t *frame(void) {
    gp_debug_capture_next(app);
    for (int i = 0; i < 1000 && !app->capture_px; i++) {
        win()->dirty_paint = true;
        gp_sleep(co_, 0.01);
    }
    return app->capture_px;
}

static int glyph_instances(int kind) {
    int n = 0;
    for (size_t i = 0; i < win()->scene.len; i++) n += (int)win()->scene.items[i].kind == kind;
    return n;
}

typedef struct {
    long spread_pixels;      /* pixels whose channels differ at all */
    long blue_dark, red_dark;   /* B well below R, R well below B: the left and right edges of stems in RGB order */
    double mean;             /* mean of all channel values */
    int lo[3], hi[3];        /* per-channel extremes */
} Stats;

static Stats stats(const uint8_t *px, int w, Box b) {
    Stats s = { 0 };
    s.lo[0] = s.lo[1] = s.lo[2] = 255;
    double sum = 0;
    long n = 0;
    for (int y = b.y0; y < b.y1; y++)
        for (int x = b.x0; x < b.x1; x++) {
            const uint8_t *p = px + ((size_t)y * (size_t)w + (size_t)x) * 3;
            if (p[0] != p[1] || p[1] != p[2]) s.spread_pixels++;
            if (p[2] + 30 < p[0]) s.blue_dark++;
            if (p[0] + 30 < p[2]) s.red_dark++;
            for (int c = 0; c < 3; c++) {
                sum += p[c];
                if (p[c] < s.lo[c]) s.lo[c] = p[c];
                if (p[c] > s.hi[c]) s.hi[c] = p[c];
            }
            n++;
        }
    s.mean = n ? sum / (double)(3 * n) : 0;
    return s;
}

static void run(void) {
    wait_draws(2);
    ntexts = 0;
    collect(win()->tree);
    CHECK(ntexts == 3, "found %d text elements, wanted 3", ntexts);
    if (ntexts != 3) return;
    float scale = win()->scale;
    CHECK(scale >= 1.f, "window scale %.2f", scale);

    Stats gray[3], rgb[3], bgr[3], vrgb[3], vbgr[3];
    int mode_of[5] = { GP_AA_GRAYSCALE, GP_AA_LCD_RGB, GP_AA_LCD_BGR, GP_AA_LCD_VRGB, GP_AA_LCD_VBGR };
    Stats *out[5] = { gray, rgb, bgr, vrgb, vbgr };
    int lcd_instances[5], plain_instances[5];
    for (int m = 0; m < 5; m++) {
        gp_fonts_set_antialiasing(app, mode_of[m]);
        wait_draws(2);
        uint8_t *px = frame();
        CHECK(px != NULL, "mode %d: no frame captured", m);
        if (!px) return;
        int w = app->capture_w;
        for (int i = 0; i < 3; i++) out[m][i] = stats(px, w, box_of(i));
        lcd_instances[m] = glyph_instances(GP_PRIM_LCD_GLYPH);
        plain_instances[m] = glyph_instances(GP_PRIM_GLYPH);
    }

    /* grayscale: exactly neutral on white (the box is white, the text black: every pixel has r = g = b) */
    CHECK(gray[0].spread_pixels == 0 && gray[2].spread_pixels == 0, "grayscale text on white is not neutral (%ld, %ld colored pixels)", gray[0].spread_pixels, gray[2].spread_pixels);
    CHECK(lcd_instances[0] == 0 && plain_instances[0] > 0, "grayscale draws plain glyph instances (%d lcd, %d plain)", lcd_instances[0], plain_instances[0]);
    CHECK(lcd_instances[1] > 0 && lcd_instances[2] > 0 && plain_instances[1] == 0 && plain_instances[2] == 0,
          "LCD modes draw LCD glyph instances only (%d/%d lcd, %d/%d plain)", lcd_instances[1], lcd_instances[2], plain_instances[1], plain_instances[2]);

    /* RGB: fringes, with blue darker at left edges and red darker at right edges; BGR the other way round */
    CHECK(rgb[0].spread_pixels > 100 && bgr[0].spread_pixels > 100, "LCD text has colored pixels (%ld, %ld)", rgb[0].spread_pixels, bgr[0].spread_pixels);
    CHECK(rgb[0].blue_dark > 20 && rgb[0].red_dark > 20, "RGB: both kinds of edge (%ld blue-dark, %ld red-dark)", rgb[0].blue_dark, rgb[0].red_dark);
    CHECK(labs(rgb[0].blue_dark - bgr[0].red_dark) <= rgb[0].blue_dark / 5 + 3 && labs(rgb[0].red_dark - bgr[0].blue_dark) <= rgb[0].red_dark / 5 + 3,
          "BGR is RGB with the stripes swapped: %ld/%ld vs %ld/%ld", rgb[0].blue_dark, rgb[0].red_dark, bgr[0].red_dark, bgr[0].blue_dark);
    /* the stem edges: in RGB the left edge of a stem darkens blue first (it is the stripe nearest the stem), so left edges give blue_dark.
     * Whichever way it is, one of the modes has the majority one way and the other the opposite */
    CHECK((rgb[0].blue_dark > rgb[0].red_dark) != (bgr[0].blue_dark > bgr[0].red_dark) || labs(rgb[0].blue_dark - rgb[0].red_dark) < 6,
          "RGB and BGR lean opposite ways (%ld/%ld vs %ld/%ld)", rgb[0].blue_dark, rgb[0].red_dark, bgr[0].blue_dark, bgr[0].red_dark);

    /* vertical stripes: fringes too, at the top and bottom edges, swapped between VRGB and VBGR, the same amount of ink */
    CHECK(vrgb[0].spread_pixels > 100 && vbgr[0].spread_pixels > 100 && lcd_instances[3] > 0 && lcd_instances[4] > 0 && plain_instances[3] == 0,
          "vertical LCD text has colored pixels (%ld, %ld) and uses LCD instances (%d, %d)", vrgb[0].spread_pixels, vbgr[0].spread_pixels, lcd_instances[3], lcd_instances[4]);
    CHECK(vrgb[0].blue_dark > 20 && vrgb[0].red_dark > 20 && labs(vrgb[0].blue_dark - vbgr[0].red_dark) <= vrgb[0].blue_dark / 5 + 3 && labs(vrgb[0].red_dark - vbgr[0].blue_dark) <= vrgb[0].red_dark / 5 + 3,
          "VBGR is VRGB with the stripes swapped: %ld/%ld vs %ld/%ld", vrgb[0].blue_dark, vrgb[0].red_dark, vbgr[0].red_dark, vbgr[0].blue_dark);
    CHECK(fabs(vrgb[0].mean - gray[0].mean) < 255 * 0.025 && fabs(vbgr[0].mean - gray[0].mean) < 255 * 0.025, "vertical ink: grayscale %.1f, VRGB %.1f, VBGR %.1f", gray[0].mean, vrgb[0].mean, vbgr[0].mean);
    CHECK(vrgb[0].spread_pixels != rgb[0].spread_pixels || vrgb[0].blue_dark != rgb[0].blue_dark, "vertical stripes are not the horizontal ones");

    /* the same amount of ink: the mean over the text's box agrees within 2.5% (of the range 0..255) */
    CHECK(fabs(rgb[0].mean - gray[0].mean) < 255 * 0.025 && fabs(bgr[0].mean - gray[0].mean) < 255 * 0.025,
          "ink: grayscale %.1f, RGB %.1f, BGR %.1f", gray[0].mean, rgb[0].mean, bgr[0].mean);

    /* colored text on a colored background: each channel stays between the background's and the text's */
    static const int bg[3] = { 40, 80, 122 }, ink[3] = { 230, 180, 60 };
    for (int m = 0; m < 5; m++)
        for (int c = 0; c < 3; c++) {
            int lo = bg[c] < ink[c] ? bg[c] : ink[c], hi = bg[c] < ink[c] ? ink[c] : bg[c];
            CHECK(out[m][1].lo[c] >= lo - 2 && out[m][1].hi[c] <= hi + 2, "mode %d channel %d of the big text runs %d..%d, outside %d..%d", m, c, out[m][1].lo[c], out[m][1].hi[c], lo, hi);
            CHECK(out[m][1].hi[c] >= hi - 2 || out[m][1].lo[c] <= lo + 2, "mode %d channel %d: the text's own color is reached", m, c);
        }

    /* faded text: blended with the background in each channel, never darker than the half-way color, still fringed in LCD */
    CHECK(gray[2].lo[0] >= 127 - 3 && rgb[2].lo[0] >= 127 - 3 && rgb[2].lo[1] >= 127 - 3 && rgb[2].lo[2] >= 127 - 3 && bgr[2].lo[2] >= 127 - 3,
          "opacity 0.5: darkest channel values %d %d %d %d", gray[2].lo[0], rgb[2].lo[0], rgb[2].lo[1], rgb[2].lo[2]);
    CHECK(rgb[2].spread_pixels > 50 && gray[2].spread_pixels == 0, "opacity 0.5: fringes in LCD only (%ld, %ld)", rgb[2].spread_pixels, gray[2].spread_pixels);
    CHECK(255 - rgb[2].mean < 0.6 * (255 - rgb[0].mean) && 255 - rgb[2].mean > 0.4 * (255 - rgb[0].mean),
          "and has about half the ink of the opaque one (%.1f vs %.1f below white)", 255 - rgb[2].mean, 255 - rgb[0].mean);

    /* automatic: what the text system says it should be for this window's scale */
    gp_fonts_set_antialiasing(app, GP_AA_AUTO);
    wait_draws(2);
    int order = gp_text_lcd_order(app->text, scale);
    CHECK((glyph_instances(GP_PRIM_LCD_GLYPH) > 0) == (order != 0), "auto: order %d, %d lcd instances", order, glyph_instances(GP_PRIM_LCD_GLYPH));
    CHECK(gp_text_lcd_order(app->text, 2.f) == 0, "auto: no LCD at 2x");
    gp_fonts_set_antialiasing(app, GP_AA_GRAYSCALE);
    wait_draws(2);
    CHECK(glyph_instances(GP_PRIM_LCD_GLYPH) == 0, "back to grayscale");
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    run();
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    gp_entity v = gp_view_new(app, 0, NULL, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "lcd_test", .width = 420, .height = 300 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "lcd_test: %d FAILURE(S)\n" : "lcd_test: all checks passed\n", failures);
    return failures != 0;
}
