#ifndef GPUIC_INTERNAL_H
#define GPUIC_INTERNAL_H

#include "gpuic/gpuic.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- arena */

typedef struct gp_arena_chunk gp_arena_chunk;
typedef struct {
    gp_arena_chunk *head, *cur;
    size_t chunk_size;   /* 0 = default */
} gp_arena;

void *gp_arena_alloc(gp_arena *a, size_t size, size_t align); /* zeroed */
void gp_arena_reset(gp_arena *a);                             /* keeps memory */
void gp_arena_free(gp_arena *a);
char *gp_arena_strdup(gp_arena *a, const char *s);

/* ---------------------------------------------------------------- style / elements */

typedef enum { GP_AUTO = 0, GP_PX, GP_FRAC } gp_length_kind;
typedef struct { uint8_t kind; float v; } gp_length;

enum { GP_DIR_COLUMN = 0, GP_DIR_ROW = 1 };
enum { GP_ALIGN_AUTO = 0, GP_ALIGN_STRETCH, GP_ALIGN_START, GP_ALIGN_CENTER, GP_ALIGN_END };
enum { GP_JUSTIFY_START = 0, GP_JUSTIFY_CENTER, GP_JUSTIFY_END, GP_JUSTIFY_BETWEEN,
       GP_JUSTIFY_AROUND, GP_JUSTIFY_EVENLY };
enum { GP_TEXT_ALIGN_LEFT = 0, GP_TEXT_ALIGN_CENTER, GP_TEXT_ALIGN_RIGHT, GP_TEXT_ALIGN_START,
       GP_TEXT_ALIGN_END };
enum { GP_CURSOR_DEFAULT = 0, GP_CURSOR_POINTER, GP_CURSOR_TEXT, GP_CURSOR_COUNT };

typedef struct gp_style {
    uint8_t dir, align_items, align_self, justify;
    uint8_t absolute, overflow_hidden, cursor;
    uint16_t font_weight;  /* 1..1000, 0 = inherit */
    uint8_t font_italic;   /* 0 = inherit, 1 = italic, 2 = upright */
    uint8_t text_align;    /* only meaningful if has_text_align */
    bool has_bg, has_text_color, has_text_align, has_hover_bg, has_active_bg, has_shadow, has_text_dir;
    uint8_t text_dir;      /* GP_BIDI_*, valid if has_text_dir (not the flex `dir` above) */
    bool has_focus_bg, has_focus_border, whitespace_pre;
    uint8_t overflow_x_scroll, overflow_y_scroll, hide_scrollbar, autohide_scrollbar;
    bool has_scrollbar_color;

    float grow, shrink;
    gp_length basis, w, h, min_w, min_h, max_w, max_h;
    gp_length inset[4];    /* left, top, right, bottom */
    float pad[4], margin[4]; /* left, top, right, bottom */
    uint8_t margin_auto;     /* bit per side (left 1, top 2, right 4, bottom 8): the margin is auto (flex items), its width in `margin` is 0 */
    bool wrap;               /* gp_flex_wrap */
    uint8_t align_content;   /* 0: stretch (the default), else 1 + GP_JUSTIFY_*: where the lines of a wrapped container go across it */
    float gap;
    float border_w, radius, opacity;
    /* Per-side borders and per-corner radii. A per-side border is folded into the padding (pad = pad_user + border) so that layout,
     * which only knows `pad` and the uniform `border_w`, is unchanged: with any side set, border_w is 0 and border_side holds the
     * widths painted. gp_border (all sides) and gp_rounded (all corners) clear their overrides, so the last one wins, as in CSS. */
    float pad_user[4];     /* left, top, right, bottom as the app set them */
    float border_uniform;  /* gp_border */
    float border_side[4];  /* gp_border_left...; used where border_set has the bit */
    uint8_t border_set;    /* bit 0 left, 1 top, 2 right, 3 bottom */
    bool has_border_sides;
    float corner[4];       /* top-left, top-right, bottom-right, bottom-left, where corner_set has the bit */
    uint8_t corner_set;
    /* Images and SVGs: filters, applied in the shader. Zero means "unchanged" for the four deltas. */
    float img_brightness, img_contrast, img_saturate;   /* value - 1 */
    float img_grayscale, img_blur;
    gp_color img_tint;     /* multiplies; used when has_img_tint */
    bool has_img_tint;
    float font_size;       /* 0 = inherit */
    const char *font_family; /* a comma-separated list, NULL = inherit */
    const char *font_features; /* OpenType features, NULL = inherit ("normal" clears) */
    const char *font_variations; /* font-variation-settings, NULL = inherit ("normal" clears) */
    const struct gp_gradient *bg_gradient;   /* replaces bg (arena of the element) */
    uint8_t object_fit;    /* GP_FIT_* (images) */
    bool image_pixelated;

    uint8_t border_style;  /* 0 solid, 1 dashed, 2 dotted */
    gp_color bcolor[4];    /* left, top, right, bottom, where bcolor_set has the bit; the others have border_color */
    uint8_t bcolor_set;
    const struct gp_gradient *border_gradient;   /* the border painted with this (arena of the element), or NULL */
    /* a paint-time transform (gp_rotate, gp_scale, gp_translate): about the point (origin_x, origin_y) of the box (fractions), the scale, then the rotation, then the move */
    /* a stroked curve (GP_EL_CURVE) */
    float stroke_w, hit_tol, miter_limit;
    gp_color stroke_color;
    bool has_stroke_color;
    uint8_t stroke_cap, stroke_join;   /* 0 round, 1 butt / 0 round, 1 miter */
    bool has_xf;
    float xf_rot, xf_sx, xf_sy, xf_tx, xf_ty, xf_ox, xf_oy;   /* degrees; scale; px; fractions of the box (0.5: the middle) */
    const struct gp_style *hover_style, *active_style, *focus_style;   /* gp_hover_style...: what the state paints over this style (arena of the element) */
    gp_color bg, border_color, text_color, hover_bg, active_bg, focus_bg, focus_border;
    gp_color scrollbar_color;
    struct { float dx, dy, blur; gp_color color; } shadow;
} gp_style;

/* Resolved, inherited text properties. */
typedef struct {
    float size;
    int weight;            /* 1..1000 */
    int family;            /* gp_text_family id; 0 = the default */
    gp_color color;
    uint8_t align;
    uint8_t dir;           /* GP_BIDI_*: requested paragraph direction */
    bool italic;
    int features;          /* gp_text_features id; 0 = none */
    int variations;        /* gp_text_variations id; 0 = none */
} gp_text_style;

enum { GP_TAG_END = 1, GP_TAG_ELEMENT, GP_TAG_MOD };
enum { GP_EL_DIV = 1, GP_EL_TEXT, GP_EL_LIST, GP_EL_IMAGE, GP_EL_SVG, GP_EL_CURVE };

/* Type-erased callback; cast back to its real type (gp_mouse_fn, gp_key_fn, ...) to call. */
typedef void (*gp_any_fn)(void);

typedef struct {
    gp_any_fn fn;
    void *data;
    gp_entity owner;
} gp_handler;

typedef struct gp_action_handler gp_action_handler;
struct gp_action_handler {
    const char *name;
    gp_action_fn fn;
    void *data;
    gp_entity owner;
    gp_action_handler *next;
};

/* Build context: set while a view's render function runs. */
typedef struct {
    gp_arena *arena;
    gp_app *app;
    gp_window *window;
    gp_entity view;      /* the view whose render function is running (the parent of views built now) */
    bool no_cache;       /* rows of a list: views in them are rebuilt with the row, never reused */
} gp_build_ctx;

typedef struct gp_el gp_el;
typedef struct gp_anim_table gp_anim_table;
typedef struct gp_vviews gp_vviews;
struct gp_node { uint8_t tag; };

