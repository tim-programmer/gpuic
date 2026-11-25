/*
 * COLR color glyphs: FreeType reads the paint graph (v1) or the layers (v0), this draws them, in
 * software, into premultiplied RGBA for the color atlas. A v0 glyph is its layers: glyphs filled
 * with one palette color each, painted in order.
 *
 * A paint draws into the current layer (float premultiplied RGBA, the glyph's box) through a
 * coverage mask: PaintGlyph multiplies the mask by the glyph's outline, rasterized by FreeType
 * under the current transform, and draws its child through it; solids and gradients fill what the
 * mask lets through; PaintComposite draws its source and backdrop into layers of their own,
 * combines them with its mode (Porter-Duff, or a W3C blend mode) and draws the result through the
 * mask. Geometry is in font units, y up, mapped to pixels by one affine matrix; a gradient is
 * evaluated at each pixel center mapped back to font units. Colors interpolate premultiplied. The
 * foreground color (palette index 0xFFFF) is the one passed in, and the caller is told whether it
 * was used (then the glyph is cached per text color). The CPAL palette is the one asked for: an
 * index, or the first marked for a light or dark background (GP_PALETTE_LIGHT / _DARK), else 0.
 */
#include "internal.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_COLOR_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H
#include FT_TRUETYPE_TAGS_H

#if FREETYPE_MAJOR == 2 && FREETYPE_MINOR < 13 /* (the COLRv1 API is 2.13's) */
bool gp_colr_glyph(FT_Face face, uint32_t glyph, float px_size, float shift, int palette, const float fg[4], bool* used_fg, uint8_t** rgba, int box[4])
{
    (void)face, (void)glyph, (void)px_size, (void)shift, (void)palette, (void)fg, (void)used_fg, (void)rgba, (void)box;
    return false;
}
#else

# define MAX_DEPTH  64    /* paints within paints (a PaintColrGlyph can loop) */
# define MAX_PAINTS 10000 /* paints in all (glyphs reused through PaintColrGlyph can multiply) */

typedef struct
{
    double a, b, c, d, e, f; /* x' = a x + c y + e, y' = b x + d y + f */
} M;

typedef struct
{
    FT_Face face;
    FT_Color* pal;
    int npal;
    const float* fg; /* the foreground color, straight RGBA */
    bool used_fg;
    int w, h;
    int depth;
    int budget;   /* paints left to draw */
    double scale; /* pixels per font unit */
    bool bounds;  /* only collecting the box of what the glyph outlines cover */
    double bb[4];
} Ctx;

typedef struct
{
    float off;
    float c[4];
} Stop;

static double fx(FT_Fixed v)
{
    return (double)v / 65536.0;
}

static M mul(M p, M q) /* q first, then p */
{
    return (M){ p.a * q.a + p.c * q.b, p.b * q.a + p.d * q.b, p.a * q.c + p.c * q.d, p.b * q.c + p.d * q.d, p.a * q.e + p.c * q.f + p.e, p.b * q.e + p.d * q.f + p.f };
}

static M about(M t, double cx, double cy) /* t around (cx, cy) */
{
    return mul((M){ 1, 0, 0, 1, cx, cy }, mul(t, (M){ 1, 0, 0, 1, -cx, -cy }));
}

static M inverse(M m)
{
    double det = m.a * m.d - m.b * m.c;
    if(fabs(det) < 1e-12) {
        return (M){ 0, 0, 0, 0, 1e30, 1e30 };
    }
    double a = m.d / det, b = -m.b / det, c = -m.c / det, d = m.a / det;
    return (M){ a, b, c, d, -(a * m.e + c * m.f), -(b * m.e + d * m.f) };
}

static void color(Ctx* x, FT_UInt16 index, FT_F2Dot14 alpha, float out[4])
{
    float r = 0, g = 0, b = 0, a = 0;
    if(index == 0xFFFF) {
        r = x->fg[0], g = x->fg[1], b = x->fg[2], a = x->fg[3];
        x->used_fg = true;
    } else if(index < x->npal) {
        FT_Color c = x->pal[index];
        r = c.red / 255.f, g = c.green / 255.f, b = c.blue / 255.f, a = c.alpha / 255.f;
    }
    a *= (float)alpha / 16384.f;
    out[0] = r * a, out[1] = g * a, out[2] = b * a, out[3] = a;
}

