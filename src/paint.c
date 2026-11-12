/* Walks a laid-out element tree and emits GPU primitives into a scene. */
#include "internal.h"

typedef struct { float x0, y0, x1, y1; } rect;

typedef struct {
    gp_scene *scene;
    gp_arena *scratch;
    gp_text_system *ts;
    float scale;
    gp_point mouse;       /* in the window */
    gp_point mouse_local; /* the same in the layout coordinates of what is being painted: only different inside a transform */
    gp_affine xf, inv;    /* the transform in force (layout coordinates to the window) and its inverse */
    bool has_xf;
    bool mouse_in, pressed;
    uint64_t focused, drag_key;
    gp_vviews *views;     /* NULL: no reuse of what cached views painted before */
    gp_app *app;          /* for statistics, may be NULL */
    uint32_t atlas_gen;
    uint32_t vis_gen;     /* the window's scroll_vis_gen: auto-hiding scrollbars changed */
    gp_color tc;          /* with has_tc: the text color a state style above set, for everything below that does not set its own */
    bool has_tc;
    /* offscreen layers open now (gp_paint_layer_begin): where each one's BEGIN is in the scene, and the box on screen of what was painted into it so far */
    int depth;
    size_t lbegin[GP_MAX_LAYERS];
    float lbox[GP_MAX_LAYERS][4];
} paint_ctx;

void gp_scene_push(gp_scene *s, const gp_instance *i) {
    if (s->len == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 1024;
        s->items = realloc(s->items, s->cap * sizeof *s->items);
        if (!s->items) abort();
    }
    s->items[s->len++] = *i;
}

void gp_scene_clear(gp_scene *s) { s->len = 0; }

static rect rect_isect(rect a, rect b) {
    return (rect){ fmaxf(a.x0, b.x0), fmaxf(a.y0, b.y0), fminf(a.x1, b.x1), fminf(a.y1, b.y1) };
}

static bool rect_has(rect r, gp_point p) { return p.x >= r.x0 && p.x < r.x1 && p.y >= r.y0 && p.y < r.y1; }

static void set4(float *dst, float a, float b, float c, float d) {
    dst[0] = a; dst[1] = b; dst[2] = c; dst[3] = d;
}

static void set_color(float *dst, gp_color c, float opacity) {
    set4(dst, c.r, c.g, c.b, c.a * opacity);
}

static void set_clip(gp_instance *i, rect clip) {
    set4(i->clip, clip.x0, clip.y0, fmaxf(clip.x1 - clip.x0, 0), fmaxf(clip.y1 - clip.y0, 0));
}

/* ---------------------------------------------------------------- transforms */

/* 4 x 4 and 3 x 3 helpers (column vectors: R = A B applies B first); out may be one of the inputs */
static void gp_m4_mul(const double A[4][4], const double B[4][4], double out[4][4]) {
    double R[4][4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) R[i][j] = A[i][0] * B[0][j] + A[i][1] * B[1][j] + A[i][2] * B[2][j] + A[i][3] * B[3][j];
    memcpy(out, R, sizeof R);
}
static void gp_m3_mul(const double A[3][3], const double B[3][3], double out[3][3]) {
    double R[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) R[i][j] = A[i][0] * B[0][j] + A[i][1] * B[1][j] + A[i][2] * B[2][j];
    memcpy(out, R, sizeof R);
}
/* M after turning about x by rx then about y by ry (degrees; CSS's rotateX / rotateY: with y down, a positive rotateY takes the right side away) */
static void gp_affine_3d_turns(double M[4][4], float rx, float ry) {
    double a = rx * M_PI / 180.0, c = cos(a), n = sin(a);
    double X[4][4] = { { 1, 0, 0, 0 }, { 0, c, -n, 0 }, { 0, n, c, 0 }, { 0, 0, 0, 1 } };
    a = ry * M_PI / 180.0; c = cos(a); n = sin(a);
    double Y[4][4] = { { c, 0, n, 0 }, { 0, 1, 0, 0 }, { -n, 0, c, 0 }, { 0, 0, 0, 1 } };
    gp_m4_mul(X, M, M);
    gp_m4_mul(Y, M, M);
}
/* M, which maps points relative to the origin (cx, cy) to points relative to it, as a map of the plane on the layout */
static gp_affine gp_m4_flatten_about(const double M[4][4], double cx, double cy) {
    double F[3][3] = { { M[0][0], M[0][1], M[0][3] }, { M[1][0], M[1][1], M[1][3] }, { M[3][0], M[3][1], M[3][3] } };
    double Tin[3][3] = { { 1, 0, -cx }, { 0, 1, -cy }, { 0, 0, 1 } }, Tout[3][3] = { { 1, 0, cx }, { 0, 1, cy }, { 0, 0, 1 } }, R[3][3], R2[3][3];
    gp_m3_mul(F, Tin, R);
    gp_m3_mul(Tout, R, R2);
    return gp_aff_from(R2);
}
static void gp_style_m4(const gp_style *s, double M[4][4]);

/* What a transformed element does to everything in it, about the point (ox, oy) of its box at layout position (x, y): scale, rotate, move. */
gp_affine gp_style_affine(const gp_style *s, float x, float y, float w, float h) {
    float cx = x + w * s->xf_ox, cy = y + h * s->xf_oy;
    float r = s->xf_rot * (float)M_PI / 180.f, cs = cosf(r), sn = sinf(r);
    if (fmodf(s->xf_rot, 90.f) == 0.f) {   /* quarter turns exactly, not nearly */
        int q = ((int)lroundf(s->xf_rot / 90.f) % 4 + 4) % 4;
        cs = q == 0 ? 1.f : q == 2 ? -1.f : 0.f;
        sn = q == 1 ? 1.f : q == 3 ? -1.f : 0.f;
    }
    gp_affine m = { cs * s->xf_sx, sn * s->xf_sx, -sn * s->xf_sy, cs * s->xf_sy, 0, 0, 0, 0, 0 };
    if (s->xf_kx != 0.f || s->xf_ky != 0.f) {   /* a skew first: x += tan(ax) y, y += tan(ay) x (beyond 89 degrees it is held there: a tangent that large is a line) */
        float kx = tanf(fminf(fmaxf(s->xf_kx, -89.f), 89.f) * (float)M_PI / 180.f), ky = tanf(fminf(fmaxf(s->xf_ky, -89.f), 89.f) * (float)M_PI / 180.f);
        gp_affine k = { 1, ky, kx, 1, 0, 0, 0, 0, 0 };
        m = gp_aff_mul(m, k);
    }
    if (s->xf_rx == 0.f && s->xf_ry == 0.f && s->xf_tz == 0.f && s->xf_persp == 0.f) {   /* flat: the 2D map as it always was */
        m.tx = cx + s->xf_tx - (m.a * cx + m.c * cy);
        m.ty = cy + s->xf_ty - (m.b * cx + m.d * cy);
        return m;
    }
    /* 3D, as CSS composes it, about the origin: the 2D part (skew, scale, the turn about z) after turns about x, then y; then the move (z too) and the element's own
     * perspective; flattened onto the plane (the rows and columns of x, y and w of the 4 x 4 matrix) */
    double M[4][4];
    gp_style_m4(s, M);
    return gp_m4_flatten_about(M, cx, cy);
}

/* the element's own transform as a 4 x 4 matrix about its origin (points relative to it, to points relative to it) */
static void gp_style_m4(const gp_style *s, double M[4][4]) {
    float r = s->xf_rot * (float)M_PI / 180.f, cs = cosf(r), sn = sinf(r);
    if (fmodf(s->xf_rot, 90.f) == 0.f) {
        int q = ((int)lroundf(s->xf_rot / 90.f) % 4 + 4) % 4;
        cs = q == 0 ? 1.f : q == 2 ? -1.f : 0.f;
        sn = q == 1 ? 1.f : q == 3 ? -1.f : 0.f;
    }
    gp_affine m = { cs * s->xf_sx, sn * s->xf_sx, -sn * s->xf_sy, cs * s->xf_sy, 0, 0, 0, 0, 0 };
    if (s->xf_kx != 0.f || s->xf_ky != 0.f) {
        float kx = tanf(fminf(fmaxf(s->xf_kx, -89.f), 89.f) * (float)M_PI / 180.f), ky = tanf(fminf(fmaxf(s->xf_ky, -89.f), 89.f) * (float)M_PI / 180.f);
        gp_affine k = { 1, ky, kx, 1, 0, 0, 0, 0, 0 };
        m = gp_aff_mul(m, k);
    }
    double B[4][4] = { { m.a, m.c, 0, 0 }, { m.b, m.d, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
    memcpy(M, B, sizeof B);
    gp_affine_3d_turns(M, s->xf_rx, s->xf_ry);
    double T[4][4] = { { 1, 0, 0, s->xf_tx }, { 0, 1, 0, s->xf_ty }, { 0, 0, 1, s->xf_tz }, { 0, 0, 0, 1 } };
    gp_m4_mul(T, M, M);
    if (s->xf_persp > 0.f) {
        double P[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, -1.0 / s->xf_persp, 1 } };
        gp_m4_mul(P, M, M);
    }
}

/* Is all of the element's box, through `m`, at or behind the viewer (a part in front is drawn: the GPU cuts away the rest)? */
bool gp_el_behind(const gp_el *n, gp_affine m, float x, float y) {
    if (!gp_aff_projective(m)) return false;
    gp_point c[4] = { { x, y }, { x + n->size.w, y }, { x + n->size.w, y + n->size.h }, { x, y + n->size.h } };
    for (int i = 0; i < 4; i++)
        if (gp_aff_w(m, c[i]) > 1e-6f) return false;
    return true;
}

/* Is the element, through `m`, seen from behind: its box's corners in the other order around (or not in front of the eye)? */
bool gp_el_backfacing(const gp_el *n, gp_affine m, float x, float y) {
    gp_point c[4] = { { x, y }, { x + n->size.w, y }, { x + n->size.w, y + n->size.h }, { x, y + n->size.h } };
    double area = 0;
    for (int i = 0; i < 4; i++) {
        if (!(gp_aff_w(m, c[i]) > 1e-6f)) return true;
        c[i] = gp_aff_apply(m, c[i]);
    }
    for (int i = 0; i < 4; i++) area += (double)c[i].x * c[(i + 1) % 4].y - (double)c[(i + 1) % 4].x * c[i].y;
    return area < 0;
}

/* ... and with the perspective its parent gives its children (gp_perspective), seen from the parent's perspective origin */
gp_affine gp_el_affine(const gp_el *n, float x, float y) {
    const gp_style *s = &n->style;
    const gp_el *p = n->parent;
    if (!p || !(p->style.persp > 0.f)) return gp_style_affine(s, x, y, n->size.w, n->size.h);
    /* the element's own 3D matrix about its origin, then the parent's perspective about the parent's perspective origin (both on the layout) */
    float cx = x + n->size.w * s->xf_ox, cy = y + n->size.h * s->xf_oy;
    float px = x - n->pos.x + p->scroll.x + p->size.w * p->style.persp_ox, py = y - n->pos.y + p->scroll.y + p->size.h * p->style.persp_oy;
    double M[4][4];
    gp_style_m4(s, M);
    double A[4][4] = { { 1, 0, 0, cx - px }, { 0, 1, 0, cy - py }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };   /* the element's origin as seen from the parent's */
    gp_m4_mul(A, M, M);
    double P[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, -1.0 / p->style.persp, 1 } };
    gp_m4_mul(P, M, M);
    /* M maps points relative to the element's origin to points relative to the parent's perspective origin: flatten with the two offsets */
    double F[3][3] = { { M[0][0], M[0][1], M[0][3] }, { M[1][0], M[1][1], M[1][3] }, { M[3][0], M[3][1], M[3][3] } };
    double Tin[3][3] = { { 1, 0, -cx }, { 0, 1, -cy }, { 0, 0, 1 } }, Tout[3][3] = { { 1, 0, px }, { 0, 1, py }, { 0, 0, 1 } }, R[3][3], R2[3][3];
    gp_m3_mul(F, Tin, R);
    gp_m3_mul(Tout, R, R2);
    return gp_aff_from(R2);
}

