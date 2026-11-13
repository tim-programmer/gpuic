/*
 * Self-checking test of timers, background jobs, posting, animation and coroutines.
 * Needs a display (it opens a tiny window to get an event loop). Exits non-zero on failure.
 *
 * One driver coroutine walks through the scenarios, so this is also a coroutine test.
 */
#include <gpuic/gpuic.h>

#include "threads.h"   /* (src: the threads the library itself uses) */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifndef _MSC_VER
#include <stdatomic.h>
#endif

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

/* Which thread this is: the address of something each thread has its own of. */
static _Thread_local char thread_mark;
static void *this_thread(void) { return &thread_mark; }
static void *main_thread;
#ifdef _WIN32
static void msleep(int ms) { Sleep((DWORD)ms); }
#else
static void msleep(int ms) { nanosleep(&(struct timespec){ 0, ms * 1000000L }, NULL); }
#endif

typedef struct { int fired, ticks; } Counter;

/* ------------------------------------------------------------ timers */

static void on_after(void *self, gp_ctx *cx, void *data) { ((Counter *)self)->fired++; }
static void on_tick(void *self, gp_ctx *cx, void *data) { ((Counter *)self)->ticks++; }

static void test_timers(gp_co *co, gp_app *app) {
    gp_entity e = gp_entity_new(app, sizeof(Counter), NULL, NULL);
    Counter *c = gp_entity_get(app, e);
    gp_ctx cx = gp_ctx_for(app, e);

    gp_task once = gp_after(&cx, 0.05, on_after, NULL);
    gp_task every = gp_every(&cx, 0.02, on_tick, NULL);
    CHECK(gp_task_active(app, once) && gp_task_active(app, every), "fresh tasks are active");
    gp_sleep(co, 0.25);
    CHECK(c->fired == 1, "one-shot fired %d times", c->fired);
    CHECK(c->ticks >= 5 && c->ticks <= 13, "every(20ms) ticked %d times in 250ms", c->ticks);
    CHECK(!gp_task_active(app, once), "one-shot is inactive after firing");

    gp_cancel(app, every);
    int ticks = c->ticks;
    gp_sleep(co, 0.1);
    CHECK(c->ticks == ticks, "cancelled timer kept ticking (%d -> %d)", ticks, c->ticks);
    gp_cancel(app, every);   /* stale handles are harmless */
    gp_cancel(app, (gp_task){ 0 });
    gp_cancel(app, (gp_task){ 999999 });

    /* released entity: its pending timer never fires */
    gp_after(&cx, 0.1, on_after, NULL);
    gp_entity_release(app, e);
    gp_sleep(co, 0.2);
    /* c is dangling now; the counter would have been touched if the timer ran (ASan would say) */
}

/* ------------------------------------------------------------ background */

typedef struct {
    void *worker;
    bool worker_ran, done_ran, done_on_main;
    int frees;
} Bg;

static void bg_work(void *p) { Bg *b = p; b->worker = this_thread(); b->worker_ran = true; msleep(30); }
static void bg_done(void *self, gp_ctx *cx, void *p) {
    Bg *b = p;
    b->done_ran = true;
    b->done_on_main = this_thread() == main_thread;
}
static void bg_free(void *p) { ((Bg *)p)->frees++; }

static void test_background(gp_co *co, gp_app *app) {
    gp_entity e = gp_entity_new(app, sizeof(Counter), NULL, NULL);
    gp_ctx cx = gp_ctx_for(app, e);

    Bg a = { 0 };
    gp_background(&cx, bg_work, bg_done, &a, bg_free);
    gp_sleep(co, 0.25);
    CHECK(a.worker_ran && a.worker != main_thread, "work did not run on a worker thread");
    CHECK(a.done_ran && a.done_on_main, "done must run on the main thread");
    CHECK(a.frees == 1, "free_data called %d times", a.frees);

    Bg b = { 0 };
    gp_task t = gp_background(&cx, bg_work, bg_done, &b, bg_free);
    gp_cancel(app, t);
    gp_sleep(co, 0.25);
    CHECK(!b.done_ran, "done ran for a cancelled job");
    CHECK(b.frees == 1, "cancelled job: free_data called %d times", b.frees);

    Bg d = { 0 };
    gp_background(&cx, bg_work, bg_done, &d, bg_free);
    gp_entity_release(app, e);
    gp_sleep(co, 0.25);
    CHECK(!d.done_ran, "done ran for a released entity");
    CHECK(d.frees == 1, "released entity: free_data called %d times", d.frees);
}

