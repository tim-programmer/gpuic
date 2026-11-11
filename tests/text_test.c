/*
 * Tests of the text system itself (font fallback, itemization, shaping, rasterization, atlases),
 * run against the fonts actually installed. No window or display needed.
 *
 * Needs: a sans-serif font, a CJK font, and a color emoji font. A missing one is reported as a
 * SKIP with the reason, not a pass.
 */
#include "../src/internal.h"

#ifdef GPUIC_HAVE_FONTCONFIG
#include <fontconfig/fontconfig.h>
#else
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#endif
#include <ft2build.h>
#include <hb.h>
#include FT_FREETYPE_H
#include FT_LCD_FILTER_H
#include FT_MULTIPLE_MASTERS_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int failures, skips;
#define CHECK(cond, ...)                                                               \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            failures++;                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                \
            printf(__VA_ARGS__);                                                       \
            printf("\n");                                                              \
        }                                                                              \
    } while (0)

static gp_text_system *ts;
#define MAXG 64

static int shape(const char *s, int weight, float size, gp_glyph_pos *g, float *w) {
    int n = gp_text_shape(ts, weight, size, s, strlen(s), g, MAXG, w);
    return n > MAXG ? MAXG : n;
}

static int shape_dir(const char *s, int dir, gp_glyph_pos *g, float *w) {
    int n = gp_text_shape_dir(ts, 400, 20, s, strlen(s), dir, g, MAXG, w);
    return n > MAXG ? MAXG : n;
}

static const char *path_of(const gp_glyph_pos *g) { return gp_text_font_path(ts, g->font); }

static bool contains_ci(const char *hay, const char *needle) {
    for (size_t i = 0; hay[i]; i++) {
        size_t j = 0;
        while (needle[j] && hay[i + j] && (hay[i + j] | 32) == (needle[j] | 32)) j++;
        if (!needle[j]) return true;
    }
    return false;
}

/* Does the system have a font that covers `cp`? Asked of fontconfig (or, in a build without it, of
 * a plain POSIX directory walk) and FreeType directly, never of the code under test: a test that
 * decides what to skip by asking the library would quietly skip exactly the regressions it exists
 * to catch. */
/* Without fontconfig the scanner picks the default faces by a preference list. A family that is one of its names comes before one that only contains it,
 * and CJK families come last for Latin text even when their names contain a Latin one: Noto Sans before Noto Sans CJK JP (Ubuntu's layout found the CJK
 * file first, and it became the face for all text). */
static void test_family_ranks(void) {
    if (gp_text_debug_family_rank(0, "Noto Sans") < 0) { printf("SKIP: family ranks are fontconfig's in this build\n"); return; }
    CHECK(gp_text_debug_family_rank(0, "Noto Sans CJK JP") == gp_text_debug_family_rank(0, "Source Han Sans SC"), "the CJK families rank together");
    const char *sans[] = { "Noto Sans", "Noto Sans Display", "DejaVu Sans", "Liberation Sans", "Cantarell", "Noto Sans CJK JP", "Some Unknown Face" };
    for (size_t i = 0; i + 1 < sizeof sans / sizeof *sans; i++)
        CHECK(gp_text_debug_family_rank(0, sans[i]) < gp_text_debug_family_rank(0, sans[i + 1]), "sans-serif: %s (%d) before %s (%d)", sans[i],
              gp_text_debug_family_rank(0, sans[i]), sans[i + 1], gp_text_debug_family_rank(0, sans[i + 1]));
    CHECK(gp_text_debug_family_rank(0, "noto sans") == gp_text_debug_family_rank(0, "Noto Sans"), "case does not matter");
    CHECK(gp_text_debug_family_rank(1, "Noto Serif") < gp_text_debug_family_rank(1, "DejaVu Serif") &&
          gp_text_debug_family_rank(1, "DejaVu Serif") < gp_text_debug_family_rank(1, "Noto Serif CJK JP"), "serif: Noto Serif, DejaVu Serif, then Noto Serif CJK");
    CHECK(gp_text_debug_family_rank(2, "DejaVu Sans Mono") < gp_text_debug_family_rank(2, "Noto Sans Mono") &&
          gp_text_debug_family_rank(2, "Noto Sans Mono") < gp_text_debug_family_rank(2, "Noto Sans Mono CJK JP"), "monospace: the plain faces before the CJK one");
}

#ifndef GPUIC_HAVE_FONTCONFIG
static bool dir_has_font_for(FT_Library lib, const char *dir, uint32_t cp, bool color, int depth) {
    DIR *d = opendir(dir);
    if (!d) return false;
    bool found = false;
    for (struct dirent *e; !found && (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            found = depth < 8 && dir_has_font_for(lib, path, cp, color, depth + 1);
            continue;
        }
        const char *dot = strrchr(path, '.');
        if (!dot || (strcasecmp(dot, ".ttf") && strcasecmp(dot, ".otf") && strcasecmp(dot, ".ttc") && strcasecmp(dot, ".otc")))
            continue;
        FT_Face face;
        if (FT_New_Face(lib, path, -1, &face) != 0) continue;
        long n = face->num_faces;
        FT_Done_Face(face);
        for (long i = 0; !found && i < n && i < 64; i++) {
            if (FT_New_Face(lib, path, (FT_Long)i, &face) != 0) continue;
            found = FT_Get_Char_Index(face, cp) != 0 && (!color || FT_HAS_COLOR(face));
            FT_Done_Face(face);
        }
    }
    closedir(d);
    return found;
}

static bool system_has_font_for(uint32_t cp, bool color) {
    FT_Library lib;
    if (FT_Init_FreeType(&lib) != 0) return false;
    bool ok = dir_has_font_for(lib, "/usr/share/fonts", cp, color, 0) ||
              dir_has_font_for(lib, "/usr/local/share/fonts", cp, color, 0);
    FT_Done_FreeType(lib);
    return ok;
}
#else
static bool system_has_font_for(uint32_t cp, bool color) {
    FcPattern *pat = FcPatternCreate();
    FcCharSet *cs = FcCharSetCreate();
    FcCharSetAddChar(cs, cp);
    FcPatternAddCharSet(pat, FC_CHARSET, cs);
    FcCharSetDestroy(cs);
    if (color) FcPatternAddBool(pat, FC_COLOR, FcTrue);
    else FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)"sans-serif");
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res;
    FcPattern *m = FcFontMatch(NULL, pat, &res);
    FcPatternDestroy(pat);
    if (!m) return false;
    FcChar8 *file = NULL;
    int index = 0;
    FcPatternGetString(m, FC_FILE, 0, &file);
    FcPatternGetInteger(m, FC_INDEX, 0, &index);
    bool ok = false;
    FT_Library lib;
    FT_Face face;
    if (file && FT_Init_FreeType(&lib) == 0) {
        if (FT_New_Face(lib, (const char *)file, index, &face) == 0) {
            ok = FT_Get_Char_Index(face, cp) != 0 && (!color || FT_HAS_COLOR(face));
            FT_Done_Face(face);
        }
        FT_Done_FreeType(lib);
    }
    FcPatternDestroy(m);
    return ok;
}
#endif

/* number of nonzero bytes / of pixels with differing channels in a rasterized glyph */
static void atlas_stats(const gp_glyph_entry *e, int *opaque, int *colored) {
    *opaque = *colored = 0;
    const uint8_t *a = e->color ? gp_text_color_atlas(ts) : gp_text_atlas(ts);
    int stride = e->color ? GP_COLOR_ATLAS_SIZE : GP_ATLAS_SIZE;
    for (int y = 0; y < (int)e->h; y++)
        for (int x = 0; x < (int)e->w; x++) {
            size_t o = ((size_t)((int)e->v + y) * stride + (size_t)((int)e->u + x)) * (e->color ? 4 : 1);
            if (e->color) {
                if (a[o + 3] > 0) (*opaque)++;
                if (a[o + 3] > 200 && (abs(a[o] - a[o + 1]) > 24 || abs(a[o + 1] - a[o + 2]) > 24)) (*colored)++;
            } else if (a[o] > 0) {
                (*opaque)++;
            }
        }
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}


/* ------------------------------------------------------------ subpixel positioning */

/* where the ink of a rasterized glyph is, horizontally, from its pen origin (coverage-weighted, in device px) */
static double ink_x(const gp_glyph_entry *e) {
    const uint8_t *a = gp_text_atlas(ts);
    double sum = 0, moment = 0;
    for (int y = 0; y < (int)e->h; y++)
        for (int x = 0; x < (int)e->w; x++) {
            double cov = a[(size_t)((int)e->v + y) * GP_ATLAS_SIZE + (size_t)((int)e->u + x)];
            sum += cov;
            moment += cov * (x + 0.5);
        }
    return sum > 0 ? (double)e->left + moment / sum : 0;
}

static void test_subpixel(void) {
    /* 1. a glyph rasterized for bin k is the glyph moved k/4 of a pixel to the right */
    const char *letters[] = { "l", "I", "o", "e", "H", "W", "a" };
    float sizes[] = { 11.f, 13.f, 16.f, 24.f, 40.f };
    int weights[] = { 400, 700 };
    int checked = 0;
    double mean_shift[GP_SUBPIXEL_BINS] = { 0 };
    int counts[GP_SUBPIXEL_BINS] = { 0 };
    /* The ink of a glyph is measured from the coverage in whole pixels, which for a stem a pixel and a half wide is a
     * little off (up to about a tenth of a pixel) whichever way it is shifted: so each glyph is held to that, and the
     * average over all of them, where the errors cancel, to something much finer. */
    for (int wi = 0; wi < 2; wi++)
        for (size_t li = 0; li < sizeof letters / sizeof *letters; li++)
            for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
                gp_glyph_pos g[MAXG];
                shape(letters[li], weights[wi], sizes[si], g, NULL);
                gp_glyph_entry e0 = gp_text_glyph_at(ts, g[0].font, g[0].glyph, sizes[si], 0);
                if (e0.w == 0) continue;
                double c0 = ink_x(&e0);
                for (int bin = 1; bin < GP_SUBPIXEL_BINS; bin++) {
                    gp_glyph_entry e = gp_text_glyph_at(ts, g[0].font, g[0].glyph, sizes[si], bin);
                    double shift = ink_x(&e) - c0;
                    CHECK(e.w > 0, "'%s' at %.0f px, bin %d: nothing rasterized", letters[li], sizes[si], bin);
                    CHECK(fabs(shift - (double)bin / GP_SUBPIXEL_BINS) < 0.12, "'%s' %d at %.0f px: bin %d moved the ink %.3f px, want %.2f",
                          letters[li], weights[wi], sizes[si], bin, shift, (double)bin / GP_SUBPIXEL_BINS);
                    mean_shift[bin] += shift;
                    counts[bin]++;
                    checked++;
                }
            }
    CHECK(checked > 100, "only %d glyph/bin pairs were checked", checked);
    for (int bin = 1; bin < GP_SUBPIXEL_BINS; bin++)
        if (counts[bin])
            CHECK(fabs(mean_shift[bin] / counts[bin] - (double)bin / GP_SUBPIXEL_BINS) < 0.025, "on average bin %d moved the ink %.3f px, want %.2f",
                  bin, mean_shift[bin] / counts[bin], (double)bin / GP_SUBPIXEL_BINS);

    /* 2. the cache: each bin is its own entry, asked twice it is the same one, nonsense bins mean bin 0 */
    gp_glyph_pos g[MAXG];
    shape("k", 400, 17, g, NULL);
    gp_glyph_entry e[GP_SUBPIXEL_BINS];
    for (int b = 0; b < GP_SUBPIXEL_BINS; b++) e[b] = gp_text_glyph_at(ts, g[0].font, g[0].glyph, 17, b);
    for (int a = 0; a < GP_SUBPIXEL_BINS; a++)
        for (int b = a + 1; b < GP_SUBPIXEL_BINS; b++)
            CHECK(e[a].u != e[b].u || e[a].v != e[b].v, "bins %d and %d share a place in the atlas", a, b);
    for (int b = 0; b < GP_SUBPIXEL_BINS; b++) {
        gp_glyph_entry again = gp_text_glyph_at(ts, g[0].font, g[0].glyph, 17, b);
        CHECK(again.u == e[b].u && again.v == e[b].v, "bin %d was rasterized again", b);
    }
    gp_glyph_entry whole = gp_text_glyph(ts, g[0].font, g[0].glyph, 17);
    CHECK(whole.u == e[0].u && whole.v == e[0].v, "gp_text_glyph is not bin 0");
    gp_glyph_entry bad = gp_text_glyph_at(ts, g[0].font, g[0].glyph, 17, 99);
    CHECK(bad.u == e[0].u && bad.v == e[0].v, "an out-of-range bin is not bin 0");
    {   /* other fonts and sizes with the same glyph number do not collide with these */
        gp_glyph_pos w[MAXG];
        shape("k", 700, 17, w, NULL);
        gp_glyph_entry other = gp_text_glyph_at(ts, w[0].font, w[0].glyph, 17, 2);
        gp_glyph_entry size = gp_text_glyph_at(ts, g[0].font, g[0].glyph, 18, 2);
        CHECK(!(other.u == e[2].u && other.v == e[2].v && w[0].font != g[0].font), "bin 2 of two fonts shares a place");
        CHECK(!(size.u == e[2].u && size.v == e[2].v), "bin 2 of two sizes shares a place");
    }

    /* 3. splitting a pen position */
    unsigned seed = 12345;
    for (int i = 0; i < 20000; i++) {
        seed = seed * 1664525u + 1013904223u;
        float x = (float)(seed >> 8) / 16777216.f * 1500.f - 100.f;
        int pixel, bin;
        gp_text_snap_x(ts, g[0].font, 16.f, x, &pixel, &bin);
        CHECK(bin >= 0 && bin < GP_SUBPIXEL_BINS, "x=%.3f gave bin %d", x, bin);
        CHECK(fabsf((float)pixel + (float)bin / GP_SUBPIXEL_BINS - x) <= 0.5f / GP_SUBPIXEL_BINS + 1e-4f,
              "x=%.4f became pixel %d + bin %d (%.3f)", x, pixel, bin, (float)pixel + (float)bin / GP_SUBPIXEL_BINS);
        if (failures > 20) break;
    }
    {
        int pixel, bin;
        gp_text_snap_x(ts, g[0].font, 16.f, 10.9f, &pixel, &bin);
        CHECK(pixel == 11 && bin == 0, "10.9 should carry to pixel 11, bin 0 (got %d, %d)", pixel, bin);
        gp_text_snap_x(ts, g[0].font, 16.f, 10.3f, &pixel, &bin);
        CHECK(pixel == 10 && bin == 1, "10.3 is pixel 10, bin 1 (got %d, %d)", pixel, bin);
        gp_text_snap_x(ts, g[0].font, 16.f, -0.3f, &pixel, &bin);
        CHECK(pixel == -1 && bin == 3, "-0.3 is pixel -1, bin 3 (got %d, %d)", pixel, bin);
        gp_text_snap_x(ts, g[0].font, 41.f, 10.3f, &pixel, &bin);
        CHECK(pixel == 10 && bin == 0, "above the size limit a glyph is on a whole pixel (got %d, %d)", pixel, bin);
        gp_text_snap_x(ts, g[0].font, 16.f, 10.6f, &pixel, &bin);
        CHECK(pixel == 11 || bin >= 2, "10.6 went to pixel %d bin %d", pixel, bin);
    }

    /* 4. what it is for: a run of the same glyph at an advance that is not a whole number of pixels is evenly spaced.
     * Where each glyph's ink ends up is compared with where it ought to be; rounding to pixels is off by up to half a
     * pixel, positioning by quarters by at most an eighth. */
    {
        const char *run = "iiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiii";
        gp_glyph_pos rg[MAXG];
        float size = 13.5f;
        int n = shape(run, 400, size, rg, NULL);
        gp_glyph_entry base = gp_text_glyph_at(ts, rg[0].font, rg[0].glyph, size, 0);
        double c0 = ink_x(&base);
        double worst_sub = 0, worst_round = 0, sum_sub = 0, sum_round = 0;
        for (int i = 0; i < n; i++) {
            int pixel, bin;
            gp_text_snap_x(ts, rg[i].font, size, rg[i].x, &pixel, &bin);
            gp_glyph_entry e1 = gp_text_glyph_at(ts, rg[i].font, rg[i].glyph, size, bin);
            double err_sub = fabs((double)pixel + ink_x(&e1) - ((double)rg[i].x + c0));
            double err_round = fabs((double)lroundf(rg[i].x) + c0 - ((double)rg[i].x + c0));
            if (err_sub > worst_sub) worst_sub = err_sub;
            if (err_round > worst_round) worst_round = err_round;
            sum_sub += err_sub;
            sum_round += err_round;
        }
        /* (the ink measure itself is good to about a tenth of a pixel, see above) */
        CHECK(worst_sub <= 0.22, "with subpixel positioning the ink is up to %.3f px from where it belongs", worst_sub);
        CHECK(sum_sub / n < 0.5 * (sum_round / n) && sum_sub / n < 0.1, "on average the ink is %.3f px off with subpixel positioning, %.3f with rounding", sum_sub / n, sum_round / n);
        CHECK(worst_round >= 0.3, "the comparison would not notice rounding (it is off by %.3f px at worst): the advance of 'i' at %.1f px is %.3f", worst_round, size, rg[0].advance);
    }

    /* 5. a color bitmap font and the switch: rounding, bin 0 */
    gp_text_set_subpixel(ts, false);
    {
        int pixel, bin;
        gp_text_snap_x(ts, g[0].font, 16.f, 10.3f, &pixel, &bin);
        CHECK(pixel == 10 && bin == 0, "switched off: pixel %d bin %d", pixel, bin);
        gp_text_snap_x(ts, g[0].font, 16.f, 10.6f, &pixel, &bin);
        CHECK(pixel == 11 && bin == 0, "switched off, 10.6: pixel %d bin %d", pixel, bin);
    }
    gp_text_set_subpixel(ts, true);
}