/* a rectangle through a transform that keeps the axes (a scale, a move, a quarter turn): the box it becomes; false for any other */
static bool aff_rect(gp_affine m, rect r, rect *out) {
    if (gp_aff_projective(m)) return false;
    bool straight = (fabsf(m.b) < 1e-5f && fabsf(m.c) < 1e-5f) || (fabsf(m.a) < 1e-5f && fabsf(m.d) < 1e-5f);
    if (!straight) return false;
    gp_point p0 = gp_aff_apply(m, (gp_point){ r.x0, r.y0 }), p1 = gp_aff_apply(m, (gp_point){ r.x1, r.y1 });
    out->x0 = fminf(p0.x, p1.x); out->x1 = fmaxf(p0.x, p1.x);
    out->y0 = fminf(p0.y, p1.y); out->y1 = fmaxf(p0.y, p1.y);
    return true;
}

/* Filters, a mask and a blend mode on a layer that closes (layer_end_fx) */
typedef struct {
    const gp_style *s;
    gp_el *mask;          /* painted at (mx, my) */
    float mx, my;
    rect clip;            /* the clip the element is under: what the layer puts down is cut to it (a blur spreads past what was painted, not past this) */
} layer_fx;

static void paint_node(paint_ctx *pc, gp_el *n, float ox, float oy, rect clip, float opacity);

/* a color filter as CSS defines it: a 4 x 5 matrix over straight (not premultiplied) RGBA, rows R G B A, the fifth column an offset */
static void fx_matrix(const gp_filter_op *op, float M[4][5]) {
    memset(M, 0, sizeof(float[4][5]));
    for (int i = 0; i < 4; i++) M[i][i] = 1.f;
    float v = op->v, s = 1.f - v;
    switch (op->kind) {
    case GP_FX_BRIGHTNESS: M[0][0] = M[1][1] = M[2][2] = v; break;
    case GP_FX_CONTRAST:
        M[0][0] = M[1][1] = M[2][2] = v;
        M[0][4] = M[1][4] = M[2][4] = 0.5f - 0.5f * v;
        break;
    case GP_FX_SATURATE: {
        const float m[3][3] = { { 0.213f + 0.787f * v, 0.715f - 0.715f * v, 0.072f - 0.072f * v },
                                { 0.213f - 0.213f * v, 0.715f + 0.285f * v, 0.072f - 0.072f * v },
                                { 0.213f - 0.213f * v, 0.715f - 0.715f * v, 0.072f + 0.928f * v } };
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) M[i][j] = m[i][j];
        break;
    }
    case GP_FX_GRAYSCALE: {
        const float m[3][3] = { { 0.2126f + 0.7874f * s, 0.7152f - 0.7152f * s, 0.0722f - 0.0722f * s },
                                { 0.2126f - 0.2126f * s, 0.7152f + 0.2848f * s, 0.0722f - 0.0722f * s },
                                { 0.2126f - 0.2126f * s, 0.7152f - 0.7152f * s, 0.0722f + 0.9278f * s } };
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) M[i][j] = m[i][j];
        break;
    }
    case GP_FX_SEPIA: {
        const float m[3][3] = { { 0.393f + 0.607f * s, 0.769f - 0.769f * s, 0.189f - 0.189f * s },
                                { 0.349f - 0.349f * s, 0.686f + 0.314f * s, 0.168f - 0.168f * s },
                                { 0.272f - 0.272f * s, 0.534f - 0.534f * s, 0.131f + 0.869f * s } };
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) M[i][j] = m[i][j];
        break;
    }
    case GP_FX_INVERT:
        M[0][0] = M[1][1] = M[2][2] = 1.f - 2.f * v;
        M[0][4] = M[1][4] = M[2][4] = v;
        break;
    case GP_FX_HUE_ROTATE: {
        float a = v * (float)M_PI / 180.f, c = cosf(a), n = sinf(a);
        const float m[3][3] = { { 0.213f + c * 0.787f - n * 0.213f, 0.715f - c * 0.715f - n * 0.715f, 0.072f - c * 0.072f + n * 0.928f },
                                { 0.213f - c * 0.213f + n * 0.143f, 0.715f + c * 0.285f + n * 0.140f, 0.072f - c * 0.072f - n * 0.283f },
                                { 0.213f - c * 0.213f - n * 0.787f, 0.715f - c * 0.715f + n * 0.715f, 0.072f + c * 0.928f + n * 0.072f } };
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) M[i][j] = m[i][j];
        break;
    }
    case GP_FX_OPACITY: M[3][3] = v; break;
    }
}

/* ---------------------------------------------------------------- layers */

/* Where an instance can draw on screen (logical px), conservatively: its quad grown by what the shader adds around it (a shadow's blur, a pixel of
 * antialiasing), through its transform, cut to its clip. An open layer's box is the union of these, and only that much of it is cleared and composited. */
static void inst_screen_box(const gp_instance *in, float out[4]) {
    if ((int)in->kind == GP_PRIM_LAYER_END) {   /* (a layer inside: its region, which already is on screen) */
        set4(out, in->bounds[0], in->bounds[1], in->bounds[0] + in->bounds[2], in->bounds[1] + in->bounds[3]);
        return;
    }
    float m = (int)in->kind == GP_PRIM_SHADOW ? in->blur * 1.5f + 2.f : 2.f;
    float x0 = in->bounds[0] - m, y0 = in->bounds[1] - m, x1 = in->bounds[0] + in->bounds[2] + m, y1 = in->bounds[1] + in->bounds[3] + m;
    float a = 1.f + in->xf0[0], b = in->xf0[1], c = in->xf0[2], d = 1.f + in->xf0[3], tx = in->xf1[0], ty = in->xf1[1], g = in->xf1[2], h = in->xf1[3];
    float xs[4] = { x0, x1, x0, x1 }, ys[4] = { y0, y0, y1, y1 };
    float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
    for (int i = 0; i < 4; i++) {
        float w = g * xs[i] + h * ys[i] + 1.f + in->xf2[0];
        if (!(w > 1e-6f)) { bx0 = by0 = -1e30f; bx1 = by1 = 1e30f; break; }   /* (a corner at or behind the eye: anywhere, as far as the clip lets it) */
        float X = (a * xs[i] + c * ys[i] + tx) / w, Y = (b * xs[i] + d * ys[i] + ty) / w;
        bx0 = fminf(bx0, X); by0 = fminf(by0, Y); bx1 = fmaxf(bx1, X); by1 = fmaxf(by1, Y);
    }
    set4(out, fmaxf(bx0, in->clip[0]), fmaxf(by0, in->clip[1]), fminf(bx1, in->clip[0] + in->clip[2]), fminf(by1, in->clip[1] + in->clip[3]));
}

/* an instance put in the scene: what it covers goes into the box of the innermost open layer */
static void scene_add(paint_ctx *pc, const gp_instance *in) {
    gp_scene_push(pc->scene, in);
    if (!pc->depth || (int)in->kind == GP_PRIM_LAYER_BEGIN) return;
    float b[4];
    inst_screen_box(in, b);
    if (!(b[2] > b[0] && b[3] > b[1])) return;
    float *L = pc->lbox[pc->depth - 1];
    L[0] = fminf(L[0], b[0]); L[1] = fminf(L[1], b[1]); L[2] = fmaxf(L[2], b[2]); L[3] = fmaxf(L[3], b[3]);
}

/* Opens a layer: what is painted until the matching end goes into a texture of its own, which is then put on what is under it at once (with an
 * opacity: a group fades as one thing, and a translucent stroke that crosses itself is not darker where it does). False when too many are open:
 * the caller paints as it would without one. */
static bool layer_begin(paint_ctx *pc) {
    if (pc->depth >= GP_MAX_LAYERS) return false;
    gp_instance in = { 0 };
    in.kind = GP_PRIM_LAYER_BEGIN;
    pc->lbegin[pc->depth] = pc->scene->len;
    gp_scene_push(pc->scene, &in);   /* (its region is known at the end) */
    set4(pc->lbox[pc->depth], 1e30f, 1e30f, -1e30f, -1e30f);
    pc->depth++;
    return true;
}

/* Closes it: the region (the box of what was painted, out to whole device pixels) goes on both markers, and the end is the quad that composites
 * the texture at `alpha`. A layer nothing was painted into is taken out. */
/* how far filters spread what is in L (logical px, x0 y0 x1 y1): a blur by three of its deviations, a shadow by its offset and its blur's */
static void filters_spread(const gp_filter_op *ops, int nf, float L[4]) {
    for (int i = 0; i < nf; i++) {
        const gp_filter_op *op = &ops[i];
        if (op->kind == GP_FX_BLUR) {
            float g = 3.f * op->v;
            L[0] -= g; L[1] -= g; L[2] += g; L[3] += g;
        } else if (op->kind == GP_FX_DROP_SHADOW) {
            float g = 1.5f * op->v;   /* (sigma is half the blur, as for gp_shadow: three of them) */
            L[0] = fminf(L[0], L[0] + op->dx - g); L[1] = fminf(L[1], L[1] + op->dy - g);
            L[2] = fmaxf(L[2], L[2] + op->dx + g); L[3] = fmaxf(L[3], L[3] + op->dy + g);
        }
    }
}

/* the passes for filters over a region (a layer's, or a backdrop's copy), in order */
static void emit_passes(paint_ctx *pc, const gp_filter_op *ops, int nf, const float region[4]) {
    for (int i = 0; i < nf; i++) {
        const gp_filter_op *op = &ops[i];
        gp_instance f = { 0 };
        memcpy(f.bounds, region, 4 * sizeof *region);
        memcpy(f.clip, region, 4 * sizeof *region);
        if (op->kind == GP_FX_BLUR || op->kind == GP_FX_DROP_SHADOW) {
            float sigma = op->kind == GP_FX_BLUR ? op->v : op->v * 0.5f;
            f.kind = GP_PRIM_FX_BLUR;
            f.blur = sigma;
            set4(f.side[0], 0.f, op->kind == GP_FX_DROP_SHADOW, 0.f, 0.f);
            gp_scene_push(pc->scene, &f);
            if (op->kind == GP_FX_BLUR) {
                f.side[0][0] = 1.f;
            } else {
                f.kind = GP_PRIM_FX_SHADOW;
                set4(f.side[0], op->dx, op->dy, 0.f, 0.f);
                set_color(f.color, op->color, 1.f);
            }
            gp_scene_push(pc->scene, &f);
        } else {
            float M[4][5];
            fx_matrix(op, M);
            f.kind = GP_PRIM_FX_MATRIX;
            for (int j = 0; j < 4; j++) {
                f.color[j] = M[0][j];
                f.border_color[j] = M[1][j];
                f.radii[j] = M[2][j];
                f.uv[j] = M[3][j];
            }
            set4(f.side[0], M[0][4], M[1][4], M[2][4], M[3][4]);
            gp_scene_push(pc->scene, &f);
        }
    }

}

