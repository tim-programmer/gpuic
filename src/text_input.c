/*
 * Text input widget.
 *
 * The widget is a view (an entity with a render function) whose state is the text buffer,
 * selection, undo history and IME composition. It is built from ordinary elements: a
 * scroller holding one text element per line plus absolutely positioned selection / caret /
 * composition-underline elements. Library scrolling therefore provides clipping, the wheel
 * and scrollbars; this file provides everything text-specific.
 *
 * Positions: `build_line()` shapes one line and records the x of every caret boundary. Both
 * rendering (where to draw the caret and selection) and hit-testing (which offset is under
 * the mouse) use it, so what you see is what you click.
 *
 * Offsets are bytes into UTF-8, always on character boundaries. Movement and deletion go by
 * grapheme clusters, UAX #29 in full: combining marks, Hangul jamo, emoji sequences and skin tones, flags, a prepended sign, and an
 * Indic consonant joined to the next by a virama (see gp_gstate_joins in text.c).
 */
#include "internal.h"

/* The public direction enum is passed straight through as a GP_BIDI_* value. */
_Static_assert((int)GP_TEXT_DIRECTION_AUTO == (int)GP_BIDI_AUTO && (int)GP_TEXT_DIRECTION_LTR == (int)GP_BIDI_LTR &&
                   (int)GP_TEXT_DIRECTION_RTL == (int)GP_BIDI_RTL, "direction enums must match");

#define TI_MAGIC 0x74696e70u
#define BORDER 1.f
#define CARET_W 2.f
#define BLINK_SECONDS 0.53
#define COALESCE_SECONDS 1.0
#define MAX_UNDO 500

/* ---------------------------------------------------------------- state */

typedef struct {
    size_t pos;                  /* the edit replaced [pos, pos+old_len) ... */
    char *old;
    size_t old_len;
    char *new_;                  /* ... with new_ (new_len bytes) */
    size_t new_len;
    size_t before_anchor, before_head, after_anchor, after_head;
    double time;
} Edit;

/* A visual row: one line as drawn. Without soft wrap every hard line is exactly one row. */
typedef struct {
    size_t start, end;           /* bytes [start, end) of the text; end excludes the '\n' */
    uint32_t hard;               /* index of the hard line it belongs to */
    bool soft_end;               /* the row ends because the line wrapped, not at a line break */
    bool rtl;                    /* its paragraph reads right to left */
    uint8_t pre;                 /* 0: the composition (if any, and if the caret is in the row) is shown whole; 1: only bytes [pre_a, pre_b) of it are (soft wrap
                                    puts a composition into the rows like the text, so a long one wraps), possibly none */
    size_t pre_a, pre_b;
} Row;

typedef struct {
    uint32_t magic;
    gp_text_input_options o;
    gp_entity self;
    gp_focus focus;
    gp_scroll scroll;

    char *text;
    size_t len, cap;
    size_t anchor, head;         /* selection; head is the caret */
    float goal_x;                /* remembered x for up/down */
    bool has_goal;

    char *preedit;               /* IME composition, "" when none */
    int preedit_cursor;

    Edit *undo, *redo;
    size_t nundo, cap_undo, nredo, cap_redo;
    bool break_coalesce;

    bool caret_on;
    gp_task blink;

    /* Rows are cached per text revision and wrap width. The wrap width comes from layout (the
     * scroller's width, reported through gp_on_layout), so it is 0 until the first layout. */
    /* The hard lines of a multi-line field, kept up to date by every edit (an edit touches the lines it overlaps and shifts the starts after
     * them), so that a document of any size has its rows (without soft wrap a row is a line), a position's row and whether it involves
     * right-to-left text at once, and nothing is rescanned. Line i is [ls[i], ls[i+1] - 1) (the last one runs to the end of the text). */
    size_t *ls;
    uint8_t *lrtl;               /* 1 if line i has a character that may be right-to-left */
    size_t nl, cap_l, rtl_lines;
    float max_w;                 /* the widest row built so far: the scroll extent across, for a document that is not built whole */
    size_t win_first, win_n;     /* the rows the last render built */
    uint64_t rev;                /* bumped on every change to the text */
    Row *rows;
    size_t nrows, cap_rows;
    bool rows_valid;
    uint64_t rows_rev, rows_pre;   /* the revision and the composition (pre_ver) the rows were made for */
    bool pend_valid;               /* exactly one edit since the rows were made: [pend_start, pend_old_end) of the old text became pend_len bytes (see update_rows) */
    size_t pend_start, pend_old_end, pend_len;
    unsigned long rows_full, rows_incremental;   /* how the rows were brought up to date (tests) */
    uint64_t pre_ver;              /* bumped whenever the composition changes */
    float rows_w;
    float wrap_w;                /* 0 = do not wrap */
    float avail_w;               /* width of the text area as laid out; right-to-left rows align to its right edge */
    bool rtl_valid;
    uint64_t rtl_rev;
    bool has_rtl_text;           /* the text (or the forced direction) involves right-to-left */
    /* A caret at a soft-wrap boundary is the same byte offset drawn at the end of one row or at the
     * start of the next; this says which one the user means (End and clicks past the end of a row
     * choose the end of the row, everything else the start of the next). */
    bool upstream;

    int drag_mode;               /* 0 chars, 1 words, 2 lines */
    size_t drag_lo, drag_hi;     /* the initially selected word / line */
} TextInput;

static TextInput *ti_get(gp_app *app, gp_entity e) {
    TextInput *ti = gp_entity_get(app, e);
    return ti && ti->magic == TI_MAGIC ? ti : NULL;
}

static float line_height(const TextInput *ti) { return ti->o.font_size * GP_LINE_HEIGHT; }
static size_t sel_lo(const TextInput *ti) { return ti->anchor < ti->head ? ti->anchor : ti->head; }
static size_t sel_hi(const TextInput *ti) { return ti->anchor < ti->head ? ti->head : ti->anchor; }
static bool has_sel(const TextInput *ti) { return ti->anchor != ti->head; }
static bool composing(const TextInput *ti) { return ti->preedit[0] != 0; }

static void set_preedit(TextInput *ti, const char *s, int cursor) {
    free(ti->preedit);
    ti->preedit = strdup(s);
    ti->preedit_cursor = cursor < 0 ? 0 : cursor;
    ti->pre_ver++;
}

/* ---------------------------------------------------------------- UTF-8 */

static uint32_t cp_decode(const char *s, size_t len, size_t i, size_t *n) {
    const unsigned char *p = (const unsigned char *)s + i;
    uint32_t cp;
    size_t k;
    if (p[0] < 0x80) { cp = p[0]; k = 1; }
    else if ((p[0] & 0xe0) == 0xc0) { cp = p[0] & 0x1f; k = 2; }
    else if ((p[0] & 0xf0) == 0xe0) { cp = p[0] & 0x0f; k = 3; }
    else if ((p[0] & 0xf8) == 0xf0) { cp = p[0] & 0x07; k = 4; }
    else { *n = 1; return 0xfffd; }
    if (i + k > len) { *n = 1; return 0xfffd; }
    for (size_t j = 1; j < k; j++) {
        if ((p[j] & 0xc0) != 0x80) { *n = 1; return 0xfffd; }
        cp = (cp << 6) | (p[j] & 0x3f);
    }
    *n = k;
    return cp;
}

/* Caret stops are grapheme clusters (UAX #29 with the Indic conjunct rule: Hangul jamo, flags, emoji sequences, combining marks, a
 * consonant joined to the next by a virama). The rules need context before a position, so going back starts at a codepoint that
 * is certainly a boundary and goes forward from there with the same state machine that char_next and build_line use. */
static size_t step_back_one(const char *s, size_t i) {
    do i--; while (i > 0 && ((unsigned char)s[i] & 0xc0) == 0x80);
    return i;
}

/* Is a cluster certain to start here, whatever comes before it? Not a codepoint that can join what precedes it, and not after a Prepend. */
static bool certain_start(const TextInput *ti, size_t i) {
    if (i == 0) return true;
    size_t n;
    uint32_t cp = cp_decode(ti->text, ti->len, i, &n);
    unsigned unsafe = GP_GB_EXTEND | GP_GB_ZWJ | GP_GB_SPACING | GP_GB_RI | GP_GB_L | GP_GB_V | GP_GB_T | GP_GB_LV | GP_GB_LVT | GP_GB_EXTPICT |
                      GP_GB_CONSONANT | GP_GB_LINKER;
    if (gp_grapheme_props(cp) & unsafe) return false;
    size_t pi = step_back_one(ti->text, i);
    return !(gp_grapheme_props(cp_decode(ti->text, ti->len, pi, &n)) & GP_GB_PREPEND);
}

static size_t char_prev(const TextInput *ti, size_t i) {
    if (i == 0) return 0;
    size_t s = step_back_one(ti->text, i), back = 0;
    while (s > 0 && !certain_start(ti, s) && back++ < 512) s = step_back_one(ti->text, s);
    size_t n, last = s, p = s;
    gp_gstate g;
    gp_gstate_start(&g, cp_decode(ti->text, ti->len, p, &n));
    p += n;
    while (p < i) {
        uint32_t cp = cp_decode(ti->text, ti->len, p, &n);
        if (!gp_gstate_joins(&g, cp)) last = p;
        p += n;
    }
    return last;
}

static size_t char_next(const TextInput *ti, size_t i) {
    if (i >= ti->len) return ti->len;
    size_t n;
    gp_gstate g;
    gp_gstate_start(&g, cp_decode(ti->text, ti->len, i, &n));
    i += n;
    while (i < ti->len) {
        uint32_t cp = cp_decode(ti->text, ti->len, i, &n);
        if (!gp_gstate_joins(&g, cp)) break;
        i += n;
    }
    return i;
}

/* 0 = space, 1 = word, 2 = punctuation */
static int cp_class(uint32_t cp) {
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == 0xa0 || cp == 0x3000) return 0;
    if ((cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
        cp == '_' || cp >= 0x80)
        return 1;
    return 2;
}

static int class_before(const TextInput *ti, size_t i) {
    size_t p = char_prev(ti, i), n;
    return cp_class(cp_decode(ti->text, ti->len, p, &n));
}

static int class_at(const TextInput *ti, size_t i) {
    size_t n;
    return cp_class(cp_decode(ti->text, ti->len, i, &n));
}

static size_t word_left(const TextInput *ti, size_t i) {
    while (i > 0 && class_before(ti, i) == 0) i = char_prev(ti, i);
    if (i == 0) return 0;
    int c = class_before(ti, i);
    while (i > 0 && class_before(ti, i) == c) i = char_prev(ti, i);
    return i;
}

static size_t word_right(const TextInput *ti, size_t i) {
    while (i < ti->len && class_at(ti, i) == 0) i = char_next(ti, i);
    if (i >= ti->len) return ti->len;
    int c = class_at(ti, i);
    while (i < ti->len && class_at(ti, i) == c) i = char_next(ti, i);
    return i;
}

static void word_bounds(const TextInput *ti, size_t off, size_t *lo, size_t *hi) {
    if (ti->len == 0) { *lo = *hi = 0; return; }
    /* a click right after the last character belongs to that character */
    size_t at = off >= ti->len ? char_prev(ti, ti->len) : off;
    int c = class_at(ti, at);
    size_t a = at, b = char_next(ti, at);
    while (a > 0 && class_before(ti, a) == c) a = char_prev(ti, a);
    while (b < ti->len && class_at(ti, b) == c) b = char_next(ti, b);
    *lo = a;
    *hi = b;
}

/* ---------------------------------------------------------------- lines */

static size_t line_start_of(const TextInput *ti, size_t off) {
    while (off > 0 && ti->text[off - 1] != '\n') off--;
    return off;
}

static size_t line_end_of(const TextInput *ti, size_t off) {
    while (off < ti->len && ti->text[off] != '\n') off++;
    return off;
}

/* A shaped line, i.e. one visual row.
 *
 * Its caret stops are cells. Stop k < n-1 owns cell k: the box (xl..xr) of the character that starts
 * at byte[k], and that character's direction. The last stop is the end of the line and has no cell.
 * In left-to-right text the cells tile the line in order; in bidirectional text they are scattered
 * (a Hebrew word's cells run right to left inside a left-to-right sentence), which is why the
 * widget works from cells and never assumes that logical order is visual order.
 *
 * A grapheme inside a multi-character glyph (a ligature, an emoji sequence, lam-alef) gets an equal
 * share of the glyph's box, in the glyph's own direction. All x are content coordinates: they
 * include row_off, the shift that right-aligns right-to-left paragraphs. */
typedef struct {
    size_t start, end;           /* real byte range */
    const char *disp;            /* what is drawn: password bullets, IME text spliced in */
    size_t disp_len;
    size_t n;                    /* stops: n-1 cells, then the end of the line */
    size_t *byte;                /* real byte offset of stop k (byte[n-1] is the end) */
    float *x;                    /* caret x of stop k: the leading edge of its cell; for the end stop,
                                    the trailing edge of the last cell */
    float *xl, *xr;              /* visual box of cell k */
    bool *rtl;                   /* direction of cell k */
    float width;                 /* shaped width of the text */
    float row_off;
    bool para_rtl, has_rtl;
    bool composing;
    float pre_x0, pre_x1;        /* extent of the composition, if composing */
    float pre_cursor_x;          /* the caret inside the composition */
    uint32_t *cpf, *cpl;         /* the first and the last codepoint of cell k (what line breaking looks at; the text's, or the composition's) */
    size_t *pbyte;               /* for a cell of the composition, its byte in it (else SIZE_MAX) */
} Line;