/* ---------------------------------------------------------------- the mask of a glyph's outline */

typedef struct
{
    float* mask;
    int w, h;
} Spans;

static void spans(int y, int count, const FT_Span* s, void* user)
{
    Spans* sp = user;
    int row = sp->h - 1 - y; /* (the raster is y up) */
    if(row < 0 || row >= sp->h) {
        return;
    }
    for(int i = 0; i < count; i++) {
        for(int k = 0; k < s[i].len; k++) {
            int col = s[i].x + k;
            if(col >= 0 && col < sp->w) {
                sp->mask[(size_t)row * sp->w + col] = s[i].coverage / 255.f;
            }
        }
    }
}

/* the coverage of an outline in font units under m (w*h floats), or NULL if it has none; in bounds
 * mode its box goes into x->bb */
static float* outline_mask(Ctx* x, const FT_Outline* src, M m)
{
    if(!src->n_points) {
        return NULL;
    }
    if(x->bounds) {
        for(int i = 0; i < src->n_points; i++) {
            double px = src->points[i].x, py = src->points[i].y;
            double X = m.a * px + m.c * py + m.e, Y = m.b * px + m.d * py + m.f;
            x->bb[0] = fmin(x->bb[0], X), x->bb[1] = fmin(x->bb[1], Y);
            x->bb[2] = fmax(x->bb[2], X), x->bb[3] = fmax(x->bb[3], Y);
        }
        return NULL;
    }
    FT_Library lib = x->face->glyph->library;
    FT_Outline o;
    if(FT_Outline_New(lib, (FT_UInt)src->n_points, src->n_contours, &o)) {
        return NULL;
    }
    memcpy(o.tags, src->tags, (size_t)src->n_points);
    memcpy(o.contours, src->contours, (size_t)src->n_contours * sizeof *o.contours);
    o.flags = src->flags;
    for(int i = 0; i < src->n_points; i++) {
        double px = src->points[i].x, py = src->points[i].y;
        o.points[i].x = (FT_Pos)lround((m.a * px + m.c * py + m.e) * 64);
        o.points[i].y = (FT_Pos)lround((x->h - (m.b * px + m.d * py + m.f)) * 64);
    }
    float* mask = calloc((size_t)x->w * x->h, sizeof *mask);
    Spans sp = { mask, x->w, x->h };
    FT_Raster_Params params = { 0 };
    params.source = &o;
    params.flags = FT_RASTER_FLAG_AA | FT_RASTER_FLAG_DIRECT | FT_RASTER_FLAG_CLIP;
    params.gray_spans = spans;
    params.user = &sp;
    params.clip_box = (FT_BBox){ 0, 0, x->w, x->h };
    FT_Outline_Render(lib, &o, &params);
    FT_Outline_Done(lib, &o);
    return mask;
}

static float* glyph_mask(Ctx* x, FT_UInt gid, M m)
{
    if(FT_Load_Glyph(x->face, gid, FT_LOAD_NO_SCALE | FT_LOAD_IGNORE_TRANSFORM)
        || x->face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
        return NULL;
    }
    return outline_mask(x, &x->face->glyph->outline, m);
}

/* the coverage of glyph `gid`'s clip box under m, or NULL if it has none */
static float* clip_box_mask(Ctx* x, FT_UInt gid, M m)
{
    FT_ClipBox cb;
    if(!FT_Get_Color_Glyph_ClipBox(x->face, gid, &cb)) {
        return NULL;
    }
    FT_Vector pts[4] = { cb.bottom_left, cb.top_left, cb.top_right, cb.bottom_right };
    for(int i = 0; i < 4; i++) { /* (26.6 pixels at the face's size -> font units) */
        pts[i] = (FT_Vector){ lround(pts[i].x / 64. / x->scale), lround(pts[i].y / 64. / x->scale) };
    }
    char tags[4] = { FT_CURVE_TAG_ON, FT_CURVE_TAG_ON, FT_CURVE_TAG_ON, FT_CURVE_TAG_ON };
    unsigned short end = 3;
    FT_Outline o = { .n_contours = 1, .n_points = 4, .points = pts, .tags = (void*)tags, .contours = (void*)&end }; /* (their types vary between FreeType versions) */
    return outline_mask(x, &o, m);
}

