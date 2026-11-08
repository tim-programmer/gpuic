/*
 * Declarative animations (gp_with_animation). Needs a display.
 *
 * Time is jumped with gp_debug_clock_skew instead of waited for, so progress values can be checked exactly
 * (to within the few milliseconds of real time between a jump and the frame that follows it). After every
 * step the frame on screen is compared with one painted without the frame caches, and the geometry with a
 * full layout, as in views_test, because animating inside cached views is where reuse could go wrong.
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
            if (failures++ < 30) {                                                     \
                printf("FAIL %s:%d: ", __FILE__, __LINE__);                            \
                printf(__VA_ARGS__);                                                   \
                printf("\n");                                                          \
            }                                                                          \
        }                                                                              \
    } while (0)

static gp_app *app;
static gp_co *co_;
static gp_window *win(void) { return app->windows[0]; }

/* ------------------------------------------------------------ the clock and frames */

static int64_t skew;
static void jump(double seconds) {
    skew += (int64_t)(seconds * 1e9);
    gp_debug_clock_skew(skew);
}

static bool wait_draws(unsigned n) {
    unsigned long target = app->stat_draws + n;
    for (int i = 0; i < 1000 && app->stat_draws < target; i++) gp_sleep(co_, 0.004);
    return app->stat_draws >= target;
}

static float geo_a[6 * 4096], geo_b[6 * 4096];
static size_t snapshot(const gp_el *n, float *geo, size_t at) {
    if (at >= 4096) return at;
    geo[at * 6 + 0] = n->pos.x; geo[at * 6 + 1] = n->pos.y; geo[at * 6 + 2] = n->size.w; geo[at * 6 + 3] = n->size.h;
    geo[at * 6 + 4] = n->style.opacity; geo[at * 6 + 5] = n->kind == GP_EL_LIST ? 0 : n->scroll.y;
    at++;
    if (n->kind == GP_EL_LIST) return at;
    for (uint32_t i = 0; i < n->nchildren; i++) at = snapshot(n->children[i], geo, at);
    return at;
}

static const char *where = "";
static bool check_frames = true;
/* what is on screen == the same tree painted without caches, laid out in full */
static void verify_frame(void) {
    gp_window *w = win();
    if (!check_frames || !w->tree || w->dirty_render || w->dirty_paint || w->dirty_layout) return;
    gp_scene fresh = { 0 };
    gp_arena scratch = { 0 };
    gp_paint_tree(&fresh, &scratch, app->text, w->tree, w->scale, w->mouse, w->mouse_in, w->pressed, w->focused, w->drag_key, w->scroll_vis_gen, NULL, NULL);
    bool same = fresh.len == w->scene.len && memcmp(fresh.items, w->scene.items, fresh.len * sizeof *fresh.items) == 0;
    CHECK(same, "[%s] the frame painted with caches differs from one painted without (%zu vs %zu instances)", where, w->scene.len, fresh.len);
    free(fresh.items);
    gp_arena_free(&scratch);

    size_t n0 = snapshot(w->tree, geo_a, 0);
    gp_debug_frame_caches(false);
    gp_layout_root(w->tree, app->text, w->size, &w->scrolls, NULL, NULL);
    size_t n1 = snapshot(w->tree, geo_b, 0);
    gp_debug_frame_caches(true);
    gp_layout_root(w->tree, app->text, w->size, &w->scrolls, NULL, NULL);
    if (n0 != n1) { CHECK(false, "[%s] a full layout changed the node count", where); return; }
    for (size_t i = 0; i < n0 * 6; i++)
        if (fabsf(geo_a[i] - geo_b[i]) > 0.01f) {
            CHECK(false, "[%s] node %zu field %zu: %.3f on screen, %.3f after a full layout", where, i / 6, i % 6, geo_a[i], geo_b[i]);
            break;
        }
    w->dirty_paint = true;   /* the check laid out again: let the next frame follow it */
    gp_sleep(co_, 0.03);
}

/* one frame after a jump of the clock */
static void step(double seconds) {
    jump(seconds);
    win()->dirty_layout = true;
    wait_draws(2);
    verify_frame();
}

