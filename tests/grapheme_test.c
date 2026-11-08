/*
 * Grapheme clusters: where a character ends, for the caret, backspace and the mouse. The rules (UAX #29, with Hangul jamo and the Indic
 * conjunct rule GB9c) are implemented in src/text.c as a state machine; the expected boundaries here come from the `regex` module's
 * own segmentation (\X) of 960 strings: hand-picked ones (a conjunct, a decomposed Hangul syllable, flags, family emoji, a prepended
 * sign, Tamil and Kannada, which have no conjuncts) and random strings over an alphabet of everything the rules look at. Each is checked
 * as the state machine alone, then in the widget three ways (forwards, backwards, and the caret stops of a laid-out row), which must
 * all give the same boundaries, or the mouse could put the caret where the keyboard never can. tools/gen_grapheme.py makes the data.
 */
#include "../src/internal.h"

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

typedef struct { const char *s; size_t len; size_t n; size_t ends[24]; } Case;
static const Case cases[] = {
#include "grapheme_cases.inc"
};

static void show(char *buf, size_t cap, const char *s, size_t len) {
    size_t o = 0;
    for (size_t i = 0; i < len && o + 8 < cap; i++) o += (size_t)snprintf(buf + o, cap - o, "%02x ", (unsigned char)s[i]);
    buf[o] = 0;
}

static size_t cp_len(const char *s, size_t len, size_t i, uint32_t *cp) {
    unsigned char c = (unsigned char)s[i];
    size_t n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    uint32_t v = n == 1 ? c : n == 2 ? c & 0x1f : n == 3 ? c & 0x0f : c & 0x07;
    for (size_t k = 1; k < n && i + k < len; k++) v = (v << 6) | ((unsigned char)s[i + k] & 0x3f);
    *cp = v;
    return n;
}

/* the state machine alone */
static void machine(const Case *c, size_t *out, size_t *nout) {
    *nout = 0;
    gp_gstate g;
    size_t i = 0;
    bool first = true;
    while (i < c->len) {
        uint32_t cp;
        size_t n = cp_len(c->s, c->len, i, &cp);
        if (first) { gp_gstate_start(&g, cp); first = false; }
        else if (!gp_gstate_joins(&g, cp)) out[(*nout)++] = i;
        i += n;
    }
    if (c->len) out[(*nout)++] = c->len;
}

static bool same(const size_t *a, size_t na, const size_t *b, size_t nb) {
    if (na != nb) return false;
    for (size_t i = 0; i < na; i++) if (a[i] != b[i]) return false;
    return true;
}

static gp_node *show_input(void *self, gp_ctx *cx) { return gp_div(gp_child_view(*(gp_entity *)self)); }

static void ready(gp_app *app, void *userdata) {
    size_t ncases = sizeof cases / sizeof *cases, bad_machine = 0, bad_fwd = 0, bad_back = 0, bad_stops = 0, widget_cases = 0;
    gp_entity input = gp_text_input_new(app, &(gp_text_input_options){ .multiline = true, .rows = 2 });
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_open_window(app, &(gp_window_options){ .title = "grapheme_test", .width = 300, .height = 100 }, gp_view_new(app, sizeof input, &input, show_input, NULL));
    for (size_t k = 0; k < ncases; k++) {
        const Case *c = &cases[k];
        size_t got[64], ng;
        char shown[256];
        machine(c, got, &ng);
        if (!same(got, ng, c->ends, c->n)) {
            bad_machine++;
            show(shown, sizeof shown, c->s, c->len);
            CHECK(false, "state machine, case %zu [%s]: %zu clusters, wanted %zu (first end %zu vs %zu)", k, shown, ng, c->n, ng ? got[0] : 0, c->n ? c->ends[0] : 0);
        }
        if (memchr(c->s, '\n', c->len)) continue;   /* (a line break is a row boundary in the widget, not a stop) */
        widget_cases++;
        gp_text_input_set_text(&cx, input, c->s);
        /* forwards: the ends */
        ng = gp_text_input_debug_clusters(app, input, 0, got, 64);
        if (!same(got, ng, c->ends, c->n)) { bad_fwd++; show(shown, sizeof shown, c->s, c->len); CHECK(false, "char_next, case %zu [%s]", k, shown); }
        /* backwards: the starts, last first */
        size_t want[64];
        for (size_t i = 0; i < c->n; i++) want[i] = c->n - 1 - i ? c->ends[c->n - 2 - i] : 0;
        ng = gp_text_input_debug_clusters(app, input, 1, got, 64);
        if (!same(got, ng, want, c->n)) { bad_back++; show(shown, sizeof shown, c->s, c->len); CHECK(false, "char_prev, case %zu [%s]: %zu starts, wanted %zu", k, shown, ng, c->n); }
        /* the stops of a laid-out row: 0, then the ends */
        size_t stops[65];
        stops[0] = 0;
        for (size_t i = 0; i < c->n; i++) stops[i + 1] = c->ends[i];
        ng = gp_text_input_debug_clusters(app, input, 2, got, 64);
        if (!same(got, ng, stops, c->n + 1)) { bad_stops++; show(shown, sizeof shown, c->s, c->len); CHECK(false, "build_line stops, case %zu [%s]: %zu, wanted %zu", k, shown, ng, c->n + 1); }
    }
    CHECK(ncases > 900 && widget_cases > 800, "%zu cases, %zu through the widget", ncases, widget_cases);
    printf("  (%zu strings; wrong: machine %zu, char_next %zu, char_prev %zu, caret stops %zu)\n", ncases, bad_machine, bad_fwd, bad_back, bad_stops);
    gp_quit(app);
}

int main(void) {
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "grapheme_test: %d FAILURE(S)\n" : "grapheme_test: all checks passed\n", failures);
    return failures != 0;
}
