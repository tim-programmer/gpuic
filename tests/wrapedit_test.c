/*
 * Editing a soft-wrapped document: the rows are kept per hard line (breaks relative to the line's start), so an edit wraps again only the lines it
 * touched and nothing else moves. After every edit the kept rows must be exactly the lines wrapped again (every field of every row), and the
 * Fenwick tree over the row counts must add up, over thousands of random edits (inserts and deletes of letters, spaces, newlines, Hebrew, CJK, long
 * words; edits that join and split lines, that cross several lines, at the start and the end of the text, on empty lines); any number of edits
 * before a read wrap only their lines; a change of width estimates everything again; and a big document is not wrapped when it is opened, only
 * where it is looked at. (tests/lazywrap_test.c scrolls such a document.) Needs a display (the field needs a width to wrap at).
 */
#include "../src/internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
static gp_entity field;
static float g_width = 220;

static gp_node *view(void *self, gp_ctx *cx) {
    return gp_div(gp_size_full(), gp_p(10), gp_bg(gp_rgb(0x1e1e2e)), gp_div(gp_w(g_width), gp_flex_none(), gp_child_view(field)));
}

static unsigned long long rng = 88172645463325252ull;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned)(rng >> 11); }
static double now(void) { return (double)gp_now_ns() * 1e-9; }

static const char *words[] = { "alpha", "beta", "gamma", "delta", "epsilon", "supercalifragilisticexpialidocious", "a", "ab", "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d", "\xe3\x81\x82\xe3\x81\x84\xe3\x81\x86\xe3\x81\x88\xe3\x81\x8a", "x-y", "tail" };
static const char *pieces[] = { " ", "\n", "\n\n", "word ", "a", "\xd7\x90\xd7\x91", "\xe3\x81\x82", " longer words and more words ", "-", "\n\xe3\x81\x82\xe3\x81\x84\n", "0123456789", "" };

static void make_doc(char *buf, size_t cap, int lines) {
    size_t n = 0;
    for (int i = 0; i < lines && n + 400 < cap; i++) {
        int nw = (int)(rnd() % 14);
        for (int w = 0; w < nw; w++) n += (size_t)snprintf(buf + n, cap - n, "%s ", words[rnd() % (sizeof words / sizeof *words)]);
        if (rnd() % 7 == 0) n += (size_t)snprintf(buf + n, cap - n, "%s", words[5]);
        buf[n++] = '\n';
    }
    buf[n] = 0;
}

static void settle(void) { gp_sleep(co_, 0.06); }

static void verify(const char *what, int i) {
    char why[300];
    unsigned long full, inc;
    const char *bad = gp_text_input_debug_rows(app, field, &full, &inc, why, sizeof why);
    CHECK(!bad, "%s %d: the rows kept up to date differ from the rows made again: %s", what, i, bad ? bad : "");
    if (i % 12 == 0) {   /* (the line index is the old test's business: every now and then) */
        const char *idx = gp_text_input_debug_check(app, field, why, sizeof why);
        CHECK(!idx, "%s %d: line index: %s", what, i, idx ? idx : "");
    }
}

