/*
 * SVG: a document is kept as text and drawn by rasterizing it (nanosvg, vendored in third_party/) at the size, in device
 * pixels, it is shown at, into an image that the image path then draws; gfx.c caches those rasters.
 *
 * `currentColor` in the document means the text color of the element that shows it, so the text is rewritten with that
 * color before it is parsed: one parse per (document, size, color) raster, which for icons is microseconds.
 *
 * What nanosvg understands: paths (all commands, arcs included), rect, circle, ellipse, line, polyline, polygon; groups and
 * transforms; fill and stroke (colors, opacity, linear and radial gradients, fill-rule, widths, joins, caps, dashes);
 * <defs> and gradient references; <style> with simple class selectors. svgx.c adds <use>, <symbol>, clip paths and masks by rewriting the
 * document into layers, and draws <text> as the outlines of its glyphs. What none of it does: filters, patterns, markers, images inside SVGs.
 */
#include "internal.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define NANOSVG_IMPLEMENTATION
#define NANOSVG_ALL_COLOR_KEYWORDS
#define NANOSVGRAST_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wcomment"
#pragma GCC diagnostic ignored "-Wstringop-truncation"
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#endif
#include "../third_party/nanosvg/nanosvg.h"
#include "../third_party/nanosvg/nanosvgrast.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define MAX_SVG_BYTES (8u * 1024u * 1024u)
#define MAX_SVG_SIDE 100000.f      /* intrinsic size in px: beyond this the document is not a drawing */

/* A copy of the document with every `currentColor` replaced by `#rrggbb`; NUL-terminated, malloc'd. */
static char *with_color(const char *src, size_t len, uint32_t rgb) {
    static const char key[] = "currentColor";
    const size_t klen = sizeof key - 1;
    char hex[8];
    snprintf(hex, sizeof hex, "#%06x", (unsigned)(rgb & 0xffffff));
    char *out = malloc(len + 1 + 8);   /* a replacement is shorter than what it replaces, so len + 1 is enough; a little slack */
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len;) {
        if (len - i >= klen && strncasecmp(src + i, key, klen) == 0) {
            memcpy(out + o, hex, 7);
            o += 7;
            i += klen;
        } else {
            out[o++] = src[i++];
        }
    }
    out[o] = 0;
    return out;
}

bool gp_svg_probe(const char *src, size_t len, float *w, float *h) {
    if (!src || len == 0 || len > MAX_SVG_BYTES) return false;
    char *copy = with_color(src, len, 0);
    if (!copy) return false;
    NSVGimage *im = nsvgParse(copy, "px", 96.f);
    free(copy);
    if (!im) return false;
    float iw = im->width, ih = im->height;
    nsvgDelete(im);
    if (!(iw > 0.f && ih > 0.f && iw <= MAX_SVG_SIDE && ih <= MAX_SVG_SIDE)) return false;
    *w = iw;
    *h = ih;
    return true;
}

/* One document drawn at w x h (straight alpha, as nanosvg hands it back), or NULL. */
static uint8_t *raster_doc(const char *doc, int w, int h, float intrinsic_w) {
    NSVGimage *im = nsvgParse((char *)doc, "px", 96.f);   /* (nanosvg works in place: the caller's copy is modified, so it is a copy) */
    if (!im) return NULL;
    uint8_t *px = calloc((size_t)w * (size_t)h, 4);
    NSVGrasterizer *rast = px ? nsvgCreateRasterizer() : NULL;
    if (rast) {
        nsvgRasterize(rast, im, 0, 0, (float)w / intrinsic_w, px, w, h, w * 4);
    } else {
        free(px);
        px = NULL;
    }
    if (rast) nsvgDeleteRasterizer(rast);
    nsvgDelete(im);
    return px;
}

static uint8_t *raster_text(const char *doc, int w, int h, float intrinsic_w) {
    char *copy = strdup(doc);
    uint8_t *px = copy ? raster_doc(copy, w, h, intrinsic_w) : NULL;
    free(copy);
    return px;
}

/* A document with <use>, clip-path or mask: each layer is drawn, cut by its clip (the alpha of the clip document) and its mask (the luminance times the
 * alpha of the mask document), and put over what is below, all in premultiplied bytes. */
static uint8_t *render_layers(struct svgx *x, int w, int h, float intrinsic_w) {
    size_t n = (size_t)w * (size_t)h;
    uint8_t *out = calloc(n, 4);
    if (!out) return NULL;
    for (int i = 0; i < gp_svgx_layers(x); i++) {
        char *ld = gp_svgx_layer_doc(x, i), *cd = gp_svgx_clip_doc(x, i), *md = gp_svgx_mask_doc(x, i);
        uint8_t *layer = ld ? raster_text(ld, w, h, intrinsic_w) : NULL;
        uint8_t *clip = cd ? raster_text(cd, w, h, intrinsic_w) : NULL;
        uint8_t *mask = md ? raster_text(md, w, h, intrinsic_w) : NULL;
        if (layer) {
            uint8_t *filtered = gp_svgx_apply_filter(x, i, layer, w, h, (float)w / intrinsic_w);   /* (before the clip and the mask) */
            if (filtered) { free(layer); layer = filtered; }
            for (size_t p = 0; p < n; p++) {
                uint8_t *s = layer + p * 4, *o = out + p * 4;
                unsigned a = s[3];
                if (!a) continue;
                if (clip) a = a * clip[p * 4 + 3] / 255;
                if (mask) {
                    const uint8_t *m = mask + p * 4;
                    float lum = (0.2125f * (float)m[0] + 0.7154f * (float)m[1] + 0.0721f * (float)m[2]) * (float)m[3] / 255.f;   /* (straight color, alpha) */
                    a = (unsigned)((float)a * lum / 255.f + 0.5f);
                }
                if (!a) continue;
                unsigned r = s[0] * a / 255, g = s[1] * a / 255, b = s[2] * a / 255;   /* the layer, premultiplied */
                unsigned inv = 255 - a;
                o[0] = (uint8_t)(r + o[0] * inv / 255);
                o[1] = (uint8_t)(g + o[1] * inv / 255);
                o[2] = (uint8_t)(b + o[2] * inv / 255);
                o[3] = (uint8_t)(a + o[3] * inv / 255);
            }
        }
        free(layer);
        free(clip);
        free(mask);
        free(ld);
        free(cd);
        free(md);
    }
    return out;
}