typedef struct { size_t cs; float x0, x1; int level; } ClusterBox;

static int cluster_cmp(const void *pa, const void *pb) {
    size_t a = ((const ClusterBox *)pa)->cs, b = ((const ClusterBox *)pb)->cs;
    return a < b ? -1 : a > b;
}

/* Shapes the real bytes [ls, le) with bytes [pa, pb) of the composition spliced in at the caret (none if pb <= pa or the caret is elsewhere). */
static void build_line_ex(TextInput *ti, gp_text_system *ts, size_t ls, size_t le, size_t pa, size_t pb,
                          bool para_rtl, gp_arena *a, Line *L) {
    bool comp = pb > pa && composing(ti) && ti->head >= ls && ti->head <= le;
    size_t rn = le - ls, pn = comp ? pb - pa : 0;
    size_t splice = comp ? ti->head - ls : rn + 1;

    size_t cap_cp = rn + pn + 2;
    char *disp = gp_arena_alloc(a, rn * 3 + pn + 8, 1);
    size_t *cp_byte = gp_arena_alloc(a, cap_cp * sizeof *cp_byte, 8);   /* real offset of each display codepoint */
    size_t *disp_off = gp_arena_alloc(a, (cap_cp + 1) * sizeof *disp_off, 8); /* its offset in `disp` */
    bool *cp_ext = gp_arena_alloc(a, cap_cp * sizeof *cp_ext, 1);
    uint32_t *cp_val = gp_arena_alloc(a, cap_cp * sizeof *cp_val, 4);   /* the codepoint of each display codepoint */
    size_t *cp_pre = gp_arena_alloc(a, cap_cp * sizeof *cp_pre, 8);     /* its byte in the composition, or SIZE_MAX */
    size_t dl = 0, ncp = 0, pre0 = 0, pre1 = 0;

    /* Caret stops follow the same cluster rules as char_next / char_prev, or the mouse could put
     * the caret where the keyboard never can: no stop inside a ZWJ sequence or a flag. */
    gp_gstate gs = { 0 };
    bool started = false;   /* the first codepoint of a line starts a cluster */
    size_t i = 0;
    bool spliced = false;
    for (;;) {
        if (comp && !spliced && i == splice) {
            spliced = true;
            pre0 = ncp;
            for (size_t k = 0; k < pn;) {
                size_t n;
                uint32_t pc = cp_decode(ti->preedit + pa, pn, k, &n);
                memcpy(disp + dl, ti->preedit + pa + k, n);
                disp_off[ncp] = dl;
                dl += n;
                cp_byte[ncp] = ls + splice;
                cp_val[ncp] = pc;
                cp_pre[ncp] = pa + k;
                cp_ext[ncp++] = false;
                k += n;
            }
            pre1 = ncp;
        }
        if (i >= rn) break;
        size_t n;
        uint32_t cp = cp_decode(ti->text + ls, rn, i, &n);
        disp_off[ncp] = dl;
        if (ti->o.password) {
            memcpy(disp + dl, "\xe2\x80\xa2", 3); /* U+2022 BULLET */
            dl += 3;
        } else {
            memcpy(disp + dl, ti->text + ls + i, n);
            dl += n;
        }
        cp_byte[ncp] = ls + i;
        cp_val[ncp] = cp;
        cp_pre[ncp] = SIZE_MAX;
        if (!started) { gp_gstate_start(&gs, cp); cp_ext[ncp++] = false; started = true; }
        else cp_ext[ncp++] = gp_gstate_joins(&gs, cp);
        i += n;
    }
    disp[dl] = 0;
    disp_off[ncp] = dl;

    /* Shape the whole line in the paragraph's direction. Glyphs come back in VISUAL order, each
     * with the logical byte offset of its cluster: that is all the information needed. */
    int gcap = (int)ncp + 1;
    gp_glyph_pos *g = gp_arena_alloc(a, (size_t)gcap * sizeof *g, 8);
    float width = 0;
    int pdir = para_rtl ? GP_BIDI_RTL : GP_BIDI_LTR;
    int ng = gp_text_shape_dir(ts, 400, ti->o.font_size, disp, dl, pdir, g, gcap, &width);
    if (ng > gcap) {
        gcap = ng;
        g = gp_arena_alloc(a, (size_t)gcap * sizeof *g, 8);
        ng = gp_text_shape_dir(ts, 400, ti->o.font_size, disp, dl, pdir, g, gcap, &width);
    }

    /* One box per cluster (the glyphs sharing a cluster value), then in logical order. */
    ClusterBox *cb = gp_arena_alloc(a, ((size_t)ng + 1) * sizeof *cb, 8);
    size_t ncb = 0;
    for (int gi = 0; gi < ng;) {
        int gj = gi;
        float x0 = g[gi].x, x1 = g[gi].x + g[gi].advance;
        while (gj + 1 < ng && g[gj + 1].cluster == g[gi].cluster) {
            gj++;
            if (g[gj].x < x0) x0 = g[gj].x;
            if (g[gj].x + g[gj].advance > x1) x1 = g[gj].x + g[gj].advance;
        }
        cb[ncb++] = (ClusterBox){ g[gi].cluster, x0, x1, g[gi].level };
        gi = gj + 1;
    }
    qsort(cb, ncb, sizeof *cb, cluster_cmp);
    size_t merged = 0; /* the same cluster seen twice (it can be split by reordering): one box */
    for (size_t c = 0; c < ncb; c++) {
        if (merged && cb[merged - 1].cs == cb[c].cs) {
            if (cb[c].x0 < cb[merged - 1].x0) cb[merged - 1].x0 = cb[c].x0;
            if (cb[c].x1 > cb[merged - 1].x1) cb[merged - 1].x1 = cb[c].x1;
        } else {
            cb[merged++] = cb[c];
        }
    }
    ncb = merged;

    /* Cells: split each cluster's box evenly among the stops inside it, in the cluster's direction. */
    float *cxl = gp_arena_alloc(a, (ncp + 1) * sizeof *cxl, 4), *cxr = gp_arena_alloc(a, (ncp + 1) * sizeof *cxr, 4);
    bool *crtl = gp_arena_alloc(a, (ncp + 1) * sizeof *crtl, 1), *cset = gp_arena_alloc(a, (ncp + 1) * sizeof *cset, 1);
#define IS_STOP(j) ((j) == 0 || !cp_ext[j])
    size_t k = 0;
    for (size_t c = 0; c < ncb; c++) {
        size_t ce = c + 1 < ncb ? cb[c + 1].cs : dl;
        size_t first = k;
        while (first < ncp && disp_off[first] < cb[c].cs) first++;
        k = first;
        while (k < ncp && disp_off[k] < ce) k++;
        size_t m = 0;
        for (size_t j = first; j < k; j++) m += IS_STOP(j);
        if (!m) continue;
        float w = cb[c].x1 - cb[c].x0, part = w / (float)m;
        bool r = cb[c].level & 1;
        size_t idx = 0;
        for (size_t j = first; j < k; j++) {
            if (!IS_STOP(j)) continue;
            float left = r ? cb[c].x1 - (float)(idx + 1) * part : cb[c].x0 + (float)idx * part;
            cxl[j] = left;
            cxr[j] = left + part;
            crtl[j] = r;
            cset[j] = true;
            idx++;
        }
    }
    float prev_right = 0;
    size_t nstops = 0;
    bool any_rtl = para_rtl;
    for (size_t j = 0; j < ncp; j++) { /* characters the font drew nothing for: zero-width cells in place */
        if (!IS_STOP(j)) continue;
        if (!cset[j]) { cxl[j] = cxr[j] = prev_right; crtl[j] = false; }
        prev_right = cxr[j];
        any_rtl |= crtl[j];
        nstops++;
    }

    float row_off = para_rtl && ti->avail_w > width ? ti->avail_w - width : 0;
    L->start = ls;
    L->end = le;
    L->disp = disp;
    L->disp_len = dl;
    L->width = width;
    L->row_off = row_off;
    L->para_rtl = para_rtl;
    L->has_rtl = any_rtl;
    L->composing = comp;
    L->n = nstops + 1;
    L->byte = gp_arena_alloc(a, L->n * sizeof(size_t), 8);
    L->cpf = gp_arena_alloc(a, L->n * sizeof(uint32_t), 4);
    L->cpl = gp_arena_alloc(a, L->n * sizeof(uint32_t), 4);
    L->pbyte = gp_arena_alloc(a, L->n * sizeof(size_t), 8);
    L->x = gp_arena_alloc(a, L->n * sizeof(float), 4);
    L->xl = gp_arena_alloc(a, L->n * sizeof(float), 4);
    L->xr = gp_arena_alloc(a, L->n * sizeof(float), 4);
    L->rtl = gp_arena_alloc(a, L->n * sizeof(bool), 1);
    size_t s = 0;
    for (size_t j = 0; j < ncp; j++) {
        if (!IS_STOP(j)) continue;
        L->byte[s] = cp_byte[j];
        L->cpf[s] = cp_val[j];
        size_t jl = j + 1;   /* the cluster goes on over the codepoints that join it */
        while (jl < ncp && !IS_STOP(jl)) jl++;
        L->cpl[s] = cp_val[jl - 1];
        L->pbyte[s] = cp_pre[j];
        L->xl[s] = cxl[j] + row_off;
        L->xr[s] = cxr[j] + row_off;
        L->rtl[s] = crtl[j];
        L->x[s] = L->rtl[s] ? L->xr[s] : L->xl[s];
        s++;
    }
    L->byte[nstops] = le;
    L->cpf[nstops] = L->cpl[nstops] = 0;
    L->pbyte[nstops] = SIZE_MAX;
    L->x[nstops] = nstops ? (L->rtl[nstops - 1] ? L->xl[nstops - 1] : L->xr[nstops - 1]) : row_off;

    L->pre_x0 = L->pre_x1 = L->pre_cursor_x = 0;
    if (comp && pre1 > pre0) {
        L->pre_x0 = cxl[pre0] + row_off;
        L->pre_x1 = cxr[pre0] + row_off;
        for (size_t j = pre0; j < pre1; j++) {
            if (cxl[j] + row_off < L->pre_x0) L->pre_x0 = cxl[j] + row_off;
            if (cxr[j] + row_off > L->pre_x1) L->pre_x1 = cxr[j] + row_off;
        }
        size_t c = (size_t)(ti->preedit_cursor < 0 ? 0 : ti->preedit_cursor), sc = 0;   /* the cursor, and the characters of the composition before this slice */
        for (size_t k = 0; k < pa;) { size_t n; cp_decode(ti->preedit, strlen(ti->preedit), k, &n); k += n; sc++; }
        size_t mine = pre1 - pre0;
        if (c < sc) c = 0; else c -= sc;
        if (c < mine) L->pre_cursor_x = (crtl[pre0 + c] ? cxr[pre0 + c] : cxl[pre0 + c]) + row_off;
        else L->pre_cursor_x = (crtl[pre1 - 1] ? cxl[pre1 - 1] : cxr[pre1 - 1]) + row_off;
    }
#undef IS_STOP
}

/* The line [ls, le) with the whole composition in it where the caret is (if `with_preedit`). */
static void build_line(TextInput *ti, gp_text_system *ts, size_t ls, size_t le, bool with_preedit,
                       bool para_rtl, gp_arena *a, Line *L) {
    bool comp = with_preedit && composing(ti) && ti->head >= ls && ti->head <= le;
    build_line_ex(ti, ts, ls, le, 0, comp ? strlen(ti->preedit) : 0, para_rtl, a, L);
}

/* A row as drawn: with the share of the composition that soft wrap gave it (`with_preedit` false: without). */
static void build_row(TextInput *ti, gp_text_system *ts, const Row *row, bool with_preedit, gp_arena *a, Line *L) {
    if (row->pre == 1) build_line_ex(ti, ts, row->start, row->end, with_preedit ? row->pre_a : 0, with_preedit ? row->pre_b : 0, row->rtl, a, L);
    else build_line(ti, ts, row->start, row->end, with_preedit, row->rtl, a, L);
}

/* Where a caret at byte `off` is drawn. A boundary between two characters has up to two places (the
 * end of the one before, the start of the one after) which differ where the direction changes;
 * `upstream` picks the trailing edge of the character before, otherwise the leading edge of the
 * one after. In a single direction both are the same point. */
static float caret_x(const Line *L, size_t off, bool upstream) {
    size_t m = L->n - 1;
    if (m == 0) return L->x[0];
    int kn = -1, kp = -1;
    for (size_t k = 0; k < m; k++) {
        if (L->byte[k] == off && kn < 0) kn = (int)k;
        if (L->byte[k + 1] == off) kp = (int)k;
    }
    if (upstream && kp >= 0) return L->rtl[kp] ? L->xl[kp] : L->xr[kp];
    if (kn >= 0) return L->rtl[kn] ? L->xr[kn] : L->xl[kn];
    if (kp >= 0) return L->rtl[kp] ? L->xl[kp] : L->xr[kp];
    return L->x[L->n - 1];
}

/* The nearest cell edge to x: the offset at it, and whether it is the trailing edge of the
 * character before (so the caret should be drawn after that character). The nearest edge, not
 * the nearest boundary, is what makes a click on the right half of a Hebrew letter land before it. */
