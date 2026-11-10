#include "internal.h"

/* ---------------------------------------------------------------- entities */

gp_entity_slot *gp_slot_for(gp_app *app, gp_entity e) {
    if (e.index >= app->nslots) return NULL;
    gp_entity_slot *s = &app->slots[e.index];
    return s->alive && s->generation == e.generation ? s : NULL;
}

gp_entity gp_view_new(gp_app *app, size_t size, const void *init, gp_render_fn render,
                      gp_drop_fn drop) {
    uint32_t idx;
    if (app->nfree) {
        idx = app->free_slots[--app->nfree];
    } else {
        if (app->nslots == app->cap_slots) {
            app->cap_slots = app->cap_slots ? app->cap_slots * 2 : 64;
            app->slots = realloc(app->slots, app->cap_slots * sizeof *app->slots);
        }
        idx = app->nslots++;
        app->slots[idx] = (gp_entity_slot){ 0 };
    }
    gp_entity_slot *s = &app->slots[idx];
    s->state = calloc(1, size ? size : 1);
    if (init && size) memcpy(s->state, init, size);
    s->size = size;
    s->generation++;
    if (s->generation == 0) s->generation = 1;
    s->render = render;
    s->drop = drop;
    s->alive = true;
    return (gp_entity){ idx, s->generation };
}

gp_entity gp_entity_new(gp_app *app, size_t size, const void *init, gp_drop_fn drop) {
    return gp_view_new(app, size, init, NULL, drop);
}

void *gp_entity_get(gp_app *app, gp_entity e) {
    gp_entity_slot *s = gp_slot_for(app, e);
    return s ? s->state : NULL;
}

void gp_entity_release(gp_app *app, gp_entity e) {
    gp_entity_slot *s = gp_slot_for(app, e);
    if (!s) return;
    /* release observers see the state while it is still valid; they may create or release entities,
     * so no slot pointer is kept across the call */
    gp_subs_entity_released(app, e, s->state);
    s = gp_slot_for(app, e);
    if (!s) return;   /* an observer released it first */
    bool was_view = s->render != NULL;
    s->notify_queued = false;
    if (s->drop) s->drop(s->state);
    free(s->state);
    s->state = NULL;
    s->alive = false;
    if (app->nfree == app->cap_free) {
        app->cap_free = app->cap_free ? app->cap_free * 2 : 64;
        app->free_slots = realloc(app->free_slots, app->cap_free * sizeof *app->free_slots);
    }
    app->free_slots[app->nfree++] = e.index;
    /* Timers, animations and coroutines owned by the entity are cancelled; a coroutine
     * unwinds here, and must not touch `self` once an await has returned false. */
    gp_exec_entity_released(app, e);
    if (was_view) gp_view_dirty(app, e);   /* whoever shows it builds again, without it */
}

/* ---------------------------------------------------------------- notification */

void gp_app_notify(gp_app *app) { gp_views_invalidate_all(app); }

/* The entity changed. If it is a view, it renders again and the windows showing it rebuild (views
 * that are cached and unaffected are reused); anything observing it is called. A ctx with no entity
 * is the app itself: everything is invalidated. */
void gp_notify(gp_ctx *cx) {
    gp_entity_slot *s = gp_slot_for(cx->app, cx->entity);
    if (!s) {
        gp_app_notify(cx->app);
        return;
    }
    if (s->render) gp_view_dirty(cx->app, cx->entity);
    if (!s->sub_target_head || s->notify_queued) return;
    s->notify_queued = true;
    gp_queue_effect(cx->app, false, cx->entity, NULL, NULL, 0);
}

/* ---------------------------------------------------------------- fonts */

bool gp_font_add_file(gp_app *app, const char *path, const gp_font_options *opts) {
    if (!app || !gp_text_add_font_file_as(app->text, path, opts ? opts->family : NULL, opts ? opts->weight : 0, opts ? opts->face_index : 0))
        return false;
    gp_app_notify(app);
    return true;
}

bool gp_font_add_memory(gp_app *app, const void *data, size_t size, const gp_font_options *opts) {
    if (!app || !gp_text_add_font_memory_as(app->text, data, size, opts ? opts->family : NULL, opts ? opts->weight : 0, opts ? opts->face_index : 0))
        return false;
    gp_app_notify(app);
    return true;
}

void gp_fonts_set_text_gamma(gp_app *app, float gamma) {
    if (!app) return;
    gp_text_set_gamma(app->text, gamma);
    for (int i = 0; i < app->nwindows; i++) app->windows[i]->dirty_paint = true;   /* (the scene is the same; the shader tones it differently) */
}

