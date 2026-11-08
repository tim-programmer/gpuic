/*
 * Descriptors in the event loop.
 *
 *   stdin    gp_watch_fd: when something is piped in, each line shows up as it arrives. Try
 *              (echo one; sleep 1; echo two; sleep 1; echo three) | ./fd
 *   TCP      a loopback echo server and a client, both coroutines (gp_co_accept, gp_co_read, gp_co_write,
 *              gp_co_connect): "Send" opens a connection, sends a line and waits for the echo, all while the UI
 *              keeps running. No threads, no callbacks, no blocking.
 *   timeout  gp_await_fd with a limit: "Wait for stdin" gives up after two seconds.
 *
 * The window sleeps in the event loop the whole time; a descriptor becoming ready wakes it.
 */
#include <gpuic/gpuic.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BASE    gp_rgb(0x1e1e2e)
#define MANTLE  gp_rgb(0x181825)
#define SURFACE gp_rgb(0x313244)
#define OVERLAY gp_rgb(0x45475a)
#define TEXT    gp_rgb(0xcdd6f4)
#define SUBTEXT gp_rgb(0x9399b2)
#define BLUE    gp_rgb(0x89b4fa)
#define GREEN   gp_rgb(0xa6e3a1)
#define PEACH   gp_rgb(0xfab387)

enum { MAX_LINES = 200, LINE = 96 };

typedef struct {
    char lines[MAX_LINES][LINE];
    int n;
    gp_scroll scroll;
    int listen_fd;
    uint16_t port;
    int sent;
    bool stdin_open;
    char partial[LINE];       /* a line that has not ended yet */
    size_t npartial;
    gp_task stdin_watch;
} Demo;

static void log_line(Demo *d, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void log_line(Demo *d, const char *fmt, ...) {
    if (d->n == MAX_LINES) {
        memmove(d->lines[0], d->lines[1], sizeof d->lines - sizeof d->lines[0]);
        d->n--;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d->lines[d->n++], LINE, fmt, ap);
    va_end(ap);
}

/* ---- stdin, from a callback */

static void stdin_ready(void *self, gp_ctx *cx, int fd, int events, void *data) {
    Demo *d = self;
    char buf[256];
    for (;;) {   /* level-triggered: read until it would block */
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] == '\n') {
                    d->partial[d->npartial] = 0;
                    log_line(d, "stdin: %s", d->partial);
                    d->npartial = 0;
                } else if (d->npartial < LINE - 8) {
                    d->partial[d->npartial++] = buf[i];
                }
            }
            continue;
        }
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {   /* end of input */
            if (d->npartial) { d->partial[d->npartial] = 0; log_line(d, "stdin: %s", d->partial); }
            log_line(d, "stdin closed");
            d->stdin_open = false;
            gp_cancel(cx->app, d->stdin_watch);
        }
        break;
    }
    gp_notify(cx);
}

/* ---- the echo server and a client, as coroutines */

static void serve_one(gp_co *co, int fd) {
    char buf[256];
    for (;;) {
        ssize_t n = gp_co_read(co, fd, buf, sizeof buf);
        if (n <= 0) break;
        if (gp_co_write(co, fd, buf, (size_t)n) < 0) break;
    }
    close(fd);
}

static void server(gp_co *co, void *self, gp_ctx *cx, void *data) {
    Demo *d = self;
    int listen_fd = d->listen_fd;
    for (;;) {
        int fd = gp_co_accept(co, listen_fd);
        if (fd < 0) break;                      /* cancelled */
        serve_one(co, fd);
        if (gp_co_cancelled(co)) break;
    }
    close(listen_fd);
}

static void client(gp_co *co, void *self, gp_ctx *cx, void *data) {
    Demo *d = self;
    uint16_t port = d->port;
    int n = ++d->sent;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    char msg[64], reply[64] = "";
    snprintf(msg, sizeof msg, "hello #%d", n);
    ssize_t got = -1;
    if (gp_co_connect(co, fd, (struct sockaddr *)&a, sizeof a) == 0 && gp_co_write(co, fd, msg, strlen(msg)) > 0)
        got = gp_co_read(co, fd, reply, sizeof reply - 1);
    close(fd);
    if (gp_co_cancelled(co)) return;            /* the window is going: `d` may be gone */
    d = gp_entity_get(cx->app, cx->entity);
    if (!d) return;
    if (got > 0) { reply[got] = 0; log_line(d, "tcp: sent \"%s\", echoed \"%s\"", msg, reply); }
    else log_line(d, "tcp: failed (%s)", strerror(errno));
    gp_notify(cx);
}

