// xbox_main.cpp -- Lus-X: 2Ship2Harkinian on the original Xbox, the entry point.
//
// mm.o2r (made on a PC from the player's ROM) and 2ship.o2r go beside default.xbe; the config and
// the saves are kept in T:, the title's storage on the hard disk. Everything 2Ship does then is its
// own main (mm/src/code/main.c), on libultraship's Xbox backends.
#include <exception>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xtl.h>

#include "lusx_debug.h"
#include "lusx_log.h"

extern "C" int SDL_main(int argc, char* argv[]); // 2Ship's main (main.c)

// ---- Logging (lusx_log.h). Until the config says whether it's on (LusxLog_Settle), lines are held here, in a buffer
// made for the start-up and freed once it's said; then sent with Logging on, dropped with it off.
#define HOLD_SIZE (16 * 1024)

volatile int gLusxLogOn = 1;
static volatile LONG sHoldLock;
static volatile int sHolding = 1;
static char* sHeld; // lines, each with its '\0'
static unsigned sHeldLength, sHeldLost;

static void HoldLock(void) {
    while (InterlockedExchange((LONG*)&sHoldLock, 1) != 0) {
        Sleep(0);
    }
}

static void HoldUnlock(void) {
    InterlockedExchange((LONG*)&sHoldLock, 0);
}

static void Send(const char* line) {
    OutputDebugStringA(line);
    LusxDebug_Line(line);
}

extern "C" void LusxLog_Send(const char* line) {
    if (!gLusxLogOn) {
        return;
    }
    if (sHolding) {
        HoldLock();
        if (sHolding) {
            const size_t length = strlen(line) + 1;
            if (sHeld == NULL) {
                sHeld = (char*)malloc(HOLD_SIZE);
            }
            if (sHeld != NULL && sHeldLength + length <= HOLD_SIZE) {
                memcpy(sHeld + sHeldLength, line, length);
                sHeldLength += (unsigned)length;
            } else {
                sHeldLost++;
            }
            HoldUnlock();
            return;
        }
        HoldUnlock();
    }
    Send(line);
}

extern "C" void LusxLog_Settle(int on) {
    char lost[80];

    HoldLock();
    if (sHolding) {
        if (on) {
            for (unsigned at = 0; at < sHeldLength; at += (unsigned)strlen(sHeld + at) + 1) {
                Send(sHeld + at);
            }
            if (sHeldLost != 0) {
                snprintf(lost, sizeof(lost), "lus-x: %u lines of the start-up lost, its buffer was full\n", sHeldLost);
                Send(lost);
            }
        }
        free(sHeld);
        sHeld = NULL;
        sHolding = 0;
    }
    gLusxLogOn = on;
    HoldUnlock();
}

// a line to the debug channel, and to the tester build's log file (unless Logging is off)
static void Out(const char* line) {
    LusxLog_Send(line);
}