/* What a subtree contains that makes its layout or its painting depend on more than its elements. */
enum {
    GP_SUB_LIST = 1,    /* a gp_list / gp_uniform_list: size depends on rows measured so far */
    GP_SUB_SCROLL = 2,  /* a scroll container: offsets live in the window's table and change without a rebuild */
    GP_SUB_HOVER = 4,   /* hover / pressed styles: painting depends on the mouse */
    GP_SUB_FOCUS = 8,   /* focusable elements: painting depends on which has focus */
    GP_SUB_AUTOHIDE = 16,   /* scrollbars that fade: painting depends on the window's scroll_vis_gen */
    GP_SUB_CLIP = 32,   /* overflow hidden: the instances below are clipped to a box that moves with the subtree */
    GP_SUB_XF = 64,     /* a transform: the instances below carry its matrix */
    GP_SUB_DYN_LAYOUT = GP_SUB_LIST | GP_SUB_SCROLL
};

/* A gp_with_animation on an element (anim.c). The window keeps when each named animation started; this
 * is what the element knows. The first of an element's animations also holds the style they are applied to. */
typedef struct gp_anim gp_anim;
struct gp_anim {
    uint64_t key;             /* hash of the id and the position in the element's list */
    gp_animation spec;
    gp_anim_style_fn fn;
    void *data;
    gp_entity view;           /* the view whose render function built it (for handlers an animation might add) */
    gp_anim *next;
    bool applied;             /* its modifiers are on the element's style, for progress applied_t */
    float applied_t, cur_t;
    gp_style base;            /* first animation only: the element's style before any animation touched it */
    bool base_valid;
};

struct gp_el {
    gp_node base;
    uint8_t kind;
    gp_style style;
    const char *text;
    gp_image image;        /* GP_EL_IMAGE */
    gp_svg svg;            /* GP_EL_SVG */
    gp_point *curve_pts;   /* GP_EL_CURVE: the points, relative to the element's box (4 for a Bezier, any number for a polyline) */
    uint32_t curve_n;
    uint8_t curve_kind;    /* 0 a cubic Bezier, 1 a polyline */
    gp_el **children;
    uint32_t nchildren;
    gp_handler on_click, on_mouse_down, on_key_down, on_key_up, on_text;
    gp_handler on_drag, on_mouse_up, on_ime, on_layout, on_scroll, on_wheel;
    float ime_rect[4];     /* element-relative caret rect for the IME candidate window */
    bool has_ime_rect;
    gp_action_handler *actions;
    const char *key_context;
    uint64_t focus_id;     /* 0 = not focusable */
    bool tab_stop;
    int tab_index;
    uint64_t scroll_id;    /* explicit gp_scroll handle, 0 = implicit */
    uint64_t scroll_key;   /* resolved at layout: handle id or path hash */
    gp_point scroll;       /* current offset */
    gp_point scroll_max;
    uint64_t path;         /* hash of the position in the tree (set at layout) */

    /* uniform list: rows are built at layout time, see layout.c */
    gp_list_item_fn list_fn;
    void *list_data;
    size_t list_count;
    float list_item_h;     /* as requested; <= 0 = measure row 0 */
    float list_ih;         /* resolved row height (0 = not yet) */
    gp_build_ctx list_bc;
    bool list_realized;
    size_t list_first, list_n;
    bool list_var;         /* gp_list: rows have their own heights (vlist.c, layout.c) */
    bool list_horiz;       /* gp_list_horizontal: a uniform list laid out along x (list_ih is the items' width) */
    gp_list_group_fn sticky_fn;   /* gp_list_sticky: which header stays at the top, asked at layout and on every scroll */
    void *sticky_data;
    gp_el *sticky;         /* the header pinned now (made by the layout of the list, in its header arena), or NULL; drawn unscrolled, above the rows */
    bool list_stick;       /* gp_list_stick_to_end */
    bool list_head, list_tail; /* the realized rows start at row 0 / end at the last row */
    /* A gp_list can be far longer than a float can place pixels in, so its rows are positioned
     * relative to the first realized row and `scroll.y` is relative to that too: the logical
     * offset (what the scroll bar and gp_scroll_offset report) is  scroll_base_y + scroll.y. */
    double scroll_base_y, scroll_base_x;   /* (x for a gp_list that runs along x) */
    float list_bb;             /* height of the realized rows together */
    float list_anchor_rel;     /* scroll.y at which the saved anchor (gp_vlist.a_idx/a_off) holds */

    /* Root of a cached view's elements (gp_child_view_cached): the entity, and what layout and paint
     * remember about the subtree so that, while nothing it depends on changed, neither redoes it. */
    gp_entity cached_view;     /* generation 0 = not such a root */
    uint8_t sub_flags;         /* GP_SUB_*: what the subtree below contains (set by prepare) */
    bool prep_valid;           /* prepare's results (path, text style, flags) hold for the whole subtree */
    bool lk_valid;             /* the subtree is laid out for the inputs in lk[] */
    float lk[4];
    uint32_t lk_ext;           /* the scroll table's ext_gen when it was laid out (roots with scrollers or lists) */
    uint32_t lk_meas;          /* gp_vlist_generation() when that layout began: rows it measured make the next one differ */
    uint32_t layout_gen;       /* bumped whenever the subtree is laid out */
    uint32_t dyn_gen;          /* bumped when a scroller below moved in place (wheel, thumb): painting changed, layout did not */
    float ext[4];              /* x0 y0 x1 y1 of everything below that can show hover / a scrollbar, relative to the root's origin */
    float thumb_alpha;         /* auto-hiding scrollbars: how visible the thumbs are now (scroll.c, set before every paint) */
    bool hide_below;           /* this element or one below it has auto-hiding scrollbars (lists always: their rows are built later) */
    gp_anim *anim;             /* gp_with_animation, in order */
    bool anim_below;           /* this element or one below it animates (lists always: their rows are built later) */

    /* layout output: position relative to parent's border box, border-box size */
    gp_point pos;
    gp_size size;
    gp_text_style ts;
    struct { float key[4]; gp_size out; } cache[4];
    uint8_t ncache;
};

typedef struct {
    gp_node base;
    int kind;
    float f[4];
    uint8_t mask;
    gp_color c;
    gp_length len;
    gp_any_fn fn;
    void *data;
    uint64_t id;
    const char *name;
} gp_mod;


gp_el *gp_el_new(gp_arena *a, int kind);
void gp_style_init(gp_style *s);
/* Builds a view's element tree. `cached`: reuse the tree from the last build if the view and
 * everything below it is clean (see views.c). */
gp_el *gp_build_view(gp_build_ctx *bc, gp_entity view, bool cached);
gp_entity gp_building_view(void);   /* the view whose render function is running, or null */

typedef enum { GP_HIT_CLICK, GP_HIT_MOUSE_DOWN, GP_HIT_CURSOR, GP_HIT_FOCUS, GP_HIT_WHEEL } gp_hit_kind;
typedef struct { gp_el *el; uint64_t path; float ox, oy; /* element's absolute origin */ float qx, qy; /* where the point is, in the element's own coordinates (as ox, oy: layout coordinates) */ } gp_hit;
bool gp_hit_test(gp_el *root, gp_point p, gp_hit_kind kind, gp_hit *out);
bool gp_hit_test_skipping(gp_el *root, gp_point p, gp_hit_kind kind, gp_hit *out, const uint64_t *skip, int nskip);   /* not the elements whose path is in `skip` */

/* ---------------------------------------------------------------- text */

#define GP_ATLAS_SIZE 2048       /* R8 coverage atlas */
#define GP_COLOR_ATLAS_SIZE 1024 /* RGBA atlas for color glyphs (emoji) */
#define GP_LCD_ATLAS_SIZE 1024   /* RGBA atlas for LCD glyphs: red, green and blue coverage (alpha unused) */
#define GP_LINE_HEIGHT 1.3f

typedef struct gp_text_system gp_text_system;
typedef struct { uint32_t start, end; float width; } gp_text_line;
/* One shaped glyph. With HarfBuzz a glyph can stand for several codepoints (a ligature, an emoji
 * sequence) and several glyphs can come from one: `cluster` is the byte offset in the shaped
 * text of the cluster the glyph belongs to. Glyph ids are only meaningful with their `font`. */
