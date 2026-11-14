/*
 * The rest of SVG (svgx.c, svgfx.c), each against something computed here rather than against itself:
 *   CSS            selectors (type, class, id, attribute, compound, child, descendant), specificity and order, inline style winning, at-rules skipped,
 *                  clip-path from a rule;
 *   nesting        a clip inside a clipped group (the intersection), a clip on a clipPath, bounding-box units for clips, mask contents and the mask's
 *                  region, mask-type alpha, group opacity (two overlapping shapes at half: the top one's color at half where they overlap, not
 *                  darker), nested opacity multiplied, a shape's fill and stroke as one;
 *   patterns       tiles where the arithmetic puts them: user space, bounding-box units, patternTransform, viewBox, href, a stroke, a pattern in a pattern;
 *   markers        where and which way: orient auto turns a triangle along the path (its pixels against the turned triangle's), auto-start-reverse,
 *                  mid markers on vertices only, markerUnits, the viewport cut and overflow visible;
 *   images         a PNG data URL against its known quadrants, an SVG one meet and slice, opacity, the size it has itself;
 *   text           text on a straight textPath is the text set there; letter-spacing widens the ink by (n - 1) times it; x lists, rotate, textLength,
 *                  vertical writing, baselines;
 *   filters        the region and subregions, primitiveUnits, morphology, convolution, component transfer, turbulence (values from librsvg, which
 *                  implements the same reference code), lighting on a flat surface, displacement by a flat map, tile, feImage, the blend modes against
 *                  their formulas, CSS filter functions, a primitive in another color space, a list of filters.
 * No display needed.
 */
#include "../src/internal.h"
#include "image_fixtures.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...)                                    \
    do {                                                    \
        if(!(cond)) {                                       \
            if(failures++ < 60) {                           \
                printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                printf(__VA_ARGS__);                        \
                printf("\n");                               \
            }                                               \
        }                                                   \
    } while(0)

#define HEAD "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' width='40' height='40' viewBox='0 0 40 40'>"
#define TAIL "</svg>"

static int W = 40;
static gp_text_system* g_ts;
static uint8_t* draw(const char* doc)
{
    return gp_svg_render(doc, strlen(doc), 0x000000, W, W, 40.f, g_ts);
}
#define P(px, x, y) ((px) + ((size_t)(y) * (size_t)W + (size_t)(x)) * 4)
static bool is_clear(const uint8_t* p)
{
    return p[3] < 8;
}
static bool is_color(const uint8_t* p, int r, int g, int b)
{
    return p[3] > 247 && abs(p[0] - r) < 8 && abs(p[1] - g) < 8 && abs(p[2] - b) < 8;
}
static bool is_red(const uint8_t* p)
{
    return is_color(p, 255, 0, 0);
}
static bool is_blue(const uint8_t* p)
{
    return is_color(p, 0, 0, 255);
}
static bool is_green(const uint8_t* p)
{
    return is_color(p, 0, 255, 0);
}
static bool near(int a, int b, int tol)
{
    return abs(a - b) <= tol;
}
static int differ(const uint8_t* a, const uint8_t* b, int tol)
{
    int bad = 0;
    for(size_t i = 0; i < (size_t)W * (size_t)W; i++) {
        for(int c = 0; c < 4; c++) {
            if(abs((int)a[i * 4 + c] - (int)b[i * 4 + c]) > tol) {
                bad++;
                break;
            }
        }
    }
    return bad;
}
static int vs(const uint8_t* px, const char* ref_doc, int tol)
{
    uint8_t* ref = draw(ref_doc);
    int bad = ref && px ? differ(px, ref, tol) : 99999;
    free(ref);
    return bad;
}
static bool ink_box(const uint8_t* px, int* l, int* t, int* r, int* b)
{
    int x0 = W, y0 = W, x1 = -1, y1 = -1;
    for(int y = 0; y < W; y++) {
        for(int x = 0; x < W; x++) {
            if(P(px, x, y)[3] > 100) {
                if(x < x0) {
                    x0 = x;
                }
                if(x > x1) {
                    x1 = x;
                }
                if(y < y0) {
                    y0 = y;
                }
                if(y > y1) {
                    y1 = y;
                }
            }
        }
    }
    *l = x0;
    *t = y0;
    *r = x1 + 1;
    *b = y1 + 1;
    return x1 >= 0;
}
#define PX(px, x, y) (px ? P(px, x, y)[0] : -1), (px ? P(px, x, y)[1] : -1), (px ? P(px, x, y)[2] : -1), (px ? P(px, x, y)[3] : -1)

/* ---------------------------------------------------------------- CSS */

static void test_css(void)
{
    uint8_t* px = draw(HEAD "<style>rect { fill: #f00 } .b { fill: #00f } #c { fill: #0f0 } g > .d { fill: #0f0 } g .e rect { fill: #00f } [data-x='1'] { fill: #0f0 }"
                            " .p.q { fill: #00f } rect:hover { fill: #0f0 } @media screen { rect { fill: #0f0 } } .late { fill: #0f0 } .late { fill: #00f }</style>"
                            "<rect width='5' height='5'/><rect x='5' class='b' width='5' height='5'/><rect x='10' id='c' class='b' width='5' height='5'/>"
                            "<rect x='15' id='c2' class='b' width='5' height='5' style='fill:#f00'/><rect x='20' fill='#0f0' width='5' height='5'/>"
                            "<g><rect y='10' class='d' width='5' height='5'/><g class='e'><g><rect x='5' y='10' width='5' height='5'/></g></g></g>"
                            "<rect x='10' y='10' data-x='1' width='5' height='5'/><rect x='15' y='10' class='p q' width='5' height='5'/><rect x='20' y='10' class='p' width='5' height='5'/>"
                            "<rect x='25' y='10' class='late' width='5' height='5'/><rect y='20' class='d' width='5' height='5'/>" TAIL);
    CHECK(px && is_red(P(px, 2, 2)), "css: a type selector");
    CHECK(px && is_blue(P(px, 7, 2)), "css: a class beats a type");
    CHECK(px && is_green(P(px, 12, 2)), "css: an id beats a class");
    CHECK(px && is_red(P(px, 17, 2)), "css: the style attribute beats every rule");
    CHECK(px && is_red(P(px, 22, 2)), "css: a rule beats a presentation attribute");
    CHECK(px && is_green(P(px, 2, 12)), "css: a child combinator");
    CHECK(px && is_blue(P(px, 7, 12)), "css: a descendant combinator, two levels down");
    CHECK(px && is_green(P(px, 12, 12)), "css: an attribute selector");
    CHECK(px && is_blue(P(px, 17, 12)) && is_red(P(px, 22, 12)), "css: a compound needs both classes");
    CHECK(px && is_blue(P(px, 27, 12)), "css: of two rules alike, the later");
    CHECK(px && is_red(P(px, 2, 22)), "css: the child combinator does not match outside a g; :hover and @media never apply");
    free(px);
    px = draw(HEAD "<style>.cut { clip-path: url(#c) }</style><defs><clipPath id='c'><rect width='20' height='40'/></clipPath></defs>"
                   "<rect class='cut' width='40' height='40' fill='#f00'/>" TAIL);
    CHECK(px && is_red(P(px, 10, 10)) && is_clear(P(px, 30, 10)), "css: clip-path from a rule");
    free(px);
}