/* the same through the painter: a text element at a fractional position, painted into a scene */
static void test_subpixel_paint(void) {
    const char *text = "iiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiii";
    float scales[] = { 1.f, 1.5f };
    for (int si = 0; si < 2; si++) {
        float scale = scales[si];
        for (int on = 0; on < 2; on++) {
            gp_text_set_subpixel(ts, on);
            gp_arena arena = { 0 }, scratch = { 0 };
            gp_el *t = gp_el_new(&arena, GP_EL_TEXT);
            t->text = text;
            t->ts = (gp_text_style){ 13.5f, 400, 0, { 0, 0, 0, 1 }, GP_TEXT_ALIGN_START, GP_BIDI_AUTO, false, 0, 0 };
            gp_size sz = gp_text_measure_dir(ts, 400, 13.5f, text, -1, false, GP_BIDI_AUTO, NULL, NULL, NULL);
            t->size = (gp_size){ ceilf(sz.w) + 2, sz.h };
            t->pos = (gp_point){ 3.4f, 5.f };
            gp_scene scene = { 0 };
            gp_paint_tree(&scene, &scratch, ts, t, scale, (gp_point){ -100, -100 }, false, false, 0, 0, 0, NULL, NULL);

            gp_glyph_pos g[MAXG];
            int n = shape(text, 400, 13.5f, g, NULL);
            gp_glyph_entry base = gp_text_glyph_at(ts, g[0].font, g[0].glyph, 13.5f * scale, 0);
            double c0 = ink_x(&base);
            int nglyphs = 0;
            double sum_err = 0, worst = 0;
            float seen_u[8];
            int nseen = 0;
            const uint8_t *a = gp_text_atlas(ts);
            for (size_t i = 0; i < scene.len && nglyphs < n; i++) {
                const gp_instance *in = &scene.items[i];
                if (in->kind != GP_PRIM_GLYPH) continue;
                int u = (int)lroundf(in->uv[0] * GP_ATLAS_SIZE), v = (int)lroundf(in->uv[1] * GP_ATLAS_SIZE);
                int w = (int)lroundf(in->uv[2] * GP_ATLAS_SIZE), h = (int)lroundf(in->uv[3] * GP_ATLAS_SIZE);
                double sum = 0, moment = 0;
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++) {
                        double cov = a[(size_t)(v + y) * GP_ATLAS_SIZE + (size_t)(u + x)];
                        sum += cov;
                        moment += cov * (x + 0.5);
                    }
                double ink = (double)in->bounds[0] * scale + (sum > 0 ? moment / sum : 0);
                double want = ((double)t->pos.x + (double)g[nglyphs].x) * scale + c0;
                double err = fabs(ink - want);
                sum_err += err;
                if (err > worst) worst = err;
                bool known = false;
                for (int k = 0; k < nseen; k++) known |= seen_u[k] == in->uv[0];
                if (!known && nseen < 8) seen_u[nseen++] = in->uv[0];
                nglyphs++;
            }
            CHECK(nglyphs == n, "scale %.1f: %d glyph instances for %d glyphs", scale, nglyphs, n);
            if (on) {
                CHECK(worst <= 0.25 && sum_err / n < 0.1, "scale %.1f: painted with subpixel positions the ink is %.3f px off on average, %.3f at worst", scale, sum_err / n, worst);
                CHECK(nseen >= 3, "scale %.1f: the same letter was drawn with %d different bitmaps along a line with a fractional advance", scale, nseen);
            } else {
                CHECK(nseen == 1, "scale %.1f: with subpixel positioning off the same letter used %d different bitmaps", scale, nseen);
                CHECK(sum_err / n > 0.1, "scale %.1f: with it off the line is just as even (%.3f px): the check cannot tell", scale, sum_err / n);
            }
            free(scene.items);
            gp_arena_free(&arena);
            gp_arena_free(&scratch);
        }
    }
    gp_text_set_subpixel(ts, true);
}

static void test_subpixel_color(void) {
    gp_glyph_pos g[MAXG];
    shape("\xf0\x9f\x98\x80", 400, 16, g, NULL);
    int pixel, bin;
    gp_text_snap_x(ts, g[0].font, 16.f, 10.3f, &pixel, &bin);
    CHECK(bin == 0, "a bitmap emoji was given subpixel bin %d", bin);
    gp_glyph_entry a = gp_text_glyph_at(ts, g[0].font, g[0].glyph, 16.f, 0);
    gp_glyph_entry b = gp_text_glyph_at(ts, g[0].font, g[0].glyph, 16.f, 2);
    CHECK(a.w > 0 && a.u == b.u && a.v == b.v, "a bitmap emoji has one shape whatever bin is asked for");
}

/* ------------------------------------------------------------ scenarios */

static void test_latin(void) {
    gp_glyph_pos g[MAXG];
    float w;
    int n = shape("Hello", 400, 16, g, &w);
    CHECK(n == 5, "\"Hello\" shaped to %d glyphs", n);
    for (int i = 1; i < n; i++) CHECK(g[i].font == g[0].font, "Latin text must stay in one font");
    for (int i = 0; i < n; i++) CHECK(g[i].cluster == (uint32_t)i, "cluster %d is %u", i, g[i].cluster);
    CHECK(w > 30 && w < 60, "\"Hello\" at 16px is %.1f wide", w);
    float sum = 0;
    for (int i = 0; i < n; i++) sum += g[i].advance;
    CHECK(fabsf(sum - w) < 0.01f, "advances sum to %.2f, width says %.2f", sum, w);

    /* characters the primary font has must not go to a fallback, even if others have them too */
    gp_glyph_pos p[MAXG], q[MAXG];
    shape("a", 400, 16, p, NULL);
    n = shape("\xc3\xa9\xc2\xbd", 400, 16, q, NULL); /* é ½ */
    CHECK(n == 2 && q[0].font == p[0].font && q[1].font == p[0].font, "é and ½ left the primary font");

    float w16, w32;
    shape("Hello", 400, 16, g, &w16);
    shape("Hello", 400, 32, g, &w32);
    CHECK(fabsf(w32 - 2 * w16) < 0.05f * w16, "width should scale with size (%.1f vs %.1f)", w16, w32);
}

static void test_cjk(void) {
    gp_glyph_pos g[MAXG], a[MAXG];
    float w;
    shape("a", 400, 20, a, NULL);
    int n = shape("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", 400, 20, g, &w); /* 日本語 */
    if (n != 3) { CHECK(0, "日本語 shaped to %d glyphs", n); return; }
    for (int i = 0; i < 3; i++) {
        CHECK(g[i].font != a[0].font, "glyph %d stayed in the primary font (%s)", i, path_of(&g[i]));
        CHECK(g[i].glyph != 0, "glyph %d is .notdef", i);
        CHECK(g[i].cluster == (uint32_t)(i * 3), "cluster %d is %u", i, g[i].cluster);
        CHECK(fabsf(g[i].advance - 20.f) < 2.f, "ideograph advance %.1f at 20px (expected ~1em)", g[i].advance);
    }
    CHECK(g[0].font == g[1].font && g[1].font == g[2].font, "one run, one font");

    gp_glyph_entry e = gp_text_glyph(ts, g[0].font, g[0].glyph, 32.f);
    int opaque, colored;
    atlas_stats(&e, &opaque, &colored);
    CHECK(!e.color && e.w >= 20 && e.h >= 20, "日 rasterized to %.0fx%.0f color=%d", e.w, e.h, e.color);
    CHECK(opaque > 100, "日 has only %d lit pixels", opaque);

    /* kana and Hangul come from fallback fonts too */
    n = shape("\xe3\x81\x82\xed\x95\x9c", 400, 20, g, &w); /* あ 한 */
    CHECK(n == 2 && g[0].glyph != 0 && g[1].glyph != 0, "kana / hangul missing (glyphs %u, %u)", g[0].glyph, g[1].glyph);

    /* mixed: the run boundaries land on the right bytes, and Latin is back in the primary font */
    n = shape("a\xe6\x97\xa5" "b", 400, 20, g, &w);
    CHECK(n == 3 && g[0].font == a[0].font && g[1].font != a[0].font && g[2].font == a[0].font,
          "mixed run fonts: %d %d %d", g[0].font, g[1].font, g[2].font);
    CHECK(g[1].x > g[0].x && g[2].x > g[1].x, "pen must advance across runs");

    /* bold asks fontconfig for a bold fallback; the glyphs must still be real */
    n = shape("\xe6\x97\xa5", 700, 20, g, &w);
    CHECK(n == 1 && g[0].glyph != 0, "bold 日 is .notdef");
}

static void test_emoji(void) {
    gp_glyph_pos g[MAXG], a[MAXG];
    float w;
    shape("a", 400, 20, a, NULL);
    int n = shape("\xf0\x9f\x98\x80", 400, 20, g, &w); /* 😀 */
    if (n != 1) { CHECK(0, "😀 shaped to %d glyphs", n); return; }
    CHECK(contains_ci(path_of(&g[0]), "emoji"), "😀 came from %s, not the color emoji font", path_of(&g[0]));
    CHECK(g[0].glyph != 0, "😀 is .notdef");
    CHECK(fabsf(g[0].advance - 20.f * 2550 / 2048) < 0.5f, "emoji advance %.2f", g[0].advance);

    gp_glyph_entry e = gp_text_glyph(ts, g[0].font, g[0].glyph, 20.f);
    int opaque, colored;
    atlas_stats(&e, &opaque, &colored);
    CHECK(e.color, "emoji must use the color atlas");
    CHECK(e.w >= 18 && e.w <= 28 && e.h >= 18 && e.h <= 28, "emoji bitmap scaled to %.0fx%.0f at 20px", e.w, e.h);
    CHECK(opaque > 200, "emoji has only %d visible pixels", opaque);
    CHECK(colored > 50, "emoji looks monochrome (%d colored pixels)", colored);
    CHECK(e.top > 0 && e.top < 30, "emoji sits at top=%.0f above the baseline", e.top);

    /* the area filter keeps the image: its coverage must not collapse when scaled down a lot */
    gp_glyph_entry small = gp_text_glyph(ts, g[0].font, g[0].glyph, 10.f);
    gp_glyph_entry big = gp_text_glyph(ts, g[0].font, g[0].glyph, 40.f);
    int o_small, o_big, dummy;
    atlas_stats(&small, &o_small, &dummy);
    atlas_stats(&big, &o_big, &dummy);
    CHECK(big.w > small.w * 3 && o_big > o_small * 8, "scaling: %.0f px -> %d lit, %.0f px -> %d lit", small.w, o_small, big.w, o_big);

    /* sequences collapse into one glyph (HarfBuzz applies the font's GSUB ligatures) */
    static const struct { const char *name, *s; int glyphs; } seq[] = {
        { "family (ZWJ)", "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7", 1 },
        { "flag (2 regional indicators)", "\xf0\x9f\x87\xaf\xf0\x9f\x87\xb5", 1 },
        { "skin tone", "\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd", 1 },
        { "keycap", "1\xef\xb8\x8f\xe2\x83\xa3", 1 },
        { "heart + VS16 (text default, emoji by request)", "\xe2\x9d\xa4\xef\xb8\x8f", 1 },
        { "two separate emoji", "\xf0\x9f\x91\x8b\xf0\x9f\x91\x8b", 2 },
    };
    for (size_t i = 0; i < sizeof seq / sizeof *seq; i++) {
        n = shape(seq[i].s, 400, 20, g, &w);
        CHECK(n == seq[i].glyphs, "%s shaped to %d glyphs, expected %d", seq[i].name, n, seq[i].glyphs);
        if (n >= 1) CHECK(contains_ci(path_of(&g[0]), "emoji"), "%s not in the emoji font (%s)", seq[i].name, path_of(&g[0]));
    }

    /* clusters are byte offsets, so a sequence is addressable as one unit: a 👍🏽 b */
    n = shape("a\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd" "b", 400, 20, g, &w);
    CHECK(n == 3 && g[0].cluster == 0 && g[1].cluster == 1 && g[2].cluster == 9,
          "clusters of a👍🏽b: %u %u %u (n=%d)", g[0].cluster, g[1].cluster, g[2].cluster, n);
    CHECK(g[0].font == a[0].font && g[2].font == a[0].font && g[1].font != a[0].font, "emoji run isolated");
}

static void test_missing_and_cache(void) {
    gp_glyph_pos g[MAXG];
    float w;
    int n = shape("\xf4\x8f\xbf\xbd", 400, 16, g, &w); /* U+10FFFD, private use: nothing has it */
    CHECK(n == 1, "uncovered codepoint shaped to %d glyphs", n);
    CHECK(g[0].glyph == 0, "an uncovered codepoint should draw .notdef (glyph %u)", g[0].glyph);

    /* the shape cache must return identical results, and key on the size */
    float w1, w2, w3;
    shape("cached text", 400, 16, g, &w1);
    shape("cached text", 400, 16, g, &w2);
    shape("cached text", 400, 17, g, &w3);
    CHECK(w1 == w2, "cached shaping differs (%.3f vs %.3f)", w1, w2);
    CHECK(w3 > w1, "a different size must not hit the 16px cache entry");

    /* fallback must cost one fontconfig query per font, not per character */
    char buf[8192];
    size_t o = 0;
    for (uint32_t cp = 0x4e00; cp < 0x4e00 + 2000 && o + 4 < sizeof buf; cp++) {
        buf[o++] = (char)(0xe0 | (cp >> 12));
        buf[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        buf[o++] = (char)(0x80 | (cp & 0x3f));
    }
    buf[o] = 0;
    int fonts_before = gp_text_font_count(ts);
    double t0 = now_ms();
    gp_glyph_pos *many = malloc(2100 * sizeof *many);
    int cnt = gp_text_shape(ts, 400, 16, buf, o, many, 2100, &w);
    double ms = now_ms() - t0;
    CHECK(cnt == 2000, "2000 ideographs shaped to %d glyphs", cnt);
    CHECK(ms < 400, "shaping 2000 distinct ideographs took %.0f ms (a fontconfig query per character?)", ms);
    CHECK(gp_text_font_count(ts) - fonts_before <= 3, "%d fonts were loaded for one script", gp_text_font_count(ts) - fonts_before);
    int notdef = 0;
    for (int i = 0; i < cnt; i++) notdef += many[i].glyph == 0;
    CHECK(notdef < 200, "%d of 2000 common ideographs are .notdef", notdef);
    free(many);
    printf("  (2000 ideographs: %.0f ms, %d fonts loaded in total)\n", ms, gp_text_font_count(ts));
}

/* Hebrew and Arabic strings (UTF-8): shin lamed vav final-mem; alef bet gimel; ... */
#define HEB_SHALOM "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d"          /* שלום */
#define HEB_ABG    "\xd7\x90\xd7\x91\xd7\x92"                   /* אבג  */
#define AR_BB      "\xd8\xa8\xd8\xa8"                            /* بب   */
#define AR_LAM_ALEF "\xd9\x84\xd8\xa7"                          /* لا   */

static void expect_clusters(const char *what, const gp_glyph_pos *g, int n, const int *want, int nwant) {
    CHECK(n == nwant, "%s: %d glyphs, expected %d", what, n, nwant);
    for (int i = 0; i < n && i < nwant; i++)
        CHECK((int)g[i].cluster == want[i], "%s: glyph %d has cluster %u, expected %d", what, i, g[i].cluster, want[i]);
}

/* UAX #9: levels, reordering of runs, direction detection, mirroring, joining. */
static void test_bidi(void) {
    gp_glyph_pos g[MAXG];
    float w;

    /* plain left-to-right text is untouched: logical order, level 0 */
    int n = shape("abc", 400, 20, g, &w);
    CHECK(n == 3 && g[0].level == 0 && g[0].cluster == 0 && g[2].cluster == 2, "LTR text changed");

    /* a Hebrew word arrives REVERSED (visual order), all at level 1, with logical clusters */
    n = shape(HEB_SHALOM, 400, 20, g, &w);
    expect_clusters("Hebrew word", g, n, (int[]){ 6, 4, 2, 0 }, 4);
    for (int i = 0; i < n; i++) CHECK(g[i].level == 1, "Hebrew glyph %d at level %d", i, g[i].level);
    for (int i = 1; i < n; i++) CHECK(g[i].x > g[i - 1].x, "visual glyph x must increase left to right");
    CHECK(g[0].glyph != 0, "Hebrew is .notdef: no font found");

    /* mixed: the Hebrew run is reversed in place, everything around it keeps its order */
    n = shape("abc " HEB_ABG " def", 400, 20, g, &w);
    /* bytes: "abc " 0-3, alef 4-5, bet 6-7, gimel 8-9, " def" 10-13 */
    expect_clusters("mixed", g, n, (int[]){ 0, 1, 2, 3, 8, 6, 4, 10, 11, 12, 13 }, 11);
    static const int lv[] = { 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0 };
    for (int i = 0; i < n && i < 11; i++) CHECK(g[i].level == lv[i], "mixed: glyph %d at level %d, expected %d", i, g[i].level, lv[i]);
    float sum = 0;
    for (int i = 0; i < n; i++) sum += g[i].advance;
    CHECK(fabsf(sum - w) < 0.01f, "mixed: advances %.2f vs width %.2f", sum, w);

    /* Latin words in an RTL paragraph stay in reading order: rule N1 gives the space between two
     * Latin words the Latin direction, so "abc def" is ONE level-2 run (this is not a reversal) */
    n = shape_dir("abc def", GP_BIDI_RTL, g, &w);
    expect_clusters("Latin in an RTL paragraph", g, n, (int[]){ 0, 1, 2, 3, 4, 5, 6 }, 7);
    CHECK(g[0].level == 2 && g[3].level == 2, "Latin in an RTL paragraph is at level %d, expected 2", g[0].level);

    /* In an RTL paragraph the RUNS read right to left. Rule L2 by hand for "abc אבג def":
     * levels 2 2 2 1 1 1 1 1 2 2 2, reverse each level-2 sequence, then reverse the whole line, giving
     * d e f _ gimel bet alef _ a b c (clusters 11 12 13 10 8 6 4 3 0 1 2). */
    n = shape_dir("abc " HEB_ABG " def", GP_BIDI_RTL, g, &w);
    expect_clusters("mixed in an RTL paragraph", g, n, (int[]){ 11, 12, 13, 10, 8, 6, 4, 3, 0, 1, 2 }, 11);
    static const int lv2[] = { 2, 2, 2, 1, 1, 1, 1, 1, 2, 2, 2 };
    for (int i = 0; i < n && i < 11; i++) CHECK(g[i].level == lv2[i], "RTL paragraph: glyph %d at level %d, expected %d", i, g[i].level, lv2[i]);
    for (int i = 1; i < n; i++) CHECK(g[i].x > g[i - 1].x, "RTL paragraph: glyph x must increase");

    /* forcing LTR on Hebrew keeps the word reversed inside an LTR paragraph */
    n = shape_dir(HEB_ABG, GP_BIDI_LTR, g, &w);
    expect_clusters("Hebrew in an LTR paragraph", g, n, (int[]){ 4, 2, 0 }, 3);

    /* P2/P3: the first strong character decides; digits and neutrals do not */
    CHECK(!gp_text_is_rtl("abc " HEB_ABG, 4 + 6, GP_BIDI_AUTO), "Latin first: not RTL");
    CHECK(gp_text_is_rtl(HEB_ABG " abc", 6 + 4, GP_BIDI_AUTO), "Hebrew first: RTL");
    CHECK(gp_text_is_rtl("123 " HEB_ABG, 4 + 6, GP_BIDI_AUTO), "digits do not decide, the Hebrew does: RTL");
    CHECK(!gp_text_is_rtl("123", 3, GP_BIDI_AUTO) && !gp_text_is_rtl("", 0, GP_BIDI_AUTO), "no strong character: LTR");
    CHECK(gp_text_is_rtl("abc", 3, GP_BIDI_RTL) && !gp_text_is_rtl(HEB_ABG, 6, GP_BIDI_LTR), "a forced direction wins");

    /* brackets mirror in RTL runs: "(א)" reads as ( א ) right to left, so the glyph on the LEFT is the
     * mirror image of the one stored first */
    gp_glyph_pos open[MAXG], close[MAXG];
    shape("(", 400, 20, open, NULL);
    shape(")", 400, 20, close, NULL);
    n = shape_dir("(" "\xd7\x90" ")", GP_BIDI_RTL, g, &w);
    CHECK(n == 3 && g[0].cluster == 3 && g[0].glyph == open[0].glyph,
          "leftmost glyph is logical ')' drawn as '(' (cluster %u glyph %u, '(' is %u)", g[0].cluster, g[0].glyph, open[0].glyph);
    CHECK(n == 3 && g[2].cluster == 0 && g[2].glyph == close[0].glyph, "rightmost glyph is logical '(' drawn as ')'");

    /* Arabic letters take joining forms. Which glyphs a font uses for them (one glyph or a base plus
     * dots, a lam-alef ligature or two joined letters) is the font's business, so the expectation is
     * HarfBuzz run directly, right to left in the Arabic script, on the font gpuic chose. */
    {
        static const char *const words[] = { "\xd8\xa8", AR_BB, AR_LAM_ALEF };
        static const char *const what[] = { "lone beh", "beh beh", "lam alef" };
        gp_glyph_pos got[3][MAXG];
        int ngot[3];
        for (int w_i = 0; w_i < 3; w_i++) {
            ngot[w_i] = shape(words[w_i], 400, 20, got[w_i], NULL);
            CHECK(ngot[w_i] > 0, "%s: no glyphs", what[w_i]);
            if (ngot[w_i] <= 0) continue;
            const char *path = gp_text_font_path(ts, got[w_i][0].font);
            hb_blob_t *blob = path ? hb_blob_create_from_file_or_fail(path) : NULL;
            CHECK(blob != NULL, "%s: cannot reopen the font gpuic used", what[w_i]);
            if (!blob) continue;
            hb_face_t *face = hb_face_create(blob, 0);
            hb_font_t *font = hb_font_create(face);
            hb_buffer_t *buf = hb_buffer_create();
            hb_buffer_add_utf8(buf, words[w_i], -1, 0, -1);
            hb_buffer_set_direction(buf, HB_DIRECTION_RTL);
            hb_buffer_set_script(buf, HB_SCRIPT_ARABIC);
            hb_buffer_set_language(buf, hb_language_from_string("ar", -1));
            hb_shape(font, buf, NULL, 0);
            unsigned nref;
            hb_glyph_info_t *ref = hb_buffer_get_glyph_infos(buf, &nref);
            CHECK((int)nref == ngot[w_i], "%s: gpuic made %d glyphs, HarfBuzz %u", what[w_i], ngot[w_i], nref);
            for (unsigned i = 0; i < nref && (int)i < ngot[w_i]; i++)
                CHECK(got[w_i][i].glyph == ref[i].codepoint && got[w_i][i].cluster == ref[i].cluster,
                      "%s glyph %u: gpuic %u/cluster %u, HarfBuzz %u/cluster %u", what[w_i], i, got[w_i][i].glyph,
                      got[w_i][i].cluster, ref[i].codepoint, ref[i].cluster);
            hb_buffer_destroy(buf);
            hb_font_destroy(font);
            hb_face_destroy(face);
            hb_blob_destroy(blob);
        }
        /* independent of any font: the first beh of a word looks different from a lone beh, and the
         * second (final form) from both. The glyphs of one logical letter share a cluster. */
        const gp_glyph_pos *word = got[1];
        int nw = ngot[1];
        CHECK(nw >= 2 && word[0].cluster == 2 && word[nw - 1].cluster == 0, "Arabic is not reversed (first cluster %u, last %u)",
              nw ? word[0].cluster : 0, nw ? word[nw - 1].cluster : 0);
        for (int i = 0; i < nw; i++) CHECK(word[i].level == 1, "Arabic glyph %d at level %d", i, word[i].level);
        uint32_t lone[MAXG], first[MAXG], second[MAXG];
        int nl = 0, nf = 0, ns = 0;
        for (int i = 0; i < ngot[0]; i++) lone[nl++] = got[0][i].glyph;
        for (int i = 0; i < nw; i++) {
            if (word[i].cluster == 0) first[nf++] = word[i].glyph;
            else second[ns++] = word[i].glyph;
        }
        CHECK(nf != nl || memcmp(first, lone, (size_t)nl * sizeof *lone) != 0, "initial beh looks like a lone beh: not shaped contextually");
        CHECK(ns != nl || memcmp(second, lone, (size_t)nl * sizeof *lone) != 0, "final beh looks like a lone beh: not shaped contextually");
        CHECK(ns != nf || memcmp(second, first, (size_t)nf * sizeof *first) != 0, "initial and final beh look the same");
    }

    /* the shape cache must not confuse directions */
    gp_glyph_pos a[MAXG], b[MAXG];
    shape_dir("abc " HEB_ABG, GP_BIDI_LTR, a, NULL);
    shape_dir("abc " HEB_ABG, GP_BIDI_RTL, b, NULL);
    shape_dir("abc " HEB_ABG, GP_BIDI_LTR, a, NULL);
    CHECK(a[0].cluster == 0 && b[0].cluster == 8, "LTR and RTL shaping of one string got mixed up in the cache (%u, %u)", a[0].cluster, b[0].cluster);

    /* a wrapped RTL paragraph: every line must fit, and lines are measured with the paragraph's direction */
    gp_arena ar = { 0 };
    gp_text_line *lines;
    int nlines;
    const char *he = HEB_SHALOM " " HEB_ABG " " HEB_SHALOM " " HEB_ABG " " HEB_SHALOM " " HEB_ABG " " HEB_SHALOM " " HEB_ABG;
    gp_size sz = gp_text_measure_dir(ts, 400, 20, he, 90.f, false, GP_BIDI_AUTO, &ar, &lines, &nlines);
    CHECK(nlines >= 3, "a 60-char Hebrew sentence in 90 px made %d line(s)", nlines);
    CHECK(sz.w <= 90.5f, "wrapped Hebrew is %.1f px wide", sz.w);
    for (int i = 0; i < nlines; i++) {
        float lw;
        gp_text_shape_dir(ts, 400, 20, he + lines[i].start, lines[i].end - lines[i].start, GP_BIDI_RTL, NULL, 0, &lw);
        CHECK(lw <= 90.5f, "Hebrew line %d is %.1f px wide when shaped as painted", i, lw);
    }
    gp_arena_free(&ar);
}

static void test_atlas_overflow(void) {
    gp_glyph_pos g[MAXG];
    shape("\xe6\x97\xa5", 400, 20, g, NULL);
    int font = g[0].font;
    /* rasterize one glyph at hundreds of sizes until the alpha atlas runs out */
    int guard = 0;
    for (float px = 12.f; !gp_text_atlas_overflowed(ts) && guard < 5000; px += 0.25f, guard++)
        gp_text_glyph(ts, font, g[0].glyph, px > 300 ? 12.f + (px - 300) * 0.01f : px);
    /* sizes are quantized to 1/4 px: sweep many glyphs instead if sizes did not suffice */
    for (uint32_t gid = 1; !gp_text_atlas_overflowed(ts) && gid < 20000; gid++) gp_text_glyph(ts, font, gid, 64.f);
    CHECK(gp_text_atlas_overflowed(ts), "could not fill the atlas to test overflow");

    gp_text_atlas_reset(ts);
    CHECK(!gp_text_atlas_overflowed(ts), "reset must clear the overflow flag");
    gp_glyph_entry e = gp_text_glyph(ts, font, g[0].glyph, 20.f);
    int opaque, colored;
    atlas_stats(&e, &opaque, &colored);
    CHECK(e.w > 0 && opaque > 50, "glyph unusable after an atlas reset (%.0fx%.0f, %d lit)", e.w, e.h, opaque);
    int dx, dy, dw, dh;
    CHECK(gp_text_atlas_take_dirty(ts, &dx, &dy, &dw, &dh) && dw == GP_ATLAS_SIZE && dh == GP_ATLAS_SIZE,
          "after a reset the whole atlas must be re-uploaded (dirty %dx%d)", dw, dh);
}

static void test_wrapping(void) {
    /* CJK text has no spaces: it must still wrap, between characters */
    gp_arena a = { 0 };
    gp_text_line *lines;
    int nlines;
    const char *s = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe3\x83\x86\xe3\x82\xad\xe3\x82\xb9\xe3\x83\x88"
                    "\xe3\x81\xaf\xe3\x82\xb9\xe3\x83\x9a\xe3\x83\xbc\xe3\x82\xb9\xe3\x81\x8c\xe3\x81\xaa\xe3\x81\x84";
    gp_size sz = gp_text_measure(ts, 400, 16, s, 100.f, false, &a, &lines, &nlines);
    CHECK(nlines >= 3, "20 kana/kanji at 100px wide made only %d line(s)", nlines);
    CHECK(sz.w <= 100.5f, "wrapped width %.1f exceeds the 100px limit", sz.w);
    for (int i = 0; i < nlines; i++) CHECK(lines[i].width <= 100.5f, "line %d is %.1f wide", i, lines[i].width);

    /* and never starts a line with closing punctuation (kinsoku) */
    const char *p = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x80\x82\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x80\x82";
    gp_size ps = gp_text_measure(ts, 400, 16, p, 50.f, false, &a, &lines, &nlines);
    (void)ps;
    for (int i = 0; i < nlines; i++)
        CHECK(!(lines[i].end > lines[i].start && strncmp(p + lines[i].start, "\xe3\x80\x82", 3) == 0),
              "line %d starts with a full stop", i);

    /* Latin still wraps at spaces, exactly as before */
    gp_size l = gp_text_measure(ts, 400, 16, "alpha beta gamma delta", 80.f, false, &a, &lines, &nlines);
    CHECK(nlines >= 2 && l.w <= 80.5f, "Latin wrap: %d lines, %.1f wide", nlines, l.w);
    gp_arena_free(&a);
}

/* Does the font file at `path` (face 0) cover `cp`? Asked of FreeType directly. */
static bool has_glyph_in(const char *path, uint32_t cp) {
    FT_Library lib;
    FT_Face face;
    bool ok = false;
    if (FT_Init_FreeType(&lib) != 0) return false;
    if (FT_New_Face(lib, path, 0, &face) == 0) {
        ok = FT_Get_Char_Index(face, cp) != 0;
        FT_Done_Face(face);
    }
    FT_Done_FreeType(lib);
    return ok;
}

/* ---------------------------------------------------------------- font registration */

/* Path of the font that draws the first glyph of `text` in text system `t`, or NULL. */
static const char *font_for(gp_text_system *t, const char *text, int weight) {
    gp_glyph_pos g[MAXG];
    int n = gp_text_shape(t, weight, 20, text, strlen(text), g, MAXG, NULL);
    return n > 0 ? gp_text_font_path(t, g[0].font) : NULL;
}
static bool same_path(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }

static gp_text_system *fresh(bool system) {
    gp_text_system *t = gp_text_system_new();
    if (t && !system) gp_text_use_system_fonts(t, false);
    return t;
}

static unsigned char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = n > 0 ? malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *len = buf ? (size_t)n : 0;
    return buf;
}

