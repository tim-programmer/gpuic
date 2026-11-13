/*
 * What the tests use that the Windows C library names differently or lacks, and two helpers for files. On Windows
 * the build includes this in every test (CMakeLists.txt), so `setenv` and `usleep` are simply there.
 */
#ifndef GPUIC_TEST_COMPAT_H
#define GPUIC_TEST_COMPAT_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <process.h>

#ifdef __cplusplus
extern "C"
#endif
__declspec(dllimport) void __stdcall Sleep(unsigned long ms);

static inline int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value);
}
static inline int unsetenv(const char *name) { return _putenv_s(name, ""); }
static inline int usleep(unsigned usec) { Sleep((usec + 999) / 1000); return 0; }
#else
#include <unistd.h>
#endif

static inline bool test_file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

/* Writes `len` bytes to a new file in the temporary directory and puts its name in `path`; false if that failed. */
static inline bool test_temp_file(char *path, size_t cap, const char *name, const void *bytes, size_t len) {
    static int serial;
#ifdef _WIN32
    const char *dir = getenv("TEMP") ? getenv("TEMP") : ".";
    int pid = _getpid();
#else
    const char *dir = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    int pid = (int)getpid();
#endif
    snprintf(path, cap, "%s/%s-%d-%d", dir, name, pid, ++serial);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(bytes, 1, len, f) == len;
    return fclose(f) == 0 && ok;
}

#endif
