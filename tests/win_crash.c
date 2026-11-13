/*
 * Linked into every test on Windows: a crash or an abort() ends the test at once, with a line saying what happened.
 *
 * Without this a crashing test hangs until ctest's timeout: Windows Error Reporting (or Wine's debugger) holds the
 * process open, and stdout, fully buffered when it is a pipe, is lost with it, so nothing says which check failed.
 * It runs before main, from the C runtime's initializer table.
 */
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

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

static void gp_test_crash_setup(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(on_crash);
    signal(SIGABRT, on_abort);
#ifdef _MSC_VER
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
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
