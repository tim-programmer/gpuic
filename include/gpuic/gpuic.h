/*
 * gpuic — a GPU-accelerated UI framework for C, modelled on Rust's GPUI.
 *
 * Concepts (and their GPUI counterparts):
 *   gp_app / gp_application_run   Application::new().run(..)
 *   gp_entity                     Entity<T>: app-owned state, addressed by handle
 *   gp_view_new + gp_render_fn    a View: an entity that implements Render
 *   gp_div / gp_text + modifiers  the element builder (div().flex().p_4()...)
 *   gp_ctx / gp_notify            Context<T> / cx.notify()
 *
 * Elements and modifiers are both `gp_node *`, allocated from a per-frame arena.
 * Every builder takes a list of nodes: modifiers style the element being built,
 * elements become its children, NULLs are skipped (so `cond ? child : NULL`
 * works). The gp_div()/gp_text() macros add the terminator for you.
 */
#ifndef GPUIC_H
#define GPUIC_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- geometry */

typedef struct { float x, y; } gp_point;
typedef struct { float w, h; } gp_size;
typedef struct { float r, g, b, a; } gp_color;
typedef struct { gp_color color; float at; } gp_color_stop;   /* a gradient's stop: see gp_bg_gradient */

/* A value of a struct type, written for both languages: C's compound literal `(T){ ... }` is not C++ (-Wpedantic says so), a braced `T{ ... }` is not C. */
#ifdef __cplusplus
#define GP_LIT(T, ...) T{ __VA_ARGS__ }
#else
#define GP_LIT(T, ...) ((T){ __VA_ARGS__ })
#endif

/* 0xRRGGBB and 0xRRGGBBAA */
static inline gp_color gp_rgb(uint32_t hex) {
    return GP_LIT(gp_color, ((hex >> 16) & 0xff) / 255.f, ((hex >> 8) & 0xff) / 255.f,
                       (hex & 0xff) / 255.f, 1.f);
}
static inline gp_color gp_rgba(uint32_t hex) {
    return GP_LIT(gp_color, ((hex >> 24) & 0xff) / 255.f, ((hex >> 16) & 0xff) / 255.f,
                       ((hex >> 8) & 0xff) / 255.f, (hex & 0xff) / 255.f);
}
static inline gp_color gp_with_alpha(gp_color c, float a) { c.a = a; return c; }

/* ---------------------------------------------------------------- app/entities */

typedef struct gp_app gp_app;
typedef struct gp_window gp_window;
typedef struct gp_node gp_node;

/* Generational handle; the zero value is never valid. */
typedef struct { uint32_t index, generation; } gp_entity;
#define GP_ENTITY_NULL GP_LIT(gp_entity, 0, 0)

/* A decoded picture (see "images" below): generational, and the zero value is never valid. */
typedef struct { uint32_t index, generation; } gp_image;
#define GP_IMAGE_NULL GP_LIT(gp_image, 0, 0)
/* A parsed vector drawing (see "SVG" below): generational like gp_image. */
typedef struct { uint32_t index, generation; } gp_svg;
#define GP_SVG_NULL GP_LIT(gp_svg, 0, 0)

typedef struct gp_ctx {
    gp_app *app;
    gp_window *window;   /* may be NULL outside of window callbacks */
    gp_entity entity;    /* the entity this callback belongs to */
    bool propagate;      /* set via gp_propagate(): let the event bubble further */
} gp_ctx;

typedef gp_node *(*gp_render_fn)(void *self, gp_ctx *cx);
typedef void (*gp_drop_fn)(void *self);

/* `init` is copied into the entity (may be NULL for zeroed state). */
gp_entity gp_entity_new(gp_app *app, size_t size, const void *init, gp_drop_fn drop);
gp_entity gp_view_new(gp_app *app, size_t size, const void *init, gp_render_fn render,
                      gp_drop_fn drop);
/* Returns NULL if the entity has been released. The pointer is stable until then. */
void *gp_entity_get(gp_app *app, gp_entity e);
void gp_entity_release(gp_app *app, gp_entity e);

#define GP_NEW(app, T, ...)            gp_entity_new((app), sizeof(T), &(T){ __VA_ARGS__ }, NULL)
#define GP_NEW_VIEW(app, T, render, ...) \
    gp_view_new((app), sizeof(T), &(T){ __VA_ARGS__ }, (render), NULL)

/* State changed. `cx->entity` is what changed:
 *   a view      it renders again, and every window showing it rebuilds its tree;
 *   any entity  whoever observes it (gp_observe, below) is called;
 *   no entity   (a ctx from gp_ctx_for(app, GP_ENTITY_NULL)) the app itself: everything is invalidated.
 * A plain entity (a model, with no render function) that nobody observes repaints nothing: the views that
 * show it must observe it, as in GPUI, and notify themselves from the observer.
 * gp_app_notify() is the last case without a ctx: every window rebuilds and every cached view renders again. */
void gp_notify(gp_ctx *cx);
void gp_app_notify(gp_app *app);
/* Every window paints again (nothing is built or laid out again): for measuring what painting costs. */
void gp_app_request_paint(gp_app *app);

/* ---- statistics ----
 * What the app has done since it started (or since gp_app_stats_reset), all windows together: what GPUIC_STATS=1 prints at exit, for performance tests of
 * one's own. Take two and subtract to measure one stretch; the times are summed over draws (divide by `draws` for one). */
typedef struct {
    unsigned long draws;            /* frames drawn */
    unsigned long wakeups;          /* times the loop woke */
    unsigned long view_renders;     /* render functions called */
    unsigned long atlas_resets;     /* glyph / gradient atlases emptied because they were full */
    unsigned long layout_skips;     /* cached views whose layout was skipped */
    unsigned long paint_hits, paint_misses, paint_shifts;   /* cached views' painting: reused, painted again, reused moved */
    int64_t build_ns, layout_ns, paint_ns, submit_ns;        /* time in each phase of drawing (submit: handing the scene to the GPU) */
    int64_t instances;              /* instances drawn, summed over draws */
    int64_t last_instances;         /* ... in the last draw */
} gp_stats;
void gp_app_stats(gp_app *app, gp_stats *out);
void gp_app_stats_reset(gp_app *app);

/* ---- observing entities ----
 * GPUI's cx.observe / cx.subscribe / cx.emit / cx.observe_release. A callback is registered by
 * the entity whose ctx is passed (`cx->entity`, which becomes the callback's `self`; with
 * GP_ENTITY_NULL it belongs to the app and `self` is NULL), and watches another entity.
 *
 * Nothing runs inside the call that causes it. gp_notify and gp_emit queue work, and the queue is
 * run as soon as the callback in progress has returned to the event loop: after each platform event,
 * each timer / frame / posted function / coroutine step, and before a window is rendered. So a
 * callback is never re-entered by its own notifications, observers see the entity as the caller
 * left it, and state they update is in place when the frame is drawn. Observers may notify and
 * emit in turn; that all runs in the same flush, in order. Several notifies of one entity before the
 * flush call its observers once. (Observers that notify each other in a cycle are cut off after
 * 100,000 rounds, with a message.) Everything is on the main thread.
 *
 * A subscription ends when the entity that registered it or the entity it watches is released,
 * or with gp_unsubscribe. A callback whose owner is released never runs, including ones already
 * queued. The `gp_subscription` handle is safe to keep after any of that. */
typedef struct { uint64_t id; } gp_subscription;

/* What gp_emit sent. `payload` is a copy that lives until the callback returns. */
typedef struct {
    gp_entity emitter;
    const char *name;
    const void *payload;
    size_t size;
} gp_event;

typedef void (*gp_observe_fn)(void *self, gp_ctx *cx, gp_entity observed, void *data);
typedef void (*gp_event_fn)(void *self, gp_ctx *cx, const gp_event *ev, void *data);
/* `state` is the released entity's state, valid for this call only (its drop function has not run). */
typedef void (*gp_release_fn)(void *self, gp_ctx *cx, gp_entity released, void *state, void *data);

/* Calls `fn` whenever `target` calls gp_notify. With fn == NULL the observer is re-rendered and
 * notified itself, which is what a view that just mirrors a model wants (as
 * cx.observe(&model, |_, _, cx| cx.notify()) does). Returns a zero handle if `target` or the
 * calling entity is already released. */
gp_subscription gp_observe(gp_ctx *cx, gp_entity target, gp_observe_fn fn, void *data);

/* Calls `fn` for events `emitter` sends with gp_emit. `event` is the event name to listen for, or
 * NULL for every event. Events are named rather than typed: agree on the payload struct for a
 * name, as with actions. */
gp_subscription gp_subscribe(gp_ctx *cx, gp_entity emitter, const char *event, gp_event_fn fn, void *data);

/* Calls `fn` as `target` is released, before its state is freed. */
gp_subscription gp_observe_release(gp_ctx *cx, gp_entity target, gp_release_fn fn, void *data);

/* Sends an event from cx->entity to its subscribers. `payload` (size bytes, may be NULL) is copied. */
/* Calls `drop(data)` exactly once after the subscription is gone (unsubscribed, or its target or owner released, or the app
 * shut down), at the next flush of effects, never inside the callback that unsubscribed: `data` (a closure) may be freed there.
 * On a handle that is already stale, it is called at once. */
void gp_subscription_on_drop(gp_app *app, gp_subscription h, gp_drop_fn drop, void *data);

void gp_emit(gp_ctx *cx, const char *event, const void *payload, size_t size);
#define gp_emit_as(cx, event, T, ...) gp_emit((cx), (event), &(T){ __VA_ARGS__ }, sizeof(T))

void gp_unsubscribe(gp_app *app, gp_subscription s);          /* safe on stale or zero handles */
bool gp_subscription_active(gp_app *app, gp_subscription s);

/* Runs the queued observers and subscribers now. The event loop does this by itself; this is for
 * code that runs outside it (tests, `ready`) and wants the effects before going on. Does nothing
 * when called from inside a flush. */
void gp_flush_effects(gp_app *app);

typedef struct {
    const char *title;
    int width, height;       /* logical pixels; 0 => 960x640 */
    gp_color background;     /* a == 0 => dark grey */
} gp_window_options;

gp_window *gp_open_window(gp_app *app, const gp_window_options *opts, gp_entity root_view);
void gp_quit(gp_app *app);

/* ---- fonts ----
 * With nothing registered, fonts come from the system (fontconfig, or a scan of the system font
 * directories). Registering fonts makes an app independent of what is installed: bundle the
 * files, or embed them as byte arrays, and register them in the `ready` callback, before the
 * first window opens.
 *
 * Registered fonts win over system fonts. A registered font belongs to a family, named by the font
 * itself or by `family` below, and text asks for a family and a weight (gp_font_family,
 * gp_font_weight). Within a family the face is chosen the way CSS does it: the weight asked for if
 * there is one, else the nearest, looking lighter first below 400, heavier first above 500. Where
 * the family has nothing heavier than 600, bold (600 and up) is synthesized by thickening the face.
 * Text that names no family uses the registered family called "sans-serif" if there is one, else the
 * family of the font registered first. A character the chosen font lacks is looked up in the rest of
 * the family list, among the other registered fonts, then, unless disabled, in the system. A
 * registered color font is used for emoji (bitmap CBDT fonts like Noto Color Emoji are the supported
 * kind). Main thread only. */
typedef struct {
    const char *family;      /* e.g. "Inter", or "serif" / "monospace" / "sans-serif" to define a generic
                                one; NULL => the family the font names itself. Copied. */
    int weight;              /* 100..900; 0 => read from the font's OS/2 table */
    int face_index;          /* which face of a collection (.ttc/.otc); 0 for a plain font */
} gp_font_options;

/* Both return false, changing nothing, if the data isn't a font FreeType can open, or the
 * options are out of range. `opts` may be NULL. Registering the same file again only updates
 * its weight. gp_font_add_memory copies `data`, so a temporary buffer or a const array both work. */
bool gp_font_add_file(gp_app *app, const char *path, const gp_font_options *opts);
bool gp_font_add_memory(gp_app *app, const void *data, size_t size, const gp_font_options *opts);
/* gp_fonts_use_system(app, false): never consult the system, so only registered fonts draw
 * text (anything they lack shows as the primary's missing-glyph box) and no system font
 * directory is ever scanned. Default: enabled. */
