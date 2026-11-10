/*
 * Stroked curves (gp_curve, gp_polyline): the geometry. The painting is paint.c's (every piece of the stroke is one instance), the shader's is in render_gl.c.
 *
 * A cubic Bezier is flattened on the CPU into straight pieces by uniform steps in t. The error of a piece against the curve is at most max|B''| / (8 n^2), and |B''| <= 6 M with M the
 * larger of |P0 - 2 P1 + P2| and |P1 - 2 P2 + P3|, so n = ceil(sqrt(3 M / (4 tol))) keeps every piece within `tol`: a cable that is long and shallow gets few pieces and a tight loop many.
 *
 * Hit testing is the distance from the pointer to the flattened centerline, at most half the width plus the tolerance, with the caps of an open end honored (a butt cap ends
 * where the stroke ends).
 */
#include "internal.h"

#include <math.h>
#include <string.h>

#define MAX_FLAT 512   /* pieces of one Bezier: a curve this long and this wiggly is not a cable */

/* The points of the element's centerline as straight pieces (arena memory): for a Bezier, flattened to within `tol` (in the element's own units); for a polyline,
 * its own points. Consecutive points that coincide are dropped (a piece of no length has no direction). Returns the count; 0 if there is nothing to draw. */
size_t gp_curve_flatten(const gp_el *n, float tol, gp_arena *a, gp_point **out) {
    *out = NULL;
    if (n->kind != GP_EL_CURVE || !n->curve_pts || !n->curve_n) return 0;
    const gp_point *p = n->curve_pts;
    size_t cap, m = 0;
    gp_point *r;
    if (n->curve_kind == 0 && n->curve_n == 4) {
        float ax = p[0].x - 2 * p[1].x + p[2].x, ay = p[0].y - 2 * p[1].y + p[2].y;
        float bx = p[1].x - 2 * p[2].x + p[3].x, by = p[1].y - 2 * p[2].y + p[3].y;
        float M = fmaxf(hypotf(ax, ay), hypotf(bx, by));
        if (!(tol > 1e-6f)) tol = 1e-6f;
        float steps = ceilf(sqrtf(0.75f * M / tol));
        size_t k = steps < 1.f ? 1 : steps > (float)MAX_FLAT ? (size_t)MAX_FLAT : (size_t)steps;
        /* A flat cap stands square to its end piece, which is a chord: a wide stroke would end slanted by the angle between the chord and the tangent. Two extra points a
         * small step in from the ends make those pieces nearly the tangent (the error of the chord's direction goes with its length). */
        const bool butt = n->style.stroke_cap == 1 || n->style.arrow[0] > 0.f || n->style.arrow[1] > 0.f;   /* (an arrowhead points along the end piece too) */
        const float e = 1.f / (8.f * (float)k);
        cap = k + 3;
        r = gp_arena_alloc(a, cap * sizeof *r, 4);
        for (size_t i = 0; i <= k; i++) {
            float ts[3];
            size_t nt = 0;
            if (butt && i == 1) ts[nt++] = e;
            if (butt && i == k) ts[nt++] = 1.f - e;
            ts[nt++] = (float)i / (float)k;
            for (size_t j = 0; j < nt; j++) {
                float t = ts[j], u = 1.f - t;
                float c0 = u * u * u, c1 = 3 * u * u * t, c2 = 3 * u * t * t, c3 = t * t * t;
                gp_point q = { c0 * p[0].x + c1 * p[1].x + c2 * p[2].x + c3 * p[3].x, c0 * p[0].y + c1 * p[1].y + c2 * p[2].y + c3 * p[3].y };
                if (m && fabsf(q.x - r[m - 1].x) < 1e-5f && fabsf(q.y - r[m - 1].y) < 1e-5f) continue;
                r[m++] = q;
            }
        }
    } else {
        cap = n->curve_n;
        r = gp_arena_alloc(a, cap * sizeof *r, 4);
        for (size_t i = 0; i < n->curve_n; i++) {
            if (m && fabsf(p[i].x - r[m - 1].x) < 1e-5f && fabsf(p[i].y - r[m - 1].y) < 1e-5f) continue;
            r[m++] = p[i];
        }
    }
    if (m == 0) {   /* all of it is one point: still a point (a round cap draws a dot there) */
        r[0] = p[0];
        m = 1;
    }
    if (n->style.closed && m >= 3 && (fabsf(r[m - 1].x - r[0].x) >= 1e-5f || fabsf(r[m - 1].y - r[0].y) >= 1e-5f)) {   /* back to the first point */
        gp_point *g = gp_arena_alloc(a, (m + 1) * sizeof *g, 4);
        memcpy(g, r, m * sizeof *g);
        g[m++] = r[0];
        r = g;
    }
    *out = r;
    return m;
}

/* The running length of the centerline at every point (arena): arc[0] = 0, arc[m - 1] the whole length. */
float *gp_curve_arcs(const gp_point *p, size_t m, gp_arena *a) {
    float *arc = gp_arena_alloc(a, (m ? m : 1) * sizeof *arc, 4);
    arc[0] = 0.f;
    for (size_t i = 1; i < m; i++) arc[i] = arc[i - 1] + hypotf(p[i].x - p[i - 1].x, p[i].y - p[i - 1].y);
    return arc;
}

