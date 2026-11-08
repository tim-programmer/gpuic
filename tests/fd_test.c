/*
 * File-descriptor watching: callbacks (gp_watch_fd) and awaits (gp_await_fd, gp_co_read, ...).
 * Needs a display, like exec_test: it opens a tiny window to get an event loop.
 *
 * Time is real here (pipes are filled from this thread, from another thread and from the other end of a
 * socket), so the checks leave generous room and look for what must or must not happen, not exact timing.
 */
#define _GNU_SOURCE   /* pipe2 */
#include "../src/internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

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

static void msleep(int ms) { nanosleep(&(struct timespec){ 0, ms * 1000000L }, NULL); }

typedef struct {
    int fired;
    int events;             /* of the last call */
    int fd;                 /* of the last call */
    int drain;              /* read what is there on each call */
    int cancel_after;       /* cancel the watch from its own callback after this many calls (0 = never) */
    gp_task task;
    char got[4096];
    size_t ngot;
    int hup, invalid, readable, writable, error;
} Watch;

static void on_fd(void *self, gp_ctx *cx, int fd, int events, void *data) {
    Watch *w = data ? data : self;
    w->fired++;
    w->events = events;
    w->fd = fd;
    if (events & GP_FD_HUP) w->hup++;
    if (events & GP_FD_INVALID) w->invalid++;
    if (events & GP_FD_READ) w->readable++;
    if (events & GP_FD_WRITE) w->writable++;
    if (events & GP_FD_ERROR) w->error++;
    if (w->drain && (events & GP_FD_READ)) {
        char buf[512];
        ssize_t n;
        while ((n = read(fd, buf, sizeof buf)) > 0)
            for (ssize_t i = 0; i < n && w->ngot < sizeof w->got; i++) w->got[w->ngot++] = buf[i];
    }
    if (w->cancel_after && w->fired >= w->cancel_after) gp_cancel(cx->app, w->task);
}

static void make_pipe(int p[2]) {
    if (pipe2(p, O_NONBLOCK | O_CLOEXEC) != 0) { perror("pipe2"); exit(2); }
}

/* ------------------------------------------------------------ callbacks */

static void test_readable(gp_co *co, gp_app *app) {
    int p[2];
    make_pipe(p);
    Watch w = { .drain = 1 };
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    w.task = gp_watch_fd(&cx, p[0], GP_FD_READ, on_fd, &w);
    CHECK(gp_task_active(app, w.task), "a new watch is active");

    gp_sleep(co, 0.1);
    CHECK(w.fired == 0, "an idle pipe called back %d times", w.fired);

    CHECK(write(p[1], "hello", 5) == 5, "write");
    gp_sleep(co, 0.1);
    CHECK(w.fired == 1 && w.ngot == 5 && memcmp(w.got, "hello", 5) == 0, "after a write: %d calls, got %zu bytes", w.fired, w.ngot);
    CHECK(w.events == GP_FD_READ && w.fd == p[0], "events %d, fd %d (want %d, %d)", w.events, w.fd, GP_FD_READ, p[0]);

    /* the watch survives its own callback: again, and again */
    for (int i = 0; i < 5; i++) {
        CHECK(write(p[1], "x", 1) == 1, "write");
        gp_sleep(co, 0.03);
    }
    CHECK(w.fired == 6, "six writes, %d calls", w.fired);

    gp_cancel(app, w.task);
    CHECK(!gp_task_active(app, w.task), "a cancelled watch is not active");
    int fired = w.fired;
    CHECK(write(p[1], "y", 1) == 1, "write");
    gp_sleep(co, 0.1);
    CHECK(w.fired == fired, "a cancelled watch called back");
    close(p[0]);
    close(p[1]);
    gp_sleep(co, 0.05);
}

/* Level-triggered: it keeps calling while the data is there, and stops when it is read. */
static void test_level_triggered(gp_co *co, gp_app *app) {
    int p[2];
    make_pipe(p);
    Watch w = { 0 };
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    w.task = gp_watch_fd(&cx, p[0], GP_FD_READ, on_fd, &w);
    CHECK(write(p[1], "abc", 3) == 3, "write");
    gp_sleep(co, 0.1);
    CHECK(w.fired >= 5, "unread data called back only %d times in 100 ms", w.fired);
    w.drain = 1;
    gp_sleep(co, 0.05);
    int fired = w.fired;
    gp_sleep(co, 0.1);
    CHECK(w.fired == fired, "after reading everything it still called back (%d -> %d)", fired, w.fired);
    CHECK(w.ngot == 3, "read %zu bytes", w.ngot);
    gp_cancel(app, w.task);
    close(p[0]);
    close(p[1]);
}