void gp_fonts_set_antialiasing(gp_app *app, int mode) {
    if (!app) return;
    gp_text_set_antialiasing(app->text, mode);
    gp_app_notify(app);
}

void gp_fonts_use_system(gp_app *app, bool enable) {
    if (!app) return;
    gp_text_use_system_fonts(app->text, enable);
    gp_app_notify(app);
}

/* ---------------------------------------------------------------- windows */

gp_window *gp_open_window(gp_app *app, const gp_window_options *opts, gp_entity root_view) {
    if (app->nwindows == GP_MAX_WINDOWS) return NULL;
    gp_window_options o = opts ? *opts : (gp_window_options){ 0 };
    gp_window *w = calloc(1, sizeof *w);
    w->app = app;
    w->root_view = root_view;
    w->background = o.background.a > 0 ? o.background : gp_rgb(0x202020);
    if (!gp_platform_open_window(app, w, &o)) {
        free(w);
        return NULL;
    }
    if (!app->renderer) {
        app->renderer = gp_renderer_new(gp_platform_gl_proc);
        if (!app->renderer) {
            gp_platform_close_window(app, w);
            free(w);
            return NULL;
        }
    }
    w->anims.app = app;
    w->anims.window = w;
    w->dirty_render = true;
    app->windows[app->nwindows++] = w;
    return w;
}

gp_window *gp_app_window_by_id(gp_app *app, uint32_t id) {
    for (int i = 0; i < app->nwindows; i++)
        if (app->windows[i]->id == id) return app->windows[i];
    return NULL;
}

/* GL objects can only be deleted while a context is current, so this runs before the last window goes. */
static void renderer_release(gp_app *app, gp_window *current) {
    if (!app->renderer) return;
    if (current) gp_platform_make_current(current);
    gp_renderer_free(app->renderer, app->gfx);
    app->renderer = NULL;
}

static void window_destroy(gp_app *app, gp_window *w) {
    gp_platform_close_window(app, w);
    gp_arena_free(&w->arena);
    gp_arena_free(&w->scratch);
    gp_views_free(w);
    gp_anim_free(&w->anims);
    free(w->scene.items);
    for (size_t i = 0; i < w->scrolls.len; i++) {
        if (w->scrolls.items[i].arena) gp_arena_free(w->scrolls.items[i].arena);
        free(w->scrolls.items[i].arena);
        if (w->scrolls.items[i].harena) gp_arena_free(w->scrolls.items[i].harena);
        free(w->scrolls.items[i].harena);
        gp_vlist_free(w->scrolls.items[i].vl);
    }
    free(w->scrolls.items);
    free(w->lstates.items);
    free(w->lstates_seen.items);
    free(w);
}

void gp_quit(gp_app *app) { app->quit = true; }

void gp_window_on_close(gp_window *w) { w->closed = true; }
void gp_window_close(gp_window *w) { w->closed = true; }
void gp_window_on_expose(gp_window *w) { w->dirty_paint = true; }

void gp_window_on_resize(gp_window *w) {
    gp_platform_update_size(w);
    w->dirty_render = true;
}

void gp_window_on_mouse_leave(gp_window *w) {
    w->mouse_in = false;
    w->dirty_paint = true;
}

/* ---------------------------------------------------------------- input */

/* `origin` is the absolute position of the element the handler belongs to, for `local`. */
static void fire(gp_window *w, const gp_handler *h, gp_point p, gp_point origin, int button,
                 int clicks, uint32_t mods) {
    void *state = gp_entity_get(w->app, h->owner);
    if (!state || !h->fn) return;
    gp_ctx cx = { w->app, w, h->owner, false };
    gp_mouse_event ev = { p, { p.x - origin.x, p.y - origin.y }, button, clicks, mods };
    ((gp_mouse_fn)h->fn)(state, &cx, &ev, h->data);
}

static void update_cursor(gp_window *w) {
    gp_hit hit;
    int cursor = GP_CURSOR_DEFAULT;
    if (w->mouse_in && gp_hit_test(w->tree, w->mouse, GP_HIT_CURSOR, &hit))
        cursor = hit.el->style.cursor;
    if (cursor != w->cursor) {
        w->cursor = cursor;
        gp_platform_set_cursor(w, cursor);
    }
}

void gp_window_on_mouse_move(gp_window *w, float x, float y) {
    w->mouse = (gp_point){ x, y };
    w->mouse_in = true;
    w->dirty_paint = true;
    if (w->drag_key) gp_scroll_drag_to(w, w->mouse);
    if (w->capture.active) /* a press is held on an element with gp_on_drag */
        fire(w, &w->capture.move, w->mouse, w->capture.origin, GP_MOUSE_LEFT, 0, 0);
    update_cursor(w);
}

