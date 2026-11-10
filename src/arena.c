#include "internal.h"

#define GP_CHUNK_SIZE (256 * 1024)

struct gp_arena_chunk {
    gp_arena_chunk *next;
    size_t cap, used;
    _Alignas(16) unsigned char data[];
};

/* Holds. An arena's epoch is the counter's value when it last started from empty; a hold is the counter's value when it began. An arena reset or
 * freed while a hold younger than its epoch is on keeps its chunks aside, tagged with that epoch, until no hold needs them. */
typedef struct kept { gp_arena_chunk *chain; uint32_t epoch; struct kept *next; } kept;
#define MAX_HOLDS 16
#define POOL_MAX 32
static _Thread_local uint32_t g_epoch = 1;
static _Thread_local uint32_t g_holds[MAX_HOLDS];
static _Thread_local int g_nholds;
static _Thread_local kept *g_kept;
static _Thread_local gp_arena_chunk *g_pool;   /* chunks let go by a hold, for the next arena that needs one of their size */
static _Thread_local int g_npool;

static bool held(uint32_t epoch) {
    for (int i = 0; i < g_nholds; i++)
        if (epoch < g_holds[i]) return true;
    return false;
}

static void chain_free(gp_arena_chunk *c) {
    for (gp_arena_chunk *n; c; c = n) {
        n = c->next;
        if (g_npool < POOL_MAX) { c->next = g_pool; g_pool = c; g_npool++; }
        else free(c);
    }
}

/* the arena's chunks go aside if a hold needs them; true if they did (the arena is then empty) */
static bool keep(gp_arena *a) {
    if (!a->head || !g_nholds || !held(a->epoch)) return false;
    kept *k = malloc(sizeof *k);
    if (!k) abort();
    *k = (kept){ a->head, a->epoch, g_kept };
    g_kept = k;
    a->head = a->cur = NULL;
    return true;
}

uint32_t gp_arena_hold(void) {
    if (g_nholds == MAX_HOLDS) abort();
    g_holds[g_nholds++] = ++g_epoch;
    return g_epoch;
}

void gp_arena_release(uint32_t hold) {
    if (!hold) return;
    for (int i = 0; i < g_nholds; i++)
        if (g_holds[i] == hold) { g_holds[i] = g_holds[--g_nholds]; break; }
    for (kept **k = &g_kept; *k;) {
        if (held((*k)->epoch)) { k = &(*k)->next; continue; }
        kept *d = *k;
        *k = d->next;
        chain_free(d->chain);
        free(d);
    }
}

void gp_arena_holds_free(void) {
    if (g_nholds) return;   /* (something still holds: what is kept stays) */
    for (gp_arena_chunk *c = g_pool, *n; c; c = n) { n = c->next; free(c); }
    g_pool = NULL;
    g_npool = 0;
}

static gp_arena_chunk *chunk_new(size_t cap) {
    for (gp_arena_chunk **c = &g_pool; *c; c = &(*c)->next)
        if ((*c)->cap == cap) {
            gp_arena_chunk *n = *c;
            *c = n->next;
            g_npool--;
            return n;
        }
    gp_arena_chunk *n = malloc(sizeof *n + cap);
    if (!n) abort();
    n->cap = cap;
    return n;
}

void *gp_arena_alloc(gp_arena *a, size_t size, size_t align) {
    if (align == 0) align = 8;
    if (!a->head) a->epoch = g_epoch;
    for (;;) {
        gp_arena_chunk *c = a->cur;
        if (c) {
            uintptr_t base = (uintptr_t)c->data;
            uintptr_t p = (base + c->used + align - 1) & ~(uintptr_t)(align - 1);
            size_t end = (size_t)(p - base) + size;
            if (end <= c->cap) {
                c->used = end;
                return memset((void *)p, 0, size);
            }
            if (c->next) {
                a->cur = c->next;
                continue;
            }
        }
        size_t chunk = a->chunk_size ? a->chunk_size : GP_CHUNK_SIZE;
        size_t cap = size + align > chunk ? size + align : chunk;
        gp_arena_chunk *n = chunk_new(cap);
        n->next = NULL;
        n->used = 0;
        if (c) c->next = n; else a->head = n;
        a->cur = n;
    }
}

void gp_arena_reset(gp_arena *a) {
    if (keep(a)) return;
    a->epoch = g_epoch;
    for (gp_arena_chunk *c = a->head; c; c = c->next) c->used = 0;
    a->cur = a->head;
}

void gp_arena_free(gp_arena *a) {
    if (keep(a)) return;
    for (gp_arena_chunk *c = a->head, *n; c; c = n) {
        n = c->next;
        free(c);
    }
    a->head = a->cur = NULL;
}

char *gp_arena_strdup(gp_arena *a, const char *s) {
    size_t n = strlen(s) + 1;
    return memcpy(gp_arena_alloc(a, n, 1), s, n);
}
