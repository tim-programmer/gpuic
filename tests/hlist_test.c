/*
 * gp_list_horizontal: a uniform list laid out along x. A strip of 100,000 items of 80 px: only the items in view are built, they sit at
 * idx * 80 (plus the padding) and are as tall as the list, the extent is the whole strip, the wheel (sideways), the thumb (on the x axis only),
 * gp_scroll_to, gp_list_scroll_to_item in all its alignments, clicks and touch work along x; a list whose items are not given a width takes
 * the first item's, and its height too if it was not given one; padding and a border move the items and the extent. Needs a display.
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
            if (failures++ < 40) {                                                     \
                printf("FAIL %s:%d: ", __FILE__, __LINE__);                            \
                printf(__VA_ARGS__);                                                   \
                printf("\n");                                                          \
            }                                                                          \
        }                                                                              \
    } while (0)

static gp_app *app;
static gp_co *co_;
static gp_window *win(void) { return app->windows[0]; }

#define N 100000
typedef struct { gp_scroll a, b, c; int clicks, last; } Fix;

static void on_item(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) { Fix *f = self; f->clicks++; f->last = (int)(intptr_t)data; }
static gp_node *item(void *self, gp_ctx *cx, size_t i, void *data) {
    return gp_div(gp_bg(i % 2 ? gp_rgb(0x2020a0) : gp_rgb(0xa02020)), gp_on_click_data(on_item, (void *)(intptr_t)i), gp_text(gp_fmt("item %zu", i)));
}
static gp_node *sized(void *self, gp_ctx *cx, size_t i, void *data) {   /* its own size: 55 x 30 */
    return gp_div(gp_w(55), gp_h(30), gp_bg(gp_rgb(0x20a020)), gp_text(gp_fmt("s%zu", i)));
}

static gp_node *render(void *self, gp_ctx *cx) {
    Fix *f = self;
    return gp_div(gp_size_full(), gp_flex_col(), gp_p(10), gp_gap(10),
                  gp_uniform_list(N, 80, item, NULL, gp_track_scroll(f->a), gp_list_horizontal(), gp_flex_none(), gp_w_full(), gp_h(100)),
                  gp_uniform_list(N, 0, sized, NULL, gp_track_scroll(f->b), gp_list_horizontal(), gp_flex_none(), gp_w_full()),
                  gp_uniform_list(N, 80, item, NULL, gp_track_scroll(f->c), gp_list_horizontal(), gp_flex_none(), gp_w_full(), gp_h(60), gp_px(15), gp_py(5), gp_border(3), gp_border_color(gp_rgb(0xffffff))));
}

static bool wait_draws(unsigned n) {
    unsigned long target = app->stat_draws + n;
    for (int i = 0; i < 125 && app->stat_draws < target; i++) gp_sleep(co_, 0.004);
    return app->stat_draws >= target;
}
static gp_ctx cx_;
static float offx(gp_scroll s) { return gp_scroll_offset(&cx_, s).x; }
static float offy(gp_scroll s) { return gp_scroll_offset(&cx_, s).y; }
static bool near(float a, float b, float tol) { return fabsf(a - b) <= tol; }
static const gp_el *list_el(int i) { return win()->tree->children[i]; }
static size_t count_els(const gp_el *n) {
    size_t c = 1;
    for (uint32_t i = 0; i < n->nchildren; i++) c += count_els(n->children[i]);
    return c;
}
static const char *text_of(const gp_el *n) {
    while (n && n->kind != GP_EL_TEXT && n->nchildren) n = n->children[0];
    return n && n->kind == GP_EL_TEXT ? n->text : NULL;
}

