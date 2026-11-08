/*
 * Declarative animations (gp_with_animation). Nothing here calls gp_notify to move anything: the elements
 * animate themselves, and the views only render when the state they show changes.
 *
 *   status dot   repeats, ping-pong, eased
 *   busy bar     a block sliding along a track, forever (linear)
 *   ball         eased bounce, ping-pong
 *   panel        opens and closes on click: a new id each time starts a new run (the function knows which way)
 *   colors       a fade between two colors, then it stays
 *   rows         a list whose rows fade in one after another (each row has its own id and delay)
 *   cached view  an animated spinner inside gp_child_view_cached: its render count stays at 1
 *
 * Try:  GPUIC_SCREENSHOT=out.ppm GPUIC_WAIT_MS=700 ./animate     (a frame from the middle of the motion)
 */
#include <gpuic/gpuic.h>

#include <stdio.h>

#define BASE    gp_rgb(0x1e1e2e)
#define MANTLE  gp_rgb(0x181825)
#define SURFACE gp_rgb(0x313244)
#define OVERLAY gp_rgb(0x45475a)
#define TEXT    gp_rgb(0xcdd6f4)
#define SUBTEXT gp_rgb(0x9399b2)
#define BLUE    gp_rgb(0x89b4fa)
#define GREEN   gp_rgb(0xa6e3a1)
#define PEACH   gp_rgb(0xfab387)
#define MAUVE   gp_rgb(0xcba6f7)

typedef struct {
    bool open;
    int toggles;              /* part of the panel animation's id: each click starts a new run */
    gp_scroll scroll;
    gp_entity spinner;
    int renders;
} Demo;

/* ---- the animation functions: progress in, modifiers out */

static gp_node *pulse(float t, void *data) { return gp_opacity(gp_lerp(0.25f, 1.f, t)); }

static gp_node *sweep(float t, void *data) { return gp_left(gp_lerp(-60.f, 300.f, t)); }

static gp_node *drop(float t, void *data) { return gp_top(gp_lerp(0.f, 70.f, t)); }

static gp_node *panel_height(float t, void *data) {
    bool opening = data != NULL;
    return gp_h(gp_lerp(0.f, 96.f, opening ? t : 1.f - t));
}

static gp_color mix(gp_color a, gp_color b, float t) {
    return (gp_color){ gp_lerp(a.r, b.r, t), gp_lerp(a.g, b.g, t), gp_lerp(a.b, b.b, t), 1.f };
}
static gp_node *warm_up(float t, void *data) { return gp_bg(mix(OVERLAY, PEACH, t)); }

static gp_node *fade_in(float t, void *data) { return gp_group(gp_opacity(t), gp_px(gp_lerp(40.f, 10.f, t))); }

static gp_node *grow_pad(float t, void *data) { return gp_group(gp_opacity(gp_lerp(0.4f, 1.f, t)), gp_w(gp_lerp(20.f, 34.f, t)), gp_h(gp_lerp(20.f, 34.f, t))); }

/* ---- a view that does not render again while it animates */

typedef struct { int renders; } Spinner;

static gp_node *spinner_render(void *self, gp_ctx *cx) {
    Spinner *s = self;
    s->renders++;
    return gp_div(gp_flex_row(), gp_gap(10), gp_items_center(),
                  gp_div(gp_size_px(34), gp_flex_none(), gp_items_center(), gp_justify_center(),
                         gp_div(gp_size_px(20), gp_rounded(17), gp_bg(MAUVE),
                                gp_with_animation("spinner", (gp_animation){ .duration = 0.9, .repeat = true, .ping_pong = true,
                                                                              .easing = GP_EASE_IN_OUT },
                                                  grow_pad, NULL))),
                  gp_text(gp_fmt("cached view, rendered %d time%s", s->renders, s->renders == 1 ? "" : "s"),
                          gp_text_size(13), gp_text_color(SUBTEXT)));
}

/* ---- the list of rows that fade in one after another */

static gp_node *fade_row(void *self, gp_ctx *cx, size_t i, void *data) {
    return gp_div(gp_h(22), gp_flex_row(), gp_items_center(), gp_py(2),
                  gp_with_animation(gp_fmt("row-%zu", i), (gp_animation){ .duration = 0.6, .delay = 0.12 * (double)(i % 12), .easing = GP_EASE_OUT },
                                    fade_in, NULL),
                  gp_text(gp_fmt("row %zu", i), gp_text_size(13)));
}

/* ---- the demo view */

static void toggle(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    Demo *d = self;
    d->open = !d->open;
    d->toggles++;
    gp_notify(cx);
}

