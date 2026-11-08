/*
 * gp_list_sticky: the group header pinned at the top of a gp_uniform_list and of a gp_list (rows of different heights). 10 groups of 10 rows,
 * the first row of each being its header. Scrolled through the whole list in steps (and at fractions of a row), the pinned header is the
 * group of the first row in view, appears only once the group's own header row has gone past the top, sits at the top of the view, and is pushed
 * up by the next group's header as it arrives (checked against where that row is). It is above the rows (the pixels show it), takes clicks and
 * hover in its own area and lets the rows below take theirs, follows a jump, and goes with the list. Needs a display.
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

#define GROUPS 10
#define PER 10
#define HDR_BG gp_rgb(0x20a020)
#define ROW_BG gp_rgb(0x2020a0)

typedef struct { gp_scroll u, v; int header_clicks, row_clicks, last_row; } Fix;

static bool is_header_row(size_t i) { return i % PER == 0; }
static float row_h_var(size_t i) { return is_header_row(i) ? 30.f : 20.f + (float)(i % 3) * 6.f; }

static void on_header(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) { ((Fix *)self)->header_clicks++; }
static void on_row(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) { Fix *f = self; f->row_clicks++; f->last_row = (int)(intptr_t)data; }

static gp_node *header_node(size_t g) {
    return gp_div(gp_h(30), gp_bg(HDR_BG), gp_on_click(on_header), gp_text(gp_fmt("Group %zu", g)));
}
static gp_node *row_u(void *self, gp_ctx *cx, size_t i, void *data) {
    if (is_header_row(i)) return header_node(i / PER);
    return gp_div(gp_h(30), gp_bg(ROW_BG), gp_on_click_data(on_row, (void *)(intptr_t)i), gp_text(gp_fmt("row %zu", i)));
}
static gp_node *row_v(void *self, gp_ctx *cx, size_t i, void *data) {
    if (is_header_row(i)) return header_node(i / PER);
    return gp_div(gp_h(row_h_var(i)), gp_bg(ROW_BG), gp_on_click_data(on_row, (void *)(intptr_t)i), gp_text(gp_fmt("row %zu", i)));
}
static gp_list_group group_of(void *self, gp_ctx *cx, size_t row, void *data) {
    size_t g = row / PER;
    return (gp_list_group){ header_node(g), g * PER, (g + 1) * PER };
}

static gp_node *render(void *self, gp_ctx *cx) {
    Fix *f = self;
    return gp_div(gp_size_full(), gp_flex_row(), gp_items_start(), gp_gap(20), gp_p(10),
                  gp_uniform_list(GROUPS * PER, 30, row_u, NULL, gp_w(200), gp_h(150), gp_track_scroll(f->u), gp_list_sticky(group_of, NULL)),
                  gp_list(GROUPS * PER, 30, row_v, NULL, gp_w(200), gp_h(150), gp_track_scroll(f->v), gp_list_sticky(group_of, NULL)));
}

static bool wait_draws(unsigned n) {
    unsigned long target = app->stat_draws + n;
    for (int i = 0; i < 125 && app->stat_draws < target; i++) gp_sleep(co_, 0.004);
    return app->stat_draws >= target;
}
static gp_ctx cx_;
static float offy(gp_scroll s) { return gp_scroll_offset(&cx_, s).y; }
static const gp_el *list_el(int i) { return win()->tree->children[i]; }
static const char *sticky_text(const gp_el *l) {
    const gp_el *s = l->sticky;
    while (s && s->kind != GP_EL_TEXT && s->nchildren) s = s->children[0];
    return s && s->kind == GP_EL_TEXT ? s->text : NULL;
}
static bool near(float a, float b, float tol) { return fabsf(a - b) <= tol; }

/* the realized row `row` of list l: its top from the top of the view */
static bool row_top(const gp_el *l, size_t row, float *top) {
    if (row < l->list_first || row >= l->list_first + l->list_n) return false;
    *top = l->children[row - l->list_first]->pos.y - l->scroll.y - l->style.border_w;
    return true;
}

