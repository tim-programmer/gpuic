/*
 * Auto-hiding scrollbars (gp_scrollbar_autohide). Needs a display.
 *
 * The thumbs are found in the painted scene by their color (each scroller in the fixture has its own), so what is
 * checked is what is drawn. Time is jumped with gp_debug_clock_skew; frames and wake-ups are counted to show that a
 * hidden or steady scrollbar costs nothing. After every step the frame is compared with one painted without the
 * frame caches, because the scrollers live inside cached views too.
 */
#include "../src/internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...)                                                               \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            if (failures++ < 30) {                                                     \
                printf("FAIL %s:%d: ", __FILE__, __LINE__);                            \
                printf(__VA_ARGS__);                                                   \
                printf("\n");                                                          \
            }                                                                          \
        }                                                                              \
    } while (0)

static gp_app *app;
static gp_co *co_;
static gp_window *win(void) { return app->windows[0]; }

#define BASE_ALPHA 0.8f
#define MAGENTA gp_rgba(0xff00ffcc)
#define CYAN gp_rgba(0x00ffffcc)
#define YELLOW gp_rgba(0xffff00cc)
#define GREEN gp_rgba(0x00ff00cc)
#define BLUE gp_rgba(0x4040ffcc)

static int64_t skew;
static void jump(double seconds) {
    skew += (int64_t)(seconds * 1e9);
    gp_debug_clock_skew(skew);
}
static bool wait_draws(unsigned n) {
    unsigned long target = app->stat_draws + n;
    for (int i = 0; i < 1000 && app->stat_draws < target; i++) gp_sleep(co_, 0.004);
    return app->stat_draws >= target;
}

/* Draws until `alpha()` reads above 0.7, at most a few: a draw already under way when an injected event arrives
 * does not show its effect, so "the next draw" is not enough to wait for something to appear. */
static void wait_shown(float (*alpha)(void)) {
    for (int i = 0; i < 6 && !(alpha() > 0.7f); i++) wait_draws(1);
}

/* the alpha of the thumb painted in `color` (rgb), or -1 if there is none */
static float thumb(float r, float g, float b) {
    gp_scene *sc = &win()->scene;
    float best = -1;
    for (size_t i = 0; i < sc->len; i++) {
        const gp_instance *in = &sc->items[i];
        if (in->kind == GP_PRIM_QUAD && fabsf(in->color[0] - r) < 0.003f && fabsf(in->color[1] - g) < 0.003f &&
            fabsf(in->color[2] - b) < 0.003f && in->color[3] > best)
            best = in->color[3];
    }
    return best;
}
static float t_magenta(void) { return thumb(1, 0, 1); }
static float t_cyan(void) { return thumb(0, 1, 1); }
static float t_yellow(void) { return thumb(1, 1, 0); }
static float t_green(void) { return thumb(0, 1, 0); }
static float t_blue(void) { return thumb(0.2509804f, 0.2509804f, 1); }

static const char *where = "";
static void verify_frame(void) {
    gp_window *w = win();
    if (!w->tree || w->dirty_render || w->dirty_paint || w->dirty_layout) return;
    gp_scene fresh = { 0 };
    gp_arena scratch = { 0 };
    gp_paint_tree(&fresh, &scratch, app->text, w->tree, w->scale, w->mouse, w->mouse_in, w->pressed, w->focused, w->drag_key,
                  w->scroll_vis_gen, NULL, NULL);
    bool same = fresh.len == w->scene.len && memcmp(fresh.items, w->scene.items, fresh.len * sizeof *fresh.items) == 0;
    CHECK(same, "[%s] the frame painted with caches differs from one painted without (%zu vs %zu instances)", where, w->scene.len, fresh.len);
    free(fresh.items);
    gp_arena_free(&scratch);
}

/* a draw after a jump of the clock (the loop would otherwise be asleep until the deadline it computed before the jump) */
static void step(double seconds) {
    jump(seconds);
    win()->dirty_paint = true;
    wait_draws(1);
    verify_frame();
}

