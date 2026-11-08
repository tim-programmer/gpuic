/*
 * Graphics resources shared by every window: the table of gradient ramps (here) and, below, images.
 *
 * A gradient is painted by the fragment shader from a ramp: a row of GP_GRAD_W premultiplied RGBA8 samples of its color
 * stops, looked up with the position along the gradient line. A ramp depends only on the stops, so every box with the
 * same stops shares one row, whatever its size or angle; an instance carries the row in its uv field. Rows are never
 * freed one by one: when the table is full the frame is finished without the gradient that did not fit, the table is cleared
 * together with the glyph atlases and everything is painted again (like an atlas overflow).
 */
#include "internal.h"

#include <math.h>
#include <string.h>

typedef struct { char *src; size_t len; float w, h; uint32_t generation; } svg_slot;
typedef struct { gp_svg svg; int w, h; uint32_t rgb; gp_image image; } svg_raster;

struct gp_gfx {
    svg_slot *svgs;                                   /* documents; a slot with a NULL src is free (its generation goes on counting) */
    uint32_t svg_cap;
    svg_raster *rasters;                              /* what was drawn from them, newest last */
    int nrasters, cap_rasters;
    size_t raster_bytes, budget_bytes;
    int budget_count;
    gp_gfx_image *images;                             /* slots; a slot with generation 0 is free */
    uint32_t ncap;
    uint32_t *free_slots;
    uint32_t nfree;
    uint32_t *dead;                                   /* textures of released images, waiting for the renderer to delete them */
    uint32_t ndead, cap_dead;
    int alive;
    uint8_t *ramps;                                   /* GP_GRAD_ROWS x GP_GRAD_W x 4 */
    gp_gradient keys[GP_GRAD_ROWS];                   /* the stops each row was made from (the rest of gp_gradient is unused) */
    int nrows;
    int dirty0, dirty1;                               /* rows to upload; empty when dirty1 <= dirty0 */
    bool overflowed;
    gp_text_system *ts;                               /* whose fonts SVG <text> is drawn with */
};

void gp_gfx_set_text(gp_gfx *g, gp_text_system *ts) { g->ts = ts; }

gp_gfx *gp_gfx_new(void) {
    gp_gfx *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->ramps = calloc((size_t)GP_GRAD_ROWS * GP_GRAD_W, 4);
    if (!g->ramps) { free(g); return NULL; }
    g->dirty0 = 0;
    g->dirty1 = GP_GRAD_ROWS;   /* the first upload covers the whole (empty) table */
    g->budget_bytes = 96u << 20;
    g->budget_count = 512;
    return g;
}

void gp_gfx_free(gp_gfx *g) {
    if (!g) return;
    for (uint32_t i = 0; i < g->svg_cap; i++) free(g->svgs[i].src);
    free(g->svgs);
    free(g->rasters);
    for (uint32_t i = 0; i < g->ncap; i++) free(g->images[i].pixels);
    free(g->images);
    free(g->free_slots);
    free(g->dead);
    free(g->ramps);
    free(g);
}

/* ---------------------------------------------------------------- stops */

static float clamp01(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }

void gp_gradient_normalize(gp_gradient *out, const gp_color_stop *stops, int n) {
    if (n > GP_GRAD_MAX_STOPS) n = GP_GRAD_MAX_STOPS;
    if (n < 0) n = 0;
    out->n = n;
    for (int i = 0; i < n; i++) out->stops[i] = stops[i];
    if (!n) return;
    /* no position: the ends default to 0 and 1, the others are spread evenly between the stops around them that have one */
    if (out->stops[0].at < 0.f) out->stops[0].at = 0.f;
    if (out->stops[n - 1].at < 0.f) out->stops[n - 1].at = 1.f;
    for (int i = 1; i < n - 1;) {
        if (out->stops[i].at >= 0.f) { i++; continue; }
        int j = i;
        while (out->stops[j].at < 0.f) j++;   /* the last stop has one, so this ends */
        float a = out->stops[i - 1].at, b = out->stops[j].at;
        for (int k = i; k < j; k++) out->stops[k].at = a + (b - a) * (float)(k - i + 1) / (float)(j - i + 1);
        i = j + 1;
    }
    float prev = 0.f;
    for (int i = 0; i < n; i++) {
        float at = clamp01(out->stops[i].at);
        if (at < prev) at = prev;   /* going backwards: a hard edge at the previous position */
        out->stops[i].at = at;
        prev = at;
        out->stops[i].color.r = clamp01(out->stops[i].color.r);
        out->stops[i].color.g = clamp01(out->stops[i].color.g);
        out->stops[i].color.b = clamp01(out->stops[i].color.b);
        out->stops[i].color.a = clamp01(out->stops[i].color.a);
    }
}

