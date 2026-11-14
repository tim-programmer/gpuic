/*
 * SVG: a document is kept as text and drawn by rasterizing it (nanosvg, vendored in third_party/) at the size, in device
 * pixels, it is shown at, into an image that the image path then draws; gfx.c caches those rasters.
 *
 * `currentColor` in the document means the text color of the element that shows it, so the text is rewritten with that
 * color before it is parsed: one parse per (document, size, color) raster, which for icons is microseconds.
 *
 * What nanosvg understands: paths (all commands, arcs included), rect, circle, ellipse, line, polyline, polygon; groups and
 * transforms; fill and stroke (colors, opacity, linear and radial gradients, fill-rule, widths, joins, caps, dashes);
 * <defs> and gradient references; <style> with simple class selectors. svgx.c does the rest (CSS, <use>, clips, masks, filters, patterns, markers,
 * images, group opacity, <text>) by rewriting the document and putting layers drawn by nanosvg together; this file is what knows nanosvg.
 */
#include "internal.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

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
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#if !defined(__clang__)   /* (warnings clang does not have: naming them would be a warning of its own) */
#pragma GCC diagnostic ignored "-Wstringop-truncation"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"   /* (nsvg__parseTransform's t[6], which every path that reads it has set first) */
#endif
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

/* One document drawn at w x h, `scale` device pixels per unit of the document (straight alpha, as nanosvg hands it back), or NULL. */
uint8_t *gp_svg_nsvg_raster(const char *doc, int w, int h, float scale) {
    if (w <= 0 || h <= 0) return NULL;
    char *copy = strdup(doc);   /* (nanosvg works in place) */
    NSVGimage *im = copy ? nsvgParse(copy, "px", 96.f) : NULL;
    free(copy);
    if (!im) return NULL;
    uint8_t *px = calloc((size_t)w * (size_t)h, 4);
    NSVGrasterizer *rast = px ? nsvgCreateRasterizer() : NULL;
    if (rast) {
        nsvgRasterize(rast, im, 0, 0, scale, px, w, h, w * 4);
    } else {
        free(px);
        px = NULL;
    }
    if (rast) nsvgDeleteRasterizer(rast);
    nsvgDelete(im);
    return px;
}

/* What nanosvg makes of a document without drawing it: the union of its shapes' boxes (with half the stroke width when `stroke`), its size, and the first
 * path's points 0, 3 and 6 (the ends of its first two segments: the probe path `M0 0L1 0L0 1` gives the transform it is under). Returns the number of shapes, -1 if
 * it does not parse. */
int gp_svg_nsvg_info(const char *doc, bool stroke, float bounds[4], float size[2], float first[6]) {
    char *copy = strdup(doc);
    NSVGimage *im = copy ? nsvgParse(copy, "px", 96.f) : NULL;
    free(copy);
    if (!im) return -1;
    int n = 0;
    if (bounds) bounds[0] = bounds[1] = bounds[2] = bounds[3] = 0.f;
    for (NSVGshape *sh = im->shapes; sh; sh = sh->next) {
        float e = stroke && sh->stroke.type != NSVG_PAINT_NONE ? sh->strokeWidth * 0.5f : 0.f;
        if (bounds) {
            float b[4] = { sh->bounds[0] - e, sh->bounds[1] - e, sh->bounds[2] + e, sh->bounds[3] + e };
            if (n == 0) memcpy(bounds, b, sizeof b);
            else {
                bounds[0] = fminf(bounds[0], b[0]);
                bounds[1] = fminf(bounds[1], b[1]);
                bounds[2] = fmaxf(bounds[2], b[2]);
                bounds[3] = fmaxf(bounds[3], b[3]);
            }
        }
        if (n == 0 && first) {
            const NSVGpath *pa = sh->paths;
            if (pa && pa->npts >= 7) {
                first[0] = pa->pts[0]; first[1] = pa->pts[1];
                first[2] = pa->pts[6]; first[3] = pa->pts[7];
                first[4] = pa->pts[12]; first[5] = pa->pts[13];
            } else {
                memset(first, 0, 6 * sizeof *first);
            }
        }
        n++;
    }
    if (size) { size[0] = im->width; size[1] = im->height; }
    nsvgDelete(im);
    return n;
}

/* A color name SVG knows (the CSS keywords), as 0xRRGGBB. */
bool gp_svg_named_color(const char *name, size_t len, uint32_t *rgb) {
    for (size_t i = 0; i < sizeof nsvg__colors / sizeof *nsvg__colors; i++)
        if (strlen(nsvg__colors[i].name) == len && strncasecmp(nsvg__colors[i].name, name, len) == 0) {
            unsigned c = nsvg__colors[i].color;   /* (nanosvg's order: r | g << 8 | b << 16) */
            *rgb = (c & 255) << 16 | (c >> 8 & 255) << 8 | (c >> 16 & 255);
            return true;
        }
    return false;
}

uint8_t *gp_svg_render(const char *src, size_t len, uint32_t rgb, int w, int h, float intrinsic_w, gp_text_system *ts) {
    if (w <= 0 || h <= 0 || w > GP_SVG_MAX_RASTER || h > GP_SVG_MAX_RASTER) return NULL;
    if (!(intrinsic_w > 0.f)) return NULL;
    char *copy = with_color(src, len, rgb);
    if (!copy) return NULL;
    uint8_t *px = gp_svgx_render_doc(copy, strlen(copy), w, h, (float)w / intrinsic_w, ts, 0);   /* (premultiplied) */
    free(copy);
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
    gp_views_svg_released(app, svg);   /* the views that show it, not every view */
}
