/*
 * Timers and async, one card per mechanism:
 *   stopwatch   gp_every           repeating timer
 *   fetch       gp_spawn           coroutine: gp_sleep + gp_await_background, spinner via gp_await_frame
 *   job         gp_background      worker thread reporting progress with gp_post
 *   slide       gp_animate         eased animation, interruptible
 *   toast       gp_after           one-shot timer, replaced when retriggered
 */
#include <gpuic/gpuic.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define BASE     gp_rgb(0x1e1e2e)
#define SURFACE  gp_rgb(0x313244)
#define MANTLE   gp_rgb(0x181825)
#define OVERLAY  gp_rgb(0x45475a)
#define TEXT     gp_rgb(0xcdd6f4)
#define SUBTEXT  gp_rgb(0x9399b2)
#define BLUE     gp_rgb(0x89b4fa)
#define BLUE_HI  gp_rgb(0xa6c4fb)
#define BLUE_LO  gp_rgb(0x6c9ef0)
#define GREEN    gp_rgb(0xa6e3a1)
#define RED      gp_rgb(0xf38ba8)
#define PEACH    gp_rgb(0xfab387)

enum { FETCH_IDLE, FETCH_LOADING, FETCH_DONE, FETCH_CANCELLED };

typedef struct {
    gp_app *app;
    gp_entity self;

    double elapsed, last;          /* stopwatch */
    gp_task tick;

    int fetch;                     /* coroutine demo */
    gp_task fetch_task;
    char fetch_msg[160];
    double phase;

    int job_pct;                   /* callback demo */
    bool job_running;
    gp_task job_task;

    float box_x, box_from, box_to; /* animation demo, 0..1 */
    gp_task box_anim;

    char toast[80];
    gp_task toast_task;
} Demo;

/* ---------------------------------------------------------------- toast: gp_after */

static void hide_toast(void *self, gp_ctx *cx, void *data) {
    ((Demo *)self)->toast[0] = 0;
    gp_notify(cx);
}

static void toast(Demo *d, gp_ctx *cx, const char *msg) {
    snprintf(d->toast, sizeof d->toast, "%s", msg);
    gp_cancel(cx->app, d->toast_task);              /* retriggering restarts the countdown */
    d->toast_task = gp_after(cx, 1.8, hide_toast, NULL);
    gp_notify(cx);
}

/* ---------------------------------------------------------------- stopwatch: gp_every */

static void on_tick(void *self, gp_ctx *cx, void *data) {
    Demo *d = self;
    double now = gp_time();
    d->elapsed += now - d->last;
    d->last = now;
    gp_notify(cx);
}

static void sw_toggle(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    Demo *d = self;
    if (gp_task_active(cx->app, d->tick)) {
        gp_cancel(cx->app, d->tick);
    } else {
        d->last = gp_time();
        d->tick = gp_every(cx, 0.05, on_tick, NULL);
    }
    gp_notify(cx);
}

static void sw_reset(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    ((Demo *)self)->elapsed = 0;
    ((Demo *)self)->last = gp_time();
    gp_notify(cx);
}

/* ---------------------------------------------------------------- fetch: coroutines */

typedef struct { int limit, count; double ms; } Primes;

static void count_primes(void *p) { /* runs on a pool thread: touches nothing but its argument */
    Primes *w = p;
    double t0 = gp_time();
    int count = 0;
    for (int n = 2; n < w->limit; n++) {
        bool prime = true;
        for (int k = 2; (long)k * k <= n; k++)
            if (n % k == 0) { prime = false; break; }
        count += prime;
    }
    w->count = count;
    w->ms = (gp_time() - t0) * 1000;
}

static void fetch_co(gp_co *co, void *self, gp_ctx *cx, void *data) {
    Demo *d = self;
    d->fetch = FETCH_LOADING;
    snprintf(d->fetch_msg, sizeof d->fetch_msg, "Connecting…");
    gp_notify(cx);

    if (!gp_sleep(co, 0.5)) return;                        /* cancelled: don't touch `self` again */
    snprintf(d->fetch_msg, sizeof d->fetch_msg, "Crunching on a worker thread…");
    gp_notify(cx);

    Primes work = { .limit = 4000000 };                    /* lives on this stack: safe, see gp_co docs */
    if (!gp_await_background(co, count_primes, &work)) return;

    d->fetch = FETCH_DONE;
    snprintf(d->fetch_msg, sizeof d->fetch_msg, "%d primes below %d, found in %.0f ms",
             work.count, work.limit, work.ms);
    toast(d, cx, "Fetch complete");
}