typedef struct {
    uint32_t glyph;
    uint16_t font;
    uint8_t level;         /* bidi embedding level of the run: odd = right-to-left */
    uint32_t cluster;
    float x;               /* pen position of this glyph, relative to the start of the text */
    float y;               /* vertical offset from the baseline, down is positive */
    float advance;
} gp_glyph_pos;
typedef struct {
    float u, v, w, h;      /* atlas texels (of the color atlas if `color`) */
    float left, top;       /* device px: from pen origin to bitmap's left / top edge */
    bool color;            /* RGBA, already premultiplied: not tinted by the text color */
    bool lcd;              /* per-channel coverage in the LCD atlas (see gp_text_glyph_lcd) */
} gp_glyph_entry;

gp_text_system *gp_text_system_new(void);
void gp_text_system_free(gp_text_system *ts);

/* Which font to draw with is one int, the `weight` argument of the functions below: a weight 1..1000 in the
 * low 16 bits (0 = 400) and a family id from gp_text_family above them. A plain weight such as 400 is that
 * weight of the default family. */
#define GP_FONT_ITALIC 0x8000   /* OR it into the weight to ask for italic */
#define GP_FONT_SEL(family, weight) ((int64_t)((unsigned)(family) << 16 | ((unsigned)(weight) & 0xffff)))
/* the OpenType feature list (gp_text_features) lives above them */
#define GP_FONT_FEATURES(id) ((int64_t)(id) << 32)
#define GP_FONT_VARIATIONS(id) ((int64_t)(id) << 48)   /* (the font-variation-settings list: 15 bits above the features) */
/* The selector of a resolved text style. */
#define GP_TEXT_SEL(ts) (GP_FONT_SEL((ts).family, (ts).weight | ((ts).italic ? GP_FONT_ITALIC : 0)) | GP_FONT_FEATURES((ts).features) | GP_FONT_VARIATIONS((ts).variations))
/* Interns an OpenType feature list ("tnum, liga=0, ss01"; HarfBuzz's syntax, minus ranges) and returns its id.
 * NULL, "" and "normal" are no features, id 0. */
int gp_text_features(gp_text_system *ts, const char *list);
int gp_text_variations(gp_text_system *ts, const char *list);   /* interns a font-variation-settings list: "wdth" 75, "opsz" 14; NULL, "" and "normal" are 0 */
/* Interns a font-family property ("Inter, Noto Sans, sans-serif"; NULL or "" is the default) and returns its id. */
int gp_text_family(gp_text_system *ts, const char *list);

/* Lays text out into lines, wrapping at word boundaries when wrap_w >= 0.
 * With `pre`, whitespace is kept and lines only break at '\n'. `lines`/`nlines`
 * may be NULL. Lines are allocated from `arena`. */
gp_size gp_text_measure(gp_text_system *ts, int64_t weight, float size, const char *text,
                        float wrap_w, bool pre, gp_arena *arena, gp_text_line **lines,
                        int *nlines);
/* `dir` is the requested paragraph direction (GP_BIDI_*); AUTO resolves per paragraph. */
gp_size gp_text_measure_dir(gp_text_system *ts, int64_t weight, float size, const char *text,
                            float wrap_w, bool pre, int dir, gp_arena *arena,
                            gp_text_line **lines, int *nlines);
/* Line-break rule for CJK and other space-less text (kinsoku): may a line break between these? */
bool gp_cjk_break_allowed(uint32_t before, uint32_t after);

/* Paragraph direction: from the first strong character (UAX #9 rules P2/P3), or forced. */
enum { GP_BIDI_AUTO = 0, GP_BIDI_LTR, GP_BIDI_RTL };
/* Could this codepoint be, or affect, right-to-left text? (A cheap test, not a classification.) */
bool gp_text_may_be_rtl(uint32_t cp);

/* Grapheme clusters (UAX #29 with the Indic conjunct rule, GB9c): the properties of a codepoint, and a state that is fed the codepoints
 * of a text from the start of a cluster and says for each whether it continues the cluster. */
enum { GP_GB_EXTEND = 1, GP_GB_ZWJ = 2, GP_GB_SPACING = 4, GP_GB_PREPEND = 8, GP_GB_EXTPICT = 16, GP_GB_CONSONANT = 32, GP_GB_LINKER = 64,
       GP_GB_RI = 128, GP_GB_L = 256, GP_GB_V = 512, GP_GB_T = 1024, GP_GB_LV = 2048, GP_GB_LVT = 4096, GP_GB_CONTROL = 8192 };
typedef struct { unsigned prev; unsigned char pict, incb, ri; } gp_gstate;
unsigned gp_grapheme_props(uint32_t cp);
void gp_gstate_start(gp_gstate *g, uint32_t first_cp);
bool gp_gstate_joins(gp_gstate *g, uint32_t cp);
/* Is a paragraph with this text (and this requested direction) right-to-left? */
bool gp_text_is_rtl(const char *text, size_t len, int dir);

/* Shapes text[0..len) (itemized by font, HarfBuzz per run). Returns the number of glyphs, which
 * may exceed `cap`; at most `cap` are written to `out` (which may be NULL). `width` may be NULL. */
int gp_text_shape(gp_text_system *ts, int64_t weight, float size, const char *text, size_t len,
                  gp_glyph_pos *out, int cap, float *width);
/* The same, for one paragraph (or one line of it) with an explicit direction. Glyphs come out in
 * VISUAL order, left to right, with `cluster` still the logical byte offset: the reordering of
 * UAX #9 (rules L1, L2) has been applied, so a Hebrew word arrives reversed. Every glyph of a
 * paragraph that wraps must be shaped with the paragraph's direction, not re-detected per line. */
int gp_text_shape_dir(gp_text_system *ts, int64_t weight, float size, const char *text, size_t len,
                      int dir, gp_glyph_pos *out, int cap, float *width);
void gp_text_metrics(gp_text_system *ts, int64_t weight, float size, float *ascent, float *descent);
gp_glyph_entry gp_text_glyph(gp_text_system *ts, int font, uint32_t glyph, float px_size);   /* at a whole pixel */
/* Subpixel positioning: a glyph is rasterized for the fraction of a pixel its pen origin falls on, in
 * GP_SUBPIXEL_BINS steps (0 = on the pixel), so spacing is even instead of rounded glyph by glyph. gp_text_snap_x
 * splits a pen position in device pixels into the pixel to draw at and the bin to ask for; it rounds (bin 0) where
 * subpixel positions do not apply (color bitmap fonts, large sizes, or when switched off). */
#define GP_SUBPIXEL_BINS 4
#define GP_SUBPIXEL_MAX_PX 40.f   /* above this size a fraction of a pixel is not visible: one rasterization per glyph */
gp_glyph_entry gp_text_glyph_at(gp_text_system *ts, int font, uint32_t glyph, float px_size, int bin);
void gp_text_snap_x(gp_text_system *ts, int font, float px_size, float x, int *pixel, int *bin);
/* LCD antialiasing: instead of one coverage value per pixel, a glyph gets one per color channel, sampled at a third of a
 * pixel apart, and is blended per channel (so a stem edge falls on the pixel's own red, green or blue stripe).
 * `lcd` is the subpixel order to draw for: 0 none (grayscale), 1 RGB, 2 BGR. It applies to outline fonts; color
 * glyphs and bitmap strikes stay as they are. gp_text_lcd_order says what to ask for at a window's scale:
 * GP_AA_AUTO takes the system's setting (fontconfig's rgba; none without fontconfig) and only below 1.5x scale. */
gp_glyph_entry gp_text_glyph_lcd(gp_text_system *ts, int font, uint32_t glyph, float px_size, int bin, int lcd);
void gp_text_set_antialiasing(gp_text_system *ts, int mode);   /* GP_AA_*; GPUIC_LCD=off|rgb|bgr|auto at startup */
int gp_text_lcd_order(gp_text_system *ts, float scale);
void gp_text_set_subpixel(gp_text_system *ts, bool on);
void gp_text_set_gamma(gp_text_system *ts, float gamma);
float gp_text_gamma(const gp_text_system *ts);   /* default on; GPUIC_SUBPIXEL=0 starts a text system with it off */

