/*
 * End-to-end test of the text input widget. Everything goes through gp_simulate_* (real
 * platform events), so key mapping, bindings, focus, text-input gating, drag capture,
 * clipboard and IME plumbing are all exercised. Needs a display; exits non-zero on failure.
 *
 * Layout is fixed so mouse tests know where things are:  window padding 20, gap 10,
 * single-line fields are 35.5 px tall, the multi-line field (4 rows) 94 px.
 */
#include <gpuic/gpuic.h>

#include "../src/internal.h" /* gp_text_shape, to compute where characters are */

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

typedef struct {
    gp_entity a, m, c, p, r, w, w2, d;
    int changes, submits;
    char submitted[64];
    /* gp_on_layout probe: one rebuild requested from a layout handler */
    bool probe_done, probe_pending;
    unsigned long draws_at_layout, draws_at_rebuild;
} Root;

static Root *root;
static gp_app *app;

static void on_change(void *self, gp_ctx *cx, gp_entity input, void *data) { ((Root *)self)->changes++; }
static void on_submit(void *self, gp_ctx *cx, gp_entity input, void *data) {
    Root *r = self;
    r->submits++;
    snprintf(r->submitted, sizeof r->submitted, "%s", gp_text_input_text(cx->app, input));
}

static void probe_layout(void *self, gp_ctx *cx, gp_size size, void *data) {
    Root *r = self;
    if (r->probe_done) return;
    r->probe_done = r->probe_pending = true;
    r->draws_at_layout = app->stat_draws;
    gp_notify(cx);
}

static gp_node *view(void *self, gp_ctx *cx) {
    Root *r = self;
    if (r->probe_pending) { /* the rebuild the handler asked for */
        r->probe_pending = false;
        r->draws_at_rebuild = app->stat_draws;
    }
    return gp_div(gp_on_layout(probe_layout),gp_size_full(), gp_flex_col(), gp_p(20), gp_gap(10), gp_bg(gp_rgb(0x1e1e2e)),
                  gp_child_view(r->a), gp_child_view(r->m), gp_child_view(r->c), gp_child_view(r->p),
                  gp_child_view(r->r), gp_child_view(r->w), gp_child_view(r->w2), gp_child_view(r->d));
}

#define LONG_TEXT "The quick brown fox jumps over the lazy dog and keeps running until the line is " \
                  "certainly much longer than the field is wide."

/* ---- helpers */

static void settle(gp_co *co) { gp_sleep(co, 0.05); }
static void keys(gp_co *co, const char *k) { gp_simulate_keystrokes(app, k); settle(co); }
static void text(gp_co *co, const char *t) { gp_simulate_text(app, t); settle(co); }
static const char *val(gp_entity e) { return gp_text_input_text(app, e); }
static void focus(gp_co *co, gp_entity e) {
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_focus_set(&cx, gp_text_input_focus(app, e));
    settle(co);
}
static void set(gp_co *co, gp_entity e, const char *s) {
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_text_input_set_text(&cx, e, s);
    settle(co);
}

#define EXPECT(e, expect)                                                              \
    CHECK(strcmp(val(e), (expect)) == 0, "text is \"%s\", expected \"%s\"", val(e), (expect))
#define EXPECT_SEL(e, lo, hi)                                                          \
    do {                                                                               \
        size_t s_, t_;                                                                 \
        gp_text_input_selection(app, (e), &s_, &t_);                                   \
        CHECK(s_ == (size_t)(lo) && t_ == (size_t)(hi), "selection %zu..%zu, expected %d..%d", s_, t_, (int)(lo), (int)(hi)); \
    } while (0)

/* window x of the caret stop at byte `off` of a single-line field's text, for mouse tests */
static float stop_x(gp_entity field_x_origin_dummy, const char *s, size_t off) {
    (void)field_x_origin_dummy;
    float w = 0;
    gp_text_shape(app->text, 400, 15.f, s, off, NULL, 0, &w);
    return 20 + 1 + 10 + w; /* window pad + border + field padding + text width */
}

/* ------------------------------------------------------------ scenarios */

static void test_keyboard(gp_co *co) {
    gp_entity a = root->a;
    focus(co, a);
    keys(co, "h e l l o space");
    text(co, "world");
    EXPECT(a, "hello world");
    EXPECT_SEL(a, 11, 11);
    CHECK(root->changes >= 2, "on_change fired %d times", root->changes);

    keys(co, "left left left");
    keys(co, "shift-right shift-right");
    EXPECT_SEL(a, 8, 10);
    keys(co, "backspace");
    EXPECT(a, "hello wod");
    EXPECT_SEL(a, 8, 8);
    keys(co, "ctrl-z");
    EXPECT(a, "hello world");
    EXPECT_SEL(a, 8, 10);               /* undo restores the selection too */
    keys(co, "ctrl-shift-z");
    EXPECT(a, "hello wod");

    keys(co, "ctrl-left");  EXPECT_SEL(a, 6, 6);   /* start of "wod" */
    keys(co, "ctrl-left");  EXPECT_SEL(a, 0, 0);
    keys(co, "ctrl-right"); EXPECT_SEL(a, 5, 5);   /* end of "hello" */
    keys(co, "ctrl-right"); EXPECT_SEL(a, 9, 9);
    keys(co, "home");       EXPECT_SEL(a, 0, 0);
    keys(co, "end");        EXPECT_SEL(a, 9, 9);
    keys(co, "shift-home"); EXPECT_SEL(a, 0, 9);
    keys(co, "right");      EXPECT_SEL(a, 9, 9);   /* collapses to the end of the selection */
    keys(co, "ctrl-shift-left"); EXPECT_SEL(a, 6, 9);

    keys(co, "ctrl-a shift-x");
    EXPECT(a, "X");
    keys(co, "ctrl-z");
    EXPECT(a, "hello wod");
    EXPECT_SEL(a, 0, 9);

    keys(co, "end ctrl-backspace");  EXPECT(a, "hello ");
    keys(co, "ctrl-backspace");      EXPECT(a, "");
    set(co, a, "one two three");
    keys(co, "home ctrl-delete");    EXPECT(a, " two three");
    keys(co, "end ctrl-u");          EXPECT(a, "");
    set(co, a, "cut here");
    keys(co, "home right right right right ctrl-k"); EXPECT(a, "cut ");
}

static void test_undo(gp_co *co) {
    gp_entity a = root->a;
    set(co, a, "");
    keys(co, "a b c");
    EXPECT(a, "abc");
    keys(co, "ctrl-z");
    EXPECT(a, "");                       /* typing coalesces into one undo step */
    keys(co, "ctrl-shift-z");
    EXPECT(a, "abc");
    keys(co, "left e");                  /* a caret move breaks the run */
    EXPECT(a, "abec");
    keys(co, "ctrl-z");
    EXPECT(a, "abc");
    keys(co, "ctrl-z");
    EXPECT(a, "");

    set(co, a, "hello");
    keys(co, "end backspace backspace backspace");
    EXPECT(a, "he");
    keys(co, "ctrl-z");
    EXPECT(a, "hello");                  /* backspaces coalesce too */
    keys(co, "ctrl-y");
    EXPECT(a, "he");
    keys(co, "x");
    keys(co, "ctrl-y");                  /* a new edit clears the redo stack */
    EXPECT(a, "hex");
}

static void test_unicode(gp_co *co) {
    gp_entity a = root->a;
    focus(co, a);
    set(co, a, "ae\xcc\x81" "b");                 /* a, e + combining acute, b */
    keys(co, "backspace");   EXPECT(a, "ae\xcc\x81");
    keys(co, "backspace");   EXPECT(a, "a");      /* the whole e+accent cluster in one go */

    set(co, a, "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e");   /* 日本語: 3 bytes each */
    keys(co, "left");        EXPECT_SEL(a, 6, 6);
    keys(co, "left");        EXPECT_SEL(a, 3, 3);
    keys(co, "backspace");   EXPECT(a, "\xe6\x9c\xac\xe8\xaa\x9e");
    keys(co, "end");
    text(co, "\xf0\x9f\x99\x82");                 /* a 4-byte emoji */
    keys(co, "left");        EXPECT_SEL(a, 6, 6);
    keys(co, "delete");      EXPECT(a, "\xe6\x9c\xac\xe8\xaa\x9e");

    /* family emoji: man ZWJ woman ZWJ girl is one caret stop, 18 bytes */
    const char *family = "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7";
    set(co, a, family);
    keys(co, "left");        EXPECT_SEL(a, 0, 0);
    keys(co, "right");       EXPECT_SEL(a, 18, 18);
    keys(co, "backspace");   EXPECT(a, "");
}

