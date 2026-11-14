/*
 * Color fonts with an 'SVG ' table: FreeType loads a glyph's document and the hooks text.c gives it draw it with svgx.c, into the color atlas.
 * The font (tests/svgfont_fixture.h, made by tools/gen_svg_font.py) has 'A' red on its left half and blue on its right, and 'B' a green square
 * cut to a circle by a clip, drawn through a <use> (both in one document, which also holds the other's glyph), and 'C' with no SVG glyph. At
 * 100 px a font unit is 0.1 px: the atlas entries are checked against that geometry. No display needed.
 */
#include "../src/internal.h"
#include "svgfont_fixture.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

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

static const uint8_t *atlas;
static gp_glyph_entry ge;
/* the atlas pixel at (x, y) pixels from the glyph's origin (y down from the baseline), as BGRA */
static const uint8_t *at(float x, float y) {
    int ax = (int)ge.u + (int)floorf(x - ge.left), ay = (int)ge.v + (int)floorf(y + ge.top);
    if (ax < (int)ge.u || ay < (int)ge.v || ax >= (int)(ge.u + ge.w) || ay >= (int)(ge.v + ge.h)) return (const uint8_t *)"\0\0\0\0";
    return atlas + ((size_t)ay * GP_COLOR_ATLAS_SIZE + (size_t)ax) * 4;
}
static bool bgra(const uint8_t *p, int r, int g, int b) { return p[3] > 247 && abs(p[2] - r) < 8 && abs(p[1] - g) < 8 && abs(p[0] - b) < 8; }
static bool clear(const uint8_t *p) { return p[3] < 8; }

int main(void) {
    if (!gp_text_svg_fonts()) {
        printf("SKIP: this FreeType is built without SVG fonts (FT_CONFIG_OPTION_SVG)\nsvgfont_test: all checks passed\n");
        return 0;
    }
    gp_text_system *ts = gp_text_system_new();
    CHECK(ts && gp_text_add_font_memory_as(ts, fx_svgfont, sizeof fx_svgfont, "GpSvgTest", 400, 0), "the font loads");
    if (!ts) return 1;
    int fam = gp_text_family(ts, "GpSvgTest");
    gp_glyph_pos g[8];
    int n = gp_text_shape(ts, GP_FONT_SEL(fam, 400), 100.f, "ABC", 3, g, 8, NULL);
    CHECK(n == 3 && g[0].glyph == 1 && g[1].glyph == 2 && g[2].glyph == 3, "ABC shapes to glyphs 1, 2, 3 of the font (%d: %u %u %u)", n, n > 0 ? g[0].glyph : 0,
          n > 1 ? g[1].glyph : 0, n > 2 ? g[2].glyph : 0);
    if (n != 3) return 1;

    /* A: red 0..30, blue 30..60, from 80 above the baseline down to it */
    ge = gp_text_glyph(ts, g[0].font, g[0].glyph, 100.f);
    atlas = gp_text_color_atlas(ts);
    CHECK(ge.color && ge.w >= 60 && ge.w <= 64 && ge.h >= 80 && ge.h <= 84, "A is a color glyph of about 60 x 80 px (%d: %.0f x %.0f)", ge.color, (double)ge.w, (double)ge.h);
    CHECK(bgra(at(15, -40), 255, 0, 0) && bgra(at(2, -78), 255, 0, 0), "its left half is red");
    CHECK(bgra(at(45, -40), 0, 0, 255) && bgra(at(58, -2), 0, 0, 255), "its right half is blue");
    CHECK(clear(at(15, 3)) && clear(at(15, -83)) && clear(at(62, -40)), "and nothing is drawn outside it");
    CHECK(fabsf(ge.left) <= 1.5f && fabsf(ge.top - 80.f) <= 1.5f, "its box starts at the origin, 80 above the baseline (%.1f, %.1f)", (double)ge.left, (double)ge.top);

    /* B: a green circle of radius 30 at (30, -40): the square cut by the clip */
    ge = gp_text_glyph(ts, g[1].font, g[1].glyph, 100.f);
    atlas = gp_text_color_atlas(ts);
    CHECK(ge.color && bgra(at(30, -40), 0, 255, 0) && bgra(at(30, -68), 0, 255, 0) && bgra(at(3, -40), 0, 255, 0), "B is green inside its circle");
    CHECK(clear(at(4, -74)) && clear(at(56, -6)) && clear(at(56, -74)), "and clear in the square's corners, outside the circle (the clip, through a <use>)");
    /* the circle's edge: every pixel whose center is more than a pixel inside it is covered, more than a pixel outside it is not */
    int bad = 0, inside = 0;
    for (int y = 0; y < (int)ge.h; y++)
        for (int x = 0; x < (int)ge.w; x++) {
            float cx = ge.left + (float)x + 0.5f, cy = -ge.top + (float)y + 0.5f, d = hypotf(cx - 30.f, cy + 40.f);
            const uint8_t *p = atlas + ((size_t)((int)ge.v + y) * GP_COLOR_ATLAS_SIZE + (size_t)((int)ge.u + x)) * 4;
            if (d < 29.f) { inside++; if (p[3] < 248) bad++; }
            else if (d > 31.f && p[3] > 7) bad++;
        }
    CHECK(bad == 0 && inside > 2500, "its edge is where the circle is (%d pixels off, %d inside)", bad, inside);

    /* C has no SVG glyph: its outline, in the coverage atlas */
    ge = gp_text_glyph(ts, g[2].font, g[2].glyph, 100.f);
    CHECK(!ge.color && ge.w >= 59 && ge.h >= 79, "C, with no SVG glyph, is its outline (%d, %.0f x %.0f)", ge.color, (double)ge.w, (double)ge.h);

    /* sizes: at 20 px A is 12 x 16 */
    ge = gp_text_glyph(ts, g[0].font, g[0].glyph, 20.f);
    atlas = gp_text_color_atlas(ts);
    CHECK(ge.color && ge.w >= 12 && ge.w <= 15 && bgra(at(3, -8), 255, 0, 0) && bgra(at(9, -8), 0, 0, 255), "at 20 px A is 12 px wide and still red and blue (%.0f)", (double)ge.w);

    gp_text_system_free(ts);
    printf(failures ? "svgfont_test: %d FAILURE(S)\n" : "svgfont_test: all checks passed\n", failures);
    return failures != 0;
}
