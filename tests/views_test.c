/*
 * Test of per-view dirty tracking and cached views (gp_child_view_cached). Needs a display.
 *
 * Every view counts its render calls and puts "v<id>=<value>" in its element tree, so the tests can
 * see both which render functions ran and what ended up on screen. The last scenario drives a random
 * tree of views with random changes and compares every render count with a small model of the rules
 * (written independently of views.c): a view renders when it is not cached, or it was notified, or a
 * view built inside it was, or everything was invalidated; and no view ever shows stale state.
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

static gp_app *app;
static gp_co *co_;

/* ------------------------------------------------------------ fixture */

enum { MAXV = 64, MAXK = 8 };
#define HOVER_C gp_rgb(0x123456)
#define FOCUS_C gp_rgb(0x654321)

typedef struct {
    int id;
    int renders;
    int value;              /* what the view shows; a change must be followed by a notify */
    int shown;              /* the value at its last render */
    int kids[MAXK], nkids;
    bool kid_cached[MAXK];
    bool hidden;            /* the parent leaves it out */
    bool reads_focus;
    gp_focus focus;
    bool focused_seen;
    bool has_scroller, has_list, has_button, has_layout_probe;
    gp_entity input;        /* a text field built inside it, if set */
    bool hover, focusable;  /* paint depends on the mouse / on focus */
    float child_opacity, child_clip_h;   /* what it wraps its kids in: inherited opacity, a clip of this height */
    bool boxed;             /* a root of fixed size: what is measured inside it sees the same constraints every time */
    bool out_hover;         /* a hover-styled box below its own box (absolute): the mouse can be over the view's painting and not over the view */
    bool long_text, auto_list;   /* content whose size depends on the inherited text style / measured rows */
    float child_font;       /* text size it gives its kids, 0 = inherit */
    gp_scroll scroll, list_scroll;
    int row_views[4], n_row_views;   /* the first rows of its list show these views */
    int layouts;
    int clicks;
} View;

static gp_entity ents[MAXV];
static int nviews;
static gp_entity root_ent;

static View *V(int id) { return gp_entity_get(app, ents[id]); }
static gp_window *win(void) { return app->windows[0]; }

static void on_layout(void *self, gp_ctx *cx, gp_size size, void *data) { ((View *)self)->layouts++; }
static void on_click(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    View *v = self;
    v->clicks++;
    v->value++;
    gp_notify(cx);
}

static gp_node *list_row(void *self, gp_ctx *cx, size_t i, void *data) {
    View *v = self;
    if (i < (size_t)v->n_row_views) return gp_div(gp_h(24), gp_child_view(ents[v->row_views[i]]));
    return gp_div(gp_h((float)(14 + (i * 7) % 23)), gp_text(gp_fmt("row %zu", i)));
}

static gp_node *render(void *self, gp_ctx *cx) {
    View *v = self;
    v->renders++;
    v->shown = v->value;
    if (v->reads_focus) v->focused_seen = gp_is_focused(cx, v->focus);

    gp_node *kids[MAXK + 4];
    size_t n = 0;
    for (int k = 0; k < v->nkids; k++) {
        if (V(v->kids[k])->hidden) continue;
        gp_node *kid = v->kid_cached[k] ? gp_child_view_cached(ents[v->kids[k]]) : gp_child_view(ents[v->kids[k]]);
        bool wrap = v->child_font > 0 || v->child_opacity > 0 || v->child_clip_h > 0;
        kids[n++] = wrap ? gp_div(v->child_font > 0 ? gp_text_size(v->child_font) : NULL,
                                  v->child_opacity > 0 ? gp_opacity(v->child_opacity) : NULL,
                                  v->child_clip_h > 0 ? gp_h(v->child_clip_h) : NULL,
                                  v->child_clip_h > 0 ? gp_overflow_hidden() : NULL,
                                  v->child_clip_h > 0 ? gp_div(gp_h(200), gp_flex_shrink_0(), kid) : kid)   /* the kid sees the same space whatever the clip */
                         : kid;
    }
    if (v->long_text)
        kids[n++] = gp_div(gp_w(120), gp_text("the quick brown fox jumps over the lazy dog and then does it all over again"));
    if (v->auto_list)
        kids[n++] = gp_list(6, 20, list_row, NULL, gp_track_scroll(v->list_scroll));
    if (v->out_hover)
        kids[n++] = gp_div(gp_absolute(), gp_left(0), gp_top(70), gp_w(60), gp_h(20), gp_hover_bg(HOVER_C));
    if (v->input.generation) kids[n++] = gp_child_view(v->input);
    if (v->has_scroller)
        kids[n++] = gp_div(gp_h(60), gp_overflow_y_scroll(), gp_track_scroll(v->scroll),
                           gp_div(gp_h(500), gp_text("scroller")));
    if (v->has_list)
        kids[n++] = gp_list(400, 20, list_row, NULL, gp_track_scroll(v->list_scroll), gp_h(80));
    gp_node **arr = gp_frame_alloc((n ? n : 1) * sizeof *arr);
    memcpy(arr, kids, n * sizeof *arr);
    return gp_div(gp_flex_col(), v->has_layout_probe ? gp_on_layout(on_layout) : NULL,
                  v->boxed ? gp_w(200) : NULL, v->boxed ? gp_h(120) : NULL,
                  v->hover ? gp_hover_bg(HOVER_C) : NULL,
                  v->focusable ? gp_track_focus(v->focus) : NULL, v->focusable ? gp_focus_bg(FOCUS_C) : NULL,
                  gp_text(gp_fmt("v%d=%d%s", v->id, v->value,
                                 v->reads_focus ? (v->focused_seen ? " focused" : " unfocused") : "")),
                  v->has_button ? gp_div(gp_h(20), gp_w(80), gp_on_click(on_click), gp_text("click")) : NULL,
                  gp_children(arr, n));
}

static int add_view(void) {
    int id = nviews++;
    ents[id] = gp_view_new(app, sizeof(View), &(View){ .id = id }, render, NULL);
    return id;
}
static void add_kid(int parent, int kid, bool cached) {
    View *p = V(parent);
    p->kids[p->nkids] = kid;
    p->kid_cached[p->nkids++] = cached;
}

static void reset_counts(void) {
    for (int i = 0; i < nviews; i++) {
        if (!V(i)) continue;   /* released */
        V(i)->renders = 0;
        V(i)->layouts = 0;
    }
}
/* ---- the frame caches must be invisible: a frame painted with them equals one painted without, and a layout
 * that skipped cached subtrees equals a full layout of the same tree. Checked after every settle. */
static const char *where = "";
static bool layout_diff = true;   /* off while an auto-height list is shown: its second layout legitimately differs from its first */
static float geo_a[6 * 8192], geo_b[6 * 8192];
static uint32_t gens[8192];

/* Geometry of the tree, node by node. The rows of a list are left out: the wheel realizes the rows around its
 * new position in place, a layout realizes them afresh, and the two build slightly different sets of rows for
 * the same view (list_test checks those). The list itself is compared by its box and its absolute offset. */
static bool has_list;
static bool compare_list_extent;   /* only right after a layout pass: the wheel realizes rows without one, and the extent it leaves is an older estimate */
static size_t snapshot(gp_el *n, float *geo, uint32_t *gen, size_t at) {
    if (at >= 8192) return at;
    geo[at * 6 + 0] = n->pos.x; geo[at * 6 + 1] = n->pos.y; geo[at * 6 + 2] = n->size.w; geo[at * 6 + 3] = n->size.h;
    bool loose = n->kind == GP_EL_LIST && !compare_list_extent;
    geo[at * 6 + 4] = loose ? 0 : n->kind == GP_EL_LIST ? (float)(n->scroll_base_y + n->scroll.y) : n->scroll.y;
    geo[at * 6 + 5] = loose ? 0 : n->scroll_max.y;
    if (gen) gen[at] = n->layout_gen;
    at++;
    if (n->kind == GP_EL_LIST) { has_list = true; return at; }
    for (uint32_t i = 0; i < n->nchildren; i++) at = snapshot(n->children[i], geo, gen, at);
    return at;
}
static size_t restore_gens(gp_el *n, const uint32_t *gen, size_t at) {
    if (at >= 8192) return at;
    n->layout_gen = gen[at++];
    if (n->kind == GP_EL_LIST) return at;
    for (uint32_t i = 0; i < n->nchildren; i++) at = restore_gens(n->children[i], gen, at);
    return at;
}

