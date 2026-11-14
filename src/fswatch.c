/*
 * Watching files and directories (gp_watch_path): one thread per app sleeps until the system reports changes, turns them into gp_fsevents
 * for the jobs that asked (exec.c), queues them and wakes the event loop. The main thread takes the queue and runs the callbacks.
 *
 * Linux: one inotify descriptor, a watch descriptor per directory (every directory below a recursive watch; new ones added as they appear,
 * with CREATED for what is already in them by then). Several jobs may watch one directory: the kernel gives them one descriptor, shared by
 * use records. A rename is the kernel's MOVED_FROM / MOVED_TO pair with one cookie; half a pair is a DELETED or a CREATED.
 *
 * Windows: a directory handle per watch read with ReadDirectoryChangesW (recursive natively), completions through one I/O completion
 * port. A cancelled watch's handle is closed by the thread when the aborted read comes back, so nothing is freed under a pending read.
 *
 * A file is watched through its directory, keeping only its own name: so it can be replaced (renamed over), deleted and made again.
 */
#if !defined(_WIN32)
#define _GNU_SOURCE
#endif
#include "internal.h"

#include "threads.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define VEC_PUSH(vec, n, cap, item)                                                     \
    do {                                                                                \
        if ((n) == (cap)) {                                                             \
            (cap) = (cap) ? (cap) * 2 : 16;                                             \
            (vec) = realloc((vec), (cap) * sizeof *(vec));                              \
            if (!(vec)) abort();                                                        \
        }                                                                               \
        (vec)[(n)++] = (item);                                                          \
    } while (0)

static char *dup_str(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *d = malloc(n + 1);
    if (!d) abort();
    memcpy(d, s, n + 1);
    return d;
}

static char *join(const char *dir, const char *name) {
    size_t a = strlen(dir), b = strlen(name);
    char *p = malloc(a + b + 2);
    if (!p) abort();
    memcpy(p, dir, a);
    size_t o = a;
    if (!(a && (dir[a - 1] == '/' || dir[a - 1] == '\\'))) p[o++] = '/';
    memcpy(p + o, name, b + 1);
    return p;
}

/* the path without trailing separators (but "/" and "C:\" stay) */
static char *trim(const char *path) {
    char *p = dup_str(path);
    size_t n = strlen(p);
    while (n > 1 && (p[n - 1] == '/' || p[n - 1] == '\\') && !(n == 3 && p[1] == ':')) p[--n] = 0;
    return p;
}

/* splits a path into its directory and last name (malloc'd both) */
static void split(const char *path, char **dir, char **name) {
    const char *slash = NULL;
    for (const char *q = path; *q; q++)
        if (*q == '/' || *q == '\\') slash = q;
    if (!slash) { *dir = dup_str("."); *name = dup_str(path); return; }
    size_t dn = (size_t)(slash - path);
    if (dn == 0 || (dn == 2 && path[1] == ':')) dn++;   /* "/x" -> "/", "C:\x" -> "C:\" */
    *dir = malloc(dn + 1);
    if (!*dir) abort();
    memcpy(*dir, path, dn);
    (*dir)[dn] = 0;
    *name = dup_str(slash + 1);
}

typedef struct gp_fswatch gp_fswatch;
static void queue(gp_fswatch *fw, uint64_t job, int kind, const char *path, const char *old_path);

/* ============================================================================================ Linux */
#if !defined(_WIN32)

#include <dirent.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

#define MASK (IN_CREATE | IN_DELETE | IN_MODIFY | IN_ATTRIB | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR | IN_EXCL_UNLINK)

typedef struct { uint64_t job; char *root, *file; bool recursive, gone; } pwatch;   /* one per job */
typedef struct { int wd; uint64_t job; char *dir; } wduse;                         /* one per (job, directory) */
typedef struct { uint32_t cookie; uint64_t job; char *path; bool dir; } moving;

struct gp_fswatch {
    gp_app *app;
    gp_mutex lock;   /* guards everything below */
    gp_thread thread;
    bool started;
    int ifd, ctl;
    pwatch *w;
    size_t nw, capw;
    wduse *u;
    size_t nu, capu;
    gp_fsevent *ev;
    size_t nev, capev;
};