/* ------------------------------------------------------------ posting from threads */

static int posted;
static void on_post(void *self, gp_ctx *cx, void *data) {
    posted++;
    if (this_thread() != main_thread) failures++;
}
static void *poster(void *arg) {
    gp_app *app = arg;
    for (int i = 0; i < 100; i++) gp_post(app, GP_ENTITY_NULL, on_post, NULL);
    return NULL;
}

static void test_post(gp_co *co, gp_app *app) {
    gp_thread th[4];
    for (int i = 0; i < 4; i++) CHECK(gp_thread_create(&th[i], poster, app), "thread %d", i);
    for (int i = 0; i < 4; i++) gp_thread_join(th[i]);
    gp_sleep(co, 0.1);
    CHECK(posted == 400, "posted callbacks delivered: %d / 400", posted);

    gp_entity e = gp_entity_new(app, sizeof(Counter), NULL, NULL);
    gp_entity_release(app, e);
    gp_post(app, e, on_post, NULL);   /* to a dead entity: dropped */
    gp_sleep(co, 0.05);
    CHECK(posted == 400, "post to a released entity was delivered");
}

/* ------------------------------------------------------------ coroutines */

static int co_stage, co_cancelled_cleanup;
static bool co_saw_false;

static void sleeper(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_stage = 1;
    if (!gp_sleep(co, 0.5)) { co_saw_false = true; co_cancelled_cleanup++; return; }
    co_stage = 2;
}

typedef struct { bool started, finished; int value; double ended; } Slow;
static Slow slow;
static void slow_work(void *p) { Slow *s = p; s->started = true; msleep(120); s->value = 42; }
static void bg_waiter(gp_co *co, void *self, gp_ctx *cx, void *data) {
    Slow local = { 0 };                            /* lives on the coroutine's stack */
    bool ok = gp_await_background(co, slow_work, &local);
    slow.ended = gp_time();
    slow.value = local.value;                      /* reading it after a cancel is safe: see gp_co docs */
    slow.finished = ok;
    slow.started = local.started;
}

static bool bound_saw_false;
static void bound(gp_co *co, void *self, gp_ctx *cx, void *data) {
    if (!gp_sleep(co, 1.0)) bound_saw_false = true;   /* must not touch `self` here */
}

static double frames_seen[8];
static int nframes;
static void framer(gp_co *co, void *self, gp_ctx *cx, void *data) {
    double t;
    while (nframes < 8 && gp_await_frame(co, &t)) frames_seen[nframes++] = t;
}

static void test_coroutines(gp_co *co, gp_app *app) {
    gp_ctx app_cx = gp_ctx_for(app, GP_ENTITY_NULL);

    gp_task s = gp_spawn(&app_cx, sleeper, NULL);
    gp_sleep(co, 0.05);
    CHECK(co_stage == 1 && gp_task_active(app, s), "sleeper should be suspended (stage %d)", co_stage);
    gp_cancel(app, s);
    CHECK(co_saw_false && co_cancelled_cleanup == 1, "cancelled sleep must return false and unwind");
    CHECK(!gp_task_active(app, s), "cancelled coroutine is still active");
    gp_sleep(co, 0.1);
    CHECK(co_stage == 1, "cancelled coroutine kept running (stage %d)", co_stage);

    /* cancel while waiting for background work: it only unwinds once the work is done */
    gp_task w = gp_spawn(&app_cx, bg_waiter, NULL);
    gp_sleep(co, 0.04);
    double cancelled_at = gp_time();
    gp_cancel(app, w);
    CHECK(gp_task_active(app, w), "cancel while awaiting background work must wait for the worker");
    gp_sleep(co, 0.3);
    CHECK(!gp_task_active(app, w), "waiter should have unwound");
    CHECK(slow.started && slow.value == 42, "worker did not complete (started=%d value=%d)", slow.started, slow.value);
    CHECK(!slow.finished, "await after cancel must report false");
    CHECK(slow.ended - cancelled_at > 0.05, "waiter resumed %.0f ms after cancel; worker takes 120 ms",
          (slow.ended - cancelled_at) * 1000);

    /* a coroutine owned by an entity is cancelled when the entity is released */
    gp_entity e = gp_entity_new(app, sizeof(Counter), NULL, NULL);
    gp_ctx ecx = gp_ctx_for(app, e);
    gp_task b = gp_spawn(&ecx, bound, NULL);
    gp_sleep(co, 0.05);
    CHECK(gp_task_active(app, b), "bound coroutine should be sleeping");
    gp_entity_release(app, e);
    CHECK(bound_saw_false && !gp_task_active(app, b), "releasing the owner must cancel its coroutine");

    gp_spawn(&app_cx, framer, NULL);
    gp_sleep(co, 0.3);
    CHECK(nframes == 8, "await_frame resumed %d times", nframes);
    for (int i = 1; i < nframes; i++)
        CHECK(frames_seen[i] > frames_seen[i - 1], "frame times must increase");
}