/* Atlas access for the renderer. */
const uint8_t *gp_text_atlas(gp_text_system *ts);
bool gp_text_atlas_take_dirty(gp_text_system *ts, int *x, int *y, int *w, int *h);
const uint8_t *gp_text_lcd_atlas(gp_text_system *ts);
bool gp_text_lcd_atlas_take_dirty(gp_text_system *ts, int *x, int *y, int *w, int *h);
const uint8_t *gp_text_color_atlas(gp_text_system *ts);
bool gp_text_color_atlas_take_dirty(gp_text_system *ts, int *x, int *y, int *w, int *h);
/* True once a glyph did not fit: finish the frame, then call gp_text_atlas_reset(). */
bool gp_text_atlas_overflowed(const gp_text_system *ts);
void gp_text_atlas_reset(gp_text_system *ts);
/* Changes whenever the atlas is cleared: atlas coordinates remembered from before are stale. */
uint32_t gp_text_atlas_generation(const gp_text_system *ts);
/* Font registration (the public gp_font_* functions in app.c wrap these). Registered fonts are
 * preferred over system ones; weight 0 reads it from the font. Memory is copied. */
bool gp_text_add_font_file(gp_text_system *ts, const char *path, int weight, int face_index);
bool gp_text_add_font_memory(gp_text_system *ts, const void *data, size_t size, int weight, int face_index);
/* The same with the family the font is registered under (NULL: the one it names itself). */
bool gp_text_add_font_file_as(gp_text_system *ts, const char *path, const char *family, int weight, int face_index);
bool gp_text_add_font_memory_as(gp_text_system *ts, const void *data, size_t size, const char *family, int weight,
                                int face_index);
void gp_text_use_system_fonts(gp_text_system *ts, bool enable);
/* What the last system font scan did (all zero when fontconfig is used instead). */
typedef struct {
    int files;            /* font files found by the directory walk */
    int opened;           /* of those, opened with FreeType (the rest came from the cache) */
    int from_cache;
    int faces;            /* usable faces known after the scan */
    bool cache_loaded;    /* a valid cache file was read */
    bool cache_written;   /* and a new one was written */
    double ms;
} gp_font_scan_stats;
gp_font_scan_stats gp_text_scan_stats(const gp_text_system *ts);
/* Introspection, for tests and diagnostics. */
int gp_text_font_count(const gp_text_system *ts);
const char *gp_text_font_path(const gp_text_system *ts, int font);

/* ---------------------------------------------------------------- graphics resources (gfx.c)
 *
 * What painting refers to by number and the renderer keeps on the GPU, besides glyphs: gradient ramps and images. They
 * live in one gp_gfx per app, which the text system points at (gp_text_gfx) so that painting, which already has the text
 * system, can reach it. */

#define GP_GRAD_W 1024            /* samples along a ramp */
#define GP_GRAD_ROWS 128          /* ramps in the table: when it is full the frame ends, the table is cleared and repainted */
#define GP_GRAD_MAX_STOPS 16

enum { GP_GRAD_LINEAR = 0, GP_GRAD_RADIAL, GP_GRAD_CONIC };
typedef struct gp_gradient {
    uint8_t kind;                 /* GP_GRAD_* */
    bool repeat;                  /* the stops are one period of a pattern that goes on both ways (linear and radial) */
    float angle;                  /* linear: degrees, CSS convention; conic: where it starts */
    float cx, cy;                 /* radial and conic: center as fractions of the box */
    int n;
    gp_color_stop stops[GP_GRAD_MAX_STOPS];   /* normalized: positions 0..1, never decreasing */
} gp_gradient;

typedef struct gp_gfx gp_gfx;
gp_gfx *gp_gfx_new(void);
void gp_gfx_free(gp_gfx *g);
void gp_text_set_gfx(gp_text_system *ts, gp_gfx *g);
/* The shapes of `text` as SVG path data in pixels (y down, the pen at x0, y0 on the baseline); *advance is its width. NULL if nothing has an outline. malloc'd. (SVG text.) */
char *gp_text_outline_path(gp_text_system *ts, int64_t weight, float size, const char *text, size_t len, float x0, float y0, float *advance);
gp_gfx *gp_text_gfx(const gp_text_system *ts);

/* Fills in `out` from user stops: positions defaulted, clamped, held monotonic; at most GP_GRAD_MAX_STOPS. */
void gp_gradient_normalize(gp_gradient *out, const gp_color_stop *stops, int n);
/* The ramp row for a gradient's stops (adding it if new), or -1 when the table is full. */
int gp_gfx_gradient_row(gp_gfx *g, const gp_gradient *grad);
const uint8_t *gp_gfx_gradient_data(const gp_gfx *g);                    /* GP_GRAD_ROWS rows of GP_GRAD_W RGBA8, premultiplied */
bool gp_gfx_gradient_take_dirty(gp_gfx *g, int *row0, int *row1);         /* rows [row0, row1) changed since last asked */
bool gp_gfx_overflowed(const gp_gfx *g);
void gp_gfx_reset(gp_gfx *g);                                             /* forget all ramps (after overflow); images stay */
int gp_gfx_gradient_count(const gp_gfx *g);

/* Images. The registry holds premultiplied RGBA8 pixels (owned) and, once the renderer made one, the GL texture. */
#define GP_IMAGE_MAX_DIM 16384
typedef struct {
    uint8_t *pixels;        /* w x h x 4, premultiplied */
    int w, h;
    uint32_t gl_tex;        /* 0 until drawn */
    uint32_t generation;    /* of the slot: 0 = free */
} gp_gfx_image;
gp_image gp_gfx_image_add(gp_gfx *g, int w, int h, uint8_t *premultiplied);   /* takes the pixels; GP_IMAGE_NULL (and frees them) if invalid */
gp_gfx_image *gp_gfx_image_get(gp_gfx *g, uint32_t index, uint32_t generation);   /* NULL for a stale handle */
bool gp_gfx_image_size(gp_gfx *g, gp_image image, int *w, int *h);
void gp_gfx_image_release(gp_gfx *g, gp_image image);
int gp_gfx_image_count(const gp_gfx *g);
/* Textures of released images, for the renderer to delete (it owns GL); and, when it is torn down, all of them. */
size_t gp_gfx_take_dead_textures(gp_gfx *g, uint32_t *out, size_t cap);
size_t gp_gfx_take_all_textures(gp_gfx *g, uint32_t *out, size_t cap);   /* forgets them: the images upload again when drawn */

/* SVG documents (svg.c, gfx.c): the registry keeps the text, and rasters of it made for the sizes and colors they were shown at. */
#define GP_SVG_MAX_RASTER 4096     /* a raster is at most this many pixels a side */
gp_svg gp_gfx_svg_add(gp_gfx *g, char *text_owned, size_t len, float w, float h);
bool gp_gfx_svg_size(gp_gfx *g, gp_svg svg, float *w, float *h);
void gp_gfx_svg_release(gp_gfx *g, gp_svg svg);
/* The image to draw for the document at this size and `currentColor` (0xRRGGBB): rasterized the first time, then found. The
 * raster covers the whole drawing; its height follows the drawing's proportions. NULL handle if it cannot be made. */
gp_image gp_gfx_svg_raster(gp_gfx *g, gp_svg svg, int w, uint32_t rgb);
int gp_gfx_svg_raster_count(const gp_gfx *g);
void gp_gfx_set_text(gp_gfx *g, gp_text_system *ts);   /* SVG <text> is drawn with this one's fonts */
void gp_gfx_svg_set_budget(gp_gfx *g, size_t bytes, int count);   /* when the rasters together pass it, the frame is cleared and repainted */
bool gp_svg_probe(const char *src, size_t len, float *w, float *h);
uint8_t *gp_svg_render(const char *src, size_t len, uint32_t rgb, int w, int h, float intrinsic_w, gp_text_system *ts);   /* ts: for <text> (may be NULL) */   /* premultiplied RGBA, malloc'd */
/* svgx.c: <use>, <symbol>, clip-path and mask, by rewriting the document into layers (see there). NULL from gp_svgx_parse: the document needs none of it. */
struct svgx;
struct svgx *gp_svgx_parse(const char *src, size_t len, gp_text_system *ts);
void gp_svgx_free(struct svgx *d);
int gp_svgx_layers(const struct svgx *d);
char *gp_svgx_layer_doc(const struct svgx *d, int i);   /* the document with only layer i's shapes; malloc'd text */
char *gp_svgx_clip_doc(const struct svgx *d, int i);    /* layer i's clip path as a document of black shapes, or NULL */
char *gp_svgx_mask_doc(const struct svgx *d, int i);    /* layer i's mask as a document, or NULL */
uint8_t *gp_svgx_apply_filter(const struct svgx *d, int i, const uint8_t *straight_rgba, int w, int h, float scale);   /* layer i's filter applied to the layer (straight RGBA bytes); malloc'd, or NULL if it has none */

