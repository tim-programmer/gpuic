/*
 * Executors: timers, animation frames, a thread-safe post queue and a background thread
 * pool, all driven from the main event loop.
 *
 * Everything in flight is a gp_job in one registry, addressed by id (gp_task), so handles
 * can go stale safely. Job pointers are stable until gp_exec_run()'s final sweep, which is
 * the only place jobs are freed, so callbacks may freely add and cancel jobs.
 *
 * Threads touch exactly three things, all under `lock`: the post queue, the work queue and
 * the completion list. Results travel back to the main thread as completion ids plus a wake
 * event; the main thread alone ever looks at jobs.
 */
#include "internal.h"

#include "threads.h"

#include <errno.h>
#include <time.h>
#ifdef GP_HAS_FD
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#endif

enum { JOB_TIMER = 1, JOB_FRAME, JOB_ANIM, JOB_BG, JOB_CORO, JOB_FD };

struct gp_job {
    uint64_t id;
    uint8_t kind;
    bool cancelled, finished;
    gp_entity owner;
    gp_window *window;

    int64_t due_ns, interval_ns, start_ns;   /* timers / animations */
    gp_task_fn fn;                           /* timer callback, or background `done` */
    gp_frame_fn frame_fn;
    gp_anim_fn anim_fn;
    int easing;
    double duration;
    void *data;

    gp_work_fn work;                         /* background */
    gp_drop_fn free_data;
    gp_drop_fn drop;                         /* gp_task_on_drop: called once, when the job is removed */
    void *drop_data;
    bool bg_running;

    gp_co *co;                               /* JOB_CORO */
    uint64_t co_job;                         /* await points: coroutine job to resume */
    double *time_out;                        /* await_frame: where to store the frame time */

    int fd, fd_events;                       /* JOB_FD: what is watched (the caller's descriptor) */
    gp_fd_fn fd_fn;                          /* callback watch; a coroutine's await has co_job instead */
    int *revents_out;                        /* await: where to store what happened (0 = timed out) */
    bool oneshot;                            /* await: finished after one delivery */
};

typedef struct { gp_entity entity; gp_task_fn fn; void *data; } post_item;
/* One watched descriptor. `armed` = the poller thread is looking at it; it is cleared when it reports readiness and
 * set again by the main thread once the callback has run, which is what makes a watch level-triggered without
 * the poller spinning on an fd the main thread has not got to yet. */
typedef struct { uint64_t job_id; int fd; short events; bool armed; } fd_watch;
typedef struct { uint64_t job_id; short revents; } fd_ready;
typedef struct { uint64_t job_id; gp_work_fn work; void *data; } work_item;

struct gp_exec {
    gp_app *app;
    gp_job **jobs;
    size_t njobs, cap_jobs;
    uint64_t next_id;
    int64_t last_frame_ns;

    gp_mutex lock;   /* guards everything below */
    gp_cond cond;
    post_item *posts;
    size_t nposts, cap_posts;
    work_item *queue;
    size_t nqueue, cap_queue;
    uint64_t *completed;
    size_t ncompleted, cap_completed;
    gp_thread *threads;
    int nthreads;
    bool shutdown;

    fd_watch *watches;      /* guarded by `lock` */
    size_t nwatches, cap_watches;
    fd_ready *fd_ready_list;
    size_t nfd_ready, cap_fd_ready;
    int wake_fd;            /* eventfd the poller also waits on: "the watch table changed" */
    gp_thread poller;
    bool poller_started;
};
typedef struct gp_exec gp_exec;

#define VEC_PUSH(vec, n, cap, item)                                                     \
    do {                                                                                \
        if ((n) == (cap)) {                                                             \
            (cap) = (cap) ? (cap) * 2 : 16;                                             \
            (vec) = realloc((vec), (cap) * sizeof *(vec));                              \
            if (!(vec)) abort();                                                        \
        }                                                                               \
        (vec)[(n)++] = (item);                                                          \
    } while (0)

static bool ent_null(gp_entity e) { return e.generation == 0; }

static int64_t g_skew_ns;
void gp_debug_clock_skew(int64_t ns) { g_skew_ns = ns; }

int64_t gp_now_ns(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    /* (whole seconds and the rest apart: ticks * 1e9 overflows 64 bits after a few hours of uptime) */
    return (t.QuadPart / freq.QuadPart) * 1000000000ll + (t.QuadPart % freq.QuadPart) * 1000000000ll / freq.QuadPart + g_skew_ns;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec + g_skew_ns;
#endif
}

double gp_time(void) { return (double)gp_now_ns() / 1e9; }

/* Not tied to a window: what is started with it lives as long as the app (or its entity). Scroll and focus calls
 * on such a ctx act on the first window, as before. */
gp_ctx gp_ctx_for(gp_app *app, gp_entity entity) {
    return (gp_ctx){ app, NULL, entity, false };
}

/* Tied to `window`: timers, frames, animations, descriptor watches and coroutines started with it end when the
 * window closes, and their callbacks see it as cx->window (the same is true of a ctx handed to an event handler). */
gp_ctx gp_ctx_for_window(gp_app *app, gp_window *window, gp_entity entity) {
    return (gp_ctx){ app, window, entity, false };
}

/* ---------------------------------------------------------------- registry */