static void layer_end_fx(paint_ctx *pc, float alpha, const layer_fx *fx) {
    pc->depth--;
    size_t at = pc->lbegin[pc->depth];
    float L[4];
    memcpy(L, pc->lbox[pc->depth], sizeof L);
    if (pc->scene->len == at + 1 || !(L[2] > L[0] && L[3] > L[1]) || alpha <= 0.f) {
        pc->scene->len = at;
        return;
    }
    const gp_style *st = fx ? fx->s : NULL;
    int nf = st ? st->nfilters : 0;
    if (nf) filters_spread(st->filters, nf, L);
    float s = pc->scale, lim = 1e6f;
    float x0 = floorf(fmaxf(L[0], -lim) * s) / s, y0 = floorf(fmaxf(L[1], -lim) * s) / s;
    float x1 = ceilf(fminf(L[2], lim) * s) / s, y1 = ceilf(fminf(L[3], lim) * s) / s;
    float region[4] = { x0, y0, x1 - x0, y1 - y0 };
    gp_instance *b = &pc->scene->items[at];
    memcpy(b->bounds, region, sizeof region);
    memcpy(b->clip, region, sizeof region);

    /* the mask: a layer of its own over the same region (cleared all over it: where the mask paints nothing, nothing is seen) */
    int mask_mode = 0;
    if (fx && fx->mask) {
        size_t mat = pc->scene->len;
        if (layer_begin(pc)) {
            paint_node(pc, fx->mask, fx->mx, fx->my, fx->clip, 1.f);
            pc->depth--;
            gp_instance *mb = &pc->scene->items[mat];
            memcpy(mb->bounds, region, sizeof region);
            memcpy(mb->clip, region, sizeof region);
            gp_instance me = { 0 };
            me.kind = GP_PRIM_MASK_END;
            memcpy(me.bounds, region, sizeof region);
            memcpy(me.clip, region, sizeof region);
            gp_scene_push(pc->scene, &me);
            mask_mode = 1 + (st && st->mask_mode == 1);
        } else {
            static bool warned;
            if (!warned) { warned = true; fprintf(stderr, "gpuic: a mask nested more than %d layers deep is not applied\n", GP_MAX_LAYERS); }
        }
    }

    emit_passes(pc, st ? st->filters : NULL, nf, region);   /* the filters, as passes over the layer's region, in order */

    gp_instance e = { 0 };
    e.kind = GP_PRIM_LAYER_END;
    memcpy(e.bounds, region, sizeof region);
    rect c = { x0, y0, x1, y1 };
    if (fx) c = rect_isect(c, fx->clip);
    set_clip(&e, c);
    set4(e.color, 1.f, 1.f, 1.f, fminf(alpha, 1.f));
    e.uv[0] = st ? (float)st->blend_mode : 0.f;
    e.uv[1] = (float)mask_mode;
    scene_add(pc, &e);
}

static void layer_end(paint_ctx *pc, float alpha) { layer_end_fx(pc, alpha, NULL); }

/* every instance carries the transform it was painted under (all zeros: none) */
static void push(paint_ctx *pc, gp_instance *inst) {
    if (pc->has_xf) {
        const gp_affine *m = &pc->xf;
        float mag = sqrtf(fabsf(m->a * m->d - m->b * m->c));
        inst->xf0[0] = m->a - 1.f; inst->xf0[1] = m->b; inst->xf0[2] = m->c; inst->xf0[3] = m->d - 1.f;
        inst->xf1[0] = m->tx; inst->xf1[1] = m->ty; inst->xf1[2] = m->g; inst->xf1[3] = m->h;
        inst->xf2[0] = m->w1;
        (void)mag;
    }
    scene_add(pc, inst);
}

/* an instance that is already where it is on the screen: put in the scene with no transform, whatever is being painted under */
static void push_placed(paint_ctx *pc, gp_instance *inst) { scene_add(pc, inst); }

/* How text of a font size is rasterized under the transform it is painted in. Glyphs are made for the size they have on the screen: the font size times the window's
 * scale times what the transform magnifies by (the root of its determinant: for sx, sy the geometric mean), to the quarter pixel the glyph cache keeps sizes by and no
 * more than GP_TEXT_MAX_RASTER_PX when the transform is what makes them big (one glyph must fit an atlas; beyond that the bitmap is stretched the rest of the way).
 *   px      the size to ask the glyph cache for
 *   k       px over the size without the transform: a bitmap is drawn at 1 / k of its size in the layout, and the transform makes up the rest
 *   placed  the transform keeps the axes and scales both alike (a zoom and a move), and px is the size on the screen: each glyph's origin is taken through the transform,
 *           snapped to the pixel grid there and drawn as it is, exactly as text under no transform is (hinted for its size, sub-pixel positions, LCD if that is on).
 * Otherwise (turned, skewed, sx != sy, capped) the bitmaps go through the transform, sampled between texels. In perspective nothing changes: the size is the layout's. */
#define GP_TEXT_MAX_RASTER_PX 256.f
typedef struct { float px, k; bool placed; } text_raster;
static text_raster text_raster_for(const paint_ctx *pc, float size) {
    text_raster r = { size * pc->scale, 1.f, false };
    if (!pc->has_xf || !(r.px > 0.f)) return r;
    const gp_affine *m = &pc->xf;
    if (m->g != 0.f || m->h != 0.f || m->w1 != 0.f) return r;
    float mag = sqrtf(fabsf(m->a * m->d - m->b * m->c));
    if (!(mag > 1e-3f)) return r;
    bool axes = fabsf(m->b) <= 1e-4f * mag && fabsf(m->c) <= 1e-4f * mag && m->a > 0.f && m->d > 0.f && fabsf(m->a - m->d) <= 1e-3f * mag;
    if (fabsf(mag - 1.f) < 1e-4f) {   /* nothing to make up for: the size it has anyway */
        r.placed = axes;
        return r;
    }
    float want = r.px * mag, q = floorf(want * 4.f + 0.5f) * 0.25f;
    if (q < 0.25f) q = 0.25f;
    if (q > GP_TEXT_MAX_RASTER_PX && mag > 1.f) q = fmaxf(GP_TEXT_MAX_RASTER_PX, floorf(r.px * 4.f + 0.5f) * 0.25f);
    r.k = q / r.px;
    r.px = q;
    r.placed = axes && fabsf(q - want) <= 0.126f;
    return r;
}

/* ---------------------------------------------------------------- stroked curves */

/* How a stroke is colored: a color, or a gradient along its whole length (ramp row `row`, `total` the length). */
typedef struct { gp_color color; float opacity; int row; float total; const gp_gradient *grad; } stroke_paint;

/* The gradient's color at t (0..1), interpolated premultiplied as the ramp is; returned straight. */
static gp_color ramp_at(const gp_gradient *g, float t) {
    if (g->n <= 0) return (gp_color){ 0, 0, 0, 0 };
    const gp_color_stop *st = g->stops;
    if (t <= st[0].at || g->n == 1) return st[0].color;
    for (int i = 1; i < g->n; i++)
        if (t <= st[i].at) {
            float span = st[i].at - st[i - 1].at, u = span > 0.f ? (t - st[i - 1].at) / span : 1.f;
            gp_color c0 = st[i - 1].color, c1 = st[i].color;
            float al = c0.a + (c1.a - c0.a) * u;
            if (al <= 0.f) return (gp_color){ 0, 0, 0, 0 };
            return (gp_color){ (c0.r * c0.a + (c1.r * c1.a - c0.r * c0.a) * u) / al, (c0.g * c0.a + (c1.g * c1.a - c0.g * c0.a) * u) / al,
                               (c0.b * c0.a + (c1.b * c1.a - c0.b * c0.a) * u) / al, al };
        }
    return st[g->n - 1].color;
}

static void stroke_color_at(gp_instance *inst, const stroke_paint *sp, float t) {
    set_color(inst->color, sp->grad ? ramp_at(sp->grad, t) : sp->color, sp->opacity);
}

/* One open (or closed) run of the stroke, p[0..m) starting at length arc0 of the whole: one instance a piece, each a capsule (or, at a joint, the part of the stroke on its own
 * side of the plane that bisects the angle there), drawn by the fragment shader from the distance to the piece, so edges are anti-aliased exactly, round caps and joins come
 * out of the distance, and no pixel is covered by two pieces: a translucent stroke is blended once everywhere. The planes make the pieces tile the stroke (a pixel belongs to
 * the piece whose side of both planes it is on). An end is capped (round or flat), or, with cut[e], cut square by the plane through it with normal cut[e] pointing out of the
 * run (where an arrowhead takes over). */