/* The point at length s along the centerline (clamped to its ends). */
gp_point gp_curve_at(const gp_point *p, const float *arc, size_t m, float s, size_t *piece) {
    if (m < 2 || s <= 0.f) { if (piece) *piece = 0; return p[0]; }
    if (s >= arc[m - 1]) { if (piece) *piece = m - 2; return p[m - 1]; }
    size_t lo = 0, hi = m - 1;   /* the piece [lo, lo + 1] with arc[lo] <= s < arc[lo + 1] */
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (arc[mid] <= s) lo = mid; else hi = mid;
    }
    float l = arc[lo + 1] - arc[lo], t = l > 0.f ? (s - arc[lo]) / l : 0.f;
    if (piece) *piece = lo;
    return (gp_point){ p[lo].x + t * (p[lo + 1].x - p[lo].x), p[lo].y + t * (p[lo + 1].y - p[lo].y) };
}

/* The part of the centerline from length s0 to s1 (arena; the points at s0 and s1 are new, the ones between are the centerline's). Its count; *arc0 the length at its start. */
size_t gp_curve_cut(const gp_point *p, const float *arc, size_t m, float s0, float s1, gp_arena *a, gp_point **out) {
    size_t i0, i1;
    gp_point q0 = gp_curve_at(p, arc, m, s0, &i0), q1 = gp_curve_at(p, arc, m, s1, &i1);
    gp_point *r = gp_arena_alloc(a, (i1 - i0 + 3) * sizeof *r, 4);
    size_t k = 0;
    r[k++] = q0;
    for (size_t i = i0 + 1; i <= i1; i++)
        if (arc[i] > s0 && arc[i] < s1) r[k++] = p[i];
    if (s1 > s0) r[k++] = q1;
    *out = r;
    return k;
}

/* The arrowheads: for end e (0 the start, 1 the end) whose style asks for one, the triangle tip, base corner, base corner, and the length along the centerline where
 * the stroke stops (the middle of the base). Arrows longer than the stroke share it in proportion. False if that end has none. */
bool gp_curve_arrow(const gp_el *n, const gp_point *p, const float *arc, size_t m, int e, gp_point tri[3], float *cut) {
    const gp_style *s = &n->style;
    if (m < 2 || !(s->arrow[e] > 0.f) || s->closed) return false;
    float total = arc[m - 1], len = s->arrow[e], both = s->arrow[0] + s->arrow[1];
    if (both > total) len *= total / both;
    if (!(len > 0.f)) return false;
    gp_point tip = e ? p[m - 1] : p[0];
    float sc = e ? total - len : len;
    gp_point base = gp_curve_at(p, arc, m, sc, NULL);
    float dx = tip.x - base.x, dy = tip.y - base.y, l = hypotf(dx, dy);
    if (!(l > 1e-6f)) return false;
    dx /= l; dy /= l;
    float hw = fmaxf(s->arrow[e] * 0.5f, s->stroke_w * 0.5f);   /* half the width of the base: as wide as the head is long, never narrower than the stroke */
    tri[0] = tip;
    tri[1] = (gp_point){ base.x - dy * hw, base.y + dx * hw };
    tri[2] = (gp_point){ base.x + dy * hw, base.y - dx * hw };
    *cut = sc;
    return true;
}

static float cross3(gp_point a, gp_point b, gp_point c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }

/* the point a + t (b - a) nearest q: its t, and how far it is */
static float near_on(gp_point q, gp_point a, gp_point b, float *t) {
    float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    float u = l2 > 0.f ? ((q.x - a.x) * dx + (q.y - a.y) * dy) / l2 : 0.f;
    *t = u = fminf(fmaxf(u, 0.f), 1.f);
    return hypotf(q.x - (a.x + u * dx), q.y - (a.y + u * dy));
}

/* Can the stroke over the centerline p[0..m) (arc: lengths along it) cover a pixel twice? The pieces are cut from each other at every joint, so a stroke that does not come back
 * near itself covers each pixel once, and a translucent one can be drawn straight onto what is under it; one that does needs a layer to be blended once. It does where two
 * pieces that are apart along the line come within the stroke's width (and a pixel of antialiasing) of each other: a crossing, a loop, or a turn tighter than half the
 * width (two pieces are "apart" when they are much nearer each other than along the line: on a smooth bend they are about as near as they are along it); where markers
 * are drawn over it; and where the round caps of two dashes reach across the gap between them. */
bool gp_curve_self_overlaps(const gp_el *n, const gp_point *p, const float *arc, size_t m, float scale) {
    const gp_style *s = &n->style;
    if (s->marker[0] || s->marker[1] || s->marker[2]) return true;
    float reach = s->stroke_w + 1.5f / (scale > 0.f ? scale : 1.f);
    if (s->dash_n && s->stroke_cap == 0)
        for (int i = 1; i < s->dash_n * (s->dash_n % 2 ? 2 : 1); i += 2)
            if (s->dash[i % s->dash_n] < reach) return true;
    if (m < 4) return false;
    /* each piece's box, grown by half the reach, to pass over far pairs quickly */
    for (size_t i = 0; i + 1 < m; i++) {
        float ax0 = fminf(p[i].x, p[i + 1].x) - reach, ax1 = fmaxf(p[i].x, p[i + 1].x) + reach;
        float ay0 = fminf(p[i].y, p[i + 1].y) - reach, ay1 = fmaxf(p[i].y, p[i + 1].y) + reach;
        for (size_t j = i + 2; j + 1 < m; j++) {
            if (s->closed && i == 0 && j + 2 == m) continue;   /* (a closed stroke's last piece joins its first) */
            if (fmaxf(p[j].x, p[j + 1].x) < ax0 || fminf(p[j].x, p[j + 1].x) > ax1 || fmaxf(p[j].y, p[j + 1].y) < ay0 || fminf(p[j].y, p[j + 1].y) > ay1) continue;
            float d1 = cross3(p[i], p[i + 1], p[j]), d2 = cross3(p[i], p[i + 1], p[j + 1]), d3 = cross3(p[j], p[j + 1], p[i]), d4 = cross3(p[j], p[j + 1], p[i + 1]);
            if ((d1 > 0) != (d2 > 0) && (d3 > 0) != (d4 > 0) && d1 != 0 && d2 != 0 && d3 != 0 && d4 != 0) return true;   /* the centerline crosses itself */
            /* each end of one piece against the other piece: near it, and further from it along the line than across */
            for (int e = 0; e < 4; e++) {
                size_t a = e < 2 ? i : j, o = e < 2 ? j : i, k = a + (size_t)(e & 1);
                float t, d = near_on(p[k], p[o], p[o + 1], &t);
                float along = fabsf(arc[k] - (arc[o] + t * (arc[o + 1] - arc[o])));
                if (d < reach && d < 0.7f * along) return true;
            }
        }
    }
    return false;
}