/* Registered fonts, tested with files the system itself picked: R = the regular sans, B = the bold
 * sans, C = a CJK font, E = a color emoji font. Fresh text systems with the system disabled prove
 * that registration alone is what draws. */
static void test_registration(const char *R, const char *B, const char *C, const char *E) {
    const char *HEB = "\xd7\x90"; /* א */
    bool have_bold = B && !same_path(R, B);
    bool cjk_distinct = C && !same_path(R, C);
    bool sys_hebrew = system_has_font_for(0x5d0, false);

    /* one registered font, system off: it draws everything, and nothing else is ever loaded */
    gp_text_system *t = fresh(false);
    CHECK(gp_text_font_count(t) == 0, "a fresh text system has loaded no fonts");
    CHECK(gp_text_add_font_file(t, R, 0, 0), "registering %s", R);
    CHECK(same_path(font_for(t, "A", 400), R), "regular text uses the registered font");
    font_for(t, HEB, 400);
    CHECK(gp_text_font_count(t) == 1, "with the system disabled nothing but the registered font is loaded (%d fonts)", gp_text_font_count(t));
    CHECK(gp_text_add_font_file(t, R, 0, 0) && gp_text_font_count(t) == 1, "registering the same file again adds no font");
    CHECK(same_path(font_for(t, "A", 700), R), "bold with only a regular registered: the same file, thickened");
    CHECK(gp_text_font_count(t) == 2, "the thickened copy is the only extra font (%d fonts)", gp_text_font_count(t));
    gp_text_system_free(t);

    /* the same Hebrew text with the system enabled finds a system font, so the check above means something */
    if (sys_hebrew) {
        t = fresh(true);
        gp_text_add_font_file(t, R, 0, 0);
        const char *h = font_for(t, HEB, 400);
        CHECK(h && !same_path(h, R) && gp_text_font_count(t) >= 2, "system enabled: Hebrew comes from a system font (%s)", h ? h : "none");
        gp_text_use_system_fonts(t, false);
        gp_text_system_free(t);
        t = fresh(true);
        gp_text_add_font_file(t, R, 0, 0);
        gp_text_use_system_fonts(t, false);
        CHECK(same_path(font_for(t, HEB, 400), R), "disabling the system afterwards stops it being consulted");
        gp_text_use_system_fonts(t, true);
        const char *h2 = font_for(t, HEB, 400);
        CHECK(h2 && !same_path(h2, R), "re-enabling the system finds the Hebrew font again");
        gp_text_system_free(t);
    }

    /* nothing registered and no system: no crash, no fonts (it does log "could not load a sans-serif font") */
    t = fresh(false);
    gp_glyph_pos g[MAXG];
    int n = gp_text_shape(t, 400, 20, "Hello", 5, g, MAXG, NULL);
    CHECK(n >= 0 && gp_text_font_count(t) == 0, "no fonts at all: shaping survives (%d glyphs, %d fonts)", n, gp_text_font_count(t));
    gp_text_system_free(t);

    /* weight picks the face: regular for 400, bold for 700, whatever the registration order */
    if (have_bold) {
        for (int order = 0; order < 2; order++) {
            t = fresh(false);
            gp_text_add_font_file(t, order ? B : R, 0, 0);
            gp_text_add_font_file(t, order ? R : B, 0, 0);
            CHECK(same_path(font_for(t, "A", 400), R) && same_path(font_for(t, "A", 700), B),
                  "weights choose the face (registered %s first)", order ? "bold" : "regular");
            gp_text_system_free(t);
        }
        /* an explicit weight overrides the font's own */
        t = fresh(false);
        gp_text_add_font_file(t, R, 700, 0);
        gp_text_add_font_file(t, B, 400, 0);
        CHECK(same_path(font_for(t, "A", 400), B) && same_path(font_for(t, "A", 700), R), "explicit weights override the OS/2 table");
        gp_text_system_free(t);
        /* registering a file again changes its weight, not its place */
        t = fresh(false);
        gp_text_add_font_file(t, R, 0, 0);
        gp_text_add_font_file(t, B, 0, 0);
        CHECK(same_path(font_for(t, "A", 400), R), "before: regular text uses the regular face");
        gp_text_add_font_file(t, R, 900, 0); /* now further from 400 than the bold face is */
        CHECK(gp_text_font_count(t) == 2 && same_path(font_for(t, "A", 400), B),
              "re-registering with a new weight updates it, without adding a font (%d)", gp_text_font_count(t));
        gp_text_system_free(t);
    } else {
        skips++;
        printf("SKIP: the system's regular and bold sans are one file, so weight selection can't be shown\n");
    }

    /* characters the primary lacks come from other registered fonts */
    if (cjk_distinct && !has_glyph_in(R, 0x65e5)) {
        t = fresh(false);
        gp_text_add_font_file(t, R, 0, 0);
        gp_text_add_font_file(t, C, 0, 0);
        CHECK(same_path(font_for(t, "\xe6\x97\xa5", 400), C), "a character the first font lacks comes from the second");
        CHECK(same_path(font_for(t, "A", 400), R), "Latin still uses the first-registered font");
        gp_text_system_free(t);
    } else if (cjk_distinct) {
        skips++;
        printf("SKIP: the regular sans already covers CJK\n");
    }

    /* registered fonts win over system ones: C covers Latin too, and the system would pick R */
    if (cjk_distinct) {
        t = fresh(true);
        CHECK(same_path(font_for(t, "A", 400), R), "baseline: the system picks the regular sans");
        gp_text_system_free(t);
        t = fresh(true);
        gp_text_add_font_file(t, C, 0, 0);
        CHECK(same_path(font_for(t, "A", 400), C), "a registered font beats the system's choice");
        gp_text_system_free(t);

        /* registering after text was shaped: earlier choices must not stick */
        t = fresh(true);
        CHECK(same_path(font_for(t, "A", 400), R), "before registering: the system's font");
        gp_text_add_font_file(t, C, 0, 0);
        CHECK(same_path(font_for(t, "A", 400), C), "after registering: the cached choice is dropped");
        gp_text_system_free(t);
    }

    /* a registered color font draws emoji, and as a color glyph */
    if (E) {
        t = fresh(false);
        gp_text_add_font_file(t, R, 0, 0);
        gp_text_add_font_file(t, E, 0, 0);
        n = gp_text_shape(t, 400, 20, "\xf0\x9f\x98\x80", 4, g, MAXG, NULL);
        CHECK(n == 1 && same_path(gp_text_font_path(t, g[0].font), E), "emoji come from the registered color font");
        if (n == 1) {
            gp_glyph_entry e = gp_text_glyph(t, g[0].font, g[0].glyph, 32.f);
            CHECK(e.color && e.w > 0, "and rasterize as color glyphs");
        }
        CHECK(same_path(font_for(t, "A", 400), R), "the color font is not used for letters");
        gp_text_system_free(t);
    }

    /* fonts from memory: the data is copied, so the caller's buffer can go straight away */
    size_t len;
    unsigned char *bytes = slurp(R, &len);
    CHECK(bytes != NULL, "reading %s", R);
    if (bytes) {
        t = fresh(false);
        gp_text_add_font_memory(t, bytes, len, 0, 0);
        memset(bytes, 0xAB, len); /* scribble, then free: nothing may still point at it */
        free(bytes);
        const char *mp = font_for(t, "A", 400);
        CHECK(mp && strncmp(mp, "memory:", 7) == 0, "a memory font has a memory: name (%s)", mp ? mp : "none");
        gp_glyph_pos from_mem[MAXG], from_file[MAXG];
        int nm = gp_text_shape(t, 400, 20, "Hello, world", 12, from_mem, MAXG, NULL);
        gp_text_system *tf = fresh(false);
        gp_text_add_font_file(tf, R, 0, 0);
        int nf = gp_text_shape(tf, 400, 20, "Hello, world", 12, from_file, MAXG, NULL);
        CHECK(nm == nf && nm > 0, "same glyph count from memory and file (%d vs %d)", nm, nf);
        for (int i = 0; i < nm && i < nf; i++)
            CHECK(from_mem[i].glyph == from_file[i].glyph && fabsf(from_mem[i].advance - from_file[i].advance) < 0.01f,
                  "glyph %d differs between memory and file", i);
        gp_glyph_entry e = gp_text_glyph(t, from_mem[0].font, from_mem[0].glyph, 24.f);
        CHECK(e.w > 0, "a glyph rasterizes from the copied data");
        gp_text_system_free(tf);
        gp_text_system_free(t);
    }

    /* rejected input changes nothing */
    t = fresh(false);
    gp_text_add_font_file(t, R, 0, 0);
    int before = gp_text_font_count(t);
    unsigned char junk[256];
    memset(junk, 0x5a, sizeof junk);
    CHECK(!gp_text_add_font_file(t, __FILE__, 0, 0), "a source file is not a font");
    CHECK(!gp_text_add_font_file(t, "/nonexistent/font.ttf", 0, 0), "a missing file is rejected");
    CHECK(!gp_text_add_font_file(t, NULL, 0, 0), "NULL path");
    CHECK(!gp_text_add_font_memory(t, junk, sizeof junk, 0, 0), "junk bytes are not a font");
    CHECK(!gp_text_add_font_memory(t, NULL, 100, 0, 0) && !gp_text_add_font_memory(t, junk, 0, 0, 0), "NULL / empty memory");
    CHECK(!gp_text_add_font_file(t, R, 50, 0) && !gp_text_add_font_file(t, R, 1000, 0), "weights outside 100..900");
    CHECK(!gp_text_add_font_file(t, R, 0, -1), "negative face index");
    CHECK(!gp_text_add_font_file(t, R, 0, 999), "a face index the file does not have");
    CHECK(gp_text_font_count(t) == before, "rejections load nothing (%d fonts, was %d)", gp_text_font_count(t), before);
    CHECK(same_path(font_for(t, "A", 400), R), "and leave the registered font working");
    gp_text_system_free(t);
}

/* ---------------------------------------------------------------- synthetic bold */

/* Total coverage of a glyph in text system `t`'s alpha atlas (a sum of the 0..255 values). */
static long ink(gp_text_system *t, int font, uint32_t glyph, float px, int *width) {
    gp_glyph_entry e = gp_text_glyph(t, font, glyph, px);
    const uint8_t *a = gp_text_atlas(t);
    long sum = 0;
    for (int y = 0; y < (int)e.h; y++)
        for (int x = 0; x < (int)e.w; x++) sum += a[((size_t)((int)e.v + y)) * GP_ATLAS_SIZE + (size_t)((int)e.u + x)];
    *width = (int)e.w;
    return sum;
}

