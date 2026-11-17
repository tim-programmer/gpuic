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
    int clicks, ticks, drops, keys;
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
/* the embedded window's view: focusable, and counting the keys it gets */
static gp_focus g_focus;
static void key_down(void* self, gp_ctx* cx, const gp_key_event* ev, void* data)
{
    if(strcmp(ev->key, "a") == 0) {
        ((Counts*)data)->keys++;
    }
}
static gp_node* key_view(void* self, gp_ctx* cx)
{
    Counts* c = *(Counts**)self;
    return gp_div(gp_size_full(), gp_bg(gp_rgb(0x304020)), gp_track_focus(g_focus), gp_on_key_down_data(key_down, c));
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
static void* x11(const char* name)
{
    static void* lib;
    lib = lib ? lib : dlopen("libX11.so.6", RTLD_NOW);
    return lib ? dlsym(lib, name) : NULL;
}
static void* x11_display(void)
{
    static void* display; /* (a connection of the test's own: any one can ask) */
    void* (*xopen)(const char*) = (void* (*)(const char*))x11("XOpenDisplay");
    if(!display && xopen) {
        display = xopen(NULL);
    }
    return display;
}
/* where a window is inside its parent */
static void x11_position(gp_window* w, int* x, int* y)
{
    int (*attrs)(void*, unsigned long, int*) = (int (*)(void*, unsigned long, int*))x11("XGetWindowAttributes");
    int a[64] = { 0 }; /* (an XWindowAttributes: x and y first) */
    if(attrs && x11_display()) {
        attrs(x11_display(), (unsigned long)(uintptr_t)gp_window_native(w), a);
    }
    *x = a[0];
    *y = a[1];
}
/* which window has the X keyboard focus */
static unsigned long x11_focus(void)
{
    int (*get)(void*, unsigned long*, int*) = (int (*)(void*, unsigned long*, int*))x11("XGetInputFocus");
    unsigned long focus = 0;
    int revert;
    if(get && x11_display()) {
        get(x11_display(), &focus, &revert);
    }
    return focus;
}
/* the X keyboard focus to `win` (as the host would hold it) */
static void x11_focus_to(unsigned long win)
{
    int (*focus)(void*, unsigned long, int, unsigned long) = (int (*)(void*, unsigned long, int, unsigned long))x11("XSetInputFocus");
    int (*sync)(void*, int) = (int (*)(void*, int))x11("XSync");
    if(focus && sync && x11_display()) {
        focus(x11_display(), win, 2, 0);
        sync(x11_display(), 0);
    }
}
/* a real key press and release of "a", through the X server (XTest), as a keyboard would make one; false without XTest */
static bool x11_type_a(void)
{
    void* tst = dlopen("libXtst.so.6", RTLD_NOW);
    int (*fake)(void*, unsigned, int, unsigned long) = tst ? (int (*)(void*, unsigned, int, unsigned long))dlsym(tst, "XTestFakeKeyEvent") : NULL;
    unsigned char (*keycode)(void*, unsigned long) = (unsigned char (*)(void*, unsigned long))x11("XKeysymToKeycode");
    int (*flush)(void*) = (int (*)(void*))x11("XFlush");
    void* d = x11_display();
    if(!fake || !keycode || !flush || !d) {
        return false;
    }
    unsigned code = keycode(d, 0x61 /* XK_a */);
    fake(d, code, 1, 0);
    fake(d, code, 0, 0);
    flush(d);
    return true;
}
/* Whether XTest's keys arrive on this display at all: typed into a plain window of the test's own (XWayland under a desktop that takes
 * emulated input through a portal drops them, Xvfb does not). */
static bool x11_keys_arrive(void)
{
    void* d = x11_display();
    unsigned long (*root)(void*) = (unsigned long (*)(void*))x11("XDefaultRootWindow");
    unsigned long (*create)(void*, unsigned long, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long) = (unsigned long (*)(void*, unsigned long, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long))x11("XCreateSimpleWindow");
    int (*select)(void*, unsigned long, long) = (int (*)(void*, unsigned long, long))x11("XSelectInput");
    int (*map)(void*, unsigned long) = (int (*)(void*, unsigned long))x11("XMapWindow");
    int (*pending)(void*) = (int (*)(void*))x11("XPending");
    int (*next)(void*, void*) = (int (*)(void*, void*))x11("XNextEvent");
    int (*focus)(void*, unsigned long, int, unsigned long) = (int (*)(void*, unsigned long, int, unsigned long))x11("XSetInputFocus");
    int (*destroy)(void*, unsigned long) = (int (*)(void*, unsigned long))x11("XDestroyWindow");
    if(!d || !root || !create || !select || !map || !pending || !next || !focus || !destroy) {
        return false;
    }
    unsigned long win = create(d, root(d), 0, 0, 40, 40, 0, 0, 0);
    select(d, win, (1L << 0) | (1L << 17)); /* KeyPressMask, StructureNotifyMask */
    map(d, win);
    long ev[24];                            /* (an XEvent: its type first) */
    bool mapped = false, typed = false, got = false;
    for(int i = 0; i < 300 && !got; i++) {
        while(pending(d)) {
            next(d, ev);
            mapped |= (int)ev[0] == 19; /* MapNotify */
            got |= (int)ev[0] == 2;     /* KeyPress */
        }
        if(mapped && !typed) {
            focus(d, win, 2, 0);
            typed = x11_type_a();
        }
        usleep(2000);
    }
    destroy(d, win);
    return got;
}
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
        int ex, ey;
#ifdef _WIN32
        RECT r;
        GetWindowRect((HWND)gp_window_native(we), &r);
        MapWindowPoints(HWND_DESKTOP, (HWND)gp_window_native(wb), (POINT*)&r, 2);
        ex = r.left;
        ey = r.top;
#else
        x11_position(we, &ex, &ey);
#endif
        CHECK(ex == 0 && ey == 0, "and sits at its parent's top-left (%d, %d)", ex, ey);
        gp_window_set_size(we, 150, 90);
        run_for(b, NULL, 200);
        CHECK((int)we->size.w == 150 && (int)we->size.h == 90, "gp_window_set_size resized it (%.0f x %.0f)", (double)we->size.w, (double)we->size.h);
#ifdef _WIN32
        GetWindowRect((HWND)gp_window_native(we), &r);
        MapWindowPoints(HWND_DESKTOP, (HWND)gp_window_native(wb), (POINT*)&r, 2);
        ex = r.left;
        ey = r.top;
#else
        x11_position(we, &ex, &ey);
#endif
        CHECK(ex == 0 && ey == 0, "and is still at its parent's top-left (%d, %d)", ex, ey);
    }

    /* the keyboard inside a host's window: a press there takes the focus from the host's top-level window, and keys come */
    static Counts ck;
    g_focus = gp_focus_new(b);
    Counts* pk = &ck;
    gp_entity kv = gp_view_new(b, sizeof pk, &pk, key_view, NULL);
    gp_window* wk = gp_open_window(b, &(gp_window_options){ .title = "host_test keys", .width = 100, .height = 60, .parent = gp_window_native(wb) }, kv);
    CHECK(wk != NULL, "a second embedded window opens");
    if(wk) {
        run_for(b, NULL, 200);
        gp_ctx ck_cx = gp_ctx_for_window(b, wk, GP_ENTITY_NULL);
        gp_focus_set(&ck_cx, g_focus);
#ifdef _WIN32 /* the host's window has the focus (opening the embedded one may have given it to it) */
        SetFocus((HWND)gp_window_native(wb));
#else
        x11_focus_to((unsigned long)(uintptr_t)gp_window_native(wb));
#endif
        gp_platform_inject_mouse(wk, GP_SIM_DOWN, 20, 20, GP_MOUSE_LEFT, 1, 0);
        gp_platform_inject_mouse(wk, GP_SIM_UP, 20, 20, GP_MOUSE_LEFT, 1, 0);
        run_for(b, NULL, 200);
#ifdef _WIN32
        CHECK(GetFocus() == (HWND)gp_window_native(wk), "a press in the embedded window gave it the keyboard focus");
#else
        CHECK(x11_focus() == (unsigned long)(uintptr_t)gp_window_native(wk), "a press in the embedded window gave it the keyboard focus (%lx, want %lx)", x11_focus(), (unsigned long)(uintptr_t)gp_window_native(wk));
        if(x11_keys_arrive()) {
            gp_platform_inject_mouse(wk, GP_SIM_DOWN, 20, 20, GP_MOUSE_LEFT, 1, 0); /* (the probe's window took the focus) */
            gp_platform_inject_mouse(wk, GP_SIM_UP, 20, 20, GP_MOUSE_LEFT, 1, 0);
            run_for(b, NULL, 200);
            x11_type_a();
            run_for(b, NULL, 300);
            if(ck.keys == 0 && !x11_keys_arrive()) { /* (this display dropped XTest's keys again: no verdict) */
                printf("(XTest's keys stopped arriving on this display: the real key into the embedded window not judged)\n");
            } else {
                CHECK(ck.keys == 1, "a key typed then reached it (%d)", ck.keys);
            }
        } else {
            printf("(XTest's keys do not arrive on this display: a real key into the embedded window not tried)\n");
        }
#endif
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
