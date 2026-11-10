/*
 * Font families and weights through the whole pipeline: gp_font_family / gp_font_weight on elements, how they
 * inherit, and what happens to cached views and layout when a parent's font changes. Needs a display.
 * The families come from the Noto files this machine has; without them the test says so and skips.
 */
#include "../src/internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures, skips;
#define CHECK(cond, ...)                                                               \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            if (failures++ < 30) {                                                     \
                printf("FAIL %s:%d: ", __FILE__, __LINE__);                            \
                printf(__VA_ARGS__);                                                   \
                printf("\n");                                                          \
            }                                                                          \
        }                                                                              \
    } while (0)

static gp_app *app;
static gp_co *co_;
static gp_window *win(void) { return app->windows[0]; }

static bool wait_draws(unsigned n) {
    unsigned long target = app->stat_draws + n;
    for (int i = 0; i < 1000 && app->stat_draws < target; i++) gp_sleep(co_, 0.004);
    return app->stat_draws >= target;
}

typedef struct {
    const char *family;      /* on the outer box; NULL = none */
    int weight;              /* 0 = none */
    bool italic;
    const char *features;    /* on the cached view's box */
    gp_entity kid;           /* a cached view under the outer box */
    int kid_renders;
} Fix;
static void notify_root(void) {
    gp_ctx cx = gp_ctx_for(app, win()->root_view);
    gp_notify(&cx);
}
static Fix *fix_of(void) { return gp_entity_get(app, win()->root_view); }

/* what the fixture draws: letters, and digits for the features that change them */
#define TXT "Weights 1011"

static gp_node *kid_render(void *self, gp_ctx *cx) {
    Fix *f = fix_of();
    if (f) f->kid_renders++;
    return gp_div(gp_text(TXT, gp_text_size(20)));
}

/* Text elements in tree order:
 *  0 plain                         1 weight 300                  2 bold            3 bold, then normal inside
 *  4 serif                         5 serif > mono                6 serif > weight 900
 *  7 "Zzz No Such, Noto Serif"     8 weight 5000                 9 weight -3
 * 10 a box whose family/weight the test changes
 * 11 the text of a cached view in a box of its own, with the same family/weight (so the view alone decides how wide the box is)
 * 12 gp_font_mono() 13 gp_font_serif() 14 gp_font_sans()
 * 17 italic   18 italic, then upright inside   19 monospace (no italic face: slanted) italic
 * 20 digits   21 digits, pnum   22 pnum then "normal" inside   23 pnum, then bold inside   24 pnum   25 small caps \"abcabc\"   26 plain \"abcabc\"
 * 15 DejaVu Sans Mono (line metrics unlike the Noto fonts')   16 wrapping text in a narrow box, monospace */
