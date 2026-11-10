/*
 * A big soft-wrapped document is wrapped lazily: opening it, scrolling it and changing its width wrap only the lines that are looked at, and
 * every other line's row count is an estimate from its length. The view is anchored to text, so when lines above it turn out to wrap into
 * more or fewer rows than estimated the offset moves and the text does not:
 *   - opening wraps a window's worth of lines; the laid-out rows are the lines' real rows, at row * line height;
 *   - a jump into the middle, then wrapping lines above the view from outside: the same text stays at the top, at the same fraction of its row;
 *   - every wheel notch up or down moves the text by exactly one notch (lines above are wrapped as they come into the overdraw);
 *   - a new width keeps the anchored text at the top and wraps only the lines around it;
 *   - a click lands on the row under the mouse; Ctrl+End, Page Up, Up and Page Down keep the caret in view, Ctrl+Home goes to the top;
 *   - typing and an IME composition in the middle keep the rows right;
 *   - scrolling to the end shows the real last row at the bottom.
 * After every step the wrapped lines are compared with the lines wrapped again and the tree with the counts (gp_text_input_debug_rows), and the
 * laid-out text elements with the rows they claim to be. Needs a display.
 *
 * Mutants (text_input.c; with tests/wrapedit_test.c and tests/input_test.c): no anchor correction, the tree not following a count, a same-size
 * edit not updating the tree (wrapedit), the anchor not following edits before it or not falling back when its text is deleted, no wrapping
 * around the caret before a reveal (needed: the caret deep in lines estimated low, the view far away), one render pass, no re-estimate on a new
 * width, a changed composition not re-wrapped, the Fenwick search off by one (hangs), no upstream affinity (input_test), mouse and Page Down from
 * a stale offset, the caret's line wrapped after the rows in view, small documents not wrapped whole (wrapedit), a glide's target not moved
 * (needed ten notches at once: one is inside the rows built above), no learning of the width of a byte, no re-estimate when it moves, set_text
 * keeping the old document's, breaks off by one, removed lines miscounted, the fraction of the top row dropped, a stale composition line kept:
 * all caught. Survivor: rows_sync wrapping the composition's line (every composition update reveals the caret, which wraps it first).
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

enum { ROWS = 12 };
#define FONT 15.f
#define X0 21.f   /* the field's content in the window: root padding 10, border 1, field padding 10 / 7 */
#define Y0 18.f

static gp_app *app;
static gp_co *co_;
static gp_entity field;
static float g_width = 320;

static gp_node *view(void *self, gp_ctx *cx) {
    return gp_div(gp_size_full(), gp_p(10), gp_bg(gp_rgb(0x1e1e2e)), gp_div(gp_w(g_width), gp_flex_none(), gp_child_view(field)));
}

static unsigned long long rng = 0x9e3779b97f4a7c15ull;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned)(rng >> 11); }

static const char *words[] = { "lorem", "ipsum", "dolor", "sit", "amet", "consectetur", "adipiscing", "elit", "sed", "do", "eiusmod", "tempor",
                               "incididunt", "ut", "labore", "et", "dolore", "magna", "aliqua", "W", "iii", "mmmmmm" };

/* Lines of very different lengths (empty, short, paragraphs that wrap into tens of rows), so that the estimates are wrong both ways; and,
 * 60% of the way in, a block of lines of wide letters, which wrap into about twice the rows their length says (g_wide = where it starts). */
static size_t g_wide;
enum { WIDE_LINES = 400 };
static char *make_doc(int lines, size_t *len) {
    size_t cap = (size_t)lines * 400 + 1000, n = 0;
    char *b = malloc(cap);
    for (int i = 0; i < lines; i++) {
        if (i == lines * 3 / 5) g_wide = n;
        if (i >= lines * 3 / 5 && i < lines * 3 / 5 + WIDE_LINES) {
            for (int w = 0; w < 25; w++) n += (size_t)snprintf(b + n, cap - n, w ? " WWMWW" : "WWMWW");
            b[n++] = '\n';
            continue;
        }
        unsigned kind = rnd() % 10, nw = kind < 3 ? 0 : kind < 7 ? rnd() % 8 : kind < 9 ? 10 + rnd() % 30 : 40 + rnd() % 60;
        for (unsigned w = 0; w < nw; w++) n += (size_t)snprintf(b + n, cap - n, w ? " %s" : "%s", words[rnd() % (sizeof words / sizeof *words)]);
        b[n++] = '\n';
    }
    b[n] = 0;
    *len = n;
    return b;
}

