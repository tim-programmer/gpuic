/*
 * End-to-end test of gp_list (variable-height virtualized list). Needs a display. Everything
 * is checked against a model: the realized rows must be contiguous, cover the viewport, have the
 * heights the model says, and sit where the model says once the rows above have been seen.
 * The scenarios that matter most are the ones an estimate-based list gets wrong: moving by a
 * few pixels across rows that were never measured, rows above the viewport changing height,
 * rows inserted above it, and the end of the list.
 *
 * Layout: window padding 10, the list is 300x300 at (10, 10) with 5 px of vertical padding.
 */
#include <gpuic/gpuic.h>

#include "../src/internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...)                                                               \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            failures++;                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                \
            printf(__VA_ARGS__);                                                       \
            printf("\n");                                                              \
        }                                                                              \
    } while (0)

#define LIST_X 10
#define LIST_Y 10
#define LIST_W 300
#define LIST_H 300
#define PAD_Y 5
#define BIG 100000

enum { FIXED, WRAP, STICK };

typedef struct {
    gp_scroll sc, sc_wrap, sc_stick;
    int mode;
    float wrap_w;
    size_t *ids;          /* the model: item id of every row */
    size_t n;
    size_t built;         /* item function calls */
    size_t clicked;       /* id of the item last clicked */
    int clicks;
    size_t ov_id;         /* one item whose height is overridden */
    float ov_h;
} Root;

static Root *root;
static gp_app *app;
static gp_entity root_entity;

/* ------------------------------------------------------------ the model */

static float row_h(size_t id) {
    if (root->ov_h > 0 && id == root->ov_id) return root->ov_h;
    if (id % 11 == 3) return 0;   /* rows of no height exist and must not trip anything */
    return (float)(18 + (id * 2654435761u >> 7) % 61);
}

static double model_top(size_t k) {   /* content offset of row k, assuming every row above is known */
    double s = PAD_Y;
    for (size_t r = 0; r < k; r++) s += row_h(root->ids[r]);
    return s;
}

static void ids_reset(size_t n) {
    free(root->ids);
    root->ids = malloc((n + 1) * sizeof *root->ids);
    for (size_t i = 0; i < n; i++) root->ids[i] = i;
    root->n = n;
}

/* Replaces rows [at, at+old) with `fresh` new items (ids from `next_id`). Does not tell the list. */
static size_t next_id = 1000000;
static void ids_splice(size_t at, size_t old, size_t fresh) {
    size_t nn = root->n - old + fresh;
    size_t *v = malloc((nn + 1) * sizeof *v);
    memcpy(v, root->ids, at * sizeof *v);
    for (size_t i = 0; i < fresh; i++) v[at + i] = next_id++;
    memcpy(v + at + fresh, root->ids + at + old, (root->n - at - old) * sizeof *v);
    free(root->ids);
    root->ids = v;
    root->n = nn;
}

/* ------------------------------------------------------------ the view */

static void row_clicked(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    Root *r = self;
    r->clicked = (size_t)data;
    r->clicks++;
}

static gp_node *fixed_row(void *self, gp_ctx *cx, size_t i, void *data) {
    Root *r = self;
    r->built++;
    /* the 1 px scroller inside every row gives each realized row a state of its own in the window's
     * scroll table, so the table grows while rows are being laid out */
    return gp_div(gp_h(row_h(r->ids[i])), gp_on_click_data(row_clicked, (void *)r->ids[i]),
                  gp_text(gp_fmt("item %zu", r->ids[i])),
                  gp_div(gp_w(1), gp_h(1), gp_overflow_y_scroll()));
}

static gp_node *wrap_row(void *self, gp_ctx *cx, size_t i, void *data) {
    Root *r = self;
    r->built++;
    size_t words = 3 + (r->ids[i] * 7) % 40;
    char *t = gp_frame_alloc(words * 6 + 1);
    t[0] = 0;
    for (size_t w = 0; w < words; w++) strcat(t, w % 5 == 4 ? "wordy " : "word ");
    return gp_div(gp_px(8), gp_py(4), gp_text(t));
}

static gp_node *view(void *self, gp_ctx *cx) {
    Root *r = self;
    gp_node *list;
    switch (r->mode) {
    case WRAP:
        list = gp_list(r->n, 40, wrap_row, NULL, gp_track_scroll(r->sc_wrap), gp_w(r->wrap_w),
                       gp_h(LIST_H), gp_py(PAD_Y));
        break;
    case STICK:
        list = gp_list(r->n, 40, fixed_row, NULL, gp_track_scroll(r->sc_stick), gp_w(LIST_W),
                       gp_h(LIST_H), gp_py(PAD_Y), gp_list_stick_to_end());
        break;
    default:
        list = gp_list(r->n, 40, fixed_row, NULL, gp_track_scroll(r->sc), gp_w(LIST_W),
                       gp_h(LIST_H), gp_py(PAD_Y));
    }
    return gp_div(gp_size_full(), gp_p(LIST_X), list);
}