static size_t offset_in_line(const Line *L, float x, bool *upstream) {
    size_t m = L->n - 1;
    bool up = false;
    size_t best_off = L->byte[0];
    if (m > 0) {
        float bd = 1e30f;
        for (size_t k = 0; k < m; k++) {
            float lead = L->rtl[k] ? L->xr[k] : L->xl[k], trail = L->rtl[k] ? L->xl[k] : L->xr[k];
            if (fabsf(x - lead) < bd) { bd = fabsf(x - lead); best_off = L->byte[k]; up = false; }
            if (fabsf(x - trail) < bd) { bd = fabsf(x - trail); best_off = L->byte[k + 1]; up = true; }
        }
    }
    if (upstream) /* without right-to-left text only the end of the row can be reached from the
                     other side, and that is the only place the distinction is visible */
        *upstream = L->has_rtl ? up : (up && best_off == L->end);
    return best_off;
}

/* A logical selection is a set of cells, which in bidirectional text can be visually separate (one
 * on each side of an embedded run). Returns the merged horizontal spans, left to right. The line
 * break after a paragraph's last row, when selected, is a stub at the paragraph's end side. */
typedef struct { float x0, x1; } Span;

static int span_cmp(const void *pa, const void *pb) {
    float a = ((const Span *)pa)->x0, b = ((const Span *)pb)->x0;
    return a < b ? -1 : a > b;
}

static size_t selection_spans(const Line *L, size_t lo, size_t hi, float stub, Span *out) {
    size_t n = 0;
    for (size_t k = 0; k + 1 < L->n; k++)
        if (L->byte[k] >= lo && L->byte[k + 1] <= hi && L->byte[k] < L->byte[k + 1])
            out[n++] = (Span){ L->xl[k], L->xr[k] };
    if (stub > 0) {
        if (L->para_rtl) out[n++] = (Span){ L->row_off - stub, L->row_off };
        else out[n++] = (Span){ L->row_off + L->width, L->row_off + L->width + stub };
    }
    qsort(out, n, sizeof *out, span_cmp);
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        if (m && out[i].x0 <= out[m - 1].x1 + 0.75f) {
            if (out[i].x1 > out[m - 1].x1) out[m - 1].x1 = out[i].x1;
        } else {
            out[m++] = out[i];
        }
    }
    return m;
}

/* Does anything here read right to left? Without it, every bidi code path is skipped and the widget
 * behaves exactly as a plain left-to-right editor. Cached per text revision. */
static bool text_has_rtl(TextInput *ti) {
    if (ti->o.direction == GP_TEXT_DIRECTION_RTL) return true;
    if (ti->o.multiline) return ti->rtl_lines > 0;   /* kept by the line index */
    if (!ti->rtl_valid || ti->rtl_rev != ti->rev) {
        bool any = false;
        for (size_t i = 0; i < ti->len && !any;) {
            size_t n;
            any = gp_text_may_be_rtl(cp_decode(ti->text, ti->len, i, &n));
            i += n;
        }
        ti->has_rtl_text = any;
        ti->rtl_valid = true;
        ti->rtl_rev = ti->rev;
    }
    return ti->has_rtl_text;
}

/* ---------------------------------------------------------------- the line index */

static bool span_has_rtl(const char *s, size_t len) {
    for (size_t i = 0; i < len;) {
        if ((unsigned char)s[i] < 0x80) { i++; continue; }
        size_t n;
        if (gp_text_may_be_rtl(cp_decode(s, len, i, &n))) return true;
        i += n;
    }
    return false;
}

static size_t line_end(const TextInput *ti, size_t i) { return i + 1 < ti->nl ? ti->ls[i + 1] - 1 : ti->len; }

/* The line holding offset `off`: the last one that starts at or before it. */
static size_t line_of(const TextInput *ti, size_t off) {
    size_t lo = 0, hi = ti->nl;   /* the answer is in [lo, hi) */
    while (hi - lo > 1) {
        size_t mid = lo + (hi - lo) / 2;
        if (ti->ls[mid] <= off) lo = mid; else hi = mid;
    }
    return lo;
}

static void lines_reserve(TextInput *ti, size_t need) {
    if (need <= ti->cap_l) return;
    size_t c = ti->cap_l ? ti->cap_l : 64;
    while (c < need) c *= 2;
    ti->ls = realloc(ti->ls, c * sizeof *ti->ls);
    ti->lrtl = realloc(ti->lrtl, c);
    if (!ti->ls || !ti->lrtl) abort();
    ti->cap_l = c;
}

/* Brings the index up to date after the text changed: [start, end) of the old text became `ilen` bytes (the text is already the new one).
 * The lines the edit overlapped are scanned again and the starts after them move by the change in length. */
static void lines_replace(TextInput *ti, size_t start, size_t end, size_t ilen) {
    if (!ti->o.multiline) return;
    if (!ti->ls) {   /* the first edit: an empty text has one empty line */
        lines_reserve(ti, 1);
        ti->ls[0] = 0;
        ti->lrtl[0] = 0;
        ti->nl = 1;
    }
    long delta = (long)ilen - (long)(end - start);
    size_t a = line_of(ti, start), b = line_of(ti, end);          /* the old index: both are in the old text */
    size_t old_end_b = b + 1 < ti->nl ? ti->ls[b + 1] - 1 : (size_t)((long)ti->len - delta);
    size_t region_lo = ti->ls[a], region_hi = (size_t)((long)old_end_b + delta);   /* the same stretch of the new text */
    size_t m = 1;
    for (const char *q = ti->text + region_lo, *e = ti->text + region_hi; (q = memchr(q, '\n', (size_t)(e - q))); q++) m++;
    size_t removed = b - a + 1;
    size_t tail = ti->nl - (b + 1);
    for (size_t i = a; i <= b; i++) ti->rtl_lines -= ti->lrtl[i];
    lines_reserve(ti, ti->nl - removed + m);
    memmove(ti->ls + a + m, ti->ls + b + 1, tail * sizeof *ti->ls);
    memmove(ti->lrtl + a + m, ti->lrtl + b + 1, tail);
    if (delta) for (size_t i = a + m; i < a + m + tail; i++) ti->ls[i] = (size_t)((long)ti->ls[i] + delta);
    ti->nl = ti->nl - removed + m;
    size_t k = a, lo = region_lo;
    for (;;) {
        const char *nl = memchr(ti->text + lo, '\n', region_hi - lo);
        size_t hi = nl ? (size_t)(nl - ti->text) : region_hi;
        ti->ls[k] = lo;
        ti->lrtl[k] = span_has_rtl(ti->text + lo, hi - lo);
        ti->rtl_lines += ti->lrtl[k];
        k++;
        if (!nl) break;
        lo = hi + 1;
    }
}

/* ---------------------------------------------------------------- rows (soft wrap) */

static bool is_space_cp(uint32_t cp) { return cp == ' ' || cp == '\t'; }

/* May a row end at caret stop `k` of the shaped hard line L? After a run of spaces (which then
 * hang past the edge instead of forcing a break), after a hyphen before a word, and between CJK
 * characters (with kinsoku, see gp_cjk_break_allowed). Looks at the line's own codepoints, so a
 * composition in it breaks like the text. */
static bool can_break_at(const Line *L, size_t k) {
    uint32_t before = L->cpl[k - 1], after = k < L->n - 1 ? L->cpf[k] : 0;
    if (!after) return false;
    if (before == ' ' || before == '\t') return after != ' ' && after != '\t';
    if ((before == '-' || before == 0x2010 || before == 0x2013 || before == 0x2014) && cp_class(after) == 1)
        return true;
    return gp_cjk_break_allowed(before, after);
}

static void rows_push(TextInput *ti, Row r) {
    if (ti->nrows == ti->cap_rows) {
        ti->cap_rows = ti->cap_rows ? ti->cap_rows * 2 : 16;
        ti->rows = realloc(ti->rows, ti->cap_rows * sizeof *ti->rows);
        if (!ti->rows) abort();
    }
    ti->rows[ti->nrows++] = r;
}

/* A row of the stops [k0, k1) of L. When the line holds the composition, the row says which bytes of it it shows (none, if none of its stops are the composition's). */
static Row make_row(const TextInput *ti, const Line *L, size_t k0, size_t k1, uint32_t hard, bool soft, bool rtl) {
    Row r = { L->byte[k0], L->byte[k1], hard, soft, rtl, 1, 0, 0 };
    if (L->composing) {
        size_t lo = SIZE_MAX, hi = 0;
        for (size_t k = k0; k < k1; k++) {
            if (L->pbyte[k] == SIZE_MAX) continue;
            size_t n;
            cp_decode(ti->preedit, strlen(ti->preedit), L->pbyte[k], &n);
            if (L->pbyte[k] < lo) lo = L->pbyte[k];
            if (L->pbyte[k] + n > hi) hi = L->pbyte[k] + n;
        }
        if (lo != SIZE_MAX) { r.pre_a = lo; r.pre_b = hi; }
    }
    return r;
}

/* Greedy line breaking of one shaped hard line into rows no wider than W. A row that would
 * overflow ends at the last break opportunity; a word too long for any row is broken between
 * characters; spaces never cause a break (they hang past the edge). */
static void wrap_line(TextInput *ti, const Line *L, float W, uint32_t hard, bool rtl, gp_arena *a) {
    const float eps = 0.01f;
    size_t n = L->n - 1; /* stops are 0..n: n clusters */
    /* Width of a logical prefix. Wrapping is decided in LOGICAL order (a row is a run of consecutive
     * characters), so widths come from summing cells, never from differences of visual x. */
    float *cum = gp_arena_alloc(a, (n + 1) * sizeof *cum, 4);
    for (size_t k = 0; k < n; k++) cum[k + 1] = cum[k] + (L->xr[k] - L->xl[k]);
    size_t row_k = 0, last_ok = 0;
    for (size_t k = 1; k <= n; k++) {
        float width = cum[k] - cum[row_k];
        bool hanging = is_space_cp(L->cpf[k - 1]);
        if (width > W + eps && !hanging) {
            size_t brk;
            if (last_ok > row_k) brk = last_ok;          /* back to the last place a break was allowed */
            else if (k - 1 > row_k) brk = k - 1;         /* one long word: break before this character */
            else brk = k;                                /* a single character wider than the row */
            rows_push(ti, make_row(ti, L, row_k, brk, hard, true, rtl));
            row_k = brk;
            last_ok = 0;
            k = brk; /* rescan from the new row's start */
            continue;
        }
        if (k < n && can_break_at(L, k)) last_ok = k;
    }
    rows_push(ti, make_row(ti, L, row_k, n, hard, false, rtl));
}

/* The rows of one hard line [ls, le), appended to ti->rows. */
static void rows_for_line(TextInput *ti, gp_text_system *ts, uint32_t hard, size_t ls, size_t le, float W, gp_arena *tmp) {
    /* the paragraph's direction, decided once for all the rows it wraps into */
    bool rtl = gp_text_is_rtl(ti->text + ls, le - ls, (int)ti->o.direction);
    if (W <= 0 || (le == ls && !(composing(ti) && ti->head == ls))) {   /* (an empty line has a row, unless a composition is in it) */
        rows_push(ti, (Row){ ls, le, hard, false, rtl, 0, 0, 0 });
    } else {
        Line L;
        build_line(ti, ts, ls, le, true, rtl, tmp, &L);   /* (with the composition, if it is in this line: it wraps like the text) */
        wrap_line(ti, &L, W, hard, rtl, tmp);
        gp_arena_reset(tmp);
    }
}

static void compute_rows(TextInput *ti, gp_text_system *ts) {
    ti->nrows = 0;
    float W = ti->o.multiline && ti->o.soft_wrap ? ti->wrap_w : 0;
    gp_arena tmp = { .chunk_size = 32768 };
    uint32_t hard = 0;
    size_t ls = 0;
    for (;;) {
        size_t le = ti->o.multiline ? line_end_of(ti, ls) : ti->len;
        rows_for_line(ti, ts, hard, ls, le, W, &tmp);
        if (!ti->o.multiline || le >= ti->len) break;
        ls = le + 1;
        hard++;
    }
    gp_arena_free(&tmp);
}

/* Soft wrap needs every row (a row's start depends on the ones before it); without it a row is a hard line and is found in the index. */
static bool eager_rows(const TextInput *ti) { return ti->o.multiline && ti->o.soft_wrap; }

/* One edit since the rows were made: only the hard lines it touched are wrapped again, the rows of the others are moved (their bytes by the change in length, their line
 * numbers by the change in the number of lines). A typed character in a big wrapped document costs a line's shaping and a pass over the rows instead of all the shaping. */
