// port_nogui.cpp -- Ship of Harkinian with no GUI: what the game calls in the files the Xbox build leaves out
// (tools/build.py's SOH_GUI_EXCLUDE), and the port menu's game pages. Game logic that lives in a GUI file is copied
// here unchanged; GUI-only features answer "none".
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include <fast/Fast3dGui.h>

#include "lusx_options.h"
#include "soh/Notification/Notification.h"
#include "soh/SohGui/SohMenu.h"
#include "soh/SohGui/SohGui.hpp"

extern "C" {
#include <z64.h>
}
#include "variables.h" // (gSaveContext: the Warp page's age and time of day)
#include <libultraship/bridge/consolevariablebridge.h>

// the layout libultraship's interpreter walks display lists by (tools/build.py: -mno-ms-bitfields)
static_assert(sizeof(Gfx) == 8, "Gfx must be the N64's 8 bytes on the Xbox's 32-bit pointers");

// Notification/Notification.cpp: its window draws them; here they go to the log
namespace Notification {
void Emit(Options notification) {
    SPDLOG_INFO("notification: {}{}{}", notification.prefix, notification.message, notification.suffix);
}
} // namespace Notification

// SohGui/: the ESC menu. Its widgets' registration (RegisterMenuInitFunc) runs only as the menu is set up, which it
// never is here, so mSohMenu stays null and AddWidget is never reached.
namespace SohGui {
std::shared_ptr<SohMenu> mSohMenu;

UIWidgets::Colors GetMenuThemeColor() {
    return UIWidgets::Colors::LightBlue;
}

void UpdateMenuLocations() {
}

void UpdateMenuTricks() {
}

WidgetInfo& SohMenu::AddWidget(WidgetPath& pathInfo, std::string widgetName, WidgetType widgetType) {
    static WidgetInfo sNone;
    return sNone;
}

void SohMenu::AddSidebarEntry(std::string sectionName, std::string sidbarName, uint32_t columnCount) {
}

// (the speedrun settings look through them: there are none)
std::vector<std::shared_ptr<Ship::GuiWindow>> GetAllGuiWindows() {
    return {};
}

// (the trackers' windows ask it to draw their settings rows; they are never drawn)
std::shared_ptr<SohMenu> GetSohMenu() {
    return mSohMenu;
}

// a popup is the PC menu's message box: here, the log
void RegisterPopup(std::string title, std::string message, std::string button1, std::string button2,
                   std::function<void()> button1callback, std::function<void()> button2callback) {
    SPDLOG_INFO("popup: {}: {}", title, message);
}
} // namespace SohGui

// SohGui/Menu.cpp: the menu's search list and its rows' drawing
namespace Ship {
void Menu::AddSearchWidget(SearchWidget widget) {
}

void Menu::MenuDrawItem(WidgetInfo& widget, UIWidgets::Colors menuThemeIndex) {
}
} // namespace Ship

// libultraship's Fast3dGui.cpp: the menus' icons (TimeDisplay, the plandomizer); no GUI, no textures
namespace Fast {
void Fast3dGui::LoadGuiTexture(const std::string& name, const std::string& path, const std::string& palettePath,
                               const ImVec4& tint) {
}

ImTextureID Fast3dGui::GetTextureByName(const std::string& name) {
    return nullptr;
}
} // namespace Fast

// SohGui/SohMenuEnhancements.cpp: Beta Quest, which z_play.c turns on and off
bool isBetaQuestEnabled = false;

extern "C" {
void enableBetaQuest() {
    isBetaQuestEnabled = true;
}
void disableBetaQuest() {
    isBetaQuestEnabled = false;
}
}

// ---- Warp (the port menu's Debug page, xbox/src/lusx_menu.cpp; its Edit save page is lusx_save_edit.cpp): SoH's debug warp screen's list (ovl_select/z_select.c's sBetterScenes), in its order: an area a row,
// left and right for its entrance (a dungeon's boss lair among them), A to go; the age and time of day above

extern "C" {
s32 LusxWarp_Areas(void);
const char* LusxWarp_AreaName(s32 area);
s32 LusxWarp_Entrances(s32 area);
const char* LusxWarp_EntranceName(s32 area, s32 entrance);
void LusxWarp_Go(s32 area, s32 entrance, s32 age, s32 time);
}

