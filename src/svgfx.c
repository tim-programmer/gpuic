/*
 * SVG filters on a layer (svgx.c draws the layer, this runs the <filter>'s primitives on it).
 *
 * The work is in premultiplied floats over the whole raster, in linearRGB unless color-interpolation-filters says sRGB (each primitive may say its own;
 * results are converted where a primitive reads one made in the other space). The filter region (filterUnits; -10% / 120% of the bounding box by
 * default) cuts the source and every result; a primitive's subregion (x, y, width, height in primitiveUnits) cuts its own result. Lengths are in user
 * units (or fractions of the bounding box with primitiveUnits="objectBoundingBox") and become device pixels through the element's transform.
 *
 * Primitives: feGaussianBlur, feOffset, feFlood, feComposite, feMerge, feBlend (all the CSS blend modes), feColorMatrix, feComponentTransfer,
 * feConvolveMatrix, feMorphology, feDisplacementMap, feTurbulence, feDiffuseLighting and feSpecularLighting (distant, point and spot lights), feImage
 * (an element or a data: picture), feTile, feDropShadow. Not done: kernelUnitLength (kernels are in device pixels), edgeMode on blurs.
 */
#include "svgx.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
    float* p;
    int w, h;
} fbuf; /* premultiplied RGBA floats, 0..1 */

static fbuf fb_new(int w, int h)
{
    fbuf b = { calloc((size_t)w * (size_t)h * 4, sizeof(float)), w, h };
    if(!b.p) {
        abort();
    }
    return b;
}
static void fb_free(fbuf* b)
{
    free(b->p);
    b->p = NULL;
}
static fbuf fb_copy(const fbuf* s)
{
    fbuf b = fb_new(s->w, s->h);
    memcpy(b.p, s->p, (size_t)s->w * (size_t)s->h * 4 * sizeof(float));
    return b;
}
#define NPX(b) ((size_t)(b)->w * (size_t)(b)->h)

static float to_linear(float c)
{
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}
static float to_srgb(float c)
{
    return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.f / 2.4f) - 0.055f;
}
static float clamp01(float v)
{
    return v < 0 ? 0 : v > 1 ? 1
                             : v;
}

/* the same picture in the other color space */
static fbuf fb_space(const fbuf* s, bool from_lin, bool to_lin)
{
    fbuf b = fb_copy(s);
    if(from_lin == to_lin) {
        return b;
    }
    for(size_t i = 0; i < NPX(&b); i++) {
        float a = b.p[i * 4 + 3];
        if(a <= 1e-6f) {
            continue;
        }
        for(int c = 0; c < 3; c++) {
            float v = clamp01(b.p[i * 4 + c] / a);
            b.p[i * 4 + c] = (to_lin ? to_linear(v) : to_srgb(v)) * a;
        }
    }
    return b;
}

/* ---------------------------------------------------------------- primitives' pieces */

static void fb_blur(fbuf* b, float sx, float sy)
{
    for(int axis = 0; axis < 2; axis++) {
        float sd = axis == 0 ? sx : sy;
        if(!(sd > 0.01f)) {
            continue;
        }
        int r = (int)ceilf(sd * 3.f);
        if(r > 2000) {
            r = 2000;
        }
        float *k = malloc((size_t)(2 * r + 1) * sizeof *k), sum = 0;
        if(!k) {
            abort();
        }
        for(int i = -r; i <= r; i++) {
            k[i + r] = expf(-(float)(i * i) / (2.f * sd * sd));
            sum += k[i + r];
        }
        for(int i = 0; i < 2 * r + 1; i++) {
            k[i] /= sum;
        }
        fbuf out = fb_new(b->w, b->h);
        for(int y = 0; y < b->h; y++) {
            for(int x = 0; x < b->w; x++) {
                float acc[4] = { 0, 0, 0, 0 };
                int lo = axis == 0 ? (x - r < 0 ? -x : -r) : (y - r < 0 ? -y : -r);
                int hi = axis == 0 ? (x + r >= b->w ? b->w - 1 - x : r) : (y + r >= b->h ? b->h - 1 - y : r);
                for(int i = lo; i <= hi; i++) {
                    int xx = axis == 0 ? x + i : x, yy = axis == 0 ? y : y + i;
                    const float* s = b->p + ((size_t)yy * (size_t)b->w + (size_t)xx) * 4;
                    float kk = k[i + r];
                    acc[0] += s[0] * kk;
                    acc[1] += s[1] * kk;
                    acc[2] += s[2] * kk;
                    acc[3] += s[3] * kk;
                }
                memcpy(out.p + ((size_t)y * (size_t)b->w + (size_t)x) * 4, acc, sizeof acc);
            }
        }
        free(k);
        fb_free(b);
        *b = out;
    }
}

static void fb_offset(fbuf* b, float dx, float dy)
{
    fbuf out = fb_new(b->w, b->h);
    float fx = floorf(dx), fy = floorf(dy), ax = dx - fx, ay = dy - fy;
    for(int y = 0; y < b->h; y++) {
        for(int x = 0; x < b->w; x++) {
            float acc[4] = { 0, 0, 0, 0 };
            for(int j = 0; j < 2; j++) {
                for(int i = 0; i < 2; i++) {
                    float wgt = (i ? ax : 1.f - ax) * (j ? ay : 1.f - ay);
                    if(wgt <= 0.f) {
                        continue;
                    }
                    int sx = x - (int)fx - i, sy = y - (int)fy - j;
                    if(sx < 0 || sy < 0 || sx >= b->w || sy >= b->h) {
                        continue;
                    }
                    const float* s = b->p + ((size_t)sy * (size_t)b->w + (size_t)sx) * 4;
                    for(int c = 0; c < 4; c++) {
                        acc[c] += s[c] * wgt;
                    }
                }
            }
            memcpy(out.p + ((size_t)y * (size_t)b->w + (size_t)x) * 4, acc, sizeof acc);
        }
    }
    fb_free(b);
    *b = out;
}

static void px_over(float* dst, const float* a, const float* b)
{
    float ia = 1.f - a[3];
    for(int c = 0; c < 4; c++) {
        dst[c] = a[c] + b[c] * ia;
    }
}

/* ---------------------------------------------------------------- blend modes (W3C compositing) */

