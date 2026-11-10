/*
 * Row-height index of a variable-height gp_list.
 *
 * Rows are only measured when they are built, and only the visible ones are. The index keeps
 * what has been measured and answers the two questions layout asks, in O(log n):
 * "where does row i start?" and "which row is at offset y?". Rows nobody has measured yet are
 * assumed to be as tall as the average of the measured ones (the given estimate before any
 * is), so the scroll extent firms up as the user scrolls and the scrollbar stays usable on a
 * list of a million rows nobody has seen.
 *
 * Two Fenwick trees over the rows: the sum of the measured heights and how many are measured.
 * The offset of row i is then  measured_sum(i) + (i - measured_count(i)) * estimate.
 */
#include "internal.h"

/* Bumped whenever any index changes what it answers: a layout that finds this unchanged since the layout
 * it compares with would measure the same rows again, and arrive at the same extent. */
static uint32_t g_generation;
uint32_t gp_vlist_generation(void) { return g_generation; }

static inline size_t lowbit(size_t i) { return i & (~i + 1); }

static double estimate(const gp_vlist *vl) {
    if (vl->nmeasured) return vl->sum / (double)vl->nmeasured;
    return vl->est0 > 0 ? vl->est0 : 1;
}

static void *grow(void *p, size_t bytes) {
    p = realloc(p, bytes);
    if (!p) abort();
    return p;
}

/* Fenwick trees from scratch, O(n): after rows are removed or re-measured en masse. */
static void rebuild(gp_vlist *vl) {
    size_t n = vl->n;
    vl->sum = 0;
    vl->nmeasured = 0;
    if (!vl->cap) return;   /* nothing was ever allocated: no rows */
    memset(vl->fsum, 0, (n + 1) * sizeof *vl->fsum);
    memset(vl->fcnt, 0, (n + 1) * sizeof *vl->fcnt);
    for (size_t i = 1; i <= n; i++) {
        if (vl->h[i - 1] >= 0) {
            vl->fsum[i] += vl->h[i - 1];
            vl->fcnt[i] += 1;
            vl->sum += vl->h[i - 1];
            vl->nmeasured++;
        }
        size_t j = i + lowbit(i);
        if (j <= n) {
            vl->fsum[j] += vl->fsum[i];
            vl->fcnt[j] += vl->fcnt[i];
        }
    }
}

static void reserve(gp_vlist *vl, size_t n) {
    if (n <= vl->cap) return;
    size_t cap = vl->cap ? vl->cap : 64;
    while (cap < n) cap *= 2;
    vl->h = grow(vl->h, cap * sizeof *vl->h);
    vl->fsum = grow(vl->fsum, (cap + 1) * sizeof *vl->fsum);
    vl->fcnt = grow(vl->fcnt, (cap + 1) * sizeof *vl->fcnt);
    vl->cap = cap;
}

void gp_vlist_free(gp_vlist *vl) {
    if (!vl) return;
    free(vl->h);
    free(vl->fsum);
    free(vl->fcnt);
    free(vl);
}

/* Makes the index describe exactly `n` rows. New rows at the end are unmeasured; with fewer
 * rows the tail is forgotten. Appending is O(new rows * log n): the new tree nodes also cover
 * rows that already existed, so each is computed from the nodes below it. */
void gp_vlist_resize(gp_vlist *vl, size_t n) {
    if (n == vl->n) return;
    g_generation++;
    reserve(vl, n);
    if (n > vl->n) {
        for (size_t i = vl->n; i < n; i++) vl->h[i] = -1;
        for (size_t p = vl->n + 1; p <= n; p++) {
            double s = 0;
            int32_t c = 0;
            size_t stop = p - lowbit(p);
            for (size_t q = p - 1; q > stop; q -= lowbit(q)) {   /* the nodes that make up p's range */
                s += vl->fsum[q];
                c += vl->fcnt[q];
            }
            vl->fsum[p] = s;   /* row p itself is unmeasured */
            vl->fcnt[p] = c;
        }
        vl->n = n;
    } else {
        vl->n = n;
        rebuild(vl);
    }
}

/* Forgets every measurement (the width changed, or the content did in unknown ways). */
void gp_vlist_clear(gp_vlist *vl) {
    g_generation++;
    for (size_t i = 0; i < vl->n; i++) vl->h[i] = -1;
    rebuild(vl);
}