/* what the pinned header should be at this offset, worked out from where the rows are */
static void check_list(const gp_el *l, const char *name, float y) {
    size_t top = (size_t)-1;
    for (size_t r = l->list_first; r < l->list_first + l->list_n; r++) {
        float t = 0, h = l->children[r - l->list_first]->size.h;
        row_top(l, r, &t);
        if (t + h > 0.001f) { top = r; break; }
    }
    if (top == (size_t)-1) { CHECK(false, "%s at %g: no row at the top", name, y); return; }
    float t0 = 0;
    row_top(l, top, &t0);
    size_t g = top / PER, first = g * PER;
    bool want = top > first || (top == first && t0 < -0.5f);
    const char *txt = sticky_text(l);
    if (!want) { CHECK(l->sticky == NULL, "%s at %g: row %zu is the first of its group and at the top: nothing is pinned yet (found \"%s\")", name, y, top, txt ? txt : ""); return; }
    char expect[32];
    snprintf(expect, sizeof expect, "Group %zu", g);
    CHECK(l->sticky && txt && strcmp(txt, expect) == 0, "%s at %g (row %zu on top): pinned \"%s\", wanted \"%s\"", name, y, top, txt ? txt : "(none)", expect);
    if (!l->sticky) return;
    float next_top = 1e9f, h = l->sticky->size.h, shift = 0;
    if (row_top(l, (g + 1) * PER, &next_top) && next_top < h) shift = next_top - h;
    CHECK(near(l->sticky->pos.y - l->style.border_w, shift, 0.01f), "%s at %g: the pinned header is %g from the top of the view, wanted %g (the next group's row is at %g)", name, y, l->sticky->pos.y - l->style.border_w, shift, next_top);
    CHECK(near(l->sticky->size.h, 30, 0.01f) && near(l->sticky->size.w, 200, 0.01f), "%s: header size %gx%g", name, l->sticky->size.w, l->sticky->size.h);
}