static void verify_frame(void) {
    gp_window *w = win();
    if (!w->tree || w->dirty_render || w->dirty_paint) return;   /* a frame is still on its way */

    gp_scene fresh = { 0 };
    gp_arena scratch = { 0 };
    gp_paint_tree(&fresh, &scratch, app->text, w->tree, w->scale, w->mouse, w->mouse_in, w->pressed, w->focused, w->drag_key, w->scroll_vis_gen, NULL, NULL);
    bool same = fresh.len == w->scene.len && memcmp(fresh.items, w->scene.items, fresh.len * sizeof *fresh.items) == 0;
    if (!same) {
        size_t i = 0;
        while (i < fresh.len && i < w->scene.len && memcmp(&fresh.items[i], &w->scene.items[i], sizeof *fresh.items) == 0) i++;
        CHECK(false, "[%s] the frame painted with caches differs from one painted without: %zu vs %zu instances, first difference at %zu", where, w->scene.len, fresh.len, i);
        if (i < fresh.len && i < w->scene.len) {
            const gp_instance *a = &w->scene.items[i], *b = &fresh.items[i];
            printf("   cached: kind %.0f bounds %.2f %.2f %.2f %.2f color %.2f %.2f %.2f %.2f\n", a->kind, a->bounds[0], a->bounds[1], a->bounds[2], a->bounds[3], a->color[0], a->color[1], a->color[2], a->color[3]);
            printf("   mouse %.1f,%.1f in=%d drag_key=%llx pressed=%d\n", w->mouse.x, w->mouse.y, w->mouse_in, (unsigned long long)w->drag_key, w->pressed);
            for (size_t q = 0; q < w->views.cap; q++) {
                gp_vview *vv = &w->views.v[q];
                if (!vv->entity.generation || !vv->chunk_valid) continue;
                for (size_t k = 0; k < vv->nchunk; k++)
                    if (memcmp(&vv->chunk[k], a, sizeof *a) == 0) {
                        float sg[8]; memcpy(sg, vv->chunk_sig, sizeof sg);
                        printf("   in the painted copy of view entity %u: flags %d, pos %.1f,%.1f size %.1fx%.1f, ext %.1f %.1f %.1f %.1f, painted at origin %.1f,%.1f, dyn_gen %u, sig dyn %u drag %x, layout_gen %u chunk_gen %u\n",
                               vv->entity.index, vv->root->sub_flags, vv->root->pos.x, vv->root->pos.y, vv->root->size.w, vv->root->size.h,
                               vv->root->ext[0], vv->root->ext[1], vv->root->ext[2], vv->root->ext[3], sg[0], sg[1], vv->root->dyn_gen, vv->chunk_sig[11], vv->chunk_sig[12], vv->root->layout_gen, vv->chunk_gen);
                        for (uint32_t c = 0; c < vv->root->nchildren; c++) {
                            gp_el *ch = vv->root->children[c];
                            printf("      child %u: kind %d pos %.1f,%.1f size %.1fx%.1f scroll %.1f max %.1f\n", c, ch->kind, ch->pos.x, ch->pos.y, ch->size.w, ch->size.h, ch->scroll.y, ch->scroll_max.y);
                        }
                        break;
                    }
            }
            printf("   fresh:  kind %.0f bounds %.2f %.2f %.2f %.2f color %.2f %.2f %.2f %.2f\n", b->kind, b->bounds[0], b->bounds[1], b->bounds[2], b->bounds[3], b->color[0], b->color[1], b->color[2], b->color[3]);
        }
    }
    free(fresh.items);
    gp_arena_free(&scratch);

    if (!layout_diff) return;
    has_list = false;
    size_t n0 = snapshot(w->tree, geo_a, gens, 0);
    gp_debug_frame_caches(false);
    gp_layout_root(w->tree, app->text, w->size, &w->scrolls, NULL, NULL);
    size_t n1 = snapshot(w->tree, geo_b, NULL, 0);
    gp_debug_frame_caches(true);
    gp_layout_root(w->tree, app->text, w->size, &w->scrolls, NULL, NULL);   /* leaves the skip state as it was: valid */
    /* A full layout rebuilds the rows of a list, and what was painted was painted for the old ones: the next frame
     * is drawn from scratch. Without lists the layout is the same and the painted copies stay valid. */
    if (has_list) {
        w->dirty_paint = true;
        gp_sleep(co_, 0.05);
    } else {
        restore_gens(w->tree, gens, 0);
    }
    if (n0 != n1) { CHECK(false, "[%s] a full layout changed the number of nodes (%zu vs %zu)", where, n0, n1); return; }
    for (size_t i = 0; i < n0 * 6; i++)
        if (fabsf(geo_a[i] - geo_b[i]) > 0.01f) {
            CHECK(false, "[%s] node %zu: layout with cached subtrees skipped has %s %.2f, a full layout %.2f", where, i / 6,
                  (const char *[]){ "x", "y", "width", "height", "scroll offset", "scroll extent" }[i % 6], geo_a[i], geo_b[i]);
            break;
        }
}

/* Sleeps, then until the window has drawn what it was asked to (a sanitizer build is slow), then checks the frame. */
static void settle_for(double secs) {
    gp_sleep(co_, secs);
    for (int i = 0; i < 150 && (win()->dirty_render || win()->dirty_paint); i++) gp_sleep(co_, 0.02);
    verify_frame();
}
static void settle(void) { settle_for(0.08); }
static void settle_fast(void) { settle_for(0.04); }
static void notify_view(int id) { gp_ctx cx = gp_ctx_for(app, ents[id]); gp_notify(&cx); }
static void change(int id) { V(id)->value++; notify_view(id); }

static bool tree_has(gp_el *n, const char *text) {
    if (n->text && strcmp(n->text, text) == 0) return true;
    for (uint32_t i = 0; i < n->nchildren; i++) if (tree_has(n->children[i], text)) return true;
    return false;
}
static int tree_count(gp_el *n, const char *text) {
    int c = n->text && strcmp(n->text, text) == 0;
    for (uint32_t i = 0; i < n->nchildren; i++) c += tree_count(n->children[i], text);
    return c;
}
/* the element whose first text child is `text`, with its absolute origin */
static gp_el *find_with_text(gp_el *n, const char *text, float ox, float oy, float *ax, float *ay) {
    float x = ox + n->pos.x, y = oy + n->pos.y;
    for (uint32_t i = 0; i < n->nchildren; i++)
        if (n->children[i]->text && strcmp(n->children[i]->text, text) == 0) { *ax = x; *ay = y; return n; }
    for (uint32_t i = 0; i < n->nchildren; i++) {
        gp_el *r = find_with_text(n->children[i], text, x - n->scroll.x, y - n->scroll.y, ax, ay);
        if (r) return r;
    }
    return NULL;
}
static gp_el *find_kind(gp_el *n, int kind) {
    if (n->kind == kind) return n;
    for (uint32_t i = 0; i < n->nchildren; i++) {
        gp_el *r = find_kind(n->children[i], kind);
        if (r) return r;
    }
    return NULL;
}

/* A tree has no node twice: a cached view reused in two places would be one node laid out in both. */
static bool collect_nodes(gp_el *n, gp_el **seen, size_t *count, size_t cap) {
    for (size_t i = 0; i < *count; i++) if (seen[i] == n) return false;
    if (*count == cap) return true;
    seen[(*count)++] = n;
    for (uint32_t i = 0; i < n->nchildren; i++)
        if (!collect_nodes(n->children[i], seen, count, cap)) return false;
    return true;
}
static bool tree_is_a_tree(void) {
    static gp_el *seen[4096];
    size_t count = 0;
    return collect_nodes(win()->tree, seen, &count, 4096);
}

/* Two element trees laid out identically? */
static bool same_layout(const gp_el *a, const gp_el *b, char *why, size_t why_len) {
    if (a->kind != b->kind || a->nchildren != b->nchildren) { snprintf(why, why_len, "structure differs"); return false; }
    if (fabsf(a->size.w - b->size.w) > 0.01f || fabsf(a->size.h - b->size.h) > 0.01f) {
        snprintf(why, why_len, "size %.2fx%.2f vs %.2fx%.2f (%s)", a->size.w, a->size.h, b->size.w, b->size.h, a->text ? a->text : "div");
        return false;
    }
    if (fabsf(a->pos.x - b->pos.x) > 0.01f || fabsf(a->pos.y - b->pos.y) > 0.01f) {
        snprintf(why, why_len, "position %.2f,%.2f vs %.2f,%.2f", a->pos.x, a->pos.y, b->pos.x, b->pos.y);
        return false;
    }
    for (uint32_t i = 0; i < a->nchildren; i++)
        if (!same_layout(a->children[i], b->children[i], why, why_len)) return false;
    return true;
}

static char vtext[48];
static const char *vt(int id) { snprintf(vtext, sizeof vtext, "v%d=%d", id, V(id)->value); return vtext; }

/* `want[i]` renders for view i since the last reset, for the first `n` views. */
static void expect(const char *what, int n, const int *want) {
    for (int i = 0; i < n; i++)
        CHECK(V(i)->renders == want[i], "%s: view %d rendered %d times, expected %d", what, i, V(i)->renders, want[i]);
}
#define EXPECT(what, ...) do { int w_[] = { __VA_ARGS__ }; expect(what, (int)(sizeof w_ / sizeof *w_), w_); } while (0)

/* ------------------------------------------------------------ scenarios */

/*   0 R ── 1 A (cached) ── 4 A1
 *      ├── 2 B
 *      └── 3 C (cached) ── 5 C1 (cached)                                                           */