/* ------------------------------------------------------------ animation */

typedef struct { int calls; float last; bool monotonic; } Anim;
static void anim_tick(void *self, gp_ctx *cx, float t, void *data) {
    Anim *a = data;
    if (t < a->last) a->monotonic = false;
    a->last = t;
    a->calls++;
}

static void test_animation(gp_co *co, gp_app *app) {
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    Anim a = { .monotonic = true };
    gp_task t = gp_animate(&cx, 0.1, GP_EASE_IN_OUT, anim_tick, &a);
    gp_sleep(co, 0.3);
    CHECK(a.calls >= 2, "animation ticked %d times", a.calls);
    CHECK(a.last == 1.0f, "final value %f, expected exactly 1", a.last);
    CHECK(a.monotonic, "ease-in-out must be monotonic");
    CHECK(!gp_task_active(app, t), "finished animation is still active");

    Anim b = { .monotonic = true };
    gp_task u = gp_animate(&cx, 1.0, GP_EASE_LINEAR, anim_tick, &b);
    gp_sleep(co, 0.1);
    gp_cancel(app, u);
    int calls = b.calls;
    gp_sleep(co, 0.1);
    CHECK(b.calls == calls && b.last < 1.0f, "cancelled animation kept running");

    CHECK(gp_ease(GP_EASE_OUT_BOUNCE, 0) == 0 && gp_ease(GP_EASE_OUT_BOUNCE, 1) == 1, "easing endpoints");
    CHECK(gp_ease(GP_EASE_OUT_BACK, 1) == 1 && gp_ease(GP_EASE_IN, 0.5f) < 0.5f, "easing shapes");
}

/* ------------------------------------------------------------ spring easing */

/* The spring against its own differential equation, x'' = -2 z w x' - w^2 (x - 1) from rest, integrated here with RK4 in double precision
 * (w as the header says: the envelope e^(-z w) is a thousandth at t = 1). */
static void spring_ode(double z, double w, double t_end, double *xs, int n) {
    double x = 0, v = 0, dt = t_end / (n * 1000.0);
    for (int k = 0; k < n; k++) {
        for (int i = 0; i < 1000; i++) {
            double k1x = v, k1v = -2 * z * w * v - w * w * (x - 1);
            double k2x = v + dt / 2 * k1v, k2v = -2 * z * w * (v + dt / 2 * k1v) - w * w * (x + dt / 2 * k1x - 1);
            double k3x = v + dt / 2 * k2v, k3v = -2 * z * w * (v + dt / 2 * k2v) - w * w * (x + dt / 2 * k2x - 1);
            double k4x = v + dt * k3v, k4v = -2 * z * w * (v + dt * k3v) - w * w * (x + dt * k3x - 1);
            x += dt / 6 * (k1x + 2 * k2x + 2 * k3x + k4x);
            v += dt / 6 * (k1v + 2 * k2v + 2 * k3v + k4v);
        }
        xs[k] = x;
    }
}