static void spinner_co(gp_co *co, void *self, gp_ctx *cx, void *data) {
    Demo *d = self;
    double t;
    while (gp_await_frame(co, &t)) {                        /* one iteration per frame */
        if (d->fetch != FETCH_LOADING) break;
        d->phase = t;
        gp_notify(cx);
    }
}

static void fetch_start(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    Demo *d = self;
    if (gp_task_active(cx->app, d->fetch_task)) return;
    d->fetch_task = gp_spawn(cx, fetch_co, NULL);
    gp_spawn(cx, spinner_co, NULL);
}

static void fetch_cancel(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    Demo *d = self;
    if (!gp_task_active(cx->app, d->fetch_task)) return;
    gp_cancel(cx->app, d->fetch_task);                      /* the awaits return false and the body unwinds */
    d->fetch = FETCH_CANCELLED;
    snprintf(d->fetch_msg, sizeof d->fetch_msg, "Cancelled");
    gp_notify(cx);
}

/* ---------------------------------------------------------------- job: gp_background + gp_post */

typedef struct { gp_app *app; gp_entity entity; int steps; } Job;

static void job_progress(void *self, gp_ctx *cx, void *data) { /* main thread */
    Demo *d = self;
    if (!d->job_running) return;
    d->job_pct = (int)(intptr_t)data;
    gp_notify(cx);
}

static void job_work(void *p) { /* pool thread: report progress via the one thread-safe call */
    Job *j = p;
    for (int i = 1; i <= j->steps; i++) {
        nanosleep(&(struct timespec){ 0, 40 * 1000 * 1000 }, NULL);
        gp_post(j->app, j->entity, job_progress, (void *)(intptr_t)(i * 100 / j->steps));
    }
}

static void job_done(void *self, gp_ctx *cx, void *data) {
    Demo *d = self;
    d->job_running = false;
    toast(d, cx, "Background job finished");
}

static void job_start(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    Demo *d = self;
    if (d->job_running) {
        gp_cancel(cx->app, d->job_task);                    /* `done` will not run; free_data still does */
        d->job_running = false;
        toast(d, cx, "Job cancelled");
        return;
    }
    Job *j = malloc(sizeof *j);
    *j = (Job){ cx->app, cx->entity, 25 };
    d->job_pct = 0;
    d->job_running = true;
    d->job_task = gp_background(cx, job_work, job_done, j, free);
    gp_notify(cx);
}

/* ---------------------------------------------------------------- slide: gp_animate */

static void slide_tick(void *self, gp_ctx *cx, float t, void *data) {
    Demo *d = self;
    d->box_x = gp_lerp(d->box_from, d->box_to, t);
    gp_notify(cx);
}

static void slide(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    Demo *d = self;
    gp_cancel(cx->app, d->box_anim);                        /* interrupt, continue from where it is */
    d->box_from = d->box_x;
    d->box_to = d->box_to > 0.5f ? 0.f : 1.f;
    d->box_anim = gp_animate(cx, 0.8, GP_EASE_OUT_BOUNCE, slide_tick, NULL);
}

/* ---------------------------------------------------------------- view */

static gp_node *button(const char *label, gp_color bg, gp_node *on_click) {
    return gp_div(gp_px(14), gp_py(8), gp_rounded(8), gp_bg(bg), gp_hover_bg(BLUE_HI),
                  gp_active_bg(BLUE_LO), gp_text_color(BASE), gp_cursor_pointer(), on_click,
                  gp_text(label, gp_font_bold()));
}

static gp_node *card(const char *title, const char *how, gp_node *a, gp_node *b, gp_node *c) {
    return gp_div(
        gp_flex_col(), gp_gap(10), gp_p(16), gp_rounded(12), gp_bg(SURFACE),
        gp_div(gp_flex_row(), gp_items_center(), gp_gap(10),
               gp_text(title, gp_font_bold()),
               gp_text(how, gp_text_size(12), gp_text_color(SUBTEXT))),
        a, b, c);
}

