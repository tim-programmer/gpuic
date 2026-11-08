/*
 * Images: gp_image_from_rgba / gp_image_load_file (PNG, JPEG, GIF, BMP), gp_img with object-fit, rounded corners, aspect
 * ratio from one side, pixel art, and mipmapped minification.
 *
 *   ./images [picture.png|jpg|gif|bmp ...]    show these too (decoded on a pool thread, so a big one does not freeze the window)
 *
 * The pictures drawn here are made in code, so the example needs no files.
 */
#include <gpuic/gpuic.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define BASE    gp_rgb(0x1e1e2e)
#define SURFACE gp_rgb(0x313244)
#define TEXT    gp_rgb(0xcdd6f4)
#define SUBTEXT gp_rgb(0x9399b2)

enum { MAX_FILES = 6 };
typedef struct {
    gp_image photo, checker, sprite, avatar;
    gp_image files[MAX_FILES];
    int nfiles;
    char *paths[MAX_FILES];
} Demo;

/* a 480 x 320 "landscape": sky gradient, a sun, hills */
static gp_image make_photo(gp_app *app) {
    enum { W = 480, H = 320 };
    unsigned char *px = malloc((size_t)W * H * 4);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float t = (float)y / H;
            float r = 60 + 150 * t, g = 120 + 100 * t, b = 230 - 60 * t;
            float dx = x - 340.f, dy = y - 90.f, d = sqrtf(dx * dx + dy * dy);
            if (d < 40) { r = 255; g = 230; b = 120; }
            else if (d < 70) { float k = (70 - d) / 30; r += (255 - r) * k * 0.5f; g += (220 - g) * k * 0.5f; }
            float hill = 220 + 28 * sinf(x * 0.012f) + 18 * sinf(x * 0.031f + 1);
            if (y > hill) { r = 40 + 30 * (y - hill) / 100; g = 120 - 30 * (y - hill) / 100; b = 70; }
            unsigned char *p = px + ((size_t)y * W + x) * 4;
            p[0] = (unsigned char)r; p[1] = (unsigned char)g; p[2] = (unsigned char)b; p[3] = 255;
        }
    gp_image im = gp_image_from_rgba(app, W, H, px);
    free(px);
    return im;
}

/* a one pixel checkerboard: shown small, a mipmapped texture averages it to gray, a plain one would shimmer */
static gp_image make_checker(gp_app *app) {
    enum { N = 512 };
    unsigned char *px = malloc((size_t)N * N * 4);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            unsigned char v = (x + y) & 1 ? 255 : 0;
            unsigned char *p = px + ((size_t)y * N + x) * 4;
            p[0] = p[1] = p[2] = v; p[3] = 255;
        }
    gp_image im = gp_image_from_rgba(app, N, N, px);
    free(px);
    return im;
}

/* 8 x 8 pixel art with transparency */
static gp_image make_sprite(gp_app *app) {
    static const char *rows[8] = { "..yyyy..", ".yyyyyy.", "yy.yy.yy", "yyyyyyyy", "yy.yy.yy", "yyy..yyy", ".yyyyyy.", "..yyyy.." };
    unsigned char px[8 * 8 * 4] = { 0 };
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            unsigned char *p = px + (y * 8 + x) * 4;
            if (rows[y][x] == 'y') { p[0] = 249; p[1] = 226; p[2] = 175; p[3] = 255; }
            else if (rows[y][x] == '.') { p[3] = 0; }
        }
    return gp_image_from_rgba(app, 8, 8, px);
}

static gp_node *card(const char *title, gp_node *body) {
    return gp_div(gp_flex_col(), gp_gap(6), gp_p(10), gp_rounded(10), gp_bg(SURFACE),
                  gp_text(title, gp_text_size(12), gp_text_color(SUBTEXT)), body);
}

static gp_node *fit_box(gp_image im, int fit, const char *label) {
    return gp_div(gp_flex_col(), gp_gap(3), gp_items_center(),
                  gp_img(im, gp_w(110), gp_h(70), gp_object_fit(fit), gp_bg(BASE)),
                  gp_text(label, gp_text_size(11), gp_text_color(SUBTEXT)));
}