/* ------------------------------------------------------------ the fixture */

typedef struct { float t; int calls; } Probe;
static gp_node *fade(float t, void *data) { Probe *p = data; if (p) { p->t = t; p->calls++; } return gp_opacity(t); }
static gp_node *grow(float t, void *data) { Probe *p = data; if (p) { p->t = t; p->calls++; } return gp_w(10 + 100 * t); }
static gp_node *both(float t, void *data) { Probe *p = data; if (p) { p->t = t; p->calls++; } return gp_group(gp_opacity(t), gp_w(10 + 100 * t), gp_bg(gp_rgb(0x00ff00))); }
/* sets a different thing in each half, so what the first half set must be gone in the second */
static gp_node *flip(float t, void *data) { return t < 0.5f ? gp_bg(gp_rgb(0x0000ff)) : gp_w(77); }
/* the inherited text size changes: layout of the text below must follow, even in a cached view */
static gp_node *bigger(float t, void *data) { return gp_text_size(10 + 30 * t); }
static gp_node *nothing(float t, void *data) { return NULL; }
static gp_node *sneaky(float t, void *data) { return gp_with_animation("inner", (gp_animation){ .duration = 1 }, fade, NULL); }

typedef struct {
    bool show, show2;
    const char *id, *id2;
    char idbuf[32], idbuf2[32];
    gp_animation anim, anim2;
    gp_anim_style_fn fn, fn2;
    Probe p, p2;
    bool twice;             /* two animations on the box, the same id */
    gp_entity kid;          /* a view shown after the boxes */
    bool kid_cached;
    int renders;
    int rows;               /* a list of animated rows below the boxes */
    gp_scroll list_scroll;
    Probe row_probe;
} Fix;

static gp_node *row(void *self, gp_ctx *cx, size_t i, void *data) {
    Fix *f = self;
    return gp_div(gp_h(20), gp_flex_row(),
                  gp_with_animation(gp_fmt("row%zu", i), (gp_animation){ .duration = 20 }, fade, &f->row_probe),
                  gp_text(gp_fmt("row %zu", i)));
}

static gp_node *render(void *self, gp_ctx *cx) {
    Fix *f = self;
    f->renders++;
    gp_node *box = NULL, *box2 = NULL;
    if (f->show) {
        box = gp_div(gp_h(16), gp_bg(gp_rgb(0xff0000)), gp_with_animation(f->id, f->anim, f->fn, &f->p),
                     f->twice ? gp_with_animation(f->id, f->anim2, f->fn2, &f->p2) : NULL, gp_text("box"));
    }
    if (f->show2)
        box2 = gp_div(gp_h(16), gp_bg(gp_rgb(0x0000ff)), gp_with_animation(f->id2, f->anim2, f->fn2, &f->p2), gp_text("box2"));
    gp_node *kid = f->kid.generation ? (f->kid_cached ? gp_child_view_cached(f->kid) : gp_child_view(f->kid)) : NULL;
    return gp_div(gp_flex_col(),
                  gp_div(gp_flex_row(), gp_h(20), box, box2, gp_div(gp_text("after"))),
                  kid,
                  f->rows ? gp_list(f->rows, 20, row, NULL, gp_track_scroll(f->list_scroll), gp_h(100)) : NULL);
}

static Fix *fix(gp_entity e) { return gp_entity_get(app, e); }
static gp_entity root, other;

static void rerender(gp_entity e) { gp_ctx cx = gp_ctx_for(app, e); gp_notify(&cx); }

static gp_el *find_parent_of_text(gp_el *n, const char *text) {
    for (uint32_t i = 0; i < n->nchildren; i++)
        if (n->children[i]->text && strcmp(n->children[i]->text, text) == 0) return n;
    for (uint32_t i = 0; i < n->nchildren; i++) {
        gp_el *r = find_parent_of_text(n->children[i], text);
        if (r) return r;
    }
    return NULL;
}
static gp_el *box(const char *text) { return find_parent_of_text(win()->tree, text); }
static float opacity(const char *text) { gp_el *e = box(text); return e ? e->style.opacity : -1; }
static float width(const char *text) { gp_el *e = box(text); return e ? e->size.w : -1; }

