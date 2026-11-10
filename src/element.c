#include "internal.h"

gp_node gp_end_marker = { GP_TAG_END };

static _Thread_local gp_build_ctx *g_bc;

/* ---------------------------------------------------------------- frame helpers */

void *gp_frame_alloc(size_t size) {
    if (!g_bc) {
        fprintf(stderr, "gpuic: gp_frame_alloc called outside of a render function\n");
        abort();
    }
    return gp_arena_alloc(g_bc->arena, size, 16);
}

const char *gp_fmt(const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *buf = gp_frame_alloc((size_t)n + 1);
    vsnprintf(buf, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return buf;
}

/* ---------------------------------------------------------------- elements */

void gp_style_init(gp_style *s) {
    memset(s, 0, sizeof *s);
    s->shrink = 1.f;
    s->opacity = 1.f;
    s->xf_sx = s->xf_sy = 1.f;
    s->xf_ox = s->xf_oy = 0.5f;
    s->persp_ox = s->persp_oy = 0.5f;
    s->stroke_w = 2.f;
    s->miter_limit = 4.f;
}

gp_el *gp_el_new(gp_arena *a, int kind) {
    gp_el *el = gp_arena_alloc(a, sizeof *el, 16);
    el->base.tag = GP_TAG_ELEMENT;
    el->kind = (uint8_t)kind;
    gp_style_init(&el->style);
    if (kind == GP_EL_LIST) el->anim_below = el->hide_below = true;   /* its rows are built at layout, and may animate or hide scrollbars */
    return el;
}

enum {
    M_DIR, M_GROW, M_SHRINK, M_FLEX1, M_FLEX_NONE, M_BASIS,
    M_W, M_H, M_MIN_W, M_MIN_H, M_MAX_W, M_MAX_H,
    M_GAP, M_PAD, M_MARGIN, M_INSET,
    M_ALIGN_ITEMS, M_ALIGN_SELF, M_JUSTIFY,
    M_ABSOLUTE, M_OVERFLOW_HIDDEN,
    M_BG, M_BORDER_W, M_BORDER_COLOR, M_RADIUS, M_SHADOW, M_OPACITY,
    M_HOVER_BG, M_ACTIVE_BG, M_CURSOR,
    M_TEXT_COLOR, M_TEXT_SIZE, M_FONT_WEIGHT, M_FONT_FAMILY, M_FONT_STYLE, M_FONT_FEATURES, M_TEXT_ALIGN,
    M_ON_CLICK, M_ON_MOUSE_DOWN,
    M_TRACK_FOCUS, M_TAB_STOP, M_TAB_INDEX, M_FOCUS_BG, M_FOCUS_BORDER,
    M_KEY_CONTEXT, M_ON_ACTION, M_ON_KEY_DOWN, M_ON_KEY_UP, M_ON_TEXT, M_WHITESPACE_PRE,
    M_SCROLL_X, M_SCROLL_Y, M_TRACK_SCROLL, M_SCROLLBAR_COLOR, M_HIDE_SCROLLBAR,
    M_CHILDREN, M_MODS, M_ON_DRAG, M_ON_MOUSE_UP, M_ON_IME, M_IME_RECT, M_CURSOR_TEXT,
    M_ON_LAYOUT, M_ON_SCROLL, M_ON_WHEEL, M_FONT_VARIATIONS, M_LIST_STICKY, M_LIST_HORIZ, M_BORDER_SIDE_COLOR, M_BORDER_STYLE, M_BORDER_GRADIENT, M_TEXT_DIR, M_LIST_STICK, M_ANIM, M_AUTOHIDE, M_BG_GRADIENT, M_OBJECT_FIT, M_PIXELATED, M_BORDER_SIDE, M_CORNER, M_CORNERS,
    M_IMG_BRIGHTNESS, M_IMG_CONTRAST, M_IMG_SATURATE, M_IMG_GRAYSCALE, M_IMG_BLUR, M_IMG_TINT, M_MARGIN_AUTO, M_WRAP, M_ALIGN_CONTENT,
    M_HOVER_STYLE, M_ACTIVE_STYLE, M_FOCUS_STYLE, M_ROTATE, M_SCALE, M_TRANSLATE, M_XF_ORIGIN, M_SKEW,
    M_STROKE_W, M_STROKE_COLOR, M_STROKE_CAP, M_STROKE_JOIN, M_MITER_LIMIT, M_HIT_TOL,
    M_DASH, M_DASH_OFF, M_STROKE_GRADIENT, M_ARROW, M_FILL, M_CLOSED, M_PATH_TEXT, M_PATH_TEXT_DY,
    M_FILTER, M_BLEND, M_MASK, M_MASK_LUM, M_FILL_RULE, M_MARKER, M_BACKDROP,
    M_ROTATE_X, M_ROTATE_Y, M_TRANSLATE_Z, M_XF_PERSP, M_PERSP, M_PERSP_ORIGIN, M_BACKFACE, M_KEY,
};

enum { SIDE_L = 1, SIDE_T = 2, SIDE_R = 4, SIDE_B = 8, SIDE_ALL = 15 };

static gp_mod *mod_new(int kind) {
    gp_mod *m = gp_frame_alloc(sizeof *m);
    m->base.tag = GP_TAG_MOD;
    m->kind = kind;
    return m;
}

static gp_node *mod_f(int kind, float f) {
    gp_mod *m = mod_new(kind);
    m->f[0] = f;
    return &m->base;
}

static gp_node *mod_len(int kind, gp_length len) {
    gp_mod *m = mod_new(kind);
    m->len = len;
    return &m->base;
}

static gp_node *mod_c(int kind, gp_color c) {
    gp_mod *m = mod_new(kind);
    m->c = c;
    return &m->base;
}

static gp_node *mod_sides(int kind, int mask, float v) {
    gp_mod *m = mod_new(kind);
    m->mask = (uint8_t)mask;
    m->f[0] = v;
    return &m->base;
}

static gp_node *mod_fn(int kind, gp_any_fn fn, void *data) {
    gp_mod *m = mod_new(kind);
    m->fn = fn;
    m->data = data;
    return &m->base;
}

#define PX(v)   ((gp_length){ GP_PX, (v) })
#define FRAC(v) ((gp_length){ GP_FRAC, (v) })

gp_node *gp_flex_row(void) { return mod_f(M_DIR, GP_DIR_ROW); }
gp_node *gp_flex_col(void) { return mod_f(M_DIR, GP_DIR_COLUMN); }
gp_node *gp_flex_1(void) { return mod_f(M_FLEX1, 0); }
gp_node *gp_flex_none(void) { return mod_f(M_FLEX_NONE, 0); }
gp_node *gp_flex_grow(void) { return mod_f(M_GROW, 1); }
gp_node *gp_flex_shrink_0(void) { return mod_f(M_SHRINK, 0); }
gp_node *gp_flex_basis(float px) { return mod_len(M_BASIS, PX(px)); }

gp_node *gp_w(float px) { return mod_len(M_W, PX(px)); }
gp_node *gp_h(float px) { return mod_len(M_H, PX(px)); }
gp_node *gp_size_px(float px) {
    gp_mod *m = mod_new(M_W);
    m->len = PX(px);
    m->mask = 1; /* M_W with mask also sets the height */
    return &m->base;
}
gp_node *gp_w_frac(float f) { return mod_len(M_W, FRAC(f)); }
gp_node *gp_h_frac(float f) { return mod_len(M_H, FRAC(f)); }
gp_node *gp_w_full(void) { return mod_len(M_W, FRAC(1)); }
gp_node *gp_h_full(void) { return mod_len(M_H, FRAC(1)); }
gp_node *gp_size_full(void) {
    gp_mod *m = mod_new(M_W);
    m->len = FRAC(1);
    m->mask = 1;
    return &m->base;
}
gp_node *gp_min_w(float px) { return mod_len(M_MIN_W, PX(px)); }
gp_node *gp_min_h(float px) { return mod_len(M_MIN_H, PX(px)); }
gp_node *gp_max_w(float px) { return mod_len(M_MAX_W, PX(px)); }
gp_node *gp_max_h(float px) { return mod_len(M_MAX_H, PX(px)); }

gp_node *gp_gap(float px) { return mod_f(M_GAP, px); }
gp_node *gp_p(float px) { return mod_sides(M_PAD, SIDE_ALL, px); }
gp_node *gp_px(float px) { return mod_sides(M_PAD, SIDE_L | SIDE_R, px); }
gp_node *gp_py(float px) { return mod_sides(M_PAD, SIDE_T | SIDE_B, px); }
gp_node *gp_pt(float px) { return mod_sides(M_PAD, SIDE_T, px); }
gp_node *gp_pr(float px) { return mod_sides(M_PAD, SIDE_R, px); }
gp_node *gp_pb(float px) { return mod_sides(M_PAD, SIDE_B, px); }
gp_node *gp_pl(float px) { return mod_sides(M_PAD, SIDE_L, px); }
gp_node *gp_m(float px) { return mod_sides(M_MARGIN, SIDE_ALL, px); }
gp_node *gp_mx(float px) { return mod_sides(M_MARGIN, SIDE_L | SIDE_R, px); }
gp_node *gp_my(float px) { return mod_sides(M_MARGIN, SIDE_T | SIDE_B, px); }
gp_node *gp_mt(float px) { return mod_sides(M_MARGIN, SIDE_T, px); }
gp_node *gp_mr(float px) { return mod_sides(M_MARGIN, SIDE_R, px); }
gp_node *gp_mb(float px) { return mod_sides(M_MARGIN, SIDE_B, px); }
gp_node *gp_ml(float px) { return mod_sides(M_MARGIN, SIDE_L, px); }
gp_node *gp_m_auto(void) { return mod_sides(M_MARGIN_AUTO, SIDE_ALL, 0); }
gp_node *gp_mx_auto(void) { return mod_sides(M_MARGIN_AUTO, SIDE_L | SIDE_R, 0); }
gp_node *gp_my_auto(void) { return mod_sides(M_MARGIN_AUTO, SIDE_T | SIDE_B, 0); }
gp_node *gp_ml_auto(void) { return mod_sides(M_MARGIN_AUTO, SIDE_L, 0); }
gp_node *gp_mr_auto(void) { return mod_sides(M_MARGIN_AUTO, SIDE_R, 0); }
gp_node *gp_mt_auto(void) { return mod_sides(M_MARGIN_AUTO, SIDE_T, 0); }
gp_node *gp_mb_auto(void) { return mod_sides(M_MARGIN_AUTO, SIDE_B, 0); }
gp_node *gp_flex_wrap(void) { return mod_f(M_WRAP, 0); }
gp_node *gp_align_content_stretch(void) { return mod_f(M_ALIGN_CONTENT, 0); }
gp_node *gp_align_content_start(void) { return mod_f(M_ALIGN_CONTENT, 1 + GP_JUSTIFY_START); }
gp_node *gp_align_content_center(void) { return mod_f(M_ALIGN_CONTENT, 1 + GP_JUSTIFY_CENTER); }
gp_node *gp_align_content_end(void) { return mod_f(M_ALIGN_CONTENT, 1 + GP_JUSTIFY_END); }
gp_node *gp_align_content_between(void) { return mod_f(M_ALIGN_CONTENT, 1 + GP_JUSTIFY_BETWEEN); }
gp_node *gp_align_content_around(void) { return mod_f(M_ALIGN_CONTENT, 1 + GP_JUSTIFY_AROUND); }
gp_node *gp_align_content_evenly(void) { return mod_f(M_ALIGN_CONTENT, 1 + GP_JUSTIFY_EVENLY); }

gp_node *gp_items_start(void) { return mod_f(M_ALIGN_ITEMS, GP_ALIGN_START); }
gp_node *gp_items_center(void) { return mod_f(M_ALIGN_ITEMS, GP_ALIGN_CENTER); }
gp_node *gp_items_end(void) { return mod_f(M_ALIGN_ITEMS, GP_ALIGN_END); }
gp_node *gp_items_stretch(void) { return mod_f(M_ALIGN_ITEMS, GP_ALIGN_STRETCH); }
gp_node *gp_self_start(void) { return mod_f(M_ALIGN_SELF, GP_ALIGN_START); }
gp_node *gp_self_center(void) { return mod_f(M_ALIGN_SELF, GP_ALIGN_CENTER); }
gp_node *gp_self_end(void) { return mod_f(M_ALIGN_SELF, GP_ALIGN_END); }
gp_node *gp_self_stretch(void) { return mod_f(M_ALIGN_SELF, GP_ALIGN_STRETCH); }
gp_node *gp_justify_start(void) { return mod_f(M_JUSTIFY, GP_JUSTIFY_START); }
gp_node *gp_justify_center(void) { return mod_f(M_JUSTIFY, GP_JUSTIFY_CENTER); }
gp_node *gp_justify_end(void) { return mod_f(M_JUSTIFY, GP_JUSTIFY_END); }
gp_node *gp_justify_between(void) { return mod_f(M_JUSTIFY, GP_JUSTIFY_BETWEEN); }
gp_node *gp_justify_around(void) { return mod_f(M_JUSTIFY, GP_JUSTIFY_AROUND); }
gp_node *gp_justify_evenly(void) { return mod_f(M_JUSTIFY, GP_JUSTIFY_EVENLY); }

gp_node *gp_absolute(void) { return mod_f(M_ABSOLUTE, 1); }
gp_node *gp_left(float px) { return mod_sides(M_INSET, SIDE_L, px); }
gp_node *gp_top(float px) { return mod_sides(M_INSET, SIDE_T, px); }
gp_node *gp_right(float px) { return mod_sides(M_INSET, SIDE_R, px); }
gp_node *gp_bottom(float px) { return mod_sides(M_INSET, SIDE_B, px); }
gp_node *gp_inset(float px) { return mod_sides(M_INSET, SIDE_ALL, px); }
gp_node *gp_children(gp_node **nodes, size_t count) {
    gp_mod *m = mod_new(M_CHILDREN);
    m->data = nodes;
    m->id = count;
    return &m->base;
}

gp_node *gp_mods(gp_node **nodes, size_t count) {
    gp_mod *m = mod_new(M_MODS);
    m->data = nodes;
    m->id = count;
    return &m->base;
}

gp_node *gp_group_(int unused, ...) {
    (void)unused;
    va_list ap;
    va_start(ap, unused);
    size_t n = 0;
    for (gp_node *x; (x = va_arg(ap, gp_node *)) != GP_END;) n++;
    va_end(ap);
    gp_node **list = gp_frame_alloc((n ? n : 1) * sizeof *list);
    va_start(ap, unused);
    size_t i = 0;
    for (gp_node *x; (x = va_arg(ap, gp_node *)) != GP_END;) list[i++] = x;
    va_end(ap);
    return gp_mods(list, n);
}

gp_node *gp_with_animation(const char *id, gp_animation a, gp_anim_style_fn fn, void *data) {
    gp_mod *m = mod_new(M_ANIM);
    uint64_t h = 14695981039346656037ull;   /* FNV-1a of the id */
    for (const char *c = id ? id : ""; *c; c++) h = (h ^ (unsigned char)*c) * 1099511628211ull;
    m->id = h;
    m->f[0] = (float)a.duration;
    m->f[1] = (float)a.delay;
    m->f[2] = (float)a.easing;
    m->mask = (uint8_t)((a.repeat ? 1 : 0) | (a.ping_pong ? 2 : 0));
    m->fn = (gp_any_fn)fn;
    m->data = data;
    return &m->base;
}

gp_node *gp_overflow_hidden(void) { return mod_f(M_OVERFLOW_HIDDEN, 1); }
gp_node *gp_overflow_x_scroll(void) { return mod_f(M_SCROLL_X, 1); }
gp_node *gp_overflow_y_scroll(void) { return mod_f(M_SCROLL_Y, 1); }
gp_node *gp_overflow_scroll(void) {
    gp_mod *m = mod_new(M_SCROLL_X);
    m->mask = 1; /* both axes */
    return &m->base;
}
gp_node *gp_track_scroll(gp_scroll h) {
    gp_mod *m = mod_new(M_TRACK_SCROLL);
    m->id = h.id;
    return &m->base;
}
gp_node *gp_scrollbar_color(gp_color c) { return mod_c(M_SCROLLBAR_COLOR, c); }
gp_node *gp_hide_scrollbar(void) { return mod_f(M_HIDE_SCROLLBAR, 1); }
gp_node *gp_scrollbar_autohide(void) { return mod_f(M_AUTOHIDE, 1); }
gp_node *gp_list_stick_to_end(void) { return mod_f(M_LIST_STICK, 1); }
gp_node *gp_list_horizontal(void) { return mod_f(M_LIST_HORIZ, 1); }
gp_node *gp_list_sticky(gp_list_group_fn fn, void *data) { return mod_fn(M_LIST_STICKY, (gp_any_fn)fn, data); }

gp_node *gp_bg(gp_color c) { return mod_c(M_BG, c); }
gp_node *gp_border(float px) { return mod_f(M_BORDER_W, px); }
gp_node *gp_border_left(float px) { return mod_sides(M_BORDER_SIDE, SIDE_L, px); }
gp_node *gp_border_top(float px) { return mod_sides(M_BORDER_SIDE, SIDE_T, px); }
gp_node *gp_border_right(float px) { return mod_sides(M_BORDER_SIDE, SIDE_R, px); }
gp_node *gp_border_bottom(float px) { return mod_sides(M_BORDER_SIDE, SIDE_B, px); }
gp_node *gp_border_x(float px) { return mod_sides(M_BORDER_SIDE, SIDE_L | SIDE_R, px); }
gp_node *gp_border_y(float px) { return mod_sides(M_BORDER_SIDE, SIDE_T | SIDE_B, px); }
gp_node *gp_rounded_corners(float tl, float tr, float br, float bl) {
    gp_mod *m = mod_new(M_CORNERS);
    m->f[0] = tl; m->f[1] = tr; m->f[2] = br; m->f[3] = bl;
    return &m->base;
}
gp_node *gp_rounded_top(float px) { return mod_sides(M_CORNER, 1 | 2, px); }
gp_node *gp_rounded_bottom(float px) { return mod_sides(M_CORNER, 4 | 8, px); }
gp_node *gp_rounded_left(float px) { return mod_sides(M_CORNER, 1 | 8, px); }
gp_node *gp_rounded_right(float px) { return mod_sides(M_CORNER, 2 | 4, px); }
gp_node *gp_image_grayscale(float amount) { return mod_f(M_IMG_GRAYSCALE, amount); }
gp_node *gp_image_brightness(float factor) { return mod_f(M_IMG_BRIGHTNESS, factor); }
gp_node *gp_image_contrast(float factor) { return mod_f(M_IMG_CONTRAST, factor); }
gp_node *gp_image_saturate(float factor) { return mod_f(M_IMG_SATURATE, factor); }
gp_node *gp_image_blur(float px) { return mod_f(M_IMG_BLUR, px); }
gp_node *gp_image_tint(gp_color c) { return mod_c(M_IMG_TINT, c); }
gp_node *gp_border_color(gp_color c) { return mod_c(M_BORDER_COLOR, c); }
static gp_node *side_color(uint8_t mask, gp_color c) { gp_mod *m = mod_new(M_BORDER_SIDE_COLOR); m->c = c; m->mask = mask; return &m->base; }
gp_node *gp_border_color_left(gp_color c) { return side_color(1, c); }
gp_node *gp_border_color_top(gp_color c) { return side_color(2, c); }
gp_node *gp_border_color_right(gp_color c) { return side_color(4, c); }
gp_node *gp_border_color_bottom(gp_color c) { return side_color(8, c); }
gp_node *gp_border_color_x(gp_color c) { return side_color(1 | 4, c); }
gp_node *gp_border_color_y(gp_color c) { return side_color(2 | 8, c); }
gp_node *gp_border_solid(void) { return mod_f(M_BORDER_STYLE, 0); }
gp_node *gp_border_dashed(void) { return mod_f(M_BORDER_STYLE, 1); }
gp_node *gp_border_dotted(void) { return mod_f(M_BORDER_STYLE, 2); }
gp_node *gp_rounded(float px) { return mod_f(M_RADIUS, px); }
gp_node *gp_rounded_full(void) { return mod_f(M_RADIUS, 1e4f); }
gp_node *gp_shadow(float dx, float dy, float blur, gp_color c) {
    gp_mod *m = mod_new(M_SHADOW);
    m->f[0] = dx; m->f[1] = dy; m->f[2] = blur;
    m->c = c;
    return &m->base;
}
gp_node *gp_opacity(float o) { return mod_f(M_OPACITY, o); }
gp_node *gp_hover_bg(gp_color c) { return mod_c(M_HOVER_BG, c); }
gp_node *gp_active_bg(gp_color c) { return mod_c(M_ACTIVE_BG, c); }
gp_node *gp_cursor_pointer(void) { return mod_f(M_CURSOR, GP_CURSOR_POINTER); }

gp_node *gp_text_color(gp_color c) { return mod_c(M_TEXT_COLOR, c); }
gp_node *gp_text_size(float px) { return mod_f(M_TEXT_SIZE, px); }
gp_node *gp_font_weight(int weight) { return mod_f(M_FONT_WEIGHT, (float)weight); }
gp_node *gp_font_bold(void) { return mod_f(M_FONT_WEIGHT, 700); }
gp_node *gp_font_normal(void) { return mod_f(M_FONT_WEIGHT, 400); }
gp_node *gp_font_features(const char *features) {
    gp_mod *m = mod_new(M_FONT_FEATURES);
    m->name = features ? gp_arena_strdup(g_bc->arena, *features ? features : "normal") : NULL;
    return &m->base;
}
gp_node *gp_font_variations(const char *settings) {
    gp_mod *m = mod_new(M_FONT_VARIATIONS);
    m->name = settings ? gp_arena_strdup(g_bc->arena, *settings ? settings : "normal") : NULL;
    return &m->base;
}
gp_node *gp_font_tabular_nums(void) { return gp_font_features("tnum"); }
gp_node *gp_font_small_caps(void) { return gp_font_features("smcp"); }
gp_node *gp_font_no_ligatures(void) { return gp_font_features("liga=0, clig=0"); }
/* The gradient lives in the element's arena, like a string: a cached view keeps its elements, and so the gradient. */
static gp_node *gradient_mod_kind(int kind, gp_gradient g);
static gp_node *gradient_mod(gp_gradient g) { return gradient_mod_kind(M_BG_GRADIENT, g); }
static gp_node *gradient_mod_kind(int kind, gp_gradient g) {
    gp_gradient *copy = gp_arena_alloc(g_bc->arena, sizeof *copy, 8);
    *copy = g;
    gp_mod *m = mod_new(kind);
    m->data = copy;
    return &m->base;
}

gp_node *gp_bg_linear_gradient(float angle_deg, const gp_color_stop *stops, int n) {
    gp_gradient g = { 0 };
    g.angle = angle_deg;
    gp_gradient_normalize(&g, stops, n);
    return gradient_mod(g);
}

gp_node *gp_bg_repeating_gradient(float angle_deg, const gp_color_stop *stops, int n) {
    gp_gradient g = { 0 };
    g.angle = angle_deg;
    g.repeat = true;
    gp_gradient_normalize(&g, stops, n);
    return gradient_mod(g);
}

gp_node *gp_bg_repeating_radial_gradient(float cx, float cy, const gp_color_stop *stops, int n) {
    gp_gradient g = { 0 };
    g.kind = GP_GRAD_RADIAL;
    g.repeat = true;
    g.cx = cx;
    g.cy = cy;
    gp_gradient_normalize(&g, stops, n);
    return gradient_mod(g);
}

gp_node *gp_border_linear_gradient(float angle_deg, const gp_color_stop *stops, int n) {
    gp_gradient g = { 0 };
    g.angle = angle_deg;
    gp_gradient_normalize(&g, stops, n);
    return gradient_mod_kind(M_BORDER_GRADIENT, g);
}
gp_node *gp_border_radial_gradient(float cx, float cy, const gp_color_stop *stops, int n) {
    gp_gradient g = { 0 };
    g.kind = GP_GRAD_RADIAL;
    g.cx = cx;
    g.cy = cy;
    gp_gradient_normalize(&g, stops, n);
    return gradient_mod_kind(M_BORDER_GRADIENT, g);
}
gp_node *gp_border_conic_gradient(float cx, float cy, float from_deg, const gp_color_stop *stops, int n) {
    gp_gradient g = { 0 };
    g.kind = GP_GRAD_CONIC;
    g.angle = from_deg;
    g.cx = cx;
    g.cy = cy;
    gp_gradient_normalize(&g, stops, n);
    return gradient_mod_kind(M_BORDER_GRADIENT, g);
}
gp_node *gp_bg_conic_gradient(float cx, float cy, float from_deg, const gp_color_stop *stops, int n) {
    gp_gradient g = { 0 };
    g.kind = GP_GRAD_CONIC;
    g.angle = from_deg;
    g.cx = cx;
    g.cy = cy;
    gp_gradient_normalize(&g, stops, n);
    return gradient_mod(g);
}

gp_node *gp_bg_radial_gradient(float cx, float cy, const gp_color_stop *stops, int n) {
    gp_gradient g = { 0 };
    g.kind = GP_GRAD_RADIAL;
    g.cx = cx;
    g.cy = cy;
    gp_gradient_normalize(&g, stops, n);
    return gradient_mod(g);
}

gp_node *gp_font_italic(void) { return mod_f(M_FONT_STYLE, 1); }
gp_node *gp_font_upright(void) { return mod_f(M_FONT_STYLE, 2); }
gp_node *gp_font_sans(void) { return gp_font_family("sans-serif"); }
gp_node *gp_font_serif(void) { return gp_font_family("serif"); }
gp_node *gp_font_mono(void) { return gp_font_family("monospace"); }
gp_node *gp_font_family(const char *families) {
    gp_mod *m = mod_new(M_FONT_FAMILY);
    m->name = families && *families ? gp_arena_strdup(g_bc->arena, families) : NULL;
    return &m->base;
}
gp_node *gp_text_left(void) { return mod_f(M_TEXT_ALIGN, GP_TEXT_ALIGN_LEFT); }
gp_node *gp_text_center(void) { return mod_f(M_TEXT_ALIGN, GP_TEXT_ALIGN_CENTER); }
gp_node *gp_text_right(void) { return mod_f(M_TEXT_ALIGN, GP_TEXT_ALIGN_RIGHT); }
gp_node *gp_text_start(void) { return mod_f(M_TEXT_ALIGN, GP_TEXT_ALIGN_START); }
gp_node *gp_text_end(void) { return mod_f(M_TEXT_ALIGN, GP_TEXT_ALIGN_END); }
gp_node *gp_text_dir_auto(void) { return mod_f(M_TEXT_DIR, GP_BIDI_AUTO); }
gp_node *gp_text_dir_ltr(void) { return mod_f(M_TEXT_DIR, GP_BIDI_LTR); }
gp_node *gp_text_dir_rtl(void) { return mod_f(M_TEXT_DIR, GP_BIDI_RTL); }

gp_node *gp_on_click(gp_mouse_fn fn) { return mod_fn(M_ON_CLICK, (gp_any_fn)fn, NULL); }
gp_node *gp_on_click_data(gp_mouse_fn fn, void *data) {
    return mod_fn(M_ON_CLICK, (gp_any_fn)fn, data);
}
gp_node *gp_on_mouse_down(gp_mouse_fn fn) { return mod_fn(M_ON_MOUSE_DOWN, (gp_any_fn)fn, NULL); }
gp_node *gp_on_mouse_down_data(gp_mouse_fn fn, void *data) {
    return mod_fn(M_ON_MOUSE_DOWN, (gp_any_fn)fn, data);
}

gp_node *gp_on_drag(gp_mouse_fn fn) { return mod_fn(M_ON_DRAG, (gp_any_fn)fn, NULL); }
gp_node *gp_on_drag_data(gp_mouse_fn fn, void *data) { return mod_fn(M_ON_DRAG, (gp_any_fn)fn, data); }
gp_node *gp_on_mouse_up(gp_mouse_fn fn) { return mod_fn(M_ON_MOUSE_UP, (gp_any_fn)fn, NULL); }
gp_node *gp_on_mouse_up_data(gp_mouse_fn fn, void *data) { return mod_fn(M_ON_MOUSE_UP, (gp_any_fn)fn, data); }
gp_node *gp_on_ime(gp_ime_fn fn) { return mod_fn(M_ON_IME, (gp_any_fn)fn, NULL); }
gp_node *gp_on_layout(gp_layout_fn fn) { return mod_fn(M_ON_LAYOUT, (gp_any_fn)fn, NULL); }
gp_node *gp_on_scroll(gp_scroll_fn fn) { return mod_fn(M_ON_SCROLL, (gp_any_fn)fn, NULL); }
gp_node *gp_on_wheel(gp_wheel_fn fn) { return mod_fn(M_ON_WHEEL, (gp_any_fn)fn, NULL); }
gp_node *gp_on_wheel_data(gp_wheel_fn fn, void *data) { return mod_fn(M_ON_WHEEL, (gp_any_fn)fn, data); }
gp_node *gp_cursor_text(void) { return mod_f(M_CURSOR_TEXT, GP_CURSOR_TEXT); }
gp_node *gp_ime_rect(float x, float y, float w, float h) {
    gp_mod *m = mod_new(M_IME_RECT);
    m->f[0] = x; m->f[1] = y; m->f[2] = w; m->f[3] = h;
    return &m->base;
}

gp_node *gp_track_focus(gp_focus f) {
    gp_mod *m = mod_new(M_TRACK_FOCUS);
    m->id = f.id;
    return &m->base;
}
gp_node *gp_tab_stop(void) { return mod_f(M_TAB_STOP, 1); }
gp_node *gp_tab_index(int index) { return mod_f(M_TAB_INDEX, (float)index); }
gp_node *gp_focus_bg(gp_color c) { return mod_c(M_FOCUS_BG, c); }
gp_node *gp_focus_border_color(gp_color c) { return mod_c(M_FOCUS_BORDER, c); }

static gp_node *mod_style(int kind, gp_node *group) {
    gp_mod *m = mod_new(kind);
    m->data = group;
    return &m->base;
}
static gp_node *mod_f2(int kind, float a, float b) {
    gp_mod *m = mod_new(kind);
    m->f[0] = a;
    m->f[1] = b;
    return &m->base;
}
gp_node *gp_stroke_width(float px) { return mod_f(M_STROKE_W, px); }
gp_node *gp_stroke_color(gp_color c) { return mod_c(M_STROKE_COLOR, c); }
gp_node *gp_stroke_cap_round(void) { return mod_f(M_STROKE_CAP, 0); }
gp_node *gp_stroke_cap_butt(void) { return mod_f(M_STROKE_CAP, 1); }
gp_node *gp_stroke_join_round(void) { return mod_f(M_STROKE_JOIN, 0); }
gp_node *gp_stroke_join_miter(void) { return mod_f(M_STROKE_JOIN, 1); }
gp_node *gp_miter_limit(float ratio) { return mod_f(M_MITER_LIMIT, ratio); }
gp_node *gp_hit_tolerance(float px) { return mod_f(M_HIT_TOL, px); }
gp_node *gp_stroke_dash(const float *pattern, int n) {
    gp_mod *m = mod_new(M_DASH);
    int k = pattern && n > 0 ? (n > 8 ? 8 : n) : 0;
    float *copy = gp_arena_alloc(g_bc->arena, (k ? (size_t)k : 1) * sizeof *copy, 4);
    float sum = 0.f;
    for (int i = 0; i < k; i++) { copy[i] = isfinite(pattern[i]) && pattern[i] > 0.f ? pattern[i] : 0.f; sum += copy[i]; }
    m->data = copy;
    m->f[0] = sum > 0.f ? (float)k : 0.f;   /* a pattern of nothing is a solid stroke, as in SVG */
    return &m->base;
}
gp_node *gp_stroke_dash_offset(float px) { return mod_f(M_DASH_OFF, isfinite(px) ? px : 0.f); }
gp_node *gp_stroke_gradient_stops(const gp_color_stop *stops, int n) {
    gp_gradient g = { 0 };
    gp_gradient_normalize(&g, stops, n);
    return gradient_mod_kind(M_STROKE_GRADIENT, g);
}
gp_node *gp_arrow_start(float length) { return mod_f2(M_ARROW, 0, length); }
gp_node *gp_arrow_end(float length) { return mod_f2(M_ARROW, 1, length); }
gp_node *gp_fill_color(gp_color c) { return mod_c(M_FILL, c); }
gp_node *gp_closed(void) { return mod_f(M_CLOSED, 1); }
gp_node *gp_fill_evenodd(void) { return mod_f(M_FILL_RULE, 1); }
static gp_node *marker(int where, int shape, float size) {
    gp_mod *m = mod_new(M_MARKER);
    m->f[0] = (float)where;
    m->f[1] = (float)(shape >= GP_MARKER_NONE && shape <= GP_MARKER_BAR ? shape : GP_MARKER_NONE);
    m->f[2] = isfinite(size) && size > 0.f ? size : 0.f;
    return &m->base;
}
gp_node *gp_marker_start(int shape, float size) { return marker(0, shape, size); }
gp_node *gp_marker_end(int shape, float size) { return marker(1, shape, size); }
gp_node *gp_marker_mid(int shape, float size) { return marker(2, shape, size); }
gp_node *gp_fill_nonzero(void) { return mod_f(M_FILL_RULE, 0); }
gp_node *gp_path_text(const char *text) {
    gp_mod *m = mod_new(M_PATH_TEXT);
    m->name = text ? gp_arena_strdup(g_bc->arena, text) : NULL;
    return &m->base;
}
gp_node *gp_path_text_dy(float px) { return mod_f(M_PATH_TEXT_DY, isfinite(px) ? px : 0.f); }
static float fin(float v, float dflt) { return isfinite(v) ? v : dflt; }
static gp_node *mod_filter(int kind, float v, float dx, float dy, gp_color c) {
    gp_mod *m = mod_new(M_FILTER);
    m->mask = (uint8_t)kind;
    m->f[0] = v; m->f[1] = dx; m->f[2] = dy;
    m->c = c;
    return &m->base;
}
static float amount01(float a) { a = fin(a, 0.f); return a < 0.f ? 0.f : a > 1.f ? 1.f : a; }
gp_node *gp_filter_blur(float px) { return mod_filter(GP_FX_BLUR, fmaxf(fin(px, 0.f), 0.f), 0, 0, (gp_color){ 0 }); }
gp_node *gp_filter_brightness(float f) { return mod_filter(GP_FX_BRIGHTNESS, fmaxf(fin(f, 1.f), 0.f), 0, 0, (gp_color){ 0 }); }
gp_node *gp_filter_contrast(float f) { return mod_filter(GP_FX_CONTRAST, fmaxf(fin(f, 1.f), 0.f), 0, 0, (gp_color){ 0 }); }
gp_node *gp_filter_saturate(float f) { return mod_filter(GP_FX_SATURATE, fmaxf(fin(f, 1.f), 0.f), 0, 0, (gp_color){ 0 }); }
gp_node *gp_filter_grayscale(float a) { return mod_filter(GP_FX_GRAYSCALE, amount01(a), 0, 0, (gp_color){ 0 }); }
gp_node *gp_filter_sepia(float a) { return mod_filter(GP_FX_SEPIA, amount01(a), 0, 0, (gp_color){ 0 }); }
gp_node *gp_filter_invert(float a) { return mod_filter(GP_FX_INVERT, amount01(a), 0, 0, (gp_color){ 0 }); }
gp_node *gp_filter_hue_rotate(float deg) { return mod_filter(GP_FX_HUE_ROTATE, fin(deg, 0.f), 0, 0, (gp_color){ 0 }); }
gp_node *gp_filter_opacity(float a) { return mod_filter(GP_FX_OPACITY, amount01(a), 0, 0, (gp_color){ 0 }); }
gp_node *gp_filter_drop_shadow(float dx, float dy, float blur, gp_color c) { return mod_filter(GP_FX_DROP_SHADOW, fmaxf(fin(blur, 0.f), 0.f), fin(dx, 0.f), fin(dy, 0.f), c); }
gp_node *gp_blend_mode(int mode) { return mod_f(M_BLEND, (float)(mode >= GP_BLEND_NORMAL && mode <= GP_BLEND_LUMINOSITY ? mode : GP_BLEND_NORMAL)); }
gp_node *gp_mask(gp_node *mask) {
    gp_mod *m = mod_new(M_MASK);
    m->data = mask;
    return &m->base;
}
gp_node *gp_mask_luminance(void) { return mod_f(M_MASK_LUM, 1); }
gp_node *gp_backdrop_filter(gp_node *filters) {
    gp_mod *m = mod_new(M_BACKDROP);
    m->data = filters;
    return &m->base;
}
gp_node *gp_rotate(float degrees) { return mod_f2(M_ROTATE, degrees, 0); }
gp_node *gp_scale(float s) { return mod_f2(M_SCALE, s, s); }
gp_node *gp_scale_xy(float sx, float sy) { return mod_f2(M_SCALE, sx, sy); }
gp_node *gp_translate(float dx, float dy) { return mod_f2(M_TRANSLATE, dx, dy); }
gp_node *gp_skew(float ax_deg, float ay_deg) { return mod_f2(M_SKEW, ax_deg, ay_deg); }
gp_node *gp_transform_origin(float fx, float fy) { return mod_f2(M_XF_ORIGIN, fx, fy); }
gp_node *gp_rotate_x(float degrees) { return mod_f(M_ROTATE_X, isfinite(degrees) ? degrees : 0.f); }
gp_node *gp_rotate_y(float degrees) { return mod_f(M_ROTATE_Y, isfinite(degrees) ? degrees : 0.f); }
gp_node *gp_translate_z(float px) { return mod_f(M_TRANSLATE_Z, isfinite(px) ? px : 0.f); }
gp_node *gp_transform_perspective(float px) { return mod_f(M_XF_PERSP, isfinite(px) && px > 0.f ? px : 0.f); }
gp_node *gp_perspective(float px) { return mod_f(M_PERSP, isfinite(px) && px > 0.f ? px : 0.f); }
gp_node *gp_perspective_origin(float fx, float fy) { return mod_f2(M_PERSP_ORIGIN, isfinite(fx) ? fx : 0.5f, isfinite(fy) ? fy : 0.5f); }
gp_node *gp_backface_hidden(void) { return mod_f(M_BACKFACE, 1); }
static uint64_t key_hash(const char *k) {
    uint64_t h = 1469598103934665603ull;
    for (; k && *k; k++) h = (h ^ (unsigned char)*k) * 1099511628211ull;
    return h | 1;
}
gp_node *gp_key(const char *key) {
    gp_mod *m = mod_new(M_KEY);
    m->id = key_hash(key);
    return &m->base;
}
gp_node *gp_key_id(uint64_t key) {
    gp_mod *m = mod_new(M_KEY);
    m->id = (key * 0x9E3779B97F4A7C15ull) | 1;
    return &m->base;
}
gp_node *gp_hover_style(gp_node *group) { return mod_style(M_HOVER_STYLE, group); }
gp_node *gp_active_style(gp_node *group) { return mod_style(M_ACTIVE_STYLE, group); }
gp_node *gp_focus_style(gp_node *group) { return mod_style(M_FOCUS_STYLE, group); }

gp_node *gp_key_context(const char *name) {
    gp_mod *m = mod_new(M_KEY_CONTEXT);
    m->name = gp_arena_strdup(g_bc->arena, name);
    return &m->base;
}
gp_node *gp_on_action_data(gp_action action, gp_action_fn fn, void *data) {
    gp_mod *m = mod_new(M_ON_ACTION);
    m->name = gp_arena_strdup(g_bc->arena, action);
    m->fn = (gp_any_fn)fn;
    m->data = data;
    return &m->base;
}
gp_node *gp_on_action(gp_action action, gp_action_fn fn) {
    return gp_on_action_data(action, fn, NULL);
}
gp_node *gp_on_key_up(gp_key_fn fn) { return mod_fn(M_ON_KEY_UP, (gp_any_fn)fn, NULL); }
gp_node *gp_on_key_up_data(gp_key_fn fn, void *data) { return mod_fn(M_ON_KEY_UP, (gp_any_fn)fn, data); }
gp_node *gp_on_key_down(gp_key_fn fn) { return mod_fn(M_ON_KEY_DOWN, (gp_any_fn)fn, NULL); }
gp_node *gp_on_key_down_data(gp_key_fn fn, void *data) {
    return mod_fn(M_ON_KEY_DOWN, (gp_any_fn)fn, data);
}
gp_node *gp_on_text_input(gp_text_input_fn fn) { return mod_fn(M_ON_TEXT, (gp_any_fn)fn, NULL); }
gp_node *gp_on_text_input_data(gp_text_input_fn fn, void *data) {
    return mod_fn(M_ON_TEXT, (gp_any_fn)fn, data);
}
gp_node *gp_whitespace_pre(void) { return mod_f(M_WHITESPACE_PRE, 1); }

static bool g_in_animation;   /* applying what an animation returned: it cannot add animations */

/* Padding and borders: layout knows padding and a uniform border; a border on some sides only goes into the padding. */
static void fold_borders(gp_style *s) {
    s->has_border_sides = s->border_set != 0;
    if (!s->has_border_sides) {
        s->border_w = s->border_uniform;
        for (int i = 0; i < 4; i++) s->pad[i] = s->pad_user[i];
        return;
    }
    s->border_w = 0;
    for (int i = 0; i < 4; i++) {
        float w = (s->border_set >> i & 1) ? s->border_side[i] : s->border_uniform;
        s->pad[i] = s->pad_user[i] + (w > 0 ? w : 0);
    }
}

/* The layout properties a modifier sets (what a state style can change in layout: see gp_style_overlay_layout) */
static uint32_t lset_of(const gp_mod *m) {
    switch (m->kind) {
    case M_DIR: return GP_LSET_DIR;
    case M_GROW: return GP_LSET_GROW;
    case M_SHRINK: return GP_LSET_SHRINK;
    case M_FLEX1: return GP_LSET_GROW | GP_LSET_SHRINK | GP_LSET_BASIS;
    case M_FLEX_NONE: return GP_LSET_GROW | GP_LSET_SHRINK;
    case M_BASIS: return GP_LSET_BASIS;
    case M_W: return GP_LSET_W | (m->mask ? GP_LSET_H : 0);
    case M_H: return GP_LSET_H;
    case M_MIN_W: return GP_LSET_MIN_W;
    case M_MIN_H: return GP_LSET_MIN_H;
    case M_MAX_W: return GP_LSET_MAX_W;
    case M_MAX_H: return GP_LSET_MAX_H;
    case M_GAP: return GP_LSET_GAP;
    case M_PAD: return GP_LSET_PAD;
    case M_MARGIN: case M_MARGIN_AUTO: return GP_LSET_MARGIN;
    case M_INSET: return GP_LSET_INSET;
    case M_ALIGN_ITEMS: return GP_LSET_ALIGN_ITEMS;
    case M_ALIGN_SELF: return GP_LSET_ALIGN_SELF;
    case M_JUSTIFY: return GP_LSET_JUSTIFY;
    case M_WRAP: return GP_LSET_WRAP;
    case M_ALIGN_CONTENT: return GP_LSET_ALIGN_CONTENT;
    case M_BORDER_W: return GP_LSET_BORDER;
    case M_BORDER_SIDE: return GP_LSET_BORDER_SIDE;
    case M_TEXT_SIZE: return GP_LSET_FONT_SIZE;
    case M_FONT_WEIGHT: return GP_LSET_FONT_WEIGHT;
    case M_FONT_FAMILY: return GP_LSET_FONT_FAMILY;
    case M_FONT_STYLE: return GP_LSET_FONT_STYLE;
    case M_FONT_FEATURES: return GP_LSET_FONT_FEATURES;
    case M_FONT_VARIATIONS: return GP_LSET_FONT_VARIATIONS;
    case M_TEXT_ALIGN: return GP_LSET_TEXT_ALIGN;
    case M_TEXT_DIR: return GP_LSET_TEXT_DIR;
    case M_WHITESPACE_PRE: return GP_LSET_WHITESPACE;
    default: return 0;
    }
}

/* The layout properties a state style sets, put over `d` in the order a modifier list would set them: the sides it names of the padding,
 * margin and inset, its border widths (all sides first, which clears the sides set before, then the sides it names), and the rest whole. */
void gp_style_overlay_layout(gp_style *d, const gp_style *st) {
    uint32_t b = st->lset;
    if (!b) return;
    if (b & GP_LSET_DIR) d->dir = st->dir;
    if (b & GP_LSET_GROW) d->grow = st->grow;
    if (b & GP_LSET_SHRINK) d->shrink = st->shrink;
    if (b & GP_LSET_BASIS) d->basis = st->basis;
    if (b & GP_LSET_W) d->w = st->w;
    if (b & GP_LSET_H) d->h = st->h;
    if (b & GP_LSET_MIN_W) d->min_w = st->min_w;
    if (b & GP_LSET_MIN_H) d->min_h = st->min_h;
    if (b & GP_LSET_MAX_W) d->max_w = st->max_w;
    if (b & GP_LSET_MAX_H) d->max_h = st->max_h;
    if (b & GP_LSET_GAP) d->gap = st->gap;
    if (b & GP_LSET_ALIGN_ITEMS) d->align_items = st->align_items;
    if (b & GP_LSET_ALIGN_SELF) d->align_self = st->align_self;
    if (b & GP_LSET_JUSTIFY) d->justify = st->justify;
    if (b & GP_LSET_WRAP) d->wrap = st->wrap;
    if (b & GP_LSET_ALIGN_CONTENT) d->align_content = st->align_content;
    for (int i = 0; i < 4; i++) {
        uint8_t bit = (uint8_t)(1 << i);
        if (st->lpad & bit) d->pad_user[i] = st->pad_user[i];
        if (st->lmargin & bit) { d->margin[i] = st->margin[i]; d->margin_auto = (uint8_t)((d->margin_auto & ~bit) | (st->margin_auto & bit)); }
        if (st->linset & bit) d->inset[i] = st->inset[i];
    }
    if (b & GP_LSET_BORDER) { d->border_uniform = st->border_uniform; d->border_set = 0; }
    if (b & (GP_LSET_BORDER | GP_LSET_BORDER_SIDE))
        for (int i = 0; i < 4; i++)
            if (st->border_set >> i & 1) { d->border_side[i] = st->border_side[i]; d->border_set |= (uint8_t)(1 << i); }
    if (b & (GP_LSET_PAD | GP_LSET_BORDER | GP_LSET_BORDER_SIDE)) fold_borders(d);
    if (b & GP_LSET_FONT_SIZE) d->font_size = st->font_size;
    if (b & GP_LSET_FONT_WEIGHT) d->font_weight = st->font_weight;
    if (b & GP_LSET_FONT_FAMILY) d->font_family = st->font_family;
    if (b & GP_LSET_FONT_STYLE) d->font_italic = st->font_italic;
    if (b & GP_LSET_FONT_FEATURES) d->font_features = st->font_features;
    if (b & GP_LSET_FONT_VARIATIONS) d->font_variations = st->font_variations;
    if (b & GP_LSET_TEXT_ALIGN) { d->text_align = st->text_align; d->has_text_align = true; }
    if (b & GP_LSET_TEXT_DIR) { d->text_dir = st->text_dir; d->has_text_dir = true; }
    if (b & GP_LSET_WHITESPACE) d->whitespace_pre = st->whitespace_pre;
}

static void apply_mod(gp_el *el, const gp_mod *m) {
    gp_style *s = &el->style;
    s->lset |= lset_of(m);
    switch (m->kind) {
    case M_ANIM: {
        if (g_in_animation) break;
        gp_anim *a = gp_arena_alloc(g_bc->arena, sizeof *a, 16);
        memset(a, 0, sizeof *a);
        a->spec = (gp_animation){ m->f[0], m->f[1], (m->mask & 1) != 0, (m->mask & 2) != 0, (int)m->f[2] };
        a->fn = (gp_anim_style_fn)m->fn;
        a->data = m->data;
        a->view = g_bc->view;
        int index = 0;
        gp_anim **tail = &el->anim;
        while (*tail) { tail = &(*tail)->next; index++; }
        *tail = a;
        a->key = m->id + (uint64_t)index * 0x9E3779B97F4A7C15ull;   /* two animations on one element, one id */
        el->anim_below = true;
        break;
    }
    case M_DIR: s->dir = (uint8_t)m->f[0]; break;
    case M_GROW: s->grow = m->f[0]; break;
    case M_SHRINK: s->shrink = m->f[0]; break;
    case M_FLEX1: s->grow = 1; s->shrink = 1; s->basis = PX(0); break;
    case M_FLEX_NONE: s->grow = 0; s->shrink = 0; break;
    case M_BASIS: s->basis = m->len; break;
    case M_W: s->w = m->len; if (m->mask) s->h = m->len; break;
    case M_H: s->h = m->len; break;
    case M_MIN_W: s->min_w = m->len; break;
    case M_MIN_H: s->min_h = m->len; break;
    case M_MAX_W: s->max_w = m->len; break;
    case M_MAX_H: s->max_h = m->len; break;
    case M_GAP: s->gap = m->f[0]; break;
    case M_PAD:
        for (int i = 0; i < 4; i++) if (m->mask & (1 << i)) s->pad_user[i] = m->f[0];
        s->lpad |= m->mask & 15;
        fold_borders(s);
        break;
    case M_MARGIN:
        for (int i = 0; i < 4; i++) if (m->mask & (1 << i)) { s->margin[i] = m->f[0]; s->margin_auto &= (uint8_t)~(1 << i); }
        s->lmargin |= m->mask & 15;
        break;
    case M_MARGIN_AUTO:
        for (int i = 0; i < 4; i++) if (m->mask & (1 << i)) { s->margin[i] = 0; s->margin_auto |= (uint8_t)(1 << i); }
        s->lmargin |= m->mask & 15;
        break;
    case M_WRAP: s->wrap = true; break;
    case M_ALIGN_CONTENT: s->align_content = (uint8_t)m->f[0]; break;
    case M_INSET:
        for (int i = 0; i < 4; i++) if (m->mask & (1 << i)) s->inset[i] = PX(m->f[0]);
        s->linset |= m->mask & 15;
        break;
    case M_ALIGN_ITEMS: s->align_items = (uint8_t)m->f[0]; break;
    case M_ALIGN_SELF: s->align_self = (uint8_t)m->f[0]; break;
    case M_JUSTIFY: s->justify = (uint8_t)m->f[0]; break;
    case M_ABSOLUTE: s->absolute = 1; break;
    case M_OVERFLOW_HIDDEN: s->overflow_hidden = 1; break;
    case M_BG: s->bg = m->c; s->has_bg = true; s->bg_gradient = NULL; break;
    case M_BG_GRADIENT: s->bg_gradient = m->data; break;
    case M_OBJECT_FIT: s->object_fit = (uint8_t)m->f[0]; break;
    case M_PIXELATED: s->image_pixelated = true; break;
    case M_BORDER_W:
        s->border_uniform = m->f[0];
        s->border_set = 0;   /* all sides: what the sides said before is replaced */
        fold_borders(s);
        break;
    case M_BORDER_SIDE:
        for (int i = 0; i < 4; i++)
            if (m->mask & (1 << i)) { s->border_side[i] = m->f[0]; s->border_set |= (uint8_t)(1 << i); }
        fold_borders(s);
        break;
    case M_BORDER_COLOR: s->border_color = m->c; s->bcolor_set = 0; s->border_gradient = NULL; break;   /* (the last one wins: all four sides, no gradient) */
    case M_BORDER_SIDE_COLOR:
        for (int i = 0; i < 4; i++) if (m->mask & (1 << i)) { s->bcolor[i] = m->c; s->bcolor_set |= (uint8_t)(1 << i); }
        break;
    case M_BORDER_STYLE: s->border_style = (uint8_t)m->f[0]; break;
    case M_BORDER_GRADIENT: s->border_gradient = m->data; break;
    case M_RADIUS: s->radius = m->f[0]; s->corner_set = 0; break;
    case M_CORNER:
        for (int i = 0; i < 4; i++)
            if (m->mask & (1 << i)) { s->corner[i] = m->f[0]; s->corner_set |= (uint8_t)(1 << i); }
        break;
    case M_CORNERS:
        for (int i = 0; i < 4; i++) { s->corner[i] = m->f[i]; }
        s->corner_set = 15;
        break;
    case M_IMG_BRIGHTNESS: s->img_brightness = m->f[0] - 1.f; break;
    case M_IMG_CONTRAST: s->img_contrast = m->f[0] - 1.f; break;
    case M_IMG_SATURATE: s->img_saturate = m->f[0] - 1.f; break;
    case M_IMG_GRAYSCALE: s->img_grayscale = m->f[0] < 0.f ? 0.f : m->f[0] > 1.f ? 1.f : m->f[0]; break;
    case M_IMG_BLUR: s->img_blur = m->f[0] < 0.f ? 0.f : m->f[0] > 200.f ? 200.f : m->f[0]; break;
    case M_IMG_TINT: s->img_tint = m->c; s->has_img_tint = true; break;
    case M_SHADOW:
        s->has_shadow = true;
        s->shadow.dx = m->f[0]; s->shadow.dy = m->f[1]; s->shadow.blur = m->f[2];
        s->shadow.color = m->c;
        break;
    case M_OPACITY: s->opacity = m->f[0]; break;
    case M_HOVER_BG: s->hover_bg = m->c; s->has_hover_bg = true; break;
    case M_ACTIVE_BG: s->active_bg = m->c; s->has_active_bg = true; break;
    case M_CURSOR: s->cursor = (uint8_t)m->f[0]; break;
    case M_TEXT_COLOR: s->text_color = m->c; s->has_text_color = true; break;
    case M_TEXT_SIZE: s->font_size = m->f[0]; break;
    case M_FONT_WEIGHT: s->font_weight = (uint16_t)(m->f[0] < 1.f ? 1 : m->f[0] > 1000.f ? 1000 : (int)(m->f[0] + 0.5f)); break;
    case M_FONT_FAMILY: s->font_family = m->name; break;
    case M_FONT_STYLE: s->font_italic = (uint8_t)m->f[0]; break;
    case M_FONT_FEATURES: s->font_features = m->name; break;
    case M_FONT_VARIATIONS: s->font_variations = m->name; break;
    case M_TEXT_ALIGN: s->text_align = (uint8_t)m->f[0]; s->has_text_align = true; break;
    case M_TEXT_DIR: s->text_dir = (uint8_t)m->f[0]; s->has_text_dir = true; break;
    case M_ON_CLICK:
        el->on_click = (gp_handler){ m->fn, m->data, g_bc->view };
        break;
    case M_ON_MOUSE_DOWN:
        el->on_mouse_down = (gp_handler){ m->fn, m->data, g_bc->view };
        break;
    case M_ON_DRAG: el->on_drag = (gp_handler){ m->fn, m->data, g_bc->view }; break;
    case M_ON_MOUSE_UP: el->on_mouse_up = (gp_handler){ m->fn, m->data, g_bc->view }; break;
    case M_ON_IME: el->on_ime = (gp_handler){ m->fn, m->data, g_bc->view }; break;
    case M_ON_LAYOUT: el->on_layout = (gp_handler){ m->fn, m->data, g_bc->view }; break;
    case M_ON_SCROLL: el->on_scroll = (gp_handler){ m->fn, m->data, g_bc->view }; break;
    case M_ON_WHEEL: el->on_wheel = (gp_handler){ m->fn, m->data, g_bc->view }; break;
    case M_IME_RECT:
        for (int i = 0; i < 4; i++) el->ime_rect[i] = m->f[i];
        el->has_ime_rect = true;
        break;
    case M_CURSOR_TEXT: s->cursor = (uint8_t)m->f[0]; break;
    case M_MODS: {
        gp_node **list = m->data;
        for (uint64_t k = 0; k < m->id; k++)
            if (list[k] && list[k]->tag == GP_TAG_MOD && ((gp_mod *)list[k])->kind != M_MODS)
                apply_mod(el, (gp_mod *)list[k]);
        break;
    }
    case M_TRACK_FOCUS: el->focus_id = m->id; break;
    case M_TAB_STOP: el->tab_stop = true; break;
    case M_TAB_INDEX: el->tab_index = (int)m->f[0]; break;
    case M_FOCUS_BG: s->focus_bg = m->c; s->has_focus_bg = true; break;
    case M_STROKE_W: s->stroke_w = m->f[0] > 0.f ? m->f[0] : 0.f; break;
    case M_STROKE_COLOR: s->stroke_color = m->c; s->has_stroke_color = true; break;
    case M_STROKE_CAP: s->stroke_cap = (uint8_t)m->f[0]; break;
    case M_STROKE_JOIN: s->stroke_join = (uint8_t)m->f[0]; break;
    case M_MITER_LIMIT: s->miter_limit = m->f[0] >= 1.f ? m->f[0] : 1.f; break;
    case M_HIT_TOL: s->hit_tol = m->f[0] > 0.f ? m->f[0] : 0.f; break;
    case M_DASH: s->dash = m->data; s->dash_n = (uint8_t)m->f[0]; break;
    case M_DASH_OFF: s->dash_off = m->f[0]; break;
    case M_STROKE_GRADIENT: s->stroke_gradient = m->data; break;
    case M_ARROW: s->arrow[m->f[0] > 0.5f] = isfinite(m->f[1]) && m->f[1] > 0.f ? m->f[1] : 0.f; break;
    case M_FILL: s->fill_color = m->c; s->has_fill = true; break;
    case M_CLOSED: s->closed = true; break;
    case M_FILL_RULE: s->fill_rule = (uint8_t)m->f[0]; break;
    case M_MARKER: s->marker[(int)m->f[0]] = (uint8_t)m->f[1]; s->marker_size[(int)m->f[0]] = m->f[2]; break;
    case M_PATH_TEXT: s->path_text = m->name; break;
    case M_PATH_TEXT_DY: s->path_text_dy = m->f[0]; break;
    case M_FILTER: {   /* one more at the end of the list: a new array (the old one may be the element's base, which an animation goes back to) */
        if (s->nfilters >= 32) break;
        gp_filter_op *ops = gp_arena_alloc(g_bc->arena, (size_t)(s->nfilters + 1) * sizeof *ops, 8);
        if (s->nfilters) memcpy(ops, s->filters, s->nfilters * sizeof *ops);
        ops[s->nfilters] = (gp_filter_op){ m->mask, m->f[0], m->f[1], m->f[2], m->c };
        s->filters = ops;
        s->nfilters++;
        break;
    }
    case M_BLEND: s->blend_mode = (uint8_t)m->f[0]; break;
    case M_MASK: {
        gp_node *g = m->data;
        if (g && g->tag == GP_TAG_ELEMENT) el->mask = (gp_el *)g;
        break;
    }
    case M_MASK_LUM: s->mask_mode = 1; break;
    case M_BACKDROP: {   /* the group's filters, applied to an element of its own */
        gp_el *tmp = gp_el_new(g_bc->arena, GP_EL_DIV);
        gp_node *g = m->data;
        if (g && g->tag == GP_TAG_MOD) apply_mod(tmp, (const gp_mod *)g);
        s->backdrop = tmp->style.filters;
        s->nbackdrop = tmp->style.nfilters;
        break;
    }
    case M_ROTATE: s->xf_rot = m->f[0]; s->has_xf = true; break;
    case M_SCALE: s->xf_sx = m->f[0]; s->xf_sy = m->f[1]; s->has_xf = true; break;
    case M_TRANSLATE: s->xf_tx = m->f[0]; s->xf_ty = m->f[1]; s->has_xf = true; break;
    case M_XF_ORIGIN: s->xf_ox = m->f[0]; s->xf_oy = m->f[1]; break;
    case M_ROTATE_X: s->xf_rx = m->f[0]; s->has_xf = true; break;
    case M_ROTATE_Y: s->xf_ry = m->f[0]; s->has_xf = true; break;
    case M_TRANSLATE_Z: s->xf_tz = m->f[0]; s->has_xf = true; break;
    case M_XF_PERSP: s->xf_persp = m->f[0]; s->has_xf = true; break;
    case M_PERSP: s->persp = m->f[0]; break;
    case M_PERSP_ORIGIN: s->persp_ox = m->f[0]; s->persp_oy = m->f[1]; break;
    case M_BACKFACE: s->backface_hidden = true; break;
    case M_KEY: el->key = m->id; break;
    case M_SKEW: s->xf_kx = isfinite(m->f[0]) ? m->f[0] : 0.f; s->xf_ky = isfinite(m->f[1]) ? m->f[1] : 0.f; s->has_xf = true; break;
    case M_HOVER_STYLE: case M_ACTIVE_STYLE: case M_FOCUS_STYLE: {
        /* the group is applied to an element of its own, whose style is what the state paints over this one's, and what it lays out
         * (layout keeps the element's style without the states in lsave) */
        gp_el *tmp = gp_el_new(g_bc->arena, GP_EL_DIV);
        gp_node *g = m->data;
        if (g && g->tag == GP_TAG_MOD) apply_mod(tmp, (const gp_mod *)g);
        if (tmp->style.lset && !el->lsave && !g_in_animation) el->lsave = gp_arena_alloc(g_bc->arena, sizeof *el->lsave, 16);
        if (m->kind == M_HOVER_STYLE) s->hover_style = &tmp->style;
        else if (m->kind == M_ACTIVE_STYLE) s->active_style = &tmp->style;
        else s->focus_style = &tmp->style;
        break;
    }
    case M_FOCUS_BORDER: s->focus_border = m->c; s->has_focus_border = true; break;
    case M_KEY_CONTEXT: el->key_context = m->name; break;
    case M_ON_ACTION: {
        gp_action_handler *h = gp_arena_alloc(g_bc->arena, sizeof *h, 8);
        *h = (gp_action_handler){ m->name, (gp_action_fn)m->fn, m->data, g_bc->view, el->actions };
        el->actions = h;
        break;
    }
    case M_ON_KEY_DOWN: el->on_key_down = (gp_handler){ m->fn, m->data, g_bc->view }; break;
    case M_ON_KEY_UP: el->on_key_up = (gp_handler){ m->fn, m->data, g_bc->view }; break;
    case M_ON_TEXT: el->on_text = (gp_handler){ m->fn, m->data, g_bc->view }; break;
    case M_WHITESPACE_PRE: s->whitespace_pre = true; break;
    case M_SCROLL_X: s->overflow_x_scroll = 1; if (m->mask) s->overflow_y_scroll = 1; break;
    case M_SCROLL_Y: s->overflow_y_scroll = 1; break;
    case M_TRACK_SCROLL: el->scroll_id = m->id; break;
    case M_SCROLLBAR_COLOR: s->scrollbar_color = m->c; s->has_scrollbar_color = true; break;
    case M_HIDE_SCROLLBAR: s->hide_scrollbar = 1; break;
    case M_AUTOHIDE: s->autohide_scrollbar = 1; el->hide_below = true; break;
    case M_LIST_STICK: el->list_stick = true; break;
    case M_LIST_HORIZ:
        if (el->kind == GP_EL_LIST) {   /* a uniform list or a gp_list: the items go along x */
            el->list_horiz = true;
            el->style.overflow_x_scroll = 1;
            el->style.overflow_y_scroll = 0;
        } else {
            static bool warned;
            if (!warned) { warned = true; fprintf(stderr, "gpuic: gp_list_horizontal is for gp_uniform_list and gp_list only\n"); }
        }
        break;
    case M_LIST_STICKY: el->sticky_fn = (gp_list_group_fn)m->fn; el->sticky_data = m->data; break;
    }
}

/* Calls an animation's function and puts what it returns on the element's style. The function builds its
 * modifiers like a render function does, so the build context is set while it runs. */
void gp_el_apply_animation(gp_el *el, const gp_anim *a, gp_build_ctx *bc) {
    gp_build_ctx *prev = g_bc;
    bool prev_flag = g_in_animation;
    g_bc = bc;
    g_in_animation = true;
    gp_node *mods = a->fn ? a->fn(a->cur_t, a->data) : NULL;
    if (mods && mods->tag == GP_TAG_MOD) apply_mod(el, (gp_mod *)mods);
    g_in_animation = prev_flag;
    g_bc = prev;
}

/* Applies mods to `el`, collects element children (second pass over `ap`). */
static void collect(gp_el *el, va_list ap, bool allow_children) {
    va_list ap2;
    va_copy(ap2, ap);
    uint32_t nchildren = 0;
    for (gp_node *n; (n = va_arg(ap2, gp_node *)) != GP_END;) {
        if (!n || !allow_children) continue;
        if (n->tag == GP_TAG_ELEMENT) {
            nchildren++;
        } else if (n->tag == GP_TAG_MOD && ((gp_mod *)n)->kind == M_CHILDREN) {
            gp_node **list = ((gp_mod *)n)->data;
            for (uint64_t k = 0; k < ((gp_mod *)n)->id; k++)
                if (list[k] && list[k]->tag == GP_TAG_ELEMENT) nchildren++;
        }
    }
    va_end(ap2);

    if (nchildren) {
        el->children = gp_arena_alloc(g_bc->arena, nchildren * sizeof *el->children, 8);
        el->nchildren = nchildren;
    }
    uint32_t i = 0;
    for (gp_node *n; (n = va_arg(ap, gp_node *)) != GP_END;) {
        if (!n) continue;
        if (n->tag == GP_TAG_MOD && ((gp_mod *)n)->kind == M_CHILDREN) {
            gp_node **list = ((gp_mod *)n)->data;
            for (uint64_t k = 0; allow_children && k < ((gp_mod *)n)->id; k++)
                if (list[k] && list[k]->tag == GP_TAG_ELEMENT) {
                    el->children[i++] = (gp_el *)list[k];
                    if (((gp_el *)list[k])->anim_below) el->anim_below = true;
                    if (((gp_el *)list[k])->hide_below) el->hide_below = true;
                }
        } else if (n->tag == GP_TAG_MOD) {
            apply_mod(el, (gp_mod *)n);
        } else if (n->tag == GP_TAG_ELEMENT && allow_children) {
            el->children[i++] = (gp_el *)n;
            if (((gp_el *)n)->anim_below) el->anim_below = true;
            if (((gp_el *)n)->hide_below) el->hide_below = true;
        }
    }
}

gp_node *gp_div_(int unused, ...) {
    (void)unused;
    gp_el *el = gp_el_new(g_bc->arena, GP_EL_DIV);
    va_list ap;
    va_start(ap, unused);
    collect(el, ap, true);
    va_end(ap);
    return &el->base;
}

/* A stroked curve or polyline: an absolutely placed element whose box is the bounding box of its points, padded by half the stroke width (more for a miter join) and the
 * hit tolerance. Its points are in the parent's space (the one gp_left and gp_top use); they are kept relative to the box. Nothing about it depends on a layout. */
static gp_node *make_curve(int kind, const gp_point *pts, size_t n, va_list ap) {
    gp_el *el = gp_el_new(g_bc->arena, GP_EL_CURVE);
    el->curve_kind = (uint8_t)kind;
    collect(el, ap, false);
    gp_style *s = &el->style;
    size_t good = 0;
    float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (size_t i = 0; i < n; i++) {   /* a point that is not a number, or is absurd, ends the points there: nothing is drawn past it */
        if (!(isfinite(pts[i].x) && isfinite(pts[i].y) && fabsf(pts[i].x) < 1e7f && fabsf(pts[i].y) < 1e7f)) break;
        x0 = fminf(x0, pts[i].x); y0 = fminf(y0, pts[i].y);
        x1 = fmaxf(x1, pts[i].x); y1 = fmaxf(y1, pts[i].y);
        good++;
    }
    if (kind == 0 && good != 4) good = 0;
    if (!good) x0 = y0 = x1 = y1 = 0;
    float hw = s->stroke_w * 0.5f;
    float pad = hw * (s->stroke_join == 1 && kind == 1 ? fmaxf(1.f, s->miter_limit) : 1.f) + s->hit_tol;
    pad = fmaxf(pad, fmaxf(s->arrow[0], s->arrow[1]) * 0.5f + s->hit_tol);   /* an arrowhead is as wide as it is long */
    for (int i = 0; i < 3; i++)   /* a marker turned on the line: its corners up to half its diagonal away */
        if (s->marker[i]) pad = fmaxf(pad, fmaxf(s->marker_size[i], s->stroke_w) * 0.7072f + s->hit_tol);
    if (s->path_text) pad = fmaxf(pad, (s->font_size > 0.f ? s->font_size : 16.f) * 1.5f + fabsf(s->path_text_dy));   /* text along it stands up to about a line from the centerline */
    el->curve_n = (uint32_t)good;
    el->curve_pts = gp_arena_alloc(g_bc->arena, (good ? good : 1) * sizeof *el->curve_pts, 4);
    for (size_t i = 0; i < good; i++) el->curve_pts[i] = (gp_point){ pts[i].x - (x0 - pad), pts[i].y - (y0 - pad) };
    s->absolute = 1;
    s->inset[0] = (gp_length){ GP_PX, x0 - pad };
    s->inset[1] = (gp_length){ GP_PX, y0 - pad };
    s->inset[2] = s->inset[3] = (gp_length){ GP_AUTO, 0 };
    s->w = (gp_length){ GP_PX, x1 - x0 + 2 * pad };
    s->h = (gp_length){ GP_PX, y1 - y0 + 2 * pad };
    s->min_w = s->min_h = s->max_w = s->max_h = (gp_length){ GP_AUTO, 0 };
    s->basis = (gp_length){ GP_AUTO, 0 };
    return &el->base;
}

gp_node *gp_curve_(gp_point p0, gp_point c1, gp_point c2, gp_point p3, ...) {
    gp_point pts[4] = { p0, c1, c2, p3 };
    va_list ap;
    va_start(ap, p3);
    gp_node *n = make_curve(0, pts, 4, ap);
    va_end(ap);
    return n;
}

gp_node *gp_polyline_(const gp_point *pts, size_t n, ...) {
    va_list ap;
    va_start(ap, n);
    gp_node *r = make_curve(1, pts, pts ? n : 0, ap);
    va_end(ap);
    return r;
}

gp_node *gp_img_(gp_image image, ...) {
    gp_el *el = gp_el_new(g_bc->arena, GP_EL_IMAGE);
    el->image = image;
    va_list ap;
    va_start(ap, image);
    collect(el, ap, false);
    va_end(ap);
    return &el->base;
}

gp_node *gp_svg_img_(gp_svg svg, ...) {
    gp_el *el = gp_el_new(g_bc->arena, GP_EL_SVG);
    el->svg = svg;
    gp_view_uses_svg(g_bc->window, g_bc->view, svg);   /* (releasing it renders this view again) */
    va_list ap;
    va_start(ap, svg);
    collect(el, ap, false);
    va_end(ap);
    return &el->base;
}

gp_node *gp_object_fit(int fit) { return mod_f(M_OBJECT_FIT, (float)fit); }
gp_node *gp_image_pixelated(void) { return mod_f(M_PIXELATED, 0); }

gp_node *gp_text_(const char *text, ...) {
    gp_el *el = gp_el_new(g_bc->arena, GP_EL_TEXT);
    el->text = gp_arena_strdup(g_bc->arena, text ? text : "");
    va_list ap;
    va_start(ap, text);
    collect(el, ap, false);
    va_end(ap);
    return &el->base;
}

/* ---------------------------------------------------------------- uniform list */

gp_node *gp_uniform_list_(size_t count, float item_height, gp_list_item_fn render, void *data, ...) {
    gp_el *el = gp_el_new(g_bc->arena, GP_EL_LIST);
    el->list_fn = render;
    el->list_data = data;
    el->list_count = count;
    el->list_item_h = item_height;
    el->list_bc = (gp_build_ctx){ NULL, g_bc->app, g_bc->window, g_bc->view, true };
    el->style.overflow_y_scroll = 1;
    va_list ap;
    va_start(ap, data);
    collect(el, ap, false);
    va_end(ap);
    return &el->base;
}

gp_node *gp_list_(size_t count, float estimated_height, gp_list_item_fn render, void *data, ...) {
    gp_el *el = gp_el_new(g_bc->arena, GP_EL_LIST);
    el->list_fn = render;
    el->list_data = data;
    el->list_count = count;
    el->list_item_h = estimated_height;
    el->list_var = true;
    el->list_bc = (gp_build_ctx){ NULL, g_bc->app, g_bc->window, g_bc->view, true };
    el->style.overflow_y_scroll = 1;
    va_list ap;
    va_start(ap, data);
    collect(el, ap, false);
    va_end(ap);
    return &el->base;
}

/* Called from layout, outside any render function: rows are built into `arena` on behalf
 * of the view that owns the list, so their handlers dispatch to that view's state. */
/* What gp_list_sticky's function says about `row`, with its header made into `arena` (as a row is). False if the list has none. */
bool gp_list_group_build(gp_el *list, size_t row, gp_arena *arena, gp_list_group *out, gp_el **header) {
    if (!list->sticky_fn) return false;
    gp_build_ctx bc = list->list_bc;
    bc.arena = arena;
    gp_build_ctx *prev = g_bc;
    g_bc = &bc;
    void *state = gp_entity_get(bc.app, bc.view);
    gp_ctx cx = { bc.app, bc.window, bc.view, false };
    *header = NULL;
    if (state) {
        *out = list->sticky_fn(state, &cx, row, list->sticky_data);
        if (out->header && out->header->tag == GP_TAG_ELEMENT) *header = (gp_el *)out->header;
    }
    g_bc = prev;
    return *header != NULL;
}

gp_el **gp_list_build(gp_el *list, size_t first, size_t n, gp_arena *arena) {
    gp_build_ctx bc = list->list_bc;
    bc.arena = arena;
    gp_build_ctx *prev = g_bc;
    g_bc = &bc;

    gp_el **out = gp_arena_alloc(arena, (n ? n : 1) * sizeof *out, 8);
    void *state = gp_entity_get(bc.app, bc.view);
    gp_ctx cx = { bc.app, bc.window, bc.view, false };
    for (size_t i = 0; i < n; i++) {
        gp_node *node = state ? list->list_fn(state, &cx, first + i, list->list_data) : NULL;
        out[i] = node && node->tag == GP_TAG_ELEMENT ? (gp_el *)node : (gp_el *)gp_div_(0, GP_END);
    }
    g_bc = prev;
    return out;
}

/* ---------------------------------------------------------------- views */

/* Renders one view into `bc->arena`. */
static gp_el *render_view(gp_build_ctx *bc, gp_entity view) {
    gp_build_ctx *prev = g_bc;
    g_bc = bc;
    gp_entity prev_view = bc->view;
    bc->view = view;

    gp_node *n = NULL;
    gp_entity_slot *slot = NULL;
    if (view.index < bc->app->nslots) {
        slot = &bc->app->slots[view.index];
        if (!slot->alive || slot->generation != view.generation) slot = NULL;
    }
    if (slot && slot->render) {
        gp_ctx cx = { bc->app, bc->window, view, false };
        bc->app->stat_view_renders++;
        n = slot->render(slot->state, &cx);
    }
    gp_el *el = (n && n->tag == GP_TAG_ELEMENT) ? (gp_el *)n : (gp_el *)gp_div_(0, GP_END);

    bc->view = prev_view;
    g_bc = prev;
    return el;
}

/* A view's element tree. A cached view whose entity, and every view built inside it, has not been
 * notified since the last build gives back that build's tree untouched; otherwise it renders again,
 * into an arena of its own (so the tree survives the window's arena being reset). Rows of a list never
 * reuse anything, and a cached view that appears twice in one build is rendered afresh for the second
 * place, because one tree cannot be laid out in two. */
gp_el *gp_build_view(gp_build_ctx *bc, gp_entity view, bool cached) {
    gp_window *w = bc->window;
    gp_vview *vv = w ? gp_views_touch(&w->views, view, bc->view) : NULL;
    bool again = false;   /* already reached in this build */
    if (vv) {
        again = vv->used_epoch == w->views.epoch;
        vv->used_epoch = w->views.epoch;
        if (!again) vv->from_row = bc->no_cache;
    }
    bool reuse = cached && vv && !bc->no_cache && !again;

    if (reuse && vv->cached && vv->root && !vv->dirty && !vv->subtree_dirty && !w->views.all_dirty) {
        gp_views_keep_subtree(&w->views, vv);
        if (bc->view.generation) gp_views_add_kid(&w->views, bc->view, view);
        return vv->root;
    }

    if (vv && !again) {   /* cleared first: a notify made by the render itself stays for the next build */
        vv->dirty = vv->subtree_dirty = false;
        vv->reads_focus = false;
        vv->nsvgs = 0;
        vv->nkids = 0;
        if (bc->view.generation && !bc->no_cache) gp_views_add_kid(&w->views, bc->view, view);
    }
    /* Rendering builds the views inside this one, which can grow the table: no entry pointer is kept
     * across it. */
    if (!reuse) {
        gp_el *el = render_view(bc, view);
        if (vv && !again) gp_view_drop_cache(gp_views_find(&w->views, view));   /* no longer cached: its old tree is dead */
        return el;
    }
    gp_build_ctx own = *bc;
    own.arena = gp_view_arena(vv);
    gp_arena_reset(own.arena);
    gp_el *el = render_view(&own, view);
    vv = gp_views_find(&w->views, view);
    vv->root = el;
    vv->cached = true;
    vv->chunk_valid = false;      /* new elements: whatever was painted for the old ones is gone */
    el->cached_view = view;       /* layout and paint recognise the root, and may reuse their work on it */
    return el;
}

gp_node *gp_child_view(gp_entity view) {
    if (!g_bc) return NULL;
    return &gp_build_view(g_bc, view, false)->base;
}

gp_node *gp_child_view_cached(gp_entity view) {
    if (!g_bc) return NULL;
    return &gp_build_view(g_bc, view, true)->base;
}

gp_entity gp_building_view(void) { return g_bc ? g_bc->view : GP_ENTITY_NULL; }

/* ---------------------------------------------------------------- hit testing */

static bool has_handler(const gp_el *el, gp_hit_kind kind) {
    switch (kind) {
    case GP_HIT_CLICK: return el->on_click.fn != NULL;
    case GP_HIT_MOUSE_DOWN: return el->on_mouse_down.fn != NULL;
    case GP_HIT_CURSOR: return el->style.cursor != GP_CURSOR_DEFAULT;
    case GP_HIT_FOCUS: return el->focus_id != 0;
    case GP_HIT_WHEEL: return el->on_wheel.fn != NULL;
    }
    return false;
}

typedef struct { float x0, y0, x1, y1; } rect;

static bool rect_has(rect r, gp_point p) { return p.x >= r.x0 && p.x < r.x1 && p.y >= r.y0 && p.y < r.y1; }

static bool aff_box(gp_affine m, rect r, rect *out) {   /* a rectangle through a transform that keeps the axes: the box it becomes */
    if (gp_aff_projective(m)) return false;   /* (in perspective a box is not a box) */
    bool straight = (fabsf(m.b) < 1e-5f && fabsf(m.c) < 1e-5f) || (fabsf(m.a) < 1e-5f && fabsf(m.d) < 1e-5f);
    if (!straight) return false;
    gp_point p0 = gp_aff_apply(m, (gp_point){ r.x0, r.y0 }), p1 = gp_aff_apply(m, (gp_point){ r.x1, r.y1 });
    out->x0 = fminf(p0.x, p1.x); out->x1 = fmaxf(p0.x, p1.x);
    out->y0 = fminf(p0.y, p1.y); out->y1 = fmaxf(p0.y, p1.y);
    return true;
}

/* `p` is the point in the window; `fwd` (if `moved`) maps layout coordinates to the window, so the point in this element's own coordinates is its inverse
 * applied to p, and the clip, which is in the window, is tested with p. */
static const uint64_t *g_hit_skip;   /* gp_hit_test_skipping: the paths not to report (the wheel goes on to the next handler when one lets it) */
static int g_hit_nskip;

static bool skipped(uint64_t path) {
    for (int i = 0; i < g_hit_nskip; i++)
        if (g_hit_skip[i] == path) return true;
    return false;
}

static bool hit_rec(gp_el *n, float ox, float oy, rect clip, gp_point p, gp_hit_kind kind,
                    uint64_t path, gp_hit *out, gp_affine fwd, bool moved) {
    float x = ox + n->pos.x, y = oy + n->pos.y;
    rect b = { x, y, x + n->size.w, y + n->size.h };
    if (n->path) path = n->path;   /* (what layout knows it by: its place, or its key) */
    if (n->style.has_xf) {
        fwd = gp_aff_mul(moved ? fwd : gp_aff_identity(), gp_el_affine(n, x, y));
        moved = true;
        if (gp_el_behind(n, fwd, x, y) || (n->style.backface_hidden && gp_el_backfacing(n, fwd, x, y))) return false;   /* behind the viewer, or turned away: not there for the pointer either */
    }
    gp_point q = moved ? gp_aff_apply(gp_aff_inverse(fwd), p) : p;
    rect child_clip = clip;
    float cx = x - n->scroll.x, cy = y - n->scroll.y; /* children live in scrolled space */
    if (n->style.overflow_hidden || n->style.overflow_x_scroll || n->style.overflow_y_scroll) {
        rect box = b;
        if (!moved || aff_box(fwd, b, &box)) {
            child_clip.x0 = fmaxf(clip.x0, box.x0); child_clip.y0 = fmaxf(clip.y0, box.y0);
            child_clip.x1 = fminf(clip.x1, box.x1); child_clip.y1 = fminf(clip.y1, box.y1);
        }
    }
    if (n->sticky && hit_rec(n->sticky, x, y, child_clip, p, kind, (path ^ 0x57ac4ull) * 1099511628211ull, out, fwd, moved)) return true;   /* a pinned header is above the rows */
    for (uint32_t i = n->nchildren; i-- > 0;) {
        uint64_t cp = (path ^ (i + 1)) * 1099511628211ull;
        if (hit_rec(n->children[i], cx, cy, child_clip, p, kind, cp, out, fwd, moved)) return true;
    }
    if (has_handler(n, kind) && !(g_hit_nskip && skipped(path)) && rect_has(clip, p) && (n->kind == GP_EL_CURVE ? rect_has(b, q) && gp_curve_hit(n, q.x - x, q.y - y) : rect_has(b, q))) {
        out->el = n;
        out->path = path;
        out->ox = x;
        out->oy = y;
        out->qx = q.x;
        out->qy = q.y;
        return true;
    }
    return false;
}

bool gp_hit_test_skipping(gp_el *root, gp_point p, gp_hit_kind kind, gp_hit *out, const uint64_t *skip, int nskip) {
    g_hit_skip = skip;
    g_hit_nskip = nskip;
    bool r = gp_hit_test(root, p, kind, out);
    g_hit_skip = NULL;
    g_hit_nskip = 0;
    return r;
}

bool gp_hit_test(gp_el *root, gp_point p, gp_hit_kind kind, gp_hit *out) {
    if (!root) return false;
    rect everything = { -1e9f, -1e9f, 1e9f, 1e9f };
    return hit_rec(root, 0, 0, everything, p, kind, 14695981039346656037ull, out, gp_aff_identity(), false);
}