/* A watch on an idle descriptor costs no wakeups at all. */
static void test_idle(gp_co *co, gp_app *app) {
    int p[2];
    make_pipe(p);
    Watch w = { 0 };
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_task t = gp_watch_fd(&cx, p[0], GP_FD_READ, on_fd, &w);
    gp_sleep(co, 0.05);
    unsigned long before = app->stat_wakeups;
    gp_sleep(co, 0.4);
    unsigned long woke = app->stat_wakeups - before;
    CHECK(woke <= 4, "the loop woke %lu times in 400 ms with one idle watch", woke);
    gp_cancel(app, t);
    close(p[0]);
    close(p[1]);
}

static void test_writable(gp_co *co, gp_app *app) {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0, "socketpair");
    /* fill the send buffer */
    char block[4096];
    memset(block, 'z', sizeof block);
    size_t total = 0;
    for (;;) {
        ssize_t n = write(sv[0], block, sizeof block);
        if (n < 0) break;
        total += (size_t)n;
    }
    CHECK(errno == EAGAIN, "filling the socket ended with errno %d", errno);

    Watch w = { 0 };
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    w.task = gp_watch_fd(&cx, sv[0], GP_FD_WRITE, on_fd, &w);
    gp_sleep(co, 0.1);
    CHECK(w.fired == 0, "a full socket reported writable (%d)", w.fired);

    /* the other side reads: room opens up (a unix socket is writable again once it is mostly empty) */
    size_t drained = 0;
    char buf[8192];
    ssize_t n;
    while ((n = read(sv[1], buf, sizeof buf)) > 0) drained += (size_t)n;
    CHECK(drained == total, "drained %zu of %zu bytes", drained, total);
    gp_sleep(co, 0.1);
    CHECK(w.fired >= 1 && (w.events & GP_FD_WRITE), "no write-ready call after the other side read (%d calls)", w.fired);
    w.cancel_after = w.fired + 1;   /* it is level-triggered: it will be called again right away */
    gp_sleep(co, 0.1);
    CHECK(!gp_task_active(app, w.task), "the watch cancelled itself from its callback and is still active");
    close(sv[0]);
    close(sv[1]);
}

static void test_hup(gp_co *co, gp_app *app) {
    int p[2];
    make_pipe(p);
    Watch w = { .drain = 1, .cancel_after = 0 };
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    w.task = gp_watch_fd(&cx, p[0], GP_FD_READ, on_fd, &w);
    CHECK(write(p[1], "bye", 3) == 3, "write");
    close(p[1]);
    gp_sleep(co, 0.1);
    CHECK(w.hup >= 1, "no HUP after the writer closed (%d calls)", w.hup);
    CHECK(w.ngot == 3 && memcmp(w.got, "bye", 3) == 0, "data before the hang-up: %zu bytes", w.ngot);
    gp_cancel(app, w.task);
    close(p[0]);
}

static void test_closed_under_a_watch(gp_co *co, gp_app *app) {
    int p[2], q[2];
    make_pipe(p);
    make_pipe(q);       /* before the close, so that the new descriptors cannot take the number that is about to be free */
    Watch w = { 0 };
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    w.task = gp_watch_fd(&cx, p[0], GP_FD_READ, on_fd, &w);
    gp_sleep(co, 0.05);
    close(p[0]);        /* a bug in the caller, but it must not hang or spin. (p[1] stays open: closing it would hang the
                           pipe up for the poll that is still holding the old file, which is a different event.) */
    /* poll() only learns of it when it starts again; adding a watch makes it */
    Watch w2 = { .drain = 1 };
    w2.task = gp_watch_fd(&cx, q[0], GP_FD_READ, on_fd, &w2);
    gp_sleep(co, 0.1);
    CHECK(w.invalid == 1 && w.fired == 1, "closing a watched descriptor: %d calls, %d with INVALID", w.fired, w.invalid);
    CHECK(!gp_task_active(app, w.task), "the watch on a closed descriptor ended");
    gp_cancel(app, w2.task);
    close(p[1]);
    close(q[0]);
    close(q[1]);
}

