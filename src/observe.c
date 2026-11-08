/*
 * Observing entities: gp_observe, gp_subscribe / gp_emit, gp_observe_release.
 *
 * Subscriptions live in one pool, each chained into two lists: the one at the entity it watches
 * (what a notify or emit walks) and the one at the entity it belongs to (what releasing the owner
 * walks). A gp_subscription handle is the pool index plus a generation, so a stale handle misses.
 *
 * gp_notify and gp_emit do not call anybody: they queue an effect, and the queue is flushed when
 * the callback that was running returns to the event loop (after each platform event, each job,
 * and before every render), or by gp_flush_effects. So a callback is never re-entered by its own
 * notifications, and an observer sees the entity in the state the caller left it in. Effects queued
 * by observers run in the same flush, in order. Notifies of one entity are merged until delivered.
 *
 * Nothing here keeps a pointer into the pool or the slot table across a callback: they are
 * arrays and callbacks create entities and subscriptions.
 */
#include "internal.h"

enum { SUB_OBSERVE = 1, SUB_EVENT, SUB_RELEASE };

#define MAX_EFFECTS_PER_FLUSH 100000

/* ---------------------------------------------------------------- pool */

static gp_sub *sub_get(gp_app *app, gp_subscription h) {
    uint32_t idx = (uint32_t)h.id, gen = (uint32_t)(h.id >> 32);
    if (!h.id || idx >= app->nsubs) return NULL;
    gp_sub *s = &app->subs[idx];
    return s->alive && s->generation == gen ? s : NULL;
}

static gp_subscription handle_of(const gp_app *app, uint32_t idx) {
    return (gp_subscription){ ((uint64_t)app->subs[idx].generation << 32) | idx };
}

static uint32_t sub_alloc(gp_app *app) {
    if (app->free_sub) {
        uint32_t idx = app->free_sub - 1;
        app->free_sub = app->subs[idx].next_free;
        return idx;
    }
    if (app->nsubs == app->cap_subs) {
        app->cap_subs = app->cap_subs ? app->cap_subs * 2 : 32;
        app->subs = realloc(app->subs, app->cap_subs * sizeof *app->subs);
        if (!app->subs) abort();
    }
    uint32_t idx = app->nsubs++;
    app->subs[idx] = (gp_sub){ .generation = 1 };
    return idx;
}

static void sub_free(gp_app *app, uint32_t idx) {
    gp_sub *s = &app->subs[idx];
    if (s->t_prev) app->subs[s->t_prev - 1].t_next = s->t_next;
    else app->slots[s->target.index].sub_target_head = s->t_next;
    if (s->t_next) app->subs[s->t_next - 1].t_prev = s->t_prev;
    if (s->owner.generation) {
        if (s->o_prev) app->subs[s->o_prev - 1].o_next = s->o_next;
        else app->slots[s->owner.index].sub_owner_head = s->o_next;
        if (s->o_next) app->subs[s->o_next - 1].o_prev = s->o_prev;
    }
    free(s->event);
    if (s->drop) {   /* not now: this may be the callback being run */
        if (app->nsub_drops == app->cap_sub_drops) {
            app->cap_sub_drops = app->cap_sub_drops ? app->cap_sub_drops * 2 : 8;
            app->sub_drops = realloc(app->sub_drops, app->cap_sub_drops * sizeof *app->sub_drops);
            if (!app->sub_drops) abort();
        }
        app->sub_drops[app->nsub_drops].fn = s->drop;
        app->sub_drops[app->nsub_drops++].data = s->drop_data;
    }
    uint32_t gen = s->generation + 1;
    *s = (gp_sub){ .generation = gen ? gen : 1, .next_free = app->free_sub };
    app->free_sub = idx + 1;
}

static gp_subscription sub_add(gp_ctx *cx, gp_entity target, int kind, const char *event, void *fn,
                               void *data) {
    gp_app *app = cx->app;
    bool has_owner = cx->entity.generation != 0;
    if (!gp_slot_for(app, target) || (has_owner && !gp_slot_for(app, cx->entity)))
        return (gp_subscription){ 0 };
    uint32_t idx = sub_alloc(app);
    gp_sub *s = &app->subs[idx];
    s->alive = true;
    s->kind = (uint8_t)kind;
    s->target = target;
    s->owner = has_owner ? cx->entity : GP_ENTITY_NULL;
    if (event) {
        s->event = strdup(event);
        if (!s->event) abort();
    }
    s->fn = fn;
    s->data = data;
    gp_entity_slot *ts = &app->slots[target.index];
    s->t_next = ts->sub_target_head;
    if (ts->sub_target_head) app->subs[ts->sub_target_head - 1].t_prev = idx + 1;
    ts->sub_target_head = idx + 1;
    if (has_owner) {
        gp_entity_slot *os = &app->slots[cx->entity.index];
        s->o_next = os->sub_owner_head;
        if (os->sub_owner_head) app->subs[os->sub_owner_head - 1].o_prev = idx + 1;
        os->sub_owner_head = idx + 1;
    }
    return handle_of(app, idx);
}

