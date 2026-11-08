/*
 * Test of gp_observe / gp_subscribe / gp_emit / gp_observe_release. Needs a display (it opens a
 * small window to get an event loop); exits non-zero on failure.
 *
 * Callbacks append to a log, so the order of delivery is checked as well as the count. A driver
 * coroutine runs the scenarios; gp_yield() lets the loop turn in which effects are flushed pass.
 */
#include <gpuic/gpuic.h>

#include "../src/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/* ------------------------------------------------------------ fixtures */

static char log_buf[4096];
static void log_clear(void) { log_buf[0] = 0; }
static void log_add(const char *s) {
    size_t n = strlen(log_buf);
    if (n + strlen(s) + 2 >= sizeof log_buf) return;   /* a long log keeps its beginning */
    if (n) log_buf[n++] = ' ';
    strcpy(log_buf + n, s);
}
#define CHECK_LOG(expect) CHECK(strcmp(log_buf, (expect)) == 0, "log is \"%s\", expected \"%s\"", log_buf, (expect))

typedef struct {
    char name;
    int value;
    int calls;
    int seen;               /* what the observed entity's value was at the last call */
    gp_entity last;
    bool dropped;
} Node;

static Node *node(gp_entity e) { return gp_entity_get(app, e); }
static void node_drop(void *self) { ((Node *)self)->dropped = true; }  /* state is freed right after: only the flag test uses it */
static gp_entity make(char name, int value) {
    return gp_entity_new(app, sizeof(Node), &(Node){ .name = name, .value = value }, NULL);
}
static gp_ctx cx_of(gp_entity e) { return gp_ctx_for(app, e); }
static void notify(gp_entity e) { gp_ctx cx = cx_of(e); gp_notify(&cx); }
static void flush(void) { gp_yield(co_); }

/* observer: log "<owner><-<observed>" and remember what it saw */
static void on_observe(void *self, gp_ctx *cx, gp_entity observed, void *data) {
    Node *me = self, *o = gp_entity_get(cx->app, observed);
    me->calls++;
    me->last = observed;
    if (o) me->seen = o->value;
    char buf[8] = { me->name, '<', o ? o->name : '?', 0 };
    log_add(buf);
}

/* ------------------------------------------------------------ the scenarios */

static void test_basics(void) {
    gp_entity m = make('M', 1), a = make('A', 0);
    gp_ctx ca = cx_of(a);
    gp_subscription s = gp_observe(&ca, m, on_observe, NULL);
    CHECK(s.id != 0 && gp_subscription_active(app, s), "observe returned no handle");

    node(m)->value = 7;
    notify(m);
    CHECK(node(a)->calls == 0, "the observer ran inside gp_notify (%d calls)", node(a)->calls);
    flush();
    CHECK(node(a)->calls == 1, "observer ran %d times after the flush", node(a)->calls);
    CHECK(node(a)->seen == 7, "observer saw value %d", node(a)->seen);
    CHECK(node(a)->last.index == m.index && node(a)->last.generation == m.generation, "wrong `observed` handle");

    /* several notifies before delivery are one call, and see the final state */
    node(m)->value = 1;
    notify(m);
    node(m)->value = 2;
    notify(m);
    node(m)->value = 3;
    notify(m);
    flush();
    CHECK(node(a)->calls == 2 && node(a)->seen == 3, "merged notifies: %d calls, saw %d", node(a)->calls, node(a)->seen);

    /* a notify after delivery is a new one */
    notify(m);
    flush();
    CHECK(node(a)->calls == 3, "a later notify was lost (%d)", node(a)->calls);

    /* an entity nobody observes: nothing happens, nothing queued */
    gp_entity lone = make('L', 0);
    notify(lone);
    CHECK(!gp_effects_pending(app), "notifying an unobserved entity queued an effect");

    gp_entity_release(app, m);
    gp_entity_release(app, a);
    gp_entity_release(app, lone);
    flush();
}

static void notify_self_cb(void *self, gp_ctx *cx, gp_entity observed, void *data) {
    Node *me = self;
    me->calls++;
    log_add((char[]){ me->name, '!', 0 });
    gp_notify(cx);   /* my own observers hear about it */
}

