/*
 * Keyboard input: focus, keymap and action dispatch (GPUI's model).
 *
 * A key press is resolved against the *focus path*: the chain of elements from
 * the window's root down to the focused element (just the root if nothing is
 * focused). Key contexts declared along that path select which bindings apply;
 * the resulting action bubbles from the focused element up through on_action
 * handlers, then to global handlers. Unhandled keys go to on_key_down handlers
 * along the same path. Tab / shift-tab cycle focus if nothing else took them.
 */
#include "internal.h"

#define MAX_PATH 128

/* ---------------------------------------------------------------- keystrokes */

static bool parse_mod(const char *tok, size_t len, uint32_t *mod) {
    struct { const char *name; uint32_t mod; } names[] = {
        { "ctrl", GP_MOD_CTRL }, { "control", GP_MOD_CTRL }, { "alt", GP_MOD_ALT },
        { "shift", GP_MOD_SHIFT }, { "super", GP_MOD_SUPER }, { "cmd", GP_MOD_SUPER },
        { "meta", GP_MOD_SUPER },
    };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        if (strlen(names[i].name) == len && strncmp(tok, names[i].name, len) == 0) {
            *mod = names[i].mod;
            return true;
        }
    }
    return false;
}

bool gp_parse_keystroke(const char *s, gp_keystroke *out) {
    memset(out, 0, sizeof *out);
    const char *p = s;
    for (;;) {
        const char *dash = strchr(p, '-');
        /* a '-' at the very start (or end) of what remains is the key itself */
        if (!dash || dash == p || dash[1] == '\0') break;
        uint32_t m;
        if (!parse_mod(p, (size_t)(dash - p), &m)) return false;
        out->mods |= m;
        p = dash + 1;
    }
    size_t n = strlen(p);
    if (n == 0 || n >= sizeof out->key) return false;
    for (size_t i = 0; i < n; i++) out->key[i] = (char)(p[i] >= 'A' && p[i] <= 'Z' ? p[i] + 32 : p[i]);
    return true;
}

/* ---------------------------------------------------------------- keymap */

bool gp_bind_key_with(gp_app *app, const char *keystroke, gp_action action, const char *context, const void *payload, size_t size) {
    gp_binding b = { 0 };
    const char *p = keystroke;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *end = strchr(p, ' ');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        char tok[32] = { 0 };
        if (n >= sizeof tok || b.nks == GP_MAX_CHORD || (memcpy(tok, p, n), !gp_parse_keystroke(tok, &b.ks[b.nks]))) {
            fprintf(stderr, "gpuic: cannot parse keystroke \"%s\"\n", keystroke);
            return false;
        }
        b.nks++;
        p += n;
    }
    if (!b.nks) {
        fprintf(stderr, "gpuic: cannot parse keystroke \"%s\"\n", keystroke);
        return false;
    }
    if (app->nbindings == app->cap_bindings) {
        app->cap_bindings = app->cap_bindings ? app->cap_bindings * 2 : 32;
        app->bindings = realloc(app->bindings, app->cap_bindings * sizeof *app->bindings);
    }
    b.action = strdup(action);
    b.context = context && *context ? strdup(context) : NULL;
    if (payload && size) {
        b.payload = malloc(size);
        memcpy(b.payload, payload, size);
        b.payload_size = size;
    }
    app->bindings[app->nbindings++] = b;
    return true;
}

bool gp_bind_key(gp_app *app, const char *keystroke, gp_action action, const char *context) {
    return gp_bind_key_with(app, keystroke, action, context, NULL, 0);
}

void gp_clear_key_bindings(gp_app *app) {
    for (size_t i = 0; i < app->nbindings; i++) {
        free(app->bindings[i].action);
        free(app->bindings[i].context);
        free(app->bindings[i].payload);
    }
    app->nbindings = 0;
}

void gp_on_global_action(gp_app *app, gp_action action, gp_action_fn fn, void *data) {
    if (app->nglobals == app->cap_globals) {
        app->cap_globals = app->cap_globals ? app->cap_globals * 2 : 8;
        app->globals = realloc(app->globals, app->cap_globals * sizeof *app->globals);
    }
    app->globals[app->nglobals++] = (gp_global_action){ strdup(action), fn, data };
}

