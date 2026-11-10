/*
 * Platform backend on SDL3: windows, GL context, input events, cursors.
 * Everything SDL-specific lives here, so a native Wayland/X11 backend only
 * has to reimplement the gp_platform_* functions from internal.h.
 */
#include "internal.h"

#include <SDL3/SDL.h>

typedef struct {
    SDL_GLContext gl;
    SDL_Cursor *cursors[GP_CURSOR_COUNT];
    SDL_FingerID finger;   /* the finger being followed (the first), 0 for none */
} sdl_platform;

typedef struct {
    SDL_Window *window;
} sdl_window;

static bool g_ignore_real_mouse;   /* GPUIC_IGNORE_REAL_MOUSE, see real_mouse() */
static Uint32 g_wake_event; /* custom event used to wake the loop from other threads */

static sdl_platform *plat(gp_app *app) { return app->platform; }
static SDL_Window *sdl_win(gp_window *w) { return ((sdl_window *)w->platform)->window; }

bool gp_platform_init(gp_app *app) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "gpuic: SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    g_wake_event = SDL_RegisterEvents(1);
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");   /* a finger is not a mouse: it scrolls (gp_scroll_touch) and a tap is made a click there */
    g_ignore_real_mouse = getenv("GPUIC_IGNORE_REAL_MOUSE") != NULL;
    sdl_platform *p = calloc(1, sizeof *p);
    p->cursors[GP_CURSOR_DEFAULT] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    p->cursors[GP_CURSOR_POINTER] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
    p->cursors[GP_CURSOR_TEXT] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
    app->platform = p;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    return true;
}

void gp_platform_shutdown(gp_app *app) {
    sdl_platform *p = plat(app);
    if (!p) return;
    for (int i = 0; i < GP_CURSOR_COUNT; i++)
        if (p->cursors[i]) SDL_DestroyCursor(p->cursors[i]);
    if (p->gl) SDL_GL_DestroyContext(p->gl);
    free(p);
    app->platform = NULL;
    SDL_Quit();
}

void gp_platform_update_size(gp_window *w) {
    int lw, lh, pw, ph;
    SDL_GetWindowSize(sdl_win(w), &lw, &lh);
    SDL_GetWindowSizeInPixels(sdl_win(w), &pw, &ph);
    w->size = (gp_size){ (float)lw, (float)lh };
    w->px_w = pw;
    w->px_h = ph;
    w->scale = lw > 0 ? (float)pw / (float)lw : 1.f;
}