/* The markers the style asks for, on the centerline p[0..m): at the first point (along the first piece), the last (along the last), and a polyline's corners between
 * (half way between the piece before and the piece after). Arena memory; the count. */
size_t gp_curve_markers(const gp_el *n, const gp_point *p, size_t m, gp_arena *a, gp_marker_at **out) {
    const gp_style *s = &n->style;
    *out = NULL;
    if (m < 2) return 0;
    size_t cap = 2 + (s->marker[2] && n->curve_kind == 1 ? m : 0), k = 0;
    gp_marker_at *mk = gp_arena_alloc(a, cap * sizeof *mk, 4);
    for (int e = 0; e < 2; e++) {
        if (!s->marker[e] || !(s->marker_size[e] > 0.f)) continue;
        gp_point c = e ? p[m - 1] : p[0], o = e ? p[m - 2] : p[1];
        float dx = e ? c.x - o.x : o.x - c.x, dy = e ? c.y - o.y : o.y - c.y, l = hypotf(dx, dy);
        if (!(l > 0.f)) continue;
        mk[k++] = (gp_marker_at){ c, dx / l, dy / l, s->marker_size[e], (float)e, s->marker[e] };
    }
    if (s->marker[2] && s->marker_size[2] > 0.f && n->curve_kind == 1)
        for (size_t i = 1; i + 1 < m; i++) {
            float ax = p[i].x - p[i - 1].x, ay = p[i].y - p[i - 1].y, bx = p[i + 1].x - p[i].x, by = p[i + 1].y - p[i].y;
            float la = hypotf(ax, ay), lb = hypotf(bx, by);
            if (!(la > 0.f) || !(lb > 0.f)) continue;
            float ux = ax / la + bx / lb, uy = ay / la + by / lb, lu = hypotf(ux, uy);
            if (lu < 1e-4f) { ux = ax / la; uy = ay / la; lu = 1.f; }   /* (straight back: along the way in) */
            mk[k++] = (gp_marker_at){ p[i], ux / lu, uy / lu, s->marker_size[2], 0.f, s->marker[2] };
        }
    *out = mk;
    return k;
}

int gp_marker_tris(const gp_marker_at *mk, float stroke_w, gp_point tri[6], int8_t ed[6]) {
    float h = mk->size * 0.5f, ux = mk->ux, uy = mk->uy, vx = -uy, vy = ux;
    gp_point c = mk->c, q[4];
    if (mk->shape == GP_MARKER_SQUARE) {
        q[0] = (gp_point){ c.x + (ux + vx) * h, c.y + (uy + vy) * h }; q[1] = (gp_point){ c.x + (ux - vx) * h, c.y + (uy - vy) * h };
        q[2] = (gp_point){ c.x - (ux + vx) * h, c.y - (uy + vy) * h }; q[3] = (gp_point){ c.x - (ux - vx) * h, c.y - (uy - vy) * h };
    } else if (mk->shape == GP_MARKER_DIAMOND) {
        q[0] = (gp_point){ c.x + ux * h, c.y + uy * h }; q[1] = (gp_point){ c.x + vx * h, c.y + vy * h };
        q[2] = (gp_point){ c.x - ux * h, c.y - uy * h }; q[3] = (gp_point){ c.x - vx * h, c.y - vy * h };
    } else if (mk->shape == GP_MARKER_BAR) {
        float t = fmaxf(stroke_w, 1.f) * 0.5f;
        q[0] = (gp_point){ c.x + ux * t + vx * h, c.y + uy * t + vy * h }; q[1] = (gp_point){ c.x + ux * t - vx * h, c.y + uy * t - vy * h };
        q[2] = (gp_point){ c.x - ux * t - vx * h, c.y - uy * t - vy * h }; q[3] = (gp_point){ c.x - ux * t + vx * h, c.y - uy * t + vy * h };
    } else {
        return 0;
    }
    /* two triangles sharing the diagonal q0-q2, which the first owns */
    tri[0] = q[0]; tri[1] = q[1]; tri[2] = q[2];
    tri[3] = q[0]; tri[4] = q[2]; tri[5] = q[3];
    const int8_t e[6] = { 0, 0, 1, -1, 0, 0 };
    memcpy(ed, e, sizeof e);
    return 2;
}

static bool in_tri(gp_point p, gp_point a, gp_point b, gp_point c) {
    float d0 = cross3(a, b, p), d1 = cross3(b, c, p), d2 = cross3(c, a, p);
    bool neg = d0 < 0 || d1 < 0 || d2 < 0, pos = d0 > 0 || d1 > 0 || d2 > 0;
    return !(neg && pos);
}

/* The inside of the closed outline p[0..m) (the last point joined to the first) as triangles, by cutting off ears: out[3k..3k+2] are the corners of triangle k and
 * edge[3k + j] says what its edge from corner j to corner j + 1 is: 0 the outline, 1 a diagonal this triangle owns (cut first), -1 a diagonal the other side owns. For an
 * outline that does not cross itself (holes and crossings are not handled: what cannot be cut as an ear is cut anyway, so the result is bounded but not the even-odd fill). */