static void mouse_away(void) {
    gp_simulate_mouse(app, GP_SIM_MOVE, 330, 580, 0, 0);
    wait_draws(1);
}

/* ------------------------------------------------------------ the fixture */

typedef struct {
    gp_scroll s_auto, s_plain, s_short, s_list, s_cached, s_clipped;
    gp_entity cached;
    int renders;
} Fix;
static gp_entity root, cached_view;
static Fix *fix(void) { return gp_entity_get(app, root); }

static gp_node *list_row(void *self, gp_ctx *cx, size_t i, void *data) {
    return gp_div(gp_h(20), gp_text(gp_fmt("row %zu", i)));
}

static gp_node *cached_render(void *self, gp_ctx *cx) {
    Fix *f = gp_entity_get(cx->app, root);
    return gp_div(gp_h(70), gp_overflow_y_scroll(), gp_track_scroll(f->s_cached), gp_scrollbar_autohide(), gp_scrollbar_color(GREEN),
                  gp_div(gp_h(300), gp_text("cached content")));
}

static gp_node *render(void *self, gp_ctx *cx) {
    Fix *f = self;
    f->renders++;
    /* the later children come in through gp_children (the other way of passing them) */
    gp_node **rest = gp_frame_alloc(3 * sizeof *rest);
    rest[0] = gp_child_view_cached(f->cached);
    rest[1] = gp_list(200, 20, list_row, NULL, gp_track_scroll(f->s_list), gp_h(60), gp_w(300), gp_scrollbar_autohide(), gp_scrollbar_color(gp_rgba(0xff8000cc)));
    /* an auto-hiding scroller taller than the box that clips it: the part outside is not there to be hovered */
    gp_node **inner = gp_frame_alloc(sizeof *inner);   /* here too the scroller is passed through gp_children */
    inner[0] = gp_div(gp_w(300), gp_h(80), gp_flex_shrink_0(), gp_overflow_y_scroll(), gp_track_scroll(f->s_clipped), gp_scrollbar_autohide(),
                      gp_scrollbar_color(BLUE), gp_div(gp_h(300), gp_text("clipped content")));
    rest[2] = gp_div(gp_w(300), gp_h(16), gp_overflow_hidden(), gp_children(inner, 1));
    return gp_div(gp_flex_col(), gp_gap(6), gp_p(6),
                  gp_div(gp_w(300), gp_h(80), gp_overflow_y_scroll(), gp_track_scroll(f->s_auto), gp_scrollbar_autohide(), gp_scrollbar_color(MAGENTA),
                         gp_div(gp_h(400), gp_text("auto content"))),
                  gp_div(gp_w(300), gp_h(60), gp_overflow_y_scroll(), gp_track_scroll(f->s_plain), gp_scrollbar_color(CYAN),
                         gp_div(gp_h(300), gp_text("plain content"))),
                  gp_div(gp_w(300), gp_h(60), gp_overflow_y_scroll(), gp_track_scroll(f->s_short), gp_scrollbar_autohide(), gp_scrollbar_color(YELLOW),
                         gp_div(gp_h(20), gp_text("short content"))),
                  gp_children(rest, 3));
}

static float t_orange(void) { return thumb(1, 0.5019608f, 0); }

/* the scroller's element, and where on the screen */
static gp_el *find_scroller(gp_el *n, uint64_t key, float ox, float oy, float *ax, float *ay) {
    float x = ox + n->pos.x, y = oy + n->pos.y;
    if ((n->style.overflow_x_scroll || n->style.overflow_y_scroll) && n->scroll_key == key) { *ax = x; *ay = y; return n; }
    for (uint32_t i = 0; i < n->nchildren; i++) {
        gp_el *r = find_scroller(n->children[i], key, x - n->scroll.x, y - n->scroll.y, ax, ay);
        if (r) return r;
    }
    return NULL;
}