static gp_node *render(void *self, gp_ctx *cx) {
    Fix *f = self;
    #define T(...) gp_text(TXT, gp_text_size(20), ##__VA_ARGS__)
    return gp_div(
        gp_flex_col(), gp_items_start(),   /* every box is as wide as its text */
        T(),
        gp_div(gp_font_weight(300), T()),
        gp_div(gp_font_bold(), T()),
        gp_div(gp_font_bold(), gp_div(gp_font_normal(), T())),
        gp_div(gp_font_family("Noto Serif"), T()),
        gp_div(gp_font_family("Noto Serif"), gp_div(gp_font_family("Noto Sans Mono"), T())),
        gp_div(gp_font_family("Noto Serif"), gp_div(gp_font_weight(900), T())),
        gp_div(gp_font_family("Zzz No Such, Noto Serif"), T()),
        gp_div(gp_font_weight(5000), T()),
        gp_div(gp_font_weight(-3), T()),
        gp_div(f->family ? gp_font_family(f->family) : NULL, f->weight ? gp_font_weight(f->weight) : NULL, f->italic ? gp_font_italic() : NULL, f->features ? gp_font_features(f->features) : NULL, T()),
        gp_div(f->family ? gp_font_family(f->family) : NULL, f->weight ? gp_font_weight(f->weight) : NULL, f->italic ? gp_font_italic() : NULL, f->features ? gp_font_features(f->features) : NULL,
               gp_child_view_cached(f->kid)),
        gp_div(gp_font_mono(), T()),
        gp_div(gp_font_serif(), T()),
        gp_div(gp_font_sans(), T()),
        gp_div(gp_font_family("DejaVu Sans Mono"), T()),
        gp_div(gp_w(150), gp_font_family("Noto Sans Mono"), gp_text("Weight Weight Weight Weight", gp_text_size(20))),
        gp_div(gp_font_italic(), T()),
        gp_div(gp_font_italic(), gp_div(gp_font_upright(), T())),
        gp_div(gp_font_family("Noto Sans Mono"), gp_font_italic(), T()),
        gp_text("1011", gp_text_size(20)),
        gp_div(gp_font_features("pnum"), gp_text("1011", gp_text_size(20))),
        gp_div(gp_font_features("pnum"), gp_div(gp_font_features("normal"), gp_text("1011", gp_text_size(20)))),
        gp_div(gp_font_features("pnum"), gp_div(gp_font_weight(700), gp_text("1011", gp_text_size(20)))),
        gp_div(gp_font_features("pnum"), T()),
        gp_div(gp_font_small_caps(), gp_text("abcabc", gp_text_size(20))),
        gp_text("abcabc", gp_text_size(20)));
    #undef T
}

static const gp_el *texts[32];
enum { NTEXTS = 27 };
static int ntexts;
static void collect_texts(const gp_el *n) {
    if (n->kind == GP_EL_TEXT && ntexts < 32) texts[ntexts++] = n;
    if (n->kind == GP_EL_LIST) return;
    for (uint32_t i = 0; i < n->nchildren; i++) collect_texts(n->children[i]);
}
static void gather(void) { ntexts = 0; collect_texts(win()->tree); }

static float width_of(int i) { return texts[i]->size.w; }

static void run(void);
/* ---- what was painted: the glyphs drawn for a text are the ones of its family and weight, put where its font's metrics say */

static bool abs_pos(const gp_el *n, const gp_el *target, float x, float y, float *ox, float *oy) {
    x += n->pos.x;
    y += n->pos.y;
    if (n == target) { *ox = x; *oy = y; return true; }
    for (uint32_t i = 0; i < n->nchildren; i++)
        if (abs_pos(n->children[i], target, x, y, ox, oy)) return true;
    return false;
}

/* Lines painted inside the box of text element i, when every line of it starts with the same glyph: the glyphs
 * at the leftmost x. */
static int painted_lines(int i) {
    float ax = 0.f, ay = 0.f;
    if (!abs_pos(win()->tree, texts[i], -win()->tree->pos.x, -win()->tree->pos.y, &ax, &ay)) return -1;
    const gp_scene *sc = &win()->scene;
    float minx = 1e9f;
    for (size_t k = 0; k < sc->len; k++) {
        const gp_instance *in = &sc->items[k];
        if (in->kind == GP_PRIM_GLYPH && in->bounds[1] >= ay - 1 && in->bounds[1] < ay + texts[i]->size.h && in->bounds[0] < minx) minx = in->bounds[0];
    }
    int n = 0;
    for (size_t k = 0; k < sc->len; k++) {
        const gp_instance *in = &sc->items[k];
        if (in->kind == GP_PRIM_GLYPH && in->bounds[1] >= ay - 1 && in->bounds[1] < ay + texts[i]->size.h && fabsf(in->bounds[0] - minx) < 0.5f) n++;
    }
    return n;
}

