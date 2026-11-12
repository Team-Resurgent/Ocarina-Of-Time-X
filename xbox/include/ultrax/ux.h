/* ux.h -- UltraX: the N64 underneath libultra, for decomp ports.
 *
 * A decomp is game code plus the libultra it shipped with. UltraX keeps both
 * and replaces what libultra itself stands on:
 *
 *   - the R4300 primitives libultra wrote in assembly (thread switch,
 *     interrupt enable, Count/Compare, caches, TLB), with N64 threads run as
 *     fibers: one runs at a time and the highest priority runnable one runs,
 *     exactly as on the console;
 *   - the RCP and PIF registers libultra and the game reach through IO_READ /
 *     IO_WRITE (MI, VI, AI, PI, SI, SP, DP), with the interrupts they raise
 *     delivered to libultra's own event table.
 *
 * What the console's hardware did with the data -- draw a display list, play
 * an audio buffer, read the cartridge, answer the controllers -- is the port's
 * job, handed over through UxPlatform below.
 *
 * This header uses no libultra types, so the platform side can include it next
 * to windows.h or xtl.h. The code that touches libultra's structures is
 * compiled with the game's own headers (see src/).
 */
#ifndef ULTRAX_UX_H
#define ULTRAX_UX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char ux_u8;
typedef unsigned short ux_u16;
typedef unsigned int ux_u32;
typedef unsigned long long ux_u64;

/* ------------------------------------------------------------- configuration */

enum {
    UX_CLOCK_REAL,    /* Count follows the host clock; the idle loop sleeps */
    UX_CLOCK_VIRTUAL  /* Count only moves when the game waits: deterministic, for tests */
};

enum { UX_TV_PAL = 0, UX_TV_NTSC = 1, UX_TV_MPAL = 2 }; /* osTvType values */

typedef struct UxConfig {
    int clock;              /* UX_CLOCK_* */
    int tv_type;            /* UX_TV_*: osTvType, and 60 or 50 retraces a second */
    ux_u32 mem_size;        /* osMemSize: 0x400000, or 0x800000 with the Expansion Pak */
    size_t fiber_stack;     /* host stack for each N64 thread (0: 64 KB) */
    int run_idle_threads;   /* 0: never run a priority-0 thread (libultra's idle loop
                               spins forever; parking it is what lets time pass) */
    int rcp_sync;           /* 1: run each SP task the moment the game starts it,
                               on the game's thread (always so with the virtual
                               clock: deterministic). 0: on a host thread of its
                               own, alongside the game, as the console's RSP and
                               RDP run alongside its CPU */
} UxConfig;

/* -------------------------------------------------------- what the port does */

/* Results of UxPlatform.sp_task: which RCP interrupts the finished task raises. */
#define UX_TASK_SP_DONE 0x1 /* SP interrupt + SIG2 (task done) */
#define UX_TASK_DP_DONE 0x2 /* DP full-sync interrupt (a display list's gDPFullSync) */
#define UX_TASK_YIELDED 0x4 /* stopped at the game's request (ux_sp_yield_requested):
                               SP interrupt + SIG1 (yielded) + SIG2; the game starts
                               it again later with OS_TASK_YIELDED in its flags */

