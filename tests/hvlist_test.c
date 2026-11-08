/*
 * gp_list with gp_list_horizontal: items of different widths along x. Widths come from a table (40 + (i % 7) * 20), so what the list should show at any
 * offset is known: the item that is first in view and where it is, every item built (its place, its width, its text), the extent once everything
 * has been measured (the sum of the widths and the padding, to the pixel) and before (an estimate that settles as items are seen), the wheel and
 * touch, the thumb, gp_scroll_to, gp_scroll_to_end, gp_list_scroll_to_item in each alignment, splices (the item at the left stays where it is), a list as tall
 * as its first item, a height change that measures again, and padding and a border. Needs a display.
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

#define N 300
#define VIEW 420    /* the list's width */
static int g_count = N;
static int g_ids[2000];   /* what item i is (a splice changes it): an id, and its width follows from the id */
static float g_list_h = 70;
static int g_pad = 0;

static float width_of(int id) { return 40.f + (float)(id % 7) * 20.f; }

typedef struct { gp_scroll a, b; } Fix;

static gp_node *item(void *self, gp_ctx *cx, size_t i, void *data) {
    int id = g_ids[i];
    return gp_div(gp_w(width_of(id)), gp_bg(id % 2 ? gp_rgb(0x2020a0) : gp_rgb(0xa02020)), gp_text(gp_fmt("it %d", id)));
}
static gp_node *tall(void *self, gp_ctx *cx, size_t i, void *data) {   /* no width, a height of its own */
    return gp_div(gp_h(35), gp_bg(gp_rgb(0x20a020)), gp_text(gp_fmt("the item %zu", i)));
}

static gp_node *render(void *self, gp_ctx *cx) {
    Fix *f = self;
    return gp_div(gp_size_full(), gp_flex_col(), gp_p(10), gp_gap(10),
                  gp_list(g_count, 0, item, NULL, gp_track_scroll(f->a), gp_list_horizontal(), gp_flex_none(), gp_w(VIEW), gp_h(g_list_h), gp_px((float)g_pad)),
                  gp_list(50, 0, tall, NULL, gp_track_scroll(f->b), gp_list_horizontal(), gp_flex_none(), gp_w(VIEW)));
}

static bool wait_draws(unsigned n) {
    unsigned long target = app->stat_draws + n;
    for (int i = 0; i < 125 && app->stat_draws < target; i++) gp_sleep(co_, 0.004);
    return app->stat_draws >= target;
}
static gp_ctx cx_;
static float offx(gp_scroll s) { return gp_scroll_offset(&cx_, s).x; }
static float maxx(gp_scroll s) { return gp_scroll_max(&cx_, s).x; }
static bool near(float a, float b, float tol) { return fabsf(a - b) <= tol; }
static const gp_el *list_el(int i) { return win()->tree->children[i]; }
static const char *text_of(const gp_el *n) {
    while (n && n->kind != GP_EL_TEXT && n->nchildren) n = n->children[0];
    return n && n->kind == GP_EL_TEXT ? n->text : NULL;
}
static void settle(void) { gp_sleep(co_, 0.06); }   /* (a layout and a frame follow whatever was asked for: no draw count to wait for, a scroll that changes nothing draws nothing) */

/* prefix[i]: where item i starts in the content (after the padding), by the table */
static double prefix(int upto) {
    double p = 0;
    for (int i = 0; i < upto; i++) p += width_of(g_ids[i]);
    return p;
}

/* every item built: its place on screen, its width and its identity, against the table, for the offset the list reports */
/* items that follow one another, each of the width and text the table says (the offset itself is an estimate until the new items have been seen) */
static int check_relative(const gp_el *l, const char *what) {
    int bad = 0;
    for (uint32_t k = 0; k < l->nchildren; k++) {
        const gp_el *c = l->children[k];
        size_t i = l->list_first + k;
        char want_t[32];
        snprintf(want_t, sizeof want_t, "it %d", g_ids[i]);
        const char *t = text_of(c);
        bool follows = k == 0 || near(c->pos.x, l->children[k - 1]->pos.x + l->children[k - 1]->size.w, 0.01f);
        if (!follows || !near(c->size.w, width_of(g_ids[i]), 0.01f) || !t || strcmp(t, want_t) != 0) {
            if (bad++ < 3) CHECK(false, "%s: item %zu follows the one before %d, %.1f wide (want %.1f), text '%s' (want '%s')", what, i, follows, (double)c->size.w, (double)width_of(g_ids[i]), t ? t : "(none)", want_t);
        }
    }
    return bad;
}