static void stroke_run(paint_ctx *pc, const gp_style *s, const gp_point *p, size_t m, float arc0, float x, float y, rect clip, float hw, bool miter, bool closed,
                       const gp_point *cut0, const gp_point *cut1, const stroke_paint *sp) {
    const bool butt = s->stroke_cap == 1;
    float pad = hw * (miter ? fmaxf(1.f, s->miter_limit) : 1.f) + 1.5f;   /* (and a pixel and a half for the soft edge) */
    if (m == 1) {   /* a point: a dot, if the ends are round */
        if (butt || cut0 || cut1) return;
        gp_instance inst = { 0 };
        set4(inst.bounds, x + p[0].x - pad, y + p[0].y - pad, 2 * pad, 2 * pad);
        set_clip(&inst, clip);
        stroke_color_at(&inst, sp, sp->total > 0.f ? arc0 / sp->total : 0.f);
        inst.kind = GP_PRIM_SEGMENT;
        set4(inst.radii, hw, 0, 0, 0);
        push(pc, &inst);
        return;
    }
    if (closed && m < 4) closed = false;   /* (a closed run needs at least three corners) */
    /* the unit directions of the pieces, and the plane at every joint: the one through the vertex whose normal is the mean direction */
    gp_point *dir = gp_arena_alloc(pc->scratch, (m - 1) * sizeof *dir, 4);
    gp_point *plane = gp_arena_alloc(pc->scratch, m * sizeof *plane, 4);   /* the mean direction at vertex i (1 .. m-2; 0 too when closed) */
    uint8_t *style = gp_arena_alloc(pc->scratch, m, 1);                    /* at vertex i: 0 round, 1 miter */
    memset(plane, 0, m * sizeof *plane);
    memset(style, 0, m);
    for (size_t i = 0; i + 1 < m; i++) {
        float dx = p[i + 1].x - p[i].x, dy = p[i + 1].y - p[i].y, l = hypotf(dx, dy);
        dir[i] = l > 0.f ? (gp_point){ dx / l, dy / l } : (gp_point){ 1, 0 };
    }
    for (size_t i = closed ? 0 : 1; i + 1 < m; i++) {
        gp_point din = i ? dir[i - 1] : dir[m - 2];
        float mx = din.x + dir[i].x, my = din.y + dir[i].y, l = hypotf(mx, my);
        if (l < 1e-4f) { plane[i] = din; style[i] = 0; continue; }   /* a turn right back on itself: cut square */
        plane[i] = (gp_point){ mx / l, my / l };
        float cos_half = plane[i].x * dir[i].x + plane[i].y * dir[i].y;   /* of half the turn: the miter is 1 / cos_half times the half width */
        style[i] = miter && cos_half > 1e-3f && 1.f / cos_half <= s->miter_limit ? 1 : 0;
    }
    if (closed) { plane[m - 1] = plane[0]; style[m - 1] = style[0]; }
    float run = arc0;
    for (size_t i = 0; i + 1 < m; i++) {
        gp_point a = p[i], b = p[i + 1];
        float len = hypotf(b.x - a.x, b.y - a.y);
        float minx = fminf(a.x, b.x) - pad, maxx = fmaxf(a.x, b.x) + pad, miny = fminf(a.y, b.y) - pad, maxy = fmaxf(a.y, b.y) + pad;
        float cx = (minx + maxx) * 0.5f, cy = (miny + maxy) * 0.5f;
        gp_instance inst = { 0 };
        set4(inst.bounds, x + minx, y + miny, maxx - minx, maxy - miny);
        set_clip(&inst, clip);
        if (sp->grad) {   /* along the stroke: the ramp from this piece's start to its end */
            set4(inst.color, 0, 0, 0, sp->opacity);
            inst.border_color[0] = run / sp->total;
            inst.border_color[1] = (run + len) / sp->total;
            inst.bsty[2] = ((float)sp->row + 0.5f) / (float)GP_GRAD_ROWS;
        } else {
            set_color(inst.color, sp->color, sp->opacity);
        }
        run += len;
        inst.kind = GP_PRIM_SEGMENT;
        set4(inst.uv, a.x - cx, a.y - cy, b.x - cx, b.y - cy);   /* the ends, relative to the middle of the bounds (what the shader's local position is) */
        bool first = i == 0 && !closed, last = i + 2 == m && !closed;
        float sa = first ? (cut0 ? 0.f : butt ? 2.f : 0.f) : (float)style[i], sb = last ? (cut1 ? 0.f : butt ? 2.f : 0.f) : (float)style[i + 1];
        set4(inst.radii, hw, sa, sb, 0);
        gp_point na = first ? (cut0 ? *cut0 : (gp_point){ 0, 0 }) : (gp_point){ -plane[i].x, -plane[i].y };
        gp_point nb = last ? (cut1 ? *cut1 : (gp_point){ 0, 0 }) : plane[i + 1];
        set4(inst.side[0], na.x, na.y, nb.x, nb.y);
        push(pc, &inst);
    }
}

/* A triangle, its corners relative to (x, y); edge[j] for the edge from corner j to j + 1: 0 anti-aliased (an outline), 1 / -1 cut hard on a line shared with another piece
 * (1: a pixel on the line is this one's), 2 / -2 the base of an arrowhead: hard as 1 / -1 where the stroke meets it (within `hw` of its middle), anti-aliased beyond. */
static void push_triangle(paint_ctx *pc, const gp_point v[3], const int8_t edge[3], float hw, float x, float y, rect clip, gp_color color, float opacity) {
    float minx = fminf(v[0].x, fminf(v[1].x, v[2].x)) - 1.5f, maxx = fmaxf(v[0].x, fmaxf(v[1].x, v[2].x)) + 1.5f;
    float miny = fminf(v[0].y, fminf(v[1].y, v[2].y)) - 1.5f, maxy = fmaxf(v[0].y, fmaxf(v[1].y, v[2].y)) + 1.5f;
    float cx = (minx + maxx) * 0.5f, cy = (miny + maxy) * 0.5f;
    gp_instance inst = { 0 };
    set4(inst.bounds, x + minx, y + miny, maxx - minx, maxy - miny);
    set_clip(&inst, clip);
    set_color(inst.color, color, opacity);
    inst.kind = GP_PRIM_TRIANGLE;
    set4(inst.uv, x + v[0].x, y + v[0].y, x + v[1].x, y + v[1].y);   /* in the layout, not relative to the bounds: see the shader */
    set4(inst.side[0], x + v[2].x, y + v[2].y, 0, 0);
    (void)cx; (void)cy;
    set4(inst.radii, edge[0], edge[1], edge[2], hw);
    push(pc, &inst);
}

/* Text along the centerline: each glyph turned to the direction at its middle, its baseline on the line (moved `path_text_dy` to the right of the direction of travel),
 * placed by the text's alignment (start, center, end) along the length. A glyph that would run off either end is not drawn. */
static void paint_path_text(paint_ctx *pc, gp_el *n, const gp_point *p, const float *arc, size_t m, float x, float y, rect clip, float opacity) {
    const char *text = n->style.path_text;
    size_t len = strlen(text);
    if (!len || m < 2) return;
    int cap = (int)len + 1;
    gp_glyph_pos *g = gp_arena_alloc(pc->scratch, (size_t)cap * sizeof *g, 8);
    float lw = 0;
    int ng = gp_text_shape_dir(pc->ts, GP_TEXT_SEL(n->ts), n->ts.size, text, len, GP_BIDI_LTR, g, cap, &lw);
    if (ng > cap) {
        cap = ng;
        g = gp_arena_alloc(pc->scratch, (size_t)cap * sizeof *g, 8);
        ng = gp_text_shape_dir(pc->ts, GP_TEXT_SEL(n->ts), n->ts.size, text, len, GP_BIDI_LTR, g, cap, &lw);
    }
    float total = arc[m - 1], start = 0.f;
    if (n->ts.align == GP_TEXT_ALIGN_CENTER) start = (total - lw) * 0.5f;
    else if (n->ts.align == GP_TEXT_ALIGN_END || n->ts.align == GP_TEXT_ALIGN_RIGHT) start = total - lw;
    const text_raster tr = text_raster_for(pc, n->ts.size);   /* (for what the curve is under: each glyph is turned as well, so none is placed) */
    const float scale = pc->scale * tr.k, px_size = tr.px, inv_alpha = 1.f / (float)GP_ATLAS_SIZE, inv_color = 1.f / (float)GP_COLOR_ATLAS_SIZE;
    const bool had = pc->has_xf;
    const gp_affine outer = had ? pc->xf : gp_aff_identity();
    for (int i = 0; i < ng; i++) {
        float mid = start + g[i].x + g[i].advance * 0.5f;
        if (start + g[i].x < -0.01f || start + g[i].x + g[i].advance > total + 0.01f) continue;
        size_t piece;
        gp_point c = gp_curve_at(p, arc, m, mid, &piece);
        float dx = p[piece + 1].x - p[piece].x, dy = p[piece + 1].y - p[piece].y, l = hypotf(dx, dy);
        if (!(l > 0.f)) continue;
        dx /= l; dy /= l;
        float dyb = n->style.path_text_dy + g[i].y;
        gp_point o = { x + c.x - dx * g[i].advance * 0.5f - dy * dyb, y + c.y - dy * g[i].advance * 0.5f + dx * dyb };   /* the glyph's origin on the baseline */
        gp_glyph_entry e = gp_text_glyph_lcd(pc->ts, g[i].font, g[i].glyph, px_size, 0, 0);
        if (e.w == 0) continue;
        gp_affine turn = { dx, dy, -dy, dx, 0, 0, 0, 0, 0 };   /* about the origin */
        turn.tx = o.x - (dx * o.x - dy * o.y);
        turn.ty = o.y - (dy * o.x + dx * o.y);
        pc->xf = gp_aff_mul(outer, turn);
        pc->has_xf = true;
        gp_instance inst = { 0 };
        set4(inst.bounds, o.x + e.left / scale, o.y - e.top / scale, e.w / scale, e.h / scale);
        set_clip(&inst, clip);
        float inv = e.color ? inv_color : inv_alpha;
        set4(inst.uv, e.u * inv, e.v * inv, e.w * inv, e.h * inv);
        if (e.color) { set_color(inst.color, (gp_color){ 1, 1, 1, 1 }, opacity); inst.kind = GP_PRIM_COLOR_GLYPH; }
        else { set_color(inst.color, n->ts.color, opacity); inst.kind = GP_PRIM_GLYPH; }
        push(pc, &inst);
    }
    pc->has_xf = had;
    pc->xf = outer;
}

