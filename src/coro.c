/*
 * Stackful coroutines (ucontext; fibers on Windows): async/await for C.
 *
 * A coroutine runs on its own stack (mmap'd, or the fiber's) and only ever switches at its await points
 * (gp_sleep, gp_yield, gp_await_*), always to and from the main loop's stack, so there is
 * no scheduler of its own: an await registers a "wake" job with the executor and
 * suspends; when that job fires the executor resumes the coroutine.
 *
 * Cancellation is cooperative so nothing leaks. Awaits on a cancelled coroutine return
 * false without suspending, and the body returns normally, running its own cleanup.
 * Cancelling a sleeping coroutine resumes it at once; one waiting for background work is
 * resumed when that work finishes, because its data lives on the coroutine's stack.
 */
#define _GNU_SOURCE   /* accept4 */
#include "internal.h"

#include <errno.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <ucontext.h>
#endif

#define CO_STACK_SIZE (256 * 1024)
#define CO_GUARD_SIZE 4096

enum { CO_NEW, CO_RUNNING, CO_SUSPENDED, CO_DONE };

struct gp_co {
    gp_app *app;
    uint64_t job_id;
    gp_ctx cx;
    gp_co_fn fn;
    void *data;

#ifdef _WIN32
    void *fiber, *caller;    /* the coroutine's fiber (NULL before the first run and after the body returned), and the one that resumed it */
#else
    ucontext_t ctx, caller;
    void *map;               /* mmap'd region: guard page + stack */
#endif
    int state;
    bool cancelled;
    uint64_t wait_job;       /* the wake job this coroutine is suspended on */
    bool wait_bg;
};

gp_co *gp_co_new(gp_app *app, uint64_t job_id, gp_ctx cx, gp_co_fn fn, void *data) {
    gp_co *co = calloc(1, sizeof *co);
    co->app = app;
    co->job_id = job_id;
    co->cx = cx;
    co->fn = fn;
    co->data = data;
    co->state = CO_NEW;
    co->wait_job = gp_exec_wake_after(app, 0, job_id); /* first run on the next loop turn */
    return co;
}

bool gp_co_done(const gp_co *co) { return co->state == CO_DONE; }
void gp_co_forget_window(gp_co *co, gp_window *w) { if (co->cx.window == w) co->cx.window = NULL; }
bool gp_co_cancelled(const gp_co *co) { return co->cancelled; }

static void co_body(gp_co *co) {
    void *self = co->cx.entity.generation ? gp_entity_get(co->app, co->cx.entity) : NULL;
    co->fn(co, self, &co->cx, co->data);
    co->state = CO_DONE;
}

#ifdef _WIN32

/* Fibers: the thread that runs the loop becomes one the first time a coroutine starts, and stays one. A fiber's
 * stack has its own guard page and grows on demand up to the reserved size. */
void gp_co_free(gp_co *co) {
    if (co->fiber) DeleteFiber(co->fiber);
    free(co);
}

static void CALLBACK co_entry(void *arg) {
    gp_co *co = arg;
    co_body(co);
    SwitchToFiber(co->caller);   /* (a fiber function that returns ends the thread) back into gp_co_resume, for good */
}

void gp_co_resume(gp_app *app, gp_co *co) {
    (void)app;
    if (co->state != CO_NEW && co->state != CO_SUSPENDED) return;
    void *me = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
    if (co->state == CO_NEW) co->fiber = me ? CreateFiberEx(0, CO_STACK_SIZE, FIBER_FLAG_FLOAT_SWITCH, co_entry, co) : NULL;
    if (!co->fiber) {
        fprintf(stderr, "gpuic: could not allocate a coroutine stack\n");
        co->state = CO_DONE;
        return;
    }
    co->state = CO_RUNNING;
    co->caller = me;
    SwitchToFiber(co->fiber);
    if (co->state == CO_DONE) { /* the body returned: its stack is dead */
        DeleteFiber(co->fiber);
        co->fiber = NULL;
    }
}

static void co_switch_out(gp_co *co) { SwitchToFiber(co->caller); }

#else

void gp_co_free(gp_co *co) {
    if (co->map) munmap(co->map, CO_STACK_SIZE + CO_GUARD_SIZE);
    free(co);
}

static void co_entry(unsigned lo, unsigned hi) {
    co_body((gp_co *)(((uintptr_t)hi << 32) | (uintptr_t)lo));
    /* returning switches to uc_link, i.e. back into gp_co_resume */
}