static void test_chains_and_order(void) {
    /* M <- A <- B: A re-notifies when it hears from M, so B hears in the same flush */
    gp_entity m = make('M', 0), a = make('A', 0), b = make('B', 0);
    gp_ctx ca = cx_of(a), cb = cx_of(b);
    gp_observe(&ca, m, notify_self_cb, NULL);
    gp_observe(&cb, a, on_observe, NULL);
    log_clear();
    notify(m);
    flush();
    CHECK_LOG("A! B<A");
    CHECK(!gp_effects_pending(app), "effects left after the flush");

    /* two observers of one entity: both, and a second entity's are not called */
    gp_entity c = make('C', 0), d = make('D', 0), other = make('O', 0);
    gp_ctx cc = cx_of(c), cd = cx_of(d);
    gp_observe(&cc, other, on_observe, NULL);
    gp_observe(&cd, other, on_observe, NULL);
    gp_observe(&cc, m, on_observe, NULL);
    log_clear();
    notify(other);
    flush();
    CHECK(node(c)->calls == 1 && node(d)->calls == 1, "both observers of one entity: %d, %d", node(c)->calls, node(d)->calls);
    CHECK(strstr(log_buf, "C<O") && strstr(log_buf, "D<O") && !strstr(log_buf, "<M"), "log: %s", log_buf);

    gp_entity all[] = { m, a, b, c, d, other };
    for (size_t i = 0; i < sizeof all / sizeof *all; i++) gp_entity_release(app, all[i]);
    flush();
}

static void test_null_fn_rerenders_the_observer(void) {
    /* C mirrors M with no function; D observes C. Notifying M reaches D only if C was notified. */
    gp_entity m = make('M', 0), c = make('C', 0), d = make('D', 0);
    gp_ctx cc = cx_of(c), cd = cx_of(d);
    gp_observe(&cc, m, NULL, NULL);
    gp_observe(&cd, c, on_observe, NULL);
    log_clear();
    notify(m);
    flush();
    CHECK_LOG("D<C");
    gp_entity_release(app, m);
    gp_entity_release(app, c);
    gp_entity_release(app, d);
    flush();
}

static void test_lifetimes(void) {
    /* owner released: its subscription goes, and nothing touches its freed state */
    gp_entity m = make('M', 0), a = make('A', 0);
    gp_ctx ca = cx_of(a);
    gp_subscription s = gp_observe(&ca, m, on_observe, NULL);
    gp_entity_release(app, a);
    CHECK(!gp_subscription_active(app, s), "subscription outlived its owner");
    log_clear();
    notify(m);
    flush();
    CHECK_LOG("");

    /* ... also when the notify was queued first */
    gp_entity a2 = make('A', 0);
    gp_ctx ca2 = cx_of(a2);
    gp_observe(&ca2, m, on_observe, NULL);
    notify(m);
    gp_entity_release(app, a2);
    flush();
    CHECK_LOG("");

    /* target released: subscription ends, handle is stale */
    gp_entity a3 = make('A', 0);
    gp_ctx ca3 = cx_of(a3);
    gp_subscription s3 = gp_observe(&ca3, m, on_observe, NULL);
    gp_entity_release(app, m);
    CHECK(!gp_subscription_active(app, s3), "subscription outlived its target");
    notify(m);      /* stale handle: harmless */
    flush();
    CHECK(node(a3)->calls == 0, "observer of a released entity was called");

    /* observing something already released, or as a released entity, gives nothing */
    gp_subscription dead = gp_observe(&ca3, m, on_observe, NULL);
    CHECK(dead.id == 0, "observing a released entity returned a handle");
    gp_ctx stale = cx_of(a2);
    CHECK(gp_observe(&stale, a3, on_observe, NULL).id == 0, "a released entity registered an observer");

    /* the slot of a released entity is reused: a notify queued for the old one must not block the new one */
    gp_entity m2 = make('M', 0), obs = make('B', 0);
    gp_ctx cobs = cx_of(obs);
    gp_observe(&cobs, m2, on_observe, NULL);
    notify(m2);                       /* queued, flag set */
    gp_entity_release(app, m2);       /* ... and the entity goes before delivery */
    gp_entity m3 = make('N', 0);      /* takes the slot */
    CHECK(m3.index == m2.index, "slot not reused (test premise): %u vs %u", m3.index, m2.index);
    gp_observe(&cobs, m3, on_observe, NULL);
    node(obs)->calls = 0;
    notify(m3);
    flush();
    CHECK(node(obs)->calls == 1, "notify on a reused slot called the observer %d times", node(obs)->calls);

    /* the old observer must not have been inherited by the new occupant of the slot */
    gp_entity a4 = make('A', 0);
    gp_ctx ca4 = cx_of(a4);
    gp_entity m4 = make('M', 0);
    gp_observe(&ca4, m4, on_observe, NULL);
    gp_entity_release(app, m4);
    gp_entity m5 = make('X', 0);
    notify(m5);
    flush();
    CHECK(node(a4)->calls == 0, "a new entity inherited the observers of the one that had its slot");

    gp_entity all[] = { a3, obs, m3, a4, m5 };
    for (size_t i = 0; i < sizeof all / sizeof *all; i++) gp_entity_release(app, all[i]);
    flush();
}

