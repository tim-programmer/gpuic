/*
 * The pointer, in three parts. Needs a display.
 *  - A drag's events carry the modifiers held at that moment (each move, and the release), not those of the press.
 *  - A drag stays with the element pressed: three boxes, the pressed one raised to the end of its siblings (as a rack raises the panel being dragged),
 *    then two moves and a release, each followed by a rebuild. The handlers' data is in the frame's arena (as a C++ lambda's captures are), so the
 *    drag must keep that frame's data too. Without keys and with them; and once more with the pressed box gone from the tree while it is dragged.
 *  - gp_on_hover: nested elements (entering the child does not leave the parent, even where the child is outside the parent), a rotated one (its corners are not in it), a child clipped by its
 *    parent, a curve (on the stroke, not in its box), one that appears under a pointer that did not move and goes away again (told with the data
 *    it had), and the pointer leaving the window.
 */
#include <gpuic/gpuic.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...)                                                               \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            failures++;                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                \
            printf(__VA_ARGS__);                                                       \
            printf("\n");                                                              \
        }                                                                              \
    } while (0)

static gp_app *app;
static gp_co *co_;
static gp_entity g_root;

enum { MODE_MODS, MODE_RAISE, MODE_HOVER };
static int g_mode;

/* ---- modifiers */
static uint32_t g_down_mods, g_up_mods, g_drag_mods[8];
static int g_ndrag_mods;

static void mods_down(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) { g_down_mods = ev->mods; }
static void mods_drag(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    if (g_ndrag_mods < 8) g_drag_mods[g_ndrag_mods++] = ev->mods;
}
static void mods_up(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) { g_up_mods = ev->mods; }

/* ---- raise */
static int g_order[3] = { 0, 1, 2 };
static bool g_keys, g_hide_pressed;
static int g_pressed = -1;
static int g_drags[3], g_ups[3];

static void reset_raise(bool keys, bool hide) {
    for (int i = 0; i < 3; i++) g_order[i] = i, g_drags[i] = g_ups[i] = 0;
    g_keys = keys;
    g_hide_pressed = hide;
    g_pressed = -1;
}

static void box_down(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    int id = *(int *)data;
    g_pressed = id;
    int k = 0;   /* to the end of its siblings */
    for (int i = 0; i < 3; i++)
        if (g_order[i] != id) g_order[k++] = g_order[i];
    g_order[2] = id;
    gp_notify(cx);
}
static void box_drag(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    g_drags[*(int *)data]++;
    gp_notify(cx);   /* (every move builds the tree again) */
}
static void box_up(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    g_ups[*(int *)data]++;
    g_pressed = -1;
    gp_notify(cx);
}

static gp_node *raise_box(int id) {
    if (g_hide_pressed && id == g_pressed) return NULL;
    int *d = gp_frame_alloc(sizeof *d);   /* the handler's data lives in this frame */
    *d = id;
    static const uint32_t colors[3] = { 0xcc4444, 0x44cc44, 0x4444cc };
    return gp_div(gp_absolute(), gp_left(20.f + 120.f * (float)id), gp_top(100), gp_w(100), gp_h(80), gp_bg(gp_rgb(colors[id])),
                  g_keys ? gp_key_id((uint64_t)id + 1) : NULL, gp_on_mouse_down_data(box_down, d), gp_on_drag_data(box_drag, d),
                  gp_on_mouse_up_data(box_up, d));
}

/* ---- hover */
static char g_log[512];
static bool g_show_t;

static void hovered(void *self, gp_ctx *cx, bool inside, void *data) {
    size_t n = strlen(g_log);
    snprintf(g_log + n, sizeof g_log - n, "%s%c%s", n ? " " : "", inside ? '+' : '-', (const char *)data);
}

static gp_node *on_hover_named(const char *name) {
    char *d = gp_frame_alloc(strlen(name) + 1);   /* (in the frame: an element that went away is told with what it had) */
    strcpy(d, name);
    return gp_on_hover_data(hovered, d);
}