/* ---------------------------------------------------------------- nesting, units, opacity */

static void test_nesting(void)
{
    uint8_t* px = draw(HEAD "<defs><clipPath id='l'><rect width='20' height='40'/></clipPath><clipPath id='t'><rect width='40' height='20'/></clipPath></defs>"
                            "<g clip-path='url(#l)'><rect width='40' height='40' fill='#f00' clip-path='url(#t)'/></g>" TAIL);
    CHECK(px && is_red(P(px, 10, 10)) && is_clear(P(px, 30, 10)) && is_clear(P(px, 10, 30)) && is_clear(P(px, 30, 30)), "a clip inside a clipped group: the intersection");
    free(px);
    px = draw(HEAD "<defs><clipPath id='t'><rect width='40' height='20'/></clipPath><clipPath id='l' clip-path='url(#t)'><rect width='20' height='40'/></clipPath></defs>"
                   "<rect width='40' height='40' fill='#f00' clip-path='url(#l)'/>" TAIL);
    CHECK(px && is_red(P(px, 10, 10)) && is_clear(P(px, 30, 10)) && is_clear(P(px, 10, 30)), "a clip path clipped itself");
    free(px);
    px = draw(HEAD "<defs><clipPath id='b' clipPathUnits='objectBoundingBox'><rect width='.5' height='.5'/></clipPath></defs>"
                   "<rect x='10' y='10' width='20' height='20' fill='#f00' clip-path='url(#b)'/>" TAIL);
    CHECK(px && is_red(P(px, 12, 12)) && is_red(P(px, 19, 19)) && is_clear(P(px, 21, 12)) && is_clear(P(px, 12, 21)), "clipPathUnits objectBoundingBox: the box's top left quarter");
    free(px);
    px = draw(HEAD "<defs><mask id='m' maskContentUnits='objectBoundingBox'><rect width='.5' height='1' fill='#fff'/></mask></defs>"
                   "<rect x='10' y='10' width='20' height='20' fill='#f00' mask='url(#m)'/>" TAIL);
    CHECK(px && is_red(P(px, 15, 20)) && is_clear(P(px, 25, 20)), "maskContentUnits objectBoundingBox: the left half of the box");
    free(px);
    px = draw(HEAD "<defs><mask id='m' x='0' y='0' width='.5' height='1'><rect width='40' height='40' fill='#fff'/></mask></defs>"
                   "<rect x='10' y='10' width='20' height='20' fill='#f00' mask='url(#m)'/>" TAIL);
    CHECK(px && is_red(P(px, 15, 20)) && is_clear(P(px, 25, 20)), "the mask's region (bounding-box units): nothing outside it");
    free(px);
    px = draw(HEAD "<defs><mask id='m'><rect width='40' height='40' fill='#fff'/></mask></defs><rect x='10' y='10' width='20' height='20' fill='#f00' mask='url(#m)'/>" TAIL);
    CHECK(px && is_red(P(px, 11, 11)) && is_red(P(px, 28, 28)), "the mask's default region is the box and 10 %% around it");
    free(px);
    px = draw(HEAD "<defs><mask id='m' mask-type='alpha'><rect width='40' height='40' fill='#000'/></mask><mask id='n'><rect width='40' height='40' fill='#000'/></mask></defs>"
                   "<rect width='20' height='20' fill='#f00' mask='url(#m)'/><rect x='20' width='20' height='20' fill='#f00' mask='url(#n)'/>" TAIL);
    CHECK(px && is_red(P(px, 10, 10)) && is_clear(P(px, 30, 10)), "mask-type alpha: an opaque black mask shows all (luminance: none)");
    free(px);

    /* group opacity: where the shapes overlap, the top one's color at half */
    px = draw(HEAD "<g opacity='0.5'><rect width='30' height='30' fill='#f00'/><rect x='10' y='10' width='30' height='30' fill='#00f'/></g>" TAIL);
    CHECK(px && near(P(px, 20, 20)[3], 128, 2) && P(px, 20, 20)[0] < 3 && near(P(px, 20, 20)[2], 128, 2), "group opacity: the overlap is blue at half (%d %d %d %d)", PX(px, 20, 20));
    CHECK(px && near(P(px, 5, 5)[3], 128, 2) && near(P(px, 5, 5)[0], 128, 2), "and red at half alone");
    free(px);
    px = draw(HEAD "<g opacity='.5'><g opacity='.5'><rect width='40' height='40' fill='#f00' opacity='.5'/></g></g>" TAIL);
    CHECK(px && near(P(px, 20, 20)[3], 32, 2), "opacity nested three times: an eighth (%d)", px ? P(px, 20, 20)[3] : -1);
    free(px);
    px = draw(HEAD "<rect x='10' y='10' width='20' height='20' fill='#f00' stroke='#00f' stroke-width='8' opacity='.5'/>" TAIL);
    CHECK(px && near(P(px, 12, 20)[3], 128, 2) && P(px, 12, 20)[0] < 3, "a shape's opacity takes fill and stroke together: no red under the stroke (%d %d %d %d)", PX(px, 12, 20));
    free(px);
}

/* ---------------------------------------------------------------- patterns */