void gp_fonts_use_system(gp_app *app, bool enable);

/* How glyph edges are smoothed. GP_AA_GRAYSCALE: one coverage value per pixel. GP_AA_LCD_RGB / GP_AA_LCD_BGR: one per
 * color stripe of an LCD pixel (sharper horizontally on a panel with that subpixel order; text gets color fringes
 * on any other, and in a screenshot or on a rotated display), blended per channel. GP_AA_LCD_VRGB / GP_AA_LCD_VBGR are for a
 * panel whose stripes run across (a display turned a quarter: red on top for VRGB), sharper vertically. GP_AA_AUTO, the default, takes the
 * system's choice (fontconfig's "rgba" setting, which is none unless configured) and uses it below 1.5x display scale,
 * where it helps; an explicit LCD mode is used at any scale. Color emoji are never affected. The environment variable
 * GPUIC_LCD=off|rgb|bgr|vrgb|vbgr|auto sets the starting mode. */
enum { GP_AA_AUTO = 0, GP_AA_GRAYSCALE, GP_AA_LCD_RGB, GP_AA_LCD_BGR, GP_AA_LCD_VRGB, GP_AA_LCD_VBGR };   /* (V: the stripes are rows, red on top for VRGB) */
void gp_fonts_set_antialiasing(gp_app *app, int mode);
/* Text blending. The renderer blends in the encoded (gamma) space, where coverage 0.5 of black on white is the middle gray and not the
 * half of the light, so dark text on light looks a little heavier than it should and light on dark a little thinner. With a gamma above 1
 * (2.2 is the usual display's) the coverage of each glyph pixel is adjusted so that the blend comes out as it would have in linear light,
 * for a background of the text's opposite luminance (white behind black text, black behind white: the usual cases; on another background
 * the correction is approximate, and mid-gray text is left alone). 1, the default, changes nothing; GPUIC_TEXT_GAMMA sets it at startup.
 * Applies to grayscale and LCD text (to each stripe), not to emoji. */
void gp_fonts_set_text_gamma(gp_app *app, float gamma);

typedef void (*gp_ready_fn)(gp_app *app, void *userdata);
/* Creates the app, calls `ready` once, then runs the event loop until quit. */
int gp_application_run(gp_ready_fn ready, void *userdata);

/* ---------------------------------------------------------------- events */

enum { GP_MOUSE_LEFT = 1, GP_MOUSE_MIDDLE = 2, GP_MOUSE_RIGHT = 3 };

typedef struct {
    gp_point position;   /* window coordinates, logical pixels */
    gp_point local;      /* position relative to the element whose handler is running */
    int button;
    int click_count;     /* 1, 2, 3... for rapid repeated presses */
    uint32_t mods;       /* GP_MOD_* held at the time (see below) */
} gp_mouse_event;

/* `self` is the state of the view that built the element. `data` is whatever was
 * passed to the *_data variant (NULL otherwise). */
typedef void (*gp_mouse_fn)(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data);

/* ---------------------------------------------------------------- keyboard */

enum { GP_MOD_CTRL = 1, GP_MOD_ALT = 2, GP_MOD_SHIFT = 4, GP_MOD_SUPER = 8 };

/* `key` is the unshifted key name: "a", "1", "/", "enter", "escape", "tab",
 * "backspace", "delete", "space", "left", "right", "up", "down", "home", "end",
 * "pageup", "pagedown", "insert", "f1".."f12". */
typedef struct {
    const char *key;
    uint32_t mods;
    bool repeat;
} gp_key_event;

typedef void (*gp_key_fn)(void *self, gp_ctx *cx, const gp_key_event *ev, void *data);
/* Key release: delivered along the same path as key down (focused element first, bubbling up unless gp_propagate). Not bound to actions. */
typedef void (*gp_text_input_fn)(void *self, gp_ctx *cx, const char *utf8, void *data);
/* Input-method composition ("preedit") text: shown inline but not yet committed. Empty text
 * means the composition ended (committed text then arrives as ordinary text input).
 * `cursor` is the caret position inside the preedit, in characters. */
typedef void (*gp_ime_fn)(void *self, gp_ctx *cx, const char *preedit, int cursor, void *data);

/* ---------------------------------------------------------------- clipboard */

/* Returns a malloc'd UTF-8 string (free it) or NULL if the clipboard holds no text. If the
 * platform has none (it can refuse an unfocused client), the app's own last copy is returned,
 * so copy/paste inside the app keeps working. */
char *gp_clipboard_get(gp_app *app);
void gp_clipboard_set(gp_app *app, const char *utf8);

/* The primary selection (X11 and Wayland): whatever was last selected, pasted with the middle mouse button, separate from the clipboard.
 * gp_text_input sets it when the mouse finishes a selection (drag, double- or triple-click) and pastes it on a middle click; a password
 * field never exports its text. Same contract as the clipboard: malloc'd UTF-8 or NULL, and the app's own last selection stands in
 * where the platform refuses to hold one (unfocused Wayland client, no session). */
char *gp_primary_get(gp_app *app);
void gp_primary_set(gp_app *app, const char *utf8);

/* ---------------------------------------------------------------- synthetic input */

/* Synthetic input for tests and demos. It is queued as real platform events, so the whole
 * path (key mapping, bindings, focus, text input gating) is exercised. Events are handled on
 * the following loop turns: yield or sleep before asserting. They go to the first window. */
enum { GP_SIM_MOVE, GP_SIM_DOWN, GP_SIM_UP, GP_SIM_LEAVE };   /* (GP_SIM_LEAVE: the pointer left the window; x and y are not used) */
void gp_simulate_keystrokes(gp_app *app, const char *keys);   /* "tab h i shift-tab ctrl-a": each is a press and a release */
void gp_simulate_key(gp_app *app, const char *keystroke, bool down);   /* one press or one release: "shift-a" */
void gp_simulate_text(gp_app *app, const char *utf8);         /* as if typed / committed by an IME */
void gp_simulate_ime(gp_app *app, const char *preedit, int cursor); /* synchronous, see app.c */
void gp_simulate_mouse(gp_app *app, int kind, float x, float y, int clicks, uint32_t mods);
void gp_simulate_wheel(gp_app *app, float x, float y, float dx, float dy, uint32_t mods);   /* in notches, SDL's way: dy < 0 scrolls down, dx > 0 right (as GPUIC_WHEEL) */
/* One finger on the touch screen, which scrolls: a tap (down, up without moving more than 8 px) is a click at that point; a drag moves the
 * scrollers under the finger with it, immediately (no smooth-scroll glide), the innermost that can move first and then the ones around it as
 * the wheel does; lifting the finger while it was moving faster than 120 px/s (measured over its last 100 ms) goes on as a fling that slows
 * with a time constant of 0.35 s (a flick of v px/s travels v * 0.35 px) until the end of the content or until it is slower than 8 px/s,
 * and a new touch or a wheel turn stops it. Real touch events (SDL's, the first finger only) take the same path; the pointer is not
 * emulated from touch. gp_simulate_touch is synchronous and takes the time of the event in seconds, so a test controls the speed. */
enum { GP_TOUCH_DOWN, GP_TOUCH_MOVE, GP_TOUCH_UP };
void gp_simulate_touch(gp_app *app, int phase, float x, float y, double time_seconds);
void gp_simulate_mouse_button(gp_app *app, int kind, float x, float y, int button, int clicks, uint32_t mods);   /* button: GP_MOUSE_LEFT / MIDDLE / RIGHT */

/* ---------------------------------------------------------------- focus */

/* A focus handle is a stable id: keep it in your view's state and hand it to
 * gp_track_focus() each time you render. */
typedef struct { uint64_t id; } gp_focus;

gp_focus gp_focus_new(gp_app *app);
void gp_focus_set(gp_ctx *cx, gp_focus f);
void gp_blur(gp_ctx *cx);
bool gp_is_focused(gp_ctx *cx, gp_focus f);
void gp_focus_next(gp_ctx *cx);
void gp_focus_prev(gp_ctx *cx);

/* ---------------------------------------------------------------- actions */

/* An action is a name, conventionally "namespace::verb". */
typedef const char *gp_action;
typedef void (*gp_action_fn)(void *self, gp_ctx *cx, void *data);

/* Keystrokes look like "ctrl-shift-s", "enter", "shift-tab", "ctrl--".
 * `context` is NULL (global) or a predicate over key contexts on the path from
 * the root to the focused element: "Editor", "Editor && !ReadOnly". Bindings
 * matching a deeper context win; later bindings win ties. Returns false if the
 * keystroke could not be parsed. */
bool gp_bind_key(gp_app *app, const char *keystroke, gp_action action, const char *context);
/* Several keystrokes in one binding: "ctrl-k ctrl-c", "g g", "ctrl-x ctrl-s" (up to four, separated by spaces). The first
 * keystroke of a chord that has bindings in the focus path's contexts is consumed and waits for the next one (see
 * gp_keys_pending); the whole chord runs the action. A key that does not continue the chord cancels it and is consumed too
 * (like Emacs: "ctrl-x x is undefined"), escape cancels it, and a chord left half-typed is forgotten after 1.5 s (the next key is then
 * an ordinary one). A binding that is exactly a prefix of another wins at once, so bind a chord's first key alone only if you do not
 * need the chord. */
/* Like gp_bind_key, with a copy of `payload` (size bytes) that a handler reads with gp_action_payload: one action, many keys
 * ("ctrl-1" .. "ctrl-9" -> "tab::select" with 1..9). */
bool gp_bind_key_with(gp_app *app, const char *keystroke, gp_action action, const char *context, const void *payload, size_t size);
/* The keystrokes of a chord typed so far in the window of `cx` ("ctrl-k"), "" when none: for a status line. */
const char *gp_keys_pending(gp_ctx *cx);
/* The payload of the action being handled: what its binding was bound with, or what gp_dispatch_action_with was given; NULL
 * (size 0) for an action with none, and outside a handler. Valid until the handler returns. */
const void *gp_action_payload(gp_ctx *cx, size_t *size);
void gp_clear_key_bindings(gp_app *app);

/* Handles an action nobody on the focus path handled; `self` is NULL. */
void gp_on_global_action(gp_app *app, gp_action action, gp_action_fn fn, void *data);

/* Dispatches `action` as if a keybinding had fired, starting at the focused
 * element. Returns true if a handler took it. */
bool gp_dispatch_action(gp_ctx *cx, gp_action action);
bool gp_dispatch_action_with(gp_ctx *cx, gp_action action, const void *payload, size_t size);   /* a handler sees it with gp_action_payload */

/* Call from a handler to let the event bubble on to the next handler. */
static inline void gp_propagate(gp_ctx *cx) { cx->propagate = true; }

/* ---------------------------------------------------------------- scrolling */

/* Scroll offsets persist across rebuilds. A container with gp_overflow_*_scroll()
 * but no handle is keyed by its position in the element tree, which is fine for
 * static layouts; give it a handle (gp_track_scroll) to control it from code or
 * when the tree around it changes shape. Offsets are in logical pixels; larger
 * offset = content further down/right. They are clamped at the next layout. */
typedef struct { uint64_t id; } gp_scroll;

gp_scroll gp_scroll_new(gp_app *app);
gp_point gp_scroll_offset(gp_ctx *cx, gp_scroll h);
gp_point gp_scroll_max(gp_ctx *cx, gp_scroll h);     /* as of the last layout */
gp_size gp_scroll_viewport(gp_ctx *cx, gp_scroll h); /* visible area, as of the last layout */
void gp_scroll_to(gp_ctx *cx, gp_scroll h, float x, float y);
void gp_scroll_by(gp_ctx *cx, gp_scroll h, float dx, float dy);
void gp_scroll_to_end(gp_ctx *cx, gp_scroll h);      /* bottom; stays put until called again */
/* Smooth wheel scrolling: with `seconds` > 0 a wheel tick moves the scroller's target and the offset glides to it (an exponential
 * approach that has covered 98% of the distance after `seconds`; more ticks add to the target, so a spinning wheel accelerates). A
 * tick that finds the scroller's target at its end goes on to the scroller above, whatever the offset has reached. Dragging the
 * thumb, a track click, gp_scroll_to and scroll-into-view end the glide where the offset is. Frames are asked for only while a glide
 * lasts. Off (0) by default, where the offset jumps; the GPUIC_SMOOTH_SCROLL environment variable (milliseconds) sets it at startup. */
