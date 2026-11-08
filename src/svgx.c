/*
 * SVG features nanosvg does not have, done by rewriting the document before it is handed over:
 *
 *   <use> and <symbol>   the referenced element is copied in place of the <use>, inside a <g> that carries the <use>'s attributes (so fill and
 *                        stroke are inherited as they should be) and its x / y as a translation; a <symbol> becomes a <g>, scaled by its viewBox
 *                        into the <use>'s width and height when it has them. Nested and recursive uses are followed to a depth.
 *   clip-path, mask      nanosvg draws every shape in one pass and has no groups to clip, so the document is cut into layers in drawing order: a
 *                        maximal run of shapes that are not clipped or masked is one layer, and each element with a clip-path or mask (with
 *                        everything inside it) is a layer of its own. Each layer is a whole copy of the document with only its shapes in it, drawn
 *                        by nanosvg as usual; the clip path becomes a second document (its shapes filled solid, in the coordinates of the clipped element)
 *                        whose alpha is the mask, a <mask> the same with luminance times alpha. The caller composites: layer * mask, over what is below.
 *
 *   <text>               drawn as the outlines of its glyphs (the text system's own fonts: font-family, font-size, font-weight, font-style, text-anchor, fill and
 *                        the rest of the paint, x and y, and tspans with x, y, dx, dy and their own paint), as paths: so it is cut by a clip and a mask like
 *                        any shape. Text and tspans are laid out one after the other (the order of text mixed with tspans is: its own text first).
 *
 *   filter               an element with `filter="url(#f)"` is a layer of its own like a clipped one; after nanosvg has drawn it the layer goes through the filter's
 *                        primitives (feGaussianBlur, feOffset, feFlood, feComposite, feMerge, feBlend, feColorMatrix, feDropShadow; named results, SourceGraphic, SourceAlpha),
 *                        in linearRGB unless color-interpolation-filters says sRGB, over the whole picture (the filter region is not applied), before the clip and the mask.
 *
 * Not done: feTurbulence, feImage, lighting, feMorphology, feConvolveMatrix, feDisplacementMap, the filter region, patterns, markers, textPath, letter-spacing, rotate, textLength, bidi in text, clipPathUnits / maskUnits objectBoundingBox (such a clip or mask is ignored), clip-path on a shape inside a clipped element
 * (only the outermost counts), the mask's region (x, y, width, height), filters, patterns.
 *
 * The parser is small and forgiving: elements, attributes, text kept as written (CDATA included, comments and the prolog dropped).
 */
#include "internal.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct xn {
    char *name;
    char **k, **v;
    int na, cap_a;
    struct xn **kid;
    int nk, cap_k;
    char *text;            /* the text inside, as written (raw), for <style>, <text> ... */
    struct xn *parent;
    int unit;              /* the layer a shape is in */
} xn;

typedef struct { char *p; size_t n, cap; } sbuf;

