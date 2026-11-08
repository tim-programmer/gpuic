/*
 * Keyboard focus and actions: a text field edited through actions bound in a
 * "NameField" key context, focusable buttons, Tab / Shift-Tab navigation,
 * Escape bubbling up to a handler on the root, and a global Ctrl-Q.
 */
#include <gpuic/gpuic.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BASE     gp_rgb(0x1e1e2e)
#define MANTLE   gp_rgb(0x181825)
#define SURFACE  gp_rgb(0x313244)
#define OVERLAY  gp_rgb(0x45475a)
#define TEXT     gp_rgb(0xcdd6f4)
#define SUBTEXT  gp_rgb(0x9399b2)
#define BLUE     gp_rgb(0x89b4fa)
#define BLUE_HI  gp_rgb(0xa6c4fb)
#define BLUE_LO  gp_rgb(0x6c9ef0)
#define ROSEWATER gp_rgb(0xf5e0dc)
#define GREEN    gp_rgb(0xa6e3a1)

typedef struct {
    gp_focus field_focus, dec_focus, inc_focus;
    char name[128];
    int caret;          /* byte offset into name */
    int count;
    char status[160];
    bool caret_on;      /* blink phase */
    gp_task blink, status_timer;
} Form;

/* ---------------------------------------------------------------- caret blink */

static void on_blink(void *self, gp_ctx *cx, void *data) {
    Form *f = self;
    f->caret_on = !f->caret_on;
    if (gp_is_focused(cx, f->field_focus)) gp_notify(cx); /* nothing to redraw otherwise */
}

/* Any edit or caret move makes the caret solid and restarts the blink, like real editors. */
static void touched(Form *f, gp_ctx *cx) {
    f->caret_on = true;
    gp_cancel(cx->app, f->blink);
    f->blink = gp_every(cx, 0.53, on_blink, NULL);
}

/* ---------------------------------------------------------------- text field actions */

static int utf8_prev(const char *s, int i) {
    do i--; while (i > 0 && ((unsigned char)s[i] & 0xc0) == 0x80);
    return i;
}

static int utf8_next(const char *s, int i) {
    do i++; while (s[i] && ((unsigned char)s[i] & 0xc0) == 0x80);
    return i;
}

static void on_text(void *self, gp_ctx *cx, const char *utf8, void *data) {
    Form *f = self;
    size_t n = strlen(utf8), len = strlen(f->name);
    if (len + n >= sizeof f->name) return;
    memmove(f->name + f->caret + n, f->name + f->caret, len - (size_t)f->caret + 1);
    memcpy(f->name + f->caret, utf8, n);
    f->caret += (int)n;
    touched(f, cx);
    gp_notify(cx);
}

static void backspace(void *self, gp_ctx *cx, void *data) {
    Form *f = self;
    if (f->caret == 0) return;
    int from = utf8_prev(f->name, f->caret);
    memmove(f->name + from, f->name + f->caret, strlen(f->name) - (size_t)f->caret + 1);
    f->caret = from;
    touched(f, cx);
    gp_notify(cx);
}

static void delete_fwd(void *self, gp_ctx *cx, void *data) {
    Form *f = self;
    if (!f->name[f->caret]) return;
    int to = utf8_next(f->name, f->caret);
    memmove(f->name + f->caret, f->name + to, strlen(f->name) - (size_t)to + 1);
    touched(f, cx);
    gp_notify(cx);
}

static void caret_left(void *self, gp_ctx *cx, void *data) {
    Form *f = self;
    if (f->caret > 0) f->caret = utf8_prev(f->name, f->caret);
    touched(f, cx);
    gp_notify(cx);
}

static void caret_right(void *self, gp_ctx *cx, void *data) {
    Form *f = self;
    if (f->name[f->caret]) f->caret = utf8_next(f->name, f->caret);
    touched(f, cx);
    gp_notify(cx);
}

static void caret_home(void *self, gp_ctx *cx, void *data) {
    ((Form *)self)->caret = 0;
    touched(self, cx);
    gp_notify(cx);
}

static void caret_end(void *self, gp_ctx *cx, void *data) {
    Form *f = self;
    f->caret = (int)strlen(f->name);
    touched(f, cx);
    gp_notify(cx);
}

static void clear(void *self, gp_ctx *cx, void *data) {
    Form *f = self;
    f->name[0] = 0;
    f->caret = 0;
    touched(f, cx);
    gp_notify(cx);
}

static void clear_status(void *self, gp_ctx *cx, void *data) {
    ((Form *)self)->status[0] = 0;
    gp_notify(cx);
}

static void submit(void *self, gp_ctx *cx, void *data) {
    Form *f = self;
    snprintf(f->status, sizeof f->status, "Saved \"%s\"", f->name);
    gp_cancel(cx->app, f->status_timer); /* a second save restarts the countdown */
    f->status_timer = gp_after(cx, 2.0, clear_status, NULL);
    gp_notify(cx);
}

/* ---------------------------------------------------------------- everything else */

static void press(void *self, gp_ctx *cx, void *data) {
    ((Form *)self)->count += (int)(intptr_t)data;
    gp_notify(cx);
}

static void click(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    press(self, cx, data);
}

static void blur(void *self, gp_ctx *cx, void *data) { gp_blur(cx); }

static void quit(void *self, gp_ctx *cx, void *data) { gp_quit(cx->app); }