/* A stroked curve: its fill, the stroke (dashed or not, cut short for its arrowheads), the arrowheads, and text along it. */
static void paint_curve(paint_ctx *pc, gp_el *n, float x, float y, rect clip, float opacity, gp_color color, bool recolored) {
    const gp_style *s = &n->style;
    float hw = s->stroke_w * 0.5f;
    float mag = 1.f;
    if (pc->has_xf) mag = sqrtf(fabsf(pc->xf.a * pc->xf.d - pc->xf.b * pc->xf.c));
    float tol = 0.25f / (pc->scale * (mag > 1e-4f ? mag : 1e-4f));   /* a quarter of a device pixel, in the element's own units */
    gp_point *p;
    size_t m = gp_curve_flatten(n, tol, pc->scratch, &p);
    if (!m) return;
    float *arc = gp_curve_arcs(p, m, pc->scratch);
    if (s->has_fill && s->fill_color.a > 0.f && m >= 3) {   /* under the stroke: triangles, outline edges soft, the diagonals between them hard and owned by one */
        gp_point *t;
        int8_t *ed;
        size_t nt = gp_curve_fill(p, m, s->fill_rule, pc->scratch, &t, &ed);
        for (size_t i = 0; i < nt; i++) push_triangle(pc, t + 3 * i, ed + 3 * i, 0.f, x, y, clip, s->fill_color, opacity);
    }
    if (hw > 0.f && color.a > 0.f) {
        stroke_paint sp = { color, opacity, -1, arc[m - 1], NULL };
        const gp_gradient *grad = s->stroke_gradient;
        if (grad && grad->n > 0 && !recolored && arc[m - 1] > 0.f) {
            gp_gfx *gfx = gp_text_gfx(pc->ts);
            int row = gfx ? gp_gfx_gradient_row(gfx, grad) : -1;
            if (row >= 0) { sp.row = row; sp.grad = grad; }
            else sp.color = grad->stops[0].color;   /* the ramp table is full: repainted after it is cleared */
        }
        /* A translucent stroke that can cover a pixel twice (it crosses or nears itself, has markers, or round-capped dashes close together) is drawn opaque into a
         * layer that is put down at its alpha, so it is no darker there. Any other is drawn straight onto what is under it: its pieces tile it, each pixel blended once,
         * at no more cost than an opaque one. A gradient's own alpha stays in its colors (the layer takes only the opacity). */
        float layer_alpha = 1.f;
        if ((sp.grad ? opacity : color.a * opacity) < 1.f && gp_curve_self_overlaps(n, p, arc, m, pc->scale) && layer_begin(pc)) {
            layer_alpha = sp.grad ? opacity : color.a * opacity;
            sp.color.a = sp.grad ? sp.color.a : 1.f;
            sp.opacity = 1.f;
        }
        const bool miter = s->stroke_join == 1 && n->curve_kind == 1;
        gp_point tri[2][3], cutn[2];
        float cut[2] = { 0.f, arc[m - 1] };
        bool head[2];
        for (int e = 0; e < 2; e++) {
            head[e] = gp_curve_arrow(n, p, arc, m, e, tri[e], &cut[e]);
            if (head[e]) {
                gp_point b = { (tri[e][1].x + tri[e][2].x) * 0.5f, (tri[e][1].y + tri[e][2].y) * 0.5f };
                float dx = tri[e][0].x - b.x, dy = tri[e][0].y - b.y, l = hypotf(dx, dy);
                cutn[e] = (gp_point){ dx / l, dy / l };   /* out of the stroke, towards the tip */
            }
        }
        if (cut[1] < cut[0]) cut[1] = cut[0];
        float pat[16], period = 0.f;
        int np = 0;
        for (int i = 0; i < s->dash_n && i < 8; i++) pat[np++] = fmaxf(s->dash[i], 0.f);
        if (np % 2) { memcpy(pat + np, pat, (size_t)np * sizeof *pat); np *= 2; }
        for (int i = 0; i < np; i++) period += pat[i];
        if (np && period > 1e-3f) {   /* dashes: each one an open run of its own, capped as the stroke is; the arrowheads' cuts stay square */
            float off = fmodf(s->dash_off, period);
            if (off < 0.f) off += period;
            int k = 0;
            float at = cut[0] - off;   /* where the current entry of the pattern starts, in lengths along the stroke */
            size_t guard = 0;
            while (at < cut[1] && guard++ < 1000000) {
                float d0 = at, d1 = at + pat[k];
                if (k % 2 == 0 && d1 > cut[0] && pat[k] > 0.f) {
                    float a0 = fmaxf(d0, cut[0]), a1 = fminf(d1, cut[1]);
                    gp_point *q;
                    size_t mq = gp_curve_cut(p, arc, m, a0, a1, pc->scratch, &q);
                    if (mq) stroke_run(pc, s, q, mq, a0, x, y, clip, hw, miter, false, head[0] && a0 <= cut[0] ? &cutn[0] : NULL,
                                       head[1] && a1 >= cut[1] ? &cutn[1] : NULL, &sp);
                }
                at = d1;
                k = (k + 1) % np;
            }
        } else {
            gp_point *q = p;
            size_t mq = m;
            if (head[0] || head[1]) mq = gp_curve_cut(p, arc, m, cut[0], cut[1], pc->scratch, &q);
            if (mq) stroke_run(pc, s, q, mq, cut[0], x, y, clip, hw, miter, s->closed, head[0] ? &cutn[0] : NULL, head[1] ? &cutn[1] : NULL, &sp);
        }
        for (int e = 0; e < 2; e++)
            if (head[e]) {
                const int8_t ed[3] = { 0, (int8_t)(e ? -2 : 2), 0 };   /* tip to corner, the base (shared with the stroke: at the start a pixel on the line is the head's, at the end the stroke's), corner to tip */
                push_triangle(pc, tri[e], ed, hw, x, y, clip, sp.grad ? ramp_at(sp.grad, (float)e) : sp.color, sp.opacity);
            }
        gp_marker_at *mk;   /* markers: over the stroke, in the same layer (so a translucent one is one blend with it) */
        size_t nmk = gp_curve_markers(n, p, m, pc->scratch, &mk);
        for (size_t i = 0; i < nmk; i++) {
            gp_color mc = sp.grad ? ramp_at(sp.grad, mk[i].t) : sp.color;
            gp_point t6[6];
            int8_t e6[6];
            if (gp_marker_tris(&mk[i], s->stroke_w, t6, e6)) {
                push_triangle(pc, t6, e6, 0.f, x, y, clip, mc, sp.opacity);
                push_triangle(pc, t6 + 3, e6 + 3, 0.f, x, y, clip, mc, sp.opacity);
            } else {   /* a circle: a round box */
                float r = mk[i].size * 0.5f;
                gp_instance inst = { 0 };
                set4(inst.bounds, x + mk[i].c.x - r, y + mk[i].c.y - r, 2 * r, 2 * r);
                set_clip(&inst, clip);
                set_color(inst.color, mc, sp.opacity);
                set4(inst.radii, r, r, r, r);
                inst.kind = GP_PRIM_QUAD;
                push(pc, &inst);
            }
        }
        if (layer_alpha < 1.f) layer_end(pc, layer_alpha);
    }
    if (s->path_text && n->ts.color.a > 0.f) paint_path_text(pc, n, p, arc, m, x, y, clip, opacity);
}

static void paint_text(paint_ctx *pc, gp_el *n, float x, float y, rect clip, float opacity) {
    const gp_style *s = &n->style;
    float bw = s->border_w;
    float cx = x + s->pad[0] + bw, cy = y + s->pad[1] + bw;
    float cw = n->size.w - (s->pad[0] + s->pad[2] + 2 * bw);

    gp_text_line *lines;
    int nlines;
    gp_text_measure_dir(pc->ts, GP_TEXT_SEL(n->ts), n->ts.size, n->text, cw, s->whitespace_pre, n->ts.dir,
                        pc->scratch, &lines, &nlines);

    float asc, desc;
    gp_text_metrics(pc->ts, GP_TEXT_SEL(n->ts), n->ts.size, &asc, &desc);
    float lh = n->ts.size * GP_LINE_HEIGHT;
    /* Measured, wrapped and shaped at the font's own size whatever it is painted under (a zoom reflows nothing); only the bitmaps are made for the size on the screen. */
    const text_raster tr = text_raster_for(pc, n->ts.size);
    const bool placed = pc->has_xf && tr.placed;   /* under a zoom and a move: each glyph goes to its place on the screen's own pixel grid */
    float scale = placed ? pc->scale : pc->scale * tr.k;   /* layout units to the pixels the bitmaps are made of */
    float px_size = tr.px;
    const float inv_alpha = 1.f / (float)GP_ATLAS_SIZE, inv_color = 1.f / (float)GP_COLOR_ATLAS_SIZE, inv_lcd = 1.f / (float)GP_LCD_ATLAS_SIZE;
    /* (the stripes of a turned or stretched glyph are not the screen's; in a layer there is nothing under the text to blend each channel with) */
    int lcd = (pc->has_xf && !placed) || pc->depth > 0 ? 0 : gp_text_lcd_order(pc->ts, pc->scale);

    for (int li = 0; li < nlines; li++) {
        const gp_text_line *ln = &lines[li];
        size_t len = ln->end - ln->start;
        if (len == 0) continue;
        /* The line's paragraph (up to the nearest line breaks) decides its direction, so every
         * wrapped line of a Hebrew paragraph is shaped right-to-left even if it holds only digits. */
        size_t ps = ln->start, pe = ln->start;
        while (ps > 0 && n->text[ps - 1] != '\n') ps--;
        while (n->text[pe] && n->text[pe] != '\n') pe++;
        bool rtl = gp_text_is_rtl(n->text + ps, pe - ps, n->ts.dir);
        int pdir = rtl ? GP_BIDI_RTL : GP_BIDI_LTR;

        /* a glyph per codepoint is the usual upper bound; ask for more if shaping disagrees */
        int cap = (int)len + 1;
        gp_glyph_pos *glyphs = gp_arena_alloc(pc->scratch, (size_t)cap * sizeof *glyphs, 8);
        float lw = 0;
        int ng = gp_text_shape_dir(pc->ts, GP_TEXT_SEL(n->ts), n->ts.size, n->text + ln->start, len, pdir,
                                   glyphs, cap, &lw);
        if (ng > cap) {
            cap = ng;
            glyphs = gp_arena_alloc(pc->scratch, (size_t)cap * sizeof *glyphs, 8);
            ng = gp_text_shape_dir(pc->ts, GP_TEXT_SEL(n->ts), n->ts.size, n->text + ln->start, len, pdir,
                                   glyphs, cap, &lw);
        }

        /* start / end are relative to the paragraph direction; left / right are not */
        int align = n->ts.align;
        if (align == GP_TEXT_ALIGN_START) align = rtl ? GP_TEXT_ALIGN_RIGHT : GP_TEXT_ALIGN_LEFT;
        else if (align == GP_TEXT_ALIGN_END) align = rtl ? GP_TEXT_ALIGN_LEFT : GP_TEXT_ALIGN_RIGHT;
        float line_x = cx;
        if (align == GP_TEXT_ALIGN_CENTER) line_x += (cw - lw) / 2;
        else if (align == GP_TEXT_ALIGN_RIGHT) line_x += cw - lw;
        float baseline = cy + (float)li * lh + (lh - (asc + desc)) / 2 + asc;

        for (int gi = 0; gi < ng; gi++) {
            int pixel, bin;
            float ox = line_x + glyphs[gi].x, oy = baseline + glyphs[gi].y;   /* the glyph's origin in the layout */
            if (placed) {   /* and where the transform puts it */
                float tx = pc->xf.a * ox + pc->xf.c * oy + pc->xf.tx, ty = pc->xf.b * ox + pc->xf.d * oy + pc->xf.ty;
                ox = tx;
                oy = ty;
            }
            gp_text_snap_x(pc->ts, glyphs[gi].font, px_size, ox * scale, &pixel, &bin);
            gp_glyph_entry e = gp_text_glyph_lcd(pc->ts, glyphs[gi].font, glyphs[gi].glyph, px_size, bin, lcd);
            if (e.w == 0) continue;
            float gx = (float)pixel + e.left;
            float gy = roundf(oy * scale) - e.top;
            gp_instance inst = { 0 };
            set4(inst.bounds, gx / scale, gy / scale, e.w / scale, e.h / scale);
            set_clip(&inst, clip);
            float inv = e.color ? inv_color : e.lcd ? inv_lcd : inv_alpha;
            set4(inst.uv, e.u * inv, e.v * inv, e.w * inv, e.h * inv);
            if (e.color) { /* already RGBA: only the opacity applies, never the text color */
                set_color(inst.color, (gp_color){ 1, 1, 1, 1 }, opacity);
                inst.kind = GP_PRIM_COLOR_GLYPH;
            } else {
                set_color(inst.color, n->ts.color, opacity);
                inst.kind = e.lcd ? GP_PRIM_LCD_GLYPH : GP_PRIM_GLYPH;
            }
            if (placed) push_placed(pc, &inst);
            else push(pc, &inst);
        }
    }
}

static void border_widths(const gp_style *s, float w4[4]);
static void corner_radii(const gp_style *s, float w, float h, float r[4]);

/* An image: inside padding and border, scaled the way object-fit says and centered; the part of it that falls outside
 * the content box is cropped by choosing the texture rectangle, so what is drawn never leaves the box. The corners follow
 * the element's radius (less its border). */
