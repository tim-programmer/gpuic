/*
 * What cpp_test.cpp cannot see through the public API: the laid-out tree and the pixels of a frame. The test builds the same
 * interface twice, once with the C macros and once with the C++ chain, and asks here whether the two came out the same.
 */
#include "../src/internal.h"

#include <stdio.h>
#include <string.h>

static const gp_el *walk(const gp_el *n, const int *path, int depth, float *ox, float *oy) {
    *ox = n->pos.x;
    *oy = n->pos.y;
    for (int i = 0; i < depth; i++) {
        if (path[i] < 0 || (uint32_t)path[i] >= n->nchildren) return NULL;
        n = n->children[path[i]];
        *ox += n->pos.x;
        *oy += n->pos.y;
    }
    return n;
}

static const gp_el *root_of(gp_app *app) { return app->nwindows ? app->windows[0]->tree : NULL; }

int cpp_probe_children(gp_app *app, const int *path, int depth) {
    float x, y;
    const gp_el *r = root_of(app);
    const gp_el *n = r ? walk(r, path, depth, &x, &y) : NULL;
    return n ? (int)n->nchildren : -1;
}

/* the element's rectangle in window coordinates */
int cpp_probe_rect(gp_app *app, const int *path, int depth, float out[4]) {
    float x, y;
    const gp_el *r = root_of(app);
    const gp_el *n = r ? walk(r, path, depth, &x, &y) : NULL;
    if (!n) return 0;
    out[0] = x - r->pos.x;
    out[1] = y - r->pos.y;
    out[2] = n->size.w;
    out[3] = n->size.h;
    return 1;
}

/* the text of a text element, NULL for anything else */
const char *cpp_probe_text(gp_app *app, const int *path, int depth) {
    float x, y;
    const gp_el *r = root_of(app);
    const gp_el *n = r ? walk(r, path, depth, &x, &y) : NULL;
    return n && n->kind == GP_EL_TEXT ? n->text : NULL;
}

/* A subtree against another: the same kinds, sizes, positions inside their parents, texts and number of children all the way down.
 * Returns NULL if they match, else says where they do not. */
static const char *same(const gp_el *a, const gp_el *b, char *why, size_t cap, int depth) {
    if (a->kind != b->kind) { snprintf(why, cap, "kind %d vs %d at depth %d", (int)a->kind, (int)b->kind, depth); return why; }
    if (a->size.w != b->size.w || a->size.h != b->size.h) { snprintf(why, cap, "size %gx%g vs %gx%g at depth %d", a->size.w, a->size.h, b->size.w, b->size.h, depth); return why; }
    if (depth && (a->pos.x != b->pos.x || a->pos.y != b->pos.y)) { snprintf(why, cap, "position %g,%g vs %g,%g at depth %d", a->pos.x, a->pos.y, b->pos.x, b->pos.y, depth); return why; }
    if (a->nchildren != b->nchildren) { snprintf(why, cap, "%u vs %u children at depth %d", a->nchildren, b->nchildren, depth); return why; }
    if (a->kind == GP_EL_TEXT && strcmp(a->text, b->text) != 0) { snprintf(why, cap, "text \"%s\" vs \"%s\"", a->text, b->text); return why; }
    for (uint32_t i = 0; i < a->nchildren; i++)
        if (same(a->children[i], b->children[i], why, cap, depth + 1)) return why;
    if ((a->mask == NULL) != (b->mask == NULL)) { snprintf(why, cap, "a mask on one only at depth %d", depth); return why; }
    if (a->mask && same(a->mask, b->mask, why, cap, depth + 1)) return why;
    return NULL;
}

const char *cpp_probe_same(gp_app *app, const int *pa, int da, const int *pb, int db, char *why, size_t cap) {
    float x, y;
    const gp_el *r = root_of(app);
    const gp_el *a = r ? walk(r, pa, da, &x, &y) : NULL, *b = r ? walk(r, pb, db, &x, &y) : NULL;
    if (!a || !b) { snprintf(why, cap, "no such element"); return why; }
    return same(a, b, why, cap, 0);
}

/* a frame as RGB bytes: width and height (device pixels) are returned; the buffer is the app's, valid until the next capture */
const unsigned char *cpp_probe_capture(gp_app *app, gp_co *co, int *w, int *h) {
    gp_debug_capture_next(app);
    for (int i = 0; i < 1000 && !app->capture_px; i++) {
        app->windows[0]->dirty_paint = true;
        gp_sleep(co, 0.01);
    }
    *w = app->capture_w;
    *h = app->capture_h;
    return app->capture_px;
}

unsigned long cpp_probe_draws(gp_app *app) { return app->stat_draws; }
void cpp_probe_repaint(gp_app *app) { app->windows[0]->dirty_paint = true; }
void cpp_probe_rebuild(gp_app *app) { app->windows[0]->dirty_render = true; }