/* gp_on_wheel: the innermost handler under the pointer; used up unless it calls gp_propagate, which goes on to the next one */
static bool wheel_handlers(gp_window *w, gp_point p, float dx, float dy, uint32_t mods) {
    uint64_t skip[16];
    for (int n = 0; n < 16; n++) {
        gp_hit hit;
        if (!gp_hit_test_skipping(w->tree, p, GP_HIT_WHEEL, &hit, skip, n)) return false;
        skip[n] = hit.path;
        void *state = gp_entity_get(w->app, hit.el->on_wheel.owner);
        if (!state) continue;
        gp_ctx cx = { w->app, w, hit.el->on_wheel.owner, false };
        gp_wheel_event ev = { p, { hit.qx - hit.ox, hit.qy - hit.oy }, dx, dy, mods };
        ((gp_wheel_fn)hit.el->on_wheel.fn)(state, &cx, &ev, hit.el->on_wheel.data);
        if (!cx.propagate) return true;
    }
    return false;
}

void gp_window_on_scroll(gp_window *w, float x, float y, float dx, float dy, uint32_t mods) {
    w->mouse = (gp_point){ x, y };
    w->mouse_in = true;
    if (w->tree && wheel_handlers(w, w->mouse, dx, dy, mods)) return;
    gp_scroll_wheel(w, w->mouse, dx, dy);
}

void gp_window_on_mouse_button(gp_window *w, float x, float y, int button, bool down, int clicks,
                               uint32_t mods) {
    gp_point p = { x, y };
    w->mouse = p;
    w->mouse_in = true;
    w->dirty_paint = true;
    gp_hit hit;

    if (down) {
        if (button == GP_MOUSE_LEFT && gp_scroll_begin_drag(w, p)) return; /* grabbed a scrollbar */
        if (button == GP_MOUSE_LEFT) {
            w->pressed = true;
            /* click focuses the topmost tracked element, or clears focus */
            gp_input_set_focus(w, gp_hit_test(w->tree, p, GP_HIT_FOCUS, &hit) ? hit.el->focus_id : 0);
        }
        w->down_path = button == GP_MOUSE_LEFT && gp_hit_test(w->tree, p, GP_HIT_CLICK, &hit)
                           ? hit.path : 0;
        if (gp_hit_test(w->tree, p, GP_HIT_MOUSE_DOWN, &hit)) {
            gp_point origin = { p.x - (hit.qx - hit.ox), p.y - (hit.qy - hit.oy) };   /* (so that p - origin is where it is in the element, through a transform too) */
            fire(w, &hit.el->on_mouse_down, p, origin, button, clicks, mods);
            if (button == GP_MOUSE_LEFT && (hit.el->on_drag.fn || hit.el->on_mouse_up.fn)) {
                /* capture by copy: the element itself may be rebuilt before the release */
                w->capture.active = true;
                w->capture.move = hit.el->on_drag;
                w->capture.up = hit.el->on_mouse_up;
                w->capture.origin = origin;
            }
        }
    } else if (button == GP_MOUSE_LEFT) {
        gp_scroll_stop_repeat(w);
        if (w->drag_key) {
            w->drag_key = 0;
            return;
        }
        w->pressed = false;
        if (w->capture.active) {
            w->capture.active = false;
            fire(w, &w->capture.up, p, w->capture.origin, button, clicks, mods);
        }
        /* A click needs press and release on the same element. */
        if (w->down_path && gp_hit_test(w->tree, p, GP_HIT_CLICK, &hit) &&
            hit.path == w->down_path)
            fire(w, &hit.el->on_click, p, (gp_point){ p.x - (hit.qx - hit.ox), p.y - (hit.qy - hit.oy) }, button, clicks, mods);
        w->down_path = 0;
    }
}

bool gp_window_on_key_up(gp_window *w, const char *key, uint32_t mods) {
    gp_key_event ev = { key, mods, false };
    return gp_input_key_up(w, &ev);
}

bool gp_window_on_key(gp_window *w, const char *key, uint32_t mods, bool repeat) {
    gp_key_event ev = { key, mods, repeat };
    bool handled = gp_input_key_down(w, &ev);
    /* A handled key must not also type its character. This is per key, not sticky: several
     * keys can arrive in one batch, and the next one's text belongs to the next one. */
    w->swallow_text = handled;
    return handled;
}

void gp_window_on_ime(gp_window *w, const char *preedit, int cursor) {
    gp_input_ime(w, preedit, cursor);
}

void gp_window_on_text(gp_window *w, const char *utf8) {
    if (w->swallow_text) return;
    gp_input_text(w, utf8);
}

/* ---------------------------------------------------------------- frames */

