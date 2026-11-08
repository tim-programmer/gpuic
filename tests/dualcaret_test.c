/*
 * The secondary caret: at a boundary between text of one direction and text of the other a caret has two places on screen (the end of the character
 * before, the start of the one after), the field draws the one typing goes to as the caret and the other as a thinner, fainter one, and
 * gp_text_input_caret_pos_alt reports it. Walks the caret through a mixed line and checks where it has a second place and where it does not
 * (never inside a direction, never in text of one direction, never with a selection), that the two places are the two edges that meet at the
 * boundary, and that the second is drawn (a faint line at its x, fainter than the caret). Needs a display, at scale 1.
 */
#include <gpuic/gpuic.h>

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
static gp_entity mixed, latin, multi;

static gp_node *view(void *self, gp_ctx *cx) {
    return gp_div(gp_size_full(), gp_flex_col(), gp_p(20), gp_gap(14), gp_bg(gp_rgb(0x000000)), gp_child_view(mixed), gp_child_view(latin), gp_child_view(multi));
}

static void settle(void) { gp_sleep(co_, 0.05); }
static void focus(gp_entity e) {
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_focus_set(&cx, gp_text_input_focus(app, e));
    settle();
}
static gp_window *win(void) { return app->windows[0]; }
static uint8_t *frame(void) {
    gp_debug_capture_next(app);
    for (int i = 0; i < 1000 && !app->capture_px; i++) {
        win()->dirty_paint = true;
        gp_sleep(co_, 0.01);
    }
    return app->capture_px;
}

/* what the sweep saw at each stop */
typedef struct { float x, y, ax, ay; bool alt; } Stop;