/* Owned by an entity: releasing it ends the watch; the callback gets the entity's state as `self`. */
typedef struct { int fired; } Owner;
static void on_owned(void *self, gp_ctx *cx, int fd, int events, void *data) {
    ((Owner *)self)->fired++;
    char buf[64];
    while (read(fd, buf, sizeof buf) > 0) {}
}
static void test_entity(gp_co *co, gp_app *app) {
    int p[2];
    make_pipe(p);
    gp_entity e = gp_entity_new(app, sizeof(Owner), NULL, NULL);
    Owner *o = gp_entity_get(app, e);
    gp_ctx cx = gp_ctx_for(app, e);
    gp_task t = gp_watch_fd(&cx, p[0], GP_FD_READ, on_owned, NULL);
    CHECK(write(p[1], "1", 1) == 1, "write");
    gp_sleep(co, 0.08);
    CHECK(o->fired == 1, "the owner's callback ran %d times", o->fired);
    gp_entity_release(app, e);
    CHECK(!gp_task_active(app, t), "releasing the owner ended the watch");
    CHECK(write(p[1], "2", 1) == 1, "write");
    gp_sleep(co, 0.08);
    close(p[0]);
    close(p[1]);
}

/* Descriptor numbers are reused: a cancelled watch must not see the next pipe that gets the same number. */
static void test_reused_descriptor(gp_co *co, gp_app *app) {
    int p[2];
    make_pipe(p);
    Watch old = { 0 };
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    old.task = gp_watch_fd(&cx, p[0], GP_FD_READ, on_fd, &old);
    gp_sleep(co, 0.03);
    int oldfd = p[0];
    gp_cancel(app, old.task);
    close(p[0]);
    close(p[1]);
    int q[2];
    make_pipe(q);
    CHECK(q[0] == oldfd || q[1] == oldfd || true, "(informational) reused number");
    CHECK(write(q[1], "z", 1) == 1, "write");
    gp_sleep(co, 0.1);
    CHECK(old.fired == 0, "a cancelled watch saw a new descriptor with the same number (%d)", old.fired);
    close(q[0]);
    close(q[1]);
}

/* Callbacks that add, cancel and close while others are being delivered. */
typedef struct { Watch a, b; int p[2], q[2]; gp_task tb; } Chain;
static void chain_a(void *self, gp_ctx *cx, int fd, int events, void *data) {
    Chain *c = data;
    c->a.fired++;
    char buf[8];
    while (read(fd, buf, sizeof buf) > 0) {}
    gp_cancel(cx->app, c->tb);                  /* cancels a sibling that is ready in the same turn */
    gp_cancel(cx->app, c->a.task);
}
static void test_callbacks_modify_watches(gp_co *co, gp_app *app) {
    Chain c = { 0 };
    make_pipe(c.p);
    make_pipe(c.q);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    c.a.task = gp_watch_fd(&cx, c.p[0], GP_FD_READ, chain_a, &c);
    c.tb = gp_watch_fd(&cx, c.q[0], GP_FD_READ, on_fd, &c.b);
    CHECK(write(c.p[1], "1", 1) == 1 && write(c.q[1], "1", 1) == 1, "write");
    gp_sleep(co, 0.1);
    CHECK(c.a.fired == 1, "first watch called %d times", c.a.fired);
    CHECK(c.b.fired <= 1, "second watch called %d times", c.b.fired);
    gp_cancel(app, c.tb);
    close(c.p[0]); close(c.p[1]); close(c.q[0]); close(c.q[1]);
}