static void write_ppm(const char *path, const uint8_t *rgb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 3, (size_t)w * h, f);
    fclose(f);
}

/* Test hook: pops one keystroke off GPUIC_KEYS ("tab h i shift-tab ctrl-q") and queues it
 * as a real platform key event. */
static void inject_key(gp_app *app, gp_window *w) {
    while (*app->keys == ' ') app->keys++;
    const char *end = strchr(app->keys, ' ');
    size_t n = end ? (size_t)(end - app->keys) : strlen(app->keys);
    char tok[32] = { 0 };
    memcpy(tok, app->keys, n < sizeof tok - 1 ? n : sizeof tok - 1);
    app->keys += n;
    while (*app->keys == ' ') app->keys++;
    app->shot_at = app->frames + 3; /* let the result render before the screenshot */

    gp_keystroke ks;
    if (gp_parse_keystroke(tok, &ks)) gp_platform_inject_key(w, ks.key, ks.mods);
}

/* Test hook: GPUIC_WHEEL="x,y,dx,dy;x,y,dx,dy" queues real SDL wheel events, one per frame.
 * In notches, SDL's convention: dy < 0 scrolls down (content moves up), dx > 0 scrolls right. */
static void inject_wheel(gp_app *app, gp_window *w) {
    float x, y, dx, dy;
    if (sscanf(app->wheel, "%f,%f,%f,%f", &x, &y, &dx, &dy) == 4) gp_platform_inject_wheel(w, x, y, dx, dy, 0);
    const char *next = strchr(app->wheel, ';');
    app->wheel = next ? next + 1 : "";
    app->shot_at = app->frames + 3;
}

/* Calls every gp_on_layout handler in the tree with its element's final size. */
static void run_layout_handlers(gp_window *w, gp_el *n) {
    if (n->on_layout.fn) {
        void *state = gp_entity_get(w->app, n->on_layout.owner);
        if (state) {
            gp_ctx cx = { w->app, w, n->on_layout.owner, false };
            ((gp_layout_fn)n->on_layout.fn)(state, &cx, n->size, n->on_layout.data);
        }
    }
    if (n->on_scroll.fn && (n->style.overflow_x_scroll || n->style.overflow_y_scroll)) gp_scroll_notify(w, n);
    for (uint32_t i = 0; i < n->nchildren; i++) run_layout_handlers(w, n->children[i]);
}

static void build_and_layout(gp_app *app, gp_window *w) {
    /* Build and lay out. gp_on_layout handlers run after each layout and may ask for another round
     * (a text field that learned its width re-wraps); iterate so that nothing is painted from a
     * tree that was about to be replaced. Capped: a handler that always asks would otherwise spin. */
    for (int pass = 0; (w->dirty_render || w->dirty_layout || !w->tree) && pass < 4; pass++) {
        gp_effects_flush(app);   /* observers bring state up to date before it is rendered */
        bool rebuild = w->dirty_render || !w->tree;   /* otherwise only the layout moved (an animation): same tree */
        w->dirty_render = w->dirty_layout = false;
        int64_t t0 = gp_now_ns();
        if (rebuild) {
            gp_views_begin(w);
            gp_arena_reset(&w->arena);
            gp_build_ctx bc = { &w->arena, app, w, GP_ENTITY_NULL, false };
            w->tree = gp_build_view(&bc, w->root_view, false);
            w->views.all_dirty = false;
        }
        int64_t t1 = gp_now_ns();
        gp_layout_use_states(&w->lstates);
        gp_layout_root(w->tree, app->text, w->size, &w->scrolls, &w->views, &w->anims);
        gp_layout_use_states(NULL);
        if (rebuild) gp_views_sweep(w);
        app->stat_build_ns += t1 - t0;
        app->stat_layout_ns += gp_now_ns() - t1;
        run_layout_handlers(w, w->tree);
        gp_input_after_render(w);
        update_cursor(w);
    }
}

static int lstate_cmp(const void *a, const void *b) {
    uint64_t x = ((const gp_lstate *)a)->path, y = ((const gp_lstate *)b)->path;
    return x < y ? -1 : x > y;
}

/* What the states of elements depend on, besides layout: where the pointer is, the button, the focus, and the scroll offsets. */
static uint64_t lstate_situation(const gp_window *w) {
    uint64_t h = 1469598103934665603ull;
    float v[3] = { w->mouse.x, w->mouse.y, (float)w->mouse_in + 2.f * (float)w->pressed };
    uint32_t bits[3];
    memcpy(bits, v, sizeof bits);
    for (int i = 0; i < 3; i++) h = (h ^ bits[i]) * 1099511628211ull;
    h = (h ^ w->focused) * 1099511628211ull;
    for (size_t i = 0; i < w->scrolls.len; i++) {
        float o[2] = { w->scrolls.items[i].x, w->scrolls.items[i].y };
        uint32_t ob[2];
        memcpy(ob, o, sizeof ob);
        h = (h ^ ob[0]) * 1099511628211ull;
        h = (h ^ ob[1]) * 1099511628211ull;
    }
    return h;
}