static gp_subscription victim_sub;
static gp_entity elsewhere;
static void on_marker(void *self, gp_ctx *cx, gp_entity observed, void *data) { log_add("NEW"); }
static void on_observe_unsub_victim(void *self, gp_ctx *cx, gp_entity observed, void *data) {
    ((Node *)self)->calls++;
    gp_unsubscribe(cx->app, victim_sub);   /* a later observer in this very delivery ... */
    gp_observe(cx, elsewhere, on_marker, NULL);   /* ... and its pool slot is taken by a new subscription */
}
static gp_subscription self_sub;
static void on_observe_once(void *self, gp_ctx *cx, gp_entity observed, void *data) {
    ((Node *)self)->calls++;
    gp_unsubscribe(cx->app, self_sub);
}

static void test_unsubscribe(void) {
    gp_entity m = make('M', 0), a = make('A', 0), b = make('B', 0);
    gp_ctx ca = cx_of(a), cb = cx_of(b);

    gp_subscription sb = gp_observe(&cb, m, on_observe, NULL);
    gp_unsubscribe(app, sb);
    CHECK(!gp_subscription_active(app, sb), "still active after gp_unsubscribe");
    notify(m);
    flush();
    CHECK(node(b)->calls == 0, "unsubscribed observer was called");
    gp_unsubscribe(app, sb);                     /* twice */
    gp_unsubscribe(app, (gp_subscription){ 0 });
    gp_unsubscribe(app, (gp_subscription){ 0xdeadbeefcafeull });

    /* the pool slot is reused: the stale handle must not remove the new subscription */
    gp_subscription sb2 = gp_observe(&cb, m, on_observe, NULL);
    gp_unsubscribe(app, sb);
    CHECK(gp_subscription_active(app, sb2), "a stale handle removed somebody else's subscription");
    notify(m);
    flush();
    CHECK(node(b)->calls == 1, "reused slot: %d calls", node(b)->calls);
    gp_unsubscribe(app, sb2);

    /* unsubscribing from inside the callback; and one callback removing a later one before its turn */
    self_sub = gp_observe(&ca, m, on_observe_once, NULL);
    notify(m);
    flush();
    notify(m);
    flush();
    CHECK(node(a)->calls == 1, "self-unsubscribing observer ran %d times", node(a)->calls);

    /* the victim is subscribed first, so the killer (newest) is delivered first and removes it before its turn;
     * a new subscription then takes the victim's pool slot. Neither may be called for this notify. */
    gp_entity v = make('V', 0), k = make('K', 0);
    elsewhere = make('E', 0);
    gp_ctx cv = cx_of(v), ck = cx_of(k);
    victim_sub = gp_observe(&cv, m, on_observe, NULL);
    gp_observe(&ck, m, on_observe_unsub_victim, NULL);
    log_clear();
    notify(m);
    flush();
    CHECK(node(k)->calls == 1, "killer ran %d times", node(k)->calls);
    CHECK(node(v)->calls == 0, "an observer ran after an earlier one in the same delivery unsubscribed it");
    CHECK_LOG("");   /* and the subscription that reused its slot has not heard of this notify either */
    CHECK(!gp_subscription_active(app, victim_sub), "victim still active");
    notify(elsewhere);
    flush();
    CHECK_LOG("NEW");   /* ... but it is live */
    gp_entity_release(app, elsewhere);

    gp_entity all[] = { m, a, b, v, k };
    for (size_t i = 0; i < sizeof all / sizeof *all; i++) gp_entity_release(app, all[i]);
    flush();
}

typedef struct { int a, b; } Payload;

static void on_event(void *self, gp_ctx *cx, const gp_event *ev, void *data) {
    Node *me = self;
    me->calls++;
    char buf[64];
    if (ev->size == sizeof(Payload)) {
        const Payload *p = ev->payload;
        snprintf(buf, sizeof buf, "%c:%s:%d,%d", me->name, ev->name, p->a, p->b);
    } else {
        snprintf(buf, sizeof buf, "%c:%s:%zu", me->name, ev->name, ev->size);
    }
    log_add(buf);
}

static gp_entity late_target;
static void on_event_subscribe_more(void *self, gp_ctx *cx, const gp_event *ev, void *data) {
    Node *me = self;
    me->calls++;
    if (me->calls == 1) gp_subscribe(cx, late_target, NULL, on_event, NULL);   /* during the first delivery */
}