/* ---------------------------------------------------------------- fills */

static int stops(Ctx* x, const FT_ColorLine* line, Stop** out)
{
    int n = 0, cap = 0;
    Stop* s = NULL;
    FT_ColorStop cs;
    FT_ColorStopIterator it = line->color_stop_iterator;
    while(FT_Get_Colorline_Stops(x->face, &cs, &it)) {
        if(n == cap) {
            s = realloc(s, (size_t)(cap = cap * 2 + 8) * sizeof *s);
        }
        Stop t = { (float)fx(cs.stop_offset), { 0 } };
        color(x, cs.color.palette_index, cs.color.alpha, t.c);
        int i = n++;
        for(; i > 0 && s[i - 1].off > t.off; i--) { /* (sorted, stable) */
            s[i] = s[i - 1];
        }
        s[i] = t;
    }
    *out = s;
    return n;
}

static void lookup(const Stop* s, int n, FT_PaintExtend extend, double t, float out[4])
{
    double first = s[0].off, last = s[n - 1].off, span = last - first;
    if(span > 1e-9 && extend != FT_COLR_PAINT_EXTEND_PAD) {
        double u = (t - first) / span;
        if(extend == FT_COLR_PAINT_EXTEND_REPEAT) {
            u -= floor(u);
        } else {
            u -= 2 * floor(u / 2);
            if(u > 1) {
                u = 2 - u;
            }
        }
        t = first + u * span;
    }
    if(t <= first) {
        memcpy(out, s[0].c, sizeof s[0].c);
        return;
    }
    for(int i = 1; i < n; i++) {
        if(t <= s[i].off) {
            float k = (float)((t - s[i - 1].off) / (s[i].off - s[i - 1].off));
            for(int j = 0; j < 4; j++) {
                out[j] = s[i - 1].c[j] + (s[i].c[j] - s[i - 1].c[j]) * k;
            }
            return;
        }
    }
    memcpy(out, s[n - 1].c, sizeof s[0].c);
}

/* the gradient parameter at point (px, py) in font units; false where the gradient paints nothing
 */
