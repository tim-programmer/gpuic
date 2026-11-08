#include "internal.h"

#define GP_CHUNK_SIZE (256 * 1024)

struct gp_arena_chunk {
    gp_arena_chunk *next;
    size_t cap, used;
    _Alignas(16) unsigned char data[];
};

void *gp_arena_alloc(gp_arena *a, size_t size, size_t align) {
    if (align == 0) align = 8;
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
        gp_arena_chunk *n = malloc(sizeof *n + cap);
        if (!n) abort();
        n->next = NULL;
        n->cap = cap;
        n->used = 0;
        if (c) c->next = n; else a->head = n;
        a->cur = n;
    }
}

void gp_arena_reset(gp_arena *a) {
    for (gp_arena_chunk *c = a->head; c; c = c->next) c->used = 0;
    a->cur = a->head;
}

void gp_arena_free(gp_arena *a) {
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
