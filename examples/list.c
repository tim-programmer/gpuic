/*
 * Virtualized lists: 100,000 fixed-height rows (gp_uniform_list), 5,000 cards whose heights vary
 * with their text (gp_list), and a chat log (gp_list again) that sticks to its end as messages
 * arrive and gets older ones spliced in above without moving what you are reading. Only the
 * visible rows are ever built; the counter shows how many item-function calls have happened.
 * Click or use Up/Down/PgUp/PgDn/Home/End on the focused list.
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

#define N1 100000
#define N2 5000

typedef struct {
    gp_focus focus;
    gp_scroll scroll, scroll2, scroll3;
    size_t sel;
    size_t built;     /* item-function calls, to show the virtualization */
    size_t chat_first, chat_n;   /* the log holds messages chat_first .. chat_first + chat_n - 1 */
} Demo;

static const char *const WORDS[] = { "layout", "the", "viewport", "glyph", "flexbox", "anchor", "row",
                                     "scroll", "measure", "estimate", "pixel", "cache", "fold" };

/* A deterministic paragraph of `n` words for message / card `seed`. */
static const char *sentence(size_t seed, size_t n) {
    char *out = gp_frame_alloc(n * 10 + 1);
    out[0] = 0;
    size_t h = seed * 2654435761u;
    for (size_t i = 0; i < n; i++) {
        h = h * 1103515245u + 12345u;
        strcat(out, WORDS[(h >> 16) % (sizeof WORDS / sizeof *WORDS)]);
        strcat(out, i + 1 < n ? " " : ".");
    }
    return out;
}

static void select_row(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    ((Demo *)self)->sel = (size_t)data;
    gp_notify(cx);
}

static void move_to(Demo *d, gp_ctx *cx, long target) {
    if (target < 0) target = 0;
    if (target >= N1) target = N1 - 1;
    d->sel = (size_t)target;
    gp_list_scroll_to_item(cx, d->scroll, d->sel, GP_ALIGN_ROW_NEAREST);
    gp_notify(cx);
}

static void act_next(void *self, gp_ctx *cx, void *data) { move_to(self, cx, (long)((Demo *)self)->sel + 1); }
static void act_prev(void *self, gp_ctx *cx, void *data) { move_to(self, cx, (long)((Demo *)self)->sel - 1); }
static void act_pgdn(void *self, gp_ctx *cx, void *data) { move_to(self, cx, (long)((Demo *)self)->sel + 12); }
static void act_pgup(void *self, gp_ctx *cx, void *data) { move_to(self, cx, (long)((Demo *)self)->sel - 12); }
static void act_first(void *self, gp_ctx *cx, void *data) { move_to(self, cx, 0); }
static void act_last(void *self, gp_ctx *cx, void *data) { move_to(self, cx, N1 - 1); }

static gp_node *simple_row(void *self, gp_ctx *cx, size_t i, void *data) {
    Demo *d = self;
    d->built++;
    bool sel = d->sel == i;
    return gp_div(
        gp_flex_row(), gp_items_center(), gp_px(14), gp_cursor_pointer(),
        gp_bg(sel ? OVERLAY : (i % 2 ? gp_rgba(0) : gp_rgba(0xffffff08))),
        gp_hover_bg(sel ? OVERLAY : SURFACE),
        gp_on_click_data(select_row, (void *)i),
        gp_text(gp_fmt("Row %zu", i), sel ? gp_font_bold() : NULL),
        gp_div(gp_flex_1()),
        gp_text(gp_fmt("0x%05zx", i), gp_text_size(12), gp_text_color(SUBTEXT)));
}

static gp_node *card_row(void *self, gp_ctx *cx, size_t i, void *data) {
    ((Demo *)self)->built++;
    return gp_div(
        gp_flex_col(), gp_gap(2), gp_px(14), gp_py(6),
        gp_text(gp_fmt("Message %zu", i), gp_font_bold()),
        gp_text(sentence(i, 2 + (i * 7) % 23), gp_text_size(12), gp_text_color(SUBTEXT)));
}

static gp_node *chat_row(void *self, gp_ctx *cx, size_t i, void *data) {
    Demo *d = self;
    d->built++;
    size_t id = d->chat_first + i;
    bool mine = id % 3 == 0;
    return gp_div(
        gp_flex_row(), mine ? gp_justify_end() : gp_justify_start(), gp_px(10), gp_py(4),
        gp_div(gp_flex_col(), gp_gap(2), gp_max_w(230), gp_px(10), gp_py(6), gp_rounded(10),
               gp_bg(mine ? gp_rgb(0x3b5a99) : SURFACE),
               gp_text(gp_fmt("#%zu", id), gp_text_size(11), gp_text_color(SUBTEXT)),
               gp_text(sentence(id, 2 + (id * 5) % 29), gp_text_size(13))));
}