gp_subscription gp_observe(gp_ctx *cx, gp_entity target, gp_observe_fn fn, void *data) {
    return sub_add(cx, target, SUB_OBSERVE, NULL, (void *)fn, data);
}

gp_subscription gp_subscribe(gp_ctx *cx, gp_entity emitter, const char *event, gp_event_fn fn, void *data) {
    if (!fn) return (gp_subscription){ 0 };
    return sub_add(cx, emitter, SUB_EVENT, event, (void *)fn, data);
}

gp_subscription gp_observe_release(gp_ctx *cx, gp_entity target, gp_release_fn fn, void *data) {
    if (!fn) return (gp_subscription){ 0 };
    return sub_add(cx, target, SUB_RELEASE, NULL, (void *)fn, data);
}

void gp_unsubscribe(gp_app *app, gp_subscription h) {
    gp_sub *s = sub_get(app, h);
    if (s) sub_free(app, (uint32_t)h.id);
}

bool gp_subscription_active(gp_app *app, gp_subscription h) { return sub_get(app, h) != NULL; }

void gp_subscription_on_drop(gp_app *app, gp_subscription h, gp_drop_fn drop, void *data) {
    gp_sub *s = sub_get(app, h);
    if (!s) { if (drop) drop(data); return; }
    s->drop = drop;
    s->drop_data = data;
}

/* Closures of freed subscriptions; a drop function may subscribe or unsubscribe, so the queue is taken first. */
static void run_sub_drops(gp_app *app) {
    while (app->nsub_drops) {
        size_t n = app->nsub_drops;
        gp_sub_drop *q = app->sub_drops;
        app->sub_drops = NULL;
        app->nsub_drops = app->cap_sub_drops = 0;
        for (size_t i = 0; i < n; i++) q[i].fn(q[i].data);
        free(q);
    }
}

/* ---------------------------------------------------------------- delivery */

typedef struct { uint32_t idx, generation; } ref;

/* The subscriptions of `kind` on `target` (matching `event`, if given) as of now. Whatever a
 * callback adds or removes meanwhile is judged when its turn comes: removed ones are skipped,
 * added ones wait for the next effect. */
static size_t snapshot(gp_app *app, gp_entity target, int kind, const char *event, ref **out, ref *stack, size_t cap) {
    size_t n = 0;
    ref *refs = stack;
    gp_entity_slot *ts = &app->slots[target.index];
    for (uint32_t at = ts->sub_target_head; at; at = app->subs[at - 1].t_next) {
        const gp_sub *s = &app->subs[at - 1];
        if (s->kind != kind || (event && s->event && strcmp(s->event, event) != 0)) continue;
        if (n == cap) {
            cap *= 2;
            ref *bigger = malloc(cap * sizeof *bigger);
            if (!bigger) abort();
            memcpy(bigger, refs, n * sizeof *bigger);
            if (refs != stack) free(refs);
            refs = bigger;
        }
        refs[n++] = (ref){ at - 1, s->generation };
    }
    *out = refs;
    return n;
}

/* The callback context of a subscription, or false if its owner is gone. */
static bool owner_ctx(gp_app *app, const gp_sub *s, gp_ctx *cx, void **self) {
    *cx = gp_ctx_for(app, s->owner);
    *self = NULL;
    if (!s->owner.generation) return true;
    *self = gp_entity_get(app, s->owner);
    return *self != NULL;
}

static void deliver_notify(gp_app *app, gp_entity target) {
    gp_entity_slot *ts = gp_slot_for(app, target);
    if (!ts) return;
    ts->notify_queued = false;   /* a notify from an observer queues the next round */
    ref stack[16], *refs;
    size_t n = snapshot(app, target, SUB_OBSERVE, NULL, &refs, stack, 16);
    for (size_t i = 0; i < n; i++) {
        if (refs[i].idx >= app->nsubs) continue;
        const gp_sub *s = &app->subs[refs[i].idx];
        if (!s->alive || s->generation != refs[i].generation) continue;
        gp_observe_fn fn = (gp_observe_fn)s->fn;
        void *data = s->data;
        gp_ctx cx;
        void *self;
        if (!owner_ctx(app, s, &cx, &self)) continue;
        if (fn) fn(self, &cx, target, data);
        else gp_notify(&cx);   /* no function: re-render the observer, as cx.observe(.., |_, _, cx| cx.notify()) */
    }
    if (refs != stack) free(refs);
}

static void deliver_emit(gp_app *app, const gp_effect *ef) {
    if (!gp_slot_for(app, ef->entity)) return;
    ref stack[16], *refs;
    size_t n = snapshot(app, ef->entity, SUB_EVENT, ef->name, &refs, stack, 16);
    gp_event ev = { ef->entity, ef->name, ef->payload, ef->size };
    for (size_t i = 0; i < n; i++) {
        if (refs[i].idx >= app->nsubs) continue;
        const gp_sub *s = &app->subs[refs[i].idx];
        if (!s->alive || s->generation != refs[i].generation) continue;
        gp_event_fn fn = (gp_event_fn)s->fn;
        void *data = s->data;
        gp_ctx cx;
        void *self;
        if (!owner_ctx(app, s, &cx, &self)) continue;
        fn(self, &cx, &ev, data);
    }
    if (refs != stack) free(refs);
}

