/*
 * Platform backend on Wayland, with no SDL (built with -DGPUIC_PLATFORM=wayland): windows (xdg-shell, decorations asked of the compositor),
 * OpenGL through EGL or a Vulkan surface, the pointer, the keyboard (xkbcommon, with key repeat done here), touch, text input and IME
 * (text-input-v3), the clipboard and the primary selection, cursors (cursor-shape-v1), and fractional scaling (fractional-scale-v1 with a
 * viewport; else the integer scale the surface asks for). What platform_sdl.c does, as gp_platform_* in internal.h says.
 *
 * One connection serves every app in the process (gp_app_new can make several): a window's events go to the app that opened it. Events
 * the tests inject wait in a queue of their own and are handed out by the pump in order, as SDL's queue does for platform_sdl.c.
 * Not here: a window inside another (Wayland has no foreign windows to embed into), and a title bar of gpuic's own where the compositor will
 * not draw one (GNOME).
 */
#define _GNU_SOURCE
#include "internal.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include <xkbcommon/xkbcommon.h>

#include "wayland/cursor-shape-v1-client.h"
#include "wayland/fractional-scale-v1-client.h"
#include "wayland/primary-selection-unstable-v1-client.h"
#include "wayland/text-input-unstable-v3-client.h"
#include "wayland/viewporter-client.h"
#include "wayland/xdg-decoration-unstable-v1-client.h"
#include "wayland/xdg-shell-client.h"

/* ---------------------------------------------------------------- state */

typedef struct wl_win wl_win;
struct wl_win
{
    gp_window* w;
    gp_app* app;
    wl_win* next;
    struct wl_surface* surface;
    struct xdg_surface* xdg;
    struct xdg_toplevel* top;
    struct zxdg_toplevel_decoration_v1* deco;
    struct wp_fractional_scale_v1* frac;
    struct wp_viewport* viewport;
    struct wl_egl_window* egl_win;
    EGLSurface egl;
    int lw, lh;               /* logical size */
    int pending_w, pending_h; /* from the last toplevel configure (0: the client's choice) */
    double scale;             /* fractional (n / 120), or the surface's integer one */
    int buffer_scale;         /* (without fractional scaling: what the surface is told) */
    bool configured;          /* the first configure came: the window may be drawn */
    bool text_input;          /* the app wants text input in it */
    char* title;              /* the last given (the compositor gives none back) */
    struct wl_output* outputs[8];
    int noutputs;
};

typedef struct
{
    struct wl_output* output;
    int refresh_mhz;
} wl_out;

/* The one connection, and what it found. */
static struct
{
    int users; /* apps alive */
    struct wl_display* display;
    struct wl_registry* registry;
    struct wl_compositor* compositor;
    uint32_t compositor_version;
    struct xdg_wm_base* wm;
    struct wl_seat* seat;
    struct wl_pointer* pointer;
    struct wl_keyboard* keyboard;
    struct wl_touch* touch;
    struct wl_data_device_manager* ddm;
    struct wl_data_device* data_device;
    struct zwp_primary_selection_device_manager_v1* psm;
    struct zwp_primary_selection_device_v1* primary_device;
    struct zwp_text_input_manager_v3* tim;
    struct zwp_text_input_v3* ti;
    struct zxdg_decoration_manager_v1* deco;
    struct wp_fractional_scale_manager_v1* frac;
    struct wp_viewporter* viewporter;
    struct wp_cursor_shape_manager_v1* cursor_mgr;
    struct wp_cursor_shape_device_v1* cursor_dev;
    wl_out outputs[8];
    int noutputs;
    wl_win* windows;
    uint32_t next_id;
    int wake_fd;
    /* input */
    uint32_t serial;         /* the last input event's: what setting a selection is checked against */
    uint32_t pointer_serial; /* the pointer's enter (setting the cursor wants it) */
    wl_win* pointer_win;
    double px, py;           /* the pointer, in the window */
    int cursor;              /* asked for */
    int axis_v120[2];        /* this frame's wheel, in 120ths of a notch (x, y) */
    double axis_px[2];       /* ... continuous (a touchpad), in surface units */
    bool axis_any;
    uint32_t last_button;    /* clicks: a press of the same button soon after and near the last is the next of a run */
    int64_t last_press_ns;
    double last_press_x, last_press_y;
    int clicks;
    wl_win* key_win;
    struct xkb_context* xkb;
    struct xkb_keymap* keymap;
    struct xkb_state* xkb_state;
    int repeat_rate, repeat_delay; /* keys a second, ms before the first */
    uint32_t repeat_key;           /* the key held (an evdev code + 8), 0 for none */
    int64_t repeat_at;             /* when it repeats next */
    int32_t touch_id;              /* the finger followed (the first), -1 for none */
    double tx, ty;                 /* ... where it is */
    wl_win* touch_win;
    /* text input: what the compositor said since the last done */
    char* ti_preedit;
    int ti_cursor;
    char* ti_commit;
    wl_win* ti_win;
    /* selections */
    struct wl_data_offer* clip_offer; /* (each offer's user data: a bool, whether it has text) */
    struct wl_data_offer* dnd_offer;  /* one dragged over a window: not used, destroyed when the drag leaves or drops */
    char* clip_text;                  /* ours, while we own the clipboard */
    struct wl_data_source* clip_source;
    struct zwp_primary_selection_offer_v1* prim_offer;
    bool prim_offer_text;
    char* prim_text;
    struct zwp_primary_selection_source_v1* prim_source;
    /* EGL (one context for every window, as platform_sdl.c has) */
    EGLDisplay egl_dpy;
    EGLConfig egl_cfg;
    EGLContext egl_ctx;
    /* Vulkan */
    void* vk_lib;
} g = { .wake_fd = -1, .touch_id = -1 };

typedef struct
{
    bool vulkan;
} wl_platform;

static bool g_ignore_real_mouse, g_ignore_real_keys;

static wl_win* win_of(gp_window* w)
{
    return w->platform;
}

static wl_win* win_for_surface(struct wl_surface* s)
{
    for(wl_win* v = g.windows; v; v = v->next) {
        if(v->surface == s) {
            return v;
        }
    }
    return NULL;
}

/* after each event handed to an app: its observers run before the next is looked at (as platform_sdl.c's pump does) */
static unsigned g_delivered; /* events handed to apps: what ends a pump's wait (not a ping, an output's mode, a configure the app does not see) */

static void flush(wl_win* v)
{
    g_delivered++;
    gp_effects_flush(v->w->app); /* (the window's: set before it opens, while v->app waits for it to be open) */
}