static void test_events(void) {
    gp_entity e = make('E', 0), s1 = make('1', 0), s2 = make('2', 0), s3 = make('3', 0);
    gp_ctx c1 = cx_of(s1), c2 = cx_of(s2), c3 = cx_of(s3), ce = cx_of(e);
    gp_subscribe(&c1, e, "alpha", on_event, NULL);
    gp_subscribe(&c2, e, NULL, on_event, NULL);          /* everything */
    gp_subscribe(&c3, e, "beta", on_event, NULL);

    log_clear();
    Payload p = { 4, 5 };
    gp_emit(&ce, "alpha", &p, sizeof p);
    p.a = 99;                                            /* the buffer is the emitter's: must have been copied */
    CHECK(node(s1)->calls == 0, "event delivered inside gp_emit");
    flush();
    CHECK(strstr(log_buf, "1:alpha:4,5") && strstr(log_buf, "2:alpha:4,5"), "log: %s", log_buf);
    CHECK(!strstr(log_buf, "3:"), "a subscriber to another name got the event: %s", log_buf);

    log_clear();
    gp_emit(&ce, "beta", NULL, 0);
    gp_emit_as(&ce, "alpha", Payload, .a = 1, .b = 2);
    flush();
    CHECK(strstr(log_buf, "3:beta:0") && strstr(log_buf, "2:beta:0") && strstr(log_buf, "1:alpha:1,2"), "log: %s", log_buf);
    CHECK(!strstr(log_buf, "1:beta"), "log: %s", log_buf);

    /* events and notifies are delivered in the order they were queued */
    gp_entity o = make('O', 0);
    gp_ctx co2 = cx_of(o);
    gp_observe(&co2, e, on_observe, NULL);
    log_clear();
    gp_entity_release(app, s2);                          /* keep the log short */
    gp_entity_release(app, s3);
    gp_emit(&ce, "alpha", &p, sizeof p);
    notify(e);
    gp_emit(&ce, "alpha", &p, sizeof p);
    flush();
    CHECK_LOG("1:alpha:99,5 O<E 1:alpha:99,5");

    /* an emitter with nobody listening, and a nameless emit, do nothing */
    gp_entity quiet = make('Q', 0);
    gp_ctx cq = cx_of(quiet);
    gp_emit(&cq, "alpha", &p, sizeof p);
    CHECK(!gp_effects_pending(app), "emit with no subscribers queued something");
    gp_emit(&ce, NULL, &p, sizeof p);
    CHECK(!gp_effects_pending(app), "emit without a name queued something");

    /* a subscription made during delivery waits for the next event */
    gp_entity maker = make('M', 0), src = make('S', 0);
    gp_ctx cm = cx_of(maker), cs = cx_of(src);
    late_target = src;
    gp_subscribe(&cm, src, "go", on_event_subscribe_more, NULL);   /* subscribes on_event, as `maker`, when it first runs */
    log_clear();
    gp_emit(&cs, "go", NULL, 0);
    flush();
    CHECK(node(maker)->calls == 1, "maker ran %d times", node(maker)->calls);
    CHECK(log_buf[0] == 0, "the new subscription got the event it was made during: %s", log_buf);
    gp_emit(&cs, "go", NULL, 0);
    flush();
    CHECK(strstr(log_buf, "M:go:0"), "the new subscription missed the next event: %s", log_buf);

    gp_entity all[] = { e, s1, o, quiet, maker, src };
    for (size_t i = 0; i < sizeof all / sizeof *all; i++) gp_entity_release(app, all[i]);
    flush();
}

typedef struct { int order_drop_ran, value_seen; gp_entity seen_self; int calls; } RelProbe;
static void on_release(void *self, gp_ctx *cx, gp_entity released, void *state, void *data) {
    RelProbe *p = data;
    Node *n = state;
    p->calls++;
    p->order_drop_ran = n->dropped;      /* the drop function runs after us */
    p->value_seen = n->value;
    log_add("rel");
    (void)self;
}

static void test_release(void) {
    gp_entity t = gp_entity_new(app, sizeof(Node), &(Node){ .name = 'T', .value = 42 }, node_drop);
    gp_entity a = make('A', 0), b = make('B', 0);
    gp_ctx ca = cx_of(a), cb = cx_of(b);
    RelProbe pa = { 0 }, pb = { 0 };
    gp_observe_release(&ca, t, on_release, &pa);
    gp_subscription sb = gp_observe_release(&cb, t, on_release, &pb);
    gp_entity_release(app, b);               /* b goes first: its callback never runs */
    CHECK(!gp_subscription_active(app, sb), "release observer outlived its owner");
    log_clear();
    gp_entity_release(app, t);
    CHECK(pa.calls == 1 && pb.calls == 0, "release observers: a %d, b %d", pa.calls, pb.calls);
    CHECK(pa.value_seen == 42 && !pa.order_drop_ran, "release observer saw value %d, drop already ran: %d", pa.value_seen, pa.order_drop_ran);
    CHECK_LOG("rel");
    CHECK(gp_entity_get(app, t) == NULL, "entity still there");

    /* one-shot: the entity is gone, and so is the subscription */
    notify(t);
    flush();
    CHECK(pa.calls == 1, "release observer called again");

    /* a release callback may release other entities and create new ones */
    gp_entity t2 = make('T', 0), r1 = make('R', 0), r2 = make('S', 0);
    gp_ctx cr1 = cx_of(r1);
    RelProbe pr = { 0 };
    gp_observe_release(&cr1, t2, on_release, &pr);
    gp_observe(&cr1, r2, on_observe, NULL);
    gp_entity_release(app, t2);
    gp_entity_release(app, r1);
    gp_entity_release(app, r2);
    gp_entity_release(app, a);
    flush();
    CHECK(pr.calls == 1, "second release probe: %d", pr.calls);
}