/* Fallback fonts + HarfBuzz: emoji sequences and CJK must behave as single units in the widget. */
static void test_fallback_text(gp_co *co) {
    gp_entity a = root->a;
    focus(co, a);

    /* a flag is two regional indicators (8 bytes) but one caret stop */
    const char *flag = "\xf0\x9f\x87\xaf\xf0\x9f\x87\xb5";
    set(co, a, "x\xf0\x9f\x87\xaf\xf0\x9f\x87\xb5y");        /* x 🇯🇵 y: bytes 0 | 1..9 | 9 */
    keys(co, "home right");  EXPECT_SEL(a, 1, 1);
    keys(co, "right");       EXPECT_SEL(a, 9, 9);   /* over the whole flag */
    keys(co, "left");        EXPECT_SEL(a, 1, 1);
    keys(co, "shift-right"); EXPECT_SEL(a, 1, 9);   /* selecting it selects both halves */
    keys(co, "delete");      EXPECT(a, "xy");
    set(co, a, "\xf0\x9f\x87\xaf\xf0\x9f\x87\xb5\xf0\x9f\x87\xba\xf0\x9f\x87\xb8"); /* 🇯🇵🇺🇸 */
    keys(co, "end left");    EXPECT_SEL(a, 8, 8);   /* flags pair up from the left */
    keys(co, "left");        EXPECT_SEL(a, 0, 0);
    keys(co, "end backspace"); EXPECT(a, flag);
    (void)flag;

    /* a skin-toned hand, a keycap and a family each delete in one go */
    set(co, a, "\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd");
    keys(co, "end backspace"); EXPECT(a, "");
    set(co, a, "1\xef\xb8\x8f\xe2\x83\xa3");
    keys(co, "end backspace"); EXPECT(a, "");

    /* CJK: three bytes per character, one caret stop each, and the clipboard keeps them intact */
    const char *cjk = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";
    set(co, a, cjk);
    keys(co, "home right");  EXPECT_SEL(a, 3, 3);
    keys(co, "shift-right"); EXPECT_SEL(a, 3, 6);
    keys(co, "ctrl-a ctrl-c");
    char *clip = gp_clipboard_get(app);
    CHECK(clip && strcmp(clip, cjk) == 0, "clipboard lost the CJK text (\"%s\")", clip ? clip : "(null)");
    free(clip);
    keys(co, "home ctrl-right");
    EXPECT_SEL(a, 9, 9);                             /* a run of ideographs is one "word" */

    /* mixed text with every kind of fallback */
    const char *mixed = "a\xe6\x97\xa5\xf0\x9f\x98\x80\xc3\xa9";
    set(co, a, mixed);
    keys(co, "home right right right");
    EXPECT_SEL(a, 8, 8);                             /* a | 日 | 😀 | é : stops at 0 1 4 8 10 */
    keys(co, "right");       EXPECT_SEL(a, 10, 10);
    keys(co, "end backspace backspace backspace backspace"); EXPECT(a, "");
}

static void test_limits_and_modes(gp_co *co) {
    gp_entity c = root->c;                       /* max_bytes = 5 */
    focus(co, c);
    text(co, "abcdefgh");
    EXPECT(c, "abcde");
    set(co, c, "abcd");
    keys(co, "end");
    text(co, "\xc3\xa9");                        /* 2 bytes would exceed the limit: refused whole */
    EXPECT(c, "abcd");

    gp_entity r = root->r;                       /* read-only */
    focus(co, r);
    const char *before = strdup(val(r));
    keys(co, "x backspace delete ctrl-v");
    text(co, "zzz");
    CHECK(strcmp(val(r), before) == 0, "read-only field was edited: \"%s\"", val(r));
    keys(co, "ctrl-a");
    EXPECT_SEL(r, 0, strlen(before));            /* but selection works */

    gp_entity a = root->a;                       /* single-line flattens newlines */
    focus(co, a);
    set(co, a, "");
    gp_clipboard_set(app, "line one\nline two\r\nline three");
    keys(co, "ctrl-v");
    EXPECT(a, "line one line two line three");
}

static void test_clipboard(gp_co *co) {
    gp_entity a = root->a;
    focus(co, a);
    set(co, a, "copy me");
    keys(co, "ctrl-a ctrl-c");
    char *clip = gp_clipboard_get(app);
    CHECK(clip && strcmp(clip, "copy me") == 0, "clipboard holds \"%s\"", clip ? clip : "(null)");
    free(clip);
    keys(co, "end ctrl-v");
    EXPECT(a, "copy mecopy me");
    keys(co, "home shift-right shift-right shift-right shift-right ctrl-x");
    EXPECT(a, " mecopy me");                      /* "copy" is gone, the space stays */
    clip = gp_clipboard_get(app);
    CHECK(clip && strcmp(clip, "copy") == 0, "cut put \"%s\" on the clipboard", clip ? clip : "(null)");
    free(clip);
    keys(co, "ctrl-v");
    EXPECT(a, "copy mecopy me");
    keys(co, "ctrl-z");
    EXPECT(a, " mecopy me");                      /* paste was its own undo step */

    gp_clipboard_set(app, "keep");               /* a password field never exports its text */
    gp_entity p = root->p;
    focus(co, p);
    text(co, "hunter2");
    keys(co, "ctrl-a ctrl-c ctrl-x");
    EXPECT(p, "hunter2");
    clip = gp_clipboard_get(app);
    CHECK(clip && strcmp(clip, "keep") == 0, "password copy leaked \"%s\"", clip ? clip : "(null)");
    free(clip);
}

static void test_submit_and_multiline(gp_co *co) {
    gp_entity a = root->a;
    focus(co, a);
    set(co, a, "done");
    int before = root->submits;
    keys(co, "enter");
    CHECK(root->submits == before + 1 && strcmp(root->submitted, "done") == 0, "submit: %d \"%s\"", root->submits, root->submitted);

    gp_entity m = root->m;
    focus(co, m);
    set(co, m, "");
    text(co, "abc");
    keys(co, "enter");
    text(co, "de");
    keys(co, "enter");
    text(co, "fghij");
    EXPECT(m, "abc\nde\nfghij");
    CHECK(root->submits == before + 1, "Enter in a multi-line field must not submit");
    keys(co, "ctrl-enter");
    CHECK(root->submits == before + 2, "ctrl-enter submits a multi-line field");

    keys(co, "up");        EXPECT_SEL(m, 6, 6);    /* "de" is shorter: clamp to its end */
    keys(co, "up");        EXPECT_SEL(m, 3, 3);
    keys(co, "down");      EXPECT_SEL(m, 6, 6);
    keys(co, "down");      EXPECT_SEL(m, 12, 12);  /* the remembered column returns on the long line */
    keys(co, "home");      EXPECT_SEL(m, 7, 7);
    keys(co, "end");       EXPECT_SEL(m, 12, 12);
    keys(co, "ctrl-home"); EXPECT_SEL(m, 0, 0);
    keys(co, "ctrl-end");  EXPECT_SEL(m, 12, 12);
    keys(co, "up up up");  EXPECT_SEL(m, 0, 0);    /* up on the first line goes to the start */
    keys(co, "shift-down shift-down"); EXPECT_SEL(m, 0, 7);

    set(co, m, "abcdef\nx\nabcdef");
    keys(co, "ctrl-end left left");                 /* column 4 of the last line */
    keys(co, "up");   EXPECT_SEL(m, 8, 8);          /* the short line */
    keys(co, "up");   EXPECT_SEL(m, 4, 4);          /* back to column 4 */
}

static void test_mouse(gp_co *co) {
    gp_entity a = root->a;
    const char *s = "hello world";
    set(co, a, s);
    settle(co);
    float y = 20 + 1 + 7 + 9;                       /* middle of the text line */

    gp_simulate_mouse(app, GP_SIM_MOVE, stop_x(a, s, 8) + 1, y, 1, 0);
    gp_simulate_mouse(app, GP_SIM_DOWN, stop_x(a, s, 8) + 1, y, 1, 0);
    gp_simulate_mouse(app, GP_SIM_UP, stop_x(a, s, 8) + 1, y, 1, 0);
    settle(co);
    EXPECT_SEL(a, 8, 8);

    gp_simulate_mouse(app, GP_SIM_DOWN, 22, y, 1, 0);          /* in the padding: caret to the start */
    gp_simulate_mouse(app, GP_SIM_UP, 22, y, 1, 0);
    settle(co);
    EXPECT_SEL(a, 0, 0);
    gp_simulate_mouse(app, GP_SIM_DOWN, 415, y, 1, 0);         /* past the end */
    gp_simulate_mouse(app, GP_SIM_UP, 415, y, 1, 0);
    settle(co);
    EXPECT_SEL(a, 11, 11);

    float x7 = stop_x(a, s, 7) + 1;                            /* inside "world" */
    gp_simulate_mouse(app, GP_SIM_DOWN, x7, y, 1, 0);
    gp_simulate_mouse(app, GP_SIM_UP, x7, y, 1, 0);
    gp_simulate_mouse(app, GP_SIM_DOWN, x7, y, 2, 0);          /* double-click: the word */
    gp_simulate_mouse(app, GP_SIM_UP, x7, y, 2, 0);
    settle(co);
    EXPECT_SEL(a, 6, 11);
    gp_simulate_mouse(app, GP_SIM_DOWN, x7, y, 3, 0);          /* triple-click: everything */
    gp_simulate_mouse(app, GP_SIM_UP, x7, y, 3, 0);
    settle(co);
    EXPECT_SEL(a, 0, 11);

    /* drag selects; the mouse stays captured outside the field */
    gp_simulate_mouse(app, GP_SIM_DOWN, stop_x(a, s, 2) + 1, y, 1, 0);
    gp_simulate_mouse(app, GP_SIM_MOVE, stop_x(a, s, 5) + 1, y, 1, 0);
    gp_simulate_mouse(app, GP_SIM_MOVE, stop_x(a, s, 9) + 1, y, 1, 0);
    gp_simulate_mouse(app, GP_SIM_UP, stop_x(a, s, 9) + 1, y, 1, 0);
    settle(co);
    EXPECT_SEL(a, 2, 9);
    gp_simulate_mouse(app, GP_SIM_DOWN, stop_x(a, s, 2) + 1, y, 1, 0);
    gp_simulate_mouse(app, GP_SIM_MOVE, 430, 400, 1, 0);       /* far outside the widget */
    gp_simulate_mouse(app, GP_SIM_UP, 430, 400, 1, 0);
    settle(co);
    EXPECT_SEL(a, 2, 11);

    /* shift-click extends from the existing anchor */
    gp_simulate_mouse(app, GP_SIM_DOWN, stop_x(a, s, 4) + 1, y, 1, 0);
    gp_simulate_mouse(app, GP_SIM_UP, stop_x(a, s, 4) + 1, y, 1, 0);
    gp_simulate_mouse(app, GP_SIM_DOWN, stop_x(a, s, 9) + 1, y, 1, GP_MOD_SHIFT);
    gp_simulate_mouse(app, GP_SIM_UP, stop_x(a, s, 9) + 1, y, 1, GP_MOD_SHIFT);
    settle(co);
    EXPECT_SEL(a, 4, 9);

    /* multi-line: click on the second line; triple-click selects that whole line */
    gp_entity m = root->m;
    set(co, m, "first\nsecond line\nthird");
    float my = 20 + 35.5f + 10 + 1 + 7 + 19.5f + 9;           /* line index 1 */
    gp_simulate_mouse(app, GP_SIM_DOWN, 415, my, 1, 0);
    gp_simulate_mouse(app, GP_SIM_UP, 415, my, 1, 0);
    settle(co);
    EXPECT_SEL(m, 17, 17);                                    /* end of "second line" */
    gp_simulate_mouse(app, GP_SIM_DOWN, 60, my, 3, 0);
    gp_simulate_mouse(app, GP_SIM_UP, 60, my, 3, 0);
    settle(co);
    EXPECT_SEL(m, 6, 18);                                     /* the line including its break */
}