void gp_input_free(gp_app *app) {
    gp_clear_key_bindings(app);
    free(app->bindings);
    for (size_t i = 0; i < app->nglobals; i++) free(app->globals[i].action);
    free(app->globals);
}

/* Does `pred` ("A && !B") hold for the context stack? Returns the depth of the
 * deepest context it matched positively (-1 if it only had negations), or -2 if
 * it does not hold. */
static int predicate_depth(const char *pred, gp_el **path, int n) {
    int depth = -1;
    const char *p = pred;
    while (*p) {
        while (*p == ' ' || *p == '&') p++;
        if (!*p) break;
        bool neg = *p == '!';
        if (neg) p++;
        const char *start = p;
        while (*p && *p != ' ' && *p != '&') p++;
        size_t len = (size_t)(p - start);
        int found = -1;
        for (int i = n - 1; i >= 0; i--) {
            const char *c = path[i]->key_context;
            if (c && strlen(c) == len && strncmp(c, start, len) == 0) { found = i; break; }
        }
        if (neg) {
            if (found >= 0) return -2;
        } else {
            if (found < 0) return -2;
            if (found > depth) depth = found;
        }
    }
    return depth;
}

static bool seq_matches(const gp_binding *b, const gp_keystroke *seq, int n) {
    for (int i = 0; i < n; i++)
        if (b->ks[i].mods != seq[i].mods || strcmp(b->ks[i].key, seq[i].key) != 0) return false;
    return true;
}

/* The binding for exactly this sequence of keystrokes that the focus path's contexts select (deepest context wins, later bindings win
 * ties), and whether a longer binding that begins with it applies too (the sequence is then the start of a chord). */
static const gp_binding *find_binding(gp_app *app, const gp_keystroke *seq, int n, gp_el **path, int pn, bool *longer) {
    const gp_binding *best = NULL;
    int best_depth = -3;
    *longer = false;
    for (size_t i = 0; i < app->nbindings; i++) {
        const gp_binding *b = &app->bindings[i];
        if (b->nks < n || !seq_matches(b, seq, n)) continue;
        int d = b->context ? predicate_depth(b->context, path, pn) : -1;
        if (d == -2) continue;
        if (b->nks > n) { *longer = true; continue; }
        if (!b->context) d = -1;
        if (d >= best_depth) { /* >= so later bindings win ties */
            best = b;
            best_depth = d;
        }
    }
    return best;
}

static bool path_rec(gp_el *n, uint64_t id, gp_el **out, int depth, int *len) {
    if (depth >= MAX_PATH) return false;
    out[depth] = n;
    if (n->focus_id == id) { *len = depth + 1; return true; }
    for (uint32_t i = 0; i < n->nchildren; i++)
        if (path_rec(n->children[i], id, out, depth + 1, len)) return true;
    return false;
}

/* Fills `out` with root..focused. Without a (live) focus it is just the root. */
static int focus_path(gp_window *w, gp_el **out) {
    int len = 0;
    if (!w->tree) return 0;
    if (w->focused && path_rec(w->tree, w->focused, out, 0, &len)) return len;
    out[0] = w->tree;
    return 1;
}

static gp_el *focused_el(gp_window *w) {
    gp_el *path[MAX_PATH];
    if (!w->focused) return NULL;
    int n = focus_path(w, path);
    return n && path[n - 1]->focus_id == w->focused ? path[n - 1] : NULL;
}

/* ---------------------------------------------------------------- dispatch */