void gp_exec_init(gp_app *app) {
    gp_exec *ex = calloc(1, sizeof *ex);
    ex->app = app;
    gp_mutex_init(&ex->lock);
    gp_cond_init(&ex->cond);
    app->exec = ex;
}

static gp_job *find_job(gp_exec *ex, uint64_t id) {
    for (size_t i = ex->njobs; i-- > 0;)
        if (ex->jobs[i]->id == id) return ex->jobs[i];
    return NULL;
}

static gp_job *new_job(gp_app *app, int kind, gp_ctx *cx) {
    gp_exec *ex = app->exec;
    gp_job *j = calloc(1, sizeof *j);
    j->id = ++ex->next_id;
    j->kind = (uint8_t)kind;
    if (cx) {
        j->owner = cx->entity;
        j->window = cx->window;
    }
    VEC_PUSH(ex->jobs, ex->njobs, ex->cap_jobs, j);
    return j;
}

static bool owner_alive(gp_app *app, const gp_job *j) {
    return ent_null(j->owner) || gp_entity_get(app, j->owner) != NULL;
}

/* The ctx and `self` a callback of this job should see. */
static void job_ctx(gp_app *app, const gp_job *j, gp_ctx *cx, void **self) {
    gp_window *w = NULL;
    for (int i = 0; i < app->nwindows; i++)
        if (app->windows[i] == j->window) w = j->window;
    *cx = (gp_ctx){ app, w, j->owner, false };
    *self = ent_null(j->owner) ? NULL : gp_entity_get(app, j->owner);
}

static bool job_active(const gp_job *j) {
    if (j->kind == JOB_CORO) return j->co && !gp_co_done(j->co);
    return !j->cancelled && !j->finished;
}

bool gp_task_active(gp_app *app, gp_task t) {
    gp_job *j = t.id ? find_job(app->exec, t.id) : NULL;
    return j && job_active(j);
}

static void cancel_job(gp_app *app, gp_job *j) {
    switch (j->kind) {
    case JOB_CORO:
        gp_co_cancel(app, j->co);
        break;
    case JOB_BG:
        if (!j->co_job) j->cancelled = true; /* a coroutine's own wait is resolved by the coroutine */
        break;
    default:
        j->cancelled = true;
    }
}

void gp_cancel(gp_app *app, gp_task t) {
    gp_job *j = t.id ? find_job(app->exec, t.id) : NULL;
    if (j) cancel_job(app, j);
}

void gp_exec_cancel_id(gp_app *app, uint64_t id) {
    gp_job *j = find_job(app->exec, id);
    if (j) j->cancelled = true;
}

/* A window is about to be destroyed: what was started for it ends here, while it is still there for the code that
 * unwinds (a cancelled coroutine's cleanup may use its ctx), and nothing keeps a pointer to it afterwards (a new
 * window can be allocated at the same address). */
void gp_exec_window_closed(gp_app *app, gp_window *w) {
    gp_exec *ex = app->exec;
    if (!ex) return;
    uint64_t *ids = NULL;
    size_t n = 0, cap = 0;
    for (size_t i = 0; i < ex->njobs; i++)
        if (ex->jobs[i]->window == w && job_active(ex->jobs[i])) VEC_PUSH(ids, n, cap, ex->jobs[i]->id);
    for (size_t i = 0; i < n; i++) {
        gp_job *j = find_job(ex, ids[i]);
        if (j) cancel_job(app, j);   /* may resume a coroutine; it can add and cancel jobs */
    }
    free(ids);
    for (size_t i = 0; i < ex->njobs; i++) {
        gp_job *j = ex->jobs[i];
        if (j->window != w) continue;
        j->window = NULL;
        if (j->co) gp_co_forget_window(j->co, w);
    }
}

void gp_exec_entity_released(gp_app *app, gp_entity e) {
    gp_exec *ex = app->exec;
    if (!ex || ent_null(e)) return;
    uint64_t *ids = NULL;
    size_t n = 0, cap = 0;
    for (size_t i = 0; i < ex->njobs; i++) {
        gp_job *j = ex->jobs[i];
        if (j->owner.index == e.index && j->owner.generation == e.generation && job_active(j))
            VEC_PUSH(ids, n, cap, j->id);
    }
    for (size_t i = 0; i < n; i++) {
        gp_job *j = find_job(ex, ids[i]);
        if (j) cancel_job(app, j); /* may resume a coroutine; it can add/cancel jobs */
    }
    free(ids);
}

/* ---------------------------------------------------------------- creating jobs */

gp_task gp_after(gp_ctx *cx, double seconds, gp_task_fn fn, void *data) {
    gp_job *j = new_job(cx->app, JOB_TIMER, cx);
    j->due_ns = gp_now_ns() + (int64_t)(seconds * 1e9);
    j->fn = fn;
    j->data = data;
    return (gp_task){ j->id };
}

gp_task gp_every(gp_ctx *cx, double seconds, gp_task_fn fn, void *data) {
    gp_job *j = new_job(cx->app, JOB_TIMER, cx);
    j->interval_ns = seconds > 0.001 ? (int64_t)(seconds * 1e9) : 1000000;
    j->due_ns = gp_now_ns() + j->interval_ns;
    j->fn = fn;
    j->data = data;
    return (gp_task){ j->id };
}

gp_task gp_next_frame(gp_ctx *cx, gp_frame_fn fn, void *data) {
    gp_job *j = new_job(cx->app, JOB_FRAME, cx);
    j->frame_fn = fn;
    j->data = data;
    return (gp_task){ j->id };
}