static void build_fixed_tree(void) {
    for (int i = 0; i < 6; i++) add_view();
    add_kid(0, 1, true);
    add_kid(0, 2, false);
    add_kid(0, 3, true);
    add_kid(1, 4, false);
    add_kid(3, 5, true);
}

static void test_dirty_paths(void) {
    where = "test_dirty_paths";
    settle();
    EXPECT("first build", 1, 1, 1, 1, 1, 1);   /* R A B C A1 C1: ids 0..5 are R A B C A1 C1 */
    CHECK(win()->views.cap > 0, "no view table");

    reset_counts();
    notify_view(0);
    settle();
    EXPECT("root notified", 1, 0, 1, 0, 0, 0);   /* B is not cached: it renders with its parent */

    reset_counts();
    notify_view(1);
    settle();
    EXPECT("cached A notified", 1, 1, 1, 0, 1, 0);   /* A1 is built by A's render; R and B because R is an ancestor */

    reset_counts();
    notify_view(4);
    settle();
    EXPECT("A1 (inside cached A) notified", 1, 1, 1, 0, 1, 0);   /* A has a dirty view below it */

    reset_counts();
    notify_view(5);
    settle();
    EXPECT("C1 (cached, inside cached C) notified", 1, 0, 1, 1, 0, 1);

    reset_counts();
    notify_view(2);
    settle();
    EXPECT("B notified", 1, 0, 1, 0, 0, 0);

    /* a plain entity nobody observes: nothing renders, nothing is drawn */
    gp_entity model = gp_entity_new(app, 8, NULL, NULL);
    reset_counts();
    unsigned long draws = app->stat_draws;
    gp_ctx mc = gp_ctx_for(app, model);
    gp_notify(&mc);
    settle();
    EXPECT("model notified", 0, 0, 0, 0, 0, 0);
    CHECK(app->stat_draws == draws, "a model nobody observes caused %lu draws", app->stat_draws - draws);

    /* a view observing a model re-renders when the model notifies, and only the views above it besides */
    gp_ctx c4 = gp_ctx_for(app, ents[4]);
    gp_observe(&c4, model, NULL, NULL);
    reset_counts();
    gp_notify(&mc);
    settle();
    EXPECT("model observed by A1", 1, 1, 1, 0, 1, 0);

    /* the app itself: everything, caches included */
    reset_counts();
    gp_app_notify(app);
    settle();
    EXPECT("gp_app_notify", 1, 1, 1, 1, 1, 1);
    reset_counts();
    gp_ctx none = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_notify(&none);
    settle();
    EXPECT("notify with no entity", 1, 1, 1, 1, 1, 1);

    gp_entity_release(app, model);
    settle();
}

static bool scene_has_color(gp_color c) {
    gp_scene *sc = &win()->scene;
    for (size_t i = 0; i < sc->len; i++)
        if (sc->items[i].kind == GP_PRIM_QUAD && fabsf(sc->items[i].color[0] - c.r) < 0.003f &&
            fabsf(sc->items[i].color[1] - c.g) < 0.003f && fabsf(sc->items[i].color[2] - c.b) < 0.003f && sc->items[i].color[3] > 0)
            return true;
    return false;
}

typedef struct { unsigned long skips, hits, misses; } Counters;
static Counters counters(void) { return (Counters){ gp_layout_skips(), app->stat_paint_hits, app->stat_paint_misses }; }
/* wait for a frame without checking it: the counters must be read before the check re-lays-out the tree */
/* Waits for the next draw and returns right after it, so that a second frame (a blink, a late event) has no time to
 * get in before the counters are read; a slow machine (a sanitizer build) takes longer, not differently. */
static void let_frame_pass(void) {
    unsigned long draws = app->stat_draws;
    for (int i = 0; i < 1000 && app->stat_draws == draws; i++) gp_sleep(co_, 0.004);
    gp_sleep(co_, 0.004);
}

/* What layout and paint of a cached view skip, and exactly when they must not. The frame after each
 * step is checked against a full layout and a paint without caches (settle -> verify_frame). */
static void test_frame_caches(void) {
    where = "test_frame_caches";
    notify_view(2); settle();
    notify_view(2); settle();                       /* warm: every cached view has laid out and painted */

    Counters c0 = counters();
    notify_view(2);                                 /* B: not cached; A and C beside it are untouched */
    let_frame_pass();
    Counters c1 = counters();
    CHECK(c1.skips - c0.skips == 2, "an unrelated notify skipped %lu cached layouts, want 2 (A and C)", c1.skips - c0.skips);
    CHECK(c1.hits - c0.hits == 2 && c1.misses == c0.misses, "an unrelated notify reused %lu paints and made %lu, want 2 and 0", c1.hits - c0.hits, c1.misses - c0.misses);
    verify_frame();

    /* the window gets wider: what was laid out for another width is laid out again, and so painted again */
    c0 = counters();
    win()->size.w += 100;
    win()->dirty_render = true;
    let_frame_pass();
    c1 = counters();
    CHECK(c1.skips == c0.skips, "a resize skipped %lu cached layouts", c1.skips - c0.skips);
    CHECK(c1.hits == c0.hits, "a resize reused %lu painted subtrees", c1.hits - c0.hits);
    verify_frame();
    win()->size.w -= 100;
    win()->dirty_render = true;
    settle();

    /* a parent that starts to fade the view: the layout is the same, and so is what the view paints (into the parent's layer) */
    V(0)->child_opacity = 0.9f;                     /* the kids now sit in a wrapper: that is a change of layout once */
    notify_view(0);
    settle();
    V(0)->child_opacity = 0.5f;
    notify_view(0);
    c0 = counters();
    let_frame_pass();
    c1 = counters();
    CHECK(c1.skips - c0.skips >= 1, "a parent's opacity change re-laid-out cached views that did not change size");
    /* (the parent fades as a group: the kids are drawn into its layer as they were, at full opacity, so what they painted is used again) */
    CHECK(c1.hits > c0.hits && c1.misses == c0.misses, "a parent's opacity change reused %lu painted subtrees and made %lu: the kids are the same in its layer",
          c1.hits - c0.hits, c1.misses - c0.misses);
    verify_frame();
    V(0)->child_opacity = 0;
    notify_view(0);
    settle();

    /* a parent that clips it: the view is laid out for the same space, only the clip around it changes */
    V(0)->child_clip_h = 30;
    notify_view(0);
    settle();
    V(0)->child_clip_h = 60;
    notify_view(0);
    c0 = counters();
    let_frame_pass();
    c1 = counters();
    CHECK(c1.skips - c0.skips >= 1, "a clip change re-laid-out cached views given the same space");
    CHECK(c1.hits == c0.hits, "a clip change reused %lu painted subtrees drawn with the old clip", c1.hits - c0.hits);
    verify_frame();
    V(0)->child_clip_h = 0;
    notify_view(0);
    settle();

    /* something before it appears: same layout, different place */
    V(2)->hidden = true;                            /* B is between A and C */
    notify_view(0);
    c0 = counters();
    let_frame_pass();
    c1 = counters();
    CHECK(c1.hits - c0.hits <= 1, "C moved up and its painted instances were reused (%lu hits): they hold the old position", c1.hits - c0.hits);
    verify_frame();
    V(2)->hidden = false;
    notify_view(0);
    settle();

    /* hover: a mouse over a subtree with hover styles is painted in full, and what is painted is not kept */
    V(1)->hover = true;
    notify_view(1);
    settle();
    float x, y;
    char key[24];
    snprintf(key, sizeof key, "v1=%d", V(1)->value);
    gp_el *a = find_with_text(win()->tree, key, 0, 0, &x, &y);
    CHECK(a != NULL, "view A not found");
    if (a) {
        CHECK(!scene_has_color(HOVER_C), "hover color painted with no mouse over the view");
        gp_simulate_mouse(app, GP_SIM_MOVE, x + 4, y + 4, 0, 0);
        settle();
        CHECK(scene_has_color(HOVER_C), "no hover color with the mouse over a cached view");
        gp_simulate_mouse(app, GP_SIM_MOVE, x + 4, y + 8, 0, 0);
        settle();
        CHECK(scene_has_color(HOVER_C), "no hover color after the mouse moved inside the view");
        gp_simulate_mouse(app, GP_SIM_MOVE, 330, 590, 0, 0);
        settle();
        CHECK(!scene_has_color(HOVER_C), "hover color left behind after the mouse moved away (a painted copy kept from the hovered frame)");
        c0 = counters();
        notify_view(2);
        let_frame_pass();
        c1 = counters();
        CHECK(c1.hits - c0.hits >= 1, "painting was not reused for a view with hover styles while the mouse is elsewhere");
        verify_frame();
        /* pressing counts too */
        gp_simulate_mouse(app, GP_SIM_MOVE, x + 4, y + 4, 0, 0);
        gp_simulate_mouse(app, GP_SIM_DOWN, x + 4, y + 4, 1, 0);
        settle();
        gp_simulate_mouse(app, GP_SIM_UP, x + 4, y + 4, 1, 0);
        gp_simulate_mouse(app, GP_SIM_MOVE, 330, 590, 0, 0);
        settle();
    }
    V(1)->hover = false;
    notify_view(1);
    settle();

    /* focus: not a re-render of the view (it does not look at focus), but its painting changes */
    int f = add_view();
    V(f)->focusable = true;
    V(f)->focus = gp_focus_new(app);
    add_kid(0, f, true);
    notify_view(0);
    settle();
    notify_view(2);
    settle();
    CHECK(!scene_has_color(FOCUS_C), "focus color painted without focus");
    reset_counts();
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_focus_set(&cx, V(f)->focus);
    settle();
    CHECK(scene_has_color(FOCUS_C), "a cached view with a focusable element does not show focus: its painted copy is stale");
    CHECK(V(f)->renders == 0, "the view rendered for a focus change although it never looks at focus");
    gp_focus_set(&cx, (gp_focus){ 0 });
    settle();
    CHECK(!scene_has_color(FOCUS_C), "focus color left behind after focus moved away");
    V(0)->nkids--;
    notify_view(0);
    settle();

    /* the glyph atlas was cleared: remembered atlas coordinates are stale */
    c0 = counters();
    gp_text_atlas_reset(app->text);
    win()->dirty_paint = true;
    let_frame_pass();
    c1 = counters();
    CHECK(c1.hits == c0.hits, "painted instances with atlas coordinates from before an atlas reset were reused (%lu)", c1.hits - c0.hits);
    verify_frame();

    /* a different scale */
    win()->scale = 2.f;
    win()->dirty_render = true;
    c0 = counters();
    let_frame_pass();
    c1 = counters();
    CHECK(c1.hits == c0.hits, "painted instances from another scale were reused (%lu)", c1.hits - c0.hits);
    verify_frame();
    win()->scale = 1.f;
    win()->dirty_render = true;
    settle();
}

