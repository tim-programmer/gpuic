/*
 * gp_font_variations through layout: the width of the same text with an axis set (optical size, on a font that has one: Adwaita Sans),
 * inheritance, "normal" putting the axes back, a list that names axes the font lacks, a weight in the list, and a view that is laid
 * out again when only the list changed. The widths come from FreeType set to the same coordinates (see text_test for the arithmetic). Needs a
 * display and /usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf (the test passes with a note if it is not installed).
 */
#include "../src/internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

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
static gp_window *win(void) { return app->windows[0]; }

typedef struct { const char *vars; gp_entity inner; } Fix;

static gp_node *inner_render(void *self, gp_ctx *cx);

#define T() gp_text("HHHH", gp_text_size(24), gp_font_family("Adwaita Sans"))

static gp_node *render(void *self, gp_ctx *cx) {
    Fix *f = self;
    return gp_div(gp_flex_col(), gp_items_start(), gp_p(10),
                  gp_div(T()),                                                                                   /* 0: the default */
                  gp_div(gp_font_variations("opsz 32"), T()),                                                    /* 1 */
                  gp_div(gp_font_variations("opsz 32"), gp_div(T())),                                            /* 2: inherited */
                  gp_div(gp_font_variations("opsz 32"), gp_div(gp_font_variations("normal"), T())),              /* 3: put back */
                  gp_div(gp_font_variations("wdth 75"), T()),                                                    /* 4: an axis it lacks */
                  gp_div(gp_font_variations("opsz 32"), gp_font_weight(700), T()),                               /* 5 */
                  gp_div(gp_font_variations("opsz 32, wght 300"), gp_font_weight(700), T()),                     /* 6: the list's weight wins */
                  gp_div(gp_font_variations("opsz 32"), gp_div(gp_font_weight(300), T())),                       /* 7: a weight inside keeps the axis */
                  gp_div(f->vars ? gp_font_variations(f->vars) : NULL, T()),                                     /* 8: the one the test changes */
                  gp_div(f->vars ? gp_font_variations(f->vars) : NULL, gp_child_view_cached(f->inner)));         /* 9: a cached view below it */
}

static gp_node *inner_render(void *self, gp_ctx *cx) { return gp_div(T()); }

static bool wait_draws(unsigned n) {
    unsigned long target = app->stat_draws + n;
    for (int i = 0; i < 1000 && app->stat_draws < target; i++) gp_sleep(co_, 0.004);
    return app->stat_draws >= target;
}

/* the text element of box i, and its width */
static float width_of(int i) {
    const gp_el *n = win()->tree->children[i];
    while (n->kind != GP_EL_TEXT && n->nchildren) n = n->children[0];
    return n->size.w;
}
static const gp_el *text_of(int i) {
    const gp_el *n = win()->tree->children[i];
    while (n->kind != GP_EL_TEXT && n->nchildren) n = n->children[0];
    return n;
}

/* the advance of HHHH with the axes set on a face of its own */
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
static float reference(float opsz, float wght) {
    FT_Library lib;
    FT_Face face;
    FT_Init_FreeType(&lib);
    FT_New_Face(lib, "/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf", 0, &face);
    FT_MM_Var *mm;
    FT_Get_MM_Var(face, &mm);
    FT_Fixed co[16];
    FT_Get_Var_Design_Coordinates(face, mm->num_axis, co);
    for (FT_UInt i = 0; i < mm->num_axis; i++) {
        if (mm->axis[i].tag == FT_MAKE_TAG('o', 'p', 's', 'z') && opsz > 0) co[i] = (FT_Fixed)(opsz * 65536.f);
        if (mm->axis[i].tag == FT_MAKE_TAG('w', 'g', 'h', 't')) co[i] = (FT_Fixed)wght << 16;
    }
    FT_Set_Var_Design_Coordinates(face, mm->num_axis, co);
    FT_Load_Glyph(face, FT_Get_Char_Index(face, 'H'), FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING);
    float w = 4.f * (float)face->glyph->metrics.horiAdvance * 24.f / (float)face->units_per_EM;
    FT_Done_MM_Var(lib, mm);
    FT_Done_Face(face);
    FT_Done_FreeType(lib);
    return w;
}
/* (layout rounds a text's width up to a whole pixel) */
static bool near(float got, float ref) { return fabsf(got - ceilf(ref - 0.001f)) < 0.01f; }