gp_task gp_animate(gp_ctx *cx, double seconds, int easing, gp_anim_fn fn, void *data) {
    gp_job *j = new_job(cx->app, JOB_ANIM, cx);
    j->start_ns = gp_now_ns();
    j->duration = seconds;
    j->easing = easing;
    j->anim_fn = fn;
    j->data = data;
    return (gp_task){ j->id };
}

float gp_ease_spring(float t, float damping) {
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    float z = damping < 0.1f ? 0.1f : damping;   /* (from 1 up it is the critically damped branch) */
    const float settle = 6.9077553f;   /* ln 1000: the envelope is a thousandth by t = 1 */
    float x;
    if (z >= 1) {                      /* critically damped: 1 - (1 + w t) e^(-w t), w from (1 + w) e^(-w) = 1/1000 */
        const float w = 9.2336f;
        x = 1 - (1 + w * t) * expf(-w * t);
        float end = 1 - (1 + w) * expf(-w);
        return x + (1 - end) * t;
    }
    float w = settle / z, wd = w * sqrtf(1 - z * z);
    float env = expf(-z * w * t);
    x = 1 - env * (cosf(wd * t) + z * w / wd * sinf(wd * t));
    float end = 1 - expf(-z * w) * (cosf(wd) + z * w / wd * sinf(wd));
    return x + (1 - end) * t;          /* (lands on 1 exactly instead of 1 - 0.001) */
}

/* ---------------------------------------------------------------- springs */

gp_spring gp_spring_make(float value, float stiffness, float damping) {
    return (gp_spring){ .value = value, .velocity = 0, .target = value, .stiffness = stiffness > 0 ? stiffness : 1, .damping = damping > 0 ? damping : 1 };
}

#define SPRING_EPS_X 0.005
#define SPRING_EPS_V 0.05

bool gp_spring_settled(const gp_spring *s) { return fabs((double)s->value - (double)s->target) < SPRING_EPS_X && fabs((double)s->velocity) < SPRING_EPS_V; }

/* The closed-form motion of x'' = -w^2 x - 2 z w x' from (x0, v0) after t: x is the distance to the target. */
static void spring_solve(double x0, double v0, double w, double z, double t, double *x, double *v) {
    if (fabs(z - 1) < 1e-4) {   /* critically damped */
        double e = exp(-w * t), b = v0 + w * x0;
        *x = (x0 + b * t) * e;
        *v = (v0 - w * b * t) * e;
    } else if (z < 1) {
        double wd = w * sqrt(1 - z * z), e = exp(-z * w * t), c = cos(wd * t), sn = sin(wd * t);
        double a = x0, b = (v0 + z * w * x0) / wd;
        *x = e * (a * c + b * sn);
        *v = e * ((-z * w * a + wd * b) * c + (-z * w * b - wd * a) * sn);
    } else {
        double q = w * sqrt(z * z - 1), r1 = -w * z + q, r2 = -w * z - q;
        double c1 = (v0 - r2 * x0) / (r1 - r2), c2 = x0 - c1;
        double e1 = exp(r1 * t), e2 = exp(r2 * t);
        *x = c1 * e1 + c2 * e2;
        *v = c1 * r1 * e1 + c2 * r2 * e2;
    }
}

bool gp_spring_step(gp_spring *s, double dt) {
    if (dt > 0) {
        double x, v;
        spring_solve((double)s->value - (double)s->target, (double)s->velocity, sqrt((double)s->stiffness), (double)s->damping, dt, &x, &v);
        s->value = (float)((double)s->target + x);
        s->velocity = (float)v;
    }
    if (gp_spring_settled(s)) {
        s->value = s->target;
        s->velocity = 0;
        return false;
    }
    return true;
}

static void spring_frame(void *self, gp_ctx *cx, double time, void *data) {
    gp_spring *s = data;
    int64_t now = gp_now_ns();
    double dt = s->last_ns ? (double)(now - s->last_ns) / 1e9 : 1.0 / 60;
    s->last_ns = now;
    bool moving = gp_spring_step(s, dt);
    gp_notify(cx);
    if (moving) gp_next_frame(cx, spring_frame, s);
    else s->running = false;
}

void gp_spring_to(gp_ctx *cx, gp_spring *s, float target) {
    s->target = target;
    if (s->running) return;   /* its frame job picks the new target up */
    if (gp_spring_settled(s)) {
        s->value = target;
        s->velocity = 0;
        return;
    }
    s->running = true;
    s->last_ns = 0;
    gp_next_frame(cx, spring_frame, s);
}

void gp_spring_jump(gp_spring *s, float value) {
    s->value = s->target = value;
    s->velocity = 0;
}

