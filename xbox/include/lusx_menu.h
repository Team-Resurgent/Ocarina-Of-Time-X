/* lusx_menu.h -- the port's own menu (xbox/src/lusx_menu.cpp): Back, pressed and let go on its own, opens it. */
#pragma once

/* a pad as the menu reads it: every pad together (xbox_gamepad.cpp) */
enum {
    LUSX_PAD_A = 1 << 0,
    LUSX_PAD_B = 1 << 1,
    LUSX_PAD_X = 1 << 2,
    LUSX_PAD_Y = 1 << 3,
    LUSX_PAD_BLACK = 1 << 4,
    LUSX_PAD_WHITE = 1 << 5,
    LUSX_PAD_LT = 1 << 6,
    LUSX_PAD_RT = 1 << 7,
    LUSX_PAD_START = 1 << 8,
    LUSX_PAD_BACK = 1 << 9,
    LUSX_PAD_LS = 1 << 10, /* the sticks' clicks */
    LUSX_PAD_RS = 1 << 11,
    LUSX_PAD_UP = 1 << 12, /* the D-pad */
    LUSX_PAD_DOWN = 1 << 13,
    LUSX_PAD_LEFT = 1 << 14,
    LUSX_PAD_RIGHT = 1 << 15,
    LUSX_PAD_R_UP = 1 << 16, /* the right stick, pushed */
    LUSX_PAD_R_DOWN = 1 << 17,
    LUSX_PAD_R_LEFT = 1 << 18,
    LUSX_PAD_R_RIGHT = 1 << 19,
    LUSX_PAD_L_UP = 1 << 20, /* the left stick, pushed: the menu's D-pad, not a button to map */
    LUSX_PAD_L_DOWN = 1 << 21,
    LUSX_PAD_L_LEFT = 1 << 22,
    LUSX_PAD_L_RIGHT = 1 << 23
};

#ifdef __cplusplus
extern "C" {
#endif

/* the pads, once a poll */
void LusxMenu_Pad(unsigned buttons);
/* Back was pressed and let go, once: asked before a tick's last frame is drawn, as the menu keeps the tick's recording
   and its textures (BenPort.cpp) */
int LusxMenu_Wanted(void);
/* the menu holds the game: the audio thread makes nothing while it does */
int LusxMenu_IsOpen(void);
/* a frame's pads, once the menu is open; 0 when it has closed */
int LusxMenu_Update(void);
void LusxMenu_Close(void);

/* xbox_gamepad.cpp: the pads' motors stopped, the game being frozen with them as they were */
void LusXbox_PadsQuiet(void);

#ifdef __cplusplus
}

namespace Fast {
class GfxRenderingAPI;
}

/* the cheats left on from an earlier run, for the log */
void LusxMenu_LogCheats(void);
/* false if it can't open (no memory for the font) */
bool LusxMenu_Open(Fast::GfxRenderingAPI* rapi);
/* the menu over a frame, between the replay and the present */
void LusxMenu_Draw(Fast::GfxRenderingAPI* rapi);
#endif