/* Where a click lands on text drawn with fallback fonts and sequences. */
/* y of the middle of the i-th field (a, m, c, p, r, w, w2, d: the root's children, in order) */
static float field_y(int i) {
    const gp_el *f = app->windows[0]->tree->children[i];
    return f->pos.y + f->size.h / 2;
}

static void test_primary_selection(gp_co *co) {
    gp_entity a = root->a;
    const char *s = "hello world";
    set(co, a, s);
    focus(co, a);
    settle(co);
    float y = 20 + 1 + 7 + 9;
    char *got;

    gp_primary_set(app, "seed");
    gp_clipboard_set(app, "clip");
    gp_simulate_mouse(app, GP_SIM_DOWN, stop_x(a, s, 3) + 1, y, 1, 0);   /* a click that selects nothing */
    gp_simulate_mouse(app, GP_SIM_UP, stop_x(a, s, 3) + 1, y, 1, 0);
    settle(co);
    EXPECT_SEL(a, 3, 3);
    got = gp_primary_get(app);
    CHECK(got && strcmp(got, "seed") == 0, "a click with no selection leaves the primary selection alone: \"%s\"", got ? got : "(null)");
    free(got);

    float x7 = stop_x(a, s, 7) + 1;
    gp_simulate_mouse(app, GP_SIM_DOWN, x7, y, 2, 0);                      /* double-click: the word */
    gp_simulate_mouse(app, GP_SIM_UP, x7, y, 2, 0);
    settle(co);
    EXPECT_SEL(a, 6, 11);
    got = gp_primary_get(app);
    CHECK(got && strcmp(got, "world") == 0, "a double-click selects a word and makes it the primary selection: \"%s\"", got ? got : "(null)");
    free(got);
    got = gp_clipboard_get(app);
    CHECK(got && strcmp(got, "clip") == 0, "the clipboard is a separate thing: \"%s\"", got ? got : "(null)");
    free(got);

    gp_simulate_mouse(app, GP_SIM_DOWN, stop_x(a, s, 2) + 1, y, 1, 0);     /* a drag */
    gp_simulate_mouse(app, GP_SIM_MOVE, stop_x(a, s, 5) + 1, y, 1, 0);
    gp_simulate_mouse(app, GP_SIM_UP, stop_x(a, s, 5) + 1, y, 1, 0);
    settle(co);
    EXPECT_SEL(a, 2, 5);
    got = gp_primary_get(app);
    CHECK(got && strcmp(got, "llo") == 0, "dragging selects, and the selection is the primary one: \"%s\"", got ? got : "(null)");
    free(got);

    gp_simulate_mouse(app, GP_SIM_DOWN, x7, y, 3, 0);                      /* a triple-click: all of it */
    gp_simulate_mouse(app, GP_SIM_UP, x7, y, 3, 0);
    settle(co);
    got = gp_primary_get(app);
    CHECK(got && strcmp(got, "hello world") == 0, "a triple-click selects the line: \"%s\"", got ? got : "(null)");
    free(got);

    /* a middle click pastes it at the click, and moves nothing else */
    gp_simulate_mouse_button(app, GP_SIM_DOWN, 22, y, GP_MOUSE_MIDDLE, 1, 0);
    gp_simulate_mouse_button(app, GP_SIM_UP, 22, y, GP_MOUSE_MIDDLE, 1, 0);
    settle(co);
    EXPECT(a, "hello worldhello world");
    EXPECT_SEL(a, 11, 11);
    got = gp_primary_get(app);
    CHECK(got && strcmp(got, "hello world") == 0, "pasting does not change the primary selection: \"%s\"", got ? got : "(null)");
    free(got);
    set(co, a, "hello world");
    gp_primary_set(app, "XY");
    gp_simulate_mouse_button(app, GP_SIM_DOWN, stop_x(a, s, 5) + 1, y, GP_MOUSE_MIDDLE, 1, 0);   /* in the middle of the text */
    gp_simulate_mouse_button(app, GP_SIM_UP, stop_x(a, s, 5) + 1, y, GP_MOUSE_MIDDLE, 1, 0);
    settle(co);
    EXPECT(a, "helloXY world");
    EXPECT_SEL(a, 7, 7);
    keys(co, "ctrl-z");
    EXPECT(a, "hello world");   /* it is one undoable edit */

    /* a password field never exports; a read-only one does not take a paste */
    gp_entity p = root->p;
    set(co, p, "secret");
    gp_primary_set(app, "kept");
    gp_simulate_mouse(app, GP_SIM_DOWN, 40, field_y(3), 2, 0);
    gp_simulate_mouse(app, GP_SIM_UP, 40, field_y(3), 2, 0);
    settle(co);
    got = gp_primary_get(app);
    CHECK(got && strcmp(got, "kept") == 0, "selecting in a password field does not export it: \"%s\"", got ? got : "(null)");
    free(got);
    gp_entity r = root->r;
    char *before = strdup(val(r));
    size_t sel0, sel1;
    gp_text_input_selection(app, r, &sel0, &sel1);
    gp_simulate_mouse_button(app, GP_SIM_DOWN, 60, field_y(4), GP_MOUSE_MIDDLE, 1, 0);
    gp_simulate_mouse_button(app, GP_SIM_UP, 60, field_y(4), GP_MOUSE_MIDDLE, 1, 0);
    settle(co);
    CHECK(strcmp(val(r), before) == 0, "a read-only field takes no paste: \"%s\"", val(r));
    EXPECT_SEL(r, sel0, sel1);   /* ...and the click does not even move its caret */
    free(before);
    /* nothing selected anywhere, nothing to paste: a middle click changes nothing */
    set(co, a, "same");
    gp_primary_set(app, "");
    gp_simulate_mouse_button(app, GP_SIM_DOWN, 40, y, GP_MOUSE_MIDDLE, 1, 0);
    gp_simulate_mouse_button(app, GP_SIM_UP, 40, y, GP_MOUSE_MIDDLE, 1, 0);
    settle(co);
    EXPECT(a, "same");
}

static void test_mouse_on_fallback_text(gp_co *co) {
    gp_entity a = root->a;
    float y = 20 + 1 + 7 + 9;

    /* "a" + family(18 bytes) + "b": caret stops exist only at 0, 1, 19, 20 */
    const char *s = "a\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7" "b";
    set(co, a, s);
    float x_before = stop_x(a, s, 1), x_after = stop_x(a, s, 19);
    CHECK(x_after - x_before > 15, "the family emoji is only %.1f px wide", x_after - x_before);
    for (int pct = 5; pct <= 95; pct += 15) {            /* click across the whole emoji */
        float x = x_before + (x_after - x_before) * (float)pct / 100.f;
        gp_simulate_mouse(app, GP_SIM_DOWN, x, y, 1, 0);
        gp_simulate_mouse(app, GP_SIM_UP, x, y, 1, 0);
        settle(co);
        size_t lo, hi;
        gp_text_input_selection(app, a, &lo, &hi);
        CHECK(lo == hi && (lo == 1 || lo == 19), "click at %d%% of the emoji put the caret at byte %zu", pct, lo);
        CHECK((pct < 45) == (lo == 1) || (pct > 55) == (lo == 19), "click at %d%% chose the wrong side (%zu)", pct, lo);
    }

    /* CJK: every character boundary is reachable, and a click near one lands exactly there */
    const char *k = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae";
    set(co, a, k);
    for (size_t off = 0; off <= 12; off += 3) {
        float x = stop_x(a, k, off) + 1;
        gp_simulate_mouse(app, GP_SIM_DOWN, x, y, 1, 0);
        gp_simulate_mouse(app, GP_SIM_UP, x, y, 1, 0);
        settle(co);
        EXPECT_SEL(a, off, off);
    }
    /* double-click selects the run of ideographs, drag selects by character */
    float x1 = stop_x(a, k, 3) + 1;
    gp_simulate_mouse(app, GP_SIM_DOWN, x1, y, 2, 0);
    gp_simulate_mouse(app, GP_SIM_UP, x1, y, 2, 0);
    settle(co);
    EXPECT_SEL(a, 0, 12);
}