static void test_screen_matches_state(void) {
    where = "test_screen_matches_state";
    for (int round = 0; round < 3; round++)
        for (int id = 0; id < 6; id++) {
            change(id);
            settle();
            for (int k = 0; k < 6; k++)
                CHECK(tree_has(win()->tree, vt(k)), "after changing view %d: view %d does not show \"%s\"", id, k, vt(k));
        }
    /* changing state without a notify is the caller's bug; the cached view keeps showing the old value */
    V(1)->value = 999;
    notify_view(2);
    settle();
    CHECK(!tree_has(win()->tree, "v1=999"), "a cached view showed state that was never notified");
    notify_view(1);
    settle();
    CHECK(tree_has(win()->tree, "v1=999"), "a cached view did not show state after its notify");
}

static void test_same_view_twice(void) {
    where = "test_same_view_twice";
    int d = add_view();
    add_kid(0, d, true);
    add_kid(2, d, true);   /* under R and under B */
    notify_view(0);
    settle();
    CHECK(tree_count(win()->tree, vt(d)) == 2, "a cached view placed twice shows %d times, want 2", tree_count(win()->tree, vt(d)));
    CHECK(tree_is_a_tree(), "the same element node is in the tree twice (one cached tree used in two places)");
    change(d);
    settle();
    CHECK(tree_count(win()->tree, vt(d)) == 2, "after a change it shows %d times, want 2", tree_count(win()->tree, vt(d)));
    for (int i = 0; i < 3; i++) { notify_view(0); settle(); }
    CHECK(tree_count(win()->tree, vt(d)) == 2, "after rebuilds it shows %d times, want 2", tree_count(win()->tree, vt(d)));
    CHECK(tree_is_a_tree(), "after rebuilds the same element node is in the tree twice");
    /* both places have their own geometry */
    float x1, y1, x2, y2;
    gp_el *e1 = find_with_text(win()->tree, vt(d), 0, 0, &x1, &y1);
    CHECK(e1 != NULL, "view not found");
    V(0)->nkids--;   /* remove it from R again: it stays under B only */
    notify_view(0);
    settle();
    CHECK(tree_count(win()->tree, vt(d)) == 1, "after removing one place: %d", tree_count(win()->tree, vt(d)));
    (void)x2; (void)y2;
    V(2)->nkids--;
    notify_view(0);
    settle();
    CHECK(tree_count(win()->tree, vt(d)) == 0, "after removing both places: %d", tree_count(win()->tree, vt(d)));
}

static void test_hide_show_release(void) {
    where = "test_hide_show_release";
    int e = add_view();
    add_kid(0, e, true);
    notify_view(0);
    settle();
    CHECK(tree_has(win()->tree, vt(e)), "new view not shown");
    CHECK(gp_views_find(&win()->views, ents[e]) != NULL, "no table entry for a shown view");

    V(e)->hidden = true;
    notify_view(0);
    settle();
    CHECK(!tree_has(win()->tree, vt(e)), "hidden view still shown");
    CHECK(gp_views_find(&win()->views, ents[e]) == NULL, "a hidden view kept its table entry (and its cached tree)");

    reset_counts();
    V(e)->value = 5;                  /* changed while hidden: no notify to anybody */
    V(e)->hidden = false;
    notify_view(0);
    settle();
    CHECK(V(e)->renders == 1, "a view shown again rendered %d times, want 1 (its cache was dropped)", V(e)->renders);
    CHECK(tree_has(win()->tree, vt(e)), "a view shown again shows nothing");

    /* releasing a shown cached view repaints the window without it, with nobody else notified */
    unsigned long draws = app->stat_draws;
    V(0)->nkids--;                    /* the parent stops building it, but is not told: the release alone must rebuild */
    gp_entity_release(app, ents[e]);
    settle();
    CHECK(app->stat_draws > draws, "releasing a shown view did not rebuild the window");
    CHECK(!tree_has(win()->tree, "v6=5") && !tree_has(win()->tree, "v7=5"), "a released view is still shown");
}

static void test_focus(void) {
    where = "test_focus";
    int f = add_view(), g = add_view();
    V(f)->reads_focus = true;
    V(f)->focus = gp_focus_new(app);
    V(g)->reads_focus = false;
    add_kid(0, f, true);
    add_kid(0, g, true);
    notify_view(0);
    settle();
    char want[48];
    reset_counts();
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_focus_set(&cx, V(f)->focus);
    settle();
    snprintf(want, sizeof want, "v%d=0 focused", f);
    CHECK(V(f)->renders == 1 && tree_has(win()->tree, want), "the view that reads focus rendered %d times and %s", V(f)->renders, tree_has(win()->tree, want) ? "shows focus" : "shows no focus");
    CHECK(V(g)->renders == 0, "a cached view that does not read focus rendered %d times on a focus change", V(g)->renders);
    CHECK(V(1)->renders == 0 && V(3)->renders == 0, "unrelated cached views rendered on a focus change");
    reset_counts();
    gp_focus_set(&cx, (gp_focus){ 0 });
    settle();
    snprintf(want, sizeof want, "v%d=0 unfocused", f);
    CHECK(V(f)->renders == 1 && tree_has(win()->tree, want), "losing focus: %d renders", V(f)->renders);
    V(0)->nkids -= 2;
    notify_view(0);
    settle();
}

static void test_scroll_list_layout_in_cached_views(void) {
    where = "test_scroll_list_layout_in_cached_views";
    int s = add_view();
    V(s)->has_scroller = true;
    V(s)->has_list = true;
    V(s)->has_layout_probe = true;
    V(s)->scroll = gp_scroll_new(app);
    V(s)->list_scroll = gp_scroll_new(app);
    add_kid(0, s, true);
    notify_view(0);
    settle();
    gp_ctx cx = gp_ctx_for(app, ents[s]);

    gp_scroll_to(&cx, V(s)->scroll, 0, 250);
    settle();
    CHECK(gp_scroll_offset(&cx, V(s)->scroll).y == 250, "scroll offset %.0f in a cached view", gp_scroll_offset(&cx, V(s)->scroll).y);

    reset_counts();
    for (int i = 0; i < 4; i++) { notify_view(2); settle(); }   /* rebuilds that reuse the cached view */
    CHECK(V(s)->renders == 0, "the cached view rendered %d times", V(s)->renders);
    CHECK(V(s)->layouts >= 4, "its gp_on_layout handler ran %d times in 4 rebuilds", V(s)->layouts);
    CHECK(gp_scroll_offset(&cx, V(s)->scroll).y == 250, "scroll offset lost across rebuilds: %.0f", gp_scroll_offset(&cx, V(s)->scroll).y);
    gp_el *sc = NULL;
    {   /* the scroller's element still carries the offset after layout */
        gp_el *n = find_kind(win()->tree, GP_EL_DIV);
        (void)n;
    }
    (void)sc;

    /* the list inside the cached view realizes rows, scrolls and jumps, across reused builds */
    gp_el *list = find_kind(win()->tree, GP_EL_LIST);
    CHECK(list && list->nchildren > 0, "no rows realized for the list in a cached view");
    gp_list_scroll_to_item(&cx, V(s)->list_scroll, 200, GP_ALIGN_ROW_START);
    settle();
    list = find_kind(win()->tree, GP_EL_LIST);
    CHECK(list && list->list_first <= 200 && 200 < list->list_first + list->nchildren, "row 200 not realized after scroll_to_item (rows %zu..)", list ? list->list_first : 0);
    for (int i = 0; i < 3; i++) { notify_view(2); settle(); }
    list = find_kind(win()->tree, GP_EL_LIST);
    CHECK(list && list->list_first <= 200 && 200 < list->list_first + list->nchildren, "list position lost across reused builds");
    /* wheeling the scroller and the list inside a cached view: nothing is re-rendered or re-laid-out, yet what is
     * drawn must move (the checks after each settle compare it with a paint without caches) */
    {
        float x, y;
        gp_el *content = find_with_text(win()->tree, "scroller", 0, 0, &x, &y);
        CHECK(content != NULL, "scroller content not found");
        if (content) {
            float before = gp_scroll_offset(&cx, V(s)->scroll).y;
            gp_scroll_wheel(win(), (gp_point){ x + 5, y + 5 + before }, 0, 40);
            settle();
            CHECK(gp_scroll_offset(&cx, V(s)->scroll).y == before + 40, "wheel moved the scroller by %.0f, want 40", gp_scroll_offset(&cx, V(s)->scroll).y - before);
            gp_scroll_wheel(win(), (gp_point){ x + 5, y + 5 + before + 40 }, 0, -15);
            settle();
        }
    }
    reset_counts();
    for (int i = 0; i < 3; i++) { notify_view(2); settle(); }
    CHECK(V(s)->renders == 0, "the cached view with a scroller rendered while others changed");
    V(0)->nkids--;
    notify_view(0);
    settle();
}