static void wheel_over(gp_scroll h, float dy) {
    float ax = 0.f, ay = 0.f;
    gp_el *sc = find_scroller(win()->tree, h.id, 0, 0, &ax, &ay);
    CHECK(sc != NULL, "scroller not found");
    if (sc) gp_scroll_wheel(win(), (gp_point){ ax + 20, ay + 20 }, 0, dy);
}

/* ------------------------------------------------------------ scenarios */

static void test_hidden_until_scrolled(void) {
    where = "test_hidden_until_scrolled";
    mouse_away();
    wait_draws(1);
    CHECK(t_magenta() < 0, "the auto-hiding scrollbar is drawn before anything happened (alpha %.2f)", t_magenta());
    CHECK(t_cyan() > 0.7f, "a plain scrollbar is not always visible (alpha %.2f)", t_cyan());
    CHECK(t_yellow() < 0, "a scroller with nothing to scroll shows a thumb (%.2f)", t_yellow());
    verify_frame();

    wheel_over(fix()->s_auto, 40);
    wait_draws(1);
    CHECK(fabsf(t_magenta() - BASE_ALPHA) < 0.02f, "after scrolling the thumb is at alpha %.2f, want %.2f", t_magenta(), BASE_ALPHA);
    verify_frame();
    CHECK(t_cyan() > 0.7f, "the plain scrollbar changed");
}

static void test_fades(void) {
    where = "test_fades";
    /* scrolled just now: it holds, then fades through values in between, then goes */
    step(0.3);
    CHECK(fabsf(t_magenta() - BASE_ALPHA) < 0.02f, "0.3 s after scrolling it is at %.2f", t_magenta());
    int partial = 0;
    float last = BASE_ALPHA + 0.01f;
    bool monotonic = true;
    for (int i = 0; i < 24 && t_magenta() >= 0; i++) {
        step(0.05);
        float a = t_magenta();
        if (a > 0.02f && a < BASE_ALPHA - 0.02f) partial++;
        if (a > last + 0.001f) monotonic = false;
        if (a >= 0) last = a;
    }
    CHECK(partial >= 2, "the fade went through %d in-between values; a fade, not a switch", partial);
    CHECK(monotonic, "it faded in and out");
    CHECK(t_magenta() < 0, "it never went away (alpha %.2f)", t_magenta());

    /* hidden: nothing is drawn or woken */
    wait_draws(1);
    gp_sleep(co_, 0.15);
    unsigned long draws = app->stat_draws, wakeups = app->stat_wakeups;
    gp_sleep(co_, 0.4);
    CHECK(app->stat_draws == draws, "%lu frames were drawn while the scrollbar was hidden", app->stat_draws - draws);
    CHECK(app->stat_wakeups - wakeups <= 4, "%lu wakeups while the scrollbar was hidden", app->stat_wakeups - wakeups);
}

static void test_shown_steadily_costs_nothing_until_it_ends(void) {
    where = "test_shown_steadily";
    wheel_over(fix()->s_auto, 40);
    wait_draws(1);
    gp_sleep(co_, 0.15);
    unsigned long draws = app->stat_draws;
    gp_sleep(co_, 0.3);
    /* during the hold nothing changes, so nothing is drawn: a wake-up is set for when it ends (no polling) */
    CHECK(app->stat_draws - draws <= 1, "%lu frames in 0.3 s of a steady scrollbar", app->stat_draws - draws);
    step(2.0);
    CHECK(t_magenta() < 0, "the hold ended and it is still drawn (%.2f)", t_magenta());
}

/* in real time, with nothing prodding the window: the wake-up set for the end of the hold starts the fade, and the fade's
 * own frames finish it */