static float blend_sep(int mode, float cb, float cs)
{
    switch(mode) {
        case 1:
            return cb * cs;           /* multiply */
        case 2:
            return cb + cs - cb * cs; /* screen */
        /* (3, overlay, is hard-light with the two swapped: see blend_px) */
        case 4:
            return fminf(cb, cs);                                                  /* darken */
        case 5:
            return fmaxf(cb, cs);                                                  /* lighten */
        case 6:
            return cb <= 0 ? 0 : cs >= 1 ? 1
                                         : fminf(1, cb / (1 - cs));                /* color-dodge */
        case 7:
            return cb >= 1 ? 1 : cs <= 0 ? 0
                                         : 1 - fminf(1, (1 - cb) / cs);            /* color-burn */
        case 8:
            return cs <= 0.5f ? cb * 2 * cs : 1 - 2 * (1 - cs) * (1 - cb) + 0 * 0; /* hard-light */
        case 9: {                                                                  /* soft-light */
            if(cs <= 0.5f) {
                return cb - (1 - 2 * cs) * cb * (1 - cb);
            }
            float dd = cb <= 0.25f ? ((16 * cb - 12) * cb + 4) * cb : sqrtf(cb);
            return cb + (2 * cs - 1) * (dd - cb);
        }
        case 10:
            return fabsf(cb - cs);        /* difference */
        case 11:
            return cb + cs - 2 * cb * cs; /* exclusion */
        default:
            return cs;                    /* normal */
    }
}
static float lum3(const float c[3])
{
    return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2];
}
static void clip_color(float c[3])
{
    float l = lum3(c), n = fminf(c[0], fminf(c[1], c[2])), x = fmaxf(c[0], fmaxf(c[1], c[2]));
    for(int i = 0; i < 3; i++) {
        if(n < 0) {
            c[i] = l + (c[i] - l) * l / (l - n);
        }
        if(x > 1) {
            c[i] = l + (c[i] - l) * (1 - l) / (x - l);
        }
    }
}
static void set_lum(float c[3], float l)
{
    float d = l - lum3(c);
    for(int i = 0; i < 3; i++) {
        c[i] += d;
    }
    clip_color(c);
}
static float sat3(const float c[3])
{
    return fmaxf(c[0], fmaxf(c[1], c[2])) - fminf(c[0], fminf(c[1], c[2]));
}
static void set_sat(float c[3], float s)
{
    int mx = 0, mn = 0;
    for(int i = 1; i < 3; i++) {
        if(c[i] > c[mx]) {
            mx = i;
        }
        if(c[i] < c[mn]) {
            mn = i;
        }
    }
    if(mx == mn) {
        c[0] = c[1] = c[2] = 0;
        return;
    }
    int md = 3 - mx - mn;
    float r = c[mx] - c[mn];
    c[md] = (c[md] - c[mn]) * s / r;
    c[mx] = s;
    c[mn] = 0;
}
static int blend_mode(const char* m)
{
    static const char* const names[] = { "normal", "multiply", "screen", "overlay", "darken", "lighten", "color-dodge", "color-burn", "hard-light", "soft-light", "difference", "exclusion", "hue", "saturation", "color", "luminosity" };
    if(!m) {
        return 0;
    }
    for(int i = 0; i < 16; i++) {
        if(strcmp(m, names[i]) == 0) {
            return i;
        }
    }
    return 0;
}
/* source x over backdrop y, premultiplied in and out */
static void blend_px(int mode, const float* x, const float* y, float* o)
{
    float as = x[3], ab = y[3];
    o[3] = as + ab - as * ab;
    float cs[3], cb[3], bl[3];
    for(int c = 0; c < 3; c++) {
        cs[c] = as > 1e-6f ? x[c] / as : 0;
        cb[c] = ab > 1e-6f ? y[c] / ab : 0;
    }
    if(mode < 12) {
        for(int c = 0; c < 3; c++) {
            bl[c] = mode == 3 ? blend_sep(8, cs[c], cb[c]) : blend_sep(mode, cb[c], cs[c]);
        }
    } else {
        float t[3];
        if(mode == 12) {
            memcpy(t, cs, sizeof t);
            set_sat(t, sat3(cb));
            set_lum(t, lum3(cb));
        } else if(mode == 13) {
            memcpy(t, cb, sizeof t);
            set_sat(t, sat3(cs));
            set_lum(t, lum3(cb));
        } else if(mode == 14) {
            memcpy(t, cs, sizeof t);
            set_lum(t, lum3(cb));
        } else {
            memcpy(t, cb, sizeof t);
            set_lum(t, lum3(cs));
        }
        memcpy(bl, t, sizeof bl);
    }
    for(int c = 0; c < 3; c++) {
        o[c] = (1 - ab) * x[c] + (1 - as) * y[c] + as * ab * clamp01(bl[c]);
    }
}

/* ---------------------------------------------------------------- turbulence (the reference code of the SVG specification) */

#define T_BSIZE    0x100
#define T_BM       0xff
#define T_PERLIN_N 0x1000
typedef struct
{
    int lattice[T_BSIZE + T_BSIZE + 2];
    double grad[4][T_BSIZE + T_BSIZE + 2][2];
} turb;
typedef struct
{
    int width, height, wrapx, wrapy;
} stitch;

static long t_random(long seed)
{
    long r = 16807 * (seed % 127773) - 2836 * (seed / 127773);
    if(r <= 0) {
        r += 2147483647;
    }
    return r;
}
static void turb_init(turb* t, long seed)
{
    if(seed <= 0) {
        seed = -(seed % (2147483647 - 1)) + 1;
    }
    if(seed > 2147483647 - 1) {
        seed = 2147483647 - 1;
    }
    int i, j, k;
    for(k = 0; k < 4; k++) {
        for(i = 0; i < T_BSIZE; i++) {
            t->lattice[i] = i;
            for(j = 0; j < 2; j++) {
                t->grad[k][i][j] = (double)(((seed = t_random(seed)) % (T_BSIZE + T_BSIZE)) - T_BSIZE) / T_BSIZE;
            }
            double s = sqrt(t->grad[k][i][0] * t->grad[k][i][0] + t->grad[k][i][1] * t->grad[k][i][1]);
            if(s > 0) {
                t->grad[k][i][0] /= s;
                t->grad[k][i][1] /= s;
            }
        }
    }
    while(--i) {
        k = t->lattice[i];
        t->lattice[i] = t->lattice[j = (int)((seed = t_random(seed)) % T_BSIZE)];
        t->lattice[j] = k;
    }
    for(i = 0; i < T_BSIZE + 2; i++) {
        t->lattice[T_BSIZE + i] = t->lattice[i];
        for(k = 0; k < 4; k++) {
            for(j = 0; j < 2; j++) {
                t->grad[k][T_BSIZE + i][j] = t->grad[k][i][j];
            }
        }
    }
}
#define S_CURVE(t)    ((t) * (t) * (3. - 2. * (t)))
#define LERP(t, a, b) ((a) + (t) * ((b) - (a)))
static double noise2(const turb* t, int ch, double vx, double vy, const stitch* st)
{
    double tt = vx + T_PERLIN_N;
    int bx0 = (int)tt, bx1 = bx0 + 1;
    double rx0 = tt - (int)tt, rx1 = rx0 - 1.0;
    tt = vy + T_PERLIN_N;
    int by0 = (int)tt, by1 = by0 + 1;
    double ry0 = tt - (int)tt, ry1 = ry0 - 1.0;
    if(st) {
        if(bx0 >= st->wrapx) {
            bx0 -= st->width;
        }
        if(bx1 >= st->wrapx) {
            bx1 -= st->width;
        }
        if(by0 >= st->wrapy) {
            by0 -= st->height;
        }
        if(by1 >= st->wrapy) {
            by1 -= st->height;
        }
    }
    bx0 &= T_BM;
    bx1 &= T_BM;
    by0 &= T_BM;
    by1 &= T_BM;
    int i = t->lattice[bx0], j = t->lattice[bx1];
    int b00 = t->lattice[i + by0], b10 = t->lattice[j + by0], b01 = t->lattice[i + by1], b11 = t->lattice[j + by1];
    double sx = S_CURVE(rx0), sy = S_CURVE(ry0);
    const double* q;
    q = t->grad[ch][b00];
    double u = rx0 * q[0] + ry0 * q[1];
    q = t->grad[ch][b10];
    double v = rx1 * q[0] + ry0 * q[1];
    double a = LERP(sx, u, v);
    q = t->grad[ch][b01];
    u = rx0 * q[0] + ry1 * q[1];
    q = t->grad[ch][b11];
    v = rx1 * q[0] + ry1 * q[1];
    double b = LERP(sx, u, v);
    return LERP(sy, a, b);
}
static double turbulence(const turb* t, int ch, double px, double py, double fx, double fy, int octaves, bool fractal, bool do_stitch, double tx, double ty, double tw,
    double th)
{
    stitch st, *pst = NULL;
    if(do_stitch) {
        if(fx != 0.0) {
            double lo = floor(tw * fx) / tw, hi = ceil(tw * fx) / tw;
            fx = (lo > 0 && fx / lo < hi / fx) ? lo : hi;
        }
        if(fy != 0.0) {
            double lo = floor(th * fy) / th, hi = ceil(th * fy) / th;
            fy = (lo > 0 && fy / lo < hi / fy) ? lo : hi;
        }
        pst = &st;
        st.width = (int)(tw * fx + 0.5);
        st.wrapx = (int)(tx * fx + T_PERLIN_N + st.width);
        st.height = (int)(th * fy + 0.5);
        st.wrapy = (int)(ty * fy + T_PERLIN_N + st.height);
    }
    double sum = 0, vx = px * fx, vy = py * fy, ratio = 1;
    for(int o = 0; o < octaves; o++) {
        double n = noise2(t, ch, vx, vy, pst);
        sum += (fractal ? n : fabs(n)) / ratio;
        vx *= 2;
        vy *= 2;
        ratio *= 2;
        if(pst) {
            st.width *= 2;
            st.wrapx = 2 * st.wrapx - T_PERLIN_N;
            st.height *= 2;
            st.wrapy = 2 * st.wrapy - T_PERLIN_N;
        }
    }
    return sum;
}

