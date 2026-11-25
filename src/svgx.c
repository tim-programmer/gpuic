/*
 * SVG beyond what nanosvg does, by rewriting the document and putting together layers that nanosvg draws.
 *
 * The document is parsed into a small tree and rewritten:
 *   CSS                  the rules of <style> elements (type, class, id, attribute and universal selectors, compounds of them, descendant and child
 *                        combinators, by specificity and then order) are written into each element's style attribute, ahead of what it had there.
 *   <use>, <symbol>      the referenced element is copied in place of the <use>, inside a <g> that carries the <use>'s attributes and its x / y as a
 *                        translation; a <symbol> becomes a <g> scaled by its viewBox into the <use>'s width and height. Nested and recursive uses stop at a budget.
 *   markers              each vertex of a path, line, polyline or polygon with marker-start / -mid / -end gets a <g> after the shape: the marker's
 *                        content placed, turned (orient) and scaled (markerUnits, viewBox, preserveAspectRatio), cut to its viewport when it overflows.
 *   <text>               the outlines of its glyphs, as paths placed character by character: x, y, dx, dy and rotate lists, letter- and word-spacing,
 *                        textLength, text-anchor per chunk, baseline shifts, tspans with their own font and paint, textPath, vertical writing modes.
 *
 * Then it is cut into layers, a tree of them: an element with a clip-path, mask, filter or group opacity is a layer that holds the layers of what is
 * inside it; a run of plain shapes is a layer nanosvg draws (the whole document written with only those shapes, so transforms and gradients are
 * as they are); a shape painted with a pattern, and an <image>, are layers drawn here. A layer with effects is drawn into a buffer of its own,
 * filtered (svgfx.c), cut by its clip (a document of its own: the clip path's shapes solid black, in the element's coordinates), multiplied by its mask
 * (luminance times alpha, or alpha) and region, and its opacity, and put over what is below, in premultiplied bytes. The documents made for clips,
 * masks and pattern tiles go through all of this again (so a clip can be clipped, a mask can hold text, a pattern can hold a pattern), to a depth.
 *
 * A document that needs none of it goes straight to nanosvg.
 */
#include "svgx.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
# define strncasecmp _strnicmp
#else
# include <strings.h>
#endif

#define MAX_DEPTH    6 /* documents made from documents (a mask in a pattern in a mask ...) */
#define MAX_PAT_TILE (2048 * 2048)

/* ---------------------------------------------------------------- the tree */

typedef struct
{
    char* p;
    size_t n, cap;
} sbuf;

