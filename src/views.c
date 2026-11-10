/*
 * Which views a window shows, and which of them have to render again.
 *
 * A window keeps one entry per view its last builds reached: who built it (its parent view),
 * whether it was notified since it last rendered (`dirty`), whether something below it was
 * (`subtree_dirty`), and for cached views (gp_child_view_cached) the element tree itself, in an arena
 * of its own so that it survives the window's arena being reset for the next build.
 *
 * gp_view_dirty marks a view and walks up the parent links marking its ancestors, because a cached
 * ancestor holds the elements of everything built inside it and has to build them again. A build
 * walks down from the root: ordinary views always render, a cached view renders only if it, or a view
 * below it, was marked (or everything was invalidated); otherwise its old tree is reused as it is.
 * Layout and paint still run over the whole tree.
 */
#include "internal.h"

#define VIEW_ARENA_CHUNK (16 * 1024)

static size_t slot_of(const gp_vviews *t, uint32_t index) { return (size_t)(index * 2654435761u) & (t->cap - 1); }

gp_vview *gp_views_find(gp_vviews *t, gp_entity e) {
    if (!t->cap || !e.generation) return NULL;
    for (size_t i = slot_of(t, e.index);; i = (i + 1) & (t->cap - 1)) {
        gp_vview *v = &t->v[i];
        if (!v->entity.generation) return NULL;
        if (v->entity.index == e.index) return v->entity.generation == e.generation ? v : NULL;
    }
}

static void drop_cache(gp_vview *v) {
    if (v->arena) {
        gp_arena_free(v->arena);
        free(v->arena);
    }
    v->arena = NULL;
    v->root = NULL;
    v->cached = false;
    v->chunk_valid = false;   /* painted from the tree that is gone */
}

static void drop_entry(gp_vview *v) {
    drop_cache(v);
    free(v->kids);
    free(v->chunk);
    free(v->svgs);
    v->kids = NULL;
    v->chunk = NULL;
    v->svgs = NULL;
    v->nsvgs = v->cap_svgs = 0;
    v->nkids = v->cap_kids = 0;
    v->nchunk = v->cap_chunk = 0;
}

static void grow(gp_vviews *t, size_t want) {
    size_t cap = 16;
    while (cap < want * 2) cap *= 2;
    gp_vview *old = t->v;
    size_t old_cap = t->cap;
    t->v = calloc(cap, sizeof *t->v);
    if (!t->v) abort();
    t->cap = cap;
    t->len = 0;
    for (size_t i = 0; i < old_cap; i++) {
        if (!old[i].entity.generation) continue;
        size_t j = slot_of(t, old[i].entity.index);
        while (t->v[j].entity.generation) j = (j + 1) & (cap - 1);
        t->v[j] = old[i];
        t->len++;
    }
    free(old);
}

gp_vview *gp_views_touch(gp_vviews *t, gp_entity e, gp_entity parent) {
    gp_vview *v = gp_views_find(t, e);
    if (!v) {
        if ((t->len + 1) * 2 > t->cap) grow(t, t->len + 1);
        size_t i = slot_of(t, e.index);
        while (t->v[i].entity.generation && t->v[i].entity.index != e.index) i = (i + 1) & (t->cap - 1);
        v = &t->v[i];
        if (v->entity.generation) {   /* the slot of a released entity, reused by this one */
            drop_entry(v);
        } else {
            t->len++;
        }
        *v = (gp_vview){ .entity = e };
    }
    v->parent = parent;
    return v;
}

void gp_views_begin(gp_window *w) { w->views.epoch++; }

void gp_views_add_kid(gp_vviews *t, gp_entity parent, gp_entity kid) {
    gp_vview *p = gp_views_find(t, parent);
    if (!p) return;
    if (p->nkids == p->cap_kids) {
        p->cap_kids = p->cap_kids ? p->cap_kids * 2 : 4;
        p->kids = realloc(p->kids, p->cap_kids * sizeof *p->kids);
        if (!p->kids) abort();
    }
    p->kids[p->nkids++] = kid;
}

/* A cached view is reused without its render function, or anything inside it, being visited: its
 * views must count as reached, or the sweep would drop them while the tree being reused still points
 * into their arenas. */
void gp_views_keep_subtree(gp_vviews *t, gp_vview *v) {
    for (uint32_t i = 0; i < v->nkids; i++) {
        gp_vview *k = gp_views_find(t, v->kids[i]);
        if (!k || k->used_epoch == t->epoch) continue;
        k->used_epoch = t->epoch;
        gp_views_keep_subtree(t, k);
    }
}

/* Whether the sweep keeps an entry. Views reached by the build do; so do the views in rows of lists whose
 * layout was skipped: nothing built those rows again, and they are still on screen. Their entries are what
 * a notify on them finds, to make the window and the cached view around the list render again. */
static bool survives(const gp_vviews *t, const gp_vview *v) {
    if (v->used_epoch == t->epoch) return true;
    if (!v->from_row) return false;
    gp_entity p = v->parent;
    for (int depth = 0; p.generation && depth < 4096; depth++) {
        const gp_vview *pv = gp_views_find((gp_vviews *)t, p);
        if (!pv) return false;
        if (pv->skipped_epoch == t->epoch) return true;
        p = pv->parent;
    }
    return false;
}