/* A bold request that lands on a font without a bold face is thickened: wider advances and more ink.
 * A real bold face, regular text, color glyphs and zero-width marks are left alone. */
static void test_synthetic_bold(const char *R, const char *B, const char *E) {
    const float size = 32.f;
    gp_glyph_pos reg[MAXG], bold[MAXG];
    float wr, wb;

    gp_text_system *t = fresh(false);
    gp_text_add_font_file(t, R, 0, 0);
    int nr = gp_text_shape(t, 400, size, "Hello", 5, reg, MAXG, &wr);
    int nb = gp_text_shape(t, 700, size, "Hello", 5, bold, MAXG, &wb);
    CHECK(nr == 5 && nb == 5, "five glyphs either way (%d, %d)", nr, nb);
    for (int i = 0; i < nr && i < nb; i++)
        CHECK(fabsf(bold[i].advance - (reg[i].advance + size / 24.f)) < 0.01f, "glyph %d: bold advance %.3f, regular %.3f + size/24", i,
              bold[i].advance, reg[i].advance);
    CHECK(fabsf(wb - (wr + 5 * size / 24.f)) < 0.05f, "the string is wider by exactly five strengths (%.2f vs %.2f)", wb, wr);
    float sum = 0;
    for (int i = 0; i < nb; i++) sum += bold[i].advance;
    CHECK(fabsf(sum - wb) < 0.05f, "the measured width is the sum of the advances drawn (%.2f vs %.2f)", wb, sum);
    CHECK(bold[0].font != reg[0].font && same_path(gp_text_font_path(t, bold[0].font), gp_text_font_path(t, reg[0].font)),
          "bold is a variant of the same file");

    int wreg, wbold;
    long inkr = ink(t, reg[0].font, reg[0].glyph, size, &wreg);
    long inkb = ink(t, bold[0].font, bold[0].glyph, size, &wbold);
    CHECK(inkb > inkr * 115 / 100, "thickened H has more ink (%ld vs %ld)", inkb, inkr);
    CHECK(wbold >= wreg, "and is not narrower (%d vs %d px)", wbold, wreg);
    int wreg2;
    CHECK(ink(t, reg[0].font, reg[0].glyph, size, &wreg2) == inkr, "regular glyphs are unaffected once the bold ones exist");
    int nr2 = gp_text_shape(t, 400, size, "Hello", 5, reg, MAXG, &wr);
    CHECK(nr2 == 5 && fabsf(wr - (wb - 5 * size / 24.f)) < 0.05f, "regular text keeps its width after bold was shaped");

    /* a combining mark has no advance, and must stay that way */
    gp_glyph_pos acc_r[MAXG], acc_b[MAXG];
    int ar = gp_text_shape(t, 400, size, "x\xcc\x81", 3, acc_r, MAXG, NULL);
    int ab = gp_text_shape(t, 700, size, "x\xcc\x81", 3, acc_b, MAXG, NULL);
    if (ar == 2 && ab == 2) {
        CHECK(acc_r[1].advance == 0.f && acc_b[1].advance == 0.f, "a combining mark stays zero-width (%.3f)", acc_b[1].advance);
        CHECK(fabsf(acc_b[0].advance - (acc_r[0].advance + size / 24.f)) < 0.01f, "while its base letter widens");
    } else {
        skips++;
        printf("SKIP: the font composes x + acute into %d glyph(s), so zero-width marks can't be shown\n", ar);
    }
    gp_text_system_free(t);

    /* the copy borrows the base's bytes: a memory font must survive both being freed once */
    size_t len;
    unsigned char *bytes = slurp(R, &len);
    if (bytes) {
        t = fresh(false);
        gp_text_add_font_memory(t, bytes, len, 0, 0);
        free(bytes);
        gp_glyph_pos mem[MAXG];
        float wm;
        int nm = gp_text_shape(t, 700, size, "Hello", 5, mem, MAXG, &wm);
        CHECK(nm == 5 && fabsf(wm - wb) < 0.05f, "a memory font thickens the same way (%.2f vs %.2f)", wm, wb);
        int dummy;
        CHECK(ink(t, mem[0].font, mem[0].glyph, size, &dummy) == inkb, "and rasterizes the same ink");
        gp_text_system_free(t);
    }

    /* a real bold face is used as is: no copy, no extra width */
    if (B && !same_path(R, B)) {
        gp_glyph_pos rb[MAXG], rb4[MAXG];
        float w7, w4;
        t = fresh(false);
        gp_text_add_font_file(t, B, 0, 0);
        gp_text_shape(t, 700, size, "Hello", 5, rb, MAXG, &w7);
        gp_text_shape(t, 400, size, "Hello", 5, rb4, MAXG, &w4);
        CHECK(fabsf(w7 - w4) < 0.01f, "a real bold face is not widened (%.2f vs %.2f)", w7, w4);
        CHECK(gp_text_font_count(t) == 1, "and gets no thickened copy (%d fonts)", gp_text_font_count(t));
        gp_text_system_free(t);
        /* with both faces registered, bold picks the real one */
        t = fresh(false);
        gp_text_add_font_file(t, R, 0, 0);
        gp_text_add_font_file(t, B, 0, 0);
        gp_text_shape(t, 700, size, "Hello", 5, rb, MAXG, &w7);
        CHECK(fabsf(w7 - w4) < 0.01f && gp_text_font_count(t) == 2, "regular + bold registered: the real bold draws (%.2f vs %.2f, %d fonts)", w7, w4,
              gp_text_font_count(t));
        gp_text_system_free(t);
    }

    /* color glyphs are never thickened */
    if (E) {
        gp_glyph_pos e4[MAXG], e7[MAXG];
        float w4, w7;
        t = fresh(false);
        gp_text_add_font_file(t, R, 0, 0);
        gp_text_add_font_file(t, E, 0, 0);
        int n4 = gp_text_shape(t, 400, size, "\xf0\x9f\x98\x80", 4, e4, MAXG, &w4);
        int n7 = gp_text_shape(t, 700, size, "\xf0\x9f\x98\x80", 4, e7, MAXG, &w7);
        CHECK(n4 == 1 && n7 == 1 && fabsf(w4 - w7) < 0.01f && e4[0].font == e7[0].font, "an emoji is the same glyph in bold (%.2f vs %.2f)", w4, w7);
        gp_text_system_free(t);
    }
}

/* ---------------------------------------------------------------- scan cache */

#ifndef GPUIC_HAVE_FONTCONFIG
/* One scan in a fresh text system: which font each probe picks (as one string), and what the scan did. */
static void scan_probe(gp_font_scan_stats *st, char *sig, size_t cap) {
    static const struct { const char *text; int weight; } probes[] = {
        { "A", 400 }, { "A", 700 }, { "\xe6\x97\xa5", 400 }, { "\xf0\x9f\x98\x80", 400 }, { "\xd7\x90", 400 }, { "\xd8\xa8", 700 },
    };
    gp_text_system *t = gp_text_system_new();
    sig[0] = 0;
    for (size_t i = 0; i < sizeof probes / sizeof *probes; i++) {
        const char *p = font_for(t, probes[i].text, probes[i].weight);
        snprintf(sig + strlen(sig), cap - strlen(sig), "%s|", p ? p : "-");
    }
    *st = gp_text_scan_stats(t);
    gp_text_system_free(t);
}

static void copy_file(const char *from, const char *to) {
    size_t len;
    unsigned char *b = slurp(from, &len);
    FILE *f = b ? fopen(to, "wb") : NULL;
    if (f) { fwrite(b, 1, len, f); fclose(f); }
    free(b);
}

static int cache_files(const char *dir) {
    int n = 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    for (struct dirent *e; (e = readdir(d));)
        if (strncmp(e->d_name, "fonts-", 6) == 0 && strstr(e->d_name, ".txt") && !strstr(e->d_name, ".tmp")) n++;
    closedir(d);
    return n;
}

static void first_cache_file(const char *dir, char *out, size_t cap) {
    out[0] = 0;
    DIR *d = opendir(dir);
    if (!d) return;
    for (struct dirent *e; (e = readdir(d));)
        if (strncmp(e->d_name, "fonts-", 6) == 0 && strstr(e->d_name, ".txt") && !strstr(e->d_name, ".tmp")) {
            if (snprintf(out, cap, "%s/%s", dir, e->d_name) >= (int)cap) out[0] = 0; /* too long: treated as not found */
            break;
        }
    closedir(d);
}

/* Rewrites a cache file's body with `edit` applied, then seals it with a correct checksum of its own,
 * so only the content (not the integrity check) can reject it. The checksum is FNV-1a 64 over every
 * byte before the final "end" line, as the format documents. */
static unsigned char *forge_cache(const unsigned char *good, size_t len, int mode, size_t *out_len) {
    const char *end = (const char *)good + len - 1;
    while (end > (const char *)good && end[-1] != '\n') end--;
    size_t body_len = (size_t)(end - (const char *)good);
    unsigned long long nfiles = 0, sum_unused;
    sscanf(end, "end\t%llu\t%llx", &nfiles, &sum_unused);
    char *body = malloc(body_len + 64);
    memcpy(body, good, body_len);
    body[body_len] = 0;
    if (mode == 0) { /* a format version this build does not know */
        char *tab = strchr(body, '\t');
        if (tab) tab[1] = '9';
    } else if (mode == 1) { /* a face whose weight is out of range */
        char *f = strstr(body, "\nf\t");
        char *w = f ? strchr(f + 3, '\t') : NULL; /* after the index */
        char *w_end = w ? strchr(w + 1, '\t') : NULL;
        if (w_end) {
            memmove(w + 5, w_end, strlen(w_end) + 1);
            memcpy(w + 1, "5000", 4);
            body_len = strlen(body);
        }
    }
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < body_len; i++) { h ^= (unsigned char)body[i]; h *= 1099511628211ull; }
    char tail[64];
    int n = snprintf(tail, sizeof tail, "end\t%llu\t%016llx\n", nfiles, (unsigned long long)h);
    unsigned char *out = malloc(body_len + (size_t)n);
    memcpy(out, body, body_len);
    memcpy(out + body_len, tail, (size_t)n);
    free(body);
    *out_len = body_len + (size_t)n;
    return out;
}

/* The scanner remembers what it learned from FreeType in a cache file and trusts it only for files
 * whose size and modification time are unchanged. Every stage compares the choices made from the
 * cache with those of a scan that has no cache, so "same as reality" is what is tested. */