static void test_spring_easing(void) {
    const double zs[] = { 0.25, 0.5, 0.8, 1.0 };
    for (int zi = 0; zi < 4; zi++) {
        double z = zs[zi], w = z >= 1 ? 9.2336 : 6.9077553 / z;
        double xs[100];
        spring_ode(z, w, 1.0, xs, 100);   /* xs[k] is the position at t = (k + 1) / 100 */
        double worst = 0, peak = 0, peak_got = 0;
        bool monotone = true;
        float prev = 0;
        for (int k = 0; k < 100; k++) {
            float got = gp_ease_spring((float)(k + 1) / 100.f, (float)z);
            if (k < 99) worst = fmax(worst, fabs(got - xs[k]));
            peak = fmax(peak, xs[k]);
            peak_got = fmax(peak_got, got);
            if (z >= 1 && got < prev - 1e-6f) monotone = false;
            prev = got;
        }
        CHECK(worst < 0.003, "spring z=%.2f follows its equation (worst difference %.5f)", z, worst);
        CHECK(fabs(peak_got - peak) < 0.003, "spring z=%.2f overshoots as the equation does (%.4f, %.4f)", z, peak_got, peak);
        if (z < 1) CHECK(fabs(peak - (1 + exp(-M_PI * z / sqrt(1 - z * z)))) < 0.003, "spring z=%.2f: the peak is the textbook 1 + e^(-pi z / sqrt(1 - z^2)) (%.4f)", z, peak);
        else CHECK(monotone && peak_got < 1.0005, "a critically damped spring does not overshoot (%.5f) and only rises", peak_got);
        CHECK(fabsf(gp_ease_spring(0.9999f, (float)z) - 1) < 2e-4f, "spring z=%.2f arrives without a jump at the end (%.5f)", z, gp_ease_spring(0.9999f, (float)z));
        CHECK(gp_ease_spring(0, (float)z) == 0 && gp_ease_spring(1, (float)z) == 1 && gp_ease_spring(-1, (float)z) == 0 && gp_ease_spring(2, (float)z) == 1, "spring endpoints z=%.2f", z);
    }
    CHECK(gp_ease(GP_EASE_SPRING, 0.3f) == gp_ease_spring(0.3f, 0.5f) && gp_ease(GP_EASE_SPRING_BOUNCY, 0.3f) == gp_ease_spring(0.3f, 0.25f), "the two named springs");
    CHECK(gp_ease(GP_EASE_SPRING, 1) == 1 && gp_ease(GP_EASE_SPRING_BOUNCY, 1) == 1 && gp_ease(GP_EASE_SPRING, 0) == 0, "named spring endpoints");
    CHECK(gp_ease_spring(0.3f, 5.f) == gp_ease_spring(0.3f, 1.f) && gp_ease_spring(0.3f, 0.f) == gp_ease_spring(0.3f, 0.1f), "damping is clamped to 0.1 .. 1");
    CHECK(gp_ease_spring(0.12f, 0.25f) > 1.f && gp_ease_spring(0.12f, 1.f) < 1.f, "the loose spring has overshot by 0.12, the critically damped one has not arrived");
}

/* ------------------------------------------------------------ shutdown */

static bool shutdown_unwound;
#ifdef _MSC_VER   /* (its <stdatomic.h> is behind an experimental switch) */
static volatile bool shutdown_work_ran;
#else
static atomic_bool shutdown_work_ran; /* written by two pool threads at once */
#endif
static int shutdown_frees;
static void long_work(void *p) { shutdown_work_ran = true; msleep(200); }
static void long_free(void *p) { shutdown_frees++; }
static void shutdown_waiter(gp_co *co, void *self, gp_ctx *cx, void *data) {
    if (!gp_await_background(co, long_work, NULL)) shutdown_unwound = true;
}
static void noop_done(void *self, gp_ctx *cx, void *data) { }

/* ------------------------------------------------------------ springs (gp_spring) */

/* x'' = -k (x - target) - 2 z sqrt(k) x' by RK4 with a tiny step, from (x, v): the reference the closed-form stepping is held to */
static void rk4(double *x, double *v, double target, double k, double z, double t_end) {
    int n = (int)ceil(t_end / 1e-5);
    double dt = t_end / n, w = sqrt(k);   /* (whole steps, or every frame would be a little long or short and the error would add up) */
    for (int i = 0; i < n; i++) {
        double a1 = -k * (*x - target) - 2 * z * w * *v;
        double x2 = *x + *v * dt / 2, v2 = *v + a1 * dt / 2, a2 = -k * (x2 - target) - 2 * z * w * v2;
        double x3 = *x + v2 * dt / 2, v3 = *v + a2 * dt / 2, a3 = -k * (x3 - target) - 2 * z * w * v3;
        double x4 = *x + v3 * dt, v4 = *v + a3 * dt, a4 = -k * (x4 - target) - 2 * z * w * v4;
        *x += dt / 6 * (*v + 2 * v2 + 2 * v3 + v4);
        *v += dt / 6 * (a1 + 2 * a2 + 2 * a3 + a4);
    }
}