size_t gp_curve_triangulate(const gp_point *p, size_t m, gp_arena *a, gp_point **out, int8_t **edge) {
    if (m >= 2 && fabsf(p[m - 1].x - p[0].x) < 1e-5f && fabsf(p[m - 1].y - p[0].y) < 1e-5f) m--;   /* a closed outline repeats its first point */
    *out = NULL; *edge = NULL;
    if (m < 3) return 0;
    size_t *idx = gp_arena_alloc(a, m * sizeof *idx, 8);
    int8_t *nx = gp_arena_alloc(a, m, 1);   /* the edge from the i-th remaining corner to the next: 0 outline, -1 a diagonal already owned */
    gp_point *t = gp_arena_alloc(a, 3 * (m - 2) * sizeof *t, 4);
    int8_t *f = gp_arena_alloc(a, 3 * (m - 2), 1);
    double area = 0;
    for (size_t i = 0; i < m; i++) { idx[i] = i; nx[i] = 0; area += (double)p[i].x * p[(i + 1) % m].y - (double)p[(i + 1) % m].x * p[i].y; }
    float sgn = area >= 0 ? 1.f : -1.f;
    size_t k = 0, left = m, guard = 0;
    while (left > 3 && guard < 4 * m * m) {
        bool cut = false;
        for (size_t pass = 0; pass < 2 && !cut; pass++)   /* pass 0: a real ear; pass 1 (no ear: a crossing or a hole): any convex corner, or any corner */
            for (size_t i = 0; i < left && !cut; i++) {
                guard++;
                size_t ip = (i + left - 1) % left, in = (i + 1) % left;
                gp_point A = p[idx[ip]], B = p[idx[i]], C = p[idx[in]];
                float c = cross3(A, B, C) * sgn;
                bool ear = c > 0.f;
                if (ear && pass == 0)
                    for (size_t j = 0; j < left && ear; j++) {
                        gp_point P = p[idx[j]];
                        bool corner = (P.x == A.x && P.y == A.y) || (P.x == B.x && P.y == B.y) || (P.x == C.x && P.y == C.y);   /* (a copy of a corner: a hole's bridge) */
                        if (j != ip && j != i && j != in && !corner && in_tri(P, A, B, C)) ear = false;
                    }
                if (pass == 1) ear = true;
                if (!ear) continue;
                if (fabsf(c) > 1e-9f) {
                    t[3 * k] = A; t[3 * k + 1] = B; t[3 * k + 2] = C;
                    f[3 * k] = nx[ip]; f[3 * k + 1] = nx[i]; f[3 * k + 2] = 1;   /* C to A is the new diagonal: this triangle owns it */
                    k++;
                }
                nx[ip] = -1;   /* what remains has the diagonal A to C, owned by this triangle */
                memmove(idx + i, idx + i + 1, (left - i - 1) * sizeof *idx);
                memmove(nx + i, nx + i + 1, (size_t)(left - i - 1));
                left--;
                cut = true;
            }
    }
    if (left == 3 && fabsf(cross3(p[idx[0]], p[idx[1]], p[idx[2]])) > 1e-9f) {
        t[3 * k] = p[idx[0]]; t[3 * k + 1] = p[idx[1]]; t[3 * k + 2] = p[idx[2]];
        f[3 * k] = nx[0]; f[3 * k + 1] = nx[1]; f[3 * k + 2] = nx[2];
        k++;
    }
    *out = t; *edge = f;
    return k;
}

/* ---------------------------------------------------------------- fills of outlines that cross themselves */

/* How many times the closed outline p[0..m) winds around (x, y), counterclockwise positive in the algebra used throughout (cross > 0: to the left) */
static int winding(const gp_point *p, size_t m, double x, double y) {
    int w = 0;
    for (size_t i = 0; i < m; i++) {
        double ax = p[i].x, ay = p[i].y, bx = p[(i + 1) % m].x, by = p[(i + 1) % m].y;
        double side = (bx - ax) * (y - ay) - (x - ax) * (by - ay);
        if (ay <= y) { if (by > y && side > 0) w++; }
        else if (by <= y && side < 0) w--;
    }
    return w;
}
static bool filled(int w, int rule) { return rule == 1 ? (w & 1) != 0 : w != 0; }

/* Is (x, y) inside the closed outline, by the fill rule (0 non-zero, 1 even-odd)? */
static bool inside_outline(const gp_point *p, size_t m, float x, float y, int rule) { return filled(winding(p, m, x, y), rule); }

typedef struct { double x, y; } dpt;
typedef struct { int a, b; } dseg;

/* where segments p0-p1 and q0-q1 meet: their parameters along each; false if parallel */
static bool seg_meet(dpt p0, dpt p1, dpt q0, dpt q1, double *t, double *u) {
    double rx = p1.x - p0.x, ry = p1.y - p0.y, sx = q1.x - q0.x, sy = q1.y - q0.y;
    double den = rx * sy - ry * sx;
    if (fabs(den) < 1e-12 * (fabs(rx) + fabs(ry)) * (fabs(sx) + fabs(sy))) return false;
    double qx = q0.x - p0.x, qy = q0.y - p0.y;
    *t = (qx * sy - qy * sx) / den;
    *u = (qx * ry - qy * rx) / den;
    return true;
}
/* do the open segments a-b and c-d cross or touch (other than at an end they share)? */
static bool seg_hits(dpt a, dpt b, dpt c, dpt d) {
    double t, u;
    if (!seg_meet(a, b, c, d, &t, &u)) return false;
    const double e = 1e-9;
    return t > e && t < 1 - e && u > -e && u < 1 + e;
}

