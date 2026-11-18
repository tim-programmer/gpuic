/*
 * File dialogs (gp_open_file_dialog / gp_save_file_dialog): answered by the test hook (a path, a cancel), delivered on the main thread
 * on a later loop turn, not called once cancelled or once the entity is gone, the closure dropped in every case; a pending task completed
 * from a raw thread wakes the loop; and the platform's real failure path (no dialog can be shown: on SDL an unknown dialog driver, on
 * Wayland no session bus) gives an error, not a cancel. The user's folders (XDG systems): from user-dirs.dirs under a made-up $HOME, the
 * default name when it is not there, none for saved games, the data folder made, and the same string again on a second call. Needs a display: a tiny window gives an event loop.
 */
#include "../src/internal.h"
#include "../src/threads.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef GP_XDG_FOLDERS
# include <sys/stat.h>
# include <unistd.h>
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

static _Thread_local bool is_main; /* (set on the main thread only) */

typedef struct
{
    int calls, count, drops;
    char path[256], error[256];
    bool on_main;
    void* self;
} Got;

static void answered(void* self, gp_ctx* cx, const char* const* paths, int count, const char* error, void* data)
{
    Got* g = data;
    g->calls++;
    g->count = count;
    g->self = self;
    snprintf(g->path, sizeof g->path, "%s", count ? paths[0] : "");
    snprintf(g->error, sizeof g->error, "%s", error ? error : "");
    g->on_main = is_main;
}
static void dropped(void* data)
{
    ((Got*)data)->drops++;
}

static const gp_file_filter kinds[] = {
    { "Effects",             "json" },
    {  "Meshes", "obj;fbx;gltf;glb" }
};

static void test_simulated(gp_co* co, gp_app* app)
{
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    static Got a, b;
    gp_simulate_file_dialog(app, "/tmp/effects/fire.json");
    gp_task t = gp_open_file_dialog(&cx, kinds, 2, "/tmp", false, answered, &a);
    gp_task_on_drop(app, t, dropped, &a);
    CHECK(a.calls == 0 && gp_task_active(app, t), "the answer comes on a later turn, not inside the call");
    gp_sleep(co, 0.05);
    CHECK(a.calls == 1 && a.count == 1 && strcmp(a.path, "/tmp/effects/fire.json") == 0 && !a.error[0] && a.on_main, "a chosen path, on the main thread (%d %d %s %s)", a.calls, a.count, a.path, a.error);
    CHECK(!gp_task_active(app, t) && a.drops == 1, "the task ends and its closure is dropped once (%d)", a.drops);

    gp_simulate_file_dialog(app, NULL);
    gp_save_file_dialog(&cx, kinds, 1, "/tmp/untitled.json", answered, &b);
    gp_sleep(co, 0.05);
    CHECK(b.calls == 1 && b.count == 0 && !b.error[0], "a cancel: no paths and no error (%d %d %s)", b.calls, b.count, b.error);
}

static void test_cancelled(gp_co* co, gp_app* app)
{
    static Got a, b;
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_simulate_file_dialog(app, "/tmp/x.json");
    gp_task t = gp_open_file_dialog(&cx, NULL, 0, NULL, true, answered, &a);
    gp_task_on_drop(app, t, dropped, &a);
    gp_cancel(app, t);
    gp_sleep(co, 0.05);
    CHECK(a.calls == 0 && a.drops == 1, "cancelled: not called, dropped (%d %d)", a.calls, a.drops);

    gp_entity e = gp_entity_new(app, sizeof(int), NULL, NULL);
    gp_ctx ecx = gp_ctx_for(app, e);
    gp_simulate_file_dialog(app, "/tmp/y.json");
    t = gp_open_file_dialog(&ecx, NULL, 0, NULL, false, answered, &b);
    gp_task_on_drop(app, t, dropped, &b);
    gp_entity_release(app, e);
    gp_sleep(co, 0.05);
    CHECK(b.calls == 0 && b.drops == 1, "entity released: not called, dropped (%d %d)", b.calls, b.drops);

    static Got c;
    gp_simulate_file_dialog(app, "/tmp/z.json");
    e = gp_entity_new(app, sizeof(int), NULL, NULL);
    void* state = gp_entity_get(app, e);
    ecx = gp_ctx_for(app, e);
    gp_open_file_dialog(&ecx, NULL, 0, NULL, false, answered, &c);
    gp_sleep(co, 0.05);
    CHECK(c.calls == 1 && c.self == state, "an entity's dialog gets its state as self");
    gp_entity_release(app, e);
}

static void* complete_later(void* arg)
{
    gp_app* app = ((void**)arg)[0];
    uint64_t id = *(uint64_t*)((void**)arg)[1];
    gp_exec_complete(app, id);
    return NULL;
}
static void pending_done(void* self, gp_ctx* cx, void* data)
{
    *(double*)data = gp_time();
}

