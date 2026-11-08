/*
 * The row-height index behind gp_list, checked against a brute-force model under random
 * operations: set, forget, splice, resize, clear. No display needed.
 */
#include "../src/internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...)                                                               \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            if (failures++ < 25) {                                                     \
                printf("FAIL %s:%d: ", __FILE__, __LINE__);                            \
                printf(__VA_ARGS__);                                                   \
                printf("\n");                                                          \
            }                                                                          \
        }                                                                              \
    } while (0)

static uint64_t rng = 88172645463325252ull;
static uint32_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)(rng >> 11);
}

/* the model: plain arrays */
typedef struct {
    float *h;        /* < 0 = not measured */
    size_t n;
    float est0;
} model;

static double model_est(const model *m) {
    double sum = 0;
    size_t cnt = 0;
    for (size_t i = 0; i < m->n; i++) if (m->h[i] >= 0) { sum += m->h[i]; cnt++; }
    return cnt ? sum / (double)cnt : (m->est0 > 0 ? m->est0 : 1);
}
static double model_h(const model *m, size_t i, double est) { return m->h[i] >= 0 ? m->h[i] : est; }
static double model_top(const model *m, size_t i) {
    double est = model_est(m), s = 0;
    for (size_t r = 0; r < i; r++) s += model_h(m, r, est);
    return s;
}

static void compare(const gp_vlist *vl, const model *m, const char *what) {
    CHECK(vl->n == m->n, "%s: n %zu vs %zu", what, vl->n, m->n);
    double tol = 1e-6 * (1 + m->n);
    for (int q = 0; q < 12 && m->n; q++) {
        size_t i = rnd() % (m->n + 1);
        double a = gp_vlist_top(vl, i), b = model_top(m, i);
        CHECK(fabs(a - b) <= tol, "%s: top(%zu) = %.6f, model %.6f", what, i, a, b);
    }
    CHECK(fabs(gp_vlist_total(vl) - model_top(m, m->n)) <= tol, "%s: total", what);

    /* find: the row y falls in, by scanning the model */
    double est = model_est(m), total = model_top(m, m->n);
    for (int q = 0; q < 12 && m->n; q++) {
        double y = (double)(rnd() % 100000) / 100000.0 * (total + 20) - 5;
        double into;
        size_t got = gp_vlist_find(vl, y, &into);
        double yy = y < 0 ? 0 : y, top = 0;
        size_t want = 0;
        for (size_t r = 0; r < m->n; r++) {
            double h = model_h(m, r, est);
            want = r;
            if (yy < top + h || r + 1 == m->n) break;
            top += h;
        }
        if (yy >= total) want = m->n - 1;
        /* with zero-height rows or fp ties, any row whose span contains y is fine */
        bool ok = got == want;
        if (!ok) {
            double gt = model_top(m, got), gh = model_h(m, got, est);
            ok = fabs(gt + gh - yy) <= tol * 10 && gh == 0 ? true : (yy >= gt - tol * 10 && yy < gt + gh + tol * 10);
        }
        CHECK(ok, "%s: find(%.4f) = row %zu, model row %zu (n=%zu)", what, y, got, want, m->n);
        CHECK(fabs(into - (yy - model_top(m, got))) <= tol * 10, "%s: find offset %.6f", what, into);
    }
}

