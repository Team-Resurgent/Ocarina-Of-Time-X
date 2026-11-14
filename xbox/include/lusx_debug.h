/* lusx_debug.h -- the test tools (xbox/src/lusx_debug.cpp), for retail consoles, where nothing hears the debug
 * channel: a log file, a black box file, the front LED and a last-run report, all only while the port menu's Logging
 * is on; and an on-screen overlay, the menu's Test overlay. Both rows are on the Debug page, off on a fresh install.
 *
 * Every build has them. LUSX_DEBUG_TOOLS 0 (tools/build.py, generate) would leave all of it out: the file then compiles
 * to nothing and every call below is an empty macro. To remove it for good: delete this header and lusx_debug.cpp, and
 * the lines naming LusxDebug_ or LUSX_DEBUG_TOOLS (git grep finds them all). */
#pragma once

#include "lusx_debug_config.h" /* written by tools/build.py into build/gen */

#if LUSX_DEBUG_TOOLS

#ifdef __cplusplus
extern "C" {
#endif

/* main, first of all: reads the last run's black box, moves the last log aside and opens this run's files */
void LusxDebug_Start(const char* built);
/* main, on the way out: this run didn't stop by accident */
void LusxDebug_Stop(void);
/* a line of the log, from any thread (it's what xbWatson gets) */
void LusxDebug_Line(const char* line);
/* Logging on or off (lusx_log.h's LusxLog_Settle): the files written from now, or closed */
void LusxDebug_Files(int on);
/* a moment worth keeping, from a thread that can wait up to 40 ms (scene changes): on disk before it returns */
void LusxDebug_Mark(const char* text);

/* the watchdog's thread, the only one that touches the files. Waits up to 100 ms; `where` is LusXbox_Where's */
void LusxDebug_Tick(const char* where, long frames);
void LusxDebug_Hang(const char* where, unsigned long seconds);
void LusxDebug_Resumed(void);

/* the crash report's end: saves it by way of the watchdog's thread, waiting up to 2 s */
void LusxDebug_Crash(unsigned long code, unsigned long address);

/* the pads' buttons, once a poll: Back, both sticks clicked, White and Black, held for 3 s, lock the console up on
   purpose: the test that the files survive one */
void LusxDebug_Pad(unsigned buttons, int white, int black);
/* the overlay: the menu's Test overlay row (gLusx.TestOverlay, saved) turns it on and off. Off, the last run's report
   isn't shown either (the log still has it, with Logging on) */
int LusxDebug_OverlayOn(void);
void LusxDebug_SetOverlay(int on);
/* the governor's figures for the overlay */
void LusxDebug_SetNeed(int needMs, int budgetMs);
/* a tick's parts: the game's update, the audio thread's synthesis, recording the list, a frame's drawing; and how long
   ticks really take */
void LusxDebug_SetCosts(int gameMs, int audioMs, int listMs, int drawMs, int tickMs);
/* the overlay's lines from the top, NULL past the last and while it's off (graph.c draws them) */
const char* LusxDebug_OverlayLine(int line);

#ifdef __cplusplus
}
#endif

#else

#define LusxDebug_Start(built) ((void)0)
#define LusxDebug_Stop() ((void)0)
#define LusxDebug_Line(line) ((void)0)
#define LusxDebug_Files(on) ((void)0)
#define LusxDebug_Mark(text) ((void)0)
#define LusxDebug_Hang(where, seconds) ((void)0)
#define LusxDebug_Resumed() ((void)0)
#define LusxDebug_Crash(code, address) ((void)0)
#define LusxDebug_Pad(buttons, white, black) ((void)0)
#define LusxDebug_SetNeed(needMs, budgetMs) ((void)0)
#define LusxDebug_SetCosts(gameMs, audioMs, listMs, drawMs, tickMs) ((void)0)

#endif
