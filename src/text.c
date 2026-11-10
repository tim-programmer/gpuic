/*
 * Text system: font discovery and fallback, shaping, glyph rasterization, glyph atlases.
 *
 *   fontconfig   finds fonts: the primary sans-serif (regular + bold) and, lazily, fallbacks
 *                for codepoints the primary lacks (CJK, symbols, emoji).
 *   itemizer     splits text into runs that share one font. Codepoints that must stay with
 *                their neighbour (ZWJ, variation selectors, combining marks, skin tones, the
 *                partner of a ZWJ) inherit the previous run's font, so sequences are not torn apart.
 *   HarfBuzz     shapes each run: kerning, ligatures and, for emoji, whole sequences (a family,
 *                a flag, a skin-toned hand) collapse into one glyph. Advances are in font units,
 *                so layout does not depend on the display scale.
 *   FreeType     rasterizes glyphs. Outline fonts go into an R8 coverage atlas (tinted by the text
 *                color); fixed-size color bitmap fonts (CBDT emoji, with a single strike) are
 *                downscaled with an area filter into an RGBA atlas and are never tinted. Other
 *                color formats (COLR/CPAL, SVG, sbix) are not handled.
 *
 *   FriBidi      resolves bidirectional text (UAX #9): embedding levels per paragraph, then rules L1
 *                and L2. Reordering happens at the level of *runs* (same font, same level), and
 *                each run is shaped in its own direction, so glyphs come out in visual order.
 *                Text with no right-to-left characters skips all of this.
 *
 * Not supported: vertical text, and bidi controls beyond what FriBidi implements.
 */
#include "internal.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H
#include FT_SFNT_NAMES_H
#include FT_MULTIPLE_MASTERS_H
#include FT_TRUETYPE_IDS_H
#include FT_LCD_FILTER_H
#ifdef GPUIC_HAVE_FONTCONFIG
#include <fontconfig/fontconfig.h>
#endif
#include <fribidi.h>
#include <hb.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>
#ifdef _WIN32
#define strcasecmp _stricmp
#define strtok_r strtok_s
#else
#include <strings.h>
#endif

#define MAX_CHAIN 8   /* fonts a family list can name */
#define SYNTHETIC_BOLD_STRENGTH (1.f / 24.f) /* of the size: FreeType's own FT_GlyphSlot_Embolden uses the same */
#define RESOLVE_CACHE 4096
#define SHAPE_CACHE 256
#define MAX_CACHED_LEN 1024

typedef struct {
    FT_Face face;
    hb_face_t *hb_face;
    hb_font_t *hb_font;
    char *path;                    /* file path, or "memory:N" for a font registered from memory */
    void *data;                    /* font file bytes when registered from memory, else NULL */
    size_t data_len;
    bool owns_data;                /* false for an embolden variant, which borrows its base's bytes */
    bool embolden;                 /* synthetic bold: a variant of font `base`, drawn and measured thicker */
    bool oblique;                  /* synthetic italic: a variant of font `base`, drawn sheared */
    int var_weight;                /* a variant of font `base` with its weight axis set to this (10, 20 ... 1000), else 0 */
    int var_id;                    /* ...and with these font variations (gp_text_variations), else 0 */
    int nax;                       /* the axes of a variable font (the first 16) */
    hb_tag_t ax_tag[16];
    float ax_min[16], ax_max[16];
    int wght_min, wght_max;        /* the range of the weight axis of a variable font, else 0 */
    int base;                      /* the font this is a variant of, -1 for a font that is not */
    int index;
    int weight;                    /* 100..900 */
    char *family;                  /* typographic family name, or what the app registered it as */
    bool italic;
    bool color;
    float upem, ascent, descent;   /* font units */
    bool bitmap_only;              /* only fixed-size strikes (e.g. CBDT emoji) */
} gp_font;

/* A shelf-packed texture atlas with a dirty rectangle for the renderer to upload. */
typedef struct {
    uint8_t *data;
    int w, h, bpp;
    int pen_x, pen_y, row_h;
    int dx0, dy0, dx1, dy1;        /* empty when dx1 <= dx0 */
    bool overflowed;
} Atlas;

typedef struct {
    uint64_t key;                  /* 0 = empty */
    gp_glyph_entry e;
} glyph_slot;

typedef struct {
    bool used;
    int style;
    int dir;
    uint32_t size_q;
    uint64_t hash;
    size_t len;
    char *text;
    gp_glyph_pos *glyphs;
    int n;
    float width;
} shape_entry;

typedef struct { gp_glyph_pos *v; int n, cap; } glyph_vec;

/* An interned OpenType feature list, parsed once. */
#define MAX_FEATURES 32
typedef struct {
    char *spec;
    hb_feature_t f[MAX_FEATURES];
    unsigned n;
} feature_entry;

/* An interned font-variation-settings list: axis tags and values (sorted by tag, values to the nearest half, so that animating one does
 * not open a face per frame). */
#define MAX_VARIATIONS 8
typedef struct {
    char *spec;                    /* canonical: "opsz=14,wdth=87.5" */
    int n;
    hb_tag_t tag[MAX_VARIATIONS];
    float val[MAX_VARIATIONS];
} variation_entry;

/* A font-family property: a comma-separated list of family names, possibly ending in a generic one. */
enum { GEN_SANS = 0, GEN_SERIF, GEN_MONO, GEN_NONE = -1 };
typedef struct {
    char *spec;
    char **names;                  /* each trimmed, unquoted */
    int *generic;                  /* GEN_* per name, or GEN_NONE for a named family */
    int n;
    int fallback_generic;          /* what a codepoint none of the names cover is searched in: the list's last generic, else sans */
    bool has_generic;
} family_entry;

/* What a (family list, weight) pair resolves to. The chain holds, in list order, the font each name
 * resolved to at this weight; `fallback` the system fonts found for codepoints the chain lacks. */
typedef struct {
    int family, weight;
    bool italic;
    int features;                  /* index into gp_text_system.features */
    int variations;                /* index into gp_text_system.variations */
    bool built;
    int chain[MAX_CHAIN];
    int nchain;
    int *fallback;
    int nfallback, cap_fallback;
} style_entry;

struct gp_text_system {
    uint32_t atlas_gen;    /* bumped by every gp_text_atlas_reset */
    FT_Library ft;
    gp_font *fonts;
    int nfonts, cap_fonts;
    family_entry *families;        /* interned family lists; id 0 is the default (empty) list */
    int nfamilies, cap_families;
    style_entry *styles;           /* one per (family list, weight) asked for; ids stay valid */
    int nstyles, cap_styles;
    feature_entry *features;       /* interned feature lists; id 0 is "none" */
    int nfeatures, cap_features;
    variation_entry *variations;   /* interned variation lists; id 0 is "none" */
    int nvariations, cap_variations;
    int *style_hash;               /* open addressing over styles: id + 1, or 0 */
    int style_hash_cap;
    struct { char *name; int generic, weight, font; } *sysmemo;   /* what the system said for a family at a weight */
    int nsysmemo, cap_sysmemo;
    int emoji;                     /* color emoji font id; -1 = not tried, -2 = unavailable */
    char *lang;                    /* fontconfig language hint for Han unification, or NULL */
    int *registered;               /* font ids the app registered, in registration order */
    int nregistered, cap_registered, nmemory;
    bool no_system;                /* never ask fontconfig / scan the system font directories */
    bool no_subpixel;              /* glyphs sit on whole pixels (gp_text_set_subpixel) */
    gp_gfx *gfx;                   /* gradients and images, owned by the app (see gfx.c) */
    int aa_mode;                   /* GP_AA_* */
    float text_gamma;              /* how text coverage is toned for blending (1: not at all; 2.2: as in linear light) */
    int system_lcd;                /* the system's subpixel order: -1 not asked yet, else 0 none / 1 RGB / 2 BGR */
#ifndef GPUIC_HAVE_FONTCONFIG
    struct scanned_font *scan;     /* every font face found in the system font directories */
    int nscan, cap_scan;
    bool scanned;
    gp_font_scan_stats scan_stats;
#endif

    struct { uint32_t cp; int font; int style; bool emoji, valid; } resolve[RESOLVE_CACHE];

    Atlas alpha, color, lcd;
    glyph_slot *glyphs;
    size_t glyph_cap, glyph_count;

    hb_buffer_t *hbuf;
    shape_entry shape_cache[SHAPE_CACHE];
    glyph_vec gv;                  /* scratch for shaping */
    /* per-codepoint scratch for shaping one string */
    uint32_t *cps;
    size_t *boff;                  /* byte offset of each codepoint; boff[ncp] = len */
    int *cpfont;
    FriBidiCharType *btypes;
    FriBidiBracketType *bbrackets;
    FriBidiLevel *blevels;
    FriBidiStrIndex *bmap;
    size_t cap_cps;
};

/* ---------------------------------------------------------------- atlases */

static void atlas_init(Atlas *a, int w, int h, int bpp) {
    a->w = w;
    a->h = h;
    a->bpp = bpp;
    a->data = calloc(1, (size_t)w * h * bpp);
    a->dx0 = a->dy0 = 0;
    a->dx1 = w;
    a->dy1 = h; /* the first upload covers the whole (empty) atlas */
}

static void atlas_mark_dirty(Atlas *a, int x, int y, int w, int h) {
    if (a->dx1 <= a->dx0) { a->dx0 = x; a->dy0 = y; a->dx1 = x + w; a->dy1 = y + h; return; }
    if (x < a->dx0) a->dx0 = x;
    if (y < a->dy0) a->dy0 = y;
    if (x + w > a->dx1) a->dx1 = x + w;
    if (y + h > a->dy1) a->dy1 = y + h;
}

/* Reserves a w x h block with a 1px gutter. */
static bool atlas_alloc(Atlas *a, int w, int h, int *x, int *y) {
    if (w + 1 > a->w || h + 1 > a->h) { a->overflowed = true; return false; }
    if (a->pen_x + w + 1 > a->w) {
        a->pen_x = 0;
        a->pen_y += a->row_h + 1;
        a->row_h = 0;
    }
    if (a->pen_y + h + 1 > a->h) { a->overflowed = true; return false; }
    *x = a->pen_x;
    *y = a->pen_y;
    a->pen_x += w + 1;
    if (h > a->row_h) a->row_h = h;
    return true;
}

static void atlas_reset(Atlas *a) {
    memset(a->data, 0, (size_t)a->w * a->h * a->bpp);
    a->pen_x = a->pen_y = a->row_h = 0;
    a->overflowed = false;
    a->dx0 = a->dy0 = 0;
    a->dx1 = a->w;
    a->dy1 = a->h;
}

static bool atlas_take_dirty(Atlas *a, int *x, int *y, int *w, int *h) {
    if (a->dx1 <= a->dx0) return false;
    *x = a->dx0; *y = a->dy0; *w = a->dx1 - a->dx0; *h = a->dy1 - a->dy0;
    a->dx0 = a->dy0 = a->dx1 = a->dy1 = 0;
    return true;
}

/* ---------------------------------------------------------------- fonts */

/* CSS-style weight: the OS/2 table's usWeightClass when it has one, else regular or bold by style flag. */
static int face_weight(FT_Face face) {
    if (FT_HAS_MULTIPLE_MASTERS(face)) {   /* a variable font: its weight axis at the instance this face is (OS/2 only knows the default) */
        FT_MM_Var *mm;
        FT_Fixed coords[16];
        if (FT_Get_MM_Var(face, &mm) == 0) {
            int w = 0;
            if (mm->num_axis <= 16 && FT_Get_Var_Design_Coordinates(face, mm->num_axis, coords) == 0)
                for (FT_UInt i = 0; i < mm->num_axis; i++)
                    if (mm->axis[i].tag == FT_MAKE_TAG('w', 'g', 'h', 't')) w = (int)((coords[i] + 0x8000) >> 16);
            FT_Done_MM_Var(face->glyph->library, mm);
            if (w >= 100 && w <= 900) return w;
        }
    }
    TT_OS2 *os2 = FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
    if (os2 && os2->usWeightClass >= 100 && os2->usWeightClass <= 900) return os2->usWeightClass;
    return face->style_flags & FT_STYLE_FLAG_BOLD ? 700 : 400;
}

/* The family a face belongs to for matching by name. The typographic family (name ID 16) when the font has
 * one, because the legacy one (ID 1) folds the weight in ("Noto Sans Light"), and the weight is what
 * the request asks for separately. Returns a heap string. */
static char *face_family(FT_Face face) {
    FT_UInt n = FT_Get_Sfnt_Name_Count(face);
    for (FT_UInt i = 0; i < n; i++) {
        FT_SfntName nm;
        if (FT_Get_Sfnt_Name(face, i, &nm) != 0 || nm.name_id != TT_NAME_ID_TYPOGRAPHIC_FAMILY || !nm.string_len) continue;
        bool utf16 = nm.platform_id == TT_PLATFORM_MICROSOFT && nm.encoding_id <= TT_MS_ID_UNICODE_CS;
        if (!utf16 && nm.platform_id != TT_PLATFORM_MACINTOSH) continue;
        char *out = malloc((size_t)nm.string_len * 2 + 4);   /* a UTF-16 unit is at most three bytes of UTF-8 */
        size_t k = 0;
        if (utf16) {
            for (FT_UInt j = 0; j + 1 < nm.string_len; j += 2) {
                uint32_t cp = (uint32_t)(nm.string[j] << 8 | nm.string[j + 1]);
                if (cp >= 0xd800 && cp < 0xdc00 && j + 3 < nm.string_len) {
                    uint32_t lo = (uint32_t)(nm.string[j + 2] << 8 | nm.string[j + 3]);
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    j += 2;
                }
                if (cp < 0x80) {
                    out[k++] = (char)cp;
                } else if (cp < 0x800) {
                    out[k++] = (char)(0xc0 | cp >> 6);
                    out[k++] = (char)(0x80 | (cp & 63));
                } else if (cp < 0x10000) {
                    out[k++] = (char)(0xe0 | cp >> 12);
                    out[k++] = (char)(0x80 | (cp >> 6 & 63));
                    out[k++] = (char)(0x80 | (cp & 63));
                } else {
                    out[k++] = (char)(0xf0 | cp >> 18);
                    out[k++] = (char)(0x80 | (cp >> 12 & 63));
                    out[k++] = (char)(0x80 | (cp >> 6 & 63));
                    out[k++] = (char)(0x80 | (cp & 63));
                }
            }
        } else {   /* Mac Roman: ASCII is what family names are in practice */
            for (FT_UInt j = 0; j < nm.string_len; j++)
                if (nm.string[j] < 0x80) out[k++] = (char)nm.string[j];
        }
        out[k] = 0;
        if (k) return out;
        free(out);
    }
    return strdup(face->family_name ? face->family_name : "");
}

/* The axes of a variable font: their tags and ranges (the first 16), and the range of the weight axis in CSS units (0, 0 if it has none). */
static void read_axes(FT_Library lib, FT_Face face, gp_font *f) {
    f->nax = 0;
    f->wght_min = f->wght_max = 0;
    if (!FT_HAS_MULTIPLE_MASTERS(face)) return;
    FT_MM_Var *mm;
    if (FT_Get_MM_Var(face, &mm) != 0) return;
    for (FT_UInt i = 0; i < mm->num_axis && i < 16; i++) {
        f->ax_tag[f->nax] = (hb_tag_t)mm->axis[i].tag;
        f->ax_min[f->nax] = (float)mm->axis[i].minimum / 65536.f;
        f->ax_max[f->nax] = (float)mm->axis[i].maximum / 65536.f;
        f->nax++;
        if (mm->axis[i].tag == FT_MAKE_TAG('w', 'g', 'h', 't')) {
            f->wght_min = (int)((mm->axis[i].minimum + 0x8000) >> 16);
            f->wght_max = (int)((mm->axis[i].maximum + 0x8000) >> 16);
        }
    }
    FT_Done_MM_Var(lib, mm);
}