/* ------------------------------------------------------------ inspection */

typedef struct {
    gp_el *list;
    float y, max, view_h;   /* y: the logical offset */
    float rel;              /* offset relative to the first realized row, which is what rows are positioned by */
    double base;            /* content offset of the first realized row */
    size_t first, n;
} Snap;

static gp_el *find_list(gp_el *n) {
    if (n->kind == GP_EL_LIST) return n;
    for (uint32_t i = 0; i < n->nchildren; i++) {
        gp_el *r = find_list(n->children[i]);
        if (r) return r;
    }
    return NULL;
}

static gp_window *win(void) { return app->windows[0]; }

static bool snap(Snap *s) {
    s->list = win()->tree ? find_list(win()->tree) : NULL;
    if (!s->list) return false;
    s->rel = s->list->scroll.y;
    s->base = s->list->scroll_base_y;
    s->y = (float)(s->base + s->rel);
    s->max = s->list->scroll_max.y;
    s->view_h = s->list->size.h - 2 * s->list->style.border_w;
    s->first = s->list->list_first;
    s->n = s->list->nchildren;
    return true;
}

/* Row k's top relative to the top of the viewport (screen position within the list). */
static float row_screen_y(const Snap *s, size_t k) {
    const gp_el *c = s->list->children[k - s->first];
    return c->pos.y - s->list->style.border_w - s->rel;
}

/* The row at the top of the viewport: the first whose bottom is below the top edge. */
static size_t top_row(const Snap *s) {
    for (size_t i = 0; i < s->n; i++) {
        const gp_el *c = s->list->children[i];
        if (c->pos.y - s->list->style.border_w + c->size.h > s->rel + 0.001f) return s->first + i;
    }
    return s->first + s->n - 1;
}

static void settle(gp_co *co) { gp_sleep(co, 0.06); }

/* Structural invariants that must hold after every layout. `fixed`: heights follow the model. */
static void inv(const char *what, bool fixed) {
    Snap s;
    if (!snap(&s)) { CHECK(false, "%s: no list in the tree", what); return; }
    size_t count = root->n;
    if (count == 0) {
        CHECK(s.n == 0 && s.max == 0, "%s: empty list has %zu rows, max %.1f", what, s.n, s.max);
        return;
    }
    const gp_el *l = s.list;
    CHECK(s.n >= 1, "%s: nothing realized", what);
    if (s.n == 0) return;
    CHECK(s.n < 80, "%s: %zu rows realized", what, s.n);
    CHECK(s.y >= -0.01f && s.y <= s.max + 0.01f, "%s: offset %.2f outside [0, %.2f]", what, s.y, s.max);
    CHECK(s.first + s.n <= count, "%s: rows %zu..%zu of %zu", what, s.first, s.first + s.n, count);
    float bw = l->style.border_w;
    for (size_t i = 0; i < s.n; i++) {
        const gp_el *c = l->children[i];
        if (fixed)
            CHECK(fabsf(c->size.h - row_h(root->ids[s.first + i])) < 0.01f, "%s: row %zu is %.2f tall, model says %.2f",
                  what, s.first + i, c->size.h, row_h(root->ids[s.first + i]));
        if (i + 1 < s.n) {
            const gp_el *d = l->children[i + 1];
            CHECK(fabsf(d->pos.y - (c->pos.y + c->size.h)) < 0.01f, "%s: gap between rows %zu and %zu", what,
                  s.first + i, s.first + i + 1);
        }
    }
    const gp_el *c0 = l->children[0], *cn = l->children[s.n - 1];
    float top = c0->pos.y - bw, bottom = cn->pos.y - bw + cn->size.h;
    bool tail = s.first + s.n == count;
    CHECK(fabsf(top) < 0.01f, "%s: rows are not positioned from their first row (%.2f)", what, top);
    if (s.first == 0) CHECK(fabs(s.base - PAD_Y) < 0.01, "%s: first row starts at %.2f", what, s.base);
    else CHECK(top <= s.rel + 0.01f, "%s: gap above the viewport (rows start at %.2f, viewport at %.2f)", what, top, s.rel);
    if (tail) CHECK(bottom + PAD_Y >= s.rel + s.view_h - 0.01f || (s.first == 0 && s.y <= 0.01f),
                    "%s: gap below the last row (%.2f vs viewport bottom %.2f)", what, bottom + PAD_Y, s.rel + s.view_h);
    else CHECK(bottom >= s.rel + s.view_h - 0.01f, "%s: gap below the viewport (rows end at %.2f)", what, bottom);
    if (s.max > 0 && s.y >= s.max - 0.01f)
        CHECK(tail && fabsf(bottom + PAD_Y - (s.rel + s.view_h)) < 0.01f, "%s: at the end but the last row is not flush (%.2f vs %.2f)",
              what, bottom + PAD_Y, s.rel + s.view_h);
    CHECK(s.list->list_head == (s.first == 0) && s.list->list_tail == tail, "%s: head/tail flags", what);
}

