/*
 * File dialogs: the request is copied into a gp_dialog, a pending task waits for it, and the platform answers (from any thread) with
 * gp_dialog_answer, which completes the task; `done` then runs on the main thread like any background task's. A dialog still open when
 * the app ends is detached (its `app` cleared under the lock), and the answer that comes later frees it.
 */
#include "internal.h"
#include "threads.h"

static gp_mutex g_lock;
static bool g_lock_made;
static gp_dialog* g_open; /* answers not yet given, of every app */

static char* dup_or_null(const char* s)
{
    return s ? strdup(s) : NULL;
}

static void dialog_free(void* p)
{
    gp_dialog* d = p;
    for(int i = 0; i < d->count; i++) {
        free(d->paths[i]);
    }
    for(int i = 0; i < d->nfilters; i++) {
        free((char*)d->filters[i].name);
        free((char*)d->filters[i].extensions);
    }
    free(d->paths);
    free(d->error);
    free(d->start);
    free(d->filters);
    free(d->platform_data);
    free(d);
}

static void dialog_done(void* self, gp_ctx* cx, void* data)
{
    gp_dialog* d = data;
    d->fn(self, cx, (const char* const*)d->paths, d->count, d->filter, d->error, d->data);
}

void gp_dialog_answer(gp_dialog* d, const char* const* paths, int count, int filter, const char* error)
{
    gp_mutex_lock(&g_lock);
    if(!d->app) {
        gp_mutex_unlock(&g_lock);
        dialog_free(d);
        return;
    }
    for(gp_dialog** pp = &g_open; *pp; pp = &(*pp)->next) {
        if(*pp == d) {
            *pp = d->next;
            break;
        }
    }
    d->paths = count ? calloc((size_t)count, sizeof *d->paths) : NULL;
    for(int i = 0; i < count; i++) {
        d->paths[i] = strdup(paths[i]);
    }
    d->count = count;
    d->filter = filter >= 0 && filter < d->nfilters ? filter : -1;
    d->error = dup_or_null(error);
    gp_exec_complete(d->app, d->job);
    gp_mutex_unlock(&g_lock);
}

static gp_task show(gp_ctx* cx, bool save, const gp_file_filter* filters, int nfilters, const char* start, bool many, gp_file_dialog_fn done,
    void* data)
{
    gp_app* app = cx->app;
    if(!g_lock_made) { /* (the main thread's first dialog: nothing else can be taking it yet) */
        gp_mutex_init(&g_lock);
        g_lock_made = true;
    }
    gp_dialog* d = calloc(1, sizeof *d);
    d->app = app;
    d->fn = done;
    d->data = data;
    d->save = save;
    d->many = many;
    d->start = dup_or_null(start && *start ? start : NULL);
    d->filters = nfilters > 0 ? calloc((size_t)nfilters, sizeof *d->filters) : NULL;
    for(int i = 0; i < nfilters; i++) {
        d->filters[i] = (gp_file_filter){ strdup(filters[i].name ? filters[i].name : ""), strdup(filters[i].extensions ? filters[i].extensions : "*") };
    }
    d->nfilters = nfilters > 0 ? nfilters : 0;
    d->job = gp_exec_pending(cx, dialog_done, d, dialog_free);
    gp_task t = { d->job };
    gp_mutex_lock(&g_lock);
    d->next = g_open;
    g_open = d;
    gp_mutex_unlock(&g_lock);
    if(app->sim_dialog) {
        app->sim_dialog = false;
        const char* p = app->sim_dialog_path;
        gp_dialog_answer(d, &p, p ? 1 : 0, app->sim_dialog_filter, NULL);
        free(app->sim_dialog_path);
        app->sim_dialog_path = NULL;
        return t;
    }
    gp_window* w = cx->window ? cx->window : app->nwindows ? app->windows[0]
                                                           : NULL;
    gp_platform_file_dialog(w, d);
    return t;
}

gp_task gp_open_file_dialog(gp_ctx* cx, const gp_file_filter* filters, int nfilters, const char* start, bool many, gp_file_dialog_fn done, void* data)
{
    return show(cx, false, filters, nfilters, start, many, done, data);
}

gp_task gp_save_file_dialog(gp_ctx* cx, const gp_file_filter* filters, int nfilters, const char* start, gp_file_dialog_fn done, void* data)
{
    return show(cx, true, filters, nfilters, start, false, done, data);
}

void gp_simulate_file_dialog(gp_app* app, const char* path, int filter)
{
    free(app->sim_dialog_path);
    app->sim_dialog = true;
    app->sim_dialog_filter = filter;
    app->sim_dialog_path = dup_or_null(path);
}

void gp_dialogs_shutdown(gp_app* app)
{
    free(app->sim_dialog_path);
    app->sim_dialog_path = NULL;
    if(!g_lock_made) {
        return;
    }
    gp_mutex_lock(&g_lock);
    for(gp_dialog** pp = &g_open; *pp;) {
        gp_dialog* d = *pp;
        if(d->app == app) {
            d->app = NULL;
            *pp = d->next;
        } else {
            pp = &d->next;
        }
    }
    gp_mutex_unlock(&g_lock);
}