static gp_node *card(const char *title, gp_node *body) {
    return gp_div(gp_flex_col(), gp_gap(8), gp_p(12), gp_rounded(10), gp_bg(SURFACE),
                  gp_text(title, gp_text_size(12), gp_text_color(SUBTEXT)), body);
}

static gp_node *demo_render(void *self, gp_ctx *cx) {
    Demo *d = self;
    d->renders++;
    gp_node *status = gp_div(gp_flex_row(), gp_gap(10), gp_items_center(),
                             gp_div(gp_size_px(14), gp_rounded(7), gp_bg(GREEN),
                                    gp_with_animation("status", (gp_animation){ .duration = 1.1, .repeat = true, .ping_pong = true,
                                                                                .easing = GP_EASE_IN_OUT },
                                                      pulse, NULL)),
                             gp_text("connected", gp_text_size(13)));
    gp_node *bar = gp_div(gp_h(8), gp_rounded(4), gp_bg(BASE), gp_overflow_hidden(),
                          gp_div(gp_absolute(), gp_w(60), gp_h(8), gp_rounded(4), gp_bg(BLUE),
                                 gp_with_animation("busy", (gp_animation){ .duration = 1.4, .repeat = true }, sweep, NULL)));
    gp_node *ball = gp_div(gp_h(100), gp_w(40),
                           gp_div(gp_absolute(), gp_left(8), gp_size_px(24), gp_rounded(12), gp_bg(PEACH),
                                  gp_with_animation("ball", (gp_animation){ .duration = 0.8, .repeat = true, .ping_pong = true,
                                                                            .easing = GP_EASE_OUT_BOUNCE },
                                                    drop, NULL)));
    gp_node *panel = gp_div(
        gp_flex_col(), gp_gap(8),
        gp_div(gp_px(14), gp_py(8), gp_rounded(8), gp_bg(OVERLAY), gp_cursor_pointer(), gp_on_click(toggle),
               gp_text(d->open ? "Close" : "Open", gp_font_bold())),
        gp_div(gp_overflow_hidden(), gp_rounded(8), gp_bg(BASE),
               /* a new id per click: a new run, starting now; the function is told which way it goes */
               gp_with_animation(gp_fmt("panel-%d", d->toggles), (gp_animation){ .duration = d->toggles ? 0.35 : 0, .easing = GP_EASE_OUT },
                                 panel_height, d->open ? (void *)1 : NULL),
               gp_div(gp_p(12), gp_flex_col(), gp_gap(4), gp_flex_shrink_0(),
                      gp_text("It opens and closes on its own,", gp_text_size(13)),
                      gp_text("no view renders per frame.", gp_text_size(13), gp_text_color(SUBTEXT)),
                      gp_text(gp_fmt("this view rendered %d times", d->renders), gp_text_size(12), gp_text_color(SUBTEXT)))));
    gp_node *colors = gp_div(gp_h(30), gp_rounded(8), gp_bg(OVERLAY),
                             gp_with_animation("warm", (gp_animation){ .duration = 2.5, .easing = GP_EASE_IN_OUT }, warm_up, NULL));
    gp_node *rows = gp_list(36, 22, fade_row, NULL, gp_track_scroll(d->scroll), gp_h(150), gp_rounded(8), gp_bg(BASE));

    return gp_div(
        gp_size_full(), gp_flex_row(), gp_gap(16), gp_p(20), gp_bg(BASE), gp_text_color(TEXT),
        gp_div(gp_flex_1(), gp_flex_col(), gp_gap(14),
               gp_text("Declarative animations", gp_text_size(20), gp_font_bold()),
               card("repeat · ping-pong · ease in-out", status),
               card("repeat · linear", bar),
               card("a color, once", colors),
               card("cached view", gp_child_view_cached(d->spinner))),
        gp_div(gp_flex_1(), gp_flex_col(), gp_gap(14),
               gp_div(gp_flex_row(), gp_gap(14),
                      gp_div(gp_flex_1(), card("ease out bounce", ball)),
                      gp_div(gp_flex_1(), card("click to open", panel))),
               card("rows, staggered by delay", rows)));
}

static void ready(gp_app *app, void *userdata) {
    gp_entity sp = gp_view_new(app, sizeof(Spinner), NULL, spinner_render, NULL);
    gp_entity demo = gp_view_new(app, sizeof(Demo), &(Demo){ .scroll = gp_scroll_new(app), .spinner = sp },
                                 demo_render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "gpuic: animations", .width = 780, .height = 520 }, demo);
}

int main(void) { return gp_application_run(ready, NULL); }
