/*
 * Very long single lines. Without wrap a long left-to-right row is cut into segments that are shaped one at a time and only where they are
 * needed; with soft wrap a long line is wrapped in pieces, each like a line of its own.
 *
 * No wrap (a 480 KB line among short ones, and a single-line password field):
 *   - opening and drawing it at its start measure a few segments, not the line;
 *   - the caret's x at an offset equals the width of the text before it shaped whole (segments are cut after spaces, where nothing kerns);
 *   - the laid-out segments sit exactly at the caret x of their first byte, show that text, and cover the view, at the start, deep in the
 *     line after a reveal, and after horizontal wheel scrolling;
 *   - a click at the caret's x lands on that offset, far into the line;
 *   - hundreds of random edits in the line (tokens, combining marks, emoji, deletions across segments): the caret x everywhere equals that of a
 *     field given the same text afresh;
 *   - a composition pushes the text after it right by its width;
 *   - selection rectangles across segments are one span.
 * Soft wrap (a 300 KB and a 150 KB line among short ones):
 *   - the long lines are units in pieces, the rest not; wrapping all of it and comparing every row with the units wrapped again, and the row of
 *     sampled offsets with a plain search, agree (gp_text_input_debug_rows / _check, which also check the unit invariants);
 *   - End at the last row of a piece puts the caret at the end of that row (upstream), not at the start of the next piece;
 *   - random edits in a long line wrap again only the piece they are in, and the rows stay right; a line break put in a long line and taken
 *     out again, a long line made short and a short one long;
 *   - the laid-out rows around piece boundaries are the rows, and a composition in a long line wraps.
 * Needs a display.
 *
 * Mutants (text_input.c), all caught: kept segments not moved by edits before them or not given the edit's length (gp_text_input_debug_segs_check
 * finds the stale row), an edit at a boundary not cutting the segment before again (a bare combining mark there), later boundaries not moved,
 * widths kept past an edit, no extra width after a composition, no margin segment, segments laid out at 0, no rebuild when scrolling sideways out
 * of what was built, either way (needs nothing focused: the caret blink rebuilds), a far-right caret measured, password widths of the real text,
 * widths off by half a pixel, a left-to-right line with right-to-left runs cut; for pieces: an edit at a piece boundary not cutting the piece
 * before, the re-cut region not grown by the edit (a 100 KB paste), later units not moved in bytes or lines, a piece's last row ending hard (in
 * the row and in line_row), no upstream caret across pieces, the composition in both pieces at their boundary, no pieces, every edit taking the
 * whole line, a same-count splice not updating the tree. Equivalent: the composition at a segment boundary drawn at the start of the next segment
 * (same x), the segment search a fraction of a segment off (the margin segment absorbs it), the selection's line-break stub after a partial row
 * (it falls outside what is built, never in view).
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

#define FONT 15.f
#define X0 21.f   /* the first field's content in the window: root padding 10, border 1, field padding 10 / 7 */
#define Y0 18.f

static gp_app *app;
static gp_co *co_;
static gp_entity subj, orac, wrapped, pw, pw_orac;

static gp_node *view(void *self, gp_ctx *cx) {
    return gp_div(gp_size_full(), gp_flex_col(), gp_p(10), gp_gap(8), gp_bg(gp_rgb(0x1e1e2e)), gp_child_view(subj), gp_child_view(orac),
                  gp_child_view(wrapped), gp_child_view(pw), gp_child_view(pw_orac));
}

static unsigned long long rng = 0x2545f4914f6cdd1dull;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned)(rng >> 11); }

static float lh(void) { return FONT * GP_LINE_HEIGHT; }
static void settle(void) { gp_sleep(co_, 0.08); }
static gp_ctx cx_;

static char *tokens(size_t from, size_t count, size_t *len) {
    char *b = malloc(count * 8 + 1);
    size_t n = 0;
    for (size_t i = 0; i < count; i++) n += (size_t)sprintf(b + n, "t%06zu ", from + i);
    *len = n;
    return b;
}