static void run(Fix *f) {
    wait_draws(2);
    cx_ = gp_ctx_for(app, GP_ENTITY_NULL);
    const char *names[2] = { "uniform", "variable" };
    gp_scroll hs[2] = { f->u, f->v };
    for (int li = 0; li < 2; li++) {
        gp_scroll_to(&cx_, hs[li], 0, 0);
        wait_draws(2);
        check_list(list_el(li), names[li], 0);
        /* through the whole list, in steps that cross every kind of position (a row is 30 px at most 38) */
        int steps = 0;
        for (float y = 0; y < (li ? 3300.f : 2850.f); y += 17.f) {
            gp_scroll_to(&cx_, hs[li], 0, y);
            wait_draws(1);
            check_list(list_el(li), names[li], offy(hs[li]));
            if (li == 0) {   /* the uniform list's rows are 30 px: where everything is follows from the offset alone */
                float oy = offy(hs[0]);
                size_t g = (size_t)(oy / 30) / PER;
                bool pinned = oy - (float)(g * PER) * 30 > 0.5f;
                float shift = fminf(0.f, ((float)((g + 1) * PER) * 30 - oy) - 30);
                const gp_el *l = list_el(0);
                CHECK((l->sticky != NULL) == pinned, "uniform at %g: pinned %d, wanted %d", oy, l->sticky != NULL, pinned);
                if (pinned && l->sticky) {
                    char want[32];
                    snprintf(want, sizeof want, "Group %zu", g);
                    const char *t = sticky_text(l);
                    CHECK(t && strcmp(t, want) == 0 && near(l->sticky->pos.y - l->style.border_w, shift, 0.01f), "uniform at %g: header \"%s\" at %g, wanted \"%s\" at %g", oy, t ? t : "", l->sticky->pos.y - l->style.border_w, want, shift);
                }
            }
            steps++;
        }
        CHECK(steps > 150, "%s: %d positions checked", names[li], steps);
    }
    /* by the wheel (the realize that follows is not the layout's) */
    {
        const gp_el *l = list_el(0);
        float ax = 10 + 20, ay = 10 + 20;
        gp_scroll_to(&cx_, f->u, 0, 0);
        wait_draws(2);
        for (int i = 0; i < 20; i++) {
            gp_scroll_wheel(win(), (gp_point){ ax, ay }, 0, 37);
            wait_draws(1);
            check_list(l, "uniform, wheel", offy(f->u));
        }
    }
    /* a jump far into the middle is pinned at once */
    gp_scroll_to(&cx_, f->u, 0, 4 * PER * 30 + 55);
    wait_draws(2);
    CHECK(sticky_text(list_el(0)) && strcmp(sticky_text(list_el(0)), "Group 4") == 0, "a jump into group 4: \"%s\"", sticky_text(list_el(0)) ? sticky_text(list_el(0)) : "(none)");

    /* the header covers the rows under it and takes the mouse there; below it the rows do */
    f->header_clicks = f->row_clicks = 0;
    gp_scroll_to(&cx_, f->u, 0, 4 * PER * 30 + 55);   /* row 41 is partly under the top: the pinned "Group 4" covers rows 40 and 41 */
    wait_draws(2);
    const gp_el *l = list_el(0);
    float lx = 10, ly = 10;   /* the list's origin in the window (root padding) */
    {
        gp_simulate_mouse(app, GP_SIM_MOVE, lx + 50, ly + 12, 0, 0);
        gp_simulate_mouse(app, GP_SIM_DOWN, lx + 50, ly + 12, 1, 0);
        gp_simulate_mouse(app, GP_SIM_UP, lx + 50, ly + 12, 1, 0);
        wait_draws(1);
        gp_sleep(co_, 0.1);
        CHECK(f->header_clicks == 1 && f->row_clicks == 0, "a click on the pinned header is the header's (%d, %d)", f->header_clicks, f->row_clicks);
        gp_simulate_mouse(app, GP_SIM_MOVE, lx + 50, ly + 30 + 12, 0, 0);
        gp_simulate_mouse(app, GP_SIM_DOWN, lx + 50, ly + 30 + 12, 1, 0);
        gp_simulate_mouse(app, GP_SIM_UP, lx + 50, ly + 30 + 12, 1, 0);
        wait_draws(1);
        gp_sleep(co_, 0.1);
        CHECK(f->row_clicks == 1 && f->last_row >= 42 && f->last_row <= 43, "a click just below it is a row's: row %d (%d)", f->last_row, f->row_clicks);
    }
    /* the pixels: the header's green at the top of the view, the rows' blue below it */
    {
        gp_debug_capture_next(app);
        for (int i = 0; i < 300 && !app->capture_px; i++) { win()->dirty_paint = true; gp_sleep(co_, 0.01); }
        const uint8_t *px = app->capture_px;
        CHECK(px != NULL, "no frame");
        if (px) {
            int pw = app->capture_w;
            float s = win()->scale;
            int x = (int)((lx + 150) * s), y_top = (int)((ly + 3) * s), y_below = (int)((ly + 40) * s);
            const uint8_t *a = px + ((size_t)y_top * (size_t)pw + (size_t)x) * 3, *b = px + ((size_t)y_below * (size_t)pw + (size_t)x) * 3;
            CHECK(a[1] > 120 && a[0] < 80 && a[2] < 80, "the top of the view is the header's green: %d %d %d", a[0], a[1], a[2]);
            CHECK(b[2] > 120 && b[1] < 80, "below it are rows: %d %d %d", b[0], b[1], b[2]);
        }
    }
    (void)l;
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
    init.u = gp_scroll_new(app);
    init.v = gp_scroll_new(app);
    gp_entity v = gp_view_new(app, sizeof init, &init, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "sticky_test", .width = 480, .height = 200 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "sticky_test: %d FAILURE(S)\n" : "sticky_test: all checks passed\n", failures);
    return failures != 0;
}