static void sb_put(sbuf* b, const char* s, size_t n)
{
    if(b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2 + 64;
        b->p = realloc(b->p, b->cap);
        if(!b->p) {
            abort();
        }
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void sb_str(sbuf* b, const char* s)
{
    sb_put(b, s, strlen(s));
}
static void sb_fmt(sbuf* b, const char* fmt, ...) GP_PRINTF(2, 3);
static void sb_fmt(sbuf* b, const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if(n > 0) {
        sb_put(b, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
    }
}

static char* dupn(const char* s, size_t n)
{
    char* d = malloc(n + 1);
    if(!d) {
        abort();
    }
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}
static char* dups(const char* s)
{
    return dupn(s, strlen(s));
}

static void x_insert(xn* parent, int at, xn* c)
{
    if(parent->nk == parent->cap_k) {
        parent->cap_k = parent->cap_k ? parent->cap_k * 2 : 4;
        parent->kid = realloc(parent->kid, (size_t)parent->cap_k * sizeof *parent->kid);
        if(!parent->kid) {
            abort();
        }
    }
    for(int j = parent->nk; j > at; j--) {
        parent->kid[j] = parent->kid[j - 1];
    }
    parent->kid[at] = c;
    parent->nk++;
    c->parent = parent;
}
static void x_append(xn* parent, xn* c)
{
    x_insert(parent, parent->nk, c);
}
static void x_remove_at(xn* parent, int i)
{
    for(int j = i + 1; j < parent->nk; j++) {
        parent->kid[j - 1] = parent->kid[j];
    }
    parent->nk--;
}

static xn* xn_new(const char* name, size_t n, xn* parent)
{
    xn* x = calloc(1, sizeof *x);
    if(!x) {
        abort();
    }
    x->name = dupn(name, n);
    x->unit = -1;
    if(parent) {
        x_append(parent, x);
    }
    return x;
}

static void x_free(xn* x)
{
    if(!x) {
        return;
    }
    for(int i = 0; i < x->na; i++) {
        free(x->k[i]);
        free(x->v[i]);
    }
    free(x->k);
    free(x->v);
    for(int i = 0; i < x->nk; i++) {
        x_free(x->kid[i]);
    }
    free(x->kid);
    free(x->name);
    free(x->text);
    free(x);
}

const char* svgx_attr(const xn* x, const char* k)
{
    for(int i = 0; i < x->na; i++) {
        if(strcmp(x->k[i], k) == 0) {
            return x->v[i];
        }
    }
    return NULL;
}
#define x_attr svgx_attr

static void x_set(xn* x, const char* k, const char* v)
{
    for(int i = 0; i < x->na; i++) {
        if(strcmp(x->k[i], k) == 0) {
            free(x->v[i]);
            x->v[i] = dups(v);
            return;
        }
    }
    if(x->na == x->cap_a) {
        x->cap_a = x->cap_a ? x->cap_a * 2 : 4;
        x->k = realloc(x->k, (size_t)x->cap_a * sizeof *x->k);
        x->v = realloc(x->v, (size_t)x->cap_a * sizeof *x->v);
        if(!x->k || !x->v) {
            abort();
        }
    }
    x->k[x->na] = dups(k);
    x->v[x->na] = dups(v);
    x->na++;
}

static void x_del(xn* x, const char* k)
{
    for(int i = 0; i < x->na; i++) {
        if(strcmp(x->k[i], k) == 0) {
            free(x->k[i]);
            free(x->v[i]);
            for(int j = i + 1; j < x->na; j++) {
                x->k[j - 1] = x->k[j];
                x->v[j - 1] = x->v[j];
            }
            x->na--;
            return;
        }
    }
}

static xn* x_clone(const xn* x, xn* parent)
{
    xn* c = xn_new(x->name, strlen(x->name), parent);
    for(int i = 0; i < x->na; i++) {
        x_set(c, x->k[i], x->v[i]);
    }
    if(x->text) {
        c->text = dups(x->text);
    }
    for(int i = 0; i < x->nk; i++) {
        x_clone(x->kid[i], c);
    }
    return c;
}

static bool is(const xn* x, const char* name)
{
    return strcmp(x->name, name) == 0;
}

/* ---------------------------------------------------------------- parse */

static bool is_name_char(unsigned char c)
{
    return isalnum(c) || c == '_' || c == '-' || c == ':' || c == '.';
}

static bool in_text(const xn* x)
{
    for(; x; x = x->parent) {
        if(is(x, "text")) {
            return true;
        } else if(!is(x, "tspan") && !is(x, "textPath") && !is(x, "a")) {
            return false;
        }
    }
    return false;
}

/* text inside an element: kept whole on it (for <style>), and as a #text child inside <text> (where the order with tspans matters) */
static void add_text(xn* cur, const char* s, size_t n)
{
    if(!cur) {
        return;
    }
    size_t old = cur->text ? strlen(cur->text) : 0;
    cur->text = realloc(cur->text, old + n + 1);
    if(!cur->text) {
        abort();
    }
    memcpy(cur->text + old, s, n);
    cur->text[old + n] = 0;
    if(in_text(cur)) {
        xn* t = xn_new("#text", 5, cur);
        t->text = dupn(s, n);
    }
}

static xn* x_parse(const char* s, size_t len)
{
    xn *root = NULL, *cur = NULL;
    size_t i = 0;
    while(i < len) {
        if(s[i] != '<') {
            size_t j = i;
            while(j < len && s[j] != '<') {
                j++;
            }
            bool blank = true;
            for(size_t q = i; q < j; q++) {
                if(!isspace((unsigned char)s[q])) {
                    blank = false;
                }
            }
            if(!blank || (cur && in_text(cur))) {
                add_text(cur, s + i, j - i);
            }
            i = j;
            continue;
        }
        if(i + 3 < len && strncmp(s + i, "<!--", 4) == 0) {
            const char* e = strstr(s + i + 4, "-->");
            i = e ? (size_t)(e - s) + 3 : len;
            continue;
        }
        if(i + 8 < len && strncmp(s + i, "<![CDATA[", 9) == 0) {
            const char* e = strstr(s + i + 9, "]]>");
            size_t end = e ? (size_t)(e - s) + 3 : len;
            add_text(cur, s + i, end - i);
            i = end;
            continue;
        }
        if(i + 1 < len && (s[i + 1] == '?' || s[i + 1] == '!')) { /* the prolog, a doctype (with a bracketed subset) */
            int depth = 0;
            size_t j = i + 1;
            for(; j < len; j++) {
                if(s[j] == '[') {
                    depth++;
                } else if(s[j] == ']') {
                    depth--;
                } else if(s[j] == '>' && depth <= 0) {
                    break;
                }
            }
            i = j < len ? j + 1 : len;
            continue;
        }
        if(i + 1 < len && s[i + 1] == '/') { /* a closing tag: up to the matching open element */
            size_t j = i + 2, ns = j;
            while(j < len && is_name_char((unsigned char)s[j])) {
                j++;
            }
            size_t nl = j - ns;
            while(j < len && s[j] != '>') {
                j++;
            }
            i = j < len ? j + 1 : len;
            for(xn* a = cur; a; a = a->parent) {
                if(strlen(a->name) == nl && strncmp(a->name, s + ns, nl) == 0) {
                    cur = a->parent;
                    break;
                }
            }
            if(!cur && root) {
                break; /* (the root is closed: what follows is not the drawing) */
            }
            continue;
        }
        size_t j = i + 1, ns = j;
        while(j < len && is_name_char((unsigned char)s[j])) {
            j++;
        }
        if(j == ns) {
            i++;
            continue;
        }
        xn* x = xn_new(s + ns, j - ns, cur);
        if(!root) {
            root = x;
        }
        bool self = false;
        for(;;) {
            while(j < len && isspace((unsigned char)s[j])) {
                j++;
            }
            if(j >= len) {
                break;
            }
            if(s[j] == '>') {
                j++;
                break;
            }
            if(s[j] == '/' && j + 1 < len && s[j + 1] == '>') {
                self = true;
                j += 2;
                break;
            }
            size_t as = j;
            while(j < len && is_name_char((unsigned char)s[j])) {
                j++;
            }
            if(j == as) {
                j++;
                continue;
            }
            char* key = dupn(s + as, j - as);
            while(j < len && isspace((unsigned char)s[j])) {
                j++;
            }
            char* val = NULL;
            if(j < len && s[j] == '=') {
                j++;
                while(j < len && isspace((unsigned char)s[j])) {
                    j++;
                }
                if(j < len && (s[j] == '"' || s[j] == '\'')) {
                    char q = s[j++];
                    size_t vs = j;
                    while(j < len && s[j] != q) {
                        j++;
                    }
                    val = dupn(s + vs, j - vs);
                    if(j < len) {
                        j++;
                    }
                } else {
                    size_t vs = j;
                    while(j < len && !isspace((unsigned char)s[j]) && s[j] != '>') {
                        j++;
                    }
                    val = dupn(s + vs, j - vs);
                }
            } else {
                val = dups("");
            }
            x_set(x, key, val);
            free(key);
            free(val);
        }
        i = j;
        if(!self) {
            cur = x;
        }
        if(x == root && self) {
            break;
        }
    }
    return root;
}

/* ---------------------------------------------------------------- write */

static void x_emit_attr(sbuf* b, const char* k, const char* v)
{
    char q = strchr(v, '"') ? '\'' : '"';
    sb_str(b, " ");
    sb_str(b, k);
    sb_str(b, "=");
    sb_put(b, &q, 1);
    sb_str(b, v);
    sb_put(b, &q, 1);
}

static bool is_shape(const xn* x)
{
    static const char* const shapes[] = { "path", "rect", "circle", "ellipse", "line", "polyline", "polygon" };
    for(size_t i = 0; i < sizeof shapes / sizeof *shapes; i++) {
        if(strcmp(x->name, shapes[i]) == 0) {
            return true;
        }
    }
    return false;
}
static bool is_holder(const xn* x)
{
    return is(x, "g") || is(x, "svg") || is(x, "a") || is(x, "switch");
}

static bool in_unit(const xn* x, int unit)
{ /* does this subtree hold a shape of the layer? */
    if(is_shape(x)) {
        return x->unit == unit;
    }
    for(int i = 0; i < x->nk; i++) {
        if(in_unit(x->kid[i], unit)) {
            return true;
        }
    }
    return false;
}

typedef struct
{
    int unit;              /* -1: everything; else only the shapes of that layer (and what holds them), plus what is not drawn */
    const xn* ovr;         /* an element whose paint is overridden ... */
    const char* ovr_style; /* ... by these declarations, after its own */
    const xn* no_tf;       /* an element written without its transform */
    bool probe;            /* for boxes: an <image> as a rect of its place */
} emit_opt;

static void x_emit(sbuf* b, const xn* x, const emit_opt* o)
{
    if(x->name[0] == '#') {
        return;
    }
    if(o->unit >= 0 && is_shape(x) && x->unit != o->unit) {
        return;
    }
    if(o->unit >= 0 && x->parent && is_holder(x) && !in_unit(x, o->unit)) {
        return; /* (the root is always written: it carries the size) */
    }
    bool img = o->probe && is(x, "image");
    sb_str(b, "<");
    sb_str(b, img ? "rect" : x->name);
    const char* style = NULL;
    for(int i = 0; i < x->na; i++) {
        if(x == o->no_tf && strcmp(x->k[i], "transform") == 0) {
            continue;
        }
        if(x->k[i][0] == '\x01') {
            continue; /* (notes of ours) */
        }
        if(x == o->ovr && strcmp(x->k[i], "style") == 0) {
            style = x->v[i];
            continue;
        }
        x_emit_attr(b, x->k[i], x->v[i]);
    }
    if(x == o->ovr) {
        sbuf st = { 0 };
        if(style) {
            sb_str(&st, style);
            sb_str(&st, ";");
        }
        sb_str(&st, o->ovr_style);
        x_emit_attr(b, "style", st.p);
        free(st.p);
    }
    if(img) {
        sb_str(b, "/>");
        return;
    }
    bool kids = false;
    for(int i = 0; i < x->nk; i++) {
        if(x->kid[i]->name[0] != '#') {
            kids = true;
        }
    }
    if(!kids && !x->text) {
        sb_str(b, "/>");
        return;
    }
    sb_str(b, ">");
    if(x->text && (is(x, "style") || is(x, "text") || is(x, "tspan") || is(x, "textPath"))) {
        sb_str(b, x->text);
    }
    for(int i = 0; i < x->nk; i++) {
        x_emit(b, x->kid[i], o);
    }
    sb_str(b, "</");
    sb_str(b, x->name);
    sb_str(b, ">");
}

float svgx_num(const xn* x, const char* k, float def)
{
    const char* v = x_attr(x, k);
    if(!v || !*v) {
        return def;
    }
    char* end;
    float f = strtof(v, &end);
    return end != v ? f : def;
}
#define num_attr svgx_num

/* ---------------------------------------------------------------- properties */

/* a declaration of a style string ("fill:red; font-size: 12px"), copied into buf */
static const char* decl_of(const char* st, const char* prop, char* buf, size_t cap)
{
    size_t pl = strlen(prop);
    const char* found = NULL;
    size_t fn = 0;
    while(st && *st) {
        while(*st == ' ' || *st == ';' || *st == '\n' || *st == '\t' || *st == '\r') {
            st++;
        }
        const char *colon = strchr(st, ':'), *semi = strchr(st, ';');
        if(!colon || (semi && semi < colon)) {
            st = semi;
            continue;
        }
        const char* ke = colon;
        while(ke > st && isspace((unsigned char)ke[-1])) {
            ke--;
        }
        const char* v = colon + 1;
        /* the value ends at a ';' that is not inside parentheses (url(data:...;base64,...)) */
        int par = 0;
        const char* e = v;
        while(*e && !(*e == ';' && par == 0)) {
            if(*e == '(') {
                par++;
            } else if(*e == ')') {
                par--;
            }
            e++;
        }
        if((size_t)(ke - st) == pl && strncmp(st, prop, pl) == 0) { /* the last one counts */
            while(v < e && isspace((unsigned char)*v)) {
                v++;
            }
            found = v;
            fn = (size_t)(e - v);
            while(fn && isspace((unsigned char)found[fn - 1])) {
                fn--;
            }
        }
        st = *e ? e + 1 : e;
    }
    if(!found) {
        return NULL;
    }
    if(fn >= 10 && strncmp(found + fn - 10, "!important", 10) == 0) {
        fn -= 10;
        while(fn && found[fn - 1] == ' ') {
            fn--;
        }
    }
    snprintf(buf, cap, "%.*s", (int)(fn < cap - 1 ? fn : cap - 1), found);
    return buf;
}

const char* svgx_prop(const xn* x, const char* prop, char* buf, size_t cap)
{
    const char* v = decl_of(x_attr(x, "style"), prop, buf, cap);
    if(v) {
        return v;
    }
    v = x_attr(x, prop);
    if(v) {
        snprintf(buf, cap, "%s", v);
        return buf;
    }
    return NULL;
}

/* a presentation property of the element or the nearest ancestor that has it ("inherit" goes on up) */
static const char* inherited(const xn* x, const char* prop, char* buf, size_t cap)
{
    for(; x; x = x->parent) {
        const char* v = svgx_prop(x, prop, buf, cap);
        if(v && strcmp(v, "inherit") != 0) {
            return v;
        }
    }
    return NULL;
}

/* the style attribute without one property */
static void style_drop(xn* x, const char* prop)
{
    const char* st = x_attr(x, "style");
    if(!st) {
        return;
    }
    sbuf out = { 0 };
    size_t pl = strlen(prop);
    const char* s = st;
    while(*s) {
        const char* e = s;
        int par = 0;
        while(*e && !(*e == ';' && par == 0)) {
            if(*e == '(') {
                par++;
            } else if(*e == ')') {
                par--;
            }
            e++;
        }
        const char* k = s;
        while(k < e && isspace((unsigned char)*k)) {
            k++;
        }
        bool drop = (size_t)(e - k) > pl && strncmp(k, prop, pl) == 0 && (k[pl] == ':' || isspace((unsigned char)k[pl]));
        if(drop) { /* (and only if what follows the name is the colon) */
            const char* c = k + pl;
            while(c < e && isspace((unsigned char)*c)) {
                c++;
            }
            drop = c < e && *c == ':';
        }
        if(!drop && e > s) {
            sb_put(&out, s, (size_t)(e - s));
            sb_str(&out, ";");
        }
        s = *e ? e + 1 : e;
    }
    if(out.p) {
        x_set(x, "style", out.p);
    } else {
        x_del(x, "style");
    }
    free(out.p);
}

/* takes a property off the element (attribute and style): its value, malloc'd, or NULL */
static char* take_prop(xn* x, const char* prop)
{
    char buf[1024];
    const char* v = svgx_prop(x, prop, buf, sizeof buf);
    char* r = v ? dups(v) : NULL;
    x_del(x, prop);
    style_drop(x, prop);
    return r;
}

/* url(#id) in a value: the id, malloc'd */
static char* url_id(const char* v)
{
    if(!v) {
        return NULL;
    }
    while(*v == ' ') {
        v++;
    }
    if(strncmp(v, "url(", 4) != 0) {
        return NULL;
    }
    v += 4;
    while(*v == ' ' || *v == '\'' || *v == '"') {
        v++;
    }
    if(*v != '#') {
        return NULL;
    }
    v++;
    size_t n = strcspn(v, " )'\"");
    return n ? dupn(v, n) : NULL;
}

float svgx_length(const char* v, float def, float percent_of)
{
    if(!v) {
        return def;
    }
    while(*v == ' ') {
        v++;
    }
    if(!*v) {
        return def;
    }
    char* end;
    float f = strtof(v, &end);
    if(end == v) {
        return def;
    }
    while(*end == ' ') {
        end++;
    }
    if(*end == '%') {
        return f * percent_of / 100.f;
    }
    if(strncmp(end, "pt", 2) == 0) {
        return f * 4.f / 3.f;
    }
    if(strncmp(end, "pc", 2) == 0) {
        return f * 16.f;
    }
    if(strncmp(end, "mm", 2) == 0) {
        return f * 96.f / 25.4f;
    }
    if(strncmp(end, "cm", 2) == 0) {
        return f * 96.f / 2.54f;
    }
    if(strncmp(end, "in", 2) == 0) {
        return f * 96.f;
    }
    if(strncmp(end, "em", 2) == 0) {
        return f * 16.f;
    }
    if(strncmp(end, "ex", 2) == 0) {
        return f * 8.f;
    }
    return f;
}

static float hue2rgb(float p, float q, float t)
{
    if(t < 0) {
        t += 1;
    }
    if(t > 1) {
        t -= 1;
    }
    if(t < 1.f / 6) {
        return p + (q - p) * 6 * t;
    }
    if(t < 0.5f) {
        return q;
    }
    if(t < 2.f / 3) {
        return p + (q - p) * (2.f / 3 - t) * 6;
    }
    return p;
}

bool svgx_color(const char* v, float rgb[3], float* alpha)
{
    if(alpha) {
        *alpha = 1.f;
    }
    rgb[0] = rgb[1] = rgb[2] = 0;
    if(!v) {
        return false;
    }
    while(*v == ' ') {
        v++;
    }
    if(*v == '#') {
        size_t n = strspn(v + 1, "0123456789abcdefABCDEF");
        char h[9] = { 0 };
        if(n == 3 || n == 4) {
            for(size_t i = 0; i < n; i++) {
                h[i * 2] = v[1 + i];
                h[i * 2 + 1] = v[1 + i];
            }
            n *= 2;
        } else if(n == 6 || n == 8) {
            memcpy(h, v + 1, n);
        } else {
            return false;
        }
        unsigned long c = strtoul(h, NULL, 16);
        if(n == 8) {
            if(alpha) {
                *alpha = (float)(c & 255) / 255.f;
            }
            c >>= 8;
        }
        rgb[0] = (float)(c >> 16 & 255) / 255.f;
        rgb[1] = (float)(c >> 8 & 255) / 255.f;
        rgb[2] = (float)(c & 255) / 255.f;
        return true;
    }
    bool hsl = strncmp(v, "hsl", 3) == 0;
    if(strncmp(v, "rgb", 3) == 0 || hsl) {
        const char* p = strchr(v, '(');
        if(!p) {
            return false;
        }
        p++;
        float c[4] = { 0, 0, 0, 1 };
        bool pct[4] = { false, false, false, false };
        int n = 0;
        while(n < 4) {
            while(*p == ' ' || *p == ',' || *p == '/') {
                p++;
            }
            char* end;
            float f = strtof(p, &end);
            if(end == p) {
                break;
            }
            p = end;
            if(*p == '%') {
                pct[n] = true;
                p++;
            } else if(strncmp(p, "deg", 3) == 0) {
                p += 3;
            }
            c[n++] = f;
        }
        if(n < 3) {
            return false;
        }
        if(hsl) {
            float hh = fmodf(c[0], 360.f) / 360.f, s = c[1] / 100.f, l = c[2] / 100.f;
            if(hh < 0) {
                hh += 1;
            }
            float q = l < 0.5f ? l * (1 + s) : l + s - l * s, pp = 2 * l - q;
            rgb[0] = hue2rgb(pp, q, hh + 1.f / 3);
            rgb[1] = hue2rgb(pp, q, hh);
            rgb[2] = hue2rgb(pp, q, hh - 1.f / 3);
        } else {
            for(int i = 0; i < 3; i++) {
                rgb[i] = fminf(fmaxf(pct[i] ? c[i] / 100.f : c[i] / 255.f, 0.f), 1.f);
            }
        }
        if(alpha && n == 4) {
            *alpha = fminf(fmaxf(pct[3] ? c[3] / 100.f : c[3], 0.f), 1.f);
        }
        return true;
    }
    size_t n = 0;
    while(isalpha((unsigned char)v[n])) {
        n++;
    }
    if(n == 11 && strncmp(v, "transparent", 11) == 0) {
        if(alpha) {
            *alpha = 0;
        }
        return true;
    }
    uint32_t c;
    if(n && gp_svg_named_color(v, n, &c)) {
        rgb[0] = (float)(c >> 16 & 255) / 255.f;
        rgb[1] = (float)(c >> 8 & 255) / 255.f;
        rgb[2] = (float)(c & 255) / 255.f;
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------- matrices */

svgx_m2 svgx_mul(svgx_m2 p, svgx_m2 q)
{
    return (svgx_m2){ p.a * q.a + p.c * q.b, p.b * q.a + p.d * q.b, p.a * q.c + p.c * q.d, p.b * q.c + p.d * q.d, p.a * q.e + p.c * q.f + p.e, p.b * q.e + p.d * q.f + p.f };
}
svgx_m2 svgx_inverse(svgx_m2 m)
{
    double det = m.a * m.d - m.b * m.c;
    if(fabs(det) < 1e-12) {
        return (svgx_m2){ 0, 0, 0, 0, 0, 0 };
    }
    double ia = m.d / det, ib = -m.b / det, ic = -m.c / det, id = m.a / det;
    return (svgx_m2){ ia, ib, ic, id, -(ia * m.e + ic * m.f), -(ib * m.e + id * m.f) };
}
static void m_apply(svgx_m2 m, double x, double y, double* ox, double* oy)
{
    *ox = m.a * x + m.c * y + m.e;
    *oy = m.b * x + m.d * y + m.f;
}
static const svgx_m2 M_ID = { 1, 0, 0, 1, 0, 0 };

static const char PROBE[] = "<path d='M0 0L1 0L0 1Z' fill='#000'/>";
static const char ROOT1[] = "<svg xmlns='http://www.w3.org/2000/svg' width='1' height='1' viewBox='0 0 1 1'>";

/* what the probe path became */
static svgx_m2 probe_matrix(const char* doc)
{
    float f[6];
    if(gp_svg_nsvg_info(doc, false, NULL, NULL, f) < 1) {
        return M_ID;
    }
    return (svgx_m2){ f[2] - f[0], f[3] - f[1], f[4] - f[0], f[5] - f[1], f[0], f[1] };
}

svgx_m2 svgx_transform(const char* list)
{
    if(!list || !*list) {
        return M_ID;
    }
    sbuf b = { 0 };
    sb_str(&b, ROOT1);
    sb_str(&b, "<g");
    x_emit_attr(&b, "transform", list);
    sb_str(&b, ">");
    sb_str(&b, PROBE);
    sb_str(&b, "</g></svg>");
    svgx_m2 m = probe_matrix(b.p);
    free(b.p);
    return m;
}

void svgx_fit(const char* par, float w, float h, float vx, float vy, float vw, float vh, double* sx, double* sy, double* tx, double* ty, bool* slice)
{
    *slice = false;
    if(!(w > 0) || !(h > 0)) {
        *sx = *sy = 1;
        *tx = vx;
        *ty = vy;
        return;
    }
    int ax = 1, ay = 1; /* 0 min, 1 mid, 2 max */
    bool none = false, sl = false;
    if(par) {
        while(*par == ' ') {
            par++;
        }
        if(strncmp(par, "defer", 5) == 0) {
            par += 5;
            while(*par == ' ') {
                par++;
            }
        }
        if(strncmp(par, "none", 4) == 0) {
            none = true;
        } else if(strlen(par) >= 8) {
            ax = strncmp(par + 1, "Min", 3) == 0 ? 0 : strncmp(par + 1, "Max", 3) == 0 ? 2
                                                                                       : 1;
            ay = strncmp(par + 5, "Min", 3) == 0 ? 0 : strncmp(par + 5, "Max", 3) == 0 ? 2
                                                                                       : 1;
        }
        sl = strstr(par, "slice") != NULL;
    }
    if(none) {
        *sx = vw / w;
        *sy = vh / h;
        *tx = vx;
        *ty = vy;
        return;
    }
    double s = sl ? fmax(vw / w, vh / h) : fmin(vw / w, vh / h);
    *sx = *sy = s;
    *tx = vx + (vw - w * s) * ax / 2.0;
    *ty = vy + (vh - h * s) * ay / 2.0;
    *slice = sl && (fabs(w * s - vw) > 1e-3 || fabs(h * s - vh) > 1e-3);
}

/* ---------------------------------------------------------------- CSS */

typedef struct
{
    char* sel;
    int spec, order;
    char* decl;
} rule;
typedef struct
{
    rule* r;
    int n, cap;
} rules;

static int sel_spec(const char* s)
{
    int a = 0, b = 0, c = 0;
    bool start = true;
    for(; *s; s++) {
        if(*s == '#') {
            a++;
            start = false;
        } else if(*s == '.' || *s == '[' || *s == ':') {
            b++;
            start = false;
            if(*s == '[') {
                while(*s && *s != ']') {
                    s++;
                }
                if(!*s) {
                    break;
                }
            }
        } else if(*s == ' ' || *s == '>' || *s == '+' || *s == '~') {
            start = true;
        } else if(start && (isalpha((unsigned char)*s) || *s == '_')) {
            c++;
            start = false;
        } else if(*s == '*') {
            start = false;
        }
    }
    return a * 10000 + b * 100 + c;
}

static void css_parse(rules* rs, const char* css, int* order)
{
    /* comments out first */
    size_t n = strlen(css);
    char* s = malloc(n + 1);
    if(!s) {
        abort();
    }
    size_t o = 0;
    for(size_t i = 0; i < n;) {
        if(css[i] == '/' && css[i + 1] == '*') {
            const char* e = strstr(css + i + 2, "*/");
            i = e ? (size_t)(e - css) + 2 : n;
            continue;
        }
        if(strncmp(css + i, "<![CDATA[", 9) == 0) {
            i += 9;
            continue;
        }
        if(strncmp(css + i, "]]>", 3) == 0) {
            i += 3;
            continue;
        }
        s[o++] = css[i++];
    }
    s[o] = 0;
    char* p = s;
    while(*p) {
        while(isspace((unsigned char)*p)) {
            p++;
        }
        if(!*p) {
            break;
        }
        if(*p == '@') { /* an at-rule: to its ';' or past its block */
            char* q = p;
            while(*q && *q != ';' && *q != '{') {
                q++;
            }
            if(*q == '{') {
                int depth = 0;
                for(; *q; q++) {
                    if(*q == '{') {
                        depth++;
                    } else if(*q == '}' && --depth == 0) {
                        q++;
                        break;
                    }
                }
            } else if(*q) {
                q++;
            }
            p = q;
            continue;
        }
        char* open = strchr(p, '{');
        if(!open) {
            break;
        }
        char* close = strchr(open, '}');
        if(!close) {
            break;
        }
        char* decl = dupn(open + 1, (size_t)(close - open - 1));
        /* each selector of the list */
        char* q = p;
        while(q < open) {
            char* e = q;
            while(e < open && *e != ',') {
                e++;
            }
            char *a = q, *z = e;
            while(a < z && isspace((unsigned char)*a)) {
                a++;
            }
            while(z > a && isspace((unsigned char)z[-1])) {
                z--;
            }
            if(z > a) {
                if(rs->n == rs->cap) {
                    rs->cap = rs->cap ? rs->cap * 2 : 16;
                    rs->r = realloc(rs->r, (size_t)rs->cap * sizeof *rs->r);
                    if(!rs->r) {
                        abort();
                    }
                }
                char* sel = dupn(a, (size_t)(z - a));
                rs->r[rs->n++] = (rule){ sel, sel_spec(sel), (*order)++, dups(decl) };
            }
            q = e + 1;
        }
        free(decl);
        p = close + 1;
    }
    free(s);
}

static bool has_class(const xn* x, const char* c, size_t n)
{
    const char* cl = x_attr(x, "class");
    while(cl && *cl) {
        while(isspace((unsigned char)*cl)) {
            cl++;
        }
        size_t m = 0;
        while(cl[m] && !isspace((unsigned char)cl[m])) {
            m++;
        }
        if(m == n && strncmp(cl, c, n) == 0) {
            return true;
        }
        cl += m;
    }
    return false;
}

static size_t ident(const char* s, size_t n)
{
    size_t i = 0;
    while(i < n && (isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == '_' || (unsigned char)s[i] >= 0x80)) {
        i++;
    }
    return i;
}

static bool match_compound(const xn* x, const char* s, size_t n)
{
    size_t i = 0;
    if(i < n && s[i] == '*') {
        i++;
    } else {
        size_t m = ident(s, n);
        if(m) {
            const char* bar = memchr(s, '|', m); /* (a namespace prefix) */
            const char* nm = bar ? bar + 1 : s;
            size_t nl = m - (size_t)(nm - s);
            if(strlen(x->name) != nl || strncmp(x->name, nm, nl) != 0) {
                return false;
            }
            i = m;
        }
    }
    while(i < n) {
        char c = s[i++];
        if(c == '.' || c == '#') {
            size_t m = ident(s + i, n - i);
            if(!m) {
                return false;
            }
            if(c == '.' && !has_class(x, s + i, m)) {
                return false;
            }
            if(c == '#') {
                const char* id = x_attr(x, "id");
                if(!id || strlen(id) != m || strncmp(id, s + i, m) != 0) {
                    return false;
                }
            }
            i += m;
        } else if(c == '[') {
            size_t e = i;
            while(e < n && s[e] != ']') {
                e++;
            }
            size_t m = ident(s + i, e - i);
            char an[64];
            snprintf(an, sizeof an, "%.*s", (int)m, s + i);
            const char* av = x_attr(x, an);
            if(!av) {
                return false;
            }
            size_t k = i + m;
            if(k < e && s[k] == '=') {
                k++;
                char q = s[k] == '"' || s[k] == '\'' ? s[k++] : 0;
                size_t ve = k;
                while(ve < e && s[ve] != q && (q || s[ve] != ']')) {
                    ve++;
                }
                if(strlen(av) != ve - k || strncmp(av, s + k, ve - k) != 0) {
                    return false;
                }
            }
            i = e + 1;
        } else if(c == ':') {
            size_t m = ident(s + i, n - i);
            if(m == 11 && strncmp(s + i, "first-child", 11) == 0) {
                if(!x->parent) {
                    return false;
                }
                for(int k = 0; k < x->parent->nk; k++) {
                    if(x->parent->kid[k]->name[0] != '#') {
                        if(x->parent->kid[k] != x) {
                            return false;
                        }
                        break;
                    }
                }
                i += m;
            } else {
                return false; /* (:hover and the rest: never, in a picture) */
            }
        } else {
            return false;
        }
    }
    return true;
}

typedef struct
{
    const char* s;
    size_t n;
    char comb;
} compound;

static bool match_from(const xn* x, const compound* c, int k)
{
    if(!match_compound(x, c[k].s, c[k].n)) {
        return false;
    }
    if(k == 0) {
        return true;
    }
    if(c[k].comb == '>') {
        return x->parent && match_from(x->parent, c, k - 1);
    }
    if(c[k].comb == ' ') {
        for(const xn* a = x->parent; a; a = a->parent) {
            if(match_from(a, c, k - 1)) {
                return true;
            }
        }
        return false;
    }
    return false; /* + and ~ are not supported */
}

static bool sel_match(const xn* x, const char* sel)
{
    compound c[16];
    int n = 0;
    const char* p = sel;
    char comb = ' ';
    while(*p && n < 16) {
        while(*p == ' ') {
            p++;
        }
        if(*p == '>' || *p == '+' || *p == '~') {
            comb = *p++;
            continue;
        }
        if(!*p) {
            break;
        }
        const char* s = p;
        while(*p && *p != ' ' && *p != '>' && *p != '+' && *p != '~') {
            if(*p == '[') {
                while(*p && *p != ']') {
                    p++;
                }
                if(*p) {
                    p++;
                }
            } else {
                p++;
            }
        }
        c[n++] = (compound){ s, (size_t)(p - s), comb };
        comb = ' ';
    }
    return n && match_from(x, c, n - 1);
}

typedef struct
{
    int spec, order;
    const char* decl;
} hit;
static int hit_cmp(const void* a, const void* b)
{
    const hit *x = a, *y = b;
    return x->spec != y->spec ? (x->spec < y->spec ? -1 : 1) : (x->order < y->order ? -1 : x->order > y->order);
}

static void css_apply(xn* x, const rules* rs)
{
    if(x->name[0] == '#') {
        return;
    }
    hit hits[64];
    int nh = 0;
    for(int i = 0; i < rs->n && nh < 64; i++) {
        if(sel_match(x, rs->r[i].sel)) {
            hits[nh++] = (hit){ rs->r[i].spec, rs->r[i].order, rs->r[i].decl };
        }
    }
    if(nh) {
        qsort(hits, (size_t)nh, sizeof *hits, hit_cmp);
        sbuf st = { 0 };
        for(int i = 0; i < nh; i++) {
            sb_str(&st, hits[i].decl);
            sb_str(&st, ";");
        }
        const char* own = x_attr(x, "style");
        if(own) {
            sb_str(&st, own);
        }
        x_del(x, "style");
        x_set(x, "style", st.p); /* (last: it wins over the attributes) */
        free(st.p);
    }
    for(int i = 0; i < x->nk; i++) {
        css_apply(x->kid[i], rs);
    }
}

static void collect_css(xn* x, rules* rs, int* order)
{
    for(int i = 0; i < x->nk;) {
        xn* c = x->kid[i];
        if(is(c, "style")) {
            const char* type = x_attr(c, "type");
            if(c->text && (!type || strstr(type, "css"))) {
                css_parse(rs, c->text, order);
            }
            x_remove_at(x, i);
            x_free(c);
            continue;
        }
        collect_css(c, rs, order);
        i++;
    }
}

static void apply_stylesheets(xn* root)
{
    rules rs = { 0 };
    int order = 0;
    collect_css(root, &rs, &order);
    if(rs.n) {
        css_apply(root, &rs);
    }
    for(int i = 0; i < rs.n; i++) {
        free(rs.r[i].sel);
        free(rs.r[i].decl);
    }
    free(rs.r);
}

/* ---------------------------------------------------------------- <use> */

static xn* find_id(xn* x, const char* id)
{
    const char* a = x_attr(x, "id");
    if(a && strcmp(a, id) == 0) {
        return x;
    }
    for(int i = 0; i < x->nk; i++) {
        xn* r = find_id(x->kid[i], id);
        if(r) {
            return r;
        }
    }
    return NULL;
}

static const char* href_of(const xn* x)
{
    const char* h = x_attr(x, "href");
    if(!h) {
        h = x_attr(x, "xlink:href");
    }
    return h && h[0] == '#' ? h + 1 : NULL;
}
static const char* href_raw(const xn* x)
{
    const char* h = x_attr(x, "href");
    return h ? h : x_attr(x, "xlink:href");
}

static void expand_uses(xn* root, xn* x, int depth, int* budget);

static xn* use_to_group(xn* root, xn* use, int depth, int* budget)
{
    xn* g = xn_new("g", 1, NULL);
    for(int i = 0; i < use->na; i++) {
        const char* k = use->k[i];
        if(strcmp(k, "href") == 0 || strcmp(k, "xlink:href") == 0 || strcmp(k, "x") == 0 || strcmp(k, "y") == 0 || strcmp(k, "width") == 0 || strcmp(k, "height") == 0 || strcmp(k, "id") == 0) {
            continue;
        }
        x_set(g, k, use->v[i]);
    }
    float x = num_attr(use, "x", 0), y = num_attr(use, "y", 0);
    const char* id = href_of(use);
    xn* target = id ? find_id(root, id) : NULL;
    if(!target || depth > 10 || *budget <= 0) {
        return g;
    }
    *budget -= 1;
    char tf[512];
    const char* old = x_attr(use, "transform");
    snprintf(tf, sizeof tf, "%s%stranslate(%g %g)", old ? old : "", old ? " " : "", (double)x, (double)y);
    if(is(target, "symbol")) {
        float vx = 0, vy = 0, vw = 0, vh = 0;
        const char* vb = x_attr(target, "viewBox");
        float w = num_attr(use, "width", 0), h = num_attr(use, "height", 0);
        if(vb && sscanf(vb, "%f%*[ ,]%f%*[ ,]%f%*[ ,]%f", &vx, &vy, &vw, &vh) == 4 && vw > 0 && vh > 0) {
            double sx, sy, ox, oy;
            bool sl;
            svgx_fit(x_attr(target, "preserveAspectRatio"), vw, vh, 0, 0, w > 0 ? w : vw, h > 0 ? h : vh, &sx, &sy, &ox, &oy, &sl);
            size_t n = strlen(tf);
            snprintf(tf + n, sizeof tf - n, " translate(%g %g) scale(%g %g) translate(%g %g)", ox, oy, sx, sy, (double)-vx, (double)-vy);
        }
        x_set(g, "transform", tf);
        for(int i = 0; i < target->na; i++) { /* the symbol's own presentation attributes pass down */
            if(strcmp(target->k[i], "id") && strcmp(target->k[i], "viewBox") && strcmp(target->k[i], "preserveAspectRatio") && !x_attr(g, target->k[i])) {
                x_set(g, target->k[i], target->v[i]);
            }
        }
        for(int i = 0; i < target->nk; i++) {
            x_clone(target->kid[i], g);
        }
    } else {
        x_set(g, "transform", tf);
        xn* c = x_clone(target, g);
        x_del(c, "id");
    }
    expand_uses(root, g, depth + 1, budget);
    return g;
}

static void expand_uses(xn* root, xn* x, int depth, int* budget)
{
    for(int i = 0; i < x->nk; i++) {
        xn* c = x->kid[i];
        if(is(c, "use")) {
            xn* g = use_to_group(root, c, depth, budget);
            g->parent = x;
            x->kid[i] = g;
            x_free(c);
        } else {
            expand_uses(root, c, depth, budget);
        }
    }
}

/* ---------------------------------------------------------------- path geometry (markers, textPath) */

typedef struct
{
    double p[8];
    bool vtx;
} cub; /* a cubic; vtx: its end is a vertex of the path (not a piece of an arc) */
typedef struct
{
    cub* c;
    int n, cap;
    double sx, sy;
    bool closed;
} subpath;
typedef struct
{
    subpath* s;
    int n, cap;
} geo;

static void geo_free(geo* g)
{
    for(int i = 0; i < g->n; i++) {
        free(g->s[i].c);
    }
    free(g->s);
    memset(g, 0, sizeof *g);
}
static subpath* geo_sub(geo* g, double x, double y)
{
    if(g->n == g->cap) {
        g->cap = g->cap ? g->cap * 2 : 4;
        g->s = realloc(g->s, (size_t)g->cap * sizeof *g->s);
        if(!g->s) {
            abort();
        }
    }
    subpath* s = &g->s[g->n++];
    memset(s, 0, sizeof *s);
    s->sx = x;
    s->sy = y;
    return s;
}
static void sub_add(subpath* s, double x0, double y0, double x1, double y1, double x2, double y2, double x3, double y3, bool vtx)
{
    if(s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->c = realloc(s->c, (size_t)s->cap * sizeof *s->c);
        if(!s->c) {
            abort();
        }
    }
    s->c[s->n++] = (cub){
        { x0, y0, x1, y1, x2, y2, x3, y3 },
        vtx
    };
}
static void sub_line(subpath* s, double x0, double y0, double x1, double y1)
{
    sub_add(s, x0, y0, x0 + (x1 - x0) / 3, y0 + (y1 - y0) / 3, x0 + (x1 - x0) * 2 / 3, y0 + (y1 - y0) * 2 / 3, x1, y1, true);
}

static double vangle(double ux, double uy, double vx, double vy)
{
    return atan2(ux * vy - uy * vx, ux * vx + uy * vy);
}

static void sub_arc(subpath* s, double x1, double y1, double rx, double ry, double phi, int fa, int fs, double x2, double y2)
{
    if(x1 == x2 && y1 == y2) {
        return;
    }
    rx = fabs(rx);
    ry = fabs(ry);
    if(rx < 1e-12 || ry < 1e-12) {
        sub_line(s, x1, y1, x2, y2);
        return;
    }
    double cp = cos(phi * M_PI / 180), sp = sin(phi * M_PI / 180);
    double dx2 = (x1 - x2) / 2, dy2 = (y1 - y2) / 2;
    double x1p = cp * dx2 + sp * dy2, y1p = -sp * dx2 + cp * dy2;
    double lam = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
    if(lam > 1) {
        rx *= sqrt(lam);
        ry *= sqrt(lam);
    }
    double num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p, den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    double coef = den > 0 ? sqrt(fmax(0, num / den)) : 0;
    if(fa == fs) {
        coef = -coef;
    }
    double cxp = coef * rx * y1p / ry, cyp = -coef * ry * x1p / rx;
    double cx = cp * cxp - sp * cyp + (x1 + x2) / 2, cy = sp * cxp + cp * cyp + (y1 + y2) / 2;
    double ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry, vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;
    double th1 = vangle(1, 0, ux, uy), dth = vangle(ux, uy, vx, vy);
    if(!fs && dth > 0) {
        dth -= 2 * M_PI;
    } else if(fs && dth < 0) {
        dth += 2 * M_PI;
    }
    int nseg = (int)ceil(fabs(dth) / (M_PI / 2) - 1e-9);
    if(nseg < 1) {
        nseg = 1;
    }
    double delta = dth / nseg, k = 4.0 / 3.0 * tan(delta / 4);
    double px = x1, py = y1;
    for(int i = 0; i < nseg; i++) {
        double t0 = th1 + i * delta, t1 = t0 + delta;
        double d0x = -rx * sin(t0) * cp - ry * cos(t0) * sp, d0y = -rx * sin(t0) * sp + ry * cos(t0) * cp;
        double d1x = -rx * sin(t1) * cp - ry * cos(t1) * sp, d1y = -rx * sin(t1) * sp + ry * cos(t1) * cp;
        double ex = cx + rx * cos(t1) * cp - ry * sin(t1) * sp, ey = cy + rx * cos(t1) * sp + ry * sin(t1) * cp;
        if(i == nseg - 1) {
            ex = x2;
            ey = y2;
        }
        sub_add(s, px, py, px + k * d0x, py + k * d0y, ex - k * d1x, ey - k * d1y, ex, ey, i == nseg - 1);
        px = ex;
        py = ey;
    }
}

static void skip_sep(const char** s)
{
    while(**s && (isspace((unsigned char)**s) || **s == ',')) {
        (*s)++;
    }
}
static bool rd(const char** s, double* v)
{
    skip_sep(s);
    char* end;
    *v = strtod(*s, &end);
    if(end == *s) {
        return false;
    }
    *s = end;
    return true;
}
static bool rd_flag(const char** s, int* f)
{
    skip_sep(s);
    if(**s != '0' && **s != '1') {
        return false;
    }
    *f = **s - '0';
    (*s)++;
    return true;
}

static void path_parse(const char* d, geo* g)
{
    const char* s = d;
    char cmd = 0;
    double cx = 0, cy = 0, sx = 0, sy = 0, lcx = 0, lcy = 0;
    char last = 0;
    subpath* cur = NULL;
    while(s && *s) {
        skip_sep(&s);
        if(!*s) {
            break;
        }
        if(isalpha((unsigned char)*s)) {
            cmd = *s++;
        } else if(!cmd) {
            break;
        }
        bool rel = islower((unsigned char)cmd);
        char up = (char)toupper((unsigned char)cmd);
        double ox = rel ? cx : 0, oy = rel ? cy : 0;
        double a[7];
        if(up == 'Z') {
            if(cur) {
                sub_line(cur, cx, cy, cur->sx, cur->sy);
                cur->closed = true;
                cx = cur->sx;
                cy = cur->sy;
            }
            cur = NULL;
            last = 'Z';
            cmd = 0; /* (numbers after Z without a command are an error: stop) */
            continue;
        }
        if(up == 'M') {
            if(!rd(&s, &a[0]) || !rd(&s, &a[1])) {
                break;
            }
            cx = ox + a[0];
            cy = oy + a[1];
            sx = cx;
            sy = cy;
            cur = geo_sub(g, cx, cy);
            cmd = rel ? 'l' : 'L';
            last = 'M';
            continue;
        }
        if(!cur || cur->closed) {
            cur = geo_sub(g, cx, cy);
        }
        if(cur->n == 0 && (cur->sx != cx || cur->sy != cy)) {
            cur->sx = cx;
            cur->sy = cy;
        }
        (void)sx;
        (void)sy;
        switch(up) {
            case 'L':
                if(!rd(&s, &a[0]) || !rd(&s, &a[1])) {
                    return;
                }
                sub_line(cur, cx, cy, ox + a[0], oy + a[1]);
                cx = ox + a[0];
                cy = oy + a[1];
                break;
            case 'H':
                if(!rd(&s, &a[0])) {
                    return;
                }
                sub_line(cur, cx, cy, ox + a[0], cy);
                cx = ox + a[0];
                break;
            case 'V':
                if(!rd(&s, &a[0])) {
                    return;
                }
                sub_line(cur, cx, cy, cx, oy + a[0]);
                cy = oy + a[0];
                break;
            case 'C':
                for(int i = 0; i < 6; i++) {
                    if(!rd(&s, &a[i])) {
                        return;
                    }
                }
                sub_add(cur, cx, cy, ox + a[0], oy + a[1], ox + a[2], oy + a[3], ox + a[4], oy + a[5], true);
                lcx = ox + a[2];
                lcy = oy + a[3];
                cx = ox + a[4];
                cy = oy + a[5];
                break;
            case 'S': {
                for(int i = 0; i < 4; i++) {
                    if(!rd(&s, &a[i])) {
                        return;
                    }
                }
                double c1x = cx, c1y = cy;
                if(last == 'C' || last == 'S') {
                    c1x = 2 * cx - lcx;
                    c1y = 2 * cy - lcy;
                }
                sub_add(cur, cx, cy, c1x, c1y, ox + a[0], oy + a[1], ox + a[2], oy + a[3], true);
                lcx = ox + a[0];
                lcy = oy + a[1];
                cx = ox + a[2];
                cy = oy + a[3];
                break;
            }
            case 'Q': {
                for(int i = 0; i < 4; i++) {
                    if(!rd(&s, &a[i])) {
                        return;
                    }
                }
                double qx = ox + a[0], qy = oy + a[1], ex = ox + a[2], ey = oy + a[3];
                sub_add(cur, cx, cy, cx + 2.0 / 3 * (qx - cx), cy + 2.0 / 3 * (qy - cy), ex + 2.0 / 3 * (qx - ex), ey + 2.0 / 3 * (qy - ey), ex, ey, true);
                lcx = qx;
                lcy = qy;
                cx = ex;
                cy = ey;
                break;
            }
            case 'T': {
                for(int i = 0; i < 2; i++) {
                    if(!rd(&s, &a[i])) {
                        return;
                    }
                }
                double qx = cx, qy = cy, ex = ox + a[0], ey = oy + a[1];
                if(last == 'Q' || last == 'T') {
                    qx = 2 * cx - lcx;
                    qy = 2 * cy - lcy;
                }
                sub_add(cur, cx, cy, cx + 2.0 / 3 * (qx - cx), cy + 2.0 / 3 * (qy - cy), ex + 2.0 / 3 * (qx - ex), ey + 2.0 / 3 * (qy - ey), ex, ey, true);
                lcx = qx;
                lcy = qy;
                cx = ex;
                cy = ey;
                break;
            }
            case 'A': {
                int fa, fs;
                if(!rd(&s, &a[0]) || !rd(&s, &a[1]) || !rd(&s, &a[2]) || !rd_flag(&s, &fa) || !rd_flag(&s, &fs) || !rd(&s, &a[5]) || !rd(&s, &a[6])) {
                    return;
                }
                double ex = ox + a[5], ey = oy + a[6];
                int before = cur->n;
                sub_arc(cur, cx, cy, a[0], a[1], a[2], fa, fs, ex, ey);
                if(cur->n == before) {
                    sub_add(cur, cx, cy, cx, cy, ex, ey, ex, ey, true); /* (a zero-length arc is still a segment) */
                }
                cx = ex;
                cy = ey;
                break;
            }
            default:
                return;
        }
        last = up;
    }
}

static void points_geo(const char* pts, bool close, geo* g)
{
    const char* s = pts;
    double x, y, px = 0, py = 0;
    subpath* cur = NULL;
    while(rd(&s, &x) && rd(&s, &y)) {
        if(!cur) {
            cur = geo_sub(g, x, y);
        } else {
            sub_line(cur, px, py, x, y);
        }
        px = x;
        py = y;
    }
    if(cur && close) {
        sub_line(cur, px, py, cur->sx, cur->sy);
        cur->closed = true;
    }
}

/* the geometry of a shape in its own coordinates (without its transform) */
static void shape_geo(const xn* x, geo* g)
{
    if(is(x, "path")) {
        path_parse(x_attr(x, "d"), g);
    } else if(is(x, "line")) {
        subpath* s = geo_sub(g, num_attr(x, "x1", 0), num_attr(x, "y1", 0));
        sub_line(s, s->sx, s->sy, num_attr(x, "x2", 0), num_attr(x, "y2", 0));
    } else if(is(x, "polyline")) {
        points_geo(x_attr(x, "points"), false, g);
    } else if(is(x, "polygon")) {
        points_geo(x_attr(x, "points"), true, g);
    } else if(is(x, "rect")) {
        double rx = num_attr(x, "x", 0), ry = num_attr(x, "y", 0), w = num_attr(x, "width", 0), h = num_attr(x, "height", 0);
        subpath* s = geo_sub(g, rx, ry);
        sub_line(s, rx, ry, rx + w, ry);
        sub_line(s, rx + w, ry, rx + w, ry + h);
        sub_line(s, rx + w, ry + h, rx, ry + h);
        sub_line(s, rx, ry + h, rx, ry);
        s->closed = true;
    } else if(is(x, "circle") || is(x, "ellipse")) {
        double cx = num_attr(x, "cx", 0), cy = num_attr(x, "cy", 0);
        double rx = is(x, "circle") ? num_attr(x, "r", 0) : num_attr(x, "rx", 0), ry = is(x, "circle") ? rx : num_attr(x, "ry", 0);
        subpath* s = geo_sub(g, cx + rx, cy);
        sub_arc(s, cx + rx, cy, rx, ry, 0, 0, 1, cx - rx, cy);
        sub_arc(s, cx - rx, cy, rx, ry, 0, 0, 1, cx + rx, cy);
        s->closed = true;
    }
}

static void cub_dir(const cub* c, bool at_end, double* dx, double* dy)
{
    const double* p = c->p;
    if(!at_end) {
        for(int i = 1; i <= 3; i++) {
            *dx = p[i * 2] - p[0];
            *dy = p[i * 2 + 1] - p[1];
            if(fabs(*dx) + fabs(*dy) > 1e-9) {
                return;
            }
        }
    } else {
        for(int i = 2; i >= 0; i--) {
            *dx = p[6] - p[i * 2];
            *dy = p[7] - p[i * 2 + 1];
            if(fabs(*dx) + fabs(*dy) > 1e-9) {
                return;
            }
        }
    }
    *dx = *dy = 0;
}

/* a polyline along the geometry, with the length up to each point (a jump between subpaths adds nothing) */
typedef struct
{
    double *x, *y, *s;
    bool* jump;
    int n, cap;
} poly;
static void poly_add(poly* p, double x, double y, bool jump)
{
    if(p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 64;
        p->x = realloc(p->x, (size_t)p->cap * sizeof *p->x);
        p->y = realloc(p->y, (size_t)p->cap * sizeof *p->y);
        p->s = realloc(p->s, (size_t)p->cap * sizeof *p->s);
        p->jump = realloc(p->jump, (size_t)p->cap * sizeof *p->jump);
        if(!p->x || !p->y || !p->s || !p->jump) {
            abort();
        }
    }
    double s = p->n && !jump ? p->s[p->n - 1] + hypot(x - p->x[p->n - 1], y - p->y[p->n - 1]) : p->n ? p->s[p->n - 1]
                                                                                                     : 0;
    p->x[p->n] = x;
    p->y[p->n] = y;
    p->s[p->n] = s;
    p->jump[p->n] = jump;
    p->n++;
}
static void poly_free(poly* p)
{
    free(p->x);
    free(p->y);
    free(p->s);
    free(p->jump);
    memset(p, 0, sizeof *p);
}

static void geo_flatten(const geo* g, svgx_m2 m, poly* out)
{
    for(int i = 0; i < g->n; i++) {
        const subpath* s = &g->s[i];
        double x, y;
        m_apply(m, s->sx, s->sy, &x, &y);
        poly_add(out, x, y, true);
        for(int k = 0; k < s->n; k++) {
            const double* p = s->c[k].p;
            int steps = 24;
            for(int j = 1; j <= steps; j++) {
                double t = (double)j / steps, u = 1 - t;
                double bx = u * u * u * p[0] + 3 * u * u * t * p[2] + 3 * u * t * t * p[4] + t * t * t * p[6];
                double by = u * u * u * p[1] + 3 * u * u * t * p[3] + 3 * u * t * t * p[5] + t * t * t * p[7];
                m_apply(m, bx, by, &x, &y);
                poly_add(out, x, y, false);
            }
        }
    }
}

/* the point at length s along the polyline, and the direction there; false when s is off it */
static bool poly_at(const poly* p, double s, double* x, double* y, double* ang)
{
    if(p->n < 2 || s < 0 || s > p->s[p->n - 1]) {
        return false;
    }
    int lo = 1, hi = p->n - 1;
    while(lo < hi) {
        int mid = (lo + hi) / 2;
        if(p->s[mid] < s) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    int i = lo;
    while(i < p->n - 1 && (p->jump[i] || p->s[i] - p->s[i - 1] < 1e-12)) {
        i++;
    }
    double seg = p->s[i] - p->s[i - 1];
    double t = seg > 1e-12 ? (s - p->s[i - 1]) / seg : 0;
    t = fmin(fmax(t, 0), 1);
    *x = p->x[i - 1] + (p->x[i] - p->x[i - 1]) * t;
    *y = p->y[i - 1] + (p->y[i] - p->y[i - 1]) * t;
    *ang = atan2(p->y[i] - p->y[i - 1], p->x[i] - p->x[i - 1]);
    return true;
}

/* ---------------------------------------------------------------- markers */

/* the presentation properties content inherits, as they are where `el` is; with the initial values of the rest when `initials` */
static char* inherited_style(const xn* el, bool initials)
{
    static const struct
    {
        const char *k, *init;
    } props[] = {
        {                        "fill",   "black" },
        {                "fill-opacity",       "1" },
        {                   "fill-rule", "nonzero" },
        {                      "stroke",    "none" },
        {                "stroke-width",       "1" },
        {              "stroke-opacity",       "1" },
        {              "stroke-linecap",    "butt" },
        {             "stroke-linejoin",   "miter" },
        {           "stroke-miterlimit",       "4" },
        {            "stroke-dasharray",    "none" },
        {           "stroke-dashoffset",       "0" },
        {                 "font-family",      NULL },
        {                   "font-size",      NULL },
        {                 "font-weight",      NULL },
        {                  "font-style",      NULL },
        {                 "text-anchor",      NULL },
        {                   "clip-rule",      NULL },
        {              "letter-spacing",      NULL },
        {                "word-spacing",      NULL },
        {                   "direction",      NULL },
        {                "writing-mode",      NULL },
        {                  "visibility",      NULL },
        {                       "color",      NULL },
        {           "dominant-baseline",      NULL },
        { "color-interpolation-filters",      NULL },
        {             "image-rendering",      NULL }
    };
    sbuf b = { 0 };
    char buf[1024];
    for(size_t i = 0; i < sizeof props / sizeof *props; i++) {
        const char* v = inherited(el, props[i].k, buf, sizeof buf);
        if(!v && initials) {
            v = props[i].init;
        }
        if(v) {
            sb_fmt(&b, "%s:%s;", props[i].k, v);
        }
    }
    if(!b.p) {
        sb_str(&b, "");
    }
    return b.p;
}

static double parse_angle(const char* v)
{
    char* end;
    double a = strtod(v, &end);
    if(strncmp(end, "rad", 3) == 0) {
        return a * 180 / M_PI;
    }
    if(strncmp(end, "grad", 4) == 0) {
        return a * 0.9;
    }
    if(strncmp(end, "turn", 4) == 0) {
        return a * 360;
    }
    return a;
}

typedef struct
{
    double x, y, ang;
    int kind;
} vertex; /* kind 0 start, 1 mid, 2 end */

static int geo_vertices(const geo* g, vertex** out)
{
    int cap = 8, n = 0;
    vertex* v = malloc((size_t)cap * sizeof *v);
    for(int i = 0; i < g->n; i++) {
        const subpath* s = &g->s[i];
        /* the vertices of the subpath: its start, then each segment end that is one */
        for(int k = -1; k < s->n; k++) {
            if(k >= 0 && !s->c[k].vtx) {
                continue;
            }
            double x = k < 0 ? s->sx : s->c[k].p[6], y = k < 0 ? s->sy : s->c[k].p[7];
            double ix = 0, iy = 0, ox = 0, oy = 0;
            bool hin = false, hout = false;
            if(k >= 0) {
                cub_dir(&s->c[k], true, &ix, &iy);
                hin = fabs(ix) + fabs(iy) > 0;
            } else if(s->closed && s->n) {
                cub_dir(&s->c[s->n - 1], true, &ix, &iy);
                hin = fabs(ix) + fabs(iy) > 0;
            }
            if(k + 1 < s->n) {
                cub_dir(&s->c[k + 1], false, &ox, &oy);
                hout = fabs(ox) + fabs(oy) > 0;
            } else if(s->closed && s->n) {
                cub_dir(&s->c[0], false, &ox, &oy);
                hout = fabs(ox) + fabs(oy) > 0;
            }
            double ang = 0;
            if(hin && hout) {
                double a1 = atan2(iy, ix), a2 = atan2(oy, ox);
                ang = atan2(sin(a1) + sin(a2), cos(a1) + cos(a2));
                if(fabs(sin(a1) + sin(a2)) < 1e-12 && fabs(cos(a1) + cos(a2)) < 1e-12) {
                    ang = a1; /* (straight back: the way in) */
                }
            } else if(hin) {
                ang = atan2(iy, ix);
            } else if(hout) {
                ang = atan2(oy, ox);
            }
            if(n == cap) {
                cap *= 2;
                v = realloc(v, (size_t)cap * sizeof *v);
                if(!v) {
                    abort();
                }
            }
            v[n++] = (vertex){ x, y, ang * 180 / M_PI, 1 };
        }
    }
    if(n) {
        v[0].kind = 0;
        v[n - 1].kind = n == 1 ? 0 : 2;
    }
    *out = v;
    return n;
}

typedef struct
{
    xn* root;
    int made;
    int budget;
} mk_ctx;

/* the <g>s of a shape's markers, put after it; returns how many */
static int add_markers(mk_ctx* mc, xn* parent, int at, xn* shape)
{
    char b1[256], b2[256], b3[256], b4[256];
    const char* all = inherited(shape, "marker", b4, sizeof b4);
    const char* spec[3] = { inherited(shape, "marker-start", b1, sizeof b1), inherited(shape, "marker-mid", b2, sizeof b2), inherited(shape, "marker-end", b3, sizeof b3) };
    xn* mk[3] = { NULL, NULL, NULL };
    bool any = false;
    for(int t = 0; t < 3; t++) {
        char* id = url_id(spec[t] ? spec[t] : all);
        xn* m = id ? find_id(mc->root, id) : NULL;
        free(id);
        if(m && is(m, "marker")) {
            mk[t] = m;
            any = true;
        }
    }
    if(!any) {
        return 0;
    }
    geo g = { 0 };
    shape_geo(shape, &g);
    vertex* v = NULL;
    int nv = geo_vertices(&g, &v);
    geo_free(&g);
    char sbuf_[64];
    float sw = svgx_length(inherited(shape, "stroke-width", sbuf_, sizeof sbuf_), 1.f, 100.f);
    int added = 0;
    for(int i = 0; i < nv && mc->budget > 0; i++) {
        xn* m = mk[v[i].kind];
        if(!m) {
            continue;
        }
        float mw = svgx_length(x_attr(m, "markerWidth"), 3.f, 100.f), mh = svgx_length(x_attr(m, "markerHeight"), 3.f, 100.f);
        if(!(mw > 0) || !(mh > 0)) {
            continue;
        }
        mc->budget--;
        const char* units = x_attr(m, "markerUnits");
        double k = units && strcmp(units, "userSpaceOnUse") == 0 ? 1 : sw;
        double vbx = 0, vby = 0, vsx = 1, vsy = 1, vtx = 0, vty = 0;
        float vb[4];
        const char* vbs = x_attr(m, "viewBox");
        bool has_vb = vbs && sscanf(vbs, "%f%*[ ,]%f%*[ ,]%f%*[ ,]%f", &vb[0], &vb[1], &vb[2], &vb[3]) == 4 && vb[2] > 0 && vb[3] > 0;
        bool slice = false;
        if(has_vb) {
            svgx_fit(x_attr(m, "preserveAspectRatio"), vb[2], vb[3], 0, 0, mw, mh, &vsx, &vsy, &vtx, &vty, &slice);
            vbx = vb[0];
            vby = vb[1];
        }
        /* the content's coordinates -> the viewport's: (c - vb origin) * s + t */
        const char *rxs = x_attr(m, "refX"), *rys = x_attr(m, "refY");
        double refx = 0, refy = 0;
        if(rxs) {
            refx = strcmp(rxs, "left") == 0 ? vbx : strcmp(rxs, "center") == 0 ? vbx + (has_vb ? vb[2] : mw) / 2
                                                : strcmp(rxs, "right") == 0    ? vbx + (has_vb ? vb[2] : mw)
                                                                               : svgx_length(rxs, 0, 100.f);
        }
        if(rys) {
            refy = strcmp(rys, "top") == 0 ? vby : strcmp(rys, "center") == 0 ? vby + (has_vb ? vb[3] : mh) / 2
                                               : strcmp(rys, "bottom") == 0   ? vby + (has_vb ? vb[3] : mh)
                                                                              : svgx_length(rys, 0, 100.f);
        }
        double rvx = (refx - vbx) * vsx + vtx, rvy = (refy - vby) * vsy + vty;
        const char* orient = x_attr(m, "orient");
        double ang = 0;
        if(orient && strcmp(orient, "auto") == 0) {
            ang = v[i].ang;
        } else if(orient && strcmp(orient, "auto-start-reverse") == 0) {
            ang = v[i].ang + (v[i].kind == 0 ? 180 : 0);
        } else if(orient) {
            ang = parse_angle(orient);
        }
        /* overflow: cut to the viewport unless it is visible or the content fits */
        char ob[64];
        const char* ov = svgx_prop(m, "overflow", ob, sizeof ob);
        bool clip = !(ov && (strcmp(ov, "visible") == 0 || strcmp(ov, "auto") == 0));
        if(clip && !slice) {
            sbuf pb = { 0 };
            sb_str(&pb, ROOT1);
            sb_fmt(&pb, "<g transform='translate(%g %g) scale(%g %g) translate(%g %g)'>", vtx, vty, vsx, vsy, -vbx, -vby);
            emit_opt eo = { -1, NULL, NULL, NULL, true };
            for(int c = 0; c < m->nk; c++) {
                x_emit(&pb, m->kid[c], &eo);
            }
            sb_str(&pb, "</g></svg>");
            float bb[4];
            int ns = gp_svg_nsvg_info(pb.p, true, bb, NULL, NULL);
            free(pb.p);
            if(ns <= 0 || (bb[0] >= -1e-3 && bb[1] >= -1e-3 && bb[2] <= mw + 1e-3 && bb[3] <= mh + 1e-3)) {
                clip = false;
            }
        }
        xn* outer = xn_new("g", 1, NULL);
        char tf[512];
        const char* st = x_attr(shape, "transform");
        snprintf(tf, sizeof tf, "%s translate(%g %g) rotate(%g) scale(%g)", st ? st : "", v[i].x, v[i].y, ang, k);
        x_set(outer, "transform", tf);
        char* inh = inherited_style(m, true);
        x_set(outer, "style", inh);
        free(inh);
        if(clip) {
            char id[48];
            snprintf(id, sizeof id, "gp-mk-%d", mc->made++);
            xn* cp = xn_new("clipPath", 8, mc->root);
            x_set(cp, "id", id);
            xn* r = xn_new("rect", 4, cp);
            char nb[32];
            snprintf(nb, sizeof nb, "%g", -rvx);
            x_set(r, "x", nb);
            snprintf(nb, sizeof nb, "%g", -rvy);
            x_set(r, "y", nb);
            snprintf(nb, sizeof nb, "%g", (double)mw);
            x_set(r, "width", nb);
            snprintf(nb, sizeof nb, "%g", (double)mh);
            x_set(r, "height", nb);
            char url[64];
            snprintf(url, sizeof url, "url(#%s)", id);
            x_set(outer, "clip-path", url);
        }
        xn* inner = xn_new("g", 1, outer);
        snprintf(tf, sizeof tf, "translate(%g %g) translate(%g %g) scale(%g %g) translate(%g %g)", -rvx, -rvy, vtx, vty, vsx, vsy, -vbx, -vby);
        x_set(inner, "transform", tf);
        for(int c = 0; c < m->nk; c++) {
            if(m->kid[c]->name[0] != '#') {
                xn* cl = x_clone(m->kid[c], inner);
                x_del(cl, "id");
            }
        }
        x_insert(parent, at + 1 + added, outer);
        added++;
    }
    free(v);
    return added;
}

static bool is_unrendered(const xn* x)
{
    return is(x, "defs") || is(x, "clipPath") || is(x, "mask") || is(x, "symbol") || is(x, "linearGradient") || is(x, "radialGradient") || is(x, "pattern") || is(x, "marker") || is(x, "style") || is(x, "filter") || is(x, "title") || is(x, "desc") || is(x, "metadata");
}

static void expand_markers(mk_ctx* mc, xn* x)
{
    if(is_unrendered(x) || x->name[0] == '#') {
        return;
    }
    for(int i = 0; i < x->nk; i++) {
        xn* c = x->kid[i];
        if(is(c, "path") || is(c, "line") || is(c, "polyline") || is(c, "polygon")) {
            i += add_markers(mc, x, i, c);
        } else {
            expand_markers(mc, c);
        }
    }
}

/* ---------------------------------------------------------------- <text> */

typedef struct
{
    uint32_t cp;
    xn* src;                  /* the text, tspan or textPath it is in */
    xn* path;                 /* its textPath, if any */
    bool hx, hy, hrot;
    float x, y, dx, dy, rot;
    float adv, xscale, after; /* advance, horizontal scale (textLength), space after (letter-, word-spacing, textLength) */
    bool skip;                /* inside a ligature or cluster: no glyphs of its own */
    int g0, ng;               /* its glyphs in the run's list */
    int run;
    float px, py;             /* where it is put */
    bool hidden;              /* off the end of a textPath */
} tchar;

typedef struct
{
    int c0, c1; /* chars */
    gp_outline_glyph* g;
    int ng;
    bool rtl;
    float size, width;
    float asc, desc;
} trun;

static uint32_t utf8_next(const char* s, size_t n, size_t* i)
{
    unsigned char c = (unsigned char)s[*i];
    uint32_t cp;
    int extra;
    if(c < 0x80) {
        cp = c;
        extra = 0;
    } else if(c >> 5 == 6) {
        cp = c & 0x1f;
        extra = 1;
    } else if(c >> 4 == 14) {
        cp = c & 0x0f;
        extra = 2;
    } else if(c >> 3 == 30) {
        cp = c & 0x07;
        extra = 3;
    } else {
        (*i)++;
        return 0xfffd;
    }
    (*i)++;
    for(int k = 0; k < extra && *i < n; k++) {
        cp = cp << 6 | ((unsigned char)s[(*i)++] & 0x3f);
    }
    return cp;
}
static int utf8_put(char* o, uint32_t cp)
{
    if(cp < 0x80) {
        o[0] = (char)cp;
        return 1;
    }
    if(cp < 0x800) {
        o[0] = (char)(0xc0 | cp >> 6);
        o[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if(cp < 0x10000) {
        o[0] = (char)(0xe0 | cp >> 12);
        o[1] = (char)(0x80 | (cp >> 6 & 0x3f));
        o[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    o[0] = (char)(0xf0 | cp >> 18);
    o[1] = (char)(0x80 | (cp >> 12 & 0x3f));
    o[2] = (char)(0x80 | (cp >> 6 & 0x3f));
    o[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

/* the characters of a text node: entities decoded, CDATA markers dropped */
static void raw_chars(const char* raw, xn* src, xn* path, tchar** v, int* n, int* cap)
{
    size_t len = strlen(raw);
    for(size_t i = 0; i < len;) {
        if(strncmp(raw + i, "<![CDATA[", 9) == 0) {
            i += 9;
            continue;
        }
        if(strncmp(raw + i, "]]>", 3) == 0) {
            i += 3;
            continue;
        }
        uint32_t cp;
        if(raw[i] == '&') {
            const char* semi = memchr(raw + i, ';', len - i < 12 ? len - i : 12);
            cp = 0;
            if(semi) {
                size_t el = (size_t)(semi - (raw + i)) - 1;
                const char* e = raw + i + 1;
                if(el == 3 && strncmp(e, "amp", 3) == 0) {
                    cp = '&';
                } else if(el == 2 && strncmp(e, "lt", 2) == 0) {
                    cp = '<';
                } else if(el == 2 && strncmp(e, "gt", 2) == 0) {
                    cp = '>';
                } else if(el == 4 && strncmp(e, "quot", 4) == 0) {
                    cp = '"';
                } else if(el == 4 && strncmp(e, "apos", 4) == 0) {
                    cp = '\'';
                } else if(el == 4 && strncmp(e, "nbsp", 4) == 0) {
                    cp = 0xa0;
                } else if(el >= 2 && e[0] == '#') {
                    cp = (uint32_t)(e[1] == 'x' || e[1] == 'X' ? strtoul(e + 2, NULL, 16) : strtoul(e + 1, NULL, 10));
                }
            }
            if(cp) {
                i = (size_t)(semi - raw) + 1;
            } else {
                cp = (uint32_t)raw[i++];
            }
        } else {
            cp = utf8_next(raw, len, &i);
        }
        if(*n == *cap) {
            *cap = *cap ? *cap * 2 : 64;
            *v = realloc(*v, (size_t)*cap * sizeof **v);
            if(!*v) {
                abort();
            }
        }
        (*v)[(*n)++] = (tchar){ .cp = cp, .src = src, .path = path, .xscale = 1 };
    }
}

static void gather_chars(xn* x, xn* path, tchar** v, int* n, int* cap)
{
    for(int i = 0; i < x->nk; i++) {
        xn* c = x->kid[i];
        if(is(c, "#text")) {
            raw_chars(c->text, x, path, v, n, cap);
        } else if(is(c, "tspan") || is(c, "a")) {
            gather_chars(c, path, v, n, cap);
        } else if(is(c, "textPath")) {
            gather_chars(c, c, v, n, cap);
        }
    }
}

/* a list attribute (x, y, dx, dy, rotate) */
static int num_list(const char* v, float* out, int cap, float pct)
{
    int n = 0;
    while(v && *v && n < cap) {
        while(*v == ' ' || *v == ',' || *v == '\n' || *v == '\t') {
            v++;
        }
        if(!*v) {
            break;
        }
        char* end;
        strtof(v, &end);
        if(end == v) {
            break;
        }
        char tok[64];
        size_t tl = strcspn(v, " ,\n\t");
        snprintf(tok, sizeof tok, "%.*s", (int)(tl < 63 ? tl : 63), v);
        out[n++] = svgx_length(tok, 0, pct);
        v += tl;
    }
    return n;
}

/* the chars an element covers: [first, first + count) */
static void char_span(const tchar* ch, int n, const xn* el, int* first, int* count)
{
    *first = -1;
    *count = 0;
    for(int i = 0; i < n; i++) {
        bool in = false;
        for(const xn* a = ch[i].src; a; a = a->parent) {
            if(a == el) {
                in = true;
                break;
            }
        }
        if(in) {
            if(*first < 0) {
                *first = i;
            }
            (*count)++;
        }
    }
}

static void apply_lists(xn* el, tchar* ch, int n, float vw, float vh)
{
    if(is(el, "text") || is(el, "tspan") || is(el, "textPath")) {
        int f, cnt;
        char_span(ch, n, el, &f, &cnt);
        if(f >= 0) {
            float* buf = malloc((size_t)(cnt + 1) * sizeof *buf);
            int k;
            if(!is(el, "textPath")) {
                k = num_list(x_attr(el, "x"), buf, cnt, vw);
                for(int i = 0; i < k; i++) {
                    ch[f + i].hx = true;
                    ch[f + i].x = buf[i];
                }
                k = num_list(x_attr(el, "y"), buf, cnt, vh);
                for(int i = 0; i < k; i++) {
                    ch[f + i].hy = true;
                    ch[f + i].y = buf[i];
                }
            }
            k = num_list(x_attr(el, "dx"), buf, cnt, vw);
            for(int i = 0; i < k; i++) {
                ch[f + i].dx = buf[i];
            }
            k = num_list(x_attr(el, "dy"), buf, cnt, vh);
            for(int i = 0; i < k; i++) {
                ch[f + i].dy = buf[i];
            }
            k = num_list(x_attr(el, "rotate"), buf, cnt, 360);
            for(int i = 0; i < cnt && k > 0; i++) {
                ch[f + i].hrot = true;
                ch[f + i].rot = buf[i < k ? i : k - 1];
            } /* (the last one goes on) */
            free(buf);
        }
    }
    for(int i = 0; i < el->nk; i++) {
        if(is(el->kid[i], "tspan") || is(el->kid[i], "a") || is(el->kid[i], "textPath")) {
            apply_lists(el->kid[i], ch, n, vw, vh);
        }
    }
}

static bool is_rtl_cp(uint32_t c)
{
    return (c >= 0x0590 && c <= 0x08ff) || (c >= 0xfb1d && c <= 0xfdff) || (c >= 0xfe70 && c <= 0xfeff) || (c >= 0x10800 && c <= 0x10fff) || (c >= 0x1e800 && c <= 0x1efff);
}
static bool is_upright_cp(uint32_t c)
{ /* set upright in vertical text (CJK and the like); the rest is turned */
    return (c >= 0x1100 && c <= 0x11ff) || (c >= 0x2e80 && c <= 0xa4cf) || (c >= 0xac00 && c <= 0xd7af) || (c >= 0xf900 && c <= 0xfaff) || (c >= 0xfe30 && c <= 0xfe4f) || (c >= 0xff00 && c <= 0xff60) || (c >= 0xffe0 && c <= 0xffe6) || (c >= 0x20000 && c <= 0x3ffff) || (c >= 0x1f000 && c <= 0x1faff);
}

typedef struct
{
    int64_t sel;
    float size;
} tfont;

static tfont font_of(gp_text_system* ts, const xn* el)
{
    char buf[256], fam[256];
    const char* family = inherited(el, "font-family", fam, sizeof fam);
    float size = svgx_length(inherited(el, "font-size", buf, sizeof buf), 16.f, 16.f);
    if(!(size > 0)) {
        size = 16.f;
    }
    const char* w = inherited(el, "font-weight", buf, sizeof buf);
    int weight = 400;
    if(w) {
        weight = strncmp(w, "bold", 4) == 0 ? 700 : strncmp(w, "normal", 6) == 0 ? 400
                                                : strncmp(w, "bolder", 6) == 0   ? 700
                                                : strncmp(w, "lighter", 7) == 0  ? 300
                                                : atoi(w)                        ? atoi(w)
                                                                                 : 400;
    }
    const char* fs = inherited(el, "font-style", buf, sizeof buf);
    bool italic = fs && (strncmp(fs, "italic", 6) == 0 || strncmp(fs, "oblique", 7) == 0);
    int fid = 0;
    if(family) {
        char clean[256];
        size_t o = 0;
        for(const char* q = family; *q && o + 1 < sizeof clean; q++) {
            if(*q != '\'' && *q != '"') {
                clean[o++] = *q;
            }
        }
        clean[o] = 0;
        fid = gp_text_family(ts, clean);
    }
    return (tfont){ GP_FONT_SEL(fid, weight | (italic ? GP_FONT_ITALIC : 0)), size };
}

static bool same_font(gp_text_system* ts, const xn* a, const xn* b)
{
    if(a == b) {
        return true;
    }
    tfont fa = font_of(ts, a), fb = font_of(ts, b);
    return fa.sel == fb.sel && fa.size == fb.size;
}

/* how far down from the alphabetic baseline the baseline `dominant-baseline` names is, and baseline-shift (up, as SVG has it) */
static float baseline_offset(const xn* el, float asc, float desc, float size)
{
    char buf[64];
    const char* db = inherited(el, "dominant-baseline", buf, sizeof buf);
    if(!db) {
        db = inherited(el, "alignment-baseline", buf, sizeof buf);
    }
    float off = 0;
    if(db) {
        if(strcmp(db, "middle") == 0) {
            off = size * 0.25f; /* (half the x-height) */
        } else if(strcmp(db, "central") == 0) {
            off = (asc - desc) / 2;
        } else if(strcmp(db, "hanging") == 0) {
            off = asc * 0.8f;
        } else if(strcmp(db, "text-before-edge") == 0 || strcmp(db, "text-top") == 0) {
            off = asc;
        } else if(strcmp(db, "text-after-edge") == 0 || strcmp(db, "text-bottom") == 0 || strcmp(db, "ideographic") == 0) {
            off = -desc;
        } else if(strcmp(db, "mathematical") == 0) {
            off = size * 0.35f;
        }
    }
    char sb[64];
    const char* bs = svgx_prop(el, "baseline-shift", sb, sizeof sb);
    if(bs) {
        if(strcmp(bs, "super") == 0) {
            off -= size * 0.33f;
        } else if(strcmp(bs, "sub") == 0) {
            off += size * 0.2f;
        } else {
            off -= svgx_length(bs, 0, size);
        }
    }
    return off;
}

static bool text_only_attr(const char* k)
{
    static const char* const names[] = { "x", "y", "dx", "dy", "rotate", "textLength", "lengthAdjust", "font-family", "font-size", "font-weight", "font-style", "text-anchor", "id", "xml:space", "letter-spacing", "word-spacing", "text-decoration", "href", "xlink:href", "startOffset", "method", "spacing", "side", "path", "dominant-baseline", "alignment-baseline", "baseline-shift", "writing-mode", "direction", "unicode-bidi", "font-variant", "font-stretch", "lengthAdjust" };
    for(size_t i = 0; i < sizeof names / sizeof *names; i++) {
        if(strcmp(k, names[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* the paint of a char's element and the tspans between it and the text: attributes (inner ones win), styles concatenated */
static void paint_of(const xn* t, const xn* src, xn* p)
{
    const xn* chain[32];
    int n = 0;
    for(const xn* a = src; a && a != t && n < 32; a = a->parent) {
        chain[n++] = a;
    }
    sbuf st = { 0 };
    for(int k = n - 1; k >= 0; k--) {
        const xn* a = chain[k];
        for(int i = 0; i < a->na; i++) {
            if(text_only_attr(a->k[i]) || strcmp(a->k[i], "transform") == 0 || a->k[i][0] == '\x01') {
                continue;
            }
            if(strcmp(a->k[i], "style") == 0) {
                sb_str(&st, a->v[i]);
                sb_str(&st, ";");
                continue;
            }
            x_set(p, a->k[i], a->v[i]);
        }
    }
    if(st.p) {
        x_set(p, "style", st.p);
        free(st.p);
    }
}

/* a textPath's polyline (its own `path`, or the shape it points at, under that shape's transform), and whether it is shown reversed */
static bool text_path_poly(xn* root, xn* tp, poly* out)
{
    geo g = { 0 };
    svgx_m2 m = M_ID;
    const char* own = x_attr(tp, "path");
    if(own) {
        path_parse(own, &g);
    } else {
        const char* id = href_of(tp);
        xn* target = id ? find_id(root, id) : NULL;
        if(!target || !is_shape(target)) {
            return false;
        }
        shape_geo(target, &g);
        m = svgx_transform(x_attr(target, "transform"));
    }
    geo_flatten(&g, m, out);
    geo_free(&g);
    const char* side = x_attr(tp, "side");
    if(side && strcmp(side, "right") == 0 && out->n > 1) { /* the other way along */
        poly r = { 0 };
        for(int i = out->n - 1; i >= 0; i--) {
            poly_add(&r, out->x[i], out->y[i], i == out->n - 1 || out->jump[i + 1 < out->n ? i + 1 : i]);
        }
        poly_free(out);
        *out = r;
    }
    return out->n > 1;
}

static void convert_text(xn* root, xn* t, gp_text_system* ts, float vw, float vh)
{
    tchar* ch = NULL;
    int n = 0, cap = 0;
    gather_chars(t, NULL, &ch, &n, &cap);
    /* white space: xml:space default drops newlines, turns tabs into spaces, collapses runs and trims the ends; preserve keeps every space */
    char spb[32];
    const char* xs = x_attr(t, "xml:space");
    const char* ws = svgx_prop(t, "white-space", spb, sizeof spb);
    bool preserve = (xs && strcmp(xs, "preserve") == 0) || (ws && (strncmp(ws, "pre", 3) == 0));
    int o = 0;
    for(int i = 0; i < n; i++) {
        uint32_t c = ch[i].cp;
        if(!preserve) {
            if(c == '\n' || c == '\r') {
                continue;
            }
            if(c == '\t') {
                c = ' ';
            }
            if(c == ' ' && (o == 0 || ch[o - 1].cp == ' ')) {
                continue;
            }
        } else if(c == '\n' || c == '\r' || c == '\t') {
            c = ' ';
        }
        ch[o] = ch[i];
        ch[o++].cp = c;
    }
    n = o;
    if(!preserve) {
        while(n && ch[n - 1].cp == ' ') {
            n--;
        }
    }
    apply_lists(t, ch, n, vw, vh);

    char buf[256];
    const char* wm = inherited(t, "writing-mode", buf, sizeof buf);
    bool vertical = wm && (strncmp(wm, "tb", 2) == 0 || strncmp(wm, "vertical", 8) == 0);
    const char* dir = inherited(t, "direction", buf, sizeof buf);
    bool rtl_dir = dir && strcmp(dir, "rtl") == 0;

    /* runs: chars of one element (and one font), cut where a char is placed absolutely */
    trun* runs = NULL;
    int nr = 0;
    for(int i = 0; i < n;) {
        int j = i + 1;
        while(j < n && !ch[j].hx && !ch[j].hy && ch[j].path == ch[i].path && same_font(ts, ch[j].src, ch[i].src) && !vertical) {
            j++;
        }
        runs = realloc(runs, (size_t)(nr + 1) * sizeof *runs);
        if(!runs) {
            abort();
        }
        trun* r = &runs[nr];
        memset(r, 0, sizeof *r);
        r->c0 = i;
        r->c1 = j;
        tfont f = font_of(ts, ch[i].src);
        r->size = f.size;
        gp_text_metrics(ts, f.sel, f.size, &r->asc, &r->desc);
        char* txt = malloc((size_t)(j - i) * 4 + 1);
        int* off = malloc((size_t)(j - i + 1) * sizeof *off);
        size_t tl = 0;
        for(int k = i; k < j; k++) {
            off[k - i] = (int)tl;
            tl += (size_t)utf8_put(txt + tl, ch[k].cp);
            if(is_rtl_cp(ch[k].cp)) {
                r->rtl = true;
            }
        }
        off[j - i] = (int)tl;
        txt[tl] = 0;
        r->ng = gp_text_outline_glyphs(ts, f.sel, f.size, txt, tl, rtl_dir ? GP_BIDI_RTL : GP_BIDI_AUTO, &r->g, &r->width);
        /* each char: its glyphs and its advance (a cluster's glyphs belong to its first char) */
        for(int k = i; k < j; k++) {
            ch[k].run = nr;
            ch[k].g0 = -1;
            ch[k].ng = 0;
            ch[k].adv = 0;
            ch[k].skip = true;
        }
        for(int gi = 0; gi < r->ng; gi++) {
            int cl = r->g[gi].cluster, k = 0;
            while(k + 1 < j - i && off[k + 1] <= cl) {
                k++;
            }
            tchar* c = &ch[i + k];
            if(c->g0 < 0) {
                c->g0 = gi;
            }
            c->ng++;
            c->adv += r->g[gi].advance;
            c->skip = false;
        }
        free(txt);
        free(off);
        nr++;
        i = j;
    }

    /* spacing: letter-spacing after each char, word-spacing after each space */
    for(int i = 0; i < n; i++) {
        float size = runs[ch[i].run].size;
        const char* ls = inherited(ch[i].src, "letter-spacing", buf, sizeof buf);
        if(ls && strcmp(ls, "normal") != 0) {
            ch[i].after += svgx_length(ls, 0, size);
        }
        if(ch[i].cp == ' ') {
            const char* wsp = inherited(ch[i].src, "word-spacing", buf, sizeof buf);
            if(wsp && strcmp(wsp, "normal") != 0) {
                ch[i].after += svgx_length(wsp, 0, size);
            }
        }
        if(vertical) {
            ch[i].adv = is_upright_cp(ch[i].cp) ? size : ch[i].adv;
        }
    }
    /* textLength: innermost elements first (deepest first in this walk), on the chars they cover */
    {
        xn *stack[64], *order[256];
        int sp = 0, no = 0;
        stack[sp++] = t;
        while(sp && no < 256) {
            xn* e = stack[--sp];
            order[no++] = e;
            for(int i = 0; i < e->nk && sp < 64; i++) {
                if(is(e->kid[i], "tspan") || is(e->kid[i], "textPath") || is(e->kid[i], "a")) {
                    stack[sp++] = e->kid[i];
                }
            }
        }
        for(int q = no - 1; q >= 0; q--) {
            xn* e = order[q];
            const char* tl = x_attr(e, "textLength");
            if(!tl) {
                continue;
            }
            float want = svgx_length(tl, -1, vertical ? vh : vw);
            if(!(want >= 0)) {
                continue;
            }
            int f, cnt;
            char_span(ch, n, e, &f, &cnt);
            if(f < 0) {
                continue;
            }
            float have = 0;
            for(int i = f; i < f + cnt; i++) {
                have += ch[i].adv * ch[i].xscale + (i + 1 < f + cnt ? ch[i].after : 0);
            }
            const char* la = x_attr(e, "lengthAdjust");
            if(la && strcmp(la, "spacingAndGlyphs") == 0) {
                float adv = 0, gaps = 0;
                for(int i = f; i < f + cnt; i++) {
                    adv += ch[i].adv * ch[i].xscale;
                    if(i + 1 < f + cnt) {
                        gaps += ch[i].after;
                    }
                }
                float k = adv > 0 ? (want - gaps) / adv : 1;
                if(k > 0) {
                    for(int i = f; i < f + cnt; i++) {
                        ch[i].xscale *= k;
                    }
                }
            } else if(cnt > 1) {
                float extra = (want - have) / (float)(cnt - 1);
                for(int i = f; i < f + cnt - 1; i++) {
                    ch[i].after += extra;
                }
            }
        }
    }

    /* positions */
    float penx = 0, peny = 0;
    if(vertical) {
        penx = 0;
        peny = 0;
    }
    int chunk0 = 0;
    xn* curpath = NULL;
    poly pl = { 0 };
    bool pl_ok = false;
    double s0 = 0;
    float along = 0, perp = 0;
    char ab[64];
#define ANCHOR(el) (inherited((el), "text-anchor", ab, sizeof ab))
    for(int i = 0; i <= n; i++) {
        bool new_chunk = i == n || ch[i].hx || ch[i].hy || ch[i].path != curpath;
        if(new_chunk && i > chunk0) {
            /* anchor the chunk [chunk0, i) */
            const char* an = ANCHOR(ch[chunk0].src);
            int a = an && strcmp(an, "middle") == 0 ? 1 : an && strcmp(an, "end") == 0 ? 2
                                                                                       : 0;
            if(rtl_dir && a != 1) {
                a = 2 - a;
            }
            if(a) {
                float lo = 1e30f, hi = -1e30f;
                for(int k = chunk0; k < i; k++) {
                    float p0 = curpath ? ch[k].px : vertical ? ch[k].py
                                                             : ch[k].px;
                    float p1 = p0 + ch[k].adv * ch[k].xscale;
                    lo = fminf(lo, p0);
                    hi = fmaxf(hi, p1);
                }
                float shift = a == 1 ? -(hi - lo) / 2 : -(hi - lo);
                for(int k = chunk0; k < i; k++) {
                    if(vertical && !curpath) {
                        ch[k].py += shift;
                    } else {
                        ch[k].px += shift;
                    }
                }
                if(!curpath) {
                    if(vertical) {
                        peny += shift;
                    } else {
                        penx += shift;
                    }
                }
            }
            if(curpath && pl_ok) { /* along the path: px is the length along it, py the offset across */
                for(int k = chunk0; k < i; k++) {
                    double mid = ch[k].px + ch[k].adv * ch[k].xscale / 2, x, y, ang;
                    if(!poly_at(&pl, mid, &x, &y, &ang)) {
                        ch[k].hidden = true;
                        continue;
                    }
                    ch[k].dx = (float)ang; /* (reused: the direction there) */
                    ch[k].px = (float)x;
                    ch[k].py = (float)y;
                }
                double ex, ey, ea;
                if(i > chunk0 && poly_at(&pl, fmin(ch[i - 1].hidden ? 0 : along, pl.s[pl.n - 1]), &ex, &ey, &ea)) {
                    penx = (float)ex;
                    peny = (float)ey;
                }
            }
            chunk0 = i;
        }
        if(i == n) {
            break;
        }
        tchar* c = &ch[i];
        if(c->path != curpath) {
            if(pl_ok) {
                poly_free(&pl);
            }
            curpath = c->path;
            pl_ok = curpath && text_path_poly(root, curpath, &pl);
            if(curpath) {
                float L = pl_ok ? (float)pl.s[pl.n - 1] : 0;
                s0 = svgx_length(x_attr(curpath, "startOffset"), 0, L);
                along = (float)s0;
                perp = 0;
            }
        }
        if(curpath) {
            if(c->hx) {
                along = c->x;
            }
            along += c->dx;
            perp += c->dy;
            c->px = along;
            c->py = perp;
            c->dy = perp;
            along += c->adv * c->xscale + c->after;
            if(!pl_ok) {
                c->hidden = true;
            }
            continue;
        }
        if(c->hx) {
            penx = c->x;
        }
        if(c->hy) {
            peny = c->y;
        }
        penx += c->dx;
        peny += c->dy;
        c->px = vertical ? penx : penx;
        c->py = peny;
        if(vertical) {
            c->px = penx;
            c->py = peny;
            peny += c->adv + c->after;
        } else {
            penx += c->adv * c->xscale + c->after;
        }
    }
#undef ANCHOR
    if(pl_ok) {
        poly_free(&pl);
    }

    /* the glyphs, as paths */
    xn* out = xn_new("g", 1, NULL);
    for(int i = 0; i < n; i++) {
        tchar* c = &ch[i];
        if(c->skip || c->hidden || c->g0 < 0) {
            continue;
        }
        trun* r = &runs[c->run];
        float bo = baseline_offset(c->src, r->asc, r->desc, r->size);
        /* where in the run its glyphs were, relative to its first */
        double pen0 = 0;
        for(int gi = 0; gi < c->g0; gi++) {
            pen0 += r->g[gi].advance;
        }
        if(r->rtl) { /* right to left: the run is set as the shaping set it, from where its first char is */
            if(i != r->c0) {
                continue;
            }
            for(int gi = 0; gi < r->ng; gi++) {
                if(!r->g[gi].d) {
                    continue;
                }
                xn* p = xn_new("path", 4, out);
                x_set(p, "d", r->g[gi].d);
                char tf[160];
                snprintf(tf, sizeof tf, "translate(%g %g)", (double)(c->px + r->g[gi].x), (double)(c->py + r->g[gi].y + bo));
                x_set(p, "transform", tf);
                paint_of(t, c->src, p);
            }
            continue;
        }
        double pen = pen0;
        for(int gi = c->g0; gi < c->g0 + c->ng && gi < r->ng; gi++) {
            const gp_outline_glyph* g = &r->g[gi];
            double rel = (pen - pen0) + (g->x - pen); /* the glyph's own offset from its pen, plus how far the pen moved within the char */
            pen += g->advance;
            if(!g->d) {
                continue;
            }
            xn* p = xn_new("path", 4, out);
            x_set(p, "d", g->d);
            char tf[256];
            if(c->path) {
                snprintf(tf, sizeof tf, "translate(%g %g) rotate(%g) translate(%g %g) rotate(%g) scale(%g 1)", (double)c->px, (double)c->py, (double)c->dx * 180 / M_PI, -(double)c->adv * c->xscale / 2 + rel * c->xscale, (double)(c->dy + g->y + bo), c->hrot ? (double)c->rot : 0.0, (double)c->xscale);
            } else if(vertical) {
                if(is_upright_cp(c->cp)) {
                    snprintf(tf, sizeof tf, "translate(%g %g) rotate(%g)", c->px - (double)g->advance / 2 + rel, (double)(c->py + r->asc), c->hrot ? (double)c->rot : 0.0);
                } else {
                    snprintf(tf, sizeof tf, "translate(%g %g) rotate(%g) translate(%g %g)", c->px - (double)(r->asc - r->desc) / 2, (double)c->py, 90.0 + (c->hrot ? (double)c->rot : 0.0), rel, (double)g->y);
                }
            } else {
                snprintf(tf, sizeof tf, "translate(%g %g) rotate(%g) scale(%g 1) translate(%g %g)", (double)c->px, (double)(c->py + bo), c->hrot ? (double)c->rot : 0.0, (double)c->xscale, rel, (double)g->y);
            }
            x_set(p, "transform", tf);
            paint_of(t, c->src, p);
        }
    }
    for(int i = 0; i < nr; i++) {
        for(int k = 0; k < runs[i].ng; k++) {
            free(runs[i].g[k].d);
        }
        free(runs[i].g);
    }
    free(runs);
    free(ch);

    /* the text becomes a group of the paths: its paint stays, its font properties go */
    for(int i = t->na - 1; i >= 0; i--) {
        if(text_only_attr(t->k[i])) {
            x_del(t, t->k[i]);
        }
    }
    for(int i = 0; i < t->nk; i++) {
        x_free(t->kid[i]);
    }
    free(t->kid);
    t->kid = out->kid;
    t->nk = out->nk;
    t->cap_k = out->cap_k;
    for(int i = 0; i < t->nk; i++) {
        t->kid[i]->parent = t;
    }
    out->kid = NULL;
    out->nk = 0;
    x_free(out);
    free(t->text);
    t->text = NULL;
    free(t->name);
    t->name = dups("g");
}

static void convert_texts(xn* root, xn* x, gp_text_system* ts, float vw, float vh)
{
    if(is_unrendered(x)) {
        return;
    }
    if(is(x, "text")) {
        convert_text(root, x, ts, vw, vh);
        return;
    }
    for(int i = 0; i < x->nk; i++) {
        convert_texts(root, x->kid[i], ts, vw, vh);
    }
}

/* ---------------------------------------------------------------- layers */

enum { U_PLAIN,
    U_GROUP,
    U_PART,
    U_PAT,
    U_IMAGE,
};

typedef struct
{
    int kind;
    xn* el;
    int parent;        /* the layer it is in (-1: the document) */
    char *clip, *mask; /* ids */
    char* filters[8];  /* ids, applied in order */
    int nf;
    float opacity;
    const char* ovr;   /* U_PART, U_PAT: the paint written over the element's */
    char* pat;         /* U_PAT: the pattern's id */
    int which;         /* U_PAT: 0 fill, 1 stroke */
    bool bb_done, bb_ok, m_done;
    float bb[4];
    svgx_m2 m;         /* user space -> the document's px */
} unit;

struct svgx
{
    xn *root, *holder; /* the document; what was taken out of it (clip paths, masks, filters, patterns, markers, symbols) */
    unit* u;
    int n, cap;
    int *first, *next; /* children of each layer (index n: the document) */
    gp_text_system* ts;
    int depth;
    char* defs;        /* everything that is not drawn, as <defs>, for the documents made from this one */
    char* root_open;   /* the root's tag */
    float vw, vh;      /* the viewBox's size */
    int made;          /* ids made up */
};

xn* svgx_find(struct svgx* d, const char* id)
{
    xn* r = find_id(d->holder, id);
    return r ? r : find_id(d->root, id);
}

static int add_unit(struct svgx* d, int kind, xn* el, int parent)
{
    if(d->n == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 8;
        d->u = realloc(d->u, (size_t)d->cap * sizeof *d->u);
        if(!d->u) {
            abort();
        }
    }
    unit* u = &d->u[d->n];
    memset(u, 0, sizeof *u);
    u->kind = kind;
    u->el = el;
    u->parent = parent;
    u->opacity = 1;
    return d->n++;
}

static bool hidden(const xn* x)
{
    char b[32];
    const char* v = svgx_prop(x, "display", b, sizeof b);
    return v && strcmp(v, "none") == 0;
}

static int count_drawn(const xn* x)
{
    if(is_unrendered(x) || x->name[0] == '#' || hidden(x)) {
        return 0;
    }
    if(is_shape(x) || is(x, "image")) {
        return 1;
    }
    int n = 0;
    for(int i = 0; i < x->nk; i++) {
        n += count_drawn(x->kid[i]);
    }
    return n;
}
static xn* only_drawn(xn* x)
{
    if(is_unrendered(x) || x->name[0] == '#' || hidden(x)) {
        return NULL;
    }
    if(is_shape(x) || is(x, "image")) {
        return x;
    }
    for(int i = 0; i < x->nk; i++) {
        xn* r = only_drawn(x->kid[i]);
        if(r) {
            return r;
        }
    }
    return NULL;
}

static bool paint_visible(const xn* x, const char* prop)
{
    char b[256];
    const char* v = inherited(x, prop, b, sizeof b);
    if(!v) {
        return strcmp(prop, "fill") == 0;
    }
    return strcmp(v, "none") != 0 && strcmp(v, "transparent") != 0;
}

/* the pattern a shape's fill or stroke is painted with */
static char* pattern_paint(struct svgx* d, const xn* x, const char* prop)
{
    char b[256];
    const char* v = inherited(x, prop, b, sizeof b);
    char* id = url_id(v);
    if(!id) {
        return NULL;
    }
    xn* p = svgx_find(d, id);
    if(p && is(p, "pattern")) {
        return id;
    }
    free(id);
    return NULL;
}

/* a filter property: url(#id)s and CSS filter functions, the functions written as filters of their own */
static int parse_filters(struct svgx* d, const char* v, char** ids, int cap)
{
    int n = 0;
    while(v && *v && n < cap) {
        while(*v == ' ' || *v == ',') {
            v++;
        }
        if(!*v) {
            break;
        }
        if(strncmp(v, "url(", 4) == 0) {
            char* id = url_id(v);
            const char* e = strchr(v, ')');
            v = e ? e + 1 : v + strlen(v);
            if(id) {
                xn* f = svgx_find(d, id);
                if(f && is(f, "filter")) {
                    ids[n++] = id;
                } else { /* a filter that is not there: the whole list is ignored (Filter Effects 1) */
                    free(id);
                    for(int i = 0; i < n; i++) {
                        free(ids[i]);
                    }
                    return 0;
                }
            }
            continue;
        }
        size_t nl = strcspn(v, "( ");
        if(v[nl] != '(') {
            break;
        }
        char name[32];
        snprintf(name, sizeof name, "%.*s", (int)(nl < 31 ? nl : 31), v);
        const char *a = v + nl + 1, *e = a;
        int par = 1;
        while(*e && par) {
            if(*e == '(') {
                par++;
            } else if(*e == ')') {
                par--;
            }
            if(par) {
                e++;
            }
        }
        char args[256];
        snprintf(args, sizeof args, "%.*s", (int)((size_t)(e - a) < 255 ? (size_t)(e - a) : 255), a);
        v = *e ? e + 1 : e;
        /* the amount: a number or a percentage */
        char* end;
        float amt = strtof(args, &end);
        bool has = end != args;
        if(has && *end == '%') {
            amt /= 100.f;
        }
        char id[48];
        snprintf(id, sizeof id, "gp-css-%d", d->made++);
        xn* f = xn_new("filter", 6, d->holder);
        x_set(f, "id", id);
        x_set(f, "color-interpolation-filters", "sRGB");
        x_set(f, "\x01noregion", "1");
        char nb[512];
        if(strcmp(name, "blur") == 0) {
            xn* p = xn_new("feGaussianBlur", 14, f);
            snprintf(nb, sizeof nb, "%g", has ? (double)svgx_length(args, 0, 0) : 0.0);
            x_set(p, "stdDeviation", nb);
        } else if(strcmp(name, "brightness") == 0 || strcmp(name, "contrast") == 0) {
            float k = has ? amt : 1;
            xn* p = xn_new("feComponentTransfer", 19, f);
            for(int c = 0; c < 3; c++) {
                xn* fn = xn_new(c == 0 ? "feFuncR" : c == 1 ? "feFuncG"
                                                            : "feFuncB",
                    7,
                    p);
                x_set(fn, "type", "linear");
                snprintf(nb, sizeof nb, "%g", (double)k);
                x_set(fn, "slope", nb);
                snprintf(nb, sizeof nb, "%g", name[0] == 'c' ? (double)(0.5f - 0.5f * k) : 0.0);
                x_set(fn, "intercept", nb);
            }
        } else if(strcmp(name, "grayscale") == 0 || strcmp(name, "sepia") == 0) {
            float s = fminf(fmaxf(has ? amt : 1, 0), 1), r = 1 - s;
            xn* p = xn_new("feColorMatrix", 13, f);
            x_set(p, "type", "matrix");
            if(name[0] == 'g') {
                snprintf(nb, sizeof nb, "%g %g %g 0 0 %g %g %g 0 0 %g %g %g 0 0 0 0 0 1 0", 0.2126 + 0.7874 * r, 0.7152 - 0.7152 * r, 0.0722 - 0.0722 * r, 0.2126 - 0.2126 * r, 0.7152 + 0.2848 * r, 0.0722 - 0.0722 * r, 0.2126 - 0.2126 * r, 0.7152 - 0.7152 * r, 0.0722 + 0.9278 * r);
            } else {
                snprintf(nb, sizeof nb, "%g %g %g 0 0 %g %g %g 0 0 %g %g %g 0 0 0 0 0 1 0", 0.393 + 0.607 * r, 0.769 - 0.769 * r, 0.189 - 0.189 * r, 0.349 - 0.349 * r, 0.686 + 0.314 * r, 0.168 - 0.168 * r, 0.272 - 0.272 * r, 0.534 - 0.534 * r, 0.131 + 0.869 * r);
            }
            x_set(p, "values", nb);
        } else if(strcmp(name, "saturate") == 0 || strcmp(name, "hue-rotate") == 0) {
            xn* p = xn_new("feColorMatrix", 13, f);
            x_set(p, "type", name[0] == 's' ? "saturate" : "hueRotate");
            snprintf(nb, sizeof nb, "%g", name[0] == 's' ? (double)(has ? amt : 1) : (has ? parse_angle(args) : 0.0));
            x_set(p, "values", nb);
        } else if(strcmp(name, "invert") == 0 || strcmp(name, "opacity") == 0) {
            float k = fminf(fmaxf(has ? amt : 1, 0), 1);
            xn* p = xn_new("feComponentTransfer", 19, f);
            if(name[0] == 'i') {
                for(int c = 0; c < 3; c++) {
                    xn* fn = xn_new(c == 0 ? "feFuncR" : c == 1 ? "feFuncG"
                                                                : "feFuncB",
                        7,
                        p);
                    x_set(fn, "type", "table");
                    snprintf(nb, sizeof nb, "%g %g", (double)k, (double)(1 - k));
                    x_set(fn, "tableValues", nb);
                }
            } else {
                xn* fn = xn_new("feFuncA", 7, p);
                x_set(fn, "type", "table");
                snprintf(nb, sizeof nb, "0 %g", (double)k);
                x_set(fn, "tableValues", nb);
            }
        } else if(strcmp(name, "drop-shadow") == 0) {
            /* lengths (dx dy [blur]) and a color, in either order */
            float len[3] = { 0, 0, 0 };
            int nlen = 0;
            char color[128] = "black";
            const char* q = args;
            while(*q) {
                while(*q == ' ') {
                    q++;
                }
                if(!*q) {
                    break;
                }
                const char* s = q;
                if(isdigit((unsigned char)*q) || *q == '-' || *q == '.' || *q == '+') {
                    while(*q && *q != ' ') {
                        q++;
                    }
                    char tok[64];
                    snprintf(tok, sizeof tok, "%.*s", (int)(q - s < 63 ? q - s : 63), s);
                    if(nlen < 3) {
                        len[nlen++] = svgx_length(tok, 0, 0);
                    }
                } else {
                    int pp = 0;
                    while(*q && (pp || *q != ' ')) {
                        if(*q == '(') {
                            pp++;
                        } else if(*q == ')') {
                            pp--;
                        }
                        q++;
                    }
                    snprintf(color, sizeof color, "%.*s", (int)(q - s < 127 ? q - s : 127), s);
                }
            }
            xn* p = xn_new("feDropShadow", 12, f);
            snprintf(nb, sizeof nb, "%g", (double)len[0]);
            x_set(p, "dx", nb);
            snprintf(nb, sizeof nb, "%g", (double)len[1]);
            x_set(p, "dy", nb);
            snprintf(nb, sizeof nb, "%g", (double)len[2] / 2);
            x_set(p, "stdDeviation", nb); /* (a blur radius is twice the deviation) */
            x_set(p, "flood-color", color);
        } else {
            continue;                     /* (unknown: as none) */
        }
        ids[n++] = dups(id);
    }
    return n;
}

static void assign(struct svgx* d, xn* x, int parent);

static void assign_kids(struct svgx* d, xn* x, int parent)
{
    for(int i = 0; i < x->nk; i++) {
        assign(d, x->kid[i], parent);
    }
}

/* a shape (or the part of one) drawn by nanosvg in a plain layer: the one before it if that is a plain one of this parent */
static void plain_shape(struct svgx* d, xn* x, int parent)
{
    int last = d->n - 1;
    if(last >= 0 && d->u[last].kind == U_PLAIN && d->u[last].parent == parent) {
        x->unit = last;
        return;
    }
    x->unit = add_unit(d, U_PLAIN, NULL, parent);
}

static void shape_content(struct svgx* d, xn* x, int parent)
{
    char *pf = pattern_paint(d, x, "fill"), *ps = pattern_paint(d, x, "stroke");
    if(!pf && !ps) {
        plain_shape(d, x, parent);
        return;
    }
    /* a pattern: fill and stroke drawn apart, in that order, each a layer */
    if(paint_visible(x, "fill")) {
        int u = add_unit(d, pf ? U_PAT : U_PART, x, parent);
        d->u[u].ovr = pf ? "fill:#000;stroke:none" : "stroke:none";
        d->u[u].pat = pf;
        d->u[u].which = 0;
        pf = NULL;
    }
    if(paint_visible(x, "stroke")) {
        int u = add_unit(d, ps ? U_PAT : U_PART, x, parent);
        d->u[u].ovr = ps ? "fill:none;stroke:#000" : "fill:none";
        d->u[u].pat = ps;
        d->u[u].which = 1;
        ps = NULL;
    }
    free(pf);
    free(ps);
}

static void assign(struct svgx* d, xn* x, int parent)
{
    if(is_unrendered(x) || x->name[0] == '#' || hidden(x)) {
        return;
    }
    bool shape = is_shape(x), image = is(x, "image");
    if(!shape && !image && !is_holder(x)) {
        return; /* (unknown elements, and what is in them, are not drawn) */
    }
    char *clip = take_prop(x, "clip-path"), *mask = take_prop(x, "mask"), *filt = take_prop(x, "filter");
    char *cid = url_id(clip), *mid = url_id(mask);
    free(clip);
    free(mask);
    if(cid) {
        xn* c = svgx_find(d, cid);
        if(!c || !is(c, "clipPath")) {
            free(cid);
            cid = NULL;
        }
    }
    if(mid) {
        xn* m = svgx_find(d, mid);
        if(!m || !is(m, "mask")) {
            free(mid);
            mid = NULL;
        }
    }
    char* fids[8];
    int nf = 0;
    if(filt && strcmp(filt, "none") != 0) {
        nf = parse_filters(d, filt, fids, 8);
    }
    free(filt);
    if(nf < 0) {
        free(cid);
        free(mid);
        return;
    }
    /* opacity: a group's applies to what it holds as a whole; one thing alone takes it as its own; a shape with both a fill and a stroke needs a layer */
    float op = 1;
    char* ops = NULL;
    if(!image) {
        ops = take_prop(x, "opacity");
    }
    if(ops) {
        op = fminf(fmaxf(strtof(ops, NULL), 0), 1);
        free(ops);
    }
    bool group_op = false;
    if(op < 1) {
        if(shape) {
            group_op = paint_visible(x, "fill") && paint_visible(x, "stroke");
            if(!group_op) {
                char b[32];
                snprintf(b, sizeof b, "%g", (double)op);
                x_set(x, "opacity", b);
            }
        } else {
            int cnt = count_drawn(x);
            if(cnt >= 2) {
                group_op = true;
            } else if(cnt == 1) {
                xn* one = only_drawn(x);
                char b[64];
                const char* ov = svgx_prop(one, "opacity", b, sizeof b);
                float own = ov ? strtof(ov, NULL) : 1;
                style_drop(one, "opacity");
                snprintf(b, sizeof b, "%g", (double)(own * op));
                x_set(one, "opacity", b);
            }
        }
    }
    if(cid || mid || nf > 0 || group_op) {
        int u = add_unit(d, U_GROUP, x, parent);
        d->u[u].clip = cid;
        d->u[u].mask = mid;
        for(int i = 0; i < nf; i++) {
            d->u[u].filters[i] = fids[i];
        }
        d->u[u].nf = nf;
        d->u[u].opacity = group_op ? op : 1;
        parent = u;
    }
    if(image) {
        char b[32];
        const char* vis = inherited(x, "visibility", b, sizeof b);
        if(!(vis && (strcmp(vis, "hidden") == 0 || strcmp(vis, "collapse") == 0))) {
            add_unit(d, U_IMAGE, x, parent);
        }
    } else if(shape) {
        shape_content(d, x, parent);
    } else {
        assign_kids(d, x, parent);
    }
}

/* ---------------------------------------------------------------- the documents made for a layer */

static void emit_defs_of(sbuf* b, const xn* x)
{
    if(x->name[0] == '#') {
        return;
    }
    if(is_unrendered(x) && !is(x, "style")) {
        emit_opt eo = { -1, NULL, NULL, NULL, false };
        x_emit(b, x, &eo);
        return;
    }
    for(int i = 0; i < x->nk; i++) {
        emit_defs_of(b, x->kid[i]);
    }
}

static const char* all_defs(struct svgx* d)
{
    if(!d->defs) {
        sbuf b = { 0 };
        sb_str(&b, "<defs>");
        emit_defs_of(&b, d->root);
        emit_opt eo = { -1, NULL, NULL, NULL, false };
        for(int i = 0; i < d->holder->nk; i++) {
            x_emit(&b, d->holder->kid[i], &eo);
        }
        sb_str(&b, "</defs>");
        d->defs = b.p;
    }
    return d->defs;
}

/* the start of a document in an element's user space: the root, the defs, a <g> for each transform from the root down to the element (its own too).
 * Returns how many <g>s to close. */
static int ctx_open(struct svgx* d, sbuf* b, const xn* el, bool defs)
{
    sb_str(b, d->root_open);
    if(defs) {
        sb_str(b, all_defs(d));
    }
    const xn* chain[64];
    int depth = 0;
    for(const xn* a = el; a && depth < 64; a = a->parent) {
        chain[depth++] = a;
    }
    int wrappers = 0;
    for(int k = depth - 1; k >= 0; k--) {
        const xn* a = chain[k];
        if(a == d->root) {
            continue;
        }
        const char* t = x_attr(a, "transform");
        if(!t) {
            continue;
        }
        sb_str(b, "<g");
        x_emit_attr(b, "transform", t);
        sb_str(b, ">");
        wrappers++;
    }
    return wrappers;
}
static void ctx_close(sbuf* b, int wrappers)
{
    for(int k = 0; k < wrappers; k++) {
        sb_str(b, "</g>");
    }
    sb_str(b, "</svg>");
}

static unit* unit_of(svgx_rc* rc)
{
    return &rc->d->u[rc->unit];
}

svgx_m2 svgx_device(svgx_rc* rc)
{
    unit* u = unit_of(rc);
    if(!u->m_done) {
        sbuf b = { 0 };
        int w = ctx_open(rc->d, &b, u->el, false);
        sb_str(&b, PROBE);
        ctx_close(&b, w);
        u->m = probe_matrix(b.p);
        free(b.p);
        u->m_done = true;
    }
    svgx_m2 s = { rc->scale, 0, 0, rc->scale, 0, 0 };
    return svgx_mul(s, u->m);
}

bool svgx_bbox(svgx_rc* rc, float bb[4])
{
    unit* u = unit_of(rc);
    if(!u->bb_done) {
        u->bb_done = true;
        sbuf b = { 0 };
        sb_str(&b, ROOT1);
        emit_opt eo = { -1, NULL, NULL, u->el, true };
        if(u->el == rc->d->root) {
            for(int i = 0; i < u->el->nk; i++) {
                x_emit(&b, u->el->kid[i], &eo);
            }
        } else {
            x_emit(&b, u->el, &eo);
        }
        sb_str(&b, "</svg>");
        float r[4];
        int ns = gp_svg_nsvg_info(b.p, false, r, NULL, NULL);
        free(b.p);
        u->bb_ok = ns > 0;
        u->bb[0] = r[0];
        u->bb[1] = r[1];
        u->bb[2] = r[2] - r[0];
        u->bb[3] = r[3] - r[1];
    }
    memcpy(bb, u->bb, sizeof u->bb);
    return u->bb_ok;
}

float svgx_viewport_diag(svgx_rc* rc, int axis)
{
    struct svgx* d = rc->d;
    return axis == 0 ? d->vw : axis == 1 ? d->vh
                                         : sqrtf((d->vw * d->vw + d->vh * d->vh) / 2);
}

static float* alpha_of(const uint8_t* straight, int w, int h)
{
    float* a = malloc((size_t)w * (size_t)h * sizeof *a);
    if(!a) {
        abort();
    }
    for(size_t i = 0; i < (size_t)w * (size_t)h; i++) {
        a[i] = straight ? (float)straight[i * 4 + 3] / 255.f : 0.f;
    }
    return a;
}

float* svgx_rect_cover(svgx_rc* rc, float x, float y, float w, float h)
{
    sbuf b = { 0 };
    int wr = ctx_open(rc->d, &b, unit_of(rc)->el, false);
    sb_fmt(&b, "<rect x='%g' y='%g' width='%g' height='%g' fill='#000'/>", (double)x, (double)y, (double)w, (double)h);
    ctx_close(&b, wr);
    uint8_t* px = gp_svg_nsvg_raster(b.p, rc->w, rc->h, rc->scale);
    free(b.p);
    float* a = alpha_of(px, rc->w, rc->h);
    free(px);
    return a;
}

static uint8_t* render_sub(struct svgx* d, const char* doc, int w, int h, float scale)
{
    return gp_svgx_render_doc(doc, strlen(doc), w, h, scale, d->ts, d->depth + 1);
}

uint8_t* svgx_render_ref(svgx_rc* rc, xn* el, float tx, float ty)
{
    sbuf b = { 0 };
    int wr = ctx_open(rc->d, &b, unit_of(rc)->el, true);
    sb_fmt(&b, "<g transform='translate(%g %g)'>", (double)tx, (double)ty);
    emit_opt eo = { -1, NULL, NULL, NULL, false };
    x_emit(&b, el, &eo);
    sb_str(&b, "</g>");
    ctx_close(&b, wr);
    uint8_t* px = render_sub(rc->d, b.p, rc->w, rc->h, rc->scale);
    free(b.p);
    return px;
}

/* ---------------------------------------------------------------- compositing (premultiplied bytes) */

static void over(uint8_t* dst, const uint8_t* src, size_t n)
{
    for(size_t p = 0; p < n; p++) {
        const uint8_t* s = src + p * 4;
        if(!s[3] && !s[0] && !s[1] && !s[2]) {
            continue;
        }
        uint8_t* o = dst + p * 4;
        unsigned inv = 255u - s[3];
        for(int c = 0; c < 4; c++) {
            unsigned v = s[c] + (o[c] * inv + 127) / 255;
            o[c] = (uint8_t)(v > 255 ? 255 : v);
        }
    }
}

static void premultiply(uint8_t* px, size_t n)
{
    gp_image_premultiply(px, n);
}

static void scale_by(uint8_t* px, const float* k, size_t n)
{
    for(size_t p = 0; p < n; p++) {
        float f = k[p];
        if(f >= 1.f) {
            continue;
        }
        for(int c = 0; c < 4; c++) {
            px[p * 4 + c] = (uint8_t)((float)px[p * 4 + c] * f + 0.5f);
        }
    }
}

/* ---------------------------------------------------------------- clips, masks */

/* a clip path's or mask's content, in the element's user space (bounding-box units through the box), wrapped with what it inherits */
static char* content_doc(struct svgx* d, svgx_rc* rc, xn* holder, bool clip, bool obb)
{
    sbuf b = { 0 };
    unit* u = unit_of(rc);
    int wr = ctx_open(d, &b, u->el, true);
    const char* tf = x_attr(holder, "transform"); /* (a clipPath's own transform) */
    if(tf && clip) {
        sb_str(&b, "<g");
        x_emit_attr(&b, "transform", tf);
        sb_str(&b, ">");
        wr++;
    }
    if(obb) {
        float bb[4];
        svgx_bbox(rc, bb);
        sb_fmt(&b, "<g transform='matrix(%g 0 0 %g %g %g)'>", (double)bb[2], (double)bb[3], (double)bb[0], (double)bb[1]);
        wr++;
    }
    const char* inh = x_attr(holder, "\x01inh");
    sb_str(&b, "<g");
    if(clip) { /* a clip on the clip path itself */
        const char* cp = x_attr(holder, "clip-path");
        if(!cp) {
            char cb[256];
            cp = decl_of(x_attr(holder, "style"), "clip-path", cb, sizeof cb);
            if(cp) {
                x_emit_attr(&b, "clip-path", cp);
            }
        } else {
            x_emit_attr(&b, "clip-path", cp);
        }
    } else if(inh && *inh) {
        x_emit_attr(&b, "style", inh);
    }
    sb_str(&b, ">");
    wr++;
    emit_opt eo = { -1, NULL, NULL, NULL, false };
    for(int k = 0; k < holder->nk; k++) {
        if(holder->kid[k]->name[0] == '#') {
            continue;
        }
        if(!clip) {
            x_emit(&b, holder->kid[k], &eo);
            continue;
        }
        /* a clip is a shape of solid black, whatever the shapes were painted with; clip-rule is the fill rule */
        xn* c = x_clone(holder->kid[k], NULL);
        xn* stack[512];
        int sp = 0;
        stack[sp++] = c;
        char crb[32];
        const char* hcr = inherited(holder, "clip-rule", crb, sizeof crb);
        char hcr_copy[32];
        snprintf(hcr_copy, sizeof hcr_copy, "%s", hcr ? hcr : "");
        while(sp) {
            xn* x = stack[--sp];
            if(is_shape(x) || is(x, "g") || is(x, "text") || is(x, "tspan") || is(x, "textPath")) {
                char rb[32];
                const char* cr = svgx_prop(x, "clip-rule", rb, sizeof rb);
                char crs[32];
                snprintf(crs, sizeof crs, "%s", cr ? cr : "");
                for(const char* const* p = (const char* const[]){ "fill", "fill-opacity", "opacity", "stroke", "fill-rule", "filter", "mask", NULL }; *p; p++) {
                    x_del(x, *p);
                    style_drop(x, *p);
                }
                if(is_shape(x) || is(x, "text")) {
                    x_set(x, "fill", "#000");
                    x_set(x, "stroke", "none");
                    if(*crs) {
                        x_set(x, "fill-rule", crs);
                    } else if(hcr_copy[0]) {
                        x_set(x, "fill-rule", hcr_copy);
                    }
                } else if(*crs) {
                    x_set(x, "clip-rule", crs);
                }
            }
            for(int q = 0; q < x->nk && sp < 512; q++) {
                stack[sp++] = x->kid[q];
            }
        }
        /* clip-rule set on a group passes to its shapes */
        x_emit(&b, c, &eo);
        x_free(c);
    }
    ctx_close(&b, wr);
    return b.p;
}

static void apply_clip(struct svgx* d, svgx_rc* rc, uint8_t* buf)
{
    unit* u = unit_of(rc);
    xn* cp = svgx_find(d, u->clip);
    if(!cp) {
        return;
    }
    const char* units = x_attr(cp, "clipPathUnits");
    bool obb = units && strcmp(units, "objectBoundingBox") == 0;
    float bb[4];
    size_t n = (size_t)rc->w * (size_t)rc->h;
    if(obb && (!svgx_bbox(rc, bb) || bb[2] <= 0 || bb[3] <= 0)) {
        memset(buf, 0, n * 4);
        return;
    }
    char* doc = content_doc(d, rc, cp, true, obb);
    uint8_t* px = render_sub(d, doc, rc->w, rc->h, rc->scale);
    free(doc);
    float* a = malloc(n * sizeof *a);
    if(!a) {
        abort();
    }
    for(size_t p = 0; p < n; p++) {
        a[p] = px ? (float)px[p * 4 + 3] / 255.f : 0.f;
    }
    scale_by(buf, a, n);
    free(a);
    free(px);
}

/* a rectangle given in bounding-box fractions or user units (percentages of the viewport) */
static void region_of(svgx_rc* rc, xn* el, bool obb, const float bb[4], const float def[4], float out[4])
{
    static const char* const names[4] = { "x", "y", "width", "height" };
    for(int i = 0; i < 4; i++) {
        const char* v = x_attr(el, names[i]);
        if(obb) {
            float f = def[i];
            if(v) {
                char* end;
                f = strtof(v, &end);
                if(end == v) {
                    f = def[i];
                } else if(*end == '%') {
                    f /= 100.f;
                }
            }
            out[i] = (i % 2 == 0 ? bb[0] * (i == 0) + bb[2] * f : bb[1] * (i == 1) + bb[3] * f);
        } else {
            float pct = svgx_viewport_diag(rc, i % 2);
            out[i] = v ? svgx_length(v, 0, pct) : (i < 2 ? (i == 0 ? bb[0] : bb[1]) + def[i] * (i == 0 ? bb[2] : bb[3]) : def[i] * (i == 2 ? bb[2] : bb[3]));
        }
    }
}

static void apply_mask(struct svgx* d, svgx_rc* rc, uint8_t* buf)
{
    unit* u = unit_of(rc);
    xn* m = svgx_find(d, u->mask);
    if(!m) {
        return;
    }
    size_t n = (size_t)rc->w * (size_t)rc->h;
    const char *mu = x_attr(m, "maskUnits"), *cu = x_attr(m, "maskContentUnits");
    bool obb = !(mu && strcmp(mu, "userSpaceOnUse") == 0), cobb = cu && strcmp(cu, "objectBoundingBox") == 0;
    float bb[4] = { 0, 0, 0, 0 };
    bool has_bb = svgx_bbox(rc, bb);
    if((obb || cobb) && (!has_bb || bb[2] <= 0 || bb[3] <= 0)) {
        memset(buf, 0, n * 4);
        return;
    }
    char* doc = content_doc(d, rc, m, false, cobb);
    uint8_t* px = render_sub(d, doc, rc->w, rc->h, rc->scale);
    free(doc);
    char tb[32];
    const char* mt = svgx_prop(m, "mask-type", tb, sizeof tb);
    bool alpha = mt && strcmp(mt, "alpha") == 0;
    float* k = malloc(n * sizeof *k);
    if(!k) {
        abort();
    }
    for(size_t p = 0; p < n; p++) {
        const uint8_t* q = px ? px + p * 4 : (const uint8_t*)"\0\0\0\0";
        k[p] = alpha ? (float)q[3] / 255.f : (0.2125f * (float)q[0] + 0.7154f * (float)q[1] + 0.0721f * (float)q[2]) / 255.f; /* (premultiplied: luminance times alpha) */
    }
    free(px);
    /* the mask's region: outside it nothing shows */
    static const float def[4] = { -0.1f, -0.1f, 1.2f, 1.2f };
    float r[4];
    region_of(rc, m, obb, bb, def, r);
    float* cov = svgx_rect_cover(rc, r[0], r[1], r[2], r[3]);
    for(size_t p = 0; p < n; p++) {
        k[p] *= cov[p];
    }
    free(cov);
    scale_by(buf, k, n);
    free(k);
}

/* ---------------------------------------------------------------- patterns */

/* a pattern attribute, from it or the patterns it points at */
static const char* pat_attr(struct svgx* d, xn* p, const char* k)
{
    for(int hop = 0; p && hop < 8; hop++) {
        const char* v = x_attr(p, k);
        if(v) {
            return v;
        }
        const char* id = href_of(p);
        xn* n = id ? svgx_find(d, id) : NULL;
        p = n && is(n, "pattern") ? n : NULL;
    }
    return NULL;
}
static xn* pat_content(struct svgx* d, xn* p)
{
    for(int hop = 0; p && hop < 8; hop++) {
        for(int i = 0; i < p->nk; i++) {
            if(p->kid[i]->name[0] != '#') {
                return p;
            }
        }
        const char* id = href_of(p);
        xn* n = id ? svgx_find(d, id) : NULL;
        p = n && is(n, "pattern") ? n : NULL;
    }
    return NULL;
}

void svgx_sample(const uint8_t* px, int w, int h, double u, double v, bool nearest, float out[4])
{
    if(nearest) {
        int x = (int)floor(u), y = (int)floor(v);
        if(x < 0 || y < 0 || x >= w || y >= h) {
            out[0] = out[1] = out[2] = out[3] = 0;
            return;
        }
        for(int c = 0; c < 4; c++) {
            out[c] = px[((size_t)y * (size_t)w + (size_t)x) * 4 + c];
        }
        return;
    }
    double fx = u - 0.5, fy = v - 0.5;
    int x0 = (int)floor(fx), y0 = (int)floor(fy);
    double ax = fx - x0, ay = fy - y0;
    for(int c = 0; c < 4; c++) {
        out[c] = 0;
    }
    for(int j = 0; j < 2; j++) {
        for(int i = 0; i < 2; i++) {
            int x = x0 + i, y = y0 + j;
            if(x < 0 || y < 0 || x >= w || y >= h) {
                continue;
            }
            double wt = (i ? ax : 1 - ax) * (j ? ay : 1 - ay);
            const uint8_t* s = px + ((size_t)y * (size_t)w + (size_t)x) * 4;
            for(int c = 0; c < 4; c++) {
                out[c] += (float)(s[c] * wt);
            }
        }
    }
}

static void sample_wrap(const uint8_t* px, int w, int h, double u, double v, float out[4])
{
    double fx = u - 0.5, fy = v - 0.5;
    int x0 = (int)floor(fx), y0 = (int)floor(fy);
    double ax = fx - x0, ay = fy - y0;
    for(int c = 0; c < 4; c++) {
        out[c] = 0;
    }
    for(int j = 0; j < 2; j++) {
        for(int i = 0; i < 2; i++) {
            int x = ((x0 + i) % w + w) % w, y = ((y0 + j) % h + h) % h;
            double wt = (i ? ax : 1 - ax) * (j ? ay : 1 - ay);
            const uint8_t* s = px + ((size_t)y * (size_t)w + (size_t)x) * 4;
            for(int c = 0; c < 4; c++) {
                out[c] += (float)(s[c] * wt);
            }
        }
    }
}

static uint8_t* render_pattern(struct svgx* d, int ui, int w, int h, float scale)
{
    unit* u = &d->u[ui];
    xn* p = svgx_find(d, u->pat);
    if(!p) {
        return NULL;
    }
    svgx_rc rc = { d, ui, w, h, scale };
    const char *pu = pat_attr(d, p, "patternUnits"), *cu = pat_attr(d, p, "patternContentUnits");
    bool obb = !(pu && strcmp(pu, "userSpaceOnUse") == 0), cobb = cu && strcmp(cu, "objectBoundingBox") == 0;
    float bb[4] = { 0, 0, 0, 0 };
    bool has_bb = svgx_bbox(&rc, bb);
    if((obb || cobb) && (!has_bb || bb[2] <= 0 || bb[3] <= 0)) {
        return NULL;
    }
    float r[4];
    static const char* const names[4] = { "x", "y", "width", "height" };
    for(int i = 0; i < 4; i++) {
        const char* v = pat_attr(d, p, names[i]);
        if(obb) {
            float f = 0;
            if(v) {
                char* end;
                f = strtof(v, &end);
                if(*end == '%') {
                    f /= 100.f;
                }
            }
            r[i] = i == 0 ? bb[0] + f * bb[2] : i == 1 ? bb[1] + f * bb[3]
                                                       : f * (i == 2 ? bb[2] : bb[3]);
        } else {
            r[i] = svgx_length(v, 0, svgx_viewport_diag(&rc, i % 2));
        }
    }
    if(!(r[2] > 0) || !(r[3] > 0)) {
        return NULL;
    }
    svgx_m2 dev = svgx_device(&rc), pt = svgx_transform(pat_attr(d, p, "patternTransform"));
    svgx_m2 mp = svgx_mul(dev, pt); /* pattern space -> device */
    double sx = hypot(mp.a, mp.b), sy = hypot(mp.c, mp.d);
    int tw = (int)ceil(r[2] * sx - 1e-6), th = (int)ceil(r[3] * sy - 1e-6);
    if(tw < 1) {
        tw = 1;
    }
    if(th < 1) {
        th = 1;
    }
    while((double)tw * th > MAX_PAT_TILE) {
        tw = (tw + 1) / 2;
        th = (th + 1) / 2;
    }
    /* the tile, drawn by everything this file does */
    xn* content = pat_content(d, p);
    sbuf b = { 0 };
    sb_fmt(&b, "<svg xmlns='http://www.w3.org/2000/svg' width='%d' height='%d' viewBox='0 0 %d %d'>", tw, th, tw, th);
    sb_str(&b, all_defs(d));
    sb_fmt(&b, "<g transform='scale(%g %g)'>", tw / (double)r[2], th / (double)r[3]);
    float vb[4];
    const char* vbs = pat_attr(d, p, "viewBox");
    if(vbs && sscanf(vbs, "%f%*[ ,]%f%*[ ,]%f%*[ ,]%f", &vb[0], &vb[1], &vb[2], &vb[3]) == 4 && vb[2] > 0 && vb[3] > 0) {
        double fx, fy, ox, oy;
        bool sl;
        svgx_fit(pat_attr(d, p, "preserveAspectRatio"), vb[2], vb[3], 0, 0, r[2], r[3], &fx, &fy, &ox, &oy, &sl);
        sb_fmt(&b, "<g transform='translate(%g %g) scale(%g %g) translate(%g %g)'>", ox, oy, fx, fy, (double)-vb[0], (double)-vb[1]);
    } else if(cobb) { /* (the content's origin is the tile's: only the box's size applies) */
        sb_fmt(&b, "<g transform='scale(%g %g)'>", (double)bb[2], (double)bb[3]);
    } else {
        sb_str(&b, "<g>");
    }
    sb_str(&b, "<g");
    const char* inh = content ? x_attr(content, "\x01inh") : NULL;
    if(inh && *inh) {
        x_emit_attr(&b, "style", inh);
    }
    sb_str(&b, ">");
    emit_opt eo = { -1, NULL, NULL, NULL, false };
    if(content) {
        for(int i = 0; i < content->nk; i++) {
            x_emit(&b, content->kid[i], &eo);
        }
    }
    sb_str(&b, "</g></g></g></svg>");
    uint8_t* tile = render_sub(d, b.p, tw, th, 1.f);
    free(b.p);
    if(!tile) {
        return NULL;
    }
    /* the shape's coverage */
    b = (sbuf){ 0 };
    emit_opt co = { ui, u->el, u->ovr, NULL, false };
    int keep = u->el->unit;
    u->el->unit = ui;
    x_emit(&b, d->root, &co);
    u->el->unit = keep;
    uint8_t* cov = gp_svg_nsvg_raster(b.p, w, h, scale);
    free(b.p);
    if(!cov) {
        free(tile);
        return NULL;
    }
    svgx_m2 inv = svgx_inverse(mp);
    uint8_t* out = calloc((size_t)w * (size_t)h, 4);
    if(!out) {
        abort();
    }
    for(int y = 0; y < h; y++) {
        for(int x = 0; x < w; x++) {
            size_t i = (size_t)y * (size_t)w + (size_t)x;
            unsigned a = cov[i * 4 + 3];
            if(!a) {
                continue;
            }
            double qx, qy;
            m_apply(inv, x + 0.5, y + 0.5, &qx, &qy);
            float c[4];
            sample_wrap(tile, tw, th, (qx - r[0]) / r[2] * tw, (qy - r[1]) / r[3] * th, c);
            for(int k = 0; k < 4; k++) {
                out[i * 4 + k] = (uint8_t)fminf(c[k] * (float)a / 255.f + 0.5f, 255.f);
            }
        }
    }
    free(cov);
    free(tile);
    return out;
}

/* ---------------------------------------------------------------- images */

static int b64v(int c)
{
    if(c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if(c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if(c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if(c == '+' || c == '-') {
        return 62;
    }
    if(c == '/' || c == '_') {
        return 63;
    }
    return -1;
}

static uint8_t* data_url(const char* href, size_t* len, bool* svg)
{
    if(strncmp(href, "data:", 5) != 0) {
        return NULL;
    }
    const char* comma = strchr(href, ',');
    if(!comma) {
        return NULL;
    }
    bool b64 = false;
    *svg = false;
    for(const char* p = href + 5; p < comma; p++) {
        if(strncmp(p, ";base64", 7) == 0) {
            b64 = true;
        }
        if(strncmp(p, "svg", 3) == 0) {
            *svg = true;
        }
    }
    const char* s = comma + 1;
    size_t n = strlen(s);
    uint8_t* out = malloc(n + 1);
    if(!out) {
        abort();
    }
    size_t o = 0;
    if(b64) {
        unsigned acc = 0;
        int bits = 0;
        for(size_t i = 0; i < n; i++) {
            int v = b64v((unsigned char)s[i]);
            if(v < 0) {
                continue;
            }
            acc = acc << 6 | (unsigned)v;
            bits += 6;
            if(bits >= 8) {
                bits -= 8;
                out[o++] = (uint8_t)(acc >> bits & 255);
            }
        }
    } else {
        for(size_t i = 0; i < n; i++) {
            if(s[i] == '%' && i + 2 < n && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
                char h[3] = { s[i + 1], s[i + 2], 0 };
                out[o++] = (uint8_t)strtoul(h, NULL, 16);
                i += 2;
            } else {
                out[o++] = (uint8_t)s[i];
            }
        }
    }
    out[o] = 0;
    *len = o;
    if(!*svg && o > 4 && (memcmp(out, "<svg", 4) == 0 || memcmp(out, "<?xm", 4) == 0)) {
        *svg = true;
    }
    return out;
}

bool svgx_load_href(svgx_rc* rc, const char* href, float device_scale, svgx_pic* out)
{
    memset(out, 0, sizeof *out);
    if(!href) {
        return false;
    }
    size_t len;
    bool svg;
    uint8_t* data = data_url(href, &len, &svg);
    if(!data) {
        return false;
    }
    bool ok = false;
    if(svg) {
        float iw, ih;
        if(gp_svg_probe((const char*)data, len, &iw, &ih) && !(device_scale > 0)) { /* (only its size) */
            out->iw = iw;
            out->ih = ih;
            ok = true;
        } else if(gp_svg_probe((const char*)data, len, &iw, &ih)) {
            float s = device_scale;
            int pw = (int)ceilf(iw * s), ph = (int)ceilf(ih * s);
            if(pw >= 1 && ph >= 1 && (double)pw * ph <= 4096.0 * 4096.0) {
                out->px = gp_svgx_render_doc((const char*)data, len, pw, ph, (float)pw / iw, rc->d->ts, rc->d->depth + 1);
                out->pw = pw;
                out->ph = ph;
                out->iw = iw;
                out->ih = ih;
                ok = out->px != NULL;
            }
        }
    } else {
        int w, h;
        out->px = gp_image_decode(data, len, &w, &h);
        if(out->px) {
            out->pw = w;
            out->ph = h;
            out->iw = (float)w;
            out->ih = (float)h;
            ok = true;
        }
    }
    free(data);
    return ok;
}

static uint8_t* render_image(struct svgx* d, int ui, int w, int h, float scale)
{
    unit* u = &d->u[ui];
    xn* el = u->el;
    svgx_rc rc = { d, ui, w, h, scale };
    svgx_m2 dev = svgx_device(&rc);
    double ds = sqrt(fabs(dev.a * dev.d - dev.b * dev.c));
    float vx = svgx_length(x_attr(el, "x"), 0, d->vw), vy = svgx_length(x_attr(el, "y"), 0, d->vh);
    const char *ws = x_attr(el, "width"), *hs = x_attr(el, "height");
    float vw = svgx_length(ws, -1, d->vw), vh = svgx_length(hs, -1, d->vh);
    svgx_pic pic;
    if(!svgx_load_href(&rc, href_raw(el), 0, &pic)) {
        return NULL; /* (an SVG: only its size, for now) */
    }
    if(!(vw >= 0) && !(vh >= 0)) {
        vw = pic.iw;
        vh = pic.ih;
    } else if(!(vw >= 0)) {
        vw = vh * pic.iw / pic.ih;
    } else if(!(vh >= 0)) {
        vh = vw * pic.ih / pic.iw;
    }
    if(!(vw > 0) || !(vh > 0)) {
        free(pic.px);
        return NULL;
    }
    double sx, sy, tx, ty;
    bool slice;
    svgx_fit(x_attr(el, "preserveAspectRatio"), pic.iw, pic.ih, vx, vy, vw, vh, &sx, &sy, &tx, &ty, &slice);
    if(!pic.px && !svgx_load_href(&rc, href_raw(el), (float)(ds * fmax(sx, sy)), &pic)) {
        return NULL; /* an SVG, drawn at the size it is shown */
    }
    char ob[32], rb[32];
    const char* ov = svgx_prop(el, "overflow", ob, sizeof ob);
    bool clip_vp = !(ov && (strcmp(ov, "visible") == 0 || strcmp(ov, "auto") == 0));
    /* the drawn rectangle: the picture's place, cut to the viewport (unless overflow is visible) */
    double rx0 = tx, ry0 = ty, rx1 = tx + pic.iw * sx, ry1 = ty + pic.ih * sy;
    if(clip_vp) {
        rx0 = fmax(rx0, vx);
        ry0 = fmax(ry0, vy);
        rx1 = fmin(rx1, vx + vw);
        ry1 = fmin(ry1, vy + vh);
    }
    if(rx1 <= rx0 || ry1 <= ry0) {
        free(pic.px);
        return NULL;
    }
    const char* ir = inherited(el, "image-rendering", rb, sizeof rb);
    bool nearest = ir && (strcmp(ir, "pixelated") == 0 || strcmp(ir, "optimizeSpeed") == 0 || strcmp(ir, "crisp-edges") == 0);
    char opb[32];
    const char* ops = svgx_prop(el, "opacity", opb, sizeof opb);
    float op = ops ? fminf(fmaxf(strtof(ops, NULL), 0), 1) : 1;
    /* device box */
    double cx[4], cy[4];
    m_apply(dev, rx0, ry0, &cx[0], &cy[0]);
    m_apply(dev, rx1, ry0, &cx[1], &cy[1]);
    m_apply(dev, rx1, ry1, &cx[2], &cy[2]);
    m_apply(dev, rx0, ry1, &cx[3], &cy[3]);
    double bx0 = fmin(fmin(cx[0], cx[1]), fmin(cx[2], cx[3])), bx1 = fmax(fmax(cx[0], cx[1]), fmax(cx[2], cx[3]));
    double by0 = fmin(fmin(cy[0], cy[1]), fmin(cy[2], cy[3])), by1 = fmax(fmax(cy[0], cy[1]), fmax(cy[2], cy[3]));
    int X0 = (int)fmax(floor(bx0), 0), X1 = (int)fmin(ceil(bx1), w), Y0 = (int)fmax(floor(by0), 0), Y1 = (int)fmin(ceil(by1), h);
    svgx_m2 inv = svgx_inverse(dev);
    uint8_t* out = calloc((size_t)w * (size_t)h, 4);
    if(!out) {
        abort();
    }
    double kx = pic.pw / (pic.iw * sx), ky = pic.ph / (pic.ih * sy); /* user units -> picture pixels */
    for(int y = Y0; y < Y1; y++) {
        for(int x = X0; x < X1; x++) {
            /* coverage of the rectangle: 4 x 4 samples in the pixel */
            int in = 0;
            for(int sj = 0; sj < 4; sj++) {
                for(int si = 0; si < 4; si++) {
                    double ux, uy;
                    m_apply(inv, x + (si + 0.5) / 4, y + (sj + 0.5) / 4, &ux, &uy);
                    if(ux >= rx0 && ux < rx1 && uy >= ry0 && uy < ry1) {
                        in++;
                    }
                }
            }
            if(!in) {
                continue;
            }
            double ux, uy;
            m_apply(inv, x + 0.5, y + 0.5, &ux, &uy);
            double pu = (ux - tx) * kx, pv = (uy - ty) * ky;
            pu = fmin(fmax(pu, 0.5), pic.pw - 0.5); /* (the picture's edge pixels reach its edge) */
            pv = fmin(fmax(pv, 0.5), pic.ph - 0.5);
            float c[4];
            svgx_sample(pic.px, pic.pw, pic.ph, nearest ? (ux - tx) * kx : pu, nearest ? (uy - ty) * ky : pv, nearest, c);
            float k = (float)in / 16.f * op;
            uint8_t* o = out + ((size_t)y * (size_t)w + (size_t)x) * 4;
            for(int q = 0; q < 4; q++) {
                o[q] = (uint8_t)fminf(c[q] * k + 0.5f, 255.f);
            }
        }
    }
    free(pic.px);
    return out;
}

/* ---------------------------------------------------------------- drawing the layers */

static uint8_t* render_children(struct svgx* d, int parent, int w, int h, float scale);

static uint8_t* render_unit(struct svgx* d, int ui, int w, int h, float scale)
{
    unit* u = &d->u[ui];
    size_t n = (size_t)w * (size_t)h;
    switch(u->kind) {
        case U_PLAIN: {
            sbuf b = { 0 };
            emit_opt eo = { ui, NULL, NULL, NULL, false };
            x_emit(&b, d->root, &eo);
            uint8_t* px = gp_svg_nsvg_raster(b.p, w, h, scale);
            free(b.p);
            if(px) {
                premultiply(px, n);
            }
            return px;
        }
        case U_PART: {
            sbuf b = { 0 };
            emit_opt eo = { ui, u->el, u->ovr, NULL, false };
            int keep = u->el->unit;
            u->el->unit = ui;
            x_emit(&b, d->root, &eo);
            u->el->unit = keep;
            uint8_t* px = gp_svg_nsvg_raster(b.p, w, h, scale);
            free(b.p);
            if(px) {
                premultiply(px, n);
            }
            return px;
        }
        case U_PAT:
            return render_pattern(d, ui, w, h, scale);
        case U_IMAGE:
            return render_image(d, ui, w, h, scale);
        case U_GROUP: {
            uint8_t* buf = render_children(d, ui, w, h, scale);
            svgx_rc rc = { d, ui, w, h, scale };
            for(int i = 0; i < u->nf; i++) {
                xn* f = svgx_find(d, u->filters[i]);
                if(!f) {
                    continue;
                }
                uint8_t* res = svgx_filter(&rc, f, buf);
                if(res) {
                    free(buf);
                    buf = res;
                }
            }
            if(u->clip) {
                apply_clip(d, &rc, buf);
            }
            if(u->mask) {
                apply_mask(d, &rc, buf);
            }
            if(u->opacity < 1) {
                for(size_t p = 0; p < n * 4; p++) {
                    buf[p] = (uint8_t)((float)buf[p] * u->opacity + 0.5f);
                }
            }
            return buf;
        }
    }
    return NULL;
}

static uint8_t* render_children(struct svgx* d, int parent, int w, int h, float scale)
{
    size_t n = (size_t)w * (size_t)h;
    uint8_t* buf = calloc(n, 4);
    if(!buf) {
        abort();
    }
    for(int c = d->first[parent < 0 ? d->n : parent]; c >= 0; c = d->next[c]) {
        uint8_t* px = render_unit(d, c, w, h, scale);
        if(px) {
            over(buf, px, n);
        }
        free(px);
    }
    return buf;
}

/* ---------------------------------------------------------------- the document */

static bool has_word(const char* s, size_t len, const char* w)
{
    size_t n = strlen(w);
    for(size_t i = 0; i + n <= len; i++) {
        if(memcmp(s + i, w, n) == 0) {
            return true;
        }
    }
    return false;
}
/* `opacity` as a property of its own (not fill-opacity, stroke-opacity, ...) */
static bool has_opacity(const char* s, size_t len)
{
    for(size_t i = 0; i + 7 <= len; i++) {
        if(memcmp(s + i, "opacity", 7) == 0 && (i == 0 || (s[i - 1] != '-' && !isalnum((unsigned char)s[i - 1]))) && i + 7 < len && (s[i + 7] == '=' || s[i + 7] == ':' || s[i + 7] == ' ')) {
            return true;
        }
    }
    return false;
}

static bool needs_svgx(const char* src, size_t len, gp_text_system* ts)
{
    static const char* const words[] = { "<use", "clip-path", "mask", "<symbol", "<clipPath", "filter", "<pattern", "marker", "<image", "<style", "textPath" };
    if(ts && has_word(src, len, "<text")) {
        return true;
    }
    for(size_t i = 0; i < sizeof words / sizeof *words; i++) {
        if(has_word(src, len, words[i])) {
            return true;
        }
    }
    return has_opacity(src, len);
}

/* takes what is not drawn out of the document (it would be drawn as plain shapes, or is found by id), noting what each inherits where it was */
static void detach(struct svgx* d, xn* x)
{
    for(int i = 0; i < x->nk;) {
        xn* c = x->kid[i];
        if(is(c, "clipPath") || is(c, "mask") || is(c, "filter") || is(c, "pattern") || is(c, "marker") || is(c, "symbol")) {
            char* inh = inherited_style(x, false);
            x_set(c, "\x01inh", inh);
            free(inh);
            x_remove_at(x, i);
            x_append(d->holder, c);
        } else {
            detach(d, c);
            i++;
        }
    }
}

/* every layer writes the whole document again with some of its shapes: the root must place them the same way each time, whatever it holds, so its
 * viewBox, width and height are written out */
static void normalize_root(struct svgx* d)
{
    xn* r = d->root;
    float vb[4];
    const char* vbs = x_attr(r, "viewBox");
    bool has_vb = vbs && sscanf(vbs, "%f%*[ ,]%f%*[ ,]%f%*[ ,]%f", &vb[0], &vb[1], &vb[2], &vb[3]) == 4 && vb[2] > 0 && vb[3] > 0;
    sbuf b = { 0 };
    sb_str(&b, "<svg");
    for(int i = 0; i < r->na; i++) {
        if(r->k[i][0] != '\x01') {
            x_emit_attr(&b, r->k[i], r->v[i]);
        }
    }
    sb_str(&b, "/>");
    float size[2] = { 0, 0 };
    gp_svg_nsvg_info(b.p, false, NULL, size, NULL);
    free(b.p);
    bool hw = x_attr(r, "width") && size[0] > 0, hh = x_attr(r, "height") && size[1] > 0;
    if(!has_vb) {
        float raw[4] = { 0, 0, 0, 0 };
        if(!hw || !hh) { /* nanosvg puts a drawing without a size where its shapes are */
            sbuf a = { 0 };
            sb_str(&a, ROOT1);
            emit_opt eo = { -1, NULL, NULL, NULL, false };
            for(int i = 0; i < r->nk; i++) {
                x_emit(&a, r->kid[i], &eo);
            }
            sb_str(&a, "</svg>");
            gp_svg_nsvg_info(a.p, false, raw, NULL, NULL);
            free(a.p);
        }
        vb[0] = hw ? 0 : raw[0];
        vb[2] = hw ? size[0] : raw[2] - raw[0];
        vb[1] = hh ? 0 : raw[1];
        vb[3] = hh ? size[1] : raw[3] - raw[1];
        if(!hw) {
            size[0] = vb[2];
        }
        if(!hh) {
            size[1] = vb[3];
        }
        if(vb[2] > 0 && vb[3] > 0) {
            char s[128];
            snprintf(s, sizeof s, "%g %g %g %g", (double)vb[0], (double)vb[1], (double)vb[2], (double)vb[3]);
            x_set(r, "viewBox", s);
            has_vb = true;
        }
    }
    if(size[0] > 0 && size[1] > 0) {
        char s[64];
        snprintf(s, sizeof s, "%g", (double)size[0]);
        x_set(r, "width", s);
        snprintf(s, sizeof s, "%g", (double)size[1]);
        x_set(r, "height", s);
    }
    d->vw = has_vb ? vb[2] : size[0];
    d->vh = has_vb ? vb[3] : size[1];
    if(!(d->vw > 0)) {
        d->vw = 100;
    }
    if(!(d->vh > 0)) {
        d->vh = 100;
    }
    sbuf o = { 0 };
    sb_str(&o, "<svg");
    for(int i = 0; i < r->na; i++) {
        if(r->k[i][0] != '\x01') {
            x_emit_attr(&o, r->k[i], r->v[i]);
        }
    }
    if(!x_attr(r, "xmlns")) {
        x_emit_attr(&o, "xmlns", "http://www.w3.org/2000/svg");
    }
    sb_str(&o, ">");
    d->root_open = o.p;
}

static void svgx_free(struct svgx* d)
{
    if(!d) {
        return;
    }
    for(int i = 0; i < d->n; i++) {
        free(d->u[i].clip);
        free(d->u[i].mask);
        for(int k = 0; k < d->u[i].nf; k++) {
            free(d->u[i].filters[k]);
        }
        free(d->u[i].pat);
    }
    free(d->u);
    free(d->first);
    free(d->next);
    x_free(d->root);
    x_free(d->holder);
    free(d->defs);
    free(d->root_open);
    free(d);
}

uint8_t* gp_svgx_render_doc(const char* doc, size_t len, int w, int h, float scale, gp_text_system* ts, int depth)
{
    if(w <= 0 || h <= 0) {
        return NULL;
    }
    size_t n = (size_t)w * (size_t)h;
    if(depth > MAX_DEPTH) {
        return calloc(n, 4);
    }
    if(!needs_svgx(doc, len, ts)) {
        char* copy = dupn(doc, len);
        uint8_t* px = gp_svg_nsvg_raster(copy, w, h, scale);
        free(copy);
        if(px) {
            premultiply(px, n);
        }
        return px;
    }
    xn* root = x_parse(doc, len);
    if(!root || !is(root, "svg")) {
        x_free(root);
        char* copy = dupn(doc, len);
        uint8_t* px = gp_svg_nsvg_raster(copy, w, h, scale);
        free(copy);
        if(px) {
            premultiply(px, n);
        }
        return px;
    }
    struct svgx* d = calloc(1, sizeof *d);
    if(!d) {
        abort();
    }
    d->root = root;
    d->holder = xn_new("defs", 4, NULL);
    d->ts = ts;
    d->depth = depth;
    apply_stylesheets(root);
    int budget = 2000; /* copies of what is pointed at: a document that points at itself, or doubles at every step, stops here */
    expand_uses(root, root, 0, &budget);
    mk_ctx mc = { root, 0, 4000 };
    expand_markers(&mc, root);
    normalize_root(d);
    if(ts) {
        convert_texts(root, root, ts, d->vw, d->vh);
    }
    detach(d, root);
    if(!hidden(root)) {
        /* the root's own effects make a layer of everything */
        assign(d, root, -1);
    }
    d->first = malloc((size_t)(d->n + 1) * sizeof *d->first);
    d->next = malloc((size_t)(d->n + 1) * sizeof *d->next);
    if(!d->first || !d->next) {
        abort();
    }
    for(int i = 0; i <= d->n; i++) {
        d->first[i] = -1;
        d->next[i] = -1;
    }
    for(int i = d->n - 1; i >= 0; i--) { /* (backwards, so each list is in drawing order) */
        int p = d->u[i].parent < 0 ? d->n : d->u[i].parent;
        d->next[i] = d->first[p];
        d->first[p] = i;
    }
    uint8_t* px = render_children(d, -1, w, h, scale);
    svgx_free(d);
    return px;
}

/* ---------------------------------------------------------------- glyphs of SVG fonts (OpenType 'SVG ') */

static bool name_char(char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '-';
}

/* The text color in a value, in place (each replacement is shorter than what it replaces): currentColor and context-fill are it, context-stroke is none (text
 * is not stroked here). True if there was any. */
static bool text_color_in(char* d, uint32_t rgb)
{
    static const char* const keys[] = { "currentColor", "context-fill", "context-stroke" };
    char hex[8];
    snprintf(hex, sizeof hex, "#%06x", (unsigned)(rgb & 0xffffff));
    bool any = false;
    size_t o = 0;
    for(size_t i = 0; d[i];) {
        int k = 0;
        bool starts = !i || !(name_char(d[i - 1]) || d[i - 1] == '#'); /* (a value, not part of a name: an id, a url(#...)) */
        while(k < 3 && !(starts && strncasecmp(d + i, keys[k], strlen(keys[k])) == 0 && !name_char(d[i + strlen(keys[k])]))) {
            k++;
        }
        if(k == 3) {
            d[o++] = d[i++];
            continue;
        }
        const char* to = k == 2 ? "none" : hex;
        memcpy(d + o, to, strlen(to));
        o += strlen(to);
        i += strlen(keys[k]);
        any = true;
    }
    d[o] = 0;
    return any;
}

/* ... in every value of the document but the names (id, href) and in what <style> holds */
static bool text_color_tree(xn* x, uint32_t rgb)
{
    bool any = x->text && text_color_in(x->text, rgb);
    for(int i = 0; i < x->na; i++) {
        if(strcmp(x->k[i], "id") != 0 && !strstr(x->k[i], "href")) {
            any |= text_color_in(x->v[i], rgb);
        }
    }
    for(int i = 0; i < x->nk; i++) {
        any |= text_color_tree(x->kid[i], rgb);
    }
    return any;
}

/* The document of one glyph: the element with id glyph<gid> (the root itself, or something in it); the rest of what is drawn is moved into <defs>,
 * where what the glyph points at can still be found. The root's size is replaced by `root_attrs`. With `used_fg`, the text color `rgb` is put in (and
 * *used_fg says whether it was asked for). NULL if there is no such glyph. */
static char* glyph_doc(const char* doc, size_t len, unsigned gid, const char* root_attrs, bool expand, uint32_t rgb, bool* used_fg)
{
    xn* root = x_parse(doc, len);
    if(!root || !is(root, "svg")) {
        x_free(root);
        return NULL;
    }
    if(used_fg) {
        *used_fg = text_color_tree(root, rgb);
    }
    char id[32];
    snprintf(id, sizeof id, "glyph%u", gid);
    xn* g = find_id(root, id);
    if(!g) {
        x_free(root);
        return NULL;
    }
    if(g != root) {
        xn* defs = xn_new("defs", 4, NULL);
        for(xn* keep = g; keep != root; keep = keep->parent) {
            xn* p = keep->parent;
            for(int i = 0; i < p->nk;) {
                xn* c = p->kid[i];
                if(c == keep || is_unrendered(c) || c->name[0] == '#') {
                    i++;
                    continue;
                }
                x_remove_at(p, i);
                x_append(defs, c);
            }
        }
        x_insert(root, 0, defs);
    }
    if(expand) { /* (for the box: what nanosvg cannot see, made into what it can) */
        apply_stylesheets(root);
        int budget = 2000;
        expand_uses(root, root, 0, &budget);
    }
    sbuf b = { 0 };
    sb_str(&b, "<svg");
    for(int i = 0; i < root->na; i++) {
        if(strcmp(root->k[i], "width") && strcmp(root->k[i], "height") && strcmp(root->k[i], "viewBox") && strcmp(root->k[i], "preserveAspectRatio")) {
            x_emit_attr(&b, root->k[i], root->v[i]);
        }
    }
    sb_str(&b, " ");
    sb_str(&b, root_attrs);
    sb_str(&b, ">");
    emit_opt eo = { -1, NULL, NULL, NULL, false };
    for(int i = 0; i < root->nk; i++) {
        x_emit(&b, root->kid[i], &eo);
    }
    sb_str(&b, "</svg>");
    x_free(root);
    return b.p;
}

/* The box of glyph `gid` in pixels at `sx` x `sy` pixels per font unit (y down, the origin on the baseline): left, top, right, bottom. */
bool gp_svgx_glyph_box(const char* doc, size_t len, unsigned gid, float sx, float sy, float box[4])
{
    char* d = glyph_doc(doc, len, gid, "width='1' height='1' viewBox='0 0 1 1'", true, 0, NULL);
    if(!d) {
        return false;
    }
    float b[4];
    int n = gp_svg_nsvg_info(d, true, b, NULL, NULL);
    free(d);
    if(n <= 0) {
        return false;
    }
    box[0] = floorf(b[0] * sx) - 1; /* (a pixel more each side: antialiasing) */
    box[1] = floorf(b[1] * sy) - 1;
    box[2] = ceilf(b[2] * sx) + 1;
    box[3] = ceilf(b[3] * sy) + 1;
    return box[2] > box[0] && box[3] > box[1];
}

/* Glyph `gid` drawn into a w x h picture whose top left is (left, top) in the pixels of gp_svgx_glyph_box, with `rgb` (0xrrggbb) as the text color (*used_fg:
 * whether the glyph has any): premultiplied RGBA, or NULL. */
uint8_t* gp_svgx_glyph_render(const char* doc, size_t len, unsigned gid, float sx, float sy, int left, int top, int w, int h, uint32_t rgb, bool* used_fg)
{
    *used_fg = false;
    if(w <= 0 || h <= 0 || !(sx > 0) || !(sy > 0)) {
        return NULL;
    }
    char attrs[256];
    snprintf(attrs, sizeof attrs, "width='%d' height='%d' viewBox='%g %g %g %g' preserveAspectRatio='none'", w, h, left / (double)sx, top / (double)sy, w / (double)sx, h / (double)sy);
    char* d = glyph_doc(doc, len, gid, attrs, false, rgb, used_fg);
    if(!d) {
        return NULL;
    }
    uint8_t* px = gp_svgx_render_doc(d, strlen(d), w, h, 1.f, NULL, 0);
    free(d);
    return px;
}