/* Forgets the views the build that just finished did not reach: removed from their parent's render,
 * hidden, or released. Their element trees go with them (nothing references those any more: the
 * window's tree has just been replaced). */
void gp_views_sweep(gp_window *w) {
    gp_vviews *t = &w->views;
    size_t live = 0;
    bool *keep = calloc(t->cap ? t->cap : 1, sizeof *keep);   /* decided while the table is whole: survives() looks parents up */
    if (!keep) abort();
    for (size_t i = 0; i < t->cap; i++) {
        gp_vview *v = &t->v[i];
        if (!v->entity.generation) continue;
        if ((keep[i] = survives(t, v))) live++;
    }
    if (live == t->len) { free(keep); return; }
    gp_vview *old = t->v;
    size_t old_cap = t->cap;
    t->v = NULL;
    t->cap = t->len = 0;
    if (live) grow(t, live);
    for (size_t i = 0; i < old_cap; i++) {
        gp_vview *v = &old[i];
        if (!v->entity.generation) continue;
        if (!keep[i]) {
            drop_entry(v);
            continue;
        }
        size_t j = slot_of(t, v->entity.index);
        while (t->v[j].entity.generation) j = (j + 1) & (t->cap - 1);
        t->v[j] = *v;
        t->len++;
    }
    free(old);
    free(keep);
}

void gp_views_free(gp_window *w) {
    for (size_t i = 0; i < w->views.cap; i++)
        if (w->views.v[i].entity.generation) drop_entry(&w->views.v[i]);
    free(w->views.v);
    w->views = (gp_vviews){ 0 };
}

/* ---------------------------------------------------------------- marking */

static void mark(gp_window *w, gp_vview *v) {
    v->dirty = true;
    w->dirty_render = true;
    gp_entity p = v->parent;
    for (int depth = 0; p.generation && depth < 4096; depth++) {   /* the cap guards against stale links forming a cycle */
        gp_vview *pv = gp_views_find(&w->views, p);
        if (!pv || pv->subtree_dirty) break;   /* an ancestor that is already marked has marked its own */
        pv->subtree_dirty = true;
        p = pv->parent;
    }
}

void gp_view_dirty(gp_app *app, gp_entity e) {
    for (int i = 0; i < app->nwindows; i++) {
        gp_window *w = app->windows[i];
        gp_vview *v = gp_views_find(&w->views, e);
        if (v) mark(w, v);
    }
}

void gp_views_invalidate_all(gp_app *app) {
    for (int i = 0; i < app->nwindows; i++) {
        app->windows[i]->dirty_render = true;
        app->windows[i]->views.all_dirty = true;
    }
}

void gp_view_reads_focus(gp_window *w, gp_entity view) {
    gp_vview *v = w ? gp_views_find(&w->views, view) : NULL;
    if (v) v->reads_focus = true;
}

void gp_view_uses_svg(gp_window *w, gp_entity view, gp_svg svg) {
    if (!w) return;
    gp_vview *v = view.generation ? gp_views_find(&w->views, view) : NULL;
    if (!v) { w->views.svg_loose = true; return; }
    for (uint32_t i = 0; i < v->nsvgs; i++)
        if (v->svgs[i].index == svg.index && v->svgs[i].generation == svg.generation) return;
    if (v->nsvgs == v->cap_svgs) {
        v->cap_svgs = v->cap_svgs ? v->cap_svgs * 2 : 4;
        v->svgs = realloc(v->svgs, v->cap_svgs * sizeof *v->svgs);
        if (!v->svgs) abort();
    }
    v->svgs[v->nsvgs++] = svg;
}

void gp_views_svg_released(gp_app *app, gp_svg svg) {
    for (int k = 0; k < app->nwindows; k++) {
        gp_window *w = app->windows[k];
        if (w->views.svg_loose) {   /* (shown by something no view owns: everything, as before) */
            w->dirty_render = true;
            w->views.all_dirty = true;
            continue;
        }
        for (size_t i = 0; i < w->views.cap; i++) {
            gp_vview *v = &w->views.v[i];
            if (!v->entity.generation) continue;
            for (uint32_t j = 0; j < v->nsvgs; j++)
                if (v->svgs[j].index == svg.index && v->svgs[j].generation == svg.generation) { mark(w, v); break; }
        }
    }
}

void gp_views_focus_changed(gp_window *w) {
    for (size_t i = 0; i < w->views.cap; i++) {
        gp_vview *v = &w->views.v[i];
        if (v->entity.generation && v->reads_focus) mark(w, v);
    }
}

/* Used by gp_build_view for cached views. */
gp_arena *gp_view_arena(gp_vview *v) {
    if (!v->arena) {
        v->arena = calloc(1, sizeof *v->arena);
        if (!v->arena) abort();
        v->arena->chunk_size = VIEW_ARENA_CHUNK;
    }
    return v->arena;
}

void gp_view_drop_cache(gp_vview *v) { drop_cache(v); }
