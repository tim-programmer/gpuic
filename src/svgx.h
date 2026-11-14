/*
 * What svgx.c (the document: parsing, CSS, <use>, text, markers, layers, clips, masks, patterns, images) and svgfx.c (filters) share. Internal.
 */
#ifndef GPUIC_SVGX_H
#define GPUIC_SVGX_H

#include "internal.h"

/* An element of the document: its name (a text node is "#text"), attributes in order, children, the text inside as written. */
typedef struct xn {
    char *name;
    char **k, **v;
    int na, cap_a;
    struct xn **kid;
    int nk, cap_k;
    char *text;
    struct xn *parent;
    int unit;              /* the layer a shape is drawn in (-1: none) */
} xn;

/* A 2D affine map: x' = a x + c y + e, y' = b x + d y + f. */
typedef struct { double a, b, c, d, e, f; } svgx_m2;

const char *svgx_attr(const xn *x, const char *k);
float svgx_num(const xn *x, const char *k, float def);
/* a property of the element: its style attribute first, then the attribute; copied into buf */
const char *svgx_prop(const xn *x, const char *prop, char *buf, size_t cap);
/* a color (#rgb, #rrggbb, #rrggbbaa, rgb(), rgba(), hsl(), hsla(), the CSS names, transparent); false if none of those */
bool svgx_color(const char *v, float rgb[3], float *alpha);
/* a length in user units: px, pt, pc, mm, cm, in, em (16 px); % of `percent_of` */
float svgx_length(const char *v, float def, float percent_of);
svgx_m2 svgx_mul(svgx_m2 p, svgx_m2 q);   /* q first, then p */
svgx_m2 svgx_inverse(svgx_m2 m);
/* the matrix a transform list stands for (as nanosvg reads it) */
svgx_m2 svgx_transform(const char *list);

struct svgx;
/* For the filters: about the element a layer is for. */
typedef struct {
    struct svgx *d;
    int unit;
    int w, h;
    float scale;
} svgx_rc;
xn *svgx_find(struct svgx *d, const char *id);           /* an element by id (the document and what was taken out of it) */
bool svgx_bbox(svgx_rc *rc, float bb[4]);                 /* the layer element's box in its user space: x, y, w, h; false if it has none */
svgx_m2 svgx_device(svgx_rc *rc);                         /* its user space to device pixels */
float svgx_viewport_diag(svgx_rc *rc, int axis);          /* what a percentage is of: 0 width, 1 height, 2 the normalized diagonal */
/* the coverage (0..1 floats, w*h) of a rectangle in the element's user space, or NULL when it is everything */
float *svgx_rect_cover(svgx_rc *rc, float x, float y, float w, float h);
/* an element (by id) drawn in the layer element's user space, translated by (tx, ty) in it; premultiplied bytes, w*h */
uint8_t *svgx_render_ref(svgx_rc *rc, xn *el, float tx, float ty);
/* a picture from an href (a data: URL with PNG, JPEG, GIF, BMP or SVG): premultiplied RGBA px x py, its own size in user units iw x ih (an SVG is drawn at
 * `device_scale` pixels per unit). false if it is none of those. */
typedef struct { uint8_t *px; int pw, ph; float iw, ih; } svgx_pic;
bool svgx_load_href(svgx_rc *rc, const char *href, float device_scale, svgx_pic *out);
/* where a box of size (w, h) goes in a viewport (vx, vy, vw, vh) under preserveAspectRatio `par`: scale sx, sy and translation; *slice: it overflows */
void svgx_fit(const char *par, float w, float h, float vx, float vy, float vw, float vh, double *sx, double *sy, double *tx, double *ty, bool *slice);
/* a sample of a premultiplied picture at (u, v) in its pixels (centers at .5), bilinear, outside transparent; out: 4 floats 0..255 */
void svgx_sample(const uint8_t *px, int w, int h, double u, double v, bool nearest, float out[4]);

/* svgfx.c: the filter on a layer (premultiplied bytes in and out, w*h); NULL: the layer is to be drawn as it is */
uint8_t *svgx_filter(svgx_rc *rc, xn *filter, const uint8_t *premul);

#endif