bool gp_platform_open_window(gp_app *app, gp_window *w, const gp_window_options *opts) {
    sdl_platform *p = plat(app);
    SDL_Window *win = SDL_CreateWindow(opts->title ? opts->title : "gpuic",
                                       opts->width > 0 ? opts->width : 960,
                                       opts->height > 0 ? opts->height : 640,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                                           SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!win) {
        fprintf(stderr, "gpuic: SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    if (!p->gl) {
        p->gl = SDL_GL_CreateContext(win);
        if (!p->gl) {
            fprintf(stderr, "gpuic: could not create an OpenGL 3.3 context: %s\n", SDL_GetError());
            SDL_DestroyWindow(win);
            return false;
        }
        SDL_GL_SetSwapInterval(1);
    }
    SDL_GL_MakeCurrent(win, p->gl);
    sdl_window *sw = calloc(1, sizeof *sw);
    sw->window = win;
    w->platform = sw;
    w->id = SDL_GetWindowID(win);
    gp_platform_update_size(w);
    return true;
}

void gp_platform_close_window(gp_app *app, gp_window *w) {
    (void)app;
    if (!w->platform) return;
    SDL_DestroyWindow(sdl_win(w));
    free(w->platform);
    w->platform = NULL;
}

void gp_platform_make_current(gp_window *w) {
    SDL_GL_MakeCurrent(sdl_win(w), plat(w->app)->gl);
}

void gp_platform_swap(gp_window *w) { SDL_GL_SwapWindow(sdl_win(w)); }

void gp_platform_set_cursor(gp_window *w, int cursor) {
    (void)w;
    SDL_SetCursor(plat(w->app)->cursors[cursor]);
}

void gp_platform_set_text_input_area(gp_window *w, float x, float y, float width, float height,
                                     float cursor_x) {
    SDL_Rect r = { (int)x, (int)y, (int)width, (int)height };
    SDL_SetTextInputArea(sdl_win(w), &r, (int)cursor_x);
}

char *gp_platform_clipboard_get(void) {
    if (!SDL_HasClipboardText()) return NULL;
    char *sdl = SDL_GetClipboardText();
    char *out = sdl && *sdl ? strdup(sdl) : NULL;
    SDL_free(sdl);
    return out;
}

void gp_platform_clipboard_set(const char *utf8) { SDL_SetClipboardText(utf8); }

char *gp_platform_primary_get(void) {
    if (!SDL_HasPrimarySelectionText()) return NULL;
    char *sdl = SDL_GetPrimarySelectionText();
    char *out = sdl && *sdl ? strdup(sdl) : NULL;
    SDL_free(sdl);
    return out;
}

bool gp_platform_primary_set(const char *utf8) { return SDL_SetPrimarySelectionText(utf8); }

void gp_platform_set_text_input(gp_window *w, bool on) {
    if (on) SDL_StartTextInput(sdl_win(w));
    else SDL_StopTextInput(sdl_win(w));
}

/* Thread-safe: SDL's event queue may be pushed to from any thread. */
void gp_platform_wake(gp_app *app) {
    (void)app;
    if (!g_wake_event) return;
    SDL_Event e;
    memset(&e, 0, sizeof e); /* `= { 0 }` would only initialize the union's first member */
    e.type = g_wake_event;
    SDL_PushEvent(&e);
}

float gp_platform_refresh_hz(gp_window *w) {
    SDL_DisplayID d = SDL_GetDisplayForWindow(sdl_win(w));
    const SDL_DisplayMode *m = d ? SDL_GetCurrentDisplayMode(d) : NULL;
    return m && m->refresh_rate > 0 ? m->refresh_rate : 60.f;
}

void *gp_platform_gl_proc(const char *name) { return (void *)SDL_GL_GetProcAddress(name); }

typedef struct {
    void (*file)(const char *path, int64_t size, int64_t mtime_ns, void *user);
    void *user;
    int depth;
} walk_ctx;

static SDL_EnumerationResult walk_entry(void *userdata, const char *dirname, const char *fname) {
    walk_ctx *ctx = userdata;
    char path[1024];
    int n = snprintf(path, sizeof path, "%s%s%s", dirname, dirname[0] && dirname[strlen(dirname) - 1] != '/' ? "/" : "", fname);
    if (n <= 0 || (size_t)n >= sizeof path) return SDL_ENUM_CONTINUE;
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) return SDL_ENUM_CONTINUE;
    if (info.type == SDL_PATHTYPE_FILE) {
        ctx->file(path, (int64_t)info.size, (int64_t)info.modify_time, ctx->user);
    } else if (info.type == SDL_PATHTYPE_DIRECTORY && ctx->depth < 8) { /* depth bounds symlink loops */
        walk_ctx sub = {ctx->file, ctx->user, ctx->depth + 1};
        SDL_EnumerateDirectory(path, walk_entry, &sub);
    }
    return SDL_ENUM_CONTINUE;
}

void gp_platform_walk_dir(const char *dir, void (*file)(const char *path, int64_t size, int64_t mtime_ns, void *user), void *user) {
    walk_ctx ctx = {file, user, 0};
    SDL_EnumerateDirectory(dir, walk_entry, &ctx);
}

/* Synthetic mouse events carry their modifiers in `which`, because the keyboard state SDL
 * would report is read when the event is handled, not when it was queued. Real mouse ids
 * are small numbers or 0xFFFFFFFF/E, so this pattern cannot collide. */
#define SYNTH_MOUSE_ID 0x5a5a0000u
/* GPUIC_IGNORE_REAL_MOUSE=1 (tests): drop what the physical pointer does, keep the events the tests inject. A pointer resting
 * over the test window, or being nudged, otherwise changes hover and repaint behavior under a test that is not about it. */
static bool real_mouse(Uint32 which) { return g_ignore_real_mouse && (which & 0xffff0000u) != SYNTH_MOUSE_ID; }

static int map_button(int b) {
    switch (b) {
    case SDL_BUTTON_LEFT: return GP_MOUSE_LEFT;
    case SDL_BUTTON_MIDDLE: return GP_MOUSE_MIDDLE;
    case SDL_BUTTON_RIGHT: return GP_MOUSE_RIGHT;
    default: return 0;
    }
}