static bool dispatch_action(gp_window *w, gp_el **path, int n, const char *name, const void *payload, size_t payload_size) {
    gp_app *app = w->app;
    const void *saved = app->action_payload;   /* a handler may dispatch another action */
    size_t saved_size = app->action_payload_size;
    app->action_payload = payload;
    app->action_payload_size = payload_size;
    bool taken = false;
    for (int i = n - 1; i >= 0 && !taken; i--) {
        for (gp_action_handler *h = path[i]->actions; h; h = h->next) {
            if (strcmp(h->name, name) != 0) continue;
            void *state = gp_entity_get(app, h->owner);
            if (!state) continue;
            gp_ctx cx = { app, w, h->owner, false };
            h->fn(state, &cx, h->data);
            if (!cx.propagate) { taken = true; break; }
        }
    }
    for (size_t i = 0; i < app->nglobals && !taken; i++) {
        if (strcmp(app->globals[i].action, name) != 0) continue;
        gp_ctx cx = { app, w, GP_ENTITY_NULL, false };
        app->globals[i].fn(NULL, &cx, app->globals[i].data);
        if (!cx.propagate) taken = true;
    }
    app->action_payload = saved;
    app->action_payload_size = saved_size;
    return taken;
}

bool gp_dispatch_action_with(gp_ctx *cx, gp_action action, const void *payload, size_t size) {
    gp_window *w = cx->window ? cx->window : (cx->app->nwindows ? cx->app->windows[0] : NULL);
    if (!w) return false;
    gp_el *path[MAX_PATH];
    int n = focus_path(w, path);
    return n && dispatch_action(w, path, n, action, payload, size);
}

bool gp_dispatch_action(gp_ctx *cx, gp_action action) { return gp_dispatch_action_with(cx, action, NULL, 0); }

const void *gp_action_payload(gp_ctx *cx, size_t *size) {
    if (size) *size = cx->app->action_payload ? cx->app->action_payload_size : 0;
    return cx->app->action_payload;
}

/* The chord typed so far, as text. */
const char *gp_keys_pending(gp_ctx *cx) {
    static char buf[160];
    gp_window *w = cx->window ? cx->window : (cx->app->nwindows ? cx->app->windows[0] : NULL);
    buf[0] = 0;
    if (!w) return buf;
    size_t o = 0;
    for (int i = 0; i < w->npending; i++) {
        const gp_keystroke *k = &w->pending[i];
        o += (size_t)snprintf(buf + o, sizeof buf - o, "%s%s%s%s%s%s", o ? " " : "", k->mods & GP_MOD_CTRL ? "ctrl-" : "", k->mods & GP_MOD_ALT ? "alt-" : "",
                              k->mods & GP_MOD_SHIFT ? "shift-" : "", k->mods & GP_MOD_SUPER ? "super-" : "", k->key);
        if (o >= sizeof buf) break;
    }
    return buf;
}

void gp_input_cancel_chord(gp_window *w) { w->npending = 0; }

bool gp_input_key_down(gp_window *w, const gp_key_event *ev) {
    gp_el *path[MAX_PATH];
    int n = focus_path(w, path);
    if (!n) return false;

    /* the keystrokes typed so far (a chord waiting for its next key, if it has not been forgotten) and this one */
    if (w->npending && gp_now_ns() - w->pending_ns > GP_CHORD_TIMEOUT_NS) w->npending = 0;
    bool was_pending = w->npending > 0;
    if (was_pending && strcmp(ev->key, "escape") == 0 && ev->mods == 0) { w->npending = 0; return true; }
    gp_keystroke seq[GP_MAX_CHORD];
    int ns = w->npending;
    memcpy(seq, w->pending, (size_t)ns * sizeof *seq);
    if (ns < GP_MAX_CHORD) {
        snprintf(seq[ns].key, sizeof seq[ns].key, "%s", ev->key);
        seq[ns].mods = ev->mods;
        ns++;
    }
    bool longer;
    const gp_binding *b = find_binding(w->app, seq, ns, path, n, &longer);
    if (b) {
        w->npending = 0;
        bool taken = dispatch_action(w, path, n, b->action, b->payload, b->payload_size);
        if (taken || was_pending) return true;   /* a finished chord is consumed even if nobody wanted its action */
    } else if (longer && ns < GP_MAX_CHORD) {
        memcpy(w->pending, seq, (size_t)ns * sizeof *seq);
        w->npending = ns;
        w->pending_ns = gp_now_ns();
        return true;
    } else if (was_pending) {
        w->npending = 0;   /* this key does not continue the chord: the chord is dropped, and the key with it */
        return true;
    }

    for (int i = n - 1; i >= 0; i--) {
        gp_handler *h = &path[i]->on_key_down;
        void *state = h->fn ? gp_entity_get(w->app, h->owner) : NULL;
        if (!state) continue;
        gp_ctx cx = { w->app, w, h->owner, false };
        ((gp_key_fn)h->fn)(state, &cx, ev, h->data);
        if (!cx.propagate) return true;
    }

    if (strcmp(ev->key, "tab") == 0 && (ev->mods & ~GP_MOD_SHIFT) == 0) {
        gp_input_focus_cycle(w, ev->mods & GP_MOD_SHIFT ? -1 : 1);
        return true;
    }
    return false;
}