static gp_node *bar(float frac, gp_color fill) {
    return gp_div(gp_h(8), gp_w_full(), gp_rounded_full(), gp_bg(MANTLE), gp_overflow_hidden(),
                  gp_div(gp_h(8), gp_w_frac(frac), gp_bg(fill)));
}

static gp_node *spinner(double phase) {
    float x = (0.5f + 0.5f * sinf((float)phase * 5.f));
    return gp_div(gp_h(8), gp_w_full(), gp_rounded_full(), gp_bg(MANTLE), gp_overflow_hidden(),
                  gp_flex_row(),
                  gp_div(gp_w_frac(x * 0.8f), gp_h(8), gp_flex_none()),
                  gp_div(gp_w_frac(0.2f), gp_h(8), gp_flex_none(), gp_bg(BLUE)));
}

static gp_node *demo_render(void *self, gp_ctx *cx) {
    Demo *d = self;
    bool running = gp_task_active(cx->app, d->tick);
    bool loading = d->fetch == FETCH_LOADING;
    gp_color status = d->fetch == FETCH_DONE ? GREEN : d->fetch == FETCH_CANCELLED ? RED : SUBTEXT;

    return gp_div(
        gp_size_full(), gp_flex_col(), gp_gap(14), gp_p(22), gp_bg(BASE), gp_text_color(TEXT),
        gp_text("Timers & async", gp_text_size(22), gp_font_bold()),

        card("Stopwatch", "gp_every",
             gp_div(gp_flex_row(), gp_items_center(), gp_gap(12),
                    gp_text(gp_fmt("%.2f s", d->elapsed), gp_text_size(28), gp_font_bold(), gp_w(150)),
                    button(running ? "Stop" : "Start", running ? PEACH : BLUE, gp_on_click(sw_toggle)),
                    button("Reset", BLUE, gp_on_click(sw_reset))),
             NULL, NULL),

        card("Fetch", "gp_spawn · gp_sleep · gp_await_background · gp_await_frame",
             gp_div(gp_flex_row(), gp_gap(10),
                    button("Fetch", BLUE, gp_on_click(fetch_start)),
                    button("Cancel", loading ? RED : OVERLAY, gp_on_click(fetch_cancel))),
             d->fetch_msg[0] ? gp_text(d->fetch_msg, gp_text_color(status), gp_text_size(13)) : NULL,
             loading ? spinner(d->phase) : NULL),

        card("Background job", "gp_background · gp_post from the worker",
             gp_div(gp_flex_row(), gp_items_center(), gp_gap(12),
                    button(d->job_running ? "Cancel job" : "Run job", d->job_running ? RED : BLUE,
                           gp_on_click(job_start)),
                    gp_text(gp_fmt("%d%%", d->job_pct), gp_text_color(SUBTEXT))),
             bar((float)d->job_pct / 100.f, BLUE), NULL),

        card("Animation", "gp_animate · GP_EASE_OUT_BOUNCE · interruptible",
             gp_div(gp_flex_row(), gp_items_center(), gp_gap(12),
                    button("Slide", BLUE, gp_on_click(slide))),
             gp_div(gp_h(52), gp_w_full(), gp_rounded(10), gp_bg(MANTLE), gp_flex_row(),
                    gp_div(gp_w_frac(d->box_x * 0.86f), gp_h(52), gp_flex_none()),
                    gp_div(gp_w(44), gp_h(44), gp_flex_none(), gp_mt(4), gp_ml(4), gp_rounded(10),
                           gp_bg(BLUE))),
             NULL),

        gp_div(gp_flex_1()),
        d->toast[0] ? gp_div(gp_self_center(), gp_px(16), gp_py(8), gp_rounded(8), gp_bg(OVERLAY),
                             gp_text(d->toast))
                    : gp_div(gp_h(34)));
}

static void ready(gp_app *app, void *userdata) {
    Demo init = { .app = app };
    gp_entity root = gp_view_new(app, sizeof init, &init, demo_render, NULL);
    ((Demo *)gp_entity_get(app, root))->self = root;
    gp_open_window(app, &(gp_window_options){ .title = "gpuic — timers & async", .width = 640,
                                              .height = 720, .background = BASE },
                   root);
}

int main(void) { return gp_application_run(ready, NULL); }