static void sb_put(sbuf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2 + 64;
        b->p = realloc(b->p, b->cap);
        if (!b->p) abort();
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void sb_str(sbuf *b, const char *s) { sb_put(b, s, strlen(s)); }

static xn *xn_new(const char *name, size_t n, xn *parent) {
    xn *x = calloc(1, sizeof *x);
    if (!x) abort();
    x->name = malloc(n + 1);
    memcpy(x->name, name, n);
    x->name[n] = 0;
    x->parent = parent;
    x->unit = -1;
    if (parent) {
        if (parent->nk == parent->cap_k) {
            parent->cap_k = parent->cap_k ? parent->cap_k * 2 : 4;
            parent->kid = realloc(parent->kid, (size_t)parent->cap_k * sizeof *parent->kid);
            if (!parent->kid) abort();
        }
        parent->kid[parent->nk++] = x;
    }
    return x;
}

static char *dupn(const char *s, size_t n) {
    char *d = malloc(n + 1);
    if (!d) abort();
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static void x_free(xn *x) {
    if (!x) return;
    for (int i = 0; i < x->na; i++) { free(x->k[i]); free(x->v[i]); }
    free(x->k);
    free(x->v);
    for (int i = 0; i < x->nk; i++) x_free(x->kid[i]);
    free(x->kid);
    free(x->name);
    free(x->text);
    free(x);
}

static const char *x_attr(const xn *x, const char *k) {
    for (int i = 0; i < x->na; i++)
        if (strcmp(x->k[i], k) == 0) return x->v[i];
    return NULL;
}

static void x_set(xn *x, const char *k, const char *v) {
    for (int i = 0; i < x->na; i++)
        if (strcmp(x->k[i], k) == 0) {
            free(x->v[i]);
            x->v[i] = strdup(v);
            return;
        }
    if (x->na == x->cap_a) {
        x->cap_a = x->cap_a ? x->cap_a * 2 : 4;
        x->k = realloc(x->k, (size_t)x->cap_a * sizeof *x->k);
        x->v = realloc(x->v, (size_t)x->cap_a * sizeof *x->v);
        if (!x->k || !x->v) abort();
    }
    x->k[x->na] = strdup(k);
    x->v[x->na] = strdup(v);
    x->na++;
}

static void x_del(xn *x, const char *k) {
    for (int i = 0; i < x->na; i++)
        if (strcmp(x->k[i], k) == 0) {
            free(x->k[i]);
            free(x->v[i]);
            for (int j = i + 1; j < x->na; j++) { x->k[j - 1] = x->k[j]; x->v[j - 1] = x->v[j]; }
            x->na--;
            return;
        }
}

static xn *x_clone(const xn *x, xn *parent) {
    xn *c = xn_new(x->name, strlen(x->name), parent);
    for (int i = 0; i < x->na; i++) x_set(c, x->k[i], x->v[i]);
    if (x->text) c->text = strdup(x->text);
    for (int i = 0; i < x->nk; i++) x_clone(x->kid[i], c);
    return c;
}

/* ---------------------------------------------------------------- parse */

static bool is_name_char(unsigned char c) { return isalnum(c) || c == '_' || c == '-' || c == ':' || c == '.'; }

static xn *x_parse(const char *s, size_t len) {
    xn *root = NULL, *cur = NULL;
    size_t i = 0;
    while (i < len) {
        if (s[i] != '<') {   /* text: kept on the element, raw */
            size_t j = i;
            while (j < len && s[j] != '<') j++;
            if (cur) {
                bool blank = true;
                for (size_t q = i; q < j; q++) if (!isspace((unsigned char)s[q])) blank = false;
                if (!blank) {
                    size_t old = cur->text ? strlen(cur->text) : 0;
                    cur->text = realloc(cur->text, old + (j - i) + 1);
                    memcpy(cur->text + old, s + i, j - i);
                    cur->text[old + (j - i)] = 0;
                }
            }
            i = j;
            continue;
        }
        if (i + 3 < len && strncmp(s + i, "<!--", 4) == 0) {
            const char *e = strstr(s + i + 4, "-->");
            i = e ? (size_t)(e - s) + 3 : len;
            continue;
        }
        if (i + 8 < len && strncmp(s + i, "<![CDATA[", 9) == 0) {
            const char *e = strstr(s + i + 9, "]]>");
            size_t end = e ? (size_t)(e - s) + 3 : len;
            if (cur) {
                size_t old = cur->text ? strlen(cur->text) : 0;
                cur->text = realloc(cur->text, old + (end - i) + 1);
                memcpy(cur->text + old, s + i, end - i);
                cur->text[old + (end - i)] = 0;
            }
            i = end;
            continue;
        }
        if (i + 1 < len && (s[i + 1] == '?' || s[i + 1] == '!')) {   /* the prolog, a doctype (with a bracketed subset) */
            int depth = 0;
            size_t j = i + 1;
            for (; j < len; j++) {
                if (s[j] == '[') depth++;
                else if (s[j] == ']') depth--;
                else if (s[j] == '>' && depth <= 0) break;
            }
            i = j < len ? j + 1 : len;
            continue;
        }
        if (i + 1 < len && s[i + 1] == '/') {   /* a closing tag: up to the matching open element */
            size_t j = i + 2, ns = j;
            while (j < len && is_name_char((unsigned char)s[j])) j++;
            size_t nl = j - ns;
            while (j < len && s[j] != '>') j++;
            i = j < len ? j + 1 : len;
            for (xn *a = cur; a; a = a->parent)
                if (strlen(a->name) == nl && strncmp(a->name, s + ns, nl) == 0) { cur = a->parent; break; }
            continue;
        }
        /* an opening tag */
        size_t j = i + 1, ns = j;
        while (j < len && is_name_char((unsigned char)s[j])) j++;
        if (j == ns) { i++; continue; }
        xn *x = xn_new(s + ns, j - ns, cur);
        if (!root) root = x;
        bool self = false;
        for (;;) {
            while (j < len && isspace((unsigned char)s[j])) j++;
            if (j >= len) break;
            if (s[j] == '>') { j++; break; }
            if (s[j] == '/' && j + 1 < len && s[j + 1] == '>') { self = true; j += 2; break; }
            size_t as = j;
            while (j < len && is_name_char((unsigned char)s[j])) j++;
            if (j == as) { j++; continue; }
            char *key = dupn(s + as, j - as);
            while (j < len && isspace((unsigned char)s[j])) j++;
            char *val = NULL;
            if (j < len && s[j] == '=') {
                j++;
                while (j < len && isspace((unsigned char)s[j])) j++;
                if (j < len && (s[j] == '"' || s[j] == '\'')) {
                    char q = s[j++];
                    size_t vs = j;
                    while (j < len && s[j] != q) j++;
                    val = dupn(s + vs, j - vs);
                    if (j < len) j++;
                } else {
                    size_t vs = j;
                    while (j < len && !isspace((unsigned char)s[j]) && s[j] != '>') j++;
                    val = dupn(s + vs, j - vs);
                }
            } else {
                val = strdup("");
            }
            x_set(x, key, val);
            free(key);
            free(val);
        }
        i = j;
        if (!self) cur = x;
        if (!root) break;
    }
    return root;
}

/* ---------------------------------------------------------------- write */

static void x_emit_attr(sbuf *b, const char *k, const char *v) {
    char q = strchr(v, '"') ? '\'' : '"';
    sb_str(b, " ");
    sb_str(b, k);
    sb_str(b, "=");
    sb_put(b, &q, 1);
    sb_str(b, v);
    sb_put(b, &q, 1);
}

static bool is_shape(const xn *x) {
    static const char *const shapes[] = { "path", "rect", "circle", "ellipse", "line", "polyline", "polygon" };
    for (size_t i = 0; i < sizeof shapes / sizeof *shapes; i++)
        if (strcmp(x->name, shapes[i]) == 0) return true;
    return false;
}

static bool in_unit(const xn *x, int unit) {   /* does this subtree hold a shape of the layer? */
    if (is_shape(x)) return x->unit == unit;
    for (int i = 0; i < x->nk; i++)
        if (in_unit(x->kid[i], unit)) return true;
    return false;
}

/* `unit` < 0: everything. Otherwise only the shapes of that layer (and what holds them), plus what is not drawn (gradients, styles). */
static void x_emit(sbuf *b, const xn *x, int unit) {
    if (unit >= 0 && is_shape(x) && x->unit != unit) return;
    bool holder = strcmp(x->name, "g") == 0 || strcmp(x->name, "svg") == 0 || strcmp(x->name, "a") == 0;
    if (unit >= 0 && x->parent && holder && !in_unit(x, unit)) return;   /* (the root is always written: it carries the size) */
    sb_str(b, "<");
    sb_str(b, x->name);
    for (int i = 0; i < x->na; i++) x_emit_attr(b, x->k[i], x->v[i]);
    if (!x->nk && !x->text) {
        sb_str(b, "/>");
        return;
    }
    sb_str(b, ">");
    if (x->text) sb_str(b, x->text);
    for (int i = 0; i < x->nk; i++) x_emit(b, x->kid[i], unit);
    sb_str(b, "</");
    sb_str(b, x->name);
    sb_str(b, ">");
}

static float num_attr(const xn *x, const char *k, float def) {
    const char *v = x_attr(x, k);
    if (!v || !*v) return def;
    char *end;
    float f = strtof(v, &end);
    return end != v ? f : def;
}

/* ---------------------------------------------------------------- <text> */

/* a property from the style attribute ("fill:red; font-size: 12px"), copied into buf */
static const char *style_prop(const xn *x, const char *prop, char *buf, size_t cap) {
    const char *st = x_attr(x, "style");
    size_t pl = strlen(prop);
    while (st && *st) {
        while (*st == ' ' || *st == ';') st++;
        if (strncmp(st, prop, pl) == 0 && (st[pl] == ':' || st[pl] == ' ')) {
            const char *v = st + pl;
            while (*v == ' ') v++;
            if (*v != ':') { st = strchr(st, ';'); continue; }
            v++;
            while (*v == ' ') v++;
            size_t n = strcspn(v, ";");
            while (n && v[n - 1] == ' ') n--;
            snprintf(buf, cap, "%.*s", (int)(n < cap - 1 ? n : cap - 1), v);
            return buf;
        }
        st = strchr(st, ';');
    }
    return NULL;
}

/* a presentation property of the element or the nearest ancestor that has it (attribute, then style) */
static const char *inherited(const xn *x, const char *prop, char *buf, size_t cap) {
    for (; x; x = x->parent) {
        const char *v = style_prop(x, prop, buf, cap);
        if (v) return v;
        v = x_attr(x, prop);
        if (v) return v;
    }
    return NULL;
}

static float parse_length(const char *v, float def) {
    if (!v || !*v) return def;
    char *end;
    float f = strtof(v, &end);
    if (end == v) return def;
    if (strncmp(end, "pt", 2) == 0) return f * 4.f / 3.f;
    if (strncmp(end, "em", 2) == 0) return f * 16.f;
    if (strncmp(end, "pc", 2) == 0) return f * 16.f;
    return f;
}

/* the text as UTF-8: entities decoded, runs of white space one space, ends trimmed (the default xml:space) */
static char *text_utf8(const char *raw) {
    size_t n = raw ? strlen(raw) : 0;
    char *out = malloc(n * 4 + 8);
    size_t o = 0;
    bool space = true;   /* leading white space is dropped */
    for (size_t i = 0; i < n;) {
        char c = raw[i];
        if (c == '<' && strncmp(raw + i, "<![CDATA[", 9) == 0) { i += 9; continue; }
        if (c == ']' && strncmp(raw + i, "]]>", 3) == 0) { i += 3; continue; }
        if (isspace((unsigned char)c)) {
            if (!space) out[o++] = ' ';
            space = true;
            i++;
            continue;
        }
        space = false;
        if (c == '&') {
            const char *semi = memchr(raw + i, ';', n - i < 12 ? n - i : 12);
            if (semi) {
                size_t el = (size_t)(semi - (raw + i)) - 1;
                const char *e = raw + i + 1;
                uint32_t cp = 0;
                if (el == 3 && strncmp(e, "amp", 3) == 0) cp = '&';
                else if (el == 2 && strncmp(e, "lt", 2) == 0) cp = '<';
                else if (el == 2 && strncmp(e, "gt", 2) == 0) cp = '>';
                else if (el == 4 && strncmp(e, "quot", 4) == 0) cp = '"';
                else if (el == 4 && strncmp(e, "apos", 4) == 0) cp = '\'';
                else if (el >= 2 && e[0] == '#') cp = (uint32_t)(e[1] == 'x' || e[1] == 'X' ? strtoul(e + 2, NULL, 16) : strtoul(e + 1, NULL, 10));
                if (cp) {
                    if (cp < 0x80) out[o++] = (char)cp;
                    else if (cp < 0x800) { out[o++] = (char)(0xc0 | cp >> 6); out[o++] = (char)(0x80 | (cp & 0x3f)); }
                    else if (cp < 0x10000) { out[o++] = (char)(0xe0 | cp >> 12); out[o++] = (char)(0x80 | (cp >> 6 & 0x3f)); out[o++] = (char)(0x80 | (cp & 0x3f)); }
                    else { out[o++] = (char)(0xf0 | cp >> 18); out[o++] = (char)(0x80 | (cp >> 12 & 0x3f)); out[o++] = (char)(0x80 | (cp >> 6 & 0x3f)); out[o++] = (char)(0x80 | (cp & 0x3f)); }
                    i += (size_t)(semi - (raw + i)) + 1;
                    continue;
                }
            }
        }
        out[o++] = c;
        i++;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
    return out;
}

static bool text_only_attr(const char *k) {
    static const char *const names[] = { "x", "y", "dx", "dy", "rotate", "textLength", "lengthAdjust", "font-family", "font-size", "font-weight",
                                         "font-style", "text-anchor", "id", "xml:space", "letter-spacing", "word-spacing", "text-decoration" };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (strcmp(k, names[i]) == 0) return true;
    return false;
}

typedef struct { char *txt; const xn *src; float x, y, dx, dy; bool abs_x, abs_y; char *path; float adv; } TextItem;

static float first_num(const char *v, float def) { return v && *v ? parse_length(v, def) : def; }

/* A <text> becomes a <g> of <path>s (the glyph outlines, placed with a translation); the paint stays on the group. */
static void convert_text(xn *t, gp_text_system *ts) {
    char buf[200], fam[200];
    const char *family = inherited(t, "font-family", fam, sizeof fam);
    float size = parse_length(inherited(t, "font-size", buf, sizeof buf), 16.f);
    const char *w = inherited(t, "font-weight", buf, sizeof buf);
    int weight = 400;
    if (w) weight = strncmp(w, "bold", 4) == 0 ? 700 : strncmp(w, "normal", 6) == 0 ? 400 : atoi(w) ? atoi(w) : 400;
    const char *fs = inherited(t, "font-style", buf, sizeof buf);
    bool italic = fs && (strncmp(fs, "italic", 6) == 0 || strncmp(fs, "oblique", 7) == 0);
    const char *an = inherited(t, "text-anchor", buf, sizeof buf);
    int anchor = an && strncmp(an, "middle", 6) == 0 ? 1 : an && strncmp(an, "end", 3) == 0 ? 2 : 0;
    int fid = 0;
    if (family) {
        char clean[200];
        size_t o = 0;
        for (const char *q = family; *q && o + 1 < sizeof clean; q++)
            if (*q != '\'' && *q != '"') clean[o++] = *q;
        clean[o] = 0;
        fid = gp_text_family(ts, clean);
    }
    int64_t sel = GP_FONT_SEL(fid, weight | (italic ? GP_FONT_ITALIC : 0));

    /* the pieces of text, in order: the text's own, then each tspan */
    TextItem items[64];
    int ni = 0;
    char *own = t->text ? text_utf8(t->text) : NULL;
    float tx = first_num(x_attr(t, "x"), 0), ty = first_num(x_attr(t, "y"), 0);
    if (own && *own) items[ni++] = (TextItem){ own, t, tx, ty, num_attr(t, "dx", 0), num_attr(t, "dy", 0), true, true, NULL, 0 };
    else free(own);
    for (int i = 0; i < t->nk && ni < 64; i++) {
        xn *c = t->kid[i];
        if (strcmp(c->name, "tspan") != 0) continue;
        char *txt = c->text ? text_utf8(c->text) : strdup("");
        bool ax = x_attr(c, "x") != NULL, ay = x_attr(c, "y") != NULL;
        items[ni++] = (TextItem){ txt, c, ax ? first_num(x_attr(c, "x"), 0) : 0, ay ? first_num(x_attr(c, "y"), 0) : 0, num_attr(c, "dx", 0), num_attr(c, "dy", 0), ax, ay, NULL, 0 };
    }

    /* outlines at the origin, and their widths */
    for (int i = 0; i < ni; i++) {
        float tsize = size;
        const char *own_size = x_attr(items[i].src, "font-size");
        if (own_size && items[i].src != t) tsize = parse_length(own_size, size);
        items[i].path = gp_text_outline_path(ts, sel, tsize, items[i].txt, strlen(items[i].txt), 0, 0, &items[i].adv);
    }

    xn *out = xn_new("g", 1, NULL);   /* the paths, until the tspans they were made from are gone */
    float penx = tx, peny = ty;
    for (int i = 0; i < ni;) {
        /* a chunk: this item and the ones after it up to the next with an x of its own; the anchor moves the whole chunk */
        int j = i + 1;
        while (j < ni && !items[j].abs_x) j++;
        float start = items[i].abs_x ? items[i].x : penx;
        if (items[i].abs_y) peny = items[i].y;
        float total = 0;
        for (int k = i; k < j; k++) total += items[k].adv + items[k].dx;
        float x0 = start + items[i].dx - (anchor == 1 ? (total - items[i].dx) / 2 : anchor == 2 ? total - items[i].dx : 0);
        float cx = x0;
        for (int k = i; k < j; k++) {
            if (k > i) cx += items[k].dx;
            if (items[k].abs_y && k > i) peny = items[k].y;
            peny += items[k].dy;
            if (items[k].path) {
                xn *p = xn_new("path", 4, out);
                x_set(p, "d", items[k].path);
                char tf[96];
                snprintf(tf, sizeof tf, "translate(%g %g)", (double)cx, (double)peny);
                x_set(p, "transform", tf);
                if (items[k].src != t)   /* a tspan's own paint */
                    for (int a = 0; a < items[k].src->na; a++)
                        if (!text_only_attr(items[k].src->k[a]) && strcmp(items[k].src->k[a], "transform") != 0) x_set(p, items[k].src->k[a], items[k].src->v[a]);
            }
            cx += items[k].adv;
        }
        penx = cx;
        i = j;
    }
    for (int i = 0; i < ni; i++) { free(items[i].txt); free(items[i].path); }

    /* the group: the text's paint (not its font properties), with the paths for children */
    for (int i = t->na - 1; i >= 0; i--)
        if (text_only_attr(t->k[i])) x_del(t, t->k[i]);
    for (int i = 0; i < t->nk; i++) x_free(t->kid[i]);
    free(t->kid);
    t->kid = out->kid;
    t->nk = out->nk;
    t->cap_k = out->cap_k;
    for (int i = 0; i < t->nk; i++) t->kid[i]->parent = t;
    out->kid = NULL;
    out->nk = 0;
    x_free(out);
    free(t->text);
    t->text = NULL;
    free(t->name);
    t->name = strdup("g");
}

static void convert_texts(xn *x, gp_text_system *ts) {
    if (strcmp(x->name, "defs") == 0 || strcmp(x->name, "symbol") == 0) return;
    if (strcmp(x->name, "text") == 0) { convert_text(x, ts); return; }
    for (int i = 0; i < x->nk; i++) convert_texts(x->kid[i], ts);
}

/* ---------------------------------------------------------------- <use> */

static xn *find_id(xn *x, const char *id) {
    const char *a = x_attr(x, "id");
    if (a && strcmp(a, id) == 0) return x;
    for (int i = 0; i < x->nk; i++) {
        xn *r = find_id(x->kid[i], id);
        if (r) return r;
    }
    return NULL;
}

static const char *href_of(const xn *x) {
    const char *h = x_attr(x, "href");
    if (!h) h = x_attr(x, "xlink:href");
    return h && h[0] == '#' ? h + 1 : NULL;
}


static void expand_uses(xn *root, xn *x, int depth, int *budget);

/* The <g> that stands for a <use>: its attributes, its translation, and a copy of what it points at. */
static xn *use_to_group(xn *root, xn *use, int depth, int *budget) {
    xn *g = xn_new("g", 1, NULL);
    for (int i = 0; i < use->na; i++) {
        const char *k = use->k[i];
        if (strcmp(k, "href") == 0 || strcmp(k, "xlink:href") == 0 || strcmp(k, "x") == 0 || strcmp(k, "y") == 0 || strcmp(k, "width") == 0 ||
            strcmp(k, "height") == 0 || strcmp(k, "id") == 0)
            continue;
        x_set(g, k, use->v[i]);
    }
    float x = num_attr(use, "x", 0), y = num_attr(use, "y", 0);
    const char *id = href_of(use);
    xn *target = id ? find_id(root, id) : NULL;
    if (!target || depth > 10 || *budget <= 0) return g;   /* nothing to draw */
    *budget -= 1;
    char tf[256];
    const char *old = x_attr(use, "transform");
    snprintf(tf, sizeof tf, "%s%stranslate(%g %g)", old ? old : "", old ? " " : "", (double)x, (double)y);
    if (strcmp(target->name, "symbol") == 0) {
        float vx = 0, vy = 0, vw = 0, vh = 0;
        const char *vb = x_attr(target, "viewBox");
        float w = num_attr(use, "width", 0), h = num_attr(use, "height", 0);
        if (vb && sscanf(vb, "%f%*[ ,]%f%*[ ,]%f%*[ ,]%f", &vx, &vy, &vw, &vh) == 4 && vw > 0 && vh > 0) {
            float sx = w > 0 ? w / vw : 1, sy = h > 0 ? h / vh : 1, s = fminf(sx, sy);   /* (xMidYMid meet) */
            float ox = w > 0 ? (w - vw * s) / 2 : 0, oy = h > 0 ? (h - vh * s) / 2 : 0;
            size_t n = strlen(tf);
            snprintf(tf + n, sizeof tf - n, " translate(%g %g) scale(%g) translate(%g %g)", (double)ox, (double)oy, (double)s, (double)-vx, (double)-vy);
        }
        x_set(g, "transform", tf);
        for (int i = 0; i < target->nk; i++) x_clone(target->kid[i], g);
    } else {
        x_set(g, "transform", tf);
        xn *c = x_clone(target, g);
        x_del(c, "id");
    }
    expand_uses(root, g, depth + 1, budget);
    return g;
}

static void expand_uses(xn *root, xn *x, int depth, int *budget) {
    for (int i = 0; i < x->nk; i++) {
        xn *c = x->kid[i];
        if (strcmp(c->name, "use") == 0) {
            xn *g = use_to_group(root, c, depth, budget);
            g->parent = x;
            x->kid[i] = g;
            x_free(c);
            c = g;
        } else {
            expand_uses(root, c, depth, budget);
        }
    }
}

/* ---------------------------------------------------------------- layers */

typedef struct { xn *el; char *clip, *mask, *filter; } Unit;

struct svgx {
    xn *root;
    xn *detached;            /* the <clipPath>s and <mask>s, taken out of the document, as children of one holder */
    Unit *units;
    int n, cap;
};

/* url(#id) of the property, in an attribute or in the style attribute */
static char *url_prop(const xn *x, const char *prop) {
    const char *v = x_attr(x, prop);
    char buf[160];
    if (!v) {
        const char *st = x_attr(x, "style");
        const char *at = st ? strstr(st, prop) : NULL;
        if (!at) return NULL;
        at += strlen(prop);
        while (*at == ' ') at++;
        if (*at != ':') return NULL;
        at++;
        while (*at == ' ') at++;
        size_t n = strcspn(at, ";");
        snprintf(buf, sizeof buf, "%.*s", (int)(n < sizeof buf - 1 ? n : sizeof buf - 1), at);
        v = buf;
    }
    while (*v == ' ') v++;
    if (strncmp(v, "url(", 4) != 0) return NULL;
    v += 4;
    while (*v == ' ' || *v == '\'' || *v == '"') v++;
    if (*v != '#') return NULL;
    v++;
    size_t n = strcspn(v, " )'\"");
    return n ? dupn(v, n) : NULL;
}

static bool is_unrendered(const xn *x) {
    return strcmp(x->name, "defs") == 0 || strcmp(x->name, "clipPath") == 0 || strcmp(x->name, "mask") == 0 || strcmp(x->name, "symbol") == 0 ||
           strcmp(x->name, "linearGradient") == 0 || strcmp(x->name, "radialGradient") == 0 || strcmp(x->name, "pattern") == 0 ||
           strcmp(x->name, "marker") == 0 || strcmp(x->name, "style") == 0;
}

static void svgx_add_unit(struct svgx *d, xn *el, char *clip, char *mask, char *filter) {
    if (d->n == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 8;
        d->units = realloc(d->units, (size_t)d->cap * sizeof *d->units);
        if (!d->units) abort();
    }
    d->units[d->n++] = (Unit){ el, clip, mask, filter };
}

static void mark_all(xn *x, int unit) {
    if (is_shape(x)) x->unit = unit;
    for (int i = 0; i < x->nk; i++) mark_all(x->kid[i], unit);
}

static bool usable_ref(struct svgx *d, const char *id, const char *kind, const char *units_attr) {
    if (!id) return false;
    xn *el = find_id(d->detached, id);
    if (!el || strcmp(el->name, kind) != 0) return false;
    const char *u = x_attr(el, units_attr);
    return !(u && strcmp(u, "objectBoundingBox") == 0 && strcmp(kind, "clipPath") == 0);   /* (the box of the element is not known here: ignored) */
}

/* Walks the document in drawing order, giving every shape its layer. */
static void assign(struct svgx *d, xn *x, int *plain) {
    if (is_unrendered(x)) return;
    char *clip = url_prop(x, "clip-path"), *mask = url_prop(x, "mask"), *filter = url_prop(x, "filter");
    if (clip && !usable_ref(d, clip, "clipPath", "clipPathUnits")) { free(clip); clip = NULL; }
    if (mask && !usable_ref(d, mask, "mask", "maskUnits")) { free(mask); mask = NULL; }
    if (filter && !usable_ref(d, filter, "filter", "filterUnits")) { free(filter); filter = NULL; }
    if (clip || mask || filter) {
        int u = d->n;
        svgx_add_unit(d, x, clip, mask, filter);
        mark_all(x, u);
        *plain = -1;   /* what follows starts a layer of its own */
        return;
    }
    if (is_shape(x)) {
        if (*plain < 0) {
            *plain = d->n;
            svgx_add_unit(d, NULL, NULL, NULL, NULL);
        }
        x->unit = *plain;
        return;
    }
    for (int i = 0; i < x->nk; i++) assign(d, x->kid[i], plain);
}

/* The clipPaths and masks are moved out of the document (they would be drawn as ordinary shapes), under one holder to find them by id. */
static void detach(struct svgx *d, xn *x) {
    for (int i = 0; i < x->nk;) {
        xn *c = x->kid[i];
        if (strcmp(c->name, "clipPath") == 0 || strcmp(c->name, "mask") == 0 || strcmp(c->name, "filter") == 0) {
            for (int j = i + 1; j < x->nk; j++) x->kid[j - 1] = x->kid[j];
            x->nk--;
            if (d->detached->nk == d->detached->cap_k) {
                d->detached->cap_k = d->detached->cap_k ? d->detached->cap_k * 2 : 4;
                d->detached->kid = realloc(d->detached->kid, (size_t)d->detached->cap_k * sizeof *d->detached->kid);
            }
            c->parent = d->detached;
            d->detached->kid[d->detached->nk++] = c;
        } else {
            detach(d, c);
            i++;
        }
    }
}

static bool has_word(const char *s, size_t len, const char *w) {
    size_t n = strlen(w);
    for (size_t i = 0; i + n <= len; i++)
        if (memcmp(s + i, w, n) == 0) return true;
    return false;
}

struct svgx *gp_svgx_parse(const char *src, size_t len, gp_text_system *ts) {
    bool texts = ts && has_word(src, len, "<text");
    if (!texts && !has_word(src, len, "<use") && !has_word(src, len, "clip-path") && !has_word(src, len, "mask") && !has_word(src, len, "<symbol") &&
        !has_word(src, len, "<clipPath") && !has_word(src, len, "filter"))
        return NULL;
    xn *root = x_parse(src, len);
    if (!root || strcmp(root->name, "svg") != 0) { x_free(root); return NULL; }
    struct svgx *d = calloc(1, sizeof *d);
    d->root = root;
    d->detached = xn_new("holder", 6, NULL);
    int budget = 2000;   /* copies of what is pointed at: a document that points at itself, or doubles at every step, stops here */
    expand_uses(root, root, 0, &budget);
    if (texts) convert_texts(root, ts);
    detach(d, root);
    int plain = -1;
    assign(d, root, &plain);
    if (d->n == 0) svgx_add_unit(d, NULL, NULL, NULL, NULL);   /* (nothing to draw: one empty layer) */
    return d;
}

/* ---------------------------------------------------------------- filters */

typedef struct { float *p; int w, h; } FBuf;   /* premultiplied RGBA floats, 0..1 */

static FBuf fb_new(int w, int h) {
    FBuf b = { calloc((size_t)w * (size_t)h * 4, sizeof(float)), w, h };
    if (!b.p) abort();
    return b;
}
static void fb_free(FBuf *b) { free(b->p); b->p = NULL; }
static FBuf fb_copy(const FBuf *s) {
    FBuf b = fb_new(s->w, s->h);
    memcpy(b.p, s->p, (size_t)s->w * (size_t)s->h * 4 * sizeof(float));
    return b;
}

static float to_linear(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static float to_srgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.f / 2.4f) - 0.055f; }

/* straight sRGB bytes <-> premultiplied floats, in linear light if `linear` */
static FBuf fb_from_bytes(const uint8_t *px, int w, int h, bool linear) {
    FBuf b = fb_new(w, h);
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
        float a = (float)px[i * 4 + 3] / 255.f;
        for (int c = 0; c < 3; c++) {
            float v = (float)px[i * 4 + c] / 255.f;
            b.p[i * 4 + c] = (linear ? to_linear(v) : v) * a;
        }
        b.p[i * 4 + 3] = a;
    }
    return b;
}
static uint8_t *fb_to_bytes(const FBuf *b, bool linear) {
    uint8_t *px = malloc((size_t)b->w * (size_t)b->h * 4);
    if (!px) abort();
    for (size_t i = 0; i < (size_t)b->w * (size_t)b->h; i++) {
        float a = fminf(fmaxf(b->p[i * 4 + 3], 0.f), 1.f);
        for (int c = 0; c < 3; c++) {
            float v = a > 1e-6f ? b->p[i * 4 + c] / a : 0.f;
            v = fminf(fmaxf(v, 0.f), 1.f);
            if (linear) v = to_srgb(v);
            px[i * 4 + c] = (uint8_t)lroundf(v * 255.f);
        }
        px[i * 4 + 3] = (uint8_t)lroundf(a * 255.f);
    }
    return px;
}

/* a gaussian, exact (not a box blur), separable; beyond the picture is transparent */
static void fb_blur(FBuf *b, float sx, float sy) {
    for (int axis = 0; axis < 2; axis++) {
        float sd = axis == 0 ? sx : sy;
        if (!(sd > 0.01f)) continue;
        int r = (int)ceilf(sd * 3.f);
        float *k = malloc((size_t)(2 * r + 1) * sizeof *k), sum = 0;
        for (int i = -r; i <= r; i++) { k[i + r] = expf(-(float)(i * i) / (2.f * sd * sd)); sum += k[i + r]; }
        for (int i = 0; i < 2 * r + 1; i++) k[i] /= sum;
        FBuf out = fb_new(b->w, b->h);
        for (int y = 0; y < b->h; y++)
            for (int x = 0; x < b->w; x++) {
                float acc[4] = { 0, 0, 0, 0 };
                for (int i = -r; i <= r; i++) {
                    int xx = axis == 0 ? x + i : x, yy = axis == 0 ? y : y + i;
                    if (xx < 0 || yy < 0 || xx >= b->w || yy >= b->h) continue;
                    const float *s = b->p + ((size_t)yy * (size_t)b->w + (size_t)xx) * 4;
                    for (int c = 0; c < 4; c++) acc[c] += s[c] * k[i + r];
                }
                memcpy(out.p + ((size_t)y * (size_t)b->w + (size_t)x) * 4, acc, sizeof acc);
            }
        free(k);
        fb_free(b);
        *b = out;
    }
}

static void fb_offset(FBuf *b, float dx, float dy) {
    FBuf out = fb_new(b->w, b->h);
    float fx = floorf(dx), fy = floorf(dy), ax = dx - fx, ay = dy - fy;
    for (int y = 0; y < b->h; y++)
        for (int x = 0; x < b->w; x++) {
            float acc[4] = { 0, 0, 0, 0 };
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++) {
                    float wgt = (i ? ax : 1.f - ax) * (j ? ay : 1.f - ay);
                    if (wgt <= 0.f) continue;
                    int sx = x - (int)fx - i, sy = y - (int)fy - j;   /* the pixel that lands here */
                    if (sx < 0 || sy < 0 || sx >= b->w || sy >= b->h) continue;
                    const float *s = b->p + ((size_t)sy * (size_t)b->w + (size_t)sx) * 4;
                    for (int c = 0; c < 4; c++) acc[c] += s[c] * wgt;
                }
            memcpy(out.p + ((size_t)y * (size_t)b->w + (size_t)x) * 4, acc, sizeof acc);
        }
    fb_free(b);
    *b = out;
}