/* a state is keyed by the id: a scenario that wants a fresh animation gets an id nobody used */
static void reset(Fix *f) {
    static int serial;
    *f = (Fix){ .show = false, .fn = fade, .fn2 = fade, .kid = f->kid, .kid_cached = f->kid_cached };
    snprintf(f->idbuf, sizeof f->idbuf, "anim%d", ++serial);
    snprintf(f->idbuf2, sizeof f->idbuf2, "anim%db", serial);
    f->id = f->idbuf;
    f->id2 = f->idbuf2;
    f->anim = (gp_animation){ .duration = 2 };
    f->anim2 = (gp_animation){ .duration = 2 };
}

#define NEAR(a, b, tol) (fabsf((float)(a) - (float)(b)) <= (tol))

/* ------------------------------------------------------------ scenarios */

static void test_one_run(void) {
    where = "test_one_run";
    Fix *f = fix(root);
    reset(f);
    f->show = true;
    rerender(root);
    wait_draws(2);
    verify_frame();
    CHECK(box("box") != NULL, "box not found");
    CHECK(opacity("box") < 0.1f, "starts at %.3f", opacity("box"));

    step(1.0);
    CHECK(NEAR(opacity("box"), 0.5f, 0.06f), "after 1 of 2 s: %.3f", opacity("box"));
    step(0.5);
    CHECK(NEAR(opacity("box"), 0.75f, 0.06f), "after 1.5 of 2 s: %.3f", opacity("box"));
    CHECK(f->renders == 1 || f->renders == 2, "the view rendered %d times while its element animated", f->renders);
    int renders = f->renders;
    step(1.0);
    CHECK(opacity("box") == 1.0f, "finished at %.3f, want exactly 1", opacity("box"));
    CHECK(f->p.t == 1.0f, "the function's last t was %.3f", f->p.t);

    /* a finished animation asks for nothing: no frames, no wakeups */
    wait_draws(2);
    gp_sleep(co_, 0.15);   /* a check's own repaint, if one is on its way */
    unsigned long draws = app->stat_draws, wakeups = app->stat_wakeups;
    gp_sleep(co_, 0.4);
    CHECK(app->stat_draws == draws, "%lu frames were drawn after the animation finished", app->stat_draws - draws);
    CHECK(app->stat_wakeups - wakeups <= 3, "%lu wakeups after the animation finished", app->stat_wakeups - wakeups);
    CHECK(f->renders == renders, "rendered during idle");

    /* rebuilding keeps showing where it ended */
    rerender(root);
    wait_draws(2);
    CHECK(opacity("box") == 1.0f, "after a rebuild a finished animation shows %.3f", opacity("box"));
    verify_frame();
}

static void test_frames_while_running(void) {
    where = "test_frames_while_running";
    Fix *f = fix(root);
    reset(f);
    f->show = true;
    f->anim.duration = 100;
    f->p.calls = 0;
    rerender(root);
    wait_draws(2);
    unsigned long draws = app->stat_draws;
    gp_sleep(co_, 0.5);
    unsigned long n = app->stat_draws - draws;
    CHECK(n >= 10, "only %lu frames in half a second of animation", n);
    CHECK(n <= 140, "%lu frames in half a second: a runaway loop?", n);
    CHECK(f->renders < 5, "the view rendered %d times for an animation", f->renders);
    CHECK(f->p.calls >= 10, "the function was called %d times", f->p.calls);
    gp_cancel(app, (gp_task){ 0 });
}