int main(void) {
    for (int round = 0; round < 40; round++) {
        float est0 = round % 5 == 0 ? 0 : 10.f + (float)(rnd() % 50);
        gp_vlist *vl = gp_vlist_new(est0);
        model m = { .est0 = est0 };
        size_t n0 = rnd() % 300;
        gp_vlist_resize(vl, n0);
        m.n = n0;
        m.h = malloc((n0 + 1) * sizeof *m.h);
        for (size_t i = 0; i < n0; i++) m.h[i] = -1;
        compare(vl, &m, "init");

        for (int step = 0; step < 150; step++) {
            int op = (int)(rnd() % 10);
            char what[48];
            snprintf(what, sizeof what, "round %d step %d op %d", round, step, op);
            if (op <= 3 && m.n) {                       /* measure */
                size_t i = rnd() % m.n;
                float h = (float)(rnd() % 90) * (rnd() % 7 == 0 ? 0 : 1);
                gp_vlist_set(vl, i, h);
                m.h[i] = h;
            } else if (op == 4 && m.n) {               /* forget a range */
                size_t s = rnd() % m.n, c = rnd() % 20;
                gp_vlist_forget(vl, s, c);
                for (size_t i = s; i < s + c && i < m.n; i++) m.h[i] = -1;
            } else if (op == 5) {                       /* splice */
                size_t s = m.n ? rnd() % (m.n + 1) : 0, o = m.n - s ? rnd() % (m.n - s + 1) % 15 : 0, f = rnd() % 15;
                gp_vlist_splice(vl, s, o, f);
                size_t nn = m.n - o + f;
                float *nh = malloc((nn + 1) * sizeof *nh);
                memcpy(nh, m.h, s * sizeof *nh);
                for (size_t i = 0; i < f; i++) nh[s + i] = -1;
                memcpy(nh + s + f, m.h + s + o, (m.n - s - o) * sizeof *nh);
                free(m.h);
                m.h = nh;
                m.n = nn;
            } else if (op == 6) {                       /* resize: grow (append) or shrink */
                size_t nn = m.n + (rnd() % 2 ? rnd() % 40 : 0);
                if (rnd() % 4 == 0 && m.n) nn = m.n - rnd() % (m.n > 30 ? 30 : m.n);
                gp_vlist_resize(vl, nn);
                float *nh = malloc((nn + 1) * sizeof *nh);
                for (size_t i = 0; i < nn; i++) nh[i] = i < m.n ? m.h[i] : -1;
                free(m.h);
                m.h = nh;
                m.n = nn;
            } else if (op == 7 && rnd() % 6 == 0) {     /* clear */
                gp_vlist_clear(vl);
                for (size_t i = 0; i < m.n; i++) m.h[i] = -1;
            } else if (m.n) {                           /* re-measure to a different height */
                size_t i = rnd() % m.n;
                float h = (float)(rnd() % 60) + 0.25f;
                gp_vlist_set(vl, i, h);
                m.h[i] = h;
            }
            compare(vl, &m, what);
        }
        free(m.h);
        gp_vlist_free(vl);
    }

    {   /* anchor bookkeeping across splices */
        gp_vlist *vl = gp_vlist_new(20);
        gp_vlist_resize(vl, 100);
        vl->a_idx = 50;
        vl->a_off = 7;
        gp_vlist_splice(vl, 10, 0, 5);                  /* insert above: the anchor row moved down */
        CHECK(vl->a_idx == 55 && vl->a_off == 7, "insert above: anchor %zu +%.1f", vl->a_idx, vl->a_off);
        gp_vlist_splice(vl, 0, 20, 0);                  /* remove above */
        CHECK(vl->a_idx == 35 && vl->a_off == 7, "remove above: anchor %zu", vl->a_idx);
        gp_vlist_splice(vl, 60, 10, 3);                 /* change below: untouched */
        CHECK(vl->a_idx == 35 && vl->a_off == 7, "change below: anchor %zu", vl->a_idx);
        gp_vlist_splice(vl, 30, 10, 2);                 /* the anchor row itself replaced */
        CHECK(vl->a_idx == 30 && vl->a_off == 0, "replace anchor: %zu +%.1f", vl->a_idx, vl->a_off);
        gp_vlist_splice(vl, 0, vl->n, 0);               /* everything gone */
        CHECK(vl->n == 0 && gp_vlist_total(vl) == 0, "emptied");
        double into;
        CHECK(gp_vlist_find(vl, 5, &into) == 0 && into == 0, "find in an empty index");
        gp_vlist_splice(vl, 0, 0, 4);
        CHECK(vl->n == 4 && gp_vlist_total(vl) == 80, "refilled with unmeasured rows: %.1f", gp_vlist_total(vl));
        gp_vlist_free(vl);
    }

    {   /* a million rows stays cheap */
        gp_vlist *vl = gp_vlist_new(30);
        gp_vlist_resize(vl, 1000000);
        for (size_t i = 0; i < 1000000; i += 97) gp_vlist_set(vl, i, 10 + (float)(i % 40));
        double into;
        size_t r = gp_vlist_find(vl, 12345678.0, &into);
        CHECK(r < 1000000 && into >= 0, "find in a big list");
        CHECK(fabs(gp_vlist_top(vl, r) + into - 12345678.0) < 1e-3, "find/top agree on a big list");
        gp_vlist_free(vl);
    }

    printf(failures ? "vlist_test: %d FAILURE(S)\n" : "vlist_test: all checks passed\n", failures);
    return failures != 0;
}
