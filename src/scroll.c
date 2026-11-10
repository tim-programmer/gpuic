/*
 * Scrolling: persistent offsets, scrollbar thumbs, wheel dispatch (with chaining to
 * ancestors) and thumb dragging.
 *
 * Offsets live in a per-window table keyed by gp_el.scroll_key, so they survive
 * view rebuilds. Layout clamps them and records the maximum; painting and hit
 * testing translate a scroller's children by -offset and clip them to its box.
 */
#include "internal.h"

#define MAX_DEPTH 128
#define THUMB_THICK 6.f
#define THUMB_INSET 2.f
#define THUMB_MIN 24.f
#define THUMB_GRAB_PAD 4.f /* extra hit area toward the content side */
#define TRACK_PAGE 0.875f /* a press on the track scrolls this much of the view */
#define TRACK_REPEAT_FIRST_NS 400000000ll   /* held, it pages again after this */
#define TRACK_REPEAT_NS 100000000ll         /* ...and then at this interval */

typedef struct { float x0, y0, x1, y1; } rect;

static bool rect_has(rect r, gp_point p) { return p.x >= r.x0 && p.x < r.x1 && p.y >= r.y0 && p.y < r.y1; }

static bool is_scroller(const gp_el *n) { return n->style.overflow_x_scroll || n->style.overflow_y_scroll; }

/* ---------------------------------------------------------------- state table */

gp_scroll_state *gp_scroll_state_get(gp_scroll_table *t, uint64_t key, bool create) {
    for (size_t i = 0; i < t->len; i++)
        if (t->items[i].key == key) return &t->items[i];
    if (!create) return NULL;
    if (t->len == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 16;
        t->items = realloc(t->items, t->cap * sizeof *t->items);
        if (!t->items) abort();
    }
    t->items[t->len] = (gp_scroll_state){ .key = key };
    return &t->items[t->len++];
}

/* ---------------------------------------------------------------- thumbs */

bool gp_scroll_thumb(const gp_el *n, int axis, float ox, float oy, gp_thumb *t) {
    const gp_style *s = &n->style;
    if (s->hide_scrollbar) return false;
    if (s->autohide_scrollbar && n->thumb_alpha <= 0.02f) return false;   /* faded out: nothing there to see or grab */
    bool sx = s->overflow_x_scroll && n->scroll_max.x > 0;
    bool sy = s->overflow_y_scroll && n->scroll_max.y > 0;
    float bw = s->border_w;
    float vw = n->size.w - 2 * bw, vh = n->size.h - 2 * bw; /* padding box */
    float corner = sx && sy ? THUMB_THICK + 2 * THUMB_INSET : 0;

    float view, max, offset, track;
    if (axis == 1) {
        if (!sy) return false;
        view = vh; max = n->scroll_max.y; offset = (float)(n->scroll_base_y + n->scroll.y);
        track = vh - 2 * THUMB_INSET - corner;
    } else {
        if (!sx) return false;
        view = vw; max = n->scroll_max.x; offset = (float)(n->scroll_base_x + n->scroll.x);
        track = vw - 2 * THUMB_INSET - corner;
    }
    if (track < THUMB_MIN / 2) return false;
    float len = fminf(fmaxf(track * view / (view + max), THUMB_MIN), track);
    float pos = track > len ? offset / max * (track - len) : 0;

    t->thumb_len = len;
    t->track_len = track;
    t->max = max;
    if (axis == 1) {
        t->track_start = oy + bw + THUMB_INSET;
        t->x = ox + n->size.w - bw - THUMB_THICK - THUMB_INSET;
        t->y = t->track_start + pos;
        t->w = THUMB_THICK;
        t->h = len;
    } else {
        t->track_start = ox + bw + THUMB_INSET;
        t->x = t->track_start + pos;
        t->y = oy + n->size.h - bw - THUMB_THICK - THUMB_INSET;
        t->w = len;
        t->h = THUMB_THICK;
    }
    return true;
}

/* ---------------------------------------------------------------- wheel */

/* chain[0..len) = root .. topmost element under p (ancestors need not contain p). */
/* A transformed ancestor: `fwd` maps layout coordinates to the window (when `moved`); the element's box is tested with the point taken back through it, and the clip, which is in
 * the window, with the point itself. A clip box through a transform that keeps the axes is the box it becomes; at any other angle it does not clip (as painting does). */
static bool aff_box(gp_affine m, rect r, rect *out) {
    bool straight = (fabsf(m.b) < 1e-5f && fabsf(m.c) < 1e-5f) || (fabsf(m.a) < 1e-5f && fabsf(m.d) < 1e-5f);
    if (!straight) return false;
    gp_point p0 = gp_aff_apply(m, (gp_point){ r.x0, r.y0 }), p1 = gp_aff_apply(m, (gp_point){ r.x1, r.y1 });
    *out = (rect){ fminf(p0.x, p1.x), fminf(p0.y, p1.y), fmaxf(p0.x, p1.x), fmaxf(p0.y, p1.y) };
    return true;
}
static gp_point enter(const gp_el *n, float x, float y, gp_point p, gp_affine *fwd, bool *moved) {
    if (n->style.has_xf) {
        *fwd = gp_aff_mul(*moved ? *fwd : gp_aff_identity(), gp_style_affine(&n->style, x, y, n->size.w, n->size.h));
        *moved = true;
    }
    return *moved ? gp_aff_apply(gp_aff_inverse(*fwd), p) : p;
}
static rect clip_to(rect clip, rect b, gp_affine fwd, bool moved) {
    rect box = b;
    if (moved && !aff_box(fwd, b, &box)) return clip;
    return (rect){ fmaxf(clip.x0, box.x0), fmaxf(clip.y0, box.y0), fminf(clip.x1, box.x1), fminf(clip.y1, box.y1) };
}