static void paint_image(paint_ctx *pc, gp_el *n, float x, float y, rect clip, float opacity) {
    const gp_style *s = &n->style;
    gp_gfx *gfx = gp_text_gfx(pc->ts);
    gp_image handle = n->image;
    float iw, ih;
    if (n->kind == GP_EL_SVG) {
        if (!gp_gfx_svg_size(gfx, n->svg, &iw, &ih)) return;
    } else {
        gp_gfx_image *im = gp_gfx_image_get(gfx, n->image.index, n->image.generation);
        if (!im || im->w <= 0 || im->h <= 0) return;
        iw = (float)im->w;
        ih = (float)im->h;
    }
    float bw = s->border_w;
    float cx = x + s->pad[0] + bw, cy = y + s->pad[1] + bw;
    float cw = n->size.w - (s->pad[0] + s->pad[2] + 2 * bw), ch = n->size.h - (s->pad[1] + s->pad[3] + 2 * bw);
    if (cw <= 0 || ch <= 0) return;
    float sw, sh;
    int fit = s->object_fit;
    if (fit == GP_FIT_SCALE_DOWN) fit = iw <= cw && ih <= ch ? GP_FIT_NONE : GP_FIT_CONTAIN;
    switch (fit) {
    case GP_FIT_CONTAIN: { float k = fminf(cw / iw, ch / ih); sw = iw * k; sh = ih * k; break; }
    case GP_FIT_COVER: { float k = fmaxf(cw / iw, ch / ih); sw = iw * k; sh = ih * k; break; }
    case GP_FIT_NONE: sw = iw; sh = ih; break;
    default: sw = cw; sh = ch; break;   /* FILL */
    }
    float rx = cx + (cw - sw) / 2, ry = cy + (ch - sh) / 2;                 /* where the whole picture would be */
    float vx0 = fmaxf(rx, cx), vy0 = fmaxf(ry, cy), vx1 = fminf(rx + sw, cx + cw), vy1 = fminf(ry + sh, cy + ch);   /* what shows of it */
    if (vx1 <= vx0 || vy1 <= vy0 || sw <= 0 || sh <= 0) return;
    if (n->kind == GP_EL_SVG) {   /* a drawing is rasterized for the size it is shown at, in device pixels, in the color `currentColor` is */
        float k = fmaxf(sw * pc->scale / iw, sh * pc->scale / ih);
        int rw = (int)ceilf(iw * k - 0.001f);
        if (rw < 1) rw = 1;
        if (rw > GP_SVG_MAX_RASTER) rw = GP_SVG_MAX_RASTER;
        const gp_color tc = n->ts.color;
        uint32_t rgb = (uint32_t)lroundf(fminf(fmaxf(tc.r, 0.f), 1.f) * 255.f) << 16 | (uint32_t)lroundf(fminf(fmaxf(tc.g, 0.f), 1.f) * 255.f) << 8 |
                       (uint32_t)lroundf(fminf(fmaxf(tc.b, 0.f), 1.f) * 255.f);
        handle = gp_gfx_svg_raster(gfx, n->svg, rw, rgb);
        if (!handle.generation) return;
    }
    gp_instance inst = { 0 };
    set4(inst.bounds, vx0, vy0, vx1 - vx0, vy1 - vy0);
    set_clip(&inst, clip);
    set4(inst.color, 1, 1, 1, opacity);
    float r4[4], b4[4], dest_half = fminf(vx1 - vx0, vy1 - vy0) / 2;
    corner_radii(s, n->size.w, n->size.h, r4);
    border_widths(s, b4);
    set4(inst.radii, fminf(fmaxf(r4[0] - (b4[0] + b4[1]) * 0.5f, 0.f), dest_half), fminf(fmaxf(r4[1] - (b4[2] + b4[1]) * 0.5f, 0.f), dest_half),
         fminf(fmaxf(r4[2] - (b4[2] + b4[3]) * 0.5f, 0.f), dest_half), fminf(fmaxf(r4[3] - (b4[0] + b4[3]) * 0.5f, 0.f), dest_half));
    set4(inst.uv, (vx0 - rx) / sw, (vy0 - ry) / sh, (vx1 - vx0) / sw, (vy1 - vy0) / sh);
    inst.kind = GP_PRIM_IMAGE;
    inst._pad = (float)handle.index;                   /* which image: slot and generation */
    inst.border_color[0] = (float)handle.generation;
    inst.blur = s->img_blur + (s->image_pixelated ? 1000.f : 0.f);   /* the blur radius; 1000 more if pixelated */
    inst.border_color[1] = s->img_grayscale;
    inst.border_color[2] = s->img_brightness;
    inst.border_color[3] = s->img_saturate;
    inst.border_w = s->img_contrast;
    if (s->has_img_tint) set4(inst.color, s->img_tint.r, s->img_tint.g, s->img_tint.b, opacity);
    push(pc, &inst);
}

static void paint_scrollbars(paint_ctx *pc, gp_el *n, float x, float y, rect clip, float opacity) {
    gp_color base = n->style.has_scrollbar_color ? n->style.scrollbar_color
                                                 : (gp_color){ 0.5f, 0.5f, 0.5f, 0.5f };
    for (int axis = 0; axis < 2; axis++) {
        gp_thumb t;
        if (!gp_scroll_thumb(n, axis, x, y, &t)) continue;
        /* under the mouse only where it can be seen: a bar clipped away is not touched (nor can it be grabbed) */
        bool active = pc->drag_key == n->scroll_key ||
                      (rect_has(clip, pc->mouse) && rect_has((rect){ t.x, t.y, t.x + t.w, t.y + t.h }, pc->mouse_local));
        gp_color c = base;
        if (active) c.a = fminf(1.f, c.a * 1.6f);
        if (n->style.autohide_scrollbar) c.a *= n->thumb_alpha;
        if (c.a <= 0.003f) continue;
        gp_instance inst = { 0 };
        set4(inst.bounds, t.x, t.y, t.w, t.h);
        set_clip(&inst, clip);
        set_color(inst.color, c, opacity);
        float r = fminf(t.w, t.h) / 2;
        set4(inst.radii, r, r, r, r);
        inst.kind = GP_PRIM_QUAD;
        push(pc, &inst);
    }
}

/* The border's width on each side (left, top, right, bottom). */
static void border_widths(const gp_style *s, float w4[4]) {
    for (int i = 0; i < 4; i++) {
        float v = s->has_border_sides ? ((s->border_set >> i & 1) ? s->border_side[i] : s->border_uniform) : s->border_w;
        w4[i] = v > 0.f ? v : 0.f;
    }
}

/* The corner radii (top-left, top-right, bottom-right, bottom-left), scaled down together where neighbors would overlap (CSS). */
static void corner_radii(const gp_style *s, float w, float h, float r[4]) {
    for (int i = 0; i < 4; i++) {
        r[i] = (s->corner_set >> i & 1) ? s->corner[i] : s->radius;
        if (r[i] < 0.f) r[i] = 0.f;
    }
    float f = 1.f;
    if (r[0] + r[1] > 0.f) f = fminf(f, w / (r[0] + r[1]));
    if (r[3] + r[2] > 0.f) f = fminf(f, w / (r[3] + r[2]));
    if (r[0] + r[3] > 0.f) f = fminf(f, h / (r[0] + r[3]));
    if (r[1] + r[2] > 0.f) f = fminf(f, h / (r[1] + r[2]));
    if (f < 1.f)
        for (int i = 0; i < 4; i++) r[i] *= f;
}

static void paint_node_inner(paint_ctx *pc, gp_el *n, float ox, float oy, rect clip, float opacity);

/* A cached view's subtree is painted again only if something it was painted for has changed: the layout of
 * the subtree (it was not laid out again), where it is, the clip and opacity it inherits, the scale, the
 * glyph atlas (cached instances hold atlas coordinates), and, if it has focusable elements, which element
 * has focus. If it has hover styles or scrollbars, a mouse over it is a reason to paint in full, and what is
 * painted then is not kept: a thumb lights up under the mouse, and a row under it takes its hover color.
 * "Over it" means over anything below it that can paint differently, which may reach outside the root's own box
 * (gp_el.ext). Scrollers and lists move in place, without a layout: that is dyn_gen, and a thumb being
 * dragged is drag_key. */
static void paint_node(paint_ctx *pc, gp_el *n, float ox, float oy, rect clip, float opacity) {
    if (!pc->views || !n->cached_view.generation || n->prep_valid == false) {
        paint_node_inner(pc, n, ox, oy, clip, opacity);
        return;
    }
    gp_vview *vv = gp_views_find(pc->views, n->cached_view);
    if (!vv || vv->root != n) {
        paint_node_inner(pc, n, ox, oy, clip, opacity);
        return;
    }
    float x = ox + n->pos.x, y = oy + n->pos.y;
    bool by_hover = false;
    if (n->sub_flags & (GP_SUB_HOVER | GP_SUB_SCROLL)) {
        rect reach = { x + n->ext[0], y + n->ext[1], x + n->ext[2], y + n->ext[3] };
        bool over = rect_has(reach, pc->mouse_local);
        /* a scrollbar under the mouse lights up whether or not the mouse is in the window (its last place counts) */
        by_hover = over && ((n->sub_flags & GP_SUB_SCROLL) || pc->mouse_in);
    }
    float vals[8] = { 0, 0, clip.x0, clip.y0, clip.x1, clip.y1, opacity, pc->scale };   /* (where it is is kept apart: see below) */
    uint32_t sig[32] = { 0 };
    memcpy(sig, vals, sizeof vals);
    if (pc->has_xf) {   /* what it is painted under: the eight numbers (not the struct: its padding is not set) */
        const float xv[9] = { pc->xf.a, pc->xf.b, pc->xf.c, pc->xf.d, pc->xf.tx, pc->xf.ty, pc->xf.g, pc->xf.h, pc->xf.w1 };
        memcpy(sig + 16, xv, sizeof xv);
    }
    sig[8] = pc->atlas_gen;
    if (pc->has_tc) {   /* a text color from a state style above (to the 8 bits a channel it is drawn with) */
        const float *c = &pc->tc.r;
        for (int i = 0; i < 4; i++) sig[26] = sig[26] << 8 | (uint32_t)lroundf(fminf(fmaxf(c[i], 0.f), 1.f) * 255.f);
        sig[27] = 1;
    }
    if (n->sub_flags & GP_SUB_FOCUS) {   /* only subtrees that can show focus depend on it */
        sig[9] = (uint32_t)pc->focused;
        sig[10] = (uint32_t)(pc->focused >> 32);
    }
    if (n->sub_flags & GP_SUB_SCROLL) {
        sig[11] = n->dyn_gen;
        sig[12] = (uint32_t)pc->drag_key;
        sig[13] = (uint32_t)(pc->drag_key >> 32);
    }
    if (n->sub_flags & GP_SUB_AUTOHIDE) sig[14] = pc->vis_gen;
    sig[15] = pc->depth > 0;   /* painted into a layer: text there is not LCD-antialiased */   /* a scrollbar below faded in or out */
    if (!by_hover && vv->chunk_valid && vv->chunk_gen == n->layout_gen && memcmp(sig, vv->chunk_sig, sizeof sig) == 0) {
        /* Where it is: the same, or moved by whole device pixels with nothing below that is clipped to a box of its own (an overflow, a scroller, a list:
         * their clip moves with the subtree and is not the clip it inherits), which is the same instances moved. A row of a list that scrolls is painted
         * once and then only shifted, whatever the offset (the offset is whole pixels while it is dragged or wheeled; a smooth scroll, which is not, repaints). */
        float dx = x - vv->chunk_x, dy = y - vv->chunk_y;
        bool same_place = dx == 0.f && dy == 0.f;
        float sx = dx * pc->scale, sy = dy * pc->scale;
        bool shiftable = !same_place && !(n->sub_flags & (GP_SUB_CLIP | GP_SUB_SCROLL | GP_SUB_LIST | GP_SUB_XF)) && !pc->has_xf &&
                         fabsf(sx - roundf(sx)) < 1e-3f && fabsf(sy - roundf(sy)) < 1e-3f && fabsf(dx) < 1e6f && fabsf(dy) < 1e6f;
        if (same_place || shiftable) {
            if (shiftable) {
                for (size_t i = 0; i < vv->nchunk; i++) {
                    vv->chunk[i].bounds[0] += dx;
                    vv->chunk[i].bounds[1] += dy;
                }
                vv->chunk_x = x;
                vv->chunk_y = y;
                if (pc->app) pc->app->stat_paint_shifts++;
            }
            for (size_t i = 0; i < vv->nchunk; i++) scene_add(pc, &vv->chunk[i]);
            if (pc->app) pc->app->stat_paint_hits++;
            return;
        }
    }
    size_t start = pc->scene->len;
    paint_node_inner(pc, n, ox, oy, clip, opacity);
    if (pc->app) pc->app->stat_paint_misses++;
    if (by_hover) return;
    vv = gp_views_find(pc->views, n->cached_view);   /* the table does not move while painting, but do not assume it */
    if (!vv) return;
    size_t count = pc->scene->len - start;
    if (count > vv->cap_chunk) {
        vv->cap_chunk = count;
        vv->chunk = realloc(vv->chunk, vv->cap_chunk * sizeof *vv->chunk);
        if (!vv->chunk) abort();
    }
    if (count) memcpy(vv->chunk, pc->scene->items + start, count * sizeof *vv->chunk);   /* (a view that paints nothing may have no copy yet, nor the scene any items: NULL, even for 0 bytes, is undefined) */
    vv->nchunk = count;
    memcpy(vv->chunk_sig, sig, sizeof sig);
    vv->chunk_x = x;
    vv->chunk_y = y;
    vv->chunk_gen = n->layout_gen;
    vv->chunk_valid = true;
}

