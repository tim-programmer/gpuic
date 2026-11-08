/* A counter, GPUI's "hello world" — a view with state, buttons and a nested view. */
#include <gpuic/gpuic.h>

#include <stdint.h>

#define BASE     gp_rgb(0x1e1e2e)
#define SURFACE  gp_rgb(0x313244)
#define TEXT     gp_rgb(0xcdd6f4)
#define SUBTEXT  gp_rgb(0x9399b2)
#define BLUE     gp_rgb(0x89b4fa)
#define BLUE_HI  gp_rgb(0xa6c4fb)
#define BLUE_LO  gp_rgb(0x6c9ef0)
#define RED      gp_rgb(0xf38ba8)

typedef struct {
    gp_entity footer;
    int count;
} Counter;

static void add(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    Counter *c = self;
    c->count += (int)(intptr_t)data;
    gp_notify(cx);
}

static void reset(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    ((Counter *)self)->count = 0;
    gp_notify(cx);
}

static gp_node *button(const char *label, gp_node *on_click) {
    return gp_div(
        gp_px(20), gp_py(10), gp_rounded(8),
        gp_bg(BLUE), gp_hover_bg(BLUE_HI), gp_active_bg(BLUE_LO),
        gp_text_color(BASE), gp_cursor_pointer(), on_click,
        gp_text(label, gp_font_bold()));
}

static gp_node *footer_render(void *self, gp_ctx *cx) {
    return gp_div(gp_text("Built with gpuic — GPUI-style UI in C", gp_text_size(13)),
                  gp_text_color(SUBTEXT));
}

static gp_node *counter_render(void *self, gp_ctx *cx) {
    Counter *c = self;
    return gp_div(
        gp_size_full(), gp_flex_col(), gp_items_center(), gp_justify_center(), gp_gap(24),
        gp_bg(BASE), gp_text_color(TEXT),

        gp_div(
            gp_flex_col(), gp_items_center(), gp_gap(20), gp_p(32), gp_w(420),
            gp_rounded(16), gp_bg(SURFACE), gp_border(1), gp_border_color(gp_rgb(0x45475a)),
            gp_shadow(0, 8, 24, gp_rgba(0x00000066)),

            gp_text("Counter", gp_text_size(14), gp_text_color(SUBTEXT)),
            gp_text(gp_fmt("%d", c->count), gp_text_size(64), gp_font_bold(),
                    c->count < 0 ? gp_text_color(RED) : NULL),
            gp_div(
                gp_flex_row(), gp_gap(12),
                button("−  Decrement", gp_on_click_data(add, (void *)(intptr_t)-1)),
                button("+  Increment", gp_on_click_data(add, (void *)(intptr_t)1))),
            c->count != 0
                ? gp_div(gp_text("Reset", gp_text_size(13), gp_text_color(SUBTEXT)),
                         gp_cursor_pointer(), gp_on_click(reset), gp_px(8), gp_py(4),
                         gp_rounded(6), gp_hover_bg(gp_rgb(0x45475a)))
                : NULL),

        gp_child_view(c->footer));
}

static void ready(gp_app *app, void *userdata) {
    gp_entity footer = gp_view_new(app, 0, NULL, footer_render, NULL);
    gp_entity root = GP_NEW_VIEW(app, Counter, counter_render, .footer = footer);
    gp_open_window(app, &(gp_window_options){ .title = "gpuic — counter", .width = 640,
                                              .height = 480, .background = BASE },
                   root);
}

int main(void) { return gp_application_run(ready, NULL); }
