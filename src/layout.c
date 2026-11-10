/*
 * Flexbox layout (a pragmatic subset of CSS flexbox, border-box sizing).
 *
 * Supported: row/column, grow/shrink/basis, min/max, gap, padding, margin,
 * border, align-items/self (stretch/start/center/end), justify-content,
 * absolute children with insets, percentage lengths, wrapped text leaves.
 * Not yet: wrap, reverse directions, auto margins, grid.
 *
 * Like Taffy, sizing is split in two: layout_node(commit=false) only measures
 * and is memoized per node; commit=true then writes positions. Each node is
 * committed once, so nested flex containers stay linear-ish instead of
 * re-measuring their subtree at every level.
 */
#include "internal.h"

static gp_text_system *g_ts;
static gp_scroll_table *g_scrolls;

static inline bool def(float v) { return !isnan(v); }
static inline bool same(float a, float b) { return a == b || (isnan(a) && isnan(b)); }

static float resolve(gp_length l, float basis) {
    switch (l.kind) {
    case GP_PX: return l.v;
    case GP_FRAC: return def(basis) ? basis * l.v : NAN;
    default: return NAN;
    }
}

static float clamp_len(float v, gp_length lo, gp_length hi, float basis) {
    if (!def(v)) return v;
    float mx = resolve(hi, basis), mn = resolve(lo, basis);
    if (def(mx) && v > mx) v = mx;
    if (def(mn) && v < mn) v = mn;
    return v;
}

static inline float maxf(float a, float b) { return a > b ? a : b; }

/* ---------------------------------------------------------------- text style pass */

static bool g_frame_caches = true;
static gp_anim_table *g_anims;  /* the window's animations: rows built during layout may animate */
static gp_vviews *g_views;      /* the window's view table, to note which cached views were not laid out again */
static unsigned long g_skips;   /* subtrees whose layout was skipped, read by gp_layout_skips */
unsigned long gp_layout_skips(void) { return g_skips; }
void gp_debug_frame_caches(bool enabled) { g_frame_caches = enabled; }
bool gp_frame_caches_enabled(void) { return g_frame_caches; }

static bool same_text_style(const gp_text_style *a, const gp_text_style *b) {
    return a->size == b->size && a->weight == b->weight && a->family == b->family && a->italic == b->italic && a->features == b->features && a->variations == b->variations && a->align == b->align && a->dir == b->dir &&
           a->color.r == b->color.r && a->color.g == b->color.g && a->color.b == b->color.b && a->color.a == b->color.a;
}

/* Resolves inherited text style and gives every scroll container its state key: the
 * explicit handle id, else a hash of its position in the tree.
 *
 * Elements of a cached view outlive a build, and with them the sizes memoized during layout (n->cache).
 * Those hold as long as nothing they were computed from changed, and two things they depend on are not
 * part of their key: the text style inherited from the ancestors, which may differ now if the cached
 * view sits under another parent or its parent changed, and, for a list, the heights measured so far
 * (an auto-height list reports a different size once its rows are known). So a node forgets its
 * memoized sizes when its inherited style changed or it has a list somewhere below it.
 *
 * Returns GP_SUB_* flags for the subtree. The root of a cached view whose subtree has no list is not
 * walked again while its inherited style and position are what they were: nothing below it can have
 * changed, since it is built once and only layout writes to it. (A scroll container is no reason to walk:
 * its key is a function of its path.) A list below has rows that come and go, and memoized sizes above
 * it that go stale as rows are measured, so such a root is walked, but a walk that finds the same place
 * leaves the verdict that its layout is still valid alone. */
enum { PREP_STATE_CHANGED = 0x100 };   /* (prepare's result, above the GP_SUB_* flags: a state style was put on or taken off below) */

static const gp_lstate_set *g_lstates;
void gp_layout_use_states(const gp_lstate_set *states) { g_lstates = states; }

static uint8_t lstate_lookup(uint64_t path) {
    if (!g_lstates) return 0;
    size_t lo = 0, hi = g_lstates->len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uint64_t p = g_lstates->items[mid].path;
        if (p == path) return g_lstates->items[mid].bits;
        if (p < path) lo = mid + 1; else hi = mid;
    }
    return 0;
}

/* State styles that lay out: which states have one, and of those which the last paint found the element in (by its path). The
 * element's style is put back to what it is without states (lsave) and the ones it is in now put over it, hover, then pressed, then focus.
 * Returns whether that changed anything. */
static bool apply_lstates(gp_el *n, uint64_t path) {
    uint8_t avail = 0;
    if (n->lsave) {
        const gp_style *s = &n->style;
        if (s->hover_style && s->hover_style->lset) avail |= GP_LSTATE_HOVER;
        if (s->active_style && s->active_style->lset) avail |= GP_LSTATE_ACTIVE;
        if (s->focus_style && s->focus_style->lset) avail |= GP_LSTATE_FOCUS;
    }
    n->lstates = avail;
    uint8_t want = avail ? lstate_lookup(path) & avail : 0;
    if (want == n->lstate_on) return false;
    if (n->lstate_on) n->style = *n->lsave;
    if (want) {
        *n->lsave = n->style;
        if (want & GP_LSTATE_HOVER) gp_style_overlay_layout(&n->style, n->style.hover_style);
        if (want & GP_LSTATE_ACTIVE) gp_style_overlay_layout(&n->style, n->style.active_style);
        if (want & GP_LSTATE_FOCUS) gp_style_overlay_layout(&n->style, n->style.focus_style);
    }
    n->lstate_on = want;
    return true;
}

static unsigned prepare(gp_el *n, gp_text_style parent, uint64_t path) {
    bool state_changed = apply_lstates(n, path);
    gp_text_style ts = parent;
    if (n->style.font_size > 0) ts.size = n->style.font_size;
    if (n->style.font_weight) ts.weight = n->style.font_weight;
    if (n->style.font_family) ts.family = gp_text_family(g_ts, n->style.font_family);
    if (n->style.font_italic) ts.italic = n->style.font_italic == 1;
    if (n->style.font_features) ts.features = gp_text_features(g_ts, n->style.font_features);
    if (n->style.font_variations) ts.variations = gp_text_variations(g_ts, n->style.font_variations);
    if (n->style.has_text_color) ts.color = n->style.text_color;
    if (n->style.has_text_align) ts.align = n->style.text_align;
    if (n->style.has_text_dir) ts.dir = n->style.text_dir;

    bool cached_root = n->cached_view.generation != 0;
    bool same_place = cached_root && g_frame_caches && n->prep_valid && n->path == path && same_text_style(&ts, &n->ts);
    if (same_place && !state_changed && !(n->sub_flags & (GP_SUB_LIST | GP_SUB_LSTATE))) return n->sub_flags;
    bool was_laid_out = same_place && n->lk_valid;

    bool style_changed = ts.size != n->ts.size || ts.weight != n->ts.weight || ts.family != n->ts.family || ts.italic != n->ts.italic || ts.features != n->ts.features || ts.variations != n->ts.variations || ts.align != n->ts.align || ts.dir != n->ts.dir;
    n->ts = ts;
    n->scroll_key = n->scroll_id ? n->scroll_id : (path & ~GP_SCROLL_EXPLICIT) | 1;
    n->path = path;
    unsigned flags = state_changed ? PREP_STATE_CHANGED : 0;
    if (n->lstates) flags |= GP_SUB_LSTATE;
    if (n->kind == GP_EL_LIST) flags |= GP_SUB_LIST;
    if (n->style.overflow_x_scroll || n->style.overflow_y_scroll) flags |= GP_SUB_SCROLL;
    if (n->style.overflow_hidden) flags |= GP_SUB_CLIP;
    if (n->style.has_xf) flags |= GP_SUB_XF;
    if (n->style.has_hover_bg || n->style.has_active_bg || n->style.hover_style || n->style.active_style) flags |= GP_SUB_HOVER;
    if (n->focus_id) flags |= GP_SUB_FOCUS;
    if (n->style.autohide_scrollbar) flags |= GP_SUB_AUTOHIDE;
    for (uint32_t i = 0; i < n->nchildren; i++) {
        gp_el *c = n->children[i];
        c->parent = n;
        uint64_t k = c->key ? c->key : c->focus_id ? (c->focus_id * 0x9E3779B97F4A7C15ull) | 1 : 0;   /* its key, its focus, or its place */
        flags |= prepare(c, ts, k ? (path ^ k ^ 0x6b6579ull) * 1099511628211ull : (path ^ (i + 1)) * 1099511628211ull);
    }
    if (n->mask) {
        n->mask->parent = n;
        flags |= prepare(n->mask, ts, (path ^ 0x6d61736bull) * 1099511628211ull);   /* (a mask inherits as a child would) */
    }
    if (style_changed || (flags & (GP_SUB_LIST | PREP_STATE_CHANGED))) n->ncache = 0;   /* (a state put on or taken off below: the sizes measured differ) */
    n->sub_flags = (uint8_t)flags;
    n->prep_valid = cached_root;
    n->lk_valid = was_laid_out && !(flags & PREP_STATE_CHANGED);   /* any other subtree was walked again: whatever it was laid out for may differ */
    return flags;
}

/* ---------------------------------------------------------------- flex */

