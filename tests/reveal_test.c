/*
 * Scroll into view: Tab and Shift-Tab (and gp_scroll_into_view) move the scrollers around the element they focus by the least that
 * shows it. A bordered column, a row, a scroller inside a scroller (both move), an element larger than its view, wrapping around,
 * and the cases that must NOT scroll (focusing by gp_focus_set, an element already in view, an element that is not in the tree).
 * Offsets are read back from the scroll handles. Needs a display.
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
            if (failures++ < 60) {                                                     \
                printf("FAIL %s:%d: ", __FILE__, __LINE__);                            \
                printf(__VA_ARGS__);                                                   \
                printf("\n");                                                          \
            }                                                                          \
        }                                                                              \
    } while (0)

static gp_app *app;
static gp_co *co_;
static gp_window *win(void) { return app->windows[0]; }

enum { N = 10 };
typedef struct {
    gp_focus a[N], b[N], c[N], d[2];
    gp_scroll sa, sb, sc, sd, se;
} Fix;

#define RED gp_rgb(0xc03030)

static gp_node *render(void *self, gp_ctx *cx) {
    Fix *f = self;
    gp_node **a = gp_frame_alloc(N * sizeof *a), **b = gp_frame_alloc(N * sizeof *b), **c = gp_frame_alloc(N * sizeof *c);
    for (int i = 0; i < N; i++) {
        a[i] = gp_div(gp_w(80), gp_h(40), gp_bg(RED), gp_track_focus(f->a[i]), gp_tab_stop());
        b[i] = gp_div(gp_w(40), gp_h(40), gp_flex_none(), gp_bg(RED), gp_track_focus(f->b[i]), gp_tab_stop());
        c[i] = gp_div(gp_w(80), gp_h(40), gp_bg(RED), gp_track_focus(f->c[i]), gp_tab_stop());
    }
    return gp_div(
        gp_size_full(), gp_flex_row(), gp_items_start(), gp_gap(10), gp_p(10),
        /* A: bordered column, view 92 high inside the border */
        gp_div(gp_w(100), gp_h(100), gp_border(4), gp_border_color(RED), gp_overflow_y_scroll(), gp_track_scroll(f->sa), gp_children(a, N)),
        /* B: a row */
        gp_div(gp_w(100), gp_h(50), gp_flex_row(), gp_overflow_x_scroll(), gp_track_scroll(f->sb), gp_children(b, N)),
        /* C: a scroller in a scroller */
        gp_div(gp_w(100), gp_h(120), gp_overflow_y_scroll(), gp_track_scroll(f->sc),
               gp_div(gp_h(200), gp_flex_none()),
               gp_div(gp_w(100), gp_h(100), gp_flex_none(), gp_overflow_y_scroll(), gp_track_scroll(f->sd), gp_children(c, N)),
               gp_div(gp_h(100), gp_flex_none())),
        /* D: one tall element, then a small one */
        gp_div(gp_w(100), gp_h(100), gp_overflow_y_scroll(), gp_track_scroll(f->se),
               gp_div(gp_h(50), gp_flex_none()),
               gp_div(gp_w(80), gp_h(300), gp_flex_none(), gp_bg(RED), gp_track_focus(f->d[0]), gp_tab_stop()),
               gp_div(gp_w(80), gp_h(20), gp_flex_none(), gp_bg(RED), gp_track_focus(f->d[1]), gp_tab_stop())));
}

static gp_ctx cx_;
static void settle(void) { gp_sleep(co_, 0.15); }
static void tab(int n, bool shift) {
    for (int i = 0; i < n; i++) {
        gp_simulate_keystrokes(app, shift ? "shift-tab" : "tab");
        settle();
    }
}
static void focus(gp_focus f) { gp_focus_set(&cx_, f); settle(); }
static bool near(float a, float b) { return fabsf(a - b) < 0.6f; }
static gp_point off(gp_scroll s) { return gp_scroll_offset(&cx_, s); }
#define OFFY(s, want, what) CHECK(near(off(s).y, (want)), what ": y offset %g, wanted %g", off(s).y, (float)(want))
#define OFFX(s, want, what) CHECK(near(off(s).x, (want)), what ": x offset %g, wanted %g", off(s).x, (float)(want))