/* ---------------------------------------------------------------- effects */

void gp_queue_effect(gp_app *app, bool emit, gp_entity e, const char *name, const void *payload, size_t size) {
    if (app->neffects == app->cap_effects) {
        app->cap_effects = app->cap_effects ? app->cap_effects * 2 : 32;
        app->effects = realloc(app->effects, app->cap_effects * sizeof *app->effects);
        if (!app->effects) abort();
    }
    gp_effect *ef = &app->effects[app->neffects++];
    *ef = (gp_effect){ .emit = emit, .entity = e, .size = size };
    if (name) {
        ef->name = strdup(name);
        if (!ef->name) abort();
    }
    if (size) {   /* the emitter's buffer may be on its stack */
        ef->payload = malloc(size);
        if (!ef->payload) abort();
        memcpy(ef->payload, payload, size);
    }
}

bool gp_effects_pending(const gp_app *app) { return app->eff_head < app->neffects; }

void gp_flush_effects(gp_app *app) { gp_effects_flush(app); }

static void effects_deliver(gp_app *app);

void gp_effects_flush(gp_app *app) {
    if (app->flushing) return;   /* called from a callback of this flush: it will get there */
    if (gp_effects_pending(app)) effects_deliver(app);
    run_sub_drops(app);
}

static void effects_deliver(gp_app *app) {
    app->flushing = true;
    size_t delivered = 0;
    while (app->eff_head < app->neffects) {
        gp_effect ef = app->effects[app->eff_head++];   /* a copy: callbacks grow the queue */
        if (++delivered > MAX_EFFECTS_PER_FLUSH) {
            fprintf(stderr, "gpuic: %d notifications in one flush: observers are notifying each other in a "
                            "cycle; dropping the rest\n", MAX_EFFECTS_PER_FLUSH);
            for (size_t i = app->eff_head - 1; i < app->neffects; i++) {
                const gp_effect *d = &app->effects[i];
                if (!d->emit) {
                    gp_entity_slot *s = gp_slot_for(app, d->entity);
                    if (s) s->notify_queued = false;
                }
                free(d->name);
                free(d->payload);
            }
            break;
        }
        if (ef.emit) deliver_emit(app, &ef);
        else deliver_notify(app, ef.entity);
        free(ef.name);
        free(ef.payload);
    }
    app->eff_head = app->neffects = 0;
    app->flushing = false;
}

void gp_emit(gp_ctx *cx, const char *event, const void *payload, size_t size) {
    gp_entity_slot *s = gp_slot_for(cx->app, cx->entity);
    if (!s || !event || !s->sub_target_head) return;   /* nobody could be listening */
    gp_queue_effect(cx->app, true, cx->entity, event, payload, payload ? size : 0);
}

/* ---------------------------------------------------------------- lifetime */

void gp_subs_entity_released(gp_app *app, gp_entity e, void *state) {
    if (e.index >= app->nslots) return;
    ref stack[16], *refs;
    size_t n = snapshot(app, e, SUB_RELEASE, NULL, &refs, stack, 16);
    for (size_t i = 0; i < n; i++) {
        if (refs[i].idx >= app->nsubs) continue;
        const gp_sub *s = &app->subs[refs[i].idx];
        if (!s->alive || s->generation != refs[i].generation) continue;
        if (s->owner.index == e.index && s->owner.generation == e.generation) continue;   /* its own end */
        gp_release_fn fn = (gp_release_fn)s->fn;
        void *data = s->data;
        gp_ctx cx;
        void *self;
        if (!owner_ctx(app, s, &cx, &self)) continue;
        fn(self, &cx, e, state, data);
    }
    if (refs != stack) free(refs);

    /* nothing may watch a dead entity, and a dead entity's callbacks never run */
    while (app->slots[e.index].sub_target_head) sub_free(app, app->slots[e.index].sub_target_head - 1);
    while (app->slots[e.index].sub_owner_head) sub_free(app, app->slots[e.index].sub_owner_head - 1);
}

void gp_subs_free(gp_app *app) {
    for (uint32_t i = 0; i < app->nsubs; i++) {
        free(app->subs[i].event);
        if (app->subs[i].alive && app->subs[i].drop) {
            gp_drop_fn d = app->subs[i].drop;
            app->subs[i].drop = NULL;
            d(app->subs[i].drop_data);
        }
    }
    run_sub_drops(app);
    free(app->subs);
    for (size_t i = app->eff_head; i < app->neffects; i++) {
        free(app->effects[i].name);
        free(app->effects[i].payload);
    }
    free(app->effects);
    app->subs = NULL;
    app->effects = NULL;
}
