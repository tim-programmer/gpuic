/*
 * Text blending in linear light (gp_fonts_set_text_gamma). Frames are read back from the renderer and every pixel a glyph covers is compared
 * with the pixel computed here from the glyph's own coverage (read from the atlas, where the instance says it is): black text on white and
 * white on black, in grayscale and in LCD mode (each stripe separately), with gamma 1 (blended in the encoded space, as before) and 2.2
 * (blended as in linear light: encode((1 - c) * decode(background) + c * decode(text))). Mid-gray text is left alone, a gamma of 1 is exactly the
 * old output, and changing the gamma repaints. Needs a display.
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

#define WHITE gp_rgb(0xffffff)
#define BLACK gp_rgb(0x000000)

static gp_node *render(void *self, gp_ctx *cx) {
    return gp_div(
        gp_size_full(), gp_flex_col(), gp_items_start(), gp_gap(10), gp_p(10), gp_bg(gp_rgb(0x808080)),
        gp_div(gp_bg(WHITE), gp_p(8), gp_text("Hamburgefonstiv", gp_text_size(36), gp_text_color(BLACK))),
        gp_div(gp_bg(BLACK), gp_p(8), gp_text("Hamburgefonstiv", gp_text_size(36), gp_text_color(WHITE))),
        gp_div(gp_bg(gp_rgb(0x888888)), gp_p(8), gp_text("Hamburgefonstiv", gp_text_size(36), gp_text_color(gp_rgb(0x808080)))));
}

static uint8_t *frame(void) {
    gp_debug_capture_next(app);
    for (int i = 0; i < 1000 && !app->capture_px; i++) {
        win()->dirty_paint = true;
        gp_sleep(co_, 0.01);
    }
    return app->capture_px;
}

static double decode(double x, double g) { return pow(x, g); }
static double encode(double x, double g) { return pow(x, 1.0 / g); }

typedef struct { long checked, bad; int worst; long changed; } Result;

/* every pixel of the glyph instances of `kind` that no other instance reaches, against what blending coverage `fg` over `bg` in light of
 * `gamma` gives; `changed` counts those where that differs from the blend in the encoded space by more than 8 levels */
static Result compare(const uint8_t *px, int pw, int kind, int bg, int fg, double gamma, int y_lo, int y_hi) {
    Result r = { 0 };
    int ph = app->capture_h;
    uint8_t *owners = calloc((size_t)pw * (size_t)ph, 1);
    const gp_scene *sc = &win()->scene;
    float s = win()->scale;
    for (size_t i = 0; i < sc->len; i++) {
        const gp_instance *it = &sc->items[i];
        if ((int)it->kind != kind) continue;
        int x0 = (int)lroundf(it->bounds[0] * s), y0 = (int)lroundf(it->bounds[1] * s), w = (int)lroundf(it->bounds[2] * s), h = (int)lroundf(it->bounds[3] * s);
        for (int y = y0; y < y0 + h; y++)
            for (int x = x0; x < x0 + w; x++)
                if (x >= 0 && y >= 0 && x < pw && y < ph && owners[(size_t)y * (size_t)pw + (size_t)x] < 2) owners[(size_t)y * (size_t)pw + (size_t)x]++;
    }
    const uint8_t *atlas = kind == GP_PRIM_LCD_GLYPH ? gp_text_lcd_atlas(app->text) : gp_text_atlas(app->text);
    int aw = kind == GP_PRIM_LCD_GLYPH ? GP_LCD_ATLAS_SIZE : GP_ATLAS_SIZE, bpp = kind == GP_PRIM_LCD_GLYPH ? 4 : 1;
    for (size_t i = 0; i < sc->len; i++) {
        const gp_instance *it = &sc->items[i];
        if ((int)it->kind != kind) continue;
        int x0 = (int)lroundf(it->bounds[0] * s), y0 = (int)lroundf(it->bounds[1] * s), w = (int)lroundf(it->bounds[2] * s), h = (int)lroundf(it->bounds[3] * s);
        int u0 = (int)lroundf(it->uv[0] * (float)aw), v0 = (int)lroundf(it->uv[1] * (float)aw);
        if (fabsf(it->bounds[0] * s - (float)x0) > 0.01f || fabsf(it->bounds[1] * s - (float)y0) > 0.01f) continue;   /* (not on a pixel: not compared) */
        for (int y = y0; y < y0 + h; y++) {
            if (y < y_lo || y >= y_hi) continue;
            for (int x = x0; x < x0 + w; x++) {
                if (x < 0 || y < 0 || x >= pw || y >= ph || owners[(size_t)y * (size_t)pw + (size_t)x] != 1) continue;
                const uint8_t *cell = atlas + ((size_t)(v0 + y - y0) * (size_t)aw + (size_t)(u0 + x - x0)) * (size_t)bpp;
                const uint8_t *p = px + ((size_t)y * (size_t)pw + (size_t)x) * 3;
                for (int c = 0; c < 3; c++) {
                    double cov = (bpp == 4 ? cell[c] : cell[0]) / 255.0;
                    double want = gamma == 1.0 ? (1 - cov) * bg + cov * fg
                                               : 255.0 * encode((1 - cov) * decode(bg / 255.0, gamma) + cov * decode(fg / 255.0, gamma), gamma);
                    double plain = (1 - cov) * bg + cov * fg;
                    int err = abs((int)lround(want) - (int)p[c]);
                    r.checked++;
                    if (err > 2) r.bad++;
                    if (err > r.worst) r.worst = err;
                    if (fabs(want - plain) > 8) r.changed++;
                }
            }
        }
    }
    free(owners);
    return r;
}