static gp_entity cyc_a, cyc_b;
static int cyc_calls;
static void cyc_cb(void *self, gp_ctx *cx, gp_entity observed, void *data) {
    cyc_calls++;
    gp_entity other = ((Node *)self)->name == 'A' ? cyc_b : cyc_a;
    notify(other);
    (void)cx; (void)data;
}

static void test_cycle_is_cut_off(void) {
    cyc_a = make('A', 0);
    cyc_b = make('B', 0);
    gp_ctx ca = cx_of(cyc_a), cb = cx_of(cyc_b);
    gp_observe(&ca, cyc_b, cyc_cb, NULL);   /* A hears B and notifies B; B hears A and notifies A */
    gp_observe(&cb, cyc_a, cyc_cb, NULL);
    cyc_calls = 0;
    fprintf(stderr, "(the next line is the cycle guard doing its job)\n");
    notify(cyc_a);
    flush();
    CHECK(cyc_calls > 1000 && cyc_calls <= 100001, "cycle ran %d times", cyc_calls);
    CHECK(!gp_effects_pending(app), "effects left after the cut-off");
    /* delivery works again afterwards: the notify flags were not left set */
    gp_entity m = make('M', 0), o = make('O', 0);
    gp_ctx co2 = cx_of(o);
    gp_observe(&co2, cyc_a, on_observe, NULL);
    notify(cyc_a);
    gp_entity_release(app, cyc_a);   /* breaks the cycle for the flush below */
    flush();
    gp_entity_release(app, cyc_b);
    gp_entity_release(app, m);
    gp_entity_release(app, o);
    flush();
}

static int reenter_calls, reenter_depth, reenter_max_depth, other_depth_seen = -1;
static gp_entity reenter_other;
static void other_observer(void *self, gp_ctx *cx, gp_entity observed, void *data) { other_depth_seen = reenter_depth; }
static void flush_inside(void *self, gp_ctx *cx, gp_entity observed, void *data) {
    reenter_calls++;
    reenter_max_depth = ++reenter_depth > reenter_max_depth ? reenter_depth : reenter_max_depth;
    if (reenter_calls == 1) notify(reenter_other);   /* real work is queued ... */
    gp_flush_effects(cx->app);                  /* ... and a flush from inside a flush must leave it for the outer one */
    if (reenter_calls < 3) notify(observed);    /* queued for another round of the same flush */
    ((Node *)self)->calls++;
    reenter_depth--;
}

static void test_reentrancy(void) {
    gp_entity m = make('M', 0), a = make('A', 0);
    gp_ctx ca = cx_of(a);
    gp_observe(&ca, m, flush_inside, NULL);
    reenter_other = make('X', 0);
    gp_ctx cx_other = cx_of(a);
    gp_observe(&cx_other, reenter_other, other_observer, NULL);
    reenter_calls = 0;
    other_depth_seen = -1;
    notify(m);
    CHECK(reenter_calls == 0, "delivered before the flush");
    gp_flush_effects(app);     /* synchronous flush from outside the loop */
    CHECK(reenter_calls == 3, "observer ran %d times; it re-notifies twice, so want 3", reenter_calls);
    CHECK(reenter_max_depth == 1, "an observer was re-entered (depth %d)", reenter_max_depth);
    CHECK(other_depth_seen == 0, "another entity's observer ran from inside the first one (depth %d): a nested flush delivered it", other_depth_seen);
    CHECK(!gp_effects_pending(app), "effects left behind");
    gp_entity_release(app, m);
    gp_entity_release(app, a);
    gp_entity_release(app, reenter_other);
    flush();
}

/* ------------------------------------------------------------ where the flushes happen */

typedef struct { gp_entity model; int clicks; } Root;

/* a layout handler that notifies: the observer must have run before the frame is painted */
static gp_entity probe_model;
static bool probe_armed, probe_fired, idle_armed, idle_fired;
static gp_entity idle_emitter;
static double idle_emit_t, idle_heard_t;
static int probe_value, probe_obs_draws = -1, probe_render_draws = -1;
static gp_entity root_entity, shared_model, shared_obs;
static int observed_at_second_timer = -1;
static int shown;