static void Log(const char* fmt, ...) {
    char line[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    strcat(line, "\n");
    Out(line);
}

/* A crash report to the debug channel (UltraX's ux_host_win32.c): the exception, the registers, and
   every word on the stack that is an address in the title's image (most are the calls that led
   there). Addresses symbolize against build/Lus-X.exe:
     llvm-symbolizer --obj=build/Lus-X.exe 0x... */
#define XBE_BASE 0x10000u

static LONG Report(const EXCEPTION_RECORD* er, const CONTEXT* c) {
    static volatile LONG sOnce;
    DWORD image_end = XBE_BASE + *(const DWORD*)(XBE_BASE + 0x10C); /* the XBE header's SizeOfImage */
    const DWORD* sp = (const DWORD*)c->Esp;
    DWORD stack_top;
    char line[160];
    int i, n = 0, shown = 0;

    if (InterlockedExchange((LONG*)&sOnce, 1)) {
        return 0; /* EXCEPTION_CONTINUE_SEARCH: the XDK headers lack the name */
    }
    if (sHolding) { /* a crash before the config is read: the start-up's lines and this report go out */
        LusxLog_Settle(1);
    }
    Log("lus-x: exception 0x%08lX at 0x%08lX (%08lX %08lX)", (unsigned long)er->ExceptionCode, (unsigned long)c->Eip,
        (unsigned long)(er->NumberParameters > 0 ? er->ExceptionInformation[0] : 0),
        (unsigned long)(er->NumberParameters > 1 ? er->ExceptionInformation[1] : 0));
    Log("  eax %08lX ebx %08lX ecx %08lX edx %08lX esi %08lX edi %08lX ebp %08lX esp %08lX", c->Eax, c->Ebx, c->Ecx,
        c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    /* the stack ends at the TIB's StackBase */
    __asm__ volatile("movl %%fs:4, %0" : "=r"(stack_top));
    for (i = 0; i < 2048 && (DWORD)&sp[i] + 4 <= stack_top; i++) {
        if (sp[i] < XBE_BASE + 0x1000 || sp[i] >= image_end) {
            continue;
        }
        if (shown == 0) {
            n = snprintf(line, sizeof(line), "  stack:");
        }
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %08lX", (unsigned long)sp[i]);
        if (++shown == 12) {
            Log("%s", line);
            shown = 0;
        }
    }
    if (shown) {
        Log("%s", line);
    }
    LusxDebug_Crash((unsigned long)er->ExceptionCode, (unsigned long)c->Eip);
    return 0;
}

static LONG WINAPI CrashReport(struct _EXCEPTION_POINTERS* ep) {
    return Report(ep->ExceptionRecord, ep->ContextRecord);
}

/* The same report from a handler at the bottom of the game thread's own exception chain (fs:0). With xbWatson
   attached the debugger takes an exception nothing handled before the unhandled-exception filter runs, so testers'
   logs never had the stack. This handler runs first: it reports an error exception (not an unwind, not a debug
   print or breakpoint), then lets the search go on, so xbWatson (or the filter) still gets it as before. */
struct SehRecord {
    SehRecord* next;
    void* handler;
};

extern "C" int __cdecl LusXbox_SehHandler(EXCEPTION_RECORD* er, void* frame, CONTEXT* c, void* dispatcher) {
    (void)frame;
    (void)dispatcher;
    if ((er->ExceptionFlags & 6) == 0 && (er->ExceptionCode & 0xC0000000u) == 0xC0000000u) { // 6: unwinding
        Report(er, c);
    }
    return 1; // ExceptionContinueSearch
}

/* a C++ exception nothing caught: what it said, then the crash report's stack (an int 3) */
static void Terminate() {
    std::exception_ptr e = std::current_exception();
    if (sHolding) { /* before the config is read: the start-up's lines and this go out */
        LusxLog_Settle(1);
    }
    if (e) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& ex) {
            Log("lus-x: uncaught exception: %s", ex.what());
        } catch (...) {
            Log("lus-x: uncaught exception (not a std::exception)");
        }
    } else {
        Log("lus-x: std::terminate");
    }
    __builtin_trap();
}
extern "C" void LusXbox_GamepadInit(void);
extern "C" void LusXbox_GamepadWait(void);

/* whether every byte of [p, p + size) is committed, readable memory: a read of memory let go would be an
   access violation (audio_load.c's copies check their source with it) */
extern "C" int LusXbox_Readable(const void* p, unsigned long size) {
    const char* at = (const char*)p;
    const char* end = at + size;

    if (p == NULL) {
        return 0;
    }
    while (at < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(at, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
            return 0;
        }
        at = (const char*)mbi.BaseAddress + mbi.RegionSize;
    }
    return 1;
}

extern "C" void LusXbox_LogMemory(const char* when) {
    MEMORYSTATUS ms;
    char line[160];

    GlobalMemoryStatus(&ms);
    snprintf(line, sizeof(line), "lus-x: %s: %lu KB free of %lu KB\n", when, (unsigned long)(ms.dwAvailPhys >> 10),
             (unsigned long)(ms.dwTotalPhys >> 10));
    Out(line);
}

static LARGE_INTEGER sStart;

/* a step of the start-up: the time since main and the memory it left, so a log that stops says where,
   and how long and how much each step took */
extern "C" void LusXbox_Step(const char* step) {
    MEMORYSTATUS ms;
    LARGE_INTEGER now, f;
    char line[200];

    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&f);
    GlobalMemoryStatus(&ms);
    snprintf(line, sizeof(line), "lus-x: step %6.0f ms %6lu KB free: %s\n",
             (double)(now.QuadPart - sStart.QuadPart) * 1000.0 / (double)f.QuadPart,
             (unsigned long)(ms.dwAvailPhys >> 10), step);
    Out(line);
}

// The watchdog: where the game last said it was (LusXbox_Where), and frames shown (LusXbox_Presented).
// A log that stops without an exception is a hang; with no frame for 3 s, it says where, and again
// every 10 s it lasts.
static const char* volatile sWhere = "starting";
static volatile LONG sPresents;

extern "C" void LusXbox_Where(const char* where) {
    sWhere = where;
}