typedef struct {
    gp_el *c;
    float base, target, min, max, grow, shrink;
    float m_main_s, m_main_e, m_cross_s, m_cross_e;
    float cross;
    uint8_t align;
    bool frozen, stretch;
    bool auto_ms, auto_me, auto_cs, auto_ce;   /* auto margins: main start / end, cross start / end */
    bool auto_min_w;   /* a row item whose automatic minimum size is its min-content width, found only if the line has to shrink */
    float spec_w;      /* ... capped by its specified width, when it has one */
} item;

static gp_size layout_node(gp_el *n, float fw, float fh, float pw, float ph, bool commit);

static float margin_total_main(const item *it) { return it->m_main_s + it->m_main_e; }

static void layout_absolute_children(gp_el *n, float out_w, float out_h) {
    const gp_style *s = &n->style;
    float bw = s->border_w;
    float cw_pad = out_w - 2 * bw, ch_pad = out_h - 2 * bw; /* padding box */
    for (uint32_t i = 0; i < n->nchildren; i++) {
        gp_el *c = n->children[i];
        if (!c->style.absolute) continue;
        const gp_style *cs = &c->style;
        float l = resolve(cs->inset[0], cw_pad), t = resolve(cs->inset[1], ch_pad);
        float r = resolve(cs->inset[2], cw_pad), b = resolve(cs->inset[3], ch_pad);
        float mh = cs->margin[0] + cs->margin[2], mv = cs->margin[1] + cs->margin[3];

        float w = resolve(cs->w, cw_pad), h = resolve(cs->h, ch_pad);
        if (!def(w) && def(l) && def(r)) w = cw_pad - l - r - mh;
        if (!def(h) && def(t) && def(b)) h = ch_pad - t - b - mv;
        gp_size sz = layout_node(c, w, h, cw_pad - mh, ch_pad - mv, false);

        float x = def(l) ? l : def(r) ? cw_pad - r - sz.w - cs->margin[2] - cs->margin[0] : 0;
        float y = def(t) ? t : def(b) ? ch_pad - b - sz.h - cs->margin[3] - cs->margin[1] : 0;
        c->pos = (gp_point){ bw + x + cs->margin[0], bw + y + cs->margin[1] };
        c->size = sz;
        layout_node(c, sz.w, sz.h, cw_pad - mh, ch_pad - mv, true);
    }
}

/* Scroll extents of a container whose children have been placed: the in-flow content in padding-box coordinates. */
static void finish_scroll(gp_el *n, float out_w, float out_h) {
    const gp_style *s = &n->style;
    const float bw = s->border_w;
    n->scroll = n->scroll_max = (gp_point){ 0, 0 };
    if (!(s->overflow_x_scroll || s->overflow_y_scroll)) return;
    float vw = out_w - 2 * bw, vh = out_h - 2 * bw;
    float ext_w = vw, ext_h = vh;
    for (uint32_t i = 0; i < n->nchildren; i++) {
        const gp_el *c = n->children[i];
        if (c->style.absolute) continue;
        ext_w = maxf(ext_w, c->pos.x + c->size.w + c->style.margin[2] - bw + s->pad[2]);
        ext_h = maxf(ext_h, c->pos.y + c->size.h + c->style.margin[3] - bw + s->pad[3]);
    }
    gp_point max = { s->overflow_x_scroll ? ext_w - vw : 0, s->overflow_y_scroll ? ext_h - vh : 0 };
    if (g_scrolls) {
        gp_scroll_state *st = gp_scroll_state_get(g_scrolls, n->scroll_key, true);
        st->max_x = max.x;
        st->max_y = max.y;
        st->view_w = vw;
        st->view_h = vh;
        st->x = fminf(fmaxf(st->x, 0), max.x);
        st->y = fminf(fmaxf(st->y, 0), max.y);
        n->scroll = (gp_point){ st->x, st->y };
    }
    n->scroll_max = max;
}

static gp_size layout_container(gp_el *n, float w, float h, float pw, float ph, bool commit);

/* gp_flex_wrap: the children are cut into lines by their base sizes (greedily, as in CSS), and every line is laid out by
 * layout_container itself as a container of its own that holds just those children: a copy of this element without padding, border,
 * wrapping, scrolling or limits, given the inner main size (so grow, shrink, justify and the auto margins work per line) and, to
 * commit, the line's height (so stretch fills the line). The lines are stacked across with the gap between them. */
static gp_size layout_wrapped(gp_el *n, float w, float h, float pw, float ph, bool commit) {
    const gp_style *s = &n->style;
    const bool row = s->dir == GP_DIR_ROW;
    const float bw = s->border_w;
    const float chrome_w = s->pad[0] + s->pad[2] + 2 * bw, chrome_h = s->pad[1] + s->pad[3] + 2 * bw;
    float aw = def(w) ? maxf(w - chrome_w, 0) : (def(pw) ? maxf(pw - chrome_w, 0) : NAN);
    float ah = def(h) ? maxf(h - chrome_h, 0) : (def(ph) ? maxf(ph - chrome_h, 0) : NAN);
    float main_avail = row ? aw : ah;
    float main_def = def(row ? w : h) ? main_avail : NAN;

    uint32_t count = 0;
    for (uint32_t i = 0; i < n->nchildren; i++)
        if (!n->children[i]->style.absolute) count++;
    gp_el **kids = malloc((count ? count : 1) * sizeof *kids);
    float *size = malloc((count ? count : 1) * sizeof *size);   /* base size with margins */
    uint32_t *start = malloc((count + 1) * sizeof *start);   /* first child of each line */
    uint32_t k = 0, nlines = 0;
    float line = 0;
    for (uint32_t i = 0; i < n->nchildren; i++) {
        gp_el *c = n->children[i];
        if (c->style.absolute) continue;
        const gp_style *cs = &c->style;
        float cw_avail = def(aw) ? maxf(aw - cs->margin[0] - cs->margin[2], 0) : NAN;
        float ch_avail = def(ah) ? maxf(ah - cs->margin[1] - cs->margin[3], 0) : NAN;
        float base;
        if (cs->basis.kind != GP_AUTO) {
            float cc = row ? cs->pad[0] + cs->pad[2] + 2 * cs->border_w : cs->pad[1] + cs->pad[3] + 2 * cs->border_w;
            base = maxf(resolve(cs->basis, main_avail), cc);
        } else {
            gp_size sz = layout_node(c, NAN, NAN, cw_avail, ch_avail, false);
            base = row ? sz.w : sz.h;
        }
        gp_length minl = row ? cs->min_w : cs->min_h, maxl = row ? cs->max_w : cs->max_h;
        if (def(resolve(maxl, main_avail))) base = fminf(base, resolve(maxl, main_avail));
        if (def(resolve(minl, main_avail))) base = fmaxf(base, resolve(minl, main_avail));
        float total = base + (row ? cs->margin[0] + cs->margin[2] : cs->margin[1] + cs->margin[3]);
        size[k] = total;
        float room = def(main_def) ? main_def : main_avail;
        if (nlines == 0 || line + s->gap + total > room) {   /* a line always has an item, so one too wide gets a line of its own */
            start[nlines++] = k;
            line = total;
        } else {
            line += s->gap + total;
        }
        kids[k++] = c;
    }
    start[nlines] = k;
    /* the widest line is what an unsized container is as wide as */
    float widest = 0;
    for (uint32_t l = 0; l < nlines; l++) {
        float sum = 0;
        for (uint32_t j = start[l]; j < start[l + 1]; j++) sum += size[j] + (j > start[l] ? s->gap : 0);
        widest = maxf(widest, sum);
    }
    float inner_main = def(main_def) ? main_def : widest;
    if (!def(main_def) && def(main_avail)) inner_main = fminf(widest, main_avail);

    gp_el tmp = *n;
    tmp.style.wrap = false;
    tmp.style.overflow_x_scroll = tmp.style.overflow_y_scroll = false;
    tmp.style.pad[0] = tmp.style.pad[1] = tmp.style.pad[2] = tmp.style.pad[3] = 0;
    tmp.style.border_w = 0;
    tmp.style.min_w = tmp.style.max_w = tmp.style.min_h = tmp.style.max_h = (gp_length){ GP_AUTO, 0 };
    float lpw = def(pw) ? maxf(pw - chrome_w, 0) : NAN, lph = def(ph) ? maxf(ph - chrome_h, 0) : NAN;

    float *cross = malloc((nlines ? nlines : 1) * sizeof *cross);
    float total_cross = 0;
    for (uint32_t l = 0; l < nlines; l++) {
        tmp.children = kids + start[l];
        tmp.nchildren = start[l + 1] - start[l];
        gp_size sz = layout_container(&tmp, row ? inner_main : NAN, row ? NAN : inner_main, lpw, lph, false);
        cross[l] = row ? sz.h : sz.w;
        total_cross += cross[l] + (l ? s->gap : 0);
    }
    float out_w = def(w) ? w : (row ? inner_main : total_cross) + chrome_w;
    float out_h = def(h) ? h : (row ? total_cross : inner_main) + chrome_h;
    out_w = clamp_len(out_w, s->min_w, s->max_w, pw);
    out_h = clamp_len(out_h, s->min_h, s->max_h, ph);

    /* align-content: the room across that the lines leave. Stretch gives every line its share (which the items that stretch then fill);
     * the others move the lines. (Without room, or in a container that sizes itself to its lines, there is nothing to do.) */
    float room = (row ? out_h - chrome_h : out_w - chrome_w) - total_cross, lead = 0, extra = 0;
    if (room > 0 && nlines) {
        switch (s->align_content) {
        case 0: for (uint32_t l = 0; l < nlines; l++) cross[l] += room / (float)nlines; break;
        case 1 + GP_JUSTIFY_CENTER: lead = room / 2; break;
        case 1 + GP_JUSTIFY_END: lead = room; break;
        case 1 + GP_JUSTIFY_BETWEEN: if (nlines > 1) extra = room / (float)(nlines - 1); break;
        case 1 + GP_JUSTIFY_AROUND: extra = room / (float)nlines; lead = extra / 2; break;
        case 1 + GP_JUSTIFY_EVENLY: extra = room / (float)(nlines + 1); lead = extra; break;
        default: break;
        }
    }

    if (commit) {
        float inner = (row ? out_w - chrome_w : out_h - chrome_h), inner_cross = row ? out_h - chrome_h : out_w - chrome_w;
        const bool rtl = n->ts.dir == GP_TEXT_DIRECTION_RTL;
        float off = lead;
        float ox = s->pad[0] + bw, oy = s->pad[1] + bw;
        for (uint32_t l = 0; l < nlines; l++) {
            tmp.children = kids + start[l];
            tmp.nchildren = start[l + 1] - start[l];
            layout_container(&tmp, row ? inner : cross[l], row ? cross[l] : inner, lpw, lph, true);
            for (uint32_t j = 0; j < tmp.nchildren; j++) {
                gp_el *c = tmp.children[j];
                c->pos.x += ox + (row ? 0 : rtl ? inner_cross - off - cross[l] : off);   /* (right-to-left columns stack from the right) */
                c->pos.y += oy + (row ? off : 0);
            }
            off += cross[l] + s->gap + extra;
        }
        layout_absolute_children(n, out_w, out_h);
        finish_scroll(n, out_w, out_h);
    }
    free(cross);
    free(start);
    free(size);
    free(kids);
    return (gp_size){ out_w, out_h };
}