static void test_scan_cache(const char *R, const char *C) {
    char root[512];
    snprintf(root, sizeof root, "%s/gpuic-scan-test-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(root)) { CHECK(false, "cannot create a temporary directory"); return; }
    char fonts[600], cache[600], fa[700], fb[700], fc[700];
    snprintf(fonts, sizeof fonts, "%s/fonts", root);
    snprintf(cache, sizeof cache, "%s/cache", root);
    mkdir(fonts, 0700);
    snprintf(fa, sizeof fa, "%s/a.ttf", fonts);   /* sorts before /usr/..., so a copy of a system font wins ties */
    snprintf(fb, sizeof fb, "%s/b.ttf", fonts);
    snprintf(fc, sizeof fc, "%s/c.ttf", fonts);
    copy_file(R, fa);
    copy_file(R, fb);
    setenv("GPUIC_FONT_PATH", fonts, 1);

    gp_font_scan_stats st, un;
    char sig[4096], ref[4096];
    /* what scanning without any cache finds right now: the reference for every stage */
#define REFERENCE() do { setenv("GPUIC_FONT_CACHE", "0", 1); scan_probe(&un, ref, sizeof ref); setenv("GPUIC_FONT_CACHE", cache, 1); } while (0)

    REFERENCE();
    CHECK(un.files > 0 && un.opened == un.files && un.from_cache == 0, "caching off: every file is opened (%d of %d)", un.opened, un.files);
    CHECK(cache_files(cache) == 0, "caching off writes nothing");
    CHECK(strstr(ref, fa) != NULL, "setup: the copy in GPUIC_FONT_PATH is what regular text picks (%s)", ref);

    /* cold: nothing cached yet */
    scan_probe(&st, sig, sizeof sig);
    CHECK(st.files == un.files && st.opened == st.files && st.from_cache == 0 && !st.cache_loaded && st.cache_written,
          "cold scan: opened %d of %d, from cache %d, loaded %d, written %d", st.opened, st.files, st.from_cache, st.cache_loaded, st.cache_written);
    CHECK(strcmp(sig, ref) == 0, "cold scan makes the same choices as without a cache");
    CHECK(cache_files(cache) == 1, "one cache file for this set of directories (%d)", cache_files(cache));
    double cold_ms = st.ms;

    /* warm: everything from the cache */
    scan_probe(&st, sig, sizeof sig);
    CHECK(st.opened == 0 && st.from_cache == st.files && st.files == un.files && st.cache_loaded && !st.cache_written,
          "warm scan opens nothing: opened %d, from cache %d of %d, written %d", st.opened, st.from_cache, st.files, st.cache_written);
    CHECK(strcmp(sig, ref) == 0, "warm scan makes the same choices as without a cache");
    CHECK(st.faces == un.faces, "and knows as many faces (%d vs %d)", st.faces, un.faces);
    printf("  scan cache: %d files, cold %.1f ms, warm %.1f ms\n", st.files, cold_ms, st.ms);
    if (cold_ms > 100) CHECK(st.ms < cold_ms / 3, "warm scan is much faster (%.1f vs %.1f ms)", st.ms, cold_ms);

    /* a changed file (different size) is read again, and only that one */
    copy_file(C, fa);
    REFERENCE();
    scan_probe(&st, sig, sizeof sig);
    CHECK(st.opened == 1 && st.cache_written, "a file with a new size is reopened, nothing else (opened %d)", st.opened);
    CHECK(strcmp(sig, ref) == 0 && strcmp(sig, "") != 0, "and the choices follow its new contents");
    scan_probe(&st, sig, sizeof sig);
    CHECK(st.opened == 0 && !st.cache_written && strcmp(sig, ref) == 0, "the update was remembered (opened %d)", st.opened);

    /* same size, only the modification time moved */
    struct utimbuf tb = { 1000000000, 1000000000 };
    CHECK(utime(fa, &tb) == 0, "setup: utime");
    scan_probe(&st, sig, sizeof sig);
    CHECK(st.opened == 1 && st.cache_written, "a new modification time alone invalidates a file (opened %d)", st.opened);
    scan_probe(&st, sig, sizeof sig);
    CHECK(st.opened == 0, "setup: settled again (opened %d)", st.opened);

    /* same modification time, only the size changed (the other half of the key) */
    copy_file(R, fa);
    CHECK(utime(fa, &tb) == 0, "setup: utime");
    REFERENCE();
    scan_probe(&st, sig, sizeof sig);
    CHECK(st.opened == 1 && st.cache_written && strcmp(sig, ref) == 0, "a new size alone invalidates a file (opened %d)", st.opened);
    scan_probe(&st, sig, sizeof sig);
    CHECK(st.opened == 0, "setup: settled again (opened %d)", st.opened);

    /* a new file appears, another vanishes */
    copy_file(R, fc);
    remove(fb);
    REFERENCE();
    scan_probe(&st, sig, sizeof sig);
    CHECK(st.opened == 1 && st.files == un.files && st.cache_written, "new file opened, deleted one dropped (opened %d, files %d vs %d)", st.opened,
          st.files, un.files);
    CHECK(strcmp(sig, ref) == 0, "and the choices match a fresh scan");
    scan_probe(&st, sig, sizeof sig);
    CHECK(st.opened == 0 && !st.cache_written, "then it is stable (opened %d, written %d)", st.opened, st.cache_written);
    remove(fc);
    scan_probe(&st, sig, sizeof sig);
    CHECK(st.opened == 0 && st.cache_written && st.files == un.files - 1, "removing a file only rewrites the cache (opened %d)", st.opened);

    /* damaged caches are ignored as a whole, then repaired */
    char cf[800];
    first_cache_file(cache, cf, sizeof cf);
    CHECK(cf[0] != 0, "setup: found the cache file");
    size_t len = 0;
    unsigned char *good = slurp(cf, &len);
    REFERENCE();
    static const char *const damage[] = { "truncated", "one flipped byte", "garbage", "empty", "edited version (bad checksum)",
                                          "unknown version (valid checksum)", "out-of-range weight (valid checksum)" };
    for (int d = 0; d < 7 && good; d++) {
        FILE *f = fopen(cf, "wb");
        if (d == 0) fwrite(good, 1, len / 2, f);
        if (d == 1) {
            unsigned char *bad = malloc(len);
            memcpy(bad, good, len);
            bad[len / 2] ^= 0x01;
            fwrite(bad, 1, len, f);
            free(bad);
        }
        if (d == 2) fputs("this is not a cache\n\x01\x02\x03", f);
        if (d == 4) {
            char *v = malloc(len + 1);
            memcpy(v, good, len);
            v[len] = 0;
            char *tab = strchr(v, '\t');
            if (tab) tab[1] = '9'; /* the version number */
            fwrite(v, 1, len, f);
            free(v);
        }
        if (d >= 5) {
            size_t flen;
            unsigned char *forged = forge_cache(good, len, d - 5, &flen);
            fwrite(forged, 1, flen, f);
            free(forged);
        }
        fclose(f);
        scan_probe(&st, sig, sizeof sig);
        CHECK(!st.cache_loaded && st.opened == st.files && st.cache_written, "%s cache: ignored and rebuilt (loaded %d, opened %d of %d)", damage[d],
              st.cache_loaded, st.opened, st.files);
        CHECK(strcmp(sig, ref) == 0, "%s cache: same choices as a fresh scan", damage[d]);
        scan_probe(&st, sig, sizeof sig);
        CHECK(st.cache_loaded && st.opened == 0, "%s cache: repaired for the next run", damage[d]);
    }
    if (good) { /* control: the forging helper with no defect must give a cache that loads, or the two stages above prove nothing */
        size_t flen;
        unsigned char *forged = forge_cache(good, len, 2, &flen);
        FILE *f = fopen(cf, "wb");
        fwrite(forged, 1, flen, f);
        fclose(f);
        free(forged);
        scan_probe(&st, sig, sizeof sig);
        CHECK(st.cache_loaded && st.opened == 0 && strcmp(sig, ref) == 0, "control: a re-sealed, undamaged cache loads (loaded %d, opened %d)",
              st.cache_loaded, st.opened);
    }
    free(good);

    /* a cache that cannot be written is not an error */
    char blocker[700], below[800];
    snprintf(blocker, sizeof blocker, "%s/blocker", root);
    copy_file(R, blocker);
    snprintf(below, sizeof below, "%s/sub", blocker);
    setenv("GPUIC_FONT_CACHE", below, 1);
    scan_probe(&st, sig, sizeof sig);
    CHECK(st.opened == st.files && !st.cache_written && strcmp(sig, ref) == 0, "an unwritable cache location: scanning still works (written %d)", st.cache_written);
    setenv("GPUIC_FONT_CACHE", cache, 1);

    /* different directories get their own cache, and keep it */
    char other[700];
    snprintf(other, sizeof other, "%s/other", root);
    mkdir(other, 0700);
    setenv("GPUIC_FONT_PATH", other, 1);
    scan_probe(&st, sig, sizeof sig);
    CHECK(st.opened > 0 && st.cache_written && cache_files(cache) == 2, "another set of directories starts its own cache (%d files)", cache_files(cache));
    setenv("GPUIC_FONT_PATH", fonts, 1);
    scan_probe(&st, sig, sizeof sig);
    CHECK(st.opened == 0 && st.cache_loaded, "and going back finds the first one intact (opened %d)", st.opened);

    unsetenv("GPUIC_FONT_PATH");
    unsetenv("GPUIC_FONT_CACHE");
    /* best-effort clean up */
    DIR *dd = opendir(cache);
    for (struct dirent *e; dd && (e = readdir(dd));) {
        char p[1000];
        snprintf(p, sizeof p, "%s/%s", cache, e->d_name);
        remove(p);
    }
    if (dd) closedir(dd);
    remove(fa); remove(fb); remove(fc); remove(blocker);
    rmdir(fonts); rmdir(cache); rmdir(other); rmdir(root);
#undef REFERENCE
}
#endif

/* ---------------------------------------------------------------- families and weights */

/* where the Noto fonts are: Arch's layout, or Debian's and Ubuntu's */
static const char *noto_dir(void) {
    static const char *dir;
    if (!dir) dir = access("/usr/share/fonts/noto/NotoSans-Regular.ttf", R_OK) == 0 ? "/usr/share/fonts/noto/" : "/usr/share/fonts/truetype/noto/";
    return dir;
}
/* the path of a Noto file (a few at once may be in use: a ring of buffers) */
static const char *noto(const char *file) {
    static char ring[8][512];
    static int at;
    char *p = ring[at++ % 8];
    snprintf(p, sizeof ring[0], "%s%s", noto_dir(), file);
    return p;
}
static bool exists(const char *path) { return access(path, R_OK) == 0; }

static int sel(gp_text_system *t, const char *family, int weight) { return GP_FONT_SEL(gp_text_family(t, family), weight); }

static const char *base_name(const char *path) {
    const char *slash = path ? strrchr(path, '/') : NULL;
    return slash ? slash + 1 : path ? path : "(none)";
}

/* Fonts registered by file, with the system out of it: which face a family and weight pick is ours to decide. */
static void test_weights_and_families(void) {
    static const struct { const char *file; int weight; } sans[] = {
        { "NotoSans-Thin.ttf", 100 }, { "NotoSans-Light.ttf", 300 }, { "NotoSans-Regular.ttf", 400 },
        { "NotoSans-Medium.ttf", 500 }, { "NotoSans-Bold.ttf", 700 }, { "NotoSans-Black.ttf", 900 },
    };
    bool have = exists(noto("NotoSerif-Regular.ttf")) && exists(noto("NotoSansMono-Regular.ttf"));
    for (size_t i = 0; i < sizeof sans / sizeof *sans; i++) {
        char p[256];
        snprintf(p, sizeof p, "%s%s", noto_dir(), sans[i].file);
        have &= exists(p);
    }
    if (!have) {
        skips++;
        printf("SKIP: the Noto Sans weights, Noto Serif and Noto Sans Mono are not in %s\n", noto_dir());
        return;
    }
    char path[256];

    /* CSS weight matching over a full set (100 300 400 500 700 900) */
    gp_text_system *t = fresh(false);
    for (size_t i = 0; i < sizeof sans / sizeof *sans; i++) {
        snprintf(path, sizeof path, "%s%s", noto_dir(), sans[i].file);
        CHECK(gp_text_add_font_file(t, path, 0, 0), "register %s", path);
    }
    static const struct { int want; const char *file; } full[] = {
        { 1, "NotoSans-Thin.ttf" },  { 100, "NotoSans-Thin.ttf" }, { 200, "NotoSans-Thin.ttf" },
        { 300, "NotoSans-Light.ttf" }, { 350, "NotoSans-Light.ttf" }, { 399, "NotoSans-Light.ttf" },
        { 400, "NotoSans-Regular.ttf" }, { 450, "NotoSans-Medium.ttf" }, { 500, "NotoSans-Medium.ttf" },
        { 501, "NotoSans-Bold.ttf" }, { 600, "NotoSans-Bold.ttf" }, { 700, "NotoSans-Bold.ttf" },
        { 800, "NotoSans-Black.ttf" }, { 900, "NotoSans-Black.ttf" }, { 1000, "NotoSans-Black.ttf" },
    };
    for (size_t i = 0; i < sizeof full / sizeof *full; i++) {
        const char *got = base_name(font_for(t, "A", full[i].want));
        CHECK(strcmp(got, full[i].file) == 0, "weight %d of the full set draws %s, wanted %s", full[i].want, got, full[i].file);
    }
    CHECK(strcmp(base_name(font_for(t, "A", 0)), "NotoSans-Regular.ttf") == 0, "weight 0 is 400");
    CHECK(strcmp(base_name(font_for(t, "A", 5000)), "NotoSans-Black.ttf") == 0, "out-of-range weights are clamped");
    /* all six are one family, whatever the legacy names of the files say ("Noto Sans Light") */
    for (int w = 100; w <= 900; w += 200) {
        int fam = sel(t, "noto sans", w);
        const char *got = base_name(font_for(t, "A", fam));
        CHECK(got && strncmp(got, "NotoSans-", 9) == 0, "family \"noto sans\" (case-insensitive) at %d draws a Noto Sans face: %s", w, got);
    }
    int widths_differ = 0;
    float w3, w9;
    gp_text_shape(t, sel(t, NULL, 300), 20, "Weight", 6, NULL, 0, &w3);
    gp_text_shape(t, sel(t, NULL, 900), 20, "Weight", 6, NULL, 0, &w9);
    widths_differ = w9 > w3 * 1.05f;
    CHECK(widths_differ, "a black string is wider than a light one (%.1f vs %.1f)", w9, w3);
    gp_text_system_free(t);

    /* italic faces are not chosen: they are listed first and match the weight exactly, and still lose */
    if (exists(noto("NotoSans-Italic.ttf")) && exists(noto("NotoSans-BoldItalic.ttf"))) {
        t = fresh(false);
        const char *order[] = { "NotoSans-Italic.ttf", "NotoSans-BoldItalic.ttf", "NotoSans-Regular.ttf", "NotoSans-Bold.ttf" };
        for (int i = 0; i < 4; i++) { snprintf(path, sizeof path, "%s%s", noto_dir(), order[i]); gp_text_add_font_file(t, path, 0, 0); }
        CHECK(strcmp(base_name(font_for(t, "A", 400)), "NotoSans-Regular.ttf") == 0, "roman beats italic at 400: %s", base_name(font_for(t, "A", 400)));
        CHECK(strcmp(base_name(font_for(t, "A", 700)), "NotoSans-Bold.ttf") == 0, "roman beats italic at 700: %s", base_name(font_for(t, "A", 700)));
        gp_text_system_free(t);
        t = fresh(false);   /* only italic ones: better than nothing */
        for (int i = 0; i < 2; i++) { snprintf(path, sizeof path, "%s%s", noto_dir(), order[i]); gp_text_add_font_file(t, path, 0, 0); }
        CHECK(strcmp(base_name(font_for(t, "A", 400)), "NotoSans-Italic.ttf") == 0, "an italic face still draws when it is all there is");
        gp_text_system_free(t);
    }

    /* a set with holes: 300 400 700 */
    t = fresh(false);
    const char *hole[] = { "NotoSans-Light.ttf", "NotoSans-Regular.ttf", "NotoSans-Bold.ttf" };
    for (int i = 0; i < 3; i++) { snprintf(path, sizeof path, "%s%s", noto_dir(), hole[i]); gp_text_add_font_file(t, path, 0, 0); }
    static const struct { int want; const char *file; } holes[] = {
        { 100, "NotoSans-Light.ttf" },        /* nothing lighter: the nearest above */
        { 450, "NotoSans-Regular.ttf" },      /* 400..500 has only 400: look at 500 first, then below */
        { 500, "NotoSans-Regular.ttf" },
        { 550, "NotoSans-Bold.ttf" },         /* above 500: heavier first */
        { 600, "NotoSans-Bold.ttf" },
        { 800, "NotoSans-Bold.ttf" },
        { 350, "NotoSans-Light.ttf" },        /* below 400: lighter first */
    };
    for (size_t i = 0; i < sizeof holes / sizeof *holes; i++) {
        const char *got = base_name(font_for(t, "A", holes[i].want));
        CHECK(strcmp(got, holes[i].file) == 0, "weight %d of {300,400,700} draws %s, wanted %s", holes[i].want, got, holes[i].file);
    }
    gp_text_system_free(t);

    /* a family with one face: lighter weights use it as is, heavy ones are thickened (600 and up) */
    t = fresh(false);
    snprintf(path, sizeof path, "%s", noto("NotoSans-Regular.ttf"));
    gp_text_add_font_file(t, path, 0, 0);
    float w400, w500, w600, w900, w100;
    gp_text_shape(t, 400, 20, "Weight", 6, NULL, 0, &w400);
    gp_text_shape(t, 500, 20, "Weight", 6, NULL, 0, &w500);
    gp_text_shape(t, 600, 20, "Weight", 6, NULL, 0, &w600);
    gp_text_shape(t, 900, 20, "Weight", 6, NULL, 0, &w900);
    gp_text_shape(t, 100, 20, "Weight", 6, NULL, 0, &w100);
    CHECK(w500 == w400 && w100 == w400, "a lone regular face is not thickened for 100 or 500 (%.2f %.2f %.2f)", w100, w400, w500);
    CHECK(w600 > w400 && w900 > w400, "but is for 600 and 900 (%.2f %.2f vs %.2f)", w600, w900, w400);
    gp_text_system_free(t);

    /* families: registered under the name the font gives itself, or under one the app picks */
    char serif[256], mono[256], regular[256], bold[256];
    snprintf(serif, sizeof serif, "%s", noto("NotoSerif-Regular.ttf"));
    snprintf(mono, sizeof mono, "%s", noto("NotoSansMono-Regular.ttf"));
    snprintf(regular, sizeof regular, "%s", noto("NotoSans-Regular.ttf"));
    snprintf(bold, sizeof bold, "%s", noto("NotoSans-Bold.ttf"));

    t = fresh(false);
    gp_text_add_font_file(t, regular, 0, 0);
    gp_text_add_font_file(t, bold, 0, 0);
    gp_text_add_font_file(t, serif, 0, 0);
    gp_text_add_font_file(t, mono, 0, 0);
    CHECK(same_path(font_for(t, "A", sel(t, "Noto Serif", 400)), serif), "family \"Noto Serif\"");
    CHECK(same_path(font_for(t, "A", sel(t, "noto sans mono", 400)), mono), "family \"noto sans mono\"");
    CHECK(same_path(font_for(t, "A", sel(t, "\"Noto Serif\"", 400)), serif), "quoted names");
    CHECK(same_path(font_for(t, "A", sel(t, "  Noto Serif  ,  serif", 400)), serif), "white space around names");
    CHECK(same_path(font_for(t, "A", sel(t, "Noto Sans", 700)), bold), "family \"Noto Sans\" at 700");
    CHECK(same_path(font_for(t, "A", sel(t, "Zzz No Such, Noto Serif", 400)), serif), "an uninstalled family is skipped");
    CHECK(same_path(font_for(t, "A", sel(t, "Noto Serif, Noto Sans Mono", 400)), serif), "the first family of a list wins");
    CHECK(same_path(font_for(t, "A", sel(t, "Noto Sans Mono, Noto Serif", 400)), mono), "...in the order given");
    CHECK(same_path(font_for(t, "A", sel(t, "Zzz No Such", 400)), regular), "a family nobody has draws the default");
    CHECK(same_path(font_for(t, "A", 400), regular), "no family at all is the family of the first registered font");
    CHECK(same_path(font_for(t, "A", sel(t, "monospace", 400)), regular),
          "with no system and no registered monospace font, \"monospace\" draws the default");
    /* the serif face has no bold: bold is made from it, in the serif */
    float ws, wsb;
    gp_text_shape(t, sel(t, "Noto Serif", 400), 20, "Serif", 5, NULL, 0, &ws);
    gp_glyph_pos gs[8];
    gp_text_shape(t, sel(t, "Noto Serif", 700), 20, "Serif", 5, gs, 8, &wsb);
    CHECK(wsb > ws && same_path(gp_text_font_path(t, gs[0].font), serif), "bold serif is the serif face thickened (%.2f vs %.2f)", wsb, ws);
    /* the same text is shaped per family: a cached run of one family is not handed out for another */
    float a1, a2;
    gp_text_shape(t, sel(t, "Noto Serif", 400), 20, "Weight", 6, NULL, 0, &a1);
    gp_text_shape(t, sel(t, "Noto Sans Mono", 400), 20, "Weight", 6, NULL, 0, &a2);
    float a3;
    gp_text_shape(t, sel(t, "Noto Serif", 400), 20, "Weight", 6, NULL, 0, &a3);
    CHECK(a1 != a2 && a1 == a3, "shape cache keeps families apart (%.2f %.2f %.2f)", a1, a2, a3);
    int fid = gp_text_family(t, "Noto Serif");
    CHECK(fid != 0 && fid == gp_text_family(t, "Noto Serif") && gp_text_family(t, "") == 0 && gp_text_family(t, NULL) == 0,
          "family ids are interned");
    {   /* line metrics come from the family's own font */
        const char *dejavu = exists("/usr/share/fonts/TTF/DejaVuSansMono.ttf") ? "/usr/share/fonts/TTF/DejaVuSansMono.ttf"
                           : exists("/usr/share/fonts/dejavu/DejaVuSansMono.ttf") ? "/usr/share/fonts/dejavu/DejaVuSansMono.ttf" : NULL;
        const char *files[3] = { serif, mono, dejavu };   /* the Noto fonts share their line metrics: DejaVu's differ */
        const char *names[3] = { "Noto Serif", "Noto Sans Mono", "DejaVu Sans Mono" };
        FT_Library lib;
        FT_Init_FreeType(&lib);
        if (dejavu) gp_text_add_font_file(t, dejavu, 0, 0);
        for (int k = 0; k < (dejavu ? 3 : 2); k++) {
            FT_Face face;
            float asc, desc;
            gp_text_metrics(t, sel(t, names[k], 400), 20, &asc, &desc);
            if (FT_New_Face(lib, files[k], 0, &face) == 0) {
                float want_asc = (float)face->ascender * 20.f / (float)face->units_per_EM;
                float want_desc = (float)-face->descender * 20.f / (float)face->units_per_EM;
                CHECK(fabsf(asc - want_asc) < 0.01f && fabsf(desc - want_desc) < 0.01f, "%s: metrics %.2f/%.2f, its font says %.2f/%.2f", names[k], asc, desc, want_asc, want_desc);
                FT_Done_Face(face);
            }
        }
        FT_Done_FreeType(lib);
    }
    gp_text_system_free(t);

    /* the app names a font's family itself, and defines the generic ones */
    t = fresh(false);
    gp_text_add_font_file_as(t, serif, "serif", 0, 0);
    gp_text_add_font_file_as(t, mono, "monospace", 0, 0);
    gp_text_add_font_file_as(t, regular, "Brand", 0, 0);
    gp_text_add_font_file_as(t, bold, "Brand", 0, 0);
    CHECK(same_path(font_for(t, "A", sel(t, "serif", 400)), serif) && same_path(font_for(t, "A", sel(t, "monospace", 400)), mono),
          "registered generic families");
    CHECK(same_path(font_for(t, "A", sel(t, "Brand", 700)), bold) && same_path(font_for(t, "A", sel(t, "Brand", 400)), regular), "a family named by the app");
    CHECK(same_path(font_for(t, "A", 400), serif), "text without a family: the first registered font's family (here the serif)");
    gp_text_add_font_file_as(t, regular, "sans-serif", 0, 0);
    CHECK(same_path(font_for(t, "A", 400), regular), "a registered \"sans-serif\" is the default (re-registering renames the file's family)");
    CHECK(same_path(font_for(t, "A", sel(t, "sans-serif, serif", 400)), regular) && same_path(font_for(t, "A", sel(t, "system-ui", 400)), regular),
          "system-ui is sans-serif");
    gp_text_system_free(t);

    /* a character no font of the family has comes from another registered font, the one nearest in weight */
    if (exists(noto("NotoSerifHebrew-Light.ttf")) && exists(noto("NotoSerifHebrew-Bold.ttf"))) {
        t = fresh(false);
        gp_text_add_font_file(t, regular, 0, 0);
        snprintf(path, sizeof path, "%s", noto("NotoSerifHebrew-Light.ttf"));
        gp_text_add_font_file(t, path, 0, 0);
        snprintf(path, sizeof path, "%s", noto("NotoSerifHebrew-Bold.ttf"));
        gp_text_add_font_file(t, path, 0, 0);
        CHECK(strcmp(base_name(font_for(t, "\xd7\x90", 700)), "NotoSerifHebrew-Bold.ttf") == 0, "Hebrew at 700 comes from the bold registered font: %s", base_name(font_for(t, "\xd7\x90", 700)));
        CHECK(strcmp(base_name(font_for(t, "\xd7\x90", 300)), "NotoSerifHebrew-Light.ttf") == 0, "Hebrew at 300 comes from the light one: %s", base_name(font_for(t, "\xd7\x90", 300)));
        gp_text_system_free(t);
    }

    /* a character the first family of the list lacks is drawn by the next */
    {
        /* ≠ is not in every Noto face; find one the mono face lacks and the serif has */
        uint32_t cp = 0;
        for (uint32_t c = 0x100; c < 0x2200 && !cp; c++)
            if (has_glyph_in(serif, c) && !has_glyph_in(mono, c) && !has_glyph_in(regular, c)) cp = c;
        if (cp) {
            t = fresh(false);
            gp_text_add_font_file(t, regular, 0, 0);
            gp_text_add_font_file(t, mono, 0, 0);
            gp_text_add_font_file(t, serif, 0, 0);
            char s[8] = { 0 };
            s[0] = (char)(0xe0 | (cp >> 12)); s[1] = (char)(0x80 | ((cp >> 6) & 63)); s[2] = (char)(0x80 | (cp & 63));
            if (cp < 0x800) { s[0] = (char)(0xc0 | (cp >> 6)); s[1] = (char)(0x80 | (cp & 63)); s[2] = 0; }
            CHECK(same_path(font_for(t, s, sel(t, "Noto Sans Mono", 400)), serif),
                  "U+%04X: the monospace family lacks it, another registered font has it", cp);
            CHECK(same_path(font_for(t, "A", sel(t, "Noto Sans Mono", 400)), mono), "while Latin stays in the monospace");
            gp_text_system_free(t);
        }
    }
}

/* How far right the top of a glyph's ink is from its bottom, in px: the centroid of the top third of its rows minus
 * the one of the bottom third. About 0 for an upright H, 0.25 * 2/3 of its height for a sheared one. */
static float slant_of(gp_text_system *t, int font, uint32_t glyph, float px) {
    gp_glyph_entry e = gp_text_glyph(t, font, glyph, px);
    const uint8_t *a = gp_text_atlas(t);
    int h = (int)e.h, third = h / 3;
    double top = 0, top_w = 0, bot = 0, bot_w = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < (int)e.w; x++) {
            double v = a[((size_t)((int)e.v + y)) * GP_ATLAS_SIZE + (size_t)((int)e.u + x)];
            if (y < third) { top += v * x; top_w += v; }
            if (y >= h - third) { bot += v * x; bot_w += v; }
        }
    return top_w > 0 && bot_w > 0 ? (float)(top / top_w - bot / bot_w) : 0.f;
}

#define ITALIC GP_FONT_ITALIC