/* Replaces rows [start, start+old) by `fresh` unmeasured ones, like Vec::splice. The anchor
 * follows the row it is on, so inserting above the viewport does not move what is visible. */
void gp_vlist_splice(gp_vlist *vl, size_t start, size_t old, size_t fresh) {
    if (start > vl->n) start = vl->n;
    if (old > vl->n - start) old = vl->n - start;
    g_generation++;
    size_t tail = vl->n - start - old, n = vl->n - old + fresh;
    reserve(vl, n);
    if (tail) memmove(vl->h + start + fresh, vl->h + start + old, tail * sizeof *vl->h);   /* (a list that never had a row has no array: NULL, even for 0 bytes, is undefined) */
    for (size_t i = 0; i < fresh; i++) vl->h[start + i] = -1;
    vl->n = n;
    rebuild(vl);

    if (vl->a_idx >= start + old) {
        vl->a_idx = vl->a_idx - old + fresh;
    } else if (vl->a_idx >= start) {   /* the anchor row itself was replaced */
        vl->a_idx = start;
        vl->a_off = 0;
    }
    if (n && vl->a_idx >= n) vl->a_idx = n - 1;
}

/* Records a row's measured height. */
void gp_vlist_set(gp_vlist *vl, size_t i, float h) {
    if (i >= vl->n) return;
    if (h < 0) h = 0;
    float old = vl->h[i];
    if (old == h) return;
    g_generation++;
    double delta = old >= 0 ? (double)h - old : h;
    int dc = old >= 0 ? 0 : 1;
    vl->h[i] = h;
    vl->sum += delta;
    vl->nmeasured += (size_t)dc;
    for (size_t p = i + 1; p <= vl->n; p += lowbit(p)) {
        vl->fsum[p] += delta;
        vl->fcnt[p] += dc;
    }
}

bool gp_vlist_measured(const gp_vlist *vl, size_t i) { return i < vl->n && vl->h[i] >= 0; }

float gp_vlist_height(const gp_vlist *vl, size_t i) {
    if (i < vl->n && vl->h[i] >= 0) return vl->h[i];
    return (float)estimate(vl);
}

/* Offset of the top of row i (i == n: the end of the last row). */
double gp_vlist_top(const gp_vlist *vl, size_t i) {
    if (i > vl->n) i = vl->n;
    double s = 0;
    int64_t c = 0;
    for (size_t p = i; p > 0; p -= lowbit(p)) {
        s += vl->fsum[p];
        c += vl->fcnt[p];
    }
    return s + (double)((int64_t)i - c) * estimate(vl);
}

double gp_vlist_total(const gp_vlist *vl) { return gp_vlist_top(vl, vl->n); }

/* The row containing offset y (clamped into the list), and how far into it y is. */
size_t gp_vlist_find(const gp_vlist *vl, double y, double *into) {
    size_t n = vl->n;
    if (!n) { *into = 0; return 0; }
    if (y < 0) y = 0;
    double est = estimate(vl), s = 0;
    int64_t c = 0;
    size_t pos = 0, step = 1;
    while (step * 2 <= n) step *= 2;
    for (; step; step >>= 1) {
        size_t np = pos + step;
        if (np > n) continue;
        double ns = s + vl->fsum[np];
        int64_t nc = c + vl->fcnt[np];
        if (ns + (double)((int64_t)np - nc) * est <= y) {
            pos = np;
            s = ns;
            c = nc;
        }
    }
    if (pos >= n) pos = n - 1;
    *into = y - gp_vlist_top(vl, pos);
    return pos;
}

gp_vlist *gp_vlist_new(float est0) {
    gp_vlist *vl = calloc(1, sizeof *vl);
    if (!vl) abort();
    vl->est0 = est0;
    vl->last_y = -1;   /* never a real offset: the first layout derives the anchor from y */
    return vl;
}

/* Marks rows [start, start+count) as not measured; the anchor stays where it is. */
void gp_vlist_forget(gp_vlist *vl, size_t start, size_t count) {
    if (start >= vl->n) return;
    if (count > vl->n - start) count = vl->n - start;
    for (size_t i = start; i < start + count; i++) {
        float old = vl->h[i];
        if (old < 0) continue;
        g_generation++;
        vl->h[i] = -1;
        vl->sum -= old;
        vl->nmeasured--;
        for (size_t p = i + 1; p <= vl->n; p += lowbit(p)) {
            vl->fsum[p] -= old;
            vl->fcnt[p] -= 1;
        }
    }
}
