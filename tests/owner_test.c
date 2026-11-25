/*
 * Windows that belong to another (gp_window_options.owner): one opens centered on its owner, or at a place from the owner's top left; a modal one keeps its
 * owner from taking clicks, keys and its close button until it closes, and then the owner takes them again; an owner that closes takes its windows with it.
 * Where windows are is asked of the system, which Wayland does not tell: those checks are skipped there. Needs a display.
 */
#include "../src/internal.h"

#include <stdio.h>
#include <stdlib.h>

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

static gp_app* app;
static gp_entity g_view;
static gp_co* co_;
static int g_clicks;
static void on_click(void* self, gp_ctx* cx, const gp_mouse_event* ev, void* data)
{
    g_clicks++;
}
static gp_node* render(void* self, gp_ctx* cx)
{
    return gp_div(gp_size_full(), gp_bg(gp_rgb(0x203040)), gp_on_click(on_click));
}

/* a coroutine of one window that closes another when it is cancelled (its window closing) */
static gp_window* g_close_on_cancel;
static void closer(gp_co* co, void* self, gp_ctx* cx, void* data)
{
    gp_sleep(co, 100);
    gp_window_close(g_close_on_cancel);
}

static void turns(int n)
{
    for(int i = 0; i < n; i++) {
        gp_sleep(co_, 0.02);
    }
}
static void click(gp_window* w)
{
    gp_window_on_mouse_move(w, 50, 50, 0);
    gp_window_on_mouse_button(w, 50, 50, GP_MOUSE_LEFT, true, 1, 0);
    gp_window_on_mouse_button(w, 50, 50, GP_MOUSE_LEFT, false, 1, 0);
}
static bool is_open(gp_window* w)
{
    for(int i = 0; i < app->nwindows; i++) {
        if(app->windows[i] == w) {
            return true;
        }
    }
    return false;
}

static void run(void)
{
    gp_entity view = g_view;
    gp_window* a = app->windows[0];
    turns(3);
    int ax, ay;
    bool told = gp_platform_position(a, &ax, &ay);

    /* centered on its owner, and at a place from its top left */
    gp_window* b = gp_open_window(app, &(gp_window_options){ .title = "owned", .width = 200, .height = 100, .owner = a }, view);
    gp_window* c = gp_open_window(app, &(gp_window_options){ .title = "placed", .width = 120, .height = 80, .owner = a, .positioned = true, .x = 30, .y = 40 }, view);
    CHECK(b && c && b->owner == a && c->owner == a, "two windows owned by the first open");
    turns(3);
    int bx, by, cx, cy;
    if(told && b && c && gp_platform_position(b, &bx, &by) && gp_platform_position(c, &cx, &cy)) {
        CHECK(abs(bx - (ax + (600 - 200) / 2)) <= 1 && abs(by - (ay + (400 - 100) / 2)) <= 1, "the owned window is centered on its owner (%d, %d; the owner at %d, %d)", bx, by, ax, ay);
        CHECK(cx == ax + 30 && cy == ay + 40, "the placed one is 30, 40 from the owner's top left (%d, %d; the owner at %d, %d)", cx, cy, ax, ay);
    } else {
        printf("(where windows are is not told here: not checked)\n");
    }
    if(b) {
        gp_window_close(b);
    }
    if(c) {
        gp_window_close(c);
    }
    turns(2);

    /* a modal one: the owner takes nothing until it closes */
    click(a);
    CHECK(g_clicks == 1, "a click on the owner, with nothing over it (%d)", g_clicks);
    gp_window* m = gp_open_window(app, &(gp_window_options){ .title = "modal", .width = 200, .height = 100, .owner = a, .modal = true }, view);
    CHECK(m && m->modal, "a modal window opens");
    turns(3);
    click(a);
    CHECK(g_clicks == 1, "a click on the owner under a modal window does nothing (%d)", g_clicks);
    CHECK(gp_window_on_key(a, "a", 0, false), "a key there is taken (and dropped)");
    gp_simulate_close_request(a);
    turns(2);
    CHECK(is_open(a) && !a->closed, "its close button does not close it");
    click(m);
    CHECK(g_clicks == 2, "the modal window itself takes clicks (%d)", g_clicks);
    gp_window_close(m);
    turns(2);
    CHECK(!is_open(m), "the modal window closed");
    click(a);
    CHECK(g_clicks == 3, "then the owner takes clicks again (%d)", g_clicks);

    /* an owner that closes takes what it owns with it; a window of its own stays */
    gp_window* other = gp_open_window(app, &(gp_window_options){ .title = "other", .width = 200, .height = 100 }, view);
    gp_window* d = gp_open_window(app, &(gp_window_options){ .title = "owned", .width = 200, .height = 100, .owner = a }, view);
    gp_window* e = gp_open_window(app, &(gp_window_options){ .title = "owned by owned", .width = 100, .height = 100, .owner = d }, view);
    turns(2);
    gp_window_close(a);
    turns(3);
    CHECK(!is_open(a) && !is_open(d) && !is_open(e) && is_open(other) && app->nwindows == 1, "closing the owner closed its windows and theirs, and not the other (%d open)", app->nwindows);

    /* a window whose tasks, ending as it closes, close an owner: what that owns goes too (and outlives it in no turn) */
    gp_window* x = gp_open_window(app, &(gp_window_options){ .title = "x", .width = 100, .height = 100 }, view);
    gp_window* a2 = gp_open_window(app, &(gp_window_options){ .title = "a2", .width = 100, .height = 100 }, view);
    gp_window* b2 = gp_open_window(app, &(gp_window_options){ .title = "b2", .width = 100, .height = 100, .owner = a2 }, view);
    g_close_on_cancel = a2;
    gp_ctx xc = gp_ctx_for_window(app, x, GP_ENTITY_NULL);
    gp_spawn(&xc, closer, NULL);
    turns(2);
    gp_window_close(x);
    turns(3);
    CHECK(!is_open(x) && !is_open(a2) && !is_open(b2) && is_open(other), "closed by a task as its window went, the owner took its window with it (%d open)", app->nwindows);
    gp_window_close(other);
}

static void driver(gp_co* co, void* self, gp_ctx* cx, void* data)
{
    co_ = co;
    run();
    gp_quit(app);
}

static void ready(gp_app* a, void* userdata)
{
    app = a;
    g_view = gp_view_new(app, 1, &(char){ 0 }, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "owner_test", .width = 600, .height = 400, .positioned = true, .x = 100, .y = 80 }, g_view);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void)
{
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "owner_test: %d FAILURE(S)\n" : "owner_test: all checks passed\n", failures);
    return failures != 0;
}