static void test_pattern(void)
{
    uint8_t* px = draw(HEAD "<defs><pattern id='p' width='10' height='10' patternUnits='userSpaceOnUse'><rect width='5' height='5' fill='#f00'/></pattern></defs>"
                            "<rect width='40' height='40' fill='url(#p)'/>" TAIL);
    bool ok = px != NULL;
    for(int y = 0; px && y < 40; y++) {
        for(int x = 0; x < 40; x++) {
            bool red = x % 10 < 5 && y % 10 < 5;
            if(red ? !is_red(P(px, x, y)) : !is_clear(P(px, x, y))) {
                ok = false;
            }
        }
    }
    CHECK(ok, "pattern: a 5 px square every 10 px, exactly");
    free(px);
    px = draw(HEAD "<defs><pattern id='p' width='10' height='10' patternUnits='userSpaceOnUse' patternTransform='translate(3 0)'><rect width='5' height='5' fill='#f00'/></pattern></defs>"
                   "<rect width='40' height='40' fill='url(#p)'/>" TAIL);
    CHECK(px && is_red(P(px, 4, 2)) && is_clear(P(px, 2, 2)) && is_red(P(px, 14, 2)), "pattern: patternTransform moves the tiles");
    free(px);
    px = draw(HEAD "<defs><pattern id='p' width='.5' height='.5'><rect width='5' height='5' fill='#f00'/></pattern></defs>"
                   "<rect x='10' y='10' width='20' height='20' fill='url(#p)'/>" TAIL);
    CHECK(px && is_red(P(px, 12, 12)) && is_clear(P(px, 17, 12)) && is_red(P(px, 22, 12)) && is_red(P(px, 22, 22)) && is_clear(P(px, 5, 5)),
        "pattern: bounding-box units: tiles of half the box from its corner, the content from the tile's");
    free(px);
    px = draw(HEAD "<defs><pattern id='p' width='20' height='20' patternUnits='userSpaceOnUse' viewBox='0 0 2 2'><rect width='1' height='1' fill='#f00'/></pattern>"
                   "<pattern id='q' href='#p' x='5'/></defs><rect width='40' height='20' fill='url(#p)'/><rect y='20' width='40' height='20' fill='url(#q)'/>" TAIL);
    CHECK(px && is_red(P(px, 5, 5)) && is_clear(P(px, 15, 5)), "pattern: a viewBox scales the content into the tile");
    CHECK(px && is_red(P(px, 7, 22)) && is_clear(P(px, 3, 22)), "pattern: href takes the other's content and attributes, with its own x");
    free(px);
    px = draw(HEAD "<defs><pattern id='p' width='4' height='4' patternUnits='userSpaceOnUse'><rect width='4' height='4' fill='#00f'/></pattern></defs>"
                   "<rect x='10' y='10' width='20' height='20' fill='#f00' stroke='url(#p)' stroke-width='4'/>" TAIL);
    CHECK(px && is_blue(P(px, 10, 20)) && is_red(P(px, 20, 20)) && is_clear(P(px, 6, 20)), "pattern: a stroke painted with a pattern, over a plain fill");
    free(px);
    px = draw(HEAD "<defs><pattern id='in' width='2' height='2' patternUnits='userSpaceOnUse'><rect width='1' height='2' fill='#0f0'/></pattern>"
                   "<pattern id='out' width='10' height='10' patternUnits='userSpaceOnUse'><rect width='5' height='10' fill='url(#in)'/></pattern></defs>"
                   "<rect width='40' height='40' fill='url(#out)'/>" TAIL);
    CHECK(px && is_green(P(px, 0, 3)) && is_clear(P(px, 1, 3)) && is_green(P(px, 2, 3)) && is_clear(P(px, 7, 3)), "pattern: a pattern inside a pattern");
    free(px);
    W = 80; /* at 2x the tile is drawn at 2x */
    px = draw(HEAD "<defs><pattern id='p' width='10' height='10' patternUnits='userSpaceOnUse'><rect width='5' height='5' fill='#f00'/></pattern></defs>"
                   "<rect width='40' height='40' fill='url(#p)'/>" TAIL);
    CHECK(px && is_red(P(px, 9, 9)) && is_clear(P(px, 11, 9)) && is_red(P(px, 21, 21)) && is_clear(P(px, 31, 21)), "pattern: at 2x, 10 px squares every 20 px");
    free(px);
    W = 40;
    px = draw(HEAD "<defs><pattern id='p' width='.25' height='.25'><rect width='2' height='2' fill='#f00'/></pattern></defs>"
                   "<rect x='13' y='13' width='20' height='20' fill='url(#p)'/>" TAIL);
    CHECK(px && is_red(P(px, 13, 13)) && is_red(P(px, 18, 13)) && is_clear(P(px, 16, 13)), "pattern: tiles start at the box's corner (13), every 5");
    free(px);
    px = draw(HEAD "<defs><pattern id='p' width='0' height='10'><rect width='5' height='5' fill='#f00'/></pattern></defs><rect width='40' height='40' fill='url(#p)'/>" TAIL);
    CHECK(px && is_clear(P(px, 2, 2)), "pattern: a tile of no width paints nothing");
    free(px);
}

/* ---------------------------------------------------------------- markers */

static void test_marker(void)
{
    /* a triangle pointing along +x in its own space: 4 long, 4 wide, its base's middle on the vertex */
#define TRI "<marker id='t' markerWidth='4' markerHeight='4' refX='0' refY='2' markerUnits='userSpaceOnUse' orient='auto'><path d='M0 0L4 2L0 4z' fill='#00f'/></marker>"
    uint8_t* px = draw(HEAD "<defs>" TRI "</defs><path d='M20 5V30' stroke='#000' marker-end='url(#t)'/>" TAIL);
    CHECK(px && is_blue(P(px, 20, 31)) && is_clear(P(px, 23, 31)) && is_clear(P(px, 20, 35)), "marker: orient auto points the triangle down the path (its tip at y 34)");
    free(px);
    px = draw(HEAD "<defs>" TRI "</defs><path d='M20 5V30' stroke='#000' marker-start='url(#t)'/>" TAIL);
    CHECK(px && is_blue(P(px, 20, 6)) && is_clear(P(px, 20, 3)), "marker: orient auto at the start points along the path too");
    free(px);
    px = draw(HEAD "<defs><marker id='t' markerWidth='4' markerHeight='4' refX='0' refY='2' markerUnits='userSpaceOnUse' orient='auto-start-reverse'><path d='M0 0L4 2L0 4z' fill='#00f'/></marker></defs>"
                   "<path d='M20 8V30' stroke='none' marker-start='url(#t)'/>" TAIL);
    CHECK(px && is_blue(P(px, 20, 6)) && is_clear(P(px, 20, 9)), "marker: auto-start-reverse turns the start one back");
    free(px);
    /* the turned triangle against the same triangle drawn turned */
    px = draw(HEAD "<defs>" TRI "</defs><path d='M5 5L25 25' stroke='none' fill='none' marker-end='url(#t)'/>" TAIL);
    CHECK(px && vs(px, HEAD "<path d='M0 0L4 2L0 4z' fill='#00f' transform='translate(25 25) rotate(45) translate(0 -2)'/>" TAIL, 3) == 0,
        "marker: placed and turned as the transform says");
    free(px);
    px = draw(HEAD "<defs><marker id='d' markerWidth='2' markerHeight='2' refX='1' refY='1' markerUnits='userSpaceOnUse'><rect width='2' height='2' fill='#f00'/></marker></defs>"
                   "<polyline points='5 20 20 10 35 20' fill='none' stroke='none' marker-mid='url(#d)'/>" TAIL);
    CHECK(px && is_red(P(px, 20, 10)) && is_clear(P(px, 5, 20)) && is_clear(P(px, 35, 20)), "marker: mid markers on the middle vertex only");
    free(px);
    px = draw(HEAD "<defs>" TRI "</defs><polyline points='5 25 20 10 35 25' fill='none' stroke='none' marker-mid='url(#t)'/>" TAIL);
    CHECK(px && vs(px, HEAD "<path d='M0 0L4 2L0 4z' fill='#00f' transform='translate(20 10) translate(0 -2)'/>" TAIL, 3) == 0, "marker: a mid marker points along the bisector (here +x)");
    free(px);
    px = draw(HEAD "<defs><marker id='d' markerWidth='2' markerHeight='2' refX='1' refY='1' markerUnits='userSpaceOnUse'><rect width='2' height='2' fill='#f00'/></marker></defs>"
                   "<path d='M5 25A15 15 0 0 1 35 25' fill='none' stroke='none' marker-mid='url(#d)'/>" TAIL);
    CHECK(px && is_clear(P(px, 20, 10)) && is_clear(P(px, 20, 9)), "marker: an arc has no vertex in its middle (it is drawn in pieces)");
    free(px);
    px = draw(HEAD "<defs><marker id='d' markerWidth='2' markerHeight='2' refX='1' refY='1'><rect width='2' height='2' fill='#f00'/></marker></defs>"
                   "<path d='M20 20h0.01' stroke='none' stroke-width='4' marker-end='url(#d)'/>" TAIL);
    CHECK(px && is_red(P(px, 17, 17)) && is_red(P(px, 22, 22)) && is_clear(P(px, 15, 20)), "marker: markerUnits strokeWidth scales it by the stroke width (8 px)");
    free(px);
    px = draw(HEAD "<defs><marker id='o' markerWidth='4' markerHeight='4' markerUnits='userSpaceOnUse'><rect width='10' height='10' fill='#f00'/></marker>"
                   "<marker id='v' markerWidth='4' markerHeight='4' markerUnits='userSpaceOnUse' overflow='visible'><rect width='10' height='10' fill='#f00'/></marker></defs>"
                   "<path d='M5 5h1' stroke='none' marker-end='url(#o)'/><path d='M25 25h1' stroke='none' marker-end='url(#v)'/>" TAIL);
    CHECK(px && is_red(P(px, 7, 7)) && is_clear(P(px, 11, 7)), "marker: cut to its viewport");
    CHECK(px && is_red(P(px, 27, 27)) && is_red(P(px, 33, 33)), "marker: overflow visible is not cut");
    free(px);
    px = draw(HEAD "<defs><marker id='d' markerWidth='2' markerHeight='2' refX='1' refY='1' markerUnits='userSpaceOnUse'><rect width='2' height='2'/></marker></defs>"
                   "<g fill='#0f0'><path d='M20 20h1' stroke='none' marker-end='url(#d)'/></g>" TAIL);
    CHECK(px && P(px, 21, 20)[3] > 200 && P(px, 21, 20)[1] < 8, "marker: its content inherits from where the marker is, not from the shape (black)");
    free(px);
}