static gp_node *hover_tree(void) {
    gp_point k0 = { 20, 280 }, k1 = { 60, 200 }, k2 = { 160, 200 }, k3 = { 200, 280 };   /* (gp_curve is a macro: no braces in its arguments) */
    return gp_div(gp_size_full(),
                  /* P (20,20 200x150), clipping: C in it, C2 hanging out of it at the bottom right */
                  gp_div(gp_absolute(), gp_left(20), gp_top(20), gp_w(200), gp_h(150), gp_overflow_hidden(), gp_bg(gp_rgb(0x334455)), on_hover_named("P"),
                         gp_div(gp_absolute(), gp_left(20), gp_top(20), gp_w(80), gp_h(60), gp_bg(gp_rgb(0x556677)), on_hover_named("C")),
                         gp_div(gp_absolute(), gp_left(180), gp_top(100), gp_w(60), gp_h(60), gp_bg(gp_rgb(0x778899)), on_hover_named("C2"))),
                  /* S: 100 x 100 at 260,20 turned 45 degrees about its center (310,70) */
                  gp_div(gp_absolute(), gp_left(260), gp_top(20), gp_w(100), gp_h(100), gp_rotate(45), gp_bg(gp_rgb(0x664422)), on_hover_named("S")),
                  /* K: a curve bulging up to y = 220 in the middle; its box reaches down to 280 */
                  gp_curve(k0, k1, k2, k3, gp_stroke_width(6),
                           gp_stroke_color(gp_rgb(0xaaaa44)), on_hover_named("K")),
                  /* Q (230,250 40x40), not clipping: QC hangs out of it to the right (290..330) */
                  gp_div(gp_absolute(), gp_left(230), gp_top(250), gp_w(40), gp_h(40), gp_bg(gp_rgb(0x553355)), on_hover_named("Q"),
                         gp_div(gp_absolute(), gp_left(60), gp_top(0), gp_w(40), gp_h(40), gp_bg(gp_rgb(0x775577)), on_hover_named("QC"))),
                  /* T: comes and goes (the last child, so that the others keep their places) */
                  g_show_t ? gp_div(gp_absolute(), gp_left(300), gp_top(200), gp_w(60), gp_h(40), gp_bg(gp_rgb(0x44aaaa)), on_hover_named("T")) : NULL);
}

static gp_node *root(void *self, gp_ctx *cx) {
    switch (g_mode) {
    case MODE_MODS:
        return gp_div(gp_size_full(), gp_div(gp_absolute(), gp_left(20), gp_top(20), gp_w(100), gp_h(100), gp_bg(gp_rgb(0x445566)),
                                             gp_on_mouse_down(mods_down), gp_on_drag(mods_drag), gp_on_mouse_up(mods_up)));
    case MODE_RAISE: {
        gp_node *kids[3];
        for (int i = 0; i < 3; i++) kids[i] = raise_box(g_order[i]);
        return gp_div(gp_size_full(), kids[0], kids[1], kids[2]);
    }
    default: return hover_tree();
    }
}

static void wait_draws(unsigned n) {
    gp_stats s;
    gp_app_stats(app, &s);
    unsigned long target = s.draws + n;
    for (int i = 0; i < 1000; i++) {
        gp_app_stats(app, &s);
        if (s.draws >= target) return;
        if (i > 5) gp_app_request_paint(app);
        gp_sleep(co_, 0.004);
    }
}

static void sim(int kind, float x, float y, uint32_t mods) {
    gp_simulate_mouse(app, kind, x, y, 1, mods);
    wait_draws(3);
}

static void set_mode(int mode) {
    g_mode = mode;
    gp_ctx c = gp_ctx_for(app, g_root);
    gp_notify(&c);
    wait_draws(2);
}

static void test_mods(void) {
    set_mode(MODE_MODS);
    sim(GP_SIM_MOVE, 50, 50, 0);
    sim(GP_SIM_DOWN, 50, 50, GP_MOD_SHIFT);
    sim(GP_SIM_MOVE, 60, 50, 0);
    sim(GP_SIM_MOVE, 70, 50, GP_MOD_SHIFT);
    sim(GP_SIM_MOVE, 80, 50, GP_MOD_CTRL | GP_MOD_SHIFT);
    sim(GP_SIM_MOVE, 200, 250, GP_MOD_ALT);   /* (outside the element: still its drag) */
    sim(GP_SIM_UP, 200, 250, GP_MOD_CTRL);
    CHECK(g_down_mods == GP_MOD_SHIFT, "the press sees shift: %#x", g_down_mods);
    CHECK(g_ndrag_mods == 4, "four drag events: %d", g_ndrag_mods);
    uint32_t want[4] = { 0, GP_MOD_SHIFT, GP_MOD_CTRL | GP_MOD_SHIFT, GP_MOD_ALT };
    for (int i = 0; i < 4 && i < g_ndrag_mods; i++) CHECK(g_drag_mods[i] == want[i], "drag %d: mods %#x, want %#x", i, g_drag_mods[i], want[i]);
    CHECK(g_up_mods == GP_MOD_CTRL, "the release sees ctrl: %#x", g_up_mods);
}

