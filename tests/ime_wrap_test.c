/*
 * A composition (IME preedit) in a soft-wrapped field wraps like text. The oracle is a second field of the same size holding the same text
 * committed: while composing, the first one must have the same rows (their count, and the row and x of the caret where the composition's own
 * caret is) as the second with that text in it, wherever the composition was started (at the end, in the middle of a line, before a space,
 * in the second paragraph), for Latin and CJK compositions, and the rows must go back when it ends or is cancelled. Needs a display.
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
static gp_entity fa, fb;   /* the composing one, the one with the text committed */

static gp_node *view(void *self, gp_ctx *cx) {
    return gp_div(gp_size_full(), gp_flex_col(), gp_p(20), gp_gap(20), gp_bg(gp_rgb(0x1e1e2e)), gp_child_view(fa), gp_child_view(fb));
}

static void settle(void) { gp_sleep(co_, 0.05); }
static void focus(gp_entity e) {
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_focus_set(&cx, gp_text_input_focus(app, e));
    settle();
}
static void set(gp_entity e, const char *s) {
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_text_input_set_text(&cx, e, s);
    settle();
}
static void home(gp_entity e) {
    focus(e);
    gp_simulate_keystrokes(app, "ctrl-home");
    settle();
}
static void right(gp_entity e, int n) {
    for (int i = 0; i < n; i++) gp_simulate_keystrokes(app, "right");
    settle();
}

#define CJK "\xe3\x81\x82\xe3\x81\x84\xe3\x81\x86\xe3\x81\x88\xe3\x81\x8a\xe3\x81\x8b\xe3\x81\x8d\xe3\x81\x8f\xe3\x81\x91\xe3\x81\x93" /* 10 hiragana */

/* The text of a scenario: `before`, the composition, `after`; the caret of the composing field is after `before` (chars). */
typedef struct { const char *name, *before, *pre, *after; int cursor; } Scenario;

static int chars(const char *s) { int n = 0; for (; *s; s++) n += ((unsigned char)*s & 0xc0) != 0x80; return n; }

static void run_scenario(const Scenario *sc) {
    char whole[1024];
    snprintf(whole, sizeof whole, "%s%s%s", sc->before, sc->pre, sc->after);

    /* the oracle: the same text committed, the caret where the composition's own caret is */
    set(fb, whole);
    home(fb);
    int steps = chars(sc->before) + sc->cursor;
    for (int i = 0; i < steps; i++) gp_simulate_keystrokes(app, "right");
    settle();
    size_t want_rows = gp_text_input_rows(app, fb);
    float wx = 0, wy = 0;
    bool wok = gp_text_input_caret_pos(app, fb, &wx, &wy);
    size_t want_row = gp_text_input_caret_row(app, fb);

    /* the composing field: the text without it, the caret where it starts, then the composition */
    char base[1024];
    snprintf(base, sizeof base, "%s%s", sc->before, sc->after);
    set(fa, base);
    home(fa);
    right(fa, chars(sc->before));
    size_t plain_rows = gp_text_input_rows(app, fa);
    gp_simulate_ime(app, sc->pre, sc->cursor);   /* (read at once: the display server sends its own empty compositions on a timer, which end it) */
    size_t rows = gp_text_input_rows(app, fa);
    float x = 0, y = 0;
    bool ok = gp_text_input_caret_pos(app, fa, &x, &y);
    size_t row = gp_text_input_caret_row(app, fa);
    CHECK(rows == want_rows, "%s: %zu rows while composing, %zu with the text committed (%zu before)", sc->name, rows, want_rows, plain_rows);
    CHECK(ok && wok && fabsf(x - wx) < 0.6f && fabsf(y - wy) < 0.6f, "%s: the composition's caret at %.1f,%.1f, the committed text's at %.1f,%.1f", sc->name, x, y, wx, wy);
    CHECK(row == want_row, "%s: caret row %zu, committed %zu", sc->name, row, want_row);
    CHECK(strcmp(gp_text_input_text(app, fa), base) == 0, "%s: the composition is not in the text", sc->name);

    /* the rows come back when the composition is cancelled */
    gp_simulate_ime(app, "", 0);
    CHECK(gp_text_input_rows(app, fa) == plain_rows, "%s: %zu rows after cancelling, %zu before", sc->name, gp_text_input_rows(app, fa), plain_rows);
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

/* How much ink (pixels unlike the field's background) is in each text row band of the field child `idx` of the root. */
static void ink(const uint8_t *px, int idx, size_t nrows, long *out) {
    const gp_el *f = win()->tree->children[idx];
    int pw = app->capture_w;
    float lh = 15 * GP_LINE_HEIGHT;
    for (size_t r = 0; r < nrows; r++) {
        long n = 0;
        int y0 = (int)(f->pos.y + 8 + (float)r * lh), y1 = y0 + (int)lh - 7;   /* (the composition's underline is below the band) */
        int x0 = (int)f->pos.x + 3, x1 = (int)(f->pos.x + f->size.w) - 3;
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) {
                const uint8_t *p = px + ((size_t)y * (size_t)pw + (size_t)x) * 3;
                if (p[0] > 120 && p[1] > 120 && p[2] > 120) n++;
            }
        out[r] = n;
    }
}