/* ---------------------------------------------------------------- lighting */

/* the surface normal from the alpha channel: the specification's Sobel kernels, with the smaller ones at the edges of the picture, which is the
 * primitive's subregion [x0, x1) x [y0, y1) */
static void normal_at(const fbuf* a, int x, int y, float ss, int x0, int y0, int x1, int y1, float* nx, float* ny)
{
    int w = a->w;
#define A(xx, yy) (a->p[((size_t)(yy) * (size_t)w + (size_t)(xx)) * 4 + 3])
    if(x1 - x0 < 2 || y1 - y0 < 2) {
        *nx = *ny = 0;
        return;
    }
    int l = x > x0, r = x<x1 - 1, t = y> y0, b = y < y1 - 1;
    int xl = l ? x - 1 : x, xr = r ? x + 1 : x, yt = t ? y - 1 : y, yb = b ? y + 1 : y;
    /* the factors: inside 1/4; along an edge 1/3 across it and 1/2 along it; at a corner 2/3 */
    float fx = (l && r) ? ((t && b) ? 1.f / 4 : 1.f / 3) : ((t && b) ? 1.f / 2 : 2.f / 3);
    float fy = (t && b) ? ((l && r) ? 1.f / 4 : 1.f / 3) : ((l && r) ? 1.f / 2 : 2.f / 3);
    float gx = 0, gy = 0;
    for(int dy = -1; dy <= 1; dy++) {
        int yy = y + dy;
        if(yy < y0 || yy >= y1) {
            continue;
        }
        gx += (dy == 0 ? 2.f : 1.f) * (A(xr, yy) - A(xl, yy));
    }
    for(int dx = -1; dx <= 1; dx++) {
        int xx = x + dx;
        if(xx < x0 || xx >= x1) {
            continue;
        }
        gy += (dx == 0 ? 2.f : 1.f) * (A(xx, yb) - A(xx, yt));
    }
#undef A
    *nx = -ss * fx * gx;
    *ny = -ss * fy * gy;
}

/* ---------------------------------------------------------------- the filter */

typedef struct
{
    char name[64];
    fbuf buf;
    bool lin;
    int r[4];
} result; /* r: its subregion, device pixels [x0, y0, x1, y1) */

typedef struct
{
    svgx_rc* rc;
    xn* f;
    bool obb_prim;
    float bb[4];
    bool has_bb;
    svgx_m2 dev, inv;
    double sx, sy;   /* device pixels per user unit along x and y */
    float region[4]; /* the filter region, user units */
    int rreg[4];     /* and in device pixels */
    float* rcov;     /* its coverage (NULL: everything) */
} fctx;

/* a primitive's number in user units: a fraction of the box with primitiveUnits="objectBoundingBox" */
static double prim_len(const fctx* c, const char* v, double def, int axis)
{
    if(!v || !*v) {
        return def;
    }
    char* end;
    double f = strtod(v, &end);
    if(end == v) {
        return def;
    }
    if(c->obb_prim) {
        if(*end == '%') {
            f /= 100;
        }
        double dim = axis == 0 ? c->bb[2] : axis == 1 ? c->bb[3]
                                                      : sqrt((c->bb[2] * c->bb[2] + c->bb[3] * c->bb[3]) / 2);
        return f * dim;
    }
    return svgx_length(v, (float)def, svgx_viewport_diag(c->rc, axis));
}
/* two numbers ("5" or "5 3"): the second is the first if missing */
static int two(const char* v, double* a, double* b)
{
    *a = *b = 0;
    if(!v) {
        return 0;
    }
    int n = sscanf(v, "%lf%*[ ,]%lf", a, b);
    if(n == 1) {
        *b = *a;
    }
    return n;
}

static void rect_dev(const fctx* c, const float r[4], int out[4])
{
    double xs[4], ys[4];
    double px[4] = { r[0], r[0] + r[2], r[0] + r[2], r[0] }, py[4] = { r[1], r[1], r[1] + r[3], r[1] + r[3] };
    for(int i = 0; i < 4; i++) {
        xs[i] = c->dev.a * px[i] + c->dev.c * py[i] + c->dev.e;
        ys[i] = c->dev.b * px[i] + c->dev.d * py[i] + c->dev.f;
    }
    double x0 = fmin(fmin(xs[0], xs[1]), fmin(xs[2], xs[3])), x1 = fmax(fmax(xs[0], xs[1]), fmax(xs[2], xs[3]));
    double y0 = fmin(fmin(ys[0], ys[1]), fmin(ys[2], ys[3])), y1 = fmax(fmax(ys[0], ys[1]), fmax(ys[2], ys[3]));
    out[0] = (int)fmax(floor(x0 + 1e-3), 0);
    out[1] = (int)fmax(floor(y0 + 1e-3), 0);
    out[2] = (int)fmin(ceil(x1 - 1e-3), c->rc->w);
    out[3] = (int)fmin(ceil(y1 - 1e-3), c->rc->h);
}

static const result* find_result(result* rs, int n, const char* name)
{
    for(int i = n - 1; i >= 0; i--) {
        if(strcmp(rs[i].name, name) == 0) {
            return &rs[i];
        }
    }
    return NULL;
}

/* the color space a primitive works in: its own property, else the filter's */
static bool prim_linear(const xn* p, bool def)
{
    char b[32];
    const char* v = svgx_prop(p, "color-interpolation-filters", b, sizeof b);
    if(!v || strcmp(v, "auto") == 0 || strcmp(v, "inherit") == 0) {
        return def;
    }
    return strcmp(v, "sRGB") != 0;
}

static bool color_prop(const xn* p, const char* prop, float rgb[3], float* alpha, const char* def)
{
    char b[128];
    const char* v = svgx_prop(p, prop, b, sizeof b);
    return svgx_color(v ? v : def, rgb, alpha);
}