static bool update_rows(TextInput *ti, gp_text_system *ts, float W) {
    size_t start = ti->pend_start, old_end = ti->pend_old_end, ilen = ti->pend_len;
    long delta = (long)ilen - (long)(old_end - start);
    if (!ti->nrows || !ti->nl) return false;
    /* the old rows of the hard lines the edit touched: [r0, r1] (the rows are in the old text's numbering until they are moved) */
    size_t lo = 0, hi = ti->nrows;   /* the last row that starts at or before `start` */
    while (hi - lo > 1) { size_t mid = lo + (hi - lo) / 2; if (ti->rows[mid].start <= start) lo = mid; else hi = mid; }
    size_t r0 = lo;
    uint32_t a = ti->rows[r0].hard;
    while (r0 > 0 && ti->rows[r0 - 1].hard == a) r0--;
    lo = 0; hi = ti->nrows;
    while (hi - lo > 1) { size_t mid = lo + (hi - lo) / 2; if (ti->rows[mid].start <= old_end) lo = mid; else hi = mid; }
    size_t r1 = lo;
    uint32_t b_old = ti->rows[r1].hard;
    while (r1 + 1 < ti->nrows && ti->rows[r1 + 1].hard == b_old) r1++;
    if (b_old < a) return false;
    size_t b_new = line_of(ti, start + ilen);   /* the new index is already made: the line holding the end of what was put in */
    if (b_new < a) return false;
    long shift = (long)(b_new - a + 1) - (long)(b_old - a + 1);

    /* the new rows of the new lines a..b_new */
    Row *keep = ti->rows;
    size_t keep_n = ti->nrows, keep_cap = ti->cap_rows;
    ti->rows = NULL;
    ti->nrows = ti->cap_rows = 0;
    gp_arena tmp = { .chunk_size = 32768 };
    for (size_t h = a; h <= b_new; h++) rows_for_line(ti, ts, (uint32_t)h, ti->ls[h], line_end(ti, h), W, &tmp);
    gp_arena_free(&tmp);
    Row *fresh = ti->rows;
    size_t fresh_n = ti->nrows;

    size_t tail = keep_n - (r1 + 1);
    size_t total = r0 + fresh_n + tail;
    if (total > keep_cap) {
        keep_cap = total * 2;
        keep = realloc(keep, keep_cap * sizeof *keep);
        if (!keep) abort();
    }
    memmove(keep + r0 + fresh_n, keep + r1 + 1, tail * sizeof *keep);
    memcpy(keep + r0, fresh, fresh_n * sizeof *keep);
    for (size_t i = r0 + fresh_n; i < total; i++) {
        keep[i].start = (size_t)((long)keep[i].start + delta);
        keep[i].end = (size_t)((long)keep[i].end + delta);
        keep[i].hard = (uint32_t)((long)keep[i].hard + shift);
    }
    free(fresh);
    ti->rows = keep;
    ti->nrows = total;
    ti->cap_rows = keep_cap;
    return true;
}

static void rows_ready(TextInput *ti, gp_text_system *ts) {
    float W = ti->o.multiline && ti->o.soft_wrap ? ti->wrap_w : 0;
    if (ti->rows_valid && ti->pend_valid && ti->rows_rev + 1 == ti->rev && ti->rows_w == W && ti->rows_pre == ti->pre_ver && W > 0) {
        bool done = update_rows(ti, ts, W);
        ti->pend_valid = false;
        if (done) {
            ti->rows_rev = ti->rev;
            ti->rows_incremental++;
            return;
        }
    }
    ti->pend_valid = false;
    if (!ti->rows_valid || ti->rows_rev != ti->rev || ti->rows_w != W || ti->rows_pre != ti->pre_ver) {
        ti->rows_full++;
        compute_rows(ti, ts);
        ti->rows_valid = true;
        ti->rows_rev = ti->rev;
        ti->rows_w = W;
        ti->rows_pre = ti->pre_ver;
    }
}

static size_t rows_count(TextInput *ti, gp_text_system *ts) {
    if (eager_rows(ti)) { rows_ready(ti, ts); return ti->nrows; }
    return ti->o.multiline ? (ti->nl ? ti->nl : 1) : 1;
}

/* Is hard line [start, end) a right-to-left paragraph? Text with nothing right-to-left in it is not, whatever the field says. */
static bool paragraph_rtl(const TextInput *ti, size_t start, size_t end, bool line_flag) {
    if (ti->o.direction == GP_TEXT_DIRECTION_RTL) return true;
    if (!line_flag) return false;
    return gp_text_is_rtl(ti->text + start, end - start, (int)ti->o.direction);
}

static Row row_at(TextInput *ti, gp_text_system *ts, size_t i) {
    if (eager_rows(ti)) { rows_ready(ti, ts); return ti->rows[i]; }
    if (!ti->o.multiline || !ti->nl)
        return (Row){ 0, ti->len, 0, false, gp_text_is_rtl(ti->text ? ti->text : "", ti->len, (int)ti->o.direction), 0, 0, 0 };
    size_t s0 = ti->ls[i], e0 = line_end(ti, i);
    return (Row){ s0, e0, (uint32_t)i, false, paragraph_rtl(ti, s0, e0, ti->lrtl[i]), 0, 0, 0 };
}

/* The row a caret at `off` is drawn on: the last row that starts at or before it, except that a caret at the end of a row that wrapped is
 * on that row if it is "upstream" (it is the same offset as the next row's start). */
static size_t row_of_off(TextInput *ti, gp_text_system *ts, size_t off, bool upstream) {
    size_t n = rows_count(ti, ts);
    if (!eager_rows(ti)) return ti->o.multiline && ti->nl ? line_of(ti, off) : 0;
    size_t lo = 0, hi = n;
    while (hi - lo > 1) {
        size_t mid = lo + (hi - lo) / 2;
        if (ti->rows[mid].start <= off) lo = mid; else hi = mid;
    }
    if (lo > 0 && upstream && ti->rows[lo - 1].soft_end && ti->rows[lo - 1].end == off && ti->rows[lo].start == off) return lo - 1;
    return lo;
}

/* The row the caret is drawn on: where its offset is, or, while composing with soft wrap, the row that holds the composition's own caret
 * (the composition can be several rows long). */
static size_t caret_row(TextInput *ti, gp_text_system *ts) {
    size_t ri = row_of_off(ti, ts, ti->head, ti->upstream);
    if (!composing(ti) || !eager_rows(ti)) return ri;
    size_t plen = strlen(ti->preedit), cb = 0;   /* the cursor's byte in the composition */
    for (int c = 0; c < ti->preedit_cursor && cb < plen; c++) { size_t n; cp_decode(ti->preedit, plen, cb, &n); cb += n; }
    size_t last = SIZE_MAX;
    for (size_t i = 0; i < ti->nrows; i++) {
        const Row *r = &ti->rows[i];
        if (r->pre != 1 || r->pre_b <= r->pre_a) continue;
        last = i;
        if (cb >= r->pre_a && cb < r->pre_b) return i;
    }
    return last != SIZE_MAX ? last : ri;   /* (the cursor is at the end of the composition: its last row) */
}

/* Position of the caret in content coordinates. */
static void caret_xy(TextInput *ti, gp_text_system *ts, gp_arena *a, float *x, float *y) {
    size_t ri = caret_row(ti, ts);
    Row row = row_at(ti, ts, ri);
    Line L;
    build_row(ti, ts, &row, true, a, &L);
    *x = L.composing ? L.pre_cursor_x : caret_x(&L, ti->head, ti->upstream);
    *y = (float)ri * line_height(ti);
}

/* Where the caret would be drawn with the other affinity, if that is somewhere else: at a place where the direction changes, a caret at one offset has two
 * places (the end of the character before, the start of the one after) and typing goes to the one the affinity picks; the other is the secondary caret. */
static bool caret_alt_xy(TextInput *ti, gp_text_system *ts, gp_arena *a, float *x, float *y) {
    if (composing(ti) || has_sel(ti)) return false;
    size_t ri = row_of_off(ti, ts, ti->head, ti->upstream);
    Row row = row_at(ti, ts, ri);
    Line L;
    build_row(ti, ts, &row, false, a, &L);
    if (!L.has_rtl) return false;   /* one direction: both places are the same */
    float here = caret_x(&L, ti->head, ti->upstream), there = caret_x(&L, ti->head, !ti->upstream);
    /* the other affinity of an offset at a row's start or end (a soft wrap) is on another row: not a second caret */
    if (fabsf(here - there) < 0.5f) return false;
    *x = there;
    *y = (float)ri * line_height(ti);
    return true;
}

/* The offset under content coordinates (x, y), with the affinity of the edge that was hit. */
static size_t offset_at(TextInput *ti, gp_text_system *ts, gp_arena *a, float x, float y,
                        bool *upstream) {
    size_t n = rows_count(ti, ts);
    long idx = (long)floorf(y / line_height(ti));
    if (idx < 0) idx = 0;
    if ((size_t)idx >= n) idx = (long)n - 1;
    Row row = row_at(ti, ts, (size_t)idx);
    Line L;
    build_row(ti, ts, &row, true, a, &L);
    return offset_in_line(&L, x, upstream);
}

/* ---------------------------------------------------------------- buffer + undo */

static void ensure_cap(TextInput *ti, size_t need) {
    if (need + 1 <= ti->cap) return;
    size_t c = ti->cap ? ti->cap : 64;
    while (c < need + 1) c *= 2;
    ti->text = realloc(ti->text, c);
    if (!ti->text) abort();
    ti->cap = c;
}

/* Replaces [start, end) with ins. No bookkeeping. */
static void raw_replace(TextInput *ti, size_t start, size_t end, const char *ins, size_t ilen) {
    ensure_cap(ti, ti->len - (end - start) + ilen);
    memmove(ti->text + start + ilen, ti->text + end, ti->len - end);
    if (ilen) memcpy(ti->text + start, ins, ilen);
    ti->len = ti->len - (end - start) + ilen;
    ti->text[ti->len] = 0;
    ti->rev++; /* any cached rows are stale */
    lines_replace(ti, start, end, ilen);
    if (eager_rows(ti) && ti->rows_valid) {   /* one edit since they were made: they can be brought up to date; another before that, and they are made again */
        if (ti->rows_rev + 1 == ti->rev && !ti->pend_valid) {
            ti->pend_valid = true;
            ti->pend_start = start;
            ti->pend_old_end = end;
            ti->pend_len = ilen;
        } else {
            ti->pend_valid = false;
        }
    }
}

static void edit_free(Edit *e) {
    free(e->old);
    free(e->new_);
}

static void clear_stack(Edit **v, size_t *n) {
    for (size_t i = 0; i < *n; i++) edit_free(&(*v)[i]);
    *n = 0;
}

static void push_edit(Edit **v, size_t *n, size_t *cap, Edit e) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 32;
        *v = realloc(*v, *cap * sizeof **v);
    }
    (*v)[(*n)++] = e;
}

static char *dup_n(const char *s, size_t n) {
    char *d = malloc(n + 1);
    if (n) memcpy(d, s, n);
    d[n] = 0;
    return d;
}

/* ---------------------------------------------------------------- notifications */

static void on_blink(void *self, gp_ctx *cx, void *data) {
    TextInput *ti = self;
    ti->caret_on = !ti->caret_on;
    if (gp_is_focused(cx, ti->focus)) gp_notify(cx); /* nothing to redraw otherwise */
}

/* Any interaction makes the caret solid and restarts the blink. */
static void touched(TextInput *ti, gp_ctx *cx) {
    ti->caret_on = true;
    gp_cancel(cx->app, ti->blink);
    gp_ctx own = gp_ctx_for(cx->app, ti->self);
    ti->blink = gp_every(&own, BLINK_SECONDS, on_blink, NULL);
}

static void fire(TextInput *ti, gp_ctx *cx, gp_text_input_event_fn fn) {
    if (!fn) return;
    gp_entity owner = ti->o.owner;
    void *state = owner.generation ? gp_entity_get(cx->app, owner) : NULL;
    if (owner.generation && !state) return;
    gp_ctx octx = { cx->app, cx->window, owner, false };
    fn(state, &octx, ti->self, ti->o.data);
}

/* Scrolls the caret into view, using the viewport as of the last layout. */
static void reveal_caret(TextInput *ti, gp_ctx *cx) {
    gp_size vp = gp_scroll_viewport(cx, ti->scroll);
    if (vp.w <= 0 || vp.h <= 0) return;
    gp_arena tmp = { .chunk_size = 16384 };
    float x, y;
    caret_xy(ti, cx->app->text, &tmp, &x, &y);
    gp_arena_free(&tmp);

    gp_point cur = gp_scroll_offset(cx, ti->scroll), want = cur;
    const float margin = 6;
    if (x - margin < cur.x) want.x = fmaxf(0, x - margin);
    else if (x + CARET_W + margin > cur.x + vp.w) want.x = x + CARET_W + margin - vp.w;
    if (ti->o.multiline) {
        float lh = line_height(ti);
        if (y < cur.y) want.y = y;
        else if (y + lh > cur.y + vp.h) want.y = y + lh - vp.h;
    }
    if (want.x != cur.x || want.y != cur.y) gp_scroll_to(cx, ti->scroll, want.x, want.y);
}

static void changed(TextInput *ti, gp_ctx *cx) {
    touched(ti, cx);
    reveal_caret(ti, cx);
    gp_notify(cx);
    fire(ti, cx, ti->o.on_change);
}

static void moved(TextInput *ti, gp_ctx *cx) {
    ti->break_coalesce = true;
    touched(ti, cx);
    reveal_caret(ti, cx);
    gp_notify(cx);
}

/* ---------------------------------------------------------------- editing */

enum { KIND_INSERT, KIND_BACKSPACE, KIND_DELETE, KIND_OTHER };

/* Replaces [start, end) with ins (clamped / truncated as needed), recording undo. Leaves the
 * caret after the inserted text. */