/* The scroller / list element with this key, and where its box starts. */
static gp_el *find_scroller(gp_el *n, uint64_t key, float ox, float oy, float *ax, float *ay) {
    float x = ox + n->pos.x, y = oy + n->pos.y;
    if ((n->style.overflow_x_scroll || n->style.overflow_y_scroll) && n->scroll_key == key) { *ax = x; *ay = y; return n; }
    for (uint32_t i = 0; i < n->nchildren; i++) {
        gp_el *r = find_scroller(n->children[i], key, x - n->scroll.x, y - n->scroll.y, ax, ay);
        if (r) return r;
    }
    return NULL;
}

/* Cached views with scrollers and lists skip layout and reuse their painting like any other, and still
 * follow every way a scroller moves: wheel, thumb, gp_scroll_to, list calls. */
static void test_dynamic_subtrees(void) {
    where = "test_dynamic_subtrees";
    gp_simulate_mouse(app, GP_SIM_MOVE, 330, 590, 0, 0);
    int rv[3];
    int s = add_view();
    for (int i = 0; i < 3; i++) { rv[i] = add_view(); V(s)->row_views[i] = rv[i]; }
    V(s)->n_row_views = 3;
    V(s)->has_scroller = V(s)->has_list = true;
    V(s)->scroll = gp_scroll_new(app);
    V(s)->list_scroll = gp_scroll_new(app);
    add_kid(0, s, true);
    notify_view(0); settle();
    notify_view(2); settle();
    gp_ctx cx = gp_ctx_for(app, ents[s]);

    /* untouched by an unrelated change: neither laid out nor painted again */
    Counters c0 = counters();
    notify_view(2);
    let_frame_pass();
    Counters c1 = counters();
    CHECK(c1.skips - c0.skips == 3, "an unrelated notify skipped %lu cached layouts, want 3 (A, C and the one with a scroller and a list)", c1.skips - c0.skips);
    CHECK(c1.hits - c0.hits == 3 && c1.misses == c0.misses, "an unrelated notify reused %lu paints and made %lu, want 3 and 0", c1.hits - c0.hits, c1.misses - c0.misses);
    verify_frame();

    /* the views in rows of a list that was not laid out again are still there to be notified. (No verify_frame
     * between the frames and the checks: it lays everything out, which registers the rows' views afresh.) */
    for (int i = 0; i < 3; i++) {
        notify_view(2); let_frame_pass();
        notify_view(2); let_frame_pass();
        CHECK(gp_views_find(&win()->views, ents[rv[i]]) != NULL, "the view in row %d was forgotten while its list was not laid out", i);
        V(rv[i])->value += 5;
        reset_counts();
        notify_view(rv[i]);
        let_frame_pass();
        char t[32];
        snprintf(t, sizeof t, "v%d=%d", rv[i], V(rv[i])->value);
        CHECK(tree_has(win()->tree, t), "[row view %d] shown stale after the list was skipped (no \"%s\")", i, t);
        CHECK(V(s)->renders <= 1, "the cached view around the list rendered %d times for a change in a row", V(s)->renders);
        verify_frame();
    }

    /* a request to bring a row into view is a change from outside: the next layout of the view does it, though
     * nothing else about the view changed and nothing was measured */
    notify_view(2); let_frame_pass();
    notify_view(2); let_frame_pass();
    gp_list_scroll_to_item(&cx, V(s)->list_scroll, 350, GP_ALIGN_ROW_START);
    let_frame_pass();
    {
        float lx, ly;
        gp_el *list = find_scroller(win()->tree, V(s)->list_scroll.id, 0, 0, &lx, &ly);
        CHECK(list && list->list_first <= 350 && 350 < list->list_first + list->nchildren,
              "gp_list_scroll_to_item(350) on a view that was not laid out again realized rows %zu..", list ? list->list_first : 0);
    }
    verify_frame();

    /* the wheel over the scroller: it moves in place, its view is painted again, and then reused again */
    float x, y;
    gp_el *content = find_with_text(win()->tree, "scroller", 0, 0, &x, &y);
    CHECK(content != NULL, "scroller content not found");
    float before = gp_scroll_offset(&cx, V(s)->scroll).y;
    if (content) {
        gp_scroll_wheel(win(), (gp_point){ x + 5, y + 5 + before }, 0, 40);
        c0 = counters();
        let_frame_pass();
        c1 = counters();
        CHECK(c1.misses - c0.misses == 1, "a wheel step over a scroller painted %lu cached subtrees, want 1", c1.misses - c0.misses);
        CHECK(gp_scroll_offset(&cx, V(s)->scroll).y == before + 40, "the wheel moved the scroller by %.0f", gp_scroll_offset(&cx, V(s)->scroll).y - before);
        verify_frame();
    }
    c0 = counters();
    notify_view(2);
    let_frame_pass();
    c1 = counters();
    CHECK(c1.skips - c0.skips == 3 && c1.hits - c0.hits == 3, "after the wheel, an unrelated notify: %lu layouts skipped, %lu paints reused, want 3 and 3", c1.skips - c0.skips, c1.hits - c0.hits);
    verify_frame();

    /* the wheel over the list scrolls rows in; a jump far away; and back */
    {
        float lx, ly;
        gp_el *list = find_scroller(win()->tree, V(s)->list_scroll.id, 0, 0, &lx, &ly);
        CHECK(list != NULL, "list not found");
        if (list) {
            for (int i = 0; i < 6; i++) {
                gp_scroll_wheel(win(), (gp_point){ lx + 20, ly + 20 }, 0, i < 4 ? 60 : -400);
                settle_fast();
            }
            gp_list_scroll_to_item(&cx, V(s)->list_scroll, 300, GP_ALIGN_ROW_START);
            settle();
            list = find_scroller(win()->tree, V(s)->list_scroll.id, 0, 0, &lx, &ly);
            CHECK(list && list->list_first <= 300 && 300 < list->list_first + list->nchildren, "row 300 not realized after scroll_to_item");
            gp_scroll_wheel(win(), (gp_point){ lx + 20, ly + 20 }, 0, 25);
            settle();
            gp_scroll_wheel(win(), (gp_point){ lx + 20, ly + 20 }, 0, 400);   /* into rows nobody has seen; no check yet: a check lays out */
            let_frame_pass();
            /* rows were measured in place: the next layout of the view is a real one, and then it is skipped again */
            c0 = counters();
            notify_view(2);
            let_frame_pass();
            c1 = counters();
            CHECK(c1.skips - c0.skips == 2, "after rows were measured by the wheel, %lu cached layouts were skipped, want 2 (not the list's)", c1.skips - c0.skips);
            compare_list_extent = true;
            verify_frame();               /* the list's extent is what a full layout gives */
            compare_list_extent = false;
            notify_view(2);
            settle();
            c0 = counters();
            notify_view(2);
            let_frame_pass();
            c1 = counters();
            CHECK(c1.skips - c0.skips == 3, "with nothing new measured, %lu cached layouts were skipped, want 3", c1.skips - c0.skips);
        }
    }

    /* the scrollbar under the mouse lights up, and stops; dragging the thumb moves the content */
    {
        float sx, sy;
        gp_el *sc = find_scroller(win()->tree, V(s)->scroll.id, 0, 0, &sx, &sy);
        gp_thumb t;
        CHECK(sc && gp_scroll_thumb(sc, 1, sx, sy, &t), "no thumb on the scroller");
        if (sc && gp_scroll_thumb(sc, 1, sx, sy, &t)) {
            float tx = t.x + t.w / 2, ty = t.y + t.h / 2;
            gp_simulate_mouse(app, GP_SIM_MOVE, tx, ty, 0, 0);
            settle();
            gp_simulate_mouse(app, GP_SIM_MOVE, 330, 590, 0, 0);
            settle();
            c0 = counters();
            notify_view(2);
            let_frame_pass();
            c1 = counters();
            CHECK(c1.hits - c0.hits == 3, "after the mouse left the scrollbar, %lu paints were reused, want 3", c1.hits - c0.hits);
            verify_frame();

            float off0 = gp_scroll_offset(&cx, V(s)->scroll).y;
            gp_simulate_mouse(app, GP_SIM_MOVE, tx, ty, 0, 0);
            gp_simulate_mouse(app, GP_SIM_DOWN, tx, ty, 1, 0);
            settle_fast();
            gp_simulate_mouse(app, GP_SIM_MOVE, tx, ty + 6, 0, 0);
            settle_fast();
            gp_simulate_mouse(app, GP_SIM_MOVE, tx + 80, ty + 12, 0, 0);   /* far from the bar: still dragging */
            settle_fast();
            gp_simulate_mouse(app, GP_SIM_MOVE, tx + 80, ty + 24, 0, 0);   /* and again, the same drag, another offset: not a painted copy of the last */
            settle_fast();
            gp_simulate_mouse(app, GP_SIM_MOVE, tx + 80, ty + 37, 0, 0);
            settle_fast();
            gp_simulate_mouse(app, GP_SIM_UP, tx + 80, ty + 12, 1, 0);
            settle();
            CHECK(gp_scroll_offset(&cx, V(s)->scroll).y != off0, "dragging the thumb did not move the scroller");
            gp_simulate_mouse(app, GP_SIM_MOVE, 330, 590, 0, 0);
            settle();
        }
    }

    /* moved from outside: laid out again for it, then skipped again */
    gp_scroll_to(&cx, V(s)->scroll, 0, 123);
    c0 = counters();
    let_frame_pass();
    c1 = counters();
    CHECK(c1.skips - c0.skips == 2, "after gp_scroll_to, %lu cached layouts were skipped, want 2 (not the scroller's)", c1.skips - c0.skips);
    CHECK(gp_scroll_offset(&cx, V(s)->scroll).y == 123, "gp_scroll_to: offset %.0f", gp_scroll_offset(&cx, V(s)->scroll).y);
    {
        float sx, sy;
        gp_el *sc = find_scroller(win()->tree, V(s)->scroll.id, 0, 0, &sx, &sy);
        CHECK(sc && sc->scroll.y == 123, "gp_scroll_to: the element's offset is %.0f", sc ? sc->scroll.y : -1);
    }
    verify_frame();
    c0 = counters();
    notify_view(2);
    let_frame_pass();
    c1 = counters();
    CHECK(c1.skips - c0.skips == 3 && c1.hits - c0.hits == 3, "after gp_scroll_to, an unrelated notify: %lu skipped, %lu reused, want 3 and 3", c1.skips - c0.skips, c1.hits - c0.hits);
    verify_frame();
    gp_list_invalidate(&cx, V(s)->list_scroll, 0, 400);
    settle();
    gp_list_reset(&cx, V(s)->list_scroll);
    settle();
    gp_scroll_to_end(&cx, V(s)->scroll);
    settle();

    /* a scroller in a cached view inside a cached view */
    V(5)->has_scroller = true;
    V(5)->scroll = gp_scroll_new(app);
    notify_view(5);
    settle();
    {
        float sx, sy;
        gp_el *sc = find_scroller(win()->tree, V(5)->scroll.id, 0, 0, &sx, &sy);
        CHECK(sc != NULL, "nested scroller not found");
        if (sc) {
            gp_scroll_wheel(win(), (gp_point){ sx + 10, sy + 10 }, 0, 30);
            settle();
            CHECK(sc->scroll.y == 30, "nested scroller at %.0f", sc->scroll.y);
            c0 = counters();
            notify_view(2);
            let_frame_pass();
            c1 = counters();
            CHECK(c1.hits - c0.hits == 3, "nested: %lu paints reused, want 3", c1.hits - c0.hits);
            verify_frame();
        }
    }
    V(5)->has_scroller = false;
    notify_view(5);
    settle();

    /* the window gets narrower: laid out for another width */
    c0 = counters();
    win()->size.w -= 60;
    win()->dirty_render = true;
    let_frame_pass();
    c1 = counters();
    CHECK(c1.skips == c0.skips, "a resize skipped %lu cached layouts", c1.skips - c0.skips);
    verify_frame();
    win()->size.w += 60;
    win()->dirty_render = true;
    settle();

    /* a list as tall as its rows settles as rows are measured, and is skipped once a layout learns nothing new */
    V(0)->nkids--;                                  /* s goes */
    int u = add_view();
    V(u)->auto_list = true;
    V(u)->list_scroll = gp_scroll_new(app);
    add_kid(0, u, true);
    layout_diff = false;
    notify_view(0); settle();
    for (int i = 0; i < 12; i++) { notify_view(2); settle(); }
    c0 = counters();
    notify_view(2);
    let_frame_pass();
    c1 = counters();
    CHECK(c1.skips - c0.skips == 3, "with a settled auto-height list: %lu cached layouts skipped, want 3", c1.skips - c0.skips);
    layout_diff = true;
    verify_frame();
    V(0)->nkids--;
    notify_view(0);
    settle();
}