static gp_size layout_image(gp_el *n, float w, float h, float pw, float ph);

/* The narrowest an element can be without breaking a word or squeezing what cannot be (CSS min-content), border box. Text is
 * measured wrapped at every opportunity (the widest word); a row adds up its items, a column or a wrapping row takes the widest. An
 * element with a width of its own is that wide, unless `ignore_w` (its automatic minimum is the smaller of this and the width); one that is a
 * list or scrolls has no content width to speak of. Absolute children are not part of it. */
static float min_content_w(gp_el *n, bool ignore_w) {
    const gp_style *s = &n->style;
    float chrome = s->pad[0] + s->pad[2] + 2 * s->border_w;
    float w;
    if (!ignore_w && s->w.kind == GP_PX) {
        w = s->w.v;
    } else if (n->kind == GP_EL_TEXT) {
        gp_size t = gp_text_measure_dir(g_ts, GP_TEXT_SEL(n->ts), n->ts.size, n->text, 0, s->whitespace_pre, n->ts.dir, NULL, NULL, NULL);
        w = ceilf(t.w) + chrome;
    } else if (n->kind == GP_EL_IMAGE || n->kind == GP_EL_SVG) {
        w = layout_image(n, NAN, NAN, NAN, NAN).w;
    } else if (n->kind == GP_EL_LIST || s->overflow_x_scroll || s->overflow_y_scroll) {
        w = chrome;
    } else {
        const bool row = s->dir == GP_DIR_ROW && !s->wrap;
        float acc = 0;
        int count = 0;
        for (uint32_t i = 0; i < n->nchildren; i++) {
            gp_el *c = n->children[i];
            if (c->style.absolute) continue;
            float cw = min_content_w(c, false) + c->style.margin[0] + c->style.margin[2];
            acc = row ? acc + cw : maxf(acc, cw);
            count++;
        }
        if (row && count > 1) acc += s->gap * (float)(count - 1);
        w = acc + chrome;
    }
    return clamp_len(w, s->min_w, s->max_w, NAN);
}

