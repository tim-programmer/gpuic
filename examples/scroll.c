/*
 * Scrolling: a 200-row list (handle + keyboard paging + hover/click while scrolled), and a
 * scrolling page containing a scrolling log (wheel chaining from inner to outer) and a
 * horizontally scrolling strip.
 */
#include <gpuic/gpuic.h>

#include <stdint.h>
#include <stdio.h>

#define BASE     gp_rgb(0x1e1e2e)
#define MANTLE   gp_rgb(0x181825)
#define SURFACE  gp_rgb(0x313244)
#define OVERLAY  gp_rgb(0x45475a)
#define TEXT     gp_rgb(0xcdd6f4)
#define SUBTEXT  gp_rgb(0x9399b2)
#define BLUE     gp_rgb(0x89b4fa)
#define BLUE_HI  gp_rgb(0xa6c4fb)
#define BLUE_LO  gp_rgb(0x6c9ef0)

#define ROWS 200
#define PAGE_PX 240

typedef struct {
    gp_focus list_focus;
    gp_scroll list_scroll, log_scroll;
    int selected;
    int log_lines;
} Demo;

static void select_row(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    ((Demo *)self)->selected = (int)(intptr_t)data;
    gp_notify(cx);
}

static void page(void *self, gp_ctx *cx, void *data) {
    gp_scroll_by(cx, ((Demo *)self)->list_scroll, 0, (float)(intptr_t)data);
}

static void to_top(void *self, gp_ctx *cx, void *data) { gp_scroll_to(cx, ((Demo *)self)->list_scroll, 0, 0); }
static void to_bottom(void *self, gp_ctx *cx, void *data) { gp_scroll_to_end(cx, ((Demo *)self)->list_scroll); }

static void add_lines(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    Demo *d = self;
    d->log_lines += (int)(intptr_t)data;
    gp_scroll_to_end(cx, d->log_scroll); /* follow the tail */
    gp_notify(cx);
}

static gp_node *row(Demo *d, int i) {
    bool sel = d->selected == i;
    return gp_div(
        gp_px(14), gp_py(7), gp_flex_none(), gp_cursor_pointer(),
        gp_bg(sel ? OVERLAY : gp_rgba(0)), gp_hover_bg(sel ? OVERLAY : SURFACE),
        gp_on_click_data(select_row, (void *)(intptr_t)i),
        gp_text(gp_fmt("Item %d", i), sel ? gp_font_bold() : NULL));
}

static gp_node *list_panel(Demo *d) {
    gp_node **rows = gp_frame_alloc(ROWS * sizeof *rows);
    for (int i = 0; i < ROWS; i++) rows[i] = row(d, i);

    return gp_div(
        gp_w(280), gp_flex_col(), gp_gap(10),
        gp_text(gp_fmt("Items — selected %d", d->selected), gp_font_bold()),
        gp_div(
            gp_track_scroll(d->list_scroll), gp_track_focus(d->list_focus), gp_tab_stop(),
            gp_key_context("List"), gp_overflow_y_scroll(), gp_flex_1(),
            gp_on_action_data("list::page_down", page, (void *)(intptr_t)PAGE_PX),
            gp_on_action_data("list::page_up", page, (void *)(intptr_t)-PAGE_PX),
            gp_on_action("list::top", to_top), gp_on_action("list::bottom", to_bottom),
            gp_border(2), gp_border_color(gp_rgba(0)), gp_focus_border_color(BLUE),
            gp_rounded(8), gp_bg(MANTLE),
            gp_children(rows, ROWS)),
        gp_text("Tab to focus the list, then PgUp / PgDn / Home / End",
                gp_text_size(12), gp_text_color(SUBTEXT)));
}

static gp_node *button(const char *label, gp_node *on_click) {
    return gp_div(gp_px(14), gp_py(8), gp_rounded(8), gp_bg(BLUE), gp_hover_bg(BLUE_HI),
                  gp_active_bg(BLUE_LO), gp_text_color(BASE), gp_cursor_pointer(), on_click,
                  gp_text(label, gp_font_bold()));
}