static void send_clicked(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    gp_spawn(cx, client, NULL);
}

/* ---- gp_await_fd with a limit */

static void wait_stdin(gp_co *co, void *self, gp_ctx *cx, void *data) {
    Demo *d = self;
    int rev = 0;
    bool open = d->stdin_open;
    log_line(d, "waiting up to 2 s for stdin%s", open ? " (the watch above reads it too)" : " (it is closed)");
    gp_notify(cx);
    if (!gp_await_fd(co, STDIN_FILENO, GP_FD_READ, 2.0, &rev)) return;
    d = gp_entity_get(cx->app, cx->entity);
    if (!d) return;
    log_line(d, rev ? "stdin became ready" : "no input within 2 s");
    gp_notify(cx);
}

static void wait_clicked(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    gp_spawn(cx, wait_stdin, NULL);
}

/* ---- the view */

static gp_node *line_row(void *self, gp_ctx *cx, size_t i, void *data) {
    const Demo *d = self;
    const char *s = i < (size_t)d->n ? d->lines[i] : "";
    return gp_div(gp_h(20), gp_px(10), gp_flex_row(), gp_items_center(),
                  gp_text(s, gp_text_size(13), gp_text_color(strncmp(s, "tcp", 3) == 0 ? GREEN : strncmp(s, "stdin", 5) == 0 ? BLUE : SUBTEXT)));
}

static gp_node *button(const char *label, gp_mouse_fn fn) {
    return gp_div(gp_px(14), gp_py(8), gp_rounded(8), gp_bg(SURFACE), gp_hover_bg(OVERLAY), gp_cursor_pointer(),
                  gp_on_click(fn), gp_text(label, gp_font_bold()));
}

static gp_node *demo_render(void *self, gp_ctx *cx) {
    Demo *d = self;
    return gp_div(
        gp_size_full(), gp_flex_col(), gp_gap(14), gp_p(20), gp_bg(BASE), gp_text_color(TEXT),
        gp_text("Descriptors in the event loop", gp_text_size(20), gp_font_bold()),
        gp_text(gp_fmt("echo server on 127.0.0.1:%u · stdin %s", d->port, d->stdin_open ? "watched" : "closed or a terminal"),
                gp_text_size(13), gp_text_color(SUBTEXT)),
        gp_div(gp_flex_row(), gp_gap(10), button("Send over TCP", send_clicked), button("Wait for stdin", wait_clicked)),
        gp_list((size_t)d->n, 20, line_row, NULL, gp_track_scroll(d->scroll), gp_list_stick_to_end(), gp_flex_1(), gp_rounded(8),
                gp_bg(MANTLE)));
}

static void ready(gp_app *app, void *userdata) {
    gp_entity demo = gp_view_new(app, sizeof(Demo), &(Demo){ .scroll = gp_scroll_new(app) }, demo_render, NULL);
    Demo *d = gp_entity_get(app, demo);
    gp_ctx cx = gp_ctx_for(app, demo);

    d->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t len = sizeof a;
    if (bind(d->listen_fd, (struct sockaddr *)&a, sizeof a) == 0 && listen(d->listen_fd, 8) == 0 &&
        getsockname(d->listen_fd, (struct sockaddr *)&a, &len) == 0) {
        d->port = ntohs(a.sin_port);
        gp_spawn(&cx, server, NULL);
    }

    if (!isatty(STDIN_FILENO)) {   /* something is piped in */
        gp_fd_set_nonblocking(STDIN_FILENO);
        d->stdin_open = true;
        d->stdin_watch = gp_watch_fd(&cx, STDIN_FILENO, GP_FD_READ, stdin_ready, NULL);
    }
    log_line(d, "ready");
    gp_open_window(app, &(gp_window_options){ .title = "gpuic: descriptors", .width = 640, .height = 440 }, demo);
}

int main(void) { return gp_application_run(ready, NULL); }
