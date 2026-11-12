// xbox_save.cpp -- where the game's saves live, as an original Xbox game keeps them: each of the three file slots is its
// own save game on the hard disk (U:), named after the slot and its player ("File 1 - Link"). The dashboard's Memory
// page lists them under "Ocarina Of Time-X" with the save image the XBE carries (tools/build.py: /DEFAULTSAVEIMAGE), and
// copies or deletes each one on its own. A slot's save game holds its fileN.sav, SoH's save of that slot. The settings
// the slots share (global.sav) aren't a save: they stay in T:\Save with the port's settings, as a game's options would.
//
// SaveManager.cpp asks for a slot's file (LusXbox_SlotPath), makes its save game on its first save (LusXbox_SlotOpen),
// names it after the player, and removes it when the game's file select erases or overwrites the slot.
//
// A slot's save game is found by the file in it, not by its name: the name changes with the player's (rewritten in its
// SaveMeta.xbx), and the system finds a save game by name through a hash of the name it was made with
// (libxapi xsaveapi.c), so after a rename only the file still says which slot it is. So a save game is removed here
// file by file, not with XDeleteSaveGame.
//
// Before this, saves were in T:\Save (builds to 2026-11-12) and then in one save game "Ocarina Of Time-X" holding all
// three slots (the 2026-11-13 04:50 build): both are moved into the slots' own save games at start.
#include <xtl.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "lusx_log.h"

#define OLD_DIR "T:\\Save"                     // the slots' files before, and the shared settings still
#define OLD_SAVE_GAME L"Ocarina Of Time-X"    // the 04:50 build's one save game for all three
#define SLOTS 3

static bool sSetUp;

static void Log(const char* fmt, ...) {
    char line[MAX_PATH + 160];
    va_list ap;

    if (!gLusxLogOn) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    strcat(line, "\n");
    LusxLog_Send(line);
}