void gp_set_smooth_scrolling(gp_app *app, float seconds);

/* Scrolls every scroller around the element tracked by this focus handle (gp_track_focus) by the least that shows it, innermost first,
 * once the next layout is done. Tab and Shift-Tab (and gp_focus_next/prev) do this for the element they focus; clicking, gp_focus_set and
 * gp_text_input's own caret reveal do not need it. An element larger than the view and filling it is left alone; one that is not in
 * the tree (a row scrolled out of a gp_list) is ignored. */
void gp_scroll_into_view(gp_ctx *cx, gp_focus f);

/* Brings row `index` of the gp_uniform_list or gp_list tracked by `h` into view. Needs one
 * layout to have happened (otherwise it does nothing). In a gp_list the row may not have been
 * measured yet: the move is made by the next layout, which lands on the row exactly, and if
 * called several times before then, the last call wins. */
enum { GP_ALIGN_ROW_NEAREST = 0, GP_ALIGN_ROW_START, GP_ALIGN_ROW_CENTER };
/* Group headers that stay put. A gp_uniform_list or gp_list with gp_list_sticky(fn, data) keeps a header at the top of its view for as long as
 * its group is on screen, and the next group's header pushes it out as it arrives (iOS and Android section headers, CSS `position: sticky`).
 * `fn(self, cx, row, data)` is asked, for the first row that is at least partly in view, for that row's group: `header` is the element to pin
 * (built like a row; NULL for a row with no group), `first` the index of the group's first row (its header row, if it has one in the list: the
 * pinned copy appears only once that row has gone past the top), `next` the index of the next group's first row (any value past the end if
 * there is none), which is what pushes it up. The header is as wide as the rows and as high as it lays out. It is drawn above the rows, takes
 * the mouse over its own area, and is rebuilt as the list scrolls, so keep `fn` cheap. */
typedef struct { gp_node *header; size_t first, next; } gp_list_group;
typedef gp_list_group (*gp_list_group_fn)(void *self, gp_ctx *cx, size_t row, void *data);
gp_node *gp_list_sticky(gp_list_group_fn fn, void *data);
/* On a gp_uniform_list: the items go side by side and the list scrolls sideways, so a carousel, a timeline or a strip of thousands of
 * columns is only as heavy as what is in view. `item_height` of the list becomes the items' width (0: the first item's own width), the items
 * are as tall as the list (give it a height, or it takes its first item's), and everything about the wheel (shift + wheel, a trackpad's sideways
 * motion), the thumb, touch, gp_scroll_to and gp_list_scroll_to_item works along x. Not for gp_list (rows of different heights), nor with
 * gp_list_sticky. */
gp_node *gp_list_horizontal(void);
void gp_list_scroll_to_item(gp_ctx *cx, gp_scroll h, size_t index, int align);

/* ---------------------------------------------------------------- timers & async */

/* Everything here runs on the main thread, inside the event loop, except gp_post() (any
 * thread) and the `work` function of gp_background() (a pool thread). Callbacks that belong
 * to an entity (cx->entity) receive its state as `self`; if the entity has been released by
 * the time they would run they are silently dropped, and tasks owned by a released entity
 * are cancelled. Use a ctx with GP_ENTITY_NULL for app-level work (`self` is then NULL). */

double gp_time(void);                              /* monotonic seconds */
gp_ctx gp_ctx_for(gp_app *app, gp_entity entity);  /* a ctx for use outside a callback: not tied to a window */

/* Per-window work. Timers, frame requests, animations, descriptor watches and coroutines belong to the window
 * of the ctx they were started with, when it has one: a ctx handed to an event handler or a render function does
 * (cx->window), gp_ctx_for_window makes one, and gp_ctx_for does not. When that window closes they are cancelled
 * (coroutines unwind through their awaits like any cancelled one, before the window goes), their callbacks see it
 * as cx->window, and frame requests are paced by that window's display instead of the first window's. Work
 * started with a plain gp_ctx_for ctx lives as long as the app, or its entity. */
gp_ctx gp_ctx_for_window(gp_app *app, gp_window *window, gp_entity entity);
void gp_window_close(gp_window *window);           /* closes it at the next turn of the loop; the app ends with its last window */

/* A handle to anything in flight: timer, animation, background job, coroutine. */
typedef struct { uint64_t id; } gp_task;
bool gp_task_active(gp_app *app, gp_task t);
void gp_cancel(gp_app *app, gp_task t);            /* safe on finished / stale handles */

typedef void (*gp_task_fn)(void *self, gp_ctx *cx, void *data);
typedef void (*gp_frame_fn)(void *self, gp_ctx *cx, double time, void *data);
typedef void (*gp_anim_fn)(void *self, gp_ctx *cx, float t, void *data);
typedef void (*gp_work_fn)(void *data);

/* Timers. Intervals do not drift; if the loop falls behind, missed ticks are skipped. */
gp_task gp_after(gp_ctx *cx, double seconds, gp_task_fn fn, void *data);
gp_task gp_every(gp_ctx *cx, double seconds, gp_task_fn fn, void *data);
/* Calls `drop(data)` exactly once, on the main thread, when the task is over for good: it ran to its end, was cancelled, its
 * entity or window went away, or the app shut down. After the callback could last run, never before, so `data` (a closure) is
 * safe to free there. On a task that is already gone (or a zero handle) it is called at once. */
void gp_task_on_drop(gp_app *app, gp_task t, gp_drop_fn drop, void *data);

/* Runs `fn` once just before the next frame is drawn (like requestAnimationFrame).
 * Re-arm it from inside the callback to animate continuously; nothing is drawn or
 * woken while no frame is requested. */
gp_task gp_next_frame(gp_ctx *cx, gp_frame_fn fn, void *data);

enum { GP_EASE_LINEAR, GP_EASE_IN, GP_EASE_OUT, GP_EASE_IN_OUT, GP_EASE_OUT_BACK,
       GP_EASE_OUT_BOUNCE,
       GP_EASE_SPRING,          /* a spring: overshoots by 16% and settles, damping ratio 0.5 */
       GP_EASE_SPRING_BOUNCY }; /* a loose one: 44% overshoot and several swings, damping ratio 0.25 */
float gp_ease(int easing, float t);                /* t in [0,1] -> eased value */
/* The step response of a damped spring, run so that it has settled (to 1/1000) at t = 1: `damping` is the damping ratio, 0.1 (rings for
 * a long time) to 1 (critically damped: no overshoot, the quickest way there), above 1 is taken as 1. A spring in the physical sense
 * (the same equation, x'' = -2 z w x' - w^2 (x - 1), with w chosen from the duration); it does not carry a velocity into a
 * new target, so an animation restarted halfway starts from rest. */
float gp_ease_spring(float t, float damping);

/* A spring as a physical thing, for values that follow a target and may be given a new one at any moment (a panel that slides to where
 * the pointer went, a scroll position, a size): x'' = -k (x - target) - 2 z sqrt(k) x', mass 1, with `stiffness` k (the natural frequency is
 * sqrt(k) radians a second: 100 is a quick, 400 a snappy one) and `damping` the damping ratio z (below 1 it overshoots and rings, 1 is the
 * quickest way without overshoot, above 1 is sluggish). Unlike gp_ease_spring (a curve over a fixed time) it has a velocity, and a new target
 * changes the force, not the position or the velocity, so retargeting in mid-flight carries on smoothly. Keep it in a view's state. It is
 * advanced exactly (the closed-form solution of the equation), so any step, a long frame included, is stable. */
typedef struct gp_spring {
    float value, velocity, target;
    float stiffness, damping;
    bool running;          /* gp_spring_to has a frame job stepping it */
    int64_t last_ns;
} gp_spring;
gp_spring gp_spring_make(float value, float stiffness, float damping);
/* Moves it on by `dt` seconds. True while it is still moving; once the distance and the speed are both tiny it lands on the target exactly
 * and stops (false). */
bool gp_spring_step(gp_spring *s, double dt);
bool gp_spring_settled(const gp_spring *s);
/* Sets the target and has the spring stepped once a frame until it settles, notifying `cx`'s entity each frame (so the view that reads
 * `value` renders again), then nothing is asked for. Calling it again while it moves just changes the target. */
void gp_spring_to(gp_ctx *cx, gp_spring *s, float target);
/* Puts it at `value` at rest, with no target change pending (it stops being stepped by its next frame). */
void gp_spring_jump(gp_spring *s, float value);
static inline float gp_lerp(float a, float b, float t) { return a + (b - a) * t; }

/* Calls `fn(self, cx, eased_t, data)` every frame for `seconds`; the last call has t == 1.
 * Call gp_notify() inside to redraw. Cancel it to stop early. */
gp_task gp_animate(gp_ctx *cx, double seconds, int easing, gp_anim_fn fn, void *data);

/* ---- declarative animations ------------------------------------------------------------- */

/* GPUI's `with_animation`: an element that animates itself without its view rendering again.
 *
 *     static gp_node *pulse(float t, void *data) { return gp_opacity(gp_lerp(0.35f, 1.f, t)); }
 *     ...
 *     gp_div(gp_size_px(12), gp_radius(6), gp_bg(green),
 *            gp_with_animation("status-dot",
 *                              (gp_animation){ .duration = 1.2, .repeat = true, .ping_pong = true,
 *                                              .easing = GP_EASE_IN_OUT },
 *                              pulse, NULL))
 *
 * Every frame while it runs, `fn(t, data)` is called with the eased progress (0 to 1; GP_EASE_OUT_BACK
 * overshoots) and returns the modifiers to lay over the element's own: one (gp_opacity(t)) or several
 * (gp_group(gp_w(...), gp_bg(...))). Only style modifiers belong there (size, color, opacity, padding,
 * ...): they are applied to a fresh copy of the element's style each time, so nothing accumulates. Layout
 * and paint run again for the animated parts; the view itself does not render, and cached views
 * (gp_child_view_cached) stay cached. Nothing is drawn or woken when no animation is running, and a
 * finished one keeps showing its last value.
 *
 * `id` names the animation: the window remembers when it started under that name (and the position in
 * the element's list of animations), so rebuilding the element continues it. Give two animated elements
 * that are on screen together two ids; change the id to start again; an element that is absent from a
 * layout starts over when it comes back. Inside a gp_list row it also starts over when the row scrolls
 * out of range and back. */
typedef struct {
    double duration;      /* seconds for one run; 0 = jump to the end */
    double delay;         /* seconds before the first run (progress stays 0) */
    bool repeat;          /* run again and again */
    bool ping_pong;       /* with repeat: every other run goes backwards */
    int easing;           /* GP_EASE_* */
} gp_animation;
typedef gp_node *(*gp_anim_style_fn)(float t, void *data);
gp_node *gp_with_animation(const char *id, gp_animation animation, gp_anim_style_fn fn, void *data);

/* Background executor. `work(data)` runs on a pool thread and must not touch the app, entities
 * or elements. Afterwards `done(self, cx, data)` runs on the main thread (not if cancelled or
 * the entity is gone). `free_data`, if given, is called on the main thread exactly once after
 * that, whatever happened — and `data` stays valid until then, so cancelling never leaves a
 * running worker with dangling data. */
gp_task gp_background(gp_ctx *cx, gp_work_fn work, gp_task_fn done, void *data,
                      gp_drop_fn free_data);

/* The only thread-safe call: run `fn` on the main thread soon. For workers that want to report
 * progress. Dropped if `entity` (non-null) is gone by then. */
void gp_post(gp_app *app, gp_entity entity, gp_task_fn fn, void *data);

/* ---- file descriptors ------------------------------------------------------------------- */

