/*
 * Work that belongs to a window: timers, frames, animations, descriptor watches and coroutines started with a
 * ctx that has a window end when it closes, and see it as cx->window; work started with a plain gp_ctx_for ctx
 * goes on. Needs a display: it opens two windows.
 */
#include "../src/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

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
static gp_window *win_a, *win_b;
static gp_entity view_a, view_b;

typedef struct {
    int calls;
    gp_window *seen;          /* cx->window in the last callback */
    int anim_calls;
    float anim_last;
    bool anim_finished;
} Count;
static Count ca, cb, capp, frames_a, frames_b;

static void tick(void *self, gp_ctx *cx, void *data) {
    Count *c = data;
    c->calls++;
    c->seen = cx->window;
}
static void frame_loop(void *self, gp_ctx *cx, double time, void *data) {
    Count *c = data;
    c->calls++;
    c->seen = cx->window;
    gp_next_frame(cx, frame_loop, data);   /* re-armed with the ctx it was called with: stays on its window */
}
static void anim_cb(void *self, gp_ctx *cx, float t, void *data) {
    Count *c = data;
    c->anim_calls++;
    c->anim_last = t;
    c->seen = cx->window;
    if (t >= 1.f) c->anim_finished = true;
}

/* a timer made from inside an event handler's ctx belongs to that window */
static Count from_layout;
static int layout_runs;
static void on_layout_b(void *self, gp_ctx *cx, gp_size size, void *data) {
    if (layout_runs++) return;
    gp_every(cx, 0.02, tick, &from_layout);
}

static gp_node *view_a_render(void *self, gp_ctx *cx) { return gp_div(gp_size_full(), gp_text("A")); }
static gp_node *view_b_render(void *self, gp_ctx *cx) { return gp_div(gp_size_full(), gp_on_layout(on_layout_b), gp_text("B")); }

/* a coroutine of a window: sleeping, and waiting for background work */
typedef struct { bool unwound, still_has_window, saw_window_in_cleanup; gp_window *window_in_cleanup; gp_window *w; } Co;
static Co co_sleep, co_bg;
static void sleeper(gp_co *co, void *self, gp_ctx *cx, void *data) {
    Co *c = data;
    bool ok = gp_sleep(co, 30);
    c->unwound = !ok;
    c->window_in_cleanup = cx->window;
}
static void slow_work(void *data) { usleep(800000); }
static void bg_waiter(gp_co *co, void *self, gp_ctx *cx, void *data) {
    Co *c = data;
    bool ok = gp_await_background(co, slow_work, NULL);
    c->unwound = !ok;
    c->window_in_cleanup = cx->window;
}
#ifdef GP_HAS_FD
static void on_fd(void *self, gp_ctx *cx, int fd, int events, void *data) {
    Count *c = data;
    c->calls++;
    char b[8];
    while (read(fd, b, sizeof b) > 0) {}
}
#endif