static void queue(gp_fswatch *fw, uint64_t job, int kind, const char *path, const char *old_path) {
    VEC_PUSH(fw->ev, fw->nev, fw->capev, ((gp_fsevent){ job, kind, dup_str(path), dup_str(old_path) }));
}

static pwatch *find_watch(gp_fswatch *fw, uint64_t job) {
    for (size_t i = 0; i < fw->nw; i++)
        if (fw->w[i].job == job) return &fw->w[i];
    return NULL;
}

static bool wd_used(gp_fswatch *fw, int wd) {
    for (size_t i = 0; i < fw->nu; i++)
        if (fw->u[i].wd == wd) return true;
    return false;
}

static void drop_use(gp_fswatch *fw, size_t i) {
    int wd = fw->u[i].wd;
    free(fw->u[i].dir);
    fw->u[i] = fw->u[--fw->nu];
    if (!wd_used(fw, wd)) inotify_rm_watch(fw->ifd, wd);
}

static bool add_dir(gp_fswatch *fw, uint64_t job, const char *dir) {
    int wd = inotify_add_watch(fw->ifd, dir, MASK);
    if (wd < 0) return false;
    for (size_t i = 0; i < fw->nu; i++)
        if (fw->u[i].wd == wd && fw->u[i].job == job) return true;   /* (already: a directory reached twice) */
    VEC_PUSH(fw->u, fw->nu, fw->capu, ((wduse){ wd, job, dup_str(dir) }));
    return true;
}

/* the directories below `dir` (it is watched already); with `report`, CREATED for each entry found (they came before the watch did) */
static void add_tree(gp_fswatch *fw, uint64_t job, const char *dir, bool report, int depth) {
    if (depth > 64) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char *full = join(dir, e->d_name);
        struct stat st;
        bool is_dir = lstat(full, &st) == 0 && S_ISDIR(st.st_mode);   /* (links are not followed) */
        if (report) queue(fw, job, GP_PATH_CREATED, full, NULL);
        if (is_dir && add_dir(fw, job, full)) add_tree(fw, job, full, report, depth + 1);
        free(full);
    }
    closedir(d);
}

/* a directory under a recursive watch moved within it: the records below it follow; out of it: they go */
static void subtree_moved(gp_fswatch *fw, uint64_t job, const char *from, const char *to) {
    size_t fl = strlen(from);
    for (size_t i = 0; i < fw->nu;) {
        wduse *u = &fw->u[i];
        if (u->job == job && strncmp(u->dir, from, fl) == 0 && (u->dir[fl] == 0 || u->dir[fl] == '/')) {
            if (to) {
                char *nd = malloc(strlen(to) + strlen(u->dir + fl) + 1);
                if (!nd) abort();
                strcpy(nd, to);
                strcat(nd, u->dir + fl);
                free(u->dir);
                u->dir = nd;
            } else {
                drop_use(fw, i);
                continue;
            }
        }
        i++;
    }
}