uint8_t* svgx_filter(svgx_rc* rc, xn* f, const uint8_t* premul)
{
    int w = rc->w, h = rc->h;
    size_t n = (size_t)w * (size_t)h;
    fctx c = {
        rc, f, false, { 0, 0, 0, 0 },
           false, { 1, 0, 0, 1, 0, 0 },
           { 1, 0, 0, 1, 0, 0 },
           1, 1, { 0, 0, 0, 0 },
           { 0, 0, w, h },
           NULL
    };
    const char *pu = svgx_attr(f, "primitiveUnits"), *fu = svgx_attr(f, "filterUnits");
    c.obb_prim = pu && strcmp(pu, "objectBoundingBox") == 0;
    bool obb_region = !(fu && strcmp(fu, "userSpaceOnUse") == 0);
    c.has_bb = svgx_bbox(rc, c.bb);
    c.dev = svgx_device(rc);
    c.inv = svgx_inverse(c.dev);
    c.sx = hypot(c.dev.a, c.dev.b);
    c.sy = hypot(c.dev.c, c.dev.d);
    bool region = svgx_attr(f, "\x01noregion") == NULL;
    if(region) {
        if((obb_region || c.obb_prim) && (!c.has_bb || c.bb[2] <= 0 || c.bb[3] <= 0)) {
            return calloc(n, 4); /* (no box: nothing is drawn) */
        }
        static const char* const names[4] = { "x", "y", "width", "height" };
        static const float def[4] = { -0.1f, -0.1f, 1.2f, 1.2f };
        for(int i = 0; i < 4; i++) {
            const char* v = svgx_attr(f, names[i]);
            if(obb_region) {
                float fr = def[i];
                if(v) {
                    char* end;
                    fr = strtof(v, &end);
                    if(end == v) {
                        fr = def[i];
                    } else if(*end == '%') {
                        fr /= 100.f;
                    }
                }
                c.region[i] = i == 0 ? c.bb[0] + fr * c.bb[2] : i == 1 ? c.bb[1] + fr * c.bb[3]
                                                                       : fr * (i == 2 ? c.bb[2] : c.bb[3]);
            } else {
                float pct = svgx_viewport_diag(rc, i % 2);
                c.region[i] = v ? svgx_length(v, 0, pct) : def[i] * pct;
            }
        }
        if(!(c.region[2] > 0) || !(c.region[3] > 0)) {
            return calloc(n, 4);
        }
        c.rcov = svgx_rect_cover(rc, c.region[0], c.region[1], c.region[2], c.region[3]);
        rect_dev(&c, c.region, c.rreg);
    } else {
        if(c.has_bb) { /* (the whole picture; the box only for primitives in bounding-box units) */
            c.region[0] = -1e6f;
            c.region[1] = -1e6f;
            c.region[2] = 2e6f;
            c.region[3] = 2e6f;
        }
    }
    char lb[32];
    const char* cifv = svgx_prop(f, "color-interpolation-filters", lb, sizeof lb);
    if(!cifv) {
        const char* inh = svgx_attr(f, "\x01inh");
        const char* at = inh ? strstr(inh, "color-interpolation-filters:") : NULL;
        if(at) {
            snprintf(lb, sizeof lb, "%.*s", (int)strcspn(at + 28, ";"), at + 28);
            cifv = lb;
        }
    }
    bool flin = !(cifv && strcmp(cifv, "sRGB") == 0);

    /* the source, in both spaces when asked for */
    fbuf src = fb_new(w, h);
    for(size_t i = 0; i < n; i++) {
        float k = c.rcov ? c.rcov[i] : 1.f;
        for(int ch = 0; ch < 4; ch++) {
            src.p[i * 4 + ch] = (float)premul[i * 4 + ch] / 255.f * k;
        }
    }
    result rs[96];
    int nr = 0;
    rs[nr++] = (result){
        "SourceGraphic", src, false, { c.rreg[0], c.rreg[1], c.rreg[2], c.rreg[3] }
    };
    fbuf alpha = fb_new(w, h);
    for(size_t i = 0; i < n; i++) {
        alpha.p[i * 4 + 3] = src.p[i * 4 + 3];
    }
    rs[nr++] = (result){
        "SourceAlpha", alpha, false, { c.rreg[0], c.rreg[1], c.rreg[2], c.rreg[3] }
    };
    static const char* const empties[] = { "BackgroundImage", "BackgroundAlpha", "FillPaint", "StrokePaint" };
    for(int i = 0; i < 4; i++) {
        rs[nr++] = (result){
            "", fb_new(w, h), false, { c.rreg[0], c.rreg[1], c.rreg[2], c.rreg[3] }
        },
        snprintf(rs[nr - 1].name, 64, "%s", empties[i]);
    }
    char lastname[64] = "SourceGraphic";
    int made = 0;

    for(int k = 0; k < f->nk && nr < 90; k++) {
        xn* p = f->kid[k];
        const char* nm = p->name;
        if(strncmp(nm, "fe", 2) != 0) {
            continue;
        }
        bool lin = prim_linear(p, flin);
        const char *in = svgx_attr(p, "in"), *in2 = svgx_attr(p, "in2");
        const result* ra = find_result(rs, nr, in && *in ? in : lastname);
        if(!ra) {
            ra = &rs[0];
        }
        const result* rb = in2 && *in2 ? find_result(rs, nr, in2) : NULL;
        if(!rb) {
            rb = &rs[0];
        }
        /* inputs in this primitive's space */
        fbuf A = fb_space(&ra->buf, ra->lin, lin), B = fb_space(&rb->buf, rb->lin, lin);
        fbuf out;
        bool out_lin = lin;
        /* the subregion: given on the primitive, else the filter region */
        float sr[4] = { c.region[0], c.region[1], c.region[2], c.region[3] };
        bool own_sr = false;
        static const char* const sn[4] = { "x", "y", "width", "height" };
        for(int i = 0; i < 4; i++) {
            const char* v = svgx_attr(p, sn[i]);
            if(!v) {
                continue;
            }
            own_sr = true;
            if(c.obb_prim) {
                char* end;
                double fr = strtod(v, &end);
                if(*end == '%') {
                    fr /= 100;
                }
                sr[i] = (float)(i == 0 ? c.bb[0] + fr * c.bb[2] : i == 1 ? c.bb[1] + fr * c.bb[3]
                                                                         : fr * (i == 2 ? c.bb[2] : c.bb[3]));
            } else {
                sr[i] = svgx_length(v, 0, svgx_viewport_diag(rc, i % 2));
            }
        }
        int rr[4] = { c.rreg[0], c.rreg[1], c.rreg[2], c.rreg[3] };
        if(own_sr) {
            rect_dev(&c, sr, rr);
        }

        if(strcmp(nm, "feGaussianBlur") == 0) {
            double sdx, sdy;
            int nn = two(svgx_attr(p, "stdDeviation"), &sdx, &sdy);
            out = fb_copy(&A);
            if(nn && sdx >= 0 && sdy >= 0) {
                double ux = c.obb_prim ? sdx * c.bb[2] : sdx, uy = c.obb_prim ? sdy * c.bb[3] : sdy;
                fb_blur(&out, (float)(ux * c.sx), (float)(uy * c.sy));
            } else if(nn) {
                fb_free(&out);
                out = fb_new(w, h);
            }
        } else if(strcmp(nm, "feOffset") == 0) {
            double dx = prim_len(&c, svgx_attr(p, "dx"), 0, 0), dy = prim_len(&c, svgx_attr(p, "dy"), 0, 1);
            out = fb_copy(&A);
            fb_offset(&out, (float)(c.dev.a * dx + c.dev.c * dy), (float)(c.dev.b * dx + c.dev.d * dy));
        } else if(strcmp(nm, "feFlood") == 0) {
            float rgb[3], al = 1;
            color_prop(p, "flood-color", rgb, &al, "black");
            char ob[32];
            const char* fo = svgx_prop(p, "flood-opacity", ob, sizeof ob);
            float a = clamp01((fo ? strtof(fo, NULL) : 1.f) * al);
            out = fb_new(w, h);
            for(size_t i = 0; i < n; i++) {
                for(int ch = 0; ch < 3; ch++) {
                    out.p[i * 4 + ch] = (lin ? to_linear(rgb[ch]) : rgb[ch]) * a;
                }
                out.p[i * 4 + 3] = a;
            }
        } else if(strcmp(nm, "feComposite") == 0) {
            const char* op = svgx_attr(p, "operator");
            float k1 = svgx_num(p, "k1", 0), k2 = svgx_num(p, "k2", 0), k3 = svgx_num(p, "k3", 0), k4 = svgx_num(p, "k4", 0);
            out = fb_new(w, h);
            for(size_t i = 0; i < n; i++) {
                const float *x = A.p + i * 4, *y = B.p + i * 4;
                float *o = out.p + i * 4, ax = x[3], ay = y[3];
                if(!op || strcmp(op, "over") == 0) {
                    px_over(o, x, y);
                } else if(strcmp(op, "in") == 0) {
                    for(int q = 0; q < 4; q++) {
                        o[q] = x[q] * ay;
                    }
                } else if(strcmp(op, "out") == 0) {
                    for(int q = 0; q < 4; q++) {
                        o[q] = x[q] * (1.f - ay);
                    }
                } else if(strcmp(op, "atop") == 0) {
                    for(int q = 0; q < 4; q++) {
                        o[q] = x[q] * ay + y[q] * (1.f - ax);
                    }
                } else if(strcmp(op, "xor") == 0) {
                    for(int q = 0; q < 4; q++) {
                        o[q] = x[q] * (1.f - ay) + y[q] * (1.f - ax);
                    }
                } else if(strcmp(op, "lighter") == 0) {
                    for(int q = 0; q < 4; q++) {
                        o[q] = fminf(x[q] + y[q], 1.f);
                    }
                } else if(strcmp(op, "arithmetic") == 0) {
                    for(int q = 0; q < 4; q++) {
                        o[q] = clamp01(k1 * x[q] * y[q] + k2 * x[q] + k3 * y[q] + k4);
                    }
                    for(int q = 0; q < 3; q++) {
                        o[q] = fminf(o[q], o[3]);
                    }
                } else {
                    px_over(o, x, y);
                }
            }
        } else if(strcmp(nm, "feMerge") == 0) {
            out = fb_new(w, h);
            for(int m = 0; m < p->nk; m++) {
                if(strcmp(p->kid[m]->name, "feMergeNode") != 0) {
                    continue;
                }
                const char* mi = svgx_attr(p->kid[m], "in");
                const result* lr = find_result(rs, nr, mi && *mi ? mi : lastname);
                if(!lr) {
                    lr = &rs[0];
                }
                fbuf L = fb_space(&lr->buf, lr->lin, lin);
                for(size_t i = 0; i < n; i++) {
                    float t[4];
                    px_over(t, L.p + i * 4, out.p + i * 4);
                    memcpy(out.p + i * 4, t, sizeof t);
                }
                fb_free(&L);
            }
        } else if(strcmp(nm, "feBlend") == 0) {
            int mode = blend_mode(svgx_attr(p, "mode"));
            out = fb_new(w, h);
            for(size_t i = 0; i < n; i++) {
                blend_px(mode, A.p + i * 4, B.p + i * 4, out.p + i * 4);
            }
        } else if(strcmp(nm, "feColorMatrix") == 0) {
            const char *type = svgx_attr(p, "type"), *vals = svgx_attr(p, "values");
            float m[20] = { 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0 };
            if(type && strcmp(type, "saturate") == 0) {
                float s = vals && *vals ? strtof(vals, NULL) : 1.f;
                float sm[20] = { 0.213f + 0.787f * s, 0.715f - 0.715f * s, 0.072f - 0.072f * s, 0, 0, 0.213f - 0.213f * s, 0.715f + 0.285f * s, 0.072f - 0.072f * s, 0, 0, 0.213f - 0.213f * s, 0.715f - 0.715f * s, 0.072f + 0.928f * s, 0, 0, 0, 0, 0, 1, 0 };
                memcpy(m, sm, sizeof m);
            } else if(type && strcmp(type, "hueRotate") == 0) {
                float t = (vals && *vals ? strtof(vals, NULL) : 0.f) * (float)M_PI / 180.f, cs = cosf(t), sn2 = sinf(t);
                float hm[20] = { 0.213f + cs * 0.787f - sn2 * 0.213f, 0.715f - cs * 0.715f - sn2 * 0.715f, 0.072f - cs * 0.072f + sn2 * 0.928f, 0, 0, 0.213f - cs * 0.213f + sn2 * 0.143f, 0.715f + cs * 0.285f + sn2 * 0.140f, 0.072f - cs * 0.072f - sn2 * 0.283f, 0, 0, 0.213f - cs * 0.213f - sn2 * 0.787f, 0.715f - cs * 0.715f + sn2 * 0.715f, 0.072f + cs * 0.928f + sn2 * 0.072f, 0, 0, 0, 0, 0, 1, 0 };
                memcpy(m, hm, sizeof m);
            } else if(type && strcmp(type, "luminanceToAlpha") == 0) {
                float lm[20] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.2125f, 0.7154f, 0.0721f, 0, 0 };
                memcpy(m, lm, sizeof m);
            } else if(vals) {
                const char* q = vals;
                float t[20];
                int cnt = 0;
                while(cnt < 20) {
                    char* end;
                    float v = strtof(q, &end);
                    if(end == q) {
                        break;
                    }
                    t[cnt++] = v;
                    q = end;
                    while(*q == ' ' || *q == ',' || *q == '\n' || *q == '\t') {
                        q++;
                    }
                }
                if(cnt == 20) {
                    memcpy(m, t, sizeof m);
                }
            }
            out = fb_new(w, h);
            for(size_t i = 0; i < n; i++) {
                const float* x = A.p + i * 4;
                float al = x[3], cc[4] = { al > 1e-6f ? x[0] / al : 0, al > 1e-6f ? x[1] / al : 0, al > 1e-6f ? x[2] / al : 0, al };
                float r[4];
                for(int row = 0; row < 4; row++) {
                    r[row] = clamp01(m[row * 5] * cc[0] + m[row * 5 + 1] * cc[1] + m[row * 5 + 2] * cc[2] + m[row * 5 + 3] * cc[3] + m[row * 5 + 4]);
                }
                float* o = out.p + i * 4;
                for(int ch = 0; ch < 3; ch++) {
                    o[ch] = r[ch] * r[3];
                }
                o[3] = r[3];
            }
        } else if(strcmp(nm, "feComponentTransfer") == 0) {
            /* a function per channel: identity, table, discrete, linear, gamma */
            struct
            {
                int type;
                float tv[256];
                int nt;
                float slope, intercept, amp, expo, off;
            } fn[4];
            for(int ch = 0; ch < 4; ch++) {
                memset(&fn[ch], 0, sizeof fn[ch]);
                fn[ch].slope = 1;
                fn[ch].amp = 1;
                fn[ch].expo = 1;
            }
            for(int m = 0; m < p->nk; m++) {
                const char* fnm = p->kid[m]->name;
                int ch = strcmp(fnm, "feFuncR") == 0 ? 0 : strcmp(fnm, "feFuncG") == 0 ? 1
                                                       : strcmp(fnm, "feFuncB") == 0   ? 2
                                                       : strcmp(fnm, "feFuncA") == 0   ? 3
                                                                                       : -1;
                if(ch < 0) {
                    continue;
                }
                const xn* e = p->kid[m];
                const char* t = svgx_attr(e, "type");
                fn[ch].type = !t ? 0 : strcmp(t, "table") == 0  ? 1
                                   : strcmp(t, "discrete") == 0 ? 2
                                   : strcmp(t, "linear") == 0   ? 3
                                   : strcmp(t, "gamma") == 0    ? 4
                                                                : 0;
                const char* tv = svgx_attr(e, "tableValues");
                while(tv && *tv && fn[ch].nt < 256) {
                    char* end;
                    float v = strtof(tv, &end);
                    if(end == tv) {
                        break;
                    }
                    fn[ch].tv[fn[ch].nt++] = v;
                    tv = end;
                    while(*tv == ' ' || *tv == ',' || *tv == '\n' || *tv == '\t') {
                        tv++;
                    }
                }
                fn[ch].slope = svgx_num(e, "slope", 1);
                fn[ch].intercept = svgx_num(e, "intercept", 0);
                fn[ch].amp = svgx_num(e, "amplitude", 1);
                fn[ch].expo = svgx_num(e, "exponent", 1);
                fn[ch].off = svgx_num(e, "offset", 0);
                if((fn[ch].type == 1 || fn[ch].type == 2) && fn[ch].nt == 0) {
                    fn[ch].type = 0; /* (an empty table: identity) */
                }
            }
            out = fb_new(w, h);
            for(size_t i = 0; i < n; i++) {
                const float* x = A.p + i * 4;
                float al = x[3], cc[4] = { al > 1e-6f ? x[0] / al : 0, al > 1e-6f ? x[1] / al : 0, al > 1e-6f ? x[2] / al : 0, al };
                for(int ch = 0; ch < 4; ch++) {
                    float v = clamp01(cc[ch]);
                    switch(fn[ch].type) {
                        case 1: {
                            int nn2 = fn[ch].nt - 1;
                            if(nn2 <= 0) {
                                v = fn[ch].tv[0];
                                break;
                            }
                            int kk = (int)floorf(v * nn2);
                            if(kk >= nn2) {
                                kk = nn2 - 1;
                            }
                            v = fn[ch].tv[kk] + (v - (float)kk / nn2) * nn2 * (fn[ch].tv[kk + 1] - fn[ch].tv[kk]);
                            break;
                        }
                        case 2: {
                            int nn2 = fn[ch].nt, kk = (int)floorf(v * nn2);
                            if(kk >= nn2) {
                                kk = nn2 - 1;
                            }
                            v = fn[ch].tv[kk];
                            break;
                        }
                        case 3:
                            v = fn[ch].slope * v + fn[ch].intercept;
                            break;
                        case 4:
                            v = fn[ch].amp * powf(v, fn[ch].expo) + fn[ch].off;
                            break;
                        default:
                            break;
                    }
                    cc[ch] = clamp01(v);
                }
                float* o = out.p + i * 4;
                for(int ch = 0; ch < 3; ch++) {
                    o[ch] = cc[ch] * cc[3];
                }
                o[3] = cc[3];
            }
        } else if(strcmp(nm, "feConvolveMatrix") == 0) {
            double ox, oy;
            int on = two(svgx_attr(p, "order"), &ox, &oy);
            int nx = on ? (int)ox : 3, ny = on ? (int)oy : 3;
            float km[1024];
            int nk = 0;
            const char* kv = svgx_attr(p, "kernelMatrix");
            while(kv && *kv && nk < 1024) {
                char* end;
                float v = strtof(kv, &end);
                if(end == kv) {
                    break;
                }
                km[nk++] = v;
                kv = end;
                while(*kv == ' ' || *kv == ',' || *kv == '\n' || *kv == '\t') {
                    kv++;
                }
            }
            int tx = svgx_attr(p, "targetX") ? (int)svgx_num(p, "targetX", 0) : nx / 2, ty = svgx_attr(p, "targetY") ? (int)svgx_num(p, "targetY", 0) : ny / 2;
            out = fb_new(w, h);
            if(nx >= 1 && ny >= 1 && nx * ny == nk && tx >= 0 && tx < nx && ty >= 0 && ty < ny) {
                float sum = 0;
                for(int i = 0; i < nk; i++) {
                    sum += km[i];
                }
                float div = svgx_attr(p, "divisor") ? svgx_num(p, "divisor", 1) : (sum != 0 ? sum : 1);
                if(div == 0) {
                    div = 1;
                }
                float bias = svgx_num(p, "bias", 0);
                const char* em = svgx_attr(p, "edgeMode");
                int edge = !em || strcmp(em, "duplicate") == 0 ? 0 : strcmp(em, "wrap") == 0 ? 1
                                                                                             : 2;
                const char* pa = svgx_attr(p, "preserveAlpha");
                bool keep_a = pa && strcmp(pa, "true") == 0;
                int X0 = rr[0], Y0 = rr[1], X1 = rr[2], Y1 = rr[3], RW = X1 - X0, RH = Y1 - Y0; /* (the edges are the subregion's) */
                for(int y = Y0; y < Y1; y++) {
                    for(int x = X0; x < X1; x++) {
                        float acc[4] = { 0, 0, 0, 0 };
                        for(int j = 0; j < ny; j++) {
                            for(int i = 0; i < nx; i++) {
                                int sx = x - tx + i, sy = y - ty + j;
                                if(sx < X0 || sy < Y0 || sx >= X1 || sy >= Y1) {
                                    if(edge == 2) {
                                        continue;
                                    }
                                    if(edge == 0) {
                                        sx = sx < X0 ? X0 : sx >= X1 ? X1 - 1
                                                                     : sx;
                                        sy = sy < Y0 ? Y0 : sy >= Y1 ? Y1 - 1
                                                                     : sy;
                                    } else {
                                        sx = X0 + ((sx - X0) % RW + RW) % RW;
                                        sy = Y0 + ((sy - Y0) % RH + RH) % RH;
                                    }
                                }
                                float kk = km[(ny - 1 - j) * nx + (nx - 1 - i)];
                                const float* s = A.p + ((size_t)sy * (size_t)w + (size_t)sx) * 4;
                                if(keep_a) {
                                    float sa = s[3];
                                    for(int q = 0; q < 3; q++) {
                                        acc[q] += (sa > 1e-6f ? s[q] / sa : 0) * kk;
                                    }
                                } else {
                                    for(int q = 0; q < 4; q++) {
                                        acc[q] += s[q] * kk;
                                    }
                                }
                            }
                        }
                        float* o = out.p + ((size_t)y * (size_t)w + (size_t)x) * 4;
                        if(keep_a) {
                            float a = A.p[((size_t)y * (size_t)w + (size_t)x) * 4 + 3];
                            for(int q = 0; q < 3; q++) {
                                o[q] = clamp01(acc[q] / div + bias) * a;
                            }
                            o[3] = a;
                        } else {
                            o[3] = clamp01(acc[3] / div + bias);
                            for(int q = 0; q < 3; q++) {
                                o[q] = fminf(clamp01(acc[q] / div + bias * o[3]), o[3]);
                            }
                        }
                    }
                }
            }
        } else if(strcmp(nm, "feMorphology") == 0) {
            double rx, ry;
            int rn = two(svgx_attr(p, "radius"), &rx, &ry);
            if(!rn) {
                rx = ry = 0;
            }
            if(c.obb_prim) {
                rx *= c.bb[2];
                ry *= c.bb[3];
            }
            const char* op = svgx_attr(p, "operator");
            bool dilate = op && strcmp(op, "dilate") == 0;
            int irx = (int)lround(rx * c.sx), iry = (int)lround(ry * c.sy);
            if(rx < 0 || ry < 0) {
                out = fb_new(w, h);
            } else if(rx == 0 && ry == 0) {
                out = fb_copy(&A);
            } else {
                out = fb_copy(&A);
                for(int axis = 0; axis < 2; axis++) {
                    int r = axis == 0 ? irx : iry;
                    if(r <= 0) {
                        continue;
                    }
                    fbuf t = fb_new(w, h);
                    for(int y = 0; y < h; y++) {
                        for(int x = 0; x < w; x++) {
                            float acc[4];
                            for(int q = 0; q < 4; q++) {
                                acc[q] = dilate ? 0.f : 1.f;
                            }
                            for(int i = -r; i <= r; i++) {
                                int sx = axis == 0 ? x + i : x, sy = axis == 0 ? y : y + i;
                                const float* s;
                                static const float zero[4] = { 0, 0, 0, 0 };
                                s = (sx < 0 || sy < 0 || sx >= w || sy >= h) ? zero : out.p + ((size_t)sy * (size_t)w + (size_t)sx) * 4;
                                for(int q = 0; q < 4; q++) {
                                    acc[q] = dilate ? fmaxf(acc[q], s[q]) : fminf(acc[q], s[q]);
                                }
                            }
                            memcpy(t.p + ((size_t)y * (size_t)w + (size_t)x) * 4, acc, sizeof acc);
                        }
                    }
                    fb_free(&out);
                    out = t;
                }
            }
        } else if(strcmp(nm, "feDisplacementMap") == 0) {
            double sc = prim_len(&c, svgx_attr(p, "scale"), 0, 2);
            const char *xc = svgx_attr(p, "xChannelSelector"), *yc = svgx_attr(p, "yChannelSelector");
            int cx = !xc ? 3 : *xc == 'R' ? 0
                           : *xc == 'G'   ? 1
                           : *xc == 'B'   ? 2
                                          : 3,
                cy = !yc ? 3 : *yc == 'R' ? 0
                           : *yc == 'G'   ? 1
                           : *yc == 'B'   ? 2
                                          : 3;
            /* the map in its own space, unpremultiplied */
            fbuf M = fb_space(&rb->buf, rb->lin, lin);
            out = fb_new(w, h);
            for(int y = 0; y < h; y++) {
                for(int x = 0; x < w; x++) {
                    const float* m = M.p + ((size_t)y * (size_t)w + (size_t)x) * 4;
                    float ma = m[3];
                    float vx = cx == 3 ? ma : (ma > 1e-6f ? m[cx] / ma : 0), vy = cy == 3 ? ma : (ma > 1e-6f ? m[cy] / ma : 0);
                    double ux = sc * (vx - 0.5), uy = sc * (vy - 0.5); /* user units -> device */
                    double dx = c.dev.a * ux + c.dev.c * uy, dy = c.dev.b * ux + c.dev.d * uy;
                    int sx = (int)floor(x + dx + 0.5), sy = (int)floor(y + dy + 0.5);
                    if(sx < 0 || sy < 0 || sx >= w || sy >= h) {
                        continue;
                    }
                    memcpy(out.p + ((size_t)y * (size_t)w + (size_t)x) * 4, A.p + ((size_t)sy * (size_t)w + (size_t)sx) * 4, 4 * sizeof(float));
                }
            }
            fb_free(&M);
        } else if(strcmp(nm, "feTurbulence") == 0) {
            double fx, fy;
            int bn = two(svgx_attr(p, "baseFrequency"), &fx, &fy);
            if(!bn) {
                fx = fy = 0;
            }
            int oct = (int)svgx_num(p, "numOctaves", 1);
            long seed = (long)lround(svgx_num(p, "seed", 0));
            const char* ty = svgx_attr(p, "type");
            bool fractal = ty && strcmp(ty, "fractalNoise") == 0;
            const char* stt = svgx_attr(p, "stitchTiles");
            bool st = stt && strcmp(stt, "stitch") == 0;
            out = fb_new(w, h);
            if(fx >= 0 && fy >= 0) {
                turb* t = malloc(sizeof *t);
                if(!t) {
                    abort();
                }
                turb_init(t, seed);
                for(int y = rr[1]; y < rr[3]; y++) {
                    for(int x = rr[0]; x < rr[2]; x++) {
                        double ux = c.inv.a * (x + 0.0) + c.inv.c * (y + 0.0) + c.inv.e, uy = c.inv.b * (x + 0.0) + c.inv.d * (y + 0.0) + c.inv.f; /* (the pixel's corner, as the specification's code takes it) */
                        float col[4];
                        for(int ch = 0; ch < 4; ch++) {
                            double v = turbulence(t, ch, ux, uy, fx, fy, oct, fractal, st, sr[0], sr[1], sr[2], sr[3]);
                            col[ch] = clamp01((float)(fractal ? (v + 1) / 2 : v));
                        }
                        float* o = out.p + ((size_t)y * (size_t)w + (size_t)x) * 4;
                        for(int ch = 0; ch < 3; ch++) {
                            o[ch] = col[ch] * col[3];
                        }
                        o[3] = col[3];
                    }
                }
                free(t);
            }
        } else if(strcmp(nm, "feDiffuseLighting") == 0 || strcmp(nm, "feSpecularLighting") == 0) {
            bool spec = nm[2] == 'S';
            float ss = svgx_num(p, "surfaceScale", 1), kd = svgx_num(p, "diffuseConstant", 1), ks = svgx_num(p, "specularConstant", 1),
                  se = svgx_num(p, "specularExponent", 1);
            float lc[3], la = 1;
            color_prop(p, "lighting-color", lc, &la, "white");
            if(lin) {
                for(int q = 0; q < 3; q++) {
                    lc[q] = to_linear(lc[q]);
                }
            }
            const xn* light = NULL;
            for(int m = 0; m < p->nk; m++) {
                if(strcmp(p->kid[m]->name, "feDistantLight") == 0 || strcmp(p->kid[m]->name, "fePointLight") == 0 || strcmp(p->kid[m]->name, "feSpotLight") == 0) {
                    light = p->kid[m];
                    break;
                }
            }
            out = fb_new(w, h);
            if(light && !(spec && (se < 1 || se > 128))) {
                int lt = light->name[2] == 'D' ? 0 : light->name[2] == 'P' ? 1
                                                                           : 2;
                double ds = sqrt(fabs(c.dev.a * c.dev.d - c.dev.b * c.dev.c));
                double az = svgx_num(light, "azimuth", 0) * M_PI / 180, el = svgx_num(light, "elevation", 0) * M_PI / 180;
                double lx = 0, ly = 0, lz = 0, px = 0, py = 0, pz = 0;
                if(lt) {
                    double ux = prim_len(&c, svgx_attr(light, "x"), 0, 0), uy = prim_len(&c, svgx_attr(light, "y"), 0, 1);
                    lx = c.dev.a * ux + c.dev.c * uy + c.dev.e;
                    ly = c.dev.b * ux + c.dev.d * uy + c.dev.f;
                    lz = prim_len(&c, svgx_attr(light, "z"), 0, 2) * ds;
                }
                if(lt == 2) {
                    double ux = prim_len(&c, svgx_attr(light, "pointsAtX"), 0, 0), uy = prim_len(&c, svgx_attr(light, "pointsAtY"), 0, 1);
                    px = c.dev.a * ux + c.dev.c * uy + c.dev.e;
                    py = c.dev.b * ux + c.dev.d * uy + c.dev.f;
                    pz = prim_len(&c, svgx_attr(light, "pointsAtZ"), 0, 2) * ds;
                }
                double spot_exp = svgx_num(light, "specularExponent", 1);
                const char* lca = svgx_attr(light, "limitingConeAngle");
                double cone = lca ? cos(fabs(strtod(lca, NULL)) * M_PI / 180) : -2;
                double Sx = px - lx, Sy = py - ly, Sz = pz - lz, Sl = sqrt(Sx * Sx + Sy * Sy + Sz * Sz);
                if(Sl > 0) {
                    Sx /= Sl;
                    Sy /= Sl;
                    Sz /= Sl;
                }
                for(int y = rr[1]; y < rr[3]; y++) {
                    for(int x = rr[0]; x < rr[2]; x++) {
                        float nx, ny;
                        normal_at(&A, x, y, ss, rr[0], rr[1], rr[2], rr[3], &nx, &ny);
                        double Nl = sqrt((double)nx * nx + (double)ny * ny + 1);
                        double Nx = nx / Nl, Ny = ny / Nl, Nz = 1 / Nl;
                        double Lx, Ly, Lz;
                        if(lt == 0) {
                            Lx = cos(az) * cos(el);
                            Ly = sin(az) * cos(el);
                            Lz = sin(el);
                        } else {
                            double z = ss * A.p[((size_t)y * (size_t)w + (size_t)x) * 4 + 3];
                            Lx = lx - (x + 0.5);
                            Ly = ly - (y + 0.5);
                            Lz = lz - z;
                            double L = sqrt(Lx * Lx + Ly * Ly + Lz * Lz);
                            if(L > 0) {
                                Lx /= L;
                                Ly /= L;
                                Lz /= L;
                            }
                        }
                        double col[3] = { lc[0], lc[1], lc[2] };
                        if(lt == 2) {
                            double minus = -(Lx * Sx + Ly * Sy + Lz * Sz);
                            double k = minus <= 0 || (lca && minus < cone) ? 0 : pow(minus, spot_exp);
                            for(int q = 0; q < 3; q++) {
                                col[q] *= k;
                            }
                        }
                        float* o = out.p + ((size_t)y * (size_t)w + (size_t)x) * 4;
                        if(!spec) {
                            double nl = Nx * Lx + Ny * Ly + Nz * Lz;
                            for(int q = 0; q < 3; q++) {
                                o[q] = clamp01((float)(kd * nl * col[q]));
                            }
                            o[3] = 1;
                        } else {
                            double Hx = Lx, Hy = Ly, Hz = Lz + 1, H = sqrt(Hx * Hx + Hy * Hy + Hz * Hz);
                            double nh = H > 0 ? (Nx * Hx + Ny * Hy + Nz * Hz) / H : 0;
                            double k = ks * pow(fmax(nh, 0), se);
                            for(int q = 0; q < 3; q++) {
                                o[q] = clamp01((float)(k * col[q]));
                            }
                            o[3] = fmaxf(o[0], fmaxf(o[1], o[2]));
                        }
                    }
                }
            }
        } else if(strcmp(nm, "feImage") == 0) {
            out = fb_new(w, h);
            const char* href = svgx_attr(p, "href");
            if(!href) {
                href = svgx_attr(p, "xlink:href");
            }
            if(href && href[0] == '#') {
                xn* el = svgx_find(rc->d, href + 1);
                if(el) {
                    uint8_t* px = svgx_render_ref(rc, el, own_sr && svgx_attr(p, "x") ? sr[0] : 0, own_sr && svgx_attr(p, "y") ? sr[1] : 0);
                    if(px) {
                        for(size_t i = 0; i < n; i++) {
                            for(int q = 0; q < 4; q++) {
                                out.p[i * 4 + q] = (float)px[i * 4 + q] / 255.f;
                            }
                        }
                        free(px);
                    }
                }
            } else if(href) {
                double ds = sqrt(fabs(c.dev.a * c.dev.d - c.dev.b * c.dev.c));
                svgx_pic pic;
                if(svgx_load_href(rc, href, 0, &pic)) {
                    double fsx, fsy, ftx, fty;
                    bool sl;
                    svgx_fit(svgx_attr(p, "preserveAspectRatio"), pic.iw, pic.ih, sr[0], sr[1], sr[2], sr[3], &fsx, &fsy, &ftx, &fty, &sl);
                    if(!pic.px) {
                        svgx_load_href(rc, href, (float)(ds * fmax(fsx, fsy)), &pic);
                    }
                    if(pic.px) {
                        double kx = pic.pw / (pic.iw * fsx), ky = pic.ph / (pic.ih * fsy);
                        for(int y = rr[1]; y < rr[3]; y++) {
                            for(int x = rr[0]; x < rr[2]; x++) {
                                double ux = c.inv.a * (x + 0.5) + c.inv.c * (y + 0.5) + c.inv.e, uy = c.inv.b * (x + 0.5) + c.inv.d * (y + 0.5) + c.inv.f;
                                float col[4];
                                svgx_sample(pic.px, pic.pw, pic.ph, (ux - ftx) * kx, (uy - fty) * ky, false, col);
                                for(int q = 0; q < 4; q++) {
                                    out.p[((size_t)y * (size_t)w + (size_t)x) * 4 + q] = col[q] / 255.f;
                                }
                            }
                        }
                    }
                    free(pic.px);
                }
            }
            /* the picture is in sRGB: into this primitive's space */
            if(lin) {
                fbuf t = fb_space(&out, false, true);
                fb_free(&out);
                out = t;
            }
        } else if(strcmp(nm, "feTile") == 0) {
            out = fb_new(w, h);
            int tw = ra->r[2] - ra->r[0], th = ra->r[3] - ra->r[1];
            if(tw > 0 && th > 0) {
                for(int y = rr[1]; y < rr[3]; y++) {
                    for(int x = rr[0]; x < rr[2]; x++) {
                        int sx = ra->r[0] + ((x - ra->r[0]) % tw + tw) % tw, sy = ra->r[1] + ((y - ra->r[1]) % th + th) % th;
                        memcpy(out.p + ((size_t)y * (size_t)w + (size_t)x) * 4, A.p + ((size_t)sy * (size_t)w + (size_t)sx) * 4, 4 * sizeof(float));
                    }
                }
            }
        } else if(strcmp(nm, "feDropShadow") == 0) {
            double sdx = 2, sdy = 2;
            if(svgx_attr(p, "stdDeviation")) {
                two(svgx_attr(p, "stdDeviation"), &sdx, &sdy);
            }
            float rgb[3], al = 1;
            color_prop(p, "flood-color", rgb, &al, "black");
            char ob[32];
            const char* fos = svgx_prop(p, "flood-opacity", ob, sizeof ob);
            float fo = (fos ? strtof(fos, NULL) : 1.f) * al;
            fbuf shadow = fb_new(w, h);
            for(size_t i = 0; i < n; i++) {
                shadow.p[i * 4 + 3] = A.p[i * 4 + 3];
            }
            fb_blur(&shadow, (float)((c.obb_prim ? sdx * c.bb[2] : sdx) * c.sx), (float)((c.obb_prim ? sdy * c.bb[3] : sdy) * c.sy));
            double dx = prim_len(&c, svgx_attr(p, "dx") ? svgx_attr(p, "dx") : "2", 2, 0), dy = prim_len(&c, svgx_attr(p, "dy") ? svgx_attr(p, "dy") : "2", 2, 1);
            fb_offset(&shadow, (float)(c.dev.a * dx + c.dev.c * dy), (float)(c.dev.b * dx + c.dev.d * dy));
            out = fb_new(w, h);
            for(size_t i = 0; i < n; i++) {
                float sa = shadow.p[i * 4 + 3] * fo, sh[4];
                for(int ch = 0; ch < 3; ch++) {
                    sh[ch] = (lin ? to_linear(rgb[ch]) : rgb[ch]) * sa;
                }
                sh[3] = sa;
                px_over(out.p + i * 4, A.p + i * 4, sh);
            }
            fb_free(&shadow);
        } else {
            out = fb_copy(&A); /* (an unknown primitive passes its input on) */
        }
        fb_free(&A);
        fb_free(&B);
        /* cut to the subregion and the filter region */
        float* scov = own_sr ? svgx_rect_cover(rc, sr[0], sr[1], sr[2], sr[3]) : NULL;
        if(scov || c.rcov) {
            for(size_t i = 0; i < n; i++) {
                float k = (scov ? scov[i] : 1.f) * (c.rcov ? c.rcov[i] : 1.f);
                if(k >= 1.f) {
                    continue;
                }
                for(int q = 0; q < 4; q++) {
                    out.p[i * 4 + q] *= k;
                }
            }
        }
        free(scov);
        const char* rn = svgx_attr(p, "result");
        result* res = &rs[nr++];
        if(rn && *rn) {
            snprintf(res->name, sizeof res->name, "%.63s", rn);
        } else {
            snprintf(res->name, sizeof res->name, "\x01%d", made);
        }
        res->buf = out;
        res->lin = out_lin;
        memcpy(res->r, rr, sizeof rr);
        snprintf(lastname, sizeof lastname, "%s", res->name);
        made++;
    }
    uint8_t* px = NULL;
    if(made) {
        const result* last = &rs[nr - 1];
        fbuf fin = fb_space(&last->buf, last->lin, false);
        px = malloc(n * 4);
        if(!px) {
            abort();
        }
        for(size_t i = 0; i < n; i++) {
            float a = clamp01(fin.p[i * 4 + 3]);
            for(int ch = 0; ch < 3; ch++) {
                px[i * 4 + ch] = (uint8_t)lroundf(fminf(clamp01(fin.p[i * 4 + ch]), a) * 255.f);
            }
            px[i * 4 + 3] = (uint8_t)lroundf(a * 255.f);
        }
        fb_free(&fin);
    } else {
        px = calloc(n, 4); /* (a filter with no primitives: nothing is drawn) */
        if(!px) {
            abort();
        }
    }
    for(int i = 0; i < nr; i++) {
        fb_free(&rs[i].buf);
    }
    free(c.rcov);
    return px;
}