float gp_ease(int easing, float t) {
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    switch (easing) {
    case GP_EASE_SPRING: return gp_ease_spring(t, 0.5f);
    case GP_EASE_SPRING_BOUNCY: return gp_ease_spring(t, 0.25f);
    case GP_EASE_IN: return t * t * t;
    case GP_EASE_OUT: { float u = 1 - t; return 1 - u * u * u; }
    case GP_EASE_IN_OUT: return t < 0.5f ? 4 * t * t * t : 1 - powf(-2 * t + 2, 3) / 2;
    case GP_EASE_OUT_BACK: {
        const float c1 = 1.70158f, c3 = c1 + 1;
        float u = t - 1;
        return 1 + c3 * u * u * u + c1 * u * u;
    }
    case GP_EASE_OUT_BOUNCE: {
        const float n1 = 7.5625f, d1 = 2.75f;
        if (t < 1 / d1) return n1 * t * t;
        if (t < 2 / d1) { t -= 1.5f / d1; return n1 * t * t + 0.75f; }
        if (t < 2.5f / d1) { t -= 2.25f / d1; return n1 * t * t + 0.9375f; }
        t -= 2.625f / d1;
        return n1 * t * t + 0.984375f;
    }
    default: return t;
    }
}

/* What a window asks of itself: a running animation wants a frame, after which the window lays out again (and so
 * moves it on); a fading scrollbar wants one to paint again. Tied to the window, so closing it ends the request. */
static void window_wake(gp_window *w, int what) {
    if (what & GP_WAKE_LAYOUT) { w->anims.job_pending = false; w->dirty_layout = true; }
    if (what & GP_WAKE_PAINT) { w->fade_job_pending = false; w->dirty_paint = true; }
}

static void window_frame_cb(void *self, gp_ctx *cx, double time, void *data) {
    if (cx->window) window_wake(cx->window, (int)(intptr_t)data);
}
static void window_timer_cb(void *self, gp_ctx *cx, void *data) {
    if (cx->window) window_wake(cx->window, (int)(intptr_t)data);
}

uint64_t gp_exec_window_frame(gp_app *app, gp_window *w, int what) {
    gp_job *j = new_job(app, JOB_FRAME, NULL);
    j->window = w;
    j->frame_fn = window_frame_cb;
    j->data = (void *)(intptr_t)what;
    if (what & GP_WAKE_LAYOUT) w->anims.job_pending = true;
    if (what & GP_WAKE_PAINT) w->fade_job_pending = true;
    return j->id;
}

uint64_t gp_exec_window_at(gp_app *app, gp_window *w, int64_t due_ns, int what) {
    gp_job *j = new_job(app, JOB_TIMER, NULL);
    j->window = w;
    j->due_ns = due_ns;
    j->fn = window_timer_cb;
    j->data = (void *)(intptr_t)what;
    return j->id;
}

/* Wake jobs: the await points of coroutines. They have no entity of their own. */

uint64_t gp_exec_wake_after(gp_app *app, int64_t delay_ns, uint64_t co_job) {
    gp_job *j = new_job(app, JOB_TIMER, NULL);
    j->due_ns = gp_now_ns() + (delay_ns > 0 ? delay_ns : 0);
    j->co_job = co_job;
    return j->id;
}

uint64_t gp_exec_wake_frame(gp_app *app, uint64_t co_job, double *time_out) {
    gp_job *j = new_job(app, JOB_FRAME, NULL);
    j->co_job = co_job;
    j->time_out = time_out;
    return j->id;
}

/* ---------------------------------------------------------------- thread pool */

static void *worker(void *arg) {
    gp_exec *ex = arg;
    for (;;) {
        gp_mutex_lock(&ex->lock);
        while (ex->nqueue == 0 && !ex->shutdown) gp_cond_wait(&ex->cond, &ex->lock);
        if (ex->shutdown) {
            gp_mutex_unlock(&ex->lock);
            return NULL; /* anything still queued is reclaimed by gp_exec_shutdown */
        }
        work_item it = ex->queue[0];
        memmove(ex->queue, ex->queue + 1, (--ex->nqueue) * sizeof *ex->queue);
        gp_mutex_unlock(&ex->lock);

        it.work(it.data);

        gp_mutex_lock(&ex->lock);
        VEC_PUSH(ex->completed, ex->ncompleted, ex->cap_completed, it.job_id);
        gp_mutex_unlock(&ex->lock);
        gp_platform_wake(ex->app);
    }
}

static void submit_work(gp_exec *ex, gp_job *j, gp_work_fn work, void *data) {
    if (!ex->nthreads) {
        int cpus = gp_cpu_count();
        int n = cpus < 2 ? 2 : cpus > 8 ? 8 : cpus;
        ex->threads = calloc((size_t)n, sizeof *ex->threads);
        for (int i = 0; i < n; i++)
            if (gp_thread_create(&ex->threads[ex->nthreads], worker, ex)) ex->nthreads++;
    }
    j->bg_running = true;
    gp_mutex_lock(&ex->lock);
    VEC_PUSH(ex->queue, ex->nqueue, ex->cap_queue, ((work_item){ j->id, work, data }));
    gp_cond_signal(&ex->cond);
    gp_mutex_unlock(&ex->lock);
}

gp_task gp_background(gp_ctx *cx, gp_work_fn work, gp_task_fn done, void *data,
                      gp_drop_fn free_data) {
    gp_job *j = new_job(cx->app, JOB_BG, cx);
    j->fn = done;
    j->data = data;
    j->free_data = free_data;
    submit_work(cx->app->exec, j, work, data);
    return (gp_task){ j->id };
}

uint64_t gp_exec_wake_background(gp_app *app, uint64_t co_job, gp_work_fn work, void *data) {
    gp_job *j = new_job(app, JOB_BG, NULL);
    j->co_job = co_job;
    submit_work(app->exec, j, work, data);
    return j->id;
}