static void test_easing_repeat_delay(void) {
    where = "test_easing_repeat_delay";
    Fix *f = fix(root);

    reset(f);
    f->show = true;
    f->anim = (gp_animation){ .duration = 40, .easing = GP_EASE_IN };
    rerender(root); wait_draws(2);
    step(20.0);
    CHECK(NEAR(opacity("box"), 0.125f, 0.03f), "ease-in at the middle: %.3f", opacity("box"));

    reset(f);
    f->show = true;
    f->anim = (gp_animation){ .duration = 10, .repeat = true };
    rerender(root); wait_draws(2);
    step(2.5);
    CHECK(NEAR(opacity("box"), 0.25f, 0.06f), "repeat, first run: %.3f", opacity("box"));
    step(10.0);
    CHECK(NEAR(opacity("box"), 0.25f, 0.06f), "repeat, second run (12.5 s into a 10 s run): %.3f", opacity("box"));
    step(100.0);
    CHECK(NEAR(opacity("box"), 0.25f, 0.06f), "repeat, run 12 (112.5 s): %.3f", opacity("box"));
    unsigned long draws = app->stat_draws;
    gp_sleep(co_, 0.2);
    CHECK(app->stat_draws - draws >= 3, "a repeating animation stopped drawing");

    reset(f);
    f->show = true;
    f->anim = (gp_animation){ .duration = 10, .repeat = true, .ping_pong = true };
    rerender(root); wait_draws(2);
    step(2.5);
    CHECK(NEAR(opacity("box"), 0.25f, 0.06f), "ping-pong, forwards: %.3f", opacity("box"));
    step(5.0);
    CHECK(NEAR(opacity("box"), 0.75f, 0.06f), "ping-pong, still forwards (7.5 s): %.3f", opacity("box"));
    step(5.0);
    CHECK(NEAR(opacity("box"), 0.75f, 0.06f), "ping-pong, backwards (12.5 s): %.3f", opacity("box"));
    step(10.0);
    CHECK(NEAR(opacity("box"), 0.25f, 0.06f), "ping-pong, forwards again (22.5 s): %.3f", opacity("box"));

    reset(f);
    f->show = true;
    f->anim = (gp_animation){ .duration = 10, .delay = 5 };
    rerender(root); wait_draws(2);
    step(2.5);
    CHECK(opacity("box") < 0.05f, "during the delay: %.3f", opacity("box"));
    unsigned long d0 = app->stat_draws;
    gp_sleep(co_, 0.15);
    CHECK(app->stat_draws - d0 >= 2, "frames stop during a delay");
    step(7.5);
    CHECK(NEAR(opacity("box"), 0.5f, 0.07f), "delay 5 + 5 into a 10 s run: %.3f", opacity("box"));

    reset(f);
    f->show = true;
    f->anim = (gp_animation){ .duration = 0 };
    rerender(root); wait_draws(2);
    CHECK(opacity("box") == 1.0f, "duration 0 shows %.3f at once", opacity("box"));
    gp_sleep(co_, 0.15);   /* a frame requested by the scenario before this one may still arrive */
    draws = app->stat_draws;
    gp_sleep(co_, 0.2);
    CHECK(app->stat_draws == draws, "duration 0 kept drawing");
}

static void test_layout_changes(void) {
    where = "test_layout_changes";
    Fix *f = fix(root);
    reset(f);
    f->show = true;
    f->fn = grow;
    f->anim = (gp_animation){ .duration = 40 };
    rerender(root); wait_draws(2);
    float after0 = -1;
    gp_el *a = find_parent_of_text(win()->tree, "after");
    (void)a;
    step(20.0);
    CHECK(NEAR(width("box"), 60, 4), "width at the middle: %.2f", width("box"));
    /* the element beside it moves with it */
    gp_el *after = NULL;
    {
        gp_el *row_el = box("box")->nchildren ? NULL : NULL;
        (void)row_el;
    }
    after = find_parent_of_text(win()->tree, "after");
    after0 = after ? after->pos.x : -1;
    CHECK(after && after0 >= 55, "the next element is at x=%.1f, expected beyond the grown box", after0);
    step(40.0);
    CHECK(NEAR(width("box"), 110, 0.5f), "final width %.2f", width("box"));
    gp_el *after2 = find_parent_of_text(win()->tree, "after");
    CHECK(after2 && after2->pos.x > after0, "the next element did not move on (%.1f -> %.1f)", after0, after2 ? after2->pos.x : -1);
}