static bool chain_rec(gp_el *n, float ox, float oy, rect clip, gp_point p, gp_el **chain,
                      int depth, int *len, gp_affine fwd, bool moved) {
    if (depth >= MAX_DEPTH) return false;
    chain[depth] = n;
    float x = ox + n->pos.x, y = oy + n->pos.y;
    rect b = { x, y, x + n->size.w, y + n->size.h };
    gp_point q = enter(n, x, y, p, &fwd, &moved);
    rect cc = clip;
    if (n->style.overflow_hidden || is_scroller(n)) cc = clip_to(clip, b, fwd, moved);
    for (uint32_t i = n->nchildren; i-- > 0;)
        if (chain_rec(n->children[i], x - n->scroll.x, y - n->scroll.y, cc, p, chain, depth + 1, len, fwd, moved))
            return true;
    if (rect_has(clip, p) && rect_has(b, q)) {
        *len = depth + 1;
        return true;
    }
    return false;
}

/* The scroller `chain[at]` moved without a layout: the cached views around it (and it, if it is the root of
 * one) were painted for the old offset. */
static void moved_in_place(gp_el **chain, int at) {
    for (int i = 0; i <= at; i++)
        if (chain[i]->cached_view.generation) chain[i]->dyn_gen++;
}

void gp_scroll_notify(gp_window *w, gp_el *n) {
    if (!n->on_scroll.fn) return;
    void *state = gp_entity_get(w->app, n->on_scroll.owner);
    if (!state) return;
    gp_ctx cx = { w->app, w, n->on_scroll.owner, false };
    ((gp_scroll_fn)n->on_scroll.fn)(state, &cx, (gp_point){ (float)(n->scroll_base_x + n->scroll.x), (float)(n->scroll_base_y + n->scroll.y) }, n->on_scroll.data);
}

/* Records a container's new offset. For a virtualized list the rows in view changed too. */
static void store_offset(gp_window *w, gp_el *n) {
    gp_scroll_state *st = gp_scroll_state_get(&w->scrolls, n->scroll_key, true);
    st->x = (float)(n->scroll_base_x + n->scroll.x);
    st->y = (float)(n->scroll_base_y + n->scroll.y);
    st->smooth = false;              /* (gp_scroll_smooth_update sets it again if the glide goes on) */
    st->activity_ns = gp_now_ns();   /* an auto-hiding scrollbar shows for a while */
    if (n->kind == GP_EL_LIST) gp_layout_list_scrolled(n, w->app->text, &w->scrolls, &w->anims);
    gp_scroll_notify(w, n);   /* (an element that draws only what is in view may need to draw something else now) */
}

/* Applies `*d` along one axis; returns true (and zeroes *d) if the container moved. The offset
 * is *off + base (a gp_list keeps *off small, see gp_el.scroll_base_y), and the sum is made in
 * double so that a very long list still moves by exactly the wheel's step. */
static bool consume(float *off, double base, float max, float *d) {
    if (*d == 0 || max <= 0) return false;
    double cur = base + *off;
    double next = fmin(fmax(cur + *d, 0), max);
    if (next == cur) return false; /* already at the limit: let an ancestor have it */
    *off = (float)(next - base);
    *d = 0;
    return true;
}

/* Smooth scrolling: the wheel moves a scroller's target, not its offset, and the offset follows (gp_scroll_smooth_update).
 * Chaining is decided against the targets, so a second tick that finds the target at the end goes to the scroller above, whatever
 * the offset has reached by then. Anything else that sets an offset (store_offset: thumb, reveal, a jump) ends the glide. */
static bool chain_to_key(gp_el *n, uint64_t key, gp_el **chain, int depth, int *len);
static bool wheel_smooth(gp_window *w, gp_el **chain, int len, float dx, float dy) {
    bool moved = false;
    for (int i = len - 1; i >= 0 && (dx != 0 || dy != 0); i--) {
        gp_el *n = chain[i];
        if (!is_scroller(n)) continue;
        gp_scroll_state *st = gp_scroll_state_get(&w->scrolls, n->scroll_key, true);
        double base = n->scroll_base_y, base_x = n->scroll_base_x;
        double cx = st->smooth ? st->tx : base_x + n->scroll.x, cy = st->smooth ? st->ty : base + n->scroll.y;
        bool m = false;
        if (n->style.overflow_y_scroll) {
            float rel = (float)(cy - base);
            if (consume(&rel, base, n->scroll_max.y, &dy)) { cy = base + rel; m = true; }
        }
        if (n->style.overflow_x_scroll) {
            float rel = (float)(cx - base_x);
            if (consume(&rel, base_x, n->scroll_max.x, &dx)) { cx = base_x + rel; m = true; }
        }
        if (!m) continue;
        bool listed = false;
        for (int k = 0; k < w->nsmooth; k++) listed |= w->smooth_keys[k] == n->scroll_key;
        if (!listed && w->nsmooth == 8) {   /* too many at once: this one jumps */
            n->scroll.x = (float)(cx - base_x);
            n->scroll.y = (float)(cy - base);
            store_offset(w, n);
            moved = true;
            continue;
        }
        if (!listed) w->smooth_keys[w->nsmooth++] = n->scroll_key;
        st->tx = (float)cx;
        st->ty = (float)cy;
        st->smooth = true;
        st->activity_ns = gp_now_ns();
        moved = true;
    }
    if (moved) w->dirty_paint = true;   /* a draw takes the first step */
    return moved;
}