/* A number out of an element's style, by the code the test uses. */
float cpp_probe_style(gp_app *app, const int *path, int depth, int field) {
    float x, y;
    const gp_el *r = root_of(app);
    const gp_el *n = r ? walk(r, path, depth, &x, &y) : NULL;
    if (!n) return -12345.f;
    const gp_style *s = &n->style;
    switch (field) {
    case 0: return s->gap;
    case 1: case 2: case 3: case 4: return s->pad_user[field - 1];
    case 5: case 6: case 7: case 8: return s->margin[field - 5];
    case 9: return s->w.v;
    case 10: return (float)s->w.kind;
    case 11: return s->h.v;
    case 12: return (float)s->h.kind;
    case 13: return s->min_w.v;
    case 14: return s->min_h.v;
    case 15: return s->max_w.v;
    case 16: return s->max_h.v;
    case 17: case 18: case 19: case 20: return s->inset[field - 17].v;
    case 21: return s->radius;
    case 22: return s->font_size;
    case 23: return (float)s->font_weight;
    case 24: return s->border_uniform;
    case 25: return s->shadow.dx;
    case 26: return s->shadow.dy;
    case 27: return s->shadow.blur;
    case 28: return s->shadow.color.a;
    case 29: return s->border_side[0];
    case 30: return s->border_side[1];
    case 31: return s->border_side[2];
    case 32: return s->border_side[3];
    case 33: return (float)s->border_set;
    default: return -12346.f;
    }
}

/* The paint-only part of two state styles (gp_hover_style...): the fields they can set, by value. */
static bool color_eq(gp_color a, gp_color b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }
static bool state_same(const gp_style *a, const gp_style *b) {
    if ((a == NULL) != (b == NULL)) return false;
    if (!a) return true;
    return a->has_bg == b->has_bg && color_eq(a->bg, b->bg) && color_eq(a->border_color, b->border_color) && a->opacity == b->opacity &&
           a->has_shadow == b->has_shadow && a->shadow.dx == b->shadow.dx && a->shadow.dy == b->shadow.dy && a->shadow.blur == b->shadow.blur &&
           color_eq(a->shadow.color, b->shadow.color) && a->radius == b->radius && a->border_style == b->border_style &&
           a->corner_set == b->corner_set && a->bcolor_set == b->bcolor_set;
}

/* Two elements of the tree (children of the root, by index) made with one modifier each, the C++ one and the C one: do they hold the
 * same? The style as a whole, with the strings compared by content and the gradient by what it holds (their addresses differ), and the
 * element's own fields that modifiers set. Returns NULL if so, else what differs. */