/* Decoding (image.c), also used off the main thread: straight RGBA8 -> premultiplied, in place. */
uint8_t *gp_image_decode(const void *data, size_t size, int *w, int *h);    /* malloc'd premultiplied RGBA, or NULL */
void gp_image_premultiply(uint8_t *rgba, size_t pixels);

/* ---------------------------------------------------------------- scene / renderer */

enum { GP_PRIM_QUAD = 0, GP_PRIM_SHADOW = 1, GP_PRIM_GLYPH = 2, GP_PRIM_COLOR_GLYPH = 3, GP_PRIM_LCD_GLYPH = 4,
       GP_PRIM_LINEAR_GRADIENT = 5, GP_PRIM_RADIAL_GRADIENT = 6, GP_PRIM_IMAGE = 7, GP_PRIM_CONIC_GRADIENT = 8, GP_PRIM_SEGMENT = 9 };

typedef struct {
    float bounds[4];       /* x y w h, logical px */
    float clip[4];         /* x y w h */
    float color[4];
    float border_color[4];
    float radii[4];        /* tl tr br bl */
    float border_w, kind, blur, _pad;
    float uv[4];           /* normalized atlas rect */
    /* borders of a quad: the colors of the top, right and bottom sides (the left is border_color), and how the ring is drawn:
     * bsty = (style: 0 solid 1 dashed 2 dotted; 1 when the sides have colors of their own; the gradient's row in the ramp table, 0 for none;
     * the gradient's kind). With a gradient, side[0] holds its geometry and opacity instead of a color. */
    float side[3][4];
    float bsty[4];
    /* the transform in force when it was painted, as an affine map of the layout coordinates above to the screen: x' = a x + c y + tx, y' = b x + d y + ty, stored as
     * (a - 1, b, c, d - 1), (tx, ty, scale - 1, 0) so that all zeros is the identity (scale: how much it magnifies, for the width of edges) */
    float xf0[4], xf1[4];
} gp_instance;

typedef struct {
    gp_instance *items;
    size_t len, cap;
} gp_scene;

/* A 2 x 3 affine map: x' = a x + c y + tx, y' = b x + d y + ty. */
typedef struct { float a, b, c, d, tx, ty; } gp_affine;
size_t gp_curve_flatten(const gp_el *n, float tol, gp_arena *a, gp_point **out);   /* curve.c: the centerline as straight pieces (arena) */
float gp_curve_distance(const gp_el *n, float x, float y);   /* to the centerline; x, y in the element's own coordinates */
bool gp_curve_hit(const gp_el *n, float x, float y);          /* on the stroke: within half the width plus the hit tolerance */
gp_affine gp_style_affine(const struct gp_style *s, float x, float y, float w, float h);   /* what a transformed element does to what is in it (paint.c) */
static inline gp_affine gp_aff_identity(void) { return (gp_affine){ 1, 0, 0, 1, 0, 0 }; }
/* p after n: the map that does n first */
static inline gp_affine gp_aff_mul(gp_affine p, gp_affine n) {
    return (gp_affine){ p.a * n.a + p.c * n.b, p.b * n.a + p.d * n.b, p.a * n.c + p.c * n.d, p.b * n.c + p.d * n.d,
                        p.a * n.tx + p.c * n.ty + p.tx, p.b * n.tx + p.d * n.ty + p.ty };
}
static inline gp_affine gp_aff_inverse(gp_affine m) {
    float det = m.a * m.d - m.b * m.c;
    if (fabsf(det) < 1e-12f) return gp_aff_identity();   /* collapsed (scale 0): nothing can be pointed at */
    float a = m.d / det, b = -m.b / det, c = -m.c / det, d = m.a / det;
    return (gp_affine){ a, b, c, d, -(a * m.tx + c * m.ty), -(b * m.tx + d * m.ty) };
}
static inline gp_point gp_aff_apply(gp_affine m, gp_point p) { return (gp_point){ m.a * p.x + m.c * p.y + m.tx, m.b * p.x + m.d * p.y + m.ty }; }




void gp_scene_push(gp_scene *s, const gp_instance *i);
void gp_scene_clear(gp_scene *s);

typedef struct gp_renderer gp_renderer;
gp_renderer *gp_renderer_new(void *(*get_proc)(const char *));
void gp_renderer_free(gp_renderer *r, gp_gfx *gfx);
void gp_renderer_draw(gp_renderer *r, gp_text_system *ts, const gp_scene *scene, int px_w,
                      int px_h, float scale, gp_color clear);
/* Tightly packed RGB, top row first. Caller frees. */
uint8_t *gp_renderer_read_pixels(int px_w, int px_h);
void gp_debug_capture_next(gp_app *app);   /* tests: app->capture_px holds the next frame drawn */

/* ---------------------------------------------------------------- scrolling */

/* What a variable-height list remembers between layouts (vlist.c). */
typedef struct gp_vlist {
    size_t n, cap;
    float *h;               /* measured height of each row, < 0 = not measured yet */
    double *fsum;           /* Fenwick trees (1-based) over the measured heights ... */
    int32_t *fcnt;          /* ... and over how many rows are measured */
    double sum;
    size_t nmeasured;
    float est0;             /* estimate for unmeasured rows until some are measured */
    float width;            /* row width the heights were measured at */
    /* The row at the top of the viewport and how far into it the viewport starts. Scrolling is
     * kept in these terms, so rows above the viewport changing height does not move the view. */
    size_t a_idx;
    double a_off;
    float last_y;           /* scroll offset the anchor belongs to; any other value = scrolled */
    bool at_end;            /* the last layout ended at the bottom */
    bool fresh;             /* no layout has realized rows yet */
    bool want;              /* gp_list_scroll_to_item, applied at the next layout */
    size_t want_idx;
    int want_align;
} gp_vlist;
gp_vlist *gp_vlist_new(float est0);
void gp_vlist_free(gp_vlist *vl);
void gp_vlist_resize(gp_vlist *vl, size_t n);
void gp_vlist_clear(gp_vlist *vl);
void gp_vlist_splice(gp_vlist *vl, size_t start, size_t old, size_t fresh);
void gp_vlist_set(gp_vlist *vl, size_t i, float h);
void gp_vlist_forget(gp_vlist *vl, size_t start, size_t count);
bool gp_vlist_measured(const gp_vlist *vl, size_t i);
float gp_vlist_height(const gp_vlist *vl, size_t i);
double gp_vlist_top(const gp_vlist *vl, size_t i);
double gp_vlist_total(const gp_vlist *vl);
size_t gp_vlist_find(const gp_vlist *vl, double y, double *into);
uint32_t gp_vlist_generation(void);

typedef struct {
    uint64_t key;
    float x, y, max_x, max_y;
    /* gp_uniform_list bookkeeping */
    float item_h, pad_top;   /* a uniform list: the item's extent along its axis, and the padding before the first item */
    bool horiz;             /* ...and that axis is x (gp_list_horizontal) */
    float view_w, view_h;   /* viewport (padding box) as of the last layout */
    float tx, ty;          /* smooth scrolling: where the wheel is taking the offset (absolute, as y is) */
    bool smooth;           /* ...and the offset is on its way there */
    int64_t activity_ns;   /* auto-hiding scrollbars: when the offset last moved or the mouse was last over it (0 = never) */
    gp_arena *arena;     /* rows of a list, reset whenever they are rebuilt */
    gp_arena *harena;    /* the pinned header of a list with gp_list_sticky, rebuilt on every scroll */
    gp_vlist *vl;        /* gp_list only */
} gp_scroll_state;
typedef struct {
    gp_scroll_state *items;
    size_t len, cap;
    uint32_t ext_gen;   /* bumped when an offset or a list is changed from outside layout (gp_scroll_to, gp_list_splice, ...) */
} gp_scroll_table;