uint8_t *gp_svg_render(const char *src, size_t len, uint32_t rgb, int w, int h, float intrinsic_w, gp_text_system *ts) {
    if (w <= 0 || h <= 0 || w > GP_SVG_MAX_RASTER || h > GP_SVG_MAX_RASTER) return NULL;
    if (!(intrinsic_w > 0.f)) return NULL;
    char *copy = with_color(src, len, rgb);
    if (!copy) return NULL;
    struct svgx *x = gp_svgx_parse(copy, strlen(copy), ts);
    uint8_t *px;
    if (x) {
        px = render_layers(x, w, h, intrinsic_w);
        gp_svgx_free(x);
        free(copy);
        return px;   /* (already premultiplied) */
    }
    px = raster_doc(copy, w, h, intrinsic_w);
    free(copy);
    if (px) gp_image_premultiply(px, (size_t)w * (size_t)h);   /* nanosvg hands back straight alpha */
    return px;
}

/* ---------------------------------------------------------------- public */

gp_svg gp_svg_load_memory(gp_app *app, const char *svg, size_t size) {
    if (!app || !svg || size == 0 || size > MAX_SVG_BYTES) return GP_SVG_NULL;
    float w, h;
    if (!gp_svg_probe(svg, size, &w, &h)) return GP_SVG_NULL;
    char *copy = malloc(size + 1);
    if (!copy) return GP_SVG_NULL;
    memcpy(copy, svg, size);
    copy[size] = 0;
    return gp_gfx_svg_add(app->gfx, copy, size, w, h);
}

gp_svg gp_svg_load_file(gp_app *app, const char *path) {
    if (!app || !path) return GP_SVG_NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return GP_SVG_NULL;
    char *buf = NULL;
    size_t len = 0, cap = 0;
    bool ok = true;
    for (;;) {
        if (len + 1 >= cap) {
            if (cap > MAX_SVG_BYTES) { ok = false; break; }
            cap = cap ? cap * 2 : 1 << 14;
            char *grown = realloc(buf, cap);
            if (!grown) { ok = false; break; }
            buf = grown;
        }
        size_t got = fread(buf + len, 1, cap - len - 1, f);
        len += got;
        if (got == 0) break;
    }
    if (ferror(f)) ok = false;
    fclose(f);
    gp_svg s = ok && len ? gp_svg_load_memory(app, buf, len) : GP_SVG_NULL;
    free(buf);
    return s;
}

/* A color as the SVG attribute value for it: "none" when invisible. */
static void paint_attr(char *out, size_t cap, gp_color c, const char *what) {
    if (c.a <= 0.f) {
        snprintf(out, cap, "%s=\"none\"", what);
        return;
    }
    int r = (int)lroundf(fminf(fmaxf(c.r, 0.f), 1.f) * 255.f), g = (int)lroundf(fminf(fmaxf(c.g, 0.f), 1.f) * 255.f),
        b = (int)lroundf(fminf(fmaxf(c.b, 0.f), 1.f) * 255.f);
    snprintf(out, cap, "%s=\"#%02x%02x%02x\" %s-opacity=\"%.4f\"", what, r, g, b, what, (double)fminf(c.a, 1.f));
}

gp_svg gp_svg_from_path(gp_app *app, const char *path_data, float width, float height, gp_color fill, gp_color stroke, float stroke_width) {
    if (!app || !path_data || !(width > 0.f) || !(height > 0.f)) return GP_SVG_NULL;
    for (const char *p = path_data; *p; p++)   /* the data goes inside an attribute: nothing that could end it */
        if (*p == '"' || *p == '<' || *p == '>' || *p == '&') return GP_SVG_NULL;
    char fa[96], sa[96];
    paint_attr(fa, sizeof fa, fill, "fill");
    paint_attr(sa, sizeof sa, stroke, "stroke");
    size_t cap = strlen(path_data) + 400;
    char *doc = malloc(cap);
    if (!doc) return GP_SVG_NULL;
    snprintf(doc, cap, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%g\" height=\"%g\" viewBox=\"0 0 %g %g\"><path d=\"%s\" %s %s stroke-width=\"%g\"/></svg>",
             (double)width, (double)height, (double)width, (double)height, path_data, fa, sa, (double)(stroke_width > 0.f ? stroke_width : 0.f));
    gp_svg s = gp_svg_load_memory(app, doc, strlen(doc));
    free(doc);
    return s;
}

bool gp_svg_size(gp_app *app, gp_svg svg, float *width, float *height) {
    return app && gp_gfx_svg_size(app->gfx, svg, width, height);
}

void gp_svg_release(gp_app *app, gp_svg svg) {
    if (!app) return;
    gp_gfx_svg_release(app->gfx, svg);
    gp_app_notify(app);
}
