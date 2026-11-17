/*
 * Platform backend on SDL3: windows, GL context (or Vulkan surfaces), input events, cursors.
 * Everything SDL-specific lives here, so a native Wayland/X11 backend only
 * has to reimplement the gp_platform_* functions from internal.h.
 */
#include "internal.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#ifdef _WIN32
# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif
# ifndef NOMINMAX
#  define NOMINMAX
# endif
# include <windows.h>
#elif defined(__linux__) || defined(__FreeBSD__)
# include <dlfcn.h>
# define GP_X11_EMBED 1
#endif

typedef struct
{
    SDL_GLContext gl;
    bool vulkan;         /* GPUIC_RENDERER=vulkan, and SDL found the Vulkan loader: windows are made for Vulkan, no GL context is */
    int nwindows;        /* open: the context goes before the last of them (see gp_platform_close_window) */
    SDL_Cursor* cursors[GP_CURSOR_COUNT];
    SDL_FingerID finger; /* the finger being followed (the first), 0 for none */
} sdl_platform;

typedef struct
{
    SDL_Window* window;
    bool embedded; /* inside a host's window: takes the keyboard focus when pressed (the host's top-level has it) */
#ifdef _WIN32
    HWND child;    /* an embedded window: the child of the host's that SDL wraps (ours to destroy) */
#endif
} sdl_window;

#define APP_PROP "gpuic.app"                                                                                                /* on each SDL window: the app that opened it, which its events go to */

static bool g_ignore_real_mouse;                                                                                            /* GPUIC_IGNORE_REAL_MOUSE, see real_mouse() */
static bool g_ignore_real_keys;                                                                                             /* GPUIC_IGNORE_REAL_KEYS, see real_key() */
static Uint32 g_wake_event;                                                                                                 /* custom event used to wake the loop from other threads */
static SDL_Cursor* g_cursor;                                                                                                /* the cursor shown: it is the process's, and every app has a set of its own */
static int g_sdl_users;
static bool g_sdl_ours; /* SDL was not running before gpuic started it: gpuic ends it (a host using SDL itself keeps it) */ /* apps alive (gp_app_new can make several): SDL starts with the first and ends with the last */

static sdl_platform* plat(gp_app* app)
{
    return app->platform;
}
static SDL_Window* sdl_win(gp_window* w)
{
    return ((sdl_window*)w->platform)->window;
}

bool gp_platform_init(gp_app* app)
{
    if(!g_sdl_users) {
        g_sdl_ours = SDL_WasInit(0) == 0;
        if(!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            fprintf(stderr, "gpuic: SDL_Init failed: %s\n", SDL_GetError());
            return false;
        }
    }
    g_sdl_users++;
    if(!g_wake_event) {
        g_wake_event = SDL_RegisterEvents(1);
    }
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0"); /* a finger is not a mouse: it scrolls (gp_scroll_touch) and a tap is made a click there */
    g_ignore_real_mouse = getenv("GPUIC_IGNORE_REAL_MOUSE") != NULL;
    g_ignore_real_keys = getenv("GPUIC_IGNORE_REAL_KEYS") != NULL;
    sdl_platform* p = calloc(1, sizeof *p);
    p->cursors[GP_CURSOR_DEFAULT] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    p->cursors[GP_CURSOR_POINTER] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
    p->cursors[GP_CURSOR_TEXT] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
    app->platform = p;
    const char* renderer = getenv("GPUIC_RENDERER");
    if(renderer && strcmp(renderer, "vulkan") == 0) {
        p->vulkan = SDL_Vulkan_LoadLibrary(NULL);
        if(!p->vulkan) {
            fprintf(stderr, "gpuic: GPUIC_RENDERER=vulkan, but the Vulkan loader was not found (%s): drawing with OpenGL\n", SDL_GetError());
        }
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    return true;
}

void gp_platform_shutdown(gp_app* app)
{
    sdl_platform* p = plat(app);
    if(!p) {
        return;
    }
    for(int i = 0; i < GP_CURSOR_COUNT; i++) {
        if(p->cursors[i]) {
            if(p->cursors[i] == g_cursor) {
                g_cursor = NULL;
            }
            SDL_DestroyCursor(p->cursors[i]);
        }
    }
    if(p->gl) {
        SDL_GL_DestroyContext(p->gl);
    }
    if(p->vulkan) {
        SDL_Vulkan_UnloadLibrary();
    }
    free(p);
    app->platform = NULL;
    if(--g_sdl_users == 0) {
        if(g_sdl_ours) {
            SDL_Quit();
        } else {
            SDL_QuitSubSystem(SDL_INIT_VIDEO); /* (only gpuic's part: a host using SDL itself keeps its own) */
        }
        g_wake_event = 0;
    }
}

#ifdef GP_X11_EMBED
/* The X11 calls embedding needs, from the libX11 SDL has loaded (so that gpuic does not link it itself). */
static void* x11_fn(const char* name)
{
    static void* lib;
    if(!lib) {
        lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_NOLOAD);
        lib = lib ? lib : dlopen("libX11.so.6", RTLD_NOW);
    }
    return lib ? dlsym(lib, name) : NULL;
}
static bool x11_window(SDL_Window* win, void** display, unsigned long* xwin)
{
    SDL_PropertiesID props = SDL_GetWindowProperties(win);
    *display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
    *xwin = (unsigned long)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    return *display && *xwin;
}
static bool x11_reparent(SDL_Window* win, unsigned long parent)
{
    int (*reparent)(void*, unsigned long, unsigned long, int, int) = (int (*)(void*, unsigned long, unsigned long, int, int))x11_fn("XReparentWindow");
    int (*flush)(void*) = (int (*)(void*))x11_fn("XFlush");
    void* display;
    unsigned long xwin;
    if(!reparent || !flush || !x11_window(win, &display, &xwin)) {
        return false;
    }
    reparent(display, xwin, parent, 0, 0);
    flush(display);
    return true;
}
/* The keyboard focus to an embedded window: the host's top-level window has it, and keys reach a child only under the pointer otherwise.
 * When the window goes, the focus goes back to its parent. */