static bool gradient_t(const FT_COLR_Paint* p, FT_PaintExtend extend, double px, double py, double* t)
{
    if(p->format == FT_COLR_PAINTFORMAT_LINEAR_GRADIENT) {
        const FT_PaintLinearGradient* g = &p->u.linear_gradient;
        double x0 = fx(g->p0.x), y0 = fx(g->p0.y), dx = fx(g->p1.x) - x0, dy = fx(g->p1.y) - y0;
        double nx = -(fx(g->p2.y) - y0), ny = fx(g->p2.x) - x0, nn = nx * nx + ny * ny;
        if(nn > 1e-12) { /* p0->p1 projected onto the normal of p0->p2 */
            double k = (dx * nx + dy * ny) / nn;
            dx = nx * k, dy = ny * k;
        }
        double len = dx * dx + dy * dy;
        if(len < 1e-12) {
            return false;
        }
        *t = ((px - x0) * dx + (py - y0) * dy) / len;
        return true;
    }
    if(p->format == FT_COLR_PAINTFORMAT_RADIAL_GRADIENT) { /* the largest t with |p - c(t)| = r(t) >= 0 */
        const FT_PaintRadialGradient* g = &p->u.radial_gradient;
        double cx = fx(g->c0.x), cy = fx(g->c0.y), r0 = fx(g->r0);
        double cdx = fx(g->c1.x) - cx, cdy = fx(g->c1.y) - cy, dr = fx(g->r1) - r0;
        double pdx = px - cx, pdy = py - cy;
        double a = cdx * cdx + cdy * cdy - dr * dr, b = pdx * cdx + pdy * cdy + r0 * dr,
               c = pdx * pdx + pdy * pdy - r0 * r0;
        if(fabs(a) < 1e-9) {
            if(fabs(b) < 1e-12) {
                return false;
            }
            *t = c / (2 * b);
            return r0 + *t * dr >= 0;
        }
        double disc = b * b - a * c;
        if(disc < 0) {
            return false;
        }
        double t1 = (b + sqrt(disc)) / a, t2 = (b - sqrt(disc)) / a;
        if(t1 < t2) {
            double s = t1;
            t1 = t2, t2 = s;
        }
        *t = r0 + t1 * dr >= 0 ? t1 : t2;
        return r0 + *t * dr >= 0;
    }
    const FT_PaintSweepGradient* g = &p->u.sweep_gradient;                             /* counter-clockwise from the x axis, in half turns */
    double start = (fx(g->start_angle) + 1) * 180, end = (fx(g->end_angle) + 1) * 180; /* (stored biased by a half turn) */
    double angle = atan2(py - fx(g->center.y), px - fx(g->center.x)) * 180 / M_PI;
    if(angle < 0) {
        angle += 360;
    }
    if(fabs(end - start) < 1e-9) { /* (padded: the first color before the angle, the last from it on; else nothing) */
        *t = angle < start ? 0 : 1;
        return extend == FT_COLR_PAINT_EXTEND_PAD;
    }
    *t = (angle - start) / (end - start);
    return true;
}

/* src-over a solid or gradient into dst through clip (NULL: everywhere) */
static void fill(Ctx* x, const FT_COLR_Paint* p, M m, const float* clip, float* dst)
{
    float c[4];
    Stop* s = NULL;
    int n = 0;
    FT_PaintExtend extend = FT_COLR_PAINT_EXTEND_PAD;
    if(p->format == FT_COLR_PAINTFORMAT_SOLID) {
        color(x, p->u.solid.color.palette_index, p->u.solid.color.alpha, c);
    } else {
        const FT_ColorLine* line = p->format == FT_COLR_PAINTFORMAT_LINEAR_GRADIENT
                                       ? &p->u.linear_gradient.colorline
                                   : p->format == FT_COLR_PAINTFORMAT_RADIAL_GRADIENT ? &p->u.radial_gradient.colorline
                                                                                      : &p->u.sweep_gradient.colorline;
        extend = line->extend;
        if(!(n = stops(x, line, &s))) {
            return;
        }
    }
    M inv = inverse(m);
    for(int y = 0; y < x->h; y++) {
        for(int i = y * x->w, end = i + x->w; i < end; i++) {
            float cov = clip ? clip[i] : 1;
            if(cov <= 0) {
                continue;
            }
            if(n) {
                double X = i - y * x->w + .5, Y = y + .5, t;
                if(!gradient_t(
                       p, extend, inv.a * X + inv.c * Y + inv.e, inv.b * X + inv.d * Y + inv.f, &t)) {
                    continue;
                }
                lookup(s, n, extend, t, c);
            }
            float *o = dst + (size_t)i * 4, k = 1 - c[3] * cov;
            for(int j = 0; j < 4; j++) {
                o[j] = c[j] * cov + o[j] * k;
            }
        }
    }
    free(s);
}

/* ---------------------------------------------------------------- compositing (premultiplied;
 * blend modes as in W3C Compositing) */