static void handle(gp_fswatch *fw, const struct inotify_event *e, moving **mv, size_t *nmv, size_t *capmv) {
    if (e->mask & IN_Q_OVERFLOW) {
        for (size_t i = 0; i < fw->nw; i++)
            if (!fw->w[i].gone) {
                char *p = fw->w[i].file ? join(fw->w[i].root, fw->w[i].file) : dup_str(fw->w[i].root);
                queue(fw, fw->w[i].job, GP_PATH_OVERFLOW, p, NULL);
                free(p);
            }
        return;
    }
    /* the (job, directory) records of this descriptor, copied: handling adds and removes records */
    typedef struct { uint64_t job; char *dir; } hit;
    hit *hits = NULL;
    size_t nh = 0, caph = 0;
    for (size_t i = 0; i < fw->nu; i++)
        if (fw->u[i].wd == e->wd) VEC_PUSH(hits, nh, caph, ((hit){ fw->u[i].job, dup_str(fw->u[i].dir) }));
    for (size_t h = 0; h < nh; h++) {
        uint64_t job = hits[h].job;
        const char *dir = hits[h].dir;
        pwatch *w = find_watch(fw, job);
        if (!w || w->gone) continue;
        bool is_root = strcmp(dir, w->root) == 0;
        if (e->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED)) {
            /* the directory went (IGNORED alone: unmounted, say); below the root, its parent reports it */
            if (is_root) {
                w->gone = true;
                char *p = w->file ? join(w->root, w->file) : dup_str(w->root);
                queue(fw, job, GP_PATH_GONE, p, NULL);
                free(p);
            }
            continue;
        }
        if (!e->len || (w->file && strcmp(e->name, w->file) != 0)) continue;
        char *full = join(dir, e->name);
        bool isdir = (e->mask & IN_ISDIR) != 0;
        if (e->mask & IN_CREATE) {
            queue(fw, job, GP_PATH_CREATED, full, NULL);
            if (isdir && w->recursive && add_dir(fw, job, full)) add_tree(fw, job, full, true, 0);
        }
        if ((e->mask & (IN_MODIFY | IN_ATTRIB)) && !isdir) queue(fw, job, GP_PATH_MODIFIED, full, NULL);
        if (e->mask & IN_DELETE) queue(fw, job, GP_PATH_DELETED, full, NULL);
        if (e->mask & IN_MOVED_FROM) VEC_PUSH(*mv, *nmv, *capmv, ((moving){ e->cookie, job, dup_str(full), isdir }));
        if (e->mask & IN_MOVED_TO) {
            size_t k = 0;
            while (k < *nmv && !((*mv)[k].cookie == e->cookie && (*mv)[k].job == job)) k++;
            if (k < *nmv) {
                queue(fw, job, GP_PATH_RENAMED, full, (*mv)[k].path);
                if (isdir && w->recursive) subtree_moved(fw, job, (*mv)[k].path, full);
                free((*mv)[k].path);
                (*mv)[k] = (*mv)[--*nmv];
            } else {
                queue(fw, job, GP_PATH_CREATED, full, NULL);
                if (isdir && w->recursive && add_dir(fw, job, full)) add_tree(fw, job, full, true, 0);
            }
        }
        free(full);
    }
    for (size_t h = 0; h < nh; h++) free(hits[h].dir);
    free(hits);
    if (e->mask & IN_IGNORED) {   /* the kernel dropped the descriptor: so do its records */
        for (size_t i = 0; i < fw->nu;) {
            if (fw->u[i].wd == e->wd) { free(fw->u[i].dir); fw->u[i] = fw->u[--fw->nu]; }
            else i++;
        }
    }
}

static void *thread_main(void *arg) {
    gp_fswatch *fw = arg;
    char buf[64 * 1024] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        struct pollfd p[2] = { { fw->ifd, POLLIN, 0 }, { fw->ctl, POLLIN, 0 } };
        if (poll(p, 2, -1) < 0) continue;   /* EINTR */
        if (p[1].revents) break;            /* (only shutdown writes it) */
        moving *mv = NULL;
        size_t nmv = 0, capmv = 0;
        bool any = false;
        for (;;) {
            ssize_t n = read(fw->ifd, buf, sizeof buf);
            if (n <= 0) break;
            gp_mutex_lock(&fw->lock);
            size_t before = fw->nev;
            for (char *q = buf; q < buf + n;) {
                const struct inotify_event *e = (const struct inotify_event *)q;
                handle(fw, e, &mv, &nmv, &capmv);
                q += sizeof *e + e->len;
            }
            any |= fw->nev > before;
            gp_mutex_unlock(&fw->lock);
            /* a rename's two halves come together: if the second may still be coming, look once more */
            if (nmv) {
                struct pollfd again = { fw->ifd, POLLIN, 0 };
                if (poll(&again, 1, 5) > 0) continue;
            }
            break;
        }
        if (nmv) {   /* half a rename: moved out of what is watched */
            gp_mutex_lock(&fw->lock);
            for (size_t k = 0; k < nmv; k++) {
                pwatch *w = find_watch(fw, mv[k].job);
                if (w && !w->gone) {
                    queue(fw, mv[k].job, GP_PATH_DELETED, mv[k].path, NULL);
                    if (mv[k].dir && w->recursive) subtree_moved(fw, mv[k].job, mv[k].path, NULL);
                    any = true;
                }
                free(mv[k].path);
            }
            gp_mutex_unlock(&fw->lock);
        }
        free(mv);
        if (any) gp_platform_wake(fw->app);
    }
    return NULL;
}

