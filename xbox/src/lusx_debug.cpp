// lusx_debug.cpp -- the tester build's tools: see lusx_debug.h. Files in T:, which is E:\TDATA\54520020 on the console:
//
//   lusx.log          this run: everything xbWatson would have shown
//   lusx_prev.log     the run before it
//   lusx_part.log     the older part of a run that passed LOG_LIMIT
//   lusx_box.txt      the black box: one sector, overwritten in place every second
//
// A hard lock can't write its own last line, so nothing here waits for the end. The black box goes straight to the disk
// (no cache, and its size never changes, so a write is that one sector), a second at a time and at each mark; a log
// that just stops, with no watchdog line and no crash report, is then itself the sign of a hard lock.
//
// All the file I/O is on the watchdog's thread. It's a CreateThread thread with XAPI's per-thread data: a std::thread has
// none, and CreateFile crashes on one, so a crash report on such a thread can't write for itself. Nothing here runs on
// the game's thread but a copy into a buffer.
#include "lusx_debug.h"
#include "lusx_log.h"

#if LUSX_DEBUG_TOOLS

#include <stdio.h>
#include <string.h>

#include <xtl.h>

extern "C" long __stdcall HalWriteSMBusValue(unsigned char address, unsigned char command, unsigned char isWord,
                                             unsigned long value);

#define LOG_PATH "T:\\lusx.log"
#define LOG_PREVIOUS "T:\\lusx_prev.log"
#define LOG_PART "T:\\lusx_part.log"
#define BOX_PATH "T:\\lusx_box.txt"

#define LOG_BUFFER (32 * 1024)      // of each of the two: a few KB are made between saves
#define LOG_LIMIT (4 * 1024 * 1024) // about 3 hours of play
#define SECTOR 512
#define LOG_EVERY_MS 5000
#define LAST_RUN_MS 20000 // the last run's report stays up this long after the first frame
#define LOCK_TEST_MS 3000
#define LED_AFTER_SECONDS 10 // a long load isn't a hang: the LED waits for the watchdog's second report

// the front LED, by way of the SMC: a sequence of four steps, red in the high nibble and green in the low
#define SMC_ADDRESS 0x20
#define SMC_LED_OVERRIDE 0x07
#define SMC_LED_SEQUENCE 0x08
#define LED_RED 0xF0
#define LED_RED_FLASHING 0xA0

#define OVERLAY_LINES 4
#define OVERLAY_COLUMNS 38 // 37 characters from column 3 of the 40 across

static CRITICAL_SECTION sLock; // the log's buffers and sMark
static bool sStarted;
static HANDLE sWake; // wakes the watchdog's thread early, for a save

static char sBuffers[2][LOG_BUFFER];
static unsigned sFilling, sLength, sLost;
static unsigned sLogBytes;
static bool sLogFailed, sBoxFailed;

static HANDLE sBox = INVALID_HANDLE_VALUE;
static char sSector[SECTOR] __attribute__((aligned(SECTOR)));

static char sBuilt[48];
static char sMark[96];
static char sNote[64];
static const char* volatile sState = "running";
static const char* volatile sWhereNow = "starting";
static volatile LONG sFlushAsked, sFlushDone;
static volatile bool sCrashed;
static bool sLedOn;

static DWORD sStartMs, sFirstFrameMs;
static unsigned long sFreeKb, sLowestKb = 0xFFFFFFFF;
static long sFrames, sFps;
static volatile int sNeedMs, sBudgetMs;
static volatile int sGameMs, sAudioMs, sListMs, sDrawMs, sTickMs;

static volatile bool sOverlayOn, sShowing;
static char sLines[2][OVERLAY_LINES][OVERLAY_COLUMNS];
static volatile int sShown;
static char sLastRun[OVERLAY_LINES][OVERLAY_COLUMNS];
static bool sHaveLastRun;

static void Led(unsigned sequence) {
    HalWriteSMBusValue(SMC_ADDRESS, SMC_LED_SEQUENCE, 0, sequence);
    HalWriteSMBusValue(SMC_ADDRESS, SMC_LED_OVERRIDE, 0, 1);
    sLedOn = true;
}

static void LedAsNormal(void) {
    HalWriteSMBusValue(SMC_ADDRESS, SMC_LED_OVERRIDE, 0, 0);
    sLedOn = false;
}

// ---- the files: the watchdog's thread only, but for Start, which is before it