/* ---------------------------------------------------------------- images */

static char* b64(const unsigned char* d, size_t n)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *o = malloc(n * 4 / 3 + 8), *p = o;
    for(size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)d[i] << 16 | (i + 1 < n ? (unsigned)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        *p++ = t[v >> 18 & 63];
        *p++ = t[v >> 12 & 63];
        *p++ = i + 1 < n ? t[v >> 6 & 63] : '=';
        *p++ = i + 2 < n ? t[v & 63] : '=';
    }
    *p = 0;
    return o;
}

static void test_image(void)
{
    char *enc = b64(fx_png, sizeof fx_png), doc[4096];
    snprintf(doc, sizeof doc, HEAD "<image width='40' height='30' preserveAspectRatio='none' href='data:image/png;base64,%s'/>" TAIL, enc);
    uint8_t* px = draw(doc);
    CHECK(px && is_red(P(px, 5, 5)) && is_green(P(px, 35, 5)) && is_blue(P(px, 5, 25)), "image: a PNG data URL stretched over 40 x 30 (its quadrants)");
    CHECK(px && near(P(px, 35, 25)[3], 128, 4) && near(P(px, 35, 25)[0], 128, 4) && is_clear(P(px, 20, 35)), "image: its half-clear white quadrant, nothing below it");
    free(px);
    snprintf(doc, sizeof doc, HEAD "<image x='4' y='4' href='data:image/png;base64,%s' opacity='.5'/>" TAIL, enc);
    px = draw(doc);
    CHECK(px && near(P(px, 5, 5)[3], 128, 3) && is_clear(P(px, 13, 5)) && P(px, 11, 5)[3] > 100, "image: without a size, its own (8 x 6); opacity");
    free(px);
    free(enc);
    px = draw(HEAD "<image width='40' height='20' href=\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='10' height='10'%3E%3Ccircle cx='5' cy='5' r='5' fill='%23f00'/%3E%3C/svg%3E\"/>" TAIL);
    CHECK(px && is_red(P(px, 20, 10)) && is_red(P(px, 20, 1)) && is_clear(P(px, 6, 10)) && is_clear(P(px, 34, 10)), "image: an SVG data URL, kept round in a wide box (meet)");
    free(px);
    px = draw(HEAD "<image width='40' height='20' preserveAspectRatio='xMinYMin slice' href=\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='10' height='10'%3E%3Crect width='5' height='5' fill='%2300f'/%3E%3Crect x='5' y='5' width='5' height='5' fill='%230f0'/%3E%3C/svg%3E\"/>" TAIL);
    CHECK(px && is_blue(P(px, 5, 5)) && is_blue(P(px, 18, 18)) && is_clear(P(px, 25, 5)) && is_clear(P(px, 25, 25)), "image: slice fills the box and is cut to it");
    free(px);
    px = draw(HEAD "<image width='40' height='40' href='http://example.com/a.png'/><rect width='5' height='5' fill='#f00'/>" TAIL);
    CHECK(px && is_red(P(px, 2, 2)) && is_clear(P(px, 20, 20)), "image: what is not a data URL is not loaded, the rest is drawn");
    free(px);
}

/* ---------------------------------------------------------------- text */