void gp_co_resume(gp_app *app, gp_co *co) {
    (void)app;
    if (co->state != CO_NEW && co->state != CO_SUSPENDED) return;
    if (co->state == CO_NEW) {
        co->map = mmap(NULL, CO_STACK_SIZE + CO_GUARD_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
        if (co->map == MAP_FAILED) {
            co->map = NULL;
            fprintf(stderr, "gpuic: could not allocate a coroutine stack\n");
            co->state = CO_DONE;
            return;
        }
        mprotect(co->map, CO_GUARD_SIZE, PROT_NONE); /* overflow faults instead of corrupting */
        getcontext(&co->ctx);
        co->ctx.uc_stack.ss_sp = (char *)co->map + CO_GUARD_SIZE;
        co->ctx.uc_stack.ss_size = CO_STACK_SIZE;
        co->ctx.uc_link = &co->caller;
        uintptr_t p = (uintptr_t)co;
        makecontext(&co->ctx, (void (*)(void))co_entry, 2, (unsigned)(p & 0xffffffffu),
                    (unsigned)(p >> 32));
    }
    co->state = CO_RUNNING;
    swapcontext(&co->caller, &co->ctx);
    if (co->state == CO_DONE && co->map) { /* the body returned: its stack is dead */
        munmap(co->map, CO_STACK_SIZE + CO_GUARD_SIZE);
        co->map = NULL;
    }
}

static void co_switch_out(gp_co *co) { swapcontext(&co->ctx, &co->caller); }

#endif

void gp_co_cancel(gp_app *app, gp_co *co) {
    if (co->state == CO_DONE) return;
    co->cancelled = true;
    /* Running (we are inside it) or waiting for background work: it notices at its next
     * await / when the work completes. Otherwise wake it now so it can unwind. */
    if (co->state == CO_NEW || (co->state == CO_SUSPENDED && !co->wait_bg)) {
        if (co->wait_job) gp_exec_cancel_id(app, co->wait_job);
        co->wait_job = 0;
        gp_co_resume(app, co);
    }
}

static bool suspend(gp_co *co) {
    co->state = CO_SUSPENDED;
    co_switch_out(co);
    co->wait_job = 0;
    co->wait_bg = false;
    return !co->cancelled;
}

bool gp_sleep(gp_co *co, double seconds) {
    if (co->cancelled) return false;
    co->wait_job = gp_exec_wake_after(co->app, (int64_t)(seconds * 1e9), co->job_id);
    return suspend(co);
}

bool gp_yield(gp_co *co) { return gp_sleep(co, 0); }

bool gp_await_frame(gp_co *co, double *time) {
    if (co->cancelled) return false;
    co->wait_job = gp_exec_wake_frame(co->app, co->job_id, time);
    return suspend(co);
}

#ifdef GP_HAS_FD
bool gp_await_fd(gp_co *co, int fd, int events, double timeout, int *revents) {
    int dummy;
    if (!revents) revents = &dummy;
    *revents = 0;
    if (co->cancelled) return false;
    co->wait_job = gp_exec_wake_fd(co->app, co->job_id, fd, events, timeout >= 0 ? (int64_t)(timeout * 1e9) : -1, revents);
    return suspend(co);
}
#endif

bool gp_await_background(gp_co *co, gp_work_fn work, void *data) {
    if (co->cancelled) return false;
    co->wait_bg = true;
    co->wait_job = gp_exec_wake_background(co->app, co->job_id, work, data);
    return suspend(co);
}

/* ---------------------------------------------------------------- non-blocking I/O */

#ifdef GP_HAS_FD

bool gp_fd_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

/* Waits for `events` on fd; false with errno set (ECANCELED) if the coroutine was cancelled. A hung-up or
 * failed descriptor counts as ready: the retried call then reports the real condition. */
static bool wait_fd(gp_co *co, int fd, int events) {
    int rev;
    if (!gp_await_fd(co, fd, events, -1, &rev)) {
        errno = ECANCELED;
        return false;
    }
    return true;
}

ssize_t gp_co_read(gp_co *co, int fd, void *buf, size_t len) {
    for (;;) {
        ssize_t n = read(fd, buf, len);
        if (n >= 0) return n;
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
        if (!wait_fd(co, fd, GP_FD_READ)) return -1;
    }
}

ssize_t gp_co_write(gp_co *co, int fd, const void *buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, (const char *)buf + done, len - done);
        if (n >= 0) { done += (size_t)n; continue; }
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
        if (!wait_fd(co, fd, GP_FD_WRITE)) return -1;
    }
    return (ssize_t)done;
}

int gp_co_accept(gp_co *co, int listen_fd) {
    for (;;) {
        int fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd >= 0) return fd;
        if (errno == EINTR || errno == ECONNABORTED) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
        if (!wait_fd(co, listen_fd, GP_FD_READ)) return -1;
    }
}

int gp_co_connect(gp_co *co, int fd, const struct sockaddr *addr, socklen_t addrlen) {
    if (connect(fd, addr, addrlen) == 0) return 0;
    if (errno != EINPROGRESS) return -1;
    if (!wait_fd(co, fd, GP_FD_WRITE)) return -1;
    int err = 0;
    socklen_t len = sizeof err;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) return -1;
    if (err) { errno = err; return -1; }
    return 0;
}

#endif   /* GP_HAS_FD */