/* a color of the document: #rgb, #rrggbb, rgb(r, g, b) and the usual names; false if it is none of those */
static bool parse_color(const char *v, float rgb[3]) {
    while (*v == ' ') v++;
    static const struct { const char *n; unsigned c; } names[] = {
        { "black", 0x000000 }, { "white", 0xffffff }, { "red", 0xff0000 }, { "green", 0x008000 }, { "lime", 0x00ff00 }, { "blue", 0x0000ff },
        { "yellow", 0xffff00 }, { "cyan", 0x00ffff }, { "aqua", 0x00ffff }, { "magenta", 0xff00ff }, { "fuchsia", 0xff00ff }, { "gray", 0x808080 },
        { "grey", 0x808080 }, { "silver", 0xc0c0c0 }, { "maroon", 0x800000 }, { "navy", 0x000080 }, { "orange", 0xffa500 }, { "purple", 0x800080 },
        { "teal", 0x008080 }, { "olive", 0x808000 } };
    unsigned c = 0;
    bool ok = false;
    if (*v == '#') {
        size_t n = strspn(v + 1, "0123456789abcdefABCDEF");
        if (n == 6) { c = (unsigned)strtoul(v + 1, NULL, 16); ok = true; }
        else if (n == 3) {
            unsigned r = (unsigned)strtoul((char[]){ v[1], 0 }, NULL, 16), g = (unsigned)strtoul((char[]){ v[2], 0 }, NULL, 16), b = (unsigned)strtoul((char[]){ v[3], 0 }, NULL, 16);
            c = (r * 17) << 16 | (g * 17) << 8 | b * 17;
            ok = true;
        }
    } else if (strncmp(v, "rgb(", 4) == 0) {
        float r, g, b;
        if (sscanf(v + 4, "%f%*[ ,]%f%*[ ,]%f", &r, &g, &b) == 3) {
            c = ((unsigned)fminf(fmaxf(r, 0), 255) << 16) | ((unsigned)fminf(fmaxf(g, 0), 255) << 8) | (unsigned)fminf(fmaxf(b, 0), 255);
            ok = true;
        }
    } else {
        for (size_t i = 0; i < sizeof names / sizeof *names; i++)
            if (strncmp(v, names[i].n, strlen(names[i].n)) == 0 && !isalpha((unsigned char)v[strlen(names[i].n)])) { c = names[i].c; ok = true; break; }
    }
    rgb[0] = (float)(c >> 16 & 255) / 255.f;
    rgb[1] = (float)(c >> 8 & 255) / 255.f;
    rgb[2] = (float)(c & 255) / 255.f;
    return ok;
}