/* Pipes, sockets, eventfds, inotify, a child's stdout: anything poll(2) understands. The event loop
 * wakes when a watched descriptor is ready (and only then: nothing is polled on a timer), and the
 * callback runs on the main thread like every other one here.
 *
 * Watching is level-triggered, as in poll(2): the callback runs again for as long as the descriptor
 * stays ready, so it must read (or write) until it would block, or cancel the watch. Set O_NONBLOCK
 * (gp_fd_set_nonblocking) on descriptors you read from the callback, or a read that finds nothing
 * blocks the whole UI.
 *
 * The descriptor stays yours: cancelling a watch (gp_cancel, or the owning entity being released)
 * does not close it. Cancel before you close, and do not reuse the number for a watch of the same
 * task. A descriptor closed under a live watch is a bug, but a survivable one: the next time the
 * watcher looks (any watch added or ended makes it look) the callback runs once with GP_FD_INVALID
 * and the watch ends. `events` passed to the callback is what happened: a hung-up pipe or socket
 * reports GP_FD_HUP (together with GP_FD_READ if there is still data), and GP_FD_ERROR on errors,
 * whichever you asked for. */
enum { GP_FD_READ = 1, GP_FD_WRITE = 2, GP_FD_ERROR = 4, GP_FD_HUP = 8, GP_FD_INVALID = 16 };
typedef void (*gp_fd_fn)(void *self, gp_ctx *cx, int fd, int events, void *data);

gp_task gp_watch_fd(gp_ctx *cx, int fd, int events, gp_fd_fn fn, void *data);
bool gp_fd_set_nonblocking(int fd);

/* ---- coroutines: async/await for C ------------------------------------------------------ */

/* A coroutine is sequential code that may suspend at await points while the event loop keeps
 * running, on its own small (256 KiB) stack:
 *
 *     static void fetch(gp_co *co, void *self, gp_ctx *cx, void *data) {
 *         Job job = { ... };
 *         if (!gp_sleep(co, 0.3)) return;                  // cancelled
 *         if (!gp_await_background(co, crunch, &job)) return;
 *         ((View *)self)->result = job.result;  gp_notify(cx);
 *     }
 *     gp_spawn(cx, fetch, NULL);
 *
 * Cancellation is cooperative: a cancelled coroutine's awaits return false immediately
 * (without suspending), and it should clean up and return. Do not use `self` after an await
 * returned false — the entity may be gone. If it is waiting for background work, cancellation
 * takes effect when that work finishes (its data lives on your stack). Coroutines run on the
 * main thread; calling a blocking function blocks the whole UI exactly as it would anywhere. */
typedef struct gp_co gp_co;
typedef void (*gp_co_fn)(gp_co *co, void *self, gp_ctx *cx, void *data);

gp_task gp_spawn(gp_ctx *cx, gp_co_fn fn, void *data);   /* starts on the next loop turn */
bool gp_sleep(gp_co *co, double seconds);
bool gp_yield(gp_co *co);                                /* let everything else run once */
bool gp_await_frame(gp_co *co, double *time);            /* resume at the next frame */
bool gp_await_background(gp_co *co, gp_work_fn work, void *data);
bool gp_co_cancelled(const gp_co *co);

/* I/O as await points. gp_await_fd suspends until `fd` is ready for `events` (GP_FD_*), or for
 * `timeout` seconds (negative = no limit); false if cancelled. *revents (may be NULL) receives what
 * happened, 0 if it timed out. The rest are the usual calls for a non-blocking descriptor
 * (gp_fd_set_nonblocking) that wait instead of failing with EAGAIN; they return false / -1 with
 * errno == ECANCELED if the coroutine is cancelled. */
bool gp_await_fd(gp_co *co, int fd, int events, double timeout, int *revents);
ssize_t gp_co_read(gp_co *co, int fd, void *buf, size_t len);          /* >= 1 bytes, 0 at end of file, -1 on error */
ssize_t gp_co_write(gp_co *co, int fd, const void *buf, size_t len);   /* all of it, or -1 */
int gp_co_accept(gp_co *co, int listen_fd);                            /* a non-blocking socket, or -1 */
int gp_co_connect(gp_co *co, int fd, const struct sockaddr *addr, socklen_t addrlen);   /* 0 or -1 */

/* ---------------------------------------------------------------- text input widget */

/* A ready-made editable text field: a view, embedded with gp_child_view(input).
 *
 *   input = gp_text_input_new(app, &(gp_text_input_options){
 *       .owner = me, .placeholder = "Your name", .on_change = changed, .on_submit = submitted });
 *
 * Single-line or multi-line (hard line breaks, scrolls instead of wrapping), UTF-8 aware, with
 * selection (shift+arrows, mouse drag, double-click word, triple-click line), word and line
 * movement, undo/redo, clipboard, IME composition, password and read-only modes, and a
 * blinking caret. Default key bindings are registered in the "TextInput" context the first
 * time one is created (ctrl- shortcuts; bind more to the text_input::* actions to change them).
 * Selection and caret are byte offsets into the UTF-8 text. */
enum { GP_TEXT_DIRECTION_AUTO = 0, GP_TEXT_DIRECTION_LTR, GP_TEXT_DIRECTION_RTL };
typedef void (*gp_text_input_event_fn)(void *self, gp_ctx *cx, gp_entity input, void *data);

typedef struct {
    gp_entity owner;                  /* entity whose state callbacks receive as `self` (may be null) */
    gp_text_input_event_fn on_change; /* text changed by the user (not by gp_text_input_set_text) */
    gp_text_input_event_fn on_submit; /* Enter (single-line) or Ctrl+Enter (multi-line) */
    void *data;
    const char *text;                 /* initial contents */
    const char *placeholder;
    bool multiline, password, read_only;
    bool soft_wrap;                   /* multi-line only: wrap long lines at the field's width */
    int direction;                    /* GP_TEXT_DIRECTION_*: auto (default) takes each paragraph's direction from its first strong character */
    int rows;                         /* visible lines when multiline (default 4) */
    size_t max_bytes;                 /* 0 = unlimited */
    float font_size;                  /* default 15 */
    float width;                      /* 0 = fill the parent */
    float radius, padding_x, padding_y;
    /* colors with alpha 0 get a dark-theme default */
    gp_color text_color, placeholder_color, bg, border_color, focus_border_color, selection_color,
        caret_color;
} gp_text_input_options;

gp_entity gp_text_input_new(gp_app *app, const gp_text_input_options *opts);
const char *gp_text_input_text(gp_app *app, gp_entity input);   /* valid until the next edit */
void gp_text_input_set_text(gp_ctx *cx, gp_entity input, const char *utf8); /* caret to end, clears undo */
void gp_text_input_selection(gp_app *app, gp_entity input, size_t *start, size_t *end);
void gp_text_input_select_all(gp_ctx *cx, gp_entity input);
const char *gp_text_input_preedit(gp_app *app, gp_entity input); /* current IME composition, "" if none */
gp_focus gp_text_input_focus(gp_app *app, gp_entity input);      /* for gp_focus_set */
gp_point gp_text_input_scroll(gp_app *app, gp_entity input);     /* how far the text is scrolled */
/* Visual rows: with soft_wrap a long line is several rows. Row `i` covers bytes [start, end) of the
 * text (a soft-wrapped row ends where the next begins; a hard line's last row ends at its '\n'). */
size_t gp_text_input_rows(gp_app *app, gp_entity input);
bool gp_text_input_row_range(gp_app *app, gp_entity input, size_t row, size_t *start, size_t *end);
size_t gp_text_input_caret_row(gp_app *app, gp_entity input);    /* the row the caret is drawn on */
/* Where the caret is drawn, in content coordinates (before scrolling). */
bool gp_text_input_caret_pos(gp_app *app, gp_entity input, float *x, float *y);
/* Where the caret's other place is, when it has two: at a boundary between text of one direction and text of the other (the end of the
 * character before, the start of the one after) a caret has two places on screen, and the field draws the one that typing goes to as the caret and this
 * one as a thinner, fainter one. false (x, y untouched) where there is one place: within a direction, at the edges of a soft-wrapped row,
 * with a selection or a composition. Same coordinates as gp_text_input_caret_pos. */
bool gp_text_input_caret_pos_alt(gp_app *app, gp_entity input, float *x, float *y);
/* The selection as drawn: one rectangle per visually contiguous piece (x, y, w, h). In
 * bidirectional text a selection can be several pieces. Returns how many (up to `max`). */
size_t gp_text_input_selection_rects(gp_app *app, gp_entity input, float (*rects)[4], size_t max);
bool gp_text_input_row_rtl(gp_app *app, gp_entity input, size_t row); /* does the row's paragraph read right to left? */

/* ---------------------------------------------------------------- frame helpers */

/* Valid until the next time the window's view tree is rebuilt. Call only while
 * building elements (inside a gp_render_fn). */
void *gp_frame_alloc(size_t size);
const char *gp_fmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---------------------------------------------------------------- elements */

extern gp_node gp_end_marker;
#define GP_END (&gp_end_marker)

gp_node *gp_div_(int unused, ...);
gp_node *gp_text_(const char *text, ...);
gp_node *gp_img_(gp_image image, ...);

/* ---------------------------------------------------------------- stroked curves */

/* A cable, a connector, a graph's edge: a cubic Bezier or a polyline stroked at a width, drawn by the GPU from a few instances (no bitmap, no upload), with hit testing on the
 * stroke only. Written like the other elements: `gp_curve(p0, c1, c2, p3, modifiers...)`, `gp_polyline(points, n, modifiers...)` (the points are copied).
 *
 * Coordinates: the points are in the PARENT's space, the one gp_left / gp_top use for an absolutely placed child (the origin is the top left of the parent's padding box, inside its
 * border): compute them from your own model, no layout is needed. The element places itself: it is absolute, its box is the bounding box of the points padded by half the
 * stroke width plus the hit tolerance (plus more for a miter join, see gp_miter_limit), and it takes no part in the layout of its siblings. Put it where the stroke should be in
 * the tree order (it paints and is hit in that order, like any element), inside a parent that covers the area. */
gp_node *gp_curve_(gp_point p0, gp_point c1, gp_point c2, gp_point p3, ...);
#define gp_curve(p0, c1, c2, p3, ...) gp_curve_((p0), (c1), (c2), (p3) __VA_OPT__(,) __VA_ARGS__, GP_END)
gp_node *gp_polyline_(const gp_point *pts, size_t n, ...);
#define gp_polyline(pts, n, ...) gp_polyline_((pts), (n) __VA_OPT__(,) __VA_ARGS__, GP_END)
#define gp_xy(px, py) GP_LIT(gp_point, (px), (py))   /* a point (not gp_pt: that is the padding at the top) */

/* How it is stroked (the stroke is `gp_stroke_color`, by default the text color, `gp_stroke_width` wide, by default 2 px). The ends are round or flat; the joins of a polyline round or
 * mitered (a Bezier's pieces are joined smoothly whatever is set); a miter longer than `gp_miter_limit` times the half width (default 4) falls back to a round join. The pixels are
 * covered exactly, with a soft edge like a rounded box's, and a translucent stroke is blended once everywhere, joints included (no darker spots where pieces meet). The Bezier is
 * cut into straight pieces to within 0.25 device pixels at the scale it is drawn at (the window's, times any transform above it). */
gp_node *gp_stroke_width(float px);
gp_node *gp_stroke_color(gp_color c);
gp_node *gp_stroke_cap_round(void);
gp_node *gp_stroke_cap_butt(void);
gp_node *gp_stroke_join_round(void);
gp_node *gp_stroke_join_miter(void);
gp_node *gp_miter_limit(float ratio);

/* Hit testing and the pointer: gp_on_click, gp_on_mouse_down, gp_on_drag, the hover and pressed styles (`gp_hover_bg(c)` and `gp_active_bg(c)` are the stroke's color while they hold, and
 * gp_hover_style(gp_group(gp_stroke_color(c)))) and the cursor work on the STROKE only: a point is on it when it is within half the width plus `gp_hit_tolerance` (default 0) of the
 * centerline (and, with butt caps, not beyond an end); everywhere else in the box the pointer goes on to what is below. Transforms and clips above it apply to the pixels and to the pointer.
 * The element has no children. Changing the points means building it again, which is the business of the view that builds it: put a cable in a cached view of its own
 * (gp_child_view_cached) and only that view is built and painted again when it moves. */