static void wheel(float dy) { gp_scroll_wheel(win(), (gp_point){ LIST_X + 100, LIST_Y + 100 }, 0, dy); }
static void notify(void) { gp_ctx cx = gp_ctx_for(app, root_entity); gp_notify(&cx); }
static gp_ctx cxr(void) { return gp_ctx_for(app, root_entity); }

/* ------------------------------------------------------------ scenarios */

/* A new model on a list that has forgotten everything it measured, scrolled to the top. */
static void fresh(gp_co *co, size_t n) {
    ids_reset(n);
    notify();
    settle(co);
    gp_ctx cx = cxr();
    gp_list_reset(&cx, root->mode == STICK ? root->sc_stick : root->sc);
    gp_scroll_to(&cx, root->mode == STICK ? root->sc_stick : root->sc, 0, 0);
    settle(co);
}

static void test_start(gp_co *co) {
    ids_reset(BIG);
    root->mode = FIXED;
    notify();
    settle(co);
    inv("start", true);
    Snap s;
    snap(&s);
    CHECK(s.first == 0 && s.y == 0, "starts at the top (first %zu, y %.1f)", s.first, s.y);
    CHECK(root->built < 40, "%zu item calls for the first screen of %d rows", root->built, BIG);
    /* nothing measured beyond the first screen: the extent is an estimate of every row */
    CHECK(s.max > 1e6, "estimated extent %.0f for %d rows", s.max, BIG);
}

static void test_sequential(gp_co *co) {
    ids_reset(BIG);
    notify();
    settle(co);
    gp_ctx cx = cxr();
    gp_scroll_to(&cx, root->sc, 0, 0);
    settle(co);
    root->built = 0;
    /* walk down in steps that are not a multiple of anything: every row crossed is measured, so the
     * offset is exactly what was scrolled and the rows sit where the model puts them */
    float want = 0;
    for (int step = 1; step <= 240; step++) {
        wheel(53);
        want += 53;
        if (step % 6 == 0) {
            settle(co);
            char what[40];
            snprintf(what, sizeof what, "sequential step %d", step);
            inv(what, true);
            Snap s;
            snap(&s);
            CHECK(fabsf(s.y - want) < 0.05f, "%s: offset %.2f, scrolled %.2f", what, s.y, want);
            CHECK(fabs(s.base - model_top(s.first)) < 0.05, "%s: row %zu at %.2f, model %.2f", what, s.first, s.base, model_top(s.first));
        }
    }
    CHECK(root->built < 3000, "walking 12k px built %zu rows", root->built);
    /* and back up, exactly */
    for (int step = 1; step <= 240; step++) wheel(-53);
    settle(co);
    inv("back at the top", true);
    Snap s;
    snap(&s);
    CHECK(s.y == 0 && s.first == 0, "back at the top: y %.2f first %zu", s.y, s.first);
}

/* A jump lands somewhere by estimate. Moving a few notches from there must still move the view by
 * exactly that much, although every row crossed has just been measured for the first time. */
static void test_small_moves_after_a_jump(gp_co *co) {
    ids_reset(BIG);
    notify();
    settle(co);
    gp_ctx cx = cxr();
    for (int round = 0; round < 6; round++) {
        float targets[] = { 2.0e6f, 1.1e6f, 3.3e6f, 7.7e5f, 2.2e6f, 9.9e5f };
        gp_scroll_to(&cx, root->sc, 0, targets[round]);
        settle(co);
        char what[48];
        snprintf(what, sizeof what, "jump %d", round);
        inv(what, true);
        Snap s;
        snap(&s);
        size_t r = top_row(&s);
        CHECK(r > 5000 && r < BIG - 5000, "%s: landed on row %zu", what, r);
        float before = row_screen_y(&s, r);

        for (int dir = 0; dir < 2; dir++) {
            float total = 0;
            for (int i = 0; i < 9; i++) {
                float d = dir ? 37 : -37;
                wheel(d);
                total += d;
                snprintf(what, sizeof what, "jump %d move %d/%d", round, dir, i);
                inv(what, true);
            }
            snap(&s);
            CHECK(r >= s.first && r < s.first + s.n, "%s: the row we watched left the realized range", what);
            if (r >= s.first && r < s.first + s.n)
                CHECK(fabsf(row_screen_y(&s, r) - (before - total)) < 0.05f,
                      "jump %d: row %zu moved to %.3f, expected %.3f (scrolled %.0f)", round, r,
                      row_screen_y(&s, r), before - total, total);
            before = row_screen_y(&s, r);
        }
    }
}