static void chat_send(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    Demo *d = self;
    d->chat_n++;   /* appended at the end: a list that sticks to its end follows */
    gp_notify(cx);
}

static void chat_older(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    Demo *d = self;
    size_t more = d->chat_first < 25 ? d->chat_first : 25;
    if (!more) return;
    d->chat_first -= more;   /* older messages arrive above: tell the list, so the view stays put */
    d->chat_n += more;
    gp_list_splice(cx, d->scroll3, 0, 0, more);
    gp_notify(cx);
}

static gp_node *button(const char *label, gp_mouse_fn fn) {
    return gp_div(gp_px(10), gp_py(4), gp_rounded(6), gp_bg(SURFACE), gp_hover_bg(OVERLAY), gp_cursor_pointer(),
                  gp_on_click(fn), gp_text(label, gp_text_size(12)));
}

static gp_node *demo_render(void *self, gp_ctx *cx) {
    Demo *d = self;
    gp_node *rows1 = gp_uniform_list(
        N1, 32, simple_row, NULL,
        gp_track_scroll(d->scroll), gp_track_focus(d->focus), gp_tab_stop(), gp_key_context("List"),
        gp_on_action("list::next", act_next), gp_on_action("list::prev", act_prev),
        gp_on_action("list::page_down", act_pgdn), gp_on_action("list::page_up", act_pgup),
        gp_on_action("list::first", act_first), gp_on_action("list::last", act_last),
        gp_flex_1(), gp_border(2), gp_border_color(gp_rgba(0)), gp_focus_border_color(BLUE),
        gp_rounded(8), gp_bg(MANTLE));
    gp_node *rows2 = gp_list(
        N2, 60, card_row, NULL,
        gp_track_scroll(d->scroll2), gp_flex_1(), gp_rounded(8), gp_bg(MANTLE));
    gp_node *chat = gp_list(
        d->chat_n, 50, chat_row, NULL,
        gp_track_scroll(d->scroll3), gp_list_stick_to_end(), gp_flex_1(), gp_rounded(8), gp_bg(MANTLE));

    return gp_div(
        gp_size_full(), gp_flex_col(), gp_gap(14), gp_p(20), gp_bg(BASE), gp_text_color(TEXT),
        gp_div(gp_flex_row(), gp_gap(16), gp_items_center(),
               gp_text("Virtualized lists", gp_text_size(20), gp_font_bold()),
               gp_text(gp_fmt("selected %zu · item fn calls so far: %zu", d->sel, d->built),
                       gp_text_color(SUBTEXT), gp_text_size(13))),
        gp_div(gp_flex_1(), gp_flex_row(), gp_gap(20),
               gp_div(gp_w(320), gp_flex_col(), gp_gap(8),
                      gp_text(gp_fmt("%d rows × 32px", N1), gp_text_size(13), gp_text_color(SUBTEXT)), rows1),
               gp_div(gp_flex_1(), gp_flex_col(), gp_gap(8),
                      gp_text(gp_fmt("%d rows of different heights (gp_list)", N2), gp_text_size(13),
                              gp_text_color(SUBTEXT)),
                      rows2),
               gp_div(gp_w(300), gp_flex_col(), gp_gap(8),
                      gp_div(gp_flex_row(), gp_gap(8), gp_items_center(),
                             gp_text("chat: sticks to the end", gp_text_size(13), gp_text_color(SUBTEXT)),
                             gp_div(gp_flex_1()), button("send", chat_send), button("older", chat_older)),
                      chat)));
}

static void ready(gp_app *app, void *userdata) {
    gp_bind_key(app, "down", "list::next", "List");
    gp_bind_key(app, "up", "list::prev", "List");
    gp_bind_key(app, "pagedown", "list::page_down", "List");
    gp_bind_key(app, "pageup", "list::page_up", "List");
    gp_bind_key(app, "home", "list::first", "List");
    gp_bind_key(app, "end", "list::last", "List");

    gp_entity root = GP_NEW_VIEW(app, Demo, demo_render, .focus = gp_focus_new(app),
                                 .scroll = gp_scroll_new(app), .scroll2 = gp_scroll_new(app),
                                 .scroll3 = gp_scroll_new(app), .chat_first = 100, .chat_n = 40);
    gp_open_window(app, &(gp_window_options){ .title = "gpuic — virtualized lists", .width = 1180,
                                              .height = 560, .background = BASE },
                   root);
}

int main(void) { return gp_application_run(ready, NULL); }