static void apply_edit(TextInput *ti, gp_ctx *cx, size_t start, size_t end, const char *ins,
                       size_t ilen, int kind) {
    if (start > end) { size_t t = start; start = end; end = t; }
    if (end > ti->len) end = ti->len;
    if (start > end) start = end;
    if (ti->o.max_bytes) {
        size_t room = ti->o.max_bytes > ti->len - (end - start) ? ti->o.max_bytes - (ti->len - (end - start)) : 0;
        if (ilen > room) { /* truncate on a character boundary */
            ilen = room;
            while (ilen > 0 && ((unsigned char)ins[ilen] & 0xc0) == 0x80) ilen--;
        }
    }
    if (start == end && ilen == 0) return;

    double now = gp_time();
    Edit *top = ti->nundo ? &ti->undo[ti->nundo - 1] : NULL;
    bool merge = top && !ti->break_coalesce && now - top->time < COALESCE_SECONDS;
    size_t before_a = ti->anchor, before_h = ti->head;
    char *old = dup_n(ti->text + start, end - start);
    size_t old_len = end - start;

    raw_replace(ti, start, end, ins, ilen);
    ti->anchor = ti->head = start + ilen;
    ti->has_goal = false;
    /* In bidirectional text the caret after a typed character belongs at that character's trailing
     * edge (so typing Hebrew after English walks leftwards); in plain text it makes no difference. */
    ti->upstream = text_has_rtl(ti);
    clear_stack(&ti->redo, &ti->nredo);

    if (merge && kind == KIND_INSERT && top->old_len == 0 && top->pos + top->new_len == start) {
        top->new_ = realloc(top->new_, top->new_len + ilen + 1);
        memcpy(top->new_ + top->new_len, ins, ilen);
        top->new_len += ilen;
        top->new_[top->new_len] = 0;
        top->after_anchor = top->after_head = ti->head;
        top->time = now;
        free(old);
    } else if (merge && kind == KIND_BACKSPACE && top->new_len == 0 && end == top->pos) {
        char *joined = malloc(old_len + top->old_len + 1);
        memcpy(joined, old, old_len);
        memcpy(joined + old_len, top->old, top->old_len);
        joined[old_len + top->old_len] = 0;
        free(top->old);
        free(old);
        top->old = joined;
        top->old_len += old_len;
        top->pos = start;
        top->after_anchor = top->after_head = ti->head;
        top->time = now;
    } else if (merge && kind == KIND_DELETE && top->new_len == 0 && start == top->pos) {
        top->old = realloc(top->old, top->old_len + old_len + 1);
        memcpy(top->old + top->old_len, old, old_len);
        top->old_len += old_len;
        top->old[top->old_len] = 0;
        top->after_anchor = top->after_head = ti->head;
        top->time = now;
        free(old);
    } else {
        if (ti->nundo == MAX_UNDO) { /* drop the oldest */
            edit_free(&ti->undo[0]);
            memmove(ti->undo, ti->undo + 1, (--ti->nundo) * sizeof *ti->undo);
        }
        push_edit(&ti->undo, &ti->nundo, &ti->cap_undo,
                  (Edit){ start, old, old_len, dup_n(ins, ilen), ilen, before_a, before_h, ti->head,
                          ti->head, now });
    }
    ti->break_coalesce = false;
    changed(ti, cx);
}

static void do_undo(TextInput *ti, gp_ctx *cx) {
    if (!ti->nundo) return;
    Edit e = ti->undo[--ti->nundo];
    raw_replace(ti, e.pos, e.pos + e.new_len, e.old, e.old_len);
    ti->anchor = e.before_anchor;
    ti->head = e.before_head;
    ti->upstream = false;
    push_edit(&ti->redo, &ti->nredo, &ti->cap_redo, e);
    ti->break_coalesce = true;
    changed(ti, cx);
}

static void do_redo(TextInput *ti, gp_ctx *cx) {
    if (!ti->nredo) return;
    Edit e = ti->redo[--ti->nredo];
    raw_replace(ti, e.pos, e.pos + e.old_len, e.new_, e.new_len);
    ti->anchor = e.after_anchor;
    ti->head = e.after_head;
    ti->upstream = false;
    push_edit(&ti->undo, &ti->nundo, &ti->cap_undo, e);
    ti->break_coalesce = true;
    changed(ti, cx);
}

/* Single-line fields flatten line breaks and drop control characters. Caller frees. */
static char *sanitize(const TextInput *ti, const char *s, size_t n) {
    char *out = malloc(n + 1);
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\r') {
            if (i + 1 < n && s[i + 1] == '\n') i++;
            c = '\n';
        }
        if (c == '\n' && !ti->o.multiline) c = ' ';
        if (c < 0x20 && c != '\n' && c != '\t') continue;
        if (c == '\t' && !ti->o.multiline) c = ' ';
        out[o++] = (char)c;
    }
    out[o] = 0;
    return out;
}

static void insert_text(TextInput *ti, gp_ctx *cx, const char *s, size_t n) {
    if (ti->o.read_only) return;
    char *clean = sanitize(ti, s, n);
    size_t cl = strlen(clean);
    if (cl) {
        apply_edit(ti, cx, sel_lo(ti), sel_hi(ti), clean, cl, KIND_INSERT);
    }
    free(clean);
}

static void delete_range(TextInput *ti, gp_ctx *cx, size_t a, size_t b, int kind) {
    if (ti->o.read_only || a == b) return;
    apply_edit(ti, cx, a, b, "", 0, kind);
}

/* ---------------------------------------------------------------- actions */

enum { M_LEFT, M_RIGHT, M_WORD_L, M_WORD_R, M_LINE_START, M_LINE_END, M_DOC_START, M_DOC_END,
       M_UP, M_DOWN, M_PAGE_UP, M_PAGE_DOWN };
#define MOVE(dir, extend) ((void *)(intptr_t)((dir) << 1 | (extend)))

static void set_caret_aff(TextInput *ti, gp_ctx *cx, size_t pos, bool extend, bool upstream) {
    ti->head = pos;
    ti->upstream = upstream;
    if (!extend) ti->anchor = pos;
    moved(ti, cx);
}

static void set_caret(TextInput *ti, gp_ctx *cx, size_t pos, bool extend) {
    set_caret_aff(ti, cx, pos, extend, false);
}

/* `scroll_px` != 0 (Page Up / Down) also scrolls the view by that much; the caret is then
 * kept visible by the usual reveal. Moves by visual rows, so with soft wrap Up and Down step
 * through the wrapped pieces of a long line. */
static void move_vertical(TextInput *ti, gp_ctx *cx, int lines, bool extend, float scroll_px) {
    if (!ti->o.multiline) { /* single line: up/down go to the ends */
        ti->has_goal = false;
        set_caret(ti, cx, lines < 0 ? 0 : ti->len, extend);
        return;
    }
    gp_arena tmp = { .chunk_size = 16384 };
    gp_text_system *ts = cx->app->text;
    float x, y;
    caret_xy(ti, ts, &tmp, &x, &y);
    if (!ti->has_goal) { ti->goal_x = x; ti->has_goal = true; }
    size_t nrows = rows_count(ti, ts);
    long cur = (long)row_of_off(ti, ts, ti->head, ti->upstream), target = cur + lines;
    size_t pos;
    bool upstream = false;
    if (target < 0) {
        pos = 0;
    } else if ((size_t)target >= nrows) {
        pos = ti->len;
    } else {
        Row trow = row_at(ti, ts, (size_t)target);
        Line L;
        build_line(ti, ts, trow.start, trow.end, false, trow.rtl, &tmp, &L);
        pos = offset_in_line(&L, ti->goal_x, &upstream); /* upstream: stay on this row */
    }
    gp_arena_free(&tmp);
    if (scroll_px != 0) gp_scroll_by(cx, ti->scroll, 0, scroll_px);
    bool boundary = target < 0 || (size_t)target >= nrows; /* hit the first / last row: new column */
    float keep = ti->goal_x;
    set_caret_aff(ti, cx, pos, extend, upstream);
    ti->goal_x = keep;
    ti->has_goal = !boundary;
}

/* Visual order of cells: the indices sorted by left edge. */
static const float *g_cell_xl; /* comparator context; the widget runs on one thread */
static int cell_visual_cmp(const void *pa, const void *pb) {
    float a = g_cell_xl[*(const size_t *)pa], b = g_cell_xl[*(const size_t *)pb];
    return a < b ? -1 : a > b;
}

/* One arrow-key step through a row that involves right-to-left text. The caret sits at an edge of
 * some cell, which is a visual slot between two neighbouring cells; Right crosses the cell on its
 * right and puts the caret on that cell's right edge (the trailing edge if the cell is
 * left-to-right, the leading edge if it is right-to-left), and Left mirrors that. Each press
 * therefore moves the caret exactly one visible character, wherever the text's logical order
 * has gone. Returns false at the visual end of the row. */
static bool visual_step(TextInput *ti, gp_ctx *cx, bool right, size_t *pos, bool *up) {
    gp_text_system *ts = cx->app->text;
    size_t ri = row_of_off(ti, ts, ti->head, ti->upstream);
    Row vrow = row_at(ti, ts, ri);
    gp_arena tmp = { .chunk_size = 16384 };
    Line L;
    build_line(ti, ts, vrow.start, vrow.end, false, vrow.rtl, &tmp, &L);
    size_t m = L.n - 1;
    bool moved_ok = false;
    if (m > 0) {
        size_t *order = gp_arena_alloc(&tmp, m * sizeof *order, 8);
        for (size_t k = 0; k < m; k++) order[k] = k;
        g_cell_xl = L.xl;
        qsort(order, m, sizeof *order, cell_visual_cmp);
        float x = caret_x(&L, ti->head, ti->upstream);
        size_t slot = 0; /* slot s is the gap before the s-th visual cell (m = after the last) */
        float best = 1e30f;
        for (size_t sl = 0; sl <= m; sl++) {
            float bx = sl == 0 ? L.xl[order[0]] : L.xr[order[sl - 1]];
            if (fabsf(x - bx) < best) { best = fabsf(x - bx); slot = sl; }
        }
        if (right && slot < m) {
            size_t c = order[slot];
            *pos = L.rtl[c] ? L.byte[c] : L.byte[c + 1];
            *up = !L.rtl[c];
            moved_ok = true;
        } else if (!right && slot > 0) {
            size_t c = order[slot - 1];
            *pos = L.rtl[c] ? L.byte[c + 1] : L.byte[c];
            *up = L.rtl[c];
            moved_ok = true;
        }
    }
    gp_arena_free(&tmp);
    return moved_ok;
}

/* Is the paragraph the caret is in right-to-left? */
static bool head_para_rtl(TextInput *ti, gp_text_system *ts) {
    return row_at(ti, ts, row_of_off(ti, ts, ti->head, ti->upstream)).rtl;
}

static void act_move(void *self, gp_ctx *cx, void *data) {
    TextInput *ti = self;
    int dir = (int)((intptr_t)data >> 1);
    bool extend = (intptr_t)data & 1;
    if (dir != M_UP && dir != M_DOWN && dir != M_PAGE_UP && dir != M_PAGE_DOWN) ti->has_goal = false;
    size_t pos = ti->head;
    switch (dir) {
    case M_LEFT:
    case M_RIGHT: {
        bool right = dir == M_RIGHT;
        bool bidi = text_has_rtl(ti);
        if (!bidi) { /* the common case: plain left-to-right text, moving by characters */
            pos = has_sel(ti) && !extend ? (right ? sel_hi(ti) : sel_lo(ti))
                                         : (right ? char_next(ti, ti->head) : char_prev(ti, ti->head));
            break;
        }
        bool para_rtl = head_para_rtl(ti, cx->app->text);
        if (has_sel(ti) && !extend) { /* collapse to the selection's edge on that side of the screen */
            pos = (right != para_rtl) ? sel_hi(ti) : sel_lo(ti);
            break;
        }
        size_t np;
        bool nu;
        if (!composing(ti) && visual_step(ti, cx, right, &np, &nu)) {
            set_caret_aff(ti, cx, np, extend, nu);
            return;
        }
        /* The visual end of the row: carry on into the next row in this paragraph's reading
         * direction (rightwards in LTR text, leftwards in RTL), where the text continues. */
        gp_text_system *ts = cx->app->text;
        size_t nrows = rows_count(ti, ts);
        size_t ri = row_of_off(ti, ts, ti->head, ti->upstream);
        if (right != para_rtl) {
            if (ri + 1 < nrows) set_caret_aff(ti, cx, row_at(ti, ts, ri + 1).start, extend, false);
        } else if (ri > 0) {
            set_caret_aff(ti, cx, row_at(ti, ts, ri - 1).end, extend, true);
        }
        return;
    }
    case M_WORD_L:
    case M_WORD_R: {
        /* "left" is backwards in a left-to-right paragraph and forwards in a right-to-left one */
        bool back = dir == M_WORD_L;
        if (text_has_rtl(ti) && head_para_rtl(ti, cx->app->text)) back = !back;
        pos = back ? word_left(ti, ti->head) : word_right(ti, ti->head);
        break;
    }
    case M_LINE_START:
    case M_LINE_END: {
        if (!ti->o.multiline) { pos = dir == M_LINE_START ? 0 : ti->len; break; }
        Row row = row_at(ti, cx->app->text, row_of_off(ti, cx->app->text, ti->head, ti->upstream));
        /* End on a wrapped row stays on that row, even though the offset is also the next row's start */
        set_caret_aff(ti, cx, dir == M_LINE_START ? row.start : row.end, extend,
                      dir == M_LINE_END && row.soft_end);
        return;
    }
    case M_DOC_START: pos = 0; break;
    case M_DOC_END: pos = ti->len; break;
    case M_UP: move_vertical(ti, cx, -1, extend, 0); return;
    case M_DOWN: move_vertical(ti, cx, 1, extend, 0); return;
    case M_PAGE_UP:
    case M_PAGE_DOWN: {
        int page = ti->o.rows > 1 ? ti->o.rows - 1 : 1, sign = dir == M_PAGE_UP ? -1 : 1;
        move_vertical(ti, cx, sign * page, extend, (float)(sign * page) * line_height(ti));
        return;
    }
    }
    set_caret(ti, cx, pos, extend);
}