static void test_ids_and_identity(void) {
    where = "test_ids_and_identity";
    Fix *f = fix(root);

    /* a rebuild in the middle continues; a new id starts over */
    reset(f);
    f->show = true;
    f->id = "one";
    f->anim = (gp_animation){ .duration = 40 };
    rerender(root); wait_draws(2);
    step(20.0);
    CHECK(NEAR(opacity("box"), 0.5f, 0.06f), "before the rebuild: %.3f", opacity("box"));
    rerender(root); wait_draws(2);
    CHECK(opacity("box") > 0.45f, "after a rebuild the animation went back to %.3f", opacity("box"));
    f->id = "two";
    rerender(root); wait_draws(2);
    CHECK(opacity("box") < 0.1f, "a new id did not start over (%.3f)", opacity("box"));
    verify_frame();

    /* gone for a layout, and back: starts over */
    step(20.0);
    CHECK(opacity("box") > 0.4f, "precondition: %.3f", opacity("box"));
    f->show = false;
    rerender(root); wait_draws(3);
    CHECK(box("box") == NULL, "the box should be hidden");
    f->show = true;
    rerender(root); wait_draws(2);
    CHECK(opacity("box") < 0.1f, "an element that came back did not start over (%.3f)", opacity("box"));

    /* two elements, two ids, independent clocks */
    reset(f);
    f->show = f->show2 = true;
    f->id = "x";
    f->id2 = "y";
    f->anim = (gp_animation){ .duration = 40 };
    f->anim2 = (gp_animation){ .duration = 40 };
    rerender(root); wait_draws(2);
    step(20.0);
    f->show2 = false;
    rerender(root); wait_draws(2);
    f->show2 = true;
    rerender(root); wait_draws(2);
    CHECK(NEAR(opacity("box"), 0.5f, 0.1f), "the first box was disturbed: %.3f", opacity("box"));
    CHECK(opacity("box2") < 0.15f, "the second box did not start over: %.3f", opacity("box2"));

    /* two animations on one element share an id and still run apart */
    reset(f);
    f->show = true;
    f->twice = true;
    f->id = "same";
    f->fn = fade;
    f->fn2 = grow;
    f->anim = (gp_animation){ .duration = 40 };
    f->anim2 = (gp_animation){ .duration = 20 };
    rerender(root); wait_draws(2);
    step(10.0);
    CHECK(NEAR(opacity("box"), 0.25f, 0.06f), "first of two animations: %.3f", opacity("box"));
    CHECK(NEAR(width("box"), 60, 5), "second of two animations: %.2f", width("box"));
    step(15.0);
    CHECK(NEAR(opacity("box"), 0.625f, 0.06f), "first of two, later: %.3f", opacity("box"));
    CHECK(NEAR(width("box"), 110, 0.5f), "second of two, finished: %.2f", width("box"));
    f->twice = false;
}