/* where the long line is in a field's text */
static size_t line_start(gp_entity f, int k) {
    const char *t = gp_text_input_text(app, f);
    size_t p = 0;
    for (int i = 0; i < k; i++) p = (size_t)(strchr(t + p, '\n') - t) + 1;
    return p;
}
static size_t line_len(gp_entity f, int k) {
    size_t ls = line_start(f, k);
    const char *t = gp_text_input_text(app, f), *e = strchr(t + ls, '\n');
    return e ? (size_t)(e - t) - ls : strlen(t) - ls;
}

static float xat(gp_entity f, size_t off) {
    gp_text_input_debug_select(app, f, off, off, false);
    float x, y;
    gp_text_input_caret_pos(app, f, &x, &y);
    return x;
}

/* The width of text shaped as one run, its advances added in double (the text system adds them in float, which drifts by pixels over tens of
 * thousands of them: the segments, which add a few thousand at a time, are closer to the truth than that). */
static float shaped_w(const char *t, size_t n) {
    int cap = (int)n + 16;
    gp_glyph_pos *g = malloc((size_t)cap * sizeof *g);
    float w = 0;
    int ng = gp_text_shape_dir(app->text, 400, FONT, t, n, GP_BIDI_LTR, g, cap, &w);
    double sum = 0;
    for (int i = 0; i < ng && i < cap; i++) sum += g[i].advance;
    free(g);
    return (float)sum;
}

static void collect_scrollers(gp_el *n, gp_el **out, int *k) {
    if (n->on_scroll.fn && (n->style.overflow_x_scroll || n->style.overflow_y_scroll)) { if (*k < 8) out[(*k)++] = n; return; }
    for (uint32_t i = 0; i < n->nchildren; i++) collect_scrollers(n->children[i], out, k);
}
static gp_el *scroller(int which) {
    gp_el *s[8];
    int k = 0;
    collect_scrollers(app->windows[0]->tree, s, &k);
    return which < k ? s[which] : NULL;
}

/* The laid-out long row of the first field: each segment's text element at the caret x of its first byte, with that text; together they
 * cover the view. Reads the layout first (finding the x of a byte selects it, which changes what the next frame draws). */
static void check_long_layout(const char *what, size_t ls, size_t ll) {
    gp_el *s = scroller(0);
    if (!s) { CHECK(false, "%s: no scroller", what); return; }
    gp_el *row = NULL;
    for (uint32_t i = 0; i < s->nchildren; i++)
        if (s->children[i]->kind == GP_EL_DIV && s->children[i]->nchildren > 0 && s->children[i]->children[0]->kind == GP_EL_TEXT) row = s->children[i];
    if (!row) { CHECK(false, "%s: the long row is not laid out in segments", what); return; }
    CHECK(fabsf(row->pos.y - lh()) < 0.01f, "%s: the long row at y %.2f, not on line 1", what, row->pos.y);
    CHECK(row->nchildren <= 8, "%s: %u segments built for one view", what, row->nchildren);
    const char *t = gp_text_input_text(app, subj);
    enum { MAXS = 16 };
    size_t offs[MAXS];
    float xs[MAXS], x_lo = 1e30f, x_hi = -1e30f;
    uint32_t n = row->nchildren < MAXS ? row->nchildren : MAXS;
    for (uint32_t i = 0; i < n; i++) {
        gp_el *c = row->children[i];
        size_t cl = strlen(c->text);
        const char *at = cl ? strstr(t + ls, c->text) : NULL;
        offs[i] = at ? (size_t)(at - t) : SIZE_MAX;
        CHECK(at && offs[i] + cl <= ls + ll, "%s: segment %u's text is not in the line", what, i);
        xs[i] = row->pos.x + c->pos.x;
        if (xs[i] < x_lo) x_lo = xs[i];
        if (xs[i] + c->size.w > x_hi) x_hi = xs[i] + c->size.w;
        if (i > 0 && offs[i] != SIZE_MAX && offs[i - 1] != SIZE_MAX)
            CHECK(offs[i - 1] + strlen(row->children[i - 1]->text) == offs[i], "%s: segments %u and %u are not consecutive", what, i - 1, i);
    }
    float sx = gp_text_input_scroll(app, subj).x, vw = 380;
    CHECK((x_lo <= sx + 0.5f || offs[0] == ls) && (x_hi >= sx + vw || offs[n - 1] + strlen(row->children[n - 1]->text) == ls + ll),
          "%s: segments cover x %.1f..%.1f, the view is %.1f..%.1f", what, x_lo, x_hi, sx, sx + vw);
    size_t keep_a, keep_h;
    gp_text_input_selection(app, subj, &keep_a, &keep_h);
    for (uint32_t i = 0; i < n; i++)
        if (offs[i] != SIZE_MAX) {
            float x = xat(subj, offs[i]);
            CHECK(fabsf(x - xs[i]) < 0.05f + fabsf(x) * 2e-7f, "%s: segment %u (byte %zu) laid out at x %.2f, the caret there is at %.2f", what, i, offs[i], xs[i], x);
        }
    gp_text_input_debug_select(app, subj, keep_a, keep_h, false);
}