#define GP_SCROLL_LINE_PX 40.f   /* one wheel notch */
#define GP_SCROLL_EXPLICIT (1ull << 63)

gp_scroll_state *gp_scroll_state_get(gp_scroll_table *t, uint64_t key, bool create);
void gp_scroll_autohide_update(gp_window *w);   /* before a paint: how visible each auto-hiding scrollbar is, and when to look again */

typedef struct {
    float x, y, w, h;            /* thumb rect, window coords */
    float track_start, track_len; /* along the scroll axis */
    float thumb_len, max;
} gp_thumb;
/* axis 0 = horizontal, 1 = vertical; (ox, oy) is the container's absolute origin. */
bool gp_scroll_thumb(const gp_el *n, int axis, float ox, float oy, gp_thumb *t);
void gp_scroll_wheel(gp_window *w, gp_point p, float dx, float dy);
void gp_text_input_debug_replace(gp_app *app, gp_entity input, size_t start, size_t end, const char *ins);   /* tests: an edit with no bookkeeping but the buffer's */
size_t gp_text_input_debug_clusters(gp_app *app, gp_entity input, int mode, size_t *out, size_t cap);   /* tests: the cluster boundaries three ways */
const char *gp_text_input_debug_check(gp_app *app, gp_entity input, char *why, size_t cap);   /* tests: NULL if the kept-up index is what a rescan gives */
const char *gp_text_input_debug_rows(gp_app *app, gp_entity input, unsigned long *full, unsigned long *incremental, char *why, size_t cap);   /* tests: the wrapped lines against the lines wrapped again; times everything was re-estimated, lines wrapped */
void gp_text_input_debug_wrap_all(gp_app *app, gp_entity input);   /* tests: wraps every line of a soft-wrapped field */
size_t gp_text_input_debug_row_of(gp_app *app, gp_entity input, size_t off, bool upstream);   /* tests */
void gp_text_input_debug_scroll_to(gp_app *app, gp_entity input, float y);   /* tests: as the wheel or a scrollbar would */
size_t gp_text_input_debug_unwrapped(gp_app *app, gp_entity input, size_t *lines);   /* tests: lines whose row counts are estimates */
void gp_scroll_notify(gp_window *w, gp_el *n);   /* calls the element's gp_on_scroll handler with its offset (base included), if it has one */
void gp_scroll_track_repeat(gp_window *w);    /* before painting: pages again if the button is still held on a track and it is time */
void gp_scroll_stop_repeat(gp_window *w);     /* the button was released */
void gp_scroll_smooth_update(gp_window *w);
void gp_scroll_touch(gp_window *w, int phase, float x, float y, int64_t t_ns);   /* one finger: a tap clicks, a drag scrolls and a flick keeps scrolling */
void gp_scroll_fling_update(gp_window *w);   /* before painting: takes the next step of a fling, asks for the following frame */
void gp_scroll_fling_stop(gp_window *w);   /* before painting: takes the next step of the wheel scrolls under way, asks for the following frame */
bool gp_scroll_reveal(gp_window *w, uint64_t focus_id);   /* moves the scrollers around the element with this focus id so that it is in view */
bool gp_scroll_begin_drag(gp_window *w, gp_point p);
void gp_scroll_drag_to(gp_window *w, gp_point p);

/* ---------------------------------------------------------------- layout / paint */

void gp_layout_root(gp_el *root, gp_text_system *ts, gp_size viewport, gp_scroll_table *scrolls, gp_vviews *views, gp_anim_table *anims);
/* A list's offset changed without a rebuild: realize the rows now in view. */
void gp_layout_list_scrolled(gp_el *list, gp_text_system *ts, gp_scroll_table *scrolls, gp_anim_table *anims);
/* Builds rows [first, first+n) of `list` into `arena` (element.c). */
gp_el **gp_list_build(gp_el *list, size_t first, size_t n, gp_arena *arena);
bool gp_list_group_build(gp_el *list, size_t row, gp_arena *arena, gp_list_group *out, gp_el **header);   /* gp_list_sticky's function for a row */
/* `views` (may be NULL = no caching) lets cached views reuse the instances they painted last time. */
void gp_paint_tree(gp_scene *scene, gp_arena *scratch, gp_text_system *ts, gp_el *root,
                   float scale, gp_point mouse, bool mouse_in, bool pressed, uint64_t focused,
                   uint64_t drag_key, uint32_t vis_gen, gp_vviews *views, gp_app *app);
/* Debugging / testing: with caches off every layout and paint is done in full. */
unsigned long gp_layout_skips(void);   /* subtrees whose layout was skipped so far */
void gp_debug_frame_caches(bool enabled);
bool gp_frame_caches_enabled(void);

/* ---------------------------------------------------------------- exec (timers, tasks, pool) */

int64_t gp_now_ns(void);
void gp_debug_clock_skew(int64_t ns);   /* tests: makes every clock here run ns ahead, so time can be jumped */
enum { GP_WAKE_LAYOUT = 1, GP_WAKE_PAINT = 2 };   /* what a window-bound frame or timer asks the window for */
uint64_t gp_exec_window_frame(gp_app *app, gp_window *w, int what);   /* a frame (at the display's rate), then the window lays out / paints again */
uint64_t gp_exec_window_at(gp_app *app, gp_window *w, int64_t due_ns, int what);   /* the same at a time */

/* anim.c */
void gp_anim_begin(gp_anim_table *t);
void gp_anim_eval(gp_el *root, gp_anim_table *t);   /* applies the animations on and below root */
void gp_anim_end(gp_anim_table *t);
void gp_anim_free(gp_anim_table *t);
/* element.c: calls an animation's function and applies the modifier (or gp_group) it returns to an element's style. */
void gp_el_apply_animation(gp_el *el, const gp_anim *a, gp_build_ctx *bc);
void gp_exec_init(gp_app *app);
void gp_exec_shutdown(gp_app *app);               /* cancels and unwinds everything, joins threads */
int gp_exec_timeout_ms(gp_app *app);              /* -1 = may block forever, 0 = do not wait */
void gp_exec_run(gp_app *app);                    /* posts, completions, due timers, frames */
void gp_exec_entity_released(gp_app *app, gp_entity e);
void gp_exec_window_closed(gp_app *app, gp_window *w);   /* ends what was started for it */

bool gp_exec_wants_frames(gp_app *app);

/* coro.c */
typedef struct gp_job gp_job;
gp_co *gp_co_new(gp_app *app, uint64_t job_id, gp_ctx cx, gp_co_fn fn, void *data);
void gp_co_resume(gp_app *app, gp_co *co);
void gp_co_cancel(gp_app *app, gp_co *co);
bool gp_co_done(const gp_co *co);
void gp_co_free(gp_co *co);
void gp_co_forget_window(gp_co *co, gp_window *w);
/* exec.c, used by coro.c to build await points; return the job id */
uint64_t gp_exec_wake_after(gp_app *app, int64_t delay_ns, uint64_t co_job);
uint64_t gp_exec_wake_frame(gp_app *app, uint64_t co_job, double *time_out);
uint64_t gp_exec_wake_fd(gp_app *app, uint64_t co_job, int fd, int events, int64_t timeout_ns, int *revents_out);
uint64_t gp_exec_wake_background(gp_app *app, uint64_t co_job, gp_work_fn work, void *data);
void gp_exec_cancel_id(gp_app *app, uint64_t id);

/* ---------------------------------------------------------------- app / window */

#define GP_MAX_WINDOWS 16

typedef struct {
    void *state;
    size_t size;
    uint32_t generation;
    gp_render_fn render;
    gp_drop_fn drop;
    bool alive;
    bool notify_queued;      /* a notify effect for this entity is waiting (observe.c) */
    uint32_t sub_target_head; /* subscriptions that watch this entity / belong to it: pool index + 1 */
    uint32_t sub_owner_head;
} gp_entity_slot;