/* Many descriptors, each delivered exactly the data written to it. */
enum { MANY = 300 };
typedef struct { int p[2]; Watch w; } Slot;
static Slot slots[MANY];
static void test_many(gp_co *co, gp_app *app) {
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    for (int i = 0; i < MANY; i++) {
        make_pipe(slots[i].p);
        slots[i].w.drain = 1;
        slots[i].w.task = gp_watch_fd(&cx, slots[i].p[0], GP_FD_READ, on_fd, &slots[i].w);
    }
    for (int i = 0; i < MANY; i += 3) { char c = (char)('a' + i % 26); CHECK(write(slots[i].p[1], &c, 1) == 1, "write"); }
    gp_sleep(co, 0.3);
    int bad = 0;
    for (int i = 0; i < MANY; i++) {
        int want = i % 3 == 0;
        if (slots[i].w.ngot != (size_t)want || (want && slots[i].w.got[0] != (char)('a' + i % 26)) || slots[i].w.fired != want) bad++;
    }
    CHECK(bad == 0, "%d of %d descriptors got the wrong thing", bad, MANY);
    /* the rest, in reverse order */
    for (int i = MANY - 1; i >= 0; i--) if (i % 3) { char c = 'q'; CHECK(write(slots[i].p[1], &c, 1) == 1, "write"); }
    gp_sleep(co, 0.4);
    bad = 0;
    for (int i = 0; i < MANY; i++) if (slots[i].w.fired != 1) bad++;
    CHECK(bad == 0, "%d of %d descriptors were not called exactly once", bad, MANY);
    for (int i = 0; i < MANY; i++) {
        gp_cancel(app, slots[i].w.task);
        close(slots[i].p[0]);
        close(slots[i].p[1]);
    }
    gp_sleep(co, 0.05);
}

/* Written from another thread, as a worker or a child process would. */
static void *late_writer(void *arg) {
    int fd = *(int *)arg;
    msleep(80);
    ssize_t r = write(fd, "from a thread", 13);
    (void)r;
    return NULL;
}
static void test_other_thread(gp_co *co, gp_app *app) {
    int p[2];
    make_pipe(p);
    Watch w = { .drain = 1 };
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    w.task = gp_watch_fd(&cx, p[0], GP_FD_READ, on_fd, &w);
    pthread_t th;
    pthread_create(&th, NULL, late_writer, &p[1]);
    double t0 = gp_time();
    while (!w.fired && gp_time() - t0 < 2) gp_sleep(co, 0.005);
    double took = gp_time() - t0;
    pthread_join(th, NULL);
    CHECK(w.fired == 1 && w.ngot == 13, "thread write: %d calls, %zu bytes", w.fired, w.ngot);
    CHECK(took > 0.07 && took < 0.3, "delivered after %.3f s (written at 0.08)", took);
    gp_cancel(app, w.task);
    close(p[0]);
    close(p[1]);
}

/* ------------------------------------------------------------ awaits */

static void test_await(gp_co *co, gp_app *app) {
    int p[2];
    make_pipe(p);
    int rev = -1;
    double t0 = gp_time();
    CHECK(gp_await_fd(co, p[0], GP_FD_READ, 0.1, &rev), "await returned false while not cancelled");
    double took = gp_time() - t0;
    CHECK(rev == 0, "a timed-out await reported events %d", rev);
    CHECK(took > 0.09 && took < 0.3, "timeout took %.3f s", took);

    CHECK(write(p[1], "k", 1) == 1, "write");
    t0 = gp_time();
    CHECK(gp_await_fd(co, p[0], GP_FD_READ, 5, &rev), "await");
    CHECK(rev == GP_FD_READ, "events %d", rev);
    CHECK(gp_time() - t0 < 0.2, "a ready descriptor took %.3f s", gp_time() - t0);
    char c;
    CHECK(read(p[0], &c, 1) == 1, "read");

    /* the timeout is cancelled with the await: nothing wakes this coroutine from a stale deadline */
    CHECK(gp_await_fd(co, p[0], GP_FD_READ, 0.05, &rev) && rev == 0, "second timeout");
    CHECK(write(p[1], "k", 1) == 1, "write");
    CHECK(gp_await_fd(co, p[0], GP_FD_READ, 0.05, &rev) && rev == GP_FD_READ, "ready with a deadline");
    t0 = gp_time();
    gp_sleep(co, 0.15);      /* the deadline of the await above passed meanwhile: it must not resume this sleep early */
    CHECK(gp_time() - t0 > 0.14, "sleep was cut short to %.3f s by a stale wake", gp_time() - t0);
    CHECK(read(p[0], &c, 1) == 1, "read");

    /* a poll-style await with no limit, released by a write from elsewhere */
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    pthread_t th;
    pthread_create(&th, NULL, late_writer, &p[1]);
    CHECK(gp_await_fd(co, p[0], GP_FD_READ, -1, &rev) && rev == GP_FD_READ, "unbounded await");
    pthread_join(th, NULL);
    (void)cx;
    close(p[0]);
    close(p[1]);
}