const char *cpp_probe_el_same(gp_app *app, int ia, int ib, char *why, size_t cap) {
    const gp_el *r = root_of(app);
    if (!r || (uint32_t)ia >= r->nchildren || (uint32_t)ib >= r->nchildren) { snprintf(why, cap, "no such element"); return why; }
    const gp_el *a = r->children[ia], *b = r->children[ib];
    gp_style sa = a->style, sb = b->style;
    if ((sa.font_family == NULL) != (sb.font_family == NULL) || (sa.font_family && strcmp(sa.font_family, sb.font_family) != 0)) { snprintf(why, cap, "font family"); return why; }
    if ((sa.font_features == NULL) != (sb.font_features == NULL) || (sa.font_features && strcmp(sa.font_features, sb.font_features) != 0)) { snprintf(why, cap, "font features"); return why; }
    if ((sa.bg_gradient == NULL) != (sb.bg_gradient == NULL) || (sa.bg_gradient && memcmp(sa.bg_gradient, sb.bg_gradient, sizeof *sa.bg_gradient) != 0)) { snprintf(why, cap, "gradient"); return why; }
    /* a length is a byte and a float: the bytes between are whatever the compound literal that set it left there */
    gp_length *la[] = { &sa.basis, &sa.w, &sa.h, &sa.min_w, &sa.min_h, &sa.max_w, &sa.max_h, &sa.inset[0], &sa.inset[1], &sa.inset[2], &sa.inset[3] };
    gp_length *lb[] = { &sb.basis, &sb.w, &sb.h, &sb.min_w, &sb.min_h, &sb.max_w, &sb.max_h, &sb.inset[0], &sb.inset[1], &sb.inset[2], &sb.inset[3] };
    for (size_t i = 0; i < sizeof la / sizeof *la; i++) {
        gp_length na, nb;
        memset(&na, 0, sizeof na);
        memset(&nb, 0, sizeof nb);
        na.kind = la[i]->kind; na.v = la[i]->v;
        nb.kind = lb[i]->kind; nb.v = lb[i]->v;
        *la[i] = na;
        *lb[i] = nb;
    }
    if ((sa.font_variations == NULL) != (sb.font_variations == NULL) || (sa.font_variations && strcmp(sa.font_variations, sb.font_variations) != 0)) { snprintf(why, cap, "font variations"); return why; }
    if (!state_same(sa.hover_style, sb.hover_style)) { snprintf(why, cap, "hover style"); return why; }
    if (!state_same(sa.active_style, sb.active_style)) { snprintf(why, cap, "active style"); return why; }
    if (!state_same(sa.focus_style, sb.focus_style)) { snprintf(why, cap, "focus style"); return why; }
    sa.hover_style = sb.hover_style = sa.active_style = sb.active_style = sa.focus_style = sb.focus_style = NULL;
    sa.font_variations = sb.font_variations = NULL;
    sa.font_family = sb.font_family = NULL;
    sa.font_features = sb.font_features = NULL;
    if ((sa.path_text == NULL) != (sb.path_text == NULL) || (sa.path_text && strcmp(sa.path_text, sb.path_text) != 0)) { snprintf(why, cap, "path text"); return why; }
    if (sa.dash_n != sb.dash_n || (sa.dash_n && memcmp(sa.dash, sb.dash, sa.dash_n * sizeof *sa.dash) != 0)) { snprintf(why, cap, "dashes"); return why; }
    if ((sa.stroke_gradient == NULL) != (sb.stroke_gradient == NULL) || (sa.stroke_gradient && memcmp(sa.stroke_gradient, sb.stroke_gradient, sizeof *sa.stroke_gradient) != 0)) { snprintf(why, cap, "stroke gradient"); return why; }
    sa.bg_gradient = sb.bg_gradient = NULL;
    sa.path_text = sb.path_text = NULL;
    sa.dash = sb.dash = NULL;
    sa.stroke_gradient = sb.stroke_gradient = NULL;
    if (sa.nfilters != sb.nfilters) { snprintf(why, cap, "filter count"); return why; }
    for (int i = 0; i < sa.nfilters; i++) {
        const gp_filter_op *x = &sa.filters[i], *y = &sb.filters[i];
        if (x->kind != y->kind || x->v != y->v || x->dx != y->dx || x->dy != y->dy || memcmp(&x->color, &y->color, sizeof x->color) != 0) { snprintf(why, cap, "filter %d", i); return why; }
    }
    sa.filters = sb.filters = NULL;
    if (memcmp(&sa, &sb, sizeof sa) != 0) {
        const unsigned char *x = (const unsigned char *)&sa, *y = (const unsigned char *)&sb;
        size_t i = 0;
        while (x[i] == y[i]) i++;
        snprintf(why, cap, "style differs at byte %zu of %zu", i, sizeof sa);
        return why;
    }
    if (a->kind != b->kind || a->focus_id != b->focus_id || a->tab_stop != b->tab_stop || a->tab_index != b->tab_index || a->scroll_id != b->scroll_id ||
        a->list_stick != b->list_stick || a->has_ime_rect != b->has_ime_rect || memcmp(a->ime_rect, b->ime_rect, sizeof a->ime_rect) != 0 ||
        a->hide_below != b->hide_below || a->anim_below != b->anim_below) {
        snprintf(why, cap, "element fields (focus %llu/%llu, scroll id %llu/%llu, tab %d/%d)", (unsigned long long)a->focus_id, (unsigned long long)b->focus_id,
                 (unsigned long long)a->scroll_id, (unsigned long long)b->scroll_id, a->tab_index, b->tab_index);
        return why;
    }
    if ((a->key_context == NULL) != (b->key_context == NULL) || (a->key_context && strcmp(a->key_context, b->key_context) != 0)) { snprintf(why, cap, "key context"); return why; }
    return NULL;
}

void cpp_probe_wheel(gp_app *app, float x, float y, float dx, float dy) { gp_scroll_wheel(app->windows[0], (gp_point){ x, y }, dx, dy); }

/* the text of a list's pinned header (gp_list_sticky), NULL if none */
const char *cpp_probe_sticky_text(gp_app *app, const int *path, int depth) {
    float x, y;
    const gp_el *r = root_of(app);
    const gp_el *n = r ? walk(r, path, depth, &x, &y) : NULL;
    const gp_el *h = n ? n->sticky : NULL;
    while (h && h->kind != GP_EL_TEXT && h->nchildren) h = h->children[0];
    return h && h->kind == GP_EL_TEXT ? h->text : NULL;
}

/* gp_set_smooth_scrolling's setting, in milliseconds. */
int cpp_probe_smooth_ms(gp_app *app) { return (int)lroundf(app->smooth_scroll_s * 1000.f); }

static const char *first_text(const gp_el *e) {
    if (e->kind == GP_EL_TEXT) return e->text;
    for (uint32_t i = 0; i < e->nchildren; i++) {
        const char *t = first_text(e->children[i]);
        if (t) return t;
    }
    return NULL;
}

/* The row at the top of the view of the list that is the root's child `idx` (the first whose bottom is below the top edge): its text, and
 * how far its top is from the top of the view. NULL if there is none. */
const char *cpp_probe_list_first(gp_app *app, int idx, float *y) {
    const gp_el *r = root_of(app);
    if (!r || (uint32_t)idx >= r->nchildren) return NULL;
    const gp_el *l = r->children[idx];
    for (uint32_t i = 0; i < l->nchildren; i++) {
        const gp_el *c = l->children[i];
        float top = c->pos.y - l->style.border_w - l->scroll.y;
        if (top + c->size.h > 0.f) {
            *y = top;
            return first_text(c);
        }
    }
    return NULL;
}