static void test_expires_by_itself(void) {
    where = "test_expires_by_itself";
    mouse_away();
    step(3.0);
    wheel_over(fix()->s_auto, 40);
    wait_draws(1);
    CHECK(t_magenta() > 0.7f, "precondition: shown");
    unsigned long draws = app->stat_draws;
    gp_sleep(co_, 0.7);
    CHECK(t_magenta() > 0.7f, "gone 0.7 s after scrolling (alpha %.2f)", t_magenta());
    CHECK(app->stat_draws - draws <= 1, "%lu frames while it was shown steadily", app->stat_draws - draws);
    gp_sleep(co_, 0.9);   /* 1.6 s in: the hold (1 s) and the fade (0.25 s) are over */
    CHECK(t_magenta() < 0, "still drawn 1.6 s after the last scroll, with nothing to redraw it (alpha %.2f)", t_magenta());
    unsigned long fade_frames = app->stat_draws - draws;
    double hz = gp_platform_refresh_hz(win());   /* the fade is paced by the display: a quarter of a second of frames */
    CHECK(fade_frames >= 0.3 * 0.25 * hz && fade_frames <= 1.4 * 0.25 * hz + 4, "%lu frames for a quarter-second fade on a %.0f Hz display", fade_frames, hz);
    verify_frame();
}

static void test_hover(void) {
    where = "test_hover";
    float ax = 0.f, ay = 0.f;
    gp_el *sc = find_scroller(win()->tree, fix()->s_auto.id, 0, 0, &ax, &ay);
    CHECK(sc != NULL, "scroller not found");
    if (!sc) return;
    gp_simulate_mouse(app, GP_SIM_MOVE, ax + 30, ay + 30, 0, 0);
    wait_draws(1);
    CHECK(fabsf(t_magenta() - BASE_ALPHA) < 0.02f, "hovering shows the thumb (%.2f)", t_magenta());
    verify_frame();
    step(5.0);
    CHECK(fabsf(t_magenta() - BASE_ALPHA) < 0.02f, "it hid while the mouse was still over the scroller (%.2f)", t_magenta());
    step(5.0);
    CHECK(t_magenta() > 0.7f, "still hovering after 10 s: %.2f", t_magenta());

    /* moving off starts the hold */
    mouse_away();
    CHECK(t_magenta() > 0.7f, "it was hidden the moment the mouse left (%.2f)", t_magenta());
    step(0.4);
    CHECK(t_magenta() > 0.7f, "gone 0.4 s after the mouse left (%.2f)", t_magenta());
    step(2.0);
    CHECK(t_magenta() < 0, "still drawn long after the mouse left (%.2f)", t_magenta());

    /* hovering over a short scroller shows nothing: there is nothing to scroll */
    gp_el *sh = find_scroller(win()->tree, fix()->s_short.id, 0, 0, &ax, &ay);
    if (sh) {
        gp_simulate_mouse(app, GP_SIM_MOVE, ax + 30, ay + 10, 0, 0);
        wait_draws(1);
        CHECK(t_yellow() < 0, "a thumb for nothing to scroll (%.2f)", t_yellow());
    }
    mouse_away();
    step(2.0);
}

static void test_hover_rules(void) {
    where = "test_hover_rules";
    float ax = 0.f, ay = 0.f;
    gp_el *sc = find_scroller(win()->tree, fix()->s_clipped.id, 0, 0, &ax, &ay);
    CHECK(sc != NULL, "the clipped scroller was not found: its parents did not pass on that something below them hides scrollbars");
    if (sc) {
        /* the scroller is 80 px tall but its parent shows 16: the mouse below that is over nothing visible */
        gp_simulate_mouse(app, GP_SIM_MOVE, ax + 50, ay + 50, 0, 0);
        wait_draws(1);
        CHECK(t_blue() < 0, "hovering the part of a scroller that is clipped away showed its scrollbar (%.2f)", t_blue());
        gp_simulate_mouse(app, GP_SIM_MOVE, ax + 50, ay + 8, 0, 0);
        wait_draws(1);
        wait_shown(t_blue);
        CHECK(t_blue() > 0.7f, "hovering the visible part did not show it (%.2f)", t_blue());
        verify_frame();
    }
    mouse_away();
    step(2.0);

    /* the mouse leaves the window while its last position is over the scroller: that is not hovering */
    sc = find_scroller(win()->tree, fix()->s_auto.id, 0, 0, &ax, &ay);
    if (!sc) return;
    gp_simulate_mouse(app, GP_SIM_MOVE, ax + 30, ay + 30, 0, 0);
    wait_draws(1);
    wait_shown(t_magenta);
    CHECK(t_magenta() > 0.7f, "hovering did not show it");
    gp_window_on_mouse_leave(win());
    wait_draws(1);
    step(2.0);
    CHECK(!win()->mouse_in, "the window still thinks the mouse is in it");
    CHECK(t_magenta() < 0, "a scrollbar stayed shown for a mouse that left the window (%.2f)", t_magenta());
}