typedef struct { int fd; bool returned, result; int rev; } Waiter;
static void waiter(gp_co *co, void *self, gp_ctx *cx, void *data) {
    Waiter *w = data;
    w->result = gp_await_fd(co, w->fd, GP_FD_READ, -1, &w->rev);
    w->returned = true;
}
static void test_cancel_await(gp_co *co, gp_app *app) {
    int p[2];
    make_pipe(p);
    Waiter w = { .fd = p[0] };
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_task t = gp_spawn(&cx, waiter, &w);
    gp_sleep(co, 0.1);
    CHECK(!w.returned && gp_task_active(app, t), "the waiter should be suspended");
    gp_cancel(app, t);
    gp_sleep(co, 0.02);
    CHECK(w.returned && !w.result, "a cancelled await returned %d (returned=%d)", w.result, w.returned);
    CHECK(!gp_task_active(app, t), "cancelled coroutine still active");
    /* the owner going away has the same effect */
    gp_entity e = gp_entity_new(app, 8, NULL, NULL);
    Waiter w2 = { .fd = p[0] };
    gp_ctx ce = gp_ctx_for(app, e);
    gp_spawn(&ce, waiter, &w2);
    gp_sleep(co, 0.05);
    gp_entity_release(app, e);
    gp_sleep(co, 0.05);
    CHECK(w2.returned && !w2.result, "an await owned by a released entity returned %d", w2.result);
    close(p[0]);
    close(p[1]);
}

/* A TCP echo server and client, both coroutines, moving more than a socket buffer holds. */
enum { PAYLOAD = 1 << 20 };
typedef struct { int listen_fd; uint16_t port; bool server_done, writer_done, reader_done; size_t served; unsigned char *sent, *received; size_t nreceived; int cfd; bool connected; int connect_errno; } Echo;

static void echo_server(gp_co *co, void *self, gp_ctx *cx, void *data) {
    Echo *e = data;
    int fd = gp_co_accept(co, e->listen_fd);
    if (fd < 0) { e->server_done = true; return; }
    char buf[4096];
    for (;;) {
        ssize_t n = gp_co_read(co, fd, buf, sizeof buf);
        if (n <= 0) break;
        if (gp_co_write(co, fd, buf, (size_t)n) != n) break;
        e->served += (size_t)n;
    }
    close(fd);
    e->server_done = true;
}

static void echo_writer(gp_co *co, void *self, gp_ctx *cx, void *data) {
    Echo *e = data;
    if (gp_co_connect(co, e->cfd, (struct sockaddr *)&(struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons(e->port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) }, sizeof(struct sockaddr_in)) < 0) {
        e->connect_errno = errno;
        e->writer_done = true;
        return;
    }
    e->connected = true;
    size_t off = 0;
    while (off < PAYLOAD) {
        size_t chunk = PAYLOAD - off < 100000 ? PAYLOAD - off : 100000;
        if (gp_co_write(co, e->cfd, e->sent + off, chunk) != (ssize_t)chunk) break;
        off += chunk;
    }
    shutdown(e->cfd, SHUT_WR);
    e->writer_done = off == PAYLOAD;
}

static void echo_reader(gp_co *co, void *self, gp_ctx *cx, void *data) {
    Echo *e = data;
    while (!e->connected && !e->writer_done) gp_sleep(co, 0.005);
    while (e->nreceived < PAYLOAD) {
        ssize_t n = gp_co_read(co, e->cfd, e->received + e->nreceived, PAYLOAD - e->nreceived);
        if (n <= 0) break;
        e->nreceived += (size_t)n;
    }
    e->reader_done = true;
}