/* The mouse over a part of a cached view that lies outside the view's own box still changes how it paints. */
static void test_hover_outside_the_root_box(void) {
    where = "test_hover_outside_the_root_box";
    gp_simulate_mouse(app, GP_SIM_MOVE, 330, 590, 0, 0);
    int h = add_view();
    V(h)->out_hover = true;
    add_kid(0, h, true);
    notify_view(0); settle();
    notify_view(2); settle();
    float x, y;
    char key[24];
    snprintf(key, sizeof key, "v%d=0", h);
    gp_el *root = find_with_text(win()->tree, key, 0, 0, &x, &y);
    CHECK(root != NULL, "view not found");
    if (root) {
        CHECK(!scene_has_color(HOVER_C), "hover color painted with no mouse over the box");
        gp_simulate_mouse(app, GP_SIM_MOVE, x + 10, y + 70 + 5, 0, 0);
        settle();
        CHECK(scene_has_color(HOVER_C), "no hover color with the mouse over a box that lies below its cached view's own");
        gp_simulate_mouse(app, GP_SIM_MOVE, 330, 590, 0, 0);
        settle();
        CHECK(!scene_has_color(HOVER_C), "hover color left behind");
    }
    V(0)->nkids--;
    notify_view(0);
    settle();
}

static void test_handlers_and_resize(void) {
    where = "test_handlers_and_resize";
    int z = add_view();
    V(z)->has_button = true;
    add_kid(0, z, true);
    notify_view(0);
    settle();
    float x, y;
    char key[32];
    snprintf(key, sizeof key, "v%d=0", z);
    gp_el *el = find_with_text(win()->tree, key, 0, 0, &x, &y);
    CHECK(el != NULL, "view with the button not found");
    reset_counts();
    for (int i = 1; i <= 3; i++) {
        gp_simulate_mouse(app, GP_SIM_MOVE, x + 10, y + 25, 0, 0);
        gp_simulate_mouse(app, GP_SIM_DOWN, x + 10, y + 25, 1, 0);
        gp_simulate_mouse(app, GP_SIM_UP, x + 10, y + 25, 1, 0);
        settle();
        snprintf(key, sizeof key, "v%d=%d", z, i);
        CHECK(V(z)->clicks == i && tree_has(win()->tree, key), "click %d: %d clicks, \"%s\" %s", i, V(z)->clicks, key, tree_has(win()->tree, key) ? "shown" : "missing");
    }
    CHECK(V(z)->renders == 3, "the clicked cached view rendered %d times for 3 clicks", V(z)->renders);
    CHECK(V(1)->renders == 0 && V(3)->renders == 0, "other cached views rendered on a click");

    /* a window resize lays cached views out again, with their old elements */
    snprintf(key, sizeof key, "v%d=3", z);
    el = find_with_text(win()->tree, key, 0, 0, &x, &y);
    float w0 = el->size.w;
    reset_counts();
    win()->size = (gp_size){ win()->size.w + 120, win()->size.h };
    win()->dirty_render = true;
    settle();
    el = find_with_text(win()->tree, key, 0, 0, &x, &y);
    CHECK(el && el->size.w > w0 + 100, "cached view is %.0f wide after the window grew by 120 (was %.0f)", el ? el->size.w : 0, w0);
    CHECK(V(z)->renders == 0, "a resize rendered the cached view");
    win()->size = (gp_size){ win()->size.w - 120, win()->size.h };
    win()->dirty_render = true;
    settle();
    V(0)->nkids--;
    notify_view(0);
    settle();
}

/* The sizes memoized during layout live on in the elements of a cached view. A cached view must lay out
 * exactly as an identical view that was built afresh, whatever happened above it. */