static void test_text(void)
{
    gp_glyph_pos g[4];
    if(!g_ts || gp_text_shape(g_ts, 400, 16.f, "H", 1, g, 4, NULL) < 1 || g[0].glyph == 0) {
        printf("SKIP: no font to draw SVG text with\n");
        return;
    }
    int save = W;
    W = 160; /* (4 px a unit) */
    uint8_t* px = draw(HEAD "<defs><path id='p' d='M4 25H40'/></defs><text font-size='8' fill='#f00'><textPath href='#p'>Hello</textPath></text>" TAIL);
    CHECK(px && vs(px, HEAD "<text x='4' y='25' font-size='8' fill='#f00'>Hello</text>" TAIL, 40) < 60, "text: on a straight path it is the text set there");
    free(px);
    px = draw(HEAD "<defs><path id='p' d='M20 2V40'/></defs><text font-size='8' fill='#f00'><textPath href='#p'>Hello</textPath></text>" TAIL);
    CHECK(px && vs(px, HEAD "<text font-size='8' fill='#f00' transform='translate(20 2) rotate(90)'>Hello</text>" TAIL, 60) < 150, "text: down a vertical path it is turned with it");
    free(px);
    px = draw(HEAD "<defs><path id='p' d='M4 25H20'/></defs><text font-size='8' fill='#f00'><textPath href='#p'>Hello world wide</textPath></text>" TAIL);
    int l, t, r, b;
    CHECK(px && ink_box(px, &l, &t, &r, &b) && r <= 21 * 4, "text: what does not fit on the path is not drawn (ink to %d)", px ? r : -1);
    free(px);
    px = draw("<svg xmlns='http://www.w3.org/2000/svg' width='40' height='40' viewBox='0 -20 40 40'><defs><path id='p' d='M4 15H20'/></defs><text font-size='8' fill='#f00'><textPath href='#p'>Hello world wide</textPath></text></svg>");
    CHECK(px && ink_box(px, &l, &t, &r, &b) && t > 25 * 4, "text: nor anywhere else (ink from row %d)", px ? t : -1);
    free(px);

    int l0, t0, r0, b0;
    uint8_t* a = draw(HEAD "<text x='2' y='20' font-size='6' fill='#f00'>HHHH</text>" TAIL);
    uint8_t* s = draw(HEAD "<text x='2' y='20' font-size='6' fill='#f00' letter-spacing='2'>HHHH</text>" TAIL);
    CHECK(a && s && ink_box(a, &l0, &t0, &r0, &b0) && ink_box(s, &l, &t, &r, &b) && near((r - l) - (r0 - l0), 3 * 2 * 4, 2), "text: letter-spacing 2 widens 4 letters by 6 units (%d px)", (r - l) - (r0 - l0));
    free(s);
    s = draw(HEAD "<text x='2' y='20' font-size='6' fill='#f00' textLength='30' lengthAdjust='spacingAndGlyphs'>HHHH</text>" TAIL);
    CHECK(s && ink_box(s, &l, &t, &r, &b) && r - l > (r0 - l0) + 40 && r - l <= 30 * 4 + 2, "text: textLength stretches the glyphs to 30 units (%d px)", r - l);
    free(s);
    s = draw(HEAD "<text x='2' y='20' font-size='6' fill='#f00' textLength='30'>HHHH</text>" TAIL);
    int l2, t2, r2, b2;
    CHECK(s && ink_box(s, &l2, &t2, &r2, &b2) && r2 - l2 > (r0 - l0) + 40 && b2 - t2 == b0 - t0, "text: textLength with spacing keeps the glyphs' size, spreads them");
    free(s);
    s = draw(HEAD "<text x='2 30' y='20' font-size='6' fill='#f00'>HH</text>" TAIL);
    CHECK(s && P(s, 31 * 4, 18 * 4)[3] > 100 && P(s, 20 * 4, 18 * 4)[3] < 10, "text: an x list puts the second letter at 30");
    free(s);
    s = draw(HEAD "<text x='2' y='20' font-size='6' fill='#f00' rotate='90'>HHHH</text>" TAIL);
    CHECK(s && ink_box(s, &l, &t, &r, &b) && (b - t) < (b0 - t0) + 2 && (r - l) < (r0 - l0) + 12, "text: rotate turns each letter about its origin");
    free(s);
    s = draw(HEAD "<text x='20' y='2' font-size='6' fill='#f00' writing-mode='tb'>HHHH</text>" TAIL);
    CHECK(s && ink_box(s, &l, &t, &r, &b) && (b - t) > 3 * (r - l), "text: vertical writing goes down (%d x %d)", r - l, b - t);
    free(s);
    s = draw(HEAD "<text x='2' y='20' font-size='6' fill='#f00' dominant-baseline='hanging'>HHHH</text>" TAIL);
    CHECK(s && ink_box(s, &l, &t, &r, &b) && t >= 20 * 4 - 2 && t > t0, "text: dominant-baseline hanging puts the letters below y");
    free(s);
    s = draw(HEAD "<text x='20' y='20' font-size='6' fill='#f00' text-anchor='middle'>HHHH</text>" TAIL);
    CHECK(s && ink_box(s, &l, &t, &r, &b) && near((l + r) / 2, 20 * 4, 3), "text: anchored in the middle");
    free(s);
    s = draw(HEAD "<text x='2' y='20' font-size='6' fill='#f00'>HH<tspan dy='-5'>HH</tspan></text>" TAIL);
    CHECK(s && ink_box(s, &l, &t, &r, &b) && near(t, t0 - 5 * 4, 2) && near(b, b0, 2), "text: a tspan's dy raises the letters after it");
    free(s);
    free(a);
    W = save;
}

/* ---------------------------------------------------------------- filters */

#define BIG    "x='-2' y='-2' width='5' height='5'"
#define RECT10 "<rect x='15' y='15' width='10' height='10' fill='#f00' filter='url(#f)'/>"

static float clampf(float v)
{
    return v < 0 ? 0 : v > 1 ? 1
                             : v;
}
static float blend_ref(int mode, float cb, float cs)
{
    switch(mode) {
        case 0:
            return cb * cs;
        case 1:
            return cb + cs - cb * cs;
        case 2:
            return cb <= 0.5f ? 2 * cs * cb : 1 - 2 * (1 - cs) * (1 - cb); /* overlay */
        case 3:
            return fminf(cb, cs);
        case 4:
            return fmaxf(cb, cs);
        case 5:
            return cb == 0 ? 0 : cs == 1 ? 1
                                         : fminf(1, cb / (1 - cs));                               /* color-dodge */
        case 6:
            return cb == 1 ? 1 : cs == 0 ? 0
                                         : 1 - fminf(1, (1 - cb) / cs);                           /* color-burn */
        case 7:
            return cs <= 0.5f ? 2 * cs * cb : 1 - 2 * (1 - cs) * (1 - cb);                        /* hard-light */
        case 8: {
            float d = cb <= 0.25f ? ((16 * cb - 12) * cb + 4) * cb : sqrtf(cb);
            return cs <= 0.5f ? cb - (1 - 2 * cs) * cb * (1 - cb) : cb + (2 * cs - 1) * (d - cb); /* soft-light */
        }
        case 9:
            return fabsf(cb - cs);
        default:
            return cb + cs - 2 * cb * cs; /* exclusion */
    }
}