typedef struct UxPlatform {
    /* PI: copy `len` bytes of cartridge space at `cart_addr` (physical, e.g.
       0x10001000) into `dst`. Return 0, or nonzero if there is nothing there. */
    int (*cart_read)(ux_u32 cart_addr, void* dst, ux_u32 len);
    /* PI: RDRAM -> cartridge space (SRAM, FlashRAM command writes, 64DD). */
    int (*cart_write)(ux_u32 cart_addr, const void* src, ux_u32 len);
    /* PI: whether reading `len` bytes at `cart_addr` is instant (the
       platform holds that part in memory, ux_cart_instant). Such a DMA is
       done at once, as a cartridge's are; the others run on a thread of
       their own beside the game. NULL: none is. */
    int (*cart_instant)(ux_u32 cart_addr, ux_u32 len);

    /* SP: run one task. `task` points at the OSTask libultra DMAed into DMEM;
       its pointers are host pointers. Return UX_TASK_* flags. NULL: every
       task completes at once with SP + DP done. Called on the RCP's thread
       unless UxConfig.rcp_sync. */
    ux_u32 (*sp_task)(const void* task);

    /* VI: called at each vertical retrace with the VI registers as the game
       left them (regs[1] is the framebuffer origin). */
    void (*vi_retrace)(const ux_u32* vi_regs);

    /* VI: the display's count of vertical blanks, where the host has a
       display at the game's refresh rate. The retraces then follow it: a
       picture each display refresh, shown in the blank, never torn nor held
       back for one. NULL: UltraX's clock times them. */
    ux_u32 (*vi_vblanks)(void);

    /* AI: a buffer of 16-bit stereo samples the game queued, at `rate` Hz. */
    void (*ai_buffer)(const void* samples, ux_u32 bytes, ux_u32 rate);

    /* SI: the game wrote PIF RAM and asked for it to be executed. Answer the
       joybus commands in place (controllers, Controller Pak, Rumble Pak,
       EEPROM). PIF RAM holds what libultra wrote with host stores, so fields
       it reads as 16-bit words (buttons) are answered in host byte order and
       the control word is pif_ram[60..63] read as a host u32. */
    void (*pif_execute)(ux_u8 pif_ram[64]);

    /* A register UltraX does not model was touched. NULL: logged once. */
    void (*unknown_io)(ux_u32 addr, int is_write, ux_u32 value);
} UxPlatform;

/* Set both before the game's boot entry runs. */
void ux_init(const UxConfig* cfg, const UxPlatform* platform);

/* Turn the calling host thread into the N64 boot context and run `entry`
   (the game's bootproc). libultra's first osStartThread leaves it for good. */
void ux_run(void (*entry)(void));

/* Count (46.875 MHz) since boot, 64-bit. */
ux_u64 ux_count64(void);

/* Retraces delivered so far. */
ux_u32 ux_retrace_count(void);

/* In UxPlatform.vi_retrace: nonzero when this retrace's blank is already
   past another (the game was late for it, UxPlatform.vi_vblanks): its picture
   would show a refresh late, so it is passed over (ux_gfx_pass). */
int ux_retrace_late(void);

/* To the log: the AI buffers since the last call that came late (played on
   from the one before) or after a gap. */
void ux_ai_log_stats(void);

/* To the log, only when the game had no idle time since the last call: the
   N64 thread that runs and the game code that last called the clock or the
   interrupt mask (a busy loop, or a hang). */
void ux_os_log_stats(void);

/* For an SP task's implementation: whether the game has asked the running
   task to yield (osSpTaskYield; MM yields graphics for audio). A task that
   can yield checks this between commands and returns UX_TASK_YIELDED. */
int ux_sp_yield_requested(void);

/* ---------------------------------------------------------- host primitives */
/* Implemented per host in src/host/: identical on Windows and the Xbox, both
   of which have Win32 fibers and QueryPerformanceCounter. */

void* ux_host_fiber_from_thread(void);
void* ux_host_fiber_create(size_t stack, void (*fn)(void*), void* arg);
void ux_host_fiber_switch(void* fiber);
void ux_host_fiber_delete(void* fiber);
ux_u64 ux_host_ticks(void);
ux_u64 ux_host_tick_rate(void);
void ux_host_sleep_ms(ux_u32 ms);
void ux_host_log(const char* fmt, ...);
void ux_host_fatal(const char* fmt, ...);
/* Memory from the host's heap, for game-side code that needs more than the
   game's own arenas give and that the RCP never reads (it may lie above
   16 MB); fatal when there is none. */
void* ux_host_alloc(size_t size);
/* Called by the host's crash report after the registers and stack, so the
   port can add what it knows (where it loaded the game's overlays...). */
void ux_host_set_crash_hook(void (*hook)(void));
/* The RCP's host thread, below the game's in priority, so the game's threads
   (interrupts, audio) run whenever they can and the RCP in the time left, as
   a console's CPU never waits on its RSP. Returns the thread. */
void* ux_host_thread_start(void (*fn)(void*), void* arg);
/* A thread's priority (NULL: the calling thread's): above the game's thread
   (urgent), or below it */
void ux_host_thread_urgent(void* thread, int urgent);
/* An auto-reset event: wait until signalled or `ms` pass (~0u: forever). */
void* ux_host_event_create(void);
void ux_host_event_signal(void* ev);
void ux_host_event_wait(void* ev, ux_u32 ms);

#ifdef __cplusplus
}
#endif

#endif