/* Scrolls along the chain at `p` by (dx, dy) at once: the innermost scroller that can still move takes what it can, the one around it the rest. */
static bool scroll_direct(gp_window *w, gp_el **chain, int len, float dx, float dy) {
    bool moved = false;
    for (int i = len - 1; i >= 0 && (dx != 0 || dy != 0); i--) {
        gp_el *n = chain[i];
        if (!is_scroller(n)) continue;
        bool m = false;
        if (n->style.overflow_y_scroll) m |= consume(&n->scroll.y, n->scroll_base_y, n->scroll_max.y, &dy);
        if (n->style.overflow_x_scroll) m |= consume(&n->scroll.x, n->scroll_base_x, n->scroll_max.x, &dx);
        if (m) {
            store_offset(w, n);
            moved_in_place(chain, i);
            moved = true;
        }
    }
    if (moved) w->dirty_paint = true;
    return moved;
}

static bool scroll_at(gp_window *w, gp_point p, float dx, float dy, bool direct) {
    if (!w->tree) return false;
    gp_el *chain[MAX_DEPTH];
    int len = 0;
    rect all = { -1e9f, -1e9f, 1e9f, 1e9f };
    if (!chain_rec(w->tree, 0, 0, all, p, chain, 0, &len, gp_aff_identity(), false)) return false;
    if (!direct && w->app->smooth_scroll_s > 0) return wheel_smooth(w, chain, len, dx, dy);
    return scroll_direct(w, chain, len, dx, dy);
}

void gp_scroll_wheel(gp_window *w, gp_point p, float dx, float dy) {
    gp_scroll_fling_stop(w);   /* (a wheel turn takes over from a fling in progress) */
    scroll_at(w, p, dx, dy, false);
}

/* ---------------------------------------------------------------- touch: drag and fling */

#define TOUCH_SLOP 8.f              /* a finger must move this far before a press is a drag and not a tap */
#define FLING_TAU 0.35              /* seconds for the speed to fall by e: a flick of v px/s goes v * 0.35 px */
#define FLING_MIN_SPEED 120.f       /* below this speed on release there is no fling */
#define FLING_STOP_SPEED 8.f
#define VELOCITY_WINDOW_NS 100000000ll
#define FINGER_STALE_NS 80000000ll   /* a finger that has not moved for this long when it lifts is not flicking */

void gp_scroll_fling_stop(gp_window *w) { w->fling.active = false; }

void gp_scroll_touch(gp_window *w, int phase, float x, float y, int64_t t_ns) {
    gp_point p = { x, y };
    if (phase == GP_TOUCH_DOWN) {
        gp_scroll_fling_stop(w);
        w->touch.down = true;
        w->touch.scrolling = false;
        w->touch.start = w->touch.last = p;
        w->touch.n = 1;
        w->touch.t[0] = t_ns;
        w->touch.p[0] = p;
        return;
    }
    if (!w->touch.down) return;
    if (phase == GP_TOUCH_MOVE) {
        if (!w->touch.scrolling && hypotf(x - w->touch.start.x, y - w->touch.start.y) < TOUCH_SLOP) return;   /* still a tap, perhaps */
        float dx = x - w->touch.last.x, dy = y - w->touch.last.y;
        if (!w->touch.scrolling) {   /* it becomes a drag now: the content follows from where the finger is, not from where it began */
            w->touch.scrolling = true;
            dx = dy = 0;
        }
        w->touch.last = p;
        if (w->touch.n == 32) { memmove(w->touch.t, w->touch.t + 1, 31 * sizeof *w->touch.t); memmove(w->touch.p, w->touch.p + 1, 31 * sizeof *w->touch.p); w->touch.n = 31; }
        w->touch.t[w->touch.n] = t_ns;
        w->touch.p[w->touch.n++] = p;
        scroll_at(w, p, -dx, -dy, true);   /* the content moves with the finger: it goes down when the finger does, so the offset shrinks */
        return;
    }
    /* up */
    w->touch.down = false;
    if (!w->touch.scrolling) {   /* a tap is a click */
        gp_window_on_mouse_move(w, x, y);
        gp_window_on_mouse_button(w, x, y, GP_MOUSE_LEFT, true, 1, 0);
        gp_window_on_mouse_button(w, x, y, GP_MOUSE_LEFT, false, 1, 0);
        gp_window_on_mouse_leave(w);   /* (a finger leaves no pointer behind to hover) */
        return;
    }
    /* the speed it leaves with: over the last 100 ms of its motion, none if it stopped before lifting */
    float vx = 0, vy = 0;
    int last = w->touch.n - 1;
    if (last >= 1 && t_ns - w->touch.t[last] <= FINGER_STALE_NS) {
        int k = last;
        while (k > 0 && w->touch.t[last] - w->touch.t[k - 1] <= VELOCITY_WINDOW_NS) k--;
        double dt = (double)(w->touch.t[last] - w->touch.t[k]) / 1e9;
        if (dt > 0 && k < last) {
            vx = (float)((double)(w->touch.p[last].x - w->touch.p[k].x) / dt);
            vy = (float)((double)(w->touch.p[last].y - w->touch.p[k].y) / dt);
        }
    }
    float speed = hypotf(vx, vy);
    if (speed >= FLING_MIN_SPEED) {
        w->fling = (typeof(w->fling)){ true, w->touch.last, -vx, -vy, 0 };   /* (content speed: against the finger's) */
        w->dirty_paint = true;   /* a draw takes the first step */
    }
}