static int observed_at_click = -1;
static int observed_calls;
static void mouse_down(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    gp_ctx m = cx_of(shared_model);
    gp_notify(&m);
}
static void click(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    Root *r = self;
    r->clicks++;
    observed_at_click = observed_calls;   /* the press, handled before this event, already notified */
}

static void count_observed(void *self, gp_ctx *cx, gp_entity observed, void *data) {
    observed_calls++;
    Node *o = gp_entity_get(cx->app, observed);
    shown = o ? o->value : -1;   /* what the next render will show ... */
    gp_ctx rc = gp_ctx_for(cx->app, root_entity);
    gp_notify(&rc);              /* ... which only renders if the view that shows it is told */
}

static void on_idle(void *self, gp_ctx *cx, const gp_event *ev, void *data) { idle_heard_t = gp_time(); }
static void probe_layout(void *self, gp_ctx *cx, gp_size size, void *data) {
    if (idle_armed && !idle_fired) {   /* an emit does not repaint: nothing else will wake the loop */
        idle_fired = true;
        idle_emit_t = gp_time();
        gp_ctx ce = cx_of(idle_emitter);
        gp_emit(&ce, "idle", NULL, 0);
    }
    if (!probe_armed || probe_fired) return;
    probe_fired = true;
    notify(probe_model);
}
static void probe_observer(void *self, gp_ctx *cx, gp_entity observed, void *data) {
    probe_value = 555;
    probe_obs_draws = (int)cx->app->stat_draws;
    gp_ctx rc = gp_ctx_for(cx->app, root_entity);
    gp_notify(&rc);
}

static gp_node *root_view(void *self, gp_ctx *cx) {
    if (probe_value == 555 && probe_render_draws < 0) probe_render_draws = (int)cx->app->stat_draws;
    return gp_div(gp_size_full(), gp_on_mouse_down(mouse_down), gp_on_click(click), gp_on_layout(probe_layout),
                  gp_text(gp_fmt("shown=%d", shown)));
}

static void timer_notifies(void *self, gp_ctx *cx, void *data) { notify(shared_model); }
static void timer_looks(void *self, gp_ctx *cx, void *data) { observed_at_second_timer = observed_calls; }
static int observed_at_second_post = -1, observed_at_second_frame = -1;
static void post_notifies(void *self, gp_ctx *cx, void *data) { node(shared_model)->value = 77; notify(shared_model); }
static void post_looks(void *self, gp_ctx *cx, void *data) { observed_at_second_post = observed_calls; }
static void frame_notifies(void *self, gp_ctx *cx, double t, void *data) { notify(shared_model); }
static void frame_looks(void *self, gp_ctx *cx, double t, void *data) { observed_at_second_frame = observed_calls; }

static bool tree_has_text(gp_el *n, const char *text) {
    if (n->text && strcmp(n->text, text) == 0) return true;
    for (uint32_t i = 0; i < n->nchildren; i++) if (tree_has_text(n->children[i], text)) return true;
    return false;
}

