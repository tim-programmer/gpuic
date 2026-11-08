/*
 * Editing a soft-wrapped document: the rows are brought up to date by wrapping again only the hard lines an edit touched and moving the rest. After every
 * edit the kept rows must be exactly the rows made again from scratch (every field of every row), over thousands of random edits (inserts and deletes of
 * letters, spaces, newlines, Hebrew, CJK, long words; edits that join and split lines, that cross several lines, at the start and the end of the text, on empty
 * lines); two edits before the rows are read take the full path; a change of width does too; and on a big document an edit takes a fraction of the time of making all the
 * rows again. Needs a display (the field needs a width to wrap at).
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
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }

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
    CHECK(inc1 - inc0 >= 2400 && full1 - full0 <= 50, "most edits updated the rows in place (%lu incremental, %lu from scratch)", inc1 - inc0, full1 - full0);

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
    CHECK(f3 == f2 + 1 && i3 == i2, "two edits before the rows were read: from scratch (%lu -> %lu from scratch, %lu -> %lu in place)", f2, f3, i2, i3);

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

    /* speed: a big wrapped document, an edit in the middle, against the rows made all over again */
    static char big[3000000];
    make_doc(big, sizeof big, 20000);
    g_width = 220;
    gp_app_notify(app);
    gp_text_input_set_text(&cx, field, big);
    settle();
    size_t rows = gp_text_input_rows(app, field);
    unsigned long f5, i5;
    gp_text_input_debug_rows(app, field, &f5, &i5, why, sizeof why);
    size_t mid = strlen(gp_text_input_text(app, field)) / 2;
    const char *t = gp_text_input_text(app, field);
    while (((unsigned char)t[mid] & 0xc0) == 0x80) mid++;
    double t0 = now();
    for (int i = 0; i < 20; i++) {
        gp_text_input_debug_replace(app, field, mid, mid, "q");
        (void)gp_text_input_rows(app, field);
    }
    double inc = (now() - t0) / 20;
    t0 = now();
    for (int i = 0; i < 5; i++) {
        gp_text_input_debug_replace(app, field, mid, mid, "q");
        gp_text_input_debug_replace(app, field, mid, mid, "q");   /* two before a read: the full path */
        (void)gp_text_input_rows(app, field);
    }
    double fullt = (now() - t0) / 5;
    verify("big", 0);
    unsigned long f6, i6;
    gp_text_input_debug_rows(app, field, &f6, &i6, why, sizeof why);
    printf("  (%zu rows: %.2f ms an edit in place, %.2f ms from scratch)\n", rows, inc * 1e3, fullt * 1e3);
    CHECK(rows > 30000 && i6 - i5 >= 20, "the big document: %zu rows, %lu edits in place", rows, i6 - i5);
    CHECK(inc * 8 < fullt, "an edit in place is at least 8 times quicker than making all the rows again (%.2f ms against %.2f ms)", inc * 1e3, fullt * 1e3);
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
