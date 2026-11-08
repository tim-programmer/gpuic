/*
 * Images: decoding (stb_image, vendored in third_party/), premultiplying, and the public gp_image_* calls. What holds them
 * and draws them is gfx.c and the renderer.
 *
 * Decoding is a pure function of the bytes, so it runs on a pool thread for gp_image_load_file_async; the image is only
 * registered (a main-thread operation) once the worker is done.
 */
#include "internal.h"

#include <stdio.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_MAX_DIMENSIONS GP_IMAGE_MAX_DIM
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include "../third_party/stb/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define MAX_PIXELS (100u * 1000u * 1000u)   /* what a decoder is willing to allocate for one picture: 400 MB of RGBA */
#define MAX_FILE_BYTES (512u * 1024u * 1024u)

void gp_image_premultiply(uint8_t *p, size_t pixels) {
    for (size_t i = 0; i < pixels; i++, p += 4) {
        unsigned a = p[3];
        if (a == 255) continue;
        p[0] = (uint8_t)((p[0] * a + 127) / 255);
        p[1] = (uint8_t)((p[1] * a + 127) / 255);
        p[2] = (uint8_t)((p[2] * a + 127) / 255);
    }
}

uint8_t *gp_image_decode(const void *data, size_t size, int *w, int *h) {
    if (!data || size == 0 || size > MAX_FILE_BYTES || size > 0x7fffffffu) return NULL;
    int iw, ih, comp;
    /* ask for the size first: a small file can claim a huge picture */
    if (!stbi_info_from_memory(data, (int)size, &iw, &ih, &comp)) return NULL;
    if (iw <= 0 || ih <= 0 || iw > GP_IMAGE_MAX_DIM || ih > GP_IMAGE_MAX_DIM || (uint64_t)iw * (uint64_t)ih > MAX_PIXELS) return NULL;
    uint8_t *px = stbi_load_from_memory(data, (int)size, &iw, &ih, &comp, 4);
    if (!px) return NULL;
    gp_image_premultiply(px, (size_t)iw * (size_t)ih);
    *w = iw;
    *h = ih;
    return px;   /* malloc'd by stb: free() */
}

/* ---------------------------------------------------------------- public */

gp_image gp_image_load_memory(gp_app *app, const void *data, size_t size) {
    if (!app) return GP_IMAGE_NULL;
    int w, h;
    uint8_t *px = gp_image_decode(data, size, &w, &h);
    return px ? gp_gfx_image_add(app->gfx, w, h, px) : GP_IMAGE_NULL;
}

/* The whole file, or NULL. */
static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *buf = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len == cap) {
            if (cap >= MAX_FILE_BYTES) { free(buf); fclose(f); return NULL; }
            cap = cap ? cap * 2 : 1 << 16;
            uint8_t *grown = realloc(buf, cap);
            if (!grown) { free(buf); fclose(f); return NULL; }
            buf = grown;
        }
        size_t got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (got == 0) break;
    }
    bool bad = ferror(f) != 0;
    fclose(f);
    if (bad || len == 0) { free(buf); return NULL; }
    *size = len;
    return buf;
}

gp_image gp_image_load_file(gp_app *app, const char *path) {
    if (!app || !path) return GP_IMAGE_NULL;
    size_t size;
    uint8_t *bytes = read_file(path, &size);
    if (!bytes) return GP_IMAGE_NULL;
    gp_image im = gp_image_load_memory(app, bytes, size);
    free(bytes);
    return im;
}

gp_image gp_image_from_rgba(gp_app *app, int width, int height, const void *rgba) {
    if (!app || !rgba || width <= 0 || height <= 0 || width > GP_IMAGE_MAX_DIM || height > GP_IMAGE_MAX_DIM ||
        (uint64_t)width * (uint64_t)height > MAX_PIXELS)
        return GP_IMAGE_NULL;
    size_t bytes = (size_t)width * (size_t)height * 4;
    uint8_t *copy = malloc(bytes);
    if (!copy) return GP_IMAGE_NULL;
    memcpy(copy, rgba, bytes);
    gp_image_premultiply(copy, (size_t)width * (size_t)height);
    return gp_gfx_image_add(app->gfx, width, height, copy);
}

bool gp_image_size(gp_app *app, gp_image image, int *width, int *height) {
    return app && gp_gfx_image_size(app->gfx, image, width, height);
}

void gp_image_release(gp_app *app, gp_image image) {
    if (!app) return;
    gp_gfx_image_release(app->gfx, image);
    gp_app_notify(app);   /* what showed it is drawn again, without it */
}

/* ---- asynchronous loading */

typedef struct {
    char *path;
    uint8_t *pixels;       /* set by the worker, taken by `done` */
    int w, h;
    gp_image_fn fn;
    void *data;
} load_job;

static void load_work(void *data) {
    load_job *j = data;
    size_t size;
    uint8_t *bytes = read_file(j->path, &size);
    if (!bytes) return;
    j->pixels = gp_image_decode(bytes, size, &j->w, &j->h);
    free(bytes);
}

static void load_done(void *self, gp_ctx *cx, void *data) {
    load_job *j = data;
    gp_image im = j->pixels ? gp_gfx_image_add(cx->app->gfx, j->w, j->h, j->pixels) : GP_IMAGE_NULL;
    j->pixels = NULL;   /* the registry owns them now (or they were refused and freed) */
    if (j->fn) j->fn(self, cx, im, j->data);
    else gp_gfx_image_release(cx->app->gfx, im);
}

static void load_free(void *data) {
    load_job *j = data;
    free(j->pixels);
    free(j->path);
    free(j);
}

gp_task gp_image_load_file_async(gp_ctx *cx, const char *path, gp_image_fn done, void *data) {
    load_job *j = calloc(1, sizeof *j);
    if (!j) return (gp_task){ 0 };
    j->path = strdup(path ? path : "");
    j->fn = done;
    j->data = data;
    return gp_background(cx, load_work, load_done, j, load_free);
}