/* Rows above the viewport that are built (overdraw) change height: what the user sees stays. */
static void test_rows_above_change(gp_co *co) {
    ids_reset(BIG);
    notify();
    settle(co);
    gp_ctx cx = cxr();
    gp_scroll_to(&cx, root->sc, 0, 1.5e6f);
    settle(co);
    Snap s;
    snap(&s);
    size_t r = top_row(&s);
    CHECK(r > s.first, "no overdraw rows above the viewport (first %zu, top %zu)", s.first, r);
    float before = row_screen_y(&s, r);

    /* an overdraw row grows */
    size_t above = r - 1;
    root->ov_id = root->ids[above];
    root->ov_h = 77;
    notify();
    settle(co);
    inv("row above grew", false);
    snap(&s);
    CHECK(top_row(&s) == r || top_row(&s) == above + 1, "top row changed from %zu to %zu", r, top_row(&s));
    CHECK(r >= s.first && fabsf(row_screen_y(&s, r) - before) < 0.05f,
          "visible row %zu moved from %.3f to %.3f when the row above it grew", r, before,
          r >= s.first ? row_screen_y(&s, r) : NAN);
    if (above >= s.first) CHECK(fabsf(s.list->children[above - s.first]->size.h - 77) < 0.01f, "row above was not re-measured");

    /* the row at the top itself grows: it keeps its top edge */
    root->ov_id = root->ids[r];
    root->ov_h = 140;
    notify();
    settle(co);
    snap(&s);
    CHECK(r >= s.first && fabsf(row_screen_y(&s, r) - before) < 0.05f,
          "top row %zu moved from %.3f to %.3f when it grew", r, before, r >= s.first ? row_screen_y(&s, r) : NAN);
    root->ov_h = 0;
}

static void test_end(gp_co *co) {
    ids_reset(BIG);
    notify();
    settle(co);
    gp_ctx cx = cxr();
    gp_scroll_to_end(&cx, root->sc);
    settle(co);
    inv("scroll to end", true);
    Snap s;
    snap(&s);
    CHECK(s.first + s.n == BIG && s.y >= s.max - 0.01f, "at the end: rows %zu..%zu, y %.1f, max %.1f", s.first, s.first + s.n, s.y, s.max);
    float bottom = s.list->children[s.n - 1]->pos.y + s.list->children[s.n - 1]->size.h + PAD_Y;
    CHECK(fabsf(bottom - (s.rel + s.view_h)) < 0.01f, "last row flush with the bottom: %.2f vs %.2f", bottom, s.rel + s.view_h);
    wheel(500);   /* nothing further */
    settle(co);
    inv("wheel at the end", true);
    snap(&s);
    CHECK(s.first + s.n == BIG, "still at the end after more wheel");

    /* back up, exactly */
    size_t r = top_row(&s);
    float before = row_screen_y(&s, r);
    for (int i = 0; i < 12; i++) { wheel(-40); settle(co); }
    inv("up from the end", true);
    snap(&s);
    CHECK(r >= s.first && fabsf(row_screen_y(&s, r) - (before + 480)) < 0.05f, "row %zu moved %.3f, expected 480", r,
          r >= s.first ? row_screen_y(&s, r) - before : NAN);

    /* and to the very top */
    gp_scroll_to(&cx, root->sc, 0, 0);
    settle(co);
    inv("scroll to start", true);
    snap(&s);
    CHECK(s.first == 0 && s.y == 0, "top after scroll_to(0)");
}

/* A small list read from end to end: once everything has been measured the extent is exact. */
static void test_exact_extent(gp_co *co) {
    fresh(co, 300);
    for (int i = 0; i < 400; i++) { wheel(60); if (i % 20 == 0) settle(co); }
    settle(co);
    inv("read to the end", true);
    Snap s;
    snap(&s);
    double total = model_top(300) + PAD_Y;
    CHECK(fabs(s.max - (total - s.view_h)) < 0.05, "extent %.3f, model %.3f", s.max, total - s.view_h);
    CHECK(s.y >= s.max - 0.01f, "at the end (y %.2f, max %.2f)", s.y, s.max);
    /* the scroll bar thumb is sized from that extent */
    gp_thumb t;
    CHECK(gp_scroll_thumb(s.list, 1, LIST_X, LIST_Y, &t), "no scroll bar thumb");
}

static void top_item_is(const char *what, size_t id, float screen_y) {
    Snap s;
    snap(&s);
    size_t r = top_row(&s);
    CHECK(root->ids[r] == id, "%s: the top item is %zu, was %zu", what, root->ids[r], id);
    if (root->ids[r] == id)
        CHECK(fabsf(row_screen_y(&s, r) - screen_y) < 0.05f, "%s: the top item moved from %.3f to %.3f", what, screen_y, row_screen_y(&s, r));
}