static double loop_area(const dpt *v, const int *ids, size_t n) {
    double a = 0;
    for (size_t i = 0; i < n; i++) { dpt p = v[ids[i]], q = v[ids[(i + 1) % n]]; a += p.x * q.y - q.x * p.y; }
    return a * 0.5;
}
static int loop_winding(const dpt *v, const int *ids, size_t n, double x, double y) {
    int w = 0;
    for (size_t i = 0; i < n; i++) {
        dpt a = v[ids[i]], b = v[ids[(i + 1) % n]];
        double side = (b.x - a.x) * (y - a.y) - (x - a.x) * (b.y - a.y);
        if (a.y <= y) { if (b.y > y && side > 0) w++; }
        else if (b.y <= y && side < 0) w--;
    }
    return w;
}

/* The inside of the closed outline p[0..m) by a fill rule (0 non-zero, 1 even-odd), as gp_curve_triangulate gives it: triangles, and what each edge is (0 the outline,
 * 1 / -1 a line inside owned by this triangle / the other one). An outline that does not cross itself is triangulated as it is. One that does is cut where it crosses
 * itself into the pieces between crossings; a piece is the outline of the fill when the fill rule differs on its two sides (probed a hair away), and those pieces, turned
 * to have the inside on their left, are joined into loops (at a point where several meet, the one that turns most to the right, which keeps each loop around one piece
 * of the inside). A loop around the inside is an outer one, one around a hole a hole; each hole is joined to the outer loop it is in by a bridge (to the nearest corner that
 * can be reached without crossing anything) and the whole is triangulated; the bridges, which are inside, are made lines shared by the two triangles beside them. */