static void test_other_thread(gp_co* co, gp_app* app)
{
    static double done_at;
    static uint64_t id;
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    id = gp_exec_pending(&cx, pending_done, &done_at, NULL);
    void* args[2] = { app, &id };
    gp_thread th;
    double t0 = gp_time();
    CHECK(gp_thread_create(&th, complete_later, args), "thread");
    gp_sleep(co, 0.3);
    gp_thread_join(th);
    CHECK(done_at > 0 && done_at - t0 < 0.2, "completed from another thread: done runs soon after (%.3f s)", done_at - t0);
}

static void test_platform_failure(gp_co* co, gp_app* app)
{
    static Got a;
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_open_file_dialog(&cx, kinds, 2, NULL, false, answered, &a); /* (main() made sure no dialog can be shown) */
    for(int i = 0; i < 300 && !a.calls; i++) {
        gp_sleep(co, 0.01);
    }
    CHECK(a.calls == 1 && a.count == 0 && a.error[0] && a.on_main, "no dialog can be shown: an error, not a cancel (%d '%s')", a.calls, a.error);
}

static char home[256];

static void test_folders(gp_app* app)
{
#ifdef GP_XDG_FOLDERS
    char want[512];
    const char* docs = gp_user_folder(app, GP_FOLDER_DOCUMENTS);
    snprintf(want, sizeof want, "%s/My \"Docs\"", home);
    CHECK(docs && strcmp(docs, want) == 0, "documents from user-dirs.dirs, $HOME and escapes (%s)", docs ? docs : "NULL");
    const char* music = gp_user_folder(app, GP_FOLDER_MUSIC);
    CHECK(music && strcmp(music, "/srv/music") == 0, "an absolute path, the last line winning (%s)", music ? music : "NULL");
    const char* pics = gp_user_folder(app, GP_FOLDER_PICTURES);
    snprintf(want, sizeof want, "%s/Pictures", home);
    CHECK(pics && strcmp(pics, want) == 0, "not in the file: $HOME/Pictures (%s)", pics ? pics : "NULL");
    const char* h = gp_user_folder(app, GP_FOLDER_HOME);
    CHECK(h && strcmp(h, home) == 0, "home");
    CHECK(!gp_user_folder(app, GP_FOLDER_SAVEDGAMES) && !gp_user_folder(app, GP_FOLDER_COUNT), "none for saved games, nor out of range");
    CHECK(gp_user_folder(app, GP_FOLDER_DOCUMENTS) == docs, "the same string again: it lives as long as the app");
    const char* data = gp_app_data_folder(app, "ppedit");
    snprintf(want, sizeof want, "%s/.local/share/ppedit", home);
    struct stat st;
    CHECK(data && strcmp(data, want) == 0 && stat(data, &st) == 0 && S_ISDIR(st.st_mode), "the data folder, made (%s)", data ? data : "NULL");
#else
    CHECK(gp_user_folder(app, GP_FOLDER_HOME) != NULL, "a home folder");
#endif
}

static void driver(gp_co* co, void* self, gp_ctx* cx, void* data)
{
    gp_app* app = cx->app;
    test_simulated(co, app);
    test_cancelled(co, app);
    test_other_thread(co, app);
    test_platform_failure(co, app);
    test_folders(app);
    gp_quit(app);
}

static gp_node* view(void* self, gp_ctx* cx)
{
    return gp_div(gp_size_full());
}

static void ready(gp_app* app, void* userdata)
{
    gp_entity v = gp_view_new(app, 0, NULL, view, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "dialog_test", .width = 200, .height = 100 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void)
{
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    setenv("SDL_FILE_DIALOG_DRIVER", "none", 1);                         /* SDL: no such driver */
    setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/bus", 1); /* Wayland: no portal to reach */
    is_main = true;
#ifdef GP_XDG_FOLDERS
    snprintf(home, sizeof home, "/tmp/gpuic-dialog-XXXXXX");
    if(!mkdtemp(home)) {
        return 2;
    }
    char path[512];
    snprintf(path, sizeof path, "%s/.config", home);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/.config/user-dirs.dirs", home);
    FILE* f = fopen(path, "w");
    fputs("# made by dialog_test\nXDG_DOCUMENTS_DIR=\"$HOME/My \\\"Docs\\\"\"\nXDG_MUSIC_DIR=\"relative/skipped\"\nXDG_MUSIC_DIR=\"/srv/music\"\n", f);
    fclose(f);
    setenv("HOME", home, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");
#endif
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
#ifdef GP_XDG_FOLDERS
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", home);
    if(system(cmd) != 0) {
        printf("(could not remove %s)\n", home);
    }
#endif
    printf(failures ? "dialog_test: %d FAILURE(S)\n" : "dialog_test: all checks passed\n", failures);
    return failures != 0;
}