static void test_group_and_oddities(void) {
    where = "test_group_and_oddities";
    Fix *f = fix(root);
    reset(f);
    f->show = true;
    f->id = "grp";
    f->fn = both;
    f->anim = (gp_animation){ .duration = 40 };
    rerender(root); wait_draws(2);
    step(20.0);
    CHECK(NEAR(opacity("box"), 0.5f, 0.06f) && NEAR(width("box"), 60, 4), "a group sets %.3f and %.2f", opacity("box"), width("box"));
    gp_el *b = box("box");
    CHECK(b && b->style.has_bg && NEAR(b->style.bg.g, 1.f, 0.01f) && b->style.bg.r < 0.01f, "the group's background did not replace the element's");
    step(40.0);

    /* nothing accumulates: the style is rebuilt from the original each time (a width that is set once and left alone) */
    reset(f);
    f->show = true;
    f->id = "base";
    f->fn = grow;
    f->anim = (gp_animation){ .duration = 2, .repeat = true, .ping_pong = true };
    rerender(root); wait_draws(2);
    for (int i = 0; i < 6; i++) step(0.5);
    CHECK(b = box("box"), "box");
    CHECK(b && b->style.h.v == 16 && b->style.has_bg, "the rest of the style changed: %.1f", b ? b->style.h.v : -1);

    /* what a half-way animation set is gone when the next half sets something else */
    reset(f);
    f->show = true;
    f->fn = flip;
    f->anim = (gp_animation){ .duration = 40 };
    rerender(root); wait_draws(2);
    step(10.0);
    b = box("box");
    CHECK(b && b->style.has_bg && b->style.bg.b > 0.9f && b->style.bg.r < 0.1f, "first half: the background should be blue");
    step(20.0);
    b = box("box");
    CHECK(b && NEAR(b->size.w, 77, 0.5f), "second half: width %.1f", b ? b->size.w : -1);
    CHECK(b && b->style.has_bg && b->style.bg.r > 0.9f && b->style.bg.b < 0.1f, "second half: the element's own red background should be back, not left blue (restored from the original style)");

    /* a function that returns nothing, and one that tries to start an animation of its own */
    reset(f);
    f->show = true;
    f->id = "none";
    f->fn = nothing;
    f->anim = (gp_animation){ .duration = 2 };
    rerender(root); wait_draws(2);
    step(1.0);
    CHECK(opacity("box") == 1.0f, "an animation with no modifiers changed the opacity to %.3f", opacity("box"));
    reset(f);
    f->show = true;
    f->id = "sneak";
    f->fn = sneaky;
    f->anim = (gp_animation){ .duration = 2 };
    rerender(root); wait_draws(2);
    step(1.0);
    CHECK(box("box") && box("box")->anim && box("box")->anim->next == NULL, "an animation function added an animation to the element");

    /* the id may be NULL */
    reset(f);
    f->show = true;
    f->id = NULL;
    rerender(root); wait_draws(2);
    step(0.5);
    CHECK(opacity("box") > 0.1f, "a NULL id: %.3f", opacity("box"));
}

/* In a cached view: no render, yet it moves; the cached layout and painted copy are not used stale. */
static void test_cached_view(void) {
    where = "test_cached_view";
    Fix *f = fix(root);
    Fix *k = fix(other);
    reset(f);
    reset(k);
    f->show = false;
    f->kid = other;
    f->kid_cached = true;
    k->show = true;
    k->id = "cached";
    k->fn = grow;
    k->anim = (gp_animation){ .duration = 80 };
    rerender(root); wait_draws(3);
    int renders = k->renders;
    step(20.0);
    CHECK(NEAR(width("box"), 35, 4), "an animation inside a cached view: width %.2f", width("box"));
    step(20.0);
    CHECK(NEAR(width("box"), 60, 4), "...later: %.2f", width("box"));
    CHECK(k->renders == renders, "the cached view rendered %d times for an animation", k->renders - renders);

    /* an unrelated rebuild of the root: the cached view is reused, and still shows where it is */
    unsigned long skips = gp_layout_skips();
    step(0.0);
    rerender(root); wait_draws(3);
    CHECK(NEAR(width("box"), 60, 5), "after the root rebuilt: %.2f", width("box"));
    CHECK(k->renders == renders, "the cached view rendered after an unrelated rebuild");
    (void)skips;
    step(20.0);
    CHECK(NEAR(width("box"), 85, 5), "...and moves on: %.2f", width("box"));
    step(100.0);
    CHECK(width("box") == 110, "finished: %.2f", width("box"));

    /* once finished, it is cached like anything: layout skipped and paint reused again */
    wait_draws(2);
    rerender(root); wait_draws(2);
    unsigned long s0 = gp_layout_skips(), h0 = app->stat_paint_hits;
    rerender(root); wait_draws(2);
    CHECK(gp_layout_skips() > s0, "a finished animation in a cached view still re-laid out every time");
    CHECK(app->stat_paint_hits > h0, "...and repainted");
    CHECK(k->renders == renders, "rendered");

    /* an animation that changes only paint (opacity), in a cached view */
    reset(k);
    k->show = true;
    k->id = "cached2";
    k->fn = fade;
    k->anim = (gp_animation){ .duration = 80 };
    rerender(other); wait_draws(3);
    step(40.0);
    CHECK(NEAR(opacity("box"), 0.5f, 0.07f), "opacity in a cached view: %.3f", opacity("box"));
    step(20.0);
    CHECK(NEAR(opacity("box"), 0.75f, 0.07f), "...and later: %.3f", opacity("box"));

    /* an inherited property: text below the animated element is measured at the new size (prepare walks again) */
    reset(k);
    k->show = true;
    k->fn = bigger;
    k->anim = (gp_animation){ .duration = 80 };
    rerender(other); wait_draws(3);
    step(1.0);
    gp_el *tb0 = box("box");
    float th0 = tb0 && tb0->nchildren ? tb0->children[0]->size.h : -1;
    step(40.0);
    gp_el *tb1 = box("box");
    float th1 = tb1 && tb1->nchildren ? tb1->children[0]->size.h : -1;
    CHECK(th0 > 0 && th1 > th0 * 1.4f, "text height %.1f then %.1f: a bigger inherited size did not reach the text in a cached view", th0, th1);

    f->kid = (gp_entity){ 0 };
    reset(k);
    rerender(root); wait_draws(2);
}

