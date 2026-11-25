/*
 * For the tests that check a frame the caches give against the same frame painted in full: one way to drop the painted copies of cached views (all of them:
 * the views table is a hash table, its slots in use are anywhere) and to count what differs. GPUIC_CACHE_DUMP=1 prints where the pixels that differ are.
 */
#ifndef GPUIC_TEST_CACHE_CHECK_H
#define GPUIC_TEST_CACHE_CHECK_H

#include "../src/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void drop_painted_copies(gp_window* w)
{
    gp_vviews* t = &w->views;
    for(size_t i = 0; i < t->cap; i++) {
        t->v[i].chunk_valid = false;
    }
}

/* takes a frame as the caches give it, drops every painted copy, takes another (a full paint) and returns how many bytes differ by more than `tol` (-1: no
 * frame); `frame` captures the next frame, `between` (may be NULL) runs after the first */
static inline long cached_vs_full(gp_window* w, uint8_t* (*frame)(void), int tol, void (*between)(void))
{
    gp_app* app = w->app;
    uint8_t* a = frame();
    if(!a) {
        return -1;
    }
    size_t n = (size_t)app->capture_w * (size_t)app->capture_h * 3;
    uint8_t* copy = malloc(n);
    if(!copy) {
        return -1;
    }
    memcpy(copy, a, n);
    if(between) {
        between();
    }
    drop_painted_copies(w);
    uint8_t* b = frame();
    long diff = b ? 0 : -1;
    int x0 = app->capture_w, y0 = app->capture_h, x1 = -1, y1 = -1;
    for(size_t i = 0; b && i < n; i++) {
        if(abs((int)copy[i] - (int)b[i]) > tol) {
            diff++;
            int px = (int)(i / 3 % (size_t)app->capture_w), py = (int)(i / 3 / (size_t)app->capture_w);
            x0 = px < x0 ? px : x0, x1 = px > x1 ? px : x1, y0 = py < y0 ? py : y0, y1 = py > y1 ? py : y1;
        }
    }
    if(diff > 0 && getenv("GPUIC_CACHE_DUMP")) {
        fprintf(stderr, "cached vs full: %ld bytes differ, in x %d..%d y %d..%d\n", diff, x0, x1, y0, y1);
    }
    free(copy);
    return diff;
}

#endif