static void fb_flood(FBuf *b, const float rgb[3], float alpha, bool linear) {
    for (size_t i = 0; i < (size_t)b->w * (size_t)b->h; i++) {
        for (int c = 0; c < 3; c++) b->p[i * 4 + c] = (linear ? to_linear(rgb[c]) : rgb[c]) * alpha;
        b->p[i * 4 + 3] = alpha;
    }
}

/* a over b (premultiplied) */
static void px_over(float *dst, const float *a, const float *b) {
    float ia = 1.f - a[3];
    for (int c = 0; c < 4; c++) dst[c] = a[c] + b[c] * ia;
}

typedef struct { char name[64]; FBuf buf; } Named;

static const FBuf *named_find(Named *n, int count, const char *name) {
    for (int i = count - 1; i >= 0; i--)
        if (strcmp(n[i].name, name) == 0) return &n[i].buf;
    return NULL;
}

/* One filter on a layer: `px` is the layer as straight RGBA bytes; the result is the same kind of buffer (the caller frees it). scale: pixels per user unit. */
uint8_t *gp_svgx_apply_filter(const struct svgx *d, int unit, const uint8_t *px, int w, int h, float scale) {
    if (unit < 0 || unit >= d->n || !d->units[unit].filter) return NULL;
    xn *f = find_id(d->detached, d->units[unit].filter);
    if (!f) return NULL;
    const char *cif = x_attr(f, "color-interpolation-filters");
    bool linear = !(cif && strncmp(cif, "sRGB", 4) == 0);
    FBuf src = fb_from_bytes(px, w, h, linear);
    FBuf alpha = fb_new(w, h);
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) alpha.p[i * 4 + 3] = src.p[i * 4 + 3];
    FBuf none = fb_new(w, h);
    Named results[64];
    int nres = 0;
    results[nres++] = (Named){ "SourceGraphic", fb_copy(&src) };
    results[nres++] = (Named){ "SourceAlpha", fb_copy(&alpha) };
    results[nres++] = (Named){ "BackgroundImage", fb_copy(&none) };
    results[nres++] = (Named){ "BackgroundAlpha", fb_copy(&none) };
    results[nres++] = (Named){ "FillPaint", fb_copy(&none) };
    results[nres++] = (Named){ "StrokePaint", fb_copy(&none) };
    int last = 0;   /* the result a primitive without `in` takes: the one before it */
    char lastname[64] = "SourceGraphic";
    int made = 0;
    for (int k = 0; k < f->nk && nres < 60; k++) {
        xn *p = f->kid[k];
        const char *nm = p->name;
        const char *in = x_attr(p, "in"), *in2 = x_attr(p, "in2");
        const FBuf *a = named_find(results, nres, in && *in ? in : lastname);
        if (!a) a = named_find(results, nres, "SourceGraphic");
        FBuf out;
        bool known = true;
        const char *pcif = x_attr(p, "color-interpolation-filters");
        (void)pcif;
        if (strcmp(nm, "feGaussianBlur") == 0) {
            float sx = 0, sy = -1;
            const char *sd = x_attr(p, "stdDeviation");
            if (sd) { int n = sscanf(sd, "%f%*[ ,]%f", &sx, &sy); if (n == 1) sy = sx; }
            if (sy < 0) sy = sx;
            out = fb_copy(a);
            fb_blur(&out, sx * scale, sy * scale);
        } else if (strcmp(nm, "feOffset") == 0) {
            out = fb_copy(a);
            fb_offset(&out, num_attr(p, "dx", 0) * scale, num_attr(p, "dy", 0) * scale);
        } else if (strcmp(nm, "feFlood") == 0) {
            float rgb[3] = { 0, 0, 0 };
            const char *fc = x_attr(p, "flood-color");
            if (fc) parse_color(fc, rgb);
            out = fb_new(w, h);
            fb_flood(&out, rgb, num_attr(p, "flood-opacity", 1), linear);
        } else if (strcmp(nm, "feComposite") == 0) {
            const FBuf *b = named_find(results, nres, in2 && *in2 ? in2 : "SourceGraphic");
            if (!b) b = a;
            const char *op = x_attr(p, "operator");
            float k1 = num_attr(p, "k1", 0), k2 = num_attr(p, "k2", 0), k3 = num_attr(p, "k3", 0), k4 = num_attr(p, "k4", 0);
            out = fb_new(w, h);
            for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
                const float *x = a->p + i * 4, *y = b->p + i * 4;
                float *o = out.p + i * 4, ax = x[3], ay = y[3];
                if (!op || strcmp(op, "over") == 0) px_over(o, x, y);
                else if (strcmp(op, "in") == 0) for (int c = 0; c < 4; c++) o[c] = x[c] * ay;
                else if (strcmp(op, "out") == 0) for (int c = 0; c < 4; c++) o[c] = x[c] * (1.f - ay);
                else if (strcmp(op, "atop") == 0) for (int c = 0; c < 4; c++) o[c] = x[c] * ay + y[c] * (1.f - ax);
                else if (strcmp(op, "xor") == 0) for (int c = 0; c < 4; c++) o[c] = x[c] * (1.f - ay) + y[c] * (1.f - ax);
                else if (strcmp(op, "arithmetic") == 0) {
                    for (int c = 0; c < 4; c++) o[c] = fminf(fmaxf(k1 * x[c] * y[c] + k2 * x[c] + k3 * y[c] + k4, 0.f), 1.f);
                    for (int c = 0; c < 3; c++) o[c] = fminf(o[c], o[3]);   /* (premultiplied colors cannot exceed the alpha) */
                } else px_over(o, x, y);
            }
        } else if (strcmp(nm, "feMerge") == 0) {
            out = fb_new(w, h);
            for (int m = 0; m < p->nk; m++) {
                if (strcmp(p->kid[m]->name, "feMergeNode") != 0) continue;
                const char *mi = x_attr(p->kid[m], "in");
                const FBuf *layer = named_find(results, nres, mi && *mi ? mi : lastname);
                if (!layer) continue;
                for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
                    float tmp[4];
                    px_over(tmp, layer->p + i * 4, out.p + i * 4);
                    memcpy(out.p + i * 4, tmp, sizeof tmp);
                }
            }
        } else if (strcmp(nm, "feBlend") == 0) {
            const FBuf *b = named_find(results, nres, in2 && *in2 ? in2 : "SourceGraphic");
            if (!b) b = a;
            const char *mode = x_attr(p, "mode");
            out = fb_new(w, h);
            for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
                const float *x = a->p + i * 4, *y = b->p + i * 4;   /* x on top of y */
                float *o = out.p + i * 4;
                o[3] = 1.f - (1.f - x[3]) * (1.f - y[3]);
                for (int c = 0; c < 3; c++) {
                    float cs = x[c], cb = y[c], as = x[3], ab = y[3];
                    if (mode && strcmp(mode, "multiply") == 0) o[c] = (1.f - as) * cb + (1.f - ab) * cs + cs * cb;
                    else if (mode && strcmp(mode, "screen") == 0) o[c] = cb + cs - cb * cs;
                    else if (mode && strcmp(mode, "darken") == 0) o[c] = fminf((1.f - as) * cb + cs, (1.f - ab) * cs + cb);
                    else if (mode && strcmp(mode, "lighten") == 0) o[c] = fmaxf((1.f - as) * cb + cs, (1.f - ab) * cs + cb);
                    else o[c] = (1.f - as) * cb + cs;
                }
            }
        } else if (strcmp(nm, "feColorMatrix") == 0) {
            const char *type = x_attr(p, "type"), *vals = x_attr(p, "values");
            float m[20] = { 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0 };
            if (type && strcmp(type, "saturate") == 0) {
                float s = vals && *vals ? strtof(vals, NULL) : 1.f;
                float sm[20] = { 0.213f + 0.787f * s, 0.715f - 0.715f * s, 0.072f - 0.072f * s, 0, 0, 0.213f - 0.213f * s, 0.715f + 0.285f * s, 0.072f - 0.072f * s, 0, 0,
                                 0.213f - 0.213f * s, 0.715f - 0.715f * s, 0.072f + 0.928f * s, 0, 0, 0, 0, 0, 1, 0 };
                memcpy(m, sm, sizeof m);
            } else if (type && strcmp(type, "hueRotate") == 0) {
                float t = (vals && *vals ? strtof(vals, NULL) : 0.f) * (float)M_PI / 180.f, cs = cosf(t), sn = sinf(t);
                float hm[20] = { 0.213f + cs * 0.787f - sn * 0.213f, 0.715f - cs * 0.715f - sn * 0.715f, 0.072f - cs * 0.072f + sn * 0.928f, 0, 0,
                                 0.213f - cs * 0.213f + sn * 0.143f, 0.715f + cs * 0.285f + sn * 0.140f, 0.072f - cs * 0.072f - sn * 0.283f, 0, 0,
                                 0.213f - cs * 0.213f - sn * 0.787f, 0.715f - cs * 0.715f + sn * 0.715f, 0.072f + cs * 0.928f + sn * 0.072f, 0, 0, 0, 0, 0, 1, 0 };
                memcpy(m, hm, sizeof m);
            } else if (type && strcmp(type, "luminanceToAlpha") == 0) {
                float lm[20] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.2125f, 0.7154f, 0.0721f, 0, 0 };
                memcpy(m, lm, sizeof m);
            } else if (vals) {   /* matrix */
                const char *q = vals;
                float t[20];
                int n = 0;
                while (n < 20) {
                    char *end;
                    float v = strtof(q, &end);
                    if (end == q) break;
                    t[n++] = v;
                    q = end;
                    while (*q == ' ' || *q == ',') q++;
                }
                if (n == 20) memcpy(m, t, sizeof m);
            }
            out = fb_new(w, h);
            for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
                const float *x = a->p + i * 4;
                float al = x[3], c[4] = { al > 1e-6f ? x[0] / al : 0, al > 1e-6f ? x[1] / al : 0, al > 1e-6f ? x[2] / al : 0, al };
                float r[4];
                for (int row = 0; row < 4; row++) r[row] = fminf(fmaxf(m[row * 5] * c[0] + m[row * 5 + 1] * c[1] + m[row * 5 + 2] * c[2] + m[row * 5 + 3] * c[3] + m[row * 5 + 4], 0.f), 1.f);
                float *o = out.p + i * 4;
                for (int ch = 0; ch < 3; ch++) o[ch] = r[ch] * r[3];
                o[3] = r[3];
            }
        } else if (strcmp(nm, "feDropShadow") == 0) {
            float sx = 0, sy = -1, rgb[3] = { 0, 0, 0 };
            const char *sd = x_attr(p, "stdDeviation");
            if (sd) { int n = sscanf(sd, "%f%*[ ,]%f", &sx, &sy); if (n == 1) sy = sx; } else sx = sy = 2;   /* (the default is 2) */
            if (sy < 0) sy = sx;
            const char *fc = x_attr(p, "flood-color");
            if (fc) parse_color(fc, rgb);
            float fo = num_attr(p, "flood-opacity", 1);
            FBuf shadow = fb_new(w, h);
            for (size_t i = 0; i < (size_t)w * (size_t)h; i++) shadow.p[i * 4 + 3] = a->p[i * 4 + 3];
            fb_blur(&shadow, sx * scale, sy * scale);
            fb_offset(&shadow, num_attr(p, "dx", 2) * scale, num_attr(p, "dy", 2) * scale);
            out = fb_new(w, h);
            for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
                float sa = shadow.p[i * 4 + 3] * fo, sh[4];
                for (int c = 0; c < 3; c++) sh[c] = (linear ? to_linear(rgb[c]) : rgb[c]) * sa;
                sh[3] = sa;
                px_over(out.p + i * 4, a->p + i * 4, sh);
            }
            fb_free(&shadow);
        } else {
            known = false;
            out = fb_copy(a);   /* a primitive that is not supported passes its input on */
        }
        (void)known;
        const char *rn = x_attr(p, "result");
        char name[64];
        if (rn && *rn) snprintf(name, sizeof name, "%.63s", rn);
        else snprintf(name, sizeof name, "\x01%d", made);
        results[nres++] = (Named){ { 0 }, out };
        snprintf(results[nres - 1].name, sizeof results[nres - 1].name, "%s", name);
        snprintf(lastname, sizeof lastname, "%s", name);
        last = nres - 1;
        made++;
    }
    uint8_t *res = made ? fb_to_bytes(&results[last].buf, linear) : NULL;
    for (int i = 0; i < nres; i++) fb_free(&results[i].buf);
    fb_free(&src);
    fb_free(&alpha);
    fb_free(&none);
    return res;
}