static int sweep(gp_entity e, int steps, Stop *out) {
    focus(e);
    gp_simulate_keystrokes(app, "ctrl-home");
    settle();
    int n = 0;
    for (int i = 0; i <= steps; i++) {
        Stop *s = &out[n];
        memset(s, 0, sizeof *s);
        gp_text_input_caret_pos(app, e, &s->x, &s->y);
        s->alt = gp_text_input_caret_pos_alt(app, e, &s->ax, &s->ay);
        n++;
        if (i < steps) {
            gp_simulate_keystrokes(app, "right");
            gp_sleep(co_, 0.01);
        }
    }
    return n;
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    settle();

    /* "abc " then three Hebrew letters then " def": two direction changes. */
    gp_ctx c = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_text_input_set_text(&c, mixed, "abc \xd7\x90\xd7\x91\xd7\x92 def");
    gp_text_input_set_text(&c, latin, "abc def ghi");
    gp_text_input_set_text(&c, multi, "line one\nabc \xd7\x90\xd7\x91\xd7\x92 def\nline three");
    settle();

    Stop st[40];
    int n = sweep(mixed, 11, st);
    int with_alt = 0;
    for (int i = 0; i < n; i++) with_alt += st[i].alt;
    CHECK(with_alt >= 2 && with_alt <= 6, "a line with two direction changes has a second caret at a few stops, not at all or at all of them: %d of %d", with_alt, n);
    CHECK(!st[0].alt, "the start of the line has one place");
    for (int i = 0; i < n; i++) {
        if (!st[i].alt) continue;
        CHECK(fabsf(st[i].x - st[i].ax) > 1.f && fabsf(st[i].y - st[i].ay) < 0.5f, "stop %d: the two places %g and %g are apart, on one row", i, st[i].x, st[i].ax);
    }

    /* the second caret is drawn: the frame with the carets on against the frame a blink later, without them: what differs is the carets */
    bool checked = false;
    for (int i = 0; i < n && !checked; i++) {
        if (!st[i].alt) continue;
        focus(mixed);
        gp_simulate_keystrokes(app, "ctrl-home");
        for (int k = 0; k < i; k++) gp_simulate_keystrokes(app, "right");
        settle();   /* (the keys are events: the frame must come after them) */
        uint8_t *on = frame();
        if (!on) continue;
        size_t bytes = (size_t)app->capture_w * (size_t)app->capture_h * 3;
        uint8_t *a = malloc(bytes);
        memcpy(a, on, bytes);
        float fx, fy, ax, ay;
        gp_text_input_caret_pos(app, mixed, &fx, &fy);
        bool has = gp_text_input_caret_pos_alt(app, mixed, &ax, &ay);
        gp_sleep(co_, 0.6);   /* the caret blinks off 0.53 s after the key */
        uint8_t *off = frame();
        const gp_el *field = win()->tree->children[0];
        if (off && has) {
            int pw = app->capture_w;
            float left = field->pos.x + 1 + 10, top = field->pos.y + 1 + 7;   /* the field's border and padding (known to a pixel or two) */
            long dp = 0, da = 0;
            for (int y = (int)top; y < (int)(top + 19); y++) {
                for (int dx = -2; dx <= 3; dx++) {
                    size_t ip = ((size_t)y * (size_t)pw + (size_t)(left + fx + (float)dx)) * 3, ia = ((size_t)y * (size_t)pw + (size_t)(left + ax + (float)dx)) * 3;
                    for (int ch = 0; ch < 3; ch++) {
                        dp += abs((int)a[ip + ch] - (int)off[ip + ch]);
                        da += abs((int)a[ia + ch] - (int)off[ia + ch]);
                    }
                }
            }
            CHECK(dp > 3000, "stop %d: the caret at %.1f appears and disappears with the blink (%ld)", i, fx, dp);
            CHECK(da > dp / 10 && da < dp * 35 / 100, "stop %d: the second caret at %.1f blinks too, with less ink (%ld against %ld)", i, ax, da, dp);
            checked = true;
        }
        free(a);
    }
    CHECK(checked, "a stop with a second caret was drawn");

    /* a line in one direction never has one */
    n = sweep(latin, 11, st);
    int bad = 0;
    for (int i = 0; i < n; i++) bad += st[i].alt;
    CHECK(bad == 0, "Latin text has no second caret (%d stops)", bad);

    /* ... and a selection has no caret at all */
    n = sweep(mixed, 11, st);   /* (st was overwritten by the Latin sweep) */
    int first_alt = 0;
    while (first_alt < n && !st[first_alt].alt) first_alt++;
    focus(mixed);
    gp_simulate_keystrokes(app, "ctrl-home");
    for (int k = 0; k < first_alt; k++) gp_simulate_keystrokes(app, "shift-right");
    settle();
    float ax, ay;
    CHECK(first_alt < n && !gp_text_input_caret_pos_alt(app, mixed, &ax, &ay), "a selection ending at a boundary has no second caret (stop %d)", first_alt);

    /* a composition has none either (its own caret is the one) */
    gp_simulate_keystrokes(app, "ctrl-home");
    for (int k = 0; k < 4; k++) gp_simulate_keystrokes(app, "right");
    app->debug_ignore_platform_ime = true;
    gp_simulate_ime(app, "xy", 1);
    CHECK(!gp_text_input_caret_pos_alt(app, mixed, &ax, &ay), "a composition has no second caret");
    gp_simulate_ime(app, "", 0);

    /* in a multi-line field too, on the row it is on */
    Stop st2[40];
    n = sweep(multi, 24, st2);
    int rows_with = 0;
    for (int i = 0; i < n; i++) {
        if (!st2[i].alt) continue;
        rows_with++;
        CHECK(fabsf(st2[i].ay - st2[i].y) < 0.5f && st2[i].y > 10.f && st2[i].y < 30.f, "multi-line stop %d: the second caret is on the row of the first (y %g, %g), the second row", i, st2[i].y, st2[i].ay);
    }
    CHECK(rows_with >= 2, "the middle line has them (%d)", rows_with);

    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    gp_entity r = gp_view_new(app, 1, &(char){ 0 }, view, NULL);
    mixed = gp_text_input_new(app, &(gp_text_input_options){ .width = 260 });
    latin = gp_text_input_new(app, &(gp_text_input_options){ .width = 260 });
    multi = gp_text_input_new(app, &(gp_text_input_options){ .width = 260, .multiline = true, .rows = 4 });
    gp_open_window(app, &(gp_window_options){ .title = "dualcaret_test", .width = 320, .height = 300 }, r);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "dualcaret_test: %d FAILURE(S)\n" : "dualcaret_test: all checks passed\n", failures);
    return failures != 0;
}