static void FlushLog(void) {
    char lost[64];
    unsigned length;
    int lostLength = 0;
    const char* data;

    EnterCriticalSection(&sLock);
    data = sBuffers[sFilling];
    length = sLength;
    if (sLost != 0) {
        lostLength = snprintf(lost, sizeof(lost), "lus-x: %u lines lost, the log's buffer was full\n", sLost);
        sLost = 0;
    }
    sFilling ^= 1;
    sLength = 0;
    LeaveCriticalSection(&sLock);
    if (length == 0) {
        return;
    }
    if (sLogBytes + length > LOG_LIMIT) {
        DeleteFileA(LOG_PART);
        MoveFileA(LOG_PATH, LOG_PART);
        sLogBytes = 0;
    }
    // opened and closed each time: a hard lock then only loses what came after the last save
    HANDLE h = CreateFileA(LOG_PATH, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        sLogFailed = true;
        return;
    }
    DWORD written = 0;
    SetFilePointer(h, 0, NULL, FILE_END);
    sLogFailed = !WriteFile(h, data, length, &written, NULL) || written != length;
    if (lostLength > 0) {
        WriteFile(h, lost, (DWORD)lostLength, &written, NULL);
    }
    FlushFileBuffers(h);
    CloseHandle(h);
    sLogBytes += length;
}

static void WriteBox(void) {
    char mark[sizeof(sMark)];
    DWORD written = 0;

    if (sBox == INVALID_HANDLE_VALUE) {
        return;
    }
    EnterCriticalSection(&sLock);
    memcpy(mark, sMark, sizeof(mark));
    LeaveCriticalSection(&sLock);
    int n = snprintf(sSector, SECTOR,
                     "LUS-X BLACK BOX\nstate: %s\nbuilt: %s\nuptime: %lu s\nframes: %ld\nfps: %ld\n"
                     "free: %lu KB, lowest %lu KB\nwhere: %s\nmark: %s\nnote: %s\n",
                     sState, sBuilt, (unsigned long)((GetTickCount() - sStartMs) / 1000), sFrames, sFps, sFreeKb,
                     sLowestKb, sWhereNow, mark, sNote);
    if (n < 0 || n >= SECTOR) {
        n = SECTOR - 1;
    }
    memset(sSector + n, ' ', SECTOR - (size_t)n); // the whole sector: last time's text doesn't show through
    SetFilePointer(sBox, 0, NULL, FILE_BEGIN);
    sBoxFailed = !WriteFile(sBox, sSector, SECTOR, &written, NULL) || written != SECTOR;
    FlushFileBuffers(sBox);
}