/* ---------------------------------------------------------------- end of filters */

void gp_svgx_free(struct svgx *d) {
    if (!d) return;
    for (int i = 0; i < d->n; i++) { free(d->units[i].clip); free(d->units[i].mask); free(d->units[i].filter); }
    free(d->units);
    x_free(d->root);
    x_free(d->detached);
    free(d);
}

int gp_svgx_layers(const struct svgx *d) { return d->n; }

char *gp_svgx_layer_doc(const struct svgx *d, int i) {
    sbuf b = { 0 };
    xn *el = d->units[i].el;
    if (el) {   /* the clipped element is drawn plain: its clip and mask are the other documents' */
        const char *a = x_attr(el, "clip-path"), *m = x_attr(el, "mask"), *fl = x_attr(el, "filter");
        char *ac = a ? strdup(a) : NULL, *am = m ? strdup(m) : NULL, *af = fl ? strdup(fl) : NULL;
        x_del(el, "clip-path");
        x_del(el, "mask");
        x_del(el, "filter");
        x_emit(&b, d->root, i);
        if (ac) { x_set(el, "clip-path", ac); free(ac); }
        if (am) { x_set(el, "mask", am); free(am); }
        if (af) { x_set(el, "filter", af); free(af); }
    } else {
        x_emit(&b, d->root, i);
    }
    return b.p;
}