gp_node *gp_hit_tolerance(float px);

/* Dashes: `gp_stroke_dashes(8, 4)` is 8 px drawn, 4 px not, along the centerline (up to 8 lengths; an odd count is used twice, as in SVG; all zero is a solid stroke), each
 * dash capped as the stroke is (round or flat), starting `gp_stroke_dash_offset` into the pattern. The gaps are still the stroke's for the pointer (a dashed cable is as easy to
 * click as a solid one). */
gp_node *gp_stroke_dash(const float *pattern, int n);
#define gp_stroke_dashes(...) gp_stroke_dash((const float[]){ __VA_ARGS__ }, (int)(sizeof((const float[]){ __VA_ARGS__ }) / sizeof(float)))
gp_node *gp_stroke_dash_offset(float px);
/* The stroke's color along its length: `gp_stroke_gradient(gp_stop(red, 0), gp_stop(blue, 1))` goes from red at the first point to blue at the last, by distance along the line
 * (the stops as gp_bg_gradient's, interpolated the same way). A hover, pressed or focus color replaces it, as it does a background gradient. */
gp_node *gp_stroke_gradient_stops(const gp_color_stop *stops, int n);
#define gp_stroke_gradient(...) \
    gp_stroke_gradient_stops((gp_color_stop[]){ __VA_ARGS__ }, (int)(sizeof((gp_color_stop[]){ __VA_ARGS__ }) / sizeof(gp_color_stop)))
/* Arrowheads: a filled triangle `length` long (and as wide, or as wide as the stroke if that is wider) with its tip on the first or last point, pointing along the line there;
 * the stroke stops at its base, so the two meet without a seam and a translucent one is blended once. In the stroke's color (with a gradient, the color at that end). Part of the
 * stroke for the pointer. Not on a closed outline. */
gp_node *gp_arrow_start(float length);
gp_node *gp_arrow_end(float length);
/* Markers, as SVG's: a shape `size` px across centered on the first point, the last, or every corner between (a polyline's), turned along the line there (at a corner, half
 * way between the two directions). In the stroke's color (with a gradient, the color at that end; in the middle, the start's), drawn over the stroke and part of it for the
 * pointer. GP_MARKER_BAR is a line across the stroke, as thick as it and `size` long. Arrowheads are gp_arrow_start / gp_arrow_end. */
enum { GP_MARKER_NONE = 0, GP_MARKER_CIRCLE, GP_MARKER_SQUARE, GP_MARKER_DIAMOND, GP_MARKER_BAR };
gp_node *gp_marker_start(int shape, float size);
gp_node *gp_marker_end(int shape, float size);
gp_node *gp_marker_mid(int shape, float size);
/* A shape: gp_closed() joins the last point back to the first (the stroke goes round, with a join there, and has no ends), and gp_fill_color(c) fills the inside, under the
 * stroke, whether or not the stroke is closed (gp_stroke_width(0) for a fill alone). Concave outlines, ones that cross themselves and ones that go round more than once are
 * filled by the fill rule, as SVG's fill-rule: non-zero by default (a point is inside where the outline winds around it, counting directions), or gp_fill_evenodd() (where it
 * crosses the outline an odd number of times going out: a star drawn in one line has a hole in the middle). Inside the fill is the shape's for the pointer, by the same rule.
 * A Bezier is closed by a straight line. */
gp_node *gp_fill_color(gp_color c);
gp_node *gp_closed(void);
gp_node *gp_fill_evenodd(void);
gp_node *gp_fill_nonzero(void);
/* Text along the centerline, in the element's text style (gp_text_size, gp_text_color, the font), each glyph turned to the line where it stands, its baseline on the line
 * (moved `gp_path_text_dy` px to the right of the direction of travel: negative is above a line drawn left to right). gp_text_center / gp_text_right place it along the length
 * (from the start by default). Glyphs that do not fit are left out. Left to right only; not hit tested (the stroke is). The string is copied. */
gp_node *gp_path_text(const char *text);
gp_node *gp_path_text_dy(float px);
gp_node *gp_svg_img_(gp_svg svg, ...);
#define gp_div(...)        gp_div_(0 __VA_OPT__(,) __VA_ARGS__, GP_END)
#define gp_text(str, ...)  gp_text_((str) __VA_OPT__(,) __VA_ARGS__, GP_END)
#define gp_img(image, ...) gp_img_((image) __VA_OPT__(,) __VA_ARGS__, GP_END)
#define gp_svg_img(svg, ...) gp_svg_img_((svg) __VA_OPT__(,) __VA_ARGS__, GP_END)
/* How an image fills its box (content box: inside padding and border), the image centered in it:
 * FILL stretches it to the box; CONTAIN fits all of it inside, keeping its proportions; COVER fills the box, keeping its
 * proportions, and crops what overflows; NONE draws it at its own size, cropped if it is bigger; SCALE_DOWN is NONE for
 * an image that fits and CONTAIN for one that does not. Default FILL, as in CSS. */
enum { GP_FIT_FILL = 0, GP_FIT_CONTAIN, GP_FIT_COVER, GP_FIT_NONE, GP_FIT_SCALE_DOWN };
gp_node *gp_object_fit(int fit);

/* ---- SVG ----
 * A vector drawing is drawn sharp at any size: it is rasterized at the size, in device pixels, it is shown at (and again
 * when that changes), by a small C rasterizer (nanosvg), and the result is drawn like an image, so everything that applies
 * to gp_img applies to gp_svg_img: sizing from the drawing's own size (width and height, else its viewBox) with the proportions
 * kept when one side is given, gp_object_fit, padding, border, rounded corners, opacity. `currentColor` in the drawing is the
 * text color where it is shown (gp_text_color; its alpha is not used, but gp_opacity is), so one icon can be drawn in many colors.
 *
 *   gp_svg check = gp_svg_load_memory(app, "<svg width='24' height='24'><path d='M4 12l5 5 11-11' stroke='currentColor' fill='none' stroke-width='3'/></svg>", ...);
 *   gp_svg_img(check, gp_size_px(20), gp_text_color(green))
 *
 * Supported: paths (all commands), basic shapes, groups and transforms, fill and stroke with colors, opacity, linear and radial
 * gradients, fill-rule, joins, caps and dashes, <defs>, simple <style> classes. Not supported: text, <use>, <symbol>, clip paths,
 * masks, filters, patterns, markers, embedded images. A document with no width, height or viewBox is as big as its shapes; one over
 * 8 MB, empty, or absurdly large (over 100000 px) is refused. gp_svg_from_path draws just one path (a "d" attribute) in a width x height box: the vector-path primitive. */
gp_svg gp_svg_load_file(gp_app *app, const char *path);                            /* GP_SVG_NULL if unreadable or not an SVG */
gp_svg gp_svg_load_memory(gp_app *app, const char *svg, size_t size);              /* the document's text; copied */
gp_svg gp_svg_from_path(gp_app *app, const char *path_data, float width, float height, gp_color fill, gp_color stroke, float stroke_width);   /* alpha 0: none */
bool gp_svg_size(gp_app *app, gp_svg svg, float *width, float *height);           /* the drawing's own size in px */
void gp_svg_release(gp_app *app, gp_svg svg);                                      /* what showed it is drawn again, without it */
gp_node *gp_image_pixelated(void);
/* Filters for an image or an SVG, done in the shader (in this order: brightness, contrast, saturate, grayscale, tint), as in CSS:
 * grayscale 0..1; brightness, contrast and saturate are factors (1 = unchanged, 0 = black / mid gray / gray, above 1 = more);
 * tint multiplies the colors (white = unchanged); blur is a radius in logical px. */
gp_node *gp_image_grayscale(float amount);
gp_node *gp_image_brightness(float factor);
gp_node *gp_image_contrast(float factor);
gp_node *gp_image_saturate(float factor);
gp_node *gp_image_tint(gp_color color);
gp_node *gp_image_blur(float px);     /* nearest-neighbor when drawn larger than it is (pixel art), not smoothed */

/* Appends `count` elements from an array (NULLs skipped), for lists whose length is only
 * known at runtime — GPUI's `.children(iter)`. Allocate the array with gp_frame_alloc(). */
gp_node *gp_children(gp_node **nodes, size_t count);
/* Applies `count` modifiers from an array (elements and NULLs are ignored): for styles and
 * handlers assembled at runtime, e.g. one gp_on_action per entry of a table. */
gp_node *gp_mods(gp_node **nodes, size_t count);
/* Several modifiers as one, for the return value of a gp_with_animation function: gp_group(gp_opacity(t), gp_w(40 + t * 60)). */
gp_node *gp_group_(int unused, ...);
#define gp_group(...) gp_group_(0 __VA_OPT__(,) __VA_ARGS__, GP_END)

/* A virtualized list of equal-height rows (GPUI's uniform_list). No row exists until
 * layout knows the viewport: only the visible rows are built, by calling
 * render(self, cx, index, data) for each one, and rows are rebuilt as the list scrolls.
 * `self` is the state of the view that built the list, so handlers inside rows work
 * as usual. `item_height` <= 0 measures row 0 and uses that for every row. The list needs
 * a bounded height (gp_h, gp_flex_1, gp_max_h...), otherwise every row is "visible".
 * `data` must outlive the frame (use gp_frame_alloc or view state). Modifiers apply to the
 * list; pass gp_track_scroll() to control it. Rows inherit text style from the list. */
typedef gp_node *(*gp_list_item_fn)(void *self, gp_ctx *cx, size_t index, void *data);
gp_node *gp_uniform_list_(size_t count, float item_height, gp_list_item_fn render, void *data, ...);
#define gp_uniform_list(count, item_height, render, data, ...) \
    gp_uniform_list_((count), (item_height), (render), (data) __VA_OPT__(,) __VA_ARGS__, GP_END)

/* A virtualized list whose rows have different heights (GPUI's `list`). It is used like
 * gp_uniform_list, and the same rules hold (bounded height, `data` outlives the frame, rows
 * are built on behalf of the owning view), but a row is as tall as its content: the width is
 * the list's, the height is whatever the row lays out to. Heights are measured when a row is
 * first built and remembered by index, so the scroll extent is exact for rows that have been
 * seen and an estimate for the rest (the average of the measured ones; `estimated_height`
 * until there are any, <= 0 measures row 0 for that). Nothing jumps as estimates are replaced:
 * the scroll position is kept as "this much into this row", so rows above the viewport changing
 * height leave what is on screen where it is.
 *
 * The measurements are thrown away when the list's width changes. When rows change or the
 * count changes by more than appended/removed rows at the end, tell the list:
 *   gp_list_splice       rows [start, start+old_count) were replaced by new_count rows (insert,
 *                        remove, prepend older items). The row at the top of the viewport stays
 *                        put, even if the change is above it.
 *   gp_list_invalidate   the content of these rows changed; measure them again when next seen.
 *   gp_list_reset        forget everything measured (font size or text changed everywhere).
 * Rows that are on screen are always measured afresh, so a visible row that changes height
 * needs no call at all. All three need the list's gp_scroll handle and one layout to have happened. */
gp_node *gp_list_(size_t count, float estimated_height, gp_list_item_fn render, void *data, ...);
#define gp_list(count, estimated_height, render, data, ...) \
    gp_list_((count), (estimated_height), (render), (data) __VA_OPT__(,) __VA_ARGS__, GP_END)
void gp_list_splice(gp_ctx *cx, gp_scroll h, size_t start, size_t old_count, size_t new_count);
void gp_list_invalidate(gp_ctx *cx, gp_scroll h, size_t start, size_t count);
void gp_list_reset(gp_ctx *cx, gp_scroll h);
/* Modifier for a gp_list: while the list is scrolled to its end it stays there as rows are
 * added or grow (a chat log), and it starts at the end. Scrolling up releases it; scrolling
 * back to the end, or gp_scroll_to_end(), takes hold again. */
gp_node *gp_list_stick_to_end(void);

/* Renders `view` (an entity created with gp_view_new) inline, as GPUI's
 * `.child(view)` does. Its render function runs every time the view that contains it renders. */