static void test_ime(gp_co *co) {
    gp_entity a = root->a;
    focus(co, a);
    set(co, a, "");
    /* IME events are delivered synchronously, so check right away (no loop turn in between) */
    gp_simulate_ime(app, "\xe3\x81\xab\xe3\x81\xbb", 1);       /* にほ */
    CHECK(strcmp(gp_text_input_preedit(app, a), "\xe3\x81\xab\xe3\x81\xbb") == 0, "preedit not stored");
    EXPECT(a, "");                                            /* composing text is not in the buffer */
    gp_simulate_ime(app, "\xe3\x81\xab\xe3\x81\xbb\xe3\x82\x93", 3);
    CHECK(strcmp(gp_text_input_preedit(app, a), "\xe3\x81\xab\xe3\x81\xbb\xe3\x82\x93") == 0, "preedit not updated");
    EXPECT(a, "");
    gp_simulate_ime(app, "", 0);                              /* composition ends ... */
    CHECK(gp_text_input_preedit(app, a)[0] == 0, "empty preedit must clear the composition");
    gp_simulate_ime(app, "\xe3\x81\xab", 1);                  /* the commit may also arrive mid-composition */
    gp_simulate_text(app, "\xe6\x97\xa5\xe6\x9c\xac");        /* ... and the commit arrives as text */
    settle(co);
    EXPECT(a, "\xe6\x97\xa5\xe6\x9c\xac");
    CHECK(gp_text_input_preedit(app, a)[0] == 0, "preedit survived the commit");

    /* committing while a preedit is still showing also clears it */
    gp_simulate_ime(app, "x", 1);
    gp_simulate_text(app, "y");
    settle(co);
    EXPECT(a, "\xe6\x97\xa5\xe6\x9c\xac" "y");
    CHECK(gp_text_input_preedit(app, a)[0] == 0, "preedit survived a commit");

    /* starting to compose replaces the selection */
    keys(co, "ctrl-a");
    gp_simulate_ime(app, "\xe3\x81\x82", 1);
    EXPECT(a, "");                                            /* the selection was replaced by the composition */
    gp_simulate_ime(app, "", 0);
    settle(co);
}

static void test_scroll_follows_caret(gp_co *co) {
    /* single-line: text far wider than the field must scroll sideways to keep the caret visible */
    gp_entity a = root->a;
    focus(co, a);
    set(co, a, "The quick brown fox jumps over the lazy dog, then keeps running far beyond the right "
               "edge of this field until it simply has to scroll sideways to be seen at all.");
    keys(co, "ctrl-end");
    gp_point at_end = gp_text_input_scroll(app, a);
    CHECK(at_end.x > 100, "ctrl-end should scroll a long line sideways (offset %.1f)", at_end.x);
    keys(co, "home");
    CHECK(gp_text_input_scroll(app, a).x == 0, "home should scroll back to the start (%.1f)",
          gp_text_input_scroll(app, a).x);
    keys(co, "ctrl-right ctrl-right ctrl-right ctrl-right ctrl-right ctrl-right ctrl-right ctrl-right "
             "ctrl-right ctrl-right ctrl-right ctrl-right ctrl-right ctrl-right ctrl-right ctrl-right");
    CHECK(gp_text_input_scroll(app, a).x > 0, "moving right by words should follow the caret");
    text(co, "!");                                 /* typing in the middle keeps the caret in view too */
    CHECK(gp_text_input_scroll(app, a).x > 0, "typing must not lose the caret");

    /* multi-line: more lines than rows scrolls vertically */
    gp_entity m = root->m;
    focus(co, m);
    set(co, m, "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12");
    keys(co, "ctrl-end");
    CHECK(gp_text_input_scroll(app, m).y > 20, "ctrl-end should scroll to the last line (%.1f)", gp_text_input_scroll(app, m).y);
    keys(co, "ctrl-home");
    CHECK(gp_text_input_scroll(app, m).y == 0, "ctrl-home should scroll to the top");
    keys(co, "down down down down down down");     /* below the 4 visible rows */
    CHECK(gp_text_input_scroll(app, m).y > 0, "moving down past the last visible row should scroll");
    keys(co, "pagedown");
    float y1 = gp_text_input_scroll(app, m).y;
    keys(co, "pageup");
    CHECK(gp_text_input_scroll(app, m).y < y1, "pageup should scroll back up");
}

/* ------------------------------------------------------------ soft wrap */

/* Width of text[start,end) without trailing spaces (which hang past the edge by design). */
static float row_width(gp_entity e, size_t start, size_t end, size_t *trimmed_end) {
    const char *t = val(e);
    while (end > start && (t[end - 1] == ' ' || t[end - 1] == '\t')) end--;
    float w = 0;
    gp_text_shape(app->text, 400, 15.f, t + start, end - start, NULL, 0, &w);
    if (trimmed_end) *trimmed_end = end;
    return w;
}

/* Field `w` is 400 wide: 400 - 2 (border) - 20 (padding) - 4 (room for the caret) = 374 px of text.
 * Field `w2` is 200 wide: 174 px. */
#define WRAP_W 374.f
#define WRAP_W2 174.f

/* The rows must tile the text exactly, and none may be wider than the field. */
static void check_rows(gp_entity e, float limit, const char *what) {
    size_t n = gp_text_input_rows(app, e), len = strlen(val(e)), prev_end = 0;
    for (size_t i = 0; i < n; i++) {
        size_t s, t;
        CHECK(gp_text_input_row_range(app, e, i, &s, &t), "%s: row %zu missing", what, i);
        CHECK(s >= prev_end && (i == 0 ? s == 0 : s == prev_end || val(e)[prev_end] == '\n'),
              "%s: row %zu starts at %zu but the previous ended at %zu", what, i, s, prev_end);
        float w = row_width(e, s, t, NULL);
        CHECK(w <= limit + 0.6f, "%s: row %zu is %.1f px wide, limit %.1f", what, i, w, limit);
        prev_end = t;
    }
    CHECK(prev_end == len, "%s: rows end at %zu of %zu bytes", what, prev_end, len);
}