static void test_layout_matches_a_fresh_build(void) {
    where = "test_layout_matches_a_fresh_build";
    layout_diff = false;
    /* two pairs: with lists and wrapped text, and in a box of fixed size */
    int t1 = add_view(), t2 = add_view(), p1 = add_view(), p2 = add_view();
    int b1 = add_view(), b2 = add_view(), q1 = add_view(), q2 = add_view();
    for (int t = t1; t <= t2; t++) {
        V(t)->long_text = true;
        V(t)->auto_list = true;
        V(t)->list_scroll = gp_scroll_new(app);
    }
    V(b1)->boxed = V(b2)->boxed = true;
    add_kid(p1, t1, true);    /* the same view, under a parent that can change the inherited text size */
    add_kid(p2, t2, false);   /* and built afresh each time */
    add_kid(q1, b1, true);
    add_kid(q2, b2, false);
    add_kid(0, p1, false);
    add_kid(0, p2, false);
    add_kid(0, q1, false);
    add_kid(0, q2, false);
    notify_view(0);
    settle();
    char w[4][24], why[160] = "";
    snprintf(w[0], sizeof w[0], "v%d=0", t1);
    snprintf(w[1], sizeof w[1], "v%d=0", t2);
    snprintf(w[2], sizeof w[2], "v%d=0", b1);
    snprintf(w[3], sizeof w[3], "v%d=0", b2);
    float fonts[] = { 0, 24, 11, 0, 17, 17, 9 };
    for (size_t step = 0; step < sizeof fonts / sizeof *fonts; step++) {
        V(p1)->child_font = V(p2)->child_font = V(q1)->child_font = V(q2)->child_font = fonts[step];
        notify_view(p1);
        notify_view(p2);
        notify_view(q1);
        notify_view(q2);
        settle();
        for (int pair = 0; pair < 2; pair++) {
            float x, y;
            gp_el *a = find_with_text(win()->tree, w[pair * 2], 0, 0, &x, &y);
            gp_el *b = find_with_text(win()->tree, w[pair * 2 + 1], 0, 0, &x, &y);
            CHECK(a && b, "twin views not found");
            if (a && b)
                CHECK(same_layout(a, b, why, sizeof why), "pair %d, parent's text size %.0f: the cached view lays out differently from a fresh one: %s", pair, fonts[step], why);
        }
        if (step == 3) for (int i = 0; i < 3; i++) { notify_view(0); settle(); }   /* rebuilds that reuse it: lists measure rows meanwhile */
    }
    CHECK(V(t1)->renders == 1 && V(b1)->renders == 1, "the cached twins rendered %d and %d times: the test is not exercising reuse", V(t1)->renders, V(b1)->renders);
    V(0)->nkids -= 4;
    notify_view(0);
    settle();
    layout_diff = true;
}

static void test_text_input_in_cached_view(void) {
    where = "test_text_input_in_cached_view";
    int p = add_view();
    gp_entity input = gp_text_input_new(app, &(gp_text_input_options){ .text = "start" });
    V(p)->input = input;
    add_kid(0, p, true);
    notify_view(0);
    settle();
    CHECK(tree_has(win()->tree, "start"), "the field's text is not shown");
    reset_counts();

    /* set from a ctx that belongs to neither the field nor its parent: only the field being a view
     * that is told can make it show */
    gp_entity other = gp_entity_new(app, 8, NULL, NULL);
    gp_ctx oc = gp_ctx_for(app, other);
    gp_text_input_set_text(&oc, input, "hello");
    settle();
    CHECK(tree_has(win()->tree, "hello"), "gp_text_input_set_text did not reach the screen through a cached parent");
    CHECK(V(1)->renders == 0 && V(3)->renders == 0, "unrelated cached views rendered for a text change");

    /* typing */
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_focus_set(&cx, gp_text_input_focus(app, input));
    settle();
    gp_simulate_text(app, "!");
    settle();
    CHECK(tree_has(win()->tree, "hello!"), "typed text did not reach the screen through a cached parent");
    gp_text_input_select_all(&oc, input);
    settle();
    gp_entity_release(app, other);
    V(0)->nkids--;
    notify_view(0);
    settle();
}

/* ------------------------------------------------------------ randomized, against a model */

typedef struct {
    int parent;
    bool cached, hidden;
    bool dirty, has_cache, used, displayed;
    int kids[MAXK], nkids;          /* structure as currently configured */
    int last[MAXK], nlast;          /* what its last render built */
    int expect;
} M;

static M model[MAXV];
static bool m_window_dirty, m_all_dirty;

static bool m_subdirty(int v) {
    for (int i = 0; i < model[v].nlast; i++) {
        int k = model[v].last[i];
        if (model[k].dirty || m_subdirty(k)) return true;
    }
    return false;
}
static void m_keep(int v) {
    for (int i = 0; i < model[v].nlast; i++) {
        int k = model[v].last[i];
        if (!model[k].used) { model[k].used = true; m_keep(k); }
    }
}
/* the cached flag is per (parent, kid) in the fixture, so the model keeps it per kid slot */
static bool slot_cached[MAXV][MAXK];
static void m_render(int v, bool cached) {
    model[v].used = true;
    if (cached && model[v].has_cache && !model[v].dirty && !m_subdirty(v) && !m_all_dirty) {
        m_keep(v);
        return;
    }
    model[v].dirty = false;
    model[v].expect++;
    model[v].has_cache = cached;
    model[v].nlast = 0;
    for (int i = 0; i < model[v].nkids; i++) {
        int k = model[v].kids[i];
        if (model[k].hidden) continue;
        model[v].last[model[v].nlast++] = k;
        m_render(k, slot_cached[v][i]);
    }
}

static void m_frame(int n) {
    if (!m_window_dirty && !m_all_dirty) return;
    for (int i = 0; i < n; i++) model[i].used = false;
    m_render(0, false);
    for (int i = 0; i < n; i++) {
        model[i].displayed = model[i].used;
        if (!model[i].used) { model[i].has_cache = false; model[i].dirty = false; }
    }
    m_window_dirty = m_all_dirty = false;
}

static uint64_t rng = 0x2545F4914F6CDD1Dull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 11); }