gp_node *gp_child_view(gp_entity view);

/* The same, but a cached view (GPUI's `AnyView::cached`): while neither it nor any view built inside
 * it has called gp_notify, its render function is not called and the elements of the last render are
 * used again as they are. Use it for views that are costly to build and change rarely, so that
 * a change elsewhere in the window does not rebuild them.
 *
 * The cache is only as fresh as the notifications: a cached view must gp_notify (or observe something
 * that does, with gp_observe) whenever what its render function reads changes. A view reading shared
 * state it does not observe goes stale. The cache is dropped by gp_app_notify (fonts changed ...), and
 * for views that call gp_is_focused when focus moves. Handlers in the elements keep working: they belong
 * to the view, not to the render. A cached view placed twice in one window renders afresh for the second
 * place; one inside the rows of a gp_list or gp_uniform_list is not cached (rows are rebuilt as they scroll). */
gp_node *gp_child_view_cached(gp_entity view);

/* ---------------------------------------------------------------- layout modifiers */

/* Every div is a flex container. Default is a column (so plain divs stack like
 * blocks); gp_flex_row() switches the main axis. */
static inline gp_node *gp_flex(void) { return NULL; }
gp_node *gp_flex_row(void);
gp_node *gp_flex_col(void);
gp_node *gp_flex_1(void);          /* grow 1, shrink 1, basis 0 */
gp_node *gp_flex_none(void);       /* grow 0, shrink 0 */
gp_node *gp_flex_grow(void);
gp_node *gp_flex_shrink_0(void);
gp_node *gp_flex_basis(float px);

gp_node *gp_w(float px);
gp_node *gp_h(float px);
gp_node *gp_size_px(float px);     /* width and height */
gp_node *gp_w_frac(float f);       /* fraction of parent, 0..1 */
gp_node *gp_h_frac(float f);
gp_node *gp_w_full(void);
gp_node *gp_h_full(void);
gp_node *gp_size_full(void);
gp_node *gp_min_w(float px);
gp_node *gp_min_h(float px);
gp_node *gp_max_w(float px);
gp_node *gp_max_h(float px);

gp_node *gp_gap(float px);
gp_node *gp_p(float px);
gp_node *gp_px(float px);
gp_node *gp_py(float px);
gp_node *gp_pt(float px);
gp_node *gp_pr(float px);
gp_node *gp_pb(float px);
gp_node *gp_pl(float px);
gp_node *gp_m(float px);
gp_node *gp_mx(float px);
gp_node *gp_my(float px);
gp_node *gp_mt(float px);
gp_node *gp_mr(float px);
gp_node *gp_mb(float px);
gp_node *gp_ml(float px);
/* Auto margins, on flex items: an auto margin takes the free space of its axis. Along the main axis the free space is shared equally by the
 * auto margins (before gp_justify_*, which then has nothing left to place), so `gp_ml_auto()` on the last item of a row pushes it to the
 * right end and `gp_mr_auto()` on the first keeps it left with the rest at the right. Across, an auto margin on both sides centers the
 * item, on one side pushes it to the other (and the item is not stretched). With no free space they are zero. A later gp_m* sets a
 * number on that side again (the last one wins). Not on absolutely positioned elements. */
gp_node *gp_m_auto(void);
gp_node *gp_mx_auto(void);
gp_node *gp_my_auto(void);
gp_node *gp_ml_auto(void);
gp_node *gp_mr_auto(void);
gp_node *gp_mt_auto(void);
gp_node *gp_mb_auto(void);
/* Multi-line flex: the children go on as many lines (rows of a row container, columns of a column one) as the container's size along
 * the main axis needs, each line laid out as its own flex line (grow, shrink, justify, align), the lines stacked across with gp_gap
 * between them as well as between items. Needs a size along the main axis (a width for a row: the container's own or what it is
 * given); without one everything stays on one line. Not with a scroller along the main axis. Lines are as tall as their tallest
 * item, and where the container is taller than its lines (it has a height of its own) gp_align_content_* says what happens to the room:
 * stretch (the default, as in CSS) shares it among the lines, so their items that stretch fill them; start, center, end put the lines
 * together there; between, around, evenly spread them as justify does items. No room (or less than none) changes nothing. */
gp_node *gp_flex_wrap(void);
gp_node *gp_align_content_stretch(void);
gp_node *gp_align_content_start(void);
gp_node *gp_align_content_center(void);
gp_node *gp_align_content_end(void);
gp_node *gp_align_content_between(void);
gp_node *gp_align_content_around(void);
gp_node *gp_align_content_evenly(void);

gp_node *gp_items_start(void);
gp_node *gp_items_center(void);
gp_node *gp_items_end(void);
gp_node *gp_items_stretch(void);
gp_node *gp_self_start(void);
gp_node *gp_self_center(void);
gp_node *gp_self_end(void);
gp_node *gp_self_stretch(void);
gp_node *gp_justify_start(void);
gp_node *gp_justify_center(void);
gp_node *gp_justify_end(void);
gp_node *gp_justify_between(void);
gp_node *gp_justify_around(void);
gp_node *gp_justify_evenly(void);

/* Out of flow, positioned against the parent's padding box. */
gp_node *gp_absolute(void);
gp_node *gp_top(float px);
gp_node *gp_right(float px);
gp_node *gp_bottom(float px);
gp_node *gp_left(float px);
gp_node *gp_inset(float px);

gp_node *gp_overflow_hidden(void);
/* Clip and scroll. Wheel / touchpad scrolls the innermost container under the
 * pointer that can still move in that direction, then chains to its ancestors.
 * A main-axis scroller stops shrinking its children and lets them overflow. */
gp_node *gp_overflow_x_scroll(void);
gp_node *gp_overflow_y_scroll(void);
gp_node *gp_overflow_scroll(void);
gp_node *gp_track_scroll(gp_scroll h);
gp_node *gp_scrollbar_color(gp_color c);
gp_node *gp_hide_scrollbar(void);
/* Overlay-style scrollbars: hidden until the content scrolls (wheel, thumb, gp_scroll_to) or the mouse is over the
 * scroller, shown while either goes on, and faded out a second after. A hidden thumb cannot be grabbed; moving the
 * mouse over the scroller shows it again. Nothing is drawn or woken while they are hidden. */
gp_node *gp_scrollbar_autohide(void);

/* ---------------------------------------------------------------- paint modifiers */

gp_node *gp_bg(gp_color c);

/* ---- gradients ----
 * A gradient replaces the background color of a box and follows its corners and border like one (gp_rounded, a
 * border, opacity and shadows all work). Colors are interpolated the way CSS does it: in sRGB, premultiplied by alpha,
 * so a fade from a color to transparent does not pass through gray. A stop's `at` is its place along the gradient line, 0 to 1; a
 * negative `at` spreads the stops without one evenly between the neighbours that have one (the first defaults to 0, the last
 * to 1); positions that go backwards are held at the previous one, which makes a hard edge. Up to 16 stops; one stop
 * is a plain color. Under the mouse, gp_hover_bg / gp_active_bg replace the gradient, as they do a plain background.
 *
 *   gp_bg_gradient(90, gp_stop(gp_rgb(0xff0000), 0), gp_stop(gp_rgb(0x0000ff), 1))   left to right
 *   gp_bg_radial(0.5, 0.5, gp_stop(white, 0), gp_stop(black, 1))                      from the middle outwards */

/* ---- images ----
 * An image is decoded once into a handle and drawn by any number of elements, in any number of windows. gp_img is a
 * leaf element: it is as big as the image (one image pixel is one logical pixel) unless the style says otherwise; with only
 * a width or only a height it keeps the image's proportions, so `gp_img(i, gp_w_full())` in a column is as tall as the
 * picture needs at that width. gp_object_fit says what happens when the box and the image differ in shape. gp_rounded
 * rounds the image's corners; padding and a border go around it; gp_opacity fades it. Photos shown smaller than they are
 * are sampled from mipmaps, so they are not shimmery or jagged.
 *
 * Decoding (stb_image): PNG, JPEG (baseline and progressive), GIF (the first frame) and BMP, up to 16384 x 16384.
 * JPEG orientation (EXIF) is not applied. Pixels are premultiplied once, and kept in memory next to the GPU copy (the
 * texture is made when an image is first drawn, and again if the GL context is ever recreated).
 *
 *   gp_image logo = gp_image_load_file(app, "logo.png");        // in `ready`, or anywhere on the main thread
 *   ... gp_img(logo, gp_w(120), gp_rounded(12), gp_object_fit(GP_FIT_COVER)) ...
 *
 * A big picture should not stall the UI: gp_image_load_file_async decodes on a pool thread and calls you back. */
gp_image gp_image_load_file(gp_app *app, const char *path);                       /* GP_IMAGE_NULL if it can't be read or decoded */
gp_image gp_image_load_memory(gp_app *app, const void *data, size_t size);        /* the file's bytes; not kept */
gp_image gp_image_from_rgba(gp_app *app, int width, int height, const void *rgba); /* 4 bytes a pixel, straight (not premultiplied) alpha; copied */
/* Decodes on a pool thread. `done` runs on the main thread with the image, or GP_IMAGE_NULL on failure (it is not called
 * if the task was cancelled, or the entity of `cx` is gone: the image is dropped then). */
typedef void (*gp_image_fn)(void *self, gp_ctx *cx, gp_image image, void *data);
gp_task gp_image_load_file_async(gp_ctx *cx, const char *path, gp_image_fn done, void *data);
bool gp_image_size(gp_app *app, gp_image image, int *width, int *height);        /* false for a released or null image */
/* Frees the pixels and the texture. Elements that still show it draw nothing (and, laid out by it, are 0 x 0). */
void gp_image_release(gp_app *app, gp_image image);
#define gp_stop(color, at) GP_LIT(gp_color_stop, (color), (at))
/* `angle_deg` as in CSS: 0 points up, 90 right, 180 down; the line is as long as the box needs to reach its corners. */
gp_node *gp_bg_linear_gradient(float angle_deg, const gp_color_stop *stops, int n);
/* A circle around (cx, cy), fractions of the box (0.5, 0.5 is the middle), out to the farthest corner. */
gp_node *gp_bg_radial_gradient(float cx, float cy, const gp_color_stop *stops, int n);
/* Repeating gradients: the stops (positions along the line, or radius, as fractions) are one period and the pattern goes on in both
 * directions: gp_bg_repeating(90, stop(red, 0), stop(blue, 0.25)) is four red-to-blue ramps across the box, which jump back at each
 * edge (add a last stop equal to the first for a zigzag). Conic: colors go around (cx, cy), starting `from_deg` (0 = up) and turning
 * clockwise, 0..1 being the full circle. */
gp_node *gp_bg_repeating_gradient(float angle_deg, const gp_color_stop *stops, int n);
gp_node *gp_bg_repeating_radial_gradient(float cx, float cy, const gp_color_stop *stops, int n);
gp_node *gp_bg_conic_gradient(float cx, float cy, float from_deg, const gp_color_stop *stops, int n);
#define gp_bg_repeating(angle, ...) \
    gp_bg_repeating_gradient((angle), (gp_color_stop[]){ __VA_ARGS__ }, (int)(sizeof((gp_color_stop[]){ __VA_ARGS__ }) / sizeof(gp_color_stop)))
#define gp_bg_repeating_radial(cx, cy, ...) \
    gp_bg_repeating_radial_gradient((cx), (cy), (gp_color_stop[]){ __VA_ARGS__ }, (int)(sizeof((gp_color_stop[]){ __VA_ARGS__ }) / sizeof(gp_color_stop)))
#define gp_bg_conic(cx, cy, from_deg, ...) \
    gp_bg_conic_gradient((cx), (cy), (from_deg), (gp_color_stop[]){ __VA_ARGS__ }, (int)(sizeof((gp_color_stop[]){ __VA_ARGS__ }) / sizeof(gp_color_stop)))
#define gp_bg_gradient(angle, ...) \
    gp_bg_linear_gradient((angle), (gp_color_stop[]){ __VA_ARGS__ }, (int)(sizeof((gp_color_stop[]){ __VA_ARGS__ }) / sizeof(gp_color_stop)))