static gp_size layout_container(gp_el *n, float w, float h, float pw, float ph, bool commit) {
    const gp_style *s = &n->style;
    const bool row = s->dir == GP_DIR_ROW;
    const float bw = s->border_w;
    const float chrome_w = s->pad[0] + s->pad[2] + 2 * bw;
    const float chrome_h = s->pad[1] + s->pad[3] + 2 * bw;

    /* Inner space: `d*` is definite (own size known), `a*` is merely available. */
    float aw = def(w) ? maxf(w - chrome_w, 0) : (def(pw) ? maxf(pw - chrome_w, 0) : NAN);
    float ah = def(h) ? maxf(h - chrome_h, 0) : (def(ph) ? maxf(ph - chrome_h, 0) : NAN);
    float dw = def(w) ? aw : NAN, dh = def(h) ? ah : NAN;

    /* A scroller on the main axis lays its children out in unbounded space and never
     * shrinks them; they overflow instead. */
    const bool scroll_main = row ? s->overflow_x_scroll : s->overflow_y_scroll;
    /* right-to-left (gp_text_dir_rtl, inherited): the horizontal axis runs backwards, so the row's main axis and a column's cross axis. It is laid out
     * as usual with the left and right margins exchanged, and mirrored about the content box at the end. (A scroller along a row is left alone.) */
    const bool rtl = n->ts.dir == GP_TEXT_DIRECTION_RTL && !(row && scroll_main);
    float main_avail = scroll_main ? NAN : (row ? aw : ah), main_def = row ? dw : dh;
    float cross_def = row ? dh : dw;
    if (s->wrap && def(main_avail)) return layout_wrapped(n, w, h, pw, ph, commit);   /* (a scroller along the main axis has none) */

    /* in-flow items */
    item stack_items[24];
    item *items = stack_items;
    uint32_t count = 0;
    for (uint32_t i = 0; i < n->nchildren; i++)
        if (!n->children[i]->style.absolute) count++;
    if (count > 24) items = malloc(count * sizeof *items);

    uint32_t k = 0;
    float sum_base = 0, sum_margin = 0;
    for (uint32_t i = 0; i < n->nchildren; i++) {
        gp_el *c = n->children[i];
        if (c->style.absolute) continue;
        const gp_style *cs = &c->style;
        item *it = &items[k++];
        memset(it, 0, sizeof *it);
        it->c = c;
        float ml = cs->margin[0], mt = cs->margin[1], mr = cs->margin[2], mb = cs->margin[3];
        const int left = 0, top = 1, right = 2, bottom = 3;   /* (the bits of margin_auto too) */
        int ms = row ? (rtl ? right : left) : top, me = row ? (rtl ? left : right) : bottom;
        int cstart = row ? top : (rtl ? right : left), cend = row ? bottom : (rtl ? left : right);
        it->m_main_s = cs->margin[ms];
        it->m_main_e = cs->margin[me];
        it->m_cross_s = cs->margin[cstart];
        it->m_cross_e = cs->margin[cend];
        it->auto_ms = (cs->margin_auto >> ms & 1) != 0;
        it->auto_me = (cs->margin_auto >> me & 1) != 0;
        it->auto_cs = (cs->margin_auto >> cstart & 1) != 0;
        it->auto_ce = (cs->margin_auto >> cend & 1) != 0;
        float cw_avail = def(aw) ? maxf(aw - ml - mr, 0) : NAN;
        float ch_avail = def(ah) ? maxf(ah - mt - mb, 0) : NAN;
        if (scroll_main) { if (row) cw_avail = NAN; else ch_avail = NAN; }

        if (cs->basis.kind != GP_AUTO) {
            float cc = row ? cs->pad[0] + cs->pad[2] + 2 * cs->border_w
                           : cs->pad[1] + cs->pad[3] + 2 * cs->border_w;
            it->base = maxf(resolve(cs->basis, main_avail), cc);
        } else {
            /* A picture that will be stretched across a container of definite size has its main size from the proportions at that
             * stretched size, as in CSS (a column of full-width images is as tall as the images need, not as their pixels). */
            float sw = NAN, sh = NAN;
            uint8_t al0 = cs->align_self ? cs->align_self : (s->align_items ? s->align_items : GP_ALIGN_STRETCH);
            if ((c->kind == GP_EL_IMAGE || c->kind == GP_EL_SVG) && al0 == GP_ALIGN_STRETCH && def(cross_def)) {
                if (row && cs->h.kind == GP_AUTO) sh = maxf(cross_def - mt - mb, 0);
                else if (!row && cs->w.kind == GP_AUTO) sw = maxf(cross_def - ml - mr, 0);
            }
            gp_size sz = layout_node(c, sw, sh, cw_avail, ch_avail, false);
            it->base = row ? sz.w : sz.h;
        }
        gp_length minl = row ? cs->min_w : cs->min_h, maxl = row ? cs->max_w : cs->max_h;
        it->min = def(resolve(minl, main_avail)) ? resolve(minl, main_avail) : 0;
        it->max = def(resolve(maxl, main_avail)) ? resolve(maxl, main_avail) : INFINITY;
        /* Automatic minimum size (CSS min-height: auto), column axis only: an item of auto
         * height does not shrink below its content, unless it clips or scrolls it. Without
         * this a full page squeezes its cards and their content spills out of them. (The
         * row axis would need min-content widths, i.e. the longest unbreakable run.) */
        if (!row && cs->h.kind == GP_AUTO && minl.kind == GP_AUTO && !cs->overflow_hidden &&
            !cs->overflow_x_scroll && !cs->overflow_y_scroll) {
            float natural = it->base;
            if (cs->basis.kind != GP_AUTO)
                natural = layout_node(c, NAN, NAN, cw_avail, ch_avail, false).h;
            it->min = fminf(fmaxf(it->min, natural), it->max);
        }
        /* ... and the row axis (CSS min-width: auto): not below the longest word of its text, or what a nest of items needs at least. Worked out
         * below, and only if the line has to shrink. A width of its own caps it. */
        if (row && !scroll_main && minl.kind == GP_AUTO && !cs->overflow_hidden && !cs->overflow_x_scroll &&
            !cs->overflow_y_scroll && c->kind != GP_EL_LIST) {
            it->auto_min_w = true;
            it->spec_w = resolve(cs->w, main_avail);
            if (cs->basis.kind != GP_AUTO) {   /* a basis below the min-content width is raised to it, shrinking or not (the hypothetical size is clamped by the minimum) */
                float mc = min_content_w(c, true);
                if (def(it->spec_w)) mc = fminf(mc, it->spec_w);
                it->min = fminf(fmaxf(it->min, mc), it->max);
                it->base = fminf(fmaxf(it->base, it->min), it->max);
                it->auto_min_w = false;   /* (done) */
            }
        }
        it->grow = cs->grow;
        it->shrink = cs->shrink;
        it->target = it->base;
        sum_base += it->base;
        sum_margin += margin_total_main(it);

        uint8_t al = cs->align_self ? cs->align_self
                                    : (s->align_items ? s->align_items : GP_ALIGN_STRETCH);
        it->align = al;
        gp_length cross_len = row ? cs->h : cs->w;
        it->stretch = al == GP_ALIGN_STRETCH && cross_len.kind == GP_AUTO && !it->auto_cs && !it->auto_ce;
    }

    float gaps = count > 1 ? s->gap * (float)(count - 1) : 0;
    float used = sum_base + sum_margin + gaps;
    float target_main = def(main_def) ? main_def
                                      : (def(main_avail) ? fminf(used, main_avail) : used);
    if (scroll_main && def(main_def)) target_main = maxf(main_def, used);

    /* Resolve flexible lengths (CSS flexbox §9.7, simplified). */
    float free0 = target_main - used;
    bool growing = free0 > 0;
    if (free0 < 0) {
        for (uint32_t i = 0; i < count; i++) {
            item *it = &items[i];
            if (!it->auto_min_w || it->shrink == 0) continue;
            float mc = min_content_w(it->c, true);
            if (def(it->spec_w)) mc = fminf(mc, it->spec_w);
            it->min = fminf(fmaxf(it->min, mc), it->max);
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        float factor = growing ? items[i].grow : items[i].shrink;
        items[i].frozen = free0 == 0 || factor == 0;
        if (items[i].frozen) items[i].target = fminf(fmaxf(items[i].base, items[i].min), items[i].max);   /* min and max hold for an inflexible item too */
    }
    for (uint32_t pass = 0; pass <= count; pass++) {
        float taken = gaps + sum_margin, factor_sum = 0;
        bool any = false;
        for (uint32_t i = 0; i < count; i++) {
            taken += items[i].frozen ? items[i].target : items[i].base;
            if (!items[i].frozen) {
                any = true;
                factor_sum += growing ? items[i].grow : items[i].shrink * items[i].base;
            }
        }
        if (!any || factor_sum <= 0) break;
        float remaining = target_main - taken;
        if (growing && factor_sum < 1) remaining *= factor_sum;

        float violation = 0;
        for (uint32_t i = 0; i < count; i++) {
            item *it = &items[i];
            if (it->frozen) continue;
            float share = (growing ? it->grow : it->shrink * it->base) / factor_sum;
            float t = it->base + remaining * share;
            float clamped = fminf(fmaxf(t, it->min), it->max);
            violation += clamped - t;
            it->target = t;
        }
        for (uint32_t i = 0; i < count; i++) {
            item *it = &items[i];
            if (it->frozen) continue;
            float clamped = fminf(fmaxf(it->target, it->min), it->max);
            if (violation == 0 || (violation > 0 && clamped > it->target) ||
                (violation < 0 && clamped < it->target)) {
                it->target = clamped;
                it->frozen = true;
            }
        }
        if (violation == 0) break;
    }

    /* Cross sizes. */
    float line_cross = 0;
    for (uint32_t i = 0; i < count; i++) {
        item *it = &items[i];
        gp_el *c = it->c;
        float ml = c->style.margin[0], mt = c->style.margin[1];
        float mr = c->style.margin[2], mb = c->style.margin[3];
        float cw_avail = def(aw) ? maxf(aw - ml - mr, 0) : NAN;
        float ch_avail = def(ah) ? maxf(ah - mt - mb, 0) : NAN;
        if (scroll_main) { if (row) cw_avail = NAN; else ch_avail = NAN; }
        float m_cross = it->m_cross_s + it->m_cross_e;
        if (it->stretch && def(cross_def)) {
            it->cross = maxf(cross_def - m_cross, 0);
        } else {
            gp_size sz = layout_node(c, row ? it->target : NAN, row ? NAN : it->target,
                                     cw_avail, ch_avail, false);
            it->cross = row ? sz.h : sz.w;
        }
        line_cross = maxf(line_cross, it->cross + m_cross);
    }
    if (def(cross_def)) line_cross = cross_def;
    for (uint32_t i = 0; i < count; i++)
        if (items[i].stretch && !def(cross_def))
            items[i].cross = maxf(line_cross - items[i].m_cross_s - items[i].m_cross_e, 0);

    float out_w = def(w) ? w : (row ? target_main : line_cross) + chrome_w;
    float out_h = def(h) ? h : (row ? line_cross : target_main) + chrome_h;
    out_w = clamp_len(out_w, s->min_w, s->max_w, pw);
    out_h = clamp_len(out_h, s->min_h, s->max_h, ph);

    if (commit) {
        float inner_main = row ? out_w - chrome_w : out_h - chrome_h;
        float inner_cross = row ? out_h - chrome_h : out_w - chrome_w;
        float sum_t = gaps;
        for (uint32_t i = 0; i < count; i++) sum_t += items[i].target + margin_total_main(&items[i]);
        float free_space = inner_main - sum_t;
        if (scroll_main && free_space < 0) free_space = 0; /* overflow starts at the origin */

        /* auto margins take the free space before justify-content gets any */
        int nauto = 0;
        for (uint32_t i = 0; i < count; i++) nauto += items[i].auto_ms + items[i].auto_me;
        float auto_share = 0;
        if (nauto && free_space > 0) {
            auto_share = free_space / (float)nauto;
            free_space = 0;
        }
        float lead = 0, between = 0;
        switch (s->justify) {
        case GP_JUSTIFY_CENTER: lead = free_space / 2; break;
        case GP_JUSTIFY_END: lead = free_space; break;
        case GP_JUSTIFY_BETWEEN:
            if (count > 1 && free_space > 0) between = free_space / (float)(count - 1);
            break;
        case GP_JUSTIFY_AROUND:
            if (count && free_space > 0) { between = free_space / (float)count; lead = between / 2; }
            break;
        case GP_JUSTIFY_EVENLY:
            if (count && free_space > 0) { between = free_space / (float)(count + 1); lead = between; }
            break;
        default: break;
        }

        float main_origin = (row ? s->pad[0] : s->pad[1]) + bw;
        float cross_origin = (row ? s->pad[1] : s->pad[0]) + bw;
        float pos = main_origin + lead;
        for (uint32_t i = 0; i < count; i++) {
            item *it = &items[i];
            gp_el *c = it->c;
            float cross_free = inner_cross - it->cross - it->m_cross_s - it->m_cross_e;
            float coff = 0;
            if (it->auto_cs || it->auto_ce) coff = cross_free > 0 ? (it->auto_cs && it->auto_ce ? cross_free / 2 : it->auto_cs ? cross_free : 0) : 0;
            else if (it->align == GP_ALIGN_CENTER) coff = cross_free / 2;
            else if (it->align == GP_ALIGN_END) coff = cross_free;

            float mpos = pos + it->m_main_s + (it->auto_ms ? auto_share : 0);
            float cpos = cross_origin + it->m_cross_s + coff;
            c->size = row ? (gp_size){ it->target, it->cross } : (gp_size){ it->cross, it->target };
            c->pos = row ? (gp_point){ mpos, cpos } : (gp_point){ cpos, mpos };
            float ml = c->style.margin[0], mt = c->style.margin[1];
            float mr = c->style.margin[2], mb = c->style.margin[3];
            float cw_avail = def(aw) ? maxf(aw - ml - mr, 0) : NAN;
            float ch_avail = def(ah) ? maxf(ah - mt - mb, 0) : NAN;
            if (scroll_main) { if (row) cw_avail = NAN; else ch_avail = NAN; }
            layout_node(c, c->size.w, c->size.h, cw_avail, ch_avail, true);
            pos = mpos + it->target + it->m_main_e + (it->auto_me ? auto_share : 0) + s->gap + between;
        }
        if (rtl) {
            float lo = bw + s->pad[0], hi = out_w - bw - s->pad[2];
            for (uint32_t i = 0; i < count; i++) items[i].c->pos.x = lo + hi - items[i].c->pos.x - items[i].c->size.w;
        }
        layout_absolute_children(n, out_w, out_h);

        finish_scroll(n, out_w, out_h);
    }

    if (items != stack_items) free(items);
    return (gp_size){ out_w, out_h };
}

/* ---------------------------------------------------------------- uniform list */

/* Rows are not built with the tree: layout decides which are visible and calls the
 * item function for just those. They live in a per-list arena (kept in the window's
 * scroll table, keyed like the scroll offset) that is reset whenever they are rebuilt,
 * so scrolling for hours does not grow memory. */

#define LIST_MAX_REALIZED 4096

static gp_arena *list_arena(gp_el *n) {
    gp_scroll_state *st = gp_scroll_state_get(g_scrolls, n->scroll_key, true);
    if (!st->arena) {
        st->arena = calloc(1, sizeof *st->arena);
        st->arena->chunk_size = 64 * 1024;
    }
    return st->arena;
}

/* Row identity is its index, not its slot, so nested scrollers keep their state. */
static uint64_t row_path(const gp_el *list, size_t index) {
    return (list->path ^ (index + 1)) * 1099511628211ull;
}

/* Builds row 0 into the list arena and measures it (width/height may be NAN). `cross` is the width the rows are given, or for a horizontal
 * list the height. */
static gp_size list_probe(gp_el *n, float cross) {
    gp_arena *a = list_arena(n);
    gp_arena_reset(a);
    gp_el **rows = gp_list_build(n, 0, 1, a);
    rows[0]->parent = n;
    prepare(rows[0], n->ts, row_path(n, 0));
    if (n->list_horiz) return layout_node(rows[0], NAN, cross, NAN, cross, false);
    return layout_node(rows[0], cross, NAN, cross, NAN, false);
}

/* The extent of an item along the list's axis: asked for, else row 0's own. */
static void list_resolve_height(gp_el *n, float cross) {
    if (n->list_ih > 0) return;
    float ih = n->list_item_h;
    if (ih <= 0 && n->list_count > 0) {
        gp_size sz = list_probe(n, cross);
        ih = n->list_horiz ? sz.w : sz.h;
    }
    n->list_ih = ih > 0 ? ih : 1;
}

/* Builds and lays out the rows that intersect the viewport at n->scroll. Unless `force`,
 * does nothing when the visible range has not changed. n->size must be final. */
static void list_realize(gp_el *n, bool force) {
    const gp_style *s = &n->style;
    const bool hz = n->list_horiz;
    float bw = s->border_w, ih = n->list_ih;
    float inner_w = fmaxf(n->size.w - (s->pad[0] + s->pad[2] + 2 * bw), 0);
    float inner_h = fmaxf(n->size.h - (s->pad[1] + s->pad[3] + 2 * bw), 0);
    float view_h = hz ? n->size.w - 2 * bw : n->size.h - 2 * bw;   /* (the view along the list's axis) */
    float scroll = hz ? n->scroll.x : n->scroll.y, pad_before = hz ? s->pad[0] : s->pad[1];

    size_t first = 0, last = 0;
    if (n->list_count) {
        float top = scroll - pad_before, bottom = scroll + view_h - pad_before;
        first = top > 0 ? (size_t)(top / ih) : 0;
        last = bottom > 0 ? (size_t)ceilf(bottom / ih) : 0;
        if (last > n->list_count) last = n->list_count;
        if (first > last) first = last;
        if (last - first > LIST_MAX_REALIZED) {
            static bool warned;
            if (!warned) {
                warned = true;
                fprintf(stderr, "gpuic: gp_uniform_list has no bounded height; showing %d rows "
                                "(give it gp_h / gp_flex_1 / gp_max_h)\n", LIST_MAX_REALIZED);
            }
            last = first + LIST_MAX_REALIZED;
        }
    }
    size_t count = last - first;
    if (!force && n->list_realized && first == n->list_first && count == n->list_n) return;

    gp_arena *a = list_arena(n);
    gp_arena_reset(a);
    n->children = gp_list_build(n, first, count, a);
    n->nchildren = (uint32_t)count;
    n->list_first = first;
    n->list_n = count;
    n->list_realized = true;
    for (size_t i = 0; i < count; i++) {
        gp_el *c = n->children[i];
        size_t idx = first + i;
        if (g_anims && c->anim_below) gp_anim_eval(c, g_anims);
        c->parent = n;
        prepare(c, n->ts, row_path(n, idx));
        if (hz) {
            c->pos = (gp_point){ bw + s->pad[0] + (float)idx * ih, bw + s->pad[1] };
            c->size = (gp_size){ ih, inner_h };
            layout_node(c, ih, inner_h, ih, inner_h, true);
        } else {
            c->pos = (gp_point){ bw + s->pad[0], bw + s->pad[1] + (float)idx * ih };
            c->size = (gp_size){ inner_w, ih };
            layout_node(c, inner_w, ih, inner_w, ih, true);
        }
    }
}

static void list_sticky_update(gp_el *n);

/* A uniform list along x (gp_list_horizontal): the same, with the axes exchanged. Fills the available width; as tall as the first item unless told. */
static gp_size layout_hlist(gp_el *n, float w, float h, float pw, float ph, bool commit) {
    const gp_style *s = &n->style;
    float bw = s->border_w;
    float chrome_h = s->pad[1] + s->pad[3] + 2 * bw;
    float out_h = h;
    if (!def(out_h)) out_h = (n->list_count ? list_probe(n, NAN).h : 0) + chrome_h;   /* (as tall as the first item: a strip does not fill the height it is offered) */
    float inner_h = fmaxf(out_h - chrome_h, 0);
    list_resolve_height(n, inner_h);
    float content_w = (float)n->list_count * n->list_ih + s->pad[0] + s->pad[2];
    float out_w = def(w) ? w : (def(pw) ? pw : content_w + 2 * bw);
    out_w = clamp_len(out_w, s->min_w, s->max_w, pw);
    out_h = clamp_len(out_h, s->min_h, s->max_h, ph);
    if (commit) {
        n->size = (gp_size){ out_w, out_h };
        float view_w = fmaxf(out_w - 2 * bw, 0);
        float max_x = fmaxf(0, content_w - view_w);
        n->scroll = (gp_point){ 0, 0 };
        n->scroll_max = (gp_point){ max_x, 0 };
        if (g_scrolls) {
            gp_scroll_state *st = gp_scroll_state_get(g_scrolls, n->scroll_key, true);
            st->max_x = max_x;
            st->max_y = 0;
            st->y = 0;
            st->x = fminf(fmaxf(st->x, 0), max_x);
            st->item_h = n->list_ih;
            st->pad_top = s->pad[0];
            st->horiz = true;
            st->view_w = view_w;
            st->view_h = fmaxf(out_h - 2 * bw, 0);
            n->scroll.x = st->x;
        }
        list_realize(n, true);
    }
    return (gp_size){ out_w, out_h };
}

static gp_size layout_list(gp_el *n, float w, float h, float pw, float ph, bool commit) {
    if (n->list_horiz) return layout_hlist(n, w, h, pw, ph, commit);
    const gp_style *s = &n->style;
    float bw = s->border_w;
    float chrome_w = s->pad[0] + s->pad[2] + 2 * bw;

    /* fills the available width unless told otherwise; with none, fits row 0 */
    float out_w = w;
    if (!def(out_w)) {
        if (def(pw)) out_w = pw;
        else out_w = (n->list_count ? list_probe(n, NAN).w : 0) + chrome_w;
    }
    list_resolve_height(n, fmaxf(out_w - chrome_w, 0));

    float content_h = (float)n->list_count * n->list_ih + s->pad[1] + s->pad[3];
    float out_h = def(h) ? h : content_h + 2 * bw;
    out_w = clamp_len(out_w, s->min_w, s->max_w, pw);
    out_h = clamp_len(out_h, s->min_h, s->max_h, ph);

    if (commit) {
        n->size = (gp_size){ out_w, out_h };
        float view_h = out_h - 2 * bw;
        float max_y = fmaxf(0, content_h - view_h);
        n->scroll = (gp_point){ 0, 0 };
        n->scroll_max = (gp_point){ 0, max_y };
        if (g_scrolls) {
            gp_scroll_state *st = gp_scroll_state_get(g_scrolls, n->scroll_key, true);
            st->max_x = 0;
            st->max_y = max_y;
            st->x = 0;
            st->y = fminf(fmaxf(st->y, 0), max_y);
            st->item_h = n->list_ih;
            st->pad_top = s->pad[1];
            st->view_h = view_h;
            st->view_w = fmaxf(n->size.w - 2 * bw, 0);
            n->scroll.y = st->y;
        }
        list_realize(n, true);
        list_sticky_update(n);
    }
    return (gp_size){ out_w, out_h };
}

static void vlist_scrolled(gp_el *n);

/* gp_list_sticky: the header of the group of the first row in view, pinned at the top of the view, pushed up by the next group's first row.
 * Done after every realize and every scroll, from where the realized rows are (their `pos` and the list's scroll offset share a space,
 * the one the base of a gp_list keeps small). */
static gp_arena *list_harena(gp_el *n) {
    gp_scroll_state *st = gp_scroll_state_get(g_scrolls, n->scroll_key, true);
    if (!st->harena) {
        st->harena = calloc(1, sizeof *st->harena);
        st->harena->chunk_size = 8 * 1024;
    }
    return st->harena;
}

static void list_sticky_update(gp_el *n) {
    n->sticky = NULL;
    if (n->list_horiz || !n->sticky_fn || !n->list_realized || !n->nchildren) return;
    const gp_style *s = &n->style;
    float bw = s->border_w, inner_w = fmaxf(n->size.w - (s->pad[0] + s->pad[2] + 2 * bw), 0);
    uint32_t top = 0;   /* the first realized row with any of it below the top of the view */
    while (top < n->nchildren && n->children[top]->pos.y - n->scroll.y - bw + n->children[top]->size.h <= 0.001f) top++;
    if (top == n->nchildren) return;
    size_t row = n->list_first + top;
    gp_arena *ha = list_harena(n);
    gp_arena_reset(ha);
    gp_list_group g;
    gp_el *hdr;
    if (!gp_list_group_build(n, row, ha, &g, &hdr)) return;
    float row_top = n->children[top]->pos.y - n->scroll.y - bw;   /* where that row starts, from the top of the view */
    if (!(row > g.first || (row == g.first && row_top < -0.5f))) return;   /* its group's own header row is in view: nothing to pin yet */
    prepare(hdr, n->ts, (n->path ^ 0x57ac4ull) * 1099511628211ull);
    float h = layout_node(hdr, inner_w, NAN, inner_w, NAN, false).h;
    if (!(h >= 0)) h = 0;
    layout_node(hdr, inner_w, h, inner_w, h, true);
    float shift = 0;
    if (g.next >= n->list_first && g.next < n->list_first + n->list_n) {   /* the next group's first row is on the way up */
        float yn = n->children[g.next - n->list_first]->pos.y - n->scroll.y - bw;
        if (yn < h) shift = yn - h;
    }
    hdr->pos = (gp_point){ bw + s->pad[0], bw + shift };
    hdr->size = (gp_size){ inner_w, h };
    n->sticky = hdr;
}

void gp_layout_list_scrolled(gp_el *list, gp_text_system *ts, gp_scroll_table *scrolls, gp_anim_table *anims) {
    g_ts = ts;
    g_scrolls = scrolls;
    g_anims = anims;
    if (anims) anims->now = gp_now_ns();   /* rows animated from here are not part of a pass: the clock is now */
    if (list->list_var) vlist_scrolled(list);
    else list_realize(list, false);
    list_sticky_update(list);
}

/* ---------------------------------------------------------------- variable-height list */

/* Same idea as the uniform list (rows built at layout, for the visible range only, into the
 * list's arena), but a row's height is whatever it lays out to, remembered in a gp_vlist.
 *
 * The scroll position is an anchor: the row at the top of the viewport and how far into it the
 * viewport starts. Realizing builds that row, then rows downward until the viewport (plus some
 * overdraw) is covered, then rows upward for whatever is left above, with every row measured
 * as it is built. So everything visible is laid out from true heights, never from estimates,
 * and the pixel offset is derived from the anchor afterwards. Estimates only decide which row
 * a far jump lands on, and the scroll extent of rows nobody has seen. */

typedef struct {
    gp_el **v;
    size_t len, cap;
} rowvec;

static void rowvec_push(rowvec *r, gp_el *e) {
    if (r->len == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 32;
        r->v = realloc(r->v, r->cap * sizeof *r->v);
        if (!r->v) abort();
    }
    r->v[r->len++] = e;
}

typedef struct {
    gp_el *n;
    gp_vlist *vl;
    gp_arena *arena;
    bool hz;              /* the list runs along x: items are as tall as the list and as wide as they come to be */
    float inner_w;        /* the size across the axis: the width rows are laid out at, or (hz) the height the items are */
    size_t count, j, k;   /* rows [j, k) are built */
    double height;        /* of those rows together */
    rowvec fwd, back;     /* rows idx.. in order; rows before idx, nearest first */
} vfill;

static void vlist_warn_cap(void) {
    static bool warned;
    if (warned) return;
    warned = true;
    fprintf(stderr, "gpuic: gp_list has no bounded height; showing %d rows "
                    "(give it gp_h / gp_flex_1 / gp_max_h)\n", LIST_MAX_REALIZED);
}

/* The extent of an element along the list's axis. */
static inline float along(const gp_el *c, bool hz) { return hz ? c->size.w : c->size.h; }

/* Builds row `idx` and measures it; the result is laid out at its own extent along the axis (a row: its height at the list's width; an item of a horizontal list: its width at the list's height). */
static gp_el *vrow(vfill *f, size_t idx) {
    gp_el **r = gp_list_build(f->n, idx, 1, f->arena);
    gp_el *c = r[0];
    if (g_anims && c->anim_below) gp_anim_eval(c, g_anims);
    c->parent = f->n;
    prepare(c, f->n->ts, row_path(f->n, idx));
    if (!f->hz) {
        float h = layout_node(c, f->inner_w, NAN, f->inner_w, NAN, false).h;
        if (!(h >= 0)) h = 0;
        layout_node(c, f->inner_w, h, f->inner_w, h, true);
    } else {
        float w = layout_node(c, NAN, f->inner_w, NAN, f->inner_w, false).w;
        if (!(w >= 0)) w = 0;
        layout_node(c, w, f->inner_w, w, f->inner_w, true);
    }
    gp_vlist_set(f->vl, idx, along(c, f->hz));
    return c;
}

/* Builds rows downward until the bottom of the block (the top of row j being at `vy0`) reaches `limit`. */
static void vfill_down(vfill *f, double vy0, double limit) {
    while (f->k < f->count && vy0 + f->height < limit) {
        if (f->k - f->j >= LIST_MAX_REALIZED) { vlist_warn_cap(); return; }
        gp_el *c = vrow(f, f->k++);
        rowvec_push(&f->fwd, c);
        f->height += along(c, f->hz);
    }
}

/* Builds rows upward until the top of the block is above `limit`; moves *vy0 with it. */
static void vfill_up(vfill *f, double *vy0, double limit) {
    while (f->j > 0 && *vy0 > limit) {
        if (f->k - f->j >= LIST_MAX_REALIZED) { vlist_warn_cap(); return; }
        gp_el *c = vrow(f, --f->j);
        rowvec_push(&f->back, c);
        f->height += along(c, f->hz);
        *vy0 -= along(c, f->hz);
    }
}

/* Where the viewport top at logical offset y is, as an anchor. Also says whether it is at the end.
 * A small move from the last anchor (`delta`, if known) is made along the rows themselves, so
 * every row crossed is measured and the view lands exactly where the user scrolled to; a big
 * jump can only go by the estimate, since nothing in between is worth building. */
static void vlist_anchor_at(const gp_el *n, const gp_vlist *vl, double y, bool have_delta, double delta,
                            size_t *idx, double *off, bool *to_end) {
    const gp_style *s = &n->style;
    const bool hz = n->list_horiz;
    float pad_a = hz ? s->pad[0] : s->pad[1], pad_b = hz ? s->pad[2] : s->pad[3];
    float view_h = (hz ? n->size.w : n->size.h) - 2 * s->border_w;
    double total = pad_a + gp_vlist_total(vl) + pad_b;
    *to_end = y >= total - view_h - 0.5;
    if (vl->n && have_delta && fabs(delta) <= 3 * view_h + 64) {
        *idx = vl->a_idx < vl->n ? vl->a_idx : vl->n - 1;
        *off = vl->a_off + delta;
        return;
    }
    double yc = y - pad_a;
    if (yc < 0 || !vl->n) { *idx = 0; *off = yc < 0 ? yc : 0; }
    else *idx = gp_vlist_find(vl, yc, off);
}

/* Builds the rows around the anchor (idx, off); if `to_end`, around the end of the list instead.
 * Fixes n->scroll / scroll_max / the saved state to match what was built. n->size must be final. */
static void vlist_realize(gp_el *n, gp_vlist *vl, size_t idx, double off, bool to_end) {
    const gp_style *s = &n->style;
    const bool hz = n->list_horiz;
    float bw = s->border_w;
    float view_h = fmaxf((hz ? n->size.w : n->size.h) - 2 * bw, 0);   /* (the extent along the axis, as the rest of this function reads it) */
    float pad_top = hz ? s->pad[0] : s->pad[1], pad_bottom = hz ? s->pad[2] : s->pad[3];
    size_t count = n->list_count;

    gp_arena *a = list_arena(n);
    gp_arena_reset(a);
    n->children = NULL;
    n->nchildren = 0;
    n->list_first = n->list_n = 0;
    n->list_realized = true;

    vfill f = { .n = n, .vl = vl, .arena = a, .count = count, .hz = hz,
                .inner_w = hz ? fmaxf(n->size.h - (s->pad[1] + s->pad[3] + 2 * bw), 0) : fmaxf(n->size.w - (s->pad[0] + s->pad[2] + 2 * bw), 0) };
    double vy0 = 0;   /* top of row j relative to the top of the viewport */
    double od = view_h / 4;
    if (count) {
        if (to_end) { idx = count - 1; off = 0; }
        if (idx >= count) idx = count - 1;
        f.j = idx;
        f.k = idx + 1;
        gp_el *c = vrow(&f, idx);
        rowvec_push(&f.fwd, c);
        f.height = along(c, hz);
        vy0 = -off;
        vfill_down(&f, vy0, view_h + od);
        /* ran out of rows with room to spare (or told to end there): slide everything down */
        if (to_end || (f.k == count && vy0 + f.height + pad_bottom < view_h))
            vy0 = view_h - pad_bottom - f.height;
        vfill_up(&f, &vy0, -od);
        if (f.j == 0 && vy0 > pad_top) {   /* the start of the list is as far as it goes */
            vy0 = pad_top;
            vfill_down(&f, vy0, view_h + od);
        }
    }

    size_t total_rows = f.back.len + f.fwd.len;
    n->children = gp_arena_alloc(a, (total_rows ? total_rows : 1) * sizeof *n->children, 8);
    size_t w = 0;
    for (size_t i = f.back.len; i-- > 0;) n->children[w++] = f.back.v[i];
    for (size_t i = 0; i < f.fwd.len; i++) n->children[w++] = f.fwd.v[i];
    n->nchildren = (uint32_t)total_rows;
    free(f.fwd.v);
    free(f.back.v);

    double block_top = pad_top + (count ? gp_vlist_top(vl, f.j) : 0);
    double cum = 0;   /* rows are positioned relative to the first one: see gp_el.scroll_base_y */
    for (size_t i = 0; i < total_rows; i++) {
        gp_el *c = n->children[i];
        c->pos = hz ? (gp_point){ bw + (float)cum, bw + s->pad[1] } : (gp_point){ bw + s->pad[0], bw + (float)cum };   /* (the padding along the axis is in the scroll base) */
        cum += along(c, hz);
    }

    double total = pad_top + gp_vlist_total(vl) + pad_bottom;
    double max = fmax(0, total - view_h);
    double y = count ? block_top - vy0 : 0;
    y = fmin(fmax(y, 0), max);
    float y_f = (float)y;

    if (count) {   /* the row now at the top of the viewport */
        size_t ai = f.j;
        double ao = y - block_top;
        while (ai + 1 < f.k && ao >= gp_vlist_height(vl, ai)) {
            ao -= gp_vlist_height(vl, ai);
            ai++;
        }
        vl->a_idx = ai;
        vl->a_off = ao;
    } else {
        vl->a_idx = 0;
        vl->a_off = 0;
    }
    vl->last_y = y_f;
    vl->at_end = y >= max - 0.5;

    n->list_first = f.j;
    n->list_n = f.k - f.j;
    n->list_head = f.j == 0;
    n->list_tail = f.k == count;
    n->list_bb = (float)f.height;
    gp_scroll_state *st;
    if (!hz) {
        n->scroll_base_y = block_top;
        n->scroll = (gp_point){ 0, (float)(y - block_top) };
        n->list_anchor_rel = n->scroll.y;
        n->scroll_max = (gp_point){ 0, (float)max };
        /* looked up only now: a row that holds a scroller of its own adds to the table while it is laid out */
        st = gp_scroll_state_get(g_scrolls, n->scroll_key, true);
        st->x = 0;
        st->y = y_f;
        st->max_y = (float)max;
        st->max_x = 0;
        st->view_h = view_h;
        st->view_w = fmaxf(n->size.w - 2 * bw, 0);
    } else {   /* the same along x */
        n->scroll_base_x = block_top;
        n->scroll = (gp_point){ (float)(y - block_top), 0 };
        n->list_anchor_rel = n->scroll.x;
        n->scroll_max = (gp_point){ (float)max, 0 };
        st = gp_scroll_state_get(g_scrolls, n->scroll_key, true);
        st->x = y_f;
        st->y = 0;
        st->max_x = (float)max;
        st->max_y = 0;
        st->view_w = view_h;
        st->view_h = fmaxf(n->size.h - 2 * bw, 0);
        st->horiz = true;
    }
    st->pad_top = pad_top;
}

/* A pending gp_list_scroll_to_item becomes an anchor. */
static void vlist_want(gp_el *n, gp_vlist *vl, float cur_y, size_t *idx, double *off, bool *to_end) {
    const gp_style *s = &n->style;
    const bool hz = n->list_horiz;
    float view_h = fmaxf((hz ? n->size.w : n->size.h) - 2 * s->border_w, 0), pad_top = hz ? s->pad[0] : s->pad[1];
    size_t want = vl->want_idx < n->list_count ? vl->want_idx : n->list_count - 1;
    vl->want = false;

    /* the row's own height: known, or built just to find out */
    float h = gp_vlist_height(vl, want);
    if (!gp_vlist_measured(vl, want)) {
        vfill probe = { .n = n, .vl = vl, .arena = list_arena(n), .hz = hz,
                        .inner_w = hz ? fmaxf(n->size.h - (s->pad[1] + s->pad[3] + 2 * s->border_w), 0) : fmaxf(n->size.w - (s->pad[0] + s->pad[2] + 2 * s->border_w), 0) };
        gp_arena_reset(probe.arena);
        h = along(vrow(&probe, want), hz);
    }
    double top = pad_top + gp_vlist_top(vl, want);   /* content coordinates */
    double y = cur_y;
    switch (vl->want_align) {
    case GP_ALIGN_ROW_START:
        *idx = want; *off = -pad_top; *to_end = false;
        break;
    case GP_ALIGN_ROW_CENTER:
        *idx = want; *off = -(view_h - h) / 2; *to_end = false;
        break;
    default:
        if (top - pad_top < y) { *idx = want; *off = -pad_top; *to_end = false; }
        else if (top + h > y + view_h) { *idx = want; *off = h - view_h; *to_end = false; }
        /* else it is in view already: keep the anchor chosen by the caller */
        break;
    }
}

static gp_vlist *vlist_state(gp_el *n, gp_scroll_state *st) {
    if (!st->vl) {
        st->vl = gp_vlist_new(n->list_item_h);
        st->vl->at_end = n->list_stick;   /* a list that sticks to the end starts there */
        st->vl->fresh = true;
    }
    gp_vlist_resize(st->vl, n->list_count);
    return st->vl;
}

static gp_size layout_vlist(gp_el *n, float w, float h, float pw, float ph, bool commit) {
    const gp_style *s = &n->style;
    const bool hz = n->list_horiz;
    float bw = s->border_w;
    float chrome_w = s->pad[0] + s->pad[2] + 2 * bw, chrome_h = s->pad[1] + s->pad[3] + 2 * bw;

    /* across the axis: a list of rows fills the width it is offered (or fits row 0); one of items as wide as they come is as tall as its first item,
     * unless told otherwise. Along it: the content, unless told. */
    float out_w = w, out_h = h;
    if (!hz) {
        if (!def(out_w)) {
            if (def(pw)) out_w = pw;
            else out_w = (n->list_count ? list_probe(n, NAN).w : 0) + chrome_w;
        }
    } else if (!def(out_h)) {
        out_h = (n->list_count ? list_probe(n, NAN).h : 0) + chrome_h;
    }
    float inner_w = hz ? fmaxf(out_h - chrome_h, 0) : fmaxf(out_w - chrome_w, 0);   /* (the size across the axis) */

    gp_scroll_state *st = gp_scroll_state_get(g_scrolls, n->scroll_key, true);
    gp_vlist *vl = vlist_state(n, st);
    if (commit && vl->width != inner_w) {   /* wrapped text has other extents at another size across */
        gp_vlist_clear(vl);
        vl->width = inner_w;
    }
    if (n->list_item_h <= 0 && !vl->nmeasured && n->list_count) {   /* the estimate is row 0 */
        gp_size p0 = list_probe(n, inner_w);
        float h0 = hz ? p0.w : p0.h;
        vl->est0 = h0 > 0 ? h0 : 1;
    }

    float content = (float)gp_vlist_total(vl) + (hz ? s->pad[0] + s->pad[2] : s->pad[1] + s->pad[3]);
    if (!hz) out_h = def(h) ? h : content + 2 * bw;
    else out_w = def(w) ? w : (def(pw) ? pw : content + 2 * bw);
    out_w = clamp_len(out_w, s->min_w, s->max_w, pw);
    out_h = clamp_len(out_h, s->min_h, s->max_h, ph);
    if (!commit) return (gp_size){ out_w, out_h };

    n->size = (gp_size){ out_w, out_h };
    st = gp_scroll_state_get(g_scrolls, n->scroll_key, true);   /* the probe above may have grown the table */
    float pos = hz ? st->x : st->y;
    size_t idx;
    double off;
    bool to_end;
    if (vl->fresh) {
        idx = 0; off = -(hz ? s->pad[0] : s->pad[1]); to_end = n->list_stick;
        vl->fresh = false;
    } else if (vl->last_y == pos) {   /* nobody scrolled: stay on the same row */
        idx = vl->a_idx; off = vl->a_off; to_end = n->list_stick && vl->at_end;
    } else {                            /* scrolled from outside (gp_scroll_to, wheel, thumb) */
        vlist_anchor_at(n, vl, pos, vl->last_y >= 0, (double)pos - vl->last_y, &idx, &off, &to_end);
    }
    if (vl->want && n->list_count) vlist_want(n, vl, pos, &idx, &off, &to_end);
    vl->want = false;
    vlist_realize(n, vl, idx, off, to_end);
    list_sticky_update(n);
    return (gp_size){ out_w, out_h };
}

/* The offset of a variable-height list changed without a rebuild (wheel, thumb): realize the
 * rows now in view, unless the rows already built cover them. */
static void vlist_scrolled(gp_el *n) {
    gp_scroll_state *st = gp_scroll_state_get(g_scrolls, n->scroll_key, false);
    if (!st || !st->vl || !n->list_realized) return;
    gp_vlist *vl = st->vl;
    const bool hz = n->list_horiz;
    float rel = hz ? n->scroll.x : n->scroll.y;   /* relative to the first realized row */
    double y = (hz ? n->scroll_base_x : n->scroll_base_y) + rel;
    float max = hz ? n->scroll_max.x : n->scroll_max.y;
    float view_h = fmaxf((hz ? n->size.w : n->size.h) - 2 * n->style.border_w, 0);
    /* the end is never taken from the rows already built: the extent is a float, and the last row
     * has to come out exactly flush with the bottom */
    if ((n->list_head || rel >= 0) && (n->list_tail || rel + view_h <= n->list_bb) &&
        y < (double)max - 0.5) {
        float yf = (float)y;
        if (hz) st->x = yf; else st->y = yf;
        vl->last_y = yf;
        vl->at_end = y >= max - 0.5;
        n->list_anchor_rel = rel;
        /* keep the anchor on the row now at the top, from the positions of the rows themselves */
        for (uint32_t i = 0; i < n->nchildren; i++) {
            const gp_el *c = n->children[i];
            float top = (hz ? c->pos.x : c->pos.y) - n->style.border_w;
            if (i + 1 == n->nchildren || rel < top + along(c, hz)) {
                vl->a_idx = n->list_first + i;
                vl->a_off = (double)rel - top;
                break;
            }
        }
        return;
    }
    size_t idx;
    double off;
    bool to_end;
    vlist_anchor_at(n, vl, y, true, (double)rel - n->list_anchor_rel, &idx, &off, &to_end);
    vlist_realize(n, vl, idx, off, to_end);
}

static gp_size layout_text(gp_el *n, float w, float h, float pw, float ph) {
    const gp_style *s = &n->style;
    float chrome_w = s->pad[0] + s->pad[2] + 2 * s->border_w;
    float chrome_h = s->pad[1] + s->pad[3] + 2 * s->border_w;
    float aw = def(w) ? maxf(w - chrome_w, 0) : (def(pw) ? maxf(pw - chrome_w, 0) : -1.f);
    gp_size t = gp_text_measure_dir(g_ts, GP_TEXT_SEL(n->ts), n->ts.size, n->text, aw, s->whitespace_pre,
                                     n->ts.dir, NULL, NULL, NULL);
    (void)ph;
    return (gp_size){ def(w) ? w : ceilf(t.w) + chrome_w, def(h) ? h : t.h + chrome_h };
}

/* An image is as big as its pixels, unless the style fixes one side (the other follows the picture's proportions) or both. */
static gp_size layout_image(gp_el *n, float w, float h, float pw, float ph) {
    const gp_style *s = &n->style;
    float chrome_w = s->pad[0] + s->pad[2] + 2 * s->border_w;
    float chrome_h = s->pad[1] + s->pad[3] + 2 * s->border_w;
    float iw = 0, ih = 0;
    (void)pw;
    (void)ph;
    bool known;
    if (n->kind == GP_EL_SVG) {
        known = gp_gfx_svg_size(gp_text_gfx(g_ts), n->svg, &iw, &ih);
    } else {
        int pxw = 0, pxh = 0;
        known = gp_gfx_image_size(gp_text_gfx(g_ts), n->image, &pxw, &pxh);
        iw = (float)pxw;
        ih = (float)pxh;
    }
    if (!known) return (gp_size){ def(w) ? w : chrome_w, def(h) ? h : chrome_h };
    float cw = def(w) ? maxf(w - chrome_w, 0) : -1.f, ch = def(h) ? maxf(h - chrome_h, 0) : -1.f;
    if (cw < 0 && ch < 0) { cw = iw; ch = ih; }
    else if (cw < 0) cw = ch * iw / ih;
    else if (ch < 0) ch = cw * ih / iw;
    return (gp_size){ def(w) ? w : cw + chrome_w, def(h) ? h : ch + chrome_h };
}

/* The box of everything below `n` that can paint differently when the mouse is over it, in the frame whose
 * origin is the top-left of the cached root: grown to include the boxes of n's descendants. A node that clips
 * (overflow hidden, a scroller) shows nothing outside its own box, however far its content reaches or scrolls,
 * and a scrollbar is inside it. */
static void extent(const gp_el *n, float lx, float ly, float *ext) {
    ext[0] = fminf(ext[0], lx);
    ext[1] = fminf(ext[1], ly);
    ext[2] = fmaxf(ext[2], lx + n->size.w);
    ext[3] = fmaxf(ext[3], ly + n->size.h);
    if (n->style.overflow_hidden || n->style.overflow_x_scroll || n->style.overflow_y_scroll) return;
    for (uint32_t i = 0; i < n->nchildren; i++)
        extent(n->children[i], lx + n->children[i]->pos.x, ly + n->children[i]->pos.y, ext);
}

static gp_size layout_node(gp_el *n, float fw, float fh, float pw, float ph, bool commit) {
    const gp_style *s = &n->style;
    float w = def(fw) ? fw : resolve(s->w, pw);
    float h = def(fh) ? fh : resolve(s->h, ph);
    w = clamp_len(w, s->min_w, s->max_w, pw);
    h = clamp_len(h, s->min_h, s->max_h, ph);

    if (!commit) {
        for (int i = 0; i < n->ncache; i++) {
            if (same(n->cache[i].key[0], w) && same(n->cache[i].key[1], h) &&
                same(n->cache[i].key[2], pw) && same(n->cache[i].key[3], ph))
                return n->cache[i].out;
        }
    }

    /* A cached view's subtree that was laid out for exactly these inputs is still laid out: nothing writes to
     * it but layout. Scrollers and lists below it are written to by the wheel and the thumbs, but those
     * keep the elements and the scroll table in step as they go. What they do not see is a change made from
     * outside (gp_scroll_to, gp_list_splice, ...): that bumps the table's ext_gen, and the subtree is laid
     * out again for it. Nor do they see rows being measured (lk_meas): a list's extent is an estimate until its
     * rows are known, and a layout after rows were measured, by the wheel or by the last layout itself, gives
     * another one. A list as tall as its rows therefore settles after a few layouts, and is skipped from then on. */
    bool reusable = n->cached_view.generation && g_frame_caches;
    bool dyn = (n->sub_flags & GP_SUB_DYN_LAYOUT) != 0;
    uint32_t ext_gen = g_scrolls ? g_scrolls->ext_gen : 0, meas = gp_vlist_generation();
    if (commit && reusable && n->lk_valid && same(n->lk[0], w) && same(n->lk[1], h) && same(n->lk[2], pw) &&
        same(n->lk[3], ph) && (!dyn || (n->lk_ext == ext_gen && n->lk_meas == meas))) {
        g_skips++;
        gp_vview *vv = g_views ? gp_views_find(g_views, n->cached_view) : NULL;
        if (vv) vv->skipped_epoch = g_views->epoch;
        return n->size;
    }

    gp_size out = n->kind == GP_EL_TEXT   ? layout_text(n, w, h, pw, ph)
                : n->kind == GP_EL_IMAGE || n->kind == GP_EL_SVG ? layout_image(n, w, h, pw, ph)
                : n->kind == GP_EL_LIST ? (n->list_var ? layout_vlist(n, w, h, pw, ph, commit)
                                                     : layout_list(n, w, h, pw, ph, commit))
                                        : layout_container(n, w, h, pw, ph, commit);
    if (commit) {
        n->size = out;
        if (n->mask) {   /* over the border box, as an absolute child filling it would be */
            n->mask->pos = (gp_point){ 0, 0 };
            layout_node(n->mask, out.w, out.h, out.w, out.h, true);
            n->mask->size = out;
        }
        if (reusable) {
            n->lk[0] = w; n->lk[1] = h; n->lk[2] = pw; n->lk[3] = ph;
            n->lk_ext = ext_gen;
            n->lk_meas = meas;   /* as of the start: a layout that measured rows is not the last word on rows' heights */
            n->lk_valid = true;
            n->layout_gen++;
            if (n->sub_flags & (GP_SUB_HOVER | GP_SUB_SCROLL)) {
                n->ext[0] = n->ext[1] = 0;
                n->ext[2] = out.w;
                n->ext[3] = out.h;
                extent(n, 0, 0, n->ext);
            }
        }
    } else {
        int slot = n->ncache < 4 ? n->ncache++ : 0;
        n->cache[slot].key[0] = w; n->cache[slot].key[1] = h;
        n->cache[slot].key[2] = pw; n->cache[slot].key[3] = ph;
        n->cache[slot].out = out;
    }
    return out;
}

void gp_layout_root(gp_el *root, gp_text_system *ts, gp_size viewport, gp_scroll_table *scrolls, gp_vviews *views,
                    gp_anim_table *anims) {
    g_ts = ts;
    g_scrolls = scrolls;
    g_views = views;
    g_anims = anims;
    if (anims) {
        gp_anim_begin(anims);
        gp_anim_eval(root, anims);   /* styles first: prepare and layout read them */
    }
    gp_text_style base = { 16.f, 400, 0, { 0, 0, 0, 1 }, GP_TEXT_ALIGN_START, GP_BIDI_AUTO, false, 0, 0 };
    prepare(root, base, 14695981039346656037ull);
    root->pos = (gp_point){ 0, 0 };
    root->size = viewport;
    layout_node(root, viewport.w, viewport.h, viewport.w, viewport.h, true);
    if (anims) gp_anim_end(anims);
    g_anims = NULL;
}