/* A document in the coordinates of unit i's element: the root's own tag, a <g> for each ancestor's transform and the element's own, then `children`. */
static char *context_doc(const struct svgx *d, int i, const xn *holder, bool clip) {
    sbuf b = { 0 };
    const xn *root = d->root;
    sb_str(&b, "<svg");
    for (int k = 0; k < root->na; k++) x_emit_attr(&b, root->k[k], root->v[k]);
    sb_str(&b, ">");
    for (int k = 0; k < root->nk; k++)   /* gradients, styles and the like, which a mask's shapes may use */
        if (is_unrendered(root->kid[k])) x_emit(&b, root->kid[k], -1);
    const xn *chain[64];
    int depth = 0;
    for (const xn *a = d->units[i].el; a && depth < 64; a = a->parent) chain[depth++] = a;
    int wrappers = 0;
    for (int k = depth - 1; k >= 0; k--) {
        const xn *a = chain[k];
        if (a == root) continue;
        const char *t = x_attr(a, "transform");
        if (!t) continue;
        sb_str(&b, "<g");
        x_emit_attr(&b, "transform", t);
        sb_str(&b, ">");
        wrappers++;
    }
    if (clip) {   /* a clip is a shape of solid black: whatever the shapes were painted with */
        for (int k = 0; k < holder->nk; k++) {
            xn *c = x_clone(holder->kid[k], NULL);
            /* every shape: black, no outline, no opacity; clip-rule is the fill rule */
            xn *stack[256];
            int sp = 0;
            stack[sp++] = c;
            while (sp) {
                xn *x = stack[--sp];
                const char *cr = x_attr(x, "clip-rule");
                if (is_shape(x)) {
                    x_del(x, "style");
                    x_set(x, "fill", "#000000");
                    x_set(x, "fill-opacity", "1");
                    x_set(x, "opacity", "1");
                    x_set(x, "stroke", "none");
                    if (cr) x_set(x, "fill-rule", cr);
                }
                x_del(x, "clip-path");
                x_del(x, "mask");
                for (int q = 0; q < x->nk && sp < 256; q++) stack[sp++] = x->kid[q];
            }
            x_emit(&b, c, -1);
            x_free(c);
        }
    } else {
        for (int k = 0; k < holder->nk; k++) x_emit(&b, holder->kid[k], -1);
    }
    for (int k = 0; k < wrappers; k++) sb_str(&b, "</g>");
    sb_str(&b, "</svg>");
    return b.p;
}

char *gp_svgx_clip_doc(const struct svgx *d, int i) {
    if (!d->units[i].clip) return NULL;
    xn *cp = find_id(d->detached, d->units[i].clip);
    if (!cp) return NULL;
    char *doc = context_doc(d, i, cp, true);
    return doc;
}

char *gp_svgx_mask_doc(const struct svgx *d, int i) {
    if (!d->units[i].mask) return NULL;
    xn *m = find_id(d->detached, d->units[i].mask);
    if (!m) return NULL;
    return context_doc(d, i, m, false);
}