static void segs_ok(const char *what, gp_entity f) {
    char why[300];
    const char *bad = gp_text_input_debug_segs_check(app, f, 0, NULL, 0, NULL, why, sizeof why);
    CHECK(!bad, "%s: %s", what, bad ? bad : "");
}

/* the caret x of sampled offsets in the long line: the same in the field and in a field given its text afresh */
static void compare_with_fresh(const char *what, gp_entity a, gp_entity b, int line) {
    segs_ok(what, a);
    gp_text_input_set_text(&cx_, b, gp_text_input_text(app, a));
    size_t ls = line_start(a, line), ll = line_len(a, line);
    const char *t = gp_text_input_text(app, a);
    for (int i = 0; i <= 40; i++) {
        size_t off = ls + ll * (size_t)i / 40;
        while (off > ls && ((unsigned char)t[off] & 0xc0) == 0x80) off--;
        float xa = xat(a, off), xb = xat(b, off);
        if (fabsf(xa - xb) > 0.05f + fabsf(xa) * 2e-7f) {   /* (the step of a float out there) */
            CHECK(false, "%s: caret at byte %zu: x %.2f after the edits, %.2f in a fresh field", what, off, xa, xb);
            return;
        }
    }
}

static void random_edit(gp_entity f, int line, int k) {
    size_t ls = line_start(f, line), ll = line_len(f, line);
    const char *t = gp_text_input_text(app, f);
    size_t at = ls + (k % 7 == 0 ? (ll / 2048) * (rnd() % 8) * 0 + ((rnd() % (ll / 2048 + 1)) * 2048 < ll ? (rnd() % (ll / 2048 + 1)) * 2048 : 0) : rnd() % (ll + 1));
    if (at > ls + ll) at = ls + ll;
    while (at > ls && ((unsigned char)t[at] & 0xc0) == 0x80) at--;
    static const char *ins[] = { "u12345 ", "e\xcc\x81", "\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd", "x", "", "longerwordwithoutspaces", " " };
    const char *s = ins[rnd() % 7];
    size_t del = rnd() % 3 == 0 ? rnd() % 3000 : 0;
    if (at + del > ls + ll) del = ls + ll - at;
    while (at + del < ls + ll && ((unsigned char)t[at + del] & 0xc0) == 0x80) del++;
    gp_text_input_debug_replace(app, f, at, at + del, s);
}

static void verify(const char *what, int i) {
    char why[400];
    const char *bad = gp_text_input_debug_rows(app, wrapped, NULL, NULL, why, sizeof why);
    CHECK(!bad, "%s %d: %s", what, i, bad ? bad : "");
}

/* the wrapped field's laid-out rows: each text element at row * line height, showing that row */
static void check_wrapped_layout(const char *what) {
    /* (what the last frame wrapped can move the width of a byte enough to estimate the rest again, which renumbers the rows: a frame with
     * the numbers settled first) */
    (void)gp_text_input_rows(app, wrapped);
    gp_app_notify(app);
    settle();
    gp_el *s = scroller(2);
    if (!s) { CHECK(false, "%s: no scroller", what); return; }
    const char *text = gp_text_input_text(app, wrapped);
    size_t nt = 0;
    for (uint32_t i = 0; i < s->nchildren; i++) {
        gp_el *c = s->children[i];
        if (c->kind != GP_EL_TEXT) continue;
        size_t r = (size_t)lroundf(c->pos.y / lh()), a = 0, b = 0;
        bool ok = gp_text_input_row_range(app, wrapped, r, &a, &b);
        CHECK(ok && strlen(c->text) == b - a && memcmp(c->text, text + a, b - a) == 0, "%s: row %zu laid out as \"%.20s\", the row is [%zu, %zu)", what, r, c->text, a, b);
        nt++;
        if (failures > 30) return;
    }
    CHECK(nt > 0, "%s: no rows laid out", what);
}

