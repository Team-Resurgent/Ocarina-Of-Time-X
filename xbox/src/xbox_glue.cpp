// xbox_glue.cpp -- what the parts Lus-X takes from elsewhere need of the Xbox:
//   UltraX's MMX mixer (audio/ux_mixer.c): ticks and a log line;
//   2Ship's synthesis: the Opus custom-audio decoder, not carried (mixer.c's other half).
#include <xtl.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include <new>
#include <stdlib.h>

#include "lusx_debug.h"
#include "lusx_log.h"
#include "ultrax/ux.h"

// new and delete as ever, on the clock: what the heap costs a tick, for the log (BenPort.cpp). Not atomic: the
// other threads' calls are rare, and a lost count is no matter here.
extern "C" {
uint64_t gLusxHeapCycles;
uint32_t gLusxHeapCalls;
}

static inline void* HeapNew(size_t size) {
    const uint64_t began = __builtin_ia32_rdtsc();
    void* p = malloc(size != 0 ? size : 1);
    gLusxHeapCycles += __builtin_ia32_rdtsc() - began;
    gLusxHeapCalls++;
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

static inline void HeapDelete(void* p) {
    const uint64_t began = __builtin_ia32_rdtsc();
    free(p);
    gLusxHeapCycles += __builtin_ia32_rdtsc() - began;
    gLusxHeapCalls++;
}

void* operator new(size_t size) {
    return HeapNew(size);
}
void* operator new[](size_t size) {
    return HeapNew(size);
}
void operator delete(void* p) noexcept {
    HeapDelete(p);
}
void operator delete[](void* p) noexcept {
    HeapDelete(p);
}
void operator delete(void* p, size_t) noexcept {
    HeapDelete(p);
}
void operator delete[](void* p, size_t) noexcept {
    HeapDelete(p);
}

extern "C" ux_u64 ux_host_ticks(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (ux_u64)t.QuadPart;
}

extern "C" ux_u64 ux_host_tick_rate(void) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (ux_u64)f.QuadPart;
}

// 2Ship's audio thread (BenPort.cpp): above the game's, so a tick's synthesis runs in one piece as soon as
// it's asked for: one core, so it was cut into the renderer's work (and timings) otherwise
extern "C" unsigned LusXbox_ThreadId(void) {
    return (unsigned)GetCurrentThreadId();
}

// A thread with the Xbox's per-thread data, for one that may do file I/O: a std::thread has none, and CreateFile or
// anything else that sets the last error crashes on it.
static DWORD WINAPI ThreadEntry(LPVOID entry) {
    ((void (*)(void))entry)();
    return 0;
}

extern "C" void LusXbox_StartThread(void (*entry)(void), unsigned stackBytes) {
    CreateThread(NULL, stackBytes, ThreadEntry, (LPVOID)entry, 0, NULL);
}

extern "C" void LusXbox_RaiseThisThread(void) {
    // and flush-to-zero for its SSE maths, as main() set for the game's thread: MXCSR is a thread's own,
    // and a denormal stalls the P3 for ~100 cycles (no DAZ: the Coppermine P3 lacks it)
    uint32_t csr;
    __asm__ volatile("stmxcsr %0" : "=m"(csr));
    csr |= 0x8000;
    __asm__ volatile("ldmxcsr %0" : : "m"(csr));
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
}

// and its wait between looks at the stream
extern "C" void LusXbox_Sleep(unsigned ms) {
    Sleep(ms);
}

// a file written whole (the audio recording for the diagnosis: BenPort.cpp); true if it was
extern "C" int LusXbox_WriteFile(const char* path, const void* data, unsigned size) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;

    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    BOOL ok = WriteFile(h, data, size, &written, NULL);
    CloseHandle(h);
    return ok && written == size;
}

extern "C" void ux_host_log(const char* fmt, ...) {
    char line[512];
    va_list ap;

    if (!gLusxLogOn) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    strcat(line, "\n");
    LusxLog_Send(line);
}

// strlen, wrapped at link time (tools/build.py: --wrap=strlen): a string pointer below 64 KB is never
// a string (the title's image starts at 0x10000) -- strlen(0x1) crashed after the title demo's Goron
// scene. Logged once with the caller's address (llvm-symbolizer --obj=build/Lus-X.exe), and taken as
// an empty string.
extern "C" size_t __real_strlen(const char* s);

extern "C" size_t __wrap_strlen(const char* s) {
    static volatile LONG sReported;

    if ((uintptr_t)s < 0x10000) {
        if (gLusxLogOn && InterlockedExchange((LONG*)&sReported, 1) == 0) {
            char line[128];
            snprintf(line, sizeof(line), "lus-x: strlen(%p) called from %p\n", (const void*)s,
                     __builtin_return_address(0));
            LusxLog_Send(line);
        }
        return 0;
    }
    return __real_strlen(s);
}

struct OpusDecState;

// Opus-encoded custom samples (SoH's mixer.c's other half, which needs libopusfile): silence
extern "C" void aOPUSdecImpl(void* source_addr, uint16_t dest_addr, uint16_t nbytes, struct OpusDecState** decState,
                             int32_t pos, uint32_t size) {
}

extern "C" void aOPUSFree(struct OpusDecState* dec) {
}