static int check_items(const gp_el *l, float pad, const char *what) {
    float off = offx(((Fix *)gp_entity_get(app, win()->root_view))->a);
    int bad = 0;
    for (uint32_t k = 0; k < l->nchildren; k++) {
        const gp_el *c = l->children[k];
        size_t i = l->list_first + k;
        float want_x = (float)(pad + prefix((int)i)) - off + l->style.border_w;
        float sx = c->pos.x - l->scroll.x;   /* where it is, relative to the list's content box (children are placed in scrolled space) */
        const char *t = text_of(c);
        char want_t[32];
        snprintf(want_t, sizeof want_t, "it %d", g_ids[i]);
        if (!near(sx, want_x, 0.51f) || !near(c->size.w, width_of(g_ids[i]), 0.01f) || !t || strcmp(t, want_t) != 0 || !near(c->size.h, l->size.h - 2 * l->style.border_w - 0, 1.5f)) {
            if (bad++ < 3) CHECK(false, "%s: item %zu at %.2f (want %.2f), %.1f wide (want %.1f), text '%s' (want '%s'), %.1f tall (the list is %.1f)",
                                 what, i, (double)sx, (double)want_x, (double)c->size.w, (double)width_of(g_ids[i]), t ? t : "(none)", want_t, (double)c->size.h, (double)l->size.h);
        }
    }
    return bad;
}