static void test_raise(bool keys, bool hide) {
    const char *what = hide ? "the pressed box gone" : keys ? "with keys" : "without keys";
    reset_raise(keys, hide);
    set_mode(MODE_RAISE);
    /* box 0 is at 20..120 x 100..180; pressing raises it (or takes it away); every event is followed by a rebuild */
    sim(GP_SIM_MOVE, 70, 140, 0);
    sim(GP_SIM_DOWN, 70, 140, 0);
    CHECK(g_order[2] == 0, "%s: the pressed box is raised", what);
    sim(GP_SIM_MOVE, 75, 140, 0);
    sim(GP_SIM_MOVE, 80, 145, 0);
    sim(GP_SIM_UP, 80, 145, 0);
    CHECK(g_drags[0] == 2 && g_drags[1] == 0 && g_drags[2] == 0, "%s: the drags went to %d/%d/%d, want 2/0/0", what, g_drags[0], g_drags[1], g_drags[2]);
    CHECK(g_ups[0] == 1 && g_ups[1] == 0 && g_ups[2] == 0, "%s: the release went to %d/%d/%d, want 1/0/0", what, g_ups[0], g_ups[1], g_ups[2]);
}

static void step(float x, float y, const char *want, const char *what) {
    g_log[0] = 0;
    sim(GP_SIM_MOVE, x, y, 0);
    CHECK(strcmp(g_log, want) == 0, "%s (%g,%g): '%s', want '%s'", what, (double)x, (double)y, g_log, want);
}

static void test_hover(void) {
    g_show_t = false;
    sim(GP_SIM_MOVE, 390, 290, 0);   /* (away from where the elements will be) */
    set_mode(MODE_HOVER);
    step(390, 290, "", "nothing there");
    step(380, 280, "", "nothing there either");
    step(30, 30, "+P", "into the parent");
    step(60, 60, "+C", "into the child: the parent is not left");
    step(65, 62, "", "a move inside the child");
    step(30, 30, "-C", "back out of the child");
    step(390, 290, "-P", "out of the parent");
    step(60, 60, "+P +C", "straight into the child: the parent first");
    step(390, 290, "-C -P", "straight out: the child first");
    step(265, 25, "", "the corner of the turned square's box is not in it");
    step(310, 70, "+S", "its middle is");
    step(240, 130, "-S", "where the clipped child would be, outside its parent's clip");
    step(210, 130, "+P +C2", "the part of it in the clip");
    step(110, 270, "-C2 -P", "in the curve's box, off the stroke");
    step(110, 221, "+K", "on the stroke");
    step(310, 270, "-K +Q +QC", "in a child outside its parent's box (no clip): in the parent too");
    step(250, 270, "-QC", "from there into the parent itself");
    step(330, 220, "-Q", "where T will be");

    /* T appears under the pointer, and goes away */
    g_log[0] = 0;
    g_show_t = true;
    gp_ctx c = gp_ctx_for(app, g_root);
    gp_notify(&c);
    wait_draws(3);
    CHECK(strcmp(g_log, "+T") == 0, "an element appears under the pointer: '%s'", g_log);
    g_log[0] = 0;
    for (int i = 0; i < 3; i++) {   /* (a few frames built again: T's data is from the frame it was entered in) */
        gp_notify(&c);
        wait_draws(2);
    }
    CHECK(g_log[0] == 0, "rebuilt under the pointer: nothing ('%s')", g_log);
    g_show_t = false;
    gp_notify(&c);
    wait_draws(3);
    CHECK(strcmp(g_log, "-T") == 0, "it goes away: told with its own data: '%s'", g_log);

    step(60, 60, "+P +C", "back in the child");
    g_log[0] = 0;
    sim(GP_SIM_LEAVE, 0, 0, 0);
    CHECK(strcmp(g_log, "-C -P") == 0, "leaving the window leaves everything: '%s'", g_log);
    step(60, 60, "+P +C", "and coming back enters again");
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    wait_draws(3);
    test_mods();
    test_raise(false, false);
    test_raise(true, false);
    test_raise(false, true);
    test_hover();
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    g_root = gp_view_new(app, 1, &(char){ 0 }, root, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "pointer_test", .width = 400, .height = 300 }, g_root);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "pointer_test: %d FAILURE(S)\n" : "pointer_test: all checks passed\n", failures);
    return failures != 0;
}