static gp_node *demo_render(void *self, gp_ctx *cx) {
    Demo *d = self;
    gp_node *loaded[MAX_FILES];
    for (int i = 0; i < d->nfiles; i++)
        loaded[i] = gp_img(d->files[i], gp_h(90), gp_rounded(6));
    return gp_div(
        gp_size_full(), gp_flex_col(), gp_gap(12), gp_p(16), gp_bg(BASE), gp_text_color(TEXT),
        gp_text("Images", gp_text_size(22), gp_font_bold()),
        gp_div(gp_flex_row(), gp_gap(12),
               card("120 px and 200 px wide (the height follows)",
                    gp_div(gp_flex_row(), gp_gap(10), gp_items_start(),
                           gp_img(d->photo, gp_w(120), gp_rounded(8)), gp_img(d->photo, gp_w(200), gp_rounded(8)))),
               card("object-fit in a 110 x 70 box",
                    gp_div(gp_flex_row(), gp_gap(8),
                           fit_box(d->photo, GP_FIT_FILL, "fill"), fit_box(d->photo, GP_FIT_CONTAIN, "contain"),
                           fit_box(d->photo, GP_FIT_COVER, "cover"), fit_box(d->photo, GP_FIT_NONE, "none")))),
        gp_div(gp_flex_row(), gp_gap(12),
               card("pixel art: smoothed, and pixelated",
                    gp_div(gp_flex_row(), gp_gap(10),
                           gp_img(d->sprite, gp_size_px(80)), gp_img(d->sprite, gp_size_px(80), gp_image_pixelated()))),
               card("a 512 px checkerboard at 64 px (mipmapped: gray)",
                    gp_img(d->checker, gp_size_px(64))),
               card("rounded, bordered, 60% opaque",
                    gp_div(gp_flex_row(), gp_gap(10),
                           gp_img(d->photo, gp_size_px(80), gp_object_fit(GP_FIT_COVER), gp_rounded(40)),
                           gp_img(d->photo, gp_size_px(80), gp_object_fit(GP_FIT_COVER), gp_rounded(14), gp_p(4), gp_border(2), gp_border_color(TEXT)),
                           gp_img(d->photo, gp_size_px(80), gp_object_fit(GP_FIT_COVER), gp_opacity(0.6f))))),
        card("filters: grayscale, brightness 0.5, saturate 0, tint, blur 6",
             gp_div(gp_flex_row(), gp_gap(8),
                    gp_img(d->photo, gp_w(100), gp_rounded(6), gp_image_grayscale(1)), gp_img(d->photo, gp_w(100), gp_rounded(6), gp_image_brightness(0.5f)),
                    gp_img(d->photo, gp_w(100), gp_rounded(6), gp_image_saturate(0)), gp_img(d->photo, gp_w(100), gp_rounded(6), gp_image_tint(gp_rgb(0xf9e2af))),
                    gp_img(d->photo, gp_w(100), gp_rounded(6), gp_image_blur(6)))),
        d->nfiles ? card("from the command line", gp_div(gp_flex_row(), gp_gap(8), gp_children(loaded, (size_t)d->nfiles))) : NULL);
}

static void loaded(void *self, gp_ctx *cx, gp_image image, void *data) {
    Demo *d = self;
    long i = (long)(intptr_t)data;
    if (!image.generation) { fprintf(stderr, "images: could not load %s\n", d->paths[i]); return; }
    if (d->nfiles < MAX_FILES) d->files[d->nfiles++] = image;
    gp_notify(cx);
}

static int g_argc;
static char **g_argv;

static void ready(gp_app *app, void *userdata) {
    Demo init = { .photo = make_photo(app), .checker = make_checker(app), .sprite = make_sprite(app) };
    for (int i = 1; i < g_argc && i <= MAX_FILES; i++) init.paths[i - 1] = g_argv[i];
    gp_entity demo = gp_view_new(app, sizeof(Demo), &init, demo_render, NULL);
    Demo *d = gp_entity_get(app, demo);
    gp_ctx cx = gp_ctx_for(app, demo);
    for (int i = 0; i < MAX_FILES && d->paths[i]; i++) gp_image_load_file_async(&cx, d->paths[i], loaded, (void *)(intptr_t)i);
    gp_open_window(app, &(gp_window_options){ .title = "gpuic: images", .width = 760, .height = 680 }, demo);
}

int main(int argc, char **argv) {
    g_argc = argc;
    g_argv = argv;
    return gp_application_run(ready, NULL);
}