#define gp_bg_radial(cx, cy, ...) \
    gp_bg_radial_gradient((cx), (cy), (gp_color_stop[]){ __VA_ARGS__ }, (int)(sizeof((gp_color_stop[]){ __VA_ARGS__ }) / sizeof(gp_color_stop)))
gp_node *gp_border(float px);
gp_node *gp_border_color(gp_color c);
/* A color for one side (or two: _x is left and right, _y top and bottom); the sides not named keep gp_border_color's, and a gp_border_color
 * after them sets all four again (the last one wins). Where two sides of different colors meet the join is the diagonal from the outer corner to
 * the inner one, as in CSS, with its pixels shared by the two colors exactly (no gap, no overlap). */
gp_node *gp_border_color_top(gp_color c);
gp_node *gp_border_color_right(gp_color c);
gp_node *gp_border_color_bottom(gp_color c);
gp_node *gp_border_color_left(gp_color c);
gp_node *gp_border_color_x(gp_color c);
gp_node *gp_border_color_y(gp_color c);
/* Dashed or dotted instead of solid. Dashes are 2 widths long with a gap of 1 (a period of 3 widths), dots are circles of the border's width
 * 2 widths apart; both start at the top left corner and run along each side by its own width (a corner is not specially joined: the pattern just
 * goes on), and the gaps show the element's fill. Anti-aliased. */
gp_node *gp_border_solid(void);
gp_node *gp_border_dashed(void);
gp_node *gp_border_dotted(void);
/* The border painted with a gradient over the whole box (the border is where the gradient shows), instead of a color: the same stops and geometry
 * as gp_bg_gradient, linear, radial or conic. Not repeating, and per-side colors do not apply with it; dashes and dots do. A focus border color
 * replaces it while focused. */
gp_node *gp_border_linear_gradient(float angle_deg, const gp_color_stop *stops, int n);
gp_node *gp_border_radial_gradient(float cx, float cy, const gp_color_stop *stops, int n);
gp_node *gp_border_conic_gradient(float cx, float cy, float from_deg, const gp_color_stop *stops, int n);
#define gp_border_gradient(angle, ...) \
    gp_border_linear_gradient((angle), (gp_color_stop[]){ __VA_ARGS__ }, (int)(sizeof((gp_color_stop[]){ __VA_ARGS__ }) / sizeof(gp_color_stop)))
#define gp_border_radial(cx, cy, ...) \
    gp_border_radial_gradient((cx), (cy), (gp_color_stop[]){ __VA_ARGS__ }, (int)(sizeof((gp_color_stop[]){ __VA_ARGS__ }) / sizeof(gp_color_stop)))
#define gp_border_conic(cx, cy, from_deg, ...) \
    gp_border_conic_gradient((cx), (cy), (from_deg), (gp_color_stop[]){ __VA_ARGS__ }, (int)(sizeof((gp_color_stop[]){ __VA_ARGS__ }) / sizeof(gp_color_stop)))
gp_node *gp_rounded(float px);
/* Corners one at a time: gp_rounded_corners(top-left, top-right, bottom-right, bottom-left); gp_rounded_top / _bottom / _left /
 * _right round two. A corner not named takes gp_rounded's value, and gp_rounded after them sets all four again (the last one wins).
 * Radii that do not fit are scaled down together, as in CSS, so two neighbors never overlap. */
gp_node *gp_rounded_corners(float tl, float tr, float br, float bl);
gp_node *gp_rounded_top(float px);
gp_node *gp_rounded_bottom(float px);
gp_node *gp_rounded_left(float px);
gp_node *gp_rounded_right(float px);
/* Borders on some sides only. gp_border sets all four and replaces earlier sides; these set one, two or none of them and a side
 * that was not named keeps gp_border's width (zero if there was none). The border is inside the box, like padding (and is added to the
 * padding for layout), so an element with gp_border_left(4) has its content 4 px further in; padding and borders can be given in any
 * order. One color for all sides (gp_border_color). */
gp_node *gp_border_left(float px);
gp_node *gp_border_top(float px);
gp_node *gp_border_right(float px);
gp_node *gp_border_bottom(float px);
gp_node *gp_border_x(float px);      /* left and right */
gp_node *gp_border_y(float px);      /* top and bottom */
gp_node *gp_rounded_full(void);
gp_node *gp_shadow(float dx, float dy, float blur, gp_color c);
/* Fades the element and everything in it as one thing, as CSS's opacity does: what it paints is drawn into an offscreen layer that is put down at `o`, so a
 * child over its parent's background shows the child faded, not the two mixed. An element with only one thing to paint (a box, a line of text) needs no layer and takes
 * none. Text inside a layer is antialiased in gray (LCD stripes need what is under them). Layers nest 8 deep; deeper ones are faded piece by piece. */
gp_node *gp_opacity(float o);

/* Filters on an element, as CSS's `filter`: the element and everything in it, drawn into a layer, then changed as a picture, in the order given (each modifier adds one,
 * so gp_filter_grayscale(1) then gp_filter_brightness(1.2) is not the same as the other way round). The color ones work on each pixel's color as CSS defines them:
 * brightness, contrast and saturate take a factor (1: unchanged), grayscale, sepia, invert and opacity an amount from 0 (unchanged) to 1, hue_rotate an angle in degrees.
 * gp_filter_blur blurs it (a Gaussian, px is its standard deviation) and gp_filter_drop_shadow puts a shadow under the shape of what is drawn (its alpha, not its box,
 * so text and icons get theirs; `blur` as in gp_shadow, the offset in screen px). The pointer is not affected: a blurred button is clicked where its box is.
 * A filter on a state style (gp_hover_style) replaces the element's list. */
gp_node *gp_filter_blur(float px);
gp_node *gp_filter_brightness(float factor);
gp_node *gp_filter_contrast(float factor);
gp_node *gp_filter_saturate(float factor);
gp_node *gp_filter_grayscale(float amount);
gp_node *gp_filter_sepia(float amount);
gp_node *gp_filter_invert(float amount);
gp_node *gp_filter_hue_rotate(float degrees);
gp_node *gp_filter_opacity(float amount);
gp_node *gp_filter_drop_shadow(float dx, float dy, float blur, gp_color c);
/* Filters on what is behind the element, as CSS's backdrop-filter: `gp_backdrop_filter(gp_group(gp_filter_blur(12), gp_filter_saturate(1.4f)))` behind a translucent
 * panel is frosted glass. What was drawn under the element's box (inside its nearest group, or the window) is filtered and drawn in the box (its rounded corners too), under
 * the element's own background and content, and faded with it. The group's filters, in order; a blur reads past the box as far as it spreads (the window's edge repeats). */
gp_node *gp_backdrop_filter(gp_node *filters);

/* How the element (and what is in it, as one picture) is put on what is under it: CSS's mix-blend-mode, with what is under it inside its nearest group (an ancestor with
 * gp_opacity, a filter, a mask or a blend mode of its own), or the window. */
enum { GP_BLEND_NORMAL = 0, GP_BLEND_MULTIPLY, GP_BLEND_SCREEN, GP_BLEND_OVERLAY, GP_BLEND_DARKEN, GP_BLEND_LIGHTEN, GP_BLEND_COLOR_DODGE, GP_BLEND_COLOR_BURN,
       GP_BLEND_HARD_LIGHT, GP_BLEND_SOFT_LIGHT, GP_BLEND_DIFFERENCE, GP_BLEND_EXCLUSION, GP_BLEND_HUE, GP_BLEND_SATURATION, GP_BLEND_COLOR, GP_BLEND_LUMINOSITY };
gp_node *gp_blend_mode(int mode);

/* A mask, as CSS's mask-image with any element for the image: `mask` is laid out over the element's border box (as an absolutely placed child filling it would be),
 * painted, and where it is transparent the element is not seen (its alpha multiplies the element's; with gp_mask_luminance, its luminance times its alpha does, as
 * mask-mode: luminance). A gradient that fades a list's bottom edge: gp_mask(gp_div(gp_size_full(), gp_bg_gradient(180, gp_stop(white, .8f), gp_stop(transparent, 1))))
 * (any color: only the alpha counts). Applied after the filters, and before the opacity and blend mode. Not hit and not animated by gp_with_animation; the pointer is not
 * affected by it. */
gp_node *gp_mask(gp_node *mask);
gp_node *gp_mask_luminance(void);

gp_node *gp_hover_bg(gp_color c);
gp_node *gp_active_bg(gp_color c);
gp_node *gp_cursor_pointer(void);
gp_node *gp_cursor_text(void);          /* I-beam */

/* Text properties inherit down the tree, like CSS. */
gp_node *gp_text_color(gp_color c);
gp_node *gp_text_size(float px);
gp_node *gp_font_bold(void);            /* weight 700 */
gp_node *gp_font_normal(void);          /* weight 400 */
/* Weight 1..1000 (CSS): 100 thin, 300 light, 400 normal, 500 medium, 600 semibold, 700 bold, 900 black.
 * A family that has no face of exactly this weight draws the nearest one (see "fonts" above). */
gp_node *gp_font_weight(int weight);
enum { GP_WEIGHT_THIN = 100, GP_WEIGHT_EXTRA_LIGHT = 200, GP_WEIGHT_LIGHT = 300, GP_WEIGHT_NORMAL = 400,
       GP_WEIGHT_MEDIUM = 500, GP_WEIGHT_SEMIBOLD = 600, GP_WEIGHT_BOLD = 700, GP_WEIGHT_EXTRA_BOLD = 800,
       GP_WEIGHT_BLACK = 900 };
/* A font-family list, like CSS: "Inter, Noto Sans, sans-serif". Names are tried in order, and a character
 * the first one lacks is drawn from the next. The generic families are "sans-serif" (the default, also
 * "system-ui"), "serif" and "monospace"; a list that names none ends in the default. A family that is not
 * installed is skipped. Inherited by the text below. */
gp_node *gp_font_family(const char *families);
/* Italic, inherited like the rest; gp_font_upright() undoes it inside. A family with an italic (or oblique) face
 * draws it, chosen by weight like any other; one without is slanted: the upright face sheared by 14 degrees
 * when rasterized, which is what browsers do too. It combines with synthetic bold. Advances are unchanged. */
gp_node *gp_font_italic(void);
/* OpenType features of the text below, in HarfBuzz's syntax: "tnum" (on), "liga=0" or "-liga" (off), "ss01",
 * "cv02=3", "aalt=2" (a feature's value), separated by commas. A font only does what it has (Noto Sans: tnum,
 * pnum, onum, lnum, smcp, c2sc, frac, zero, ...; ligatures and kerning are on by default). Ranges ("liga[3:5]") are
 * ignored: a feature applies to the whole text. Inherited, and a list replaces the inherited one rather than
 * adding to it, as in CSS; gp_font_features("normal") turns everything back to the default. */
gp_node *gp_font_features(const char *features);
/* font-variation-settings: the axes of a variable font other than weight (which gp_font_weight sets), by tag and value as in CSS:
 * gp_font_variations("\"opsz\" 32, \"wdth\" 87.5") or `opsz=32, wdth=87.5`. A value is clamped to the axis's range and kept to the
 * nearest half (so that animating one does not make a face per frame); an axis the font does not have is ignored, and a list that names
 * none of a font's axes changes nothing. A list sets "wght" over gp_font_weight. Inherited, and replacing, not adding to, the list
 * above (as in CSS); gp_font_variations("normal") puts every axis back. Up to 8 axes in a list. */
gp_node *gp_font_variations(const char *settings);
gp_node *gp_font_tabular_nums(void);    /* "tnum": digits of one width, for columns of numbers */
gp_node *gp_font_small_caps(void);      /* "smcp" */
gp_node *gp_font_no_ligatures(void);    /* "liga=0, clig=0" */
gp_node *gp_font_upright(void);
gp_node *gp_font_sans(void);            /* gp_font_family("sans-serif") */
gp_node *gp_font_serif(void);           /* gp_font_family("serif") */
gp_node *gp_font_mono(void);            /* gp_font_family("monospace") */
gp_node *gp_text_left(void);
gp_node *gp_text_center(void);
gp_node *gp_text_right(void);
/* Alignment relative to the paragraph direction: `start` is the left edge of an LTR paragraph and
 * the right edge of an RTL one (the default); `end` is the opposite. */