void gp_scroll_fling_update(gp_window *w) {
    if (!w->fling.active) return;
    int64_t now = gp_now_ns();
    double dt = w->fling.last_ns ? (double)(now - w->fling.last_ns) / 1e9 : 1.0 / 60;
    w->fling.last_ns = now;
    /* v' = -v / tau: after dt the speed is v e^(-dt/tau) and the distance gone is v tau (1 - e^(-dt/tau)): exact, whatever the frame */
    double e = exp(-dt / FLING_TAU), travel = FLING_TAU * (1 - e);
    float dx = (float)(w->fling.vx * travel), dy = (float)(w->fling.vy * travel);
    w->fling.vx *= (float)e;
    w->fling.vy *= (float)e;
    bool moved = scroll_at(w, w->fling.at, dx, dy, true);
    if (!moved || hypotf(w->fling.vx, w->fling.vy) < FLING_STOP_SPEED) {   /* the end of the content, or friction has won */
        w->fling.active = false;
        w->fling.last_ns = 0;
        return;
    }
    if (!w->fade_job_pending) gp_exec_window_frame(w->app, w, GP_WAKE_PAINT);
}


void gp_scroll_smooth_update(gp_window *w) {
    if (!w->nsmooth || !w->tree) { w->smooth_last_ns = 0; w->nsmooth = 0; return; }
    int64_t now = gp_now_ns();
    double dt = w->smooth_last_ns ? (double)(now - w->smooth_last_ns) / 1e9 : 1.0 / 60;
    w->smooth_last_ns = now;
    /* an exponential approach: 98% of the way after the time the app asked for */
    double tau = (double)w->app->smooth_scroll_s / 4;
    double alpha = tau > 0 ? 1 - exp(-dt / tau) : 1;
    int keep = 0;
    uint64_t keys[8];
    memcpy(keys, w->smooth_keys, sizeof keys);
    for (int k = 0; k < w->nsmooth; k++) {
        gp_scroll_state *st = gp_scroll_state_get(&w->scrolls, keys[k], false);
        if (!st || !st->smooth) continue;
        gp_el *chain[MAX_DEPTH];
        int len = 0;
        if (!chain_to_key(w->tree, keys[k], chain, 0, &len)) { st->smooth = false; continue; }   /* gone from the tree */
        gp_el *n = chain[len - 1];
        double base = n->scroll_base_y, base_x = n->scroll_base_x;
        double tx = fmin(fmax(st->tx, 0), n->scroll_max.x), ty = fmin(fmax(st->ty, 0), n->scroll_max.y);   /* (the content may have shrunk) */
        double cx = base_x + n->scroll.x, cy = base + n->scroll.y;
        cx = fabs(tx - cx) < 0.5 ? tx : cx + (tx - cx) * alpha;
        cy = fabs(ty - cy) < 0.5 ? ty : cy + (ty - cy) * alpha;
        bool done = cx == tx && cy == ty;
        n->scroll.x = (float)(cx - base_x);
        n->scroll.y = (float)(cy - base);
        store_offset(w, n);   /* (for a list this builds the rows now in view, so nothing below `n` in `chain` may be used) */
        moved_in_place(chain, len - 1);
        st = gp_scroll_state_get(&w->scrolls, keys[k], false);
        st->tx = (float)tx;
        st->ty = (float)ty;
        st->smooth = !done;
        if (!done) w->smooth_keys[keep++] = keys[k];
    }
    w->nsmooth = keep;
    if (keep && !w->fade_job_pending) gp_exec_window_frame(w->app, w, GP_WAKE_PAINT);
    else if (!keep) w->smooth_last_ns = 0;
}

void gp_set_smooth_scrolling(gp_app *app, float seconds) { app->smooth_scroll_s = seconds > 0 ? seconds : 0; }

/* ---------------------------------------------------------------- scroll into view */

static bool chain_to_focus(gp_el *n, uint64_t id, gp_el **chain, int depth, int *len) {
    if (depth >= MAX_DEPTH) return false;
    chain[depth] = n;
    if (n->focus_id == id) { *len = depth + 1; return true; }
    for (uint32_t i = 0; i < n->nchildren; i++)
        if (chain_to_focus(n->children[i], id, chain, depth + 1, len)) return true;
    return false;
}

/* How far a span [top, top + size) must move to be inside [lo, hi): the nearest way, none if it already is or covers the view. */
static float reveal_delta(float top, float size, float lo, float hi) {
    float bottom = top + size;
    if (top >= lo && bottom <= hi) return 0;
    if (top < lo && bottom > hi) return 0;   /* larger than the view and filling it */
    if (top < lo) return top - lo;
    return fminf(bottom - hi, top - lo);     /* (never past its own top edge) */
}