static gp_fswatch *get(gp_app *app, bool make) {
    if (app->fswatch || !make) return app->fswatch;
    gp_fswatch *fw = calloc(1, sizeof *fw);
    if (!fw) abort();
    fw->app = app;
    fw->ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    fw->ctl = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fw->ifd < 0 || fw->ctl < 0) {
        fprintf(stderr, "gpuic: cannot watch files: %s\n", strerror(errno));
        if (fw->ifd >= 0) close(fw->ifd);
        if (fw->ctl >= 0) close(fw->ctl);
        free(fw);
        return NULL;
    }
    gp_mutex_init(&fw->lock);
    if (!gp_thread_create(&fw->thread, thread_main, fw)) {
        close(fw->ifd);
        close(fw->ctl);
        gp_mutex_destroy(&fw->lock);
        free(fw);
        return NULL;
    }
    fw->started = true;
    app->fswatch = fw;
    return fw;
}

bool gp_fswatch_add(gp_app *app, uint64_t job, const char *path, int flags) {
    gp_fswatch *fw = get(app, true);
    if (!fw) return false;
    char *p = trim(path);
    struct stat st;
    pwatch w = { job, NULL, NULL, (flags & GP_WATCH_RECURSIVE) != 0, false };
    if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
        w.root = p;
    } else {   /* a file, or a name that is not there yet: its directory must be */
        char *dir, *name;
        split(p, &dir, &name);
        free(p);
        if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode) || !*name) { free(dir); free(name); return false; }
        w.root = dir;
        w.file = name;
        w.recursive = false;
    }
    gp_mutex_lock(&fw->lock);
    bool ok = add_dir(fw, job, w.root);
    if (ok) {
        VEC_PUSH(fw->w, fw->nw, fw->capw, w);
        if (w.recursive) add_tree(fw, job, w.root, false, 0);
    }
    gp_mutex_unlock(&fw->lock);
    if (!ok) { free(w.root); free(w.file); }
    return ok;
}

void gp_fswatch_remove(gp_app *app, uint64_t job) {
    gp_fswatch *fw = get(app, false);
    if (!fw) return;
    gp_mutex_lock(&fw->lock);
    for (size_t i = 0; i < fw->nu;)
        if (fw->u[i].job == job) drop_use(fw, i);
        else i++;
    for (size_t i = 0; i < fw->nw; i++)
        if (fw->w[i].job == job) {
            free(fw->w[i].root);
            free(fw->w[i].file);
            fw->w[i] = fw->w[--fw->nw];
            break;
        }
    gp_mutex_unlock(&fw->lock);
}

void gp_fswatch_shutdown(gp_app *app) {
    gp_fswatch *fw = get(app, false);
    if (!fw) return;
    uint64_t one = 1;
    ssize_t r = write(fw->ctl, &one, sizeof one);
    (void)r;
    gp_thread_join(fw->thread);
    close(fw->ifd);
    close(fw->ctl);
    for (size_t i = 0; i < fw->nw; i++) { free(fw->w[i].root); free(fw->w[i].file); }
    for (size_t i = 0; i < fw->nu; i++) free(fw->u[i].dir);
    for (size_t i = 0; i < fw->nev; i++) { free(fw->ev[i].path); free(fw->ev[i].old_path); }
    free(fw->w);
    free(fw->u);
    free(fw->ev);
    gp_mutex_destroy(&fw->lock);
    free(fw);
    app->fswatch = NULL;
}

/* ============================================================================================ Windows */
#else

