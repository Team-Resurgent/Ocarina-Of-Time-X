// lusx_overlay.h -- HD drawing for the port's menu (lusx_menu.cpp): text in a real font and shapes, sharp at the
// display's own resolution, over the game's frame. After Diddy-Kong-Racing-X's dkr_overlay.
//
// Everything is laid out in a 960x720 space, the 4:3 picture at 720p, centred on a 16:9 display.
#pragma once

#include <stdint.h>

namespace Fast {
class GfxRenderingAPI;
}

#define OVL_W 960
#define OVL_H 720

enum { OVL_FONT_BODY, OVL_FONT_TITLE, OVL_FONT_SMALL, OVL_FONT_COUNT };
enum { OVL_ALIGN_LEFT, OVL_ALIGN_CENTRE, OVL_ALIGN_RIGHT };

// The font, from D:\menu.ttf, and its atlases: under 1 MB of textures that only exist while the menu is open
bool Ovl_Load(Fast::GfxRenderingAPI* rapi);
void Ovl_Unload(void);

// a frame's drawing starts here: the screen, whole, no depth
void Ovl_Begin(void);

// colours are 0xAARRGGBB
void Ovl_Rect(float x0, float y0, float x1, float y1, uint32_t top, uint32_t bottom);
void Ovl_RoundRect(float x0, float y0, float x1, float y1, float radius, uint32_t top, uint32_t bottom);
void Ovl_Circle(float cx, float cy, float radius, uint32_t colour);
void Ovl_Tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t colour);
// y is the middle of the line
void Ovl_Text(int font, float x, float y, const char* text, uint32_t colour, int align);
float Ovl_TextWidth(int font, const char* text);
// what's drawn from here on stays between these two x positions; Ovl_ClipOff ends it
void Ovl_Clip(float x0, float x1);
void Ovl_ClipOff(void);