/* Paints, and lays out again if what was painted is in other states than it was laid out in (a hover style that lays out: the element
 * under the pointer grows), until the two agree. A style that moves its element away from the pointer would have it go back and forth
 * for ever, so only so many layouts follow while the pointer, the button, the focus and the scroll offsets stay where they are; then
 * what was last laid out stays (as in a browser, which looks again only when the pointer moves). */
static void paint_window(gp_app *app, gp_window *w) {
    for (;;) {
        int64_t tp = gp_now_ns();
        gp_paint_collect_states(&w->lstates_seen);
        gp_paint_tree(&w->scene, &w->scratch, app->text, w->tree, w->scale, w->mouse, w->mouse_in,
                      w->pressed, w->focused, w->drag_key, w->scroll_vis_gen, &w->views, app);
        gp_paint_collect_states(NULL);
        app->stat_paint_ns += gp_now_ns() - tp;
        gp_lstate_set *seen = &w->lstates_seen, *used = &w->lstates;
        if (seen->len > 1) qsort(seen->items, seen->len, sizeof *seen->items, lstate_cmp);
        bool same = seen->len == used->len;
        for (size_t i = 0; same && i < seen->len; i++) same = seen->items[i].path == used->items[i].path && seen->items[i].bits == used->items[i].bits;
        if (same) return;
        uint64_t sit = lstate_situation(w);
        if (sit != w->lstate_situation || w->anims.running) {
            w->lstate_situation = sit;
            w->lstate_budget = 4;
        }
        if (w->lstate_budget <= 0) return;
        w->lstate_budget--;
        gp_lstate_set t = *used;
        *used = *seen;
        *seen = t;
        w->dirty_layout = true;
        build_and_layout(app, w);
    }
}

static void window_draw(gp_app *app, gp_window *w) {
    /* GPUIC_IME=text: show a composition once the scripted keys are done. Applied right before
     * every draw, because the compositor answers each text-input-area update with its own (for a
     * simulated IME: empty) preedit, which would otherwise erase it before the frame is drawn. */
    if (app->ime_text && app->ime_shot_set)
        gp_window_on_ime(w, app->ime_text, (int)strlen(app->ime_text));

    build_and_layout(app, w);
    gp_scroll_track_repeat(w);
    gp_scroll_fling_update(w);
    gp_scroll_smooth_update(w);
    if (w->reveal_id) {
        uint64_t id = w->reveal_id;
        w->reveal_id = 0;
        gp_scroll_reveal(w, id);
    }
    /* something is still animating: ask for the next frame (at the display's rate); when nothing is, no more are */
    if (w->anims.running && !w->anims.job_pending) gp_exec_window_frame(app, w, GP_WAKE_LAYOUT);
    w->dirty_paint = false;
    gp_scroll_autohide_update(w);   /* how visible the fading scrollbars are now, and when to look again */
    paint_window(app, w);

    gp_platform_make_current(w);
    int64_t ts = gp_now_ns();
    gp_renderer_draw(app->renderer, app->text, &w->scene, w->px_w, w->px_h, w->scale,
                     w->background);
    app->stat_submit_ns += gp_now_ns() - ts;
    app->stat_instances += (int64_t)w->scene.len;
    app->frames++;
    bool forced = app->force_atlas_reset_every > 0 && app->frames % app->force_atlas_reset_every == 0;
    if (gp_text_atlas_overflowed(app->text) || gp_gfx_overflowed(app->gfx) || forced) {
        app->stat_atlas_resets++;
        /* This frame is missing the glyphs that did not fit. Start over with empty atlases and
         * repaint: everything on screen fits in a fresh atlas unless a single frame needs more
         * glyphs than an atlas holds, which no ordinary window does. */
        gp_text_atlas_reset(app->text);   /* (also what ages the painted copies of cached views: they hold glyph and ramp numbers) */
        gp_gfx_reset(app->gfx);
        for (int i = 0; i < app->nwindows; i++) app->windows[i]->dirty_paint = true;
    }
    app->stat_draws++;

    bool script_pending = (app->keys && *app->keys) || (app->wheel && *app->wheel) || app->has_drag;
    for (int i = 0; i < app->nclicks; i++) script_pending |= !app->clicks[i].done;
    bool waited = (gp_now_ns() - app->start_ns) / 1000000 >= app->wait_ms;
    if (app->shot_path && app->frames >= app->shot_at && !script_pending && waited) {
        uint8_t *px = gp_renderer_read_pixels(w->px_w, w->px_h);
        write_ppm(app->shot_path, px, w->px_w, w->px_h);
        free(px);
        app->quit = true;
    }
    if (app->capture_next) {
        free(app->capture_px);
        app->capture_px = gp_renderer_read_pixels(w->px_w, w->px_h);
        app->capture_w = w->px_w;
        app->capture_h = w->px_h;
        app->capture_next = false;
    }
    gp_platform_swap(w);
}