static const struct { SDL_Keycode k; const char *name; } named[] = {
        { SDLK_RETURN, "enter" }, { SDLK_KP_ENTER, "enter" }, { SDLK_ESCAPE, "escape" },
        { SDLK_TAB, "tab" }, { SDLK_BACKSPACE, "backspace" }, { SDLK_DELETE, "delete" },
        { SDLK_SPACE, "space" }, { SDLK_LEFT, "left" }, { SDLK_RIGHT, "right" },
        { SDLK_UP, "up" }, { SDLK_DOWN, "down" }, { SDLK_HOME, "home" }, { SDLK_END, "end" },
        { SDLK_PAGEUP, "pageup" }, { SDLK_PAGEDOWN, "pagedown" }, { SDLK_INSERT, "insert" },
        { SDLK_F1, "f1" }, { SDLK_F2, "f2" }, { SDLK_F3, "f3" }, { SDLK_F4, "f4" },
        { SDLK_F5, "f5" }, { SDLK_F6, "f6" }, { SDLK_F7, "f7" }, { SDLK_F8, "f8" },
        { SDLK_F9, "f9" }, { SDLK_F10, "f10" }, { SDLK_F11, "f11" }, { SDLK_F12, "f12" },
};

/* Canonical (unshifted, lowercase) key name; NULL for keys we do not name. */
static const char *key_name(SDL_Keycode k, char buf[8]) {
    for (size_t i = 0; i < sizeof named / sizeof *named; i++)
        if (named[i].k == k) return named[i].name;
    if (k > 0x20 && k < 0x7f) {
        buf[0] = (char)(k >= 'A' && k <= 'Z' ? k + 32 : k);
        buf[1] = 0;
        return buf;
    }
    return NULL;
}

static uint32_t map_mods(SDL_Keymod m) {
    return ((m & SDL_KMOD_CTRL) ? GP_MOD_CTRL : 0) | ((m & SDL_KMOD_ALT) ? GP_MOD_ALT : 0) |
           ((m & SDL_KMOD_SHIFT) ? GP_MOD_SHIFT : 0) | ((m & SDL_KMOD_GUI) ? GP_MOD_SUPER : 0);
}