void gp_post(gp_app *app, gp_entity entity, gp_task_fn fn, void *data) {
    gp_exec *ex = app->exec;
    gp_mutex_lock(&ex->lock);
    VEC_PUSH(ex->posts, ex->nposts, ex->cap_posts, ((post_item){ entity, fn, data }));
    gp_mutex_unlock(&ex->lock);
    gp_platform_wake(app);
}

/* ---------------------------------------------------------------- file descriptors */

#ifdef GP_HAS_FD

/* A thread that does nothing but poll(2) the armed descriptors and the control eventfd. When one is ready it
 * is disarmed, queued for the main thread, and the event loop is woken; the main thread owns jobs, runs the
 * callbacks and re-arms. The loop itself stays in SDL's wait, so nothing about window events changes. */
static short to_poll(int events) {
    short p = 0;
    if (events & GP_FD_READ) p |= POLLIN;
    if (events & GP_FD_WRITE) p |= POLLOUT;
    return p;
}

static int from_poll(short revents) {
    int e = 0;
    if (revents & POLLIN) e |= GP_FD_READ;
    if (revents & POLLOUT) e |= GP_FD_WRITE;
    if (revents & POLLERR) e |= GP_FD_ERROR;
    if (revents & POLLHUP) e |= GP_FD_HUP;
    if (revents & POLLNVAL) e |= GP_FD_INVALID;
    return e;
}

static void poke(gp_exec *ex) {
    uint64_t one = 1;
    ssize_t r = write(ex->wake_fd, &one, sizeof one);
    (void)r;   /* the counter cannot overflow in practice; a full one is already a pending wake */
}

static void *poller_main(void *arg) {
    gp_exec *ex = arg;
    struct pollfd *pfds = NULL;
    uint64_t *ids = NULL;
    size_t cap = 0;
    for (;;) {
        gp_mutex_lock(&ex->lock);
        if (ex->shutdown) {
            gp_mutex_unlock(&ex->lock);
            break;
        }
        if (cap < ex->nwatches + 1) {
            cap = ex->nwatches + 16;
            pfds = realloc(pfds, cap * sizeof *pfds);
            ids = realloc(ids, cap * sizeof *ids);
            if (!pfds || !ids) abort();
        }
        size_t n = 0;
        pfds[n] = (struct pollfd){ ex->wake_fd, POLLIN, 0 };
        ids[n++] = 0;
        for (size_t i = 0; i < ex->nwatches; i++) {
            if (!ex->watches[i].armed) continue;
            pfds[n] = (struct pollfd){ ex->watches[i].fd, ex->watches[i].events, 0 };
            ids[n++] = ex->watches[i].job_id;
        }
        gp_mutex_unlock(&ex->lock);

        int r = poll(pfds, (nfds_t)n, -1);
        if (r < 0) continue;   /* EINTR */
        if (pfds[0].revents) {
            uint64_t v;
            ssize_t rr = read(ex->wake_fd, &v, sizeof v);
            (void)rr;
        }
        bool any = false;
        gp_mutex_lock(&ex->lock);
        for (size_t k = 1; k < n; k++) {
            if (!pfds[k].revents) continue;
            for (size_t i = 0; i < ex->nwatches; i++) {   /* the table may have changed while polling */
                fd_watch *w = &ex->watches[i];
                if (w->job_id != ids[k] || !w->armed) continue;
                w->armed = false;
                VEC_PUSH(ex->fd_ready_list, ex->nfd_ready, ex->cap_fd_ready, ((fd_ready){ w->job_id, pfds[k].revents }));
                any = true;
                break;
            }
        }
        gp_mutex_unlock(&ex->lock);
        if (any) gp_platform_wake(ex->app);
    }
    free(pfds);
    free(ids);
    return NULL;
}

static void poller_start(gp_exec *ex) {
    if (ex->poller_started) return;
    ex->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (ex->wake_fd < 0) {
        fprintf(stderr, "gpuic: eventfd failed: %s\n", strerror(errno));
        return;
    }
    if (!gp_thread_create(&ex->poller, poller_main, ex)) {
        close(ex->wake_fd);
        return;
    }
    ex->poller_started = true;
}

static void watch_add(gp_exec *ex, const gp_job *j) {
    poller_start(ex);
    if (!ex->poller_started) return;
    gp_mutex_lock(&ex->lock);
    VEC_PUSH(ex->watches, ex->nwatches, ex->cap_watches, ((fd_watch){ j->id, j->fd, to_poll(j->fd_events), true }));
    gp_mutex_unlock(&ex->lock);
    poke(ex);
}

static void watch_remove(gp_exec *ex, uint64_t job_id) {
    if (!ex->poller_started) return;
    gp_mutex_lock(&ex->lock);
    for (size_t i = 0; i < ex->nwatches; i++)
        if (ex->watches[i].job_id == job_id) {
            ex->watches[i] = ex->watches[--ex->nwatches];
            break;
        }
    gp_mutex_unlock(&ex->lock);
    poke(ex);   /* it may be polling a descriptor the caller is about to close */
}

static void watch_rearm(gp_exec *ex, uint64_t job_id) {
    gp_mutex_lock(&ex->lock);
    for (size_t i = 0; i < ex->nwatches; i++)
        if (ex->watches[i].job_id == job_id) {
            ex->watches[i].armed = true;
            break;
        }
    gp_mutex_unlock(&ex->lock);
    poke(ex);
}