static float lh(void) { return FONT * GP_LINE_HEIGHT; }
static float sy(void) { return gp_text_input_scroll(app, field).y; }
static void settle(void) { gp_sleep(co_, 0.08); }

static size_t row_of(size_t off) { return gp_text_input_debug_row_of(app, field, off, false); }

/* The text at the top of the view: the start of the row there and how far into it the view starts. */
static size_t top_text(float *frac) {
    float y = sy();
    size_t r = (size_t)(y / lh()), s = 0, e = 0;
    gp_text_input_row_range(app, field, r, &s, &e);
    *frac = y - (float)r * lh();
    return s;
}

static void verify(const char *what) {
    char why[400];
    const char *bad = gp_text_input_debug_rows(app, field, NULL, NULL, why, sizeof why);
    CHECK(!bad, "%s: %s", what, bad ? bad : "");
}

static gp_el *find_scroller(gp_el *n) {
    if (n->on_scroll.fn && n->style.overflow_y_scroll) return n;
    for (uint32_t i = 0; i < n->nchildren; i++) {
        gp_el *s = find_scroller(n->children[i]);
        if (s) return s;
    }
    return NULL;
}

/* What is laid out: every text element sits at row * line height and shows that row's text; together they cover the view; the scroller's
 * offset is the field's; the content is as high as all the rows. */
static void check_layout(const char *what) {
    gp_el *s = find_scroller(app->windows[0]->tree);
    if (!s) { CHECK(false, "%s: no scroller", what); return; }
    const char *text = gp_text_input_text(app, field);
    size_t lo = SIZE_MAX, hi = 0, nt = 0;
    for (uint32_t i = 0; i < s->nchildren; i++) {
        gp_el *c = s->children[i];
        if (c->kind != GP_EL_TEXT) continue;
        float rf = c->pos.y / lh();
        size_t r = (size_t)lroundf(rf), a = 0, b = 0;
        CHECK(fabsf(rf - (float)r) < 0.02f, "%s: a row at y %.2f, not a multiple of the line height", what, c->pos.y);
        bool ok = gp_text_input_row_range(app, field, r, &a, &b);
        CHECK(ok && strlen(c->text) == b - a && memcmp(c->text, text + a, b - a) == 0, "%s: row %zu laid out as \"%.30s\", the row is [%zu, %zu) \"%.30s\"", what, r,
              c->text, a, b, ok ? text + a : "");
        if (r < lo) lo = r;
        if (r > hi) hi = r;
        nt++;
        if (failures > 20) return;
    }
    float y = sy();
    size_t v0 = (size_t)(y / lh()), v1 = (size_t)((y + (float)ROWS * lh()) / lh()), n = gp_text_input_rows(app, field);
    if (v1 >= n) v1 = n - 1;
    CHECK(nt > 0 && lo <= v0 && hi >= v1, "%s: rows %zu..%zu laid out, %zu..%zu in view", what, lo, hi, v0, v1);
    CHECK(fabsf((float)(s->scroll_base_y + s->scroll.y) - y) < 0.01f, "%s: the scroller is at %.2f, the field says %.2f", what, (float)(s->scroll_base_y + s->scroll.y), y);
    float extent = s->scroll_max.y + (float)ROWS * lh();
    CHECK(fabsf(extent - (float)n * lh()) < 2.f, "%s: content %.1f high, %zu rows are %.1f", what, extent, n, (float)n * lh());
}

/* The text that was at the top is `by` pixels further down than it was (0: it is still at the top, at the same place in its row). */
static void expect_moved(size_t off, float frac, float by, const char *what) {
    float at = (float)row_of(off) * lh() + frac - sy();
    CHECK(fabsf(at - by) < 0.6f, "%s: the text at the top moved to %.2f, should be at %.2f", what, at, by);
}