static void test_soft_wrap(gp_co *co) {
    gp_entity w = root->w;
    focus(co, w);

    /* words wrap at spaces, spaces hang, and every row fits */
    set(co, w, LONG_TEXT);
    settle(co);
    size_t rows = gp_text_input_rows(app, w);
    CHECK(rows >= 3, "a ~130 character sentence in a 374 px field made %zu row(s)", rows);
    check_rows(w, WRAP_W, "sentence");
    for (size_t i = 0; i + 1 < rows; i++) {
        size_t s, e;
        gp_text_input_row_range(app, w, i, &s, &e);
        CHECK(val(w)[e - 1] == ' ', "row %zu ends mid-word (\"%.*s\")", i, (int)(e - s), val(w) + s);
        CHECK(val(w)[e] != ' ', "row %zu+1 starts with a space", i);
    }

    /* A word that ends within a space's width of the limit keeps that space on its row: spaces
     * hang past the edge, they never push the word they follow onto the next row. */
    /* Build a word whose width is in (limit - space, limit]: pad wide letters, then search a short
     * suffix of narrow ones that lands in the window (a single step would be too coarse). */
    float space_w = 0;
    gp_text_shape(app->text, 400, 15.f, " ", 1, NULL, 0, &space_w);
    char fit[400];
    size_t k = 0;
    float fw = 0;
    for (; k < 100; k++) {
        fit[k] = 'x';
        gp_text_shape(app->text, 400, 15.f, fit, k + 1, NULL, 0, &fw);
        if (fw > WRAP_W - 14) break;
    }
    fit[k] = 0;
    size_t base = k;
    static const char alphabet[] = "il.,'j!tfr:;|I1";
    bool found = false;
    for (size_t drop = 0; drop < 2 && !found; drop++) {
        size_t n0 = base - drop;
        for (size_t i = 0; alphabet[i] && !found; i++)
            for (size_t j = 0; alphabet[j] && !found; j++)
                for (size_t l = 0; alphabet[l] && !found; l++) {
                    memcpy(fit + n0, (char[]){ alphabet[i], alphabet[j], alphabet[l], 0 }, 4);
                    gp_text_shape(app->text, 400, 15.f, fit, n0 + 3, NULL, 0, &fw);
                    found = fw <= WRAP_W && fw > WRAP_W - space_w + 0.3f;
                }
    }
    CHECK(found, "test setup: could not build a word within one space (%.2f px) of the %.0f px limit", space_w, WRAP_W);
    char with_tail[512];
    snprintf(with_tail, sizeof with_tail, "%s tail", fit);
    set(co, w, with_tail);
    settle(co);
    size_t rs0, re0, rs1;
    CHECK(gp_text_input_rows(app, w) == 2, "word + space + word made %zu rows", gp_text_input_rows(app, w));
    gp_text_input_row_range(app, w, 0, &rs0, &re0);
    gp_text_input_row_range(app, w, 1, &rs1, NULL);
    CHECK(re0 == strlen(fit) + 1 && val(w)[re0 - 1] == ' ', "the space did not stay with its word: row 0 is [%zu,%zu)", rs0, re0);
    CHECK(strncmp(val(w) + rs1, "tail", 4) == 0, "row 1 starts with \"%.4s\"", val(w) + rs1);

    /* the unwrapped multi-line field keeps the whole line on one row */
    set(co, root->m, LONG_TEXT);
    CHECK(gp_text_input_rows(app, root->m) == 1, "a field without soft_wrap wrapped");

    /* a word too long for any row is broken between characters */
    char xs[201];
    memset(xs, 'x', 200);
    xs[200] = 0;
    set(co, w, xs);
    settle(co);
    rows = gp_text_input_rows(app, w);
    CHECK(rows >= 3, "200 x's made %zu row(s)", rows);
    check_rows(w, WRAP_W, "long word");

    /* CJK has no spaces: it wraps between characters, and never starts a row with a full stop */
    set(co, w, "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe3\x83\x86\xe3\x82\xad\xe3\x82\xb9\xe3\x83\x88"
               "\xe3\x81\xaf\xe3\x82\xb9\xe3\x83\x9a\xe3\x83\xbc\xe3\x82\xb9\xe3\x81\x8c\xe3\x81\xaa\xe3\x81\x84\xe3\x80\x82"
               "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe3\x83\x86\xe3\x82\xad\xe3\x82\xb9\xe3\x83\x88"
               "\xe3\x81\xaf\xe3\x82\xb9\xe3\x83\x9a\xe3\x83\xbc\xe3\x82\xb9\xe3\x81\x8c\xe3\x81\xaa\xe3\x81\x84\xe3\x80\x82"
               "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe3\x83\x86\xe3\x82\xad\xe3\x82\xb9\xe3\x83\x88");
    settle(co);
    rows = gp_text_input_rows(app, w);
    CHECK(rows >= 2, "CJK text made %zu row(s)", rows);
    check_rows(w, WRAP_W, "CJK");
    for (size_t i = 1; i < rows; i++) {
        size_t s;
        gp_text_input_row_range(app, w, i, &s, NULL);
        CHECK(strncmp(val(w) + s, "\xe3\x80\x82", 3) != 0, "row %zu starts with a full stop", i);
    }

    /* hard line breaks and wrapping together: rows never span a '\n' */
    set(co, w, "short\n" LONG_TEXT "\n\nend");
    settle(co);
    rows = gp_text_input_rows(app, w);
    size_t s0, e0, sl, el;
    gp_text_input_row_range(app, w, 0, &s0, &e0);
    gp_text_input_row_range(app, w, rows - 1, &sl, &el);
    CHECK(s0 == 0 && e0 == 5, "first row is [%zu,%zu), expected the line \"short\"", s0, e0);
    CHECK(strcmp(val(w) + sl, "end") == 0 && el == strlen(val(w)), "last row is not \"end\"");
    CHECK(rows >= 6, "5 lines (one long, one empty) made %zu rows", rows);
    for (size_t i = 0; i < rows; i++) {
        size_t s, e;
        gp_text_input_row_range(app, w, i, &s, &e);
        CHECK(memchr(val(w) + s, '\n', e - s) == NULL, "row %zu contains a line break", i);
    }
    check_rows(w, WRAP_W, "mixed");

    /* the wrap width is the field's width as laid out, not a constant: 200 px wraps more.
     * (w2 also starts with long initial text, so this covers the very first layout.) */
    size_t narrow = gp_text_input_rows(app, root->w2);
    set(co, w, LONG_TEXT);
    settle(co);
    CHECK(narrow > gp_text_input_rows(app, w), "200 px field has %zu rows, 400 px field %zu", narrow, gp_text_input_rows(app, w));
    check_rows(root->w2, WRAP_W2, "narrow field, initial text");
}

static void test_soft_wrap_editing(gp_co *co) {
    gp_entity w = root->w;
    focus(co, w);
    set(co, w, LONG_TEXT);
    settle(co);
    size_t rows = gp_text_input_rows(app, w);
    size_t s[8], e[8];
    for (size_t i = 0; i < rows && i < 8; i++) gp_text_input_row_range(app, w, i, &s[i], &e[i]);
    CHECK(rows >= 3, "need 3 rows, have %zu", rows);

    /* Up / Down walk the wrapped rows of ONE line */
    keys(co, "ctrl-home");
    CHECK(gp_text_input_caret_row(app, w) == 0, "ctrl-home is on row %zu", gp_text_input_caret_row(app, w));
    keys(co, "down");
    size_t lo, hi;
    gp_text_input_selection(app, w, &lo, &hi);
    CHECK(gp_text_input_caret_row(app, w) == 1 && lo >= s[1] && lo <= e[1], "down: row %zu, offset %zu (row 1 is [%zu,%zu])",
          gp_text_input_caret_row(app, w), lo, s[1], e[1]);
    keys(co, "down");
    CHECK(gp_text_input_caret_row(app, w) == 2, "second down is on row %zu", gp_text_input_caret_row(app, w));
    keys(co, "up");
    CHECK(gp_text_input_caret_row(app, w) == 1, "up is on row %zu", gp_text_input_caret_row(app, w));

    /* Home / End are per visual row, and End stays on the row it was pressed on */
    keys(co, "end");
    gp_text_input_selection(app, w, &lo, &hi);
    CHECK(lo == e[1], "end went to %zu, the end of row 1 is %zu", lo, e[1]);
    CHECK(gp_text_input_caret_row(app, w) == 1, "after End the caret jumped to row %zu", gp_text_input_caret_row(app, w));
    keys(co, "home");
    gp_text_input_selection(app, w, &lo, &hi);
    CHECK(lo == s[1] && gp_text_input_caret_row(app, w) == 1, "home: %zu on row %zu, row 1 starts at %zu", lo,
          gp_text_input_caret_row(app, w), s[1]);
    keys(co, "end right");                          /* one character past the end of row 1 */
    gp_text_input_selection(app, w, &lo, &hi);
    CHECK(lo == e[1] + 1 && gp_text_input_caret_row(app, w) == 2, "right from End: offset %zu on row %zu",
          lo, gp_text_input_caret_row(app, w));
    keys(co, "home");
    gp_text_input_selection(app, w, &lo, &hi);
    CHECK(lo == s[2], "home on row 2 went to %zu, row 2 starts at %zu", lo, s[2]);
    keys(co, "left");                               /* back over the soft break */
    gp_text_input_selection(app, w, &lo, &hi);
    CHECK(lo == s[2] - 1 && gp_text_input_caret_row(app, w) == 1, "left from a row start: offset %zu on row %zu", lo,
          gp_text_input_caret_row(app, w));

    /* shift+down selects across rows; typing replaces the selection and re-wraps */
    keys(co, "ctrl-home shift-down shift-down");
    gp_text_input_selection(app, w, &lo, &hi);
    CHECK(lo == 0 && hi >= s[2] && hi <= e[2], "shift-down x2 selected %zu..%zu", lo, hi);
    keys(co, "x");
    CHECK(val(w)[0] == 'x' && strlen(val(w)) < strlen(LONG_TEXT) - 50, "typing over a multi-row selection: \"%s\"", val(w));
    check_rows(w, WRAP_W, "after replacing");
    keys(co, "ctrl-z");
    EXPECT(w, LONG_TEXT);
    check_rows(w, WRAP_W, "after undo");

    /* typing at the end grows the number of rows */
    set(co, w, "start");
    keys(co, "ctrl-end");
    size_t before = gp_text_input_rows(app, w);
    text(co, LONG_TEXT);
    settle(co);
    CHECK(gp_text_input_rows(app, w) > before, "typing a long sentence left %zu row(s)", gp_text_input_rows(app, w));
    check_rows(w, WRAP_W, "typed");

    /* scroll follows the caret down the wrapped rows */
    char big[2048] = "";
    for (int i = 0; i < 12; i++) strcat(big, LONG_TEXT " ");
    set(co, w, big);
    settle(co);
    keys(co, "ctrl-end");
    CHECK(gp_text_input_scroll(app, w).y > 50, "ctrl-end did not scroll the wrapped text (%.1f)", gp_text_input_scroll(app, w).y);
    CHECK(gp_text_input_caret_row(app, w) == gp_text_input_rows(app, w) - 1, "caret is on row %zu of %zu",
          gp_text_input_caret_row(app, w), gp_text_input_rows(app, w));
    keys(co, "ctrl-home");
    CHECK(gp_text_input_scroll(app, w).y == 0, "ctrl-home did not scroll back");
}