static void dispatch(gp_app *app, const SDL_Event *e) {
    gp_window *w;
    switch (e->type) {
    case SDL_EVENT_QUIT:
        gp_quit(app);
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if ((w = gp_app_window_by_id(app, e->window.windowID))) gp_window_on_close(w);
        break;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        if ((w = gp_app_window_by_id(app, e->window.windowID))) gp_window_on_resize(w);
        break;
    case SDL_EVENT_WINDOW_EXPOSED:
        if ((w = gp_app_window_by_id(app, e->window.windowID))) gp_window_on_expose(w);
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        if (g_ignore_real_mouse && (Uint32)e->window.data1 != SYNTH_MOUSE_ID) break;   /* (gp_simulate_mouse's GP_SIM_LEAVE is marked so) */
        if ((w = gp_app_window_by_id(app, e->window.windowID))) gp_window_on_mouse_leave(w);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (real_mouse(e->motion.which)) break;
        if ((w = gp_app_window_by_id(app, e->motion.windowID)))
            gp_window_on_mouse_move(w, e->motion.x, e->motion.y,
                                    (e->motion.which & 0xffff0000u) == SYNTH_MOUSE_ID ? (e->motion.which & 0xffu) : map_mods(SDL_GetModState()));
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (real_mouse(e->button.which)) break;
        if ((w = gp_app_window_by_id(app, e->button.windowID)) && map_button(e->button.button))
            gp_window_on_mouse_button(w, e->button.x, e->button.y, map_button(e->button.button),
                                      e->type == SDL_EVENT_MOUSE_BUTTON_DOWN, e->button.clicks,
                                      (e->button.which & 0xffff0000u) == SYNTH_MOUSE_ID
                                          ? (e->button.which & 0xffu) : map_mods(SDL_GetModState()));
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (real_mouse(e->wheel.which)) break;
        if ((w = gp_app_window_by_id(app, e->wheel.windowID))) {
            float dx = e->wheel.x, dy = e->wheel.y;
            if (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) { dx = -dx; dy = -dy; }
            /* shift + wheel scrolls horizontally: rolling "down" (y < 0) goes right (x > 0) */
            uint32_t mods = (e->wheel.which & 0xffff0000u) == SYNTH_MOUSE_ID ? (e->wheel.which & 0xffu) : map_mods(SDL_GetModState());
            if (dx == 0 && (mods & GP_MOD_SHIFT)) { dx = -dy; dy = 0; }
            /* SDL: x > 0 scrolls right, y > 0 scrolls up. Offsets grow rightwards / downwards. */
            gp_window_on_scroll(w, e->wheel.mouse_x, e->wheel.mouse_y, dx * GP_SCROLL_LINE_PX,
                                -dy * GP_SCROLL_LINE_PX, mods);
        }
        break;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP: {
        sdl_platform *p = plat(app);
        if (!(w = gp_app_window_by_id(app, e->tfinger.windowID))) break;
        if (e->type == SDL_EVENT_FINGER_DOWN) { if (p->finger) break; p->finger = e->tfinger.fingerID; }   /* the first finger only */
        else if (p->finger != e->tfinger.fingerID) break;
        gp_scroll_touch(w, e->type == SDL_EVENT_FINGER_DOWN ? GP_TOUCH_DOWN : e->type == SDL_EVENT_FINGER_MOTION ? GP_TOUCH_MOVE : GP_TOUCH_UP,
                        e->tfinger.x * w->size.w, e->tfinger.y * w->size.h, (int64_t)e->tfinger.timestamp);
        if (e->type == SDL_EVENT_FINGER_UP) p->finger = 0;
        break;
    }
    case SDL_EVENT_KEY_DOWN: {
        char buf[8];
        const char *name = key_name(e->key.key, buf);
        if (name && (w = gp_app_window_by_id(app, e->key.windowID)))
            gp_window_on_key(w, name, map_mods(e->key.mod), e->key.repeat);
        break;
    }
    case SDL_EVENT_KEY_UP: {
        char buf[8];
        const char *name = key_name(e->key.key, buf);
        if (name && (w = gp_app_window_by_id(app, e->key.windowID))) gp_window_on_key_up(w, name, map_mods(e->key.mod));
        break;
    }
    case SDL_EVENT_TEXT_EDITING:
        if (!app->debug_ignore_platform_ime && (w = gp_app_window_by_id(app, e->edit.windowID))) gp_window_on_ime(w, e->edit.text, e->edit.start);
        break;
    case SDL_EVENT_TEXT_INPUT:
        if ((w = gp_app_window_by_id(app, e->text.windowID))) gp_window_on_text(w, e->text.text);
        break;
    default:
        break;
    }
}

/* SDL keeps the `text` pointer of TEXT_INPUT / TEXT_EDITING events it is given instead of
 * copying it, so injected text needs storage that outlives the queue: a ring of buffers
 * (large enough that an event is long handled before its slot is reused). */
static char *inject_text_slot(const char *s) {
    enum { SLOTS = 1024, SIZE = 256 };
    static char ring[SLOTS][SIZE];
    static unsigned next;
    char *buf = ring[next++ % SLOTS];
    snprintf(buf, SIZE, "%s", s);
    return buf;
}

/* Test hook: queue a real SDL key-down (plus the text event SDL would add when text
 * input is active) so the whole platform path is exercised. */
void gp_platform_inject_key(gp_window *w, const char *key, uint32_t mods) {
    SDL_Keycode k = 0;
    for (size_t i = 0; i < sizeof named / sizeof *named; i++)
        if (strcmp(named[i].name, key) == 0) { k = named[i].k; break; }
    if (!k && strlen(key) == 1) k = (SDL_Keycode)key[0];
    if (!k) return;

    SDL_Event e;
    memset(&e, 0, sizeof e); /* `= { 0 }` would only initialize the union's first member */
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.windowID = w->id;
    e.key.key = k;
    e.key.down = true;
    e.key.mod = ((mods & GP_MOD_CTRL) ? SDL_KMOD_LCTRL : 0) | ((mods & GP_MOD_ALT) ? SDL_KMOD_LALT : 0) |
                ((mods & GP_MOD_SHIFT) ? SDL_KMOD_LSHIFT : 0) | ((mods & GP_MOD_SUPER) ? SDL_KMOD_LGUI : 0);
    SDL_PushEvent(&e);

    char text[2] = { k == SDLK_SPACE ? ' ' : (k > 0x20 && k < 0x7f ? (char)k : 0), 0 };
    if (text[0] >= 'a' && text[0] <= 'z' && (mods & GP_MOD_SHIFT)) text[0] -= 32;
    /* Always queue the text a real keyboard would produce; if the key was handled, the app drops
     * it (swallow_text), and if nothing wants text it is ignored, exactly as with real SDL. */
    if (text[0] && !(mods & (GP_MOD_CTRL | GP_MOD_ALT | GP_MOD_SUPER))) {
        SDL_Event t;
    memset(&t, 0, sizeof t); /* `= { 0 }` would only initialize the union's first member */
        t.type = SDL_EVENT_TEXT_INPUT;
        t.text.windowID = w->id;
        t.text.text = inject_text_slot(text);
        SDL_PushEvent(&t);
    }
}

