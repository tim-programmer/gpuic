/*
 * Declarative animations (gp_with_animation).
 *
 * An animated element carries a list of gp_anim (what to call and how long it runs); the window keeps, per
 * name, when that animation started. Before every layout the tree is walked, but only along the paths that
 * lead to an animated element (gp_el.anim_below): each animation's progress is worked out from the clock,
 * and an element whose progress moved gets its style reset to what it was built with and the modifiers the
 * animation functions return applied on top. The view that built the element is not involved, so a cached
 * view (gp_child_view_cached) animates without rendering; what that costs it is its cached layout and
 * painted copy, which are dropped for the roots around an element that changed.
 *
 * While anything is unfinished the table says `running`, and the window asks for a frame at the display's
 * refresh rate (app.c); when everything is finished no frames are requested at all.
 */
#include "internal.h"

#include <math.h>

#define MAX_CACHED_DEPTH 256

void gp_anim_begin(gp_anim_table *t) {
    t->pass++;
    t->running = false;
    t->now = gp_now_ns();
}

/* An animation that was not met in the last layout is forgotten, so an element that went away starts over
 * when it returns. */
void gp_anim_end(gp_anim_table *t) {
    size_t out = 0;
    for (size_t i = 0; i < t->len; i++)
        if (t->items[i].seen == t->pass) t->items[out++] = t->items[i];
    t->len = out;
}

void gp_anim_free(gp_anim_table *t) {
    free(t->items);
    gp_arena_free(&t->arena);
    t->items = NULL;
    t->len = t->cap = 0;
}

static gp_anim_state *state_for(gp_anim_table *t, uint64_t key) {
    for (size_t i = 0; i < t->len; i++)
        if (t->items[i].key == key) return &t->items[i];
    if (t->len == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 8;
        t->items = realloc(t->items, t->cap * sizeof *t->items);
        if (!t->items) abort();
    }
    t->items[t->len] = (gp_anim_state){ .key = key, .start_ns = t->now };
    return &t->items[t->len++];
}

/* Eased progress of `a` after `elapsed_ns`, and whether it is still going. */
static float progress(const gp_animation *a, int64_t elapsed_ns, bool *running) {
    double el = (double)elapsed_ns / 1e9 - a->delay;
    if (el < 0) {
        *running = true;   /* waiting for its delay */
        return 0;
    }
    if (a->duration <= 0) {
        *running = false;
        return 1;
    }
    double x = el / a->duration;
    if (a->repeat) {
        *running = true;
        double whole = floor(x), f = x - whole;
        if (a->ping_pong && ((long long)whole & 1)) f = 1 - f;
        return gp_ease(a->easing, (float)f);
    }
    if (x >= 1) {
        *running = false;
        return 1;
    }
    *running = true;
    return gp_ease(a->easing, (float)x);
}

typedef struct {
    gp_anim_table *t;
    gp_el *cached[MAX_CACHED_DEPTH];   /* the cached view roots on the way down */
    int ncached;
} walk;

static void eval_element(walk *w, gp_el *n) {
    gp_anim_table *t = w->t;
    bool changed = false;
    for (gp_anim *a = n->anim; a; a = a->next) {
        gp_anim_state *st = state_for(t, a->key);
        st->seen = t->pass;
        bool running;
        a->cur_t = progress(&a->spec, t->now - st->start_ns, &running);
        if (running) t->running = true;
        if (!a->applied || a->applied_t != a->cur_t) changed = true;
    }
    if (!changed) return;

    gp_anim *head = n->anim;
    bool before = head->applied;   /* this element was already showing an earlier progress */
    if (!head->base_valid) {
        head->base = n->style;
        head->base_valid = true;
    } else {
        n->style = head->base;
        n->lstate_on = 0;   /* (a state style it was laid out in is gone with the rest: layout puts it back over what the animation sets) */
    }
    gp_arena_reset(&t->arena);
    for (gp_anim *a = n->anim; a; a = a->next) {
        gp_build_ctx bc = { &t->arena, t->app, t->window, a->view, true };
        gp_el_apply_animation(n, a, &bc);
        a->applied = true;
        a->applied_t = a->cur_t;
    }
    /* The roots of cached views around it keep their old layout and painting unless told: they are not
     * built again, and nothing else tells them this element changed. Without prep_valid, prepare walks the
     * subtree again (an inherited text size may have changed) and with it forgets that it was laid out, so
     * layout runs and the painted copy, which is tied to the layout, goes too. */
    if (before)
        for (int i = 0; i < w->ncached; i++) w->cached[i]->prep_valid = false;
}

static void walk_tree(walk *w, gp_el *n) {
    if (!n->anim_below) return;
    bool root = n->cached_view.generation != 0 && w->ncached < MAX_CACHED_DEPTH;
    if (root) w->cached[w->ncached++] = n;
    if (n->anim) eval_element(w, n);
    for (uint32_t i = 0; i < n->nchildren; i++) walk_tree(w, n->children[i]);
    if (root) w->ncached--;
}

void gp_anim_eval(gp_el *root, gp_anim_table *t) {
    if (!root || !t) return;
    walk w = { .t = t };
    walk_tree(&w, root);
}