void gp_debug_capture_next(gp_app *app) {
    free(app->capture_px);
    app->capture_px = NULL;
    app->capture_next = true;
}

int gp_application_run(gp_ready_fn ready, void *userdata) {
    gp_app *app = calloc(1, sizeof *app);
    if (!gp_platform_init(app)) {
        free(app);
        return 1;
    }
    app->text = gp_text_system_new();
    if (!app->text) {
        gp_platform_shutdown(app);
        free(app);
        return 1;
    }

    app->gfx = gp_gfx_new();
    if (!app->gfx) {
        gp_text_system_free(app->text);
        gp_platform_shutdown(app);
        free(app);
        return 1;
    }
    gp_text_set_gfx(app->text, app->gfx);

    gp_exec_init(app);

    /* Test hooks: GPUIC_SCREENSHOT=out.ppm [GPUIC_HOVER=x,y] [GPUIC_CLICK=x,y]
     * [GPUIC_CLICKS="x,y@ms;..."] [GPUIC_WAIT_MS=n] ... see inject_* below */
    app->start_ns = gp_now_ns();
    const char *wait = getenv("GPUIC_WAIT_MS");
    app->wait_ms = wait ? atoi(wait) : 0;
    const char *clicks = getenv("GPUIC_CLICKS");
    while (clicks && *clicks && app->nclicks < 8) {
        float x, y;
        int ms;
        if (sscanf(clicks, "%f,%f@%d", &x, &y, &ms) != 3) break;
        app->clicks[app->nclicks].at = (gp_point){ x, y };
        app->clicks[app->nclicks++].ms = ms;
        clicks = strchr(clicks, ';');
        if (clicks) clicks++;
    }
    app->shot_path = getenv("GPUIC_SCREENSHOT");
    const char *click = getenv("GPUIC_CLICK");
    if (click && sscanf(click, "%f,%f", &app->click_at.x, &app->click_at.y) == 2)
        app->has_click = true;
    app->keys = getenv("GPUIC_KEYS");
    const char *reset_every = getenv("GPUIC_ATLAS_RESET_EVERY");
    app->force_atlas_reset_every = reset_every ? atoi(reset_every) : 0;
    app->ime_text = getenv("GPUIC_IME");
    app->wheel = getenv("GPUIC_WHEEL");
    const char *smooth = getenv("GPUIC_SMOOTH_SCROLL");
    if (smooth) app->smooth_scroll_s = (float)atof(smooth) / 1000.f;
    const char *drag = getenv("GPUIC_DRAG");
    if (drag && sscanf(drag, "%f,%f,%f,%f", &app->drag[0], &app->drag[1], &app->drag[2], &app->drag[3]) == 4)
        app->has_drag = true;
    app->shot_at = 4;
    const char *hover = getenv("GPUIC_HOVER");
    if (hover && sscanf(hover, "%f,%f", &app->hover_at.x, &app->hover_at.y) == 2)
        app->has_hover = true;

    ready(app, userdata);
    gp_effects_flush(app);

    int status = app->nwindows ? 0 : 1;
    while (!app->quit && app->nwindows) {
        /* sleep until the next event, timer or frame; test mode keeps frames flowing */
        bool scripted = app->shot_path || app->nclicks;
        int timeout = app->shot_path ? 0 : gp_exec_timeout_ms(app);
        if (scripted && !app->shot_path && (timeout < 0 || timeout > 10)) timeout = 10; /* poll the script */
        if (gp_effects_pending(app)) timeout = 0;
        gp_platform_pump(app, timeout);
        app->stat_wakeups++;
        for (int i = 0; i < app->nwindows; i++) app->windows[i]->swallow_text = false;
        gp_exec_run(app);
        gp_effects_flush(app);   /* what drawing or the script hooks below queued last turn */

        for (int i = 0; i < app->nwindows;) {
            gp_window *w = app->windows[i];
            if (w->closed) {
                gp_exec_window_closed(app, w);
                if (app->nwindows == 1) renderer_release(app, w);
                window_destroy(app, w);
                memmove(&app->windows[i], &app->windows[i + 1],
                        (size_t)(app->nwindows - i - 1) * sizeof w);
                app->nwindows--;
                continue;
            }
            if (w->dirty_render || w->dirty_paint || w->dirty_layout || !w->tree) window_draw(app, w);
            i++;
        }

        if (scripted && app->nwindows) {
            gp_window *w = app->windows[0];
            if (app->frames == 2 && app->has_hover) {
                app->has_hover = false;
                gp_window_on_mouse_move(w, app->hover_at.x, app->hover_at.y);
            }
            if (app->frames == 2 && app->has_click) {
                app->has_click = false;
                gp_window_on_mouse_move(w, app->click_at.x, app->click_at.y);
                gp_window_on_mouse_button(w, app->click_at.x, app->click_at.y, GP_MOUSE_LEFT, true, 1, 0);
                gp_window_on_mouse_button(w, app->click_at.x, app->click_at.y, GP_MOUSE_LEFT, false, 1, 0);
            }
            for (int i = 0; i < app->nclicks; i++) {
                int64_t elapsed_ms = (gp_now_ns() - app->start_ns) / 1000000;
                if (app->clicks[i].done || app->frames < 1 || elapsed_ms < app->clicks[i].ms) continue;
                app->clicks[i].done = true;
                gp_point p = app->clicks[i].at;
                gp_window_on_mouse_move(w, p.x, p.y);
                gp_window_on_mouse_button(w, p.x, p.y, GP_MOUSE_LEFT, true, 1, 0);
                gp_window_on_mouse_button(w, p.x, p.y, GP_MOUSE_LEFT, false, 1, 0);
                app->shot_at = app->frames + 3;
            }
            if (app->frames >= 2 && app->keys && *app->keys) inject_key(app, w);
            if (app->frames >= 3 && app->ime_text && !(app->keys && *app->keys) && !app->ime_shot_set) {
                app->ime_shot_set = true;
                app->shot_at = app->frames + 4;
            }
            if (app->frames >= 2 && app->wheel && *app->wheel) inject_wheel(app, w);
            if (app->has_drag && app->frames >= 2 + app->drag_stage) {
                /* press, move halfway, move to the end, release: one step per frame */
                float *d = app->drag;
                switch (app->drag_stage++) {
                case 0: gp_window_on_mouse_move(w, d[0], d[1]);
                        gp_window_on_mouse_button(w, d[0], d[1], GP_MOUSE_LEFT, true, 1, 0); break;
                case 1: gp_window_on_mouse_move(w, (d[0] + d[2]) / 2, (d[1] + d[3]) / 2); break;
                case 2: gp_window_on_mouse_move(w, d[2], d[3]); break;
                default:
                    gp_window_on_mouse_button(w, d[2], d[3], GP_MOUSE_LEFT, false, 1, 0);
                    app->has_drag = false;
                    app->shot_at = app->frames + 3;
                }
            }
            if (app->shot_path) w->dirty_paint = true; /* keep frames flowing in test mode */
        }
    }

    if (getenv("GPUIC_STATS"))
        fprintf(stderr, "gpuic: %lu loop wakeups, %lu draws, %lu atlas resets in %.2f s; %lu view renders; "
                        "per draw: build %.3f ms, layout %.3f ms, paint %.3f ms, GL submit %.3f ms (%ld instances); cached subtrees: %lu layouts skipped, "
                        "%lu paints reused, %lu painted\n",
                app->stat_wakeups, app->stat_draws, app->stat_atlas_resets,
                (double)(gp_now_ns() - app->start_ns) / 1e9, app->stat_view_renders,
                app->stat_draws ? (double)app->stat_build_ns / 1e6 / (double)app->stat_draws : 0,
                app->stat_draws ? (double)app->stat_layout_ns / 1e6 / (double)app->stat_draws : 0,
                app->stat_draws ? (double)app->stat_paint_ns / 1e6 / (double)app->stat_draws : 0,
                app->stat_draws ? (double)app->stat_submit_ns / 1e6 / (double)app->stat_draws : 0,
                app->stat_draws ? (long)(app->stat_instances / (int64_t)app->stat_draws) : 0L,
                gp_layout_skips(), app->stat_paint_hits, app->stat_paint_misses);
    gp_exec_shutdown(app); /* cancel tasks, unwind coroutines, join the pool: entities still alive */
    renderer_release(app, app->nwindows ? app->windows[0] : NULL);
    for (int i = 0; i < app->nwindows; i++) window_destroy(app, app->windows[i]);
    for (uint32_t i = 0; i < app->nslots; i++) {
        gp_entity_slot *s = &app->slots[i];
        if (!s->alive) continue;
        if (s->drop) s->drop(s->state);
        free(s->state);
    }
    gp_subs_free(app);
    free(app->slots);
    free(app->free_slots);
    gp_input_free(app);
    free(app->capture_px);
    free(app->clip_local);
    free(app->primary_local);
    gp_text_system_free(app->text);
    gp_gfx_free(app->gfx);
    gp_platform_shutdown(app);
    free(app);
    return status;
}