typedef struct rec {
    uint64_t job;
    HANDLE h;
    OVERLAPPED ov;
    char *root;            /* as given (UTF-8, trimmed) */
    wchar_t *wfile;        /* a file watch: its name; else NULL */
    char *file;
    bool recursive;
    bool removed;          /* cancelled: the thread frees it when the aborted read comes back */
    bool dead;             /* the handle failed and is closed: no read pending */
    DWORD buf[16384];      /* 64 KB, DWORD aligned as ReadDirectoryChangesW wants */
} rec;

struct gp_fswatch {
    gp_app *app;
    gp_mutex lock;
    gp_thread thread;
    HANDLE port;
    bool quitting;
    rec **r;
    size_t nr, capr;
    int live;              /* records with a read pending */
    gp_fsevent *ev;
    size_t nev, capev;
};

static void queue(gp_fswatch *fw, uint64_t job, int kind, const char *path, const char *old_path) {
    VEC_PUSH(fw->ev, fw->nev, fw->capev, ((gp_fsevent){ job, kind, dup_str(path), dup_str(old_path) }));
}

static wchar_t *to_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc((size_t)n * sizeof *w);
    if (!w) abort();
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
static char *from_wide(const wchar_t *w, int len) {
    if (len < 0) len = (int)wcslen(w);
    int n = WideCharToMultiByte(CP_UTF8, 0, w, len, NULL, 0, NULL, NULL);
    char *s = malloc((size_t)n + 1);
    if (!s) abort();
    WideCharToMultiByte(CP_UTF8, 0, w, len, s, n, NULL, NULL);
    s[n] = 0;
    for (char *q = s; *q; q++)
        if (*q == '\\') *q = '/';
    return s;
}

static bool issue(rec *r) {
    memset(&r->ov, 0, sizeof r->ov);
    DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
                   FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;
    return ReadDirectoryChangesW(r->h, r->buf, sizeof r->buf, r->recursive, filter, NULL, &r->ov, NULL) != 0;
}

static void free_rec(rec *r) {
    free(r->root);
    free(r->wfile);
    free(r->file);
    free(r);
}

static void unlist(gp_fswatch *fw, rec *r) {
    for (size_t i = 0; i < fw->nr; i++)
        if (fw->r[i] == r) { fw->r[i] = fw->r[--fw->nr]; return; }
}