static void test_filter(void)
{
    uint8_t* px = draw(HEAD "<defs><filter id='f' x='0' y='0' width='.5' height='1'><feFlood flood-color='#00f'/></filter></defs>" RECT10 TAIL);
    CHECK(px && is_blue(P(px, 16, 20)) && is_clear(P(px, 21, 20)) && is_clear(P(px, 16, 13)), "filter: the region (half the box): a flood fills it and no more");
    free(px);
    px = draw(HEAD "<defs><filter id='f' filterUnits='userSpaceOnUse' x='5' y='5' width='10' height='30'><feFlood flood-color='#00f'/></filter></defs>" RECT10 TAIL);
    CHECK(px && is_blue(P(px, 6, 30)) && is_clear(P(px, 16, 20)), "filter: a region in user space");
    free(px);
    px = draw(HEAD "<defs><filter id='f' filterUnits='userSpaceOnUse' x='20' y='0' width='20' height='40'><feOffset dx='10'/></filter></defs>"
                   "<rect x='10' y='15' width='10' height='10' fill='#f00' filter='url(#f)'/>" TAIL);
    CHECK(px && is_clear(P(px, 25, 20)), "filter: the source is cut to the region before the primitives see it");
    free(px);
    px = draw(HEAD "<defs><filter id='f'><feFlood flood-color='#00f'/></filter></defs>" RECT10 TAIL);
    CHECK(px && is_blue(P(px, 14, 14)) && is_clear(P(px, 13, 20)) && is_clear(P(px, 27, 20)), "filter: the default region is the box and 10 %% around it (14..26)");
    free(px);
    px = draw(HEAD "<defs><filter id='f' " BIG "><feFlood flood-color='#00f' x='2' y='3' width='4' height='5'/></filter></defs>" RECT10 TAIL);
    CHECK(px && is_blue(P(px, 3, 4)) && is_blue(P(px, 5, 7)) && is_clear(P(px, 7, 4)) && is_clear(P(px, 3, 9)), "filter: a primitive's subregion");
    free(px);
    px = draw(HEAD "<defs><filter id='f' " BIG " primitiveUnits='objectBoundingBox'><feOffset dx='.5' dy='0'/></filter></defs>" RECT10 TAIL);
    CHECK(px && is_red(P(px, 21, 20)) && is_red(P(px, 29, 20)) && is_clear(P(px, 18, 20)), "filter: primitiveUnits objectBoundingBox: dx .5 of a 10 wide box is 5");
    free(px);

    /* morphology: a 10 px square grows and shrinks by the radius */
    px = draw(HEAD "<defs><filter id='f' " BIG "><feMorphology operator='dilate' radius='2'/></filter></defs>" RECT10 TAIL);
    CHECK(px && is_red(P(px, 13, 20)) && is_red(P(px, 26, 26)) && is_clear(P(px, 12, 20)) && is_clear(P(px, 27, 20)), "filter: dilate 2: 13..27");
    free(px);
    px = draw(HEAD "<defs><filter id='f' " BIG "><feMorphology radius='2 0'/></filter></defs>" RECT10 TAIL);
    CHECK(px && is_red(P(px, 17, 15)) && is_red(P(px, 22, 24)) && is_clear(P(px, 16, 20)) && is_clear(P(px, 23, 20)), "filter: erode 2 across only: 17..23 wide, as tall as before");
    free(px);
    /* convolution: a kernel with one 1 moves the picture (the kernel is turned half round: a 1 at the left of the middle row takes from the right) */
    px = draw(HEAD "<defs><filter id='f' " BIG "><feConvolveMatrix order='3' kernelMatrix='0 0 0 1 0 0 0 0 0'/></filter></defs>" RECT10 TAIL);
    CHECK(px && is_red(P(px, 14, 20)) && is_red(P(px, 23, 20)) && is_clear(P(px, 24, 20)), "filter: feConvolveMatrix with one 1 left of the middle moves it a pixel left");
    free(px);
    px = draw(HEAD "<defs><filter id='f' " BIG "><feConvolveMatrix order='3' kernelMatrix='1 1 1 1 1 1 1 1 1'/></filter></defs>" RECT10 TAIL);
    CHECK(px && is_red(P(px, 20, 20)) && near(P(px, 15, 20)[3], 170, 3) && near(P(px, 15, 15)[3], 113, 3), "filter: a box kernel averages (2/3 at an edge, 4/9 at a corner: %d %d)", px ? P(px, 15, 20)[3] : -1, px ? P(px, 15, 15)[3] : -1);
    free(px);
    /* component transfer on a flat gray of 128 (sRGB) */
    px = draw(HEAD "<defs><filter id='f' color-interpolation-filters='sRGB'><feComponentTransfer><feFuncR type='discrete' tableValues='0 1'/>"
                   "<feFuncG type='linear' slope='.5' intercept='.25'/><feFuncB type='gamma' amplitude='1' exponent='2' offset='0'/>"
                   "<feFuncA type='table' tableValues='0 .5'/></feComponentTransfer></filter></defs><rect x='15' y='15' width='10' height='10' fill='#808080' filter='url(#f)'/>" TAIL);
    {
        float v = 128.f / 255.f, a = 0.5f;
        int r = 255, g = (int)lroundf(clampf(v * .5f + .25f) * 255), bl = (int)lroundf(v * v * 255);
        CHECK(px && near(P(px, 20, 20)[3], 128, 1) && near(P(px, 20, 20)[0], (int)lroundf(r * a), 2) && near(P(px, 20, 20)[1], (int)lroundf(g * a), 2) && near(P(px, 20, 20)[2], (int)lroundf(bl * a), 2),
            "filter: component transfer (discrete, linear, gamma, table): %d %d %d %d",
            PX(px, 20, 20));
    }
    free(px);
    /* turbulence: what librsvg (the same reference code) makes of it */
    px = draw(HEAD "<defs><filter id='f' x='0' y='0' width='1' height='1' color-interpolation-filters='sRGB'><feTurbulence baseFrequency='0.07' numOctaves='2' seed='5'/></filter></defs>"
                   "<rect width='40' height='40' fill='#000' filter='url(#f)'/>" TAIL);
    {
        static const int ref[5][6] = {
            {  3,  4, 39, 27, 17,  71 },
            { 17,  9, 18,  7,  9,  30 },
            { 25, 30,  4, 14, 27,  49 },
            { 36, 12,  3,  6,  8,  20 },
            {  8, 33, 23,  9, 11, 113 }
        };
        int bad = 0;
        for(int i = 0; i < 5 && px; i++) {
            for(int c = 0; c < 4; c++) {
                if(!near(P(px, ref[i][0], ref[i][1])[c], ref[i][2 + c], 3)) {
                    bad++;
                }
            }
        }
        CHECK(px && bad == 0, "filter: feTurbulence gives librsvg's values (%d off)", bad);
    }
    free(px);
    px = draw(HEAD "<defs><filter id='f' x='0' y='0' width='1' height='1'><feTurbulence type='fractalNoise' baseFrequency='0.1' seed='1'/></filter>"
                   "<filter id='g' x='0' y='0' width='1' height='1'><feTurbulence type='fractalNoise' baseFrequency='0.1' seed='2'/></filter></defs>"
                   "<rect width='20' height='40' filter='url(#f)'/><rect x='20' width='20' height='40' filter='url(#g)'/>" TAIL);
    {
        long sum = 0;
        int same = 0;
        for(int y = 0; px && y < 40; y++) {
            for(int x = 0; x < 20; x++) {
                sum += P(px, x, y)[3];
                if(P(px, x, y)[3] == P(px, x + 20, y)[3]) {
                    same++;
                }
            }
        }
        CHECK(px && near((int)(sum / 800), 128, 20) && same < 400, "filter: fractal noise averages a half and changes with the seed (%ld, %d)", sum / 800, same);
    }
    free(px);
    /* stitching: a tile of 10 and a frequency of 0.32 becomes 0.3 (nearer by ratio than 0.4): the noise is zero on its lattice, which has (0, 0) and not (5, 5) */
    px = draw(HEAD "<defs><filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='10' height='10' color-interpolation-filters='sRGB'>"
                   "<feTurbulence baseFrequency='0.32' seed='3' stitchTiles='stitch'/></filter></defs><rect width='40' height='40' filter='url(#f)'/>" TAIL);
    CHECK(px && P(px, 0, 0)[3] == 0 && P(px, 5, 5)[3] > 10 && P(px, 5, 0)[3] > 0 && P(px, 0, 5)[3] > 10, "filter: stitchTiles rounds the frequency to whole cells of the tile (%d %d)", px ? P(px, 0, 0)[3] : -1, px ? P(px, 5, 5)[3] : -1);
    free(px);
    /* lighting on a flat surface: diffuse is kd sin(elevation); specular from straight above is ks, with that as its alpha */
    px = draw(HEAD "<defs><filter id='f' filterUnits='userSpaceOnUse' x='10' y='10' width='20' height='20' color-interpolation-filters='sRGB'>"
                   "<feDiffuseLighting diffuseConstant='1' lighting-color='#fff'><feDistantLight azimuth='0' elevation='30'/></feDiffuseLighting></filter></defs>"
                   "<rect width='40' height='40' filter='url(#f)'/>" TAIL);
    CHECK(px && near(P(px, 20, 20)[0], 128, 2) && P(px, 20, 20)[3] == 255 && is_clear(P(px, 5, 5)), "filter: diffuse light at 30 degrees on a flat surface: half (%d)", px ? P(px, 20, 20)[0] : -1);
    free(px);
    px = draw(HEAD "<defs><filter id='f' filterUnits='userSpaceOnUse' x='10' y='10' width='20' height='20' color-interpolation-filters='sRGB'>"
                   "<feSpecularLighting specularConstant='.5' specularExponent='4' lighting-color='#fff'><feDistantLight elevation='90'/></feSpecularLighting></filter></defs>"
                   "<rect width='40' height='40' filter='url(#f)'/>" TAIL);
    CHECK(px && near(P(px, 20, 20)[3], 128, 2) && near(P(px, 20, 20)[0], 128, 2), "filter: specular light from above on a flat surface: ks, alpha ks (%d %d)", px ? P(px, 20, 20)[0] : -1, px ? P(px, 20, 20)[3] : -1);
    free(px);
    /* a slope: the side facing the light is lighter than the side facing away */
    px = draw(HEAD "<defs><filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='40' height='40'><feGaussianBlur in='SourceAlpha' stdDeviation='2'/>"
                   "<feDiffuseLighting surfaceScale='5'><feDistantLight azimuth='0' elevation='45'/></feDiffuseLighting></filter></defs><rect x='10' y='10' width='20' height='20' filter='url(#f)'/>" TAIL);
    CHECK(px && P(px, 30, 20)[0] > P(px, 20, 20)[0] + 20 && P(px, 10, 20)[0] + 20 < P(px, 20, 20)[0], "filter: lighting from +x lights the right edge and darkens the left (%d %d %d)", px ? P(px, 10, 20)[0] : -1, px ? P(px, 20, 20)[0] : -1, px ? P(px, 30, 20)[0] : -1);
    free(px);
    /* displacement by a flat map: red 1 and green 0 move by (+5, -5) of the source: the picture goes left 5 and down 5 */
    px = draw(HEAD "<defs><filter id='f' " BIG " color-interpolation-filters='sRGB'><feFlood flood-color='#f00' result='m'/><feDisplacementMap in='SourceGraphic' in2='m' scale='10' xChannelSelector='R' yChannelSelector='G'/></filter></defs>" RECT10 TAIL);
    CHECK(px && is_red(P(px, 10, 20)) && is_red(P(px, 19, 29)) && is_clear(P(px, 21, 20)) && is_clear(P(px, 15, 18)), "filter: displacement by a flat map is a move");
    free(px);
    /* tile: a 10 px piece of red and blue, repeated */
    px = draw(HEAD "<defs><filter id='f' x='-1.5' y='-1.5' width='4' height='4'><feOffset x='15' y='15' width='10' height='10' result='t'/><feTile in='t'/></filter></defs>"
                   "<g filter='url(#f)'><rect x='15' y='15' width='5' height='10' fill='#f00'/><rect x='20' y='15' width='5' height='10' fill='#00f'/></g>" TAIL);
    CHECK(px && is_red(P(px, 7, 7)) && is_blue(P(px, 12, 7)) && is_red(P(px, 37, 37)) && is_blue(P(px, 2, 32)), "filter: feTile repeats the piece's subregion");
    free(px);
    px = draw(HEAD "<defs><rect id='r' width='10' height='10' fill='#00f'/><filter id='f' " BIG "><feImage href='#r'/></filter></defs>" RECT10 TAIL);
    CHECK(px && is_blue(P(px, 5, 5)) && is_clear(P(px, 20, 20)), "filter: feImage of an element draws it, not the source");
    free(px);
    {
        char *enc = b64(fx_png, sizeof fx_png), doc[4096];
        snprintf(doc, sizeof doc, HEAD "<defs><filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='40' height='40'><feImage x='0' y='0' width='40' height='30' "
                                       "preserveAspectRatio='none' href='data:image/png;base64,%s'/></filter></defs>" RECT10 TAIL,
            enc);
        free(enc);
        px = draw(doc);
        CHECK(px && is_red(P(px, 5, 5)) && is_green(P(px, 35, 5)) && is_blue(P(px, 5, 25)), "filter: feImage of a PNG in its subregion");
        free(px);
    }
    /* blend modes of two floods, against their formulas (sRGB) */
    {
        static const char* const modes[] = { "multiply", "screen", "overlay", "darken", "lighten", "color-dodge", "color-burn", "hard-light", "soft-light", "difference", "exclusion" };
        const float cs[3] = { 1.f, 128 / 255.f, 64 / 255.f }, cb[3] = { 64 / 255.f, 128 / 255.f, 1.f };
        int bad = 0;
        for(int m = 0; m < 11; m++) {
            char doc[1024];
            snprintf(doc, sizeof doc, HEAD "<defs><filter id='f' " BIG " color-interpolation-filters='sRGB'><feFlood flood-color='#ff8040' result='s'/><feFlood flood-color='#4080ff' result='b'/>"
                                           "<feBlend in='s' in2='b' mode='%s'/></filter></defs>" RECT10 TAIL,
                modes[m]);
            px = draw(doc);
            for(int c = 0; c < 3 && px; c++) {
                int want = (int)lroundf(clampf(blend_ref(m, cb[c], cs[c])) * 255.f);
                if(!near(P(px, 20, 20)[c], want, 2)) {
                    bad++;
                    printf("  %s channel %d: %d, wanted %d\n", modes[m], c, P(px, 20, 20)[c], want);
                }
            }
            free(px);
        }
        CHECK(bad == 0, "filter: the separable blend modes follow their formulas");
        px = draw(HEAD "<defs><filter id='f' " BIG " color-interpolation-filters='sRGB'><feFlood flood-color='#333333' result='s'/><feFlood flood-color='#8c8c8c' result='b'/>"
                       "<feBlend in='s' in2='b' mode='overlay'/></filter></defs>" RECT10 TAIL);
        int ov = (int)lroundf(blend_ref(2, 140 / 255.f, 51 / 255.f) * 255.f);
        CHECK(px && near(P(px, 20, 20)[0], ov, 2), "filter: overlay above the backdrop's middle (%d, wanted %d)", px ? P(px, 20, 20)[0] : -1, ov);
        free(px);
        px = draw(HEAD "<defs><filter id='f' " BIG " color-interpolation-filters='sRGB'><feFlood flood-color='#ff0000' result='s'/><feFlood flood-color='#808080' result='b'/>"
                       "<feBlend in='s' in2='b' mode='luminosity'/></filter></defs>" RECT10 TAIL);
        int l = (int)lroundf((0.3f * 1.f) * 255.f); /* the gray with the red's luminosity */
        CHECK(px && near(P(px, 20, 20)[0], l, 2) && near(P(px, 20, 20)[1], l, 2) && near(P(px, 20, 20)[2], l, 2), "filter: luminosity: the backdrop's color (gray) at the source's luminosity (%d)", l);
        free(px);
    }
    /* CSS filter functions, in sRGB */
    px = draw(HEAD "<rect width='10' height='10' fill='#f00' filter='brightness(.5)'/><rect x='10' width='10' height='10' fill='#f00' style='filter: grayscale(100%)'/>"
                   "<rect x='20' width='10' height='10' fill='#f00' filter='invert(1)'/><rect x='30' width='10' height='10' fill='#f00' filter='opacity(.5)'/>" TAIL);
    CHECK(px && near(P(px, 5, 5)[0], 128, 1), "css filter: brightness(.5) of red is 128 (%d)", px ? P(px, 5, 5)[0] : -1);
    CHECK(px && near(P(px, 15, 5)[0], 54, 2) && near(P(px, 15, 5)[1], 54, 2), "css filter: grayscale of red is 0.2126 (%d)", px ? P(px, 15, 5)[0] : -1);
    CHECK(px && is_color(P(px, 25, 5), 0, 255, 255), "css filter: invert of red is cyan");
    CHECK(px && near(P(px, 35, 5)[3], 128, 1), "css filter: opacity(.5)");
    free(px);
    px = draw(HEAD "<rect x='10' y='10' width='10' height='10' fill='#f00' filter='drop-shadow(4px 3px 2px #00f)'/>" TAIL);
    CHECK(px && vs(px, HEAD "<defs><filter id='f' " BIG " color-interpolation-filters='sRGB'><feDropShadow dx='4' dy='3' stdDeviation='1' flood-color='#00f'/></filter></defs>"
                            "<rect x='10' y='10' width='10' height='10' fill='#f00' filter='url(#f)'/>" TAIL,
                    3)
                    == 0,
        "css filter: drop-shadow(4px 3px 2px) is feDropShadow with a deviation of half the blur");
    free(px);
    px = draw(HEAD "<defs><filter id='a' " BIG "><feOffset dx='3'/></filter><filter id='b' " BIG "><feOffset dy='4'/></filter></defs>"
                   "<rect x='10' y='10' width='10' height='10' fill='#f00' filter='url(#a) url(#b)'/>" TAIL);
    CHECK(px && is_red(P(px, 14, 15)) && is_clear(P(px, 11, 15)) && is_clear(P(px, 14, 12)), "filter: a list of filters applies one after the other");
    free(px);
    px = draw(HEAD "<defs><filter id='a' " BIG "><feOffset dx='3'/></filter></defs><rect x='10' y='10' width='10' height='10' fill='#f00' filter='url(#a) url(#none)'/>" TAIL);
    CHECK(px && is_red(P(px, 11, 15)), "filter: a list with a filter that is not there is ignored whole");
    free(px);
    /* a primitive in sRGB inside a linear filter is the same as all in sRGB */
    uint8_t* a = draw(HEAD "<defs><filter id='f' " BIG "><feGaussianBlur stdDeviation='3' color-interpolation-filters='sRGB'/></filter></defs>"
                           "<g filter='url(#f)'><rect x='5' y='5' width='15' height='30' fill='#fff'/><rect x='20' y='5' width='15' height='30' fill='#000'/></g>" TAIL);
    CHECK(a && vs(a, HEAD "<defs><filter id='f' " BIG " color-interpolation-filters='sRGB'><feGaussianBlur stdDeviation='3'/></filter></defs>"
                          "<g filter='url(#f)'><rect x='5' y='5' width='15' height='30' fill='#fff'/><rect x='20' y='5' width='15' height='30' fill='#000'/></g>" TAIL,
                   2)
                   == 0,
        "filter: a primitive's own color-interpolation-filters");
    free(a);
    /* a filter under a transform: the offset turns and scales with it */
    px = draw(HEAD "<defs><filter id='f' " BIG " color-interpolation-filters='sRGB'><feOffset dx='5'/></filter></defs>"
                   "<g transform='translate(20 20) rotate(90) scale(2)'><rect width='4' height='4' fill='#f00' filter='url(#f)'/></g>" TAIL);
    CHECK(px && is_red(P(px, 16, 32)) && is_clear(P(px, 16, 22)), "filter: dx 5 under rotate(90) scale(2) moves 10 down");
    free(px);
}

int main(void)
{
    test_css();
    test_nesting();
    test_pattern();
    test_marker();
    test_image();
    test_filter();
    g_ts = gp_text_system_new();
    test_text();
    gp_text_system_free(g_ts);
    printf(failures ? "svgx2_test: %d FAILURE(S)\n" : "svgx2_test: all checks passed\n", failures);
    return failures != 0;
}
