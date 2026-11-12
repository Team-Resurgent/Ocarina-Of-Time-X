/* imgui.h -- Lus-X: NOT Dear ImGui. The Xbox build has no GUI; this declares
 * only the ImGui *types* that libultraship's and 2Ship's headers mention in
 * signatures and members (ImVec2, ImVec4, ImTextureID, ...), so those headers
 * build unchanged. There are no ImGui functions: a source file that calls one
 * is left out of the build (tools/build.py) and its entry points stubbed in
 * xbox/src/lus/NoGui.cpp. */
#pragma once

#include <stdint.h>
#include <stddef.h>

#define IMGUI_VERSION "lusx-types-only"
#define ImTextureID void* /* as LUS's imconfig.h */

typedef unsigned int ImGuiID;
typedef int ImGuiKey;
typedef int ImGuiCol;
typedef int ImGuiCond;
typedef int ImGuiStyleVar;
typedef int ImGuiWindowFlags;
typedef int ImGuiTableFlags;
typedef int ImGuiTableColumnFlags;
typedef int ImGuiTreeNodeFlags;
typedef int ImGuiInputTextFlags;
typedef int ImGuiSliderFlags;
typedef int ImGuiComboFlags;
typedef int ImGuiSelectableFlags;
typedef int ImGuiColorEditFlags;
typedef int ImGuiHoveredFlags;
typedef int ImGuiFocusedFlags;
typedef int ImGuiChildFlags;
typedef int ImGuiButtonFlags;
typedef int ImGuiItemFlags;
typedef int ImGuiDir;
typedef int ImGuiMouseButton;
typedef unsigned int ImU32;
typedef unsigned short ImWchar;
typedef unsigned char ImU8;
typedef signed char ImS8;
typedef unsigned short ImU16;
typedef short ImS16;
typedef int ImS32;
typedef unsigned long long ImU64;
typedef long long ImS64;

struct ImVec2 {
    float x, y;
    constexpr ImVec2() : x(0.0f), y(0.0f) {
    }
    constexpr ImVec2(float _x, float _y) : x(_x), y(_y) {
    }
};

struct ImVec4 {
    float x, y, z, w;
    constexpr ImVec4() : x(0.0f), y(0.0f), z(0.0f), w(0.0f) {
    }
    constexpr ImVec4(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {
    }
};

struct ImRect {
    ImVec2 Min, Max;
};

/* a member type only (GuiWindow's label stack): never grown here */
template <typename T> struct ImVector {
    int Size = 0;
    int Capacity = 0;
    T* Data = nullptr;
};

struct ImGuiIO;
struct ImGuiContext;
struct ImDrawData;
struct ImDrawList;
struct ImFont;
struct ImFontAtlas;
struct ImGuiStyle;
struct ImGuiWindow;
struct ImGuiViewport;
struct ImGuiInputTextCallbackData;
struct ImGuiSizeCallbackData;
struct ImGuiTableSortSpecs;
struct ImGuiPayload;

enum ImGuiWindowFlags_ {
    ImGuiWindowFlags_None = 0,
    ImGuiWindowFlags_NoTitleBar = 1 << 0,
    ImGuiWindowFlags_NoResize = 1 << 1,
    ImGuiWindowFlags_NoMove = 1 << 2,
    ImGuiWindowFlags_NoScrollbar = 1 << 3,
    ImGuiWindowFlags_NoScrollWithMouse = 1 << 4,
    ImGuiWindowFlags_NoCollapse = 1 << 5,
    ImGuiWindowFlags_AlwaysAutoResize = 1 << 6,
    ImGuiWindowFlags_NoBackground = 1 << 7,
    ImGuiWindowFlags_NoSavedSettings = 1 << 8,
    ImGuiWindowFlags_NoMouseInputs = 1 << 9,
    ImGuiWindowFlags_MenuBar = 1 << 10,
    ImGuiWindowFlags_HorizontalScrollbar = 1 << 11,
    ImGuiWindowFlags_NoFocusOnAppearing = 1 << 12,
    ImGuiWindowFlags_NoBringToFrontOnFocus = 1 << 13,
    ImGuiWindowFlags_AlwaysVerticalScrollbar = 1 << 14,
    ImGuiWindowFlags_AlwaysHorizontalScrollbar = 1 << 15,
    ImGuiWindowFlags_NoNavInputs = 1 << 16,
    ImGuiWindowFlags_NoNavFocus = 1 << 17,
    ImGuiWindowFlags_UnsavedDocument = 1 << 18,
    ImGuiWindowFlags_NoDocking = 1 << 19,
    ImGuiWindowFlags_NoNav = ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoNavFocus,
    ImGuiWindowFlags_NoDecoration = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse,
    ImGuiWindowFlags_NoInputs = ImGuiWindowFlags_NoMouseInputs | ImGuiWindowFlags_NoNavInputs |
                                ImGuiWindowFlags_NoNavFocus,
};