static void run(Fix *f) {
    wait_draws(2);
    cx_ = gp_ctx_for(app, GP_ENTITY_NULL);
    const gp_el *l = list_el(0);
    CHECK(l->kind == GP_EL_LIST && l->list_horiz && l->style.overflow_x_scroll && !l->style.overflow_y_scroll, "it is a list that scrolls along x");
    CHECK(near(l->size.w, 380, 0.01f) && near(l->size.h, 100, 0.01f), "380 x 100: %g x %g", l->size.w, l->size.h);
    /* only what is in view is built, side by side, as tall as the list */
    CHECK(l->list_first == 0 && l->list_n == 5 && l->nchildren == 5, "five items in 380 px of 80: first %zu, %zu built", l->list_first, l->list_n);
    bool placed = true;
    for (uint32_t i = 0; i < l->nchildren; i++) placed &= near(l->children[i]->pos.x, 80.f * (float)i, 0.01f) && near(l->children[i]->pos.y, 0, 0.01f) && near(l->children[i]->size.w, 80, 0.01f) && near(l->children[i]->size.h, 100, 0.01f);
    CHECK(placed, "items at idx * 80, 80 x 100");
    CHECK(near(l->scroll_max.x, (float)N * 80 - 380, 1.f) && l->scroll_max.y == 0, "the extent is the whole strip: %g", l->scroll_max.x);
    CHECK(count_els(win()->tree) < 100, "a small tree: %zu elements", count_els(win()->tree));
    gp_thumb t;
    CHECK(gp_scroll_thumb(l, 0, 10, 10, &t) && !gp_scroll_thumb(l, 1, 10, 10, &t), "a thumb along x and none along y");

    /* the wheel, sideways: 160 px = two items */
    gp_scroll_wheel(win(), (gp_point){ 50, 50 }, 160, 0);
    wait_draws(2);
    l = list_el(0);
    CHECK(near(offx(f->a), 160, 0.01f) && l->list_first == 2 && text_of(l->children[0]) && strcmp(text_of(l->children[0]), "item 2") == 0, "after 160 px: item %zu first (%s), offset %g", l->list_first, text_of(l->children[0]), offx(f->a));
    gp_scroll_wheel(win(), (gp_point){ 50, 50 }, 0, 100);   /* up and down is not this list's */
    wait_draws(1);
    CHECK(near(offx(f->a), 160, 0.01f) && offy(f->a) == 0, "a vertical wheel turn does nothing here");
    gp_scroll_wheel(win(), (gp_point){ 50, 50 }, 30, 0);   /* between two items: three built beyond the five */
    wait_draws(2);
    l = list_el(0);
    CHECK(l->list_first == 2 && l->list_n == 6, "190 px along: items 2 to 7 (%zu from %zu)", l->list_n, l->list_first);

    /* a jump far along, and the items there */
    gp_scroll_to(&cx_, f->a, 80.f * 50000 + 40, 0);
    wait_draws(2);
    l = list_el(0);
    CHECK(l->list_first == 50000 && text_of(l->children[0]) && strcmp(text_of(l->children[0]), "item 50000") == 0 && count_els(win()->tree) < 100, "a jump to the middle: first %zu, %zu elements", l->list_first, count_els(win()->tree));
    CHECK(near(l->children[0]->pos.x, 80.f * 50000, 1.f), "item 50000 is at %g", l->children[0]->pos.x);

    /* gp_list_scroll_to_item: start, centre, nearest in view (no change) and nearest beyond the right edge */
    gp_list_scroll_to_item(&cx_, f->a, 1234, GP_ALIGN_ROW_START);
    wait_draws(2);
    CHECK(near(offx(f->a), 1234 * 80, 0.01f), "start: %g", offx(f->a));
    gp_list_scroll_to_item(&cx_, f->a, 2000, GP_ALIGN_ROW_CENTER);
    wait_draws(2);
    CHECK(near(offx(f->a), 2000 * 80 - (380 - 80) / 2.f, 0.01f), "centre: %g", offx(f->a));
    float before = offx(f->a);
    gp_list_scroll_to_item(&cx_, f->a, 2000, GP_ALIGN_ROW_NEAREST);
    wait_draws(1);
    CHECK(near(offx(f->a), before, 0.01f), "nearest, already in view: no move");
    gp_list_scroll_to_item(&cx_, f->a, 2010, GP_ALIGN_ROW_NEAREST);
    wait_draws(2);
    CHECK(near(offx(f->a), 2010 * 80 + 80 - 380, 0.01f), "nearest, beyond the right edge: its right edge to the view's: %g", offx(f->a));
    gp_list_scroll_to_item(&cx_, f->a, 100, GP_ALIGN_ROW_NEAREST);
    wait_draws(2);
    CHECK(near(offx(f->a), 100 * 80, 0.01f), "nearest, beyond the left edge: %g", offx(f->a));

    /* a click is the item under it; the position is the list's origin (10, 10) plus x */
    f->clicks = 0;
    gp_scroll_to(&cx_, f->a, 80.f * 300 + 25, 0);
    wait_draws(2);
    gp_simulate_mouse(app, GP_SIM_MOVE, 10 + 100, 10 + 50, 0, 0);   /* 100 px into the list: 125 px into item 300: item 301 */
    gp_simulate_mouse(app, GP_SIM_DOWN, 10 + 100, 10 + 50, 1, 0);
    gp_simulate_mouse(app, GP_SIM_UP, 10 + 100, 10 + 50, 1, 0);
    wait_draws(1);
    gp_sleep(co_, 0.1);
    CHECK(f->clicks == 1 && f->last == 301, "a click at 100 px with the strip 25 px along item 300: item %d", f->last);

    /* a finger drags it along: the content follows */
    gp_scroll_to(&cx_, f->a, 0, 0);
    wait_draws(2);
    gp_simulate_touch(app, GP_TOUCH_DOWN, 300, 60, 0);
    gp_simulate_touch(app, GP_TOUCH_MOVE, 290, 60, 0.01);
    gp_simulate_touch(app, GP_TOUCH_MOVE, 190, 60, 0.5);
    gp_simulate_touch(app, GP_TOUCH_UP, 190, 60, 0.6);
    CHECK(near(offx(f->a), 100, 0.01f), "a touch drag of 100 px to the left: %g", offx(f->a));

    /* items that are not given a width: the first one's (55); the list as tall as it (30) */
    wait_draws(2);
    const gp_el *b = list_el(1);
    CHECK(near(b->size.h, 30, 0.01f) && b->list_ih == 55.f && b->list_n >= 6, "no sizes given: items %g wide, list %g high, %zu built", b->list_ih, b->size.h, b->list_n);
    CHECK(near(b->children[1]->pos.x, 55, 0.01f), "item 1 at 55");

    /* padding and a border: the items start inside them, the extent includes them */
    const gp_el *c = list_el(2);
    CHECK(near(c->children[0]->pos.x, 3 + 15, 0.01f) && near(c->children[0]->pos.y, 3 + 5, 0.01f) && near(c->children[0]->size.h, 60 - 2 * 8, 0.01f), "padding 15 and 5 inside border 3: first item at (%g, %g), %g high", c->children[0]->pos.x, c->children[0]->pos.y, c->children[0]->size.h);
    CHECK(near(c->scroll_max.x, (float)N * 80 + 30 - (380 - 6), 1.f), "extent with padding: %g", c->scroll_max.x);
    gp_scroll_to(&cx_, f->c, 1000, 0);
    wait_draws(2);
    c = list_el(2);
    /* the first item in view is the one under the padding edge: (1000 - 15) / 80 = 12 */
    CHECK(c->list_first == 12, "padding is not scrolled over: first item %zu at offset 1000", c->list_first);
    /* an item with only its last 5 px in view (item 12 spans 975..1055, the view starts at 1050) is still built */
    gp_scroll_to(&cx_, f->c, 1050, 0);
    wait_draws(2);
    c = list_el(2);
    CHECK(c->list_first == 12, "an item showing 5 px at the edge is built: first item %zu at offset 1050", c->list_first);
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    run(gp_entity_get(app, win()->root_view));
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    Fix init;
    memset(&init, 0, sizeof init);
    init.a = gp_scroll_new(app); init.b = gp_scroll_new(app); init.c = gp_scroll_new(app);
    gp_entity v = gp_view_new(app, sizeof init, &init, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "hlist_test", .width = 400, .height = 340 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "hlist_test: %d FAILURE(S)\n" : "hlist_test: all checks passed\n", failures);
    return failures != 0;
}