/* An embedded window resized in place. SDL would move it too: it keeps a window's position in screen coordinates (from X's notifications,
 * translated to the root) and applies it again with every resize, which inside a parent puts the window that far off its corner. */
static void x11_resize(SDL_Window* win, int width, int height)
{
    int (*move_resize)(void*, unsigned long, int, int, unsigned, unsigned) = (int (*)(void*, unsigned long, int, int, unsigned, unsigned))x11_fn("XMoveResizeWindow");
    int (*flush)(void*) = (int (*)(void*))x11_fn("XFlush");
    void* display;
    unsigned long xwin;
    if(move_resize && flush && x11_window(win, &display, &xwin)) {
        /* (X refuses a size beyond what it can hold, and Xlib's default handler would end the host's process over it) */
        width = width < 1 ? 1 : width > 16384 ? 16384
                                              : width;
        height = height < 1 ? 1 : height > 16384 ? 16384
                                                 : height;
        move_resize(display, xwin, 0, 0, (unsigned)width, (unsigned)height);
        flush(display);
    }
}

static int x11_ignore_error(void* display, void* error)
{
    (void)display, (void)error;
    return 0;
}
static void x11_take_focus(SDL_Window* win)
{
    typedef int (*handler_fn)(void*, void*);
    int (*set_focus)(void*, unsigned long, int, unsigned long) = (int (*)(void*, unsigned long, int, unsigned long))x11_fn("XSetInputFocus");
    int (*sync)(void*, int) = (int (*)(void*, int))x11_fn("XSync");
    handler_fn (*set_handler)(handler_fn) = (handler_fn (*)(handler_fn))x11_fn("XSetErrorHandler");
    void* display;
    unsigned long xwin;
    if(set_focus && sync && set_handler && x11_window(win, &display, &xwin)) {
        /* a window that cannot take it (hidden with its host: BadMatch; gone: BadWindow) keeps what it has: Xlib's own handler would end the
         * host's process over it */
        sync(display, 0);
        handler_fn old = set_handler(x11_ignore_error);
        set_focus(display, xwin, 2 /* RevertToParent */, 0 /* CurrentTime */);
        sync(display, 0);
        set_handler(old);
    }
}
#endif

#ifdef _WIN32
/* An embedded window on Windows: a child of the host's window, made here and wrapped by SDL (which sets it up for GL and takes its messages). */
static HWND win32_child(HWND parent, int width, int height)
{
    static ATOM cls;
    if(!cls) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.style = CS_OWNDC; /* (GL keeps its device context) */
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = L"gpuic_embedded";
        cls = RegisterClassExW(&wc);
    }
    return CreateWindowExW(0, L"gpuic_embedded", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, width, height, parent, NULL, GetModuleHandleW(NULL), NULL);
}
#endif

void gp_platform_update_size(gp_window* w)
{
    int lw, lh, pw, ph;
    SDL_GetWindowSize(sdl_win(w), &lw, &lh);
    SDL_GetWindowSizeInPixels(sdl_win(w), &pw, &ph);
    w->size = (gp_size){ (float)lw, (float)lh };
    w->px_w = pw;
    w->px_h = ph;
    w->scale = lw > 0 ? (float)pw / (float)lw : 1.f;
}