static void caret_in_view(const char *what) {
    float x, y;
    gp_text_input_caret_pos(app, field, &x, &y);
    float top = sy();
    CHECK(y >= top - 0.5f && y + lh() <= top + (float)ROWS * lh() + 0.5f, "%s: the caret at y %.1f, the view is %.1f..%.1f", what, y, top, top + (float)ROWS * lh());
}

static void keys(const char *k) { gp_simulate_keystrokes(app, k); settle(); }

static void run(void) {
    settle();
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    size_t len;
    char *doc = make_doc(40000, &len);

    /* opening */
    unsigned long f0, i0;
    gp_text_input_debug_rows(app, field, &f0, &i0, (char[8]){ 0 }, 8);
    gp_text_input_set_text(&cx, field, doc);
    settle();
    size_t lines, unwrapped = gp_text_input_debug_unwrapped(app, field, &lines);
    unsigned long f1, i1;
    gp_text_input_debug_rows(app, field, &f1, &i1, (char[8]){ 0 }, 8);
    printf("  (%zu bytes, %zu lines, ~%zu rows: %lu lines wrapped to open it)\n", len, lines, gp_text_input_rows(app, field), i1 - i0);
    CHECK(lines == 40001 && unwrapped + 200 > lines, "opening wrapped only the lines in view (%zu of %zu not wrapped)", unwrapped, lines);
    verify("opened");
    check_layout("opened");

    /* the wheel at the top: one notch is one notch */
    float x = X0 + 40, y = Y0 + 60;
    gp_simulate_mouse(app, GP_SIM_MOVE, x, y, 1, 0);
    for (int i = 1; i <= 3; i++) {
        gp_simulate_wheel(app, x, y, 0, -1, 0);
        settle();
        CHECK(fabsf(sy() - GP_SCROLL_LINE_PX * (float)i) < 0.01f, "notch %d at the top: offset %.2f", i, sy());
    }

    /* a jump to the middle: rows there are the real ones */
    gp_text_input_debug_scroll_to(app, field, (float)gp_text_input_rows(app, field) * lh() / 2);
    settle();
    verify("jumped");
    check_layout("jumped");
    float frac;
    size_t a = top_text(&frac);
    CHECK(a > len / 4 && a < len * 3 / 4, "the jump went to the middle (offset %zu of %zu)", a, len);

    /* lines above the view wrapped from outside (as a selection's rows or a caret query would): the view stays on its text */
    float before = sy();
    size_t r = (size_t)(before / lh());
    for (size_t k = 1; k <= 400 && k * 37 < r; k++) gp_text_input_row_range(app, field, r - k * 37, NULL, NULL);
    gp_app_notify(app);
    settle();
    expect_moved(a, frac, 0, "lines above wrapped");
    CHECK(fabsf(sy() - before) > lh(), "the estimates above were off by less than a row (%.1f -> %.1f): this tests nothing", before, sy());
    verify("lines above wrapped");
    check_layout("lines above wrapped");

    /* an edit above the view (another view of the same document, a search and replace) keeps the view on its text; an edit that swallows the
     * text at the top leaves the edit's place at the top */
    a = top_text(&frac);
    gp_text_input_debug_replace(app, field, a / 3, a / 3, "inserted\nlines\nabove the view, which push the text down by a few rows\n");
    size_t ins = strlen("inserted\nlines\nabove the view, which push the text down by a few rows\n");
    gp_app_notify(app);
    settle();
    expect_moved(a + ins, frac, 0, "an edit above the view");
    a = top_text(&frac);
    gp_text_input_debug_replace(app, field, a - 500, a + 300, "");
    gp_app_notify(app);
    settle();
    expect_moved(a - 500, 0, 0, "an edit that took the top text away");
    check_layout("edits");
    verify("edits");

    /* the wheel in the middle, up then down: each notch moves the text by one notch, whatever was wrapped meanwhile */
    for (int i = 0; i < 12; i++) {
        bool up = i < 6;
        a = top_text(&frac);
        gp_simulate_wheel(app, x, y, 0, up ? 1 : -1, 0);
        settle();
        expect_moved(a, frac, up ? GP_SCROLL_LINE_PX : -GP_SCROLL_LINE_PX, up ? "a notch up" : "a notch down");
    }
    verify("wheel");
    check_layout("wheel");

    /* a new width: the same text at the top, only lines around it wrapped */
    a = top_text(&frac);
    gp_text_input_debug_rows(app, field, &f1, &i1, (char[8]){ 0 }, 8);
    g_width = 230;
    gp_app_notify(app);
    settle();
    unsigned long f2, i2;
    gp_text_input_debug_rows(app, field, &f2, &i2, (char[8]){ 0 }, 8);
    CHECK(f2 == f1 + 1 && i2 - i1 < 400, "a new width: everything estimated again, few lines wrapped (%lu re-estimates, %lu lines)", f2 - f1, i2 - i1);
    expect_moved(a, frac, 0, "new width");
    CHECK((size_t)(sy() / lh()) == row_of(a), "new width: the anchored text is on the top row (%zu, top %zu)", row_of(a), (size_t)(sy() / lh()));
    verify("new width");
    check_layout("new width");

    /* a click lands on the row under the mouse */
    gp_focus_set(&cx, gp_text_input_focus(app, field));
    settle();
    float top = sy(), cy = Y0 + 5.5f * lh();
    gp_simulate_mouse(app, GP_SIM_DOWN, X0 + 3, cy, 1, 0);
    gp_simulate_mouse(app, GP_SIM_UP, X0 + 3, cy, 1, 0);
    settle();
    size_t want = (size_t)((top + cy - Y0) / lh());
    CHECK(gp_text_input_caret_row(app, field) == want, "a click on row %zu put the caret on row %zu", want, gp_text_input_caret_row(app, field));
    size_t cs, ce;
    gp_text_input_selection(app, field, &cs, &ce);
    size_t rs = 0, re = 0;
    gp_text_input_row_range(app, field, want, &rs, &re);
    CHECK(cs == rs, "a click at the start of row %zu: caret at %zu, the row starts at %zu", want, cs, rs);

    /* lines above wrapped and then a click, with no frame in between: the click still lands on the row under the mouse (the offset follows
     * the anchor before the mouse is mapped) */
    a = top_text(&frac);
    r = (size_t)(sy() / lh());
    for (size_t k = 1; k <= 200 && k * 41 < r; k++) gp_text_input_row_range(app, field, r - k * 41, NULL, NULL);
    gp_simulate_mouse(app, GP_SIM_DOWN, X0 + 3, cy, 1, 0);
    gp_simulate_mouse(app, GP_SIM_UP, X0 + 3, cy, 1, 0);
    settle();
    gp_text_input_selection(app, field, &cs, &ce);
    size_t want_row = row_of(a) + (size_t)((frac + cy - Y0) / lh());
    gp_text_input_row_range(app, field, want_row, &rs, &re);
    CHECK(cs == rs, "a click after lines above were wrapped: caret at %zu, the row under the mouse (%zu) starts at %zu", cs, want_row, rs);

    /* the caret's line above the view and not wrapped (a new width forgets every line): drawing the caret wraps it, before the rows in view
     * are placed */
    gp_text_input_debug_scroll_to(app, field, sy() + 300 * lh());
    settle();
    g_width = 260;
    gp_app_notify(app);
    settle();
    check_layout("caret above the view, new width");
    verify("caret above the view, new width");

    /* Page Down right after lines above were wrapped (no frame in between): the view moves by a page of text (the caret is put in view first:
     * a caret elsewhere would be revealed) */
    gp_simulate_mouse(app, GP_SIM_DOWN, X0 + 3, Y0 + 2.5f * lh(), 1, 0);
    gp_simulate_mouse(app, GP_SIM_UP, X0 + 3, Y0 + 2.5f * lh(), 1, 0);
    settle();
    a = top_text(&frac);
    r = (size_t)(sy() / lh());
    for (size_t k = 1; k <= 200 && k * 43 < r; k++) gp_text_input_row_range(app, field, r - k * 43, NULL, NULL);
    keys("pagedown");
    expect_moved(a, frac, -(float)(ROWS - 1) * lh(), "page down after lines above were wrapped");

    /* the block of wide lines, whose estimates are too low: walking into it with the keyboard keeps the caret in view (the rows between the
     * view's top and the caret are wrapped before the reveal works out where the top goes) */
    gp_text_input_debug_scroll_to(app, field, (float)row_of(g_wide) * lh() - 3 * lh());
    settle();
    gp_simulate_mouse(app, GP_SIM_DOWN, X0 + 3, Y0 + 1.5f * lh(), 1, 0);
    gp_simulate_mouse(app, GP_SIM_UP, X0 + 3, Y0 + 1.5f * lh(), 1, 0);
    settle();
    for (int i = 0; i < 8; i++) {
        keys("pagedown");
        caret_in_view("page down into wide lines");
    }
    for (int i = 0; i < 30; i++) {
        gp_simulate_keystrokes(app, "down");
        if (i % 5 == 4) { settle(); caret_in_view("down into wide lines"); }
    }
    settle();
    check_layout("wide lines");
    verify("wide lines");

    /* the caret deep in the wide lines, the view far away and every line forgotten (a new width): typing reveals the caret, and the rows between
     * the view's new top and the caret are wrapped first (their estimates are half the truth), so the caret is in view after the frame */
    gp_text_input_debug_scroll_to(app, field, sy() * 0.3f);
    settle();
    g_width = 300;
    gp_app_notify(app);
    settle();
    gp_simulate_text(app, "x");
    settle();
    caret_in_view("typed far from the view, in wide lines");
    check_layout("typed far from the view");

    /* the keyboard keeps the caret in view */
    keys("pagedown pagedown pagedown");
    caret_in_view("page down");
    check_layout("page down");
    keys("ctrl-end");
    caret_in_view("ctrl-end");
    check_layout("ctrl-end");
    CHECK(gp_text_input_caret_row(app, field) + 1 == gp_text_input_rows(app, field), "ctrl-end: the caret on the last row");
    for (int i = 0; i < 6; i++) {
        keys("pageup");
        caret_in_view("page up");
    }
    for (int i = 0; i < 20; i++) keys("up");
    caret_in_view("up");
    check_layout("up");
    verify("keys");

    /* typing and composing in the middle */
    gp_text_input_debug_scroll_to(app, field, (float)gp_text_input_rows(app, field) * lh() * 0.4f);
    settle();
    gp_simulate_mouse(app, GP_SIM_DOWN, X0 + 30, Y0 + 4.5f * lh(), 1, 0);
    gp_simulate_mouse(app, GP_SIM_UP, X0 + 30, Y0 + 4.5f * lh(), 1, 0);
    settle();
    gp_simulate_text(app, "typed in the middle of a big document, long enough to wrap into a second row of the field\n");
    settle();
    caret_in_view("typed");
    verify("typed");
    check_layout("typed");
    app->debug_ignore_platform_ime = true;
    size_t n0 = gp_text_input_rows(app, field);
    gp_simulate_ime(app, "\xe3\x81\x8b\xe3\x81\x8d\xe3\x81\x8f\xe3\x81\x91\xe3\x81\x93\xe3\x81\x95\xe3\x81\x97\xe3\x81\x99\xe3\x81\x9b\xe3\x81\x9d"
                         "\xe3\x81\x9f\xe3\x81\xa1\xe3\x81\xa4\xe3\x81\xa6\xe3\x81\xa8\xe3\x81\xaa\xe3\x81\xab\xe3\x81\xac\xe3\x81\xad\xe3\x81\xae", 20);
    size_t n1 = gp_text_input_rows(app, field);
    verify("composing");
    CHECK(n1 > n0, "a long composition wraps into more rows (%zu -> %zu)", n0, n1);
    gp_simulate_ime(app, "\xe3\x81\x8b", 1);   /* (a different composition in the same place) */
    CHECK(gp_text_input_rows(app, field) < n1, "a shorter composition, fewer rows (%zu, were %zu)", gp_text_input_rows(app, field), n1);
    verify("composing again");
    gp_simulate_ime(app, "", 0);
    CHECK(gp_text_input_rows(app, field) == n0, "the composition gone, the rows are back (%zu, were %zu)", gp_text_input_rows(app, field), n0);
    verify("composed");
    app->debug_ignore_platform_ime = false;
    settle();

    /* smooth scrolling: a glide that is under way when lines above are wrapped ends one notch from where it started, in text */
    gp_set_smooth_scrolling(app, 0.12);
    gp_text_input_debug_scroll_to(app, field, (float)gp_text_input_rows(app, field) * lh() * 0.25f);
    settle();
    for (int i = 0; i < 4; i++) {   /* (ten notches at once: further than the rows built above the view, so lines are wrapped during the glide) */
        a = top_text(&frac);
        gp_simulate_wheel(app, x, y, 0, 10, 0);
        gp_sleep(co_, 0.6);
        expect_moved(a, frac, 10 * GP_SCROLL_LINE_PX, "a smooth glide of ten notches up");
    }
    gp_set_smooth_scrolling(app, 0);

    /* the end */
    gp_text_input_debug_scroll_to(app, field, 1e9f);
    settle();
    settle();
    check_layout("end");
    size_t n = gp_text_input_rows(app, field);
    CHECK(fabsf(sy() + (float)ROWS * lh() - (float)n * lh()) < 1.f, "at the end the last row is at the bottom (offset %.1f, %zu rows)", sy(), n);
    verify("end");

    unwrapped = gp_text_input_debug_unwrapped(app, field, &lines);
    printf("  (%zu of %zu lines still not wrapped at the end)\n", unwrapped, lines);
    CHECK(unwrapped * 10 > lines * 9, "most lines were never wrapped (%zu of %zu)", unwrapped, lines);

    /* the estimates, from the width of a byte in the lines seen so far, against the real count */
    size_t est = gp_text_input_rows(app, field);
    gp_text_input_debug_wrap_all(app, field);
    size_t real = gp_text_input_rows(app, field);
    printf("  (%zu rows estimated, %zu real)\n", est, real);
    CHECK(est * 100 > real * 90 && est * 100 < real * 110, "the estimate is within 10%% (%zu estimated, %zu real)", est, real);
    verify("all wrapped");
    free(doc);

    /* a document in another script: Cyrillic letters are two bytes each, so the guess from Latin text is twice the truth; once the lines in view
     * are measured, the rest are estimated again from them */
    static const char *cyr[] = { "\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82", "\xd0\xbc\xd0\xb8\xd1\x80", "\xd0\xb4\xd0\xbe\xd0\xbc",
                                 "\xd1\x81\xd0\xbb\xd0\xbe\xd0\xb2\xd0\xbe", "\xd0\xb8" };
    size_t cap = 30000 * 300, m = 0;
    char *c = malloc(cap);
    for (int i = 0; i < 30000; i++) {
        unsigned nw = rnd() % 4 == 0 ? 0 : 5 + rnd() % 30;
        for (unsigned w = 0; w < nw; w++) m += (size_t)snprintf(c + m, cap - m, w ? " %s" : "%s", cyr[rnd() % 5]);
        c[m++] = '\n';
    }
    c[m] = 0;
    gp_text_input_set_text(&cx, field, c);
    settle();
    settle();
    est = gp_text_input_rows(app, field);
    check_layout("cyrillic");
    gp_text_input_debug_wrap_all(app, field);
    real = gp_text_input_rows(app, field);
    printf("  (cyrillic: %zu rows estimated, %zu real)\n", est, real);
    CHECK(est * 100 > real * 85 && est * 100 < real * 115, "another script: the estimate is within 15%% once lines were seen (%zu estimated, %zu real)", est, real);
    verify("cyrillic");
    free(c);
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    run();
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    field = gp_text_input_new(app, &(gp_text_input_options){ .multiline = true, .soft_wrap = true, .rows = ROWS, .font_size = FONT });
    gp_entity r = gp_view_new(app, 1, &(char){ 0 }, view, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "lazywrap_test", .width = 400, .height = 360 }, r);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "lazywrap_test: %d FAILURE(S)\n" : "lazywrap_test: all checks passed\n", failures);
    return failures != 0;
}