static void test_spring_math(void) {
    const double zs[] = { 0.3, 1.0, 2.5 };
    for (int zi = 0; zi < 3; zi++) {
        double z = zs[zi];
        gp_spring s = gp_spring_make(0, 200, (float)z);
        s.target = 100;
        double ox = 0, ov = 0, worst = 0, worstv = 0;
        for (int step = 1; step <= 60; step++) {   /* frames of 1/60 s for a second */
            gp_spring_step(&s, 1.0 / 60);
            rk4(&ox, &ov, 100, 200, z, 1.0 / 60);
            worst = fmax(worst, fabs(s.value - ox));
            worstv = fmax(worstv, fabs(s.velocity - ov));
        }
        CHECK(worst < 0.01 && worstv < 0.1, "spring z=%.1f follows its equation (position off by %.4f, velocity by %.4f)", z, worst, worstv);
        /* one long step is the same as many short ones: the solution is exact, not integrated */
        gp_spring a = gp_spring_make(0, 200, (float)z), b = a;
        a.target = b.target = 100;
        gp_spring_step(&a, 0.4);
        for (int i = 0; i < 400; i++) gp_spring_step(&b, 0.001);
        CHECK(fabsf(a.value - b.value) < 0.01f && fabsf(a.velocity - b.velocity) < 0.1f, "spring z=%.1f: one step of 0.4 s is 400 steps of a millisecond (%.4f vs %.4f)", z, a.value, b.value);
        /* it settles, on the target exactly, and says so */
        gp_spring c = gp_spring_make(0, 200, (float)z);
        c.target = 100;
        int n = 0;
        while (gp_spring_step(&c, 1.0 / 60) && n < 2000) n++;
        CHECK(n < 2000 && c.value == 100.f && c.velocity == 0 && gp_spring_settled(&c), "spring z=%.1f settles on the target after %d frames (%g, %g)", z, n, c.value, c.velocity);
    }
    {   /* the overshoot of an underdamped spring is the textbook one: exp(-pi z / sqrt(1 - z^2)) of the distance */
        gp_spring s = gp_spring_make(0, 400, 0.25f);
        s.target = 100;
        float peak = 0;
        for (int i = 0; i < 2000; i++) { gp_spring_step(&s, 0.001); if (s.value > peak) peak = s.value; }
        double want = 100 * (1 + exp(-M_PI * 0.25 / sqrt(1 - 0.0625)));
        CHECK(fabs(peak - want) < 0.5, "overshoot of z=0.25: peak %.2f, textbook %.2f", peak, want);
        gp_spring crit = gp_spring_make(0, 400, 1.f);
        crit.target = 100;
        float cpeak = 0;
        for (int i = 0; i < 2000; i++) { gp_spring_step(&crit, 0.001); if (crit.value > cpeak) cpeak = crit.value; }
        CHECK(cpeak <= 100.001f, "critically damped does not overshoot (%.4f)", cpeak);
    }
    {   /* passing through the target fast is not being settled; a jump puts a moving one at rest */
        gp_spring s = gp_spring_make(100, 200, 0.3f);
        s.velocity = 500;
        CHECK(!gp_spring_settled(&s), "on the target but moving is not settled");
        CHECK(gp_spring_step(&s, 0.01) && s.value > 100.5f, "...and a step takes it past (%.2f)", s.value);
        gp_spring_jump(&s, 3);
        CHECK(s.value == 3.f && s.target == 3.f && s.velocity == 0 && gp_spring_settled(&s), "a jump puts a moving spring at rest where it was told (%g, %g, %g)", s.value, s.target, s.velocity);
    }
    {   /* a new target in mid-flight keeps the position and the velocity, and the motion carries on from there (RK4 with the new target) */
        gp_spring s = gp_spring_make(0, 150, 0.4f);
        s.target = 100;
        double ox = 0, ov = 0;
        for (int i = 0; i < 12; i++) { gp_spring_step(&s, 1.0 / 60); rk4(&ox, &ov, 100, 150, 0.4, 1.0 / 60); }
        float x_before = s.value, v_before = s.velocity;
        CHECK(v_before > 20.f, "(the spring is moving: %.1f)", v_before);
        s.target = 30;
        CHECK(s.value == x_before && s.velocity == v_before, "a new target changes neither position nor velocity");
        double worst = 0;
        for (int i = 0; i < 90; i++) {
            gp_spring_step(&s, 1.0 / 60);
            rk4(&ox, &ov, 30, 150, 0.4, 1.0 / 60);
            worst = fmax(worst, fabs(s.value - ox));
        }
        CHECK(worst < 0.02, "after retargeting it follows the equation with the new target (off by %.4f)", worst);
        /* what a curve restarted from rest would do instead: the first frame after the change would go the other way or stand still */
        gp_spring t = gp_spring_make(0, 150, 0.4f);
        t.target = 100;
        for (int i = 0; i < 12; i++) gp_spring_step(&t, 1.0 / 60);
        float before = t.value;
        t.target = 30;
        gp_spring_step(&t, 1.0 / 60);
        CHECK(t.value > before, "still moving up for a frame after the target was moved down (velocity carried: %.2f -> %.2f)", before, t.value);
    }
}

