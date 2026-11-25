/*
 * COLR color fonts: colr.c draws FreeType's paint graph into the color atlas. The font
 * (tests/colr_fixture.h, made by tools/gen_colr_font.py) has one glyph per feature, each on the em
 * square: layers with alpha, linear (with p2 rotating it), radial and sweep gradients, a rotation,
 * dest-out and multiply composites, a skew, a glyph with no color, a COLRv0 glyph, and one half in the foreground color; and a
 * second palette, marked for dark backgrounds. At 100 px a font unit is 0.1 px, so the em is (0, -100)..(100, 0) in pixels from
 * the origin, y down. Then an sbix font (tests/sbix_fixture.h, made by tools/gen_sbix_font.py). No display needed.
 */
#include "../src/internal.h"
#include "colr_fixture.h"
#include "sbix_fixture.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;
#define CHECK(cond, ...)                                \
    do {                                                \
        if(!(cond)) {                                   \
            failures++;                                 \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while(0)

static gp_text_system* ts;
static gp_glyph_pos g[16];
static gp_glyph_entry ge;
static const uint8_t* atlas;

static void glyph(int i, float size)
{
    ge = gp_text_glyph(ts, g[i].font, g[i].glyph, size);
    atlas = gp_text_color_atlas(ts);
}
/* the atlas pixel at (x, y) pixels from the glyph's origin (y down from the baseline), as RGBA */
static const uint8_t* at(float x, float y)
{
    int ax = (int)ge.u + (int)floorf(x - ge.left), ay = (int)ge.v + (int)floorf(y + ge.top);
    if(!ge.color || ax < (int)ge.u || ay < (int)ge.v || ax >= (int)(ge.u + ge.w)
        || ay >= (int)(ge.v + ge.h)) {
        return (const uint8_t*)"\0\0\0\0";
    }
    return atlas + ((size_t)ay * GP_COLOR_ATLAS_SIZE + (size_t)ax) * 4;
}
static bool rgba(const uint8_t* p, int r, int g, int b, int a)
{
    return abs(p[3] - a) < 8 && abs(p[0] - r) < 8 && abs(p[1] - g) < 8 && abs(p[2] - b) < 8;
}
static bool clear(const uint8_t* p)
{
    return p[3] < 8;
}

int main(void)
{
    ts = gp_text_system_new();
    CHECK(
        ts && gp_text_add_font_memory_as(ts, fx_colrfont, sizeof fx_colrfont, "GpColrTest", 400, 0),
        "the font loads");
    if(!ts) {
        return 1;
    }
    int n = gp_text_shape(
        ts, GP_FONT_SEL(gp_text_family(ts, "GpColrTest"), 400), 100.f, "ABCDEFGHZVW", 11, g, 16, NULL);
    CHECK(n == 11 && g[0].glyph == 1 && g[10].glyph == 11, "ABCDEFGHZVW shapes to glyphs 1..11 (%d)", n);
    if(n != 11) {
        return 1;
    }

    glyph(0, 100.f); /* A: layers */
    CHECK(
        ge.color && fabsf(ge.left) <= 1 && fabsf(ge.top - 100.f) <= 1 && ge.w >= 99 && ge.w <= 101,
        "A is a color glyph on the em (%d: %.0f, %.0f, %.0f x %.0f)",
        ge.color,
        (double)ge.left,
        (double)ge.top,
        (double)ge.w,
        (double)ge.h);
    CHECK(rgba(at(25, -50), 255, 0, 0, 255), "its left half is red");
    CHECK(
        rgba(at(75, -50), 0, 0, 128, 128), "its right half is blue at half alpha (premultiplied)");

    glyph(1, 100.f); /* B: linear, along x once p2 has turned it */
    const uint8_t *top = at(50, -90), *bottom = at(50, -10);
    CHECK(rgba(at(5, -50), 242, 0, 13, 255) && rgba(at(95, -50), 13, 0, 242, 255),
        "B goes from red on the left to blue on the right");
    CHECK(rgba(top, 128, 0, 128, 255) && rgba(bottom, 128, 0, 128, 255),
        "and is the same mix at the middle's top and bottom: p2 makes it run along x (%d,%d)",
        top[2],
        bottom[2]);

    glyph(2, 100.f); /* C: radial, with a clip box */
    CHECK(ge.w >= 99 && ge.w <= 101 && ge.h >= 99 && ge.h <= 101,
        "C's box is its clip box (%.0f x %.0f)",
        (double)ge.w,
        (double)ge.h);
    CHECK(rgba(at(50, -50), 255, 0, 0, 255), "C is red at the center");
    CHECK(rgba(at(75, -50), 128, 0, 128, 255) && rgba(at(50, -25), 128, 0, 128, 255),
        "half way out, an even mix");
    CHECK(rgba(at(2, -2), 0, 0, 255, 255), "past the end circle, blue (padded)");

    glyph(3, 100.f); /* D: sweep, counter-clockwise from +x */
    CHECK(rgba(at(90, -51), 255, 0, 0, 255), "D is red just above the +x axis (%d, %d)", at(90, -51)[0], at(90, -51)[2]);
    CHECK(rgba(at(90, -49), 0, 0, 255, 255), "blue just below it");
    CHECK(rgba(at(50, -90), 191, 0, 64, 255), "and a quarter of the way at the top (%d, %d)", at(50, -90)[0], at(50, -90)[2]);

    glyph(4, 100.f); /* E: rotated 90 degrees */
    CHECK(rgba(at(50, -25), 255, 0, 0, 255) && clear(at(50, -75)),
        "E's left half turned a quarter counter-clockwise is the bottom half");

    glyph(5, 100.f); /* F: dest-out */
    CHECK(
        clear(at(50, -50)) && rgba(at(5, -5), 255, 0, 0, 255) && rgba(at(50, -85), 255, 0, 0, 255),
        "F is red with the circle cut out");

    glyph(6, 100.f); /* G: multiply */
    CHECK(rgba(at(50, -50), 128, 128, 0, 255), "G, yellow times gray, is olive");

    glyph(7, 100.f); /* H: skew */
    CHECK(ge.left <= -99 && ge.left >= -101, "H's box reaches left to where the bar leans (%.0f)", (double)ge.left);
    CHECK(
        rgba(at(-80, -90), 255, 0, 0, 255) && clear(at(10, -90)) && rgba(at(5, -5), 255, 0, 0, 255),
        "its bar leans left by its height");

    glyph(8, 100.f); /* Z: no color glyph */
    CHECK(!ge.color && ge.w >= 99, "Z, with no color glyph, is its outline");

    glyph(9, 100.f); /* V: COLRv0 layers */
    CHECK(ge.color && fabsf(ge.left) <= 1 && ge.w >= 99 && ge.w <= 101 && rgba(at(25, -50), 255, 0, 0, 255) && rgba(at(75, -50), 0, 0, 255, 255),
        "V, a COLRv0 glyph, is red on the left and blue on the right, on the em (%d, %.0f x %.0f)",
        ge.color,
        (double)ge.w,
        (double)ge.h);

    glyph(0, 20.f);
    CHECK(ge.color && ge.w >= 19 && ge.w <= 21 && rgba(at(5, -10), 255, 0, 0, 255)
              && rgba(at(15, -10), 0, 0, 128, 128),
        "at 20 px A is 20 px wide and the same colors (%.0f)",
        (double)ge.w);

    glyph(10, 100.f); /* W: the foreground color */
    CHECK(rgba(at(25, -50), 0, 0, 0, 255) && rgba(at(75, -50), 255, 0, 0, 255), "W's foreground half is black by default");
    ge = gp_text_glyph_tinted(ts, g[10].font, g[10].glyph, 100.f, 0, 0, (gp_color){ 0, 1, 0, 1 });
    gp_glyph_entry green = ge;
    CHECK(rgba(at(25, -50), 0, 255, 0, 255) && rgba(at(75, -50), 255, 0, 0, 255), "in green text it is green there, and still red beside it");
    ge = gp_text_glyph_tinted(ts, g[10].font, g[10].glyph, 100.f, 0, 0, (gp_color){ 0, 0, 1, .5f });
    CHECK(rgba(at(25, -50), 0, 0, 128, 128) && ge.u != green.u, "in half-transparent blue text, that (premultiplied), drawn again");
    ge = gp_text_glyph_tinted(ts, g[10].font, g[10].glyph, 100.f, 0, 0, (gp_color){ 0, 1, 0, 1 });
    CHECK(ge.u == green.u && ge.v == green.v, "green again is the cached one");
    ge = gp_text_glyph_tinted(ts, g[0].font, g[0].glyph, 100.f, 0, 0, (gp_color){ 0, 1, 0, 1 });
    CHECK(rgba(at(25, -50), 255, 0, 0, 255), "A, with no foreground, is not tinted");

    static const struct
    {
        int palette;
        bool green;
        const char* what;
    } pals[] = {
        {  GP_PALETTE_DARK,  true, "GP_PALETTE_DARK picks the palette marked for dark backgrounds" },
        { GP_PALETTE_LIGHT, false,                       "GP_PALETTE_LIGHT the one for light ones" },
        {                1,  true,                                   "an index picks that palette" },
        {                7, false,                               "one the font lacks is its first" },
        {                0, false,                                                "0 is the first" },
    };
    for(size_t i = 0; i < sizeof pals / sizeof pals[0]; i++) {
        gp_text_set_palette(ts, pals[i].palette);
        glyph(0, 100.f);
        CHECK(rgba(at(25, -50), pals[i].green ? 0 : 255, pals[i].green ? 255 : 0, 0, 255), "%s", pals[i].what);
    }

    CHECK(gp_text_add_font_memory_as(ts, fx_sbixfont, sizeof fx_sbixfont, "GpSbixTest", 400, 0), "the sbix font loads");
    gp_glyph_pos sg[2];
    n = gp_text_shape(ts, GP_FONT_SEL(gp_text_family(ts, "GpSbixTest"), 400), 32.f, "A", 1, sg, 2, NULL);
    CHECK(n == 1, "its A shapes");
    if(n == 1) {
        ge = gp_text_glyph(ts, sg[0].font, sg[0].glyph, 32.f);
        atlas = gp_text_color_atlas(ts);
        CHECK(ge.color && ge.w == 32 && ge.h == 32 && fabsf(ge.left) <= 1 && fabsf(ge.top - 32.f) <= 1,
            "an sbix glyph at 32 px is its 64 px bitmap halved, on the baseline (%d: %.0f, %.0f, %.0f x %.0f)",
            ge.color,
            (double)ge.left,
            (double)ge.top,
            (double)ge.w,
            (double)ge.h);
        CHECK(rgba(at(8, -16), 255, 0, 0, 255) && rgba(at(24, -16), 0, 0, 255, 255), "red on the left, blue on the right");
        CHECK(gp_text_glyph_at(ts, sg[0].font, sg[0].glyph, 32.f, 2).u == ge.u, "with one shape at every quarter pixel");
    }

    gp_text_system_free(ts);
    printf(failures ? "colr_test: %d FAILURE(S)\n" : "colr_test: all checks passed\n", failures);
    return failures != 0;
}