typedef struct { gp_drop_fn fn; void *data; } gp_sub_drop;

/* Observers, event subscriptions and release observers (observe.c), pooled and chained per entity. */
typedef struct {
    uint32_t generation;     /* bumped when freed, so stale gp_subscription handles miss */
    bool alive;
    uint8_t kind;
    gp_entity target, owner; /* what is watched; whose callback it is (null = the app) */
    char *event;             /* event name filter, NULL = every event */
    void *fn, *data;
    uint32_t t_prev, t_next; /* chain at the target's slot: pool index + 1, 0 = none */
    uint32_t o_prev, o_next; /* chain at the owner's slot */
    gp_drop_fn drop;         /* gp_subscription_on_drop */
    void *drop_data;
    uint32_t next_free;
} gp_sub;

/* Something to run once the callback that caused it has returned. */
typedef struct {
    bool emit;               /* else a notify */
    gp_entity entity;
    char *name;
    void *payload;
    size_t size;
} gp_effect;

/* observe.c */
gp_entity_slot *gp_slot_for(gp_app *app, gp_entity e);      /* NULL if released (app.c) */
void gp_queue_effect(gp_app *app, bool emit, gp_entity e, const char *name, const void *payload, size_t size);
bool gp_effects_pending(const gp_app *app);
void gp_effects_flush(gp_app *app);                         /* also public as gp_flush_effects */
/* An entity is going away: run its release observers (its state is still valid), then drop every
 * subscription that watches it or belongs to it. */
void gp_subs_entity_released(gp_app *app, gp_entity e, void *state);
void gp_subs_free(gp_app *app);


/* What a window knows about the views it renders (views.c): who built whom, which need to render
 * again, and the element tree of cached ones, which lives in an arena of its own. */
typedef struct {
    gp_entity entity;        /* generation 0 = empty slot */
    gp_entity parent;        /* the view whose render function built this one at the last build */
    bool dirty;              /* notified since it last rendered */
    bool subtree_dirty;      /* a view below it was */
    bool cached;             /* `root` is its element tree, reused while clean */
    bool reads_focus;        /* its last render called gp_is_focused */
    uint64_t used_epoch;     /* the build it was last reached in; others are dropped after a build */
    uint64_t skipped_epoch;  /* the build in which its layout was skipped: the rows of lists below it were not built again */
    bool from_row;           /* built for a row of a list (at layout or scroll time): not a kid of its parent */
    gp_arena *arena;         /* cached views: where `root` and everything built for it lives */
    gp_el *root;
    gp_entity *kids;         /* the views its last render built directly (rows of lists excepted) */
    uint32_t nkids, cap_kids;
    /* the instances painted for the subtree, and what they were painted for (paint.c) */
    gp_instance *chunk;
    size_t nchunk, cap_chunk;
    bool chunk_valid;
    uint32_t chunk_gen;      /* layout_gen of the root when painted */
    uint32_t chunk_sig[24];  /* bits of: (origin, which is kept apart), clip, opacity, scale, atlas generation, focus id, dyn_gen, drag key, scrollbar visibility */
    float chunk_x, chunk_y;  /* where the root was when it was painted: a subtree with no clip of its own that has only moved by whole device pixels is the same instances moved */
} gp_vview;

struct gp_vviews {
    gp_vview *v;
    size_t cap, len;
    uint64_t epoch;          /* bumped at the start of every build of the window */
    bool all_dirty;          /* every cached view renders again at the next build */
};

/* views.c */
gp_vview *gp_views_find(gp_vviews *t, gp_entity e);
gp_vview *gp_views_touch(gp_vviews *t, gp_entity e, gp_entity parent);   /* find or create */
void gp_views_add_kid(gp_vviews *t, gp_entity parent, gp_entity kid);
void gp_views_keep_subtree(gp_vviews *t, gp_vview *v);   /* a cached view is reused: so is everything inside it */
void gp_views_begin(gp_window *w);                  /* a build is starting */
void gp_views_sweep(gp_window *w);                  /* a build is done: forget views it did not reach */
void gp_views_free(gp_window *w);
/* `e` changed: it, and the views above it, render again; windows showing it rebuild. */
void gp_view_dirty(gp_app *app, gp_entity e);
/* Everything may have changed (fonts, ...): all windows rebuild and every cached view renders again. */
void gp_views_invalidate_all(gp_app *app);
/* Focus moved: views whose render looked at focus render again. */
void gp_views_focus_changed(gp_window *w);
void gp_view_reads_focus(gp_window *w, gp_entity view);
gp_arena *gp_view_arena(gp_vview *v);               /* a cached view's own arena, created on first use */
void gp_view_drop_cache(gp_vview *v);

typedef struct { uint64_t key; int64_t start_ns; uint64_t seen; } gp_anim_state;
struct gp_anim_table {
    gp_app *app;
    gp_window *window;
    gp_anim_state *items;
    size_t len, cap;
    uint64_t pass;            /* bumped by every layout; an animation not seen in one starts over */
    int64_t now;              /* the clock of the current pass */
    bool running;             /* something animated is not finished: another frame is wanted */
    bool job_pending;         /* a frame request is in flight for this window */
    gp_arena arena;           /* the modifiers an animation function returns; reset before each element */
};

typedef struct {
    uint32_t mods;
    char key[16];
} gp_keystroke;

#define GP_MAX_CHORD 4        /* keystrokes in one binding ("ctrl-k ctrl-c") */
#define GP_CHORD_TIMEOUT_NS 1500000000ll   /* a half-typed chord is forgotten after this long */

struct gp_window {
    gp_app *app;
    void *platform;        /* backend window handle */
    uint32_t id;
    gp_entity root_view;
    gp_color background;
    gp_size size;          /* logical px */
    float scale;
    int px_w, px_h;

    gp_arena arena;        /* the element tree */
    gp_arena scratch;      /* per-paint temporaries */
    gp_el *tree;
    gp_scene scene;

    gp_point mouse;
    bool mouse_in, pressed;
    uint64_t down_path, down_path_md;
    int cursor;

    gp_vviews views;
    gp_scroll_table scrolls;
    gp_anim_table anims;
    struct { bool active; gp_handler move, up; gp_point origin; } capture; /* gp_on_drag */
    uint64_t drag_key;     /* scroll key whose thumb is being dragged, 0 = none */
    uint64_t page_key;     /* scroll key whose track the button is held on (it pages again and again), 0 = none */
    int page_axis;
    int64_t page_due_ns;   /* when it pages next */
    uint64_t page_timer;   /* the wake-up for that */
    int drag_axis;
    float drag_grab;

    uint64_t focused;      /* focus id, 0 = none */
    uint64_t reveal_id;    /* focus id to scroll into view once the next layout is done (Tab, gp_scroll_into_view), 0 = none */
    bool text_input_on;
    float ime_rect[4];     /* last rect given to the platform, to avoid redundant updates */
    bool ime_rect_set;
    bool swallow_text;     /* a keydown was handled; drop the text event it would produce */
    gp_keystroke pending[GP_MAX_CHORD];   /* the keystrokes of a chord typed so far */
    int npending;
    int64_t pending_ns;    /* when the last of them was typed */

    bool dirty_render, dirty_paint;
    int64_t last_frame_ns; /* when frame requests tied to this window last ran */
    uint32_t scroll_vis_gen;   /* bumped when an auto-hiding scrollbar's visibility changed: painted copies of views with one are stale */
    uint64_t smooth_keys[8];   /* scrollers with a smooth scroll under way, and when the last step was taken */
    int nsmooth;
    int64_t smooth_last_ns;
    /* touch: a finger down, whether it has become a scroll, and its recent positions for the speed it leaves with; then a fling in progress */
    struct { bool down, scrolling; gp_point start, last; int n; int64_t t[32]; gp_point p[32]; } touch;   /* (the last 32 samples: 100 ms of a 240 Hz finger) */
    struct { bool active; gp_point at; float vx, vy; int64_t last_ns; } fling;
    bool fade_job_pending;     /* a paint-only frame is requested for a fading scrollbar */
    uint64_t autohide_timer;   /* job: when the scrollbars that are showing start to fade */
    int64_t autohide_timer_due;
    bool dirty_layout;     /* lay the tree out again without rebuilding it (an animation moved) */
    bool closed;
};