/* ---------------------------------------------------------------- ramps */

static void bake(uint8_t *row, const gp_gradient *gr) {
    for (int i = 0; i < GP_GRAD_W; i++) {
        float t = ((float)i + 0.5f) / (float)GP_GRAD_W;
        float rgba[4];
        if (gr->n == 0) {
            rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0.f;
        } else if (t <= gr->stops[0].at) {
            const gp_color c = gr->stops[0].color;
            rgba[0] = c.r * c.a; rgba[1] = c.g * c.a; rgba[2] = c.b * c.a; rgba[3] = c.a;
        } else if (t >= gr->stops[gr->n - 1].at) {
            const gp_color c = gr->stops[gr->n - 1].color;
            rgba[0] = c.r * c.a; rgba[1] = c.g * c.a; rgba[2] = c.b * c.a; rgba[3] = c.a;
        } else {
            int k = 1;
            while (k < gr->n - 1 && t > gr->stops[k].at) k++;   /* t lies in (stops[k - 1], stops[k]] */
            const gp_color_stop *a = &gr->stops[k - 1], *b = &gr->stops[k];
            float span = b->at - a->at, u = span > 0.f ? (t - a->at) / span : 1.f;
            float ar[4] = { a->color.r * a->color.a, a->color.g * a->color.a, a->color.b * a->color.a, a->color.a };
            float br[4] = { b->color.r * b->color.a, b->color.g * b->color.a, b->color.b * b->color.a, b->color.a };
            for (int c = 0; c < 4; c++) rgba[c] = ar[c] + (br[c] - ar[c]) * u;   /* premultiplied: CSS's way */
        }
        for (int c = 0; c < 4; c++) row[i * 4 + c] = (uint8_t)lroundf(clamp01(rgba[c]) * 255.f);
    }
}

static bool same_stops(const gp_gradient *a, const gp_gradient *b) {
    if (a->n != b->n) return false;
    for (int i = 0; i < a->n; i++)
        if (a->stops[i].at != b->stops[i].at || a->stops[i].color.r != b->stops[i].color.r || a->stops[i].color.g != b->stops[i].color.g ||
            a->stops[i].color.b != b->stops[i].color.b || a->stops[i].color.a != b->stops[i].color.a)
            return false;
    return true;
}

int gp_gfx_gradient_row(gp_gfx *g, const gp_gradient *grad) {
    for (int i = 0; i < g->nrows; i++)
        if (same_stops(&g->keys[i], grad)) return i;
    if (g->nrows == GP_GRAD_ROWS) {
        g->overflowed = true;
        return -1;
    }
    int row = g->nrows++;
    g->keys[row] = *grad;
    bake(g->ramps + (size_t)row * GP_GRAD_W * 4, grad);
    if (g->dirty1 <= g->dirty0) { g->dirty0 = row; g->dirty1 = row + 1; }
    else {
        if (row < g->dirty0) g->dirty0 = row;
        if (row + 1 > g->dirty1) g->dirty1 = row + 1;
    }
    return row;
}

const uint8_t *gp_gfx_gradient_data(const gp_gfx *g) { return g->ramps; }

bool gp_gfx_gradient_take_dirty(gp_gfx *g, int *row0, int *row1) {
    if (g->dirty1 <= g->dirty0) return false;
    *row0 = g->dirty0;
    *row1 = g->dirty1;
    g->dirty0 = g->dirty1 = 0;
    return true;
}

bool gp_gfx_overflowed(const gp_gfx *g) { return g->overflowed; }
int gp_gfx_gradient_count(const gp_gfx *g) { return g->nrows; }

static void drop_rasters(gp_gfx *g) {
    for (int i = 0; i < g->nrasters; i++) gp_gfx_image_release(g, g->rasters[i].image);
    g->nrasters = 0;
    g->raster_bytes = 0;
}