extern "C" void LusXbox_Presented(void) {
    InterlockedIncrement((LONG*)&sPresents);
}

static DWORD WINAPI Watchdog(LPVOID) {
    LONG last = -1;
    DWORD stalled = 0, reported = 0;

    for (;;) {
#if LUSX_DEBUG_TOOLS
        for (int i = 0; i < 5; i++) {
            LusxDebug_Tick(sWhere, sPresents); // up to 100 ms each
        }
#else
        Sleep(500);
#endif
        LONG now = sPresents;
        if (now != last || now == 0) {
            last = now;
            if (reported != 0) {
                LusxDebug_Resumed();
            }
            stalled = reported = 0;
            continue;
        }
        stalled += 500;
        if (stalled >= 3000 && (reported == 0 || stalled - reported >= 10000)) {
            MEMORYSTATUS ms;
            char line[200];
            GlobalMemoryStatus(&ms);
            snprintf(line, sizeof(line), "lus-x: watchdog: no frame for %lu s; last at: %s (%lu KB free)\n",
                     (unsigned long)(stalled / 1000), sWhere, (unsigned long)(ms.dwAvailPhys >> 10));
            Out(line);
            LusxDebug_Hang(sWhere, (unsigned long)(stalled / 1000));
            reported = stalled;
        }
    }
    return 0;
}

// SSE's flush-to-zero: denormals cost a Pentium III dearly, and the N64 flushed them too
static void FpuSetup(void) {
    unsigned int csr;

    __asm__ volatile("stmxcsr %0" : "=m"(csr));
    csr |= 0x8000;
    __asm__ volatile("ldmxcsr %0" : : "m"(csr));
}

extern "C" {
double gLusxCyclesPerMs = 733333.0; // rdtsc's rate, once MeasureCpu has run
}

// The CPU this is really on, as a log can't tell otherwise: a console's can be upgraded, and timings from one
// aren't a stock 733 MHz's. rdtsc against the timer, which doesn't move with the CPU; then the chip's id and
// its cache descriptors (cpuid 1 and 2).
static void MeasureCpu(void) {
    LARGE_INTEGER f, a, b;
    unsigned int id, r[4], unused[3];
    char line[120];

    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    const uint64_t began = __builtin_ia32_rdtsc();
    do {
        QueryPerformanceCounter(&b);
    } while ((b.QuadPart - a.QuadPart) * 50 < f.QuadPart); // 20 ms
    const uint64_t cycles = __builtin_ia32_rdtsc() - began;
    gLusxCyclesPerMs = (double)cycles * (double)f.QuadPart / ((double)(b.QuadPart - a.QuadPart) * 1000.0);
    __asm__ volatile("cpuid" : "=a"(id), "=b"(unused[0]), "=c"(unused[1]), "=d"(unused[2]) : "a"(1));
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(2));
    snprintf(line, sizeof(line), "lus-x: CPU: %.0f MHz, id 0x%08x, caches %08x %08x %08x %08x\n",
             gLusxCyclesPerMs / 1000.0, id, r[0], r[1], r[2], r[3]);
    Out(line);
}

int __cdecl main(void) {
    static char arg0[] = "D:\\default.xbe";
    static char* argv[] = { arg0, NULL };

    QueryPerformanceCounter(&sStart);
    LusxDebug_Start(__DATE__ " " __TIME__);
    Out("lus-x: built " __DATE__ " " __TIME__ "\n");
    MeasureCpu();
    SetUnhandledExceptionFilter(CrashReport);
    std::set_terminate(Terminate);
    LusXbox_LogMemory("at main");
    FpuSetup();
    HANDLE wd = CreateThread(NULL, 16 * 1024, Watchdog, NULL, 0, NULL);
    if (wd != NULL) {
        SetThreadPriority(wd, THREAD_PRIORITY_HIGHEST);
        CloseHandle(wd);
    }
    LusXbox_GamepadInit(); // enumeration is asynchronous: started first
    LusXbox_GamepadWait();
    SehRecord seh; // on this thread's stack, as the kernel requires, for as long as the game runs
    seh.handler = (void*)LusXbox_SehHandler;
    __asm__ volatile("movl %%fs:0, %0" : "=r"(seh.next));
    __asm__ volatile("movl %0, %%fs:0" : : "r"(&seh) : "memory");
    SDL_main(1, argv);
    __asm__ volatile("movl %0, %%fs:0" : : "r"(seh.next) : "memory");
    LusxDebug_Stop();
    XLaunchNewImage(NULL, NULL);
    return 0;
}