static void test_splice(gp_co *co) {
    ids_reset(300);
    notify();
    settle(co);
    gp_ctx cx = cxr();
    gp_scroll_to(&cx, root->sc, 0, 0);
    settle(co);
    for (int i = 0; i < 80; i++) wheel(40);       /* read 3200 px: a few dozen rows in, all measured */
    settle(co);
    Snap s;
    snap(&s);
    size_t r = top_row(&s);
    size_t id = root->ids[r];
    float at = row_screen_y(&s, r);
    CHECK(r > 40, "scrolled only to row %zu", r);

    ids_splice(0, 0, 50);                          /* older items arrive above */
    gp_list_splice(&cx, root->sc, 0, 0, 50);
    notify();
    settle(co);
    inv("prepend", false);
    top_item_is("prepend 50", id, at);

    ids_splice(5, 20, 0);                          /* some rows far above are removed */
    gp_list_splice(&cx, root->sc, 5, 20, 0);
    notify();
    settle(co);
    inv("remove above", false);
    top_item_is("remove 20 above", id, at);

    snap(&s);
    size_t r2 = top_row(&s);
    ids_splice(r2 + 6, 3, 8);                      /* a change below the top: top row stays */
    gp_list_splice(&cx, root->sc, r2 + 6, 3, 8);
    notify();
    settle(co);
    inv("change below", false);
    top_item_is("change below", id, at);

    ids_splice(r2 - 2, 1, 1);                      /* one row just above the viewport replaced */
    gp_list_splice(&cx, root->sc, r2 - 2, 1, 1);
    notify();
    settle(co);
    inv("replace just above", false);
    top_item_is("replace just above", id, at);

    /* without telling the list, rows still lay out correctly (they are remeasured) */
    ids_splice(root->n, 0, 5);
    notify();
    settle(co);
    inv("append", false);
    snap(&s);
    CHECK(s.max > 0, "extent after append");

    /* removing the row at the top moves the anchor to where it was */
    snap(&s);
    r2 = top_row(&s);
    ids_splice(r2, 1, 0);
    gp_list_splice(&cx, root->sc, r2, 1, 0);
    notify();
    settle(co);
    inv("remove the top row", false);
    snap(&s);
    CHECK(top_row(&s) == r2 || top_row(&s) == r2 + 1 || top_row(&s) == r2 - 1, "after removing the top row the view is at row %zu (was %zu)", top_row(&s), r2);

    /* empty it, refill it */
    gp_scroll_to(&cx, root->sc, 0, 0);
    settle(co);
    size_t all = root->n;
    ids_splice(0, all, 0);
    gp_list_splice(&cx, root->sc, 0, all, 0);
    notify();
    settle(co);
    inv("emptied", false);
    ids_splice(0, 0, 3);
    gp_list_splice(&cx, root->sc, 0, 0, 3);
    notify();
    settle(co);
    inv("three rows", false);
    snap(&s);
    CHECK(s.max == 0 && s.first == 0 && s.n == 3, "three short rows: max %.1f first %zu n %zu", s.max, s.first, s.n);
}

static void test_invalidate(gp_co *co) {
    ids_reset(200);
    root->ov_h = 0;
    notify();
    settle(co);
    gp_ctx cx = cxr();
    gp_list_reset(&cx, root->sc);
    gp_scroll_to(&cx, root->sc, 0, 0);
    settle(co);
    for (int i = 0; i < 300; i++) { wheel(60); if (i % 25 == 0) settle(co); }
    settle(co);                                    /* all 200 rows measured, at the end */
    Snap s;
    snap(&s);
    float max0 = s.max;

    root->ov_id = 5;
    root->ov_h = 1000;                             /* row 5 is taller than it was when measured */
    notify();
    settle(co);
    snap(&s);
    CHECK(fabsf(s.max - max0) < 0.01f, "an unseen row changed and the extent moved (%.2f -> %.2f)", max0, s.max);

    gp_list_invalidate(&cx, root->sc, 5, 1);       /* forgotten: the average stands in until it is seen */
    settle(co);
    snap(&s);
    CHECK(fabsf(s.max - max0) > 0.5f, "invalidating a row did not change the extent");

    gp_scroll_to(&cx, root->sc, 0, 0);
    settle(co);
    inv("after invalidate", false);
    for (int i = 0; i < 30; i++) { wheel(60); }
    settle(co);
    snap(&s);
    float want = (float)(model_top(200) + PAD_Y) - s.view_h;
    gp_scroll_to_end(&cx, root->sc);
    settle(co);
    snap(&s);
    CHECK(fabsf(s.max - want) < 0.1f, "extent %.2f after the row was seen again, model %.2f", s.max, want);

    gp_list_reset(&cx, root->sc);
    settle(co);
    inv("after reset", false);
    root->ov_h = 0;
}