static void test_flush_points(void) {
    shared_model = make('M', 0);
    shared_obs = make('A', 0);
    gp_ctx ca = cx_of(shared_obs);
    gp_observe(&ca, shared_model, count_observed, NULL);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);

    /* after each timer callback and before the next one runs */
    observed_calls = 0;
    gp_after(&cx, 0.05, timer_notifies, NULL);
    gp_after(&cx, 0.05, timer_looks, NULL);
    gp_sleep(co_, 0.25);
    CHECK(observed_calls == 1, "observer ran %d times after a timer's notify", observed_calls);
    CHECK(observed_at_second_timer == 1, "the observer had run %d times by the time the next timer fired (want 1)", observed_at_second_timer);

    /* after a posted function */
    observed_calls = 0;
    gp_post(app, GP_ENTITY_NULL, post_notifies, NULL);
    gp_post(app, GP_ENTITY_NULL, post_looks, NULL);
    gp_sleep(co_, 0.15);
    CHECK(observed_calls == 1 && shown == 77, "after gp_post: %d calls, shown %d", observed_calls, shown);
    CHECK(observed_at_second_post == 1, "the observer had run %d times when the next posted function ran (want 1)", observed_at_second_post);

    /* ... and after a frame callback */
    observed_calls = 0;
    gp_next_frame(&cx, frame_notifies, NULL);
    gp_next_frame(&cx, frame_looks, NULL);
    gp_sleep(co_, 0.2);
    CHECK(observed_calls == 1, "after a frame callback: %d observer calls", observed_calls);
    CHECK(observed_at_second_frame == 1, "the observer had run %d times when the next frame callback ran (want 1)", observed_at_second_frame);
    observed_calls = 0;
    CHECK(tree_has_text(app->windows[0]->tree, "shown=77"), "the frame drawn after the notify does not show what the observer set");

    /* after a platform event: a click whose handler notifies */
    observed_calls = 0;
    gp_simulate_mouse(app, GP_SIM_MOVE, 100, 100, 0, 0);
    gp_simulate_mouse(app, GP_SIM_DOWN, 100, 100, 1, 0);
    gp_simulate_mouse(app, GP_SIM_UP, 100, 100, 1, 0);
    gp_sleep(co_, 0.15);
    Root *r = gp_entity_get(app, root_entity);
    CHECK(r->clicks == 1 && observed_calls == 1, "after a click: %d clicks, %d observer calls", r->clicks, observed_calls);
    CHECK(observed_at_click == 1, "the observer had run %d times when the next platform event (the release) was handled (want 1)", observed_at_click);

    /* the observer's change is in the tree the same frame, with no render of the observer's own */
    node(shared_model)->value = 123;
    notify(shared_model);
    gp_sleep(co_, 0.15);
    CHECK(tree_has_text(app->windows[0]->tree, "shown=123"), "stale frame after a notify");

    /* a notify from a layout handler: its observer runs and the view re-renders before anything is painted */
    probe_model = make('P', 0);
    gp_entity probe_obs = make('Q', 0);
    gp_ctx cq = cx_of(probe_obs);
    gp_observe(&cq, probe_model, probe_observer, NULL);
    probe_armed = true;
    gp_ctx croot = cx_of(root_entity);
    gp_notify(&croot);                  /* a render and a layout, in which the handler fires */
    gp_sleep(co_, 0.2);
    CHECK(probe_fired && probe_value == 555, "layout-time notify: fired %d, value %d", probe_fired, probe_value);
    CHECK(probe_obs_draws >= 0 && probe_render_draws == probe_obs_draws,
          "the observer ran at draw %d but the view rendered its result at draw %d: a frame was painted in between",
          probe_obs_draws, probe_render_draws);
    gp_entity_release(app, probe_model);
    gp_entity_release(app, probe_obs);

    /* an effect queued while drawing, by something that does not repaint, must not wait for the next event */
    idle_emitter = make('I', 0);
    gp_entity idle_listener = make('J', 0);
    gp_ctx cj = cx_of(idle_listener);
    gp_subscribe(&cj, idle_emitter, "idle", on_idle, NULL);
    gp_sleep(co_, 0.05);
    idle_armed = true;
    gp_notify(&croot);                  /* one more layout, in which the handler emits */
    gp_sleep(co_, 0.4);
    CHECK(idle_fired && idle_heard_t > 0, "idle emit: fired %d, heard at %.3f", idle_fired, idle_heard_t);
    CHECK(idle_heard_t - idle_emit_t < 0.1, "an event emitted while drawing waited %.0f ms for the loop to wake", (idle_heard_t - idle_emit_t) * 1e3);
    gp_entity_release(app, idle_emitter);
    gp_entity_release(app, idle_listener);

    /* an idle app stays idle: a pending-effects check does not keep the loop spinning */
    unsigned long wake = app->stat_wakeups;
    gp_sleep(co_, 0.5);
    CHECK(app->stat_wakeups - wake < 15, "loop woke %lu times in an idle half second", app->stat_wakeups - wake);

    gp_entity_release(app, shared_model);
    gp_entity_release(app, shared_obs);
    flush();
}

/* ------------------------------------------------------------ randomized, against a model */

enum { NE = 12, MAXSUB = 400 };
typedef struct { int calls; } Count;
typedef struct {
    bool live;
    int owner, target;
    gp_subscription handle;
    Count *count;     /* calls the callback has made */
    int expect;       /* calls the model says it should have made */
} MSub;

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 11); }

static void count_cb(void *self, gp_ctx *cx, gp_entity observed, void *data) { ((Count *)data)->calls++; (void)self; }