bool gp_scroll_reveal(gp_window *w, uint64_t focus_id) {
    if (!w->tree || !focus_id) return false;
    gp_el *chain[MAX_DEPTH];
    int len = 0;
    if (!chain_to_focus(w->tree, focus_id, chain, 0, &len)) return false;   /* (a row scrolled out of a list is not there) */
    const gp_el *el = chain[len - 1];

    /* Innermost scroller first: moving it moves the element in the next one's space. The new offsets are worked out on copies, and
     * only then written, because writing a list's offset builds its rows again, and `chain` below that list is gone after that. */
    gp_point off[MAX_DEPTH];
    bool changed[MAX_DEPTH] = { false };
    for (int j = 0; j < len; j++) off[j] = chain[j]->scroll;
    bool any = false;
    for (int i = len - 2; i >= 0; i--) {
        gp_el *s = chain[i];
        if (!is_scroller(s)) continue;
        float x = 0, y = 0;   /* the element's top left in the space s scrolls (what its children's `pos` are relative to) */
        for (int j = i + 1; j < len; j++) {
            x += chain[j]->pos.x;
            y += chain[j]->pos.y;
            if (j < len - 1) { x -= off[j].x; y -= off[j].y; }
        }
        float bw = s->style.border_w;
        if (s->style.overflow_y_scroll) {
            float d = reveal_delta(y - off[i].y, el->size.h, bw, s->size.h - bw);
            if (consume(&off[i].y, s->scroll_base_y, s->scroll_max.y, &d)) changed[i] = any = true;
        }
        if (s->style.overflow_x_scroll) {
            float d = reveal_delta(x - off[i].x, el->size.w, bw, s->size.w - bw);
            if (consume(&off[i].x, s->scroll_base_x, s->scroll_max.x, &d)) changed[i] = any = true;
        }
    }
    if (!any) return false;
    /* Innermost first again: storing a list's offset builds its rows anew, which leaves nothing valid below it, but the elements above
     * (all that moved_in_place looks at) stay. */
    for (int i = len - 2; i >= 0; i--) {
        if (!changed[i]) continue;
        chain[i]->scroll = off[i];
        store_offset(w, chain[i]);
        moved_in_place(chain, i);
    }
    w->dirty_paint = true;
    return true;
}

/* ---------------------------------------------------------------- thumb dragging */

typedef struct { gp_el *n; int axis; float ox, oy; gp_thumb t; int page; gp_point q; } thumb_hit;   /* q: the point in the scroller's own coordinates */   /* page: -1 / 1 for a press on the track before / after the thumb, 0 on the thumb */

static bool thumb_rec(gp_el *n, float ox, float oy, rect clip, gp_point p, thumb_hit *out, gp_affine fwd, bool moved) {
    float x = ox + n->pos.x, y = oy + n->pos.y;
    rect b = { x, y, x + n->size.w, y + n->size.h };
    gp_point q = enter(n, x, y, p, &fwd, &moved);
    rect cc = clip;
    if (n->style.overflow_hidden || is_scroller(n)) cc = clip_to(clip, b, fwd, moved);
    if (is_scroller(n) && rect_has(cc, p) && rect_has(b, q)) {
        /* a container's own thumbs paint after its children, so they are on top */
        for (int axis = 1; axis >= 0; axis--) {
            gp_thumb t;
            if (!gp_scroll_thumb(n, axis, x, y, &t)) continue;
            rect r = { t.x, t.y, t.x + t.w, t.y + t.h };
            if (axis == 1) r.x0 -= THUMB_GRAB_PAD; else r.y0 -= THUMB_GRAB_PAD;
            if (rect_has(r, q)) {
                *out = (thumb_hit){ n, axis, x, y, t, 0, q };
                return true;
            }
            /* the rest of the track, over its whole length, on the same strip */
            rect tr = axis == 1 ? (rect){ r.x0, t.track_start, r.x1, t.track_start + t.track_len}
                                : (rect){ t.track_start, r.y0, t.track_start + t.track_len, r.y1 };
            if (rect_has(tr, q)) {
                float mouse = axis == 1 ? q.y : q.x, start = axis == 1 ? t.y : t.x;
                *out = (thumb_hit){ n, axis, x, y, t, mouse < start ? -1 : 1, q };
                return true;
            }
        }
    }
    for (uint32_t i = n->nchildren; i-- > 0;)
        if (thumb_rec(n->children[i], x - n->scroll.x, y - n->scroll.y, cc, p, out, fwd, moved)) return true;
    return false;
}

static bool chain_to_key(gp_el *n, uint64_t key, gp_el **chain, int depth, int *len);
static gp_el *find_by_key(gp_el *n, uint64_t key, float ox, float oy, float *fx, float *fy, gp_affine *inv);

/* One page along `axis` toward `dir` (-1 / 1); false if the scroller is already at that end. */
static bool page_scroller(gp_window *w, gp_el *n, int axis, int dir) {
    float view = axis == 1 ? n->size.h - 2 * n->style.border_w : n->size.w - 2 * n->style.border_w;
    float d = (float)dir * view * TRACK_PAGE;
    bool moved = axis == 1 ? consume(&n->scroll.y, n->scroll_base_y, n->scroll_max.y, &d)
                           : consume(&n->scroll.x, n->scroll_base_x, n->scroll_max.x, &d);
    if (!moved) return false;
    store_offset(w, n);
    gp_el *chain[MAX_DEPTH];
    int len = 0;
    if (chain_to_key(w->tree, n->scroll_key, chain, 0, &len)) moved_in_place(chain, len - 1);
    w->dirty_paint = true;
    return true;
}