void gp_gfx_reset(gp_gfx *g) {
    g->nrows = 0;
    g->overflowed = false;
    g->dirty0 = g->dirty1 = 0;
    drop_rasters(g);   /* painted copies hold their image numbers, and the app ages them along with this */
}

/* ---------------------------------------------------------------- images */

gp_image gp_gfx_image_add(gp_gfx *g, int w, int h, uint8_t *premultiplied) {
    if (!premultiplied) return GP_IMAGE_NULL;
    if (w <= 0 || h <= 0 || w > GP_IMAGE_MAX_DIM || h > GP_IMAGE_MAX_DIM) {
        free(premultiplied);
        return GP_IMAGE_NULL;
    }
    if (!g->nfree) {   /* grow: the new slots go on the free list, the lowest index to be taken first */
        uint32_t ncap = g->ncap ? g->ncap * 2 : 16;
        gp_gfx_image *grown = realloc(g->images, (size_t)ncap * sizeof *grown);
        uint32_t *fl = grown ? realloc(g->free_slots, (size_t)ncap * sizeof *fl) : NULL;
        if (!grown || !fl) {
            if (grown) g->images = grown;
            free(premultiplied);
            return GP_IMAGE_NULL;
        }
        g->images = grown;
        g->free_slots = fl;
        for (uint32_t i = g->ncap; i < ncap; i++) g->images[i] = (gp_gfx_image){ 0 };
        for (uint32_t i = ncap; i-- > g->ncap;) g->free_slots[g->nfree++] = i;
        g->ncap = ncap;
    }
    uint32_t index = g->free_slots[--g->nfree];
    gp_gfx_image *im = &g->images[index];
    uint32_t gen = im->generation;   /* a freed slot keeps counting, so a stale handle stays stale */
    im->pixels = premultiplied;
    im->w = w;
    im->h = h;
    im->gl_tex = 0;
    im->generation = gen + 1;
    g->alive++;
    return (gp_image){ index, im->generation };
}

gp_gfx_image *gp_gfx_image_get(gp_gfx *g, uint32_t index, uint32_t generation) {
    if (!g || generation == 0 || index >= g->ncap || g->images[index].generation != generation || !g->images[index].pixels) return NULL;
    return &g->images[index];
}

bool gp_gfx_image_size(gp_gfx *g, gp_image image, int *w, int *h) {
    gp_gfx_image *im = gp_gfx_image_get(g, image.index, image.generation);
    if (!im) return false;
    if (w) *w = im->w;
    if (h) *h = im->h;
    return true;
}

void gp_gfx_image_release(gp_gfx *g, gp_image image) {
    gp_gfx_image *im = gp_gfx_image_get(g, image.index, image.generation);
    if (!im) return;
    if (im->gl_tex) {
        if (g->ndead == g->cap_dead) {
            g->cap_dead = g->cap_dead ? g->cap_dead * 2 : 8;
            g->dead = realloc(g->dead, (size_t)g->cap_dead * sizeof *g->dead);
        }
        g->dead[g->ndead++] = im->gl_tex;
    }
    free(im->pixels);
    uint32_t keep = im->generation;   /* the slot goes on counting from here */
    *im = (gp_gfx_image){ 0 };
    im->generation = keep;
    /* a free slot is told from a live one by its pixels; generation stays so the next image there is a new generation */
    g->free_slots[g->nfree++] = image.index;   /* (the list was sized for every slot when they were made) */
    g->alive--;
}

int gp_gfx_image_count(const gp_gfx *g) { return g->alive; }

size_t gp_gfx_take_dead_textures(gp_gfx *g, uint32_t *out, size_t cap) {
    size_t n = g->ndead < cap ? g->ndead : cap;
    if (n == 0) return 0;   /* (g->dead may be NULL: memcpy and memmove must not see a null pointer, even for no bytes) */
    memcpy(out, g->dead, n * sizeof *out);
    memmove(g->dead, g->dead + n, (g->ndead - n) * sizeof *g->dead);
    g->ndead -= (uint32_t)n;
    return n;
}

size_t gp_gfx_take_all_textures(gp_gfx *g, uint32_t *out, size_t cap) {
    size_t n = 0;
    for (uint32_t i = 0; i < g->ncap && n < cap; i++)
        if (g->images[i].pixels && g->images[i].gl_tex) {
            out[n++] = g->images[i].gl_tex;
            g->images[i].gl_tex = 0;
        }
    return n;
}