gp_node *gp_text_start(void);
gp_node *gp_text_end(void);

/* Bidirectional text. Text is reordered per UAX #9 and shaped by direction automatically; this only
 * chooses each paragraph's base direction. Auto (the default) takes it from the first strong
 * character, so a Hebrew paragraph is right-to-left and a Latin one left-to-right, with digits
 * and punctuation not deciding. Inherited, like the other text properties. A paragraph runs to the
 * next line break. gp_text_dir_rtl() on a container also mirrors its flex layout (a row starts at the right, a column's items_start is the
 * right edge; margins, paddings and insets stay physical); see the README. Auto is left to right for layout. */
gp_node *gp_text_dir_auto(void);
gp_node *gp_text_dir_ltr(void);
gp_node *gp_text_dir_rtl(void);

/* ---------------------------------------------------------------- input modifiers */

/* Focus: tracked elements take focus on click; tab stops join the Tab cycle
 * (tree order, or ascending gp_tab_index). */
gp_node *gp_track_focus(gp_focus f);
gp_node *gp_tab_stop(void);
gp_node *gp_tab_index(int index);
gp_node *gp_focus_bg(gp_color c);
gp_node *gp_focus_border_color(gp_color c);
/* A style for a state, over the element's own: `gp_hover_style(gp_group(gp_bg(c), gp_opacity(.8f)))` while the pointer is over it (also
 * `gp_active_style` while it is pressed on top of that, `gp_focus_style` while it has focus). What can change: background (color or gradient), border
 * colors, border style and gradient, rounded corners, shadow, opacity, and text color (gp_text_color: the element's text and everything in it that does
 * not set a color of its own, as inheritance goes; a stroke whose color is the text color too); and what lays out: sizes (gp_w, gp_h, the minimums and
 * maximums, flex grow, shrink and basis), padding, margins, insets, border widths, gap, direction and alignment, wrapping, and the font (size, weight,
 * family, style, features, variations, alignment, direction: inherited by the text inside, as the element's own would be). The element is laid out
 * again in the state, and what is around it moves; a modifier that is neither (a handler, a scroller, a transform, the cursor) is ignored there.
 * The states stack (hover, then pressed, then focus: a later one's value wins), and gp_hover_bg / gp_active_bg / gp_focus_bg / gp_focus_border_color
 * still work and win over these.
 *
 * Which elements are hovered for layout is what the frame painted before showed: as in a browser, a style that grows the element keeps the pointer over
 * it, and one that moves it away (shrinking it, pushing a child from under the pointer with padding) leaves it, so it is laid out out of the state again.
 * The frame is drawn once the two agree; one that keeps going back and forth is laid out only a few times while the pointer, the button, the focus
 * and the scroll offsets stay as they are, and shows the last of those until one of them changes. An element is known by its place in the tree, so
 * one that moves to another place (a sibling before it comes or goes) starts out of its state for a frame. */
/* Transforms, as in CSS: they move what is painted, not the layout (nothing around the element moves or resizes), and apply to the element and everything in it.
 * They are made about the middle of the element's box (gp_transform_origin(fx, fy) moves that point: fractions of the box, 0 0 its top left corner), in the order
 * skew, scale, rotate, translate; a later one of the same kind replaces an earlier one. Nested elements compose. An angle is in degrees, clockwise (the y axis goes down).
 * The pointer follows them: a click or a hover lands on what is seen where it is. Meant for gp_with_animation (a spinner: gp_rotate(t * 360), a pop-in:
 * gp_scale(0.8 + 0.2 * t)). Text is drawn rotated without LCD antialiasing. An overflow clip (and so a scroller) inside a transformed element is cut to the transformed box only
 * when the transform keeps the axes (a scale, a move, a quarter turn); at another angle it does not clip. */
gp_node *gp_rotate(float degrees);
gp_node *gp_scale(float s);
gp_node *gp_scale_xy(float sx, float sy);
gp_node *gp_translate(float dx, float dy);
/* A skew, as CSS skew(ax, ay): x moves by tan(ax) times y, and y by tan(ay) times x, about the transform origin (angles in degrees, held within 89). The pointer follows it;
 * edges are anti-aliased as under a scale that is not the same both ways (the soft edge is a little narrower across a steeply sheared side), and a clip inside a skewed
 * element is not cut (as at an angle that is not a quarter turn). */
gp_node *gp_skew(float ax_deg, float ay_deg);
gp_node *gp_transform_origin(float fx, float fy);
/* 3D, as CSS's flat transform style: the element (a plane) is turned about x or y (degrees: with y down, a positive gp_rotate_y takes its right side away from the
 * viewer, a positive gp_rotate_x its bottom), moved toward the viewer (gp_translate_z), and seen in perspective, either its own (gp_transform_perspective(d): as CSS's
 * perspective() at the end of the transform list) or its parent's: gp_perspective(d) on a parent (CSS's perspective property) puts every child's transform in perspective
 * from the point gp_perspective_origin(fx, fy) of the parent's box (default its middle) at the distance d. The order, about the transform origin: skew, scale, rotate_x,
 * rotate_y, rotate (about z), translate (x, y and z), then the perspective. Nested 3D transforms are flattened at each element, as in CSS's default (no preserve-3d: no
 * depth sorting between siblings); what is in the element is drawn on its plane, text and images included. Without a perspective a turn about x or y only foreshortens.
 * The pointer follows: it lands where the turned element is seen. A part that would be at or behind the viewer is not drawn. An overflow clip inside an element in
 * perspective does not clip (as at an angle). gp_backface_hidden(): seen from behind (turned past 90 degrees), the element and what is in it are not drawn or hit
 * (a card's two faces, one turned 180 degrees). */
gp_node *gp_rotate_x(float degrees);
gp_node *gp_rotate_y(float degrees);
gp_node *gp_translate_z(float px);
gp_node *gp_transform_perspective(float px);
gp_node *gp_perspective(float px);
gp_node *gp_perspective_origin(float fx, float fy);
gp_node *gp_backface_hidden(void);
/* What the element is known by among its siblings, instead of its place among them (React's key): its hover, pressed and focus layout states, a scroller's offset and
 * the like follow it when siblings before it come and go. An element that tracks focus (gp_track_focus) is known by its focus already. Unique among siblings. */
gp_node *gp_key(const char *key);
gp_node *gp_key_id(uint64_t key);
gp_node *gp_hover_style(gp_node *group);
gp_node *gp_active_style(gp_node *group);
gp_node *gp_focus_style(gp_node *group);

/* Keyboard. Handlers run on the focused element, then up through its ancestors. */
gp_node *gp_key_context(const char *name);
gp_node *gp_on_action(gp_action action, gp_action_fn fn);
gp_node *gp_on_action_data(gp_action action, gp_action_fn fn, void *data);
gp_node *gp_on_key_up(gp_key_fn fn);
gp_node *gp_on_key_up_data(gp_key_fn fn, void *data);
gp_node *gp_on_key_down(gp_key_fn fn);
gp_node *gp_on_key_down_data(gp_key_fn fn, void *data);
/* Only elements with this handler enable text input (and IME) when focused. */
gp_node *gp_on_text_input(gp_text_input_fn fn);
gp_node *gp_on_text_input_data(gp_text_input_fn fn, void *data);

/* Text elements: keep spaces and only break on '\n' (for editable text). */
gp_node *gp_whitespace_pre(void);

gp_node *gp_on_click(gp_mouse_fn fn);
gp_node *gp_on_click_data(gp_mouse_fn fn, void *data);
gp_node *gp_on_mouse_down(gp_mouse_fn fn);
gp_node *gp_on_mouse_down_data(gp_mouse_fn fn, void *data);
/* If the element has a mouse-down handler and the press lands on it, the element captures
 * the mouse until release: every move goes to on_drag and the release to on_mouse_up, with
 * `local` relative to the element, even outside it, and `mods` the modifiers held at that moment.
 * The capture keeps the handlers (and their data) of the element pressed until the release, however
 * the tree changes meanwhile: rebuilt, moved among its siblings, or gone. */
gp_node *gp_on_drag(gp_mouse_fn fn);
gp_node *gp_on_drag_data(gp_mouse_fn fn, void *data);
gp_node *gp_on_mouse_up(gp_mouse_fn fn);
gp_node *gp_on_mouse_up_data(gp_mouse_fn fn, void *data);
/* The pointer entered (`inside` true) or left (false) the element: its hit area (as for the hover style: through its transforms and clips, on the
 * stroke for a curve), or that of anything inside it. Entering a child does not leave its parent (mouseenter and mouseleave, not mouseover and
 * mouseout); leaving the window leaves everything. Also called when the tree changes under a pointer that did not move: an element that appears
 * under it is entered, one that goes away (or moves away) is left, with the handler (and the data) it had when it was entered. The element is
 * known by its place in the tree, or its gp_key: give one to an element whose siblings come and go, or another element that takes its place
 * is taken for it (if its handler differs, the one is left and the other entered). Leaves come first (innermost first), then entries
 * (outermost first), just before the frame they change is painted. */
typedef void (*gp_hover_fn)(void *self, gp_ctx *cx, bool inside, void *data);
gp_node *gp_on_hover(gp_hover_fn fn);
gp_node *gp_on_hover_data(gp_hover_fn fn, void *data);

/* Called after every layout with the element's final (border-box) size. Lets an element react to
 * the space it was given, which only layout can decide (a text field wrapping at its width). If
 * the handler calls gp_notify() the view tree is rebuilt and laid out again before anything is
 * painted (a few passes at most), so the first frame is already right. Only notify when something
 * actually changed, or you will rebuild every frame. */
typedef void (*gp_layout_fn)(void *self, gp_ctx *cx, gp_size size, void *data);
gp_node *gp_on_layout(gp_layout_fn fn);
/* Called with a scroller's offset whenever it moves (wheel, thumb, track click, scroll-into-view, smooth steps) and after every layout
 * (which is where gp_scroll_to and a content change take effect), so an element that draws only what is in view can notice that the
 * view left what it drew: gp_notify from the handler rebuilds, and the new tree is laid out before anything is painted, as with
 * gp_on_layout. It runs often (once per layout pass): do nothing unless something changed. Only on an element that scrolls. */
typedef void (*gp_scroll_fn)(void *self, gp_ctx *cx, gp_point offset, void *data);
gp_node *gp_on_scroll(gp_scroll_fn fn);

/* The mouse wheel (or touchpad scroll) turned with the pointer over the element: `dx`, `dy` are in logical pixels, the way scroll offsets grow (dy > 0 is "down": the content
 * would move up; dx > 0 is "right"; shift + wheel arrives as dx), `mods` the GP_MOD_* held. The innermost element under the pointer that has a handler gets the event, BEFORE
 * any scroller does, and it is used up: nothing scrolls, unless the handler calls gp_propagate(cx), which passes it on to the next handler under the pointer and then to
 * scrolling as if there had been none. Hit testing is the element's own: on a curve only the stroke counts, under a transform the transformed shape. `local` is where the pointer is in
 * the element's own coordinates. Touch scrolling and flings do not come through here. */
typedef struct {
    gp_point position;   /* window coordinates, logical pixels */
    gp_point local;      /* position relative to the element */
    float dx, dy;
    uint32_t mods;
} gp_wheel_event;
typedef void (*gp_wheel_fn)(void *self, gp_ctx *cx, const gp_wheel_event *ev, void *data);
gp_node *gp_on_wheel(gp_wheel_fn fn);
gp_node *gp_on_wheel_data(gp_wheel_fn fn, void *data);

/* Composition events, delivered to the focused element (or its ancestors) that also has
 * gp_on_text_input. gp_ime_rect tells the platform where to put the candidate window, in
 * coordinates relative to this element (typically the caret). */
gp_node *gp_on_ime(gp_ime_fn fn);
gp_node *gp_ime_rect(float x, float y, float w, float h);

#ifdef __cplusplus
}
#endif

#endif /* GPUIC_H */