static void test_hidden_thumb_cannot_be_grabbed(void) {
    where = "test_hidden_thumb_cannot_be_grabbed";
    mouse_away();
    {   /* back to the top, where the thumb is at the top of the track (this is where the press below lands) */
        gp_ctx top = gp_ctx_for(app, GP_ENTITY_NULL);
        gp_scroll_to(&top, fix()->s_auto, 0, 0);
        wait_draws(1);
    }
    step(3.0);
    CHECK(t_magenta() < 0, "precondition: hidden (%.2f)", t_magenta());
    float ax = 0.f, ay = 0.f;
    gp_el *sc = find_scroller(win()->tree, fix()->s_auto.id, 0, 0, &ax, &ay);
    if (!sc) { CHECK(false, "scroller not found"); return; }
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    float before = gp_scroll_offset(&cx, fix()->s_auto).y;
    /* press where the thumb would be, drag down, release: a hidden thumb is not there to take */
    float tx = ax + 300 - 5, ty = ay + 10;
    gp_simulate_mouse(app, GP_SIM_MOVE, tx, ty, 0, 0);
    gp_simulate_mouse(app, GP_SIM_DOWN, tx, ty, 1, 0);
    gp_simulate_mouse(app, GP_SIM_MOVE, tx, ty + 50, 0, 0);
    gp_simulate_mouse(app, GP_SIM_UP, tx, ty + 50, 1, 0);
    wait_draws(1);
    CHECK(before == 0, "precondition: the scroller is at the top (%.0f)", before);
    CHECK(gp_scroll_offset(&cx, fix()->s_auto).y == before, "a hidden thumb was dragged (%.0f -> %.0f)", before, gp_scroll_offset(&cx, fix()->s_auto).y);
    /* the mouse over the scroller showed it; now it can be taken, and dragging keeps it shown */
    gp_simulate_mouse(app, GP_SIM_MOVE, ax + 30, ay + 30, 0, 0);
    wait_draws(1);
    sc = find_scroller(win()->tree, fix()->s_auto.id, 0, 0, &ax, &ay);
    gp_thumb th;
    CHECK(sc && gp_scroll_thumb(sc, 1, ax, ay, &th), "no thumb after hovering");
    if (sc && gp_scroll_thumb(sc, 1, ax, ay, &th)) {
        float gx = th.x + th.w / 2, gy = th.y + th.h / 2;
        gp_simulate_mouse(app, GP_SIM_MOVE, gx, gy, 0, 0);
        gp_simulate_mouse(app, GP_SIM_DOWN, gx, gy, 1, 0);
        wait_draws(1);
        gp_simulate_mouse(app, GP_SIM_MOVE, gx + 120, gy + 20, 0, 0);   /* away from the scroller, still holding */
        wait_draws(1);
        step(5.0);
        CHECK(t_magenta() > 0.7f, "the thumb faded while it was being dragged (%.2f)", t_magenta());
        CHECK(gp_scroll_offset(&cx, fix()->s_auto).y > before, "dragging did not scroll");
        gp_simulate_mouse(app, GP_SIM_UP, gx + 120, gy + 20, 1, 0);
        wait_draws(1);
        step(0.4);
        CHECK(t_magenta() > 0.7f, "gone as soon as the drag ended (%.2f)", t_magenta());
        step(3.0);
        CHECK(t_magenta() < 0, "still drawn long after the drag (%.2f)", t_magenta());
    }
    mouse_away();
    step(2.0);
}