/* What a state style (gp_hover_style, gp_active_style, gp_focus_style) changes: the paint-only properties it sets (background, border
 * colors, style and gradient, corner radii, shadow, opacity, text color, filters, blend mode), put over `dst`. Anything that would move or resize the element is not here. */
static void overlay_style(gp_style *dst, const gp_style *src) {
    if (src->has_bg) { dst->bg = src->bg; dst->has_bg = true; dst->bg_gradient = NULL; }
    if (src->has_stroke_color) { dst->stroke_color = src->stroke_color; dst->has_stroke_color = true; }
    if (src->has_text_color) { dst->text_color = src->text_color; dst->has_text_color = true; }
    if (src->bg_gradient) dst->bg_gradient = src->bg_gradient;
    if (src->border_color.a > 0) { dst->border_color = src->border_color; dst->bcolor_set = 0; dst->border_gradient = NULL; }
    for (int i = 0; i < 4; i++)
        if (src->bcolor_set & (1 << i)) { dst->bcolor[i] = src->bcolor[i]; dst->bcolor_set |= (uint8_t)(1 << i); dst->border_gradient = NULL; }
    if (src->border_gradient) dst->border_gradient = src->border_gradient;
    if (src->border_style) dst->border_style = src->border_style;
    if (src->has_shadow) { dst->shadow = src->shadow; dst->has_shadow = true; }
    if (src->opacity != 1.f) dst->opacity = src->opacity;
    if (src->radius > 0.f) { dst->radius = src->radius; dst->corner_set = 0; }
    for (int i = 0; i < 4; i++)
        if (src->corner_set & (1 << i)) { dst->corner[i] = src->corner[i]; dst->corner_set |= (uint8_t)(1 << i); }
    if (src->nfilters) { dst->filters = src->filters; dst->nfilters = src->nfilters; }
    if (src->blend_mode) dst->blend_mode = src->blend_mode;
}

/* Whether an element's opacity needs a layer: what it paints could overlap (children, a box under text, a shadow under a box, a fill under a stroke) */
static bool wants_group(const gp_el *n, const gp_style *s) {
    if (n->nchildren || n->sticky) return true;
    if (n->kind == GP_EL_CURVE) return (s->has_fill && s->fill_color.a > 0.f) || s->path_text;   /* (a stroke alone takes its own layer) */
    bool box = (s->has_bg && s->bg.a > 0.f) || s->bg_gradient || s->border_w > 0.f || s->has_border_sides || s->has_hover_bg || s->has_active_bg || s->has_focus_bg;
    bool shadow = s->has_shadow && s->shadow.color.a > 0.f;
    bool content = n->kind == GP_EL_TEXT || n->kind == GP_EL_IMAGE || n->kind == GP_EL_SVG;
    bool scrolls = s->overflow_x_scroll || s->overflow_y_scroll;
    return (shadow && (box || content)) || (box && (content || scrolls));
}

static gp_lstate_set *g_collect;
void gp_paint_collect_states(gp_lstate_set *out) {
    g_collect = out;
    if (out) out->len = 0;
}

