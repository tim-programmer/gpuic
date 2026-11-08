/*
 * The gradient ramp table (gfx.c), without a display: how stops are normalized, what a ramp holds (premultiplied
 * interpolation, hard stops), that equal stops share a row whatever the angle, and what happens when the table fills.
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
            failures++;                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                \
            printf(__VA_ARGS__);                                                       \
            printf("\n");                                                              \
        }                                                                              \
    } while (0)

static const gp_color RED = { 1, 0, 0, 1 }, BLUE = { 0, 0, 1, 1 }, GREEN = { 0, 1, 0, 1 }, WHITE = { 1, 1, 1, 1 };

static gp_gradient norm(const gp_color_stop *s, int n) {
    gp_gradient g = { 0 };
    gp_gradient_normalize(&g, s, n);
    return g;
}

static bool near(float a, float b) { return fabsf(a - b) < 1e-5f; }

static const uint8_t *texel(gp_gfx *g, int row, int x) { return gp_gfx_gradient_data(g) + ((size_t)row * GP_GRAD_W + (size_t)x) * 4; }

static void test_normalize(void) {
    gp_gradient g = norm((gp_color_stop[]){ { RED, -1 }, { GREEN, -1 }, { BLUE, -1 }, { WHITE, -1 } }, 4);
    CHECK(g.n == 4 && near(g.stops[0].at, 0) && near(g.stops[1].at, 1.f / 3) && near(g.stops[2].at, 2.f / 3) && near(g.stops[3].at, 1),
          "no positions: spread evenly (%.3f %.3f %.3f %.3f)", g.stops[0].at, g.stops[1].at, g.stops[2].at, g.stops[3].at);
    g = norm((gp_color_stop[]){ { RED, 0 }, { GREEN, -1 }, { BLUE, -1 }, { WHITE, 0.9f } }, 4);
    CHECK(near(g.stops[1].at, 0.3f) && near(g.stops[2].at, 0.6f), "unpositioned stops between two positioned ones (%.3f %.3f)", g.stops[1].at, g.stops[2].at);
    g = norm((gp_color_stop[]){ { RED, 0.2f }, { GREEN, -1 }, { BLUE, 0.4f } }, 3);
    CHECK(near(g.stops[0].at, 0.2f) && near(g.stops[1].at, 0.3f) && near(g.stops[2].at, 0.4f), "a lone stop sits in the middle of its neighbours");
    g = norm((gp_color_stop[]){ { RED, 0.5f }, { BLUE, 0.2f } }, 2);
    CHECK(near(g.stops[1].at, 0.5f), "a stop that goes backwards is held at the previous one (%.3f)", g.stops[1].at);
    g = norm((gp_color_stop[]){ { RED, 1.5f }, { BLUE, 7.f } }, 2);
    CHECK(near(g.stops[0].at, 1.f) && near(g.stops[1].at, 1.f), "positions above 1 are clamped");
    g = norm((gp_color_stop[]){ { { 2, -1, 0.5f, 3 }, 0 }, { BLUE, 1 } }, 2);
    CHECK(g.stops[0].color.r == 1.f && g.stops[0].color.g == 0.f && g.stops[0].color.a == 1.f, "channels are clamped to 0..1");
    gp_color_stop many[40];
    for (int i = 0; i < 40; i++) many[i] = (gp_color_stop){ RED, (float)i / 39.f };
    g = norm(many, 40);
    CHECK(g.n == GP_GRAD_MAX_STOPS, "at most %d stops (%d)", GP_GRAD_MAX_STOPS, g.n);
    g = norm(NULL, 0);
    CHECK(g.n == 0, "no stops");
}

static void test_ramps(void) {
    gp_gfx *g = gp_gfx_new();
    gp_gradient a = norm((gp_color_stop[]){ { RED, 0 }, { BLUE, 1 } }, 2);
    int row = gp_gfx_gradient_row(g, &a);
    CHECK(row == 0, "the first ramp is row 0 (%d)", row);
    const uint8_t *first = texel(g, row, 0), *mid = texel(g, row, GP_GRAD_W / 2), *last = texel(g, row, GP_GRAD_W - 1);
    CHECK(first[0] >= 253 && first[2] <= 2 && first[3] == 255, "the start is red: %d %d %d %d", first[0], first[1], first[2], first[3]);
    CHECK(abs(mid[0] - 128) <= 2 && abs(mid[2] - 128) <= 2 && mid[1] == 0 && mid[3] == 255, "halfway is purple: %d %d %d %d", mid[0], mid[1], mid[2], mid[3]);
    CHECK(last[2] >= 253 && last[0] <= 2, "the end is blue");

    /* the same stops under another angle or center are the same ramp; other colors are not */
    gp_gradient b = a;
    b.angle = 77;
    b.kind = GP_GRAD_RADIAL;
    b.cx = 0.3f;
    CHECK(gp_gfx_gradient_row(g, &b) == row && gp_gfx_gradient_count(g) == 1, "a ramp depends on the stops alone");
    gp_gradient c = norm((gp_color_stop[]){ { RED, 0 }, { GREEN, 1 } }, 2);
    CHECK(gp_gfx_gradient_row(g, &c) == 1, "different stops, another row");
    gp_gradient d = norm((gp_color_stop[]){ { RED, 0 }, { BLUE, 0.9f } }, 2);
    CHECK(gp_gfx_gradient_row(g, &d) == 2, "different positions, another row");

    /* premultiplied interpolation: transparent red to opaque blue passes through blue of rising alpha, never purple */
    gp_gradient fade = norm((gp_color_stop[]){ { { 1, 0, 0, 0 }, 0 }, { BLUE, 1 } }, 2);
    int fr = gp_gfx_gradient_row(g, &fade);
    const uint8_t *q = texel(g, fr, GP_GRAD_W / 2);
    CHECK(q[0] <= 2 && abs(q[2] - 128) <= 2 && abs(q[3] - 128) <= 2, "halfway from transparent red to blue is half-opaque blue, premultiplied: %d %d %d %d", q[0], q[1], q[2], q[3]);
    q = texel(g, fr, GP_GRAD_W / 4);
    CHECK(q[0] <= 2 && abs(q[2] - 64) <= 2 && abs(q[3] - 64) <= 2, "a quarter of the way: %d %d %d %d", q[0], q[1], q[2], q[3]);
    /* the usual fade: a color to the same color transparent keeps the color */
    gp_gradient same = norm((gp_color_stop[]){ { GREEN, 0 }, { { 0, 1, 0, 0 }, 1 } }, 2);
    int sr = gp_gfx_gradient_row(g, &same);
    q = texel(g, sr, GP_GRAD_W / 2);
    CHECK(q[0] == 0 && q[2] == 0 && abs(q[1] - 128) <= 2 && abs(q[3] - 128) <= 2, "green to transparent green: %d %d %d %d", q[0], q[1], q[2], q[3]);

    /* hard stops */
    gp_gradient hard = norm((gp_color_stop[]){ { RED, 0 }, { RED, 0.5f }, { BLUE, 0.5f }, { BLUE, 1 } }, 4);
    int hr = gp_gfx_gradient_row(g, &hard);
    CHECK(texel(g, hr, GP_GRAD_W / 2 - 2)[0] == 255 && texel(g, hr, GP_GRAD_W / 2 - 2)[2] == 0 && texel(g, hr, GP_GRAD_W / 2 + 2)[2] == 255 && texel(g, hr, GP_GRAD_W / 2 + 2)[0] == 0,
          "a hard stop is a hard edge");
    /* a single stop is a plain color; stops that do not reach 0 and 1 hold their color beyond */
    gp_gradient one = norm((gp_color_stop[]){ { GREEN, 0.5f } }, 1);
    int orow = gp_gfx_gradient_row(g, &one);
    CHECK(texel(g, orow, 0)[1] == 255 && texel(g, orow, GP_GRAD_W - 1)[1] == 255 && texel(g, orow, 5)[0] == 0, "one stop colors the whole ramp");
    gp_gradient inner = norm((gp_color_stop[]){ { RED, 0.25f }, { BLUE, 0.75f } }, 2);
    int mrow = gp_gfx_gradient_row(g, &inner);
    CHECK(texel(g, mrow, 10)[0] == 255 && texel(g, mrow, GP_GRAD_W - 10)[2] == 255, "before the first stop and after the last, their colors");

    /* uploads: the whole table once, then only what changed */
    int r0, r1;
    CHECK(gp_gfx_gradient_take_dirty(g, &r0, &r1) && r0 == 0 && r1 == GP_GRAD_ROWS, "the first upload is everything (%d..%d)", r0, r1);
    CHECK(!gp_gfx_gradient_take_dirty(g, &r0, &r1), "then nothing");
    gp_gradient e = norm((gp_color_stop[]){ { WHITE, 0 }, { RED, 1 } }, 2);
    int er = gp_gfx_gradient_row(g, &e);
    CHECK(gp_gfx_gradient_take_dirty(g, &r0, &r1) && r0 == er && r1 == er + 1, "a new ramp marks its row (%d..%d, row %d)", r0, r1, er);
    gp_gfx_gradient_row(g, &e);
    CHECK(!gp_gfx_gradient_take_dirty(g, &r0, &r1), "a known ramp marks nothing");
    gp_gradient f = norm((gp_color_stop[]){ { WHITE, 0 }, { GREEN, 1 } }, 2), h = norm((gp_color_stop[]){ { WHITE, 0 }, { BLUE, 1 } }, 2);
    int fr2 = gp_gfx_gradient_row(g, &f), hr2 = gp_gfx_gradient_row(g, &h);
    CHECK(gp_gfx_gradient_take_dirty(g, &r0, &r1) && r0 == fr2 && r1 == hr2 + 1, "two new ramps: one range (%d..%d)", r0, r1);

    /* the table fills up: a ramp that does not fit gets -1, the others are still there, a reset starts over */
    int made = gp_gfx_gradient_count(g);
    int refused = 0;
    for (int i = 0; i < GP_GRAD_ROWS + 10; i++) {
        gp_gradient x = norm((gp_color_stop[]){ { { (float)i / 200.f, 0.5f, 0.1f, 1 }, 0 }, { BLUE, 1 } }, 2);
        if (gp_gfx_gradient_row(g, &x) < 0) refused++;
    }
    CHECK(refused >= 10 && gp_gfx_overflowed(g) && gp_gfx_gradient_count(g) == GP_GRAD_ROWS, "overflow: %d refused, %d rows (had %d)", refused, gp_gfx_gradient_count(g), made);
    CHECK(gp_gfx_gradient_row(g, &a) == row && gp_gfx_gradient_row(g, &d) == 2, "ramps already in the table are still found when it is full");
    gp_gfx_reset(g);
    CHECK(!gp_gfx_overflowed(g) && gp_gfx_gradient_count(g) == 0, "a reset empties it");
    CHECK(gp_gfx_gradient_row(g, &d) == 0 && texel(g, 0, GP_GRAD_W - 1)[2] == 255, "and ramps can be made again (row 0 is now the new one)");
    gp_gfx_free(g);
}

int main(void) {
    test_normalize();
    test_ramps();
    printf(failures ? "gfx_test: %d FAILURE(S)\n" : "gfx_test: all checks passed\n", failures);
    return failures != 0;
}