gp_task gp_watch_fd(gp_ctx *cx, int fd, int events, gp_fd_fn fn, void *data) {
    gp_job *j = new_job(cx->app, JOB_FD, cx);
    j->fd = fd;
    j->fd_events = events;
    j->fd_fn = fn;
    j->data = data;
    watch_add(cx->app->exec, j);
    return (gp_task){ j->id };
}

uint64_t gp_exec_wake_fd(gp_app *app, uint64_t co_job, int fd, int events, int64_t timeout_ns, int *revents_out) {
    gp_job *j = new_job(app, JOB_FD, NULL);
    j->fd = fd;
    j->fd_events = events;
    j->co_job = co_job;
    j->revents_out = revents_out;
    j->oneshot = true;
    if (timeout_ns >= 0) j->due_ns = gp_now_ns() + timeout_ns;
    watch_add(app->exec, j);
    return j->id;
}

#endif   /* GP_HAS_FD */

/* ---------------------------------------------------------------- running */

static void wake_co(gp_app *app, uint64_t co_job) {
    gp_job *cj = find_job(app->exec, co_job);
    if (cj && cj->co && !gp_co_done(cj->co)) gp_co_resume(app, cj->co);
}

static void run_posts(gp_app *app) {
    gp_exec *ex = app->exec;
    gp_mutex_lock(&ex->lock);
    post_item *items = ex->posts;
    size_t n = ex->nposts;
    ex->posts = NULL;
    ex->nposts = ex->cap_posts = 0;
    gp_mutex_unlock(&ex->lock);

    for (size_t i = 0; i < n; i++) {
        void *self = ent_null(items[i].entity) ? NULL : gp_entity_get(app, items[i].entity);
        if (!ent_null(items[i].entity) && !self) continue; /* entity is gone */
        gp_ctx cx = gp_ctx_for(app, items[i].entity);
        items[i].fn(self, &cx, items[i].data);
        gp_effects_flush(app);
    }
    free(items);
}

static void drain_completed(gp_app *app) {
    gp_exec *ex = app->exec;
    gp_mutex_lock(&ex->lock);
    uint64_t *ids = ex->completed;
    size_t n = ex->ncompleted;
    ex->completed = NULL;
    ex->ncompleted = ex->cap_completed = 0;
    gp_mutex_unlock(&ex->lock);

    for (size_t i = 0; i < n; i++) {
        gp_job *j = find_job(ex, ids[i]);
        if (!j) continue;
        j->bg_running = false;
        if (j->co_job) {
            wake_co(app, j->co_job);
        } else if (j->fn && !j->cancelled && owner_alive(app, j)) {
            gp_ctx cx;
            void *self;
            job_ctx(app, j, &cx, &self);
            j->fn(self, &cx, j->data);
        }
        if (j->free_data) j->free_data(j->data);
        j->finished = true;
        gp_effects_flush(app);
    }
    free(ids);
}

#ifdef GP_HAS_FD
/* Hands a ready (or timed out) watch to its callback or coroutine. events == 0 means the await timed out. */
static void deliver_fd(gp_app *app, gp_job *j, int events) {
    gp_exec *ex = app->exec;
    if (!owner_alive(app, j)) { j->cancelled = true; return; }
    if (j->oneshot) {
        j->finished = true;
        if (j->revents_out) *j->revents_out = events;
        if (j->co_job) wake_co(app, j->co_job);
        return;
    }
    gp_ctx cx;
    void *self;
    job_ctx(app, j, &cx, &self);
    j->fd_fn(self, &cx, j->fd, events, j->data);
    if (events & GP_FD_INVALID) j->cancelled = true;   /* the descriptor is gone: nothing more to watch */
    if (job_active(j)) watch_rearm(ex, j->id);
}

static void drain_fds(gp_app *app) {
    gp_exec *ex = app->exec;
    gp_mutex_lock(&ex->lock);
    fd_ready *items = ex->fd_ready_list;
    size_t n = ex->nfd_ready;
    ex->fd_ready_list = NULL;
    ex->nfd_ready = ex->cap_fd_ready = 0;
    gp_mutex_unlock(&ex->lock);

    for (size_t i = 0; i < n; i++) {
        gp_job *j = find_job(ex, items[i].job_id);
        if (!j || !job_active(j)) continue;
        deliver_fd(app, j, from_poll(items[i].revents));
        gp_effects_flush(app);
    }
    free(items);

    /* awaits with a deadline */
    int64_t now = gp_now_ns();
    uint64_t *ids = NULL;
    size_t m = 0, cap = 0;
    for (size_t i = 0; i < ex->njobs; i++) {
        gp_job *j = ex->jobs[i];
        if (j->kind == JOB_FD && j->oneshot && j->due_ns && j->due_ns <= now && job_active(j))
            VEC_PUSH(ids, m, cap, j->id);
    }
    for (size_t i = 0; i < m; i++) {
        gp_job *j = find_job(ex, ids[i]);
        if (!j || !job_active(j)) continue;
        deliver_fd(app, j, 0);
        gp_effects_flush(app);
    }
    free(ids);
}
#endif   /* GP_HAS_FD */