static void test_rows(void) {
    where = "test_rows";
    Fix *f = fix(root);
    reset(f);
    f->rows = 200;
    f->list_scroll = gp_scroll_new(app);
    rerender(root); wait_draws(3);
    gp_el *r0 = box("row 0");
    CHECK(r0 != NULL, "row 0 not found");
    CHECK(r0 && r0->style.opacity < 0.2f, "a new row starts at %.3f", r0 ? r0->style.opacity : -1);
    step(10.0);
    r0 = box("row 0");
    CHECK(r0 && NEAR(r0->style.opacity, 0.5f, 0.07f), "row 0 after 1 of 2 s: %.3f", r0 ? r0->style.opacity : -1);
    gp_el *r1 = box("row 1");
    CHECK(r1 && NEAR(r1->style.opacity, 0.5f, 0.07f), "row 1 has its own animation, started with the list: %.3f", r1 ? r1->style.opacity : -1);
    step(20.0);
    r0 = box("row 0");
    CHECK(r0 && r0->style.opacity == 1.0f, "row 0 finished at %.3f", r0 ? r0->style.opacity : -1);

    /* a wheel brings in rows that start fresh, in the middle of a frame that nothing else animates */
    gp_scroll_wheel(win(), (gp_point){ 10, 60 }, 0, 240);
    wait_draws(2);
    gp_el *r8 = box("row 12");
    if (r8) CHECK(r8->style.opacity < 0.5f, "a row brought in by the wheel starts at %.3f", r8->style.opacity);
    else CHECK(false, "row 12 not realized after scrolling");
    step(10.0);
    r8 = box("row 12");
    CHECK(r8 && NEAR(r8->style.opacity, 0.5f, 0.1f), "...and runs: %.3f", r8 ? r8->style.opacity : -1);
    f->rows = 0;
    rerender(root); wait_draws(2);
}

/* No view needs to be alive for a frame request to come: closing everything while one is pending must not crash (ASan). */
static void test_shutdown_with_animation(void) {
    where = "test_shutdown_with_animation";
    Fix *f = fix(root);
    reset(f);
    f->show = true;
    f->id = "last";
    f->anim = (gp_animation){ .duration = 100, .repeat = true };
    rerender(root); wait_draws(2);
}

/* ------------------------------------------------------------ driver */

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    gp_sleep(co, 0.3);
    check_frames = true;
    test_one_run();
    test_frames_while_running();
    test_easing_repeat_delay();
    test_layout_changes();
    test_ids_and_identity();
    test_group_and_oddities();
    test_cached_view();
    test_rows();
    test_shutdown_with_animation();
    gp_debug_clock_skew(0);
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    Fix init = { 0 };
    other = gp_view_new(app, sizeof(Fix), &init, render, NULL);
    root = gp_view_new(app, sizeof(Fix), &init, render, NULL);
    fix(root)->kid = other;
    gp_open_window(app, &(gp_window_options){ .title = "anim_test", .width = 300, .height = 220 }, root);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);   /* a physical pointer over the window is not part of the test */
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "anim_test: %d FAILURE(S)\n" : "anim_test: all checks passed\n", failures);
    return failures != 0;
}