static void run(Fix *f) {
    settle();
    cx_ = gp_ctx_for(app, GP_ENTITY_NULL);

    /* A: the view is [4, 96] inside the border; item i is at 4 + 40 i */
    focus(f->a[0]);
    OFFY(f->sa, 0, "focusing by gp_focus_set does not scroll");
    focus(f->a[9]);
    OFFY(f->sa, 0, "...not even an element far out of view");
    focus(f->a[0]);
    tab(1, false);
    OFFY(f->sa, 0, "A1 is in view");
    tab(1, false);
    OFFY(f->sa, 28, "A2 sticks out at the bottom: its bottom edge (124) goes to the view's (96)");
    tab(1, false);
    OFFY(f->sa, 68, "A3");
    tab(1, true);
    OFFY(f->sa, 68, "back to A2: in view, no move");
    tab(2, true);
    OFFY(f->sa, 0, "back to A0: its top edge goes to the view's");

    /* B: a row of 40-wide items in a view 100 wide */
    focus(f->a[9]);
    tab(1, false);
    OFFX(f->sb, 0, "B0");
    tab(2, false);
    OFFX(f->sb, 20, "B2 (80..120)");
    tab(1, false);
    OFFX(f->sb, 60, "B3");
    OFFY(f->sb, 0, "B has nothing to scroll across");
    tab(1, true);
    OFFX(f->sb, 60, "B2 is in view");
    tab(3, true);
    OFFX(f->sb, 0, "B... back to the start");

    /* C: the inner scroller starts 200 down the outer one */
    focus(f->b[9]);
    tab(1, false);
    OFFY(f->sc, 120, "C0 is at 200..240 of the outer content, view 120 high: the outer scrolls");
    OFFY(f->sd, 0, "...the inner stays");
    tab(1, false);
    OFFY(f->sc, 160, "C1 (240..280)");
    OFFY(f->sd, 0, "...");
    tab(1, false);
    OFFY(f->sd, 20, "C2 sticks out of the inner view: the inner scrolls");
    OFFY(f->sc, 180, "...and the outer by what that left (it is at 260..300 now)");
    tab(1, true);
    OFFY(f->sc, 180, "C1 is in view of both");
    OFFY(f->sd, 20, "...");

    /* D: 50 down, a tall element (50..350) and then a small one (350..370), in a view 100 high */
    focus(f->c[9]);
    tab(1, false);
    OFFY(f->se, 50, "D0 is taller than the view and begins inside it: its top edge goes to the view's");
    tab(1, false);
    OFFY(f->se, 270, "D1 (350..370): its bottom goes to the view's");
    tab(1, true);
    OFFY(f->se, 50, "D0 again: it ends inside the view, so it is the top edge that decides");
    gp_scroll_to(&cx_, f->se, 0, 120);
    settle();
    gp_scroll_into_view(&cx_, f->d[0]);
    settle();
    OFFY(f->se, 120, "D0 fills the view from above to below: left alone");

    /* wrapping around, and the explicit call */
    gp_scroll_to(&cx_, f->sa, 0, 68);
    settle();
    focus(f->d[1]);
    tab(1, false);
    OFFY(f->sa, 0, "Tab from the last stop wraps to A0 and shows it");
    focus(f->a[0]);
    gp_scroll_into_view(&cx_, f->a[5]);
    settle();
    OFFY(f->sa, 148, "gp_scroll_into_view: A5 (204..244) to the bottom");
    gp_scroll_into_view(&cx_, gp_focus_new(app));   /* tracked by nothing: ignored */
    gp_scroll_into_view(&cx_, (gp_focus){ 0 });
    settle();
    OFFY(f->sa, 148, "an unknown handle changes nothing");
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    Fix *fx = gp_entity_get(app, win()->root_view);
    run(fx);
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    Fix init;
    memset(&init, 0, sizeof init);
    for (int i = 0; i < N; i++) { init.a[i] = gp_focus_new(app); init.b[i] = gp_focus_new(app); init.c[i] = gp_focus_new(app); }
    init.d[0] = gp_focus_new(app);
    init.d[1] = gp_focus_new(app);
    init.sa = gp_scroll_new(app); init.sb = gp_scroll_new(app); init.sc = gp_scroll_new(app); init.sd = gp_scroll_new(app); init.se = gp_scroll_new(app);
    gp_entity v = gp_view_new(app, sizeof init, &init, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "reveal_test", .width = 520, .height = 300 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "reveal_test: %d FAILURE(S)\n" : "reveal_test: all checks passed\n", failures);
    return failures != 0;
}