static void act_select_all(void *self, gp_ctx *cx, void *data) {
    TextInput *ti = self;
    ti->anchor = 0;
    ti->head = ti->len;
    ti->upstream = false;
    moved(ti, cx);
}

static void act_backspace(void *self, gp_ctx *cx, void *data) {
    TextInput *ti = self;
    if (has_sel(ti)) delete_range(ti, cx, sel_lo(ti), sel_hi(ti), KIND_OTHER);
    else delete_range(ti, cx, char_prev(ti, ti->head), ti->head, KIND_BACKSPACE);
}

static void act_delete(void *self, gp_ctx *cx, void *data) {
    TextInput *ti = self;
    if (has_sel(ti)) delete_range(ti, cx, sel_lo(ti), sel_hi(ti), KIND_OTHER);
    else delete_range(ti, cx, ti->head, char_next(ti, ti->head), KIND_DELETE);
}

static void act_delete_word_left(void *self, gp_ctx *cx, void *data) {
    TextInput *ti = self;
    if (has_sel(ti)) delete_range(ti, cx, sel_lo(ti), sel_hi(ti), KIND_OTHER);
    else delete_range(ti, cx, word_left(ti, ti->head), ti->head, KIND_OTHER);
}

static void act_delete_word_right(void *self, gp_ctx *cx, void *data) {
    TextInput *ti = self;
    if (has_sel(ti)) delete_range(ti, cx, sel_lo(ti), sel_hi(ti), KIND_OTHER);
    else delete_range(ti, cx, ti->head, word_right(ti, ti->head), KIND_OTHER);
}

static void act_delete_to_line_start(void *self, gp_ctx *cx, void *data) {
    TextInput *ti = self;
    size_t from = ti->o.multiline ? line_start_of(ti, ti->head) : 0;
    if (from == ti->head && ti->o.multiline) from = char_prev(ti, ti->head); /* join with previous line */
    delete_range(ti, cx, from, ti->head, KIND_OTHER);
}

static void act_delete_to_line_end(void *self, gp_ctx *cx, void *data) {
    TextInput *ti = self;
    size_t to = ti->o.multiline ? line_end_of(ti, ti->head) : ti->len;
    if (to == ti->head && ti->o.multiline) to = char_next(ti, ti->head);
    delete_range(ti, cx, ti->head, to, KIND_OTHER);
}

static void act_copy(void *self, gp_ctx *cx, void *data) {
    TextInput *ti = self;
    if (!has_sel(ti) || ti->o.password) return; /* a password field never exports its text */
    char *s = dup_n(ti->text + sel_lo(ti), sel_hi(ti) - sel_lo(ti));
    gp_clipboard_set(cx->app, s);
    free(s);
}

static void act_cut(void *self, gp_ctx *cx, void *data) {
    TextInput *ti = self;
    if (ti->o.read_only) return;
    act_copy(self, cx, data);
    if (!ti->o.password) delete_range(ti, cx, sel_lo(ti), sel_hi(ti), KIND_OTHER);
}

static void act_paste(void *self, gp_ctx *cx, void *data) {
    TextInput *ti = self;
    char *s = gp_clipboard_get(cx->app);
    if (!s) return;
    ti->break_coalesce = true;
    insert_text(ti, cx, s, strlen(s));
    ti->break_coalesce = true;
    free(s);
}

static void act_undo(void *self, gp_ctx *cx, void *data) { if (!((TextInput *)self)->o.read_only) do_undo(self, cx); }
static void act_redo(void *self, gp_ctx *cx, void *data) { if (!((TextInput *)self)->o.read_only) do_redo(self, cx); }

static void act_newline(void *self, gp_ctx *cx, void *data) {
    TextInput *ti = self;
    if (!ti->o.multiline) return;
    insert_text(ti, cx, "\n", 1);
}

static void act_submit(void *self, gp_ctx *cx, void *data) {
    TextInput *ti = self;
    fire(ti, cx, ti->o.on_submit);
}

/* Enter: a new line in a multi-line field, otherwise submit. */
static void act_enter(void *self, gp_ctx *cx, void *data) {
    if (((TextInput *)self)->o.multiline) act_newline(self, cx, data);
    else act_submit(self, cx, data);
}

static const struct { const char *name; gp_action_fn fn; void *data; } ACTIONS[] = {
    { "text_input::move_left", act_move, MOVE(M_LEFT, 0) },
    { "text_input::move_right", act_move, MOVE(M_RIGHT, 0) },
    { "text_input::move_word_left", act_move, MOVE(M_WORD_L, 0) },
    { "text_input::move_word_right", act_move, MOVE(M_WORD_R, 0) },
    { "text_input::move_line_start", act_move, MOVE(M_LINE_START, 0) },
    { "text_input::move_line_end", act_move, MOVE(M_LINE_END, 0) },
    { "text_input::move_doc_start", act_move, MOVE(M_DOC_START, 0) },
    { "text_input::move_doc_end", act_move, MOVE(M_DOC_END, 0) },
    { "text_input::move_up", act_move, MOVE(M_UP, 0) },
    { "text_input::move_down", act_move, MOVE(M_DOWN, 0) },
    { "text_input::page_up", act_move, MOVE(M_PAGE_UP, 0) },
    { "text_input::page_down", act_move, MOVE(M_PAGE_DOWN, 0) },
    { "text_input::select_left", act_move, MOVE(M_LEFT, 1) },
    { "text_input::select_right", act_move, MOVE(M_RIGHT, 1) },
    { "text_input::select_word_left", act_move, MOVE(M_WORD_L, 1) },
    { "text_input::select_word_right", act_move, MOVE(M_WORD_R, 1) },
    { "text_input::select_line_start", act_move, MOVE(M_LINE_START, 1) },
    { "text_input::select_line_end", act_move, MOVE(M_LINE_END, 1) },
    { "text_input::select_doc_start", act_move, MOVE(M_DOC_START, 1) },
    { "text_input::select_doc_end", act_move, MOVE(M_DOC_END, 1) },
    { "text_input::select_up", act_move, MOVE(M_UP, 1) },
    { "text_input::select_down", act_move, MOVE(M_DOWN, 1) },
    { "text_input::select_page_up", act_move, MOVE(M_PAGE_UP, 1) },
    { "text_input::select_page_down", act_move, MOVE(M_PAGE_DOWN, 1) },
    { "text_input::select_all", act_select_all, NULL },
    { "text_input::backspace", act_backspace, NULL },
    { "text_input::delete", act_delete, NULL },
    { "text_input::delete_word_left", act_delete_word_left, NULL },
    { "text_input::delete_word_right", act_delete_word_right, NULL },
    { "text_input::delete_to_line_start", act_delete_to_line_start, NULL },
    { "text_input::delete_to_line_end", act_delete_to_line_end, NULL },
    { "text_input::copy", act_copy, NULL },
    { "text_input::cut", act_cut, NULL },
    { "text_input::paste", act_paste, NULL },
    { "text_input::undo", act_undo, NULL },
    { "text_input::redo", act_redo, NULL },
    { "text_input::newline", act_newline, NULL },
    { "text_input::submit", act_submit, NULL },
    { "text_input::enter", act_enter, NULL },
};
#define NACTIONS (sizeof ACTIONS / sizeof *ACTIONS)

static const struct { const char *key, *action; } BINDINGS[] = {
    { "left", "move_left" }, { "right", "move_right" },
    { "ctrl-left", "move_word_left" }, { "ctrl-right", "move_word_right" },
    { "home", "move_line_start" }, { "end", "move_line_end" },
    { "ctrl-home", "move_doc_start" }, { "ctrl-end", "move_doc_end" },
    { "up", "move_up" }, { "down", "move_down" }, { "pageup", "page_up" }, { "pagedown", "page_down" },
    { "shift-left", "select_left" }, { "shift-right", "select_right" },
    { "ctrl-shift-left", "select_word_left" }, { "ctrl-shift-right", "select_word_right" },
    { "shift-home", "select_line_start" }, { "shift-end", "select_line_end" },
    { "ctrl-shift-home", "select_doc_start" }, { "ctrl-shift-end", "select_doc_end" },
    { "shift-up", "select_up" }, { "shift-down", "select_down" },
    { "shift-pageup", "select_page_up" }, { "shift-pagedown", "select_page_down" },
    { "ctrl-a", "select_all" },
    { "backspace", "backspace" }, { "shift-backspace", "backspace" }, { "delete", "delete" },
    { "ctrl-backspace", "delete_word_left" }, { "ctrl-delete", "delete_word_right" },
    { "ctrl-u", "delete_to_line_start" }, { "ctrl-k", "delete_to_line_end" },
    { "ctrl-c", "copy" }, { "ctrl-insert", "copy" }, { "ctrl-x", "cut" }, { "shift-delete", "cut" },
    { "ctrl-v", "paste" }, { "shift-insert", "paste" },
    { "ctrl-z", "undo" }, { "ctrl-shift-z", "redo" }, { "ctrl-y", "redo" },
    { "enter", "enter" }, { "shift-enter", "enter" }, { "ctrl-enter", "submit" },
};

static void bind_defaults(gp_app *app) {
    if (app->text_input_keys_bound) return;
    app->text_input_keys_bound = true;
    for (size_t i = 0; i < sizeof BINDINGS / sizeof *BINDINGS; i++)
    {
        char name[64];
        snprintf(name, sizeof name, "text_input::%s", BINDINGS[i].action);
        gp_bind_key(app, BINDINGS[i].key, name, "TextInput");
    }
}

/* ---------------------------------------------------------------- text, IME, mouse */

static void on_text(void *self, gp_ctx *cx, const char *utf8, void *data) {
    TextInput *ti = self;
    if (ti->o.read_only) return;
    if (composing(ti)) { /* the committed text replaces the composition */
        set_preedit(ti, "", 0);
    }
    insert_text(ti, cx, utf8, strlen(utf8));
}

static void on_ime(void *self, gp_ctx *cx, const char *preedit, int cursor, void *data) {
    TextInput *ti = self;
    if (ti->o.read_only) return;
    const char *next = preedit ? preedit : "";
    /* Platforms re-send the current preedit (often empty, e.g. whenever text input is
     * (re)started); reacting to an unchanged one would only re-render for nothing. */
    if (strcmp(next, ti->preedit) == 0 && (cursor < 0 ? 0 : cursor) == ti->preedit_cursor) return;
    set_preedit(ti, next, cursor);
    if (composing(ti) && has_sel(ti)) /* starting to compose replaces the selection */
        delete_range(ti, cx, sel_lo(ti), sel_hi(ti), KIND_OTHER);
    touched(ti, cx);
    reveal_caret(ti, cx);
    gp_notify(cx);
}

/* Window position -> content coordinates of the scroller. */
static gp_point to_content(TextInput *ti, gp_ctx *cx, gp_point local) {
    gp_point s = gp_scroll_offset(cx, ti->scroll);
    return (gp_point){ local.x - (BORDER + ti->o.padding_x) + s.x, local.y - (BORDER + ti->o.padding_y) + s.y };
}

static void line_bounds_at(TextInput *ti, size_t off, size_t *lo, size_t *hi) {
    if (!ti->o.multiline) { *lo = 0; *hi = ti->len; return; }
    *lo = line_start_of(ti, off);
    *hi = line_end_of(ti, off);
    if (*hi < ti->len) *hi += 1; /* include the newline so a triple-click selects the whole line */
}

/* Pastes the primary selection at the click (a middle click, as on X11 and Wayland); it does not move or replace the selection. */
static void paste_primary_at(TextInput *ti, gp_ctx *cx, const gp_mouse_event *ev) {
    if (ti->o.read_only) return;
    char *s = gp_primary_get(cx->app);
    if (!s) return;
    gp_arena tmp = { .chunk_size = 16384 };
    gp_point c = to_content(ti, cx, ev->local);
    bool up;
    size_t off = offset_at(ti, cx->app->text, &tmp, c.x, c.y, &up);
    gp_arena_free(&tmp);
    ti->anchor = ti->head = off;
    ti->upstream = up;
    ti->has_goal = false;
    ti->break_coalesce = true;
    insert_text(ti, cx, s, strlen(s));
    ti->break_coalesce = true;
    free(s);
}

static void on_mouse_down(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    TextInput *ti = self;
    if (ev->button == GP_MOUSE_MIDDLE) { paste_primary_at(ti, cx, ev); return; }
    if (ev->button != GP_MOUSE_LEFT) return;
    gp_arena tmp = { .chunk_size = 16384 };
    gp_point c = to_content(ti, cx, ev->local);
    bool up;
    size_t off = offset_at(ti, cx->app->text, &tmp, c.x, c.y, &up);
    gp_arena_free(&tmp);
    ti->has_goal = false;
    ti->upstream = up;

    int clicks = ev->click_count < 1 ? 1 : ev->click_count > 3 ? 3 : ev->click_count;
    if (clicks == 1) {
        ti->drag_mode = 0;
        ti->head = off;
        if (!(ev->mods & GP_MOD_SHIFT)) ti->anchor = off;
    } else if (clicks == 2) {
        ti->upstream = false;
        ti->drag_mode = 1;
        word_bounds(ti, off, &ti->drag_lo, &ti->drag_hi);
        ti->anchor = ti->drag_lo;
        ti->head = ti->drag_hi;
    } else {
        ti->upstream = false;
        ti->drag_mode = 2;
        line_bounds_at(ti, off, &ti->drag_lo, &ti->drag_hi);
        ti->anchor = ti->drag_lo;
        ti->head = ti->drag_hi;
    }
    moved(ti, cx);
}