static void run_timers(gp_app *app) {
    gp_exec *ex = app->exec;
    int64_t now = gp_now_ns();
    uint64_t *ids = NULL;
    size_t n = 0, cap = 0;
    for (size_t i = 0; i < ex->njobs; i++) {
        gp_job *j = ex->jobs[i];
        if (j->kind == JOB_TIMER && !j->cancelled && !j->finished && j->due_ns <= now)
            VEC_PUSH(ids, n, cap, j->id);
    }
    for (size_t i = 0; i < n; i++) {
        gp_job *j = find_job(ex, ids[i]);
        if (!j || j->cancelled || j->finished) continue;
        if (!owner_alive(app, j)) { j->cancelled = true; continue; }
        if (j->interval_ns) {
            do j->due_ns += j->interval_ns; while (j->due_ns <= now); /* skip missed ticks */
        } else {
            j->finished = true;
        }
        if (j->co_job) {
            wake_co(app, j->co_job);
        } else {
            gp_ctx cx;
            void *self;
            job_ctx(app, j, &cx, &self);
            j->fn(self, &cx, j->data);
        }
        gp_effects_flush(app);
    }
    free(ids);
}

/* Frames are paced per window, by the refresh rate of the display it is on; work not tied to a window follows the
 * first window. */
static int64_t frame_interval_ns(gp_app *app, gp_window *w) {
    gp_window *ref = w ? w : (app->nwindows ? app->windows[0] : NULL);
    float hz = ref ? gp_platform_refresh_hz(ref) : 60.f;
    return (int64_t)(1e9 / (hz >= 10.f ? hz : 60.f));
}

static int64_t *last_frame(gp_exec *ex, gp_window *w) { return w ? &w->last_frame_ns : &ex->last_frame_ns; }

static void run_frames(gp_app *app) {
    gp_exec *ex = app->exec;
    uint64_t *ids = NULL;
    size_t n = 0, cap = 0;
    for (size_t i = 0; i < ex->njobs; i++) {
        gp_job *j = ex->jobs[i];
        if ((j->kind == JOB_FRAME || j->kind == JOB_ANIM) && !j->cancelled && !j->finished)
            VEC_PUSH(ids, n, cap, j->id);
    }
    if (n == 0) {
        free(ids);
        return;
    }
    /* Which windows (NULL = none) have a frame due. Requests made during this run wait for the next frame; 0.5 ms
     * of slack on the deadline. */
    int64_t now = gp_now_ns();
    struct { gp_window *w; bool due; } seen[GP_MAX_WINDOWS + 1];
    int nseen = 0;
    for (size_t i = 0; i < n; i++) {
        gp_job *j = find_job(ex, ids[i]);
        bool known = false;
        for (int k = 0; k < nseen; k++) known |= seen[k].w == j->window;
        if (known) continue;
        bool due = now >= *last_frame(ex, j->window) + frame_interval_ns(app, j->window) - 500000;
        if (due) *last_frame(ex, j->window) = now;
        seen[nseen].w = j->window;
        seen[nseen++].due = due;
    }
    double t_now = (double)now / 1e9;

    for (size_t i = 0; i < n; i++) {
        gp_job *j = find_job(ex, ids[i]);
        if (!j || j->cancelled || j->finished) continue;
        bool due = false;
        for (int k = 0; k < nseen; k++) if (seen[k].w == j->window) due = seen[k].due;
        if (!due) continue;
        if (!owner_alive(app, j)) { j->cancelled = true; continue; }
        gp_ctx cx;
        void *self;
        job_ctx(app, j, &cx, &self);
        if (j->kind == JOB_FRAME) {
            j->finished = true;
            if (j->co_job) {
                if (j->time_out) *j->time_out = t_now;
                wake_co(app, j->co_job);
            } else {
                j->frame_fn(self, &cx, t_now, j->data);
            }
        } else {
            float t = j->duration > 0 ? (float)((double)(now - j->start_ns) / (j->duration * 1e9)) : 1.f;
            if (t >= 1.f) { t = 1.f; j->finished = true; }
            j->anim_fn(self, &cx, gp_ease(j->easing, t), j->data);
        }
        gp_effects_flush(app);
    }
    free(ids);
}

static bool removable(const gp_job *j) {
    switch (j->kind) {
    case JOB_CORO: return j->co && gp_co_done(j->co);
    case JOB_BG: return !j->bg_running && (j->cancelled || j->finished);
    default: return j->cancelled || j->finished;
    }
}

/* The drop functions of removed jobs run after the list is compacted: one may start or cancel work. */
typedef struct { gp_drop_fn fn; void *data; } pending_drop;

static void run_drops(pending_drop *d, size_t n) {
    for (size_t i = 0; i < n; i++) d[i].fn(d[i].data);
    free(d);
}

static void sweep(gp_exec *ex) {
    size_t out = 0, ndrops = 0, cap_drops = 0;
    pending_drop *drops = NULL;
    for (size_t i = 0; i < ex->njobs; i++) {
        gp_job *j = ex->jobs[i];
        if (removable(j)) {
#ifdef GP_HAS_FD
            if (j->kind == JOB_FD) watch_remove(ex, j->id);
#endif
            if (j->co) gp_co_free(j->co);
            if (j->drop) VEC_PUSH(drops, ndrops, cap_drops, ((pending_drop){ j->drop, j->drop_data }));
            free(j);
        } else {
            ex->jobs[out++] = j;
        }
    }
    ex->njobs = out;
    run_drops(drops, ndrops);
}