static void test_pixels(void) {
    /* a long composition drawn: row for row the ink of the committed text, nothing run past the edge (the floor on the ink
     * is only "something was drawn": the CJK fallback on Windows is lighter than Noto) */
    const char *base = "hello world", *pre = CJK CJK CJK CJK CJK;
    char whole[512];
    snprintf(whole, sizeof whole, "%s%s", base, pre);
    set(fb, whole);
    set(fa, base);
    focus(fa);
    gp_simulate_keystrokes(app, "ctrl-end");
    settle();
    gp_simulate_ime(app, pre, 50);
    settle();
    CHECK(strlen(gp_text_input_preedit(app, fa)) > 0, "the composition is still there");
    size_t rows = gp_text_input_rows(app, fa);
    uint8_t *f = frame();
    CHECK(f != NULL && rows == 5, "a frame while composing, %zu rows", rows);
    if (!f || rows != 5) return;
    long a[8] = { 0 }, b[8] = { 0 };
    ink(f, 0, rows, a);
    ink(f, 1, rows, b);
    for (size_t r = 0; r < rows; r++)
        CHECK(a[r] > 100 && labs(a[r] - b[r]) < 60, "row %zu: %ld ink pixels composing, %ld committed", r, a[r], b[r]);
    gp_simulate_ime(app, "", 0);
    settle();
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    app->debug_ignore_platform_ime = true;   /* a composition then stays until the test ends it */
    settle();
    CHECK(gp_text_input_rows(app, fa) == 1, "an empty field is a row");

    /* ten hiragana are ~150 px: a 200 px wide field has room for about 11 per row */
    static const Scenario scenarios[] = {
        { "short at the end", "hello", CJK, "", 10 },
        { "long at the end", "hello world", CJK CJK CJK CJK CJK, "", 50 },
        { "long, caret in the middle of it", "hello world", CJK CJK CJK CJK CJK, "", 23 },
        { "long, caret at its start", "hello world", CJK CJK CJK CJK CJK, "", 0 },
        { "long, in the middle of a line", "hello", CJK CJK CJK CJK, " and the rest of the line", 40 },
        { "long, before a space", "one two", "abcdefghij abcdefghij abcdefghij", " tail", 32 },
        { "latin words wrap at spaces", "start ", "alpha beta gamma delta epsilon zeta eta theta", "", 47 },
        { "one long word", "", "supercalifragilisticexpialidocious_and_more_and_more", "", 52 },
        { "in the second paragraph", "first\nsecond ", CJK CJK CJK CJK CJK CJK, "\nthird", 60 },
    };
    focus(fa);
    for (size_t i = 0; i < sizeof scenarios / sizeof *scenarios; i++) {
        focus(fa);
        run_scenario(&scenarios[i]);
    }

    /* the composition ends exactly where its row ends and the text after it needs the next row: its own caret stays at its end, on the row it ends */
    {
        char pre6[64] = "";
        for (int i = 0; i < 6; i++) strcat(pre6, "\xe3\x81\x82");
        set(fa, "hello world\xe3\x81\x82");   /* (the char after the caret does not fit the row) */
        focus(fa);
        gp_simulate_keystrokes(app, "ctrl-home");
        right(fa, 11);
        gp_simulate_ime(app, pre6, 6);
        size_t rows = gp_text_input_rows(app, fa);
        float x = 0, y = 0;
        bool ok = gp_text_input_caret_pos(app, fa, &x, &y);
        CHECK(rows == 2 && ok && fabsf(y) < 0.6f && x > 150.f && gp_text_input_caret_row(app, fa) == 0,
              "composition ending its row: %zu rows, caret at %.1f,%.1f on row %zu", rows, x, y, gp_text_input_caret_row(app, fa));
        gp_simulate_ime(app, "", 0);
    }

    /* a composition that grows one character at a time, as typing does: the rows follow it each step */
    set(fa, "hello");
    focus(fa);
    gp_simulate_keystrokes(app, "ctrl-end");
    settle();
    size_t last_rows = 0;
    char pre[256] = "";
    for (int i = 1; i <= 40; i++) {
        strcat(pre, "\xe3\x81\x82");
        gp_simulate_ime(app, pre, i);
        size_t rows = gp_text_input_rows(app, fa);
        CHECK(rows >= last_rows, "growing: %zu rows at %d characters after %zu", rows, i, last_rows);
        last_rows = rows;
    }
    CHECK(last_rows >= 3, "40 characters make %zu rows", last_rows);
    gp_simulate_text(app, pre);   /* the commit */
    settle();
    CHECK(strlen(gp_text_input_preedit(app, fa)) == 0 && gp_text_input_rows(app, fa) == last_rows, "committed: %zu rows", gp_text_input_rows(app, fa));

    test_pixels();

    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    gp_entity r = gp_view_new(app, 1, &(char){ 0 }, view, NULL);
    fa = gp_text_input_new(app, &(gp_text_input_options){ .multiline = true, .soft_wrap = true, .rows = 6, .width = 200 });
    fb = gp_text_input_new(app, &(gp_text_input_options){ .multiline = true, .soft_wrap = true, .rows = 6, .width = 200 });
    gp_open_window(app, &(gp_window_options){ .title = "ime_wrap_test", .width = 300, .height = 520 }, r);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "ime_wrap_test: %d FAILURE(S)\n" : "ime_wrap_test: all checks passed\n", failures);
    return failures != 0;
}