static void run(void) {
    wait_draws(2);
    float s = win()->scale;
    /* the three boxes' rows: white-on-black is the second text element */
    CHECK(win()->tree->nchildren == 3, "found %u boxes", win()->tree->nchildren);
    if (win()->tree->nchildren != 3) return;
    int rows_y[4];
    for (int i = 0; i < 3; i++) rows_y[i] = (int)floorf((win()->tree->children[i]->pos.y) * s);
    rows_y[3] = (int)ceilf((win()->tree->children[2]->pos.y + win()->tree->children[2]->size.h) * s);
    printf("  (window scale %.2f)\n", s);

    static const struct { int mode; int kind; const char *name; } modes[] = { { GP_AA_GRAYSCALE, GP_PRIM_GLYPH, "grayscale" }, { GP_AA_LCD_RGB, GP_PRIM_LCD_GLYPH, "LCD" } };
    for (int m = 0; m < 2; m++) {
        gp_fonts_set_antialiasing(app, modes[m].mode);
        for (int pass = 0; pass < 2; pass++) {
            double gamma = pass ? 2.2 : 1.0;
            gp_fonts_set_text_gamma(app, (float)gamma);
            wait_draws(2);
            uint8_t *px = frame();
            CHECK(px != NULL, "no frame");
            if (!px) return;
            int pw = app->capture_w;
            Result rb = compare(px, pw, modes[m].kind, 255, 0, gamma, rows_y[0], rows_y[1]);      /* black on white */
            Result rw = compare(px, pw, modes[m].kind, 0, 255, gamma, rows_y[1], rows_y[2]);      /* white on black */
            CHECK(rb.checked > 3000 && rw.checked > 3000, "%s gamma %.1f: samples compared %ld / %ld", modes[m].name, gamma, rb.checked, rw.checked);
            CHECK(rb.bad == 0 && rw.bad == 0, "%s gamma %.1f: pixels off the model: black on white %ld of %ld (worst %d), white on black %ld of %ld (worst %d)",
                  modes[m].name, gamma, rb.bad, rb.checked, rb.worst, rw.bad, rw.checked, rw.worst);
            if (pass) CHECK(rb.changed > 200 && rw.changed > 200, "%s: linear blending is not the encoded one (%ld, %ld samples differ by more than 8)", modes[m].name, rb.changed, rw.changed);
            if (m == 0 && pass == 0) {   /* mid-gray text on a lighter gray: the same at any gamma */
                uint8_t *copy = malloc((size_t)pw * (size_t)app->capture_h * 3);
                memcpy(copy, px, (size_t)pw * (size_t)app->capture_h * 3);
                gp_fonts_set_text_gamma(app, 2.2f);
                wait_draws(2);
                uint8_t *px2 = frame();
                long diff = 0;
                for (int y = rows_y[2]; y < rows_y[3]; y++)
                    for (int x = 0; x < pw; x++)
                        for (int c = 0; c < 3; c++) diff += abs((int)copy[((size_t)y * (size_t)pw + (size_t)x) * 3 + (size_t)c] - (int)px2[((size_t)y * (size_t)pw + (size_t)x) * 3 + (size_t)c]) > 1;
                CHECK(diff == 0, "mid-gray text is left alone by the gamma (%ld samples changed)", diff);
                free(copy);
                gp_fonts_set_text_gamma(app, 1.f);
                wait_draws(2);
            }
        }
    }
    /* changing it repaints by itself */
    {
        wait_draws(2);
        gp_sleep(co_, 0.2);
        unsigned long before = app->stat_draws;
        gp_fonts_set_text_gamma(app, 2.2f);
        wait_draws(1);
        CHECK(app->stat_draws > before, "a new gamma is drawn without anything else asking for a frame");
        gp_fonts_set_text_gamma(app, 1.f);
        wait_draws(2);
    }
    /* out of range is clamped, and the default is the old behavior */
    gp_fonts_set_text_gamma(app, 0.2f);
    CHECK(gp_text_gamma(app->text) == 1.f, "a gamma below 1 is 1");
    gp_fonts_set_text_gamma(app, 9.f);
    CHECK(gp_text_gamma(app->text) == 3.f, "and above 3 is 3");
    gp_fonts_set_text_gamma(app, 1.f);
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    run();
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    CHECK(fabsf(gp_text_gamma(app->text) - 1.8f) < 1e-6f, "GPUIC_TEXT_GAMMA sets the starting gamma: %g", gp_text_gamma(app->text));
    gp_open_window(app, &(gp_window_options){ .title = "gamma_test", .width = 420, .height = 260 }, gp_view_new(app, 1, NULL, render, NULL));
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    setenv("GPUIC_TEXT_GAMMA", "1.8", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "gamma_test: %d FAILURE(S)\n" : "gamma_test: all checks passed\n", failures);
    return failures != 0;
}