void gp_task_on_drop(gp_app *app, gp_task t, gp_drop_fn drop, void *data) {
    gp_job *j = t.id ? find_job(app->exec, t.id) : NULL;
    if (!j) { if (drop) drop(data); return; }   /* already gone: it is over */
    j->drop = drop;
    j->drop_data = data;
}

void gp_exec_run(gp_app *app) {
    run_posts(app);
    drain_completed(app);
#ifdef GP_HAS_FD
    drain_fds(app);
#endif
    run_timers(app);
    run_frames(app);
    sweep(app->exec);
}

int gp_exec_timeout_ms(gp_app *app) {
    gp_exec *ex = app->exec;
    gp_mutex_lock(&ex->lock);
    bool pending = ex->nposts || ex->ncompleted || ex->nfd_ready;
    gp_mutex_unlock(&ex->lock);
    if (pending) return 0;

    int64_t now = gp_now_ns(), best = INT64_MAX;
    for (size_t i = 0; i < ex->njobs; i++) {
        const gp_job *j = ex->jobs[i];
        if (j->cancelled || j->finished) continue;
        if ((j->kind == JOB_TIMER || (j->kind == JOB_FD && j->due_ns)) && j->due_ns < best) best = j->due_ns;
        if (j->kind == JOB_FRAME || j->kind == JOB_ANIM) {
            int64_t next = *last_frame(ex, j->window) + frame_interval_ns(app, j->window);
            if (next < best) best = next;
        }
    }
    if (best == INT64_MAX) return -1;
    if (best <= now) return 0;
    int64_t ms = (best - now + 999999) / 1000000;
    return ms > 1000000 ? 1000000 : (int)ms;
}

bool gp_exec_wants_frames(gp_app *app) {
    gp_exec *ex = app->exec;
    for (size_t i = 0; i < ex->njobs; i++) {
        const gp_job *j = ex->jobs[i];
        if ((j->kind == JOB_FRAME || j->kind == JOB_ANIM) && !j->cancelled && !j->finished) return true;
    }
    return false;
}

/* ---------------------------------------------------------------- shutdown */

void gp_exec_shutdown(gp_app *app) {
    gp_exec *ex = app->exec;
    if (!ex) return;

    /* 1. cancel everything. Coroutines sleeping or yielding resume now and unwind; ones
     *    waiting for background work unwind when that work finishes (step 3). */
    uint64_t *ids = NULL;
    size_t n = 0, cap = 0;
    for (size_t i = 0; i < ex->njobs; i++)
        if (job_active(ex->jobs[i])) VEC_PUSH(ids, n, cap, ex->jobs[i]->id);
    for (size_t i = 0; i < n; i++) {
        gp_job *j = find_job(ex, ids[i]);
        if (j) cancel_job(app, j);
    }
    free(ids);

    /* 2. stop the pool; running work finishes, queued work is reclaimed as "completed" so
     *    its free_data / coroutine gets its turn. */
    gp_mutex_lock(&ex->lock);
    ex->shutdown = true;
    gp_cond_broadcast(&ex->cond);
    gp_mutex_unlock(&ex->lock);
    for (int i = 0; i < ex->nthreads; i++) gp_thread_join(ex->threads[i]);
#ifdef GP_HAS_FD
    if (ex->poller_started) {
        poke(ex);
        gp_thread_join(ex->poller);
        close(ex->wake_fd);
        ex->poller_started = false;
    }
#endif
    for (size_t i = 0; i < ex->nqueue; i++)
        VEC_PUSH(ex->completed, ex->ncompleted, ex->cap_completed, ex->queue[i].job_id);
    ex->nqueue = 0;

    /* 3. let cancelled coroutines run to completion (bounded: a body that keeps awaiting
     *    returns false immediately, so a few rounds always suffice) */
    for (int round = 0; round < 64; round++) {
        drain_completed(app);
        run_timers(app);
        sweep(ex);
        bool coros = false;
        for (size_t i = 0; i < ex->njobs; i++) coros |= ex->jobs[i]->kind == JOB_CORO;
        if (!coros && !ex->ncompleted) break;
    }

    free(ex->posts);
    free(ex->watches);
    free(ex->fd_ready_list);
    free(ex->queue);
    free(ex->completed);
    free(ex->threads);
    pending_drop *drops = NULL;
    size_t ndrops = 0, cap_drops = 0;
    for (size_t i = 0; i < ex->njobs; i++) {
        if (ex->jobs[i]->co) gp_co_free(ex->jobs[i]->co);
        if (ex->jobs[i]->drop) VEC_PUSH(drops, ndrops, cap_drops, ((pending_drop){ ex->jobs[i]->drop, ex->jobs[i]->drop_data }));
        free(ex->jobs[i]);
    }
    free(ex->jobs);
    ex->jobs = NULL;
    ex->njobs = 0;
    run_drops(drops, ndrops);
    gp_mutex_destroy(&ex->lock);
    gp_cond_destroy(&ex->cond);
    free(ex);
    app->exec = NULL;
}

/* ---------------------------------------------------------------- coroutine spawn */

gp_task gp_spawn(gp_ctx *cx, gp_co_fn fn, void *data) {
    gp_job *j = new_job(cx->app, JOB_CORO, cx);
    j->co = gp_co_new(cx->app, j->id, *cx, fn, data);
    return (gp_task){ j->id };
}