static void test_programmatic_scroll(void) {
    where = "test_programmatic_scroll";
    mouse_away();
    step(3.0);
    CHECK(t_magenta() < 0, "precondition: hidden");
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_scroll_to(&cx, fix()->s_auto, 0, 100);
    wait_draws(1);
    CHECK(t_magenta() > 0.7f, "gp_scroll_to did not show the scrollbar (%.2f)", t_magenta());
    verify_frame();
    step(3.0);
    CHECK(t_magenta() < 0, "and it did not hide again (%.2f)", t_magenta());
}

static void test_list_and_cached(void) {
    where = "test_list_and_cached";
    mouse_away();
    step(3.0);
    CHECK(t_orange() < 0, "the list's thumb is drawn at rest (%.2f)", t_orange());
    CHECK(t_green() < 0, "the cached view's thumb is drawn at rest (%.2f)", t_green());
    wheel_over(fix()->s_list, 60);
    wait_draws(1);
    CHECK(t_orange() > 0.7f, "a gp_list's scrollbar does not show on scrolling (%.2f)", t_orange());
    verify_frame();
    step(3.0);
    CHECK(t_orange() < 0, "...or hide (%.2f)", t_orange());

    int renders = fix()->renders;
    wheel_over(fix()->s_cached, 30);
    wait_draws(1);
    CHECK(t_green() > 0.7f, "a scrollbar in a cached view does not show on scrolling (%.2f)", t_green());
    verify_frame();
    float seen = t_green();
    int partial = 0;
    for (int i = 0; i < 30 && t_green() >= 0; i++) {
        step(0.05 + (i < 18 ? 0.05 : 0));
        float a = t_green();
        if (a > 0.02f && a < seen - 0.02f) partial++;
    }
    CHECK(partial >= 1, "the fade in a cached view had %d in-between frames", partial);
    CHECK(t_green() < 0, "a cached view's scrollbar stays drawn (%.2f)", t_green());
    CHECK(fix()->renders == renders, "the window's view rendered %d times for a scrollbar", fix()->renders - renders);

    /* the cached view's layout and paint are reused while the bar is hidden */
    wait_draws(1);
    unsigned long hits = app->stat_paint_hits;
    win()->dirty_paint = true;
    wait_draws(1);
    CHECK(app->stat_paint_hits > hits, "a view with a hidden auto-hiding scrollbar is not reused from its painted copy");
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    gp_sleep(co, 0.3);
    test_hidden_until_scrolled();
    test_fades();
    test_shown_steadily_costs_nothing_until_it_ends();
    test_expires_by_itself();
    test_hover();
    test_hover_rules();
    test_hidden_thumb_cannot_be_grabbed();
    test_programmatic_scroll();
    test_list_and_cached();
    gp_debug_clock_skew(0);
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    Fix init = { .s_auto = gp_scroll_new(app), .s_plain = gp_scroll_new(app), .s_short = gp_scroll_new(app), .s_list = gp_scroll_new(app),
                 .s_cached = gp_scroll_new(app), .s_clipped = gp_scroll_new(app) };
    root = gp_view_new(app, sizeof(Fix), &init, render, NULL);
    cached_view = gp_view_new(app, 0, NULL, cached_render, NULL);
    fix()->cached = cached_view;
    gp_open_window(app, &(gp_window_options){ .title = "autohide_test", .width = 340, .height = 600 }, root);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);   /* a physical pointer over the window is not part of the test */
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "autohide_test: %d FAILURE(S)\n" : "autohide_test: all checks passed\n", failures);
    return failures != 0;
}