void gp_platform_inject_key_up(gp_window *w, const char *key, uint32_t mods) {
    SDL_Keycode k = 0;
    for (size_t i = 0; i < sizeof named / sizeof *named; i++)
        if (strcmp(named[i].name, key) == 0) { k = named[i].k; break; }
    if (!k && strlen(key) == 1) k = (SDL_Keycode)key[0];
    if (!k) return;
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_KEY_UP;
    e.key.windowID = w->id;
    e.key.key = k;
    e.key.down = false;
    e.key.mod = ((mods & GP_MOD_CTRL) ? SDL_KMOD_LCTRL : 0) | ((mods & GP_MOD_ALT) ? SDL_KMOD_LALT : 0) |
                ((mods & GP_MOD_SHIFT) ? SDL_KMOD_LSHIFT : 0) | ((mods & GP_MOD_SUPER) ? SDL_KMOD_LGUI : 0);
    SDL_PushEvent(&e);
}

void gp_platform_inject_text(gp_window *w, const char *utf8) {
    SDL_Event t;
    memset(&t, 0, sizeof t); /* `= { 0 }` would only initialize the union's first member */
    t.type = SDL_EVENT_TEXT_INPUT;
    t.text.windowID = w->id;
    t.text.text = inject_text_slot(utf8);
    SDL_PushEvent(&t);
}

void gp_platform_inject_mouse(gp_window *w, int kind, float x, float y, int button, int clicks, uint32_t mods) {
    SDL_Event e;
    memset(&e, 0, sizeof e); /* `= { 0 }` would only initialize the union's first member */
    if (kind == GP_SIM_LEAVE) {
        e.type = SDL_EVENT_WINDOW_MOUSE_LEAVE;
        e.window.windowID = w->id;
        e.window.data1 = (Sint32)SYNTH_MOUSE_ID;
    } else if (kind == GP_SIM_MOVE) {
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.windowID = w->id;
        e.motion.which = SYNTH_MOUSE_ID | (mods & 0xffu); /* (motion carries no modifiers either: smuggled) */
        e.motion.x = x;
        e.motion.y = y;
    } else {
        e.type = kind == GP_SIM_DOWN ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.windowID = w->id;
        e.button.button = button == 2 ? SDL_BUTTON_MIDDLE : button == 3 ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
        e.button.down = kind == GP_SIM_DOWN;
        e.button.clicks = (Uint8)clicks;
        e.button.which = SYNTH_MOUSE_ID | (mods & 0xffu); /* button events carry no modifiers: smuggle them */
        e.button.x = x;
        e.button.y = y;
    }
    SDL_PushEvent(&e);
}

void gp_platform_inject_wheel(gp_window *w, float x, float y, float dx, float dy, uint32_t mods) {
    SDL_Event e;
    memset(&e, 0, sizeof e); /* `= { 0 }` would only initialize the union's first member */
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

void gp_platform_pump(gp_app *app, int timeout_ms) {
    SDL_Event e;
    if (timeout_ms != 0) {
        if (!SDL_WaitEventTimeout(&e, timeout_ms)) return;
        dispatch(app, &e);
        gp_effects_flush(app);   /* observers run before the next event is looked at */
    }
    while (SDL_PollEvent(&e)) {
        dispatch(app, &e);
        gp_effects_flush(app);
    }
}

bool gp_platform_mkdirs(const char *dir) { return SDL_CreateDirectory(dir); }

bool gp_platform_rename(const char *from, const char *to) { return SDL_RenamePath(from, to); }