static void test_random(void) {
    where = "test_random";
    if (getenv("VIEWS_TEST_SEED")) rng ^= strtoull(getenv("VIEWS_TEST_SEED"), NULL, 10) * 0x9E3779B97F4A7C15ull;
    int steps = getenv("VIEWS_TEST_STEPS") ? atoi(getenv("VIEWS_TEST_STEPS")) : 200;
    /* a fresh tree in a second set of entities, shown by a second root */
    int base = nviews;
    enum { N = 22 };
    for (int i = 0; i < N; i++) add_view();
    memset(model, 0, sizeof model);
    for (int i = 1; i < N; i++) {
        int p;
        do p = (int)(rnd() % (uint32_t)i); while (V(base + p)->nkids >= MAXK);
        bool cached = rnd() % 3 != 0;
        add_kid(base + p, base + i, cached);
        model[p].kids[model[p].nkids] = i;
        slot_cached[p][model[p].nkids++] = cached;
        model[i].parent = p;
        V(base + i)->hover = rnd() % 3 == 0;
        if (rnd() % 5 == 0) { V(base + i)->has_scroller = true; V(base + i)->scroll = gp_scroll_new(app); }
        if (rnd() % 6 == 0) { V(base + i)->has_list = true; V(base + i)->list_scroll = gp_scroll_new(app); }
        if (rnd() % 4 == 0) { V(base + i)->focusable = true; V(base + i)->focus = gp_focus_new(app); }
    }
    /* the window shows this tree under the old root, which is not cached: it renders whenever the window
     * rebuilds, and builds the model's view 0 as an ordinary child */
    V(0)->nkids = 0;
    add_kid(0, base, false);
    gp_entity models[3];
    for (int m = 0; m < 3; m++) models[m] = gp_entity_new(app, 8, NULL, NULL);
    int observer_of[3] = { -1, -1, -1 };
    for (int m = 0; m < 3; m++) {
        observer_of[m] = 1 + (int)(rnd() % (N - 1));
        gp_ctx c = gp_ctx_for(app, ents[base + observer_of[m]]);
        gp_observe(&c, models[m], NULL, NULL);
    }

    notify_view(0);
    settle();
    m_window_dirty = true;   /* the model's first build: every view renders once, nothing is cached yet */
    m_frame(N);

    int bad = 0;
    uint64_t focus_now = win()->focused;
    for (int step = 0; step < steps && bad < 8; step++) {
        int op = (int)(rnd() % 38);
        int v = (int)(rnd() % N);
        char desc[96];
        if (op < 8) {                                    /* change + notify */
            V(base + v)->value++;
            notify_view(base + v);
            if (model[v].displayed) { model[v].dirty = true; m_window_dirty = true; }
            snprintf(desc, sizeof desc, "change+notify %d", v);
        } else if (op < 10) {                            /* notify without a change */
            notify_view(base + v);
            if (model[v].displayed) { model[v].dirty = true; m_window_dirty = true; }
            snprintf(desc, sizeof desc, "notify %d", v);
        } else if (op == 10) {                           /* everything */
            if (rnd() % 2) gp_app_notify(app); else { gp_ctx n = gp_ctx_for(app, GP_ENTITY_NULL); gp_notify(&n); }
            m_all_dirty = true;
            m_window_dirty = true;
            snprintf(desc, sizeof desc, "app notify");
        } else if (op < 14 && v > 0) {                   /* hide / show, and tell the parent */
            int p = model[v].parent;
            V(base + v)->hidden = !V(base + v)->hidden;
            model[v].hidden = V(base + v)->hidden;
            notify_view(base + p);
            if (model[p].displayed) { model[p].dirty = true; m_window_dirty = true; }
            snprintf(desc, sizeof desc, "toggle hidden %d (parent %d)", v, p);
        } else if (op < 17 && v > 0) {                   /* cached <-> plain at the parent */
            int p = model[v].parent;
            for (int i = 0; i < model[p].nkids; i++)
                if (model[p].kids[i] == v) {
                    slot_cached[p][i] = !slot_cached[p][i];
                    V(base + p)->kid_cached[i] = slot_cached[p][i];
                }
            notify_view(base + p);
            if (model[p].displayed) { model[p].dirty = true; m_window_dirty = true; }
            snprintf(desc, sizeof desc, "toggle cached %d (parent %d)", v, p);
        } else if (op >= 20 && op < 23) {               /* what a parent wraps its kids in */
            int p = v;
            float opac[] = { 0, 0.4f, 0.9f }, clip[] = { 0, 25, 70 }, font[] = { 0, 10, 20 };
            V(base + p)->child_opacity = opac[rnd() % 3];
            V(base + p)->child_clip_h = clip[rnd() % 3];
            V(base + p)->child_font = font[rnd() % 3];
            notify_view(base + p);
            if (model[p].displayed) { model[p].dirty = true; m_window_dirty = true; }
            snprintf(desc, sizeof desc, "wrap kids of %d", p);
        } else if (op >= 23 && op < 26) {               /* the mouse */
            gp_simulate_mouse(app, GP_SIM_MOVE, (float)(rnd() % 340), (float)(rnd() % 600), 0, 0);
            if (rnd() % 4 == 0) { gp_simulate_mouse(app, GP_SIM_DOWN, (float)(rnd() % 340), (float)(rnd() % 600), 1, 0); gp_simulate_mouse(app, GP_SIM_UP, 5, 5, 1, 0); }
            snprintf(desc, sizeof desc, "mouse");
        } else if (op == 26) {                           /* focus */
            gp_ctx fc = gp_ctx_for(app, GP_ENTITY_NULL);
            gp_focus next = V(base + v)->focusable ? V(base + v)->focus : (gp_focus){ 0 };
            gp_focus_set(&fc, next);
            snprintf(desc, sizeof desc, "focus view %d", v);
        } else if (op == 27) {                           /* window size */
            win()->size.w = 340 + (float)(rnd() % 3) * 50;
            win()->dirty_render = true;
            m_window_dirty = true;
            snprintf(desc, sizeof desc, "resize");
        } else if (op == 28) {                           /* atlas */
            gp_text_atlas_reset(app->text);
            win()->dirty_paint = true;
            snprintf(desc, sizeof desc, "atlas reset");
        } else if (op == 29) {                           /* scale */
            win()->scale = win()->scale == 1.f ? 2.f : 1.f;
            win()->dirty_render = true;
            m_window_dirty = true;
            snprintf(desc, sizeof desc, "scale");
        } else if (op >= 30 && op < 33) {               /* the wheel, anywhere */
            gp_scroll_wheel(win(), (gp_point){ (float)(rnd() % 340), (float)(rnd() % 600) },
                            rnd() % 4 == 0 ? (float)((int)(rnd() % 61) - 30) : 0, (float)((int)(rnd() % 161) - 60));
            snprintf(desc, sizeof desc, "wheel");
        } else if (op == 33 || op == 34) {              /* a scroller or a list is moved from outside */
            gp_ctx sc_cx = gp_ctx_for(app, ents[base + v]);
            View *vv = V(base + v);
            gp_scroll_state *lst = vv->list_scroll.id ? gp_scroll_state_get(&win()->scrolls, vv->list_scroll.id, false) : NULL;
            bool dirties = false;   /* these ask the window to lay out again, which rebuilds it: ordinary views render */
            switch (rnd() % 5) {
            case 0: if (vv->has_scroller) { gp_scroll_to(&sc_cx, vv->scroll, 0, (float)(rnd() % 400)); dirties = true; } break;
            case 1: if (vv->has_list) { gp_scroll_to_end(&sc_cx, vv->list_scroll); dirties = true; } break;
            case 2: if (lst && lst->vl) { gp_list_scroll_to_item(&sc_cx, vv->list_scroll, rnd() % 400, (int)(rnd() % 3)); dirties = true; } break;
            case 3: if (lst && lst->vl) { gp_list_invalidate(&sc_cx, vv->list_scroll, rnd() % 40, 1 + rnd() % 60); dirties = true; } break;
            default: if (vv->has_list) { gp_scroll_by(&sc_cx, vv->list_scroll, 0, (float)((int)(rnd() % 300) - 100)); dirties = true; } break;
            }
            if (dirties) m_window_dirty = true;
            snprintf(desc, sizeof desc, "scroll from outside (view %d)", v);
        } else if (op == 35 || op == 36) {              /* the thumb of some scroller: hover, press, drag, release */
            gp_el *sc = NULL;
            float sx = 0, sy = 0;
            uint64_t key = V(base + v)->scroll.id ? V(base + v)->scroll.id : V(base + v)->list_scroll.id;
            if (key) sc = find_scroller(win()->tree, key, 0, 0, &sx, &sy);
            gp_thumb t;
            if (sc && gp_scroll_thumb(sc, 1, sx, sy, &t)) {
                float tx = t.x + t.w / 2, ty = t.y + t.h / 2;
                gp_simulate_mouse(app, GP_SIM_MOVE, tx, ty, 0, 0);
                if (rnd() % 3) {
                    gp_simulate_mouse(app, GP_SIM_DOWN, tx, ty, 1, 0);
                    gp_simulate_mouse(app, GP_SIM_MOVE, tx + (float)(rnd() % 100), ty + (float)(rnd() % 40), 0, 0);
                    if (rnd() % 2) gp_simulate_mouse(app, GP_SIM_UP, tx, ty + 10, 1, 0);   /* else it is still held for a step */
                }
            }
            snprintf(desc, sizeof desc, "thumb of view %d", v);
        } else {                                         /* a model notifies; its observer's view re-renders */
            int m = (int)(rnd() % 3);
            gp_ctx c = gp_ctx_for(app, models[m]);
            gp_notify(&c);
            int o = observer_of[m];
            if (model[o].displayed) { model[o].dirty = true; m_window_dirty = true; }
            snprintf(desc, sizeof desc, "model %d notifies (observed by %d)", m, o);
        }
        static char where_buf[160];
        snprintf(where_buf, sizeof where_buf, "test_random step %d: %s", step, desc);
        where = where_buf;
        settle_fast();
        /* focus can move by itself (a click on a focusable view), and a change rebuilds the window: ordinary views render */
        if (win()->focused != focus_now) { focus_now = win()->focused; m_window_dirty = true; }
        m_frame(N);
        if (!tree_is_a_tree()) { CHECK(false, "step %d (%s): a node is in the tree twice", step, desc); bad++; }
        for (int i = 0; i < N; i++) {
            if (V(base + i)->renders != model[i].expect) {
                CHECK(false, "step %d (%s): view %d rendered %d times in total, model says %d", step, desc, i, V(base + i)->renders, model[i].expect);
                model[i].expect = V(base + i)->renders;   /* report once */
                bad++;
            }
            if (model[i].displayed) {
                char t[40];
                snprintf(t, sizeof t, "v%d=%d", base + i, V(base + i)->value);
                if (!tree_has(win()->tree, t)) { CHECK(false, "step %d (%s): view %d is shown stale, not as \"%s\"", step, desc, i, t); bad++; }
            }
        }
    }
    for (int m = 0; m < 3; m++) gp_entity_release(app, models[m]);
}

/* ------------------------------------------------------------ driver */

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    gp_sleep(co, 0.3);
    test_dirty_paths();
    test_frame_caches();
    test_screen_matches_state();
    test_same_view_twice();
    test_hide_show_release();
    test_focus();
    test_scroll_list_layout_in_cached_views();
    test_dynamic_subtrees();
    test_hover_outside_the_root_box();
    test_handlers_and_resize();
    test_text_input_in_cached_view();
    test_layout_matches_a_fresh_build();
    test_random();
    gp_quit(app);
}

static void ready(gp_app *a_, void *userdata) {
    app = a_;
    build_fixed_tree();
    root_ent = ents[0];
    gp_open_window(app, &(gp_window_options){ .title = "views_test", .width = 340, .height = 600 }, root_ent);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);   /* a physical pointer over the window is not part of the test */
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "views_test: %d FAILURE(S)\n" : "views_test: all checks passed\n", failures);
    return failures != 0;
}