static void test_italic(void) {
    static const char *const files[] = { "NotoSans-Regular.ttf", "NotoSans-Italic.ttf", "NotoSans-Light.ttf", "NotoSans-LightItalic.ttf",
                                         "NotoSans-Bold.ttf", "NotoSans-BoldItalic.ttf", "NotoSansMono-Regular.ttf", "NotoSerif-Italic.ttf",
                                         "NotoSerif-Regular.ttf" };
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        char p[256];
        snprintf(p, sizeof p, "%s%s", noto_dir(), files[i]);
        if (!exists(p)) { skips++; printf("SKIP: italic tests need %s\n", p); return; }
    }
    char path[256];
    gp_text_system *t = fresh(false);
    for (int i = 0; i < 7; i++) { snprintf(path, sizeof path, "%s%s", noto_dir(), files[i]); gp_text_add_font_file(t, path, 0, 0); }
    static const struct { int weight; const char *file; } real[] = {
        { 400, "NotoSans-Italic.ttf" }, { 300, "NotoSans-LightItalic.ttf" }, { 700, "NotoSans-BoldItalic.ttf" },
        { 500, "NotoSans-Italic.ttf" }, { 900, "NotoSans-BoldItalic.ttf" },
    };
    int nfonts = gp_text_font_count(t);
    for (size_t i = 0; i < sizeof real / sizeof *real; i++) {
        const char *got = base_name(font_for(t, "A", real[i].weight | ITALIC));
        CHECK(strcmp(got, real[i].file) == 0, "italic %d draws %s, wanted %s", real[i].weight, got, real[i].file);
    }
    CHECK(gp_text_font_count(t) == nfonts, "real italic faces are not slanted again (%d fonts, was %d)", gp_text_font_count(t), nfonts);
    CHECK(strcmp(base_name(font_for(t, "A", 400)), "NotoSans-Regular.ttf") == 0 && strcmp(base_name(font_for(t, "A", 300)), "NotoSans-Light.ttf") == 0,
          "upright text still draws the upright faces");
    CHECK(strcmp(base_name(font_for(t, "A", sel(t, "Noto Sans", 400 | ITALIC))), "NotoSans-Italic.ttf") == 0, "a named family's italic");

    /* a family with no italic: its upright face, sheared */
    gp_glyph_pos up[8], it[8];
    float wu, wi;
    gp_text_shape(t, sel(t, "Noto Sans Mono", 400), 40, "HHH", 3, up, 8, &wu);
    gp_text_shape(t, sel(t, "Noto Sans Mono", 400 | ITALIC), 40, "HHH", 3, it, 8, &wi);
    CHECK(it[0].font != up[0].font && same_path(gp_text_font_path(t, it[0].font), gp_text_font_path(t, up[0].font)), "synthetic italic is a variant of the same file");
    CHECK(wu == wi, "and has the same advances (%.3f vs %.3f)", wu, wi);
    float s_up = slant_of(t, up[0].font, up[0].glyph, 40.f), s_it = slant_of(t, it[0].font, it[0].glyph, 40.f);
    CHECK(fabsf(s_up) < 0.4f && s_it > 0.15f * 40.f * 0.7f, "upright H leans %.2f px, the oblique one %.2f px (a 14 degree shear of a 40 px H is about 5 px)", s_up, s_it);
    CHECK(s_it < 0.25f * 40.f, "and not more than the shear allows (%.2f px)", s_it);
    int w_up, w_it;
    long ink_up = ink(t, up[0].font, up[0].glyph, 40.f, &w_up), ink_it = ink(t, it[0].font, it[0].glyph, 40.f, &w_it);
    CHECK(w_it > w_up + 3 && labs(ink_it - ink_up) < ink_up / 10, "the sheared glyph is wider (%d vs %d px) with about the same ink (%ld vs %ld)", w_it, w_up, ink_it, ink_up);
    /* shaping italic did not change upright */
    float wu2;
    gp_text_shape(t, sel(t, "Noto Sans Mono", 400), 40, "HHH", 3, up, 8, &wu2);
    CHECK(wu2 == wu && fabsf(slant_of(t, up[0].font, up[0].glyph, 40.f)) < 0.4f, "upright text is unaffected once italic exists");

    /* bold and italic together, both made up: one variant that is both */
    gp_text_system *t2 = fresh(false);
    snprintf(path, sizeof path, "%s", noto("NotoSerif-Regular.ttf"));
    gp_text_add_font_file(t2, path, 0, 0);
    gp_glyph_pos r2[8], b2[8], i2[8], bi2[8];
    float w_r, w_b, w_i, w_bi;
    gp_text_shape(t2, 400, 40, "HHH", 3, r2, 8, &w_r);
    gp_text_shape(t2, 700, 40, "HHH", 3, b2, 8, &w_b);
    gp_text_shape(t2, 400 | ITALIC, 40, "HHH", 3, i2, 8, &w_i);
    gp_text_shape(t2, 700 | ITALIC, 40, "HHH", 3, bi2, 8, &w_bi);
    CHECK(w_i == w_r && w_bi == w_b && w_b > w_r, "bold widens, italic does not (%.2f %.2f %.2f %.2f)", w_r, w_b, w_i, w_bi);
    CHECK(bi2[0].font != b2[0].font && bi2[0].font != i2[0].font && bi2[0].font != r2[0].font, "bold italic is its own variant");
    CHECK(slant_of(t2, bi2[0].font, bi2[0].glyph, 40.f) > 3.f && fabsf(slant_of(t2, b2[0].font, b2[0].glyph, 40.f)) < 0.4f, "bold italic leans, bold does not");
    int wd1, wd2;
    long ink_b = ink(t2, b2[0].font, b2[0].glyph, 40.f, &wd1), ink_bi = ink(t2, bi2[0].font, bi2[0].glyph, 40.f, &wd2);
    CHECK(labs(ink_bi - ink_b) < ink_b / 10, "bold italic is as heavy as bold (%ld vs %ld)", ink_bi, ink_b);
    /* the same request again finds the same variant instead of opening another one */
    int count = gp_text_font_count(t2);
    gp_text_shape(t2, 700 | ITALIC, 40, "HHH", 3, bi2, 8, NULL);
    CHECK(gp_text_font_count(t2) == count, "variants are reused (%d fonts, was %d)", gp_text_font_count(t2), count);
    gp_text_system_free(t2);

    /* only an italic face registered: upright text uses it rather than nothing, and italic does not shear it more */
    t2 = fresh(false);
    snprintf(path, sizeof path, "%s", noto("NotoSerif-Italic.ttf"));
    gp_text_add_font_file(t2, path, 0, 0);
    CHECK(same_path(font_for(t2, "A", 400), path) && same_path(font_for(t2, "A", 400 | ITALIC), path), "an italic-only family draws it for both");
    gp_glyph_pos g1[4], g2[4];
    gp_text_shape(t2, 400, 40, "H", 1, g1, 4, NULL);
    gp_text_shape(t2, 400 | ITALIC, 40, "H", 1, g2, 4, NULL);
    CHECK(g1[0].font == g2[0].font, "(the same font: it is italic already)");
    gp_text_system_free(t2);
    gp_text_system_free(t);

    /* a character the family lacks comes from another registered font, the italic one if italic is asked for */
    {
        char serif_r[256], serif_i[256], mono[256];
        snprintf(serif_r, sizeof serif_r, "%s", noto("NotoSerif-Regular.ttf"));
        snprintf(serif_i, sizeof serif_i, "%s", noto("NotoSerif-Italic.ttf"));
        snprintf(mono, sizeof mono, "%s", noto("NotoSansMono-Regular.ttf"));
        uint32_t cp = 0;
        for (uint32_t c = 0x100; c < 0xfd00 && !cp; c++)
            if (has_glyph_in(serif_r, c) && has_glyph_in(serif_i, c) && !has_glyph_in(mono, c)) cp = c;
        if (cp) {
            char u[4] = { 0 };
            if (cp < 0x800) { u[0] = (char)(0xc0 | (cp >> 6)); u[1] = (char)(0x80 | (cp & 63)); }
            else { u[0] = (char)(0xe0 | (cp >> 12)); u[1] = (char)(0x80 | ((cp >> 6) & 63)); u[2] = (char)(0x80 | (cp & 63)); }
            t2 = fresh(false);
            gp_text_add_font_file(t2, mono, 0, 0);
            gp_text_add_font_file(t2, serif_r, 0, 0);
            gp_text_add_font_file(t2, serif_i, 0, 0);
            CHECK(same_path(font_for(t2, u, 400 | ITALIC), serif_i) && same_path(font_for(t2, u, 400), serif_r),
                  "U+%04X missing from the family: the italic request takes the italic font, the upright one the upright", cp);
            gp_text_system_free(t2);
        } else {
            skips++;
            printf("SKIP: no character found that Noto Sans Mono lacks and both Noto Serif faces have\n");
        }
    }

    /* the system's fonts */
    t = fresh(true);
    CHECK(strcmp(base_name(font_for(t, "A", sel(t, "Noto Sans", 400 | ITALIC))), "NotoSans-Italic.ttf") == 0, "system: Noto Sans italic: %s", base_name(font_for(t, "A", sel(t, "Noto Sans", 400 | ITALIC))));
    CHECK(strcmp(base_name(font_for(t, "A", sel(t, "Noto Sans", 700 | ITALIC))), "NotoSans-BoldItalic.ttf") == 0, "system: Noto Sans bold italic");
    CHECK(strcmp(base_name(font_for(t, "A", sel(t, "Noto Serif", 400 | ITALIC))), "NotoSerif-Italic.ttf") == 0, "system: Noto Serif italic");
    CHECK(strcmp(base_name(font_for(t, "A", sel(t, "Noto Sans", 400))), "NotoSans-Regular.ttf") == 0, "system: upright is still upright");
    CHECK(strcmp(base_name(font_for(t, "A", 400 | ITALIC)), "NotoSans-Italic.ttf") == 0 || strstr(base_name(font_for(t, "A", 400 | ITALIC)), "Italic") ||
              strstr(base_name(font_for(t, "A", 400 | ITALIC)), "Oblique"),
          "system: the default family's italic is an italic face (%s)", base_name(font_for(t, "A", 400 | ITALIC)));
    gp_glyph_pos m1[4], m2[4];
    gp_text_shape(t, sel(t, "Noto Sans Mono", 400), 40, "H", 1, m1, 4, NULL);
    gp_text_shape(t, sel(t, "Noto Sans Mono", 400 | ITALIC), 40, "H", 1, m2, 4, NULL);
    CHECK(same_path(gp_text_font_path(t, m2[0].font), gp_text_font_path(t, m1[0].font)) && m2[0].font != m1[0].font &&
              slant_of(t, m2[0].font, m2[0].glyph, 40.f) > 3.f,
          "system: a family with no italic is slanted (%.2f px)", slant_of(t, m2[0].font, m2[0].glyph, 40.f));
    gp_glyph_pos c1[4];
    gp_text_shape(t, sel(t, "serif", 400 | ITALIC), 40, "\xe6\x97\xa5", 3, c1, 4, NULL);   /* a CJK ideograph in an italic serif: the fallback font is slanted too */
    const char *cp = gp_text_font_path(t, c1[0].font);
    CHECK(cp != NULL, "system: an italic CJK fallback resolves (%s)", cp ? cp : "(none)");
    gp_text_system_free(t);
}

/* ---------------------------------------------------------------- OpenType features */

static int shape_feat(gp_text_system *t, const char *features, const char *text, gp_glyph_pos *g, float *w) {
    int64_t sel_ = GP_FONT_SEL(gp_text_family(t, "Noto Sans"), 400) | GP_FONT_FEATURES(gp_text_features(t, features));
    return gp_text_shape(t, sel_, 20, text, strlen(text), g, MAXG, w);
}

static void test_features(void) {
    if (!exists(noto("NotoSans-Regular.ttf")) || !exists(noto("NotoSansMono-Regular.ttf"))) {
        skips++;
        printf("SKIP: OpenType feature tests need Noto Sans and Noto Sans Mono\n");
        return;
    }
    gp_text_system *t = fresh(false);
    gp_text_add_font_file(t, noto("NotoSans-Regular.ttf"), 0, 0);
    gp_text_add_font_file(t, noto("NotoSansMono-Regular.ttf"), 0, 0);

    CHECK(gp_text_features(t, NULL) == 0 && gp_text_features(t, "") == 0 && gp_text_features(t, "normal") == 0 && gp_text_features(t, "None") == 0,
          "no features is id 0");
    int a = gp_text_features(t, "tnum, liga=0");
    CHECK(a != 0 && a == gp_text_features(t, "tnum, liga=0") && a != gp_text_features(t, "tnum"), "feature lists are interned");

    gp_glyph_pos g0[MAXG], g1[MAXG];
    float w0, w1;
    /* ligatures: on by default, "liga=0" and "-liga" turn the fi ligature into two glyphs, "liga" and "+liga" keep it */
    int n0 = shape_feat(t, NULL, "fi", g0, &w0);
    CHECK(n0 == 1, "fi is a ligature by default (%d glyphs)", n0);
    CHECK(shape_feat(t, "liga=0", "fi", g1, &w1) == 2 && g1[0].glyph != g0[0].glyph, "liga=0 splits it");
    CHECK(shape_feat(t, "-liga", "fi", g1, &w1) == 2, "-liga too");
    CHECK(shape_feat(t, "liga", "fi", g1, &w1) == 1 && shape_feat(t, "+liga", "fi", g1, &w1) == 1, "liga and +liga keep it");
    CHECK(shape_feat(t, "tnum, liga=0", "fi", g1, &w1) == 2 && shape_feat(t, "liga=0 , tnum", "fi", g1, &w1) == 2, "items are comma separated, in any order");
    /* a clause HarfBuzz cannot parse is ignored, the rest still applies */
    CHECK(shape_feat(t, "bogus!!, liga=0", "fi", g1, &w1) == 2, "an unparsable item is skipped");
    CHECK(shape_feat(t, "bogus!!", "fi", g1, &w1) == 1, "and a list of nothing else changes nothing");
    CHECK(shape_feat(t, NULL, "fi fi", g1, &w1) == 3 && shape_feat(t, "liga=0", "fi fi", g1, &w1) == 5, "(both ligatures, then neither)");
    CHECK(shape_feat(t, "liga[3:5]=0", "fi fi", g1, &w1) == 5, "a range applies to the whole text: not just the second fi");

    /* figures: Noto Sans' default digits are tabular */
    int n = shape_feat(t, NULL, "1011", g0, &w0);
    CHECK(n == 4 && g0[0].advance == g0[1].advance, "default digits have one width (%.2f %.2f)", g0[0].advance, g0[1].advance);
    n = shape_feat(t, "pnum", "1011", g1, &w1);
    CHECK(n == 4 && g1[0].advance < g1[1].advance && w1 < w0 - 1.f, "pnum: a 1 is narrower than a 0 (%.2f vs %.2f; %.2f vs %.2f in all)", g1[0].advance, g1[1].advance, w1, w0);
    n = shape_feat(t, "tnum", "1011", g1, &w1);
    CHECK(n == 4 && w1 == w0 && g1[0].glyph == g0[0].glyph, "tnum is the default for this font");
    n = shape_feat(t, "onum", "1011", g1, &w1);
    CHECK(n == 4 && g1[0].glyph != g0[0].glyph && w1 != w0, "onum draws old-style figures");
    n = shape_feat(t, "zero", "1011", g1, &w1);
    CHECK(n == 4 && g1[1].glyph != g0[1].glyph && g1[0].glyph == g0[0].glyph, "zero changes the 0 and not the 1");
    /* kerning, and several at once */
    shape_feat(t, NULL, "AV", g0, &w0);
    shape_feat(t, "kern=0", "AV", g1, &w1);
    CHECK(w1 > w0 + 0.5f, "kern=0: AV is wider (%.2f vs %.2f)", w1, w0);
    shape_feat(t, "smcp", "abc", g1, &w1);
    shape_feat(t, NULL, "abc", g0, &w0);
    CHECK(g1[0].glyph != g0[0].glyph && w1 < w0, "smcp draws small capitals (%.2f vs %.2f)", w1, w0);
    float w2;
    shape_feat(t, "smcp, kern=0", "AVa", g1, &w1);
    shape_feat(t, "smcp", "AVa", g0, &w2);
    CHECK(w1 > w2 + 0.3f, "two features both apply (%.2f vs %.2f)", w1, w2);
    /* a feature the font lacks is ignored */
    gp_glyph_pos m0[MAXG], m1[MAXG];
    float wm0, wm1;
    int64_t mono0 = GP_FONT_SEL(gp_text_family(t, "Noto Sans Mono"), 400);
    gp_text_shape(t, mono0, 20, "fi 1011", 7, m0, MAXG, &wm0);
    gp_text_shape(t, mono0 | GP_FONT_FEATURES(gp_text_features(t, "tnum, pnum, liga=0, ss99")), 20, "fi 1011", 7, m1, MAXG, &wm1);
    CHECK(wm0 == wm1 && m0[0].glyph == m1[0].glyph, "a font without the feature is not changed (%.2f %.2f)", wm0, wm1);

    /* the shape cache keeps lists apart */
    shape_feat(t, NULL, "fi", g0, &w0);
    int na = shape_feat(t, "liga=0", "fi", g1, &w1);
    int nb = shape_feat(t, NULL, "fi", g0, &w0);
    CHECK(na == 2 && nb == 1, "cached runs of one list are not handed out for another (%d, %d glyphs)", na, nb);

    /* it works across a run of Hebrew too (same shaping path for any direction), and in a font-fallback run */
    gp_glyph_pos h[MAXG];
    CHECK(shape_feat(t, "liga=0", "fi \xe6\x97\xa5 fi", h, NULL) >= 5, "features and fallback fonts shape together");
    gp_text_system_free(t);
}

/* ---------------------------------------------------------------- LCD antialiasing */

/* The LCD atlas cell of an entry as a copy: w x h pixels of r, g, b. */
static uint8_t *lcd_cell(gp_text_system *t, gp_glyph_entry e) {
    const uint8_t *a = gp_text_lcd_atlas(t);
    uint8_t *out = malloc((size_t)e.w * (size_t)e.h * 3 + 1);
    for (int y = 0; y < (int)e.h; y++)
        for (int x = 0; x < (int)e.w; x++)
            for (int c = 0; c < 3; c++)
                out[((size_t)y * (size_t)e.w + (size_t)x) * 3 + (size_t)c] = a[(((size_t)((int)e.v + y)) * GP_LCD_ATLAS_SIZE + (size_t)((int)e.u + x)) * 4 + (size_t)c];
    return out;
}