static void on_drag(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    TextInput *ti = self;
    gp_arena tmp = { .chunk_size = 16384 };
    gp_point c = to_content(ti, cx, ev->local);
    bool up;
    size_t off = offset_at(ti, cx->app->text, &tmp, c.x, c.y, &up);
    gp_arena_free(&tmp);
    ti->upstream = up;

    if (ti->drag_mode == 0) {
        ti->head = off;
    } else {
        size_t lo, hi;
        if (ti->drag_mode == 1) word_bounds(ti, off, &lo, &hi); else line_bounds_at(ti, off, &lo, &hi);
        if (off < ti->drag_lo) { ti->anchor = ti->drag_hi; ti->head = lo; }
        else if (off > ti->drag_hi) { ti->anchor = ti->drag_lo; ti->head = hi; }
        else { ti->anchor = ti->drag_lo; ti->head = ti->drag_hi; }
    }
    moved(ti, cx);
}

/* A selection finished with the mouse becomes the primary selection. */
static void on_mouse_up(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    TextInput *ti = self;
    if (ev->button != GP_MOUSE_LEFT || !has_sel(ti) || ti->o.password) return;
    char *s = dup_n(ti->text + sel_lo(ti), sel_hi(ti) - sel_lo(ti));
    gp_primary_set(cx->app, s);
    free(s);
}

/* ---------------------------------------------------------------- rendering */

static gp_color with_alpha_mul(gp_color c, float m) { c.a *= m; return c; }

static gp_node *overlay(float x, float y, float w, float h, gp_color c) {
    return gp_div(gp_absolute(), gp_left(x), gp_top(y), gp_w(w), gp_h(h), gp_bg(c));
}

/* The wrap width is whatever the scroller was given, minus room for the caret at the end of a
 * full row. Only layout knows it, so it arrives here; a change re-wraps and rebuilds. */
static void on_scroller_layout(void *self, gp_ctx *cx, gp_size size, void *data) {
    TextInput *ti = self;
    float w = size.w - (CARET_W + 2);
    if (w < 1) w = 1;
    if (fabsf(w - ti->avail_w) < 0.5f) return; /* unchanged: do not rebuild for nothing */
    ti->avail_w = w;
    bool wrap = ti->o.multiline && ti->o.soft_wrap;
    if (wrap) ti->wrap_w = w;
    /* A rebuild is only needed if the width changes what is drawn: wrapping, or right-to-left text,
     * which aligns to the right edge. Plain left-to-right fields never rebuild for a resize. */
    if (wrap || text_has_rtl(ti)) gp_notify(cx);
}

/* The rows to build for a view scrolled to `y`: the visible ones and as many again on each side. */
static void window_for(const TextInput *ti, size_t nrows, float y, size_t *first, size_t *end) {
    float lh = line_height(ti);
    size_t view_rows = (size_t)(ti->o.rows > 0 ? ti->o.rows : 4);
    long vis0 = (long)floorf(y / lh);
    if (vis0 < 0) vis0 = 0;
    long over = (long)(view_rows < 16 ? 16 : view_rows);
    long f = vis0 - over, e = vis0 + (long)view_rows + 1 + over;
    if (f < 0) f = 0;
    if ((size_t)e > nrows) e = (long)nrows;
    if ((size_t)f > nrows) f = (long)nrows;
    *first = (size_t)f;
    *end = (size_t)e;
}

/* The view moved: if what it shows is no longer well inside the rows that were built, build others. */
static void on_scroll(void *self, gp_ctx *cx, gp_point offset, void *data) {
    TextInput *ti = self;
    size_t nrows = rows_count(ti, cx->app->text), first, end;
    window_for(ti, nrows, offset.y, &first, &end);
    if (first == ti->win_first && end == ti->win_first + ti->win_n) return;
    float lh = line_height(ti);
    size_t view_rows = (size_t)(ti->o.rows > 0 ? ti->o.rows : 4);
    size_t vis0 = (size_t)(offset.y > 0 ? floorf(offset.y / lh) : 0), vis1 = vis0 + view_rows + 1;
    if (vis1 > nrows) vis1 = nrows;
    size_t have_end = ti->win_first + ti->win_n, margin = view_rows / 2 + 1;
    bool covered = vis0 >= ti->win_first + (ti->win_first > 0 ? margin : 0) && vis1 + (have_end < nrows ? margin : 0) <= have_end;
    if (covered) return;   /* still well inside: no need to build again yet */
    gp_notify(cx);
}

static gp_node *render(void *self, gp_ctx *cx) {
    TextInput *ti = self;
    const gp_text_input_options *o = &ti->o;
    gp_text_system *ts = cx->app->text;
    bool focused = gp_is_focused(cx, ti->focus);
    if (!focused && composing(ti)) { /* focus left mid-composition: the platform cancels it */
        set_preedit(ti, "", 0);
    }
    float lh = line_height(ti);
    size_t nrows = rows_count(ti, ts);
    size_t comp_row = caret_row(ti, ts); /* the row showing a composition (with soft wrap, the rows that hold a share of it say so themselves) */
    size_t lo = sel_lo(ti), hi = sel_hi(ti);

    /* Only the rows in view and a view's worth either side are built: the rest of the document is a spacer of their height. Scrolling out of
     * what was built tells on_scroll, which asks for a rebuild. */
    size_t first_row = 0, end_row = nrows;
    if (o->multiline) window_for(ti, nrows, gp_scroll_offset(cx, ti->scroll).y, &first_row, &end_row);
    ti->win_first = first_row;
    ti->win_n = end_row - first_row;

    gp_arena tmp = { .chunk_size = 32768 };
    size_t cap = (end_row - first_row) * 3 + 8;
    /* The nodes are collected in a growing array: bidirectional selections can add any number of
     * rectangles per row. */
    gp_node **vec = NULL;
    size_t nk = 0, vcap = 0;
#define KID(node)                                                                          \
    do {                                                                                   \
        if (nk == vcap) { vcap = vcap ? vcap * 2 : cap; vec = realloc(vec, vcap * sizeof *vec); if (!vec) abort(); } \
        vec[nk++] = (node);                                                                \
    } while (0)

    gp_color sel_color = focused ? o->selection_color : with_alpha_mul(o->selection_color, 0.5f);
    bool windowed = o->multiline && (first_row > 0 || end_row < nrows);
    if (windowed) {   /* the widest row seen so far keeps the content as wide as it is known to be (and zero high) */
        KID(gp_div(gp_w(ti->max_w), gp_h(0), gp_flex_none()));
        if (first_row > 0) KID(gp_div(gp_w(1), gp_h((float)first_row * lh), gp_flex_none()));
    }
    for (size_t ri = first_row; ri < end_row; ri++) {
        Row rowv = row_at(ti, ts, ri);
        const Row *row = &rowv;
        Line L;
        build_row(ti, ts, row, row->pre == 1 || ri == comp_row, &tmp, &L);   /* (a row with soft wrap's share of the composition shows it) */
        float y = (float)ri * lh;
        if (L.row_off + L.width > ti->max_w) ti->max_w = L.row_off + L.width;

        if (has_sel(ti)) {
            /* the selected line break after a hard line's last row (not after a soft wrap) */
            bool covers_break = o->multiline && !row->soft_end && hi > row->end && lo <= row->end;
            Span *spans = gp_arena_alloc(&tmp, (L.n + 2) * sizeof *spans, 4);
            size_t ns = selection_spans(&L, lo, hi, covers_break ? o->font_size * 0.35f : 0, spans);
            for (size_t k = 0; k < ns; k++)
                if (spans[k].x1 > spans[k].x0) KID(overlay(spans[k].x0, y, spans[k].x1 - spans[k].x0, lh, sel_color));
        }
        KID(gp_text(L.disp, gp_whitespace_pre(), gp_text_size(o->font_size),
                    gp_text_color(o->text_color), gp_flex_none(),
                    gp_self_start(), /* natural width: a stretched line would never overflow */
                    row->rtl ? gp_text_dir_rtl() : gp_text_dir_ltr(), /* the paragraph's direction, not re-detected per row */
                    L.row_off > 0 ? gp_ml(L.row_off) : NULL)); /* right-to-left rows hug the right edge */
        if (L.composing && L.pre_x1 > L.pre_x0) /* underline the not-yet-committed text */
            KID(overlay(L.pre_x0, y + lh - 2, L.pre_x1 - L.pre_x0, 1.5f, o->text_color));
    }
    gp_arena_free(&tmp);
    if (windowed && end_row < nrows) KID(gp_div(gp_w(1), gp_h((float)(nrows - end_row) * lh), gp_flex_none()));

    float cx_, cy_;
    gp_arena tmp2 = { .chunk_size = 16384 };
    caret_xy(ti, ts, &tmp2, &cx_, &cy_);
    gp_arena_free(&tmp2);
    if (focused && ti->caret_on && !has_sel(ti)) {
        KID(overlay(cx_, cy_, CARET_W, lh, o->caret_color));
        float ax, ay;
        gp_arena tmp3 = { .chunk_size = 16384 };
        if (caret_alt_xy(ti, ts, &tmp3, &ax, &ay))   /* the other place the caret could be, thinner and fainter (as in Windows and Firefox) */
            KID(overlay(ax, ay, CARET_W * 0.5f, lh, with_alpha_mul(o->caret_color, 0.45f)));
        gp_arena_free(&tmp3);
    }
    if (ti->len == 0 && !composing(ti) && ti->o.placeholder && *ti->o.placeholder)
        KID(gp_div(gp_absolute(), gp_left(0), gp_top(0),
                   gp_text(ti->o.placeholder, gp_text_size(o->font_size),
                           gp_text_color(o->placeholder_color))));
    gp_node **kids = gp_frame_alloc((nk ? nk : 1) * sizeof *kids);
    memcpy(kids, vec, nk * sizeof *kids);
    free(vec);
#undef KID

    /* all the actions, as one modifier list */
    gp_node **mods = gp_frame_alloc(NACTIONS * sizeof *mods);
    for (size_t i = 0; i < NACTIONS; i++) mods[i] = gp_on_action_data(ACTIONS[i].name, ACTIONS[i].fn, ACTIONS[i].data);

    gp_point sc = gp_scroll_offset(cx, ti->scroll);
    float view_h = (o->multiline ? (float)(o->rows > 0 ? o->rows : 4) : 1.f) * lh;

    return gp_div(
        gp_track_focus(ti->focus), gp_tab_stop(), gp_key_context("TextInput"), gp_mods(mods, NACTIONS),
        gp_on_text_input(on_text), gp_on_ime(on_ime),
        gp_on_mouse_down(on_mouse_down), gp_on_drag(on_drag), gp_on_mouse_up(on_mouse_up),
        gp_cursor_text(),
        gp_ime_rect(BORDER + o->padding_x + cx_ - sc.x, BORDER + o->padding_y + cy_ - sc.y, CARET_W, lh),

        o->width > 0 ? gp_w(o->width) : gp_w_full(),
        gp_px(o->padding_x), gp_py(o->padding_y), gp_rounded(o->radius), gp_bg(o->bg),
        gp_border(BORDER), gp_border_color(o->border_color),
        gp_focus_border_color(o->focus_border_color),

        gp_div(gp_track_scroll(ti->scroll), gp_w_full(), gp_h(view_h), gp_flex_none(),
               o->soft_wrap ? NULL : gp_overflow_x_scroll(), o->multiline ? gp_overflow_y_scroll() : NULL,
               o->multiline ? NULL : gp_hide_scrollbar(), gp_pr(CARET_W + 2),
               gp_on_layout(on_scroller_layout), o->multiline ? gp_on_scroll(on_scroll) : NULL,
               gp_children(kids, nk)));
}

/* ---------------------------------------------------------------- lifecycle + public api */

static gp_color or_default(gp_color c, gp_color d) { return c.a > 0 ? c : d; }

static void ti_drop(void *p) {
    TextInput *ti = p;
    free(ti->text);
    free(ti->preedit);
    free(ti->rows);
    free(ti->ls);
    free(ti->lrtl);
    free((char *)ti->o.placeholder);
    clear_stack(&ti->undo, &ti->nundo);
    clear_stack(&ti->redo, &ti->nredo);
    free(ti->undo);
    free(ti->redo);
}