static void test_scroll_to_item(gp_co *co) {
    fresh(co, BIG);
    gp_ctx cx = cxr();
    Snap s;

    /* far, never seen */
    gp_list_scroll_to_item(&cx, root->sc, 50000, GP_ALIGN_ROW_START);
    settle(co);
    inv("to item (start)", true);
    snap(&s);
    CHECK(s.first <= 50000 && 50000 < s.first + s.n, "row 50000 is not realized (rows %zu..%zu)", s.first, s.first + s.n);
    CHECK(fabsf(row_screen_y(&s, 50000) - PAD_Y) < 0.05f, "row 50000 at %.3f, expected the padding (%d)", row_screen_y(&s, 50000), PAD_Y);

    gp_list_scroll_to_item(&cx, root->sc, 70001, GP_ALIGN_ROW_CENTER);
    settle(co);
    inv("to item (center)", true);
    snap(&s);
    float h = s.list->children[70001 - s.first]->size.h;
    CHECK(fabsf(row_screen_y(&s, 70001) - (s.view_h - h) / 2) < 0.05f, "row 70001 (%.0f tall) at %.3f, expected %.3f", h, row_screen_y(&s, 70001), (s.view_h - h) / 2);

    /* NEAREST: already in view = no movement */
    float y0 = s.y;
    gp_list_scroll_to_item(&cx, root->sc, 70001, GP_ALIGN_ROW_NEAREST);
    settle(co);
    snap(&s);
    CHECK(fabsf(s.y - y0) < 0.01f, "nearest on a visible row moved the view by %.2f", s.y - y0);

    /* NEAREST: below the viewport -> its bottom edge flush with the bottom */
    size_t below = top_row(&s) + 12;
    gp_list_scroll_to_item(&cx, root->sc, below, GP_ALIGN_ROW_NEAREST);
    settle(co);
    inv("to item (nearest below)", true);
    snap(&s);
    float hb = s.list->children[below - s.first]->size.h;
    CHECK(fabsf(row_screen_y(&s, below) + hb - s.view_h) < 0.05f, "row %zu bottom at %.3f, viewport %.0f", below, row_screen_y(&s, below) + hb, s.view_h);

    /* NEAREST: above -> top edge at the top */
    size_t above = top_row(&s) - 9;
    gp_list_scroll_to_item(&cx, root->sc, above, GP_ALIGN_ROW_NEAREST);
    settle(co);
    inv("to item (nearest above)", true);
    snap(&s);
    CHECK(fabsf(row_screen_y(&s, above) - PAD_Y) < 0.05f, "row %zu top at %.3f", above, row_screen_y(&s, above));

    /* the ends */
    gp_list_scroll_to_item(&cx, root->sc, BIG - 1, GP_ALIGN_ROW_START);
    settle(co);
    inv("last item at the start", true);
    snap(&s);
    CHECK(s.first + s.n == BIG && s.y >= s.max - 0.01f, "last row cannot go above the end");
    gp_list_scroll_to_item(&cx, root->sc, 0, GP_ALIGN_ROW_CENTER);
    settle(co);
    inv("first item centered", true);
    snap(&s);
    CHECK(s.first == 0 && s.y == 0, "first row cannot go below the start (y %.2f)", s.y);

    /* two requests before a layout: the last one wins */
    gp_list_scroll_to_item(&cx, root->sc, 100, GP_ALIGN_ROW_START);
    gp_list_scroll_to_item(&cx, root->sc, 200, GP_ALIGN_ROW_START);
    settle(co);
    snap(&s);
    CHECK(fabsf(row_screen_y(&s, 200) - PAD_Y) < 0.05f, "second request not honoured");
}

static void test_small_lists(gp_co *co) {
    size_t sizes[] = { 0, 1, 2, 5, 0, 7, 1 };
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        ids_reset(sizes[i]);
        notify();
        settle(co);
        char what[32];
        snprintf(what, sizeof what, "%zu rows", sizes[i]);
        inv(what, true);
        wheel(100);
        settle(co);
        inv(what, true);
    }
    ids_reset(BIG);
    notify();
    settle(co);
    ids_reset(3);                                  /* shrinks from far down, to fewer rows than a screen */
    notify();
    settle(co);
    inv("shrunk", true);
}