/* Moves axes of a face and of its HarfBuzz font (outlines and advances): the weight axis to `weight` (0: leave it) and the axes a
 * variation list names to its values (clamped to each axis's range), leaving the rest where they are (a named instance keeps its
 * width...). HarfBuzz takes all the axes at once, so every one is passed. False if nothing was set. */
static bool set_axes(FT_Library lib, FT_Face face, hb_font_t *hb, int weight, const variation_entry *vl) {
    FT_MM_Var *mm;
    if (!FT_HAS_MULTIPLE_MASTERS(face) || FT_Get_MM_Var(face, &mm) != 0) return false;
    FT_Fixed coords[16];
    hb_variation_t vars[16];
    bool ok = false;
    if (mm->num_axis <= 16 && FT_Get_Var_Design_Coordinates(face, mm->num_axis, coords) == 0) {
        for (FT_UInt i = 0; i < mm->num_axis; i++) {
            if (weight && mm->axis[i].tag == FT_MAKE_TAG('w', 'g', 'h', 't')) { coords[i] = (FT_Fixed)weight << 16; ok = true; }
            for (int k = 0; vl && k < vl->n; k++)
                if (vl->tag[k] == (hb_tag_t)mm->axis[i].tag) {
                    double v = vl->val[k];
                    double lo = (double)mm->axis[i].minimum / 65536.0, hi = (double)mm->axis[i].maximum / 65536.0;
                    coords[i] = (FT_Fixed)((v < lo ? lo : v > hi ? hi : v) * 65536.0);
                    ok = true;
                }
        }
        ok = ok && FT_Set_Var_Design_Coordinates(face, mm->num_axis, coords) == 0;
        if (ok) {
            for (FT_UInt i = 0; i < mm->num_axis; i++) vars[i] = (hb_variation_t){ (hb_tag_t)mm->axis[i].tag, (float)coords[i] / 65536.f };
            hb_font_set_variations(hb, vars, mm->num_axis);
        }
    }
    FT_Done_MM_Var(lib, mm);
    return ok;
}

/* Opens a font from a file (`data` NULL) or from memory. `data` is a heap copy whose ownership
 * passes to the font on success; on failure the caller still owns it. */
static int font_open(gp_text_system *ts, const char *path, int index, void *data, size_t len, bool owns_data,
                     bool embolden, bool oblique, int var_weight, int var_id, int base) {
    FT_Face face;
    hb_blob_t *blob;
    if (data) {
        if (FT_New_Memory_Face(ts->ft, data, (FT_Long)len, index, &face) != 0) return -1;
        blob = hb_blob_create(data, (unsigned)len, HB_MEMORY_MODE_READONLY, NULL, NULL);
    } else {
        if (FT_New_Face(ts->ft, path, index, &face) != 0) return -1;
        blob = hb_blob_create_from_file_or_fail(path);
    }
    if (!blob) { FT_Done_Face(face); return -1; }
    hb_face_t *hb_face = hb_face_create(blob, (unsigned)index);
    hb_blob_destroy(blob);

    if (ts->nfonts == ts->cap_fonts) {
        ts->cap_fonts = ts->cap_fonts ? ts->cap_fonts * 2 : 8;
        ts->fonts = realloc(ts->fonts, (size_t)ts->cap_fonts * sizeof *ts->fonts);
    }
    gp_font *f = &ts->fonts[ts->nfonts];
    f->face = face;
    f->hb_face = hb_face;
    f->hb_font = hb_font_create(hb_face); /* scale defaults to units-per-em: advances in font units */
    f->path = strdup(path);
    f->data = data;
    f->data_len = len;
    f->owns_data = data && owns_data;
    f->embolden = embolden;
    f->oblique = oblique;
    f->var_weight = 0;
    f->wght_min = f->wght_max = 0;
    f->base = base;
    f->index = index;
    f->family = face_family(face);
    f->italic = (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
    f->color = FT_HAS_COLOR(face) != 0;
    if (index >> 16) hb_font_set_var_named_instance(f->hb_font, (unsigned)(index >> 16) - 1);   /* an instance of a variable font */
    read_axes(ts->ft, face, f);
    f->var_id = 0;
    if ((var_weight || var_id) && set_axes(ts->ft, face, f->hb_font, var_weight, var_id ? &ts->variations[var_id] : NULL)) {
        f->var_weight = var_weight;
        f->var_id = var_id;
    }
    f->weight = face_weight(face);   /* (after the axis was set: a variable font's weight is where its axis is) */
    unsigned upem = hb_face_get_upem(hb_face);
    f->upem = upem ? (float)upem : 1000.f;
    f->ascent = (float)face->ascender;
    f->descent = (float)-face->descender;
    f->bitmap_only = !FT_IS_SCALABLE(face) && face->num_fixed_sizes > 0;
    if (f->ascent <= 0 && f->descent <= 0) { f->ascent = f->upem * 0.8f; f->descent = f->upem * 0.2f; }
    return ts->nfonts++;
}

static int font_add_ex(gp_text_system *ts, const char *path, int index, void *data, size_t len) {
    if (!data)
        for (int i = 0; i < ts->nfonts; i++)
            if (ts->fonts[i].base < 0 && ts->fonts[i].index == index && strcmp(ts->fonts[i].path, path) == 0) return i;
    return font_open(ts, path, index, data, len, true, false, false, 0, 0, -1);
}

/* Synthetic bold and italic. A bold request (600 and up) that resolved to a font lighter than that (a family
 * with no bold face, or a single registered font), and an italic request that resolved to an upright one, are
 * drawn with a variant of the font: a second face over the same file or bytes. Bold glyphs are thickened when
 * rasterized and their advances widened to match; oblique glyphs are sheared (advances are not touched).
 * Variants are only ever returned from resolve_font, never searched for glyphs. */
#define SYNTHETIC_BOLD_BELOW 600
#define SYNTHETIC_OBLIQUE_SHEAR 0.25f   /* x += 0.25 y: 14 degrees, what browsers use */
static int font_variant(gp_text_system *ts, int id, int weight, bool want_italic, int vid) {
    gp_font *f = &ts->fonts[id];
    if (f->color || f->bitmap_only) return id;
    int root = f->base >= 0 ? f->base : id;
    const gp_font *r = &ts->fonts[root];
    /* a variable font is set to the weight asked for (to the nearest 10, so that an animation does not open a face per frame), within
     * its axis; only a font that cannot reach a bold weight is thickened */
    int var = 0, have = r->weight;
    if (r->wght_max > r->wght_min && weight > 0) {
        int want = (weight + 5) / 10 * 10;
        want = want < r->wght_min ? r->wght_min : want > r->wght_max ? r->wght_max : want;
        if (abs(want - r->weight) >= 5) { var = want; have = want; }
    }
    /* font-variation-settings: only the axes this font has count (a list naming none of them changes nothing, so it needs no face of its
     * own). A weight it sets overrides the request's without making the face lighter than bold for the purpose of synthetic bold: the
     * face covers the weights of its axis, as it does in CSS */
    if (vid > 0 && vid < ts->nvariations) {
        const variation_entry *vl = &ts->variations[vid];
        bool applies = false;
        for (int k = 0; k < vl->n; k++)
            for (int a = 0; a < r->nax; a++)
                if (vl->tag[k] == r->ax_tag[a]) applies = true;
        if (!applies) vid = 0;
    } else {
        vid = 0;
    }
    bool bold = weight >= SYNTHETIC_BOLD_BELOW && have < SYNTHETIC_BOLD_BELOW, slant = want_italic && !r->italic;
    if (!bold && !slant && !var && !vid) return root;
    for (int i = 0; i < ts->nfonts; i++)
        if (ts->fonts[i].base == root && ts->fonts[i].embolden == bold && ts->fonts[i].oblique == slant && ts->fonts[i].var_weight == var &&
            ts->fonts[i].var_id == vid)
            return i;
    int v = font_open(ts, r->path, r->index, r->data, r->data_len, false, bold, slant, var, vid, root);
    return v >= 0 ? v : root; /* could not reopen: plain is better than nothing */
}

static int font_add(gp_text_system *ts, const char *path, int index) {
    return font_add_ex(ts, path, index, NULL, 0);
}

/* Which generic family a name is, or GEN_NONE. */
static int generic_of(const char *name) {
    if (strcasecmp(name, "sans-serif") == 0 || strcasecmp(name, "system-ui") == 0) return GEN_SANS;
    if (strcasecmp(name, "serif") == 0) return GEN_SERIF;
    if (strcasecmp(name, "monospace") == 0) return GEN_MONO;
    return GEN_NONE;
}

/* CSS font matching (Fonts 4, 5.2): how good a face of weight `have` is for a request of `want`; smaller
 * is better. Within 400..500 a request looks upwards to 500 first, then downwards, then past 500;
 * below 400 it looks lighter first, above 500 heavier first. */
static int weight_rank(int want, int have) {
    if (want >= 400 && want <= 500) {
        if (have >= want && have <= 500) return have - want;
        if (have < want) return 1000 + (want - have);
        return 2000 + (have - 500);
    }
    if (want < 400) return have <= want ? want - have : 1000 + (have - want);
    return have >= want ? have - want : 1000 + (want - have);
}

/* The weight the draw code will use for a request: 1..1000, 0 meaning the default. */
static int clamp_weight(int w) { return w <= 0 ? 400 : w > 1000 ? 1000 : w; }

/* A request for a face: its weight, with the italic bit (GP_FONT_ITALIC) set when italic is asked for. */
#define REQ_ITALIC GP_FONT_ITALIC
static int req_weight(int req) { return clamp_weight(req & 0x7fff); }
static bool req_italic(int req) { return (req & REQ_ITALIC) != 0; }

#ifdef GPUIC_HAVE_FONTCONFIG
static const char *const generic_names[] = { "sans-serif", "serif", "monospace" };

/* Asks fontconfig for a font. `name` is a family name, or NULL for the generic family `generic`. With
 * `cp` the font must cover it (fallback search); with a name and cp 0 it must be that family, since
 * fontconfig always returns something. */
static int fc_find(gp_text_system *ts, const char *name, int generic, int req, uint32_t cp, bool color) {
    int weight = req_weight(req);
    FcPattern *pat = FcPatternCreate();
    if (!color) FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)(name ? name : generic_names[generic]));
    FcPatternAddInteger(pat, FC_WEIGHT, FcWeightFromOpenType(weight));
    FcPatternAddInteger(pat, FC_SLANT, req_italic(req) ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
    if (cp) {
        FcCharSet *cs = FcCharSetCreate();
        FcCharSetAddChar(cs, cp);
        FcPatternAddCharSet(pat, FC_CHARSET, cs);
        FcCharSetDestroy(cs);
    }
    if (color) FcPatternAddBool(pat, FC_COLOR, FcTrue);
    if (ts->lang) FcPatternAddString(pat, FC_LANG, (const FcChar8 *)ts->lang);
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res;
    FcPattern *match = FcFontMatch(NULL, pat, &res);
    FcPatternDestroy(pat);
    if (!match) return -1;
    if (name && !color) {   /* an alias for something else is not the family that was asked for */
        bool same = false;
        FcChar8 *fam;
        for (int i = 0; !same && FcPatternGetString(match, FC_FAMILY, i, &fam) == FcResultMatch; i++)
            same = strcasecmp((const char *)fam, name) == 0;
        if (!same) { FcPatternDestroy(match); return -1; }
    }
    FcChar8 *file = NULL;
    int index = 0;
    FcPatternGetString(match, FC_FILE, 0, &file);
    FcPatternGetInteger(match, FC_INDEX, 0, &index);
    int id = file ? font_add(ts, (const char *)file, index) : -1;
    int fcw;
    if (id >= 0 && (index >> 16) && FcPatternGetInteger(match, FC_WEIGHT, 0, &fcw) == FcResultMatch)   /* the OS/2 weight of a variable font's instance is its default's */
        ts->fonts[id].weight = FcWeightToOpenType(fcw);
    FcPatternDestroy(match);
    return id;
}
#else
/*
 * Without fontconfig the system font directories are scanned once, and a font
 * for a codepoint is the best-ranked scanned font whose cmap covers it. There
 * is no language matching, so Han unification picks whichever CJK font ranks first.
 */
typedef struct scanned_font {
    char *path;
    char *family;           /* kept so rankings follow the preference list, even for faces read back from the cache */
    int index, rank, weight;
    bool color, italic, fixed;
} scanned_font;

/* Families that make a good default UI face, best first; CJK families last so they only win
 * for characters the Latin ones lack. */
static const char *const preferred_families[] = {
    "segoe ui", "sf pro", "helvetica neue", "noto sans", "dejavu sans", "liberation sans", "arial",
    "roboto", "ubuntu", "cantarell", "open sans", "helvetica", "verdana", "tahoma",
    "source han sans", "pingfang", "hiragino", "yu gothic", "meiryo", "microsoft yahei", "malgun",
    "wenquanyi", "droid sans fallback",
};
#define RANK_UNKNOWN 1000   /* families not in the list; also the group that is never weight-refined */
#define RANK_PENALTY 5000

static bool has_font_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return false;
    static const char *const exts[] = {".ttf", ".otf", ".ttc", ".otc"};
    for (size_t i = 0; i < sizeof exts / sizeof *exts; i++)
        if (strcasecmp(dot, exts[i]) == 0) return true;
    return false;
}

static void lower_into(char *low, size_t cap, const char *family) {
    size_t n = 0;
    for (; family && family[n] && n + 1 < cap; n++) low[n] = (char)tolower((unsigned char)family[n]);
    low[n] = 0;
}

static int family_rank(const char *family) {
    char low[128];
    lower_into(low, sizeof low, family);
    for (size_t i = 0; i < sizeof preferred_families / sizeof *preferred_families; i++)
        if (strstr(low, preferred_families[i])) return (int)i;
    return RANK_UNKNOWN;
}

static bool looks_monospaced(const char *family) {
    char low[128];
    lower_into(low, sizeof low, family);
    return strstr(low, "mono") || strstr(low, "code") || strstr(low, "coding") || strstr(low, "nerd");
}

static void rank_face(scanned_font *f) {
    f->rank = family_rank(f->family);
    if (f->fixed || looks_monospaced(f->family)) f->rank += RANK_PENALTY;
}

static scanned_font *scan_push(gp_text_system *ts, const char *path, int index, const char *family, int weight,
                               bool color, bool italic, bool fixed) {
    if (ts->nscan == ts->cap_scan) {
        ts->cap_scan = ts->cap_scan ? ts->cap_scan * 2 : 128;
        ts->scan = realloc(ts->scan, (size_t)ts->cap_scan * sizeof *ts->scan);
    }
    scanned_font *f = &ts->scan[ts->nscan++];
    f->path = strdup(path);
    f->family = strdup(family ? family : "");
    for (char *c = f->family; *c; c++) /* the cache is line and tab separated */
        if ((unsigned char)*c < 0x20) *c = ' ';
    f->index = index;
    f->weight = weight;
    f->color = color;
    f->italic = italic;
    f->fixed = fixed;
    rank_face(f);
    return f;
}

/* ---- the scan cache
 *
 * Opening every font file with FreeType is what makes a scan slow (~0.3 s for 3,000 files), so what
 * it learned is remembered in one text file per set of scanned directories. The walk itself still
 * runs every time, because it is cheap (directory listing and stat), and decides what to trust:
 * a file whose size and modification time are unchanged is read back from the cache, anything
 * else is opened again. Files that vanished simply drop out. The file ends with a checksum, so a
 * truncated or damaged cache is ignored and rebuilt; any I/O failure just means no caching. */