/* The next repeat is due in `ns`; a window timer wakes the window for it. */
static void repeat_in(gp_window *w, int64_t ns) {
    w->page_due_ns = gp_now_ns() + ns;
    gp_cancel(w->app, (gp_task){ w->page_timer });
    w->page_timer = gp_exec_window_at(w->app, w, w->page_due_ns, GP_WAKE_PAINT);
}

void gp_scroll_stop_repeat(gp_window *w) {
    if (!w->page_key) return;
    w->page_key = 0;
    gp_cancel(w->app, (gp_task){ w->page_timer });
    w->page_timer = 0;
}

void gp_scroll_track_repeat(gp_window *w) {
    if (!w->page_key) return;
    if (!w->tree || gp_now_ns() < w->page_due_ns) return;
    float ox, oy;
    gp_affine inv;
    gp_el *n = find_by_key(w->tree, w->page_key, 0, 0, &ox, &oy, &inv);
    gp_thumb t;
    if (!n || !gp_scroll_thumb(n, w->page_axis, ox, oy, &t)) { gp_scroll_stop_repeat(w); return; }
    /* toward the pointer, until the thumb is under it */
    gp_point mq = gp_aff_apply(inv, w->mouse);
    float m = w->page_axis == 1 ? mq.y : mq.x, start = w->page_axis == 1 ? t.y : t.x, len = w->page_axis == 1 ? t.h : t.w;
    if (m >= start && m <= start + len) { gp_scroll_stop_repeat(w); return; }
    if (!page_scroller(w, n, w->page_axis, m < start ? -1 : 1)) { gp_scroll_stop_repeat(w); return; }
    repeat_in(w, TRACK_REPEAT_NS);
}

bool gp_scroll_begin_drag(gp_window *w, gp_point p) {
    thumb_hit hit;
    rect all = { -1e9f, -1e9f, 1e9f, 1e9f };
    if (!w->tree || !thumb_rec(w->tree, 0, 0, all, p, &hit, gp_aff_identity(), false)) return false;
    if (hit.page) {   /* a press on the track pages toward it, by most of a view, and goes on while the button is held */
        if (page_scroller(w, hit.n, hit.axis, hit.page)) {
            w->page_key = hit.n->scroll_key;
            w->page_axis = hit.axis;
            repeat_in(w, TRACK_REPEAT_FIRST_NS);
        }
        return true;
    }
    w->drag_key = hit.n->scroll_key;
    w->drag_axis = hit.axis;
    float start = hit.axis == 1 ? hit.t.y : hit.t.x;
    float mouse = hit.axis == 1 ? hit.q.y : hit.q.x;
    /* grabbing outside the thumb itself (the padded hit area) pins the grab to its edge */
    w->drag_grab = fminf(fmaxf(mouse - start, 0), hit.t.thumb_len);
    w->dirty_paint = true;
    return true;
}

/* The scroller with this key, where its box is, and (`inv`) the map from the window to its coordinates: the inverse of the transforms above it, if any. */
static gp_el *find_rec(gp_el *n, uint64_t key, float ox, float oy, float *fx, float *fy, gp_affine fwd, bool moved, gp_affine *inv) {
    float x = ox + n->pos.x, y = oy + n->pos.y;
    if (n->style.has_xf) {
        fwd = gp_aff_mul(moved ? fwd : gp_aff_identity(), gp_style_affine(&n->style, x, y, n->size.w, n->size.h));
        moved = true;
    }
    if (is_scroller(n) && n->scroll_key == key) {
        *fx = x; *fy = y;
        if (inv) *inv = moved ? gp_aff_inverse(fwd) : gp_aff_identity();
        return n;
    }
    for (uint32_t i = 0; i < n->nchildren; i++) {
        gp_el *r = find_rec(n->children[i], key, x - n->scroll.x, y - n->scroll.y, fx, fy, fwd, moved, inv);
        if (r) return r;
    }
    return NULL;
}
static gp_el *find_by_key(gp_el *n, uint64_t key, float ox, float oy, float *fx, float *fy, gp_affine *inv) {
    return find_rec(n, key, ox, oy, fx, fy, gp_aff_identity(), false, inv);
}

/* The elements from the root down to the scroller with this key (`chain[0..len)`), or false. */
static bool chain_to_key(gp_el *n, uint64_t key, gp_el **chain, int depth, int *len) {
    if (depth >= MAX_DEPTH) return false;
    chain[depth] = n;
    if (is_scroller(n) && n->scroll_key == key) { *len = depth + 1; return true; }
    for (uint32_t i = 0; i < n->nchildren; i++)
        if (chain_to_key(n->children[i], key, chain, depth + 1, len)) return true;
    return false;
}

