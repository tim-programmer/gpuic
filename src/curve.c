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
                    for (size_t j = 0; j < left && ear; j++)
                        if (j != ip && j != i && j != in && in_tri(p[idx[j]], A, B, C)) ear = false;
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

/* Is (x, y) inside the closed outline (even-odd)? */
static bool inside_outline(const gp_point *p, size_t m, float x, float y) {
    bool in = false;
    for (size_t i = 0, j = m - 1; i < m; j = i++)
        if ((p[i].y > y) != (p[j].y > y) && x < (p[j].x - p[i].x) * (y - p[i].y) / (p[j].y - p[i].y) + p[i].x) in = !in;
    return in;
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
    if (!s->has_fill && !(s->arrow[0] > 0.f) && !(s->arrow[1] > 0.f)) return false;
    gp_arena tmp = { .chunk_size = 4096 };
    gp_point *pts, tri[3];
    size_t m = gp_curve_flatten(n, 0.05f, &tmp, &pts);
    bool hit = s->has_fill && s->fill_color.a > 0.f && m >= 3 && inside_outline(pts, m, x, y);
    float *arc = m ? gp_curve_arcs(pts, m, &tmp) : NULL, cut;
    for (int e = 0; e < 2 && !hit && s->stroke_w > 0.f; e++)
        if (m && gp_curve_arrow(n, pts, arc, m, e, tri, &cut)) hit = in_tri((gp_point){ x, y }, tri[0], tri[1], tri[2]);
    gp_arena_free(&tmp);
    return hit;
}