typedef struct { char *family; int index, weight; unsigned flags; } cached_face;   /* flags: 1 color, 2 italic, 4 fixed */
typedef struct { char *path; int64_t size, mtime; int first, count; } cached_file;
typedef struct { char *path; int64_t size, mtime; int first, count; bool cacheable; } seen_file;

typedef struct {
    gp_text_system *ts;
    char *buf;                      /* the cache file's contents; cached_* point into it */
    cached_file *files;
    int nfiles;
    cached_face *faces;
    int nfaces;
    int used;                       /* cached files that were still present and unchanged */
    seen_file *seen;
    int nseen, cap_seen;
    bool changed;                   /* what was learned differs from the cache file */
} scan_ctx;

#define CACHE_MAGIC "gpuic-font-cache\t2"   /* 2: families are the typographic ones */

static uint64_t fnv64(uint64_t h, const void *data, size_t n) {
    const unsigned char *p = data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
#define FNV_INIT 14695981039346656037ull

/* Where this set of directories keeps its cache, or NULL when caching is off. GPUIC_FONT_CACHE=0
 * turns it off and GPUIC_FONT_CACHE=<dir> chooses the directory. */
static bool cache_location(const char *dirs, char *dir, size_t dir_cap, char *file, size_t file_cap) {
    const char *env = getenv("GPUIC_FONT_CACHE");
    if (env && (strcmp(env, "0") == 0 || strcasecmp(env, "off") == 0)) return false;
    if (env && *env) {
        snprintf(dir, dir_cap, "%s", env);
    } else {
        const char *home = getenv("HOME");
#if defined(_WIN32)
        const char *base = getenv("LOCALAPPDATA");
        if (!base) return false;
        snprintf(dir, dir_cap, "%s\\gpuic", base);
        (void)home;
#elif defined(__APPLE__)
        if (!home) return false;
        snprintf(dir, dir_cap, "%s/Library/Caches/gpuic", home);
#else
        const char *xdg = getenv("XDG_CACHE_HOME");
        if (xdg && *xdg) snprintf(dir, dir_cap, "%s/gpuic", xdg);
        else if (home) snprintf(dir, dir_cap, "%s/.cache/gpuic", home);
        else return false;
#endif
    }
    snprintf(file, file_cap, "%s/fonts-%016llx.txt", dir, (unsigned long long)fnv64(FNV_INIT, dirs, strlen(dirs)));
    return true;
}

static char *next_field(char **p) {
    char *start = *p, *tab = strchr(start, '\t');
    if (tab) { *tab = 0; *p = tab + 1; } else { *p = start + strlen(start); }
    return start;
}

static bool parse_int(const char *s, long long *out) {
    char *end;
    if (!*s) return false;
    *out = strtoll(s, &end, 10);
    return *end == 0;
}

static int cached_cmp(const void *a, const void *b) { return strcmp(((const cached_file *)a)->path, ((const cached_file *)b)->path); }

/* Reads and verifies the cache file. On any doubt the cache is dropped as a whole. */
static void cache_load(scan_ctx *c, const char *file) {
    FILE *f = fopen(file, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > (64l << 20)) { fclose(f); return; }
    c->buf = malloc((size_t)size + 1);
    if (!c->buf || fread(c->buf, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(c->buf); c->buf = NULL; return; }
    fclose(f);
    c->buf[size] = 0;

    /* the last line is "end <files> <checksum of everything before it>" */
    char *end = c->buf + size - 1;
    if (*end != '\n') goto bad;
    char *last = end;
    while (last > c->buf && last[-1] != '\n') last--;
    unsigned long long want_files, want_sum;
    if (sscanf(last, "end\t%llu\t%llx", &want_files, &want_sum) != 2) goto bad;
    if (fnv64(FNV_INIT, c->buf, (size_t)(last - c->buf)) != want_sum) goto bad;
    *last = 0;

    char *p = c->buf, *line_end;
    if (!(line_end = strchr(p, '\n')) || (*line_end = 0, strcmp(p, CACHE_MAGIC) != 0)) goto bad;
    p = line_end + 1;
    int cap_f = 0, cap_g = 0;
    while (*p) {
        line_end = strchr(p, '\n');
        if (!line_end) goto bad;
        *line_end = 0;
        char *q = p;
        char *kind = next_field(&q);
        if (strcmp(kind, "F") != 0) goto bad;
        char *path = next_field(&q);
        long long fsize, mtime, count;
        if (!parse_int(next_field(&q), &fsize) || !parse_int(next_field(&q), &mtime) || !parse_int(next_field(&q), &count) ||
            count < 0 || count > 64 || !*path)
            goto bad;
        if (c->nfiles == cap_f) {
            cap_f = cap_f ? cap_f * 2 : 256;
            c->files = realloc(c->files, (size_t)cap_f * sizeof *c->files);
        }
        c->files[c->nfiles++] = (cached_file){ path, fsize, mtime, c->nfaces, (int)count };
        p = line_end + 1;
        for (long long i = 0; i < count; i++) {
            line_end = strchr(p, '\n');
            if (!line_end) goto bad;
            *line_end = 0;
            q = p;
            if (strcmp(next_field(&q), "f") != 0) goto bad;
            long long index, weight, flags;
            if (!parse_int(next_field(&q), &index) || !parse_int(next_field(&q), &weight) || !parse_int(next_field(&q), &flags) ||
                index < 0 || weight < 100 || weight > 900 || flags < 0 || flags > 7)
                goto bad;
            if (c->nfaces == cap_g) {
                cap_g = cap_g ? cap_g * 2 : 512;
                c->faces = realloc(c->faces, (size_t)cap_g * sizeof *c->faces);
            }
            c->faces[c->nfaces++] = (cached_face){ q, (int)index, (int)weight, (unsigned)flags };
            p = line_end + 1;
        }
    }
    if ((unsigned long long)c->nfiles != want_files) goto bad;
    qsort(c->files, (size_t)c->nfiles, sizeof *c->files, cached_cmp);
    return;
bad:
    free(c->buf);
    free(c->files);
    free(c->faces);
    c->buf = NULL;
    c->files = NULL;
    c->faces = NULL;
    c->nfiles = c->nfaces = 0;
}

/* Writes what this scan learned, atomically: a temporary file, then a rename over the old cache. */
static void cache_write(scan_ctx *c, const char *dir, const char *file) {
    gp_text_system *ts = c->ts;
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.%llx.tmp", file, (unsigned long long)gp_now_ns());
    gp_platform_mkdirs(dir);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    uint64_t sum = FNV_INIT;
    bool ok = true;
#define PUT(...)                                                                          \
    do {                                                                                  \
        char line_[1400];                                                                 \
        int n_ = snprintf(line_, sizeof line_, __VA_ARGS__);                              \
        if (n_ < 0 || n_ >= (int)sizeof line_) { ok = false; break; }                     \
        sum = fnv64(sum, line_, (size_t)n_);                                              \
        ok &= fwrite(line_, 1, (size_t)n_, f) == (size_t)n_;                              \
    } while (0)
    PUT(CACHE_MAGIC "\n");
    int nfiles = 0;
    for (int i = 0; ok && i < c->nseen; i++) {
        seen_file *sf = &c->seen[i];
        if (!sf->cacheable) continue;
        PUT("F\t%s\t%lld\t%lld\t%d\n", sf->path, (long long)sf->size, (long long)sf->mtime, sf->count);
        for (int k = 0; ok && k < sf->count; k++) {
            scanned_font *g = &ts->scan[sf->first + k];
            PUT("f\t%d\t%d\t%u\t%s\n", g->index, g->weight, (g->color ? 1u : 0u) | (g->italic ? 2u : 0u) | (g->fixed ? 4u : 0u), g->family);
        }
        nfiles++;
    }
    if (ok) {
        char last[64];
        int n = snprintf(last, sizeof last, "end\t%d\t%016llx\n", nfiles, (unsigned long long)sum);
        ok = fwrite(last, 1, (size_t)n, f) == (size_t)n;
    }
#undef PUT
    ok = fclose(f) == 0 && ok;
    if (!ok || !gp_platform_rename(tmp, file)) {
        remove(tmp);
        return;
    }
    ts->scan_stats.cache_written = true;
}

static void scan_open(scan_ctx *c, const char *path) {
    gp_text_system *ts = c->ts;
    ts->scan_stats.opened++;
    FT_Face face;
    if (FT_New_Face(ts->ft, path, -1, &face) != 0) return;
    int count = (int)face->num_faces;
    FT_Done_Face(face);
    if (count > 64) count = 64;
    for (int i = 0; i < count; i++) {
        if (FT_New_Face(ts->ft, path, i, &face) != 0) continue;
        if (FT_IS_SCALABLE(face) || FT_HAS_COLOR(face)) {
            char *fam = face_family(face);
            scan_push(ts, path, i, fam, face_weight(face), FT_HAS_COLOR(face) != 0,
                      (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0, FT_IS_FIXED_WIDTH(face) != 0);
            free(fam);
        }
        FT_Done_Face(face);
    }
}

static void scan_file(const char *path, int64_t size, int64_t mtime, void *user) {
    scan_ctx *c = user;
    gp_text_system *ts = c->ts;
    if (!has_font_ext(path)) return;
    ts->scan_stats.files++;
    int first = ts->nscan;

    cached_file key = { .path = (char *)path }, *hit = NULL;
    if (c->nfiles) hit = bsearch(&key, c->files, (size_t)c->nfiles, sizeof *c->files, cached_cmp);
    if (hit && hit->size == size && hit->mtime == mtime) {
        for (int k = 0; k < hit->count; k++) {
            cached_face *g = &c->faces[hit->first + k];
            scan_push(ts, path, g->index, g->family, g->weight, g->flags & 1, g->flags & 2, g->flags & 4);
        }
        c->used++;
        ts->scan_stats.from_cache++;
    } else {
        scan_open(c, path);
        c->changed = true;
    }

    if (c->nseen == c->cap_seen) {
        c->cap_seen = c->cap_seen ? c->cap_seen * 2 : 256;
        c->seen = realloc(c->seen, (size_t)c->cap_seen * sizeof *c->seen);
    }
    c->seen[c->nseen++] = (seen_file){ strdup(path), size, mtime, first, ts->nscan - first,
                                       !strchr(path, '\t') && !strchr(path, '\n') };
}

static int scan_cmp(const void *a, const void *b) {
    const scanned_font *x = a, *y = b;
    if (x->rank != y->rank) return x->rank - y->rank;
    int c = strcmp(x->path, y->path);
    return c ? c : x->index - y->index;
}

static void dirs_append(char *dirs, size_t cap, const char *more) {
#ifdef _WIN32
    const char *sep = ";";
#else
    const char *sep = ":";
#endif
    if (!more || !*more) return;
    size_t n = strlen(dirs);
    snprintf(dirs + n, cap - n, "%s%s", n ? sep : "", more);
}

static void scan_fonts(gp_text_system *ts) {
    uint64_t t0 = gp_now_ns();
    ts->scanned = true;

    char dirs[4096] = "", buf[1024];
    dirs_append(dirs, sizeof dirs, getenv("GPUIC_FONT_PATH"));
    const char *home = getenv("HOME");
#if defined(_WIN32)
    const char *win = getenv("WINDIR");
    snprintf(buf, sizeof buf, "%s\\Fonts", win ? win : "C:\\Windows");
    dirs_append(dirs, sizeof dirs, buf);
#elif defined(__APPLE__)
    dirs_append(dirs, sizeof dirs, "/System/Library/Fonts:/Library/Fonts");
    if (home) { snprintf(buf, sizeof buf, "%s/Library/Fonts", home); dirs_append(dirs, sizeof dirs, buf); }
#else
    dirs_append(dirs, sizeof dirs, "/usr/share/fonts:/usr/local/share/fonts");
    if (home) {
        snprintf(buf, sizeof buf, "%s/.local/share/fonts:%s/.fonts", home, home);
        dirs_append(dirs, sizeof dirs, buf);
    }
#endif

    scan_ctx c = { .ts = ts };
    char cache_dir[1024], cache_file[1100];
    bool caching = cache_location(dirs, cache_dir, sizeof cache_dir, cache_file, sizeof cache_file);
    if (caching) cache_load(&c, cache_file);
    ts->scan_stats.cache_loaded = c.nfiles > 0;
    if (!c.buf) c.changed = true; /* nothing usable on disk: write one */

    char *copy = strdup(dirs), *save = NULL;
#ifdef _WIN32
    const char *sep = ";";
#else
    const char *sep = ":";
#endif
    for (char *d = strtok_r(copy, sep, &save); d; d = strtok_r(NULL, sep, &save)) gp_platform_walk_dir(d, scan_file, &c);
    free(copy);

    if (c.used != c.nfiles) c.changed = true; /* cached files that are gone */
    if (caching && c.changed) cache_write(&c, cache_dir, cache_file);

    for (int i = 0; i < c.nseen; i++) free(c.seen[i].path);
    free(c.seen);
    free(c.files);
    free(c.faces);
    free(c.buf);
    if (ts->nscan) qsort(ts->scan, (size_t)ts->nscan, sizeof *ts->scan, scan_cmp);
    ts->scan_stats.faces = ts->nscan;
    ts->scan_stats.ms = (double)(gp_now_ns() - t0) / 1e6;
}

typedef struct { long long key; int i; } scan_cand;
static int cand_cmp(const void *a, const void *b) {
    const scan_cand *x = a, *y = b;
    return x->key < y->key ? -1 : x->key > y->key ? 1 : x->i - y->i;
}

static int family_rank_in(const char *const *list, size_t n, const char *family) {
    char low[128];
    lower_into(low, sizeof low, family);
    for (size_t i = 0; i < n; i++)
        if (strstr(low, list[i])) return (int)i;
    return RANK_UNKNOWN;
}

/* How well a scanned face stands for a generic family; smaller is better. */
static int generic_rank(int generic, const scanned_font *f, bool italic) {
    if (generic == GEN_SANS) return f->rank + (f->italic != italic ? 2 * RANK_PENALTY : 0);
    static const char *const serifs[] = {
        "noto serif", "dejavu serif", "liberation serif", "times new roman", "tinos", "georgia", "source serif",
        "cambria", "palatino", "garamond", "times", "source han serif", "songti", "mincho", "ming",
    };
    static const char *const monos[] = {
        "dejavu sans mono", "noto sans mono", "liberation mono", "jetbrains mono", "fira mono", "fira code",
        "source code", "consolas", "menlo", "sf mono", "cascadia", "ubuntu mono", "roboto mono", "inconsolata",
        "cousine", "courier new", "courier", "hack",
    };
    bool mono = f->fixed || looks_monospaced(f->family);
    int r = generic == GEN_SERIF ? family_rank_in(serifs, sizeof serifs / sizeof *serifs, f->family)
                                 : family_rank_in(monos, sizeof monos / sizeof *monos, f->family);
    if (generic == GEN_SERIF && mono) r += RANK_PENALTY;
    if (generic == GEN_MONO && !mono) r += RANK_PENALTY;
    if (f->italic != italic) r += 2 * RANK_PENALTY;
    return r;
}

/* A scanned font: the family called `name`, or the best stand-in for generic family `generic` (name NULL),
 * at the weight CSS matching prefers. With `cp` it must also cover that codepoint. */
static int fc_find(gp_text_system *ts, const char *name, int generic, int req, uint32_t cp, bool color) {
    int weight = req_weight(req);
    bool italic = req_italic(req);
    if (!ts->scanned) scan_fonts(ts);
    scan_cand *c = malloc((size_t)(ts->nscan ? ts->nscan : 1) * sizeof *c);
    int n = 0;
    for (int i = 0; i < ts->nscan; i++) {
        const scanned_font *f = &ts->scan[i];
        if (f->color != color) continue;
        long long key;
        if (name && !color) {
            if (strcasecmp(f->family, name) != 0) continue;
            key = weight_rank(weight, f->weight) + (f->italic != italic ? 10000 : 0);
        } else {
            int rank = generic_rank(generic, f, italic);
            key = ((long long)rank << 20) | (rank >= RANK_UNKNOWN ? 0 : weight_rank(weight, f->weight));
        }
        c[n++] = (scan_cand){ key, i };
    }
    qsort(c, (size_t)n, sizeof *c, cand_cmp);
    int best = -1;
    for (int k = 0; k < n && best < 0; k++) {
        const scanned_font *f = &ts->scan[c[k].i];
        if (cp) {
            FT_Face face;
            if (FT_New_Face(ts->ft, f->path, f->index, &face) != 0) continue;
            bool covers = FT_Get_Char_Index(face, cp) != 0;
            FT_Done_Face(face);
            if (!covers) continue;
        }
        best = c[k].i;
    }
    free(c);
    return best >= 0 ? font_add(ts, ts->scan[best].path, ts->scan[best].index) : -1;
}
#endif

static bool has_glyph(const gp_text_system *ts, int font, uint32_t cp) {
    return FT_Get_Char_Index(ts->fonts[font].face, cp) != 0;
}

/* ---- family lists and styles */

static family_entry *family_parse(gp_text_system *ts, const char *spec) {
    if (ts->nfamilies == ts->cap_families) {
        ts->cap_families = ts->cap_families ? ts->cap_families * 2 : 8;
        ts->families = realloc(ts->families, (size_t)ts->cap_families * sizeof *ts->families);
    }
    family_entry *fe = &ts->families[ts->nfamilies++];
    *fe = (family_entry){ .spec = strdup(spec), .fallback_generic = GEN_SANS };
    fe->names = calloc(MAX_CHAIN, sizeof *fe->names);
    fe->generic = calloc(MAX_CHAIN, sizeof *fe->generic);
    const char *p = spec;
    while (*p && fe->n < MAX_CHAIN) {
        const char *end = strchr(p, ',');
        if (!end) end = p + strlen(p);
        const char *lo = p, *hi = end;
        while (lo < hi && isspace((unsigned char)*lo)) lo++;
        while (hi > lo && isspace((unsigned char)hi[-1])) hi--;
        if (hi - lo >= 2 && (*lo == '"' || *lo == '\'') && hi[-1] == *lo) { lo++; hi--; }
        if (hi > lo) {
            char *name = malloc((size_t)(hi - lo) + 1);
            memcpy(name, lo, (size_t)(hi - lo));
            name[hi - lo] = 0;
            int g = generic_of(name);
            if (g != GEN_NONE) {
                fe->has_generic = true;
                fe->fallback_generic = g;
            }
            fe->names[fe->n] = name;
            fe->generic[fe->n++] = g;
        }
        p = *end ? end + 1 : end;
    }
    return fe;
}

/* Interns a font-family property ("Inter, sans-serif"); the id goes into a font selector. NULL and "" are
 * the default family, id 0. */
int gp_text_family(gp_text_system *ts, const char *spec) {
    if (!spec || !*spec) return 0;
    for (int i = 1; i < ts->nfamilies; i++)
        if (strcmp(ts->families[i].spec, spec) == 0) return i;
    if (ts->nfamilies >= 0xffff) return 0;
    family_parse(ts, spec);
    return ts->nfamilies - 1;
}

/* Interns an OpenType feature list. Each item is parsed by HarfBuzz ("tnum", "-liga", "liga=0", "cv02=3"); one it
 * cannot parse is ignored, and so is a range ("liga[3:5]"), because the text is shaped in runs. */
int gp_text_features(gp_text_system *ts, const char *list) {
    if (!list || !*list || strcasecmp(list, "normal") == 0 || strcasecmp(list, "none") == 0) return 0;
    for (int i = 1; i < ts->nfeatures; i++)
        if (strcmp(ts->features[i].spec, list) == 0) return i;
    if (ts->nfeatures >= 0xffff) return 0;
    if (ts->nfeatures == ts->cap_features) {
        ts->cap_features = ts->cap_features ? ts->cap_features * 2 : 8;
        ts->features = realloc(ts->features, (size_t)ts->cap_features * sizeof *ts->features);
    }
    feature_entry *fe = &ts->features[ts->nfeatures];
    memset(fe, 0, sizeof *fe);
    fe->spec = strdup(list);
    const char *p = list;
    while (*p && fe->n < MAX_FEATURES) {
        const char *end = strchr(p, ',');
        if (!end) end = p + strlen(p);
        const char *lo = p, *hi = end;
        while (lo < hi && isspace((unsigned char)*lo)) lo++;
        while (hi > lo && isspace((unsigned char)hi[-1])) hi--;
        hb_feature_t f;
        if (hi > lo && hb_feature_from_string(lo, (int)(hi - lo), &f)) {
            f.start = HB_FEATURE_GLOBAL_START;
            f.end = HB_FEATURE_GLOBAL_END;
            fe->f[fe->n++] = f;
        }
        p = *end ? end + 1 : end;
    }
    return ts->nfeatures++;
}

/* Interns a font-variation-settings list: "wdth" 75, "opsz" 14 (CSS) or wdth=75, opsz=14. An item that does not parse is ignored; a repeated
 * axis takes its last value; values are kept to the nearest half. NULL, "" and "normal" are no variations, id 0. */
int gp_text_variations(gp_text_system *ts, const char *list) {
    if (!list || !*list || strcasecmp(list, "normal") == 0 || strcasecmp(list, "none") == 0) return 0;
    variation_entry e;
    memset(&e, 0, sizeof e);
    const char *p = list;
    while (*p) {
        const char *end = strchr(p, ',');
        if (!end) end = p + strlen(p);
        const char *q = p;
        while (q < end && (isspace((unsigned char)*q) || *q == '"' || *q == '\'')) q++;
        if (end - q >= 4) {
            hb_tag_t tag = HB_TAG(q[0], q[1], q[2], q[3]);
            q += 4;
            while (q < end && (isspace((unsigned char)*q) || *q == '"' || *q == '\'' || *q == '=' || *q == ':')) q++;
            char *num_end;
            double v = strtod(q, &num_end);
            if (num_end != q && num_end <= end) {
                v = floor(v * 2 + 0.5) / 2;
                int k = 0;
                while (k < e.n && e.tag[k] != tag) k++;
                if (k < MAX_VARIATIONS) { e.tag[k] = tag; e.val[k] = (float)v; if (k == e.n) e.n++; }
            }
        }
        p = *end ? end + 1 : end;
    }
    if (!e.n) return 0;
    for (int i = 1; i < e.n; i++)   /* sorted by tag: the same set of axes is one list */
        for (int j = i; j > 0 && e.tag[j - 1] > e.tag[j]; j--) {
            hb_tag_t t = e.tag[j]; e.tag[j] = e.tag[j - 1]; e.tag[j - 1] = t;
            float x = e.val[j]; e.val[j] = e.val[j - 1]; e.val[j - 1] = x;
        }
    char spec[MAX_VARIATIONS * 20], *o = spec;
    *o = 0;
    for (int i = 0; i < e.n; i++) {
        char tg[5] = { (char)(e.tag[i] >> 24), (char)(e.tag[i] >> 16), (char)(e.tag[i] >> 8), (char)e.tag[i], 0 };
        o += snprintf(o, sizeof spec - (size_t)(o - spec), "%s%s=%g", i ? "," : "", tg, e.val[i]);
    }
    for (int i = 1; i < ts->nvariations; i++)
        if (strcmp(ts->variations[i].spec, spec) == 0) return i;
    if (ts->nvariations >= 0x7fff) return 0;
    if (ts->nvariations == ts->cap_variations) {
        ts->cap_variations = ts->cap_variations ? ts->cap_variations * 2 : 8;
        ts->variations = realloc(ts->variations, (size_t)ts->cap_variations * sizeof *ts->variations);
    }
    e.spec = strdup(spec);
    ts->variations[ts->nvariations] = e;
    return ts->nvariations++;
}

static uint64_t style_key(int family, int weight, bool italic, int features, int variations) {
    return (uint64_t)variations << 48 | (uint64_t)features << 32 | (uint64_t)family << 16 | (uint64_t)weight | (italic ? REQ_ITALIC : 0u);
}

static int style_id(gp_text_system *ts, int64_t sel) {
    int weight = req_weight((int)sel), family = (int)((sel >> 16) & 0xffff), features = (int)((sel >> 32) & 0xffff), variations = (int)((sel >> 48) & 0x7fff);
    bool italic = req_italic((int)sel);
    if (family >= ts->nfamilies) family = 0;
    if (features >= ts->nfeatures) features = 0;
    if (variations >= ts->nvariations) variations = 0;
    uint64_t key = style_key(family, weight, italic, features, variations);
    if (ts->nstyles * 2 >= ts->style_hash_cap) {   /* grow, and rehash */
        int cap = ts->style_hash_cap ? ts->style_hash_cap * 2 : 64;
        free(ts->style_hash);
        ts->style_hash = calloc((size_t)cap, sizeof(int));
        ts->style_hash_cap = cap;
        for (int i = 0; i < ts->nstyles; i++) {
            uint64_t k = style_key(ts->styles[i].family, ts->styles[i].weight, ts->styles[i].italic, ts->styles[i].features, ts->styles[i].variations);
            size_t h = (size_t)(k * 0x9e3779b97f4a7c15ull >> 32) & (size_t)(cap - 1);
            while (ts->style_hash[h]) h = (h + 1) & (size_t)(cap - 1);
            ts->style_hash[h] = i + 1;
        }
    }
    size_t mask = (size_t)ts->style_hash_cap - 1, h = (size_t)(key * 0x9e3779b97f4a7c15ull >> 32) & mask;
    for (; ts->style_hash[h]; h = (h + 1) & mask) {
        style_entry *st = &ts->styles[ts->style_hash[h] - 1];
        if (st->family == family && st->weight == weight && st->italic == italic && st->features == features && st->variations == variations) return ts->style_hash[h] - 1;
    }
    if (ts->nstyles == ts->cap_styles) {
        ts->cap_styles = ts->cap_styles ? ts->cap_styles * 2 : 16;
        ts->styles = realloc(ts->styles, (size_t)ts->cap_styles * sizeof *ts->styles);
    }
    ts->styles[ts->nstyles] = (style_entry){ .family = family, .weight = weight, .italic = italic, .features = features, .variations = variations };
    ts->style_hash[h] = ts->nstyles + 1;
    return ts->nstyles++;
}

/* The registered text font of family `fam` whose weight CSS matching prefers for `weight`; the earliest
 * registered wins a tie. */
static int registered_family_best(const gp_text_system *ts, const char *fam, int req) {
    int weight = req_weight(req);
    bool italic = req_italic(req);
    int best = -1, best_rank = 0;
    for (int i = 0; i < ts->nregistered; i++) {
        int id = ts->registered[i];
        const gp_font *f = &ts->fonts[id];
        if (f->color || strcasecmp(f->family, fam) != 0) continue;
        int r = weight_rank(weight, f->weight) + (f->italic != italic ? 10000 : 0);
        if (best < 0 || r < best_rank) { best = id; best_rank = r; }
    }
    return best;
}

/* What the default sans-serif is among the registered fonts: the family the app called "sans-serif", else
 * the family of the first registered font that can draw Latin text. NULL when nothing is registered. */
static const char *registered_sans_family(const gp_text_system *ts) {
    for (int i = 0; i < ts->nregistered; i++)
        if (!ts->fonts[ts->registered[i]].color && strcasecmp(ts->fonts[ts->registered[i]].family, "sans-serif") == 0)
            return "sans-serif";
    for (int i = 0; i < ts->nregistered; i++) {
        const gp_font *f = &ts->fonts[ts->registered[i]];
        if (!f->color && FT_Get_Char_Index(f->face, 'A') != 0) return f->family;
    }
    return NULL;
}

/* The system's font for a family at a weight. A system query costs milliseconds and a text whose weight
 * moves (an animation) asks at every weight in between, so weights are rounded to 10 for the query, and
 * the answer is remembered until the set of fonts changes. */
static int system_family(gp_text_system *ts, const char *name, int generic, int req) {
    int q = (req_weight(req) + 5) / 10 * 10;
    if (q < 10) q = 10;
    q |= req & REQ_ITALIC;
    const char *key = generic == GEN_NONE ? name : "";
    for (int i = 0; i < ts->nsysmemo; i++)
        if (ts->sysmemo[i].generic == generic && ts->sysmemo[i].weight == q && strcasecmp(ts->sysmemo[i].name, key) == 0)
            return ts->sysmemo[i].font;
    int id = generic == GEN_NONE ? fc_find(ts, name, GEN_NONE, q, 0, false) : fc_find(ts, NULL, generic, q, 'A', false);
    if (ts->nsysmemo == ts->cap_sysmemo) {
        ts->cap_sysmemo = ts->cap_sysmemo ? ts->cap_sysmemo * 2 : 32;
        ts->sysmemo = realloc(ts->sysmemo, (size_t)ts->cap_sysmemo * sizeof *ts->sysmemo);
    }
    ts->sysmemo[ts->nsysmemo++] = (typeof(*ts->sysmemo)){ strdup(key), generic, q, id };
    return id;
}

/* The font for one name of a family list at a weight: a registered font of that family, else the
 * system's. Generic names mean the app's registered default, or the system's. */
static int find_named(gp_text_system *ts, const char *name, int generic, int req) {
    const char *reg = generic == GEN_SANS ? registered_sans_family(ts) : name;
    int id = reg ? registered_family_best(ts, reg, req) : -1;
    if (id < 0 && !ts->no_system) id = system_family(ts, name, generic, req);
    return id;
}

static bool in_chain(const style_entry *st, int id) {
    for (int i = 0; i < st->nchain; i++)
        if (st->chain[i] == id) return true;
    return false;
}

static int st_req(const style_entry *st) { return st->weight | (st->italic ? REQ_ITALIC : 0); }

static void style_build(gp_text_system *ts, style_entry *st) {
    const family_entry *fe = &ts->families[st->family];
    st->nchain = 0;
    st->built = true;
    for (int i = 0; i < fe->n; i++) {
        int id = find_named(ts, fe->names[i], fe->generic[i], st_req(st));
        if (id >= 0 && !in_chain(st, id)) st->chain[st->nchain++] = id;
    }
    if (!fe->has_generic) {   /* a list that names no generic family ends in the default one, as in CSS browsers */
        int id = find_named(ts, "sans-serif", GEN_SANS, st_req(st));
        if (id >= 0 && !in_chain(st, id)) st->chain[st->nchain++] = id;
    }
    if (st->nchain == 0) fprintf(stderr, "gpuic: could not load a sans-serif font\n");
}

/* The font that draws Latin text for a style, which also gives its line metrics; -1 if there is none. */
static int primary_font(gp_text_system *ts, int style) {
    style_entry *st = &ts->styles[style];
    if (!st->built) style_build(ts, st);
    return st->nchain ? st->chain[0] : -1;
}

/* A registered font covering `cp`, of any family, whose weight is the closest to the request; the earliest
 * registered wins a tie. `color` picks color fonts (emoji) or monochrome ones. */
static int registered_cover(gp_text_system *ts, int req, uint32_t cp, bool color) {
    int weight = req_weight(req);
    bool italic = req_italic(req);
    int best = -1, best_rank = 0;
    for (int i = 0; i < ts->nregistered; i++) {
        int id = ts->registered[i];
        if (ts->fonts[id].color != color || !has_glyph(ts, id, cp)) continue;
        int r = weight_rank(weight, ts->fonts[id].weight) + (ts->fonts[id].italic != italic ? 10000 : 0);
        if (best < 0 || r < best_rank) { best = id; best_rank = r; }
    }
    return best;
}

static int emoji_font(gp_text_system *ts) {
    if (ts->emoji == -1) {
        int id = registered_cover(ts, 400, 0x1f600, true);
        if (id < 0 && !ts->no_system) id = fc_find(ts, NULL, GEN_SANS, 400, 0x1f600, true);
        if (id >= 0 && FT_Get_Char_Index(ts->fonts[id].face, 0x1f600) == 0) id = -1;
        ts->emoji = id >= 0 ? id : -2;
    }
    return ts->emoji >= 0 ? ts->emoji : -1;
}

/* Codepoints that default to emoji presentation (Emoji_Presentation=Yes). Anything else gets
 * the emoji font only when followed by U+FE0F, which asks for it explicitly. */
static bool is_emoji_presentation(uint32_t cp) {
    if (cp >= 0x1f300 && cp <= 0x1faff) return true;
    static const struct { uint32_t lo, hi; } r[] = {
        { 0x1f1e6, 0x1f1ff }, /* regional indicators: the two halves of a flag */
        { 0x1f004, 0x1f004 }, { 0x1f0cf, 0x1f0cf }, { 0x1f18e, 0x1f18e }, { 0x1f191, 0x1f19a },
        { 0x1f201, 0x1f201 }, { 0x1f21a, 0x1f21a }, { 0x1f22f, 0x1f22f }, { 0x1f232, 0x1f236 },
        { 0x1f238, 0x1f23a }, { 0x1f250, 0x1f251 },
        { 0x231a, 0x231b }, { 0x23e9, 0x23ec }, { 0x23f0, 0x23f0 }, { 0x23f3, 0x23f3 },
        { 0x25fd, 0x25fe }, { 0x2614, 0x2615 }, { 0x2648, 0x2653 }, { 0x267f, 0x267f },
        { 0x2693, 0x2693 }, { 0x26a1, 0x26a1 }, { 0x26aa, 0x26ab }, { 0x26bd, 0x26be },
        { 0x26c4, 0x26c5 }, { 0x26ce, 0x26ce }, { 0x26d4, 0x26d4 }, { 0x26ea, 0x26ea },
        { 0x26f2, 0x26f3 }, { 0x26f5, 0x26f5 }, { 0x26fa, 0x26fa }, { 0x26fd, 0x26fd },
        { 0x2705, 0x2705 }, { 0x270a, 0x270b }, { 0x2728, 0x2728 }, { 0x274c, 0x274c },
        { 0x274e, 0x274e }, { 0x2753, 0x2755 }, { 0x2757, 0x2757 }, { 0x2795, 0x2797 },
        { 0x27b0, 0x27b0 }, { 0x27bf, 0x27bf }, { 0x2b1b, 0x2b1c }, { 0x2b50, 0x2b50 },
        { 0x2b55, 0x2b55 },
    };
    for (size_t i = 0; i < sizeof r / sizeof *r; i++)
        if (cp >= r[i].lo && cp <= r[i].hi) return true;
    return false;
}

/* Codepoints that modify the one before them and must be shaped in its font. */
static bool is_inherit(uint32_t cp) {
    return cp == 0x200d || cp == 0x20e3 || (cp >= 0xfe00 && cp <= 0xfe0f) ||
           (cp >= 0x0300 && cp <= 0x036f) || (cp >= 0x1ab0 && cp <= 0x1aff) ||
           (cp >= 0x1dc0 && cp <= 0x1dff) || (cp >= 0x20d0 && cp <= 0x20ff) ||
           (cp >= 0xfe20 && cp <= 0xfe2f) || (cp >= 0x1f3fb && cp <= 0x1f3ff) ||
           (cp >= 0xe0020 && cp <= 0xe007f) || (cp >= 0xe0100 && cp <= 0xe01ef);
}

static uint32_t decode(const char *s, size_t len, size_t i, size_t *n) {
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

/* Which font draws `cp`. Order: the emoji font for emoji-presentation codepoints, the fonts the style's
 * family list names, the app's other registered fonts, fallbacks already loaded, then (once) whatever the
 * system suggests. System queries are slow, so a miss is only paid for the first character a fallback font
 * will cover; the result for every codepoint is cached, including "nothing has it" (the primary's .notdef). */
static int resolve_font(gp_text_system *ts, int style, uint32_t cp, bool want_emoji) {
    size_t h = (cp * 2654435761u + (uint32_t)style * 40503u + (want_emoji ? 977u : 0)) % RESOLVE_CACHE;
    typeof(ts->resolve[0]) *c = &ts->resolve[h];
    if (c->valid && c->cp == cp && c->style == style && c->emoji == want_emoji) return c->font;

    int primary = primary_font(ts, style);
    style_entry *st = &ts->styles[style];
    int weight = st->weight, req = st_req(st);
    int id = -1;
    if (want_emoji) {
        int e = emoji_font(ts);
        if (e >= 0 && has_glyph(ts, e, cp)) id = e;
    }
    for (int i = 0; id < 0 && i < st->nchain; i++)
        if (has_glyph(ts, st->chain[i], cp)) id = st->chain[i];
    if (id < 0) id = registered_cover(ts, req, cp, false);
    if (id < 0) id = registered_cover(ts, req, cp, true);
    for (int i = 0; id < 0 && i < st->nfallback; i++)
        if (has_glyph(ts, st->fallback[i], cp)) id = st->fallback[i];
    if (id < 0 && ts->emoji >= 0 && has_glyph(ts, ts->emoji, cp)) id = ts->emoji;
    if (id < 0 && cp >= 0x80 && !ts->no_system) {
        int g = ts->families[st->family].fallback_generic;
        int f = fc_find(ts, NULL, g, req, cp, false);
        if (f < 0 || !has_glyph(ts, f, cp)) f = fc_find(ts, NULL, g, req, cp, true); /* maybe a color-only font */
        if (f >= 0 && has_glyph(ts, f, cp)) {
            id = f;
            if (!in_chain(st, f)) {
                bool known = false;
                for (int i = 0; i < st->nfallback; i++) known |= st->fallback[i] == f;
                if (!known) {
                    if (st->nfallback == st->cap_fallback) {
                        st->cap_fallback = st->cap_fallback ? st->cap_fallback * 2 : 4;
                        st->fallback = realloc(st->fallback, (size_t)st->cap_fallback * sizeof(int));
                    }
                    st->fallback[st->nfallback++] = f;
                }
            }
        }
    }
    if (id < 0) id = primary; /* draw the primary's .notdef box */
    if (id >= 0) id = font_variant(ts, id, weight, st->italic, st->variations);
    c->cp = cp;
    c->style = style;
    c->emoji = want_emoji;
    c->font = id;
    c->valid = true;
    return id;
}

/* ---------------------------------------------------------------- system */

static char *locale_language(void) {
    const char *loc = getenv("LC_ALL");
    if (!loc || !*loc) loc = getenv("LC_CTYPE");
    if (!loc || !*loc) loc = getenv("LANG");
    if (!loc) return NULL;
    /* Only CJK locales need a hint: it picks the regional glyph variants of shared Han characters */
    if (strncmp(loc, "ja", 2) == 0) return strdup("ja");
    if (strncmp(loc, "ko", 2) == 0) return strdup("ko");
    if (strncmp(loc, "zh_TW", 5) == 0 || strncmp(loc, "zh_HK", 5) == 0) return strdup("zh-tw");
    if (strncmp(loc, "zh", 2) == 0) return strdup("zh-cn");
    return NULL;
}

gp_text_system *gp_text_system_new(void) {
    gp_text_system *ts = calloc(1, sizeof *ts);
    if (FT_Init_FreeType(&ts->ft) != 0) {
        free(ts);
        return NULL;
    }
#ifdef GPUIC_HAVE_FONTCONFIG
    FcInit();
#endif
    family_parse(ts, "");   /* id 0: the default family */
    ts->features = calloc(8, sizeof *ts->features);   /* id 0: no features */
    ts->cap_features = 8;
    ts->nfeatures = 1;
    ts->features[0].spec = strdup("");
    ts->variations = calloc(8, sizeof *ts->variations);   /* id 0: no variations */
    ts->cap_variations = 8;
    ts->nvariations = 1;
    ts->variations[0].spec = strdup("");
    ts->emoji = -1;
    ts->no_subpixel = getenv("GPUIC_SUBPIXEL") && strcmp(getenv("GPUIC_SUBPIXEL"), "0") == 0;
    ts->lang = locale_language();
    atlas_init(&ts->alpha, GP_ATLAS_SIZE, GP_ATLAS_SIZE, 1);
    atlas_init(&ts->color, GP_COLOR_ATLAS_SIZE, GP_COLOR_ATLAS_SIZE, 4);
    atlas_init(&ts->lcd, GP_LCD_ATLAS_SIZE, GP_LCD_ATLAS_SIZE, 4);
    ts->system_lcd = -1;
    {
        const char *aa = getenv("GPUIC_LCD");
        if (aa && (strcmp(aa, "off") == 0 || strcmp(aa, "0") == 0 || strcasecmp(aa, "gray") == 0)) ts->aa_mode = GP_AA_GRAYSCALE;
        else if (aa && strcasecmp(aa, "rgb") == 0) ts->aa_mode = GP_AA_LCD_RGB;
        else if (aa && strcasecmp(aa, "bgr") == 0) ts->aa_mode = GP_AA_LCD_BGR;
        else if (aa && strcasecmp(aa, "vrgb") == 0) ts->aa_mode = GP_AA_LCD_VRGB;
        else if (aa && strcasecmp(aa, "vbgr") == 0) ts->aa_mode = GP_AA_LCD_VBGR;
    }
    FT_Library_SetLcdFilter(ts->ft, FT_LCD_FILTER_DEFAULT);   /* a no-op error where FreeType renders LCD its own way */
    ts->text_gamma = 1.f;
    if (getenv("GPUIC_TEXT_GAMMA")) gp_text_set_gamma(ts, (float)atof(getenv("GPUIC_TEXT_GAMMA")));
    ts->glyph_cap = 1024;
    ts->glyphs = calloc(ts->glyph_cap, sizeof *ts->glyphs);
    ts->hbuf = hb_buffer_create();
    return ts;
}

void gp_text_system_free(gp_text_system *ts) {
    if (!ts) return;
    for (int i = 0; i < ts->nfonts; i++) {
        hb_font_destroy(ts->fonts[i].hb_font);
        hb_face_destroy(ts->fonts[i].hb_face);
        FT_Done_Face(ts->fonts[i].face);
        free(ts->fonts[i].path);
        free(ts->fonts[i].family);
    }
    for (int i = 0; i < ts->nfonts; i++)
        if (ts->fonts[i].owns_data) free(ts->fonts[i].data); /* after every face over it is gone */
    free(ts->fonts);
    free(ts->registered);
    for (int i = 0; i < ts->nfamilies; i++) {
        for (int k = 0; k < ts->families[i].n; k++) free(ts->families[i].names[k]);
        free(ts->families[i].names);
        free(ts->families[i].generic);
        free(ts->families[i].spec);
    }
    free(ts->families);
    for (int i = 0; i < ts->nfeatures; i++) free(ts->features[i].spec);
    free(ts->features);
    for (int i = 0; i < ts->nvariations; i++) free(ts->variations[i].spec);
    free(ts->variations);
    for (int i = 0; i < ts->nstyles; i++) free(ts->styles[i].fallback);
    free(ts->styles);
    free(ts->style_hash);
    for (int i = 0; i < ts->nsysmemo; i++) free(ts->sysmemo[i].name);
    free(ts->sysmemo);
    for (int i = 0; i < SHAPE_CACHE; i++) {
        free(ts->shape_cache[i].text);
        free(ts->shape_cache[i].glyphs);
    }
    hb_buffer_destroy(ts->hbuf);
    free(ts->gv.v);
    free(ts->cps);
    free(ts->boff);
    free(ts->cpfont);
    free(ts->btypes);
    free(ts->bbrackets);
    free(ts->blevels);
    free(ts->bmap);
    free(ts->lang);
#ifndef GPUIC_HAVE_FONTCONFIG
    for (int i = 0; i < ts->nscan; i++) {
        free(ts->scan[i].path);
        free(ts->scan[i].family);
    }
    free(ts->scan);
#endif
    FT_Done_FreeType(ts->ft);
    free(ts->alpha.data);
    free(ts->color.data);
    free(ts->lcd.data);
    free(ts->glyphs);
    free(ts);
}

/* ---------------------------------------------------------------- shaping */

static void gv_push(glyph_vec *gv, gp_glyph_pos g) {
    if (gv->n == gv->cap) {
        gv->cap = gv->cap ? gv->cap * 2 : 64;
        gv->v = realloc(gv->v, (size_t)gv->cap * sizeof *gv->v);
        if (!gv->v) abort();
    }
    gv->v[gv->n++] = g;
}

static void scratch_reserve(gp_text_system *ts, size_t n) {
    if (n + 1 <= ts->cap_cps) return;
    size_t c = ts->cap_cps ? ts->cap_cps : 128;
    while (c < n + 1) c *= 2;
    ts->cps = realloc(ts->cps, c * sizeof *ts->cps);
    ts->boff = realloc(ts->boff, (c + 1) * sizeof *ts->boff);
    ts->cpfont = realloc(ts->cpfont, c * sizeof *ts->cpfont);
    ts->btypes = realloc(ts->btypes, c * sizeof *ts->btypes);
    ts->bbrackets = realloc(ts->bbrackets, c * sizeof *ts->bbrackets);
    ts->blevels = realloc(ts->blevels, c * sizeof *ts->blevels);
    ts->bmap = realloc(ts->bmap, c * sizeof *ts->bmap);
    if (!ts->cps || !ts->boff || !ts->cpfont || !ts->btypes || !ts->bbrackets || !ts->blevels || !ts->bmap)
        abort();
    ts->cap_cps = c;
}

/* Could this codepoint be, or affect, right-to-left text? A cheap test that lets pure left-to-right
 * text skip the bidi algorithm altogether: Hebrew, Arabic, Syriac, Thaana, NKo, the presentation
 * forms, the historic RTL scripts, and the explicit directional formatting characters. */
static bool may_be_rtl(uint32_t cp) {
    return (cp >= 0x0590 && cp <= 0x08ff) || cp == 0x200e || cp == 0x200f || cp == 0x061c ||
           (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2066 && cp <= 0x2069) ||
           (cp >= 0xfb1d && cp <= 0xfdff) || (cp >= 0xfe70 && cp <= 0xfeff) ||
           (cp >= 0x10800 && cp <= 0x10fff) || (cp >= 0x1e800 && cp <= 0x1efff);
}

bool gp_text_may_be_rtl(uint32_t cp) { return may_be_rtl(cp); }

/* ---------------------------------------------------------------- grapheme clusters */

#include "grapheme_data.h"

static bool in_ranges(const uint32_t (*t)[2], size_t n, uint32_t cp) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp < t[mid][0]) hi = mid;
        else if (cp > t[mid][1]) lo = mid + 1;
        else return true;
    }
    return false;
}
#define IN_TABLE(table, cp) in_ranges(table, sizeof table / sizeof *table, cp)

/* The grapheme-break properties of a codepoint (GP_GB_*). Hangul syllable types are arithmetic; the rest are tables. */
unsigned gp_grapheme_props(uint32_t cp) {
    unsigned p = 0;
    if (cp < 0xa0) return cp < 0x20 || cp == 0x7f ? GP_GB_CONTROL : 0;   /* (the Latin-1 supplement is the first place with anything else) */
    if (cp == 0x200d) return GP_GB_ZWJ;
    if (cp >= 0x1100 && cp <= 0x115f) p |= GP_GB_L;
    else if (cp >= 0xa960 && cp <= 0xa97c) p |= GP_GB_L;
    else if ((cp >= 0x1160 && cp <= 0x11a7) || (cp >= 0xd7b0 && cp <= 0xd7c6)) p |= GP_GB_V;
    else if ((cp >= 0x11a8 && cp <= 0x11ff) || (cp >= 0xd7cb && cp <= 0xd7fb)) p |= GP_GB_T;
    else if (cp >= 0xac00 && cp <= 0xd7a3) p |= (cp - 0xac00) % 28 == 0 ? GP_GB_LV : GP_GB_LVT;
    if (cp >= 0x1f1e6 && cp <= 0x1f1ff) p |= GP_GB_RI;
    if (IN_TABLE(gb_extend, cp)) p |= GP_GB_EXTEND;
    if (IN_TABLE(gb_spacing, cp)) p |= GP_GB_SPACING;
    if (IN_TABLE(gb_prepend, cp)) p |= GP_GB_PREPEND;
    if (IN_TABLE(gb_control, cp)) p |= GP_GB_CONTROL;
    if (IN_TABLE(gb_extpict, cp)) p |= GP_GB_EXTPICT;
    if (IN_TABLE(gb_consonant, cp)) p |= GP_GB_CONSONANT;
    if (IN_TABLE(gb_linker, cp)) p |= GP_GB_LINKER;
    return p;
}

void gp_gstate_start(gp_gstate *g, uint32_t cp) {
    g->prev = gp_grapheme_props(cp);
    g->pict = (g->prev & GP_GB_EXTPICT) ? 1 : 0;
    g->incb = (g->prev & GP_GB_CONSONANT) ? 1 : 0;
    g->ri = (g->prev & GP_GB_RI) ? 1 : 0;
}

/* UAX #29 GB3..GB13 with the Indic conjunct rule (GB9c): does `cp` continue the cluster whose last codepoint was the one the state
 * holds? Updates the state either way: a codepoint that does not join starts the next cluster. */
bool gp_gstate_joins(gp_gstate *g, uint32_t cp) {
    unsigned p = gp_grapheme_props(cp), q = g->prev;
    bool join;
    if ((q | p) & GP_GB_CONTROL) join = false;                                          /* GB4, GB5 */
    else if ((q & GP_GB_L) && (p & (GP_GB_L | GP_GB_V | GP_GB_LV | GP_GB_LVT))) join = true;   /* GB6 */
    else if ((q & (GP_GB_LV | GP_GB_V)) && (p & (GP_GB_V | GP_GB_T))) join = true;     /* GB7 */
    else if ((q & (GP_GB_LVT | GP_GB_T)) && (p & GP_GB_T)) join = true;                 /* GB8 */
    else if (p & (GP_GB_EXTEND | GP_GB_ZWJ)) join = true;                               /* GB9 */
    else if (p & GP_GB_SPACING) join = true;                                            /* GB9a */
    else if (q & GP_GB_PREPEND) join = true;                                            /* GB9b */
    else if (g->incb == 2 && (p & GP_GB_CONSONANT)) join = true;                        /* GB9c */
    else if (g->pict == 2 && (p & GP_GB_EXTPICT)) join = true;                          /* GB11 */
    else if ((q & GP_GB_RI) && (p & GP_GB_RI) && g->ri == 1) join = true;               /* GB12, GB13 */
    else join = false;
    if (!join) { gp_gstate_start(g, cp); return false; }
    /* ExtPict Extend* ZWJ: 1 after the pictograph and its extenders, 2 once the ZWJ is in */
    if (p & GP_GB_EXTPICT) g->pict = 1;
    else if (g->pict == 1 && (p & GP_GB_EXTEND)) g->pict = 1;
    else if (g->pict == 1 && (p & GP_GB_ZWJ)) g->pict = 2;
    else g->pict = 0;
    /* Consonant [Extend Linker]* Linker [Extend Linker]* Consonant: 1 after a consonant, 2 once a linker follows it */
    if (p & GP_GB_CONSONANT) g->incb = 1;
    else if (p & GP_GB_LINKER) g->incb = g->incb ? 2 : 0;
    else if (!(p & (GP_GB_EXTEND | GP_GB_ZWJ))) g->incb = 0;
    g->ri = (p & GP_GB_RI) ? (unsigned char)((g->ri + 1) % 2) : 0;   /* 1: one more flag half is wanted */
    g->prev = p;
    return true;
}

/* Decodes text into ts->cps / ts->boff. Returns the number of codepoints. */
static size_t decode_all(gp_text_system *ts, const char *text, size_t len) {
    scratch_reserve(ts, len);
    size_t n = 0;
    for (size_t i = 0; i < len;) {
        size_t cl;
        ts->cps[n] = decode(text, len, i, &cl);
        ts->boff[n++] = i;
        i += cl;
    }
    ts->boff[n] = len;
    return n;
}

static FriBidiParType par_type(int dir) {
    return dir == GP_BIDI_RTL ? FRIBIDI_PAR_RTL : dir == GP_BIDI_LTR ? FRIBIDI_PAR_LTR : FRIBIDI_PAR_ON;
}

bool gp_text_is_rtl(const char *text, size_t len, int dir) {
    if (dir == GP_BIDI_RTL) return true;
    if (dir == GP_BIDI_LTR) return false;
    /* P2/P3: the first strong character decides (isolates are skipped by FriBidi) */
    bool any = false;
    for (size_t i = 0; i < len && !any;) {
        size_t cl;
        any = may_be_rtl(decode(text, len, i, &cl));
        i += cl;
    }
    if (!any) return false;
    size_t n = 0;
    FriBidiChar *cps = malloc((len + 1) * sizeof *cps);
    for (size_t i = 0; i < len;) {
        size_t cl;
        cps[n++] = decode(text, len, i, &cl);
        i += cl;
    }
    FriBidiCharType *types = malloc((n + 1) * sizeof *types);
    FriBidiBracketType *br = malloc((n + 1) * sizeof *br);
    FriBidiLevel *lv = malloc((n + 1) * sizeof *lv);
    fribidi_get_bidi_types(cps, (FriBidiStrIndex)n, types);
    fribidi_get_bracket_types(cps, (FriBidiStrIndex)n, types, br);
    FriBidiParType base = FRIBIDI_PAR_ON;
    FriBidiLevel top = fribidi_get_par_embedding_levels_ex(types, br, (FriBidiStrIndex)n, &base, lv);
    free(cps);
    free(types);
    free(br);
    free(lv);
    return top != 0 && FRIBIDI_IS_RTL(base); /* 0 means FriBidi failed: assume left-to-right */
}

/* Assigns a font to every codepoint. Codepoints that must stay with their neighbour (ZWJ,
 * variation selectors, combining marks, skin tones, and whatever a ZWJ joins) inherit the previous
 * codepoint's font, so sequences are not torn apart. */
static void assign_fonts(gp_text_system *ts, int style, size_t ncp) {
    int prev_font = -1;
    bool prev_zwj = false;
    for (size_t i = 0; i < ncp; i++) {
        uint32_t cp = ts->cps[i];
        int font;
        if (prev_font >= 0 && (is_inherit(cp) || prev_zwj)) {
            font = prev_font;
        } else {
            bool emoji = is_emoji_presentation(cp);
            if (!emoji && i + 1 < ncp) emoji = ts->cps[i + 1] == 0xfe0f; /* U+FE0F asks for the emoji form */
            font = resolve_font(ts, style, cp, emoji);
        }
        ts->cpfont[i] = font;
        prev_font = font;
        prev_zwj = cp == 0x200d;
    }
}

static void shape_run(gp_text_system *ts, int fid, float size, const char *text, size_t len,
                      size_t byte_start, size_t byte_end, int level, double *pen, const feature_entry *feat) {
    gp_font *f = &ts->fonts[fid];
    hb_buffer_reset(ts->hbuf);
    hb_buffer_add_utf8(ts->hbuf, text, (int)len, (unsigned)byte_start, (int)(byte_end - byte_start));
    hb_buffer_set_direction(ts->hbuf, (level & 1) ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    hb_buffer_guess_segment_properties(ts->hbuf); /* script and language; the direction is ours */
    hb_shape(f->hb_font, ts->hbuf, feat->n ? feat->f : NULL, feat->n);
    unsigned n;
    hb_glyph_info_t *gi = hb_buffer_get_glyph_infos(ts->hbuf, &n);
    hb_glyph_position_t *gp = hb_buffer_get_glyph_positions(ts->hbuf, &n);
    float k = size / f->upem;
    float extra = f->embolden ? size * SYNTHETIC_BOLD_STRENGTH : 0.f; /* what rasterizing adds to each glyph */
    for (unsigned i = 0; i < n; i++) { /* HarfBuzz already returns RTL runs in visual order */
        float adv = (float)gp[i].x_advance * k + (gp[i].x_advance ? extra : 0.f); /* marks stay zero-width */
        gv_push(&ts->gv, (gp_glyph_pos){ gi[i].codepoint, (uint16_t)fid, (uint8_t)level, gi[i].cluster,
                                         (float)(*pen + (float)gp[i].x_offset * k), -(float)gp[i].y_offset * k, adv });
        *pen += adv;
    }
}

static void shape_uncached(gp_text_system *ts, int style, float size, const char *text, size_t len,
                           int dir, float *width) {
    ts->gv.n = 0;
    double pen = 0;   /* (in double: added up in float, the pen drifts by pixels over a long text) */
    size_t ncp = decode_all(ts, text, len);
    assign_fonts(ts, style, ncp);

    bool bidi = dir == GP_BIDI_RTL;
    for (size_t i = 0; i < ncp && !bidi; i++) bidi = may_be_rtl(ts->cps[i]);

    for (size_t i = 0; i < ncp; i++) ts->bmap[i] = (FriBidiStrIndex)i;
    if (bidi) {
        /* UAX #9: embedding levels for the paragraph, then L1 (trailing whitespace goes back to
         * the paragraph level) and L2 (reverse, level by level) in fribidi_reorder_line, which
         * also hands back the visual-to-logical map. No flags: marks stay with their base, and
         * explicit formatting characters stay in the string (HarfBuzz gives them no width). */
        fribidi_get_bidi_types(ts->cps, (FriBidiStrIndex)ncp, ts->btypes);
        fribidi_get_bracket_types(ts->cps, (FriBidiStrIndex)ncp, ts->btypes, ts->bbrackets);
        FriBidiParType base = par_type(dir);
        FriBidiLevel top = fribidi_get_par_embedding_levels_ex(ts->btypes, ts->bbrackets, (FriBidiStrIndex)ncp,
                                                                &base, ts->blevels);
        if (top == 0 ||
            fribidi_reorder_line(0, ts->btypes, (FriBidiStrIndex)ncp, 0, base, ts->blevels, NULL, ts->bmap) == 0) {
            /* FriBidi gave up: treat it as plain left-to-right */
            memset(ts->blevels, 0, ncp * sizeof *ts->blevels);
            for (size_t i = 0; i < ncp; i++) ts->bmap[i] = (FriBidiStrIndex)i;
        }
    } else {
        memset(ts->blevels, 0, ncp * sizeof *ts->blevels);
    }

    /* Walk the codepoints in visual order, cutting them into runs of one level and one font that
     * are contiguous in the logical text (ascending for even levels, descending for odd ones). */
    for (size_t v = 0; v < ncp;) {
        size_t j = (size_t)ts->bmap[v], lo = j, hi = j, w = v + 1;
        int level = ts->blevels[j], font = ts->cpfont[j];
        for (; w < ncp; w++) {
            size_t nj = (size_t)ts->bmap[w];
            bool contiguous = (level & 1) ? nj + 1 == (size_t)ts->bmap[w - 1] : nj == (size_t)ts->bmap[w - 1] + 1;
            if (ts->blevels[nj] != level || ts->cpfont[nj] != font || !contiguous) break;
            if (nj < lo) lo = nj;
            if (nj > hi) hi = nj;
        }
        if (font >= 0) shape_run(ts, font, size, text, len, ts->boff[lo], ts->boff[hi + 1], level, &pen, &ts->features[ts->styles[style].features]);
        v = w;
    }
    *width = (float)pen;
}

static uint64_t fnv(const char *s, size_t n) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 1099511628211ull;
    return h;
}

int gp_text_shape_dir(gp_text_system *ts, int64_t sel, float size, const char *text, size_t len,
                      int dir, gp_glyph_pos *out, int cap, float *width) {
    int style = style_id(ts, sel);
    uint32_t size_q = (uint32_t)(size * 64.f + 0.5f);
    float w = 0;
    const gp_glyph_pos *src;
    int n;

    shape_entry *e = NULL;
    uint64_t h = fnv(text, len) ^ ((uint64_t)style * 0x9e3779b97f4a7c15ull) ^ ((uint64_t)dir << 58) ^ ((uint64_t)size_q << 32);
    if (len <= MAX_CACHED_LEN) {
        e = &ts->shape_cache[h % SHAPE_CACHE];
        if (e->used && e->hash == h && e->len == len && e->style == style && e->size_q == size_q &&
            e->dir == dir && memcmp(e->text, text, len) == 0) {
            src = e->glyphs;
            n = e->n;
            w = e->width;
            goto copy;
        }
    }
    shape_uncached(ts, style, size, text, len, dir, &w);
    n = ts->gv.n;
    src = ts->gv.v;
    if (e) { /* remember it, replacing whatever shared the slot */
        free(e->text);
        free(e->glyphs);
        e->used = true;
        e->style = style;
        e->dir = dir;
        e->size_q = size_q;
        e->hash = h;
        e->len = len;
        e->text = malloc(len + 1);
        memcpy(e->text, text, len);
        e->glyphs = malloc((size_t)(n ? n : 1) * sizeof *e->glyphs);
        if (n) memcpy(e->glyphs, src, (size_t)n * sizeof *e->glyphs); /* src is NULL when no font could be loaded */
        e->n = n;
        e->width = w;
    }
copy:
    if (out) for (int i = 0; i < n && i < cap; i++) out[i] = src[i];
    if (width) *width = w;
    return n;
}

int gp_text_shape(gp_text_system *ts, int64_t weight, float size, const char *text, size_t len,
                  gp_glyph_pos *out, int cap, float *width) {
    return gp_text_shape_dir(ts, weight, size, text, len, GP_BIDI_AUTO, out, cap, width);
}

static float text_width(gp_text_system *ts, int64_t weight, float size, const char *s, size_t len, int dir) {
    float w;
    gp_text_shape_dir(ts, weight, size, s, len, dir, NULL, 0, &w);
    return w;
}

/* The resolved direction (as a GP_BIDI_* value) of the paragraph that starts at text[from]: it ends
 * at the next line break. Resolving it once per paragraph and passing it down is what lets every
 * wrapped line of a Hebrew paragraph be shaped right-to-left even if one line holds only digits. */
static int paragraph_dir(const char *text, size_t from, int dir) {
    const char *nl = strchr(text + from, '\n');
    size_t len = nl ? (size_t)(nl - (text + from)) : strlen(text + from);
    return gp_text_is_rtl(text + from, len, dir) ? GP_BIDI_RTL : GP_BIDI_LTR;
}

void gp_text_metrics(gp_text_system *ts, int64_t weight, float size, float *ascent, float *descent) {
    int fid = primary_font(ts, style_id(ts, weight));
    if (fid < 0) { *ascent = size * 0.8f; *descent = size * 0.2f; return; }
    gp_font *f = &ts->fonts[fid];
    *ascent = f->ascent * size / f->upem;
    *descent = f->descent * size / f->upem;
}

/* ---------------------------------------------------------------- measuring / wrapping */

/* Line breaking for scripts written without spaces. Between two characters a line may break if
 * either is an ideograph, kana or CJK symbol, unless that would start a line with closing
 * punctuation or small kana (kinsoku shori), or end one with an opening bracket. A simplified
 * take on UAX #14, enough for ordinary Chinese and Japanese text. Hangul uses spaces, so it
 * wraps like Latin. */
static bool breaks_around(uint32_t cp) {
    return (cp >= 0x2e80 && cp <= 0x9fff) || (cp >= 0xa000 && cp <= 0xa4cf) ||
           (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xff00 && cp <= 0xffef) ||
           (cp >= 0x20000 && cp <= 0x3ffff);
}

static bool cannot_start_line(uint32_t cp) {
    static const uint32_t set[] = {
        0x3001, 0x3002, 0xff0c, 0xff0e, 0xff1a, 0xff1b, 0xff01, 0xff1f, 0xff09, 0xff3d, 0xff5d,
        0x3015, 0x3009, 0x300b, 0x300d, 0x300f, 0x3011, 0x3019, 0x3017, 0x301f, 0x2019, 0x201d,
        0xbb, 0x30fb, 0x30fc, 0x3005, 0x301c, 0x2026, 0x2025, 0x30fd, 0x30fe, 0x309d, 0x309e,
        /* small kana */
        0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x3063, 0x3083, 0x3085, 0x3087, 0x308e,
        0x30a1, 0x30a3, 0x30a5, 0x30a7, 0x30a9, 0x30c3, 0x30e3, 0x30e5, 0x30e7, 0x30ee,
        /* ASCII closers, so Latin punctuation after CJK stays attached */
        ')', ']', '}', '!', '?', ',', '.', ';', ':',
    };
    for (size_t i = 0; i < sizeof set / sizeof *set; i++)
        if (set[i] == cp) return true;
    return false;
}

static bool cannot_end_line(uint32_t cp);

/* May a line break between these two codepoints, for scripts written without spaces? */
bool gp_cjk_break_allowed(uint32_t before, uint32_t after) {
    return (breaks_around(before) || breaks_around(after)) && !cannot_start_line(after) &&
           !cannot_end_line(before);
}

static bool cannot_end_line(uint32_t cp) {
    static const uint32_t set[] = {
        0xff08, 0xff3b, 0xff5b, 0x3014, 0x3008, 0x300a, 0x300c, 0x300e, 0x3010, 0x3018, 0x3016,
        0x301d, 0x2018, 0x201c, 0xab, '(', '[', '{',
    };
    for (size_t i = 0; i < sizeof set / sizeof *set; i++)
        if (set[i] == cp) return true;
    return false;
}

gp_size gp_text_measure(gp_text_system *ts, int64_t weight, float size, const char *text,
                        float wrap_w, bool pre, gp_arena *arena, gp_text_line **lines_out,
                        int *nlines_out) {
    return gp_text_measure_dir(ts, weight, size, text, wrap_w, pre, GP_BIDI_AUTO, arena, lines_out,
                               nlines_out);
}

gp_size gp_text_measure_dir(gp_text_system *ts, int64_t weight, float size, const char *text,
                            float wrap_w, bool pre, int req_dir, gp_arena *arena,
                            gp_text_line **lines_out, int *nlines_out) {
    int cap = 8, n = 0;
    gp_text_line *lines = NULL;
    gp_text_line stack_lines[8];
    /* Lines are collected in a growing heap array, then copied into the arena. */
    gp_text_line *tmp = stack_lines;
    bool tmp_heap = false;

#define PUSH_LINE(s, e, w)                                                               \
    do {                                                                                 \
        if (n == cap) {                                                                  \
            cap *= 2;                                                                    \
            gp_text_line *nt = malloc((size_t)cap * sizeof *nt);                         \
            memcpy(nt, tmp, (size_t)n * sizeof *nt);                                     \
            if (tmp_heap) free(tmp);                                                     \
            tmp = nt;                                                                    \
            tmp_heap = true;                                                             \
        }                                                                                \
        tmp[n++] = (gp_text_line){ (uint32_t)(s), (uint32_t)(e), (w) };                  \
    } while (0)

    if (pre) {
        float max_pre = 0;
        size_t start = 0, len_pre = strlen(text);
        for (size_t i = 0; i <= len_pre; i++) {
            if (i < len_pre && text[i] != '\n') continue;
            float lw = text_width(ts, weight, size, text + start, i - start,
                                  gp_text_is_rtl(text + start, i - start, req_dir) ? GP_BIDI_RTL : GP_BIDI_LTR);
            PUSH_LINE(start, i, lw);
            if (lw > max_pre) max_pre = lw;
            start = i + 1;
        }
        if (lines_out && arena) {
            *lines_out = gp_arena_alloc(arena, (size_t)n * sizeof **lines_out, 8);
            memcpy(*lines_out, tmp, (size_t)n * sizeof **lines_out);
        }
        if (nlines_out) *nlines_out = n;
        if (tmp_heap) free(tmp);
        return (gp_size){ max_pre, (float)n * size * GP_LINE_HEIGHT };
    }

    const float eps = 0.01f;
    float space_w = text_width(ts, weight, size, " ", 1, GP_BIDI_LTR);
    int pdir = paragraph_dir(text, 0, req_dir);
    float max_w = 0;
    size_t len = strlen(text);
    size_t line_start = 0, line_end = 0;
    float line_w = 0;
    bool line_empty = true;

    size_t i = 0;
    while (i <= len) {
        if (i == len || text[i] == '\n') {
            PUSH_LINE(line_start, line_empty ? line_start : line_end, line_empty ? 0 : line_w);
            if (!line_empty && line_w > max_w) max_w = line_w;
            line_start = line_end = i + 1;
            line_w = 0;
            line_empty = true;
            i++;
            if (i <= len) pdir = paragraph_dir(text, i, req_dir); /* a new paragraph, a new direction */
            continue;
        }
        if (text[i] == ' ') {
            i++;
            continue;
        }
        size_t ws = i;
        while (i < len && text[i] != ' ' && text[i] != '\n') { /* one unbreakable "word" */
            size_t n1, n2;
            uint32_t cp = decode(text, len, i, &n1);
            i += n1;
            if (i >= len || text[i] == ' ' || text[i] == '\n') break;
            uint32_t next = decode(text, len, i, &n2);
            if (gp_cjk_break_allowed(cp, next)) break;
        }
        float ww = text_width(ts, weight, size, text + ws, i - ws, pdir);
        if (line_empty) {
            line_start = ws;
            line_w = ww;
            line_empty = false;
        } else {
            /* distance from the end of the previous word to this one, spaces included */
            float gap = 0;
            for (size_t k = line_end; k < ws; k++) gap += space_w;
            if (wrap_w >= 0 && line_w + gap + ww > wrap_w + eps) {
                PUSH_LINE(line_start, line_end, line_w);
                if (line_w > max_w) max_w = line_w;
                line_start = ws;
                line_w = ww;
            } else {
                line_w += gap + ww;
            }
        }
        line_end = i;
    }
    if (line_w > max_w) max_w = line_w;

    if (lines_out && arena) {
        lines = gp_arena_alloc(arena, (size_t)n * sizeof *lines, 8);
        memcpy(lines, tmp, (size_t)n * sizeof *lines);
        *lines_out = lines;
    }
    if (nlines_out) *nlines_out = n;
    if (tmp_heap) free(tmp);
#undef PUSH_LINE
    return (gp_size){ max_w, (float)n * size * GP_LINE_HEIGHT };
}

/* ---------------------------------------------------------------- glyph cache */

static uint64_t glyph_key(int font, uint32_t glyph, uint32_t size_q, int bin, int lcd) {
    return ((uint64_t)(font + 1) << 52) | ((uint64_t)(lcd & 7) << 49) | ((uint64_t)(bin & 3) << 47) | ((uint64_t)(size_q & 0x7fff) << 32) | glyph;
}

static glyph_slot *glyph_find(gp_text_system *ts, uint64_t key) {
    size_t mask = ts->glyph_cap - 1;
    size_t i = (size_t)((key * 0x9E3779B97F4A7C15ull) >> 20) & mask;
    while (ts->glyphs[i].key && ts->glyphs[i].key != key) i = (i + 1) & mask;
    return &ts->glyphs[i];
}

static void glyph_table_grow(gp_text_system *ts) {
    glyph_slot *old = ts->glyphs;
    size_t old_cap = ts->glyph_cap;
    ts->glyph_cap *= 2;
    ts->glyphs = calloc(ts->glyph_cap, sizeof *ts->glyphs);
    for (size_t i = 0; i < old_cap; i++)
        if (old[i].key) *glyph_find(ts, old[i].key) = old[i];
    free(old);
}

/* Premultiplied BGRA (any size) -> premultiplied RGBA at `scale`, with an area filter: every
 * destination pixel averages exactly the source area it covers, which keeps a 136 px emoji
 * crisp at 18 px where nearest or bilinear sampling would alias badly. */
static void scale_bgra(const uint8_t *src, int sw, int sh, int pitch, float scale, uint8_t *dst,
                       int dw, int dh) {
    float inv = 1.f / scale;
    for (int y = 0; y < dh; y++) {
        float y0 = (float)y * inv, y1 = fminf((float)(y + 1) * inv, (float)sh);
        for (int x = 0; x < dw; x++) {
            float x0 = (float)x * inv, x1 = fminf((float)(x + 1) * inv, (float)sw);
            float acc[4] = { 0, 0, 0, 0 }, wsum = 0;
            for (int sy = (int)y0; sy < sh && (float)sy < y1; sy++) {
                float wy = fminf((float)(sy + 1), y1) - fmaxf((float)sy, y0);
                for (int sx = (int)x0; sx < sw && (float)sx < x1; sx++) {
                    float wgt = wy * (fminf((float)(sx + 1), x1) - fmaxf((float)sx, x0));
                    const uint8_t *p = src + (size_t)sy * (size_t)pitch + (size_t)sx * 4;
                    acc[0] += wgt * p[2]; /* BGRA -> RGBA */
                    acc[1] += wgt * p[1];
                    acc[2] += wgt * p[0];
                    acc[3] += wgt * p[3];
                    wsum += wgt;
                }
            }
            uint8_t *d = dst + ((size_t)y * (size_t)dw + (size_t)x) * 4;
            for (int c = 0; c < 4; c++) d[c] = wsum > 0 ? (uint8_t)fminf(255.f, acc[c] / wsum + 0.5f) : 0;
        }
    }
}

gp_glyph_entry gp_text_glyph(gp_text_system *ts, int font_id, uint32_t glyph, float px_size) {
    return gp_text_glyph_lcd(ts, font_id, glyph, px_size, 0, 0);
}

gp_glyph_entry gp_text_glyph_at(gp_text_system *ts, int font_id, uint32_t glyph, float px_size, int bin) {
    return gp_text_glyph_lcd(ts, font_id, glyph, px_size, bin, 0);
}

#ifdef GPUIC_HAVE_FONTCONFIG
/* The subpixel order the user's fontconfig setup says the display has. */
static int fontconfig_lcd_order(void) {
    int order = 0;
    FcPattern *pat = FcPatternCreate();
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    int rgba;
    if (FcPatternGetInteger(pat, FC_RGBA, 0, &rgba) == FcResultMatch)
        order = rgba == FC_RGBA_RGB ? 1 : rgba == FC_RGBA_BGR ? 2 : rgba == FC_RGBA_VRGB ? 3 : rgba == FC_RGBA_VBGR ? 4 : 0;
    FcPatternDestroy(pat);
    return order;
}
#endif

void gp_text_set_antialiasing(gp_text_system *ts, int mode) {
    if (mode < GP_AA_AUTO || mode > GP_AA_LCD_VBGR) mode = GP_AA_AUTO;
    if (ts->aa_mode == mode) return;
    ts->aa_mode = mode;
    ts->atlas_gen++;   /* what was painted for the old mode is out of date */
}

int gp_text_lcd_order(gp_text_system *ts, float scale) {
    switch (ts->aa_mode) {
    case GP_AA_GRAYSCALE: return 0;
    case GP_AA_LCD_RGB: return 1;
    case GP_AA_LCD_BGR: return 2;
    case GP_AA_LCD_VRGB: return 3;
    case GP_AA_LCD_VBGR: return 4;
    default: break;
    }
    if (scale >= 1.5f) return 0;   /* a high-density display: the stripes are too small to help */
    if (ts->system_lcd < 0) {
#ifdef GPUIC_HAVE_FONTCONFIG
        ts->system_lcd = fontconfig_lcd_order();
#else
        ts->system_lcd = 0;
#endif
    }
    return ts->system_lcd;
}

void gp_text_set_subpixel(gp_text_system *ts, bool on) { ts->no_subpixel = !on; }
void gp_text_set_gamma(gp_text_system *ts, float gamma) { ts->text_gamma = gamma < 1.f ? 1.f : gamma > 3.f ? 3.f : gamma; }
float gp_text_gamma(const gp_text_system *ts) { return ts->text_gamma; }
/* ---------------------------------------------------------------- outlines (for text inside an SVG) */

typedef struct { char *p; size_t n, cap; float ox, oy; bool open; } outline_out;

static void ol_put(outline_out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void ol_put(outline_out *o, const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (o->n + (size_t)n + 1 > o->cap) {
        o->cap = (o->n + (size_t)n + 1) * 2 + 256;
        o->p = realloc(o->p, o->cap);
        if (!o->p) abort();
    }
    memcpy(o->p + o->n, buf, (size_t)n + 1);
    o->n += (size_t)n;
}

#define OLX(o, v) ((double)(o)->ox + (double)(v).x / 64.0)
#define OLY(o, v) ((double)(o)->oy - (double)(v).y / 64.0)
static int ol_move(const FT_Vector *to, void *u) {
    outline_out *o = u;
    if (o->open) ol_put(o, "Z");
    ol_put(o, "M%.3f %.3f", OLX(o, *to), OLY(o, *to));
    o->open = true;
    return 0;
}
static int ol_line(const FT_Vector *to, void *u) { outline_out *o = u; ol_put(o, "L%.3f %.3f", OLX(o, *to), OLY(o, *to)); return 0; }
static int ol_conic(const FT_Vector *c, const FT_Vector *to, void *u) {
    outline_out *o = u;
    ol_put(o, "Q%.3f %.3f %.3f %.3f", OLX(o, *c), OLY(o, *c), OLX(o, *to), OLY(o, *to));
    return 0;
}
static int ol_cubic(const FT_Vector *c1, const FT_Vector *c2, const FT_Vector *to, void *u) {
    outline_out *o = u;
    ol_put(o, "C%.3f %.3f %.3f %.3f %.3f %.3f", OLX(o, *c1), OLY(o, *c1), OLX(o, *c2), OLY(o, *c2), OLX(o, *to), OLY(o, *to));
    return 0;
}

/* The shapes of `text` as SVG path data, in pixels, y down: the pen starts at (x0, y0) on the baseline. *advance is the text's width. Glyphs without an
 * outline (bitmap and color fonts, spaces) add nothing. NULL if there is no text system or nothing to draw; malloc'd. */
char *gp_text_outline_path(gp_text_system *ts, int64_t weight, float size, const char *text, size_t len, float x0, float y0, float *advance) {
    if (advance) *advance = 0;
    if (!ts || !text || !len || !(size > 0.f)) return NULL;
    int cap = (int)len + 8;
    gp_glyph_pos *g = malloc((size_t)cap * sizeof *g);
    if (!g) return NULL;
    float width = 0;
    int n = gp_text_shape(ts, weight, size, text, len, g, cap, &width);
    if (n > cap) {
        cap = n;
        g = realloc(g, (size_t)cap * sizeof *g);
        n = gp_text_shape(ts, weight, size, text, len, g, cap, &width);
    }
    if (advance) *advance = width;
    outline_out o = { 0 };
    FT_Outline_Funcs fn = { ol_move, ol_line, ol_conic, ol_cubic, 0, 0 };
    for (int i = 0; i < n && i < cap; i++) {
        if (g[i].font >= ts->nfonts) continue;
        gp_font *f = &ts->fonts[g[i].font];
        if (f->bitmap_only) continue;
        FT_Set_Char_Size(f->face, 0, (FT_F26Dot6)(size * 64.f + 0.5f), 72, 72);
        if (FT_Load_Glyph(f->face, g[i].glyph, FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING) != 0 || f->face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) continue;
        if (f->oblique) {
            FT_Matrix slant = { 0x10000, (FT_Fixed)(SYNTHETIC_OBLIQUE_SHEAR * 65536.f), 0, 0x10000 };
            FT_Outline_Transform(&f->face->glyph->outline, &slant);
        }
        if (f->embolden) FT_Outline_Embolden(&f->face->glyph->outline, (FT_Pos)(size * SYNTHETIC_BOLD_STRENGTH * 64.f + 0.5f));
        o.ox = x0 + g[i].x;
        o.oy = y0 + g[i].y;
        FT_Outline_Decompose(&f->face->glyph->outline, &fn, &o);
        if (o.open) { ol_put(&o, "Z"); o.open = false; }
    }
    free(g);
    return o.p;
}

void gp_text_set_gfx(gp_text_system *ts, gp_gfx *g) {
    ts->gfx = g;
    if (g) gp_gfx_set_text(g, ts);
}
gp_gfx *gp_text_gfx(const gp_text_system *ts) { return ts->gfx; }

void gp_text_snap_x(gp_text_system *ts, int font_id, float px_size, float x, int *pixel, int *bin) {
    bool exact = ts->no_subpixel || px_size > GP_SUBPIXEL_MAX_PX || font_id < 0 || font_id >= ts->nfonts ||
                 ts->fonts[font_id].bitmap_only;
    if (exact) {
        *pixel = (int)lroundf(x);
        *bin = 0;
        return;
    }
    float whole = floorf(x);
    int b = (int)floorf((x - whole) * GP_SUBPIXEL_BINS + 0.5f);
    if (b >= GP_SUBPIXEL_BINS) { b = 0; whole += 1; }   /* nearer the next pixel than its last bin */
    *pixel = (int)whole;
    *bin = b;
}

gp_glyph_entry gp_text_glyph_lcd(gp_text_system *ts, int font_id, uint32_t glyph, float px_size, int bin, int lcd) {
    gp_glyph_entry none = { 0 };
    if (font_id < 0 || font_id >= ts->nfonts) return none;
    gp_font *f = &ts->fonts[font_id];
    if (f->bitmap_only || bin < 0 || bin >= GP_SUBPIXEL_BINS) bin = 0;   /* a bitmap strike has one shape */
    if (f->bitmap_only || f->color || lcd < 0 || lcd > 4) lcd = 0;

    uint32_t size_q = (uint32_t)(px_size * 4.f + 0.5f);
    uint64_t key = glyph_key(font_id, glyph, size_q, bin, lcd);
    glyph_slot *gs = glyph_find(ts, key);
    if (gs->key) return gs->e;

    gp_glyph_entry e = { 0 };
    bool cache = true;
    if (f->bitmap_only) {
        /* the strike nearest above the target size, else the largest: always downscaled */
        int best = -1, largest = 0;
        for (int i = 0; i < f->face->num_fixed_sizes; i++) {
            int p = f->face->available_sizes[i].y_ppem >> 6;
            if (p > (f->face->available_sizes[largest].y_ppem >> 6)) largest = i;
            if ((float)p >= px_size && (best < 0 || p < (f->face->available_sizes[best].y_ppem >> 6))) best = i;
        }
        if (best < 0) best = largest;
        FT_Select_Size(f->face, best);
        if (FT_Load_Glyph(f->face, glyph, FT_LOAD_COLOR) == 0 &&
            f->face->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
            FT_GlyphSlot g = f->face->glyph;
            float ppem = (float)(f->face->available_sizes[best].y_ppem >> 6);
            float scale = px_size / (ppem > 0 ? ppem : px_size);
            int sw = (int)g->bitmap.width, sh = (int)g->bitmap.rows;
            int dw = (int)lroundf((float)sw * scale), dh = (int)lroundf((float)sh * scale);
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;
            int x, y;
            if (atlas_alloc(&ts->color, dw, dh, &x, &y)) {
                uint8_t *tmp = malloc((size_t)dw * dh * 4);
                scale_bgra(g->bitmap.buffer, sw, sh, g->bitmap.pitch, scale, tmp, dw, dh);
                for (int row = 0; row < dh; row++)
                    memcpy(ts->color.data + ((size_t)(y + row) * ts->color.w + (size_t)x) * 4,
                           tmp + (size_t)row * (size_t)dw * 4, (size_t)dw * 4);
                free(tmp);
                atlas_mark_dirty(&ts->color, x, y, dw, dh);
                e = (gp_glyph_entry){ (float)x, (float)y, (float)dw, (float)dh,
                                      (float)lroundf((float)g->bitmap_left * scale),
                                      (float)lroundf((float)g->bitmap_top * scale), true, false };
            } else {
                cache = false; /* atlas full: retried after the reset */
            }
        }
    } else {
        FT_Set_Char_Size(f->face, 0, (FT_F26Dot6)(px_size * 64.f + 0.5f), 72, 72);
        bool loaded = FT_Load_Glyph(f->face, glyph, FT_LOAD_TARGET_LIGHT) == 0;
        if (loaded && f->oblique && f->face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {   /* x += shear * y, about the baseline */
            FT_Matrix slant = { 0x10000, (FT_Fixed)(SYNTHETIC_OBLIQUE_SHEAR * 65536.f), 0, 0x10000 };
            FT_Outline_Transform(&f->face->glyph->outline, &slant);
        }
        if (loaded && f->embolden && f->face->glyph->format == FT_GLYPH_FORMAT_OUTLINE)
            FT_Outline_Embolden(&f->face->glyph->outline, (FT_Pos)(px_size * SYNTHETIC_BOLD_STRENGTH * 64.f + 0.5f));
        /* the glyph as it looks with its origin `bin` quarters of a pixel to the right (light hinting leaves x alone) */
        if (loaded && bin && f->face->glyph->format == FT_GLYPH_FORMAT_OUTLINE)
            FT_Outline_Translate(&f->face->glyph->outline, (FT_Pos)(bin * 64 / GP_SUBPIXEL_BINS), 0);
        bool done = false;
        const bool vertical = lcd >= 3;   /* the stripes are rows: three samples per pixel down, not across */
        if (loaded && lcd && f->face->glyph->format == FT_GLYPH_FORMAT_OUTLINE &&
            FT_Render_Glyph(f->face->glyph, vertical ? FT_RENDER_MODE_LCD_V : FT_RENDER_MODE_LCD) == 0 &&
            f->face->glyph->bitmap.pixel_mode == (vertical ? FT_PIXEL_MODE_LCD_V : FT_PIXEL_MODE_LCD)) {
            /* three samples per pixel: the glyph's size in pixels is a third of the bitmap's, across or down */
            FT_GlyphSlot g = f->face->glyph;
            int w = vertical ? (int)g->bitmap.width : (int)g->bitmap.width / 3, h = vertical ? (int)g->bitmap.rows / 3 : (int)g->bitmap.rows, x, y;
            const bool swap = lcd == 2 || lcd == 4;   /* BGR: the stripes run the other way */
            done = true;
            if (w > 0 && h > 0) {
                if (atlas_alloc(&ts->lcd, w, h, &x, &y)) {
                    const size_t pitch = (size_t)g->bitmap.pitch;
                    for (int row = 0; row < h; row++) {
                        uint8_t *dst = ts->lcd.data + ((size_t)(y + row) * ts->lcd.w + (size_t)x) * 4;
                        for (int col = 0; col < w; col++) {
                            uint8_t s0, s1, s2;
                            if (vertical) {
                                const uint8_t *src = g->bitmap.buffer + (size_t)(row * 3) * pitch + (size_t)col;
                                s0 = src[0]; s1 = src[pitch]; s2 = src[2 * pitch];
                            } else {
                                const uint8_t *src = g->bitmap.buffer + (size_t)row * pitch + (size_t)col * 3;
                                s0 = src[0]; s1 = src[1]; s2 = src[2];
                            }
                            dst[col * 4 + 0] = swap ? s2 : s0;
                            dst[col * 4 + 1] = s1;
                            dst[col * 4 + 2] = swap ? s0 : s2;
                            dst[col * 4 + 3] = 255;
                        }
                    }
                    atlas_mark_dirty(&ts->lcd, x, y, w, h);
                    e = (gp_glyph_entry){ (float)x, (float)y, (float)w, (float)h, (float)g->bitmap_left, (float)g->bitmap_top, false, true };
                } else {
                    cache = false;
                }
            }
        }
        if (!done && loaded && FT_Render_Glyph(f->face->glyph, FT_RENDER_MODE_NORMAL) == 0) {
            FT_GlyphSlot g = f->face->glyph;
            int w = (int)g->bitmap.width, h = (int)g->bitmap.rows, x, y;
            if (w > 0 && h > 0) {
                if (atlas_alloc(&ts->alpha, w, h, &x, &y)) {
                    for (int row = 0; row < h; row++)
                        memcpy(ts->alpha.data + (size_t)(y + row) * ts->alpha.w + (size_t)x,
                               g->bitmap.buffer + (size_t)row * (size_t)g->bitmap.pitch, (size_t)w);
                    atlas_mark_dirty(&ts->alpha, x, y, w, h);
                    e = (gp_glyph_entry){ (float)x, (float)y, (float)w, (float)h,
                                          (float)g->bitmap_left, (float)g->bitmap_top, false, false };
                } else {
                    cache = false;
                }
            }
        }
    }
    if (!cache) return none;

    if ((ts->glyph_count + 1) * 2 > ts->glyph_cap) {
        glyph_table_grow(ts);
        gs = glyph_find(ts, key);
    }
    gs->key = key;
    gs->e = e;
    ts->glyph_count++;
    return e;
}

const uint8_t *gp_text_atlas(gp_text_system *ts) { return ts->alpha.data; }
const uint8_t *gp_text_color_atlas(gp_text_system *ts) { return ts->color.data; }
bool gp_text_atlas_take_dirty(gp_text_system *ts, int *x, int *y, int *w, int *h) {
    return atlas_take_dirty(&ts->alpha, x, y, w, h);
}
bool gp_text_color_atlas_take_dirty(gp_text_system *ts, int *x, int *y, int *w, int *h) {
    return atlas_take_dirty(&ts->color, x, y, w, h);
}
const uint8_t *gp_text_lcd_atlas(gp_text_system *ts) { return ts->lcd.data; }
bool gp_text_lcd_atlas_take_dirty(gp_text_system *ts, int *x, int *y, int *w, int *h) {
    return atlas_take_dirty(&ts->lcd, x, y, w, h);
}

bool gp_text_atlas_overflowed(const gp_text_system *ts) {
    return ts->alpha.overflowed || ts->color.overflowed || ts->lcd.overflowed;
}

/* Glyph entries handed out earlier point into the old atlases, so this is only safe between
 * frames: a frame that ran out of room draws without the glyphs that did not fit, and the next
 * one starts from scratch. */
uint32_t gp_text_atlas_generation(const gp_text_system *ts) { return ts->atlas_gen; }

void gp_text_atlas_reset(gp_text_system *ts) {
    ts->atlas_gen++;
    atlas_reset(&ts->alpha);
    atlas_reset(&ts->color);
    atlas_reset(&ts->lcd);
    memset(ts->glyphs, 0, ts->glyph_cap * sizeof *ts->glyphs);
    ts->glyph_count = 0;
}

/* ---------------------------------------------------------------- font registration */

/* Font choices were made with a different set of fonts: forget them (the fonts stay loaded, and
 * glyphs already in the atlases stay valid because font ids never change). */
static void fonts_changed(gp_text_system *ts) {
    for (int i = 0; i < ts->nsysmemo; i++) free(ts->sysmemo[i].name);
    ts->nsysmemo = 0;
    for (int i = 0; i < ts->nstyles; i++) {
        ts->styles[i].built = false;
        ts->styles[i].nchain = 0;
        ts->styles[i].nfallback = 0;
    }
    ts->emoji = -1;
    for (int i = 0; i < RESOLVE_CACHE; i++) ts->resolve[i].valid = false;
    for (int i = 0; i < SHAPE_CACHE; i++) {
        free(ts->shape_cache[i].text);
        free(ts->shape_cache[i].glyphs);
        ts->shape_cache[i].text = NULL;
        ts->shape_cache[i].glyphs = NULL;
        ts->shape_cache[i].used = false;
    }
}

static bool register_font(gp_text_system *ts, int id, int weight, const char *family) {
    if (id < 0) return false;
    if (weight) ts->fonts[id].weight = weight;
    if (family && *family) {
        free(ts->fonts[id].family);
        ts->fonts[id].family = strdup(family);
    }
    for (int i = 0; i < ts->nregistered; i++)
        if (ts->registered[i] == id) { fonts_changed(ts); return true; } /* the same file again: only the weight may differ */
    if (ts->nregistered == ts->cap_registered) {
        ts->cap_registered = ts->cap_registered ? ts->cap_registered * 2 : 8;
        ts->registered = realloc(ts->registered, (size_t)ts->cap_registered * sizeof *ts->registered);
    }
    ts->registered[ts->nregistered++] = id;
    fonts_changed(ts);
    return true;
}

static bool weight_ok(int weight) { return weight == 0 || (weight >= 100 && weight <= 900); }

bool gp_text_add_font_file_as(gp_text_system *ts, const char *path, const char *family, int weight, int face_index) {
    if (!ts || !path || face_index < 0 || !weight_ok(weight)) return false;
    return register_font(ts, font_add(ts, path, face_index), weight, family);
}

bool gp_text_add_font_file(gp_text_system *ts, const char *path, int weight, int face_index) {
    return gp_text_add_font_file_as(ts, path, NULL, weight, face_index);
}

bool gp_text_add_font_memory(gp_text_system *ts, const void *data, size_t size, int weight, int face_index) {
    return gp_text_add_font_memory_as(ts, data, size, NULL, weight, face_index);
}

bool gp_text_add_font_memory_as(gp_text_system *ts, const void *data, size_t size, const char *family, int weight,
                                int face_index) {
    if (!ts || !data || !size || size > 0x7fffffffu || face_index < 0 || !weight_ok(weight)) return false;
    void *copy = malloc(size);
    if (!copy) return false;
    memcpy(copy, data, size);
    char name[32];
    snprintf(name, sizeof name, "memory:%d", ts->nmemory);
    int id = font_add_ex(ts, name, face_index, copy, size);
    if (id < 0) {
        free(copy);
        return false;
    }
    ts->nmemory++;
    return register_font(ts, id, weight, family);
}

void gp_text_use_system_fonts(gp_text_system *ts, bool enable) {
    if (ts->no_system == !enable) return;
    ts->no_system = !enable;
    fonts_changed(ts);
}

gp_font_scan_stats gp_text_scan_stats(const gp_text_system *ts) {
#ifdef GPUIC_HAVE_FONTCONFIG
    (void)ts;
    return (gp_font_scan_stats){ 0 };
#else
    return ts->scan_stats;
#endif
}

int gp_text_font_count(const gp_text_system *ts) { return ts->nfonts; }
const char *gp_text_font_path(const gp_text_system *ts, int font) {
    return font >= 0 && font < ts->nfonts ? ts->fonts[font].path : "";
}
