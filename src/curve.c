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
        const bool butt = n->style.stroke_cap == 1;
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
    *out = r;
    return m;
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
    float reach = n->style.stroke_w * 0.5f + n->style.hit_tol;
    if (!(reach >= 0.f)) return false;
    return gp_curve_distance(n, x, y) <= reach;
}