static void test_tcp_echo(gp_co *co, gp_app *app) {
    Echo e = { 0 };
    e.sent = malloc(PAYLOAD);
    e.received = malloc(PAYLOAD);
    for (size_t i = 0; i < PAYLOAD; i++) e.sent[i] = (unsigned char)(i * 2654435761u >> 13);
    e.listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int one = 1;
    setsockopt(e.listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    CHECK(bind(e.listen_fd, (struct sockaddr *)&a, sizeof a) == 0 && listen(e.listen_fd, 4) == 0, "listen");
    socklen_t len = sizeof a;
    getsockname(e.listen_fd, (struct sockaddr *)&a, &len);
    e.port = ntohs(a.sin_port);
    e.cfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int small = 8192;   /* a tiny send buffer: a write of 100 KB goes in many pieces, with EAGAIN between them */
    setsockopt(e.cfd, SOL_SOCKET, SO_SNDBUF, &small, sizeof small);

    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, echo_server, &e);
    gp_spawn(&cx, echo_writer, &e);
    gp_spawn(&cx, echo_reader, &e);
    double t0 = gp_time();
    while (!(e.server_done && e.writer_done && e.reader_done) && gp_time() - t0 < 10) gp_sleep(co, 0.02);
    CHECK(e.connected, "client did not connect (errno %d)", e.connect_errno);
    CHECK(e.writer_done && e.reader_done && e.server_done, "echo unfinished: writer %d reader %d server %d (received %zu, served %zu)",
          e.writer_done, e.reader_done, e.server_done, e.nreceived, e.served);
    CHECK(e.nreceived == PAYLOAD && memcmp(e.sent, e.received, PAYLOAD) == 0, "the echo of %d bytes came back as %zu bytes, %s", PAYLOAD,
          e.nreceived, e.nreceived == PAYLOAD ? "different" : "short");
    close(e.cfd);
    close(e.listen_fd);
    free(e.sent);
    free(e.received);
}

static void refused(gp_co *co, void *self, gp_ctx *cx, void *data) {
    int *out = data;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    /* a port that was just closed: nothing listens */
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    bind(probe, (struct sockaddr *)&a, sizeof a);
    socklen_t len = sizeof a;
    getsockname(probe, (struct sockaddr *)&a, &len);
    close(probe);
    int r = gp_co_connect(co, fd, (struct sockaddr *)&a, sizeof a);
    out[0] = r;
    out[1] = errno;
    close(fd);
}
static void test_connect_refused(gp_co *co, gp_app *app) {
    int out[2] = { 99, 0 };
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, refused, out);
    gp_sleep(co, 0.2);
    CHECK(out[0] == -1 && out[1] == ECONNREFUSED, "connect to a closed port: %d, errno %d (%s)", out[0], out[1], strerror(out[1]));
}

static void reading_forever(gp_co *co, void *self, gp_ctx *cx, void *data) {
    int *r = data;
    char buf[8];
    errno = 0;
    r[0] = (int)gp_co_read(co, r[2], buf, sizeof buf);
    r[1] = errno;
}
static void test_read_cancelled(gp_co *co, gp_app *app) {
    int p[2];
    make_pipe(p);
    int r[3] = { 99, 0, p[0] };
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_task t = gp_spawn(&cx, reading_forever, r);
    gp_sleep(co, 0.05);
    gp_cancel(app, t);
    gp_sleep(co, 0.03);
    CHECK(r[0] == -1 && r[1] == ECANCELED, "gp_co_read cancelled: %d, errno %d", r[0], r[1]);
    close(p[0]);
    close(p[1]);
}

/* ------------------------------------------------------------ driver */

static void leave_watches(gp_co *co, gp_app *app, int pipes[3][2]) {
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    static Watch w[3];
    for (int i = 0; i < 3; i++) {
        make_pipe(pipes[i]);
        gp_watch_fd(&cx, pipes[i][0], GP_FD_READ, on_fd, &w[i]);
    }
    static Waiter wa;
    wa.fd = pipes[0][0];
    gp_spawn(&cx, waiter, &wa);
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    gp_app *app = cx->app;
    test_readable(co, app);
    test_level_triggered(co, app);
    test_idle(co, app);
    test_writable(co, app);
    test_hup(co, app);
    test_closed_under_a_watch(co, app);
    test_entity(co, app);
    test_reused_descriptor(co, app);
    test_callbacks_modify_watches(co, app);
    test_many(co, app);
    test_other_thread(co, app);
    test_await(co, app);
    test_cancel_await(co, app);
    test_tcp_echo(co, app);
    test_connect_refused(co, app);
    test_read_cancelled(co, app);

    /* quit with watches and a waiting coroutine still in flight: shutdown must stop the poller and unwind */
    int pipes[3][2];
    leave_watches(co, app, pipes);
    gp_sleep(co, 0.05);
    gp_quit(app);
}

static gp_node *view(void *self, gp_ctx *cx) { return gp_div(gp_size_full()); }

static void ready(gp_app *app, void *userdata) {
    gp_entity root = gp_view_new(app, 0, NULL, view, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "fd_test", .width = 200, .height = 100 }, root);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);   /* a physical pointer over the window is not part of the test */
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "fd_test: %d FAILURE(S)\n" : "fd_test: all checks passed\n", failures);
    return failures != 0;
}