/* Every glyph of text element i is in the scene with the atlas cell and position its own family and weight give. */
static int check_glyphs(int i, const char *text, int *found_out) {
    const gp_el *n = texts[i];
    float ax = 0.f, ay = 0.f;
    if (!abs_pos(win()->tree, n, -win()->tree->pos.x, -win()->tree->pos.y, &ax, &ay)) return -1;
    gp_text_system *ts = app->text;
    int64_t sel = GP_TEXT_SEL(n->ts);
    gp_glyph_pos g[48];
    float width;
    int ng = gp_text_shape(ts, sel, n->ts.size, text, strlen(text), g, 48, &width);
    float asc, desc;
    gp_text_metrics(ts, sel, n->ts.size, &asc, &desc);
    float scale = win()->scale, lh = n->ts.size * GP_LINE_HEIGHT, px = n->ts.size * scale;
    float baseline = ay + (lh - (asc + desc)) / 2 + asc;
    const gp_scene *sc = &win()->scene;
    int missing = 0, found = 0;
    for (int k = 0; k < ng; k++) {
        int pixel, bin;
        gp_text_snap_x(ts, g[k].font, px, (ax + g[k].x) * scale, &pixel, &bin);
        gp_glyph_entry e = gp_text_glyph_at(ts, g[k].font, g[k].glyph, px, bin);
        if (!e.w) continue;
        float gx = ((float)pixel + e.left) / scale, gy = (roundf((baseline + g[k].y) * scale) - e.top) / scale;
        float u = (float)e.u / GP_ATLAS_SIZE, v = (float)e.v / GP_ATLAS_SIZE;
        bool hit = false;
        for (size_t m = 0; m < sc->len && !hit; m++) {
            const gp_instance *in = &sc->items[m];
            hit = in->kind == GP_PRIM_GLYPH && fabsf(in->uv[0] - u) < 1e-6f && fabsf(in->uv[1] - v) < 1e-6f &&
                  fabsf(in->bounds[0] - gx) < 0.01f && fabsf(in->bounds[1] - gy) < 0.01f;
        }
        if (hit) found++; else missing++;
    }
    *found_out = found;
    return missing;
}

static void check_painted(void) {
    static const int idx[] = { 0, 1, 2, 4, 5, 6, 7, 12, 13, 17, 18, 19, 24 };
    for (size_t k = 0; k < sizeof idx / sizeof *idx; k++) {
        int found;
        int missing = check_glyphs(idx[k], TXT, &found);
        CHECK(missing == 0 && found > 0, "text %d (family %d, weight %d): %d of its glyphs are not in the scene where its font puts them (%d are)",
              idx[k], texts[idx[k]]->ts.family, texts[idx[k]]->ts.weight, missing, found);
    }
    for (int i = 20; i <= 23; i++) {
        int found;
        int missing = check_glyphs(i, "1011", &found);
        CHECK(missing == 0 && found > 0, "digits %d (features %d, weight %d): %d glyphs are not painted as shaped (%d are)", i, texts[i]->ts.features, texts[i]->ts.weight, missing, found);
    }
    if (access("/usr/share/fonts/TTF/DejaVuSansMono.ttf", R_OK) == 0 || access("/usr/share/fonts/dejavu/DejaVuSansMono.ttf", R_OK) == 0) {
        int found;
        int missing = check_glyphs(15, TXT, &found);
        CHECK(missing == 0 && found > 0, "DejaVu Sans Mono sits on its own baseline (%d glyphs off, %d right)", missing, found);
    }
    /* wrapped: the lines painted are the lines laid out */
    float lh = texts[16]->ts.size * GP_LINE_HEIGHT;
    int laid = (int)lroundf(texts[16]->size.h / lh), painted = painted_lines(16);
    CHECK(laid == 4 && painted == laid, "wrapped monospace text: %d lines laid out (want 4), %d painted", laid, painted);
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    run();
    gp_quit(app);
}