static void run(void) {
    settle();
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    static char doc[200000];
    make_doc(doc, sizeof doc, 400);
    gp_text_input_set_text(&cx, field, doc);
    settle();
    unsigned long full0, inc0;
    char why[300];
    CHECK(gp_text_input_debug_rows(app, field, &full0, &inc0, why, sizeof why) == NULL, "the rows of the document: %s", why);
    CHECK(gp_text_input_rows(app, field) > 800, "a document that wraps to many rows (%zu)", gp_text_input_rows(app, field));
    size_t lines, unwrapped = gp_text_input_debug_unwrapped(app, field, &lines);
    CHECK(unwrapped == 0, "a small document is wrapped whole, its rows exact (%zu of %zu lines not wrapped)", unwrapped, lines);

    /* random edits, the rows read (so brought up to date) after each */
    for (int i = 0; i < 2500; i++) {
        size_t len = strlen(gp_text_input_text(app, field));
        size_t start = rnd() % (len + 1), del = rnd() % 4 == 0 ? rnd() % 60 : rnd() % 3 == 0 ? rnd() % 4 : 0;
        if (rnd() % 9 == 0) start = 0;
        if (rnd() % 9 == 0) start = len;
        if (start + del > len) del = len - start;
        const char *ins = pieces[rnd() % (sizeof pieces / sizeof *pieces)];
        /* edits go on character boundaries */
        const char *t = gp_text_input_text(app, field);
        while (start > 0 && ((unsigned char)t[start] & 0xc0) == 0x80) start--;
        while (start + del < len && ((unsigned char)t[start + del] & 0xc0) == 0x80) del++;
        gp_text_input_debug_replace(app, field, start, start + del, ins);
        verify("random edit", i);
        if (failures) return;
    }
    unsigned long full1, inc1;
    gp_text_input_debug_rows(app, field, &full1, &inc1, why, sizeof why);
    CHECK(full1 == full0 && inc1 - inc0 <= 2500 * 3, "edits wrap only the lines they touch (%lu lines wrapped by 2500 edits, %lu re-estimates)", inc1 - inc0, full1 - full0);

    /* edits that join and split lines: delete a line break; put one in the middle of a long line; a range across many lines */
    {
        const char *t = gp_text_input_text(app, field);
        const char *nl = strchr(t + 100, '\n');
        gp_text_input_debug_replace(app, field, (size_t)(nl - t), (size_t)(nl - t) + 1, "");
        verify("join", 0);
        t = gp_text_input_text(app, field);
        gp_text_input_debug_replace(app, field, 300, 300, "\n");
        verify("split", 0);
        gp_text_input_debug_replace(app, field, 200, 3000, "X");
        verify("many lines", 0);
        gp_text_input_debug_replace(app, field, 0, 0, "first\n");
        verify("at the start", 0);
        size_t end = strlen(gp_text_input_text(app, field));
        gp_text_input_debug_replace(app, field, end, end, "\nlast");
        verify("at the end", 0);
        end = strlen(gp_text_input_text(app, field));
        gp_text_input_debug_replace(app, field, 0, end, "");
        verify("everything deleted", 0);
        gp_text_input_debug_replace(app, field, 0, 0, "one\n\n\nthree");
        verify("into nothing", 0);
        gp_text_input_debug_replace(app, field, 4, 5, "");
        verify("an empty line removed", 0);
    }

    /* two edits before the rows are read: made again from scratch (and right) */
    gp_text_input_set_text(&cx, field, doc);
    settle();
    unsigned long f2, i2;
    gp_text_input_debug_rows(app, field, &f2, &i2, why, sizeof why);
    gp_text_input_debug_replace(app, field, 50, 50, "A\n");
    gp_text_input_debug_replace(app, field, 700, 705, "B");
    verify("two edits", 0);
    unsigned long f3, i3;
    gp_text_input_debug_rows(app, field, &f3, &i3, why, sizeof why);
    CHECK(f3 == f2 && i3 - i2 <= 4, "two edits before the rows were read: only their lines (%lu -> %lu re-estimates, %lu lines wrapped)", f2, f3, i3 - i2);

    /* a change of width: again from scratch */
    g_width = 150;
    gp_app_notify(app);
    settle();
    verify("narrower", 0);
    unsigned long f4, i4;
    gp_text_input_debug_rows(app, field, &f4, &i4, why, sizeof why);
    CHECK(f4 > f3, "a new width makes the rows again (%lu -> %lu)", f3, f4);
    gp_text_input_debug_replace(app, field, 10, 10, "zz");
    verify("after the width changed", 0);
    gp_text_input_debug_rows(app, field, &f4, &i4, why, sizeof why);
    /* an edit and then a change of width before the rows are read: the rows are all made again at the new width, not just the line that was edited */
    gp_text_input_debug_replace(app, field, 20, 20, "yy");
    g_width = 300;
    gp_app_notify(app);
    settle();
    verify("an edit, then a width", 0);

    /* a big wrapped document: opening it wraps only what is looked at (the rows of the rest are estimated); an edit wraps its line */
    static char big[3000000];
    make_doc(big, sizeof big, 20000);
    g_width = 220;
    gp_app_notify(app);
    settle();
    double t0 = now();
    gp_text_input_set_text(&cx, field, big);
    size_t est = gp_text_input_rows(app, field);
    double open_t = now() - t0;
    settle();
    lines = 0;
    unwrapped = gp_text_input_debug_unwrapped(app, field, &lines);
    CHECK(lines > 19000 && unwrapped + 200 > lines, "opening the big document wrapped only the lines in view (%zu of %zu not wrapped)", unwrapped, lines);
    verify("big, opened", 1);
    unsigned long f5, i5;
    gp_text_input_debug_rows(app, field, &f5, &i5, why, sizeof why);
    size_t mid = strlen(gp_text_input_text(app, field)) / 2;
    const char *t = gp_text_input_text(app, field);
    while (((unsigned char)t[mid] & 0xc0) == 0x80) mid++;
    for (int i = 0; i < 20; i++) {
        gp_text_input_debug_replace(app, field, mid, mid, "q");
        (void)gp_text_input_rows(app, field);
    }
    unsigned long f6, i6;
    gp_text_input_debug_rows(app, field, &f6, &i6, why, sizeof why);
    CHECK(f6 == f5 && i6 == i5, "edits out of view wrap nothing (%lu lines wrapped)", i6 - i5);
    t0 = now();
    gp_text_input_debug_wrap_all(app, field);
    double all_t = now() - t0;
    const char *idx = gp_text_input_debug_check(app, field, why, sizeof why);
    CHECK(!idx, "big: line index: %s", idx ? idx : "");
    verify("big, all wrapped", 1);
    size_t rows = gp_text_input_rows(app, field);
    printf("  (%zu rows, estimated %zu when opened: %.2f ms to open, %.0f ms to wrap it all)\n", rows, est, open_t * 1e3, all_t * 1e3);
    CHECK(rows > 30000, "the big document: %zu rows", rows);
    CHECK(est * 10 > rows * 7 && est * 10 < rows * 13, "the estimate is within 30%% (%zu estimated, %zu rows)", est, rows);
    CHECK(open_t * 8 < all_t, "opening is at least 8 times quicker than wrapping everything (%.2f ms against %.2f ms)", open_t * 1e3, all_t * 1e3);
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    run();
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    field = gp_text_input_new(app, &(gp_text_input_options){ .multiline = true, .soft_wrap = true, .rows = 8 });
    gp_entity r = gp_view_new(app, 1, &(char){ 0 }, view, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "wrapedit_test", .width = 300, .height = 300 }, r);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "wrapedit_test: %d FAILURE(S)\n" : "wrapedit_test: all checks passed\n", failures);
    return failures != 0;
}
