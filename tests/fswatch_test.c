/*
 * Watching files and directories (gp_watch_path), on Linux and Windows: in a fresh temporary directory, files are made, written, renamed
 * and removed from the test (standard C calls), and the events that come back are checked for kind and path. A directory watch (each kind,
 * a rename's two names, nothing about a subdirectory's contents unless recursive), a recursive watch (a directory made later, and one moved in
 * with files already in it), a file watch (only its name; replaced by a rename; deleted and made again; a file that does not exist yet), two
 * watches on one directory, cancelling (gp_cancel and the owning entity released), a path that cannot be watched, the watched directory
 * deleted (GONE, Linux), and quitting with watches live. Needs a display, like fd_test: a tiny window gives an event loop.
 *
 * Time is real: events are waited for (up to 2 s each), and "nothing came" is checked after a pause in which the system would have spoken.
 */
#include "../src/internal.h"
#include "../src/threads.h"
#include "compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
# include <direct.h>
# define mkdir_(p) _mkdir(p)
#else
# include <sys/stat.h>
# include <unistd.h>
# define mkdir_(p) mkdir(p, 0755)
#endif

static int failures;
#define CHECK(cond, ...)                                \
    do {                                                \
        if(!(cond)) {                                   \
            failures++;                                 \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while(0)

static char root[512];
static char* P(const char* rel)
{ /* a path under the test's directory (a few at once) */
    static char buf[8][600];
    static int k;
    k = (k + 1) % 8;
    snprintf(buf[k], sizeof buf[k], "%s/%s", root, rel);
    return buf[k];
}
static void write_file(const char* path, const char* text)
{
    FILE* f = fopen(path, "wb");
    if(!f) {
        printf("cannot write %s\n", path);
        exit(2);
    }
    fputs(text, f);
    fclose(f);
}
static void append_file(const char* path, const char* text)
{
    FILE* f = fopen(path, "ab");
    if(!f) {
        printf("cannot append %s\n", path);
        exit(2);
    }
    fputs(text, f);
    fclose(f);
}

/* ------------------------------------------------------------ recording */

typedef struct
{
    int kind;
    char path[600], old[600];
} Ev;
typedef struct
{
    Ev ev[256];
    int n;
    int calls;
} Log;

static void on_event(void* self, gp_ctx* cx, const gp_path_event* e, void* data)
{
    Log* l = data;
    l->calls++;
    if(l->n >= 256) {
        return;
    }
    Ev* v = &l->ev[l->n++];
    v->kind = e->kind;
    snprintf(v->path, sizeof v->path, "%s", e->path ? e->path : "");
    snprintf(v->old, sizeof v->old, "%s", e->old_path ? e->old_path : "");
}

/* Windows names paths case-insensitively and with either separator */
static bool same_path(const char* a, const char* b)
{
    for(; *a && *b; a++, b++) {
        char x = *a == '\\' ? '/' : *a, y = *b == '\\' ? '/' : *b;
#ifdef _WIN32
        if(x >= 'A' && x <= 'Z') {
            x = (char)(x + 32);
        }
        if(y >= 'A' && y <= 'Z') {
            y = (char)(y + 32);
        }
#endif
        if(x != y) {
            return false;
        }
    }
    return *a == *b;
}
static int find(const Log* l, int kind, const char* path)
{
    for(int i = 0; i < l->n; i++) {
        if(l->ev[i].kind == kind && same_path(l->ev[i].path, path)) {
            return i;
        }
    }
    return -1;
}
static int count_about(const Log* l, const char* path)
{
    int c = 0;
    for(int i = 0; i < l->n; i++) {
        if(same_path(l->ev[i].path, path) || same_path(l->ev[i].old, path)) {
            c++;
        }
    }
    return c;
}
/* waits (up to 2 s) until the log has an event of that kind about that path */
static bool wait_for(gp_co* co, const Log* l, int kind, const char* path)
{
    for(int i = 0; i < 200; i++) {
        if(find(l, kind, path) >= 0) {
            return true;
        }
        gp_sleep(co, 0.01);
    }
    return false;
}
static void settle(gp_co* co)
{
    gp_sleep(co, 0.3);
}
static void dump(const char* what, const Log* l)
{
    printf("  %s: %d events\n", what, l->n);
    for(int i = 0; i < l->n; i++) {
        printf("    %d %s %s\n", l->ev[i].kind, l->ev[i].path, l->ev[i].old);
    }
}

/* ------------------------------------------------------------ tests */

static void test_directory(gp_co* co, gp_app* app)
{
    mkdir_(P("d"));
    mkdir_(P("d/sub"));
    write_file(P("d/sub/deep.txt"), "x");
    static Log l;
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_task t = gp_watch_path(&cx, P("d"), 0, on_event, &l);
    CHECK(gp_task_active(app, t), "dir: a directory can be watched");

    write_file(P("d/a.txt"), "hello");
    CHECK(wait_for(co, &l, GP_PATH_CREATED, P("d/a.txt")), "dir: a file made in it is CREATED, with its full path");
    append_file(P("d/a.txt"), " world");
    CHECK(wait_for(co, &l, GP_PATH_MODIFIED, P("d/a.txt")), "dir: written to is MODIFIED");
    rename(P("d/a.txt"), P("d/b.txt"));
    CHECK(wait_for(co, &l, GP_PATH_RENAMED, P("d/b.txt")), "dir: a rename inside it is RENAMED");
    int r = find(&l, GP_PATH_RENAMED, P("d/b.txt"));
    CHECK(r >= 0 && same_path(l.ev[r].old, P("d/a.txt")), "dir: with where it was (%s)", r >= 0 ? l.ev[r].old : "-");
    remove(P("d/b.txt"));
    CHECK(wait_for(co, &l, GP_PATH_DELETED, P("d/b.txt")), "dir: removed is DELETED");
    write_file(P("d/leaving.txt"), "x");
    CHECK(wait_for(co, &l, GP_PATH_CREATED, P("d/leaving.txt")), "dir: (made)");
    rename(P("d/leaving.txt"), P("left.txt"));
    CHECK(wait_for(co, &l, GP_PATH_DELETED, P("d/leaving.txt")), "dir: moved out of it is DELETED");
    rename(P("left.txt"), P("d/back.txt"));
    CHECK(wait_for(co, &l, GP_PATH_CREATED, P("d/back.txt")), "dir: moved in from outside is CREATED");
    append_file(P("d/sub/deep.txt"), "y");
    settle(co);
    CHECK(count_about(&l, P("d/sub/deep.txt")) == 0, "dir: not recursive: nothing about a file in a subdirectory");
    /* the order is the order things happened */
    int c = find(&l, GP_PATH_CREATED, P("d/a.txt")), m = find(&l, GP_PATH_MODIFIED, P("d/a.txt")), d = find(&l, GP_PATH_DELETED, P("d/b.txt"));
    d = find(&l, GP_PATH_DELETED, P("d/b.txt"));
    CHECK(c >= 0 && c < m && m < r && r < d, "dir: in the order they happened (%d %d %d %d)", c, m, r, d);
    /* many writes at once arrive as few events, not one per write */
    l.n = 0;
    for(int i = 0; i < 50; i++) { /* (alternating: the system cannot fold them, they are folded here) */
        append_file(P("d/c.txt"), "z");
        append_file(P("d/e.txt"), "z");
    }
    CHECK(wait_for(co, &l, GP_PATH_MODIFIED, P("d/c.txt")) && wait_for(co, &l, GP_PATH_MODIFIED, P("d/e.txt")), "dir: writes again");
    settle(co);
    int mods = 0;
    for(int i = 0; i < l.n; i++) {
        mods += l.ev[i].kind == GP_PATH_MODIFIED;
    }
    CHECK(mods >= 2 && mods <= 6, "dir: a hundred writes to two files, made at once, come as a MODIFIED or so each (%d)", mods);
    if(failures) {
        dump("directory", &l);
    }
    gp_cancel(app, t);
    settle(co);
    int before = l.calls;
    write_file(P("d/after.txt"), "x");
    settle(co);
    CHECK(l.calls == before && !gp_task_active(app, t), "dir: cancelled, nothing more comes");
}

static void test_recursive(gp_co* co, gp_app* app)
{
    mkdir_(P("r"));
    mkdir_(P("r/x"));
    mkdir_(P("r/x/y"));
    write_file(P("r/x/y/f.txt"), "1");
    static Log l;
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_task t = gp_watch_path(&cx, P("r"), GP_WATCH_RECURSIVE, on_event, &l);
    append_file(P("r/x/y/f.txt"), "2");
    CHECK(wait_for(co, &l, GP_PATH_MODIFIED, P("r/x/y/f.txt")), "recursive: a file two levels down");
    mkdir_(P("r/new"));
    CHECK(wait_for(co, &l, GP_PATH_CREATED, P("r/new")), "recursive: a directory made");
    gp_sleep(co, 0.05);
    write_file(P("r/new/g.txt"), "3");
    CHECK(wait_for(co, &l, GP_PATH_CREATED, P("r/new/g.txt")), "recursive: and a file made in it later");
    /* a directory with a file in it, moved in from outside */
    mkdir_(P("outside"));
    write_file(P("outside/h.txt"), "4");
    rename(P("outside"), P("r/moved"));
    CHECK(wait_for(co, &l, GP_PATH_CREATED, P("r/moved")), "recursive: a directory moved in is CREATED");
    CHECK(wait_for(co, &l, GP_PATH_CREATED, P("r/moved/h.txt")), "recursive: and what is in it, CREATED too");
    gp_sleep(co, 0.05);
    append_file(P("r/moved/h.txt"), "5");
    CHECK(wait_for(co, &l, GP_PATH_MODIFIED, P("r/moved/h.txt")), "recursive: and what was in it is watched");
    /* a subdirectory renamed: what is in it is reported under its new name */
    rename(P("r/x"), P("r/x2"));
    CHECK(wait_for(co, &l, GP_PATH_RENAMED, P("r/x2")), "recursive: a subdirectory renamed");
    gp_sleep(co, 0.05);
    append_file(P("r/x2/y/f.txt"), "6");
    CHECK(wait_for(co, &l, GP_PATH_MODIFIED, P("r/x2/y/f.txt")), "recursive: a file in it is reported under the new name");
    if(failures) {
        dump("recursive", &l);
    }
    gp_cancel(app, t);
}

static void test_file(gp_co* co, gp_app* app)
{
    mkdir_(P("f"));
    write_file(P("f/watched.txt"), "1");
    static Log l;
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    mkdir_(P("f/sub"));
    gp_task t = gp_watch_path(&cx, P("f/watched.txt"), GP_WATCH_RECURSIVE, on_event, &l); /* (recursive means nothing for a file) */
    CHECK(gp_task_active(app, t), "file: a file can be watched");
    write_file(P("f/other.txt"), "x");
    write_file(P("f/sub/watched.txt"), "x");
    append_file(P("f/watched.txt"), "2");
    CHECK(wait_for(co, &l, GP_PATH_MODIFIED, P("f/watched.txt")), "file: written to is MODIFIED");
    settle(co);
    CHECK(count_about(&l, P("f/other.txt")) == 0 && count_about(&l, P("f/sub/watched.txt")) == 0, "file: nothing about another file beside it, nor one of its name below");
    /* saved the way editors do: a new file renamed over it */
    write_file(P("f/watched.txt.tmp"), "new contents");
    remove(P("f/watched.txt")); /* (rename does not replace on Windows) */
    rename(P("f/watched.txt.tmp"), P("f/watched.txt"));
    CHECK(wait_for(co, &l, GP_PATH_CREATED, P("f/watched.txt")), "file: replaced by a rename: CREATED");
    CHECK(count_about(&l, P("f/watched.txt.tmp")) == 0, "file: nothing about the temporary file");
    remove(P("f/watched.txt"));
    CHECK(wait_for(co, &l, GP_PATH_DELETED, P("f/watched.txt")), "file: deleted");
    l.n = 0;
    write_file(P("f/watched.txt"), "again");
    CHECK(wait_for(co, &l, GP_PATH_CREATED, P("f/watched.txt")) && gp_task_active(app, t), "file: the watch goes on: made again is CREATED");
    if(failures) {
        dump("file", &l);
    }
    gp_cancel(app, t);

    static Log later;
    t = gp_watch_path(&cx, P("f/not-yet.txt"), 0, on_event, &later);
    CHECK(gp_task_active(app, t), "file: a name that is not there yet can be watched (its directory is)");
    write_file(P("f/not-yet.txt"), "now");
    CHECK(wait_for(co, &later, GP_PATH_CREATED, P("f/not-yet.txt")), "file: and its making is seen");
    gp_cancel(app, t);

    gp_task bad = gp_watch_path(&cx, P("no/such/dir/file.txt"), 0, on_event, &later);
    CHECK(!gp_task_active(app, bad) && bad.id == 0, "a path whose directory does not exist: a null task");
    bad = gp_watch_path(&cx, "", 0, on_event, &later);
    CHECK(bad.id == 0, "an empty path: a null task");
}

static void test_two_watches(gp_co* co, gp_app* app)
{
    mkdir_(P("two"));
    static Log a, b;
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_task ta = gp_watch_path(&cx, P("two"), 0, on_event, &a);
    gp_task tb = gp_watch_path(&cx, P("two"), 0, on_event, &b);
    write_file(P("two/1.txt"), "x");
    CHECK(wait_for(co, &a, GP_PATH_CREATED, P("two/1.txt")) && wait_for(co, &b, GP_PATH_CREATED, P("two/1.txt")), "two watches of one directory both see it");
    gp_cancel(app, ta);
    settle(co);
    write_file(P("two/2.txt"), "x");
    CHECK(wait_for(co, &b, GP_PATH_CREATED, P("two/2.txt")), "one cancelled, the other goes on");
    settle(co);
    CHECK(find(&a, GP_PATH_CREATED, P("two/2.txt")) < 0, "and the cancelled one hears nothing");
    gp_cancel(app, tb);
}

typedef struct
{
    int unused;
} Owner;
static void test_entity(gp_co* co, gp_app* app)
{
    mkdir_(P("ent"));
    gp_entity e = gp_entity_new(app, sizeof(Owner), &(Owner){ 0 }, NULL);
    gp_ctx cx = gp_ctx_for(app, e);
    static Log l;
    gp_task t = gp_watch_path(&cx, P("ent"), 0, on_event, &l);
    write_file(P("ent/1.txt"), "x");
    CHECK(wait_for(co, &l, GP_PATH_CREATED, P("ent/1.txt")), "entity: its watch works");
    gp_entity_release(app, e);
    settle(co);
    int before = l.calls;
    write_file(P("ent/2.txt"), "x");
    settle(co);
    CHECK(l.calls == before && !gp_task_active(app, t), "entity: released, its watch ends");
}

static void test_gone(gp_co* co, gp_app* app)
{
#ifndef _WIN32
    mkdir_(P("gone"));
    static Log l;
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_task t = gp_watch_path(&cx, P("gone"), 0, on_event, &l);
    rmdir(P("gone"));
    CHECK(wait_for(co, &l, GP_PATH_GONE, P("gone")), "gone: the watched directory deleted");
    settle(co);
    CHECK(!gp_task_active(app, t), "gone: and the watch has ended");
#else
    (void)co;
    (void)app;
#endif
}

/* a callback that cancels its own watch: what was already on its way is not delivered */
static gp_task self_cancel_task;
static int self_cancel_calls;
static void cancel_self(void* self, gp_ctx* cx, const gp_path_event* e, void* data)
{
    self_cancel_calls++;
    gp_cancel(cx->app, self_cancel_task);
}
static void test_cancel_inside(gp_co* co, gp_app* app)
{
    mkdir_(P("cs"));
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    self_cancel_task = gp_watch_path(&cx, P("cs"), 0, cancel_self, NULL);
    for(int i = 0; i < 5; i++) {
        char name[32];
        snprintf(name, sizeof name, "cs/%d.txt", i);
        write_file(P(name), "x");
    }
    settle(co);
    CHECK(self_cancel_calls == 1, "a watch that cancels itself in its callback gets nothing after (%d calls)", self_cancel_calls);
}

/* the loop wakes for an event when nothing else would wake it: the file is written from a worker while the test sleeps */
static double written_at, heard_at;
static void* write_later(void* data)
{ /* (a thread of its own: a worker's completion would wake the loop itself) */
    usleep(200 * 1000);
    written_at = gp_time();
    write_file(P("wake/now.txt"), "x");
    return NULL;
}
static void heard(void* self, gp_ctx* cx, const gp_path_event* e, void* data)
{
    if(!heard_at) {
        heard_at = gp_time();
    }
}
static void test_wakes(gp_co* co, gp_app* app)
{
    mkdir_(P("wake"));
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_task t = gp_watch_path(&cx, P("wake"), 0, heard, NULL);
    gp_thread th;
    bool started = gp_thread_create(&th, write_later, NULL);
    gp_sleep(co, 2.0);
    if(started) {
        gp_thread_join(th);
    }
    CHECK(heard_at > 0 && heard_at - written_at < 0.5, "the event wakes the loop (heard %.3f s after the write)", heard_at - written_at);
    gp_cancel(app, t);
}

static void test_released(gp_co* co, gp_app* app)
{
    settle(co); /* (cancelled watches are dropped at the end of a loop turn) */
    int watches, dirs;
    gp_fswatch_debug_counts(app, &watches, &dirs);
    CHECK(watches == 0 && dirs == 0, "every watch cancelled: nothing is held, by us or the system (%d watches, %d directories)", watches, dirs);
}

/* ------------------------------------------------------------ driver */

static void driver(gp_co* co, void* self, gp_ctx* cx, void* data)
{
    gp_app* app = cx->app;
    test_directory(co, app);
    test_recursive(co, app);
    test_file(co, app);
    test_two_watches(co, app);
    test_entity(co, app);
    test_gone(co, app);
    test_cancel_inside(co, app);
    test_wakes(co, app);
    test_released(co, app);
    /* quit with watches live: shutdown must stop the thread */
    static Log l;
    gp_ctx c = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_watch_path(&c, root, GP_WATCH_RECURSIVE, on_event, &l);
    gp_watch_path(&c, P("two"), 0, on_event, &l);
    gp_sleep(co, 0.05);
    gp_quit(app);
}

static gp_node* view(void* self, gp_ctx* cx)
{
    return gp_div(gp_size_full());
}

static void ready(gp_app* app, void* userdata)
{
    gp_entity v = gp_view_new(app, 0, NULL, view, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "fswatch_test", .width = 200, .height = 100 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void)
{
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
#ifdef _WIN32
    const char* tmp = getenv("TEMP") ? getenv("TEMP") : ".";
    snprintf(root, sizeof root, "%s/gpuic-fswatch-%d", tmp, (int)_getpid());
    if(_mkdir(root) != 0) {
        printf("cannot make %s\n", root);
        return 2;
    }
#else
    snprintf(root, sizeof root, "/tmp/gpuic-fswatch-XXXXXX");
    if(!mkdtemp(root)) {
        perror("mkdtemp");
        return 2;
    }
#endif
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    char cmd[700];
#ifdef _WIN32
    snprintf(cmd, sizeof cmd, "rmdir /s /q \"%s\"", root);
#else
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
#endif
    if(system(cmd) != 0) {
        printf("(could not remove %s)\n", root);
    }
    printf(failures ? "fswatch_test: %d FAILURE(S)\n" : "fswatch_test: all checks passed\n", failures);
    return failures != 0;
}
