/*
 * The few thread primitives the executor needs (a lock, a condition, joinable threads), over
 * pthreads or over Win32. Included only by the files that start threads: on Windows it brings
 * in <windows.h>.
 */
#ifndef GPUIC_THREADS_H
#define GPUIC_THREADS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

typedef void *(*gp_thread_fn)(void *arg);

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>

typedef SRWLOCK gp_mutex;
typedef CONDITION_VARIABLE gp_cond;
typedef HANDLE gp_thread;

static inline void gp_mutex_init(gp_mutex *m) { InitializeSRWLock(m); }
static inline void gp_mutex_destroy(gp_mutex *m) { (void)m; }
static inline void gp_mutex_lock(gp_mutex *m) { AcquireSRWLockExclusive(m); }
static inline void gp_mutex_unlock(gp_mutex *m) { ReleaseSRWLockExclusive(m); }

static inline void gp_cond_init(gp_cond *c) { InitializeConditionVariable(c); }
static inline void gp_cond_destroy(gp_cond *c) { (void)c; }
static inline void gp_cond_wait(gp_cond *c, gp_mutex *m) { SleepConditionVariableSRW(c, m, INFINITE, 0); }
static inline void gp_cond_signal(gp_cond *c) { WakeConditionVariable(c); }
static inline void gp_cond_broadcast(gp_cond *c) { WakeAllConditionVariable(c); }

typedef struct { gp_thread_fn fn; void *arg; } gp_thread_start_;

static unsigned __stdcall gp_thread_entry_(void *p) {
    gp_thread_start_ s = *(gp_thread_start_ *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}

static inline bool gp_thread_create(gp_thread *t, gp_thread_fn fn, void *arg) {
    gp_thread_start_ *s = (gp_thread_start_ *)malloc(sizeof *s);
    if (!s) return false;
    s->fn = fn;
    s->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0, gp_thread_entry_, s, 0, NULL);
    if (!h) {
        free(s);
        return false;
    }
    *t = (HANDLE)h;
    return true;
}

static inline void gp_thread_join(gp_thread t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

static inline int gp_cpu_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}

#else

#include <pthread.h>
#include <unistd.h>

typedef pthread_mutex_t gp_mutex;
typedef pthread_cond_t gp_cond;
typedef pthread_t gp_thread;

static inline void gp_mutex_init(gp_mutex *m) { pthread_mutex_init(m, NULL); }
static inline void gp_mutex_destroy(gp_mutex *m) { pthread_mutex_destroy(m); }
static inline void gp_mutex_lock(gp_mutex *m) { pthread_mutex_lock(m); }
static inline void gp_mutex_unlock(gp_mutex *m) { pthread_mutex_unlock(m); }

static inline void gp_cond_init(gp_cond *c) { pthread_cond_init(c, NULL); }
static inline void gp_cond_destroy(gp_cond *c) { pthread_cond_destroy(c); }
static inline void gp_cond_wait(gp_cond *c, gp_mutex *m) { pthread_cond_wait(c, m); }
static inline void gp_cond_signal(gp_cond *c) { pthread_cond_signal(c); }
static inline void gp_cond_broadcast(gp_cond *c) { pthread_cond_broadcast(c); }

static inline bool gp_thread_create(gp_thread *t, gp_thread_fn fn, void *arg) { return pthread_create(t, NULL, fn, arg) == 0; }
static inline void gp_thread_join(gp_thread t) { pthread_join(t, NULL); }

static inline int gp_cpu_count(void) { return (int)sysconf(_SC_NPROCESSORS_ONLN); }

#endif

#endif