static void paint_node_body(paint_ctx *pc, gp_el *n, float ox, float oy, rect clip, float opacity) {
    const gp_style *s = &n->style;
    float x = ox + n->pos.x, y = oy + n->pos.y;
    float w = n->size.w, h = n->size.h;

    rect bounds = { x, y, x + w, y + h };
    bool hovered = pc->mouse_in && rect_has(bounds, pc->mouse_local) && rect_has(clip, pc->mouse);
    if (hovered && n->kind == GP_EL_CURVE) {   /* a stroke is hovered on the stroke, not in its box (looked at only if something depends on it) */
        const gp_style *cs = &n->style;
        hovered = (cs->has_hover_bg || cs->has_active_bg || cs->hover_style || cs->active_style) && gp_curve_hit(n, pc->mouse_local.x - x, pc->mouse_local.y - y);
    }
    bool focused = n->focus_id && n->focus_id == pc->focused;
    if (n->lstates && g_collect) {   /* a state style that lays out: what layout is to lay it out in */
        uint8_t bits = (uint8_t)((hovered ? GP_LSTATE_HOVER : 0) | (hovered && pc->pressed ? GP_LSTATE_ACTIVE : 0) | (focused ? GP_LSTATE_FOCUS : 0)) & n->lstates;
        if (bits) {
            gp_lstate_set *o = g_collect;
            if (o->len == o->cap) {
                o->cap = o->cap ? o->cap * 2 : 16;
                o->items = realloc(o->items, o->cap * sizeof *o->items);
                if (!o->items) abort();
            }
            o->items[o->len++] = (gp_lstate){ n->path, bits };
        }
    }
    gp_style stated;
    if ((hovered && (s->hover_style || (pc->pressed && s->active_style))) || (focused && s->focus_style)) {
        stated = *s;
        if (hovered && s->hover_style) overlay_style(&stated, s->hover_style);
        if (hovered && pc->pressed && s->active_style) overlay_style(&stated, s->active_style);
        if (focused && s->focus_style) overlay_style(&stated, s->focus_style);
        s = &stated;
    }
    /* the text color: an element's own stops what a state style above it set; a state style of its own sets it for it and everything in it (the caller puts both back) */
    if (n->style.has_text_color) pc->has_tc = false;
    if (s == &stated && s->has_text_color) { pc->tc = s->text_color; pc->has_tc = true; }
    if (pc->has_tc) n->ts.color = pc->tc;
    /* An opacity fades the element and what is in it as one thing (CSS's group opacity): painted into a layer that is put down at it, when
     * there is more than one thing to paint that could overlap; else it is put on the one instance, which is the same and costs nothing. */
    float group = 1.f;
    bool layered = false, effects = s->nfilters || s->blend_mode || n->mask;   /* (these always take a layer) */
    if (s->opacity <= 0.f || opacity <= 0.f) return;
    if (s->nbackdrop && w > 0.f && h > 0.f) {   /* what is behind, filtered, in the box: before the element's own layer (it reads what is under that) */
        float r4b[4];
        corner_radii(s, w, h, r4b);
        gp_instance box = { 0 };
        set4(box.bounds, x, y, w, h);
        set_clip(&box, clip);
        set4(box.radii, r4b[0], r4b[1], r4b[2], r4b[3]);
        set_color(box.color, (gp_color){ 1, 1, 1, 1 }, opacity * s->opacity);
        box.kind = GP_PRIM_BACKDROP_END;
        if (pc->has_xf) {   /* (as push() would: for where it is on screen) */
            const gp_affine *mx = &pc->xf;
            box.xf0[0] = mx->a - 1.f; box.xf0[1] = mx->b; box.xf0[2] = mx->c; box.xf0[3] = mx->d - 1.f;
            box.xf1[0] = mx->tx; box.xf1[1] = mx->ty; box.xf1[2] = mx->g; box.xf1[3] = mx->h;
            box.xf2[0] = mx->w1;
        }
        float L[4];
        inst_screen_box(&box, L);
        filters_spread(s->backdrop, s->nbackdrop, L);
        float sc = pc->scale, lim = 1e6f;
        float region[4] = { floorf(fmaxf(L[0], -lim) * sc) / sc, floorf(fmaxf(L[1], -lim) * sc) / sc, 0, 0 };
        region[2] = ceilf(fminf(L[2], lim) * sc) / sc - region[0];
        region[3] = ceilf(fminf(L[3], lim) * sc) / sc - region[1];
        if (region[2] > 0.f && region[3] > 0.f) {
            gp_instance b = { 0 };
            b.kind = GP_PRIM_BACKDROP;
            memcpy(b.bounds, region, sizeof region);
            memcpy(b.clip, region, sizeof region);
            gp_scene_push(pc->scene, &b);
            emit_passes(pc, s->backdrop, s->nbackdrop, region);
            scene_add(pc, &box);
        }
    }
    if ((effects || (s->opacity < 1.f && wants_group(n, s))) && layer_begin(pc)) {
        layered = true;
        group = s->opacity;
    } else {
        opacity *= s->opacity;
    }
    if (n->kind == GP_EL_CURVE) {
        gp_color c = s->has_stroke_color ? s->stroke_color : n->ts.color;
        if (hovered && pc->pressed && s->has_active_bg) c = s->active_bg;
        else if (hovered && s->has_hover_bg) c = s->hover_bg;
        if (focused && s->has_focus_bg && !(hovered && s->has_hover_bg)) c = s->focus_bg;
        paint_curve(pc, n, x, y, clip, opacity, c, (hovered && pc->pressed && s->has_active_bg) || (hovered && s->has_hover_bg) || (focused && s->has_focus_bg));
        goto done;
    }
    float r4[4];
    corner_radii(s, w, h, r4);

    if (s->has_shadow && s->shadow.color.a > 0) {
        gp_instance inst = { 0 };
        set4(inst.bounds, x + s->shadow.dx, y + s->shadow.dy, w, h);
        set_clip(&inst, clip);
        set_color(inst.color, s->shadow.color, opacity);
        set4(inst.radii, r4[0], r4[1], r4[2], r4[3]);
        inst.blur = s->shadow.blur;
        inst.kind = GP_PRIM_SHADOW;
        push(pc, &inst);
    }

    gp_color bg = s->bg;
    bool has_bg = s->has_bg;
    if (hovered && pc->pressed && s->has_active_bg) { bg = s->active_bg; has_bg = true; }
    else if (hovered && s->has_hover_bg) { bg = s->hover_bg; has_bg = true; }

    gp_color border = s->border_color;
    bool has_border_color = s->border_color.a > 0;
    if (focused && s->has_focus_border) { border = s->focus_border; has_border_color = true; }
    if (focused && s->has_focus_bg && !(hovered && s->has_hover_bg)) { bg = s->focus_bg; has_bg = true; }

    float b4[4];
    border_widths(s, b4);
    /* the border: a color (one for all sides, or some of its own) or a gradient; a focus border color replaces both */
    bool focus_ring = focused && s->has_focus_border;
    const gp_gradient *bgrad = !focus_ring && s->border_gradient && s->border_gradient->n > 0 ? s->border_gradient : NULL;
    bool sided = !focus_ring && !bgrad && s->bcolor_set != 0;
    gp_color side_c[4];
    for (int i = 0; i < 4; i++) side_c[i] = sided && (s->bcolor_set & (1 << i)) ? s->bcolor[i] : border;
    bool any_side_color = false;
    for (int i = 0; i < 4; i++) any_side_color |= side_c[i].a > 0;
    bool has_border = (b4[0] > 0.f || b4[1] > 0.f || b4[2] > 0.f || b4[3] > 0.f) && (bgrad ? true : sided ? any_side_color : has_border_color);
    /* a gradient is the background unless a state (hover, press, focus) asks for a color of its own */
    bool recolored = (hovered && pc->pressed && s->has_active_bg) || (hovered && s->has_hover_bg) || (focused && s->has_focus_bg);
    const gp_gradient *grad = s->bg_gradient && s->bg_gradient->n > 0 && !recolored ? s->bg_gradient : NULL;
    if ((has_bg && bg.a > 0) || has_border || grad) {
        gp_instance inst = { 0 };
        set4(inst.bounds, x, y, w, h);
        set_clip(&inst, clip);
        gp_gfx *gfx = gp_text_gfx(pc->ts);
        /* a repeating gradient's ramp is one period: the stops stretched to 0..1, with the period's start and 1 / length for the shader */
        gp_gradient period;
        float first = 0.f, inv_span = 0.f;
        if (grad && grad->repeat && grad->kind != GP_GRAD_CONIC && grad->n >= 2 && grad->stops[grad->n - 1].at - grad->stops[0].at > 1e-4f) {
            first = grad->stops[0].at;
            inv_span = 1.f / (grad->stops[grad->n - 1].at - first);
            period = *grad;
            for (int i = 0; i < period.n; i++) period.stops[i].at = (period.stops[i].at - first) * inv_span;
            grad = &period;
        }
        int row = grad && gfx ? gp_gfx_gradient_row(gfx, grad) : -1;
        if (grad && row < 0) {   /* the ramp table is full: this frame is repainted after it is cleared; for now, the first color */
            bg = grad->stops[0].color;
            has_bg = true;
            grad = NULL;
        }
        if (grad) {
            if (grad->kind == GP_GRAD_LINEAR) {
                float a = grad->angle * (float)M_PI / 180.f, sn = sinf(a), cs = cosf(a);
                float len = fabsf(w * sn) + fabsf(h * cs);
                set4(inst.color, sn, -cs, len > 1e-3f ? len : 1.f, opacity);
                inst.kind = GP_PRIM_LINEAR_GRADIENT;
            } else if (grad->kind == GP_GRAD_CONIC) {
                set4(inst.color, (grad->cx - 0.5f) * w, (grad->cy - 0.5f) * h, grad->angle * (float)M_PI / 180.f, opacity);
                inst.kind = GP_PRIM_CONIC_GRADIENT;
            } else {
                float radius_to_corner = hypotf(fmaxf(grad->cx, 1.f - grad->cx) * w, fmaxf(grad->cy, 1.f - grad->cy) * h);
                set4(inst.color, (grad->cx - 0.5f) * w, (grad->cy - 0.5f) * h, radius_to_corner > 1e-3f ? radius_to_corner : 1.f, opacity);
                inst.kind = GP_PRIM_RADIAL_GRADIENT;
            }
            inst.uv[0] = ((float)row + 0.5f) / (float)GP_GRAD_ROWS;
            inst.blur = inv_span;   /* 0: not repeating */
            inst._pad = first;
        } else if (has_bg) {
            set_color(inst.color, bg, opacity);
        }
        if (has_border) {
            set_color(inst.border_color, sided ? side_c[0] : border, opacity);
            inst.bsty[0] = (float)s->border_style;
            if (bgrad) {   /* painted with a gradient: its own row, and its geometry over the box (as a background's) in the first side slot */
                gp_gfx *bg_gfx = gp_text_gfx(pc->ts);
                int brow = bg_gfx ? gp_gfx_gradient_row(bg_gfx, bgrad) : -1;
                if (brow >= 0) {
                    if (bgrad->kind == GP_GRAD_LINEAR) {
                        float a = bgrad->angle * (float)M_PI / 180.f, sn = sinf(a), cs = cosf(a);
                        float len = fabsf(w * sn) + fabsf(h * cs);
                        set4(inst.side[0], sn, -cs, len > 1e-3f ? len : 1.f, opacity);
                        inst.bsty[3] = 5.f;
                    } else if (bgrad->kind == GP_GRAD_CONIC) {
                        set4(inst.side[0], (bgrad->cx - 0.5f) * w, (bgrad->cy - 0.5f) * h, bgrad->angle * (float)M_PI / 180.f, opacity);
                        inst.bsty[3] = 8.f;
                    } else {
                        float rc = hypotf(fmaxf(bgrad->cx, 1.f - bgrad->cx) * w, fmaxf(bgrad->cy, 1.f - bgrad->cy) * h);
                        set4(inst.side[0], (bgrad->cx - 0.5f) * w, (bgrad->cy - 0.5f) * h, rc > 1e-3f ? rc : 1.f, opacity);
                        inst.bsty[3] = 6.f;
                    }
                    inst.bsty[2] = ((float)brow + 0.5f) / (float)GP_GRAD_ROWS;
                } else {   /* the ramp table is full: repainted after it is cleared; for now the first color */
                    set_color(inst.border_color, bgrad->stops[0].color, opacity);
                }
            } else if (sided) {
                set_color(inst.side[0], side_c[1], opacity);
                set_color(inst.side[1], side_c[2], opacity);
                set_color(inst.side[2], side_c[3], opacity);
                inst.bsty[1] = 1.f;
            }
        }
        set4(inst.radii, r4[0], r4[1], r4[2], r4[3]);
        if (has_border) {   /* the widths: left in border_w, then top, right, bottom in the uv (a quad has no texture) */
            inst.border_w = b4[0];
            inst.uv[1] = b4[1];
            inst.uv[2] = b4[2];
            inst.uv[3] = b4[3];
        }
        if (!grad) inst.kind = GP_PRIM_QUAD;
        push(pc, &inst);
    }

    if (n->kind == GP_EL_TEXT) {
        paint_text(pc, n, x, y, clip, opacity);
        goto done;
    }
    if (n->kind == GP_EL_IMAGE || n->kind == GP_EL_SVG) {
        paint_image(pc, n, x, y, clip, opacity);
        goto done;
    }

    bool scrolls = s->overflow_x_scroll || s->overflow_y_scroll;
    rect child_clip = clip;
    if (s->overflow_hidden || scrolls) {
        if (!pc->has_xf) child_clip = rect_isect(clip, bounds);
        else {   /* the box as it is on screen: cut to it if the transform keeps the axes, else not cut (a rotated clip is not made) */
            rect on_screen;
            if (aff_rect(pc->xf, bounds, &on_screen)) child_clip = rect_isect(clip, on_screen);
        }
    }
    for (uint32_t i = 0; i < n->nchildren; i++)
        paint_node(pc, n->children[i], x - n->scroll.x, y - n->scroll.y, child_clip, opacity);
    if (n->sticky) paint_node(pc, n->sticky, x, y, child_clip, opacity);   /* (a list's pinned header: not scrolled, above the rows) */
    if (scrolls) paint_scrollbars(pc, n, x, y, child_clip, opacity);
done:
    if (layered) {
        layer_fx fx = { s, n->mask, x, y, clip };
        layer_end_fx(pc, group, &fx);
    }
}

/* An element with a transform paints itself and everything in it under it: the pointer is taken back through it for what hover looks at. */
/* The body, with what it changes for its subtree put back after: the text color a state style set (and the element's own copy of it, which only painting reads). */
static void paint_node_scoped(paint_ctx *pc, gp_el *n, float ox, float oy, rect clip, float opacity) {
    const gp_color keep = n->ts.color, tc = pc->tc;
    const bool has_tc = pc->has_tc;
    paint_node_body(pc, n, ox, oy, clip, opacity);
    n->ts.color = keep;
    pc->tc = tc;
    pc->has_tc = has_tc;
}

static void paint_node_inner(paint_ctx *pc, gp_el *n, float ox, float oy, rect clip, float opacity) {
    if (!n->style.has_xf) {
        paint_node_scoped(pc, n, ox, oy, clip, opacity);
        return;
    }
    paint_ctx saved = *pc;
    gp_affine mine = gp_el_affine(n, ox + n->pos.x, oy + n->pos.y);
    gp_affine total = gp_aff_mul(pc->has_xf ? pc->xf : gp_aff_identity(), mine);
    if (gp_el_behind(n, total, ox + n->pos.x, oy + n->pos.y)) return;   /* at or behind the viewer: nothing of it is seen */
    if (n->style.backface_hidden && gp_el_backfacing(n, total, ox + n->pos.x, oy + n->pos.y)) return;   /* turned away: not drawn (gp_backface_hidden) */
    pc->xf = total;
    pc->has_xf = true;
    pc->inv = gp_aff_inverse(pc->xf);
    pc->mouse_local = gp_aff_apply(pc->inv, pc->mouse);
    paint_node_scoped(pc, n, ox, oy, clip, opacity);
    pc->xf = saved.xf;
    pc->inv = saved.inv;
    pc->has_xf = saved.has_xf;
    pc->mouse_local = saved.mouse_local;
}

void gp_paint_tree(gp_scene *scene, gp_arena *scratch, gp_text_system *ts, gp_el *root,
                   float scale, gp_point mouse, bool mouse_in, bool pressed, uint64_t focused,
                   uint64_t drag_key, uint32_t vis_gen, gp_vviews *views, gp_app *app) {
    gp_scene_clear(scene);
    gp_arena_reset(scratch);
    if (!root) return;
    paint_ctx pc = { .scene = scene, .scratch = scratch, .ts = ts, .scale = scale, .mouse = mouse, .mouse_local = mouse, .xf = gp_aff_identity(), .inv = gp_aff_identity(),
                     .mouse_in = mouse_in, .pressed = pressed, .focused = focused, .drag_key = drag_key,
                     .views = gp_frame_caches_enabled() ? views : NULL, .app = app, .atlas_gen = gp_text_atlas_generation(ts), .vis_gen = vis_gen };
    rect everything = { -1e9f, -1e9f, 1e9f, 1e9f };
    paint_node(&pc, root, 0, 0, everything, 1.f);
}