static void test_soft_wrap_mouse(gp_co *co) {
    gp_entity w = root->w;
    set(co, w, LONG_TEXT);
    settle(co);
    size_t s[6], e[6];
    for (size_t i = 0; i < 4; i++) gp_text_input_row_range(app, w, i, &s[i], &e[i]);
    float y0 = 306 + 1 + 7, lh = 19.5f; /* field w: top of its first text row */
    float row_y[3];
    for (int i = 0; i < 3; i++) row_y[i] = y0 + (float)i * lh + lh / 2;

    /* a click past the end of row 0 puts the caret at the END of row 0, not the start of row 1 */
    gp_simulate_mouse(app, GP_SIM_DOWN, 415, row_y[0], 1, 0);
    gp_simulate_mouse(app, GP_SIM_UP, 415, row_y[0], 1, 0);
    settle(co);
    size_t lo, hi;
    gp_text_input_selection(app, w, &lo, &hi);
    CHECK(lo == e[0] && gp_text_input_caret_row(app, w) == 0, "click past row 0: offset %zu on row %zu (row 0 ends at %zu)", lo,
          gp_text_input_caret_row(app, w), e[0]);

    /* a click at the left edge of row 1 puts it at the start of row 1 */
    gp_simulate_mouse(app, GP_SIM_DOWN, 25, row_y[1], 1, 0);
    gp_simulate_mouse(app, GP_SIM_UP, 25, row_y[1], 1, 0);
    settle(co);
    gp_text_input_selection(app, w, &lo, &hi);
    CHECK(lo == s[1] && gp_text_input_caret_row(app, w) == 1, "click at row 1's start: offset %zu on row %zu (row 1 starts at %zu)", lo,
          gp_text_input_caret_row(app, w), s[1]);

    /* a click inside row 1 lands inside row 1; a double-click selects the word under it */
    float xm = 20 + 1 + 10 + 60;
    gp_simulate_mouse(app, GP_SIM_DOWN, xm, row_y[1], 1, 0);
    gp_simulate_mouse(app, GP_SIM_UP, xm, row_y[1], 1, 0);
    settle(co);
    gp_text_input_selection(app, w, &lo, &hi);
    CHECK(lo > s[1] && lo < e[1] && gp_text_input_caret_row(app, w) == 1, "click inside row 1: offset %zu", lo);
    gp_simulate_mouse(app, GP_SIM_DOWN, xm, row_y[1], 2, 0);
    gp_simulate_mouse(app, GP_SIM_UP, xm, row_y[1], 2, 0);
    settle(co);
    gp_text_input_selection(app, w, &lo, &hi);
    CHECK(hi > lo && hi - lo < 14 && lo >= s[1] - 1 && hi <= e[1], "double-click selected %zu..%zu", lo, hi);
    CHECK(strchr(val(w) + lo, ' ') == NULL || (size_t)(strchr(val(w) + lo, ' ') - val(w)) >= hi - 0, "the selected word contains a space");

    /* a drag from row 0 to row 2 selects across the wrap */
    gp_simulate_mouse(app, GP_SIM_DOWN, 60, row_y[0], 1, 0);
    gp_simulate_mouse(app, GP_SIM_MOVE, 100, row_y[1], 1, 0);
    gp_simulate_mouse(app, GP_SIM_MOVE, 120, row_y[2], 1, 0);
    gp_simulate_mouse(app, GP_SIM_UP, 120, row_y[2], 1, 0);
    settle(co);
    gp_text_input_selection(app, w, &lo, &hi);
    CHECK(lo > s[0] && lo < e[0] && hi > s[2] && hi < e[2], "drag across rows selected %zu..%zu", lo, hi);

    /* a triple-click selects the whole hard line, however many rows it takes */
    gp_simulate_mouse(app, GP_SIM_DOWN, 60, row_y[1], 3, 0);
    gp_simulate_mouse(app, GP_SIM_UP, 60, row_y[1], 3, 0);
    settle(co);
    EXPECT_SEL(w, 0, strlen(LONG_TEXT));
}

/* How big a document can a wrapping field take? Measured, and bounded generously: the point is to
 * catch an accidental quadratic, and to put a number on "sized for small documents". */
static void test_soft_wrap_scale(gp_co *co) {
    gp_entity w = root->w;
    focus(co, w);
    static char doc[40000];
    size_t o = 0;
    for (int line = 0; line < 300; line++)
        o += (size_t)snprintf(doc + o, sizeof doc - o,
                              "Line %03d: the quick brown fox jumps over the lazy dog, again and again, until it is wrapped.\n", line);
    double t0 = gp_time();
    set(co, w, doc);
    for (int i = 0; i < 100 && app->stat_draws == 0; i++) gp_sleep(co, 0.01);
    double load_ms = (gp_time() - t0) * 1000 - 50; /* set() settles for 50 ms */
    size_t rows = gp_text_input_rows(app, w);
    CHECK(rows >= 600, "30 KB made only %zu rows", rows);
    check_rows(w, WRAP_W, "large document");

    keys(co, "ctrl-end");
    unsigned long d0 = app->stat_draws;
    double t1 = gp_time();
    gp_simulate_text(app, "z");
    while (app->stat_draws == d0 && gp_time() - t1 < 2.0) gp_sleep(co, 0.002);
    double edit_ms = (gp_time() - t1) * 1000;
    CHECK(edit_ms < 400, "typing one character into a %zu-row document took %.0f ms to reach a painted frame", rows, edit_ms);
    printf("  (30 KB, %zu rows: load ~%.0f ms, one keystroke to next frame %.0f ms)\n", rows, load_ms, edit_ms);
}

/* gp_on_layout: a handler that calls gp_notify gets a rebuild BEFORE anything is painted. */
static void test_layout_handler_rebuilds_before_paint(gp_co *co) {
    CHECK(root->probe_done, "the layout handler never ran");
    CHECK(root->draws_at_rebuild == root->draws_at_layout,
          "a frame was painted between the layout (draw %lu) and the rebuild it requested (draw %lu)",
          root->draws_at_layout, root->draws_at_rebuild);
}

/* ------------------------------------------------------------ bidirectional text */

/* Fields `a`, `w` are 400 wide: 374 px of text area (400 - 2 border - 20 padding - 4 for the caret). A
 * right-to-left row is right-aligned inside it, so its glyphs start at 374 - width. */
#define AVAIL 374.f
#define HEB_ABG  "אבג"
#define MIXED    "ab " HEB_ABG " cd"   /* a0 b1 _2 alef3 bet5 gimel7 _9 c10 d11, end 12 */

static float caret_x_of(gp_entity e) {
    float x = -1;
    gp_text_input_caret_pos(app, e, &x, NULL);
    return x;
}

/* The caret offset after each key; `asserts` is a human-readable name. Also checks that every press
 * moved the caret strictly right (dx > 0) or left (dx < 0) ON SCREEN, which is what visual movement means. */
static void walk(gp_co *co, gp_entity e, const char *key, int dx, const size_t *want, size_t n, const char *what) {
    float prev = caret_x_of(e);
    for (size_t i = 0; i < n; i++) {
        keys(co, key);
        size_t lo, hi;
        gp_text_input_selection(app, e, &lo, &hi);
        float x = caret_x_of(e);
        CHECK(lo == want[i] && hi == want[i], "%s: press %zu put the caret at %zu, expected %zu", what, i + 1, lo, want[i]);
        CHECK(dx > 0 ? x > prev + 0.01f : x < prev - 0.01f, "%s: press %zu moved the caret %s on screen (%.2f -> %.2f)", what, i + 1,
              dx > 0 ? "left or not at all" : "right or not at all", prev, x);
        prev = x;
    }
}

static void test_bidi_basics(gp_co *co) {
    gp_entity a = root->a;
    focus(co, a);
    set(co, a, "");

    /* typing Hebrew makes the paragraph right-to-left; each typed letter grows the text LEFTWARDS,
     * so the caret (at the trailing edge of the new letter) walks left across the screen */
    float prev = caret_x_of(a);
    CHECK(!gp_text_input_row_rtl(app, a, 0), "an empty field is not right-to-left");
    const char *letters[] = { "ש", "ל", "ו", "ם" };
    float xs[4];
    for (int i = 0; i < 4; i++) {
        text(co, letters[i]);
        xs[i] = caret_x_of(a);
        if (i > 0) CHECK(xs[i] < prev - 1.f, "typing Hebrew letter %d: caret %.1f -> %.1f, expected it to move left", i + 1, prev, xs[i]);
        prev = xs[i];
    }
    EXPECT(a, "שלום");
    CHECK(gp_text_input_row_rtl(app, a, 0), "Hebrew text is not a right-to-left paragraph");
    CHECK(xs[3] > 0 && xs[3] < AVAIL - 20, "caret at %.1f after typing 4 letters (area is %.0f wide, text is right-aligned)", xs[3], AVAIL);
    keys(co, "backspace");
    EXPECT(a, "שלו");                       /* one letter = 2 bytes */

    /* the direction follows the first strong character: replacing it flips the paragraph */
    set(co, a, "abc");
    CHECK(!gp_text_input_row_rtl(app, a, 0), "Latin is right-to-left");
    keys(co, "ctrl-a");
    text(co, "ש");
    CHECK(gp_text_input_row_rtl(app, a, 0), "the paragraph did not flip to right-to-left");
    keys(co, "ctrl-a");
    text(co, "x");
    CHECK(!gp_text_input_row_rtl(app, a, 0), "the paragraph did not flip back");

    /* in an RTL paragraph the start of the text is at the RIGHT edge, and Left walks into the text */
    set(co, a, HEB_ABG);
    keys(co, "home");
    float x0 = caret_x_of(a);
    CHECK(fabsf(x0 - AVAIL) < 1.5f, "the start of Hebrew text is at x=%.1f, expected the right edge (%.0f)", x0, AVAIL);
    walk(co, a, "left", -1, (size_t[]){ 2, 4, 6 }, 3, "Left through Hebrew");
    keys(co, "left");
    size_t lo, hi;
    gp_text_input_selection(app, a, &lo, &hi);
    CHECK(lo == 6, "Left at the visual end of the row moved the caret to %zu", lo);
    walk(co, a, "right", +1, (size_t[]){ 4, 2, 0 }, 3, "Right through Hebrew");
    keys(co, "right");
    gp_text_input_selection(app, a, &lo, &hi);
    CHECK(lo == 0, "Right at the visual end moved the caret to %zu", lo);

    /* a forced direction: Latin text in an RTL field hugs the right edge, but reads left to right */
    gp_entity d = root->d;
    focus(co, d);
    CHECK(gp_text_input_row_rtl(app, d, 0), "a field with direction=RTL is not right-to-left");
    float aw;
    gp_text_shape_dir(app->text, 400, 15.f, "abc", 3, GP_BIDI_RTL, NULL, 0, &aw);
    keys(co, "home");
    CHECK(fabsf(caret_x_of(d) - (AVAIL - aw)) < 1.5f, "start of 'abc' in an RTL field at x=%.1f, expected %.1f", caret_x_of(d), AVAIL - aw);
    keys(co, "end");
    CHECK(fabsf(caret_x_of(d) - AVAIL) < 1.5f, "end of 'abc' in an RTL field at x=%.1f, expected %.0f", caret_x_of(d), AVAIL);
    walk(co, d, "left", -1, (size_t[]){ 2, 1, 0 }, 3, "Left through Latin in an RTL field");
    focus(co, a);
}