static bool Exists(const char* path) {
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static void Strip(char* dir) {
    size_t n = strlen(dir);
    while (n > 0 && dir[n - 1] == '\\') {
        dir[--n] = '\0';
    }
}

// The save game named `name`, or else (name NULL) the one holding slot `slot`'s file, skipping the old one for all
// three: its folder, without the trailing backslash.
static bool Find(const wchar_t* name, int slot, char* dir, size_t size) {
    XGAME_FIND_DATA fd;
    char file[MAX_PATH];
    bool found = false;

    HANDLE h = XFindFirstSaveGame("U:\\", &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    do {
        Strip(fd.szSaveGameDirectory);
        if (name != NULL) {
            found = wcscmp(fd.szSaveGameName, name) == 0;
        } else if (wcscmp(fd.szSaveGameName, OLD_SAVE_GAME) != 0) {
            snprintf(file, sizeof(file), "%s\\file%d.sav", fd.szSaveGameDirectory, slot + 1);
            found = Exists(file);
        }
        if (found) {
            snprintf(dir, size, "%s", fd.szSaveGameDirectory);
        }
    } while (!found && XFindNextSaveGame(h, &fd));
    XFindClose(h);
    return found;
}

// a save game's folder and everything in it
static void RemoveSaveGame(const char* dir) {
    WIN32_FIND_DATAA fd;
    char path[MAX_PATH];

    snprintf(path, sizeof(path), "%s\\*", dir);
    HANDLE h = FindFirstFileA(path, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
                SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL); // (SaveMeta.xbx is a system file)
                DeleteFileA(path);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}

// the shown name of a save game, in its SaveMeta.xbx: the format XCreateSaveGame writes (a BOM, then "Name=...")
static void Rename(const char* dir, const wchar_t* name) {
    char path[MAX_PATH];
    wchar_t text[160];
    DWORD put = 0;

    snprintf(path, sizeof(path), "%s\\SaveMeta.xbx", dir);
    int n = _snwprintf(text, sizeof(text) / sizeof(text[0]) - 1, L"%lcName=%ls\r\n", (wint_t)0xFEFF, name);
    if (n <= 0) {
        return;
    }
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_SYSTEM, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    WriteFile(h, text, (DWORD)n * sizeof(wchar_t), &put, NULL);
    FlushFileBuffers(h);
    CloseHandle(h);
}

static void ShownName(int slot, const char* player, wchar_t* out, size_t size) {
    int n = _snwprintf(out, size - 1, L"File %d", slot + 1);
    if (n > 0 && player != NULL && player[0] != '\0') {
        n += _snwprintf(out + n, size - 1 - n, L" - ");
        for (const char* p = player; *p != '\0' && (size_t)n < size - 1; p++) {
            out[n++] = (wchar_t)(unsigned char)*p;
        }
    }
    out[n > 0 ? ((size_t)n < size ? n : size - 1) : 0] = L'\0';
}

// The name the dashboard shows for the game: U:\TitleMeta.xbx, which the system writes only the first time (libxapi
// xapiinit.c: "only write if this is an empty file"), so it kept the name of the build that first ran ("Soh-X").
// Rewritten when it isn't this XBE's.
static void FixTitleName(void) {
    // the running XBE's certificate (its header is at 0x10000, the certificate's address at +0x118), and the title
    // name in it (+0xC, 40 wide characters)
    const BYTE* cert = *(const BYTE* const*)(0x10000 + 0x118);
    wchar_t title[41];
    memcpy(title, cert + 0xC, 40 * sizeof(wchar_t));
    title[40] = L'\0';
    wchar_t want[64], have[64];
    DWORD got = 0, put = 0;

    int n = _snwprintf(want, sizeof(want) / sizeof(want[0]) - 1, L"%lcTitleName=%ls\r\n", (wint_t)0xFEFF, title);
    if (n <= 0) {
        return;
    }
    HANDLE h = CreateFileA("U:\\TitleMeta.xbx", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        ReadFile(h, have, sizeof(have), &got, NULL);
        CloseHandle(h);
        if (got == (DWORD)n * sizeof(wchar_t) && memcmp(have, want, got) == 0) {
            return;
        }
    }
    SetFileAttributesA("U:\\TitleMeta.xbx", FILE_ATTRIBUTE_NORMAL);
    h = CreateFileA("U:\\TitleMeta.xbx", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_SYSTEM, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    WriteFile(h, want, (DWORD)n * sizeof(wchar_t), &put, NULL);
    FlushFileBuffers(h);
    CloseHandle(h);
    Log("lus-x: save: the dashboard's name for the game set to \"%ls\"", title);
}

// a file moved across drives (copied, then the old one deleted); an existing target is left alone
static bool Move(const char* from, const char* to) {
    if (Exists(to)) {
        return false;
    }
    if (!CopyFileA(from, to, TRUE)) {
        Log("lus-x: save: %s not moved to %s (%lu)", from, to, (unsigned long)GetLastError());
        return false;
    }
    DeleteFileA(from);
    return true;
}

// Saves where the builds before kept them, into the slots' own save games ("File N" until SaveManager names them)
static void MoveOldSaves(void) {
    char combined[MAX_PATH], from[MAX_PATH], to[MAX_PATH], dir[MAX_PATH];
    const bool haveCombined = Find(OLD_SAVE_GAME, 0, combined, sizeof(combined));
    int moved = 0;

    for (int slot = 0; slot < SLOTS; slot++) {
        snprintf(from, sizeof(from), "%s\\file%d.sav", combined, slot + 1);
        if (!haveCombined || !Exists(from)) {
            snprintf(from, sizeof(from), "%s\\file%d.sav", OLD_DIR, slot + 1);
            if (!Exists(from)) {
                continue;
            }
        }
        if (Find(NULL, slot, dir, sizeof(dir))) {
            continue; // the slot has its save game already: the old file is left where it is
        }
        wchar_t name[32];
        ShownName(slot, NULL, name, sizeof(name) / sizeof(name[0]));
        if (XCreateSaveGame("U:\\", name, OPEN_ALWAYS, 0, dir, sizeof(dir)) != ERROR_SUCCESS) {
            continue;
        }
        Strip(dir);
        snprintf(to, sizeof(to), "%s\\file%d.sav", dir, slot + 1);
        if (Move(from, to)) {
            moved++;
        }
    }
    if (haveCombined) {
        snprintf(from, sizeof(from), "%s\\global.sav", combined);
        if (Exists(from)) {
            CreateDirectoryA(OLD_DIR, NULL);
            snprintf(to, sizeof(to), "%s\\global.sav", OLD_DIR);
            Move(from, to);
        }
        bool empty = true;
        for (int slot = 0; slot < SLOTS; slot++) {
            snprintf(from, sizeof(from), "%s\\file%d.sav", combined, slot + 1);
            empty = empty && !Exists(from);
        }
        if (empty) {
            RemoveSaveGame(combined);
        }
    }
    if (moved > 0) {
        Log("lus-x: save: %d saves moved into their own save games", moved);
    }
}

// ---- the old title ID's saves
//
// Builds before 2026-11-13 05:30 were title 0x4C580002 (LX-002); from then on 0x54520020 (TR's, the TitleID-DB repo).
// A title ID has drives of its own, so this one starts empty: the old title's saves are copied over once, its drives
// mounted as an alternate title (the XBE lists 0x4C580002 as one: tools/build.py /TESTALTID). Copied, not moved: the
// old title stays on the dashboard, renamed "... (old saves)", for the player to delete when they like. A marker file
// on T: keeps it from happening again (so a save deleted later doesn't come back).
#define OLD_TITLE_ID 0x4C580002
#define COPIED_MARK "T:\\lusx_copied_4c580002"

// a file copied to `to` unless there's one there already
static bool CopyIfNone(const char* from, const char* to) {
    if (!Exists(from) || Exists(to)) {
        return false;
    }
    if (!CopyFileA(from, to, TRUE)) {
        Log("lus-x: save: %s not copied to %s (%lu)", from, to, (unsigned long)GetLastError());
        return false;
    }
    return true;
}

static void CopyFromOldTitle(void) {
    char oldU = 0, oldT = 0, from[MAX_PATH], to[MAX_PATH], dir[MAX_PATH];
    WIN32_FIND_DATAA fd;
    int copied = 0;

    if (Exists(COPIED_MARK)) {
        return;
    }
    DWORD rvU = XMountAlternateTitle("U:\\", OLD_TITLE_ID, &oldU);
    DWORD rvT = XMountAlternateTitle("T:\\", OLD_TITLE_ID, &oldT);
    CreateDirectoryA(OLD_DIR, NULL);
    // the old title's save games first (the newest saves), into T:\Save, where MoveOldSaves takes them into their
    // own save games here: each slot's file, and global.sav (in the one save game the 04:50 build had)
    if (rvU == ERROR_SUCCESS) {
        snprintf(from, sizeof(from), "%c:\\*", oldU);
        HANDLE h = FindFirstFileA(from, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    continue;
                }
                for (int slot = 0; slot <= SLOTS; slot++) {
                    const char* name = slot < SLOTS ? NULL : "global.sav";
                    char file[16];
                    snprintf(file, sizeof(file), "file%d.sav", slot + 1);
                    if (name == NULL && Find(NULL, slot, dir, sizeof(dir))) {
                        continue; // the slot has a save here already
                    }
                    snprintf(from, sizeof(from), "%c:\\%s\\%s", oldU, fd.cFileName, name ? name : file);
                    snprintf(to, sizeof(to), "%s\\%s", OLD_DIR, name ? name : file);
                    copied += CopyIfNone(from, to);
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        // the old title's name on the dashboard, so the two aren't told apart only by their saves
        static const wchar_t kOldName[] = L"\xFEFFTitleName=Ocarina Of Time-X (old saves)\r\n";
        snprintf(to, sizeof(to), "%c:\\TitleMeta.xbx", oldU);
        SetFileAttributesA(to, FILE_ATTRIBUTE_NORMAL);
        HANDLE m = CreateFileA(to, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_SYSTEM, NULL);
        if (m != INVALID_HANDLE_VALUE) {
            DWORD put = 0;
            WriteFile(m, kOldName, sizeof(kOldName) - sizeof(wchar_t), &put, NULL);
            FlushFileBuffers(m);
            CloseHandle(m);
        }
    }
    // then its T:\Save (builds to 2026-11-12 kept the slots there; global.sav from 05:11 on)
    if (rvT == ERROR_SUCCESS) {
        for (int slot = 0; slot <= SLOTS; slot++) {
            char file[16];
            snprintf(file, sizeof(file), slot < SLOTS ? "file%d.sav" : "global.sav", slot + 1);
            if (slot < SLOTS && Find(NULL, slot, dir, sizeof(dir))) {
                continue;
            }
            snprintf(from, sizeof(from), "%c:\\Save\\%s", oldT, file);
            snprintf(to, sizeof(to), "%s\\%s", OLD_DIR, file);
            copied += CopyIfNone(from, to);
        }
    }
    if (oldU != 0) {
        XUnmountAlternateTitle(oldU);
    }
    if (oldT != 0) {
        XUnmountAlternateTitle(oldT);
    }
    // done for good once the old title's drives could be looked at (or it never had any on this console)
    const bool none = (rvU == ERROR_PATH_NOT_FOUND || rvU == ERROR_FILE_NOT_FOUND) &&
                      (rvT == ERROR_PATH_NOT_FOUND || rvT == ERROR_FILE_NOT_FOUND);
    if ((rvU == ERROR_SUCCESS && rvT == ERROR_SUCCESS) || none ||
        (rvU == ERROR_SUCCESS && (rvT == ERROR_PATH_NOT_FOUND || rvT == ERROR_FILE_NOT_FOUND))) {
        HANDLE m = CreateFileA(COPIED_MARK, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (m != INVALID_HANDLE_VALUE) {
            CloseHandle(m);
        }
    }
    Log("lus-x: save: title 0x%08X's drives (U: %lu, T: %lu): %d files copied over", OLD_TITLE_ID,
        (unsigned long)rvU, (unsigned long)rvT, copied);
}

static void SetUp(void) {
    if (sSetUp) {
        return;
    }
    sSetUp = true;
    FixTitleName();
    CopyFromOldTitle();
    MoveOldSaves();
}

// ---- the save thread
//
// SoH writes its saves on a worker of its own (SaveManager's BS::thread_pool), a std::thread: on the Xbox that is a
// bare kernel thread, without XAPI's per-thread data, and the first XAPI call on it that records an error
// (SetLastError: a file not there, say) writes through a null pointer (2026-11-13: eip in _SetLastError@4, write at
// 0x1C, making a new save). The saves run on this XAPI thread instead, one at a time, in the order they came.
struct SaveJob {
    void (*run)(void*);
    void* arg;
};
static CRITICAL_SECTION sJobLock;
static HANDLE sJobWake, sJobsDone; // a semaphore counting jobs; set while none is queued or running
static SaveJob sJobs[32];
static unsigned sJobHead, sJobCount;
static volatile LONG sJobsPending;

static DWORD WINAPI SaveThread(LPVOID) {
    for (;;) {
        WaitForSingleObject(sJobWake, INFINITE);
        EnterCriticalSection(&sJobLock);
        SaveJob job = sJobs[sJobHead];
        sJobHead = (sJobHead + 1) % (sizeof(sJobs) / sizeof(sJobs[0]));
        sJobCount--;
        LeaveCriticalSection(&sJobLock);
        job.run(job.arg);
        if (InterlockedDecrement(&sJobsPending) == 0) {
            SetEvent(sJobsDone);
        }
    }
    return 0;
}

static bool StartSaveThread(void) {
    static int sStarted; // 0 not yet, 1 running, -1 couldn't
    if (sStarted == 0) {
        InitializeCriticalSection(&sJobLock);
        sJobWake = CreateSemaphore(NULL, 0, sizeof(sJobs) / sizeof(sJobs[0]), NULL);
        sJobsDone = CreateEvent(NULL, TRUE, TRUE, NULL);
        // 128 KB: nlohmann::json's dump of a save recurses a few levels deep at most
        HANDLE h = sJobWake && sJobsDone ? CreateThread(NULL, 128 * 1024, SaveThread, NULL, 0, NULL) : NULL;
        sStarted = h != NULL ? 1 : -1;
        if (h != NULL) {
            CloseHandle(h);
        } else {
            Log("lus-x: save: no save thread (%lu): saves are written on the game's thread", (unsigned long)GetLastError());
        }
    }
    return sStarted > 0;
}

extern "C" {

// SaveManager::SaveSection: run(arg) on the save thread, after the saves queued before it (on the caller's thread if
// there's no save thread, or the queue is full)
void LusXbox_SaveRun(void (*run)(void*), void* arg) {
    bool queued = false;

    if (StartSaveThread()) {
        EnterCriticalSection(&sJobLock);
        if (sJobCount < sizeof(sJobs) / sizeof(sJobs[0])) {
            if (InterlockedIncrement(&sJobsPending) == 1) {
                ResetEvent(sJobsDone);
            }
            sJobs[(sJobHead + sJobCount) % (sizeof(sJobs) / sizeof(sJobs[0]))] = { run, arg };
            sJobCount++;
            queued = true;
        }
        LeaveCriticalSection(&sJobLock);
    }
    if (queued) {
        ReleaseSemaphore(sJobWake, 1, NULL);
    } else {
        run(arg);
    }
}

// SaveManager::ThreadPoolWait: until every queued save is written
void LusXbox_SaveWait(void) {
    if (sJobsDone != NULL) {
        WaitForSingleObject(sJobsDone, INFINITE);
    }
}

// SaveManager::Init, first: the game's name fixed and older builds' saves moved, before it reads T:\Save\global.sav
void LusXbox_SaveSetUp(void) {
    SetUp();
}

// The file of slot `slot` (0-2): in its save game if it has one (1), else where it would be in T:\Save (0: there's no
// such file there either, unless a save game couldn't be made)
int LusXbox_SlotPath(int slot, char* out, unsigned size) {
    char dir[MAX_PATH];

    SetUp();
    if (Find(NULL, slot, dir, sizeof(dir))) {
        snprintf(out, size, "%s\\file%d.sav", dir, slot + 1);
        return 1;
    }
    snprintf(out, size, "%s\\file%d.sav", OLD_DIR, slot + 1);
    return 0;
}

// The file to save slot `slot` to: its save game's, made now if it has none, and named after `player` (ASCII). T:\Save's
// if the save game can't be made (the disk full, say): then the save still goes somewhere.
int LusXbox_SlotOpen(int slot, const char* player, char* out, unsigned size) {
    char dir[MAX_PATH];
    wchar_t name[48];

    SetUp();
    ShownName(slot, player, name, sizeof(name) / sizeof(name[0]));
    if (Find(NULL, slot, dir, sizeof(dir))) {
        XGAME_FIND_DATA fd;
        bool same = false;
        HANDLE h = XFindFirstSaveGame("U:\\", &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                Strip(fd.szSaveGameDirectory);
                if (strcmp(fd.szSaveGameDirectory, dir) == 0) {
                    same = wcscmp(fd.szSaveGameName, name) == 0;
                    break;
                }
            } while (XFindNextSaveGame(h, &fd));
            XFindClose(h);
        }
        if (!same) {
            Rename(dir, name);
        }
    } else {
        DWORD rv = XCreateSaveGame("U:\\", name, OPEN_ALWAYS, 0, dir, sizeof(dir));
        if (rv != ERROR_SUCCESS) {
            Log("lus-x: save: no save game for file %d (%lu): saved in %s", slot + 1, (unsigned long)rv, OLD_DIR);
            CreateDirectoryA(OLD_DIR, NULL);
            snprintf(out, size, "%s\\file%d.sav", OLD_DIR, slot + 1);
            return 0;
        }
        Strip(dir);
        Log("lus-x: save: file %d's save game made: %s", slot + 1, dir);
    }
    snprintf(out, size, "%s\\file%d.sav", dir, slot + 1);
    return 1;
}

// the slot's save game renamed after its player, if it has one (at start, for saves moved in as "File N")
void LusXbox_SlotName(int slot, const char* player) {
    char dir[MAX_PATH], path[MAX_PATH];

    SetUp();
    if (Find(NULL, slot, dir, sizeof(dir))) {
        LusXbox_SlotOpen(slot, player, path, sizeof(path));
    }
}

// the slot erased in the file select (or about to be copied over): its save game goes, so the dashboard has no empty one
void LusXbox_SlotDelete(int slot) {
    char dir[MAX_PATH];

    SetUp();
    for (int tries = 0; tries < 4 && Find(NULL, slot, dir, sizeof(dir)); tries++) {
        RemoveSaveGame(dir);
        Log("lus-x: save: file %d's save game removed", slot + 1);
    }
}

// after each save: the folder's own entries (the renamed file) on the disk too, not only the file's bytes, before
// anyone turns the console off (the power button skips the cache's flush)
void LusXbox_FlushDirOf(const char* file) {
    char dir[MAX_PATH];

    snprintf(dir, sizeof(dir), "%s", file);
    for (char* p = dir; *p; p++) {
        if (*p == '/') {
            *p = '\\';
        }
    }
    char* cut = strrchr(dir, '\\');
    if (cut == NULL) {
        return;
    }
    *cut = '\0';
    HANDLE h = CreateFileA(dir, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    FlushFileBuffers(h);
    CloseHandle(h);
}

}