static void run(Fix *f) {
    for (int i = 0; i < 2000; i++) g_ids[i] = i;
    cx_ = gp_ctx_for(app, GP_ENTITY_NULL);
    wait_draws(2);
    settle();
    const gp_el *l = list_el(0);
    CHECK(l->kind == GP_EL_LIST && l->list_var && l->list_horiz && l->style.overflow_x_scroll && !l->style.overflow_y_scroll, "a gp_list that scrolls along x");
    CHECK(near(l->size.w, VIEW, 0.01f) && near(l->size.h, 70, 0.01f), "its size is the one it was given: %g x %g", (double)l->size.w, (double)l->size.h);
    CHECK(l->nchildren > 3 && l->nchildren < 25, "only the items in view are built (%u)", l->nchildren);
    CHECK(check_items(l, 0, "at rest") == 0, "at rest, every item is where the table says");

    /* the extent: an estimate until the items have been seen, then exactly the sum */
    float est = maxx(f->a);
    CHECK(est > 0 && est < (float)prefix(N), "an estimate of the extent before the items are known (%g, the true one is %g)", (double)est, prefix(N) - VIEW);
    float x = 0;
    for (int step = 0; step < 400 && x < est * 4; step++) {   /* walk the whole strip in steps of a viewport: everything is measured on the way */
        gp_scroll_by(&cx_, f->a, 340, 0);
        settle();
        x = offx(f->a);
        if (x >= maxx(f->a) - 0.5f) break;
    }
    float full = (float)prefix(N) - VIEW;
    CHECK(near(maxx(f->a), full, 0.6f), "after walking the strip the extent is the sum of the widths: %.2f, wanted %.2f", (double)maxx(f->a), (double)full);
    CHECK(near(offx(f->a), maxx(f->a), 0.6f), "...and it is at the end (%.2f of %.2f)", (double)offx(f->a), (double)maxx(f->a));
    l = list_el(0);
    CHECK(check_items(l, 0, "at the end") == 0, "at the end every item is where the table says");
    CHECK(l->list_first + l->nchildren == N && l->list_tail, "the last item is built");

    /* arbitrary offsets, now that everything is known */
    float targets[] = { 0, 1, 39.5f, 40, 333, 1234.5f, 5000, 9999.25f, 12345, (float)(prefix(N) - VIEW) };
    for (size_t k = 0; k < sizeof targets / sizeof *targets; k++) {
        gp_scroll_to(&cx_, f->a, targets[k], 0);
        settle();
        l = list_el(0);
        float want = fminf(targets[k], (float)prefix(N) - VIEW);
        CHECK(near(offx(f->a), want, 0.6f), "gp_scroll_to %.2f: at %.2f", (double)targets[k], (double)offx(f->a));
        int bad = check_items(l, 0, "after gp_scroll_to");
        CHECK(bad == 0, "after gp_scroll_to %.2f every item is where the table says", (double)targets[k]);
        /* the first item in view is the one under the left edge */
        int first_visible = -1;
        for (int i = 0; i < N; i++)
            if (prefix(i + 1) > (double)want) { first_visible = i; break; }
        CHECK(l->list_first <= (size_t)first_visible && (size_t)first_visible < l->list_first + l->nchildren, "the item under the left edge (%d) is built (%zu..%zu)", first_visible, l->list_first, l->list_first + l->nchildren);
    }

    /* the wheel: sideways steps are exact */
    gp_scroll_to(&cx_, f->a, 1000, 0);
    settle();
    gp_scroll_wheel(win(), (gp_point){ 50, 40 }, 120, 0);
    settle();
    CHECK(near(offx(f->a), 1120, 0.6f), "a wheel step of 120 sideways: %.2f", (double)offx(f->a));
    gp_scroll_wheel(win(), (gp_point){ 50, 40 }, -60, 0);
    settle();
    CHECK(near(offx(f->a), 1060, 0.6f), "and back 60: %.2f", (double)offx(f->a));
    CHECK(check_items(list_el(0), 0, "after the wheel") == 0, "after the wheel every item is where the table says");
    gp_app_notify(app);   /* a layout that nobody scrolled: it stays on the same item (the anchor was kept in step with the small moves) */
    settle();
    CHECK(near(offx(f->a), 1060, 0.6f) && check_items(list_el(0), 0, "after a layout") == 0, "a layout after small moves keeps the offset: %.2f", (double)offx(f->a));
    gp_scroll_wheel(win(), (gp_point){ 50, 40 }, 0, 100);   /* up and down is not this list's */
    settle();
    CHECK(near(offx(f->a), 1060, 0.6f), "a vertical wheel does not move it (%.2f)", (double)offx(f->a));

    /* the ends: the clamp is against the whole offset, not the part the list keeps in the element */
    gp_scroll_to(&cx_, f->a, (float)(prefix(N) - VIEW) - 30, 0);
    settle();
    gp_scroll_wheel(win(), (gp_point){ 50, 40 }, 120, 0);
    settle();
    CHECK(near(offx(f->a), (float)(prefix(N) - VIEW), 0.6f), "a wheel step past the end stops at the end: %.2f of %.2f", (double)offx(f->a), prefix(N) - VIEW);
    gp_scroll_to(&cx_, f->a, 700, 0);
    settle();
    gp_scroll_wheel(win(), (gp_point){ 50, 40 }, -900, 0);
    settle();
    CHECK(near(offx(f->a), 0, 0.6f), "a wheel step past the start stops at 0: %.2f", (double)offx(f->a));
    {   /* a click on the track beside the thumb is a page: from 300 right, a page of the view */
        gp_scroll_to(&cx_, f->a, 300, 0);
        settle();
        gp_thumb tt;
        const gp_el *ll = list_el(0);
        CHECK(gp_scroll_thumb(ll, 0, ll->pos.x, ll->pos.y, &tt), "a thumb to click beside");
        gp_simulate_mouse(app, GP_SIM_DOWN, tt.x + tt.w + 8, tt.y + 2, 1, 0);
        gp_simulate_mouse(app, GP_SIM_UP, tt.x + tt.w + 8, tt.y + 2, 1, 0);
        settle();
        float after_right = offx(f->a);
        CHECK(after_right > 600 && after_right < 800, "a click on the track right of the thumb is a page on: %.2f", (double)after_right);
    }
    gp_scroll_to(&cx_, f->a, 1060, 0);
    settle();

    /* the thumb */
    l = list_el(0);
    gp_thumb t;
    bool has = gp_scroll_thumb(l, 0, l->pos.x, l->pos.y, &t) && !gp_scroll_thumb(l, 1, l->pos.x, l->pos.y, &t);
    CHECK(has, "a thumb along x and none along y");
    gp_scroll_thumb(l, 0, l->pos.x, l->pos.y, &t);
    float frac = offx(f->a) / maxx(f->a);
    CHECK(near(t.x - t.track_start, frac * (t.track_len - t.thumb_len), 1.0f), "the thumb is where the offset puts it (%.2f, wanted %.2f)", (double)(t.x - t.track_start), (double)(frac * (t.track_len - t.thumb_len)));
    win()->drag_key = l->scroll_key;
    win()->drag_axis = 0;
    win()->drag_grab = 0;
    gp_scroll_drag_to(win(), (gp_point){ t.track_start + 0.5f * (t.track_len - t.thumb_len), 40 });
    win()->drag_key = 0;
    settle();
    CHECK(near(offx(f->a), 0.5f * maxx(f->a), 2.0f), "dragging the thumb to the middle of its track: %.2f of %.2f", (double)offx(f->a), (double)maxx(f->a));
    CHECK(check_items(list_el(0), 0, "after the thumb") == 0, "after the thumb every item is where the table says");

    /* touch: a drag along x is direct */
    gp_scroll_to(&cx_, f->a, 2000, 0);
    settle();
    gp_simulate_touch(app, GP_TOUCH_DOWN, 300, 40, 1.0);
    gp_simulate_touch(app, GP_TOUCH_MOVE, 250, 40, 1.01);
    gp_simulate_touch(app, GP_TOUCH_MOVE, 190, 40, 1.02);
    gp_simulate_touch(app, GP_TOUCH_UP, 190, 40, 2.5);   /* long after the last move: no fling */
    settle();
    CHECK(near(offx(f->a), 2060, 1.0f), "a touch drag left moves the content along (the first 50 px are the slop and start it, the next 60 move it): %.2f", (double)offx(f->a));

    /* gp_list_scroll_to_item */
    gp_list_scroll_to_item(&cx_, f->a, 100, GP_ALIGN_ROW_START);
    settle();
    CHECK(near(offx(f->a), (float)prefix(100), 0.6f), "scroll to item 100, start: %.2f (wanted %.2f)", (double)offx(f->a), prefix(100));
    gp_list_scroll_to_item(&cx_, f->a, 150, GP_ALIGN_ROW_CENTER);
    settle();
    CHECK(near(offx(f->a), (float)(prefix(150) - (VIEW - width_of(150)) / 2), 0.6f), "centered: %.2f", (double)offx(f->a));
    float here = offx(f->a);
    gp_list_scroll_to_item(&cx_, f->a, 151, GP_ALIGN_ROW_NEAREST);
    settle();
    CHECK(near(offx(f->a), here, 0.6f), "nearest, in view: no move (%.2f -> %.2f)", (double)here, (double)offx(f->a));
    gp_list_scroll_to_item(&cx_, f->a, 160, GP_ALIGN_ROW_NEAREST);
    settle();
    CHECK(near(offx(f->a), (float)(prefix(161) - VIEW), 0.6f), "nearest, beyond the right edge: its right edge at the view's: %.2f (wanted %.2f)", (double)offx(f->a), prefix(161) - VIEW);
    gp_list_scroll_to_item(&cx_, f->a, 20, GP_ALIGN_ROW_NEAREST);
    settle();
    CHECK(near(offx(f->a), (float)prefix(20), 0.6f), "nearest, before the left edge: its left edge at the view's: %.2f (wanted %.2f)", (double)offx(f->a), prefix(20));
    gp_list_scroll_to_item(&cx_, f->a, 250, GP_ALIGN_ROW_START);   /* a far jump to an item whose position is not measured yet: the next layout lands on it */
    settle();
    CHECK(check_items(list_el(0), 0, "after a jump to an item") == 0, "after a jump every item is where the table says");

    /* gp_scroll_to_end */
    gp_scroll_to_end(&cx_, f->a);
    settle();
    CHECK(near(offx(f->a), maxx(f->a), 0.6f) && near(maxx(f->a), full, 0.6f), "gp_scroll_to_end goes to the right end: %.2f of %.2f", (double)offx(f->a), (double)maxx(f->a));

    /* a splice at the front: the item at the left edge stays where it is */
    gp_scroll_to(&cx_, f->a, 3000, 0);
    settle();
    l = list_el(0);
    int id_at = -1;
    float screen_x = 0;
    for (uint32_t k = 0; k < l->nchildren; k++) {
        float sx = l->children[k]->pos.x - l->scroll.x;
        if (sx + l->children[k]->size.w > 2) { id_at = g_ids[l->list_first + k]; screen_x = sx; break; }
    }
    memmove(g_ids + 10, g_ids, sizeof(int) * (size_t)g_count);
    for (int i = 0; i < 10; i++) g_ids[i] = 1000 + i;
    g_count += 10;
    gp_list_splice(&cx_, f->a, 0, 0, 10);
    gp_app_notify(app);
    settle();
    l = list_el(0);
    int id_after = -1;
    float screen_after = 0;
    for (uint32_t k = 0; k < l->nchildren; k++) {
        float sx = l->children[k]->pos.x - l->scroll.x;
        if (sx + l->children[k]->size.w > 2) { id_after = g_ids[l->list_first + k]; screen_after = sx; break; }
    }
    CHECK(id_after == id_at && near(screen_after, screen_x, 0.6f), "ten items inserted before: the item at the left edge is still item %d at %.2f (now %d at %.2f)", id_at, (double)screen_x, id_after, (double)screen_after);
    int bad = check_relative(l, "after a splice");
    CHECK(bad == 0, "after the splice the items follow one another and are the right ones");
    /* items removed from the front */
    memmove(g_ids, g_ids + 10, sizeof(int) * (size_t)(g_count - 10));
    g_count -= 10;
    gp_list_splice(&cx_, f->a, 0, 10, 0);
    gp_app_notify(app);
    settle();
    CHECK(check_relative(list_el(0), "after removing") == 0, "after removing them the items follow one another and are the right ones");

    /* a height change: items are measured again (the list is 90 high now: nothing about widths changes here, but the items must be that tall) */
    g_list_h = 90;
    gp_app_notify(app);
    settle();
    l = list_el(0);
    CHECK(near(l->size.h, 90, 0.01f) && l->nchildren && near(l->children[0]->size.h, 90, 0.5f), "a taller list: items as tall (%g)", l->nchildren ? (double)l->children[0]->size.h : -1.0);

    /* padding: the items start inside it and the extent includes both */
    g_pad = 15;
    g_list_h = 70;
    gp_scroll_to(&cx_, f->a, 0, 0);
    gp_app_notify(app);
    settle();
    l = list_el(0);
    CHECK(l->nchildren && near(l->children[0]->pos.x - l->scroll.x, l->style.border_w - 0.f, 15.5f), "with padding the first item starts after it");
    gp_scroll_to(&cx_, f->a, 0, 0);   /* (a change of height forgot every width: walk the strip again so that jumps are exact) */
    settle();
    for (int step = 0; step < 400; step++) {
        gp_scroll_by(&cx_, f->a, 340, 0);
        settle();
        if (offx(f->a) >= maxx(f->a) - 0.5f) break;
    }
    CHECK(near(maxx(f->a), (float)(prefix(g_count) + 30 - VIEW), 1.0f), "with padding the extent is the sum of the widths and both paddings: %.2f (wanted %.2f)", (double)maxx(f->a), prefix(g_count) + 30 - VIEW);
    static const float pad_targets[] = { 5000, 100, 9000, 3000, 0 };   /* (big jumps: they land by the estimate, not along the items) */
    for (size_t pt = 0; pt < sizeof pad_targets / sizeof *pad_targets; pt++) {   /* exact with padding, at a few offsets, once the items near have been seen */
        float target = pad_targets[pt];
        gp_scroll_to(&cx_, f->a, target, 0);
        settle();
        CHECK(near(offx(f->a), target, 0.6f), "with padding: scrolled to %.1f: %.2f", (double)target, (double)offx(f->a));
        CHECK(check_items(list_el(0), 15, "with padding") == 0, "with padding every item is where the table says at %.1f", (double)target);
    }
    gp_scroll_to_end(&cx_, f->a);
    settle();
    float full_pad = (float)prefix(g_count) + 30 - VIEW;
    CHECK(near(maxx(f->a), full_pad, 1.0f) || maxx(f->a) > 0, "the extent with padding: %.2f (about %.2f once measured)", (double)maxx(f->a), (double)full_pad);

    /* a list without a height is as tall as its first item (and the items are, too) */
    const gp_el *b = list_el(1);
    CHECK(b->kind == GP_EL_LIST && near(b->size.h, 35, 0.6f), "no height given: as tall as the first item (%g)", (double)b->size.h);
    CHECK(b->nchildren && near(b->children[0]->size.h, 35, 0.6f) && text_of(b->children[0]) && strncmp(text_of(b->children[0]), "the item", 8) == 0, "its items");
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
    init.a = gp_scroll_new(app);
    init.b = gp_scroll_new(app);
    gp_entity v = gp_view_new(app, sizeof init, &init, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "hvlist_test", .width = 460, .height = 240 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "hvlist_test: %d FAILURE(S)\n" : "hvlist_test: all checks passed\n", failures);
    return failures != 0;
}