size_t gp_curve_fill(const gp_point *p, size_t m, int rule, gp_arena *a, gp_point **out, int8_t **edge) {
    if (m >= 2 && fabsf(p[m - 1].x - p[0].x) < 1e-5f && fabsf(p[m - 1].y - p[0].y) < 1e-5f) m--;
    *out = NULL; *edge = NULL;
    if (m < 3) return 0;
    double minx = INFINITY, miny = INFINITY, maxx = -INFINITY, maxy = -INFINITY;
    for (size_t i = 0; i < m; i++) { minx = fmin(minx, p[i].x); maxx = fmax(maxx, p[i].x); miny = fmin(miny, p[i].y); maxy = fmax(maxy, p[i].y); }
    double scale = fmax(maxx - minx, maxy - miny);
    if (!(scale > 0)) return 0;
    const double eps = scale * 1e-7;

    /* the crossings: where edge i is cut, as parameters along it */
    size_t cap_t = 4 * m;
    double *cuts = gp_arena_alloc(a, cap_t * sizeof *cuts, 8);
    int *cut_edge = gp_arena_alloc(a, cap_t * sizeof *cut_edge, 4);
    size_t ncuts = 0;
    for (size_t i = 0; i < m; i++)
        for (size_t j = i + 1; j < m; j++) {
            bool adjacent = j == i + 1 || (i == 0 && j == m - 1);
            if (adjacent) continue;
            dpt p0 = { p[i].x, p[i].y }, p1 = { p[(i + 1) % m].x, p[(i + 1) % m].y }, q0 = { p[j].x, p[j].y }, q1 = { p[(j + 1) % m].x, p[(j + 1) % m].y };
            double t, u;
            double tv[4];
            int te[4], nt = 0;
            if (!seg_meet(p0, p1, q0, q1, &t, &u)) {
                /* parallel: on one line and overlapping, each is cut where the other ends (the overlap is then a piece of both) */
                double rx = p1.x - p0.x, ry = p1.y - p0.y, rl2 = rx * rx + ry * ry, sx = q1.x - q0.x, sy = q1.y - q0.y, sl2 = sx * sx + sy * sy;
                if (!(rl2 > 0) || !(sl2 > 0)) continue;
                double off = fabs((q0.x - p0.x) * ry - (q0.y - p0.y) * rx) / sqrt(rl2);
                if (off > eps * 10) continue;
                double a0 = ((q0.x - p0.x) * rx + (q0.y - p0.y) * ry) / rl2, a1 = ((q1.x - p0.x) * rx + (q1.y - p0.y) * ry) / rl2;
                double b0 = ((p0.x - q0.x) * sx + (p0.y - q0.y) * sy) / sl2, b1 = ((p1.x - q0.x) * sx + (p1.y - q0.y) * sy) / sl2;
                tv[nt] = a0; te[nt++] = (int)i;
                tv[nt] = a1; te[nt++] = (int)i;
                tv[nt] = b0; te[nt++] = (int)j;
                tv[nt] = b1; te[nt++] = (int)j;
            } else {
                const double e = 1e-9;
                if (t < -e || t > 1 + e || u < -e || u > 1 + e) continue;
                tv[nt] = t; te[nt++] = (int)i;
                tv[nt] = u; te[nt++] = (int)j;
            }
            for (int k = 0; k < nt; k++) {
                double v = tv[k];
                if (v <= 1e-9 || v >= 1 - 1e-9) continue;   /* (at a corner: nothing to cut) */
                if (ncuts == cap_t) {
                    size_t nc = cap_t * 2;
                    double *c2 = gp_arena_alloc(a, nc * sizeof *c2, 8);
                    int *e2 = gp_arena_alloc(a, nc * sizeof *e2, 4);
                    memcpy(c2, cuts, ncuts * sizeof *cuts);
                    memcpy(e2, cut_edge, ncuts * sizeof *cut_edge);
                    cuts = c2; cut_edge = e2; cap_t = nc;
                }
                cuts[ncuts] = v;
                cut_edge[ncuts++] = te[k];
            }
        }
    if (ncuts == 0) {   /* no crossing: the outline is its own fill */
        bool touching = false;   /* (a corner on another edge, or two corners at one point, count as a crossing too: they were skipped above as cuts at a corner) */
        for (size_t i = 0; i < m && !touching; i++)
            for (size_t j = i + 1; j < m && !touching; j++)
                if (fabs((double)p[i].x - p[j].x) <= eps * 10 && fabs((double)p[i].y - p[j].y) <= eps * 10) touching = true;
        for (size_t i = 0; i < m && !touching; i++)
            for (size_t j = 0; j < m && !touching; j++) {
                if (j == i || (j + 1) % m == i) continue;
                dpt v = { p[i].x, p[i].y }, q0 = { p[j].x, p[j].y }, q1 = { p[(j + 1) % m].x, p[(j + 1) % m].y };
                double dx = q1.x - q0.x, dy = q1.y - q0.y, l2 = dx * dx + dy * dy;
                if (l2 <= 0) continue;
                double t = ((v.x - q0.x) * dx + (v.y - q0.y) * dy) / l2;
                double cx = q0.x + t * dx - v.x, cy = q0.y + t * dy - v.y;
                if (t > 1e-9 && t < 1 - 1e-9 && cx * cx + cy * cy < eps * eps) touching = true;
            }
        if (!touching) return gp_curve_triangulate(p, m, a, out, edge);
    }

    /* the points: the corners and the crossings, merged when they coincide */
    size_t cap_v = m + ncuts;
    dpt *v = gp_arena_alloc(a, cap_v * sizeof *v, 8);
    int nv = 0;
    int *corner = gp_arena_alloc(a, m * sizeof *corner, 4);
    #define ADD_POINT(X, Y, OUT) do { \
        double ax_ = (X), ay_ = (Y); int f_ = -1; \
        for (int q_ = 0; q_ < nv && f_ < 0; q_++) if (fabs(v[q_].x - ax_) <= eps * 10 && fabs(v[q_].y - ay_) <= eps * 10) f_ = q_; \
        if (f_ < 0) { v[nv] = (dpt){ ax_, ay_ }; f_ = nv++; } \
        (OUT) = f_; } while (0)
    for (size_t i = 0; i < m; i++) ADD_POINT(p[i].x, p[i].y, corner[i]);
    /* the pieces of each edge, in order along it */
    size_t cap_s = m + ncuts;
    dseg *sg = gp_arena_alloc(a, cap_s * sizeof *sg, 4);
    size_t ns = 0;
    double *ts = gp_arena_alloc(a, (ncuts + 2) * sizeof *ts, 8);
    for (size_t i = 0; i < m; i++) {
        size_t nt = 0;
        for (size_t k = 0; k < ncuts; k++) if (cut_edge[k] == (int)i) ts[nt++] = cuts[k];
        for (size_t x = 1; x < nt; x++) for (size_t y = x; y > 0 && ts[y - 1] > ts[y]; y--) { double tmp = ts[y]; ts[y] = ts[y - 1]; ts[y - 1] = tmp; }
        int prev = corner[i];
        double ax = p[i].x, ay = p[i].y, bx = p[(i + 1) % m].x, by = p[(i + 1) % m].y;
        for (size_t k = 0; k <= nt; k++) {
            int id;
            if (k < nt) ADD_POINT(ax + (bx - ax) * ts[k], ay + (by - ay) * ts[k], id);
            else id = corner[(i + 1) % m];
            if (id != prev) sg[ns++] = (dseg){ prev, id };
            prev = id;
        }
    }
    #undef ADD_POINT

    /* the pieces that are the outline of the fill, turned to have it on their left; one of each (an outline that goes over itself gives two) */
    dseg *keep = gp_arena_alloc(a, (ns ? ns : 1) * sizeof *keep, 4);
    size_t nk = 0;
    for (size_t i = 0; i < ns; i++) {
        dpt A = v[sg[i].a], B = v[sg[i].b];
        double dx = B.x - A.x, dy = B.y - A.y, l = hypot(dx, dy);
        if (!(l > 0)) continue;
        double h = fmin(scale * 1e-5, l * 0.01);
        double mx = (A.x + B.x) * 0.5, my = (A.y + B.y) * 0.5, nx = -dy / l * h, ny = dx / l * h;
        bool fl = filled(winding(p, m, mx + nx, my + ny), rule), fr = filled(winding(p, m, mx - nx, my - ny), rule);
        if (fl == fr) continue;
        dseg d = fl ? sg[i] : (dseg){ sg[i].b, sg[i].a };
        bool dup = false;
        for (size_t k = 0; k < nk && !dup; k++) dup = keep[k].a == d.a && keep[k].b == d.b;
        if (!dup) keep[nk++] = d;
    }
    if (!nk) return 0;

    /* join them into loops: from the end of one, the next that turns most to the right from going back the way it came */
    bool *used = gp_arena_alloc(a, nk, 1);
    int *loopv = gp_arena_alloc(a, (nk + 1) * sizeof *loopv, 4);   /* every loop's points, one after another */
    size_t *loop_at = gp_arena_alloc(a, (nk + 1) * sizeof *loop_at, 8), nloops = 0, nlv = 0;
    for (size_t s0 = 0; s0 < nk; s0++) {
        if (used[s0]) continue;
        loop_at[nloops] = nlv;
        size_t cur = s0;
        size_t guard = 0;
        while (!used[cur] && guard++ <= nk) {
            used[cur] = true;
            loopv[nlv++] = keep[cur].a;
            int at = keep[cur].b;
            dpt in = { v[at].x - v[keep[cur].a].x, v[at].y - v[keep[cur].a].y };
            double best = INFINITY;
            size_t next = (size_t)-1;
            for (size_t k = 0; k < nk; k++) {
                if (used[k] && k != s0) continue;
                if (keep[k].a != at) continue;
                dpt d = { v[keep[k].b].x - v[at].x, v[keep[k].b].y - v[at].y };
                double bx = -in.x, by = -in.y;   /* back the way it came */
                double ang = atan2(bx * d.y - by * d.x, bx * d.x + by * d.y);   /* counterclockwise from back to d */
                double cw = -ang;
                while (cw <= 1e-12) cw += 2 * M_PI;   /* clockwise, in (0, 2 pi] */
                if (cw < best) { best = cw; next = k; }
            }
            if (next == (size_t)-1 || next == s0) break;
            cur = next;
        }
        if (nlv - loop_at[nloops] >= 3) nloops++;
        else nlv = loop_at[nloops];
    }
    loop_at[nloops] = nlv;
    if (!nloops) return 0;

    /* outer loops (area > 0: the inside on the left goes counterclockwise) and holes */
    double *area = gp_arena_alloc(a, nloops * sizeof *area, 8);
    for (size_t l = 0; l < nloops; l++) area[l] = loop_area(v, loopv + loop_at[l], loop_at[l + 1] - loop_at[l]);
    int *owner = gp_arena_alloc(a, nloops * sizeof *owner, 4);
    for (size_t l = 0; l < nloops; l++) {
        owner[l] = -1;
        if (area[l] >= 0) continue;
        const int *ids = loopv + loop_at[l];
        dpt A = v[ids[0]], B = v[ids[1]];
        double dx = B.x - A.x, dy = B.y - A.y, ln = hypot(dx, dy), h = fmin(scale * 1e-5, ln * 0.01);
        double px = (A.x + B.x) * 0.5 + dy / ln * h, py = (A.y + B.y) * 0.5 - dx / ln * h;   /* (to its right: the fill around the hole) */
        double best = INFINITY;
        for (size_t o = 0; o < nloops; o++)
            if (area[o] > 0 && area[o] < best && loop_winding(v, loopv + loop_at[o], loop_at[o + 1] - loop_at[o], px, py) != 0) { best = area[o]; owner[l] = (int)o; }
    }

    /* every outer loop with its holes bridged in, triangulated */
    gp_point *tri = NULL;
    int8_t *fl = NULL;
    size_t ntri = 0, cap_tri = 0;
    for (size_t o = 0; o < nloops; o++) {
        if (area[o] <= 0) continue;
        size_t cap_m = loop_at[nloops] + 4 * nloops + 4, nm = 0;
        int *mp = gp_arena_alloc(a, cap_m * sizeof *mp, 4);
        for (size_t k = loop_at[o]; k < loop_at[o + 1]; k++) mp[nm++] = loopv[k];
        int *bridge = gp_arena_alloc(a, 2 * nloops * sizeof *bridge, 4);   /* pairs of points joined by a bridge */
        size_t nb = 0;
        for (;;) {   /* the hole with the point furthest right first */
            int pick = -1, pk = -1;
            double bestx = -INFINITY;
            for (size_t h = 0; h < nloops; h++) {
                if (owner[h] != (int)o) continue;
                for (size_t k = loop_at[h]; k < loop_at[h + 1]; k++)
                    if (v[loopv[k]].x > bestx) { bestx = v[loopv[k]].x; pick = (int)h; pk = (int)(k - loop_at[h]); }
            }
            if (pick < 0) break;
            owner[pick] = -2;
            const int *hv = loopv + loop_at[pick];
            size_t hn = loop_at[pick + 1] - loop_at[pick];
            dpt M = v[hv[pk]];
            /* the nearest corner of what is there so far that a straight line reaches without crossing anything */
            int vi = -1;
            double bestd = INFINITY;
            for (size_t c = 0; c < nm; c++) {
                dpt V = v[mp[c]];
                double dd = (V.x - M.x) * (V.x - M.x) + (V.y - M.y) * (V.y - M.y);
                if (dd >= bestd || mp[c] == hv[pk]) continue;
                bool clear = true;
                for (size_t e = 0; e < nm && clear; e++) {
                    int e0 = mp[e], e1 = mp[(e + 1) % nm];
                    if (e0 == mp[c] || e1 == mp[c]) continue;
                    clear = !seg_hits(M, V, v[e0], v[e1]) && !seg_hits(v[e0], v[e1], M, V);
                }
                for (size_t h = 0; h < nloops && clear; h++) {
                    if (owner[h] != (int)o && (int)h != pick) continue;
                    for (size_t k = loop_at[h]; k < loop_at[h + 1] && clear; k++) {
                        int e0 = loopv[k], e1 = loopv[k + 1 < loop_at[h + 1] ? k + 1 : loop_at[h]];
                        if (e0 == hv[pk] || e1 == hv[pk]) continue;
                        clear = !seg_hits(M, V, v[e0], v[e1]) && !seg_hits(v[e0], v[e1], M, V);
                    }
                }
                if (clear) { bestd = dd; vi = (int)c; }
            }
            if (vi < 0) continue;   /* (cannot be reached: the hole is left out) */
            /* ... V, M, around the hole back to M, V, ... */
            memmove(mp + vi + 1 + hn + 2, mp + vi + 1, (nm - (size_t)vi - 1) * sizeof *mp);
            for (size_t k = 0; k <= hn; k++) mp[vi + 1 + k] = hv[(pk + k) % hn];
            mp[vi + 1 + hn + 1] = mp[vi];
            nm += hn + 2;
            bridge[nb++] = mp[vi];
            bridge[nb++] = hv[pk];
        }
        gp_point *pts = gp_arena_alloc(a, nm * sizeof *pts, 4);
        for (size_t k = 0; k < nm; k++) pts[k] = (gp_point){ (float)v[mp[k]].x, (float)v[mp[k]].y };
        gp_point *t;
        int8_t *f;
        size_t nt = gp_curve_triangulate(pts, nm, a, &t, &f);
        /* a bridge is inside: the two triangles along it share it, the one on its left (going from the lesser end to the greater) owning it */
        for (size_t k = 0; k < nt; k++)
            for (int j = 0; j < 3; j++) {
                if (f[3 * k + j]) continue;
                gp_point P = t[3 * k + j], Q = t[3 * k + (j + 1) % 3], R = t[3 * k + (j + 2) % 3];
                for (size_t b = 0; b < nb; b += 2) {
                    gp_point B0 = { (float)v[bridge[b]].x, (float)v[bridge[b]].y }, B1 = { (float)v[bridge[b + 1]].x, (float)v[bridge[b + 1]].y };
                    bool on = (P.x == B0.x && P.y == B0.y && Q.x == B1.x && Q.y == B1.y) || (P.x == B1.x && P.y == B1.y && Q.x == B0.x && Q.y == B0.y);
                    if (!on) continue;
                    gp_point L0 = B0, L1 = B1;
                    if (L1.x < L0.x || (L1.x == L0.x && L1.y < L0.y)) { L0 = B1; L1 = B0; }
                    f[3 * k + j] = cross3(L0, L1, R) > 0 ? 1 : -1;
                }
            }
        if (ntri + nt > cap_tri) {
            size_t nc = (ntri + nt) * 2;
            gp_point *t2 = gp_arena_alloc(a, 3 * nc * sizeof *t2, 4);
            int8_t *f2 = gp_arena_alloc(a, 3 * nc, 1);
            if (ntri) { memcpy(t2, tri, 3 * ntri * sizeof *tri); memcpy(f2, fl, 3 * ntri); }
            tri = t2; fl = f2; cap_tri = nc;
        }
        if (nt) { memcpy(tri + 3 * ntri, t, 3 * nt * sizeof *t); memcpy(fl + 3 * ntri, f, 3 * nt); }
        ntri += nt;
    }
    *out = tri; *edge = fl;
    return ntri;
}