/* ---------------------------------------------------------------- clipboard / synthetic input */

/* The platform clipboard can refuse us (Wayland only lets a focused client set it; headless
 * sessions have none), which would make copy/paste inside the app silently stop working. So the
 * app remembers its last copy and uses it when the platform has nothing. Text copied in another
 * application still wins, because the platform clipboard is asked first. */
char *gp_clipboard_get(gp_app *app) {
    char *platform = gp_platform_clipboard_get();
    if (platform) return platform;
    return app->clip_local ? strdup(app->clip_local) : NULL;
}

void gp_clipboard_set(gp_app *app, const char *utf8) {
    free(app->clip_local);
    app->clip_local = strdup(utf8 ? utf8 : "");
    gp_platform_clipboard_set(utf8);
}

/* The primary selection (X11 / Wayland: what is selected, pasted with the middle button). Like the clipboard, the platform can refuse
 * it (an unfocused client on Wayland, no display server session), so the app remembers its own selection and uses it then. Once the
 * platform has taken one of ours, it is the platform that is asked, so a selection made in another application is pasted here. */
char *gp_primary_get(gp_app *app) {
    if (!app->primary_ours) {
        char *platform = gp_platform_primary_get();
        if (platform) return platform;
    }
    return app->primary_local ? strdup(app->primary_local) : NULL;
}

