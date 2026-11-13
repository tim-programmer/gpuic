/*
 * Linked into every test on Windows: a crash or an abort() ends the test at once, with a line saying what happened,
 * and so does a test that runs too long, with where each of its threads is.
 *
 * Without this a crashing test hangs until ctest's timeout: Windows Error Reporting (or Wine's debugger) holds the
 * process open, and stdout, fully buffered when it is a pipe, is lost with it, so nothing says which check failed.
 * It runs before main, from the C runtime's initializer table.
 *
 * A crash prints its stack, with names and lines when the test's .pdb is there (the workflow builds one).
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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* dbghelp, loaded when first needed (no link dependency): names and lines where a .pdb is found */
typedef struct { ULONG SizeOfStruct, TypeIndex; ULONG64 Reserved[2]; ULONG Index, Size; ULONG64 ModBase; ULONG Flags;
                 ULONG64 Value, Address; ULONG Register, Scope, Tag, NameLen, MaxNameLen; CHAR Name[1]; } sym_info;
typedef struct { DWORD SizeOfStruct; PVOID Key; DWORD LineNumber; PCHAR FileName; DWORD64 Address; } sym_line;
static BOOL (WINAPI *p_SymInitialize)(HANDLE, PCSTR, BOOL);
static BOOL (WINAPI *p_SymFromAddr)(HANDLE, DWORD64, DWORD64 *, sym_info *);
static BOOL (WINAPI *p_SymGetLineFromAddr64)(HANDLE, DWORD64, DWORD *, sym_line *);
static bool sym_ready;

static void sym_init(void) {
    if (sym_ready) return;
    sym_ready = true;
    HMODULE d = LoadLibraryA("dbghelp.dll");
    if (!d) return;
    p_SymInitialize = (BOOL (WINAPI *)(HANDLE, PCSTR, BOOL))(void (*)(void))GetProcAddress(d, "SymInitialize");
    p_SymFromAddr = (BOOL (WINAPI *)(HANDLE, DWORD64, DWORD64 *, sym_info *))(void (*)(void))GetProcAddress(d, "SymFromAddr");
    p_SymGetLineFromAddr64 = (BOOL (WINAPI *)(HANDLE, DWORD64, DWORD *, sym_line *))(void (*)(void))GetProcAddress(d, "SymGetLineFromAddr64");
    if (!p_SymInitialize || !p_SymInitialize(GetCurrentProcess(), NULL, TRUE)) p_SymFromAddr = NULL;
}

static void print_address(DWORD64 addr) {
    HMODULE m = NULL;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)addr, &m))
        GetModuleFileNameA(m, path, sizeof path);
    const char *name = strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
    fprintf(stderr, "    %s+0x%llx", name, (unsigned long long)(addr - (DWORD64)(uintptr_t)m));
    sym_init();
    union { sym_info s; char buf[sizeof(sym_info) + 256]; } si;
    memset(&si, 0, sizeof si);
    si.s.SizeOfStruct = sizeof(sym_info);
    si.s.MaxNameLen = 255;
    DWORD64 off = 0;
    if (p_SymFromAddr && p_SymFromAddr(GetCurrentProcess(), addr, &off, &si.s)) {
        fprintf(stderr, "  %s+0x%llx", si.s.Name, (unsigned long long)off);
        sym_line ln;
        memset(&ln, 0, sizeof ln);
        ln.SizeOfStruct = sizeof ln;
        DWORD disp = 0;
        if (p_SymGetLineFromAddr64 && p_SymGetLineFromAddr64(GetCurrentProcess(), addr - 1, &disp, &ln))
            fprintf(stderr, "  %s:%lu", ln.FileName, (unsigned long)ln.LineNumber);
    }
    fprintf(stderr, "\n");
}

/* Prints the stack from a context (consumed: it is unwound in place). */
static void print_stack(CONTEXT *ctx) {
#if defined(_M_X64) || defined(__x86_64__)
    for (int i = 0; i < 40 && ctx->Rip; i++) {
        print_address(ctx->Rip);
        DWORD64 base = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx->Rip, &base, NULL);
        if (f) {
            void *handler_data;
            DWORD64 frame;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx->Rip, f, ctx, &handler_data, &frame, NULL);
        } else {   /* a leaf: the return address is on top of the stack */
            ctx->Rip = *(DWORD64 *)(uintptr_t)ctx->Rsp;
            ctx->Rsp += 8;
        }
    }
#else
    (void)ctx;
#endif
}

static void print_here(void) {
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    print_stack(&ctx);
}

static void die(int code) {
    fflush(stdout);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), (UINT)code);
}

static LONG WINAPI on_crash(EXCEPTION_POINTERS *e) {
    const EXCEPTION_RECORD *r = e->ExceptionRecord;
    fprintf(stderr, "CRASH: exception 0x%08lx at %p", (unsigned long)r->ExceptionCode, r->ExceptionAddress);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
        fprintf(stderr, " (%s 0x%llx)", r->ExceptionInformation[0] == 8 ? "executing" : r->ExceptionInformation[0] ? "writing" : "reading",
                (unsigned long long)r->ExceptionInformation[1]);
    fprintf(stderr, "\n");
    CONTEXT ctx = *e->ContextRecord;
    print_stack(&ctx);
    die(3);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void on_abort(int sig) {
    (void)sig;
    fprintf(stderr, "CRASH: abort()\n");
    print_here();
    die(3);
}

#ifdef _MSC_VER
/* what the C runtime does with a bad argument (a NULL stream, a bad format...): by default a __fastfail, 0xc0000409, with no word */
static void on_invalid_parameter(const wchar_t *expr, const wchar_t *func, const wchar_t *file, unsigned line, uintptr_t reserved) {
    (void)reserved;
    fprintf(stderr, "CRASH: invalid parameter to the C runtime (%ls in %ls, %ls:%u)\n", expr ? expr : L"?", func ? func : L"?",
            file ? file : L"?", line);
    print_here();
    die(3);
}
static void on_purecall(void) {
    fprintf(stderr, "CRASH: pure virtual call\n");
    print_here();
    die(3);
}
#endif

static void print_thread(DWORD tid) {
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!t) return;
    fprintf(stderr, "  thread %lu:\n", (unsigned long)tid);
    if (SuspendThread(t) != (DWORD)-1) {
        CONTEXT ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(t, &ctx)) print_stack(&ctx);
        ResumeThread(t);
    }
    CloseHandle(t);
}

static DWORD WINAPI watchdog(void *arg) {
    Sleep((DWORD)(uintptr_t)arg * 1000);
    sym_init();   /* (before any thread is suspended: one of them may hold the heap lock that loading symbols needs) */
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
    _set_invalid_parameter_handler(on_invalid_parameter);
    _set_purecall_handler(on_purecall);
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