static float blend1(int mode, float cb, float cs)
{
    switch(mode) {
        case FT_COLR_COMPOSITE_MULTIPLY:
            return cb * cs;
        case FT_COLR_COMPOSITE_SCREEN:
            return cb + cs - cb * cs;
        case FT_COLR_COMPOSITE_OVERLAY:
            return blend1(FT_COLR_COMPOSITE_HARD_LIGHT, cs, cb);
        case FT_COLR_COMPOSITE_DARKEN:
            return fminf(cb, cs);
        case FT_COLR_COMPOSITE_LIGHTEN:
            return fmaxf(cb, cs);
        case FT_COLR_COMPOSITE_COLOR_DODGE:
            return cb <= 0 ? 0 : cs >= 1 ? 1
                                         : fminf(1, cb / (1 - cs));
        case FT_COLR_COMPOSITE_COLOR_BURN:
            return cb >= 1 ? 1 : cs <= 0 ? 0
                                         : 1 - fminf(1, (1 - cb) / cs);
        case FT_COLR_COMPOSITE_HARD_LIGHT:
            return cs <= .5f ? cb * 2 * cs : blend1(FT_COLR_COMPOSITE_SCREEN, cb, 2 * cs - 1);
        case FT_COLR_COMPOSITE_SOFT_LIGHT: {
            if(cs <= .5f) {
                return cb - (1 - 2 * cs) * cb * (1 - cb);
            }
            float d = cb <= .25f ? ((16 * cb - 12) * cb + 4) * cb : sqrtf(cb);
            return cb + (2 * cs - 1) * (d - cb);
        }
        case FT_COLR_COMPOSITE_DIFFERENCE:
            return fabsf(cb - cs);
        default: /* exclusion */
            return cb + cs - 2 * cb * cs;
    }
}

static float lum(const float c[3])
{
    return .3f * c[0] + .59f * c[1] + .11f * c[2];
}
static void set_lum(float c[3], float l)
{
    float d = l - lum(c);
    for(int i = 0; i < 3; i++) {
        c[i] += d;
    }
    l = lum(c);
    float n = fminf(c[0], fminf(c[1], c[2])), x = fmaxf(c[0], fmaxf(c[1], c[2]));
    for(int i = 0; i < 3; i++) {
        if(n < 0) {
            c[i] = l + (c[i] - l) * l / (l - n);
        }
        if(x > 1) {
            c[i] = l + (c[i] - l) * (1 - l) / (x - l);
        }
    }
}
static void set_sat(float c[3], float s)
{
    int hi = 0, lo = 0;
    for(int i = 1; i < 3; i++) {
        hi = c[i] > c[hi] ? i : hi;
        lo = c[i] < c[lo] ? i : lo;
    }
    if(hi == lo) {
        c[0] = c[1] = c[2] = 0;
        return;
    }
    int mid = 3 - hi - lo;
    c[mid] = (c[mid] - c[lo]) * s / (c[hi] - c[lo]);
    c[hi] = s, c[lo] = 0;
}
static float sat(const float c[3])
{
    return fmaxf(c[0], fmaxf(c[1], c[2])) - fminf(c[0], fminf(c[1], c[2]));
}

static void composite(int mode, const float* s, float* b) /* the result into b */
{
    static const signed char pd[][2] = {
        /* Porter-Duff: Fa, Fb as 0, 1, ab (2), 1-ab (3), as (2), 1-as (3) */
        { 0, 0 },
        { 1, 0 },
        { 0, 1 },
        { 1, 3 },
        { 3, 1 },
        { 2, 0 },
        { 0, 2 },
        { 3, 0 },
        { 0, 3 },
        { 2, 3 },
        { 3, 2 },
        { 3, 3 },
        { 1, 1 }
    };
    float as = s[3], ab = b[3], out[4];
    if(mode <= FT_COLR_COMPOSITE_PLUS) {
        float fa = pd[mode][0] < 2 ? pd[mode][0] : pd[mode][0] == 2 ? ab
                                                                    : 1 - ab;
        float fb = pd[mode][1] < 2 ? pd[mode][1] : pd[mode][1] == 2 ? as
                                                                    : 1 - as;
        for(int j = 0; j < 4; j++) {
            out[j] = fminf(1, s[j] * fa + b[j] * fb);
        }
    } else {
        float cs[3], cb[3], r[3];
        for(int j = 0; j < 3; j++) {
            cs[j] = as > 0 ? s[j] / as : 0;
            cb[j] = ab > 0 ? b[j] / ab : 0;
        }
        if(mode < FT_COLR_COMPOSITE_HSL_HUE) {
            for(int j = 0; j < 3; j++) {
                r[j] = blend1(mode, cb[j], cs[j]);
            }
        } else {
            memcpy(r, mode == FT_COLR_COMPOSITE_HSL_LUMINOSITY ? cb : cs, sizeof r);
            if(mode == FT_COLR_COMPOSITE_HSL_HUE) {
                set_sat(r, sat(cb));
            } else if(mode == FT_COLR_COMPOSITE_HSL_SATURATION) {
                memcpy(r, cb, sizeof r);
                set_sat(r, sat(cs));
            }
            set_lum(r, lum(mode == FT_COLR_COMPOSITE_HSL_LUMINOSITY ? cs : cb));
        }
        for(int j = 0; j < 3; j++) {
            out[j] = s[j] * (1 - ab) + b[j] * (1 - as) + as * ab * r[j];
        }
        out[3] = as + ab - as * ab;
    }
    memcpy(b, out, sizeof out);
}