static void test_lcd(const char *R, const char *E) {
    gp_text_system *t = fresh(false);
    gp_text_add_font_file(t, R, 0, 0);
    gp_glyph_pos g[4];
    gp_text_shape(t, 400, 20, "l", 1, g, 4, NULL);   /* a plain vertical stem */
    const float px = 20.f;

    gp_glyph_entry gray = gp_text_glyph_lcd(t, g[0].font, g[0].glyph, px, 0, 0);
    gp_glyph_entry rgb = gp_text_glyph_lcd(t, g[0].font, g[0].glyph, px, 0, 1);
    gp_glyph_entry bgr = gp_text_glyph_lcd(t, g[0].font, g[0].glyph, px, 0, 2);
    CHECK(!gray.lcd && rgb.lcd && bgr.lcd, "lcd flag: %d %d %d", gray.lcd, rgb.lcd, bgr.lcd);
    CHECK(rgb.w >= gray.w && rgb.w <= gray.w + 4 && rgb.h == gray.h && fabsf(rgb.left - gray.left) <= 2.f && rgb.top == gray.top,
          "an LCD glyph is the grayscale one plus the filter's padding: %.0fx%.0f at %.0f,%.0f vs %.0fx%.0f at %.0f,%.0f", rgb.w, rgb.h, rgb.left, rgb.top, gray.w, gray.h, gray.left, gray.top);

    /* each channel carries the glyph's ink (the same total, within the filter's rounding), and they differ at the edges */
    uint8_t *a = lcd_cell(t, rgb), *b = lcd_cell(t, bgr);
    long ch[3] = { 0, 0, 0 }, differing = 0;
    for (size_t k = 0; k < (size_t)(rgb.w * rgb.h); k++) {
        for (int c = 0; c < 3; c++) ch[c] += a[k * 3 + (size_t)c];
        if (abs((int)a[k * 3] - (int)a[k * 3 + 2]) > 30) differing++;
    }
    int ww;
    long gray_ink = ink(t, g[0].font, g[0].glyph, px, &ww);
    for (int c = 0; c < 3; c++)
        CHECK(labs(ch[c] - gray_ink) < gray_ink / 8, "channel %d holds %ld of coverage, grayscale %ld", c, ch[c], gray_ink);
    CHECK(differing >= 2, "the channels differ where the stem's edges cut a pixel (%ld pixels)", differing);
    /* against FreeType's own LCD rendering of the same glyph (its default filter), sample for sample */
    {
        FT_Library lib;
        FT_Face face;
        FT_Init_FreeType(&lib);
        FT_Library_SetLcdFilter(lib, FT_LCD_FILTER_DEFAULT);
        if (FT_New_Face(lib, R, 0, &face) == 0) {
            FT_Set_Char_Size(face, 0, (FT_F26Dot6)(px * 64.f + 0.5f), 72, 72);
            if (FT_Load_Glyph(face, g[0].glyph, FT_LOAD_TARGET_LIGHT) == 0 && FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LCD) == 0) {
                FT_Bitmap *bm = &face->glyph->bitmap;
                CHECK((int)bm->width / 3 == (int)rgb.w && (int)bm->rows == (int)rgb.h && face->glyph->bitmap_left == (int)rgb.left && face->glyph->bitmap_top == (int)rgb.top,
                      "the cell is FreeType's: %dx%d at %d,%d vs %.0fx%.0f at %.0f,%.0f", (int)bm->width / 3, (int)bm->rows, face->glyph->bitmap_left, face->glyph->bitmap_top, rgb.w, rgb.h, rgb.left, rgb.top);
                long wrong = 0;
                for (int y = 0; y < (int)bm->rows && y < (int)rgb.h; y++)
                    for (int x = 0; x < (int)bm->width / 3 && x < (int)rgb.w; x++)
                        for (int c = 0; c < 3; c++)
                            wrong += bm->buffer[(size_t)y * (size_t)bm->pitch + (size_t)(x * 3 + c)] != a[((size_t)y * (size_t)rgb.w + (size_t)x) * 3 + (size_t)c];
                CHECK(wrong == 0, "and so are its samples (%ld of them differ)", wrong);
            }
            FT_Done_Face(face);
        }
        FT_Done_FreeType(lib);
    }
    /* BGR is RGB with the red and blue samples exchanged, pixel for pixel */
    bool swapped = rgb.w == bgr.w && rgb.h == bgr.h;
    for (size_t k = 0; swapped && k < (size_t)(rgb.w * rgb.h); k++)
        swapped = a[k * 3] == b[k * 3 + 2] && a[k * 3 + 1] == b[k * 3 + 1] && a[k * 3 + 2] == b[k * 3];
    CHECK(swapped, "BGR is RGB with red and blue exchanged");
    free(a);
    free(b);

    /* vertical stripes (a panel turned a quarter): the glyph has a cross-bar, whose top and bottom edges are what shows them */
    {
        gp_glyph_pos hg[4];
        gp_text_shape(t, 400, 20, "e", 1, hg, 4, NULL);
        gp_glyph_entry v0 = gp_text_glyph_lcd(t, hg[0].font, hg[0].glyph, px, 0, 0);
        gp_glyph_entry vr = gp_text_glyph_lcd(t, hg[0].font, hg[0].glyph, px, 0, 3), vb = gp_text_glyph_lcd(t, hg[0].font, hg[0].glyph, px, 0, 4);
        gp_glyph_entry hr = gp_text_glyph_lcd(t, hg[0].font, hg[0].glyph, px, 0, 1);
        CHECK(vr.lcd && vb.lcd && vr.w > 0 && vr.h > 0, "the vertical orders make LCD glyphs");
        CHECK(vr.w == v0.w || vr.w <= v0.w + 1, "a vertical LCD glyph is as wide as the grayscale one (%.0f vs %.0f)", vr.w, v0.w);
        CHECK(vr.h >= v0.h && vr.h <= v0.h + 4, "...and taller by the filter's padding (%.0f vs %.0f)", vr.h, v0.h);
        CHECK(vr.u != hr.u || vr.v != hr.v, "horizontal and vertical versions are cached apart");
        uint8_t *va = lcd_cell(t, vr), *vbb = lcd_cell(t, vb);
        FT_Library lib;
        FT_Face face;
        FT_Init_FreeType(&lib);
        FT_Library_SetLcdFilter(lib, FT_LCD_FILTER_DEFAULT);
        if (FT_New_Face(lib, R, 0, &face) == 0) {
            FT_Set_Char_Size(face, 0, (FT_F26Dot6)(px * 64.f + 0.5f), 72, 72);
            if (FT_Load_Glyph(face, hg[0].glyph, FT_LOAD_TARGET_LIGHT) == 0 && FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LCD_V) == 0) {
                FT_Bitmap *bm = &face->glyph->bitmap;
                CHECK((int)bm->width == (int)vr.w && (int)bm->rows / 3 == (int)vr.h && face->glyph->bitmap_left == (int)vr.left && face->glyph->bitmap_top == (int)vr.top,
                      "the vertical cell is FreeType's: %dx%d at %d,%d vs %.0fx%.0f at %.0f,%.0f", (int)bm->width, (int)bm->rows / 3, face->glyph->bitmap_left, face->glyph->bitmap_top, vr.w, vr.h, vr.left, vr.top);
                long wrong = 0, cells = 0;
                for (int y = 0; y < (int)bm->rows / 3 && y < (int)vr.h; y++)
                    for (int x = 0; x < (int)bm->width && x < (int)vr.w; x++)
                        for (int c = 0; c < 3; c++) {
                            wrong += bm->buffer[(size_t)(y * 3 + c) * (size_t)bm->pitch + (size_t)x] != va[((size_t)y * (size_t)vr.w + (size_t)x) * 3 + (size_t)c];
                            cells++;
                        }
                CHECK(wrong == 0 && cells > 100, "and so are its samples, three rows to a pixel (%ld of %ld differ)", wrong, cells);
            }
            FT_Done_Face(face);
        }
        FT_Done_FreeType(lib);
        bool sw = vr.w == vb.w && vr.h == vb.h;
        for (size_t k = 0; sw && k < (size_t)(vr.w * vr.h); k++) sw = va[k * 3] == vbb[k * 3 + 2] && va[k * 3 + 1] == vbb[k * 3 + 1] && va[k * 3 + 2] == vbb[k * 3];
        CHECK(sw, "VBGR is VRGB with red and blue exchanged");
        long differ = 0, ch[3] = { 0, 0, 0 };
        for (size_t k = 0; k < (size_t)(vr.w * vr.h); k++) {
            for (int c = 0; c < 3; c++) ch[c] += va[k * 3 + (size_t)c];
            if (abs((int)va[k * 3] - (int)va[k * 3 + 2]) > 30) differ++;
        }
        int ww2;
        long gray_ink2 = ink(t, hg[0].font, hg[0].glyph, px, &ww2);
        for (int c = 0; c < 3; c++) CHECK(labs(ch[c] - gray_ink2) < gray_ink2 / 8, "vertical channel %d holds %ld of coverage, grayscale %ld", c, ch[c], gray_ink2);
        CHECK(differ >= 4, "the channels differ where the bar's edges cut a pixel (%ld pixels)", differ);
        free(va);
        free(vbb);
    }

    /* the gray entry is not disturbed by LCD ones, and a repeat is the same cell */
    gp_glyph_entry gray2 = gp_text_glyph_lcd(t, g[0].font, g[0].glyph, px, 0, 0), rgb2 = gp_text_glyph_lcd(t, g[0].font, g[0].glyph, px, 0, 1);
    CHECK(gray2.u == gray.u && gray2.v == gray.v && !gray2.lcd && rgb2.u == rgb.u && rgb2.v == rgb.v, "the three versions are cached apart");
    CHECK(gp_text_glyph_at(t, g[0].font, g[0].glyph, px, 0).u == gray.u, "gp_text_glyph_at is the grayscale one");

    /* a quarter-pixel position shifts the LCD glyph too */
    gp_glyph_entry q = gp_text_glyph_lcd(t, g[0].font, g[0].glyph, px, 2, 1);
    uint8_t *q0 = lcd_cell(t, rgb), *q2 = lcd_cell(t, q);
    double c0 = 0, w0 = 0, c2 = 0, w2 = 0;
    for (int y = 0; y < (int)rgb.h; y++)
        for (int x = 0; x < (int)rgb.w; x++) { double v = q0[((size_t)y * (size_t)rgb.w + (size_t)x) * 3 + 1]; c0 += v * x; w0 += v; }
    for (int y = 0; y < (int)q.h; y++)
        for (int x = 0; x < (int)q.w; x++) { double v = q2[((size_t)y * (size_t)q.w + (size_t)x) * 3 + 1]; c2 += v * x; w2 += v; }
    double shift = (c2 / w2 + q.left) - (c0 / w0 + rgb.left);
    CHECK(fabs(shift - 0.5) < 0.12, "bin 2 moves the LCD glyph half a pixel right (%.3f px)", shift);
    free(q0);
    free(q2);

    /* what it does not apply to */
    if (E) {
        gp_text_system *te = fresh(false);
        gp_text_add_font_file(te, R, 0, 0);
        gp_text_add_font_file(te, E, 0, 0);
        gp_glyph_pos eg[4];
        gp_text_shape(te, 400, 20, "\xf0\x9f\x98\x80", 4, eg, 4, NULL);
        gp_glyph_entry ee = gp_text_glyph_lcd(te, eg[0].font, eg[0].glyph, 20.f, 0, 1);
        CHECK(ee.w > 0 && ee.color && !ee.lcd, "a color emoji stays a color glyph in LCD mode");
        gp_text_system_free(te);
    }
    CHECK(!gp_text_glyph_lcd(t, g[0].font, g[0].glyph, px, 0, 7).lcd && !gp_text_glyph_lcd(t, g[0].font, g[0].glyph, px, 0, -1).lcd, "an order out of range is grayscale");

    /* which order to ask for */
    gp_text_set_antialiasing(t, GP_AA_GRAYSCALE);
    CHECK(gp_text_lcd_order(t, 1.f) == 0 && gp_text_lcd_order(t, 2.f) == 0, "grayscale: order 0");
    gp_text_set_antialiasing(t, GP_AA_LCD_RGB);
    CHECK(gp_text_lcd_order(t, 1.f) == 1 && gp_text_lcd_order(t, 3.f) == 1, "rgb: order 1 at any scale");
    uint32_t gen = gp_text_atlas_generation(t);
    gp_text_set_antialiasing(t, GP_AA_LCD_BGR);
    CHECK(gp_text_lcd_order(t, 1.f) == 2 && gp_text_atlas_generation(t) != gen, "bgr: order 2, and what was painted is out of date");
    gp_text_set_antialiasing(t, GP_AA_LCD_VRGB);
    CHECK(gp_text_lcd_order(t, 1.f) == 3 && gp_text_lcd_order(t, 3.f) == 3, "vrgb: order 3 at any scale");
    gp_text_set_antialiasing(t, GP_AA_LCD_VBGR);
    CHECK(gp_text_lcd_order(t, 1.f) == 4, "vbgr: order 4");
    {   /* the environment variable names the starting mode */
        char *saved = getenv("GPUIC_LCD") ? strdup(getenv("GPUIC_LCD")) : NULL;
        static const struct { const char *name; int order; } envs[] = { { "rgb", 1 }, { "bgr", 2 }, { "vrgb", 3 }, { "vbgr", 4 }, { "off", 0 } };
        for (size_t k = 0; k < sizeof envs / sizeof *envs; k++) {
            setenv("GPUIC_LCD", envs[k].name, 1);
            gp_text_system *te = fresh(false);
            CHECK(gp_text_lcd_order(te, 1.f) == envs[k].order, "GPUIC_LCD=%s: order %d, wanted %d", envs[k].name, gp_text_lcd_order(te, 1.f), envs[k].order);
            gp_text_system_free(te);
        }
        if (saved) setenv("GPUIC_LCD", saved, 1); else unsetenv("GPUIC_LCD");
        free(saved);
    }
    gen = gp_text_atlas_generation(t);
    gp_text_set_antialiasing(t, GP_AA_LCD_VBGR);
    CHECK(gp_text_atlas_generation(t) == gen, "setting the same mode again changes nothing");
    gp_text_set_antialiasing(t, GP_AA_AUTO);
#ifdef GPUIC_HAVE_FONTCONFIG
    /* AUTO follows fontconfig's "rgba" setting: a configuration that says rgb, then bgr, then nothing */
    {
        static const struct { const char *rgba; int order; } cases[] = { { "rgb", 1 }, { "bgr", 2 }, { "none", 0 }, { "vrgb", 3 }, { "vbgr", 4 } };
        char dir[512], file[600];
        snprintf(dir, sizeof dir, "%s/gpuic-fc-lcd-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
        if (mkdtemp(dir)) {
            const char *old = getenv("FONTCONFIG_FILE");
            char *saved = old ? strdup(old) : NULL;
            const char *old_aa = getenv("GPUIC_LCD");   /* the environment may pin a mode (ctest sets "off"); AUTO is what is tested here */
            char *saved_aa = old_aa ? strdup(old_aa) : NULL;
            unsetenv("GPUIC_LCD");
            for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
                snprintf(file, sizeof file, "%s/fonts.conf", dir);
                FILE *f = fopen(file, "w");
                if (!f) break;
                fprintf(f, "<?xml version=\"1.0\"?><!DOCTYPE fontconfig SYSTEM \"fonts.dtd\"><fontconfig><include ignore_missing=\"yes\">/etc/fonts/fonts.conf</include>"
                           "<match target=\"pattern\"><edit name=\"rgba\" mode=\"assign\"><const>%s</const></edit></match></fontconfig>\n", cases[k].rgba);
                fclose(f);
                setenv("FONTCONFIG_FILE", file, 1);
                FcInitReinitialize();
                gp_text_system *ta = fresh(false);
                CHECK(gp_text_lcd_order(ta, 1.f) == cases[k].order, "auto with fontconfig rgba=%s: order %d, wanted %d", cases[k].rgba, gp_text_lcd_order(ta, 1.f), cases[k].order);
                CHECK(gp_text_lcd_order(ta, 2.f) == 0 && gp_text_lcd_order(ta, 1.49f) == cases[k].order, "...and none at 2x, still used just below 1.5x");
                gp_text_set_antialiasing(ta, GP_AA_LCD_RGB);
                CHECK(gp_text_lcd_order(ta, 2.f) == 1, "...while an explicit mode ignores it");
                gp_text_system_free(ta);
                remove(file);
            }
            if (saved) setenv("FONTCONFIG_FILE", saved, 1); else unsetenv("FONTCONFIG_FILE");
            free(saved);
            if (saved_aa) setenv("GPUIC_LCD", saved_aa, 1);
            free(saved_aa);
            FcInitReinitialize();
            rmdir(dir);
        }
    }
#else
    CHECK(gp_text_lcd_order(t, 1.f) == 0 && gp_text_lcd_order(t, 2.f) == 0, "auto without fontconfig: grayscale");
#endif

    /* the LCD atlas fills up, reports it, and starts over after a reset */
    gp_text_set_antialiasing(t, GP_AA_LCD_RGB);
    gp_text_atlas_reset(t);
    int x, y, w, h;
    while (gp_text_lcd_atlas_take_dirty(t, &x, &y, &w, &h)) {}
    CHECK(!gp_text_lcd_atlas_take_dirty(t, &x, &y, &w, &h), "no dirty rectangle once taken");
    gp_text_glyph_lcd(t, g[0].font, g[0].glyph, 33.f, 0, 1);
    CHECK(gp_text_lcd_atlas_take_dirty(t, &x, &y, &w, &h) && w > 0 && h > 0, "a new LCD glyph marks its rectangle dirty");
    bool overflowed = false;
    for (int i = 0; i < 6000 && !overflowed; i++) {
        gp_text_glyph_lcd(t, g[0].font, g[0].glyph, 20.f + (float)i * 0.25f, 0, 1);
        overflowed = gp_text_atlas_overflowed(t);
    }
    CHECK(overflowed, "filling the LCD atlas is reported");
    gp_text_atlas_reset(t);
    CHECK(!gp_text_atlas_overflowed(t), "and a reset clears it");
    gp_glyph_entry again = gp_text_glyph_lcd(t, g[0].font, g[0].glyph, px, 0, 1);
    CHECK(again.lcd && again.w > 0, "glyphs can be made again after the reset");
    gp_text_system_free(t);
}

