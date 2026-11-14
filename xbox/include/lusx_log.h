/* lusx_log.h -- the port menu's Logging switch (Debug page, gLusx.Logging, off unless turned on). Off: nothing goes
 * to the debug channel (xbWatson) or to the log file, the black box isn't written (lusx_debug.h), and SoH's and
 * libultraship's spdlog lines aren't even made. Until the config is read, lines are held (xbox_main.cpp), then sent
 * or dropped as it says; a crash before that sends them. Only the runtime's own start-up lines (RXDK) come before any
 * of this. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 1 while lines are taken (held, until LusxLog_Settle, or sent); 0 with Logging off */
extern volatile int gLusxLogOn;
/* a line ('\n' at its end) to the debug channel and the tester build's log file, from any thread */
void LusxLog_Send(const char* line);
/* the config is read: sends the held lines (on) or drops them (off), and from then on as `on` says */
void LusxLog_Settle(int on);
/* reads gLusx.Logging and switches to it (lusx_menu.cpp): once the config is loaded, and when the row changes */
void LusxLog_Apply(void);

#ifdef __cplusplus
}
#endif