static gp_spring g_sp;
static gp_node *view(void *self, gp_ctx *cx);
static unsigned long g_renders;   /* how many times the window's view was built: every gp_notify of the whole app builds it again */
static void test_spring_frames(gp_co *co, gp_app *app) {
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    g_sp = gp_spring_make(0, 300, 0.5f);
    unsigned long draws0 = g_renders;
    gp_spring_to(&cx, &g_sp, 100);
    CHECK(g_sp.running && g_sp.target == 100, "gp_spring_to starts it");
    float last = 0, vmax = 0;
    bool retargeted = false;
    float v_at = 0, v_after = 0;
    for (int i = 0; i < 600 && g_sp.running; i++) {
        gp_sleep(co, 0.005);
        float v = g_sp.velocity;
        if (!retargeted && g_sp.value > 60) {   /* mid-flight, and fast: now go somewhere else */
            v_at = v;
            gp_spring_to(&cx, &g_sp, 20);
            retargeted = true;
            CHECK(g_sp.running, "retargeting leaves it running (one frame job, not two)");
        } else if (retargeted && v_after == 0) {
            v_after = v;
        }
        if (fabsf(v) > vmax) vmax = fabsf(v);
        last = g_sp.value;
    }
    CHECK(!g_sp.running && g_sp.value == 20.f && g_sp.velocity == 0, "it ends on the new target, at rest, and stops asking for frames (%g, %g, running %d)", g_sp.value, g_sp.velocity, g_sp.running);
    CHECK(retargeted && v_at > 100.f, "it was fast when it was retargeted (%.0f)", v_at);
    CHECK(v_after > 0.4f * v_at, "and the velocity went on from there, not from rest (%.0f then %.0f)", v_at, v_after);
    CHECK(g_renders - draws0 > 10, "its view was told every frame (%lu renders)", g_renders - draws0);
    (void)last;
    gp_sleep(co, 0.1);
    unsigned long idle0 = g_renders;
    gp_sleep(co, 0.3);
    CHECK(g_renders - idle0 <= 1, "nothing is built once it has settled (%lu)", g_renders - idle0);
    gp_spring to_self = gp_spring_make(5, 100, 1.f);
    gp_spring_to(&cx, &to_self, 5);
    CHECK(!to_self.running, "a spring already there is not started");
    gp_spring_to(&cx, &to_self, 9);
    CHECK(to_self.running, "...and one that has to move is");
    gp_spring_jump(&to_self, 3);
    gp_sleep(co, 0.1);
    CHECK(!to_self.running && to_self.value == 3.f && to_self.velocity == 0, "a jump puts it at rest where it was put and its job ends (%g, running %d)", to_self.value, to_self.running);
}

/* ------------------------------------------------------------ driver */

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    gp_app *app = cx->app;
    test_timers(co, app);
    test_background(co, app);
    test_post(co, app);
    test_coroutines(co, app);
    test_animation(co, app);
    test_spring_frames(co, app);

    /* leave work in flight and quit: shutdown must join the pool and unwind everything */
    gp_spawn(cx, shutdown_waiter, NULL);
    gp_background(cx, long_work, noop_done, NULL, long_free);
    gp_sleep(co, 0.05);
    gp_quit(app);
}

static gp_node *view(void *self, gp_ctx *cx) { g_renders++; return gp_div(gp_size_full()); }

static void ready(gp_app *app, void *userdata) {
    gp_entity root = gp_view_new(app, 0, NULL, view, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "exec_test", .width = 200, .height = 100 }, root);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);   /* a physical pointer over the window is not part of the test */
    main_thread = this_thread();
    test_spring_easing();
    test_spring_math();
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    CHECK(shutdown_work_ran, "shutdown scenario never started its work");
    CHECK(shutdown_unwound, "coroutine waiting on a worker was not unwound at shutdown");
    CHECK(shutdown_frees == 1, "shutdown: free_data called %d times", shutdown_frees);
    printf(failures ? "exec_test: %d FAILURE(S)\n" : "exec_test: all checks passed\n", failures);
    return failures != 0;
}
