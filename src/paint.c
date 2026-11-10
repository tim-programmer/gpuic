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

/* What a transformed element does to everything in it, about the point (ox, oy) of its box at layout position (x, y): scale, rotate, move. */
gp_affine gp_style_affine(const gp_style *s, float x, float y, float w, float h) {
    float cx = x + w * s->xf_ox, cy = y + h * s->xf_oy;
    float r = s->xf_rot * (float)M_PI / 180.f, cs = cosf(r), sn = sinf(r);
    if (fmodf(s->xf_rot, 90.f) == 0.f) {   /* quarter turns exactly, not nearly */
        int q = ((int)lroundf(s->xf_rot / 90.f) % 4 + 4) % 4;
        cs = q == 0 ? 1.f : q == 2 ? -1.f : 0.f;
        sn = q == 1 ? 1.f : q == 3 ? -1.f : 0.f;
    }
    gp_affine m = { cs * s->xf_sx, sn * s->xf_sx, -sn * s->xf_sy, cs * s->xf_sy, 0, 0 };
    if (s->xf_kx != 0.f || s->xf_ky != 0.f) {   /* a skew first: x += tan(ax) y, y += tan(ay) x (beyond 89 degrees it is held there: a tangent that large is a line) */
        float kx = tanf(fminf(fmaxf(s->xf_kx, -89.f), 89.f) * (float)M_PI / 180.f), ky = tanf(fminf(fmaxf(s->xf_ky, -89.f), 89.f) * (float)M_PI / 180.f);
        gp_affine k = { 1, ky, kx, 1, 0, 0 };
        m = gp_aff_mul(m, k);
    }
    m.tx = cx + s->xf_tx - (m.a * cx + m.c * cy);
    m.ty = cy + s->xf_ty - (m.b * cx + m.d * cy);
    return m;
}

/* a rectangle through a transform that keeps the axes (a scale, a move, a quarter turn): the box it becomes; false for any other */
static bool aff_rect(gp_affine m, rect r, rect *out) {
    bool straight = (fabsf(m.b) < 1e-5f && fabsf(m.c) < 1e-5f) || (fabsf(m.a) < 1e-5f && fabsf(m.d) < 1e-5f);
    if (!straight) return false;
    gp_point p0 = gp_aff_apply(m, (gp_point){ r.x0, r.y0 }), p1 = gp_aff_apply(m, (gp_point){ r.x1, r.y1 });
    out->x0 = fminf(p0.x, p1.x); out->x1 = fmaxf(p0.x, p1.x);
    out->y0 = fminf(p0.y, p1.y); out->y1 = fmaxf(p0.y, p1.y);
    return true;
}

/* every instance carries the transform it was painted under (all zeros: none) */
static void push(paint_ctx *pc, gp_instance *inst) {
    if (pc->has_xf) {
        const gp_affine *m = &pc->xf;
        float mag = sqrtf(fabsf(m->a * m->d - m->b * m->c));
        inst->xf0[0] = m->a - 1.f; inst->xf0[1] = m->b; inst->xf0[2] = m->c; inst->xf0[3] = m->d - 1.f;
        inst->xf1[0] = m->tx; inst->xf1[1] = m->ty; inst->xf1[2] = mag - 1.f; inst->xf1[3] = 0.f;
    }
    gp_scene_push(pc->scene, inst);
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
    const float scale = pc->scale, px_size = n->ts.size * scale, inv_alpha = 1.f / (float)GP_ATLAS_SIZE, inv_color = 1.f / (float)GP_COLOR_ATLAS_SIZE;
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
        gp_affine turn = { dx, dy, -dy, dx, 0, 0 };   /* about the origin */
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
        size_t nt = gp_curve_triangulate(p, m, pc->scratch, &t, &ed);
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
                push_triangle(pc, tri[e], ed, hw, x, y, clip, sp.grad ? ramp_at(sp.grad, (float)e) : sp.color, opacity);
            }
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
    float scale = pc->scale;
    float px_size = n->ts.size * scale;
    const float inv_alpha = 1.f / (float)GP_ATLAS_SIZE, inv_color = 1.f / (float)GP_COLOR_ATLAS_SIZE, inv_lcd = 1.f / (float)GP_LCD_ATLAS_SIZE;
    int lcd = pc->has_xf ? 0 : gp_text_lcd_order(pc->ts, scale);   /* (the stripes of a rotated or scaled glyph are not the screen's) */

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
            gp_text_snap_x(pc->ts, glyphs[gi].font, px_size, (line_x + glyphs[gi].x) * scale, &pixel, &bin);
            gp_glyph_entry e = gp_text_glyph_lcd(pc->ts, glyphs[gi].font, glyphs[gi].glyph, px_size, bin, lcd);
            if (e.w == 0) continue;
            float gx = (float)pixel + e.left;
            float gy = roundf((baseline + glyphs[gi].y) * scale) - e.top;
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
            push(pc, &inst);
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
    uint32_t sig[24] = { 0 };
    memcpy(sig, vals, sizeof vals);
    if (pc->has_xf) memcpy(sig + 16, &pc->xf, sizeof pc->xf);   /* what it is painted under */
    sig[8] = pc->atlas_gen;
    if (pc->has_tc) {   /* a text color from a state style above (to the 8 bits a channel it is drawn with) */
        const float *c = &pc->tc.r;
        for (int i = 0; i < 4; i++) sig[22] = sig[22] << 8 | (uint32_t)lroundf(fminf(fmaxf(c[i], 0.f), 1.f) * 255.f);
        sig[23] = 1;
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
    if (n->sub_flags & GP_SUB_AUTOHIDE) sig[14] = pc->vis_gen;   /* a scrollbar below faded in or out */
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
            for (size_t i = 0; i < vv->nchunk; i++) gp_scene_push(pc->scene, &vv->chunk[i]);
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
    memcpy(vv->chunk, pc->scene->items + start, count * sizeof *vv->chunk);
    vv->nchunk = count;
    memcpy(vv->chunk_sig, sig, sizeof sig);
    vv->chunk_x = x;
    vv->chunk_y = y;
    vv->chunk_gen = n->layout_gen;
    vv->chunk_valid = true;
}

/* What a state style (gp_hover_style, gp_active_style, gp_focus_style) changes: the paint-only properties it sets (background, border
 * colors, style and gradient, corner radii, shadow, opacity, text color), put over `dst`. Anything that would move or resize the element is not here. */
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
    opacity *= s->opacity;
    if (opacity <= 0) return;
    if (n->kind == GP_EL_CURVE) {
        gp_color c = s->has_stroke_color ? s->stroke_color : n->ts.color;
        if (hovered && pc->pressed && s->has_active_bg) c = s->active_bg;
        else if (hovered && s->has_hover_bg) c = s->hover_bg;
        if (focused && s->has_focus_bg && !(hovered && s->has_hover_bg)) c = s->focus_bg;
        paint_curve(pc, n, x, y, clip, opacity, c, (hovered && pc->pressed && s->has_active_bg) || (hovered && s->has_hover_bg) || (focused && s->has_focus_bg));
        return;
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
        return;
    }
    if (n->kind == GP_EL_IMAGE || n->kind == GP_EL_SVG) {
        paint_image(pc, n, x, y, clip, opacity);
        return;
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
    gp_affine mine = gp_style_affine(&n->style, ox + n->pos.x, oy + n->pos.y, n->size.w, n->size.h);
    pc->xf = gp_aff_mul(pc->has_xf ? pc->xf : gp_aff_identity(), mine);
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