void gp_primary_set(gp_app *app, const char *utf8) {
    free(app->primary_local);
    app->primary_local = strdup(utf8 ? utf8 : "");
    app->primary_ours = !gp_platform_primary_set(utf8 ? utf8 : "");
}

static gp_window *sim_window(gp_app *app) { return app->nwindows ? app->windows[0] : NULL; }

void gp_simulate_key(gp_app *app, const char *keystroke, bool down) {
    gp_window *w = sim_window(app);
    gp_keystroke ks;
    if (!w || !gp_parse_keystroke(keystroke, &ks)) return;
    if (down) gp_platform_inject_key(w, ks.key, ks.mods);
    else gp_platform_inject_key_up(w, ks.key, ks.mods);
}

void gp_simulate_keystrokes(gp_app *app, const char *keys) {
    gp_window *w = sim_window(app);
    while (w && *keys) {
        while (*keys == ' ') keys++;
        const char *end = strchr(keys, ' ');
        size_t n = end ? (size_t)(end - keys) : strlen(keys);
        char tok[32] = { 0 };
        memcpy(tok, keys, n < sizeof tok - 1 ? n : sizeof tok - 1);
        keys += n;
        gp_keystroke ks;
        if (tok[0] && gp_parse_keystroke(tok, &ks)) {
            gp_platform_inject_key(w, ks.key, ks.mods);
            gp_platform_inject_key_up(w, ks.key, ks.mods);
        }
    }
}

void gp_simulate_text(gp_app *app, const char *utf8) {
    gp_window *w = sim_window(app);
    if (w) gp_platform_inject_text(w, utf8);
}

/* Delivered synchronously, not queued: compositors send their own composition updates (empty
 * ones, for an app whose IME is idle) on a timer, and a queued fake preedit loses that race. */
void gp_simulate_ime(gp_app *app, const char *preedit, int cursor) {
    gp_window *w = sim_window(app);
    if (w) gp_window_on_ime(w, preedit, cursor);
}

void gp_simulate_mouse_button(gp_app *app, int kind, float x, float y, int button, int clicks, uint32_t mods) {
    gp_window *w = sim_window(app);
    if (w) gp_platform_inject_mouse(w, kind, x, y, button, clicks, mods);
}

void gp_simulate_touch(gp_app *app, int phase, float x, float y, double time_seconds) {
    gp_window *w = sim_window(app);
    if (w) gp_scroll_touch(w, phase, x, y, (int64_t)(time_seconds * 1e9));
}

void gp_simulate_wheel(gp_app *app, float x, float y, float dx, float dy, uint32_t mods) {
    gp_window *w = sim_window(app);
    if (w) gp_platform_inject_wheel(w, x, y, dx, dy, mods);
}

void gp_simulate_mouse(gp_app *app, int kind, float x, float y, int clicks, uint32_t mods) {
    gp_simulate_mouse_button(app, kind, x, y, GP_MOUSE_LEFT, clicks, mods);
}