static void driver(gp_co *co, void *self, gp_ctx *appcx, void *data) {
    gp_ctx cx_a = gp_ctx_for_window(app, win_a, GP_ENTITY_NULL);
    gp_ctx cx_b = gp_ctx_for_window(app, win_b, GP_ENTITY_NULL);
    gp_ctx cx_app = gp_ctx_for(app, GP_ENTITY_NULL);
    CHECK(cx_app.window == NULL, "gp_ctx_for is tied to a window");
    CHECK(cx_a.window == win_a && cx_b.window == win_b, "gp_ctx_for_window");

    gp_sleep(co, 0.1);   /* both windows laid out: B's on_layout started its timer */
    CHECK(layout_runs >= 1, "on_layout of window B did not run");

    gp_task t_a = gp_every(&cx_a, 0.02, tick, &ca);
    gp_task t_b = gp_every(&cx_b, 0.02, tick, &cb);
    gp_task t_app = gp_every(&cx_app, 0.02, tick, &capp);
    gp_task t_after_a = gp_after(&cx_a, 30, tick, &ca);        /* a one-shot that would be far in the future */
    gp_task f_a = gp_next_frame(&cx_a, frame_loop, &frames_a);
    gp_next_frame(&cx_b, frame_loop, &frames_b);
    gp_task an_a = gp_animate(&cx_a, 30, GP_EASE_LINEAR, anim_cb, &ca);
    gp_animate(&cx_b, 0.15, GP_EASE_LINEAR, anim_cb, &cb);
    static Count fd_a;
#ifdef GP_HAS_FD
    int pipe_fd[2];
    CHECK(pipe(pipe_fd) == 0, "pipe");
    gp_fd_set_nonblocking(pipe_fd[0]);
    gp_task w_a = gp_watch_fd(&cx_a, pipe_fd[0], GP_FD_READ, on_fd, &fd_a);
#else   /* no descriptor watches here: a task that is over stands in, so the checks below hold as they are */
    gp_task w_a = { 0 };
#endif
    gp_task c_sleep = gp_spawn(&cx_a, sleeper, &co_sleep);
    gp_task c_bg = gp_spawn(&cx_a, bg_waiter, &co_bg);

    gp_sleep(co, 0.25);
    CHECK(ca.calls >= 5 && cb.calls >= 5 && capp.calls >= 5, "ticks before closing: A %d, B %d, app %d", ca.calls, cb.calls, capp.calls);
    CHECK(from_layout.calls >= 5, "the timer made in B's layout handler ticked %d times", from_layout.calls);
    CHECK(frames_a.calls >= 5 && frames_b.calls >= 5, "frame callbacks: A %d, B %d", frames_a.calls, frames_b.calls);
    CHECK(ca.seen == win_a && cb.seen == win_b, "a timer sees its window: A saw %p (want %p), B saw %p (want %p)", (void *)ca.seen, (void *)win_a, (void *)cb.seen, (void *)win_b);
    CHECK(capp.seen == NULL, "an app-level timer sees a window (%p)", (void *)capp.seen);
    CHECK(frames_a.seen == win_a && frames_b.seen == win_b, "a frame request keeps its window across re-arming");
    CHECK(from_layout.seen == win_b, "a timer from an event handler's ctx belongs to that window (%p, want %p)", (void *)from_layout.seen, (void *)win_b);
    CHECK(an_a.id && gp_task_active(app, an_a) && ca.anim_calls >= 5, "A's animation runs");
    CHECK(gp_task_active(app, c_sleep) && gp_task_active(app, c_bg), "A's coroutines run");
#ifdef GP_HAS_FD
    CHECK(write(pipe_fd[1], "x", 1) == 1, "write");
    gp_sleep(co, 0.1);
    CHECK(fd_a.calls == 1, "A's descriptor watch called %d times", fd_a.calls);
#endif

    /* A closes */
    gp_window_close(win_a);
    gp_sleep(co, 0.1);
    CHECK(!gp_task_active(app, t_a) && !gp_task_active(app, t_after_a) && !gp_task_active(app, f_a) &&
          !gp_task_active(app, an_a) && !gp_task_active(app, w_a), "A's timers, frame request, animation and watch are still active after it closed");
    CHECK(gp_task_active(app, t_b) && gp_task_active(app, t_app), "B's or the app's work was cancelled with A");
    CHECK(app->nwindows == 1 && app->windows[0] == win_b, "window A is still open (%d windows)", app->nwindows);
    CHECK(co_sleep.unwound, "A's sleeping coroutine was not cancelled");
    CHECK(co_sleep.window_in_cleanup == win_a, "A's coroutine saw window %p while unwinding, want A %p", (void *)co_sleep.window_in_cleanup, (void *)win_a);
    CHECK(!gp_task_active(app, c_sleep), "A's coroutine is still active");

    int a_calls = ca.calls, a_anim = ca.anim_calls, a_frames = frames_a.calls, fdc = fd_a.calls;
    int b_calls = cb.calls, app_calls = capp.calls, b_frames = frames_b.calls;
#ifdef GP_HAS_FD
    CHECK(write(pipe_fd[1], "y", 1) == 1, "write");
#endif
    gp_sleep(co, 0.2);
    CHECK(ca.calls == a_calls, "A's timer ticked after the window closed (%d -> %d)", a_calls, ca.calls);
    CHECK(ca.anim_calls == a_anim && !ca.anim_finished, "A's animation went on (%d -> %d calls, finished %d)", a_anim, ca.anim_calls, ca.anim_finished);
    CHECK(frames_a.calls == a_frames, "A's frame request went on (%d -> %d)", a_frames, frames_a.calls);
    CHECK(fd_a.calls == fdc, "A's descriptor watch called back after the window closed");
    CHECK(cb.calls > b_calls + 5 && capp.calls > app_calls + 5, "B or the app stopped ticking (B %d -> %d, app %d -> %d)", b_calls, cb.calls, app_calls, capp.calls);
    CHECK(frames_b.calls > b_frames + 5, "B's frames stopped (%d -> %d)", b_frames, frames_b.calls);
    CHECK(cb.anim_finished && cb.anim_last == 1.f, "B's animation did not finish (%d calls, last t %.2f)", cb.anim_calls, cb.anim_last);

    /* the coroutine that was waiting for background work unwinds when the work ends, and does not point at the dead window */
    gp_sleep(co, 0.6);
    CHECK(co_bg.unwound, "A's coroutine waiting for background work never unwound");
    CHECK(co_bg.window_in_cleanup == NULL, "it still pointed at the closed window (%p)", (void *)co_bg.window_in_cleanup);

    /* a window made after: new window, none of A's leftovers */
    gp_window *win_c = gp_open_window(app, &(gp_window_options){ .title = "window_test C", .width = 200, .height = 100 }, view_a);
    CHECK(win_c != NULL, "could not open a third window");
    gp_sleep(co, 0.15);
    CHECK(ca.calls == a_calls, "A's timer ticked after a new window took its place");
#ifdef GP_HAS_FD
    close(pipe_fd[0]);
    close(pipe_fd[1]);
#endif

    /* the last two windows close: the app ends, with B's and the app's timers still running (shutdown clears them) */
    gp_window_close(win_b);
    gp_sleep(co, 0.1);
    CHECK(!gp_task_active(app, t_b), "B's work outlived B");
    CHECK(gp_task_active(app, t_app), "the app-level timer was cancelled by a window closing");
    gp_window_close(win_c);
    gp_sleep(co, 0.2);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    view_a = gp_view_new(app, 0, NULL, view_a_render, NULL);
    view_b = gp_view_new(app, 0, NULL, view_b_render, NULL);
    win_a = gp_open_window(app, &(gp_window_options){ .title = "window_test A", .width = 220, .height = 120 }, view_a);
    win_b = gp_open_window(app, &(gp_window_options){ .title = "window_test B", .width = 220, .height = 120 }, view_b);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);   /* a physical pointer over the window is not part of the test */
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "window_test: %d FAILURE(S)\n" : "window_test: all checks passed\n", failures);
    return failures != 0;
}