/* The system's own fonts, by family name and weight. Needs the same Noto files; the rest is what the system says. */
static void test_system_families(void) {
    if (!exists(noto("NotoSans-Light.ttf")) || !exists(noto("NotoSerif-Regular.ttf")) || !exists(noto("NotoSansMono-Regular.ttf")) ||
        !exists(noto("NotoSans-Black.ttf")) || !exists(noto("NotoSans-Medium.ttf"))) {
        skips++;
        printf("SKIP: system family lookups are shown with the Noto families, which are not installed\n");
        return;
    }
    gp_text_system *t = fresh(true);
    struct { const char *family; int weight; const char *file; } cases[] = {
        { "Noto Sans", 400, "NotoSans-Regular.ttf" },
        { "Noto Sans", 300, "NotoSans-Light.ttf" },
        { "Noto Sans", 500, "NotoSans-Medium.ttf" },
        { "Noto Sans", 900, "NotoSans-Black.ttf" },
        { "Noto Sans", 700, "NotoSans-Bold.ttf" },
        { "noto serif", 400, "NotoSerif-Regular.ttf" },
        { "Noto Serif", 700, "NotoSerif-Bold.ttf" },
        { "Noto Sans Mono", 400, "NotoSansMono-Regular.ttf" },
        { "Zzz No Such, Noto Serif", 400, "NotoSerif-Regular.ttf" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        const char *got = base_name(font_for(t, "A", sel(t, cases[i].family, cases[i].weight)));
        CHECK(strcmp(got, cases[i].file) == 0, "system: \"%s\" at %d draws %s, wanted %s", cases[i].family, cases[i].weight, got, cases[i].file);
    }
    CHECK(strcmp(base_name(font_for(t, "A", sel(t, "Noto Sans", 350))), "NotoSans-Light.ttf") == 0, "system: weight 350 draws the face below it, not the nearest");
    CHECK(strcmp(base_name(font_for(t, "A", sel(t, "Noto Sans", 450))), "NotoSans-Medium.ttf") == 0, "system: weight 450 draws the medium face");
    CHECK(strcmp(base_name(font_for(t, "A", sel(t, "Noto Sans", 5000))), "NotoSans-Black.ttf") == 0, "system: weights above 1000 are the heaviest");
    if (exists(noto("NotoSerifHebrew-Regular.ttf")) && exists(noto("NotoSansHebrew-Regular.ttf"))) {
        /* a character the family lacks is looked for in the list's generic family: a serif stays serif */
        CHECK(strstr(base_name(font_for(t, "\xd7\x90", sel(t, "serif", 400))), "Serif") != NULL, "system: Hebrew in \"serif\" comes from a serif font (%s)", base_name(font_for(t, "\xd7\x90", sel(t, "serif", 400))));
        CHECK(strstr(base_name(font_for(t, "\xd7\x90", sel(t, "Noto Serif, serif", 400))), "Serif") != NULL, "system: and in \"Noto Serif, serif\" (%s)", base_name(font_for(t, "\xd7\x90", sel(t, "Noto Serif, serif", 400))));
        CHECK(strstr(base_name(font_for(t, "\xd7\x90", 400)), "Serif") == NULL, "system: but not in the default (%s)", base_name(font_for(t, "\xd7\x90", 400)));
    }
    const char *def = font_for(t, "A", 400);
    CHECK(same_path(font_for(t, "A", sel(t, "Zzz No Such", 400)), def), "system: a family nobody has draws the default");

    /* generics: the system's serif is not the sans, and its monospace is monospaced */
    const char *serif = font_for(t, "A", sel(t, "serif", 400));
    const char *mono = font_for(t, "A", sel(t, "monospace", 400));
    CHECK(serif && !same_path(serif, def), "system: serif differs from sans-serif (%s)", base_name(serif));
    CHECK(mono && !same_path(mono, def) && !same_path(mono, serif), "system: monospace differs from both (%s)", base_name(mono));
    float wi, wm;
    gp_text_shape(t, sel(t, "monospace", 400), 20, "iiiiiiii", 8, NULL, 0, &wi);
    gp_text_shape(t, sel(t, "monospace", 400), 20, "WWWWWWWW", 8, NULL, 0, &wm);
    CHECK(fabsf(wi - wm) < 0.01f, "system: the monospace font is monospaced (%.2f vs %.2f)", wi, wm);
    gp_text_system_free(t);

    /* a variable font offers its named instances as weights, in shaping and in the glyphs drawn */
#ifdef GPUIC_HAVE_FONTCONFIG
    if (exists("/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf")) {
        t = fresh(true);
        int prev_w = 0;
        long prev_ink = 0;
        bool monotone = true, distinct = true;
        int last_font = -1;
        for (int w = 200; w <= 900; w += 200) {
            gp_glyph_pos g[8];
            float width;
            gp_text_shape(t, sel(t, "Adwaita Sans", w), 24, "H", 1, g, 8, &width);
            int iw;
            long ink_now = ink(t, g[0].font, g[0].glyph, 24.f, &iw);
            if (prev_w && (ink_now <= prev_ink || width <= (float)prev_w / 100.f)) monotone = false;
            if (g[0].font == last_font) distinct = false;
            last_font = g[0].font;
            prev_ink = ink_now;
            prev_w = (int)(width * 100.f);
        }
        gp_text_system_free(t);
        /* registering an instance by face index: its weight is read from the font's weight axis */
        t = fresh(false);
        gp_text_add_font_file(t, "/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf", 0, 0);                  /* default instance */
        gp_text_add_font_file(t, "/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf", 0, (7 << 16) | 0);      /* the 7th named instance: Bold */
        float wd, wb2;
        gp_glyph_pos g1[8], g2[8];
        gp_text_shape(t, 400, 24, "Hamburg", 7, g1, 8, &wd);
        gp_text_shape(t, 700, 24, "Hamburg", 7, g2, 8, &wb2);
        CHECK(g1[0].font == 0 && g2[0].font == 1 && wb2 > wd * 1.03f,
              "registered instance: 700 draws the Bold instance (font %d, not a thickened default), 400 the default (font %d) (%.2f vs %.2f)", g2[0].font, g1[0].font, wb2, wd);
        gp_text_system_free(t);
        t = fresh(true);
        CHECK(distinct, "variable font: each weight is its own instance");
        CHECK(monotone, "variable font: heavier weights have more ink and are wider");
        gp_text_system_free(t);
    }
#endif
}

/* The weight axis of a variable font at any weight, not just at its named instances: widths against FreeType set to the same axis value
 * on its own, the quantization to 10, the clamp to the axis, the default instance used when the request is its weight, ink growing. */
static void test_variable_weights(const char *path) {
    FT_Library lib;
    FT_Face face;
    if (FT_Init_FreeType(&lib) || FT_New_Face(lib, path, 0, &face)) { CHECK(false, "variable font %s opens", path); return; }
    FT_MM_Var *mm = NULL;
    int wi = -1, lo = 0, hi = 0, def = 400;
    if (FT_Get_MM_Var(face, &mm) == 0)
        for (FT_UInt i = 0; i < mm->num_axis; i++)
            if (mm->axis[i].tag == FT_MAKE_TAG('w', 'g', 'h', 't')) {
                wi = (int)i;
                lo = (int)(mm->axis[i].minimum >> 16);
                hi = (int)(mm->axis[i].maximum >> 16);
                def = (int)(mm->axis[i].def >> 16);
            }
    if (wi < 0 || hi - lo < 200) { printf("SKIP: %s has no usable weight axis\n", base_name(path)); skips++; goto done; }

    gp_text_system *t = fresh(false);
    CHECK(gp_text_add_font_file(t, path, 0, 0), "%s registers", base_name(path));
    const char *H4 = "HHHH";
    /* oracle: the advance of H with the axis at `w`, from FreeType, scaled to 24 px */
    #define ORACLE(w, out)                                                                                          \
        do {                                                                                                         \
            FT_Fixed c_[16];                                                                                         \
            FT_Get_Var_Design_Coordinates(face, mm->num_axis, c_);                                                   \
            c_[wi] = (FT_Fixed)(w) << 16;                                                                            \
            FT_Set_Var_Design_Coordinates(face, mm->num_axis, c_);                                                   \
            FT_Load_Glyph(face, FT_Get_Char_Index(face, 'H'), FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING);                \
            (out) = 4.f * (float)face->glyph->metrics.horiAdvance * 24.f / (float)face->units_per_EM;                \
        } while (0)
    int tried[] = { lo, lo + (hi - lo) / 5, 333, 450, 550, 650, hi - (hi - lo) / 6, hi };
    int prev_font = -1;
    long prev_ink = 0;
    bool ink_grows = true, widths_ok = true;
    float worst = 0;
    for (size_t k = 0; k < sizeof tried / sizeof *tried; k++) {
        int w = tried[k];
        gp_glyph_pos g[8];
        float width;
        gp_text_shape(t, w, 24, H4, 4, g, 8, &width);
        int q = (w + 5) / 10 * 10;
        q = q < lo ? lo : q > hi ? hi : q;
        float want;
        ORACLE(q, want);
        worst = fmaxf(worst, fabsf(width - want));
        if (fabsf(width - want) > 0.06f) { widths_ok = false; printf("  weight %d (axis %d): width %.3f, FreeType says %.3f\n", w, q, width, want); }
        int iw;
        long ink_now = ink(t, g[0].font, g[0].glyph, 24.f, &iw);
        if (k && ink_now <= prev_ink) ink_grows = false;
        prev_ink = ink_now;
        prev_font = g[0].font;
    }
    (void)prev_font;
    CHECK(widths_ok, "%s: the width at every weight is the axis at that weight (worst error %.3f px)", base_name(path), worst);
    CHECK(ink_grows, "%s: heavier weights have no less ink", base_name(path));

    /* quantized to 10, the default for its own weight, clamped to the axis */
    gp_glyph_pos a[8], b[8], c[8], d[8], e[8];
    gp_text_shape(t, 551, 24, H4, 4, a, 8, NULL);
    gp_text_shape(t, 554, 24, H4, 4, b, 8, NULL);
    gp_text_shape(t, 556, 24, H4, 4, c, 8, NULL);
    CHECK(a[0].font == b[0].font && b[0].font != c[0].font, "%s: weights are rounded to 10 (551, 554 share a face, 556 does not)", base_name(path));
    int nf = gp_text_font_count(t);
    for (int w = 600; w < 640; w++) gp_text_shape(t, w, 24, H4, 4, d, 8, NULL);
    CHECK(gp_text_font_count(t) - nf <= 5, "%s: sweeping 40 weights opens at most 5 faces (%d)", base_name(path), gp_text_font_count(t) - nf);
    gp_text_shape(t, def + 3, 24, H4, 4, d, 8, NULL);
    gp_text_shape(t, def, 24, H4, 4, e, 8, NULL);
    CHECK(d[0].font == e[0].font, "%s: a weight within 5 of the default instance is the default", base_name(path));
    float wh, wo;
    gp_text_shape(t, hi, 24, H4, 4, NULL, 0, &wh);
    gp_text_shape(t, 1000, 24, H4, 4, NULL, 0, &wo);
    CHECK(fabsf(wh - wo) < 0.01f, "%s: a weight past the axis is the end of it (%.3f, %.3f)", base_name(path), wh, wo);
    gp_text_shape(t, hi, 24, H4, 4, d, 8, NULL);
    gp_text_shape(t, 1000, 24, H4, 4, e, 8, NULL);
    CHECK(d[0].font == e[0].font, "%s: ...and it is the same face, not another one opened for the clamped value", base_name(path));
    gp_text_system_free(t);
    #undef ORACLE
done:
    if (mm) FT_Done_MM_Var(lib, mm);
    FT_Done_Face(face);
    FT_Done_FreeType(lib);
}

/* font-variation-settings: axes other than weight, against FreeType set to the same coordinates on its own; interning, canonical form,
 * clamping and the half-unit quantization; lists naming axes the font lacks change nothing; a list's weight wins over the request. */
static void test_variations(const char *path) {
    FT_Library lib;
    FT_Face face;
    if (FT_Init_FreeType(&lib) || FT_New_Face(lib, path, 0, &face)) { CHECK(false, "variable font %s opens", path); return; }
    FT_MM_Var *mm = NULL;
    int oi = -1, wi = -1;
    float olo = 0, ohi = 0;
    if (FT_Get_MM_Var(face, &mm) == 0)
        for (FT_UInt i = 0; i < mm->num_axis; i++) {
            if (mm->axis[i].tag == FT_MAKE_TAG('o', 'p', 's', 'z')) { oi = (int)i; olo = (float)mm->axis[i].minimum / 65536.f; ohi = (float)mm->axis[i].maximum / 65536.f; }
            if (mm->axis[i].tag == FT_MAKE_TAG('w', 'g', 'h', 't')) wi = (int)i;
        }
    if (oi < 0 || wi < 0) { printf("SKIP: %s has no opsz and wght axes\n", base_name(path)); skips++; goto done; }
    gp_text_system *t = fresh(false);
    CHECK(gp_text_add_font_file(t, path, 0, 0), "%s registers", base_name(path));
    /* interning */
    int a = gp_text_variations(t, "\"opsz\" 20"), b = gp_text_variations(t, "opsz=20"), c = gp_text_variations(t, "'opsz' 20.1"), d = gp_text_variations(t, " opsz : 20 ");
    CHECK(a > 0 && a == b && b == c && c == d, "one list in four spellings: %d %d %d %d", a, b, c, d);
    int e1 = gp_text_variations(t, "wdth 75, opsz 20"), e2 = gp_text_variations(t, "opsz 20, wdth 75"), e3 = gp_text_variations(t, "opsz 14, opsz 20, wdth 75");
    CHECK(e1 > 0 && e1 == e2 && e1 == e3 && e1 != a, "tags are sorted and the last of a repeated axis wins: %d %d %d", e1, e2, e3);
    CHECK(gp_text_variations(t, NULL) == 0 && gp_text_variations(t, "") == 0 && gp_text_variations(t, "normal") == 0 && gp_text_variations(t, "none") == 0 && gp_text_variations(t, "nonsense") == 0 && gp_text_variations(t, "opsz") == 0,
          "no list, empty, normal, none and what does not parse are id 0");
    CHECK(gp_text_variations(t, "opsz 20.4") != a && gp_text_variations(t, "opsz 20.2") == a, "values are kept to the nearest half");
    {
        int many = gp_text_variations(t, "aaa1 1, aaa2 2, aaa3 3, aaa4 4, aaa5 5, aaa6 6, aaa7 7, aaa8 8, aaa9 9, aaaa 10");
        CHECK(many > 0, "a long list is taken (the first eight axes)");
    }
    /* what a face does at the coordinates */
    #define SELV(w, list) ((int64_t)(w) | ((int64_t)gp_text_family(t, NULL) << 16) | GP_FONT_VARIATIONS(gp_text_variations(t, list)))
    const char *H4 = "HHHH";
    struct { const char *list; int weight; float opsz; float wght; } cases[] = {
        { "opsz 14", 400, 14, 400 }, { "opsz 20", 400, 20, 400 }, { "opsz 32", 400, 32, 400 }, { "opsz 32", 700, 32, 700 }, { "opsz 25.5", 300, 25.5f, 300 },
        { "opsz 100", 400, ohi, 400 }, { "opsz 5", 400, olo, 400 }, { "opsz 32, wght 200", 700, 32, 200 }, { "wght 850", 400, 14, 850 } };
    bool ok = true;
    float worst = 0;
    for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
        gp_glyph_pos g[8];
        float width;
        gp_text_shape(t, SELV(cases[k].weight, cases[k].list), 24, H4, 4, g, 8, &width);
        FT_Fixed co[16];
        FT_Get_Var_Design_Coordinates(face, mm->num_axis, co);
        co[oi] = (FT_Fixed)(cases[k].opsz * 65536.f);
        co[wi] = (FT_Fixed)cases[k].wght << 16;
        FT_Set_Var_Design_Coordinates(face, mm->num_axis, co);
        FT_Load_Glyph(face, FT_Get_Char_Index(face, 'H'), FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING);
        float want = 4.f * (float)face->glyph->metrics.horiAdvance * 24.f / (float)face->units_per_EM;
        worst = fmaxf(worst, fabsf(width - want));
        if (fabsf(width - want) > 0.08f) { ok = false; printf("  \"%s\" at weight %d: width %.3f, FreeType says %.3f\n", cases[k].list, cases[k].weight, width, want); }
    }
    CHECK(ok, "%s: widths with variation lists match the axes set in FreeType (worst %.3f px)", base_name(path), worst);
    {   /* the extremes really differ: the test is about something */
        float w14, w32;
        gp_text_shape(t, SELV(400, "opsz 14"), 24, H4, 4, NULL, 0, &w14);
        gp_text_shape(t, SELV(400, "opsz 32"), 24, H4, 4, NULL, 0, &w32);
        CHECK(fabsf(w14 - w32) > 0.5f, "opsz changes the width (%.2f vs %.2f)", w14, w32);
    }
    {   /* faces: one per distinct setting, none for a list naming no axis of the font, the default face for the default */
        gp_glyph_pos g0[8], g1[8], g2[8], g3[8], g4[8], g5[8];
        gp_text_shape(t, 400, 24, H4, 4, g0, 8, NULL);
        gp_text_shape(t, SELV(400, "wdth 75"), 24, H4, 4, g1, 8, NULL);
        gp_text_shape(t, SELV(400, "opsz 20"), 24, H4, 4, g2, 8, NULL);
        gp_text_shape(t, SELV(400, "opsz 20.2"), 24, H4, 4, g3, 8, NULL);
        gp_text_shape(t, SELV(400, "opsz 14"), 24, H4, 4, g4, 8, NULL);
        gp_text_shape(t, SELV(400, "opsz 20, wght 400"), 24, H4, 4, g5, 8, NULL);
        CHECK(g1[0].font == g0[0].font, "an axis the font does not have changes nothing, and needs no face of its own");
        CHECK(g2[0].font != g0[0].font && g2[0].font == g3[0].font, "opsz 20 and 20.2 are one face, not the default");
        CHECK(g4[0].font == g0[0].font || g4[0].font != g2[0].font, "opsz 14 (the default value) is its own setting");
        CHECK(g5[0].font != g2[0].font, "another list is another face");
        int nf = gp_text_font_count(t);
        for (int k = 0; k < 30; k++) { char l[32]; snprintf(l, sizeof l, "opsz %g", 14 + 0.5 * k); gp_text_shape(t, SELV(400, l), 24, H4, 4, g0, 8, NULL); }
        int added = gp_text_font_count(t) - nf;
        CHECK(added <= 31, "sweeping 30 half-steps of an axis opens a face each at most (%d)", added);
        nf = gp_text_font_count(t);
        for (int k = 0; k < 40; k++) { char l[32]; snprintf(l, sizeof l, "opsz %g", 14 + 0.01 * k); gp_text_shape(t, SELV(400, l), 24, H4, 4, g0, 8, NULL); }
        CHECK(gp_text_font_count(t) - nf <= 2, "40 steps of a hundredth are at most two faces (%d)", gp_text_font_count(t) - nf);
    }
    #undef SELV
    gp_text_system_free(t);
done:
    if (mm) FT_Done_MM_Var(lib, mm);
    FT_Done_Face(face);
    FT_Done_FreeType(lib);
}

int main(void) {
#ifndef GPUIC_HAVE_FONTCONFIG
    /* never write into the user's real cache from a test run */
    char cache_root[512];
    snprintf(cache_root, sizeof cache_root, "%s/gpuic-text-test-cache-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (mkdtemp(cache_root)) setenv("GPUIC_FONT_CACHE", cache_root, 1);
#endif
    ts = gp_text_system_new();
    if (!ts) { printf("FAIL: could not create the text system\n"); return 1; }

    gp_glyph_pos g[MAXG];
    shape("a", 400, 16, g, NULL);
    shape("\xe6\x97\xa5", 400, 16, g + 1, NULL);
    shape("\xf0\x9f\x98\x80", 400, 16, g + 2, NULL);
    bool have_cjk = system_has_font_for(0x65e5, false);
    bool have_emoji = system_has_font_for(0x1f600, true);
    printf("fonts: primary=%s\n       cjk=%s\n       emoji=%s\n", path_of(&g[0]), have_cjk ? path_of(&g[1]) : "(none)",
           have_emoji ? path_of(&g[2]) : "(none)");
    printf("system has: CJK font=%s, color emoji font=%s\n", have_cjk ? "yes" : "no", have_emoji ? "yes" : "no");

    test_family_ranks();
    test_latin();
    if (have_cjk) { test_cjk(); test_wrapping(); } else { skips++; printf("SKIP: no CJK font installed\n"); }
    if (have_emoji) test_emoji(); else { skips++; printf("SKIP: no color emoji font installed\n"); }
    test_missing_and_cache();
    test_subpixel();
    test_subpixel_paint();
    if (have_emoji) test_subpixel_color();
    if (system_has_font_for(0x5d0, false) && system_has_font_for(0x628, false)) test_bidi();
    else { skips++; printf("SKIP: no Hebrew/Arabic font installed\n"); }
    test_atlas_overflow();
    {
        gp_glyph_pos rg[3];
        shape("A", 400, 16, &rg[0], NULL);
        shape("A", 700, 16, &rg[1], NULL);
        char r[1024], b[1024], c[1024], e[1024];
        snprintf(r, sizeof r, "%s", path_of(&rg[0]));
        snprintf(b, sizeof b, "%s", path_of(&rg[1]));
        snprintf(c, sizeof c, "%s", have_cjk ? path_of(&g[1]) : "");
        snprintf(e, sizeof e, "%s", have_emoji ? path_of(&g[2]) : "");
        test_registration(r, b, have_cjk ? c : NULL, have_emoji ? e : NULL);
        test_synthetic_bold(r, b, have_emoji ? e : NULL);
        test_weights_and_families();
        test_system_families();
        {
            const char *vf[] = { "/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf", "/usr/share/fonts/cantarell/Cantarell-VF.otf" };
            bool any = false;
            for (size_t i = 0; i < sizeof vf / sizeof *vf; i++)
                if (exists(vf[i])) { test_variable_weights(vf[i]); test_variations(vf[i]); any = true; }
            if (!any) { skips++; printf("SKIP: no variable font installed\n"); }
        }
        test_italic();
        test_features();
        test_lcd(r, have_emoji ? e : NULL);
#ifndef GPUIC_HAVE_FONTCONFIG
        if (have_cjk) test_scan_cache(r, c);
        else { skips++; printf("SKIP: scan cache test needs a CJK font to swap in\n"); }
#endif
    }

    gp_text_system_free(ts);
#ifndef GPUIC_HAVE_FONTCONFIG
    if (getenv("GPUIC_FONT_CACHE")) { /* the cache directory made at the top */
        DIR *cd = opendir(getenv("GPUIC_FONT_CACHE"));
        for (struct dirent *de; cd && (de = readdir(cd));) {
            char p[1000];
            snprintf(p, sizeof p, "%s/%s", getenv("GPUIC_FONT_CACHE"), de->d_name);
            remove(p);
        }
        if (cd) closedir(cd);
        rmdir(getenv("GPUIC_FONT_CACHE"));
    }
#endif
    printf(failures ? "text_test: %d FAILURE(S)\n" : (skips ? "text_test: passed with %d skip(s)\n" : "text_test: all checks passed\n"),
           failures ? failures : skips);
    return failures != 0;
}