typedef struct {
    gp_keystroke ks[GP_MAX_CHORD];
    int nks;
    char *action;
    char *context;   /* NULL = global */
    void *payload;   /* copied at bind time; NULL if none */
    size_t payload_size;
} gp_binding;

typedef struct {
    char *action;
    gp_action_fn fn;
    void *data;
} gp_global_action;

struct gp_app {
    gp_binding *bindings;
    size_t nbindings, cap_bindings;
    gp_global_action *globals;
    size_t nglobals, cap_globals;
    const void *action_payload;   /* of the action being dispatched, for gp_action_payload */
    size_t action_payload_size;
    uint64_t next_focus_id, next_scroll_id;
    bool text_input_keys_bound;
    char *clip_local;      /* last text this app copied; see gp_clipboard_get */
    char *primary_local;   /* last text this app selected; see gp_primary_get */
    float smooth_scroll_s; /* gp_set_smooth_scrolling: seconds a wheel scroll takes to arrive, 0 = jump */
    bool primary_ours;     /* the platform would not take it, so this is the primary selection until the platform has one */
    struct gp_exec *exec;   /* timers, tasks, background pool (exec.c) */

    gp_sub *subs;
    uint32_t nsubs, cap_subs, free_sub;
    gp_sub_drop *sub_drops;   /* freed subscriptions' closures, run at the next flush */
    size_t nsub_drops, cap_sub_drops;
    gp_effect *effects;
    size_t neffects, eff_head, cap_effects;
    bool flushing;

    gp_entity_slot *slots;
    uint32_t nslots, cap_slots;
    uint32_t *free_slots;
    uint32_t nfree, cap_free;

    gp_window *windows[GP_MAX_WINDOWS];
    int nwindows;

    gp_text_system *text;
    gp_renderer *renderer;
    void *gl_context;      /* backend-owned */
    void *platform;
    bool quit;

    /* test hooks, driven by GPUIC_* env vars */
    const char *shot_path;
    bool debug_ignore_platform_ime;   /* tests: the display server's own composition updates (empty ones, on a timer) are dropped, so a simulated composition stays */
    /* Test hook: gp_debug_capture_next makes the next window_draw read its frame back (before it is swapped away). */
    gp_gfx *gfx;           /* gradients and images (gfx.c) */
    bool capture_next;
    uint8_t *capture_px;   /* RGB, top row first; replaced by each capture */
    int capture_w, capture_h;
    bool has_click, has_hover;
    gp_point click_at, hover_at;
    int64_t start_ns;
    unsigned long stat_wakeups, stat_draws, stat_atlas_resets;
    unsigned long stat_view_renders;   /* render functions called */
    unsigned long stat_layout_skips, stat_paint_hits, stat_paint_misses, stat_paint_shifts;   /* cached subtrees not laid out / painted again / reused moved */
    int64_t stat_build_ns, stat_layout_ns, stat_paint_ns, stat_submit_ns, stat_instances;   /* time in each phase of drawing, for GPUIC_STATS */
    int force_atlas_reset_every; /* GPUIC_ATLAS_RESET_EVERY=n: reset the glyph atlases every n draws (test hook) */ /* GPUIC_STATS=1 prints these at exit */
    int wait_ms;           /* GPUIC_WAIT_MS: hold the screenshot until this long after start */
    struct { gp_point at; int ms; bool done; } clicks[8]; /* GPUIC_CLICKS="x,y@ms;..." */
    int nclicks;
    const char *keys;      /* GPUIC_KEYS: space-separated keystrokes still to inject */
    const char *ime_text;  /* GPUIC_IME: composition text to show */
    bool ime_shot_set;
    const char *wheel;     /* GPUIC_WHEEL: "x,y,dx,dy;..." notches still to inject */
    bool has_drag;         /* GPUIC_DRAG=x0,y0,x1,y1 */
    float drag[4];
    int drag_stage;
    int shot_at;           /* take the screenshot once this many frames were drawn */
    int frames;
};

/* Window event entry points, called by the platform backend. */
void gp_window_on_mouse_move(gp_window *w, float x, float y);
void gp_window_on_mouse_button(gp_window *w, float x, float y, int button, bool down, int clicks,
                               uint32_t mods);
void gp_window_on_mouse_leave(gp_window *w);
void gp_window_on_resize(gp_window *w);
void gp_window_on_expose(gp_window *w);
void gp_window_on_close(gp_window *w);
bool gp_window_on_key(gp_window *w, const char *key, uint32_t mods, bool repeat);
bool gp_window_on_key_up(gp_window *w, const char *key, uint32_t mods);
bool gp_input_key_up(gp_window *w, const gp_key_event *ev);
void gp_input_cancel_chord(gp_window *w);
void gp_window_on_text(gp_window *w, const char *utf8);
void gp_window_on_ime(gp_window *w, const char *preedit, int cursor);
void gp_window_on_scroll(gp_window *w, float x, float y, float dx, float dy, uint32_t mods);

/* input.c: focus, keymap, action dispatch */
void gp_input_free(gp_app *app);
bool gp_parse_keystroke(const char *s, gp_keystroke *out);
bool gp_input_key_down(gp_window *w, const gp_key_event *ev);
void gp_input_text(gp_window *w, const char *utf8);
void gp_input_ime(gp_window *w, const char *preedit, int cursor);
void gp_input_set_focus(gp_window *w, uint64_t id);
void gp_input_after_render(gp_window *w);   /* sync text input with the focused element */
void gp_input_focus_cycle(gp_window *w, int dir);
gp_window *gp_app_window_by_id(gp_app *app, uint32_t id);

/* Platform backend (platform_sdl.c). */
bool gp_platform_init(gp_app *app);
void gp_platform_shutdown(gp_app *app);
bool gp_platform_open_window(gp_app *app, gp_window *w, const gp_window_options *opts);
void gp_platform_close_window(gp_app *app, gp_window *w);
void gp_platform_update_size(gp_window *w);
void gp_platform_make_current(gp_window *w);
void gp_platform_swap(gp_window *w);
void gp_platform_set_cursor(gp_window *w, int cursor);
void gp_platform_set_text_input(gp_window *w, bool on);
void gp_platform_set_text_input_area(gp_window *w, float x, float y, float width, float height,
                                     float cursor_x);
char *gp_platform_primary_get(void);
bool gp_platform_primary_set(const char *utf8);   /* false if the platform would not take it */
char *gp_platform_clipboard_get(void);
void gp_platform_clipboard_set(const char *utf8);
void gp_platform_wake(gp_app *app);               /* thread-safe: make gp_platform_pump return */
float gp_platform_refresh_hz(gp_window *w);
void gp_platform_inject_key(gp_window *w, const char *key, uint32_t mods); /* test hook */
void gp_platform_inject_key_up(gp_window *w, const char *key, uint32_t mods);
void gp_platform_inject_wheel(gp_window *w, float x, float y, float dx, float dy, uint32_t mods); /* test hook */
void gp_platform_inject_text(gp_window *w, const char *utf8);
void gp_platform_inject_mouse(gp_window *w, int kind, float x, float y, int button, int clicks, uint32_t mods);
void *gp_platform_gl_proc(const char *name);
/* Calls `file` (path, size, modification time) for every regular file under `dir`, recursively (bounded depth). */
void gp_platform_walk_dir(const char *dir, void (*file)(const char *path, int64_t size, int64_t mtime_ns, void *user), void *user);
/* Creates `dir` and its parents; true if it exists afterwards. */
bool gp_platform_mkdirs(const char *dir);
/* Renames `from` over `to`, replacing it. */
bool gp_platform_rename(const char *from, const char *to);
/* Blocks up to timeout_ms (-1 = forever) for events, then dispatches all pending. */
void gp_platform_pump(gp_app *app, int timeout_ms);

#endif
