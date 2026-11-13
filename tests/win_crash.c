/*
 * Linked into every test on Windows: a crash or an abort() ends the test at once, with a line saying what happened,
 * and so does a test that runs too long, with where each of its threads is.
 *
 * Without this a crashing test hangs until ctest's timeout: Windows Error Reporting (or Wine's debugger) holds the
 * process open, and stdout, fully buffered when it is a pipe, is lost with it, so nothing says which check failed.
 * It runs before main, from the C runtime's initializer table.
 *
 * The watchdog: after GPUIC_TEST_WATCHDOG seconds (default 240, under ctest's 300; 0 turns it off) every thread's
 * stack is printed as module+offset (no symbols needed: x64 unwind data) and the test exits with 4.
 */
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static LONG WINAPI on_crash(EXCEPTION_POINTERS *e) {
    fprintf(stderr, "CRASH: exception 0x%08lx at %p\n", (unsigned long)e->ExceptionRecord->ExceptionCode,
            e->ExceptionRecord->ExceptionAddress);
    fflush(stdout);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void on_abort(int sig) {
    (void)sig;
    fprintf(stderr, "CRASH: abort()\n");
    fflush(stdout);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 3);
}

static void print_address(DWORD64 addr) {
    HMODULE m = NULL;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)addr, &m))
        GetModuleFileNameA(m, path, sizeof path);
    const char *name = strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
    fprintf(stderr, "    %s+0x%llx\n", name, (unsigned long long)(addr - (DWORD64)(uintptr_t)m));
}

static void print_thread(DWORD tid) {
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!t) return;
    fprintf(stderr, "  thread %lu:\n", (unsigned long)tid);
    if (SuspendThread(t) != (DWORD)-1) {
#if defined(_M_X64) || defined(__x86_64__)
        CONTEXT ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(t, &ctx)) {
            for (int i = 0; i < 40 && ctx.Rip; i++) {
                print_address(ctx.Rip);
                DWORD64 base = 0;
                PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
                if (f) {
                    void *handler_data;
                    DWORD64 frame;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, f, &ctx, &handler_data, &frame, NULL);
                } else {   /* a leaf: the return address is on top of the stack */
                    ctx.Rip = *(DWORD64 *)(uintptr_t)ctx.Rsp;
                    ctx.Rsp += 8;
                }
            }
        }
#endif
        ResumeThread(t);
    }
    CloseHandle(t);
}

static DWORD WINAPI watchdog(void *arg) {
    Sleep((DWORD)(uintptr_t)arg * 1000);
    fprintf(stderr, "TIMEOUT: still running after %lu s; the threads:\n", (unsigned long)(uintptr_t)arg);
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid && te.th32ThreadID != me) print_thread(te.th32ThreadID);
            te.dwSize = sizeof te;
        } while (Thread32Next(snap, &te));
    }
    fflush(stdout);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 4);
    return 0;
}

static void gp_test_crash_setup(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(on_crash);
    signal(SIGABRT, on_abort);
#ifdef _MSC_VER
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    const char *env = getenv("GPUIC_TEST_WATCHDOG");
    int seconds = env ? atoi(env) : 240;
    if (seconds > 0) {
        HANDLE t = CreateThread(NULL, 0, watchdog, (void *)(uintptr_t)seconds, 0, NULL);
        if (t) CloseHandle(t);
    }
}

#ifdef _MSC_VER
/* (C has no constructors: a pointer in .CRT$XCU is called by the runtime's startup; /include keeps the linker from dropping it) */
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) void (*gp_test_crash_setup_ptr)(void) = gp_test_crash_setup;
#pragma comment(linker, "/include:gp_test_crash_setup_ptr")
#else
__attribute__((constructor)) static void gp_test_crash_setup_ctor(void) { gp_test_crash_setup(); }
#endif

#endif