static void run(Fix *f) {
    wait_draws(2);
    float def = reference(0, 400), o32 = reference(32, 400), o32b = reference(32, 700), o32l = reference(32, 300);
    CHECK(ceilf(def) != ceilf(o32) && ceilf(o32b) != ceilf(o32l), "(the font's optical size changes the width: %.2f vs %.2f; %.2f vs %.2f)", def, o32, o32b, o32l);
    CHECK(near(width_of(0), def), "default: %.2f, FreeType %.2f", width_of(0), def);
    CHECK(near(width_of(1), o32), "opsz 32: %.2f, FreeType %.2f", width_of(1), o32);
    CHECK(near(width_of(2), o32) && text_of(2)->ts.variations == text_of(1)->ts.variations && text_of(2)->ts.variations != 0, "inherited by a nested box");
    CHECK(near(width_of(3), def) && text_of(3)->ts.variations == 0, "\"normal\" puts the axes back: %.2f", width_of(3));
    CHECK(near(width_of(4), def), "an axis the font has not changes nothing: %.2f", width_of(4));
    CHECK(near(width_of(5), o32b), "opsz 32 with font-weight 700: %.2f, FreeType %.2f", width_of(5), o32b);
    CHECK(near(width_of(6), o32l), "a weight in the list wins over font-weight: %.2f, FreeType (opsz 32, wght 300) %.2f", width_of(6), o32l);
    CHECK(near(width_of(7), o32l), "a weight set inside keeps the axis set outside: %.2f", width_of(7));
    CHECK(near(width_of(8), def), "no list yet: %.2f", width_of(8));
    /* only the list changes: the view is built again (it is the view's state) and laid out with the new text style */
    f->vars = "opsz 32";
    gp_view_dirty(app, win()->root_view);
    gp_ctx cx = gp_ctx_for(app, win()->root_view);
    gp_notify(&cx);
    wait_draws(2);
    CHECK(near(width_of(8), o32), "a view whose list changed: %.2f, wanted %.2f", width_of(8), o32);
    f->vars = "normal";
    gp_notify(&cx);
    wait_draws(2);
    CHECK(near(width_of(8), def), "and back: %.2f", width_of(8));
    CHECK(near(width_of(9), def), "a cached view below, before: %.2f", width_of(9));
    f->vars = "opsz 32";
    gp_notify(&cx);
    wait_draws(2);
    CHECK(near(width_of(9), o32), "a cached view is laid out again when only the list above it changed: %.2f, wanted %.2f", width_of(9), o32);
    f->vars = NULL;
    gp_notify(&cx);
    wait_draws(2);
    CHECK(near(width_of(9), def), "and again when it goes: %.2f", width_of(9));
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    Fix *fx = gp_entity_get(app, win()->root_view);
    run(fx);
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    gp_font_add_file(app, "/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf", &(gp_font_options){ .family = "Adwaita Sans" });
    Fix init = { .vars = NULL, .inner = gp_view_new(app, 1, NULL, inner_render, NULL) };
    gp_entity v = gp_view_new(app, sizeof init, &init, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "variations_test", .width = 300, .height = 400 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    if (!test_file_exists("/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf")) {
        printf("variations_test: skipped, Adwaita Sans is not installed\n");
        return 0;
    }
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "variations_test: %d FAILURE(S)\n" : "variations_test: all checks passed\n", failures);
    return failures != 0;
}