static uint32_t mods_now(void)
{
    if(!g.xkb_state) {
        return 0;
    }
    uint32_t m = 0;
    m |= xkb_state_mod_name_is_active(g.xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0 ? GP_MOD_CTRL : 0;
    m |= xkb_state_mod_name_is_active(g.xkb_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0 ? GP_MOD_ALT : 0;
    m |= xkb_state_mod_name_is_active(g.xkb_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0 ? GP_MOD_SHIFT : 0;
    m |= xkb_state_mod_name_is_active(g.xkb_state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0 ? GP_MOD_SUPER : 0;
    return m;
}

/* ---------------------------------------------------------------- outputs (the refresh rate) */

static void output_geometry(void* d, struct wl_output* o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sub, const char* make, const char* model, int32_t tr)
{
}
static void output_mode(void* d, struct wl_output* o, uint32_t flags, int32_t w, int32_t h, int32_t refresh)
{
    wl_out* out = d;
    if(flags & WL_OUTPUT_MODE_CURRENT) {
        out->refresh_mhz = refresh;
    }
}
static void output_done(void* d, struct wl_output* o)
{
}
static void output_scale(void* d, struct wl_output* o, int32_t f)
{
}
static void output_name(void* d, struct wl_output* o, const char* n)
{
}
static void output_description(void* d, struct wl_output* o, const char* n)
{
}
static const struct wl_output_listener output_listener = { output_geometry, output_mode, output_done, output_scale, output_name, output_description };

/* ---------------------------------------------------------------- windows */

/* what the window is at now: logical size, the scale, and so its pixels; the surface told what its buffers are */
static void apply_size(wl_win* v)
{
    int pw = (int)lround(v->lw * v->scale), ph = (int)lround(v->lh * v->scale);
    pw = pw < 1 ? 1 : pw;
    ph = ph < 1 ? 1 : ph;
    if(v->viewport) {
        wp_viewport_set_destination(v->viewport, v->lw, v->lh);
    } else {
        wl_surface_set_buffer_scale(v->surface, v->buffer_scale);
    }
    if(v->egl_win) {
        wl_egl_window_resize(v->egl_win, pw, ph, 0, 0);
    }
    struct wl_region* opaque = wl_compositor_create_region(g.compositor); /* (whatever the buffer's alpha: the window is opaque) */
    wl_region_add(opaque, 0, 0, v->lw, v->lh);
    wl_surface_set_opaque_region(v->surface, opaque);
    wl_region_destroy(opaque);
}

static void resized(wl_win* v)
{
    apply_size(v);
    if(v->configured && v->app) {
        gp_window_on_resize(v->w);
        flush(v);
    }
}

static void wm_ping(void* d, struct xdg_wm_base* wm, uint32_t serial)
{
    xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void xdg_configure(void* d, struct xdg_surface* xs, uint32_t serial)
{
    wl_win* v = d;
    xdg_surface_ack_configure(xs, serial);
    bool changed = (v->pending_w > 0 && v->pending_w != v->lw) || (v->pending_h > 0 && v->pending_h != v->lh);
    if(v->pending_w > 0) {
        v->lw = v->pending_w;
    }
    if(v->pending_h > 0) {
        v->lh = v->pending_h;
    }
    bool first = !v->configured;
    v->configured = true;
    if(changed || first) { /* (another configure, a change of focus or of tiling, needs no frame: the compositor keeps the last) */
        resized(v);
    }
}
static const struct xdg_surface_listener xdg_surface_listener = { xdg_configure };

static void top_configure(void* d, struct xdg_toplevel* t, int32_t w, int32_t h, struct wl_array* states)
{
    wl_win* v = d;
    v->pending_w = w;
    v->pending_h = h;
}
static void top_close(void* d, struct xdg_toplevel* t)
{
    wl_win* v = d;
    gp_window_on_close(v->w);
    flush(v);
}
static void top_bounds(void* d, struct xdg_toplevel* t, int32_t w, int32_t h)
{
}
static void top_caps(void* d, struct xdg_toplevel* t, struct wl_array* caps)
{
}
static const struct xdg_toplevel_listener top_listener = { top_configure, top_close, top_bounds, top_caps };

static void frac_preferred(void* d, struct wp_fractional_scale_v1* f, uint32_t scale)
{
    wl_win* v = d;
    double s = scale / 120.0;
    if(s != v->scale) {
        v->scale = s;
        resized(v);
    }
}
static const struct wp_fractional_scale_v1_listener frac_listener = { frac_preferred };

static void surface_enter(void* d, struct wl_surface* s, struct wl_output* o)
{
    wl_win* v = d;
    if(v->noutputs < 8) {
        v->outputs[v->noutputs++] = o;
    }
}
static void surface_leave(void* d, struct wl_surface* s, struct wl_output* o)
{
    wl_win* v = d;
    for(int i = 0; i < v->noutputs; i++) {
        if(v->outputs[i] == o) {
            v->outputs[i] = v->outputs[--v->noutputs];
            break;
        }
    }
}
static void surface_buffer_scale(void* d, struct wl_surface* s, int32_t f)
{
    wl_win* v = d;
    if(!v->frac && f >= 1 && f != v->buffer_scale) { /* (fractional scaling, where there is some, says it better) */
        v->buffer_scale = f;
        v->scale = f;
        resized(v);
    }
}
static void surface_buffer_transform(void* d, struct wl_surface* s, uint32_t t)
{
}
static const struct wl_surface_listener surface_listener = { surface_enter, surface_leave, surface_buffer_scale, surface_buffer_transform };

/* ---------------------------------------------------------------- the pointer */

static int64_t now_ns(void)
{
    return gp_now_ns();
}

static void apply_cursor(void)
{
    if(!g.cursor_dev || !g.pointer_win) {
        return;
    }
    uint32_t shape = g.cursor == GP_CURSOR_POINTER ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER : g.cursor == GP_CURSOR_TEXT ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT
                                                                                                                          : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    wp_cursor_shape_device_v1_set_shape(g.cursor_dev, g.pointer_serial, shape);
}

static void pointer_enter(void* d, struct wl_pointer* p, uint32_t serial, struct wl_surface* s, wl_fixed_t x, wl_fixed_t y)
{
    g.pointer_win = s ? win_for_surface(s) : NULL;
    g.pointer_serial = serial;
    g.px = wl_fixed_to_double(x);
    g.py = wl_fixed_to_double(y);
    apply_cursor();
    if(g.pointer_win && !g_ignore_real_mouse) {
        gp_window_on_mouse_move(g.pointer_win->w, (float)g.px, (float)g.py, mods_now());
        flush(g.pointer_win);
    }
}
static void pointer_leave(void* d, struct wl_pointer* p, uint32_t serial, struct wl_surface* s)
{
    wl_win* v = g.pointer_win;
    g.pointer_win = NULL;
    if(v && !g_ignore_real_mouse) {
        gp_window_on_mouse_leave(v->w);
        flush(v);
    }
}
static void pointer_motion(void* d, struct wl_pointer* p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    g.px = wl_fixed_to_double(x);
    g.py = wl_fixed_to_double(y);
    if(g.pointer_win && !g_ignore_real_mouse) {
        gp_window_on_mouse_move(g.pointer_win->w, (float)g.px, (float)g.py, mods_now());
        flush(g.pointer_win);
    }
}
static void pointer_button(void* d, struct wl_pointer* p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    g.serial = serial;
    int b = button == BTN_LEFT ? GP_MOUSE_LEFT : button == BTN_MIDDLE ? GP_MOUSE_MIDDLE
                                             : button == BTN_RIGHT    ? GP_MOUSE_RIGHT
                                                                      : 0;
    bool down = state == WL_POINTER_BUTTON_STATE_PRESSED;
    if(down) { /* a run of clicks: the same button again within half a second and a few pixels (SDL's defaults) */
        int64_t t = now_ns();
        bool again = button == g.last_button && t - g.last_press_ns < 500000000 && fabs(g.px - g.last_press_x) <= 32 && fabs(g.py - g.last_press_y) <= 32;
        g.clicks = again ? g.clicks + 1 : 1;
        g.last_button = button;
        g.last_press_ns = t;
        g.last_press_x = g.px;
        g.last_press_y = g.py;
    }
    if(b && g.pointer_win && !g_ignore_real_mouse) {
        gp_window_on_mouse_button(g.pointer_win->w, (float)g.px, (float)g.py, b, down, g.clicks, mods_now());
        flush(g.pointer_win);
    }
}
static void pointer_axis(void* d, struct wl_pointer* p, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    g.axis_px[axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? 0 : 1] += wl_fixed_to_double(value);
    g.axis_any = true;
}
static void pointer_frame(void* d, struct wl_pointer* p)
{
    if(!g.axis_any) {
        return;
    }
    g.axis_any = false;
    /* A wheel says how far in 120ths of a notch; a touchpad only in surface units, where SDL takes 10 for a notch. Positive is down and right. */
    float nx = g.axis_v120[0] ? g.axis_v120[0] / 120.f : (float)(g.axis_px[0] / 10.0);
    float ny = g.axis_v120[1] ? g.axis_v120[1] / 120.f : (float)(g.axis_px[1] / 10.0);
    g.axis_v120[0] = g.axis_v120[1] = 0;
    g.axis_px[0] = g.axis_px[1] = 0;
    if(!g.pointer_win || g_ignore_real_mouse || (nx == 0 && ny == 0)) {
        return;
    }
    uint32_t mods = mods_now();
    if(nx == 0 && (mods & GP_MOD_SHIFT)) { /* shift + wheel scrolls sideways */
        nx = ny;
        ny = 0;
    }
    gp_window_on_scroll(g.pointer_win->w, (float)g.px, (float)g.py, nx * GP_SCROLL_LINE_PX, ny * GP_SCROLL_LINE_PX, mods);
    flush(g.pointer_win);
}
static void pointer_axis_source(void* d, struct wl_pointer* p, uint32_t source)
{
}
static void pointer_axis_stop(void* d, struct wl_pointer* p, uint32_t time, uint32_t axis)
{
}
static void pointer_axis_discrete(void* d, struct wl_pointer* p, uint32_t axis, int32_t discrete)
{
}
static void pointer_axis_value120(void* d, struct wl_pointer* p, uint32_t axis, int32_t v120)
{
    g.axis_v120[axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? 0 : 1] += v120;
    g.axis_any = true;
}
static void pointer_axis_relative(void* d, struct wl_pointer* p, uint32_t axis, uint32_t dir)
{
}
/* (named: libwayland adds events at the end, and the seat is bound at version 8, so the later ones are not sent) */
static const struct wl_pointer_listener pointer_listener = { .enter = pointer_enter, .leave = pointer_leave, .motion = pointer_motion, .button = pointer_button, .axis = pointer_axis, .frame = pointer_frame, .axis_source = pointer_axis_source, .axis_stop = pointer_axis_stop, .axis_discrete = pointer_axis_discrete, .axis_value120 = pointer_axis_value120, .axis_relative_direction = pointer_axis_relative };

/* ---------------------------------------------------------------- the keyboard */

static const struct
{
    xkb_keysym_t sym;
    const char* name;
} named[] = {
    {       XKB_KEY_Return,     "enter" },
    {     XKB_KEY_KP_Enter,     "enter" },
    {       XKB_KEY_Escape,    "escape" },
    {          XKB_KEY_Tab,       "tab" },
    { XKB_KEY_ISO_Left_Tab,       "tab" },
    {    XKB_KEY_BackSpace, "backspace" },
    {       XKB_KEY_Delete,    "delete" },
    {        XKB_KEY_space,     "space" },
    {         XKB_KEY_Left,      "left" },
    {        XKB_KEY_Right,     "right" },
    {           XKB_KEY_Up,        "up" },
    {         XKB_KEY_Down,      "down" },
    {         XKB_KEY_Home,      "home" },
    {          XKB_KEY_End,       "end" },
    {        XKB_KEY_Prior,    "pageup" },
    {         XKB_KEY_Next,  "pagedown" },
    {       XKB_KEY_Insert,    "insert" },
    {           XKB_KEY_F1,        "f1" },
    {           XKB_KEY_F2,        "f2" },
    {           XKB_KEY_F3,        "f3" },
    {           XKB_KEY_F4,        "f4" },
    {           XKB_KEY_F5,        "f5" },
    {           XKB_KEY_F6,        "f6" },
    {           XKB_KEY_F7,        "f7" },
    {           XKB_KEY_F8,        "f8" },
    {           XKB_KEY_F9,        "f9" },
    {          XKB_KEY_F10,       "f10" },
    {          XKB_KEY_F11,       "f11" },
    {          XKB_KEY_F12,       "f12" },
};

/* the key's canonical name (unshifted, lowercase), as platform_sdl.c's key_name gives it; NULL for keys not named */
static const char* key_name(uint32_t key, char buf[8])
{
    if(!g.keymap || !g.xkb_state) {
        return NULL;
    }
    const xkb_keysym_t* syms;
    xkb_layout_index_t layout = xkb_state_key_get_layout(g.xkb_state, key);
    int n = xkb_keymap_key_get_syms_by_level(g.keymap, key, layout == XKB_LAYOUT_INVALID ? 0 : layout, 0, &syms);
    if(n < 1) {
        return NULL;
    }
    xkb_keysym_t s = syms[0];
    for(size_t i = 0; i < sizeof named / sizeof *named; i++) {
        if(named[i].sym == s) {
            return named[i].name;
        }
    }
    if(s > 0x20 && s < 0x7f) {
        buf[0] = (char)(s >= 'A' && s <= 'Z' ? s + 32 : s);
        buf[1] = 0;
        return buf;
    }
    return NULL;
}

/* a key pressed (or repeating): its name to the window, and the text it types where the window takes text */
static void key_press(wl_win* v, uint32_t key, bool repeat)
{
    char buf[8];
    const char* name = key_name(key, buf);
    uint32_t mods = mods_now();
    if(name) {
        gp_window_on_key(v->w, name, mods, repeat);
        flush(v);
    }
    if(v->text_input && g.xkb_state && !(mods & (GP_MOD_CTRL | GP_MOD_ALT | GP_MOD_SUPER))) {
        char text[64];
        int n = xkb_state_key_get_utf8(g.xkb_state, key, text, sizeof text);
        if(n > 0 && (unsigned char)text[0] >= 0x20 && text[0] != 0x7f) {
            gp_window_on_text(v->w, text);
            flush(v);
        }
    }
}

static void keyboard_keymap(void* d, struct wl_keyboard* k, uint32_t format, int32_t fd, uint32_t size)
{
    if(format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
    char* map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if(map == MAP_FAILED) {
        return;
    }
    struct xkb_keymap* km = xkb_keymap_new_from_string(g.xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if(!km) {
        return;
    }
    if(g.xkb_state) {
        xkb_state_unref(g.xkb_state);
    }
    if(g.keymap) {
        xkb_keymap_unref(g.keymap);
    }
    g.keymap = km;
    g.xkb_state = xkb_state_new(km);
}
static void keyboard_enter(void* d, struct wl_keyboard* k, uint32_t serial, struct wl_surface* s, struct wl_array* keys)
{
    g.serial = serial;
    g.key_win = s ? win_for_surface(s) : NULL;
}
static void keyboard_leave(void* d, struct wl_keyboard* k, uint32_t serial, struct wl_surface* s)
{
    g.key_win = NULL;
    g.repeat_key = 0;
}
static void keyboard_key(void* d, struct wl_keyboard* k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    g.serial = serial;
    uint32_t code = key + 8; /* (evdev to xkb) */
    wl_win* v = g.key_win;
    if(state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        if(g.keymap && xkb_keymap_key_repeats(g.keymap, code) && g.repeat_rate > 0) {
            g.repeat_key = code;
            g.repeat_at = now_ns() + (int64_t)g.repeat_delay * 1000000;
        }
        if(v && !g_ignore_real_keys) {
            key_press(v, code, false);
        }
    } else {
        if(g.repeat_key == code) {
            g.repeat_key = 0;
        }
        char buf[8];
        const char* name = key_name(code, buf);
        if(v && name && !g_ignore_real_keys) {
            gp_window_on_key_up(v->w, name, mods_now());
            flush(v);
        }
    }
}
static void keyboard_modifiers(void* d, struct wl_keyboard* k, uint32_t serial, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group)
{
    if(g.xkb_state) {
        xkb_state_update_mask(g.xkb_state, dep, lat, lock, 0, 0, group);
    }
}
static void keyboard_repeat(void* d, struct wl_keyboard* k, int32_t rate, int32_t delay)
{
    g.repeat_rate = rate;
    g.repeat_delay = delay;
}
static const struct wl_keyboard_listener keyboard_listener = { keyboard_keymap, keyboard_enter, keyboard_leave, keyboard_key, keyboard_modifiers, keyboard_repeat };

/* ---------------------------------------------------------------- touch (the first finger) */

static void touch_down(void* d, struct wl_touch* t, uint32_t serial, uint32_t time, struct wl_surface* s, int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    g.serial = serial;
    wl_win* v = s ? win_for_surface(s) : NULL;
    if(!v || g.touch_id >= 0) {
        return;
    }
    g.touch_id = id;
    g.touch_win = v;
    g.tx = wl_fixed_to_double(x);
    g.ty = wl_fixed_to_double(y);
    gp_scroll_touch(v->w, GP_TOUCH_DOWN, (float)g.tx, (float)g.ty, (int64_t)time * 1000000);
    flush(v);
}
static void touch_up(void* d, struct wl_touch* t, uint32_t serial, uint32_t time, int32_t id)
{
    wl_win* v = g.touch_win;
    if(id != g.touch_id || !v) {
        return;
    }
    g.touch_id = -1;
    g.touch_win = NULL;
    gp_scroll_touch(v->w, GP_TOUCH_UP, (float)g.tx, (float)g.ty, (int64_t)time * 1000000);
    flush(v);
}
static void touch_motion(void* d, struct wl_touch* t, uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    wl_win* v = g.touch_win;
    if(id != g.touch_id || !v) {
        return;
    }
    g.tx = wl_fixed_to_double(x);
    g.ty = wl_fixed_to_double(y);
    gp_scroll_touch(v->w, GP_TOUCH_MOVE, (float)g.tx, (float)g.ty, (int64_t)time * 1000000);
    flush(v);
}
static void touch_frame(void* d, struct wl_touch* t)
{
}
static void touch_cancel(void* d, struct wl_touch* t)
{
    g.touch_id = -1;
    g.touch_win = NULL;
}
static void touch_shape(void* d, struct wl_touch* t, int32_t id, wl_fixed_t major, wl_fixed_t minor)
{
}
static void touch_orientation(void* d, struct wl_touch* t, int32_t id, wl_fixed_t o)
{
}
static const struct wl_touch_listener touch_listener = { touch_down, touch_up, touch_motion, touch_frame, touch_cancel, touch_shape, touch_orientation };

/* ---------------------------------------------------------------- text input (IME) */

static void ti_send_state(void)
{
    wl_win* v = g.ti_win;
    if(!g.ti || !v) {
        return;
    }
    if(v->text_input) {
        zwp_text_input_v3_enable(g.ti);
    } else {
        zwp_text_input_v3_disable(g.ti);
    }
    zwp_text_input_v3_commit(g.ti);
}
static void ti_enter(void* d, struct zwp_text_input_v3* t, struct wl_surface* s)
{
    g.ti_win = s ? win_for_surface(s) : NULL;
    ti_send_state();
}
static void ti_leave(void* d, struct zwp_text_input_v3* t, struct wl_surface* s)
{
    if(g.ti) {
        zwp_text_input_v3_disable(g.ti);
        zwp_text_input_v3_commit(g.ti);
    }
    g.ti_win = NULL;
}
static void ti_preedit(void* d, struct zwp_text_input_v3* t, const char* text, int32_t begin, int32_t end)
{
    free(g.ti_preedit);
    g.ti_preedit = text ? strdup(text) : NULL;
    int chars = 0; /* (the cursor, in characters, as SDL gives it: the compositor's is in bytes) */
    for(int i = 0; text && text[i] && i < begin; i++) {
        chars += ((unsigned char)text[i] & 0xc0) != 0x80;
    }
    g.ti_cursor = begin < 0 ? -1 : chars;
}
static void ti_commit(void* d, struct zwp_text_input_v3* t, const char* text)
{
    free(g.ti_commit);
    g.ti_commit = text ? strdup(text) : NULL;
}
static void ti_delete(void* d, struct zwp_text_input_v3* t, uint32_t before, uint32_t after)
{
}
static void ti_done(void* d, struct zwp_text_input_v3* t, uint32_t serial)
{
    wl_win* v = g.ti_win;
    if(v && g.ti_commit && !g_ignore_real_keys) {
        gp_window_on_text(v->w, g.ti_commit);
        flush(v);
    }
    if(v && !v->w->app->debug_ignore_platform_ime && !g_ignore_real_keys) {
        gp_window_on_ime(v->w, g.ti_preedit ? g.ti_preedit : "", g.ti_cursor);
        flush(v);
    }
    free(g.ti_commit);
    free(g.ti_preedit);
    g.ti_commit = g.ti_preedit = NULL;
}
static const struct zwp_text_input_v3_listener ti_listener = { ti_enter, ti_leave, ti_preedit, ti_commit, ti_delete, ti_done };

/* ---------------------------------------------------------------- selections */

static bool text_mime(const char* m)
{
    return strcmp(m, "text/plain;charset=utf-8") == 0 || strcmp(m, "UTF8_STRING") == 0 || strcmp(m, "text/plain") == 0;
}

static void offer_offer(void* d, struct wl_data_offer* o, const char* mime)
{
    *(bool*)d |= text_mime(mime);
}

static void offer_free(struct wl_data_offer* o)
{
    if(o) {
        free(wl_data_offer_get_user_data(o));
        wl_data_offer_destroy(o);
    }
}
static void offer_actions(void* d, struct wl_data_offer* o, uint32_t a)
{
}
static void offer_action(void* d, struct wl_data_offer* o, uint32_t a)
{
}
static const struct wl_data_offer_listener offer_listener = { offer_offer, offer_actions, offer_action };

static void dd_offer(void* d, struct wl_data_device* dd, struct wl_data_offer* o)
{
    wl_data_offer_add_listener(o, &offer_listener, calloc(1, sizeof(bool))); /* (its mime types come before the selection or the drag that names it) */
}
static void dd_enter(void* d, struct wl_data_device* dd, uint32_t serial, struct wl_surface* s, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer* o)
{
    if(g.dnd_offer != o) {
        offer_free(g.dnd_offer);
    }
    g.dnd_offer = o;
}
static void dd_leave(void* d, struct wl_data_device* dd)
{
    offer_free(g.dnd_offer);
    g.dnd_offer = NULL;
}
static void dd_motion(void* d, struct wl_data_device* dd, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{
}
static void dd_drop(void* d, struct wl_data_device* dd)
{
    offer_free(g.dnd_offer); /* (nothing is dropped on gpuic) */
    g.dnd_offer = NULL;
}
static void dd_selection(void* d, struct wl_data_device* dd, struct wl_data_offer* o)
{
    if(g.clip_offer != o) {
        offer_free(g.clip_offer);
    }
    g.clip_offer = o;
}
static const struct wl_data_device_listener dd_listener = { dd_offer, dd_enter, dd_leave, dd_motion, dd_drop, dd_selection };

static void prim_offer_offer(void* d, struct zwp_primary_selection_offer_v1* o, const char* mime)
{
    g.prim_offer_text |= text_mime(mime);
}
static const struct zwp_primary_selection_offer_v1_listener prim_offer_listener = { prim_offer_offer };
static void prim_dev_offer(void* d, struct zwp_primary_selection_device_v1* dev, struct zwp_primary_selection_offer_v1* o)
{
    zwp_primary_selection_offer_v1_add_listener(o, &prim_offer_listener, NULL);
    g.prim_offer_text = false;
}
static void prim_dev_selection(void* d, struct zwp_primary_selection_device_v1* dev, struct zwp_primary_selection_offer_v1* o)
{
    if(g.prim_offer && g.prim_offer != o) {
        zwp_primary_selection_offer_v1_destroy(g.prim_offer);
    }
    g.prim_offer = o;
}
static const struct zwp_primary_selection_device_v1_listener prim_dev_listener = { prim_dev_offer, prim_dev_selection };

/* all of a pipe's contents (the other client writes and closes it), giving up after a second of silence */
static char* read_pipe(int fd)
{
    size_t len = 0, cap = 4096;
    char* buf = malloc(cap);
    for(;;) {
        struct pollfd p = { fd, POLLIN, 0 };
        if(poll(&p, 1, 1000) <= 0) {
            break;
        }
        if(len + 1 >= cap) {
            char* bigger = realloc(buf, cap *= 2);
            if(!bigger) {
                break;
            }
            buf = bigger;
        }
        ssize_t n = read(fd, buf + len, cap - len - 1);
        if(n <= 0) {
            break;
        }
        len += (size_t)n;
    }
    close(fd);
    buf[len] = 0;
    if(!len) {
        free(buf);
        return NULL;
    }
    return buf;
}

/* The text into the pipe another client reads, without blocking: a reader that is itself waiting on us would hang both, so a second without room
 * gives up (a paste of a lot of text into a client that is slow to read can come out short) */
static void write_all(int fd, const char* text)
{
    size_t len = text ? strlen(text) : 0, done = 0;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    while(done < len) {
        ssize_t n = write(fd, text + done, len - done);
        if(n > 0) {
            done += (size_t)n;
        } else if(n < 0 && errno == EAGAIN) {
            struct pollfd p = { fd, POLLOUT, 0 };
            if(poll(&p, 1, 1000) <= 0) {
                break;
            }
        } else if(!(n < 0 && errno == EINTR)) {
            break;
        }
    }
    close(fd);
}

static void source_target(void* d, struct wl_data_source* s, const char* m)
{
}
static void source_send(void* d, struct wl_data_source* s, const char* mime, int32_t fd)
{
    write_all(fd, g.clip_text);
}
static void source_cancelled(void* d, struct wl_data_source* s)
{
    if(s == g.clip_source) {
        g.clip_source = NULL;
        free(g.clip_text);
        g.clip_text = NULL;
    }
    wl_data_source_destroy(s);
}
static void source_dnd_drop(void* d, struct wl_data_source* s)
{
}
static void source_dnd_finished(void* d, struct wl_data_source* s)
{
}
static void source_action(void* d, struct wl_data_source* s, uint32_t a)
{
}
static const struct wl_data_source_listener source_listener = { source_target, source_send, source_cancelled, source_dnd_drop, source_dnd_finished, source_action };

static void prim_source_send(void* d, struct zwp_primary_selection_source_v1* s, const char* mime, int32_t fd)
{
    write_all(fd, g.prim_text);
}
static void prim_source_cancelled(void* d, struct zwp_primary_selection_source_v1* s)
{
    if(s == g.prim_source) {
        g.prim_source = NULL;
        free(g.prim_text);
        g.prim_text = NULL;
    }
    zwp_primary_selection_source_v1_destroy(s);
}
static const struct zwp_primary_selection_source_v1_listener prim_source_listener = { prim_source_send, prim_source_cancelled };

char* gp_platform_clipboard_get(void)
{
    if(g.clip_source) { /* (ours: reading it through the compositor would wait on ourselves) */
        return g.clip_text && *g.clip_text ? strdup(g.clip_text) : NULL;
    }
    if(!g.clip_offer || !*(bool*)wl_data_offer_get_user_data(g.clip_offer)) {
        return NULL;
    }
    int fds[2];
    if(pipe2(fds, O_CLOEXEC) != 0) {
        return NULL;
    }
    wl_data_offer_receive(g.clip_offer, "text/plain;charset=utf-8", fds[1]);
    close(fds[1]);
    wl_display_flush(g.display);
    return read_pipe(fds[0]);
}

void gp_platform_clipboard_set(const char* utf8)
{
    if(!g.ddm || !g.data_device || !g.serial) { /* (no input yet: the compositor would ignore it, and we would think it ours; the app keeps its copy) */
        return;
    }
    free(g.clip_text);
    g.clip_text = strdup(utf8 ? utf8 : "");
    struct wl_data_source* s = wl_data_device_manager_create_data_source(g.ddm);
    wl_data_source_add_listener(s, &source_listener, NULL);
    wl_data_source_offer(s, "text/plain;charset=utf-8");
    wl_data_source_offer(s, "text/plain");
    wl_data_source_offer(s, "UTF8_STRING");
    wl_data_device_set_selection(g.data_device, s, g.serial);
    g.clip_source = s; /* (the old one, if any, is cancelled by the compositor) */
    wl_display_flush(g.display);
}

char* gp_platform_primary_get(void)
{
    if(g.prim_source) {
        return g.prim_text && *g.prim_text ? strdup(g.prim_text) : NULL;
    }
    if(!g.prim_offer || !g.prim_offer_text) {
        return NULL;
    }
    int fds[2];
    if(pipe2(fds, O_CLOEXEC) != 0) {
        return NULL;
    }
    zwp_primary_selection_offer_v1_receive(g.prim_offer, "text/plain;charset=utf-8", fds[1]);
    close(fds[1]);
    wl_display_flush(g.display);
    return read_pipe(fds[0]);
}

bool gp_platform_primary_set(const char* utf8)
{
    if(!g.psm || !g.primary_device || !g.serial) {
        return false;
    }
    free(g.prim_text);
    g.prim_text = strdup(utf8 ? utf8 : "");
    struct zwp_primary_selection_source_v1* s = zwp_primary_selection_device_manager_v1_create_source(g.psm);
    zwp_primary_selection_source_v1_add_listener(s, &prim_source_listener, NULL);
    zwp_primary_selection_source_v1_offer(s, "text/plain;charset=utf-8");
    zwp_primary_selection_source_v1_offer(s, "text/plain");
    zwp_primary_selection_source_v1_offer(s, "UTF8_STRING");
    zwp_primary_selection_device_v1_set_selection(g.primary_device, s, g.serial);
    g.prim_source = s;
    wl_display_flush(g.display);
    return true;
}

/* ---------------------------------------------------------------- the seat and the globals */

static void seat_caps(void* d, struct wl_seat* seat, uint32_t caps)
{
    if((caps & WL_SEAT_CAPABILITY_POINTER) && !g.pointer) {
        g.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(g.pointer, &pointer_listener, NULL);
        if(g.cursor_mgr) {
            g.cursor_dev = wp_cursor_shape_manager_v1_get_pointer(g.cursor_mgr, g.pointer);
        }
    }
    if((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !g.keyboard) {
        g.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(g.keyboard, &keyboard_listener, NULL);
    }
    if((caps & WL_SEAT_CAPABILITY_TOUCH) && !g.touch) {
        g.touch = wl_seat_get_touch(seat);
        wl_touch_add_listener(g.touch, &touch_listener, NULL);
    }
}
static void seat_name(void* d, struct wl_seat* seat, const char* name)
{
}
static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

static uint32_t min_u(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

static void registry_global(void* d, struct wl_registry* r, uint32_t name, const char* iface, uint32_t version)
{
    if(strcmp(iface, wl_compositor_interface.name) == 0) {
        g.compositor_version = min_u(version, 6);
        g.compositor = wl_registry_bind(r, name, &wl_compositor_interface, g.compositor_version);
    } else if(strcmp(iface, xdg_wm_base_interface.name) == 0) {
        g.wm = wl_registry_bind(r, name, &xdg_wm_base_interface, min_u(version, 5));
        xdg_wm_base_add_listener(g.wm, &wm_listener, NULL);
    } else if(strcmp(iface, wl_seat_interface.name) == 0 && !g.seat) {
        g.seat = wl_registry_bind(r, name, &wl_seat_interface, min_u(version, 8));
        wl_seat_add_listener(g.seat, &seat_listener, NULL);
    } else if(strcmp(iface, wl_output_interface.name) == 0 && g.noutputs < 8) {
        wl_out* o = &g.outputs[g.noutputs++];
        o->output = wl_registry_bind(r, name, &wl_output_interface, min_u(version, 4));
        wl_output_add_listener(o->output, &output_listener, o);
    } else if(strcmp(iface, wl_data_device_manager_interface.name) == 0) {
        g.ddm = wl_registry_bind(r, name, &wl_data_device_manager_interface, min_u(version, 3));
    } else if(strcmp(iface, zwp_primary_selection_device_manager_v1_interface.name) == 0) {
        g.psm = wl_registry_bind(r, name, &zwp_primary_selection_device_manager_v1_interface, 1);
    } else if(strcmp(iface, zwp_text_input_manager_v3_interface.name) == 0) {
        g.tim = wl_registry_bind(r, name, &zwp_text_input_manager_v3_interface, 1);
    } else if(strcmp(iface, zxdg_decoration_manager_v1_interface.name) == 0) {
        g.deco = wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if(strcmp(iface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        g.frac = wl_registry_bind(r, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else if(strcmp(iface, wp_viewporter_interface.name) == 0) {
        g.viewporter = wl_registry_bind(r, name, &wp_viewporter_interface, 1);
    } else if(strcmp(iface, wp_cursor_shape_manager_v1_interface.name) == 0) {
        g.cursor_mgr = wl_registry_bind(r, name, &wp_cursor_shape_manager_v1_interface, 1);
    }
}
static void registry_remove(void* d, struct wl_registry* r, uint32_t name)
{
}
static const struct wl_registry_listener registry_listener = { registry_global, registry_remove };

static bool connect_display(void)
{
    g.display = wl_display_connect(NULL);
    if(!g.display) {
        fprintf(stderr, "gpuic: no Wayland display (WAYLAND_DISPLAY)\n");
        return false;
    }
    g.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    g.registry = wl_display_get_registry(g.display);
    wl_registry_add_listener(g.registry, &registry_listener, NULL);
    wl_display_roundtrip(g.display); /* the globals */
    if(!g.compositor || !g.wm) {
        fprintf(stderr, "gpuic: the Wayland compositor has no xdg-shell\n");
        wl_display_disconnect(g.display);
        g.display = NULL;
        return false;
    }
    if(g.seat && g.ddm) {
        g.data_device = wl_data_device_manager_get_data_device(g.ddm, g.seat);
        wl_data_device_add_listener(g.data_device, &dd_listener, NULL);
    }
    if(g.seat && g.psm) {
        g.primary_device = zwp_primary_selection_device_manager_v1_get_device(g.psm, g.seat);
        zwp_primary_selection_device_v1_add_listener(g.primary_device, &prim_dev_listener, NULL);
    }
    if(g.seat && g.tim) {
        g.ti = zwp_text_input_manager_v3_get_text_input(g.tim, g.seat);
        zwp_text_input_v3_add_listener(g.ti, &ti_listener, NULL);
    }
    wl_display_roundtrip(g.display); /* the seat's devices, the outputs' modes, the keymap */
    g.wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    g.next_id = 1;
    return true;
}

static void disconnect_display(void)
{
    if(g.egl_dpy) {
        eglMakeCurrent(g.egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if(g.egl_ctx) {
            eglDestroyContext(g.egl_dpy, g.egl_ctx);
        }
        eglTerminate(g.egl_dpy);
    }
    free(g.clip_text);
    free(g.prim_text);
    free(g.ti_preedit);
    free(g.ti_commit);
    if(g.xkb_state) {
        xkb_state_unref(g.xkb_state);
    }
    if(g.keymap) {
        xkb_keymap_unref(g.keymap);
    }
    if(g.xkb) {
        xkb_context_unref(g.xkb);
    }
    if(g.wake_fd >= 0) {
        close(g.wake_fd);
    }
    if(g.display) {
        wl_display_disconnect(g.display); /* (the proxies go with it) */
    }
    if(g.vk_lib) {
        dlclose(g.vk_lib);
    }
    memset(&g, 0, sizeof g);
    g.wake_fd = -1;
    g.touch_id = -1;
}

/* ---------------------------------------------------------------- the platform */

bool gp_platform_init(gp_app* app)
{
    if(!g.users && !connect_display()) {
        return false;
    }
    g.users++;
    g_ignore_real_mouse = getenv("GPUIC_IGNORE_REAL_MOUSE") != NULL;
    g_ignore_real_keys = getenv("GPUIC_IGNORE_REAL_KEYS") != NULL;
    wl_platform* p = calloc(1, sizeof *p);
    const char* renderer = getenv("GPUIC_RENDERER");
    if(renderer && strcmp(renderer, "vulkan") == 0) {
        if(!g.vk_lib) {
            g.vk_lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        }
        p->vulkan = g.vk_lib != NULL;
        if(!p->vulkan) {
            fprintf(stderr, "gpuic: GPUIC_RENDERER=vulkan, but the Vulkan loader was not found: drawing with OpenGL\n");
        }
    }
    app->platform = p;
    return true;
}

void gp_platform_shutdown(gp_app* app)
{
    free(app->platform);
    app->platform = NULL;
    if(--g.users == 0) {
        disconnect_display();
    }
}

static bool egl_start(void)
{
    if(g.egl_ctx) {
        return true;
    }
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    g.egl_dpy = get_display ? get_display(EGL_PLATFORM_WAYLAND_KHR, g.display, NULL) : eglGetDisplay((EGLNativeDisplayType)g.display);
    if(!g.egl_dpy || !eglInitialize(g.egl_dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_API)) {
        fprintf(stderr, "gpuic: EGL could not start on the Wayland display\n");
        return false;
    }
    const EGLint cfg[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
    EGLint n = 0;
    if(!eglChooseConfig(g.egl_dpy, cfg, &g.egl_cfg, 1, &n) || n < 1) {
        fprintf(stderr, "gpuic: no EGL configuration for an 8-bit RGB window\n");
        return false;
    }
    const EGLint ctx[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
    g.egl_ctx = eglCreateContext(g.egl_dpy, g.egl_cfg, EGL_NO_CONTEXT, ctx);
    if(!g.egl_ctx) {
        fprintf(stderr, "gpuic: could not create an OpenGL 3.3 context\n");
        return false;
    }
    return true;
}

bool gp_platform_open_window(gp_app* app, gp_window* w, const gp_window_options* opts)
{
    wl_platform* p = app->platform;
    if(opts->parent) {
        fprintf(stderr, "gpuic: a window inside another (gp_window_options.parent) is not supported on Wayland (build with the SDL platform, on X11)\n");
        return false;
    }
    if(!p->vulkan && !egl_start()) {
        return false;
    }
    wl_win* v = calloc(1, sizeof *v);
    v->w = w;
    v->lw = opts->width > 0 ? opts->width : 960;
    v->lh = opts->height > 0 ? opts->height : 640;
    v->scale = 1.0;
    v->buffer_scale = 1;
    v->surface = wl_compositor_create_surface(g.compositor);
    wl_surface_add_listener(v->surface, &surface_listener, v);
    if(g.frac && g.viewporter) {
        v->frac = wp_fractional_scale_manager_v1_get_fractional_scale(g.frac, v->surface);
        wp_fractional_scale_v1_add_listener(v->frac, &frac_listener, v);
        v->viewport = wp_viewporter_get_viewport(g.viewporter, v->surface);
    }
    v->xdg = xdg_wm_base_get_xdg_surface(g.wm, v->surface);
    xdg_surface_add_listener(v->xdg, &xdg_surface_listener, v);
    v->top = xdg_surface_get_toplevel(v->xdg);
    xdg_toplevel_add_listener(v->top, &top_listener, v);
    v->title = strdup(opts->title ? opts->title : "gpuic");
    xdg_toplevel_set_title(v->top, v->title);
    xdg_toplevel_set_app_id(v->top, "gpuic");
    if(g.deco) {
        v->deco = zxdg_decoration_manager_v1_get_toplevel_decoration(g.deco, v->top);
        zxdg_toplevel_decoration_v1_set_mode(v->deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    wl_surface_commit(v->surface);
    v->next = g.windows;
    g.windows = v;
    w->platform = v;
    w->id = g.next_id++;
    for(int i = 0; i < 100 && !v->configured; i++) { /* the first configure: then the window may be drawn */
        wl_display_roundtrip(g.display);
    }
    if(!p->vulkan) {
        v->egl_win = wl_egl_window_create(v->surface, (int)lround(v->lw * v->scale), (int)lround(v->lh * v->scale));
        v->egl = eglCreateWindowSurface(g.egl_dpy, g.egl_cfg, (EGLNativeWindowType)v->egl_win, NULL);
        if(v->egl == EGL_NO_SURFACE) {
            fprintf(stderr, "gpuic: no EGL surface for the window\n");
            gp_platform_close_window(app, w);
            return false;
        }
        eglMakeCurrent(g.egl_dpy, v->egl, v->egl, g.egl_ctx);
        eglSwapInterval(g.egl_dpy, 0); /* (the app paces its frames: a window the compositor is not showing is not held to its frame callbacks) */
    }
    apply_size(v);
    v->app = app;                      /* (from here on its events go to the app) */
    gp_platform_update_size(w);
    return true;
}

void gp_platform_close_window(gp_app* app, gp_window* w)
{
    wl_win* v = win_of(w);
    if(!v) {
        return;
    }
    for(wl_win** pp = &g.windows; *pp; pp = &(*pp)->next) {
        if(*pp == v) {
            *pp = v->next;
            break;
        }
    }
    if(g.pointer_win == v) {
        g.pointer_win = NULL;
    }
    if(g.key_win == v) {
        g.key_win = NULL;
        g.repeat_key = 0;
    }
    if(g.ti_win == v) {
        g.ti_win = NULL;
    }
    if(g.touch_win == v) {
        g.touch_win = NULL;
        g.touch_id = -1;
    }
    if(v->egl) {
        if(eglGetCurrentSurface(EGL_DRAW) == v->egl) {
            eglMakeCurrent(g.egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
        eglDestroySurface(g.egl_dpy, v->egl);
    }
    if(v->egl_win) {
        wl_egl_window_destroy(v->egl_win);
    }
    if(v->deco) {
        zxdg_toplevel_decoration_v1_destroy(v->deco);
    }
    if(v->viewport) {
        wp_viewport_destroy(v->viewport);
    }
    if(v->frac) {
        wp_fractional_scale_v1_destroy(v->frac);
    }
    xdg_toplevel_destroy(v->top);
    xdg_surface_destroy(v->xdg);
    wl_surface_destroy(v->surface);
    wl_display_flush(g.display);
    free(v->title);
    free(v);
    w->platform = NULL;
}

void gp_platform_update_size(gp_window* w)
{
    wl_win* v = win_of(w);
    int pw = (int)lround(v->lw * v->scale), ph = (int)lround(v->lh * v->scale);
    w->size = (gp_size){ (float)v->lw, (float)v->lh };
    w->px_w = pw < 1 ? 1 : pw;
    w->px_h = ph < 1 ? 1 : ph;
    w->scale = v->lw > 0 ? (float)w->px_w / (float)v->lw : 1.f;
}

void gp_platform_set_size(gp_window* w, int width, int height)
{
    wl_win* v = win_of(w);
    v->lw = width < 1 ? 1 : width;
    v->lh = height < 1 ? 1 : height;
    xdg_surface_set_window_geometry(v->xdg, 0, 0, v->lw, v->lh); /* (what the window is now: the compositor may ask for another size later) */
    apply_size(v);
    gp_window_on_resize(w);
}

void gp_platform_set_title(gp_window* w, const char* title)
{
    wl_win* v = win_of(w);
    char* t = strdup(title);
    if(!t) {
        return; /* (it keeps the one it has) */
    }
    free(v->title);
    v->title = t;
    xdg_toplevel_set_title(v->top, t);
    wl_display_flush(g.display);
}

const char* gp_platform_title(gp_window* w)
{
    return win_of(w)->title;
}

void gp_platform_make_current(gp_window* w)
{
    wl_win* v = win_of(w);
    if(v && v->egl) {
        eglMakeCurrent(g.egl_dpy, v->egl, v->egl, g.egl_ctx);
    }
}

void gp_platform_swap(gp_window* w)
{
    wl_win* v = win_of(w);
    if(v && v->egl) {
        eglSwapBuffers(g.egl_dpy, v->egl);
    }
}

void* gp_platform_gl_proc(const char* name)
{
    return (void*)eglGetProcAddress(name);
}

void* gp_platform_native(gp_window* w)
{
    return NULL;
}

void gp_platform_set_cursor(gp_window* w, int cursor)
{
    if(cursor != g.cursor) {
        g.cursor = cursor;
        apply_cursor();
    }
}

void gp_platform_set_text_input(gp_window* w, bool on)
{
    wl_win* v = win_of(w);
    if(v->text_input != on) {
        v->text_input = on;
        if(g.ti_win == v) {
            ti_send_state();
        }
    }
}

void gp_platform_set_text_input_area(gp_window* w, float x, float y, float width, float height, float cursor_x)
{
    wl_win* v = win_of(w);
    if(g.ti && g.ti_win == v && v->text_input) {
        zwp_text_input_v3_set_cursor_rectangle(g.ti, (int32_t)(x + cursor_x), (int32_t)y, 1, (int32_t)height);
        zwp_text_input_v3_commit(g.ti);
    }
}

float gp_platform_refresh_hz(gp_window* w)
{
    wl_win* v = win_of(w);
    int best = 0;
    for(int i = 0; v && i < v->noutputs; i++) {
        for(int k = 0; k < g.noutputs; k++) {
            if(g.outputs[k].output == v->outputs[i] && g.outputs[k].refresh_mhz > best) {
                best = g.outputs[k].refresh_mhz;
            }
        }
    }
    if(!best) { /* (not on an output yet: the fastest there is) */
        for(int k = 0; k < g.noutputs; k++) {
            best = g.outputs[k].refresh_mhz > best ? g.outputs[k].refresh_mhz : best;
        }
    }
    return best > 0 ? best / 1000.f : 60.f;
}

/* Thread-safe: an eventfd the pump polls. */
void gp_platform_wake(gp_app* app)
{
    if(g.wake_fd >= 0) {
        uint64_t one = 1;
        ssize_t n = write(g.wake_fd, &one, sizeof one);
        (void)n;
    }
}

/* ---------------------------------------------------------------- Vulkan */

bool gp_platform_vulkan(gp_app* app)
{
    return ((wl_platform*)app->platform)->vulkan;
}

void gp_platform_drop_vulkan(gp_app* app)
{
    ((wl_platform*)app->platform)->vulkan = false;
}

void* gp_platform_vk_loader(void)
{
    return g.vk_lib ? dlsym(g.vk_lib, "vkGetInstanceProcAddr") : NULL;
}

const char* const* gp_platform_vk_extensions(uint32_t* count)
{
    static const char* const names[] = { "VK_KHR_surface", "VK_KHR_wayland_surface" };
    *count = 2;
    return names;
}

/* (the few Vulkan types this needs, so that the platform does not need Vulkan's headers) */
typedef struct
{
    int sType; /* VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR */
    const void* pNext;
    uint32_t flags;
    struct wl_display* display;
    struct wl_surface* surface;
} vk_wayland_surface_info;
typedef int (*vk_create_wayland_surface)(void* instance, const vk_wayland_surface_info* info, const void* allocator, uint64_t* surface);

uint64_t gp_platform_vk_surface(gp_window* w, void* instance)
{
    void* (*get_proc)(void*, const char*) = (void* (*)(void*, const char*))gp_platform_vk_loader();
    vk_create_wayland_surface create = get_proc ? (vk_create_wayland_surface)get_proc(instance, "vkCreateWaylandSurfaceKHR") : NULL;
    vk_wayland_surface_info info = { 1000006000, NULL, 0, g.display, win_of(w)->surface };
    uint64_t surface = 0;
    if(!create || create(instance, &info, NULL, &surface) != 0) {
        fprintf(stderr, "gpuic: no Vulkan surface for the window\n");
        return 0;
    }
    return surface;
}

/* ---------------------------------------------------------------- what the tests inject */

enum { IN_KEY,
    IN_KEY_UP,
    IN_TEXT,
    IN_MOVE,
    IN_BUTTON,
    IN_LEAVE,
    IN_WHEEL,
};
typedef struct
{
    int type;
    gp_app* app;
    uint32_t window;
    char text[256]; /* a key's name, or text */
    float x, y, dx, dy;
    int button, clicks;
    bool down;
    uint32_t mods;
} injected;
static injected* g_queue;
static size_t g_qlen, g_qcap, g_qhead;

static void inject(injected e)
{
    if(g_qlen == g_qcap) {
        g_qcap = g_qcap ? g_qcap * 2 : 64;
        g_queue = realloc(g_queue, g_qcap * sizeof *g_queue);
        if(!g_queue) {
            abort();
        }
    }
    g_queue[g_qlen++] = e;
}

static bool known_key(const char* key)
{
    for(size_t i = 0; i < sizeof named / sizeof *named; i++) {
        if(strcmp(named[i].name, key) == 0) {
            return true;
        }
    }
    return strlen(key) == 1;
}

/* a key down, and (as a keyboard would) the text it types: the app drops it if the key was handled (swallow_text), as with SDL */
void gp_platform_inject_key(gp_window* w, const char* key, uint32_t mods)
{
    if(!known_key(key)) {
        return;
    }
    injected e = { .type = IN_KEY, .app = w->app, .window = w->id, .mods = mods };
    snprintf(e.text, sizeof e.text, "%s", key);
    inject(e);
    char c = strcmp(key, "space") == 0 ? ' ' : (strlen(key) == 1 && key[0] > 0x20 && key[0] < 0x7f ? key[0] : 0);
    if(c >= 'a' && c <= 'z' && (mods & GP_MOD_SHIFT)) {
        c -= 32;
    }
    if(c && !(mods & (GP_MOD_CTRL | GP_MOD_ALT | GP_MOD_SUPER))) {
        injected t = { .type = IN_TEXT, .app = w->app, .window = w->id };
        t.text[0] = c;
        inject(t);
    }
}

void gp_platform_inject_key_up(gp_window* w, const char* key, uint32_t mods)
{
    if(!known_key(key)) {
        return;
    }
    injected e = { .type = IN_KEY_UP, .app = w->app, .window = w->id, .mods = mods };
    snprintf(e.text, sizeof e.text, "%s", key);
    inject(e);
}

void gp_platform_inject_text(gp_window* w, const char* utf8)
{
    injected e = { .type = IN_TEXT, .app = w->app, .window = w->id };
    snprintf(e.text, sizeof e.text, "%s", utf8);
    inject(e);
}

void gp_platform_inject_mouse(gp_window* w, int kind, float x, float y, int button, int clicks, uint32_t mods)
{
    injected e = { .type = kind == GP_SIM_LEAVE ? IN_LEAVE : kind == GP_SIM_MOVE ? IN_MOVE
                                                                                 : IN_BUTTON,
        .app = w->app,
        .window = w->id,
        .x = x,
        .y = y,
        .button = button == 2 ? GP_MOUSE_MIDDLE : button == 3 ? GP_MOUSE_RIGHT
                                                              : GP_MOUSE_LEFT,
        .clicks = clicks,
        .down = kind == GP_SIM_DOWN,
        .mods = mods };
    inject(e);
}

void gp_platform_inject_wheel(gp_window* w, float x, float y, float dx, float dy, uint32_t mods)
{
    injected e = { .type = IN_WHEEL, .app = w->app, .window = w->id, .x = x, .y = y, .dx = dx, .dy = dy, .mods = mods };
    inject(e);
}

/* the queued events, in order (one handed out may queue more: those wait for the next pump) */
static bool deliver_injected(void)
{
    size_t end = g_qlen;
    bool any = g_qhead < end;
    while(g_qhead < end) {
        injected e = g_queue[g_qhead++];
        gp_window* w = gp_app_window_by_id(e.app, e.window);
        if(!w) {
            continue;
        }
        switch(e.type) {
            case IN_KEY:
                gp_window_on_key(w, e.text, e.mods, false);
                break;
            case IN_KEY_UP:
                gp_window_on_key_up(w, e.text, e.mods);
                break;
            case IN_TEXT:
                gp_window_on_text(w, e.text);
                break;
            case IN_MOVE:
                gp_window_on_mouse_move(w, e.x, e.y, e.mods);
                break;
            case IN_BUTTON:
                gp_window_on_mouse_button(w, e.x, e.y, e.button, e.down, e.clicks, e.mods);
                break;
            case IN_LEAVE:
                gp_window_on_mouse_leave(w);
                break;
            case IN_WHEEL: { /* in notches, SDL's way: dy < 0 scrolls down, dx > 0 right; shift + wheel sideways */
                float dx = e.dx, dy = e.dy;
                if(dx == 0 && (e.mods & GP_MOD_SHIFT)) {
                    dx = -dy;
                    dy = 0;
                }
                gp_window_on_scroll(w, e.x, e.y, dx * GP_SCROLL_LINE_PX, -dy * GP_SCROLL_LINE_PX, e.mods);
                break;
            }
        }
        gp_effects_flush(e.app);
    }
    if(g_qhead == g_qlen) {
        g_qhead = g_qlen = 0;
    }
    return any;
}

/* ---------------------------------------------------------------- the loop */

/* a held key's repeats that are due */
static bool deliver_repeats(void)
{
    if(!g.repeat_key || !g.key_win || g.repeat_rate <= 0 || g_ignore_real_keys) {
        return false;
    }
    bool any = false;
    int64_t t = now_ns();
    for(int i = 0; i < 8 && g.repeat_key && g.key_win && t >= g.repeat_at; i++) {
        g.repeat_at += 1000000000 / g.repeat_rate;
        key_press(g.key_win, g.repeat_key, true);
        any = true;
    }
    return any;
}

/* Waits up to timeout_ms (-1: until something comes) for events, then hands out all that are there. */
void gp_platform_pump(gp_app* app, int timeout_ms)
{
    int64_t deadline = timeout_ms > 0 ? gp_now_ns() + (int64_t)timeout_ms * 1000000 : 0;
    unsigned seen = g_delivered;
    bool got = deliver_injected();
    for(;;) {
        while(wl_display_prepare_read(g.display) != 0) {
            if(wl_display_dispatch_pending(g.display) < 0) {
                break;
            }
        }
        got |= g_delivered != seen;
        bool unsent = wl_display_flush(g.display) < 0 && errno == EAGAIN; /* (the socket is full: wait until it takes more) */
        int wait = 0;
        if(!got) {
            if(timeout_ms < 0) {
                wait = -1;
            } else if(timeout_ms > 0) {
                int64_t left = deadline - gp_now_ns();
                wait = left > 0 ? (int)((left + 999999) / 1000000) : 0;
            }
            if(g.repeat_key && g.key_win) { /* (a held key repeats on time) */
                int64_t r = g.repeat_at - gp_now_ns();
                int rw = r > 0 ? (int)((r + 999999) / 1000000) : 0;
                wait = wait < 0 || rw < wait ? rw : wait;
            }
        }
        struct pollfd fds[2] = {
            { wl_display_get_fd(g.display), (short)(POLLIN | (unsent ? POLLOUT : 0)), 0 },
            {                    g.wake_fd,                                   POLLIN, 0 }
        };
        int n = poll(fds, 2, wait);
        bool lost = n > 0 && (fds[0].revents & (POLLERR | POLLHUP)) && !(fds[0].revents & POLLIN);
        if(n > 0 && (fds[0].revents & POLLIN)) {
            lost |= wl_display_read_events(g.display) < 0;
        } else {
            wl_display_cancel_read(g.display);
        }
        lost |= wl_display_dispatch_pending(g.display) < 0;
        if(lost || wl_display_get_error(g.display)) { /* the compositor went away: nothing more will come, and the app ends */
            static bool said;
            if(!said) {
                said = true;
                fprintf(stderr, "gpuic: the Wayland connection was lost (%s)\n", strerror(wl_display_get_error(g.display)));
            }
            gp_quit(app);
            return;
        }
        got |= g_delivered != seen;
        if(n > 0 && (fds[1].revents & POLLIN)) {
            uint64_t v;
            ssize_t r = read(g.wake_fd, &v, sizeof v);
            (void)r;
            got = true;
        }
        got |= deliver_repeats();
        got |= deliver_injected();
        if(n < 0 && errno != EINTR) {
            return;
        }
        if(got || timeout_ms == 0 || (timeout_ms > 0 && gp_now_ns() >= deadline)) {
            return;
        }
    }
}

/* ---------------------------------------------------------------- files */

static void walk(const char* dir, void (*file)(const char* path, int64_t size, int64_t mtime_ns, void* user), void* user, int depth)
{
    DIR* d = opendir(dir);
    if(!d) {
        return;
    }
    struct dirent* e;
    while((e = readdir(d))) {
        if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        char path[1024];
        int n = snprintf(path, sizeof path, "%s%s%s", dir, dir[0] && dir[strlen(dir) - 1] != '/' ? "/" : "", e->d_name);
        struct stat st;
        if(n <= 0 || (size_t)n >= sizeof path || stat(path, &st) != 0) {
            continue;
        }
        if(S_ISREG(st.st_mode)) {
            file(path, (int64_t)st.st_size, (int64_t)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec, user);
        } else if(S_ISDIR(st.st_mode) && depth < 8) { /* depth bounds symlink loops */
            walk(path, file, user, depth + 1);
        }
    }
    closedir(d);
}

void gp_platform_walk_dir(const char* dir, void (*file)(const char* path, int64_t size, int64_t mtime_ns, void* user), void* user)
{
    walk(dir, file, user, 0);
}

bool gp_platform_mkdirs(const char* dir)
{
    char path[1024];
    if(snprintf(path, sizeof path, "%s", dir) >= (int)sizeof path) {
        return false;
    }
    for(char* p = path + 1; *p; p++) {
        if(*p == '/') {
            *p = 0;
            mkdir(path, 0755);
            *p = '/';
        }
    }
    mkdir(path, 0755);
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool gp_platform_rename(const char* from, const char* to)
{
    return rename(from, to) == 0;
}