static bool is_dir_path(const char *p) {
    wchar_t *w = to_wide(p);
    DWORD a = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
    free(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void gone(gp_fswatch *fw, rec *r) {
    char *p = r->file ? join(r->root, r->file) : dup_str(r->root);
    queue(fw, r->job, GP_PATH_GONE, p, NULL);
    free(p);
    CloseHandle(r->h);
    r->h = NULL;
    r->dead = true;
    fw->live--;
}

/* CREATED for what is in a directory that appeared under a recursive watch (moved in with its contents, or filled before it was seen): Windows
 * reports only the directory, Linux's side does this too */
static void report_tree(gp_fswatch *fw, rec *r, const char *dir, int depth) {
    if (depth > 64) return;
    char *pat = join(dir, "*");
    wchar_t *wp = to_wide(pat);
    free(pat);
    WIN32_FIND_DATAW fd;
    HANDLE h = wp ? FindFirstFileW(wp, &fd) : INVALID_HANDLE_VALUE;
    free(wp);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        char *name = from_wide(fd.cFileName, -1);
        char *full = join(dir, name);
        free(name);
        queue(fw, r->job, GP_PATH_CREATED, full, NULL);
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) report_tree(fw, r, full, depth + 1);
        free(full);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void parse(gp_fswatch *fw, rec *r) {
    char *old = NULL;   /* a rename's first half */
    const BYTE *q = (const BYTE *)r->buf;
    for (;;) {
        const FILE_NOTIFY_INFORMATION *fi = (const FILE_NOTIFY_INFORMATION *)q;
        int wlen = (int)(fi->FileNameLength / sizeof(WCHAR));
        bool mine = true;
        if (r->wfile) mine = (int)wcslen(r->wfile) == wlen && _wcsnicmp(fi->FileName, r->wfile, (size_t)wlen) == 0;
        if (mine) {
            char *name = from_wide(fi->FileName, wlen);
            char *full = join(r->root, name);
            free(name);
            switch (fi->Action) {
            case FILE_ACTION_ADDED:
                queue(fw, r->job, GP_PATH_CREATED, full, NULL);
                if (r->recursive && is_dir_path(full)) report_tree(fw, r, full, 0);
                break;
            case FILE_ACTION_REMOVED: queue(fw, r->job, GP_PATH_DELETED, full, NULL); break;
            case FILE_ACTION_MODIFIED:
                if (!is_dir_path(full)) queue(fw, r->job, GP_PATH_MODIFIED, full, NULL);   /* (a directory "changes" when its entries do) */
                break;
            case FILE_ACTION_RENAMED_OLD_NAME:
                if (r->wfile) queue(fw, r->job, GP_PATH_DELETED, full, NULL);
                else { if (old) { queue(fw, r->job, GP_PATH_DELETED, old, NULL); free(old); } old = dup_str(full); }
                break;
            case FILE_ACTION_RENAMED_NEW_NAME:
                if (old) { queue(fw, r->job, GP_PATH_RENAMED, full, old); free(old); old = NULL; }
                else {
                    queue(fw, r->job, GP_PATH_CREATED, full, NULL);
                    if (r->recursive && is_dir_path(full)) report_tree(fw, r, full, 0);
                }
                break;
            default: break;
            }
            free(full);
        }
        if (!fi->NextEntryOffset) break;
        q += fi->NextEntryOffset;
    }
    if (old) { queue(fw, r->job, GP_PATH_DELETED, old, NULL); free(old); }
}

static void *thread_main(void *arg) {
    gp_fswatch *fw = arg;
    for (;;) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED *pov = NULL;
        BOOL ok = GetQueuedCompletionStatus(fw->port, &bytes, &key, &pov, INFINITE);
        DWORD err = ok ? 0 : GetLastError();
        gp_mutex_lock(&fw->lock);
        if (!pov) {   /* the quit packet */
            fw->quitting = true;
            bool done = fw->live == 0;
            gp_mutex_unlock(&fw->lock);
            if (done) break;
            continue;
        }
        rec *r = (rec *)key;
        bool any = false;
        if (r->removed) {
            CloseHandle(r->h);
            fw->live--;
            free_rec(r);
        } else if (!ok) {
            if (err != ERROR_OPERATION_ABORTED) { gone(fw, r); any = true; }
        } else {
            size_t before = fw->nev;
            if (bytes == 0) {   /* the buffer overflowed: what changed is not known */
                char *p = r->file ? join(r->root, r->file) : dup_str(r->root);
                queue(fw, r->job, GP_PATH_OVERFLOW, p, NULL);
                free(p);
            } else {
                parse(fw, r);
            }
            any = fw->nev > before;
            if (!issue(r)) { gone(fw, r); any = true; }
        }
        bool done = fw->quitting && fw->live == 0;
        gp_mutex_unlock(&fw->lock);
        if (any) gp_platform_wake(fw->app);
        if (done) break;
    }
    return NULL;
}

static gp_fswatch *get(gp_app *app, bool make) {
    if (app->fswatch || !make) return app->fswatch;
    gp_fswatch *fw = calloc(1, sizeof *fw);
    if (!fw) abort();
    fw->app = app;
    fw->port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    if (!fw->port) { free(fw); return NULL; }
    gp_mutex_init(&fw->lock);
    if (!gp_thread_create(&fw->thread, thread_main, fw)) {
        CloseHandle(fw->port);
        free(fw);
        return NULL;
    }
    app->fswatch = fw;
    return fw;
}

bool gp_fswatch_add(gp_app *app, uint64_t job, const char *path, int flags) {
    gp_fswatch *fw = get(app, true);
    if (!fw) return false;
    rec *r = calloc(1, sizeof *r);
    if (!r) abort();
    r->job = job;
    r->recursive = (flags & GP_WATCH_RECURSIVE) != 0;
    char *p = trim(path);
    if (is_dir_path(p)) {
        r->root = p;
    } else {
        char *dir, *name;
        split(p, &dir, &name);
        free(p);
        if (!is_dir_path(dir) || !*name) { free(dir); free(name); free(r); return false; }
        r->root = dir;
        r->file = name;
        r->wfile = to_wide(name);
        r->recursive = false;
    }
    wchar_t *wroot = to_wide(r->root);
    r->h = wroot ? CreateFileW(wroot, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL)
                 : INVALID_HANDLE_VALUE;
    free(wroot);
    if (r->h == INVALID_HANDLE_VALUE || !CreateIoCompletionPort(r->h, fw->port, (ULONG_PTR)r, 0)) {
        if (r->h != INVALID_HANDLE_VALUE) CloseHandle(r->h);
        free_rec(r);
        return false;
    }
    gp_mutex_lock(&fw->lock);
    bool ok = issue(r);
    if (ok) {
        VEC_PUSH(fw->r, fw->nr, fw->capr, r);
        fw->live++;
    }
    gp_mutex_unlock(&fw->lock);
    if (!ok) { CloseHandle(r->h); free_rec(r); }
    return ok;
}

void gp_fswatch_remove(gp_app *app, uint64_t job) {
    gp_fswatch *fw = get(app, false);
    if (!fw) return;
    gp_mutex_lock(&fw->lock);
    for (size_t i = 0; i < fw->nr; i++) {
        rec *r = fw->r[i];
        if (r->job != job) continue;
        unlist(fw, r);
        if (r->dead) free_rec(r);
        else {   /* the read comes back aborted (or done, if it was finishing): the thread frees it then */
            r->removed = true;
            CancelIoEx(r->h, &r->ov);
        }
        break;
    }
    gp_mutex_unlock(&fw->lock);
}

void gp_fswatch_shutdown(gp_app *app) {
    gp_fswatch *fw = get(app, false);
    if (!fw) return;
    gp_mutex_lock(&fw->lock);
    while (fw->nr) {
        rec *r = fw->r[0];
        unlist(fw, r);
        if (r->dead) free_rec(r);
        else { r->removed = true; CancelIoEx(r->h, &r->ov); }
    }
    gp_mutex_unlock(&fw->lock);
    PostQueuedCompletionStatus(fw->port, 0, 0, NULL);
    gp_thread_join(fw->thread);
    CloseHandle(fw->port);
    for (size_t i = 0; i < fw->nev; i++) { free(fw->ev[i].path); free(fw->ev[i].old_path); }
    free(fw->ev);
    free(fw->r);
    gp_mutex_destroy(&fw->lock);
    free(fw);
    app->fswatch = NULL;
}

#endif

/* ============================================================================================ both */

/* For tests: the watches held (jobs) and the directories the system watches for them (Linux: watch descriptors in use; Windows: handles). */
void gp_fswatch_debug_counts(gp_app *app, int *watches, int *dirs) {
    gp_fswatch *fw = app->fswatch;
    *watches = *dirs = 0;
    if (!fw) return;
    gp_mutex_lock(&fw->lock);
#if !defined(_WIN32)
    *watches = (int)fw->nw;
    *dirs = (int)fw->nu;
#else
    *watches = (int)fw->nr;
    *dirs = fw->live;
#endif
    gp_mutex_unlock(&fw->lock);
}

size_t gp_fswatch_take(gp_app *app, gp_fsevent **out) {
    gp_fswatch *fw = app->fswatch;
    *out = NULL;
    if (!fw) return 0;
    gp_mutex_lock(&fw->lock);
    size_t n = fw->nev;
    *out = fw->ev;
    fw->ev = NULL;
    fw->nev = fw->capev = 0;
    gp_mutex_unlock(&fw->lock);
    return n;
}

bool gp_fswatch_pending(gp_app *app) {
    gp_fswatch *fw = app->fswatch;
    if (!fw) return false;
    gp_mutex_lock(&fw->lock);
    bool p = fw->nev > 0;
    gp_mutex_unlock(&fw->lock);
    return p;
}