/* ---------------------------------------------------------------- SVG documents and their rasters */

gp_svg gp_gfx_svg_add(gp_gfx *g, char *text, size_t len, float w, float h) {
    uint32_t index = g->svg_cap;
    for (uint32_t i = 0; i < g->svg_cap; i++)
        if (!g->svgs[i].src) { index = i; break; }
    if (index == g->svg_cap) {
        uint32_t cap = g->svg_cap ? g->svg_cap * 2 : 8;
        svg_slot *grown = realloc(g->svgs, (size_t)cap * sizeof *grown);
        if (!grown) {
            free(text);
            return GP_SVG_NULL;
        }
        g->svgs = grown;
        memset(g->svgs + g->svg_cap, 0, (size_t)(cap - g->svg_cap) * sizeof *grown);
        g->svg_cap = cap;
    }
    svg_slot *sl = &g->svgs[index];
    sl->src = text;
    sl->len = len;
    sl->w = w;
    sl->h = h;
    sl->generation++;
    return (gp_svg){ index, sl->generation };
}

static svg_slot *svg_get(const gp_gfx *g, gp_svg svg) {
    if (!g || svg.generation == 0 || svg.index >= g->svg_cap) return NULL;
    svg_slot *sl = &g->svgs[svg.index];
    return sl->src && sl->generation == svg.generation ? sl : NULL;
}

bool gp_gfx_svg_size(gp_gfx *g, gp_svg svg, float *w, float *h) {
    svg_slot *sl = svg_get(g, svg);
    if (!sl) return false;
    if (w) *w = sl->w;
    if (h) *h = sl->h;
    return true;
}

void gp_gfx_svg_release(gp_gfx *g, gp_svg svg) {
    svg_slot *sl = svg_get(g, svg);
    if (!sl) return;
    for (int i = 0; i < g->nrasters;) {
        if (g->rasters[i].svg.index == svg.index && g->rasters[i].svg.generation == svg.generation) {
            int bw = 0, bh = 0;
            gp_gfx_image_size(g, g->rasters[i].image, &bw, &bh);
            g->raster_bytes -= (size_t)bw * (size_t)bh * 4;
            gp_gfx_image_release(g, g->rasters[i].image);
            g->rasters[i] = g->rasters[--g->nrasters];
        } else {
            i++;
        }
    }
    free(sl->src);
    sl->src = NULL;
}

gp_image gp_gfx_svg_raster(gp_gfx *g, gp_svg svg, int w, uint32_t rgb) {
    svg_slot *sl = svg_get(g, svg);
    if (!sl || w <= 0) return GP_IMAGE_NULL;
    int h = (int)lroundf((float)w * sl->h / sl->w);
    if (h < 1) h = 1;
    if (w > GP_SVG_MAX_RASTER || h > GP_SVG_MAX_RASTER) return GP_IMAGE_NULL;
    for (int i = 0; i < g->nrasters; i++) {
        const svg_raster *r = &g->rasters[i];
        if (r->svg.index == svg.index && r->svg.generation == svg.generation && r->w == w && r->rgb == rgb) return r->image;
    }
    uint8_t *px = gp_svg_render(sl->src, sl->len, rgb, w, h, sl->w, g->ts);
    if (!px) return GP_IMAGE_NULL;
    gp_image im = gp_gfx_image_add(g, w, h, px);
    if (!im.generation) return GP_IMAGE_NULL;
    if (g->nrasters == g->cap_rasters) {
        g->cap_rasters = g->cap_rasters ? g->cap_rasters * 2 : 16;
        g->rasters = realloc(g->rasters, (size_t)g->cap_rasters * sizeof *g->rasters);
    }
    g->rasters[g->nrasters++] = (svg_raster){ svg, w, h, rgb, im };
    g->raster_bytes += (size_t)w * (size_t)h * 4;
    if (g->raster_bytes > g->budget_bytes || g->nrasters > g->budget_count) g->overflowed = true;   /* cleared (with the atlases) after this frame */
    return im;
}

int gp_gfx_svg_raster_count(const gp_gfx *g) { return g->nrasters; }

void gp_gfx_svg_set_budget(gp_gfx *g, size_t bytes, int count) {
    g->budget_bytes = bytes;
    g->budget_count = count;
}