/* ---------------------------------------------------------------- the graph */

static void paint(Ctx* x, FT_OpaquePaint op, M m, const float* clip, float* dst)
{
    FT_COLR_Paint p;
    if(x->depth >= MAX_DEPTH || --x->budget < 0 || !FT_Get_Paint(x->face, op, &p)) {
        return;
    }
    x->depth++;
    M t = { 1, 0, 0, 1, 0, 0 };
    FT_OpaquePaint child = { NULL, 0 };
    switch(p.format) {
        case FT_COLR_PAINTFORMAT_COLR_LAYERS: {
            FT_LayerIterator it = p.u.colr_layers.layer_iterator;
            FT_OpaquePaint layer = { NULL, 0 };
            while(FT_Get_Paint_Layers(x->face, &it, &layer)) {
                paint(x, layer, m, clip, dst);
            }
            break;
        }
        case FT_COLR_PAINTFORMAT_SOLID:
        case FT_COLR_PAINTFORMAT_LINEAR_GRADIENT:
        case FT_COLR_PAINTFORMAT_RADIAL_GRADIENT:
        case FT_COLR_PAINTFORMAT_SWEEP_GRADIENT:
            if(!x->bounds) {
                fill(x, &p, m, clip, dst);
            }
            break;
        case FT_COLR_PAINTFORMAT_GLYPH: {
            float* mask = glyph_mask(x, p.u.glyph.glyphID, m);
            if(mask) {
                for(size_t i = 0; clip && i < (size_t)x->w * x->h; i++) {
                    mask[i] *= clip[i];
                }
                paint(x, p.u.glyph.paint, m, mask, dst);
                free(mask);
            }
            break;
        }
        case FT_COLR_PAINTFORMAT_COLR_GLYPH: {
            FT_OpaquePaint root = { NULL, 0 };
            if(!FT_Get_Color_Glyph_Paint(x->face, p.u.colr_glyph.glyphID, FT_COLOR_NO_ROOT_TRANSFORM, &root)) {
                break;
            }
            float* box = clip_box_mask(x, p.u.colr_glyph.glyphID, m); /* (its clip box applies) */
            for(size_t i = 0; box && clip && i < (size_t)x->w * x->h; i++) {
                box[i] *= clip[i];
            }
            paint(x, root, m, box ? box : clip, dst);
            free(box);
            break;
        }
        case FT_COLR_PAINTFORMAT_TRANSFORM: {
            const FT_Affine23* a = &p.u.transform.affine;
            t = (M){ fx(a->xx), fx(a->yx), fx(a->xy), fx(a->yy), fx(a->dx), fx(a->dy) };
            child = p.u.transform.paint;
            break;
        }
        case FT_COLR_PAINTFORMAT_TRANSLATE:
            t.e = fx(p.u.translate.dx), t.f = fx(p.u.translate.dy);
            child = p.u.translate.paint;
            break;
        case FT_COLR_PAINTFORMAT_SCALE:
            t = about((M){ fx(p.u.scale.scale_x), 0, 0, fx(p.u.scale.scale_y), 0, 0 },
                fx(p.u.scale.center_x),
                fx(p.u.scale.center_y));
            child = p.u.scale.paint;
            break;
        case FT_COLR_PAINTFORMAT_ROTATE: {
            double r = fx(p.u.rotate.angle) * M_PI, c = cos(r), s = sin(r);
            t = about((M){ c, s, -s, c, 0, 0 }, fx(p.u.rotate.center_x), fx(p.u.rotate.center_y));
            child = p.u.rotate.paint;
            break;
        }
        case FT_COLR_PAINTFORMAT_SKEW: /* (a positive x angle leans the y axis left, as fontTools has
                                          it) */
            t = about((M){ 1, tan(fx(p.u.skew.y_skew_angle) * M_PI), -tan(fx(p.u.skew.x_skew_angle) * M_PI), 1, 0, 0 },
                fx(p.u.skew.center_x),
                fx(p.u.skew.center_y));
            child = p.u.skew.paint;
            break;
        case FT_COLR_PAINTFORMAT_COMPOSITE: {
            size_t n = (size_t)x->w * x->h * 4;
            float* src = x->bounds ? NULL : calloc(n, sizeof *src);
            float* back = x->bounds ? NULL : calloc(n, sizeof *back);
            paint(x, p.u.composite.backdrop_paint, m, NULL, back);
            paint(x, p.u.composite.source_paint, m, NULL, src);
            if(!x->bounds) {
                for(size_t i = 0; i < n; i += 4) {
                    composite(p.u.composite.composite_mode, src + i, back + i);
                    float cov = clip ? clip[i / 4] : 1, k = 1 - back[i + 3] * cov;
                    for(int j = 0; j < 4; j++) {
                        dst[i + j] = back[i + j] * cov + dst[i + j] * k;
                    }
                }
            }
            free(src);
            free(back);
            break;
        }
        default:
            break;
    }
    if(child.p) {
        paint(x, child, mul(m, t), clip, dst);
    }
    x->depth--;
}