static void test_bidi_mixed(gp_co *co) {
    gp_entity a = root->a;
    focus(co, a);
    set(co, a, MIXED);
    CHECK(!gp_text_input_row_rtl(app, a, 0), "mixed text starting with Latin is not left-to-right");

    /* visual order is: a b _ gimel bet alef _ c d. Right steps one VISIBLE character at a time, so the
     * logical offset jumps back and forth, and offset 3 is visited twice at two different places */
    keys(co, "home");
    walk(co, a, "right", +1, (size_t[]){ 1, 2, 3, 7, 5, 3, 10, 11, 12 }, 9, "Right through ab אבג cd");
    keys(co, "right");
    size_t lo, hi;
    gp_text_input_selection(app, a, &lo, &hi);
    CHECK(lo == 12, "Right at the end moved to %zu", lo);
    walk(co, a, "left", -1, (size_t[]){ 11, 10, 9, 5, 7, 9, 2, 1, 0 }, 9, "Left through ab אבג cd");

    /* a logical selection can be visually split: b, _, alef, bet (gimel is NOT selected, and sits between them) */
    set(co, a, MIXED);
    keys(co, "home right shift-right shift-right");
    gp_text_input_selection(app, a, &lo, &hi);
    CHECK(lo == 1 && hi == 3, "selection is %zu..%zu, expected 1..3", lo, hi);
    float rects[8][4];
    CHECK(gp_text_input_selection_rects(app, a, rects, 8) == 1, "b + space is contiguous on screen");
    keys(co, "shift-right shift-right");        /* head crosses gimel then bet: 3 -> 7 -> 5 */
    gp_text_input_selection(app, a, &lo, &hi);
    CHECK(lo == 1 && hi == 5, "selection is %zu..%zu, expected 1..5 (b, space, alef)", lo, hi);
    size_t nr = gp_text_input_selection_rects(app, a, rects, 8);
    CHECK(nr == 2, "'b space alef' made %zu rectangle(s); alef is at the far side of the Hebrew run, so 2 were expected", nr);
    if (nr == 2) CHECK(rects[0][0] + rects[0][2] < rects[1][0], "the two rectangles overlap");

    /* the whole thing is one rectangle covering the whole text */
    keys(co, "ctrl-a");
    float mw;
    gp_text_shape_dir(app->text, 400, 15.f, MIXED, strlen(MIXED), GP_BIDI_LTR, NULL, 0, &mw);
    nr = gp_text_input_selection_rects(app, a, rects, 8);
    CHECK(nr == 1 && fabsf(rects[0][2] - mw) < 1.f, "select-all: %zu rect(s), width %.1f, text is %.1f wide", nr, nr ? rects[0][2] : 0, mw);

    /* typing Hebrew after Latin: the Hebrew run grows to the LEFT of the typed letter, so the caret
     * does not move at all (it is always at the run's left edge, next to the Latin), until Latin returns */
    set(co, a, "ab");
    keys(co, "end");
    float after_ab = caret_x_of(a);
    text(co, "ש");
    float x1 = caret_x_of(a);
    text(co, "ל");
    float x2 = caret_x_of(a);
    CHECK(fabsf(x1 - after_ab) < 0.5f && fabsf(x2 - after_ab) < 0.5f,
          "typing Hebrew after Latin moved the caret: %.2f, %.2f, %.2f (it should stay beside the Latin)", after_ab, x1, x2);
    text(co, "c");
    float x3 = caret_x_of(a);
    CHECK(x3 > x2 + 5, "typing Latin after the Hebrew run: caret %.2f -> %.2f, expected it to jump past the run", x2, x3);
    EXPECT(a, "abשלc");

    /* The same in the MIDDLE of text, where a character follows the caret. This is where affinity
     * matters: typing Hebrew between "ab" and " cd" must leave the caret on the LEFT edge of the new
     * letter (the next Hebrew letter appears there), not on its right edge beside the space. */
    set(co, a, "ab cd");
    keys(co, "home right right");
    float before = caret_x_of(a);
    text(co, "ש");
    float mid1 = caret_x_of(a);
    text(co, "ל");
    float mid2 = caret_x_of(a);
    EXPECT(a, "abשל cd");
    CHECK(fabsf(mid1 - before) < 0.5f && fabsf(mid2 - before) < 0.5f,
          "typing Hebrew mid-text moved the caret: %.2f, %.2f, %.2f (it should stay at the left edge of the Hebrew run)", before, mid1, mid2);
    text(co, "x");
    CHECK(caret_x_of(a) > mid2 + 5, "typing Latin after the Hebrew run did not move the caret past it");
}

static void test_bidi_mouse(gp_co *co) {
    gp_entity a = root->a;
    float y = 20 + 1 + 7 + 9;

    /* right-to-left text: each letter is hit from the nearest EDGE. The left half of an RTL letter is
     * its trailing edge (offset after it); the right half is its leading edge (offset before it) */
    set(co, a, HEB_ABG);
    gp_glyph_pos g[8];
    float w;
    int n = gp_text_shape_dir(app->text, 400, 15.f, HEB_ABG, strlen(HEB_ABG), GP_BIDI_RTL, g, 8, &w);
    CHECK(n == 3, "Hebrew shaped to %d glyphs", n);
    float origin = 20 + 1 + 10 + (AVAIL - w);       /* the row is right-aligned */
    for (int i = 0; i < n; i++) {
        size_t cl = g[i].cluster;
        float left_q = origin + g[i].x + g[i].advance * 0.25f, right_q = origin + g[i].x + g[i].advance * 0.75f;
        gp_simulate_mouse(app, GP_SIM_DOWN, left_q, y, 1, 0);
        gp_simulate_mouse(app, GP_SIM_UP, left_q, y, 1, 0);
        settle(co);
        size_t lo, hi;
        gp_text_input_selection(app, a, &lo, &hi);
        CHECK(lo == cl + 2, "click on the LEFT half of the letter at byte %zu put the caret at %zu, expected %zu", cl, lo, cl + 2);
        gp_simulate_mouse(app, GP_SIM_DOWN, right_q, y, 1, 0);
        gp_simulate_mouse(app, GP_SIM_UP, right_q, y, 1, 0);
        settle(co);
        gp_text_input_selection(app, a, &lo, &hi);
        CHECK(lo == cl, "click on the RIGHT half of the letter at byte %zu put the caret at %zu, expected %zu", cl, lo, cl);
    }

    /* a Hebrew word inside a left-to-right sentence is selected whole by a double-click on any of its letters */
    set(co, a, MIXED);
    n = gp_text_shape_dir(app->text, 400, 15.f, MIXED, strlen(MIXED), GP_BIDI_LTR, g, 8 > 12 ? 8 : 8, &w);
    gp_glyph_pos mg[16];
    n = gp_text_shape_dir(app->text, 400, 15.f, MIXED, strlen(MIXED), GP_BIDI_LTR, mg, 16, &w);
    float bet_x = 0, bet_adv = 0;
    for (int i = 0; i < n; i++) if (mg[i].cluster == 5) { bet_x = mg[i].x; bet_adv = mg[i].advance; }
    float bx = 20 + 1 + 10 + bet_x + bet_adv * 0.5f;
    gp_simulate_mouse(app, GP_SIM_DOWN, bx, y, 2, 0);
    gp_simulate_mouse(app, GP_SIM_UP, bx, y, 2, 0);
    settle(co);
    EXPECT_SEL(a, 3, 9);
    float rects[4][4];
    CHECK(gp_text_input_selection_rects(app, a, rects, 4) == 1, "a selected Hebrew word is one rectangle");

    /* a click in the left half of the Hebrew letter bet in a LTR sentence: its trailing edge, offset 7 */
    gp_simulate_mouse(app, GP_SIM_DOWN, 20 + 1 + 10 + bet_x + bet_adv * 0.25f, y, 1, 0);
    gp_simulate_mouse(app, GP_SIM_UP, 20 + 1 + 10 + bet_x + bet_adv * 0.25f, y, 1, 0);
    settle(co);
    EXPECT_SEL(a, 7, 7);
}