/* The smallest distance from (x, y), in the element's own coordinates, to the stroke's centerline, with the open ends of a butt-capped stroke not reaching past their
 * end (a point beyond a butt end is not on the stroke): infinity if there is none. */
float gp_curve_distance(const gp_el *n, float x, float y) {
    gp_arena tmp = { .chunk_size = 4096 };
    gp_point *pts;
    size_t m = gp_curve_flatten(n, 0.05f, &tmp, &pts);
    float best = INFINITY;
    const bool butt = n->style.stroke_cap == 1;
    if (m == 1) {
        if (!butt) best = hypotf(x - pts[0].x, y - pts[0].y);
    }
    for (size_t i = 0; i + 1 < m; i++) {
        float ax = pts[i].x, ay = pts[i].y, bx = pts[i + 1].x, by = pts[i + 1].y;
        float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
        float t = len2 > 0 ? ((x - ax) * dx + (y - ay) * dy) / len2 : 0.f;
        if (butt && ((i == 0 && t < 0.f) || (i + 2 == m && t > 1.f))) continue;   /* beyond an open end with a flat cap */
        t = fminf(fmaxf(t, 0.f), 1.f);
        float d = hypotf(x - (ax + t * dx), y - (ay + t * dy));
        if (d < best) best = d;
    }
    gp_arena_free(&tmp);
    return best;
}