static void test_random(void) {
    gp_entity ents[NE];
    bool alive[NE];
    for (int i = 0; i < NE; i++) { ents[i] = make((char)('a' + i), 0); alive[i] = true; }
    MSub subs[MAXSUB];
    int nsubs = 0;
    bool pending[NE] = { 0 };
    int bad = 0;

    for (int step = 0; step < 4000 && bad < 10; step++) {
        int op = (int)(rnd() % 10);
        if (op <= 3 && nsubs < MAXSUB) {                 /* observe */
            int o = (int)(rnd() % NE), t = (int)(rnd() % NE);
            gp_ctx cx = cx_of(ents[o]);
            MSub *s = &subs[nsubs++];
            *s = (MSub){ .owner = o, .target = t, .count = calloc(1, sizeof(Count)) };
            s->handle = gp_observe(&cx, ents[t], count_cb, s->count);
            s->live = alive[o] && alive[t];
            if (s->live != (s->handle.id != 0)) { CHECK(false, "step %d: observe returned %s for owner %d alive=%d target %d alive=%d", step, s->handle.id ? "a handle" : "none", o, alive[o], t, alive[t]); bad++; }
        } else if (op <= 5) {                            /* notify */
            int t = (int)(rnd() % NE);
            gp_ctx cx = cx_of(ents[t]);
            gp_notify(&cx);
            bool any = false;
            for (int i = 0; i < nsubs; i++) any |= subs[i].live && subs[i].target == t;
            if (alive[t] && any) pending[t] = true;
        } else if (op == 6 && nsubs) {                   /* unsubscribe */
            MSub *s = &subs[rnd() % nsubs];
            gp_unsubscribe(app, s->handle);
            s->live = false;
        } else if (op == 7) {                            /* release (and replace) an entity */
            int e = (int)(rnd() % NE);
            if (alive[e]) {
                gp_entity_release(app, ents[e]);
                alive[e] = false;
                pending[e] = false;
                for (int i = 0; i < nsubs; i++)
                    if (subs[i].owner == e || subs[i].target == e) subs[i].live = false;
            }
            if (!alive[e] && rnd() % 2) { ents[e] = make((char)('a' + e), 0); alive[e] = true; }
        } else {                                         /* flush */
            /* the model: every pending target delivers once to each live subscription on it */
            for (int t = 0; t < NE; t++) {
                if (!pending[t]) continue;
                for (int i = 0; i < nsubs; i++) if (subs[i].live && subs[i].target == t) subs[i].expect++;
                pending[t] = false;
            }
            gp_flush_effects(app);
            for (int i = 0; i < nsubs; i++) {
                if (subs[i].count->calls != subs[i].expect) {
                    CHECK(false, "step %d: subscription %d (owner %d -> target %d) called %d times, model says %d", step, i,
                          subs[i].owner, subs[i].target, subs[i].count->calls, subs[i].expect);
                    subs[i].expect = subs[i].count->calls;
                    bad++;
                }
                if (subs[i].live != gp_subscription_active(app, subs[i].handle)) {
                    CHECK(false, "step %d: subscription %d active=%d, model says %d", step, i, gp_subscription_active(app, subs[i].handle), subs[i].live);
                    bad++;
                }
            }
        }
    }
    for (int i = 0; i < NE; i++) if (alive[i]) gp_entity_release(app, ents[i]);
    for (int i = 0; i < nsubs; i++) free(subs[i].count);
    gp_flush_effects(app);
}

/* many observers of one entity, and a lot of notifies of entities nobody watches */
static void test_scale(void) {
    gp_entity m = make('M', 0);
    enum { N = 3000 };
    gp_entity *obs = malloc(N * sizeof *obs);
    gp_subscription *subs = malloc(N * sizeof *subs);
    for (int i = 0; i < N; i++) {
        obs[i] = make('o', 0);
        gp_ctx c = cx_of(obs[i]);
        subs[i] = gp_observe(&c, m, on_observe, NULL);
    }
    log_clear();
    notify(m);
    flush();
    int total = 0;
    for (int i = 0; i < N; i++) total += node(obs[i])->calls;
    CHECK(total == N, "%d observers, %d calls", N, total);
    for (int i = 0; i < N; i += 2) gp_entity_release(app, obs[i]);
    for (int i = 0; i < N; i++) if (i % 2) node(obs[i])->calls = 0;
    notify(m);
    flush();
    total = 0;
    for (int i = 1; i < N; i += 2) total += node(obs[i])->calls;
    CHECK(total == N / 2, "after releasing half: %d calls, want %d", total, N / 2);

    gp_entity lone = make('L', 0);
    gp_ctx cl = cx_of(lone);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 1000000; i++) gp_notify(&cl);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    CHECK(ms < 500, "a million notifies of an unobserved entity took %.0f ms", ms);
    CHECK(!gp_effects_pending(app), "effects queued for an unobserved entity");

    for (int i = 1; i < N; i += 2) gp_entity_release(app, obs[i]);
    gp_entity_release(app, m);
    gp_entity_release(app, lone);
    free(obs);
    free(subs);
    flush();
}

/* ------------------------------------------------------------ driver */

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    gp_sleep(co, 0.3);
    test_basics();
    test_chains_and_order();
    test_null_fn_rerenders_the_observer();
    test_lifetimes();
    test_unsubscribe();
    test_events();
    test_release();
    test_cycle_is_cut_off();
    test_reentrancy();
    test_flush_points();
    test_random();
    test_scale();
    gp_quit(app);
}

static void ready(gp_app *a_, void *userdata) {
    app = a_;
    root_entity = gp_view_new(app, sizeof(Root), &(Root){ 0 }, root_view, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "observe_test", .width = 300, .height = 200 }, root_entity);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);   /* a physical pointer over the window is not part of the test */
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "observe_test: %d FAILURE(S)\n" : "observe_test: all checks passed\n", failures);
    return failures != 0;
}