void gp_scroll_drag_to(gp_window *w, gp_point p) {
    float ox, oy;
    gp_affine inv;
    gp_el *n = w->tree ? find_by_key(w->tree, w->drag_key, 0, 0, &ox, &oy, &inv) : NULL;
    gp_thumb t;
    if (!n || !gp_scroll_thumb(n, w->drag_axis, ox, oy, &t)) return;
    gp_point q = gp_aff_apply(inv, p);   /* (along the track as the scroller sees it: under a turned ancestor the drag turns with it) */
    float mouse = w->drag_axis == 1 ? q.y : q.x;
    float range = t.track_len - t.thumb_len;
    float frac = range > 0 ? fminf(fmaxf((mouse - w->drag_grab - t.track_start) / range, 0), 1) : 0;
    if (w->drag_axis == 1) n->scroll.y = (float)((double)frac * t.max - n->scroll_base_y);
    else n->scroll.x = (float)((double)frac * t.max - n->scroll_base_x);
    store_offset(w, n);
    gp_el *chain[MAX_DEPTH];
    int len = 0;
    if (chain_to_key(w->tree, w->drag_key, chain, 0, &len)) moved_in_place(chain, len - 1);
    w->dirty_paint = true;
}

/* ---------------------------------------------------------------- handle api */

static gp_window *ctx_window(gp_ctx *cx) {
    if (cx->window) return cx->window;
    return cx->app->nwindows ? cx->app->windows[0] : NULL;
}

gp_scroll gp_scroll_new(gp_app *app) {
    return (gp_scroll){ GP_SCROLL_EXPLICIT | ++app->next_scroll_id };
}

gp_point gp_scroll_offset(gp_ctx *cx, gp_scroll h) {
    gp_window *w = ctx_window(cx);
    gp_scroll_state *st = w ? gp_scroll_state_get(&w->scrolls, h.id, false) : NULL;
    return st ? (gp_point){ st->x, st->y } : (gp_point){ 0, 0 };
}

gp_point gp_scroll_max(gp_ctx *cx, gp_scroll h) {
    gp_window *w = ctx_window(cx);
    gp_scroll_state *st = w ? gp_scroll_state_get(&w->scrolls, h.id, false) : NULL;
    return st ? (gp_point){ st->max_x, st->max_y } : (gp_point){ 0, 0 };
}

gp_size gp_scroll_viewport(gp_ctx *cx, gp_scroll h) {
    gp_window *w = ctx_window(cx);
    gp_scroll_state *st = w ? gp_scroll_state_get(&w->scrolls, h.id, false) : NULL;
    return st ? (gp_size){ st->view_w, st->view_h } : (gp_size){ 0, 0 };
}

void gp_scroll_to(gp_ctx *cx, gp_scroll h, float x, float y) {
    gp_window *w = ctx_window(cx);
    if (!w) return;
    gp_scroll_state *st = gp_scroll_state_get(&w->scrolls, h.id, true);
    st->x = x;
    st->y = y;
    st->smooth = false;
    st->activity_ns = gp_now_ns();
    w->scrolls.ext_gen++;   /* cached views with scrollers lay out again */
    w->dirty_render = true; /* layout clamps against the real extent */
}

void gp_scroll_by(gp_ctx *cx, gp_scroll h, float dx, float dy) {
    gp_point o = gp_scroll_offset(cx, h);
    gp_scroll_to(cx, h, o.x + dx, o.y + dy);
}

void gp_scroll_into_view(gp_ctx *cx, gp_focus f) {
    gp_window *w = ctx_window(cx);
    if (!w || !f.id) return;
    w->reveal_id = f.id;
    w->dirty_paint = true;   /* a draw happens, and after its layout the scrollers move */
}

void gp_scroll_to_end(gp_ctx *cx, gp_scroll h) {
    gp_point o = gp_scroll_offset(cx, h);
    gp_window *w = ctx_window(cx);
    gp_scroll_state *st = w ? gp_scroll_state_get(&w->scrolls, h.id, false) : NULL;
    if (st && st->horiz) gp_scroll_to(cx, h, 1e30f, o.y);   /* a strip's end is its right */
    else gp_scroll_to(cx, h, o.x, 1e30f);
}

void gp_list_scroll_to_item(gp_ctx *cx, gp_scroll h, size_t index, int align) {
    gp_window *w = ctx_window(cx);
    gp_scroll_state *st = w ? gp_scroll_state_get(&w->scrolls, h.id, false) : NULL;
    if (st && st->vl) {   /* gp_list: the next layout knows the heights, so it does the move */
        st->vl->want = true;
        st->vl->want_idx = index;
        st->vl->want_align = align;
        w->scrolls.ext_gen++;
        w->dirty_render = true;
        return;
    }
    if (!st || st->item_h <= 0) return; /* not laid out yet */
    /* along the list's axis: y for a column of rows, x for a strip (gp_list_horizontal) */
    float cur = st->horiz ? st->x : st->y, view = st->horiz ? st->view_w : st->view_h;
    float top = st->pad_top + (float)index * st->item_h, bottom = top + st->item_h;
    float pos = cur;
    switch (align) {
    case GP_ALIGN_ROW_START: pos = top - st->pad_top; break;
    case GP_ALIGN_ROW_CENTER: pos = top - (view - st->item_h) / 2; break;
    default:
        if (top - st->pad_top < cur) pos = top - st->pad_top;
        else if (bottom > cur + view) pos = bottom - view;
        break;
    }
    if (st->horiz) gp_scroll_to(cx, h, pos, st->y);
    else gp_scroll_to(cx, h, st->x, pos);
}