static void run(void) {
    settle();
    cx_ = gp_ctx_for(app, GP_ENTITY_NULL);

    /* ---------------- no wrap */
    size_t tl;
    char *tok = tokens(0, 60000, &tl);
    size_t cap = tl + 4096, n = 0;
    char *doc = malloc(cap);
    n += (size_t)sprintf(doc, "first line\n");
    memcpy(doc + n, tok, tl);
    n += tl;
    n += (size_t)sprintf(doc + n, "\nafter the long line\nand one more\n");
    gp_text_input_set_text(&cx_, subj, doc);   /* (the caret at the end of the text, the view at its start) */
    settle();
    size_t ls = line_start(subj, 1), ll = line_len(subj, 1), measured, nseg = gp_text_input_debug_segments(app, subj, &measured);
    printf("  (a %zu-byte line: %zu segments, %zu measured to draw its start)\n", ll, nseg, measured);
    CHECK(nseg > 200 && measured < 10, "drawing the start of the long line measured %zu of %zu segments", measured, nseg);
    gp_text_input_debug_select(app, subj, ls + ll, ls + ll, false);   /* the caret at the end of the long line, the view at its start */
    settle();
    gp_text_input_debug_segments(app, subj, &measured);
    CHECK(measured < 10, "a caret far right of the view measured %zu of %zu segments to be drawn", measured, nseg);
    gp_text_input_debug_select(app, subj, 0, 0, true);
    settle();
    check_long_layout("at the start", ls, ll);

    /* the caret x against the text shaped whole */
    const char *t = gp_text_input_text(app, subj);
    for (int i = 1; i <= 12; i++) {
        size_t off = ls + (size_t)i * 1999;
        float x = xat(subj, off), w = shaped_w(t + ls, off - ls);
        CHECK(fabsf(x - w) < 0.25f, "the caret at byte %zu of the line: x %.2f, the text before it shaped whole is %.2f wide", off - ls, x, w);
    }

    /* deep in the line: revealed, laid out, clicked */
    gp_focus_set(&cx_, gp_text_input_focus(app, subj));
    for (int i = 0; i < 6; i++) {
        size_t off = ls + ll * (size_t)(i * 2 + 3) / 14;
        while (t[off] != ' ') off++;   /* (a place a click can land on unambiguously) */
        off++;
        gp_text_input_debug_select(app, subj, off, off, true);
        settle();
        check_long_layout("revealed deep in the line", ls, ll);
        off -= 32;   /* (four tokens left of the revealed caret: clear of the scroll bar at the right edge) */
        gp_text_input_debug_select(app, subj, off, off, false);
        float x, y;
        gp_text_input_caret_pos(app, subj, &x, &y);
        gp_point sc = gp_text_input_scroll(app, subj);
        gp_text_input_debug_select(app, subj, 0, 0, false);   /* (so that the click is what puts it back; the view stays) */
        settle();
        gp_simulate_mouse(app, GP_SIM_DOWN, X0 + x - sc.x + 0.3f, Y0 + y - sc.y + lh() / 2, 1, 0);
        gp_simulate_mouse(app, GP_SIM_UP, X0 + x - sc.x + 0.3f, Y0 + y - sc.y + lh() / 2, 1, 0);
        settle();
        size_t a, h;
        gp_text_input_selection(app, subj, &a, &h);
        CHECK(h == off, "a click at the caret x of byte %zu of the line landed at %zu", off - ls, h - ls);
        gp_text_input_debug_select(app, subj, off, off, false);
        settle();
    }
    gp_text_input_debug_segments(app, subj, &measured);
    CHECK(measured < nseg, "clicking deep into the line measured only up to there (%zu of %zu)", measured, nseg);
    /* horizontal wheel, right then left, further than the segments that were built (each is some 15,000 px wide). Nothing focused: a
     * blinking caret would rebuild the field twice a second and hide a scroll that does not */
    gp_blur(&cx_);
    settle();
    for (int i = 0; i < 24; i++) {
        gp_simulate_wheel(app, X0 + 50, Y0 + lh() * 1.5f, i < 12 ? 60 : -60, 0, 0);
        settle();
        check_long_layout(i < 12 ? "after a horizontal wheel to the right" : "after a horizontal wheel to the left", ls, ll);
    }
    gp_focus_set(&cx_, gp_text_input_focus(app, subj));

    /* selection rectangles across segments: one span */
    {
        size_t a = ls + 5000, b = ls + 12000;
        gp_text_input_debug_select(app, subj, a, b, false);
        float rects[8][4];
        size_t nr = gp_text_input_selection_rects(app, subj, rects, 8);
        float xa = xat(subj, a), xb = xat(subj, b);
        CHECK(nr == 1 && fabsf(rects[0][0] - xa) < 0.05f && fabsf(rects[0][0] + rects[0][2] - xb) < 0.05f, "a selection across segments: %zu rects from %.1f, wanted one %.1f..%.1f", nr,
              nr ? rects[0][0] : 0, xa, xb);
    }

    /* a bare combining mark put right at segment boundaries: it joins the space that ends the segment before, so that segment is cut again */
    {
        size_t bounds[400], nb;
        char why[300];
        gp_text_input_debug_segs_check(app, subj, line_start(subj, 1) + 10, bounds, 400, &nb, why, sizeof why);
        CHECK(nb > 100, "the long line's boundaries (%zu)", nb);
        for (size_t j = 1; j < 6 && j * 17 < nb; j++) {
            gp_text_input_debug_replace(app, subj, bounds[j * 17] + 2 * (j - 1), bounds[j * 17] + 2 * (j - 1), "\xcc\x81");   /* (each puts 2 bytes in before the next) */
            segs_ok("a combining mark at a boundary", subj);
        }
    }

    /* random edits: the segments are cut again where they happen; edits in the lines around move them */
    for (int k = 0; k < 400; k++) {
        if (k % 9 == 4) gp_text_input_debug_replace(app, subj, 3, 3 + (k % 2), k % 2 ? "" : "ab");   /* (line 0) */
        else if (k % 9 == 7) { size_t l2 = line_start(subj, 2); gp_text_input_debug_replace(app, subj, l2 + 2, l2 + 2, "zz"); }
        else random_edit(subj, 1, k);
        if (k % 10 == 0) segs_ok("an edit", subj);
        if (k % 50 == 49) {
            compare_with_fresh("random edits", subj, orac, 1);
            gp_text_input_debug_select(app, subj, line_start(subj, 1) + 100, line_start(subj, 1) + 100, true);
            settle();
            check_long_layout("after edits", line_start(subj, 1), line_len(subj, 1));
            if (failures) break;
        }
    }

    /* a composition pushes the text after it */
    {
        ls = line_start(subj, 1);
        ll = line_len(subj, 1);
        t = gp_text_input_text(app, subj);
        size_t off = ls + ll / 3;
        while (t[off] != ' ') off++;
        off++;
        gp_text_input_debug_select(app, subj, off, off, true);
        settle();
        float x0 = xat(subj, off), after = xat(subj, off + 3000);
        gp_text_input_debug_select(app, subj, off, off, true);
        settle();
        app->debug_ignore_platform_ime = true;
        const char *pre = "\xe3\x81\x82\xe3\x81\x84\xe3\x81\x86";
        gp_simulate_ime(app, pre, 3);
        settle();
        float w = shaped_w(pre, strlen(pre)), x, y;
        gp_text_input_caret_pos(app, subj, &x, &y);
        CHECK(fabsf(x - (x0 + w)) < 1.f, "composing: the caret at %.1f, wanted the end of the composition %.1f", x, x0 + w);
        /* the segment after the one holding the composition: at the x its first byte had, plus the composition's width */
        gp_el *s = scroller(0), *row = NULL;
        for (uint32_t i = 0; s && i < s->nchildren; i++)
            if (s->children[i]->kind == GP_EL_DIV && s->children[i]->nchildren > 0 && s->children[i]->children[0]->kind == GP_EL_TEXT) row = s->children[i];
        bool seen = false;
        size_t next_off = 0;
        float next_x = 0;
        for (uint32_t i = 0; row && i + 1 < row->nchildren; i++)
            if (strstr(row->children[i]->text, pre)) {
                const char *at = strstr(gp_text_input_text(app, subj) + ls, row->children[i + 1]->text);
                if (at) { seen = true; next_off = (size_t)(at - gp_text_input_text(app, subj)); next_x = row->pos.x + row->children[i + 1]->pos.x; }
            }
        CHECK(seen, "composing: the segment after the composition was not laid out");
        gp_simulate_ime(app, "", 0);
        app->debug_ignore_platform_ime = false;
        settle();
        if (seen) {
            float plain = xat(subj, next_off);
            CHECK(fabsf(next_x - (plain + w)) < 0.05f + fabsf(next_x) * 2e-7f, "composing: the text after it is at %.2f, wanted %.2f + %.2f", next_x, plain, w);
        }
        (void)after;
    }

    /* a password field: one long line of bullets, edited, against a fresh one */
    gp_text_input_set_text(&cx_, pw, tok);
    for (int k = 0; k < 120; k++) random_edit(pw, 0, k);
    compare_with_fresh("password", pw, pw_orac, 0);
    {   /* and against bullets, one per character, shaped whole */
        const char *pt = gp_text_input_text(app, pw);
        size_t plen = strlen(pt);
        for (int i = 1; i <= 6; i++) {
            size_t off = plen * (size_t)i / 7, nb = 0;
            while (off > 0 && ((unsigned char)pt[off] & 0xc0) == 0x80) off--;
            for (size_t j = 0; j < off; j++) nb += ((unsigned char)pt[j] & 0xc0) != 0x80;
            char *bul = malloc(nb * 3 + 1);
            for (size_t j = 0; j < nb; j++) memcpy(bul + j * 3, "\xe2\x80\xa2", 3);
            float x = xat(pw, off), want = shaped_w(bul, nb * 3);
            CHECK(fabsf(x - want) < 0.1f + want * 2e-7f, "password: the caret after %zu characters at %.2f, %zu bullets are %.2f wide", nb, x, nb, want);
            free(bul);
        }
    }
    CHECK(gp_text_input_debug_segments(app, pw, NULL) > 50, "the password field cuts its long line into segments");
    free(doc);

    /* a long right-to-left line is not cut (bidi reorders across the whole paragraph): it is built whole */
    {
        size_t before = gp_text_input_debug_segments(app, subj, NULL), hn = 0;
        char *heb = malloc(20000);
        hn += (size_t)sprintf(heb, "x\nleft to right first, then ");   /* (a left-to-right paragraph with right-to-left runs in it) */
        while (hn < 12000) hn += (size_t)sprintf(heb + hn, "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d %d ", (int)hn);
        gp_text_input_set_text(&cx_, orac, heb);
        gp_text_input_debug_select(app, orac, 4, 4, true);
        settle();
        CHECK(gp_text_input_debug_segments(app, orac, NULL) == 0, "a long right-to-left line was cut into %zu segments", gp_text_input_debug_segments(app, orac, NULL));
        (void)before;
        free(heb);
    }

    /* ---------------- soft wrap */
    size_t l1, l2;
    char *a = tokens(0, 37500, &l1), *b = tokens(500000, 18750, &l2);
    cap = l1 + l2 + 4096;
    doc = malloc(cap);
    n = (size_t)sprintf(doc, "short\n");
    memcpy(doc + n, a, l1);
    n += l1;
    n += (size_t)sprintf(doc + n, "\nshort after\n");
    memcpy(doc + n, b, l2);
    n += l2;
    n += (size_t)sprintf(doc + n, "\nend");
    gp_text_input_set_text(&cx_, wrapped, doc);
    settle();
    size_t starts[64], nu = gp_text_input_debug_units(app, wrapped, starts, 64), lines;
    gp_text_input_debug_unwrapped(app, wrapped, &lines);
    printf("  (wrapped: %zu lines in %zu units)\n", lines, nu);
    CHECK(lines == 5 && nu >= 5 + 9 + 3 && nu <= 5 + 14 + 7, "the long lines are in pieces (%zu units for %zu lines)", nu, lines);
    verify("opened", 0);
    check_wrapped_layout("opened");

    /* around each piece boundary of the first long line: laid out right, and End stays on the piece's last row */
    for (size_t u = 2; u < nu && starts[u] < 6 + l1; u++) {
        size_t r = gp_text_input_debug_row_of(app, wrapped, starts[u], false);
        gp_text_input_debug_scroll_to(app, wrapped, (float)r * lh() - 2 * lh());
        settle();
        check_wrapped_layout("a piece boundary");
        size_t rs, re;
        gp_text_input_row_range(app, wrapped, r - 1, &rs, &re);
        CHECK(re == starts[u], "the row before piece %zu ends at %zu, the piece starts at %zu", u, re, starts[u]);
        gp_focus_set(&cx_, gp_text_input_focus(app, wrapped));
        gp_text_input_debug_select(app, wrapped, starts[u] - 3, starts[u] - 3, false);
        gp_simulate_keystrokes(app, "end");
        settle();
        size_t sa, sh;
        gp_text_input_selection(app, wrapped, &sa, &sh);
        CHECK(sh == starts[u] && gp_text_input_caret_row(app, wrapped) == r - 1, "End on the last row of a piece: caret at %zu on row %zu, wanted %zu on row %zu", sh,
              gp_text_input_caret_row(app, wrapped), starts[u], r - 1);
    }
    gp_text_input_debug_wrap_all(app, wrapped);
    char why[400];
    const char *bad = gp_text_input_debug_check(app, wrapped, why, sizeof why);
    CHECK(!bad, "wrapped, all: %s", bad ? bad : "");
    verify("all wrapped", 0);

    /* random edits in the first long line: one piece wrapped again each */
    unsigned long f0, w0, f1, w1;
    for (int k = 0; k < 300; k++) {
        gp_text_input_debug_rows(app, wrapped, &f0, &w0, why, sizeof why);
        size_t before = gp_text_input_debug_unwrapped(app, wrapped, NULL);
        random_edit(wrapped, 1, k);
        size_t after = gp_text_input_debug_unwrapped(app, wrapped, NULL);
        CHECK(after <= before + 3, "an edit in a long line left %zu units unwrapped (were %zu)", after, before);
        gp_text_input_debug_wrap_all(app, wrapped);
        gp_text_input_debug_rows(app, wrapped, &f1, &w1, why, sizeof why);
        CHECK(w1 - w0 <= 3, "an edit in a long line wrapped %lu units again", w1 - w0);
        if (k % 10 == 9) verify("random edit", k);
        if (failures) break;
    }
    bad = gp_text_input_debug_check(app, wrapped, why, sizeof why);
    CHECK(!bad, "wrapped, after edits: %s", bad ? bad : "");

    /* a bare combining mark right at piece boundaries (it joins the piece before: that piece is cut again), and 100 KB pasted into the
     * middle of a long line (the piece it lands in is cut again into pieces of the usual size) */
    {
        nu = gp_text_input_debug_units(app, wrapped, starts, 64);
        size_t ls1 = line_start(wrapped, 1), le1 = ls1 + line_len(wrapped, 1), done = 0;
        for (size_t u = 1; u < nu && done < 3; u++)
            if (starts[u] > ls1 && starts[u] < le1) {
                gp_text_input_debug_replace(app, wrapped, starts[u] + 2 * done, starts[u] + 2 * done, "\xcc\x81");
                done++;
                verify("a combining mark at a piece boundary", (int)done);
            }
        size_t pl;
        char *paste = tokens(900000, 12500, &pl);
        size_t mid = line_start(wrapped, 1) + line_len(wrapped, 1) / 2;
        t = gp_text_input_text(app, wrapped);
        while (((unsigned char)t[mid] & 0xc0) == 0x80) mid++;
        gp_text_input_debug_replace(app, wrapped, mid, mid, paste);
        verify("100 KB pasted into a long line", 0);
        free(paste);
    }

    /* structure: a line break into a long line and out again; a long line made short; a short one made long */
    {
        size_t ls1 = line_start(wrapped, 1), mid = ls1 + line_len(wrapped, 1) / 2;
        t = gp_text_input_text(app, wrapped);
        while (((unsigned char)t[mid] & 0xc0) == 0x80) mid++;
        gp_text_input_debug_replace(app, wrapped, mid, mid, "\n");
        verify("split", 0);
        gp_text_input_debug_replace(app, wrapped, mid, mid + 1, "");
        verify("joined", 0);
        size_t ls3 = line_start(wrapped, 3), ll3 = line_len(wrapped, 3);
        gp_text_input_debug_replace(app, wrapped, ls3 + 1000, ls3 + ll3, "");
        verify("made short", 0);
        gp_text_input_debug_replace(app, wrapped, 2, 2, a);
        verify("made long", 0);
        nu = gp_text_input_debug_units(app, wrapped, starts, 64);
        gp_text_input_debug_wrap_all(app, wrapped);
        bad = gp_text_input_debug_check(app, wrapped, why, sizeof why);
        CHECK(!bad, "wrapped, after the structure changed: %s", bad ? bad : "");
    }

    /* a composition right at a piece boundary: in the piece after it (the boundary is the start of that piece) */
    {
        nu = gp_text_input_debug_units(app, wrapped, starts, 64);
        size_t b = 0;
        for (size_t u = 1; u < nu; u++)
            if (starts[u] > line_start(wrapped, 1) && starts[u] < line_start(wrapped, 1) + line_len(wrapped, 1)) { b = starts[u]; break; }
        CHECK(b, "no piece boundary in line 1");
        gp_text_input_debug_select(app, wrapped, b, b, true);
        settle();
        size_t n0 = gp_text_input_rows(app, wrapped);
        app->debug_ignore_platform_ime = true;
        gp_simulate_ime(app, "composition at a boundary between two pieces of a long line, long enough to wrap", 10);
        verify("composing at a boundary", 0);
        CHECK(gp_text_input_rows(app, wrapped) > n0, "a composition at a piece boundary adds rows (%zu, were %zu)", gp_text_input_rows(app, wrapped), n0);
        gp_simulate_ime(app, "", 0);
        app->debug_ignore_platform_ime = false;
        verify("composed at a boundary", 0);
        CHECK(gp_text_input_rows(app, wrapped) == n0, "the composition at a boundary gone, the rows are back");
    }

    /* a composition in a long line */
    {
        size_t ls1 = line_start(wrapped, 1), off = ls1 + line_len(wrapped, 1) / 3;
        t = gp_text_input_text(app, wrapped);
        while (t[off] != ' ') off++;
        off++;
        gp_text_input_debug_select(app, wrapped, off, off, true);
        settle();
        size_t n0 = gp_text_input_rows(app, wrapped);
        app->debug_ignore_platform_ime = true;
        gp_simulate_ime(app, "a very long composition that takes a row of its own and then some more of the next", 20);
        verify("composing", 0);
        CHECK(gp_text_input_rows(app, wrapped) > n0, "a long composition in a long line adds rows (%zu, were %zu)", gp_text_input_rows(app, wrapped), n0);
        settle();
        gp_simulate_ime(app, "", 0);
        app->debug_ignore_platform_ime = false;
        verify("composed", 0);
        CHECK(gp_text_input_rows(app, wrapped) == n0, "the composition gone, the rows are back");
    }
    free(a);
    free(b);
    free(doc);
    free(tok);
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    run();
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    gp_text_input_options nowrap = { .multiline = true, .rows = 4, .width = 420, .font_size = FONT };
    subj = gp_text_input_new(app, &nowrap);
    orac = gp_text_input_new(app, &nowrap);
    wrapped = gp_text_input_new(app, &(gp_text_input_options){ .multiline = true, .soft_wrap = true, .rows = 6, .width = 420, .font_size = FONT });
    gp_text_input_options p = { .password = true, .width = 420, .font_size = FONT };
    pw = gp_text_input_new(app, &p);
    pw_orac = gp_text_input_new(app, &p);
    gp_entity r = gp_view_new(app, 1, &(char){ 0 }, view, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "longline_test", .width = 480, .height = 560 }, r);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "longline_test: %d FAILURE(S)\n" : "longline_test: all checks passed\n", failures);
    return failures != 0;
}