/* a COLRv0 glyph: each layer's glyph filled with its color, in order */
static void layers_v0(Ctx* x, FT_UInt glyph, M m, float* dst)
{
    FT_LayerIterator it = { 0 };
    FT_UInt gid, index;
    while(FT_Get_Color_Glyph_Layer(x->face, glyph, &gid, &index, &it)) {
        float* mask = glyph_mask(x, gid, m);
        if(mask) {
            FT_COLR_Paint solid = { .format = FT_COLR_PAINTFORMAT_SOLID };
            solid.u.solid.color = (FT_ColorIndex){ (FT_UInt16)index, 1 << 14 };
            fill(x, &solid, m, mask, dst);
            free(mask);
        }
    }
}

static uint32_t be(const uint8_t* p, int n)
{
    uint32_t v = 0;
    for(int i = 0; i < n; i++) {
        v = v << 8 | p[i];
    }
    return v;
}

/* the palette `want` names (see the top), of npal. The background flags are read from the CPAL table itself: FreeType (2.14.3) reads its
 * 32-bit palette types as 16-bit ones. */
static int pick_palette(FT_Face face, int npal, int want)
{
    if(want >= 0) {
        return want < npal ? want : 0;
    }
    uint32_t flag = want == GP_PALETTE_DARK ? 2 : 1;
    FT_ULong len = 0;
    if(FT_Load_Sfnt_Table(face, TTAG_CPAL, 0, NULL, &len) || len < 12) {
        return 0;
    }
    uint8_t* t = malloc(len);
    int pick = 0;
    size_t field = 12 + 2 * (size_t)npal; /* (paletteTypesArrayOffset, in version 1) */
    if(!FT_Load_Sfnt_Table(face, TTAG_CPAL, 0, t, &len) && be(t, 2) >= 1 && field + 4 <= len) {
        size_t off = be(t + field, 4);
        for(int i = 0; off && i < npal && off + 4 * (size_t)i + 4 <= len; i++) {
            if(be(t + off + 4 * (size_t)i, 4) & flag) {
                pick = i;
                break;
            }
        }
    }
    free(t);
    return pick;
}