static void OpenBox(void) {
    DWORD written = 0;

    // made at full size and closed first, so the directory entry is settled: a write after this is only its one sector
    memset(sSector, ' ', SECTOR);
    HANDLE h = CreateFileA(BOX_PATH, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        sBoxFailed = true;
        return;
    }
    WriteFile(h, sSector, SECTOR, &written, NULL);
    FlushFileBuffers(h);
    CloseHandle(h);
    sBox = CreateFileA(BOX_PATH, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, NULL);
    if (sBox == INVALID_HANDLE_VALUE) {
        // through the cache, then, with the flush after each write to push it out
        sBox = CreateFileA(BOX_PATH, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    }
    sBoxFailed = sBox == INVALID_HANDLE_VALUE;
}

static void Field(const char* box, const char* name, char* out, size_t size) {
    const char* p = strstr(box, name);
    size_t n = 0;

    if (p != NULL) {
        p += strlen(name);
        while (p[n] != '\0' && p[n] != '\n' && n + 1 < size) {
            out[n] = p[n];
            n++;
        }
    }
    out[n] = '\0';
}

// what the last run's black box says, as the overlay's lines
static void ReadLastRun(void) {
    char state[16], where[64], mark[96], note[64], uptime[16], memory[40];
    DWORD got = 0;

    HANDLE h = CreateFileA(BOX_PATH, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    BOOL ok = ReadFile(h, sSector, SECTOR, &got, NULL);
    CloseHandle(h);
    if (!ok || got != SECTOR || strncmp(sSector, "LUS-X BLACK BOX", 15) != 0) {
        return;
    }
    sSector[SECTOR - 1] = '\0';
    Field(sSector, "state: ", state, sizeof(state));
    Field(sSector, "where: ", where, sizeof(where));
    Field(sSector, "mark: ", mark, sizeof(mark));
    Field(sSector, "note: ", note, sizeof(note));
    Field(sSector, "uptime: ", uptime, sizeof(uptime));
    Field(sSector, "free: ", memory, sizeof(memory));
    if (strcmp(state, "exited") == 0) {
        return;
    }
    snprintf(sLastRun[0], OVERLAY_COLUMNS, "%s",
             strcmp(state, "crashed") == 0 ? "LAST RUN CRASHED"
             : strcmp(state, "hung") == 0  ? "LAST RUN HUNG"
                                           : "LAST RUN STOPPED: POWER OFF OR LOCK");
    snprintf(sLastRun[1], OVERLAY_COLUMNS, "%s", note[0] != '\0' ? note : where);
    snprintf(sLastRun[2], OVERLAY_COLUMNS, "%s", mark);
    snprintf(sLastRun[3], OVERLAY_COLUMNS, "up %s, free %s", uptime, memory);
    sHaveLastRun = true;
}

static void UpdateOverlay(DWORD now) {
    const int next = !sShown;
    // the last run's report, first, for a while: only with the overlay on (a player sees nothing of the tester tools)
    const bool lastRun =
        sOverlayOn && sHaveLastRun && (sFirstFrameMs == 0 || now - sFirstFrameMs < LAST_RUN_MS);
    char(*lines)[OVERLAY_COLUMNS] = sLines[next];

    memset(lines, 0, sizeof(sLines[0]));
    if (lastRun) {
        memcpy(lines, sLastRun, sizeof(sLastRun));
    } else {
        snprintf(lines[0], OVERLAY_COLUMNS, "fps %ld  needs %d of %d  tick %d", sFps, sNeedMs, sBudgetMs, sTickMs);
        snprintf(lines[1], OVERLAY_COLUMNS, "free %lu KB  lowest %lu", sFreeKb, sLowestKb);
        snprintf(lines[2], OVERLAY_COLUMNS, "game %d snd %d list %d draw %d", sGameMs, sAudioMs, sListMs, sDrawMs);
        if (sLogFailed || sBoxFailed) {
            snprintf(lines[3], OVERLAY_COLUMNS, "CAN'T WRITE:%s%s", sLogFailed ? " LOG" : "", sBoxFailed ? " BOX" : "");
        }
    }
    sShown = next;
    sShowing = lastRun || sOverlayOn;
}

// ---- for the other threads

// asks the watchdog's thread to save now, and waits for it
static void FlushAndWait(unsigned ms) {
    const LONG asked = InterlockedIncrement((LONG*)&sFlushAsked);

    SetEvent(sWake);
    for (unsigned waited = 0; waited < ms && (LONG)(sFlushDone - asked) < 0; waited += 2) {
        Sleep(2);
    }
}

extern "C" void LusxDebug_Line(const char* line) {
    size_t length;

    if (!sStarted) { // (lusx_log.h's LusxLog_Send is what calls this, and only with Logging on)
        return;
    }
    length = strlen(line);
    EnterCriticalSection(&sLock);
    if (sLength + length <= LOG_BUFFER) {
        memcpy(sBuffers[sFilling] + sLength, line, length);
        sLength += (unsigned)length;
    } else {
        sLost++;
    }
    LeaveCriticalSection(&sLock);
}

extern "C" void LusxDebug_Mark(const char* text) {
    char line[128];

    if (!sStarted) {
        return;
    }
    snprintf(line, sizeof(line), "lus-x: mark: %s\n", text);
    LusxLog_Send(line);
    EnterCriticalSection(&sLock);
    snprintf(sMark, sizeof(sMark), "%s", text);
    LeaveCriticalSection(&sLock);
    FlushAndWait(40);
}

extern "C" void LusxDebug_Crash(unsigned long code, unsigned long address) {
    if (!sStarted) {
        return;
    }
    snprintf(sNote, sizeof(sNote), "exception 0x%08lX at 0x%08lX", code, address);
    sState = "crashed";
    sCrashed = true;
    FlushAndWait(2000);
}

extern "C" void LusxDebug_Start(const char* built) {
    InitializeCriticalSection(&sLock);
    sWake = CreateEvent(NULL, FALSE, FALSE, NULL);
    sStartMs = GetTickCount();
    snprintf(sBuilt, sizeof(sBuilt), "%s", built);
    ReadLastRun();
    DeleteFileA(LOG_PREVIOUS);
    MoveFileA(LOG_PATH, LOG_PREVIOUS);
    DeleteFileA(LOG_PART);
    OpenBox();
    sStarted = true;
    if (sHaveLastRun) {
        char line[160];
        snprintf(line, sizeof(line), "lus-x: %s; %s; %s; %s\n", sLastRun[0], sLastRun[1], sLastRun[2], sLastRun[3]);
        LusxLog_Send(line);
    }
    WriteBox();
}

extern "C" void LusxDebug_Stop(void) {
    if (sStarted) {
        sState = "exited";
        FlushAndWait(200);
    }
}

extern "C" void LusxDebug_Tick(const char* where, long frames) {
    static DWORD sLastSecond, sLastLog;
    static long sFramesThen;
    MEMORYSTATUS ms;

    WaitForSingleObject(sWake, 100);
    const DWORD now = GetTickCount();
    const LONG asked = sFlushAsked;
    const bool second = now - sLastSecond >= 1000;

    GlobalMemoryStatus(&ms);
    sFreeKb = (unsigned long)(ms.dwAvailPhys >> 10);
    if (sFreeKb < sLowestKb) {
        sLowestKb = sFreeKb;
    }
    // each new low a MB under the last one said, once the game runs: the log's lines around it say where it was
    {
        static unsigned long sLoggedKb = 0xFFFFFFFF;
        if (frames > 0 && sLowestKb + 1024 <= sLoggedKb) {
            char line[160];
            sLoggedKb = sLowestKb;
            snprintf(line, sizeof(line), "lus-x: free memory down to %lu KB, the lowest yet (where: %s)\n", sLowestKb,
                     where != NULL ? where : "?");
            LusxLog_Send(line);
        }
    }
    sFrames = frames;
    sWhereNow = where;
    if (frames > 0 && sFirstFrameMs == 0) {
        sFirstFrameMs = now;
    }
    if (second) {
        sFps = sLastSecond != 0 ? (long)((frames - sFramesThen) * 1000 / (long)(now - sLastSecond)) : 0;
        sFramesThen = frames;
        sLastSecond = now;
        UpdateOverlay(now);
    }
    if (sCrashed && !sLedOn) {
        Led(LED_RED_FLASHING);
    }
    if (asked != sFlushDone || now - sLastLog >= LOG_EVERY_MS) {
        FlushLog();
        sLastLog = now;
    }
    if (asked != sFlushDone || second) {
        WriteBox();
    }
    sFlushDone = asked;
}

extern "C" void LusxDebug_Hang(const char* where, unsigned long seconds) {
    sState = "hung";
    sWhereNow = where;
    if (seconds >= LED_AFTER_SECONDS && !sLedOn) {
        Led(LED_RED);
    }
    FlushLog();
    WriteBox();
}

extern "C" void LusxDebug_Resumed(void) {
    if (!sCrashed) {
        sState = "running";
        if (sLedOn) {
            LedAsNormal();
        }
    }
}

extern "C" void LusxDebug_SetCosts(int gameMs, int audioMs, int listMs, int drawMs, int tickMs) {
    sGameMs = gameMs;
    sAudioMs = audioMs;
    sListMs = listMs;
    sDrawMs = drawMs;
    sTickMs = tickMs;
}

extern "C" void LusxDebug_SetNeed(int needMs, int budgetMs) {
    sNeedMs = needMs;
    sBudgetMs = budgetMs;
}

extern "C" int LusxDebug_OverlayOn(void) {
    return sOverlayOn;
}

extern "C" void LusxDebug_SetOverlay(int on) {
    sOverlayOn = on != 0;
    sShowing = sOverlayOn;
}

// the test that a hard lock leaves the files as they should be: interrupts off and nowhere to go, as the real thing
static void LockTest(void) {
    LusxDebug_Mark("lock test: stopped on purpose");
    __asm__ volatile("cli");
    for (;;) {
        __asm__ volatile("" ::: "memory");
    }
}

extern "C" void LusxDebug_Pad(unsigned buttons, int white, int black) {
    static const unsigned kCombo = XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;
    static DWORD sLockSince;

    if ((buttons & kCombo) != kCombo || !white || !black) {
        sLockSince = 0;
    } else if (sLockSince == 0) {
        sLockSince = GetTickCount() | 1;
    } else if (GetTickCount() - sLockSince >= LOCK_TEST_MS) {
        LockTest();
    }
}

extern "C" const char* LusxDebug_OverlayLine(int line) {
    if (!sShowing || line >= OVERLAY_LINES) {
        return NULL;
    }
    const char* text = sLines[sShown][line];
    return text[0] != '\0' ? text : NULL;
}

#endif