static void test_bidi_arabic_and_wrap(gp_co *co) {
    gp_entity a = root->a;
    focus(co, a);
    /* sin lam alef meem: lam + alef ligate into ONE glyph, but there are still four caret stops, and
     * the caret passes through the middle of the ligature */
    set(co, a, "سلام");
    keys(co, "home");
    walk(co, a, "left", -1, (size_t[]){ 2, 4, 6, 8 }, 4, "Left through Arabic with a ligature");

    /* a wrapped right-to-left paragraph: rows tile the text, fit the width, hug the right edge, and
     * Down moves through them */
    gp_entity w = root->w;
    focus(co, w);
    const char *he = "זוהי פסקה ארוכה בעברית שנועדה להראות כיצד טקסט מימין לשמאל נשבר לשורות ומיושר לצד ימין של התיבה, "
                     "גם כאשר הוא ארוך מאוד ומשתרע על פני כמה שורות שלמות בתוך שדה הטקסט הזה שנבנה לצורך הבדיקה";
    set(co, w, he);
    settle(co);
    size_t rows = gp_text_input_rows(app, w), prev_end = 0;
    CHECK(rows >= 3, "a Hebrew paragraph made %zu row(s) at 374 px", rows);
    for (size_t i = 0; i < rows; i++) {
        size_t s0, e0;
        gp_text_input_row_range(app, w, i, &s0, &e0);
        CHECK(gp_text_input_row_rtl(app, w, i), "row %zu of a Hebrew paragraph is not right-to-left", i);
        CHECK(s0 == prev_end || i == 0, "row %zu starts at %zu, the previous row ended at %zu", i, s0, prev_end);
        size_t te = e0;
        while (te > s0 && he[te - 1] == ' ') te--;
        float rw;
        gp_text_shape_dir(app->text, 400, 15.f, he + s0, te - s0, GP_BIDI_RTL, NULL, 0, &rw);
        CHECK(rw <= AVAIL + 0.6f, "Hebrew row %zu is %.1f px wide, the limit is %.0f", i, rw, AVAIL);
        /* the start of each row (its first logical character) is at the right edge */
        keys(co, i == 0 ? "ctrl-home" : "down home");
        float cx = caret_x_of(w);
        CHECK(fabsf(cx - AVAIL) < 2.5f, "row %zu starts at x=%.1f, expected the right edge (%.0f)", i, cx, AVAIL);
        prev_end = e0;
    }
    CHECK(prev_end == strlen(he), "rows end at %zu of %zu bytes", prev_end, strlen(he));
}

/* A focused field must be idle: the only redraws are the caret blinking. This guards against any
 * source of needless redraws (timers that notify unconditionally, events that always re-render);
 * it is a property check, and does not single out one cause. */
static void test_idle_is_idle(gp_co *co) {
    gp_entity a = root->a;
    focus(co, a);
    set(co, a, "idle");
    gp_sleep(co, 0.3);                                  /* let startup chatter settle */
    unsigned long before = app->stat_draws;
    gp_sleep(co, 1.2);
    unsigned long draws = app->stat_draws - before;
    CHECK(draws <= 6, "a focused, untouched field redrew %lu times in 1.2 s (blink alone is ~2)", draws);
    CHECK(draws >= 1, "the caret did not blink (%lu draws)", draws);
}

static void test_tab_and_focus(gp_co *co) {
    gp_entity a = root->a;
    focus(co, a);
    set(co, a, "tabbed");
    keys(co, "tab");                               /* leaves the field; Tab is not text */
    EXPECT(a, "tabbed");
    keys(co, "x");                                 /* typing goes to the new focus, not to `a` */
    EXPECT(a, "tabbed");
    CHECK(strcmp(val(root->m), "x") == 0, "Tab should have focused the next field (got \"%s\")", val(root->m));
}

/* ------------------------------------------------------------ driver */

/* Wait until the window is demonstrably receiving text before the real scenarios start. Twice, in
 * a cluster of runs right after builds, the very first typing step lost one character; those
 * failures were not reproduced in 15 clean runs and the cause was not established. Window focus
 * arriving mid-typing (SDL re-evaluates text input on a focus change) is a suspect, so this makes
 * the test independent of start-up timing. Note that it would also hide a bug of that kind. */
static void wait_until_receiving_text(gp_co *co) {
    focus(co, root->a);
    for (int i = 0; i < 40 && val(root->a)[0] != '~'; i++) {
        gp_simulate_text(app, "~");
        settle(co);
    }
    CHECK(val(root->a)[0] == '~', "the window never delivered text events");
    set(co, root->a, "");
    root->changes = 0;
}

/* The public font API on a live app. It registers the very font the system already chose for
 * regular text, so nothing the other tests rely on changes; what is checked is the plumbing:
 * return values, that a registration repaints, and that toggling the system keeps text working. */
static void test_font_registration_api(gp_co *co) {
    gp_glyph_pos g;
    gp_text_shape(app->text, 400, 16, "A", 1, &g, 1, NULL);
    char path[1024];
    snprintf(path, sizeof path, "%s", gp_text_font_path(app->text, g.font));

    int fonts = gp_text_font_count(app->text);
    gp_sleep(co, 0.3);
    gp_window *win = app->windows[0];
    CHECK(!win->dirty_render, "setup: the window should be clean before registering"); /* else the next check proves nothing */
    CHECK(gp_font_add_file(app, path, NULL), "registering %s", path);
    CHECK(win->dirty_render, "registering a font must schedule a rebuild (nothing ran in between, so ambient redraws can't explain it)");
    gp_sleep(co, 0.3);
    CHECK(gp_font_add_file(app, path, &(gp_font_options){ .weight = 400 }), "registering it again with options");
    CHECK(gp_text_font_count(app->text) == fonts, "the system's own font was already loaded: no new font (%d vs %d)",
          gp_text_font_count(app->text), fonts);

    CHECK(!gp_font_add_file(app, "/nonexistent/font.ttf", NULL), "a missing file is rejected");
    CHECK(!gp_font_add_file(app, path, &(gp_font_options){ .weight = 5 }), "an invalid weight is rejected");
    CHECK(!gp_font_add_file(NULL, path, NULL) && !gp_font_add_memory(NULL, "x", 1, NULL) && !gp_font_add_memory(app, NULL, 0, NULL),
          "NULL app / data are rejected, not crashed on");
    CHECK(!gp_font_add_memory(app, "not a font", 10, NULL), "bytes that are not a font are rejected");

    gp_fonts_use_system(app, false);
    focus(co, root->a);
    set(co, root->a, "plain text");
    CHECK(strcmp(gp_text_input_text(app, root->a), "plain text") == 0, "typing works with the system disabled");
    gp_fonts_use_system(app, true);
    set(co, root->a, "");
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    gp_sleep(co, 0.3); /* first layout */
    wait_until_receiving_text(co);
    test_font_registration_api(co);
    test_keyboard(co);
    test_undo(co);
    test_unicode(co);
    test_fallback_text(co);
    test_limits_and_modes(co);
    test_clipboard(co);
    test_submit_and_multiline(co);
    test_mouse(co);
    test_primary_selection(co);
    test_mouse_on_fallback_text(co);
    test_ime(co);
    test_scroll_follows_caret(co);
    test_idle_is_idle(co);
    test_layout_handler_rebuilds_before_paint(co);
    test_soft_wrap(co);
    test_soft_wrap_editing(co);
    test_soft_wrap_mouse(co);
    test_soft_wrap_scale(co);
    test_bidi_basics(co);
    test_bidi_mixed(co);
    test_bidi_mouse(co);
    test_bidi_arabic_and_wrap(co);
    {   /* a focused right-to-left field must not turn into a relayout loop either (it re-lays out once, for its width) */
        focus(co, root->a);
        set(co, root->a, "שלום עולם");
        gp_sleep(co, 0.3);
        unsigned long before = app->stat_draws;
        gp_sleep(co, 1.2);
        CHECK(app->stat_draws - before <= 6, "a focused RTL field redrew %lu times in 1.2 s", app->stat_draws - before);
    }
    focus(co, root->w);
    set(co, root->w, LONG_TEXT);
    {   /* wrapping must not turn into a relayout loop either */
        gp_sleep(co, 0.3);
        unsigned long before = app->stat_draws;
        gp_sleep(co, 1.2);
        CHECK(app->stat_draws - before <= 6, "a focused wrapping field redrew %lu times in 1.2 s", app->stat_draws - before);
    }
    set(co, root->m, "");
    set(co, root->a, "");
    test_tab_and_focus(co);
    gp_quit(cx->app);
}

static void ready(gp_app *a_, void *userdata) {
    app = a_;
    Root init = { 0 };
    gp_entity r = gp_view_new(app, sizeof init, &init, view, NULL);
    root = gp_entity_get(app, r);
    root->a = gp_text_input_new(app, &(gp_text_input_options){ .owner = r, .on_change = on_change, .on_submit = on_submit });
    root->m = gp_text_input_new(app, &(gp_text_input_options){ .owner = r, .multiline = true, .rows = 4,
                                                               .on_change = on_change, .on_submit = on_submit });
    root->c = gp_text_input_new(app, &(gp_text_input_options){ .max_bytes = 5 });
    root->p = gp_text_input_new(app, &(gp_text_input_options){ .password = true });
    root->r = gp_text_input_new(app, &(gp_text_input_options){ .read_only = true, .text = "fixed text" });
    root->w = gp_text_input_new(app, &(gp_text_input_options){ .owner = r, .multiline = true, .soft_wrap = true, .rows = 4,
                                                               .on_change = on_change });
    root->w2 = gp_text_input_new(app, &(gp_text_input_options){ .multiline = true, .soft_wrap = true, .rows = 3,
                                                                .width = 200, .text = LONG_TEXT });
    root->d = gp_text_input_new(app, &(gp_text_input_options){ .direction = GP_TEXT_DIRECTION_RTL, .text = "abc" });
    gp_open_window(app, &(gp_window_options){ .title = "input_test", .width = 440, .height = 600 }, r);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);   /* a physical pointer over the window is not part of the test */
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "input_test: %d FAILURE(S)\n" : "input_test: all checks passed\n", failures);
    return failures != 0;
}