static void test_width(gp_co *co) {
    root->mode = WRAP;
    root->wrap_w = 300;
    ids_reset(300);
    notify();
    settle(co);
    inv("wrap, wide", false);
    Snap s;
    snap(&s);
    float wide_h = s.list->children[0]->size.h;
    for (int i = 0; i < 1200; i++) wheel(40);        /* read all 300 rows at this width: the extent is exact */
    settle(co);
    snap(&s);
    float max_wide = s.max;
    CHECK(s.first + s.n == 300, "wrap list not read to its end");
    gp_ctx cx = cxr();
    gp_scroll_to(&cx, root->sc_wrap, 0, max_wide * 0.4f);
    settle(co);
    inv("wrap, scrolled", false);
    snap(&s);
    size_t r = top_row(&s);
    float at = row_screen_y(&s, r);
    CHECK(r > 20, "wrap list only reached row %zu", r);

    float rh_wide = r >= s.first ? s.list->children[r - s.first]->size.h : 0;

    root->wrap_w = 140;                            /* narrower: more lines per row */
    notify();
    settle(co);
    inv("wrap, narrow", false);
    snap(&s);
    CHECK(s.max > max_wide * 1.3f, "narrowing 300 -> 140 px left the extent at %.0f (was %.0f): old heights kept", s.max, max_wide);
    CHECK(top_row(&s) == r, "the top row changed from %zu to %zu when the width changed", r, top_row(&s));
    if (r >= s.first) {
        CHECK(fabsf(row_screen_y(&s, r) - at) < 0.05f, "the top row moved from %.3f to %.3f on a width change", at, row_screen_y(&s, r));
        float rh_narrow = s.list->children[r - s.first]->size.h;
        CHECK(rh_narrow > rh_wide + 5, "row %zu is %.1f tall at 140 px and %.1f at 300: not remeasured", r, rh_narrow, rh_wide);
    }

    root->wrap_w = 300;
    notify();
    settle(co);
    inv("wrap, wide again", false);
    snap(&s);
    CHECK(top_row(&s) == r, "the top row changed from %zu to %zu going back", r, top_row(&s));
    if (r >= s.first) CHECK(fabsf(s.list->children[r - s.first]->size.h - rh_wide) < 0.01f, "row %zu is not back to %.1f", r, rh_wide);
    root->mode = FIXED;
    (void)wide_h;
}


/* Three million rows is ~150 million px: far beyond what a float can place pixels in. Rows must
 * still sit exactly against each other, and moves must still be exact. */
static void test_huge(gp_co *co) {
    fresh(co, 3000000);
    gp_ctx cx = cxr();
    gp_scroll_to(&cx, root->sc, 0, 7.0e7f);
    settle(co);
    inv("huge: jump", true);
    Snap s;
    snap(&s);
    size_t r = top_row(&s);
    CHECK(r > 500000 && r < 2500000, "huge: landed on row %zu", r);
    float before = row_screen_y(&s, r);
    float total = 0;
    for (int i = 0; i < 25; i++) {
        float d = i < 12 ? 29 : -29;
        wheel(d);
        total += d;
        inv("huge: move", true);
    }
    snap(&s);
    CHECK(r >= s.first && r < s.first + s.n && fabsf(row_screen_y(&s, r) - (before - total)) < 0.01f,
          "huge: row %zu moved %.4f, expected %.4f", r, r >= s.first ? row_screen_y(&s, r) - before : NAN, -total);

    /* drag the scroll bar thumb to the middle of its track */
    snap(&s);
    gp_thumb t;
    CHECK(gp_scroll_thumb(s.list, 1, LIST_X, LIST_Y, &t), "huge: no thumb");
    gp_point grab = { t.x + t.w / 2, t.y + t.h / 2 };
    CHECK(gp_scroll_begin_drag(win(), grab), "huge: thumb not grabbed");
    float to = t.track_start + (t.track_len - t.thumb_len) / 2 + t.thumb_len / 2;
    gp_scroll_drag_to(win(), (gp_point){ grab.x, to });
    win()->drag_key = 0;
    inv("huge: dragged", true);
    snap(&s);
    CHECK(fabsf(s.y / s.max - 0.5f) < 0.02f, "huge: thumb dragged to the middle, offset is %.3f of the extent", s.y / s.max);

    /* ... and all the way down, where the last row must be flush */
    gp_scroll_to_end(&cx, root->sc);
    settle(co);
    inv("huge: end", true);
    snap(&s);
    CHECK(s.first + s.n == 3000000, "huge: end reached");
    for (int i = 0; i < 20; i++) wheel(-33);
    inv("huge: up from the end", true);
    ids_reset(BIG);
}