/* Glyph `glyph` of a COLR font at px_size, its origin `shift` px right, with CPAL palette `palette`
 * and foreground color `fg` (straight RGBA; *used_fg says whether it was used): premultiplied RGBA
 * in *rgba (w*h, NULL if empty) and its box (left, top above the baseline, w, h). False if the
 * glyph has no color (v1 paint or v0 layers). Sets the face's char size. */
bool gp_colr_glyph(FT_Face face, uint32_t glyph, float px_size, float shift, int palette, const float fg[4], bool* used_fg, uint8_t** rgba, int box[4])
{
    FT_OpaquePaint root = { NULL, 0 };
    *rgba = NULL;
    *used_fg = false;
    box[0] = box[1] = box[2] = box[3] = 0;
    bool v1 = FT_Get_Color_Glyph_Paint(face, glyph, FT_COLOR_NO_ROOT_TRANSFORM, &root);
    FT_LayerIterator probe = { 0 };
    FT_UInt probe_gid, probe_index;
    if(!v1 && !FT_Get_Color_Glyph_Layer(face, glyph, &probe_gid, &probe_index, &probe)) {
        return false;
    }
    FT_Set_Char_Size(face, 0, (FT_F26Dot6)(px_size * 64.f + .5f), 72, 72);
    double s = px_size / (double)face->units_per_EM;
    Ctx x = {
        .face = face, .fg = fg, .budget = MAX_PAINTS, .scale = s, .bb = { 1e30, 1e30, -1e30, -1e30 }
    };
    FT_Palette_Data pd;
    if(!FT_Palette_Data_Get(face, &pd) && pd.num_palettes && !FT_Palette_Select(face, (FT_UShort)pick_palette(face, pd.num_palettes, palette), &x.pal)) {
        x.npal = pd.num_palette_entries;
    }
    M m = { s, 0, 0, -s, shift, 0 }; /* font units, y up -> pixels, y down from the origin */
    FT_ClipBox cb;
    if(v1 && FT_Get_Color_Glyph_ClipBox(face, glyph, &cb)) {
        x.bb[0] = (cb.bottom_left.x + 0.) / 64 + shift,
        x.bb[2] = (cb.top_right.x + 0.) / 64 + shift;
        x.bb[1] = -(cb.top_right.y + 0.) / 64, x.bb[3] = -(cb.bottom_left.y + 0.) / 64;
    } else {
        x.bounds = true;
        if(v1) {
            paint(&x, root, m, NULL, NULL);
        } else {
            layers_v0(&x, glyph, m, NULL);
        }
        x.bounds = false;
        x.budget = MAX_PAINTS;
    }
    if(x.bb[2] <= x.bb[0] || x.bb[3] <= x.bb[1]) {
        return true;
    }
    int left = (int)floor(x.bb[0]), top = (int)floor(x.bb[1]);
    x.w = (int)ceil(x.bb[2]) - left, x.h = (int)ceil(x.bb[3]) - top;
    if(x.w > GP_COLOR_ATLAS_SIZE || x.h > GP_COLOR_ATLAS_SIZE) { /* (it would not fit) */
        return true;
    }
    m.e -= left, m.f -= top;
    float* layer = calloc((size_t)x.w * x.h * 4, sizeof *layer);
    if(v1) {
        paint(&x, root, m, NULL, layer);
    } else {
        layers_v0(&x, glyph, m, layer);
    }
    uint8_t* out = malloc((size_t)x.w * x.h * 4);
    for(size_t i = 0; i < (size_t)x.w * x.h; i++) {
        for(int j = 0; j < 4; j++) {
            out[i * 4 + j] = (uint8_t)lroundf(fminf(fmaxf(layer[i * 4 + j], 0), 1) * 255);
        }
    }
    free(layer);
    *used_fg = x.used_fg;
    *rgba = out;
    box[0] = left, box[1] = -top, box[2] = x.w, box[3] = x.h;
    return true;
}
#endif