#define WARP_SETTINGS 2 // the rows before the areas
#define WARP_AREAS 50   // sBetterScenes's
#define WARP_TIMES (2 + 11) // day, night, cutscenes 0 to A (the warp screen's 0xFFF0 to 0xFFFA)
#define CVAR_WARP_AREA "gLusx.Warp.Area"
#define CVAR_WARP_ENTRANCE "gLusx.Warp.Entrance"

static Option sWarpRows[WARP_SETTINGS + WARP_AREAS];
static char sWarpLabels[WARP_AREAS][40];
static unsigned char sWarpEntrance[WARP_AREAS];
static int sWarpAge, sWarpTime;
static int sWarpTo = -1; // the area picked, for when the menu has closed

static int WarpWrap(int value, int low, int high) {
    return value < low ? high : value > high ? low : value;
}

static void StepWarpAge(int, int) {
    sWarpAge = sWarpAge == LINK_AGE_ADULT ? LINK_AGE_CHILD : LINK_AGE_ADULT;
}

static const char* TextWarpAge(int, char*, size_t) {
    return sWarpAge == LINK_AGE_ADULT ? "Adult" : "Child";
}

static void StepWarpTime(int, int direction) {
    sWarpTime = WarpWrap(sWarpTime + direction, 0, WARP_TIMES - 1);
}

static const char* TextWarpTime(int, char* buffer, size_t size) {
    if (sWarpTime < 2) {
        return sWarpTime == 0 ? "Day" : "Night";
    }
    snprintf(buffer, size, "Cutscene %X", sWarpTime - 2);
    return buffer;
}

static void StepWarpEntrance(int area, int direction) {
    sWarpEntrance[area] = WarpWrap(sWarpEntrance[area] + direction, 0, LusxWarp_Entrances(area) - 1);
}

static const char* TextWarpEntrance(int area, char*, size_t) {
    return LusxWarp_Entrances(area) > 0 ? LusxWarp_EntranceName(area, sWarpEntrance[area]) : "";
}

static void PressWarp(int area) {
    sWarpTo = area;
    CVarSetInteger(CVAR_WARP_AREA, area);
    CVarSetInteger(CVAR_WARP_ENTRANCE, sWarpEntrance[area]); // (saved as the menu closes)
}

static Option WarpSetting(const char* label, void (*step)(int, int), const char* (*text)(int, char*, size_t)) {
    Option row = {};
    row.kind = OPT_CUSTOM;
    row.label = label;
    row.seqPlayer = -1;
    row.step = step;
    row.text = text;
    return row;
}

void LusxWarp_Open(const Option** list, int* count, int* cursor, int* pinned) {
    const int areas = LusxWarp_Areas() < WARP_AREAS ? LusxWarp_Areas() : WARP_AREAS;
    int last = CVarGetInteger(CVAR_WARP_AREA, 0);
    last = last >= 0 && last < areas ? last : 0;

    sWarpRows[0] = WarpSetting("Age", StepWarpAge, TextWarpAge);
    sWarpRows[1] = WarpSetting("Time of day", StepWarpTime, TextWarpTime);
    for (int i = 0; i < areas; i++) {
        Option& row = sWarpRows[WARP_SETTINGS + i];

        snprintf(sWarpLabels[i], sizeof(sWarpLabels[i]), "%d  %s", i + 1, LusxWarp_AreaName(i));
        row = WarpSetting(sWarpLabels[i], StepWarpEntrance, TextWarpEntrance);
        row.arg = i;
        row.press = LusxWarp_Entrances(i) > 0 ? PressWarp : NULL;
    }
    const int entrance = CVarGetInteger(CVAR_WARP_ENTRANCE, 0);
    sWarpEntrance[last] = entrance >= 0 && entrance < LusxWarp_Entrances(last) ? entrance : 0;
    // the save's age and time of day, or a child's day with none loaded
    sWarpAge = gSaveContext.fileNum != 0xFF ? (gSaveContext.linkAge == LINK_AGE_ADULT ? LINK_AGE_ADULT : LINK_AGE_CHILD)
                                            : LINK_AGE_CHILD;
    sWarpTime = gSaveContext.fileNum != 0xFF && gSaveContext.nightFlag ? 1 : 0;
    *list = sWarpRows;
    *count = WARP_SETTINGS + areas;
    *cursor = WARP_SETTINGS + last;
    *pinned = WARP_SETTINGS;
}

void LusxWarp_Run(void) {
    if (sWarpTo >= 0) {
        LusxWarp_Go(sWarpTo, sWarpEntrance[sWarpTo], sWarpAge, sWarpTime);
        sWarpTo = -1;
    }
}