static void run(void) {
    wait_draws(2);
    gather();
    CHECK(ntexts == NTEXTS, "found %d text elements, wanted %d", ntexts, NTEXTS);
    if (ntexts != NTEXTS) return;
    gp_text_system *ts = app->text;
    int serif = gp_text_family(ts, "Noto Serif");
    int mono = gp_text_family(ts, "Noto Sans Mono");

    /* what each element inherited */
    CHECK(texts[0]->ts.weight == 400 && texts[0]->ts.family == 0, "plain text: 400, default family (%d, %d)", texts[0]->ts.weight, texts[0]->ts.family);
    CHECK(texts[1]->ts.weight == 300, "gp_font_weight(300) -> %d", texts[1]->ts.weight);
    CHECK(texts[2]->ts.weight == 700, "gp_font_bold -> %d", texts[2]->ts.weight);
    CHECK(texts[3]->ts.weight == 400, "normal inside bold -> %d", texts[3]->ts.weight);
    CHECK(texts[4]->ts.family == serif && texts[4]->ts.weight == 400, "serif box");
    CHECK(texts[5]->ts.family == mono, "a nested family replaces the outer one");
    CHECK(texts[6]->ts.family == serif && texts[6]->ts.weight == 900, "a nested weight keeps the outer family");
    CHECK(texts[7]->ts.family == gp_text_family(ts, "Zzz No Such, Noto Serif"), "a list is one family value");
    CHECK(texts[8]->ts.weight == 1000, "weights clamp to 1000 (%d)", texts[8]->ts.weight);
    CHECK(texts[9]->ts.weight == 1, "and to 1 (%d)", texts[9]->ts.weight);
    CHECK(texts[12]->ts.family == gp_text_family(ts, "monospace") && texts[13]->ts.family == gp_text_family(ts, "serif") &&
              texts[14]->ts.family == gp_text_family(ts, "sans-serif"),
          "gp_font_mono / serif / sans");

    CHECK(texts[17]->ts.italic && !texts[18]->ts.italic && texts[19]->ts.italic && !texts[0]->ts.italic, "italic inherits, and upright undoes it");

    CHECK(texts[21]->ts.features != 0 && texts[22]->ts.features == 0 && texts[23]->ts.features == texts[21]->ts.features &&
              texts[20]->ts.features == 0,
          "features inherit, \"normal\" clears them, a bold box keeps them");

    bool noto = access("/usr/share/fonts/noto/NotoSerif-Regular.ttf", R_OK) == 0 &&
                access("/usr/share/fonts/noto/NotoSansMono-Regular.ttf", R_OK) == 0 &&
                access("/usr/share/fonts/noto/NotoSans-Black.ttf", R_OK) == 0 && access("/usr/share/fonts/noto/NotoSans-Light.ttf", R_OK) == 0;
    if (!noto) {
        skips++;
        printf("SKIP: the Noto families are not installed, so widths can't be compared\n");
        return;
    }

    /* and the layout followed: the box around each text is as wide as its font makes it */
    CHECK(width_of(1) < width_of(0) && width_of(0) < width_of(2) && width_of(2) < width_of(8),
          "light < regular < bold < black: %.1f %.1f %.1f %.1f", width_of(1), width_of(0), width_of(2), width_of(8));
    CHECK(width_of(3) == width_of(0), "normal inside bold is the regular width");
    CHECK(width_of(4) != width_of(0) && width_of(5) != width_of(4) && width_of(6) > width_of(4),
          "serif, mono and black serif differ: %.1f %.1f %.1f vs %.1f", width_of(4), width_of(5), width_of(6), width_of(0));
    CHECK(width_of(7) == width_of(4), "an uninstalled first family falls through to the second");
    CHECK(width_of(8) == width_of(2) || width_of(8) > width_of(2), "weight 1000 is at least as wide as bold");
    CHECK(width_of(12) == width_of(5) || width_of(12) != width_of(0), "gp_font_mono is not the default face");

    CHECK(width_of(17) != width_of(0), "an italic face is not the upright one's width (%.1f vs %.1f)", width_of(17), width_of(0));
    CHECK(width_of(18) == width_of(0), "upright inside italic is upright again");
    CHECK(width_of(19) == width_of(5), "slanting a family without an italic keeps its advances (%.1f vs %.1f)", width_of(19), width_of(5));

    CHECK(width_of(21) < width_of(20), "pnum makes the digits narrower (%.1f vs %.1f)", width_of(21), width_of(20));
    CHECK(width_of(22) == width_of(20), "\"normal\" inside is the default again");
    CHECK(width_of(23) != width_of(20) && width_of(23) != width_of(21), "a nested weight keeps the features: bold proportional digits (%.1f)", width_of(23));
    CHECK(width_of(25) < width_of(26), "small caps are narrower than lowercase (%.1f vs %.1f)", width_of(25), width_of(26));

    /* the cached view under a box whose font changes */
    Fix *f = fix_of();
    float kid0 = width_of(11);
    CHECK(kid0 == width_of(0), "the cached view starts with the default font (%.1f vs %.1f)", kid0, width_of(0));
    int renders0 = f->kid_renders;
    f->family = "Noto Serif";
    notify_root();
    wait_draws(2);
    gather();
    CHECK(texts[11]->ts.family == serif, "the cached view's text inherits the new family (%d)", texts[11]->ts.family);
    CHECK(width_of(11) == width_of(4) && width_of(10) == width_of(4), "and was laid out with it (%.1f, serif %.1f)", width_of(11), width_of(4));
    CHECK(width_of(4) != kid0, "(a different width, or this shows nothing)");
    CHECK(f->kid_renders == renders0, "without rendering the cached view again (%d -> %d)", renders0, f->kid_renders);
    f->weight = 900;
    notify_root();
    wait_draws(2);
    gather();
    CHECK(width_of(11) == width_of(6), "weight changes reach it too (%.1f vs %.1f)", width_of(11), width_of(6));
    f->weight = 0;
    f->family = NULL;
    notify_root();
    wait_draws(2);
    gather();
    CHECK(width_of(11) == width_of(0) && texts[11]->ts.family == 0 && texts[11]->ts.weight == 400, "back to the default (%.1f)", width_of(11));
    f->features = "pnum";   /* nothing else changes with it: only the features say the view must be laid out again */
    notify_root();
    wait_draws(2);
    gather();
    CHECK(texts[11]->ts.features == texts[24]->ts.features && width_of(11) == width_of(24) && width_of(24) < width_of(0),
          "features alone reach the cached view (%.1f vs %.1f, default %.1f)", width_of(11), width_of(24), width_of(0));
    f->features = NULL;
    notify_root();
    wait_draws(2);
    gather();
    CHECK(width_of(11) == width_of(0) && texts[11]->ts.features == 0, "and leave it again (%.1f)", width_of(11));
    f->italic = true;
    notify_root();
    wait_draws(2);
    gather();
    CHECK(texts[11]->ts.italic && width_of(11) == width_of(17), "italic reaches the cached view too (%.1f vs %.1f)", width_of(11), width_of(17));
    f->italic = false;
    f->family = NULL;
    notify_root();
    wait_draws(2);
    gather();
    CHECK(width_of(11) == width_of(0) && texts[11]->ts.family == 0 && texts[11]->ts.weight == 400, "and back to the default (%.1f)", width_of(11));

    check_painted();

    /* what is on screen is what a fresh paint without caches draws */
    gp_window *w = win();
    gp_scene fresh = { 0 };
    gp_arena scratch = { 0 };
    gp_paint_tree(&fresh, &scratch, app->text, w->tree, w->scale, w->mouse, w->mouse_in, w->pressed, w->focused, w->drag_key, w->scroll_vis_gen, NULL, NULL);
    CHECK(fresh.len == w->scene.len && memcmp(fresh.items, w->scene.items, fresh.len * sizeof *fresh.items) == 0,
          "the frame on screen is what a paint without caches draws");
    free(fresh.items);
    gp_arena_free(&scratch);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    gp_entity kid = gp_view_new(app, 0, NULL, kid_render, NULL);
    gp_entity v = gp_view_new(app, sizeof(Fix), &(Fix){ .kid = kid }, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "fonts_test", .width = 360, .height = 640 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    setenv("GPUIC_LCD", "off", 1);   /* glyph instances are plain coverage glyphs, whatever the system says about subpixels */
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "fonts_test: %d FAILURE(S)\n" : skips ? "fonts_test: passed with %d skip(s)\n" : "fonts_test: all checks passed\n",
           failures ? failures : skips);
    return failures != 0;
}