static gp_node *text_field(Form *f, gp_ctx *cx) {
    bool focused = gp_is_focused(cx, f->field_focus);
    bool empty = f->name[0] == 0;
    return gp_div(
        gp_track_focus(f->field_focus), gp_tab_stop(), gp_key_context("NameField"),
        gp_on_text_input(on_text),
        gp_on_action("input::backspace", backspace),
        gp_on_action("input::delete", delete_fwd),
        gp_on_action("input::left", caret_left),
        gp_on_action("input::right", caret_right),
        gp_on_action("input::home", caret_home),
        gp_on_action("input::end", caret_end),
        gp_on_action("input::clear", clear),
        gp_on_action("input::submit", submit),

        gp_w_full(), gp_flex_row(), gp_items_center(), gp_px(12), gp_py(8),
        gp_rounded(8), gp_bg(MANTLE), gp_border(2), gp_border_color(OVERLAY),
        gp_focus_border_color(BLUE), gp_overflow_hidden(),

        empty ? NULL : gp_text(gp_fmt("%.*s", f->caret, f->name), gp_whitespace_pre()),
        gp_div(gp_w(2), gp_h(20), gp_bg(focused && f->caret_on ? BLUE : gp_rgba(0))),
        empty ? gp_text("Type your name…", gp_text_color(SUBTEXT))
              : gp_text(gp_fmt("%s", f->name + f->caret), gp_whitespace_pre()));
}

static gp_node *button(gp_focus focus, const char *label, int delta) {
    return gp_div(
        gp_track_focus(focus), gp_tab_stop(), gp_key_context("Button"),
        gp_on_action_data("button::press", press, (void *)(intptr_t)delta),
        gp_on_click_data(click, (void *)(intptr_t)delta),
        gp_px(20), gp_py(10), gp_rounded(8), gp_border(2), gp_border_color(gp_rgba(0)),
        gp_focus_border_color(ROSEWATER),
        gp_bg(BLUE), gp_hover_bg(BLUE_HI), gp_active_bg(BLUE_LO),
        gp_text_color(BASE), gp_cursor_pointer(),
        gp_text(label, gp_font_bold()));
}

static gp_node *form_render(void *self, gp_ctx *cx) {
    Form *f = self;
    return gp_div(
        gp_on_action("form::blur", blur),
        gp_size_full(), gp_flex_col(), gp_items_center(), gp_justify_center(),
        gp_bg(BASE), gp_text_color(TEXT),

        gp_div(
            gp_flex_col(), gp_gap(18), gp_p(32), gp_w(460), gp_rounded(16), gp_bg(SURFACE),
            gp_shadow(0, 8, 24, gp_rgba(0x00000066)),

            gp_text("Keyboard focus & actions", gp_text_size(20), gp_font_bold()),
            gp_text(gp_fmt("Hello, %s!", f->name[0] ? f->name : "world"), gp_text_size(15)),
            text_field(f, cx),
            f->status[0] ? gp_text(f->status, gp_text_color(GREEN), gp_text_size(13)) : NULL,

            gp_div(gp_flex_row(), gp_items_center(), gp_gap(12),
                   button(f->dec_focus, "−", -1),
                   gp_text(gp_fmt("%d", f->count), gp_text_size(24), gp_font_bold(),
                           gp_w(48), gp_text_center()),
                   button(f->inc_focus, "+", 1)),

            gp_text("Tab / Shift-Tab: move focus   Enter / Space: press a button\n"
                    "Esc: blur   Ctrl-A / Ctrl-E: home / end   Ctrl-U: clear   Ctrl-Q: quit",
                    gp_text_size(12), gp_text_color(SUBTEXT))));
}

static void ready(gp_app *app, void *userdata) {
    gp_bind_key(app, "backspace", "input::backspace", "NameField");
    gp_bind_key(app, "delete", "input::delete", "NameField");
    gp_bind_key(app, "left", "input::left", "NameField");
    gp_bind_key(app, "right", "input::right", "NameField");
    gp_bind_key(app, "home", "input::home", "NameField");
    gp_bind_key(app, "ctrl-a", "input::home", "NameField");
    gp_bind_key(app, "end", "input::end", "NameField");
    gp_bind_key(app, "ctrl-e", "input::end", "NameField");
    gp_bind_key(app, "ctrl-u", "input::clear", "NameField");
    gp_bind_key(app, "enter", "input::submit", "NameField");
    gp_bind_key(app, "enter", "button::press", "Button");
    gp_bind_key(app, "space", "button::press", "Button");
    gp_bind_key(app, "escape", "form::blur", NULL);
    gp_bind_key(app, "ctrl-q", "app::quit", NULL);
    gp_on_global_action(app, "app::quit", quit, NULL);

    gp_entity root = GP_NEW_VIEW(app, Form, form_render, .field_focus = gp_focus_new(app),
                                 .dec_focus = gp_focus_new(app), .inc_focus = gp_focus_new(app));
    Form *f = gp_entity_get(app, root);
    gp_ctx cx = gp_ctx_for(app, root);
    f->caret_on = true;
    f->blink = gp_every(&cx, 0.53, on_blink, NULL);

    gp_open_window(app, &(gp_window_options){ .title = "gpuic — focus & actions", .width = 640,
                                              .height = 480, .background = BASE },
                   root);
}

int main(void) { return gp_application_run(ready, NULL); }