bool gp_platform_open_window(gp_app* app, gp_window* w, const gp_window_options* opts)
{
    sdl_platform* p = plat(app);
    int width = opts->width > 0 ? opts->width : 960, height = opts->height > 0 ? opts->height : 640;
    SDL_WindowFlags api = p->vulkan ? SDL_WINDOW_VULKAN : SDL_WINDOW_OPENGL;
    SDL_Window* win = NULL;
#ifdef _WIN32
    HWND child = NULL;
#endif
    if(!opts->parent) {
        win = SDL_CreateWindow(opts->title ? opts->title : "gpuic", width, height, api | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    } else {
#if defined(_WIN32)
        child = win32_child((HWND)opts->parent, width, height);
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER, child);
        SDL_SetBooleanProperty(props, p->vulkan ? SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN : SDL_PROP_WINDOW_CREATE_OPENGL_BOOLEAN, true);
        win = child ? SDL_CreateWindowWithProperties(props) : NULL;
        SDL_DestroyProperties(props);
        if(!win && child) {
            DestroyWindow(child);
        }
        SDL_SetHint(SDL_HINT_WINDOWS_ENABLE_MESSAGELOOP, "0"); /* (the host's loop runs it, and takes this window's messages to SDL) */
#elif defined(GP_X11_EMBED)
        if(!SDL_GetCurrentVideoDriver() || strcmp(SDL_GetCurrentVideoDriver(), "x11") != 0) {
            fprintf(stderr, "gpuic: a window inside another (gp_window_options.parent) needs SDL's x11 video driver, not %s (SDL_VIDEO_DRIVER=x11)\n", SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "none");
            return false;
        }
        win = SDL_CreateWindow(opts->title ? opts->title : "gpuic", width, height, api | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIDDEN);
        if(win && !x11_reparent(win, (unsigned long)(uintptr_t)opts->parent)) {
            fprintf(stderr, "gpuic: could not put the window inside its parent (libX11 or SDL's X11 window not found)\n");
            SDL_DestroyWindow(win);
            return false;
        }
        if(win) {
            SDL_SetWindowPosition(win, 0, 0); /* (where SDL had it on the screen is now a place inside the parent: centered, off its corner) */
            SDL_ShowWindow(win);
        }
#else
        fprintf(stderr, "gpuic: a window inside another (gp_window_options.parent) is not supported on this platform\n");
        return false;
#endif
    }
    if(!win) {
        fprintf(stderr, "gpuic: SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetPointerProperty(SDL_GetWindowProperties(win), APP_PROP, app);
    if(!p->gl && !p->vulkan) {
        p->gl = SDL_GL_CreateContext(win);
        if(!p->gl) {
            fprintf(stderr, "gpuic: could not create an OpenGL 3.3 context: %s\n", SDL_GetError());
            SDL_DestroyWindow(win);
#ifdef _WIN32
            if(child) {
                DestroyWindow(child);
            }
#endif
            return false;
        }
        SDL_GL_SetSwapInterval(1);
#ifdef _WIN32
        /* opengl32.dll stays loaded until the process ends. SDL frees it when the last GL window closes, and a driver
         * that runs threads of its own (Mesa's llvmpipe) can deadlock there, joining them under the loader lock. */
        HMODULE gl_dll;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"opengl32.dll", &gl_dll);
#endif
    }
    if(p->gl) {
        SDL_GL_MakeCurrent(win, p->gl);
    }
    p->nwindows++;
    sdl_window* sw = calloc(1, sizeof *sw);
    sw->window = win;
    sw->embedded = opts->parent != NULL;
#ifdef _WIN32
    sw->child = child;
#endif
    w->platform = sw;
    w->id = SDL_GetWindowID(win);
    gp_platform_update_size(w);
    return true;
}

void gp_platform_close_window(gp_app* app, gp_window* w)
{
    if(!w->platform) {
        return;
    }
    sdl_platform* p = plat(app);
    if(--p->nwindows == 0 && p->gl) {
        /* The context goes before its last window, never after: a window destroyed under a live context leaves NVIDIA's driver waiting
         * (about 5 s, in the first swap) in every window made after two such ends, which is a plugin's editor closed and opened again. */
        SDL_GL_MakeCurrent(sdl_win(w), NULL);
        SDL_GL_DestroyContext(p->gl);
        p->gl = NULL;
    }
    SDL_DestroyWindow(sdl_win(w));
#ifdef _WIN32
    if(((sdl_window*)w->platform)->child) {
        DestroyWindow(((sdl_window*)w->platform)->child); /* (SDL leaves a window it did not make) */
    }
#endif
    free(w->platform);
    w->platform = NULL;
}

static void take_focus(gp_window* w)
{
#if defined(_WIN32)
    SetFocus(((sdl_window*)w->platform)->child);
#elif defined(GP_X11_EMBED)
    x11_take_focus(sdl_win(w));
#else
    (void)w;
#endif
}

void* gp_platform_native(gp_window* w)
{
    SDL_PropertiesID props = SDL_GetWindowProperties(sdl_win(w));
#ifdef _WIN32
    return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
#else
    return (void*)(uintptr_t)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
#endif
}

void gp_platform_set_size(gp_window* w, int width, int height)
{
#ifdef GP_X11_EMBED
    if(((sdl_window*)w->platform)->embedded) {
        x11_resize(sdl_win(w), width, height); /* (SDL's size and its next event follow from X's notification) */
        return;
    }
#endif
    SDL_SetWindowSize(sdl_win(w), width, height);
    SDL_SyncWindow(sdl_win(w));
    gp_platform_update_size(w);
}

void gp_platform_make_current(gp_window* w)
{
    if(plat(w->app)->gl) {
        SDL_GL_MakeCurrent(sdl_win(w), plat(w->app)->gl);
    }
}

void gp_platform_swap(gp_window* w)
{
    SDL_GL_SwapWindow(sdl_win(w));
}

bool gp_platform_vulkan(gp_app* app)
{
    return plat(app)->vulkan;
}

void gp_platform_drop_vulkan(gp_app* app)
{
    sdl_platform* p = plat(app);
    if(p->vulkan) {
        SDL_Vulkan_UnloadLibrary();
        p->vulkan = false;
    }
}

void* gp_platform_vk_loader(void)
{
    return (void*)SDL_Vulkan_GetVkGetInstanceProcAddr();
}

const char* const* gp_platform_vk_extensions(uint32_t* count)
{
    Uint32 n = 0;
    const char* const* names = SDL_Vulkan_GetInstanceExtensions(&n);
    *count = n;
    return names;
}

uint64_t gp_platform_vk_surface(gp_window* w, void* instance)
{
    VkSurfaceKHR surface = 0;
    if(!SDL_Vulkan_CreateSurface(sdl_win(w), (VkInstance)instance, NULL, &surface)) {
        fprintf(stderr, "gpuic: no Vulkan surface for the window: %s\n", SDL_GetError());
        return 0;
    }
    return (uint64_t)surface;
}

void gp_platform_set_cursor(gp_window* w, int cursor)
{
    SDL_Cursor* c = plat(w->app)->cursors[cursor];
    if(c != g_cursor) {
        g_cursor = c;
        SDL_SetCursor(c);
    }
}

void gp_platform_set_text_input_area(gp_window* w, float x, float y, float width, float height,
    float cursor_x)
{
    SDL_Rect r = { (int)x, (int)y, (int)width, (int)height };
    SDL_SetTextInputArea(sdl_win(w), &r, (int)cursor_x);
}

char* gp_platform_clipboard_get(void)
{
    if(!SDL_HasClipboardText()) {
        return NULL;
    }
    char* sdl = SDL_GetClipboardText();
    char* out = sdl && *sdl ? strdup(sdl) : NULL;
    SDL_free(sdl);
    return out;
}

void gp_platform_clipboard_set(const char* utf8)
{
    SDL_SetClipboardText(utf8);
}

char* gp_platform_primary_get(void)
{
    if(!SDL_HasPrimarySelectionText()) {
        return NULL;
    }
    char* sdl = SDL_GetPrimarySelectionText();
    char* out = sdl && *sdl ? strdup(sdl) : NULL;
    SDL_free(sdl);
    return out;
}

bool gp_platform_primary_set(const char* utf8)
{
    return SDL_SetPrimarySelectionText(utf8);
}

void gp_platform_set_text_input(gp_window* w, bool on)
{
    if(on) {
        SDL_StartTextInput(sdl_win(w));
    } else {
        SDL_StopTextInput(sdl_win(w));
    }
}

/* Thread-safe: SDL's event queue may be pushed to from any thread. */
void gp_platform_wake(gp_app* app)
{
    (void)app;
    if(!g_wake_event) {
        return;
    }
    SDL_Event e;
    memset(&e, 0, sizeof e); /* `= { 0 }` would only initialize the union's first member */
    e.type = g_wake_event;
    SDL_PushEvent(&e);
}

float gp_platform_refresh_hz(gp_window* w)
{
    SDL_DisplayID d = SDL_GetDisplayForWindow(sdl_win(w));
    const SDL_DisplayMode* m = d ? SDL_GetCurrentDisplayMode(d) : NULL;
    return m && m->refresh_rate > 0 ? m->refresh_rate : 60.f;
}

void* gp_platform_gl_proc(const char* name)
{
    return (void*)SDL_GL_GetProcAddress(name);
}

typedef struct
{
    void (*file)(const char* path, int64_t size, int64_t mtime_ns, void* user);
    void* user;
    int depth;
} walk_ctx;

static SDL_EnumerationResult walk_entry(void* userdata, const char* dirname, const char* fname)
{
    walk_ctx* ctx = userdata;
    char path[1024];
    int n = snprintf(path, sizeof path, "%s%s%s", dirname, dirname[0] && dirname[strlen(dirname) - 1] != '/' ? "/" : "", fname);
    if(n <= 0 || (size_t)n >= sizeof path) {
        return SDL_ENUM_CONTINUE;
    }
    SDL_PathInfo info;
    if(!SDL_GetPathInfo(path, &info)) {
        return SDL_ENUM_CONTINUE;
    }
    if(info.type == SDL_PATHTYPE_FILE) {
        ctx->file(path, (int64_t)info.size, (int64_t)info.modify_time, ctx->user);
    } else if(info.type == SDL_PATHTYPE_DIRECTORY && ctx->depth < 8) { /* depth bounds symlink loops */
        walk_ctx sub = { ctx->file, ctx->user, ctx->depth + 1 };
        SDL_EnumerateDirectory(path, walk_entry, &sub);
    }
    return SDL_ENUM_CONTINUE;
}

void gp_platform_walk_dir(const char* dir, void (*file)(const char* path, int64_t size, int64_t mtime_ns, void* user), void* user)
{
    walk_ctx ctx = { file, user, 0 };
    SDL_EnumerateDirectory(dir, walk_entry, &ctx);
}

/* Synthetic mouse events carry their modifiers in `which`, because the keyboard state SDL
 * would report is read when the event is handled, not when it was queued. Real mouse ids
 * are small numbers or 0xFFFFFFFF/E, so this pattern cannot collide. */
#define SYNTH_MOUSE_ID 0x5a5a0000u
/* GPUIC_IGNORE_REAL_MOUSE=1 (tests): drop what the physical pointer does, keep the events the tests inject. A pointer resting
 * over the test window, or being nudged, otherwise changes hover and repaint behavior under a test that is not about it. */
static bool real_mouse(Uint32 which)
{
    return g_ignore_real_mouse && (which & 0xffff0000u) != SYNTH_MOUSE_ID;
}

static int map_button(int b)
{
    switch(b) {
        case SDL_BUTTON_LEFT:
            return GP_MOUSE_LEFT;
        case SDL_BUTTON_MIDDLE:
            return GP_MOUSE_MIDDLE;
        case SDL_BUTTON_RIGHT:
            return GP_MOUSE_RIGHT;
        default:
            return 0;
    }
}

static const struct
{
    SDL_Keycode k;
    const char* name;
} named[] = {
    {    SDLK_RETURN,     "enter" },
    {  SDLK_KP_ENTER,     "enter" },
    {    SDLK_ESCAPE,    "escape" },
    {       SDLK_TAB,       "tab" },
    { SDLK_BACKSPACE, "backspace" },
    {    SDLK_DELETE,    "delete" },
    {     SDLK_SPACE,     "space" },
    {      SDLK_LEFT,      "left" },
    {     SDLK_RIGHT,     "right" },
    {        SDLK_UP,        "up" },
    {      SDLK_DOWN,      "down" },
    {      SDLK_HOME,      "home" },
    {       SDLK_END,       "end" },
    {    SDLK_PAGEUP,    "pageup" },
    {  SDLK_PAGEDOWN,  "pagedown" },
    {    SDLK_INSERT,    "insert" },
    {        SDLK_F1,        "f1" },
    {        SDLK_F2,        "f2" },
    {        SDLK_F3,        "f3" },
    {        SDLK_F4,        "f4" },
    {        SDLK_F5,        "f5" },
    {        SDLK_F6,        "f6" },
    {        SDLK_F7,        "f7" },
    {        SDLK_F8,        "f8" },
    {        SDLK_F9,        "f9" },
    {       SDLK_F10,       "f10" },
    {       SDLK_F11,       "f11" },
    {       SDLK_F12,       "f12" },
};

/* Canonical (unshifted, lowercase) key name; NULL for keys we do not name. */
static const char* key_name(SDL_Keycode k, char buf[8])
{
    for(size_t i = 0; i < sizeof named / sizeof *named; i++) {
        if(named[i].k == k) {
            return named[i].name;
        }
    }
    if(k > 0x20 && k < 0x7f) {
        buf[0] = (char)(k >= 'A' && k <= 'Z' ? k + 32 : k);
        buf[1] = 0;
        return buf;
    }
    return NULL;
}

static uint32_t map_mods(SDL_Keymod m)
{
    return ((m & SDL_KMOD_CTRL) ? GP_MOD_CTRL : 0) | ((m & SDL_KMOD_ALT) ? GP_MOD_ALT : 0) | ((m & SDL_KMOD_SHIFT) ? GP_MOD_SHIFT : 0) | ((m & SDL_KMOD_GUI) ? GP_MOD_SUPER : 0);
}

/* GPUIC_IGNORE_REAL_KEYS=1 (test runs): drop what the physical keyboard does, keep the keys and text the tests inject.
 * Injected keys carry SYNTH_MOUSE_ID in `which`; injected text is told by where it is stored (inject_text_slot). */
static bool real_key(Uint32 which)
{
    return g_ignore_real_keys && which != SYNTH_MOUSE_ID;
}
static bool injected_text(const char* text);

static void dispatch(gp_app* app, const SDL_Event* e)
{
    gp_window* w;
    SDL_Window* target = SDL_GetWindowFromEvent(e);
    gp_app* owner = target ? SDL_GetPointerProperty(SDL_GetWindowProperties(target), APP_PROP, NULL) : NULL;
    if(owner) {
        app = owner; /* several apps share SDL's one queue: a window's events go to the app that opened it, whichever app took them */
    }
    switch(e->type) {
        case SDL_EVENT_QUIT:
            if(!app->hosted) { /* (an app the host runs ends when the host says) */
                gp_quit(app);
            }
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if((w = gp_app_window_by_id(app, e->window.windowID))) {
                gp_window_on_close(w);
            }
            break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            if((w = gp_app_window_by_id(app, e->window.windowID))) {
                gp_window_on_resize(w);
            }
            break;
        case SDL_EVENT_WINDOW_EXPOSED:
            if((w = gp_app_window_by_id(app, e->window.windowID))) {
                gp_window_on_expose(w);
            }
            break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            if(g_ignore_real_mouse && (Uint32)e->window.data1 != SYNTH_MOUSE_ID) {
                break; /* (gp_simulate_mouse's GP_SIM_LEAVE is marked so) */
            }
            if((w = gp_app_window_by_id(app, e->window.windowID))) {
                gp_window_on_mouse_leave(w);
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if(real_mouse(e->motion.which)) {
                break;
            }
            if((w = gp_app_window_by_id(app, e->motion.windowID))) {
                gp_window_on_mouse_move(w, e->motion.x, e->motion.y, (e->motion.which & 0xffff0000u) == SYNTH_MOUSE_ID ? (e->motion.which & 0xffu) : map_mods(SDL_GetModState()));
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if(e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && (w = gp_app_window_by_id(app, e->button.windowID)) && ((sdl_window*)w->platform)->embedded) {
                take_focus(w); /* (a press in a host's window: its keys are ours from now on) */
            }
            if(real_mouse(e->button.which)) {
                break;
            }
            if((w = gp_app_window_by_id(app, e->button.windowID)) && map_button(e->button.button)) {
                gp_window_on_mouse_button(w, e->button.x, e->button.y, map_button(e->button.button), e->type == SDL_EVENT_MOUSE_BUTTON_DOWN, e->button.clicks, (e->button.which & 0xffff0000u) == SYNTH_MOUSE_ID ? (e->button.which & 0xffu) : map_mods(SDL_GetModState()));
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if(real_mouse(e->wheel.which)) {
                break;
            }
            if((w = gp_app_window_by_id(app, e->wheel.windowID))) {
                float dx = e->wheel.x, dy = e->wheel.y;
                if(e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                    dx = -dx;
                    dy = -dy;
                }
                /* shift + wheel scrolls horizontally: rolling "down" (y < 0) goes right (x > 0) */
                uint32_t mods = (e->wheel.which & 0xffff0000u) == SYNTH_MOUSE_ID ? (e->wheel.which & 0xffu) : map_mods(SDL_GetModState());
                if(dx == 0 && (mods & GP_MOD_SHIFT)) {
                    dx = -dy;
                    dy = 0;
                }
                /* SDL: x > 0 scrolls right, y > 0 scrolls up. Offsets grow rightwards / downwards. */
                gp_window_on_scroll(w, e->wheel.mouse_x, e->wheel.mouse_y, dx * GP_SCROLL_LINE_PX, -dy * GP_SCROLL_LINE_PX, mods);
            }
            break;
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP: {
            sdl_platform* p = plat(app);
            if(!(w = gp_app_window_by_id(app, e->tfinger.windowID))) {
                break;
            }
            if(e->type == SDL_EVENT_FINGER_DOWN) {
                if(p->finger) {
                    break;
                }
                p->finger = e->tfinger.fingerID;
            } /* the first finger only */
            else if(p->finger != e->tfinger.fingerID) {
                break;
            }
            gp_scroll_touch(w, e->type == SDL_EVENT_FINGER_DOWN ? GP_TOUCH_DOWN : e->type == SDL_EVENT_FINGER_MOTION ? GP_TOUCH_MOVE
                                                                                                                     : GP_TOUCH_UP,
                e->tfinger.x * w->size.w,
                e->tfinger.y * w->size.h,
                (int64_t)e->tfinger.timestamp);
            if(e->type == SDL_EVENT_FINGER_UP) {
                p->finger = 0;
            }
            break;
        }
        case SDL_EVENT_KEY_DOWN: {
            if(real_key(e->key.which)) {
                break;
            }
            char buf[8];
            const char* name = key_name(e->key.key, buf);
            if(name && (w = gp_app_window_by_id(app, e->key.windowID))) {
                gp_window_on_key(w, name, map_mods(e->key.mod), e->key.repeat);
            }
            break;
        }
        case SDL_EVENT_KEY_UP: {
            if(real_key(e->key.which)) {
                break;
            }
            char buf[8];
            const char* name = key_name(e->key.key, buf);
            if(name && (w = gp_app_window_by_id(app, e->key.windowID))) {
                gp_window_on_key_up(w, name, map_mods(e->key.mod));
            }
            break;
        }
        case SDL_EVENT_TEXT_EDITING:
            if(!app->debug_ignore_platform_ime && (w = gp_app_window_by_id(app, e->edit.windowID))) {
                gp_window_on_ime(w, e->edit.text, e->edit.start);
            }
            break;
        case SDL_EVENT_TEXT_INPUT:
            if(g_ignore_real_keys && !injected_text(e->text.text)) {
                break;
            }
            if((w = gp_app_window_by_id(app, e->text.windowID))) {
                gp_window_on_text(w, e->text.text);
            }
            break;
        default:
            break;
    }
}

/* SDL keeps the `text` pointer of TEXT_INPUT / TEXT_EDITING events it is given instead of
 * copying it, so injected text needs storage that outlives the queue: a ring of buffers
 * (large enough that an event is long handled before its slot is reused). */
enum { INJECT_SLOTS = 1024,
    INJECT_SIZE = 256,
};
static char g_inject_ring[INJECT_SLOTS][INJECT_SIZE];

static char* inject_text_slot(const char* s)
{
    static unsigned next;
    char* buf = g_inject_ring[next++ % INJECT_SLOTS];
    snprintf(buf, INJECT_SIZE, "%s", s);
    return buf;
}

static bool injected_text(const char* text)
{
    return (uintptr_t)text >= (uintptr_t)g_inject_ring && (uintptr_t)text < (uintptr_t)g_inject_ring + sizeof g_inject_ring;
}

/* Test hook: queue a real SDL key-down (plus the text event SDL would add when text
 * input is active) so the whole platform path is exercised. */
void gp_platform_inject_key(gp_window* w, const char* key, uint32_t mods)
{
    SDL_Keycode k = 0;
    for(size_t i = 0; i < sizeof named / sizeof *named; i++) {
        if(strcmp(named[i].name, key) == 0) {
            k = named[i].k;
            break;
        }
    }
    if(!k && strlen(key) == 1) {
        k = (SDL_Keycode)key[0];
    }
    if(!k) {
        return;
    }

    SDL_Event e;
    memset(&e, 0, sizeof e); /* `= { 0 }` would only initialize the union's first member */
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.windowID = w->id;
    e.key.which = SYNTH_MOUSE_ID;
    e.key.key = k;
    e.key.down = true;
    e.key.mod = ((mods & GP_MOD_CTRL) ? SDL_KMOD_LCTRL : 0) | ((mods & GP_MOD_ALT) ? SDL_KMOD_LALT : 0) | ((mods & GP_MOD_SHIFT) ? SDL_KMOD_LSHIFT : 0) | ((mods & GP_MOD_SUPER) ? SDL_KMOD_LGUI : 0);
    SDL_PushEvent(&e);

    char text[2] = { k == SDLK_SPACE ? ' ' : (k > 0x20 && k < 0x7f ? (char)k : 0), 0 };
    if(text[0] >= 'a' && text[0] <= 'z' && (mods & GP_MOD_SHIFT)) {
        text[0] -= 32;
    }
    /* Always queue the text a real keyboard would produce; if the key was handled, the app drops
     * it (swallow_text), and if nothing wants text it is ignored, exactly as with real SDL. */
    if(text[0] && !(mods & (GP_MOD_CTRL | GP_MOD_ALT | GP_MOD_SUPER))) {
        SDL_Event t;
        memset(&t, 0, sizeof t); /* `= { 0 }` would only initialize the union's first member */
        t.type = SDL_EVENT_TEXT_INPUT;
        t.text.windowID = w->id;
        t.text.text = inject_text_slot(text);
        SDL_PushEvent(&t);
    }
}

void gp_platform_inject_key_up(gp_window* w, const char* key, uint32_t mods)
{
    SDL_Keycode k = 0;
    for(size_t i = 0; i < sizeof named / sizeof *named; i++) {
        if(strcmp(named[i].name, key) == 0) {
            k = named[i].k;
            break;
        }
    }
    if(!k && strlen(key) == 1) {
        k = (SDL_Keycode)key[0];
    }
    if(!k) {
        return;
    }
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_KEY_UP;
    e.key.windowID = w->id;
    e.key.which = SYNTH_MOUSE_ID;
    e.key.key = k;
    e.key.down = false;
    e.key.mod = ((mods & GP_MOD_CTRL) ? SDL_KMOD_LCTRL : 0) | ((mods & GP_MOD_ALT) ? SDL_KMOD_LALT : 0) | ((mods & GP_MOD_SHIFT) ? SDL_KMOD_LSHIFT : 0) | ((mods & GP_MOD_SUPER) ? SDL_KMOD_LGUI : 0);
    SDL_PushEvent(&e);
}

void gp_platform_inject_text(gp_window* w, const char* utf8)
{
    SDL_Event t;
    memset(&t, 0, sizeof t); /* `= { 0 }` would only initialize the union's first member */
    t.type = SDL_EVENT_TEXT_INPUT;
    t.text.windowID = w->id;
    t.text.text = inject_text_slot(utf8);
    SDL_PushEvent(&t);
}

void gp_platform_inject_mouse(gp_window* w, int kind, float x, float y, int button, int clicks, uint32_t mods)
{
    SDL_Event e;
    memset(&e, 0, sizeof e); /* `= { 0 }` would only initialize the union's first member */
    if(kind == GP_SIM_LEAVE) {
        e.type = SDL_EVENT_WINDOW_MOUSE_LEAVE;
        e.window.windowID = w->id;
        e.window.data1 = (Sint32)SYNTH_MOUSE_ID;
    } else if(kind == GP_SIM_MOVE) {
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.windowID = w->id;
        e.motion.which = SYNTH_MOUSE_ID | (mods & 0xffu); /* (motion carries no modifiers either: smuggled) */
        e.motion.x = x;
        e.motion.y = y;
    } else {
        e.type = kind == GP_SIM_DOWN ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.windowID = w->id;
        e.button.button = button == 2 ? SDL_BUTTON_MIDDLE : button == 3 ? SDL_BUTTON_RIGHT
                                                                        : SDL_BUTTON_LEFT;
        e.button.down = kind == GP_SIM_DOWN;
        e.button.clicks = (Uint8)clicks;
        e.button.which = SYNTH_MOUSE_ID | (mods & 0xffu); /* button events carry no modifiers: smuggle them */
        e.button.x = x;
        e.button.y = y;
    }
    SDL_PushEvent(&e);
}

void gp_platform_inject_wheel(gp_window* w, float x, float y, float dx, float dy, uint32_t mods)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);                         /* `= { 0 }` would only initialize the union's first member */
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.windowID = w->id;
    e.wheel.which = SYNTH_MOUSE_ID | (mods & 0xffu); /* (a wheel event carries no modifiers either: smuggled) */
    e.wheel.x = dx;
    e.wheel.y = dy;
    e.wheel.mouse_x = x;
    e.wheel.mouse_y = y;
    e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    SDL_PushEvent(&e);
}

void gp_platform_pump(gp_app* app, int timeout_ms)
{
    SDL_Event e;
    if(timeout_ms > 0) {
        /* A wait can end with no event a little before its time (on Windows by up to a timer tick): wait out the rest
         * here, so the loop wakes once, when the timer it slept for is due. */
        int64_t deadline = gp_now_ns() + (int64_t)timeout_ms * 1000000;
        for(;;) {
            int64_t left = deadline - gp_now_ns();
            if(left <= 0) {
                return;
            }
            if(SDL_WaitEventTimeout(&e, (Sint32)((left + 999999) / 1000000))) {
                break;
            }
        }
        dispatch(app, &e);
        gp_effects_flush(app); /* observers run before the next event is looked at */
    } else if(timeout_ms < 0) {
        if(!SDL_WaitEvent(&e)) {
            return;
        }
        dispatch(app, &e);
        gp_effects_flush(app); /* observers run before the next event is looked at */
    }
    while(SDL_PollEvent(&e)) {
        dispatch(app, &e);
        gp_effects_flush(app);
    }
}

bool gp_platform_mkdirs(const char* dir)
{
    return SDL_CreateDirectory(dir);
}

bool gp_platform_rename(const char* from, const char* to)
{
    return SDL_RenamePath(from, to);
}