/* ---------------------------------------------------------------- gp_list bookkeeping */

static gp_vlist *ctx_vlist(gp_ctx *cx, gp_scroll h, gp_window **w_out) {
    gp_window *w = ctx_window(cx);
    gp_scroll_state *st = w ? gp_scroll_state_get(&w->scrolls, h.id, false) : NULL;
    if (!st || !st->vl) return NULL;
    *w_out = w;
    return st->vl;
}

void gp_list_splice(gp_ctx *cx, gp_scroll h, size_t start, size_t old_count, size_t new_count) {
    gp_window *w;
    gp_vlist *vl = ctx_vlist(cx, h, &w);
    if (!vl) return;
    gp_vlist_splice(vl, start, old_count, new_count);
    w->scrolls.ext_gen++;
    w->dirty_render = true;
}

void gp_list_invalidate(gp_ctx *cx, gp_scroll h, size_t start, size_t count) {
    gp_window *w;
    gp_vlist *vl = ctx_vlist(cx, h, &w);
    if (!vl) return;
    gp_vlist_forget(vl, start, count);
    w->scrolls.ext_gen++;
    w->dirty_render = true;
}

void gp_list_reset(gp_ctx *cx, gp_scroll h) {
    gp_window *w;
    gp_vlist *vl = ctx_vlist(cx, h, &w);
    if (!vl) return;
    gp_vlist_clear(vl);
    w->scrolls.ext_gen++;
    w->dirty_render = true;
}

/* ---------------------------------------------------------------- auto-hiding scrollbars */

/* A scrollbar made with gp_scrollbar_autohide is shown while its scroller is being scrolled or the mouse is over
 * it (or its thumb is being dragged), for HOLD after the last of that, and then fades out over FADE. Nothing
 * here runs on a timer of its own: this is called before every paint, works out how visible each one is now,
 * and, if something is still going to change, asks for a frame (while fading) or a wake-up (when the hold
 * ends). A scrollbar that is shown steadily or hidden asks for nothing. */
#define AUTOHIDE_HOLD_NS 1000000000ll
#define AUTOHIDE_FADE_NS 250000000ll

typedef struct {
    gp_window *w;
    int64_t now;
    bool changed, fading;
    int64_t deadline;   /* earliest end of a hold, 0 = none */
} hide_walk_ctx;

static void hide_one(hide_walk_ctx *h, gp_el *n, rect box, rect clip) {
    gp_window *w = h->w;
    gp_scroll_state *st = gp_scroll_state_get(&w->scrolls, n->scroll_key, false);
    if (!st) return;
    bool hovered = w->mouse_in && rect_has(clip, w->mouse) && rect_has(box, w->mouse);
    bool held = hovered || w->drag_key == n->scroll_key;
    if (held) st->activity_ns = h->now;
    float alpha = 0;
    if (st->activity_ns) {
        int64_t idle = h->now - st->activity_ns;
        if (idle < AUTOHIDE_HOLD_NS) {
            alpha = 1;
            if (!held) {
                int64_t end = st->activity_ns + AUTOHIDE_HOLD_NS;
                if (!h->deadline || end < h->deadline) h->deadline = end;
            }
        } else if (idle < AUTOHIDE_HOLD_NS + AUTOHIDE_FADE_NS) {
            alpha = 1.f - (float)(idle - AUTOHIDE_HOLD_NS) / (float)AUTOHIDE_FADE_NS;
            h->fading = true;
        }
    }
    if (alpha != n->thumb_alpha) {
        n->thumb_alpha = alpha;
        h->changed = true;
    }
}

static void hide_walk(hide_walk_ctx *h, gp_el *n, float ox, float oy, rect clip) {
    if (!n->hide_below) return;
    float x = ox + n->pos.x, y = oy + n->pos.y;
    rect b = { x, y, x + n->size.w, y + n->size.h };
    rect cc = clip;
    bool scroller = is_scroller(n);
    if (n->style.overflow_hidden || scroller)
        cc = (rect){ fmaxf(clip.x0, b.x0), fmaxf(clip.y0, b.y0), fminf(clip.x1, b.x1), fminf(clip.y1, b.y1) };
    if (scroller && n->style.autohide_scrollbar) hide_one(h, n, b, cc);
    for (uint32_t i = 0; i < n->nchildren; i++)
        hide_walk(h, n->children[i], x - n->scroll.x, y - n->scroll.y, cc);
}

void gp_scroll_autohide_update(gp_window *w) {
    if (!w->tree || !w->tree->hide_below) return;
    hide_walk_ctx h = { .w = w, .now = gp_now_ns() };
    rect all = { -1e9f, -1e9f, 1e9f, 1e9f };
    hide_walk(&h, w->tree, 0, 0, all);
    if (h.changed) w->scroll_vis_gen++;
    if (h.fading && !w->fade_job_pending) gp_exec_window_frame(w->app, w, GP_WAKE_PAINT);
    if (h.deadline && (h.deadline != w->autohide_timer_due || !gp_task_active(w->app, (gp_task){ w->autohide_timer }))) {
        gp_cancel(w->app, (gp_task){ w->autohide_timer });
        w->autohide_timer = gp_exec_window_at(w->app, w, h.deadline, GP_WAKE_PAINT);
        w->autohide_timer_due = h.deadline;
    }
}