/* Hit testing and hover use the scrolled coordinates: a click must reach the row under the pointer. */
static void test_clicks(gp_co *co) {
    fresh(co, BIG);
    gp_ctx cx = cxr();
    float targets[] = { 0.f, 1.2e6f, 3.1e6f, 55.5f };
    for (int round = 0; round < 4; round++) {
        gp_scroll_to(&cx, root->sc, 0, targets[round]);
        settle(co);
        wheel(17);
        Snap s;
        snap(&s);
        int hits = 0;
        for (size_t k = s.first; k < s.first + s.n && hits < 4; k++) {
            float ry = row_screen_y(&s, k), h = s.list->children[k - s.first]->size.h;
            if (h < 8 || ry < 2 || ry + h > s.view_h - 2) continue;
            root->clicked = (size_t)-1;
            int before = root->clicks;
            float x = LIST_X + 60, y = LIST_Y + ry + h / 2;
            gp_simulate_mouse(app, GP_SIM_MOVE, x, y, 0, 0);
            gp_simulate_mouse(app, GP_SIM_DOWN, x, y, 1, 0);
            gp_simulate_mouse(app, GP_SIM_UP, x, y, 1, 0);
            settle(co);
            CHECK(root->clicks == before + 1 && root->clicked == root->ids[k],
                  "click on row %zu (item %zu, y %.1f): got item %zu after %d click(s)", k, root->ids[k], y,
                  root->clicked, root->clicks - before);
            snap(&s);   /* a click rebuilds nothing, but make sure the view is still the one we clicked in */
            hits++;
        }
        CHECK(hits >= 3, "round %d: only %d clickable rows", round, hits);
    }
}

static void test_stick(gp_co *co) {
    root->mode = STICK;
    ids_reset(50);
    notify();
    settle(co);
    gp_ctx cx = cxr();
    Snap s;
    inv("stick: start", true);
    snap(&s);
    CHECK(s.max > 0 && s.y >= s.max - 0.01f && s.first + s.n == 50, "a sticky list starts at its end (y %.1f of %.1f)", s.y, s.max);

    ids_splice(root->n, 0, 10);                    /* rows arrive at the end: it follows */
    notify();
    settle(co);
    inv("stick: appended", true);
    snap(&s);
    CHECK(s.y >= s.max - 0.01f && s.first + s.n == 60, "followed the appended rows (y %.1f of %.1f, rows to %zu)", s.y, s.max, s.first + s.n);

    root->ov_id = root->ids[59];                   /* the last row grows: still at the end */
    root->ov_h = 120;
    notify();
    settle(co);
    inv("stick: last row grew", false);
    snap(&s);
    CHECK(s.y >= s.max - 0.01f, "stayed at the end when the last row grew");
    root->ov_h = 0;
    notify();
    settle(co);

    wheel(-130);                                   /* the user scrolls up: released */
    settle(co);
    snap(&s);
    CHECK(s.y < s.max - 50, "wheel up left the end");
    size_t r = top_row(&s);
    float at = row_screen_y(&s, r);
    ids_splice(root->n, 0, 10);
    notify();
    settle(co);
    inv("stick: released", true);
    snap(&s);
    CHECK(top_row(&s) == r && fabsf(row_screen_y(&s, r) - at) < 0.05f, "released list moved when rows were appended");

    gp_scroll_to_end(&cx, root->sc_stick);         /* and taken hold of again */
    settle(co);
    ids_splice(root->n, 0, 4);
    notify();
    settle(co);
    snap(&s);
    CHECK(s.y >= s.max - 0.01f && s.first + s.n == root->n, "scroll_to_end did not take hold again");

    wheel(-1000);                                  /* scrolling back to the end counts too */
    settle(co);
    for (int i = 0; i < 80; i++) wheel(60);
    settle(co);
    ids_splice(root->n, 0, 4);
    notify();
    settle(co);
    inv("stick: back at the end", true);
    snap(&s);
    CHECK(s.y >= s.max - 0.01f && s.first + s.n == root->n, "scrolling to the end by hand did not take hold");
    root->mode = FIXED;
}

/* ------------------------------------------------------------ driver */

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    gp_sleep(co, 0.3);
    test_start(co);
    test_sequential(co);
    test_small_moves_after_a_jump(co);
    test_rows_above_change(co);
    test_end(co);
    test_exact_extent(co);
    test_splice(co);
    test_invalidate(co);
    test_scroll_to_item(co);
    test_small_lists(co);
    test_huge(co);
    test_clicks(co);
    test_width(co);
    test_stick(co);
    gp_quit(app);
}

static void ready(gp_app *a_, void *userdata) {
    app = a_;
    Root init = { 0 };
    root_entity = gp_view_new(app, sizeof init, &init, view, NULL);
    root = gp_entity_get(app, root_entity);
    root->sc = gp_scroll_new(app);
    root->sc_wrap = gp_scroll_new(app);
    root->sc_stick = gp_scroll_new(app);
    root->wrap_w = 300;
    ids_reset(BIG);
    gp_open_window(app, &(gp_window_options){ .title = "list_test", .width = 340, .height = 340 }, root_entity);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);   /* a physical pointer over the window is not part of the test */
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "list_test: %d FAILURE(S)\n" : "list_test: all checks passed\n", failures);
    return failures != 0;
}