static gp_node *log_card(Demo *d) {
    int n = d->log_lines;
    gp_node **lines = gp_frame_alloc((size_t)(n ? n : 1) * sizeof *lines);
    for (int i = 0; i < n; i++)
        lines[i] = gp_text(gp_fmt("[%03d] something happened here", i + 1),
                           gp_text_size(13), gp_text_color(i % 2 ? TEXT : SUBTEXT));
    return gp_div(
        gp_flex_col(), gp_gap(8),
        gp_div(gp_flex_row(), gp_items_center(), gp_gap(8),
               gp_text("Log", gp_font_bold()),
               button("+5 lines", gp_on_click_data(add_lines, (void *)(intptr_t)5)),
               button("+1", gp_on_click_data(add_lines, (void *)(intptr_t)1))),
        gp_div(gp_track_scroll(d->log_scroll), gp_overflow_y_scroll(), gp_h(150), gp_flex_none(),
               gp_flex_col(), gp_p(10), gp_gap(2), gp_rounded(8), gp_bg(MANTLE),
               gp_children(lines, (size_t)n)));
}

static gp_node *strip(void) {
    gp_node **boxes = gp_frame_alloc(30 * sizeof *boxes);
    for (int i = 0; i < 30; i++) {
        float t = (float)i / 30;
        gp_color c = { 0.35f + 0.5f * t, 0.45f + 0.3f * (1 - t), 0.9f - 0.5f * t, 1 };
        boxes[i] = gp_div(gp_w(72), gp_h(72), gp_flex_none(), gp_rounded(10), gp_bg(c),
                          gp_items_center(), gp_justify_center(), gp_text_color(BASE),
                          gp_text(gp_fmt("%d", i), gp_font_bold()));
    }
    return gp_div(gp_flex_col(), gp_gap(8),
                  gp_text("Horizontal strip", gp_font_bold()),
                  gp_div(gp_overflow_x_scroll(), gp_flex_row(), gp_gap(8), gp_flex_none(),
                         gp_pb(14), gp_children(boxes, 30)));
}

static gp_node *paragraphs(void) {
    return gp_div(
        gp_flex_col(), gp_gap(8),
        gp_text("About this page", gp_font_bold()),
        gp_text("The whole right-hand side is itself a scroller. When the log above reaches its "
                "end the wheel chains to this page, the way nested scroll areas behave in a browser. "
                "Scroll offsets persist across rebuilds: add some log lines, or click a row in the "
                "list, and nothing jumps.", gp_text_color(SUBTEXT)),
        gp_text("Scrollbar thumbs can be dragged. Hover brightens them; they hide when there is "
                "nothing to scroll.", gp_text_color(SUBTEXT)));
}

static gp_node *demo_render(void *self, gp_ctx *cx) {
    Demo *d = self;
    return gp_div(
        gp_size_full(), gp_flex_row(), gp_gap(20), gp_p(20), gp_bg(BASE), gp_text_color(TEXT),
        list_panel(d),
        gp_div(gp_flex_1(), gp_flex_col(), gp_gap(22), gp_overflow_y_scroll(), gp_pr(10),
               gp_text("Scrolling", gp_text_size(22), gp_font_bold()),
               log_card(d), strip(), paragraphs(),
               paragraphs()));
}

static void ready(gp_app *app, void *userdata) {
    gp_bind_key(app, "pagedown", "list::page_down", "List");
    gp_bind_key(app, "pageup", "list::page_up", "List");
    gp_bind_key(app, "home", "list::top", "List");
    gp_bind_key(app, "end", "list::bottom", "List");

    gp_entity root = GP_NEW_VIEW(app, Demo, demo_render, .list_focus = gp_focus_new(app),
                                 .list_scroll = gp_scroll_new(app), .log_scroll = gp_scroll_new(app),
                                 .selected = -1, .log_lines = 24);
    gp_open_window(app, &(gp_window_options){ .title = "gpuic — scrolling", .width = 860,
                                              .height = 560, .background = BASE },
                   root);
}

int main(void) { return gp_application_run(ready, NULL); }