bool gp_input_key_up(gp_window *w, const gp_key_event *ev) {
    gp_el *path[MAX_PATH];
    int n = focus_path(w, path);
    for (int i = n - 1; i >= 0; i--) {
        gp_handler *h = &path[i]->on_key_up;
        void *state = h->fn ? gp_entity_get(w->app, h->owner) : NULL;
        if (!state) continue;
        gp_ctx cx = { w->app, w, h->owner, false };
        ((gp_key_fn)h->fn)(state, &cx, ev, h->data);
        if (!cx.propagate) return true;
    }
    return false;
}

void gp_input_text(gp_window *w, const char *utf8) {
    gp_el *path[MAX_PATH];
    int n = focus_path(w, path);
    for (int i = n - 1; i >= 0; i--) {
        gp_handler *h = &path[i]->on_text;
        void *state = h->fn ? gp_entity_get(w->app, h->owner) : NULL;
        if (!state) continue;
        gp_ctx cx = { w->app, w, h->owner, false };
        ((gp_text_input_fn)h->fn)(state, &cx, utf8, h->data);
        if (!cx.propagate) return;
    }
}

void gp_input_ime(gp_window *w, const char *preedit, int cursor) {
    gp_el *path[MAX_PATH];
    int n = focus_path(w, path);
    for (int i = n - 1; i >= 0; i--) {
        gp_handler *h = &path[i]->on_ime;
        void *state = h->fn ? gp_entity_get(w->app, h->owner) : NULL;
        if (!state) continue;
        gp_ctx cx = { w->app, w, h->owner, false };
        ((gp_ime_fn)h->fn)(state, &cx, preedit, cursor, h->data);
        if (!cx.propagate) return;
    }
}

/* ---------------------------------------------------------------- focus */

/* Absolute origin of the element with `focus_id` (scroll offsets of ancestors applied). */
static gp_el *find_origin(gp_el *n, uint64_t id, float ox, float oy, float *fx, float *fy) {
    float x = ox + n->pos.x, y = oy + n->pos.y;
    if (n->focus_id == id) { *fx = x; *fy = y; return n; }
    for (uint32_t i = 0; i < n->nchildren; i++) {
        gp_el *r = find_origin(n->children[i], id, x - n->scroll.x, y - n->scroll.y, fx, fy);
        if (r) return r;
    }
    return NULL;
}

/* Tell the platform where the caret is so IME candidate windows appear next to it. */
static void sync_ime_area(gp_window *w) {
    if (!w->text_input_on || !w->focused || !w->tree) return;
    float ox, oy;
    gp_el *f = find_origin(w->tree, w->focused, 0, 0, &ox, &oy);
    if (!f || !f->has_ime_rect) return;
    float r[4] = { ox + f->ime_rect[0], oy + f->ime_rect[1], f->ime_rect[2], f->ime_rect[3] };
    /* Only on change. Updating the rect is a round trip to the compositor on Wayland, and
     * most frames do not move the caret. */
    if (w->ime_rect_set && memcmp(r, w->ime_rect, sizeof r) == 0) return;
    memcpy(w->ime_rect, r, sizeof r);
    w->ime_rect_set = true;
    gp_platform_set_text_input_area(w, r[0], r[1], r[2], r[3], 0);
}