gp_entity gp_text_input_new(gp_app *app, const gp_text_input_options *opts) {
    bind_defaults(app);
    TextInput init = { .magic = TI_MAGIC };
    if (opts) init.o = *opts;
    gp_text_input_options *o = &init.o;
    if (o->font_size <= 0) o->font_size = 15;
    if (o->padding_x <= 0) o->padding_x = 10;
    if (o->padding_y <= 0) o->padding_y = 7;
    if (o->radius <= 0) o->radius = 8;
    if (o->rows <= 0) o->rows = 4;
    if (!o->multiline) { o->rows = 1; o->soft_wrap = false; } /* wrapping is for multi-line fields */
    if (o->direction < GP_TEXT_DIRECTION_AUTO || o->direction > GP_TEXT_DIRECTION_RTL) o->direction = GP_TEXT_DIRECTION_AUTO;
    o->text_color = or_default(o->text_color, gp_rgb(0xcdd6f4));
    o->placeholder_color = or_default(o->placeholder_color, gp_rgb(0x7f849c));
    o->bg = or_default(o->bg, gp_rgb(0x181825));
    o->border_color = or_default(o->border_color, gp_rgb(0x45475a));
    o->focus_border_color = or_default(o->focus_border_color, gp_rgb(0x89b4fa));
    o->selection_color = or_default(o->selection_color, gp_rgba(0x89b4fa59));
    o->caret_color = or_default(o->caret_color, o->text_color);
    const char *initial = o->text;
    o->text = NULL;
    o->placeholder = o->placeholder ? strdup(o->placeholder) : NULL;
    init.focus = gp_focus_new(app);
    init.scroll = gp_scroll_new(app);
    init.preedit = strdup("");
    init.caret_on = true;

    gp_entity e = gp_view_new(app, sizeof init, &init, render, ti_drop);
    TextInput *ti = gp_entity_get(app, e);
    ti->self = e;
    if (initial && *initial) {
        char *clean = sanitize(ti, initial, strlen(initial));
        size_t n = strlen(clean);
        if (o->max_bytes && n > o->max_bytes) n = o->max_bytes;
        raw_replace(ti, 0, 0, clean, n);
        free(clean);
        ti->anchor = ti->head = ti->len;
    }
    gp_ctx cx = gp_ctx_for(app, e);
    ti->blink = gp_every(&cx, BLINK_SECONDS, on_blink, NULL);
    return e;
}

const char *gp_text_input_text(gp_app *app, gp_entity input) {
    TextInput *ti = ti_get(app, input);
    return ti ? (ti->text ? ti->text : "") : "";
}

void gp_text_input_set_text(gp_ctx *cx, gp_entity input, const char *utf8) {
    TextInput *ti = ti_get(cx->app, input);
    if (!ti) return;
    char *clean = sanitize(ti, utf8 ? utf8 : "", utf8 ? strlen(utf8) : 0);
    size_t n = strlen(clean);
    if (ti->o.max_bytes && n > ti->o.max_bytes) n = ti->o.max_bytes;
    raw_replace(ti, 0, ti->len, clean, n);
    free(clean);
    ti->max_w = 0;   /* a new document: its widest row is not known yet */
    ti->anchor = ti->head = ti->len;
    clear_stack(&ti->undo, &ti->nundo);
    clear_stack(&ti->redo, &ti->nredo);
    set_preedit(ti, "", 0);
    gp_view_dirty(cx->app, input);   /* the field is a view: it draws the new text */
    gp_notify(cx);
}

void gp_text_input_selection(gp_app *app, gp_entity input, size_t *start, size_t *end) {
    TextInput *ti = ti_get(app, input);
    if (start) *start = ti ? sel_lo(ti) : 0;
    if (end) *end = ti ? sel_hi(ti) : 0;
}

void gp_text_input_select_all(gp_ctx *cx, gp_entity input) {
    TextInput *ti = ti_get(cx->app, input);
    if (!ti) return;
    ti->anchor = 0;
    ti->head = ti->len;
    gp_view_dirty(cx->app, input);
    gp_notify(cx);
}

const char *gp_text_input_preedit(gp_app *app, gp_entity input) {
    TextInput *ti = ti_get(app, input);
    return ti ? ti->preedit : "";
}

size_t gp_text_input_rows(gp_app *app, gp_entity input) {
    TextInput *ti = ti_get(app, input);
    return ti ? rows_count(ti, app->text) : 0;
}

bool gp_text_input_row_range(gp_app *app, gp_entity input, size_t row, size_t *start, size_t *end) {
    TextInput *ti = ti_get(app, input);
    if (!ti || row >= rows_count(ti, app->text)) return false;
    Row r = row_at(ti, app->text, row);
    if (start) *start = r.start;
    if (end) *end = r.end;
    return true;
}

size_t gp_text_input_caret_row(gp_app *app, gp_entity input) {
    TextInput *ti = ti_get(app, input);
    return ti ? caret_row(ti, app->text) : 0;
}

bool gp_text_input_caret_pos(gp_app *app, gp_entity input, float *x, float *y) {
    TextInput *ti = ti_get(app, input);
    if (!ti) return false;
    gp_arena tmp = { .chunk_size = 16384 };
    float cx, cy;
    caret_xy(ti, app->text, &tmp, &cx, &cy);
    gp_arena_free(&tmp);
    if (x) *x = cx;
    if (y) *y = cy;
    return true;
}

bool gp_text_input_caret_pos_alt(gp_app *app, gp_entity input, float *x, float *y) {
    TextInput *ti = ti_get(app, input);
    if (!ti) return false;
    gp_arena tmp = { .chunk_size = 16384 };
    float cx, cy;
    bool has = caret_alt_xy(ti, app->text, &tmp, &cx, &cy);
    gp_arena_free(&tmp);
    if (has) {
        if (x) *x = cx;
        if (y) *y = cy;
    }
    return has;
}

size_t gp_text_input_selection_rects(gp_app *app, gp_entity input, float (*rects)[4], size_t max) {
    TextInput *ti = ti_get(app, input);
    if (!ti || !has_sel(ti)) return 0;
    size_t out = 0;
    gp_text_system *ts = app->text;
    size_t nrows = rows_count(ti, ts), first = row_of_off(ti, ts, sel_lo(ti), false), last = row_of_off(ti, ts, sel_hi(ti), true);
    float lh = line_height(ti);
    gp_arena tmp = { .chunk_size = 32768 };
    for (size_t ri = first; ri < nrows && ri <= last + 1 && out < max; ri++) {   /* (the rows the selection reaches, not the whole document) */
        Row rowv = row_at(ti, ts, ri);
        const Row *row = &rowv;
        Line L;
        build_line(ti, app->text, row->start, row->end, false, row->rtl, &tmp, &L);
        bool covers_break = ti->o.multiline && !row->soft_end && sel_hi(ti) > row->end && sel_lo(ti) <= row->end;
        Span *spans = gp_arena_alloc(&tmp, (L.n + 2) * sizeof *spans, 4);
        size_t ns = selection_spans(&L, sel_lo(ti), sel_hi(ti), covers_break ? ti->o.font_size * 0.35f : 0, spans);
        for (size_t k = 0; k < ns && out < max; k++)
            if (spans[k].x1 > spans[k].x0)
                memcpy(rects[out++], (float[4]){ spans[k].x0, (float)ri * lh, spans[k].x1 - spans[k].x0, lh }, 4 * sizeof(float));
        gp_arena_reset(&tmp);
    }
    gp_arena_free(&tmp);
    return out;
}

bool gp_text_input_row_rtl(gp_app *app, gp_entity input, size_t row) {
    TextInput *ti = ti_get(app, input);
    return ti && row < rows_count(ti, app->text) && row_at(ti, app->text, row).rtl;
}

gp_point gp_text_input_scroll(gp_app *app, gp_entity input) {
    TextInput *ti = ti_get(app, input);
    gp_ctx cx = gp_ctx_for(app, input);
    return ti ? gp_scroll_offset(&cx, ti->scroll) : (gp_point){ 0, 0 };
}

gp_focus gp_text_input_focus(gp_app *app, gp_entity input) {
    TextInput *ti = ti_get(app, input);
    return ti ? ti->focus : (gp_focus){ 0 };
}

/* ---------------------------------------------------------------- test hooks */

/* Replaces [start, end) with `ins` through the same buffer path every edit takes (and so the line index), with no undo, notification or
 * selection change: the index is what is being tested. */
void gp_text_input_debug_replace(gp_app *app, gp_entity input, size_t start, size_t end, const char *ins) {
    TextInput *ti = ti_get(app, input);
    if (!ti) return;
    if (end > ti->len) end = ti->len;
    if (start > end) start = end;
    raw_replace(ti, start, end, ins, strlen(ins));
    ti->anchor = ti->head = ti->head > ti->len ? ti->len : ti->head;
}

/* Tests: the rows brought up to date (as the widget does) and then made again from scratch: NULL if they are the same, else what differs. Also how they were
 * brought up to date: since the start, `*full` times from scratch, `*incremental` times by moving the rows of the lines the edit did not touch. */
const char *gp_text_input_debug_rows(gp_app *app, gp_entity input, unsigned long *full, unsigned long *incremental, char *why, size_t cap) {
    TextInput *ti = ti_get(app, input);
    if (!ti) { snprintf(why, cap, "no such input"); return why; }
    gp_text_system *ts = app->text;
    size_t n = rows_count(ti, ts);
    if (full) *full = ti->rows_full;
    if (incremental) *incremental = ti->rows_incremental;
    if (!eager_rows(ti)) return NULL;
    Row *mine = ti->rows;
    size_t mine_n = ti->nrows, mine_cap = ti->cap_rows;
    ti->rows = NULL;
    ti->nrows = ti->cap_rows = 0;
    compute_rows(ti, ts);
    const char *bad = NULL;
    if (ti->nrows != n) { snprintf(why, cap, "%zu rows kept, %zu made again", mine_n, ti->nrows); bad = why; }
    for (size_t i = 0; !bad && i < n; i++) {
        const Row *a = &mine[i], *b = &ti->rows[i];
        if (a->start != b->start || a->end != b->end || a->hard != b->hard || a->soft_end != b->soft_end || a->rtl != b->rtl || a->pre != b->pre || a->pre_a != b->pre_a || a->pre_b != b->pre_b) {
            snprintf(why, cap, "row %zu: [%zu, %zu) line %u soft %d rtl %d, made again [%zu, %zu) line %u soft %d rtl %d", i, a->start, a->end, a->hard, a->soft_end, a->rtl, b->start, b->end, b->hard, b->soft_end, b->rtl);
            bad = why;
        }
    }
    free(ti->rows);
    ti->rows = mine;
    ti->nrows = mine_n;
    ti->cap_rows = mine_cap;
    return bad;
}

/* Is everything that is kept incrementally what a rescan of the text gives? NULL if so, else what is not. The row of an offset is also
 * checked against the plain search it replaced. */
const char *gp_text_input_debug_check(gp_app *app, gp_entity input, char *why, size_t cap) {
    TextInput *ti = ti_get(app, input);
    if (!ti) { snprintf(why, cap, "no such input"); return why; }
    gp_text_system *ts = app->text;
    if (ti->o.multiline && ti->nl) {
        size_t want_nl = 1, rtl = 0;
        for (size_t i = 0; i < ti->len; i++) want_nl += ti->text[i] == '\n';
        if (want_nl != ti->nl) { snprintf(why, cap, "%zu lines indexed, %zu in the text", ti->nl, want_nl); return why; }
        size_t at = 0;
        for (size_t l = 0; l < ti->nl; l++) {
            if (ti->ls[l] != at) { snprintf(why, cap, "line %zu starts at %zu, indexed at %zu", l, at, ti->ls[l]); return why; }
            size_t e = at;
            while (e < ti->len && ti->text[e] != '\n') e++;
            bool flag = span_has_rtl(ti->text + at, e - at);
            if (flag != (ti->lrtl[l] != 0)) { snprintf(why, cap, "line %zu: right-to-left flag %d, indexed %d", l, flag, ti->lrtl[l]); return why; }
            rtl += flag;
            at = e + 1;
        }
        if (rtl != ti->rtl_lines) { snprintf(why, cap, "%zu right-to-left lines counted, %zu indexed", rtl, ti->rtl_lines); return why; }
    }
    size_t n = rows_count(ti, ts);
    if (n == 0) { snprintf(why, cap, "no rows"); return why; }
    /* the row of an offset against a plain search, for a sample of offsets and both affinities */
    size_t step = ti->len > 4000 ? ti->len / 2000 : 1;
    for (size_t off = 0; off <= ti->len; off += step ? step : 1)
        for (int up = 0; up < 2; up++) {
            size_t ref = n - 1;
            for (size_t i = 0; i < n; i++) {
                Row r = row_at(ti, ts, i);
                if (off < r.start) break;
                if (off > r.end) continue;
                if (off == r.end && r.soft_end && !up) continue;
                ref = i;
                break;
            }
            size_t got = row_of_off(ti, ts, off, up);
            if (got != ref) { snprintf(why, cap, "offset %zu (upstream %d) is on row %zu, a search says %zu", off, up, got, ref); return why; }
        }
    return NULL;
}

/* The cluster boundaries as the widget finds them three ways, for the test that compares them with a segmentation made elsewhere: 0 going
 * forward with char_next (the ends of the clusters), 1 going back with char_prev (their starts, last first), 2 the caret stops of the
 * first row as build_line lays them out (the starts and the end). Returns how many were written. */
size_t gp_text_input_debug_clusters(gp_app *app, gp_entity input, int mode, size_t *out, size_t cap) {
    TextInput *ti = ti_get(app, input);
    size_t n = 0;
    if (!ti) return 0;
    if (mode == 0) {
        for (size_t p = 0; p < ti->len && n < cap;) { p = char_next(ti, p); out[n++] = p; }
    } else if (mode == 1) {
        for (size_t p = ti->len; p > 0 && n < cap;) { p = char_prev(ti, p); out[n++] = p; }
    } else {
        Row row = row_at(ti, app->text, 0);
        gp_arena a = { .chunk_size = 16384 };
        Line L;
        build_line(ti, app->text, row.start, row.end, false, row.rtl, &a, &L);
        for (size_t k = 0; k < L.n && n < cap; k++) out[n++] = L.byte[k];
        gp_arena_free(&a);
    }
    return n;
}
