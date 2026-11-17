/*
 * An app whose loop the caller runs (gp_app_new, gp_app_step, gp_app_free), as a plugin's host would, and windows inside other windows
 * (gp_window_options.parent). Two apps live at once and share SDL: an event for one's window goes to it whichever app's step takes it, and
 * freeing one leaves the other running. The embedded window is checked to be a child of its parent (X11: XQueryTree; Windows: GetParent) and
 * to follow gp_window_set_size. Runs on SDL's x11 driver on Linux (embedding is X11's). Needs a display.
 */
#include "../src/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif
# include <windows.h>
#else
# include <dlfcn.h>
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

typedef struct
{
    int clicks, ticks, drops;
} Counts;
static Counts ca, cb;

static void clicked(void* self, gp_ctx* cx, const gp_mouse_event* ev, void* data)
{
    ((Counts*)data)->clicks++;
}
static gp_node* view(void* self, gp_ctx* cx)
{
    Counts* c = *(Counts**)self;
    return gp_div(gp_size_full(), gp_bg(gp_rgb(0x203040)), gp_on_click_data(clicked, c));
}
static void tick(void* self, gp_ctx* cx, void* data)
{
    ((Counts*)data)->ticks++;
}
static void dropped(void* state)
{
    (*(Counts**)state)->drops++;
}

static unsigned long draws(gp_app* app)
{
    gp_stats s;
    gp_app_stats(app, &s);
    return s.draws;
}
/* steps both (b may be NULL) for about `ms` milliseconds, as a host's timer would */
static void run_for(gp_app* a, gp_app* b, int ms)
{
    for(int t = 0; t < ms; t += 5) {
        if(a) {
            gp_app_step(a, NULL);
        }
        if(b) {
            gp_app_step(b, NULL);
        }
        usleep(5000);
    }
}

static gp_window* open_view(gp_app* app, Counts* c, void* parent, int w, int h)
{
    gp_entity v = gp_view_new(app, sizeof c, &c, view, dropped);
    return gp_open_window(app, &(gp_window_options){ .title = "host_test", .width = w, .height = h, .parent = parent }, v);
}

#ifndef _WIN32
/* the parent of an X11 window, through the libX11 SDL has loaded */
static unsigned long x11_parent(gp_window* w)
{
    void* lib = dlopen("libX11.so.6", RTLD_NOW);
    int (*query)(void*, unsigned long, unsigned long*, unsigned long*, unsigned long**, unsigned*) = lib ? (int (*)(void*, unsigned long, unsigned long*, unsigned long*, unsigned long**, unsigned*))dlsym(lib, "XQueryTree") : NULL;
    int (*xfree)(void*) = lib ? (int (*)(void*))dlsym(lib, "XFree") : NULL;
    void* (*xopen)(const char*) = lib ? (void* (*)(const char*))dlsym(lib, "XOpenDisplay") : NULL;
    static void* display; /* (a connection of the test's own: any one can ask) */
    if(!display && xopen) {
        display = xopen(NULL);
    }
    unsigned long root = 0, parent = 0, *kids = NULL;
    unsigned n = 0;
    if(query && display && query(display, (unsigned long)(uintptr_t)gp_window_native(w), &root, &parent, &kids, &n) && kids) {
        xfree(kids);
    }
    return parent;
}
#endif

int main(void)
{
#ifndef _WIN32
    setenv("SDL_VIDEO_DRIVER", "x11", 1); /* (embedding is X11's: XWayland on a Wayland desktop) */
#endif
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    gp_app* a = gp_app_new();
    gp_app* b = gp_app_new();
    CHECK(a && b, "two apps start");
    if(!a || !b) {
        return 1;
    }
    int next = 0;
    CHECK(gp_app_step(a, &next) && next == -1, "an app with nothing to do steps and wants nothing (%d)", next);

    gp_window* wa = open_view(a, &ca, NULL, 240, 160);
    gp_window* wb = open_view(b, &cb, NULL, 240, 160);
    CHECK(wa && wb, "each opens a window");
    run_for(a, b, 300);
    CHECK(draws(a) >= 1 && draws(b) >= 1, "both draw when stepped (%lu, %lu)", draws(a), draws(b));

    gp_ctx cx = gp_ctx_for(a, GP_ENTITY_NULL);
    gp_after(&cx, 0.1, tick, &ca);
    gp_app_step(a, &next);
    CHECK(next > 0 && next <= 100, "a timer 100 ms away: next step in %d ms", next);
    run_for(a, NULL, 200);
    CHECK(ca.ticks == 1, "the timer fired through the steps (%d)", ca.ticks);

    /* a click on b's window, and only a stepping: it still reaches b */
    gp_simulate_mouse_button(b, GP_SIM_DOWN, 50, 50, GP_MOUSE_LEFT, 1, 0);
    gp_simulate_mouse_button(b, GP_SIM_UP, 50, 50, GP_MOUSE_LEFT, 1, 0);
    run_for(a, NULL, 100);
    CHECK(cb.clicks == 1 && ca.clicks == 0, "b's click went to b though a took it (a %d, b %d)", ca.clicks, cb.clicks);
    run_for(b, NULL, 50); /* (b's own turn: what the click queued) */

    /* a window inside b's: a child of it, and resized by the host */
    gp_window* we = open_view(b, &cb, gp_window_native(wb), 120, 80);
    CHECK(we != NULL, "a window opens inside another");
    if(we) {
        run_for(b, NULL, 200);
#ifdef _WIN32
        CHECK(GetParent((HWND)gp_window_native(we)) == (HWND)gp_window_native(wb), "the embedded window is a child of its parent");
#else
        CHECK(x11_parent(we) == (unsigned long)(uintptr_t)gp_window_native(wb), "the embedded window is a child of its parent (%lx, want %lx)", x11_parent(we), (unsigned long)(uintptr_t)gp_window_native(wb));
#endif
        CHECK((int)we->size.w == 120 && (int)we->size.h == 80, "it has the size asked for (%.0f x %.0f)", (double)we->size.w, (double)we->size.h);
        gp_window_set_size(we, 150, 90);
        run_for(b, NULL, 200);
        CHECK((int)we->size.w == 150 && (int)we->size.h == 90, "gp_window_set_size resized it (%.0f x %.0f)", (double)we->size.w, (double)we->size.h);
    }

    /* a quits; freeing it drops its entities, and b goes on */
    gp_quit(a);
    CHECK(!gp_app_step(a, &next), "a step after gp_quit says so");
    gp_app_free(a);
    CHECK(ca.drops == 1, "freeing the app dropped its view (%d)", ca.drops);
    unsigned long before = draws(b);
    gp_app_request_paint(b);
    run_for(b, NULL, 200);
    CHECK(draws(b) > before, "the other app still draws after one was freed (%lu -> %lu)", before, draws(b));
    int clicks = cb.clicks;
    gp_simulate_mouse_button(b, GP_SIM_DOWN, 50, 50, GP_MOUSE_LEFT, 1, 0);
    gp_simulate_mouse_button(b, GP_SIM_UP, 50, 50, GP_MOUSE_LEFT, 1, 0);
    run_for(b, NULL, 100);
    CHECK(cb.clicks == clicks + 1, "and still gets its events: SDL lives while an app does (%d clicks, want %d)", cb.clicks, clicks + 1);
    gp_app_free(b);
    CHECK(cb.drops == 2, "and the last one ends cleanly (%d views dropped)", cb.drops);

    printf(failures ? "host_test: %d FAILURE(S)\n" : "host_test: all checks passed\n", failures);
    return failures != 0;
}