static void sync_text_input(gp_window *w) {
    gp_el *f = focused_el(w);
    bool want = false;
    if (f) {
        gp_el *path[MAX_PATH];
        int n = focus_path(w, path);
        for (int i = n - 1; i >= 0 && !want; i--) want = path[i]->on_text.fn != NULL;
    }
    if (want != w->text_input_on) {
        w->text_input_on = want;
        w->ime_rect_set = false; /* a fresh text-input session: tell the platform again */
        gp_platform_set_text_input(w, want);
    }
}

void gp_input_set_focus(gp_window *w, uint64_t id) {
    if (w->focused == id) return;
    w->focused = id;
    w->dirty_render = true;
    gp_views_focus_changed(w); /* views whose render looks at focus render again */
    sync_text_input(w);
}

/* Focus lives in the handle, not the element: it survives the element being absent from
 * the tree (e.g. a row scrolled out of a virtualized list). While absent, keys just go
 * to the root of the focus path. */
void gp_input_after_render(gp_window *w) {
    sync_text_input(w);
    sync_ime_area(w);
}

static void collect_tab_stops(gp_el *n, gp_el ***list, uint32_t *count, uint32_t *cap) {
    if (n->focus_id && n->tab_stop) {
        if (*count == *cap) {
            *cap = *cap ? *cap * 2 : 16;
            *list = realloc(*list, *cap * sizeof **list);
        }
        (*list)[(*count)++] = n;
    }
    for (uint32_t i = 0; i < n->nchildren; i++) collect_tab_stops(n->children[i], list, count, cap);
}

void gp_input_focus_cycle(gp_window *w, int dir) {
    if (!w->tree) return;
    gp_el **stops = NULL;
    uint32_t count = 0, cap = 0;
    collect_tab_stops(w->tree, &stops, &count, &cap);
    if (!count) { free(stops); return; }

    /* stable insertion sort by tab_index */
    for (uint32_t i = 1; i < count; i++) {
        gp_el *e = stops[i];
        uint32_t j = i;
        while (j > 0 && stops[j - 1]->tab_index > e->tab_index) { stops[j] = stops[j - 1]; j--; }
        stops[j] = e;
    }
    int cur = -1;
    for (uint32_t i = 0; i < count; i++)
        if (stops[i]->focus_id == w->focused) cur = (int)i;
    int next = cur < 0 ? (dir > 0 ? 0 : (int)count - 1) : (cur + dir + (int)count) % (int)count;
    gp_input_set_focus(w, stops[next]->focus_id);
    w->reveal_id = stops[next]->focus_id;   /* once laid out, the scrollers around it show it */
    free(stops);
}

/* ---------------------------------------------------------------- public focus api */

static gp_window *ctx_window(gp_ctx *cx) {
    if (cx->window) return cx->window;
    return cx->app->nwindows ? cx->app->windows[0] : NULL;
}

gp_focus gp_focus_new(gp_app *app) { return (gp_focus){ ++app->next_focus_id }; }

void gp_focus_set(gp_ctx *cx, gp_focus f) {
    gp_window *w = ctx_window(cx);
    if (w) gp_input_set_focus(w, f.id);
}

void gp_blur(gp_ctx *cx) {
    gp_window *w = ctx_window(cx);
    if (w) gp_input_set_focus(w, 0);
}

bool gp_is_focused(gp_ctx *cx, gp_focus f) {
    gp_window *w = ctx_window(cx);
    if (w) gp_view_reads_focus(w, cx->entity);   /* so that a focus change renders this view again, even if cached */
    return w && f.id && w->focused == f.id;
}

void gp_focus_next(gp_ctx *cx) {
    gp_window *w = ctx_window(cx);
    if (w) gp_input_focus_cycle(w, 1);
}

void gp_focus_prev(gp_ctx *cx) {
    gp_window *w = ctx_window(cx);
    if (w) gp_input_focus_cycle(w, -1);
}