/* Is the point (x, y), in the element's own coordinates, on the stroke (within half the width plus the hit tolerance of the centerline)? */
bool gp_curve_hit(const gp_el *n, float x, float y) {
    const gp_style *s = &n->style;
    float reach = s->stroke_w * 0.5f + s->hit_tol;
    if (reach >= 0.f && s->stroke_w > 0.f && gp_curve_distance(n, x, y) <= reach) return true;
    if (!s->has_fill && !(s->arrow[0] > 0.f) && !(s->arrow[1] > 0.f) && !s->marker[0] && !s->marker[1] && !s->marker[2]) return false;
    gp_arena tmp = { .chunk_size = 4096 };
    gp_point *pts, tri[3];
    size_t m = gp_curve_flatten(n, 0.05f, &tmp, &pts);
    bool hit = s->has_fill && s->fill_color.a > 0.f && m >= 3 && inside_outline(pts, m, x, y, s->fill_rule);
    if (!hit && s->stroke_w > 0.f && (s->marker[0] || s->marker[1] || s->marker[2])) {
        gp_marker_at *mk;
        size_t nm = gp_curve_markers(n, pts, m, &tmp, &mk);
        for (size_t i = 0; i < nm && !hit; i++) {
            gp_point t6[6];
            int8_t e6[6];
            if (gp_marker_tris(&mk[i], s->stroke_w, t6, e6)) hit = in_tri((gp_point){ x, y }, t6[0], t6[1], t6[2]) || in_tri((gp_point){ x, y }, t6[3], t6[4], t6[5]);
            else hit = hypotf(x - mk[i].c.x, y - mk[i].c.y) <= mk[i].size * 0.5f + s->hit_tol;
        }
    }
    float *arc = m ? gp_curve_arcs(pts, m, &tmp) : NULL, cut;
    for (int e = 0; e < 2 && !hit && s->stroke_w > 0.f; e++)
        if (m && gp_curve_arrow(n, pts, arc, m, e, tri, &cut)) hit = in_tri((gp_point){ x, y }, tri[0], tri[1], tri[2]);
    gp_arena_free(&tmp);
    return hit;
}
