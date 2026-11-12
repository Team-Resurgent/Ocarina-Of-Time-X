// lusx_menu.cpp -- the port's own menu, drawn over the game. Back, pressed and let go on its own, opens and closes it.
// After Diddy-Kong-Racing-X's dkr_menu.c.
//
// While it is open the game does not run at all: OTRGlobals.cpp's XboxRunMenu sits inside the tick, replaying the
// tick's recording with the menu over it, and goes back to the game once the menu has closed. Nothing in the game can
// tell. The audio thread makes nothing meanwhile, so the music stops and takes up where it was.
//
// The controls are SoH's own mappings (port 1's, what its input editor changes on a PC): a row shows the Xbox inputs
// on an N64 button, and they're kept in the config file with the rest. The Cheats and Options pages are SoH's settings
// (kCheatGroups, kOptionGroups), saved once as the menu closes.
#include <memory>
#include <stdio.h>
#include <string>

#include <SDL2/SDL.h>
#include <spdlog/spdlog.h>

#include "ship/Context.h"
#include "ship/config/ConsoleVariable.h"
#include "ship/controller/controldeck/ControlDeck.h"
#include "ship/controller/controldevice/controller/Controller.h"
#include "ship/controller/controldevice/controller/ControllerButton.h"
#include "ship/controller/controldevice/controller/mapping/sdl/SDLAxisDirectionToButtonMapping.h"
#include "ship/controller/controldevice/controller/mapping/sdl/SDLButtonToButtonMapping.h"
#include "libultraship/libultra/controller.h"
#include "libultraship/bridge/consolevariablebridge.h"
#include "soh/ShipInit.hpp"

// after the C++ headers: the Xbox's xtl.h defines min and max (and ignores NOMINMAX)
#include <xtl.h>

extern "C" void Audio_SetGameVolume(int player_id, float volume); // SoH's (include/z64audio.h)

#include "lusx_debug.h"
#include "lusx_log.h"
#include "lusx_menu.h"
#include "lusx_options.h"
#include "lusx_overlay.h"

// the layout, in lusx_overlay.h's 960x720
#define PANEL_X0 150.0f
#define PANEL_X1 810.0f
#define PANEL_Y0 60.0f
#define PANEL_Y1 676.0f
#define BORDER 4.0f
#define RADIUS 30.0f
#define HEADER_Y1 154.0f
#define TITLE_Y 107.0f
#define ROWS_Y 208.0f
#define ROW_STEP 58.0f
#define ROW_HALF 24.0f
#define LIST_Y 192.0f
#define LIST_STEP 46.0f
#define LIST_HALF 20.0f
#define LIST_ROWS 9 // of the controls page, on screen at once
#define ROW_X0 186.0f
#define ROW_X1 774.0f
#define LABEL_X 214.0f
#define VALUE_X 744.0f
#define FOOTER_Y 634.0f
#define BADGE_RADIUS 15.0f
#define ARROW_GAP 12.0f
#define ARROW_SIZE 9.0f
#define CENTRE_X ((PANEL_X0 + PANEL_X1) * 0.5f)

// colours, 0xAARRGGBB
#define C_DIM 0xA0000010u
#define C_SHADOW 0x70000000u
#define C_GOLD_TOP 0xFFFFDA55u
#define C_GOLD_BOTTOM 0xFFE09A18u
#define C_PANEL_TOP 0xF5382A6Eu
#define C_PANEL_BOTTOM 0xF5140E2Cu
#define C_HEADER_TOP 0xFF6A4CC0u
#define C_HEADER_BOTTOM 0xFF3A2A80u
#define C_HEADER_MIDDLE 0xFF4A3896u
#define C_ROW 0x16FFFFFFu
#define C_CHOSEN_TOP 0xF07A5CE0u
#define C_CHOSEN_BOTTOM 0xF04A32A8u
#define C_TEXT_SHADOW 0x90000000u
#define C_TITLE 0xFFFFFFFFu
#define C_LABEL 0xFFE8E4FFu
#define C_VALUE 0xFFC8B8FFu
#define C_CHOSEN 0xFFFFFFFFu
#define C_CHOSEN_VALUE 0xFFFFE060u
#define C_HINT 0xFFC4BCEEu
#define C_BADGE_A 0xFF3DB24Au
#define C_BADGE_B 0xFFD8443Cu
#define C_BADGE_X 0xFF3C78D8u

#define REPEAT_AFTER 24 // frames a direction is held before it repeats
#define REPEAT_EVERY 6
#define SCROLL_HOLD 60    // frames a long label waits at each end
#define SCROLL_SPEED 1.5f // and the layout's pixels it moves a frame

static const unsigned kMenuLeft = LUSX_PAD_LEFT | LUSX_PAD_L_LEFT, kMenuRight = LUSX_PAD_RIGHT | LUSX_PAD_L_RIGHT;

enum Page { PAGE_MAIN, PAGE_CONTROLS, PAGE_CAPTURE, PAGE_GROUPS, PAGE_LIST, PAGE_DEBUG, PAGE_CONFIRM };

enum MainRow { ROW_RESUME, ROW_CONTROLS, ROW_CHEATS, ROW_OPTIONS, ROW_DEBUG, ROW_RESTART, ROW_EXIT, ROW_COUNT };

// The warp test's two rows on the Debug page: 0 hides them (a build for playing), 1 shows them with Debug mode on (a
// build for a test run). The test itself is still in the game (ovl_select/z_select.c), with nothing to start it.
#define LUSX_WARP_TEST_ROWS 0

// the Debug page: the switches always shown first, then the tools Debug mode shows (Warp and Edit save), then Back
enum DebugRow {
    DEBUG_MODE,
    DEBUG_LOGGING, // lusx_log.h
#if LUSX_DEBUG_TOOLS
    DEBUG_OVERLAY,
#endif
    DEBUG_WARP,       // the Warp page: an area and its entrance (port_nogui.cpp)
#if LUSX_WARP_TEST_ROWS
    DEBUG_TEST,       // the warp test, from the number chosen on it (the last one loaded, after a crash)
    DEBUG_TEST_START, // the warp test, from the first
#endif
    DEBUG_EDIT,
    DEBUG_BACK,
    DEBUG_COUNT
};

// the N64's buttons, as the controls page lists them
struct N64Button {
    const char* name;
    CONTROLLERBUTTONS_T mask;
};
static const N64Button kButtons[] = {
    { "A", BTN_A },           { "B", BTN_B },           { "Z", BTN_Z },
    { "R", BTN_R },           { "L", BTN_L },           { "Start", BTN_START },
    { "C-Up", BTN_CUP },      { "C-Down", BTN_CDOWN },  { "C-Left", BTN_CLEFT },
    { "C-Right", BTN_CRIGHT }, { "D-Pad Up", BTN_DUP },  { "D-Pad Down", BTN_DDOWN },
    { "D-Pad Left", BTN_DLEFT }, { "D-Pad Right", BTN_DRIGHT },
};
enum {
    BUTTON_COUNT = sizeof(kButtons) / sizeof(kButtons[0]),
    LIST_RUMBLE = BUTTON_COUNT,
    LIST_RESET,
    LIST_BACK,
    LIST_COUNT
};

// the Xbox's inputs that can go on one. Not Back, which is the menu's, nor the left stick, which is the N64's stick.
struct XboxInput {
    unsigned bit;
    const char* name;
    int button;         // an SDL button, or
    int axis, direction; // an SDL axis' half
};
static const XboxInput kInputs[] = {
    { LUSX_PAD_A, "A", SDL_CONTROLLER_BUTTON_A, 0, 0 },
    { LUSX_PAD_B, "B", SDL_CONTROLLER_BUTTON_B, 0, 0 },
    { LUSX_PAD_X, "X", SDL_CONTROLLER_BUTTON_X, 0, 0 },
    { LUSX_PAD_Y, "Y", SDL_CONTROLLER_BUTTON_Y, 0, 0 },
    { LUSX_PAD_BLACK, "Black", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0, 0 },
    { LUSX_PAD_WHITE, "White", SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0, 0 },
    { LUSX_PAD_LT, "Left trigger", -1, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1 },
    { LUSX_PAD_RT, "Right trigger", -1, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 1 },
    { LUSX_PAD_START, "Start", SDL_CONTROLLER_BUTTON_START, 0, 0 },
    { LUSX_PAD_LS, "Left click", SDL_CONTROLLER_BUTTON_LEFTSTICK, 0, 0 },
    { LUSX_PAD_RS, "Right click", SDL_CONTROLLER_BUTTON_RIGHTSTICK, 0, 0 },
    { LUSX_PAD_UP, "D-Up", SDL_CONTROLLER_BUTTON_DPAD_UP, 0, 0 },
    { LUSX_PAD_DOWN, "D-Down", SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0, 0 },
    { LUSX_PAD_LEFT, "D-Left", SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0, 0 },
    { LUSX_PAD_RIGHT, "D-Right", SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0, 0 },
    { LUSX_PAD_R_UP, "Stick Up", -1, SDL_CONTROLLER_AXIS_RIGHTY, -1 },
    { LUSX_PAD_R_DOWN, "Stick Down", -1, SDL_CONTROLLER_AXIS_RIGHTY, 1 },
    { LUSX_PAD_R_LEFT, "Stick Left", -1, SDL_CONTROLLER_AXIS_RIGHTX, -1 },
    { LUSX_PAD_R_RIGHT, "Stick Right", -1, SDL_CONTROLLER_AXIS_RIGHTX, 1 },
};
enum { INPUT_COUNT = sizeof(kInputs) / sizeof(kInputs[0]) };

// The Cheats and Options pages: rows over SoH's own settings, the ones its PC menu changes (soh/SohGui/SohMenu*.cpp,
// by the same names). A setting's hooks are made again when it changes (ShipInit::Init), as on a PC.
#define OPT_SWITCH(label, cvar) { OPT_ON_OFF, label, cvar, 0, NULL, NULL, 0, -1, NULL }
#define OPT_SWITCH_ON(label, cvar) { OPT_ON_OFF, label, cvar, 1, NULL, NULL, 0, -1, NULL }
#define OPT_NAMES(label, cvar, names) { OPT_LIST, label, cvar, 0, names, NULL, ARRAY_LEN(names), -1, NULL }
#define OPT_VALUES(label, cvar, fallback, names, values) \
    { OPT_LIST, label, cvar, fallback, names, values, ARRAY_LEN(names), -1, NULL }
// SoH's volumes are percentages, with its own default (in tenths) when never set. A music player's is given to the
// audio engine as SoH's slider does (Audio_SetGameVolume); the master volume's ShipInit hook reads it again itself.
#define OPT_VOLUME(label, cvar, tenths, player) { OPT_PERCENT, label, cvar, tenths, NULL, NULL, 11, player, NULL }
#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

// SoH's multipliers are floats; a row here is a list, so each has an integer setting of its own (gLusx.*) and gives
// SoH's the chosen multiplier when it changes
#define CVAR_SPEED "gLusx.LinkSpeed"
#define CVAR_SPEED_VALUE "gCheats.SpeedModifier.Value"
#define CVAR_SPEED_TOGGLE "gCheats.SpeedModifier.SpeedToggle"
#define CVAR_BOMB_TIMER "gLusx.BombTimer"
#define CVAR_HOOKSHOT_REACH "gLusx.HookshotReach"

static const char* const kSpeedNames[] = { "Normal", "1.5x", "2x", "3x" };
static const float kSpeeds[] = { 1.0f, 1.5f, 2.0f, 3.0f };
static const char* const kBombTimerNames[] = { "Normal", "Half", "Double", "Triple" };
static const float kBombTimers[] = { 1.0f, 0.5f, 2.0f, 3.0f };
static const char* const kReachNames[] = { "Normal", "1.5x", "2x", "3x", "5x" };
static const float kReaches[] = { 1.0f, 1.5f, 2.0f, 3.0f, 5.0f };
static const char* const kDekuStickNames[] = { "Normal", "Unbreakable", "Unbreakable, always lit" };

static const char* const kBootNames[] = { "N64 logo", "Skip to file select" };
static const int kBootValues[] = { 1, 2 }; // SoH's BOOTSEQUENCE_AUTHENTIC, BOOTSEQUENCE_FILESELECT (enhancementTypes.h)
static const char* const kTextSpeedNames[] = { "Normal", "2x", "3x", "4x", "5x", "Instant" };
static const int kTextSpeedValues[] = { 1, 2, 3, 4, 5, 6 };
static const char* const kForcedDialogNames[] = { "None", "Navi", "Other people", "All" };
static const char* const kTimeTravelNames[] = { "Off", "Ocarina of Time", "OoT + Master Sword", "Any ocarina",
                                                "Any + Master Sword" };
static const char* const kDamageNames[] = { "Normal", "2x", "4x", "8x", "16x", "32x", "64x", "128x", "One hit" };
static const char* const kFrameCapNames[] = { "Auto", "40", "30", "20" };
static const int kFrameCapValues[] = { 60, 40, 30, 20 };
static const char* const kRumbleNames[] = { "Full", "Medium", "Low", "Off" };

// Link's speed, always on at the chosen multiplier: SoH's own needs a button held or toggled
// (SpeedModifiers.cpp reads gLusx.LinkSpeed on the Xbox)
static void AfterSpeed(void) {
    CVarSetFloat(CVAR_SPEED_VALUE, kSpeeds[CVarGetInteger(CVAR_SPEED, 0)]);
    CVarSetInteger(CVAR_SPEED_TOGGLE, 0);
    ShipInit::Init(CVAR_SPEED_VALUE);
}

static void AfterBombTimer(void) {
    CVarSetFloat("gCheats.BombTimerMultiplier", kBombTimers[CVarGetInteger(CVAR_BOMB_TIMER, 0)]);
}

static void AfterHookshotReach(void) {
    CVarSetFloat("gCheats.HookshotReachMultiplier", kReaches[CVarGetInteger(CVAR_HOOKSHOT_REACH, 0)]);
}

#define OPT_MULTIPLIER(label, cvar, names, after) { OPT_LIST, label, cvar, 0, names, NULL, ARRAY_LEN(names), -1, after }

// The Cheats page: SoH's Cheats menu (SohMenuEnhancements.cpp), less its glitch aids, save states, Beta Quest and
// the rows that are a button there (change age, clear the cutscene pointer)
static const Option kCheatInfinite[] = {
    OPT_SWITCH("Health", "gCheats.InfiniteHealth"),
    OPT_SWITCH("Magic", "gCheats.InfiniteMagic"),
    OPT_SWITCH("Rupees", "gCheats.InfiniteMoney"),
    OPT_SWITCH("Ammo", "gCheats.InfiniteAmmo"),
    OPT_SWITCH("Nayru's Love", "gCheats.InfiniteNayru"),
    OPT_SWITCH("Epona's carrots", "gCheats.InfiniteEponaBoost"),
};

static const Option kCheatItems[] = {
    OPT_SWITCH("Timeless equipment", "gCheats.TimelessEquipment"),
    OPT_SWITCH("Use any item anywhere", "gCheats.NoRestrictItems"),
    OPT_SWITCH("Super tunic", "gCheats.SuperTunic"),
    OPT_SWITCH("Fireproof Deku Shield", "gCheats.FireproofDekuShield"),
    OPT_SWITCH("Shield with two-handed weapons", "gCheats.ShieldTwoHanded"),
    OPT_NAMES("Deku Sticks", "gCheats.DekuStick", kDekuStickNames),
    OPT_MULTIPLIER("Bomb fuse", CVAR_BOMB_TIMER, kBombTimerNames, AfterBombTimer),
    OPT_SWITCH("Hookshot anything", "gCheats.HookshotEverything"),
    OPT_MULTIPLIER("Hookshot reach", CVAR_HOOKSHOT_REACH, kReachNames, AfterHookshotReach),
};

static const Option kCheatLink[] = {
    OPT_MULTIPLIER("Link's speed", CVAR_SPEED, kSpeedNames, AfterSpeed),
    OPT_SWITCH("No clip", "gCheats.NoClip"),
    OPT_SWITCH("Climb anything", "gCheats.ClimbEverything"),
    OPT_SWITCH("Moon jump (hold L)", "gCheats.MoonJumpOnL"),
};

static const Option kCheatWorld[] = {
    OPT_SWITCH("No ReDead/Gibdo freeze", "gCheats.NoRedeadFreeze"),
    OPT_SWITCH("Keese and Guay ignore you", "gCheats.NoKeeseGuayTarget"),
    OPT_SWITCH("No Wasteland sandstorm", "gCheats.DisableSandstorm"),
    OPT_SWITCH("Targetable Gold Skulltulas", "gCheats.GSTargetable"),
    OPT_SWITCH("Drops don't disappear", "gCheats.DropsDontDie"),
    OPT_SWITCH("Fish don't disappear", "gCheats.NoFishDespawn"),
    OPT_SWITCH("Bugs don't disappear", "gCheats.NoBugsDespawn"),
    OPT_SWITCH("Freeze time of day", "gCheats.FreezeTime"),
};

// on the Debug page; logged as the cheats are. As Lus-X's MM: it shows the Debug page's tools (Warp, the warp test,
// Edit save) and nothing else. The port's own setting, not SoH's gDeveloperTools.DebugEnabled, which turns on the
// PC's button combos (map select on L+R+Z, noclip, the L inventory editor in the pause menu, a debug save in file 1,
// the button display), whose places the port menu's Warp and Edit save take.
static const Option kDebugMode = OPT_SWITCH("Debug mode", "gLusx.DebugMode");
#define SOH_DEBUG_MODE "gDeveloperTools.DebugEnabled"

// on the Debug page, whatever Debug mode is: lusx_log.h. Off on a fresh install.
static const Option kLogging = { OPT_ON_OFF, "Logging", "gLusx.Logging", 0, NULL, NULL, 0, -1, LusxLog_Apply };

// the spdlog level that Logging's Off replaces, given back by On
static spdlog::level::level_enum sLogLevel = spdlog::level::info;

extern "C" void LusxLog_Apply(void) {
    static bool sSettled;
    const bool on = CVarGetInteger(kLogging.cvar, kLogging.fallback) != 0;
    const std::shared_ptr<spdlog::logger> logger = spdlog::default_logger();

    if (!sSettled) { // the first, as the config is read: the start-up's held lines go out, or are dropped
        sSettled = true;
        if (!on) {
            sLogLevel = logger->level();
            logger->set_level(spdlog::level::off);
        }
        LusxLog_Settle(on);
        return;
    }
    if (!on && gLusxLogOn) {
        SPDLOG_INFO("lus-x: logging off (port menu, Debug page)");
        sLogLevel = logger->level();
        logger->set_level(spdlog::level::off);
        LusxLog_Settle(0);
    } else if (on && !gLusxLogOn) {
        LusxLog_Settle(1);
        logger->set_level(sLogLevel);
        SPDLOG_INFO("lus-x: logging on (port menu, Debug page)");
    }
}

#if LUSX_DEBUG_TOOLS
// on the Debug page, whatever Debug mode is: the tester build's overlay, and the last run's report on it (lusx_debug.h).
// Saved, off on a fresh install.
static void ApplyOverlay(void) {
    LusxDebug_SetOverlay(CVarGetInteger("gLusx.TestOverlay", 0));
}
static const Option kTestOverlay = { OPT_ON_OFF, "Test overlay", "gLusx.TestOverlay", 0, NULL, NULL, 0, -1, ApplyOverlay };
#endif

// The Options page: SoH's Enhancements menu, the rows that make sense with only the normal game and no PC windows
static const Option kSaving[] = {
    OPT_SWITCH("Autosave", "gEnhancements.Autosave"),
    OPT_SWITCH("Remember save location", "gEnhancements.RememberSaveLocation"),
    OPT_SWITCH("Skip save confirmation", "gEnhancements.SkipSaveConfirmation"),
    OPT_SWITCH("\"Link\" as the default name", "gEnhancements.LinkDefaultName"),
};

static const Option kSkips[] = {
    // (the libultraship logo, SoH's PC default, isn't offered: 4 MB of textures; CustomLogoTitle.cpp)
    OPT_VALUES("At start-up", "gSettings.BootSequence", 1, kBootNames, kBootValues),
    OPT_SWITCH("Skip intro", "gEnhancements.TimeSavers.SkipCutscene.Intro"),
    OPT_SWITCH("Skip entrance cutscenes", "gEnhancements.TimeSavers.SkipCutscene.Entrances"),
    OPT_SWITCH("Skip story cutscenes", "gEnhancements.TimeSavers.SkipCutscene.Story"),
    OPT_SWITCH("Skip song cutscenes", "gEnhancements.TimeSavers.SkipCutscene.LearnSong"),
    OPT_SWITCH("Skip warp song cutscenes", "gEnhancements.WarpSongSkipAnimation"),
    OPT_SWITCH("Skip boss introductions", "gEnhancements.TimeSavers.SkipCutscene.BossIntro"),
    OPT_SWITCH("Quick boss deaths", "gEnhancements.TimeSavers.SkipCutscene.QuickBossDeaths"),
    OPT_SWITCH("Skip chest and door cutscenes", "gEnhancements.TimeSavers.SkipCutscene.OnePoint"),
    OPT_SWITCH("Skip owl talks", "gEnhancements.TimeSavers.SkipOwlInteractions"),
    OPT_SWITCH("Hide area title cards", "gEnhancements.TimeSavers.DisableTitleCard"),
    OPT_NAMES("Skip forced talks", "gEnhancements.TimeSavers.SkipForcedDialog", kForcedDialogNames),
};

static const Option kSpeedUps[] = {
    OPT_VALUES("Text speed", "gEnhancements.TextSpeed", 1, kTextSpeedNames, kTextSpeedValues),
    OPT_SWITCH("Hold B to skip text", "gEnhancements.SkipText"),
    OPT_SWITCH("No message for small pickups", "gEnhancements.FastDrops"),
    OPT_SWITCH("Fast chests", "gEnhancements.FastChests"),
    OPT_SWITCH("Fast ocarina playback", "gEnhancements.FastOcarinaPlayback"),
    OPT_SWITCH("Faster pause menu", "gEnhancements.FasterPauseMenu"),
    OPT_SWITCH("Faster rupee counter", "gEnhancements.FasterRupeeAccumulator"),
    OPT_SWITCH("Empty bottles faster", "gEnhancements.FasterBottleEmpty"),
    OPT_SWITCH("Improved roll", "gEnhancements.ImprovedRoll"),
    OPT_SWITCH("No breath animation in water", "gEnhancements.SkipSwimDeepEndAnim"),
};

static const Option kGameplay[] = {
    // SoH's default is on: up on the stick aims down, as on the N64
    OPT_SWITCH_ON("Invert aiming up and down", "gSettings.Controls.InvertAimingYAxis"),
    OPT_SWITCH("Invert aiming left and right", "gSettings.Controls.InvertAimingXAxis"),
    OPT_SWITCH("Instant putaway", "gEnhancements.InstantPutaway"),
    OPT_SWITCH("Equip items on the D-pad", "gEnhancements.DpadEquips"),
    OPT_SWITCH("Shields, tunics and boots on C", "gEnhancements.AssignableTunicsAndBoots"),
    OPT_SWITCH("Ask to equip new items", "gEnhancements.AskToEquip"),
    OPT_SWITCH("Bow as child, slingshot as adult", "gEnhancements.BowSlingshotAmmoFix"),
    OPT_SWITCH("Aiming reticle", "gEnhancements.BowReticle"),
    OPT_SWITCH("No explosive limit", "gEnhancements.RemoveExplosiveLimit"),
    OPT_NAMES("Song of Time age change", "gEnhancements.TimeTravel", kTimeTravelNames),
    OPT_SWITCH("Warp songs from the pause menu", "gEnhancements.PauseWarp"),
    OPT_SWITCH("Shops and games always open", "gEnhancements.OpenAllHours"),
    OPT_SWITCH("Dampe's grave pull by day", "gEnhancements.DayGravePull"),
    OPT_SWITCH("Dampe out all night", "gEnhancements.DampeAllNight"),
    OPT_SWITCH("Leave the market at night", "gEnhancements.MarketSneak"),
    OPT_SWITCH("Better owl", "gEnhancements.BetterOwl"),
    OPT_SWITCH("Link's cow in both times", "gEnhancements.CowOfTime"),
    OPT_NAMES("Damage taken", "gEnhancements.DamageMult", kDamageNames),
    OPT_SWITCH("Spawn with full health", "gEnhancements.FullHealthSpawn"),
    OPT_SWITCH("No heart drops", "gEnhancements.NoHeartDrops"),
    OPT_SWITCH("Always win the Goron pot", "gEnhancements.GoronPot"),
    OPT_SWITCH("Always win Dampe's digging", "gEnhancements.DampeWin"),
};

static const Option kDisplay[] = {
    OPT_VALUES("Frame rate limit", "gLusx.FrameCap", 60, kFrameCapNames, kFrameCapValues),
    OPT_SWITCH("Enemy health bars", "gEnhancements.EnemyHealthBar"),
    OPT_SWITCH("Visual Stone of Agony", "gEnhancements.VisualAgony"),
    OPT_SWITCH("Animated Link on the pause menu", "gEnhancements.PauseMenuAnimatedLink"),
    OPT_SWITCH("3D dropped items", "gEnhancements.NewDrops"),
    OPT_SWITCH("No black bars in cutscenes", "gEnhancements.DisableBlackBars"),
    OPT_SWITCH("Disable level of detail", "gEnhancements.DisableLOD"),
    OPT_SWITCH("No Kokiri fade at a distance", "gEnhancements.DisableKokiriDrawDistance"),
    OPT_SWITCH("No sword trail", "gEnhancements.DisableLinkSwordTrail"),
    OPT_SWITCH("Fix falling off vine edges", "gEnhancements.FixVineFall"),
    OPT_SWITCH("Fix camera swing", "gEnhancements.FixCameraSwing"),
    OPT_SWITCH("Fix two-handed idle poses", "gEnhancements.TwoHandedIdle"),
    OPT_SWITCH("Fix Navi's text position", "gEnhancements.NaviTextFix"),
    OPT_SWITCH("Fix Link's eyes when sleeping", "gEnhancements.FixEyesOpenWhileSleeping"),
    OPT_SWITCH("Fix the hand holding the hammer", "gEnhancements.FixHammerHand"),
    OPT_SWITCH("Fix Gerudo warriors' colours", "gEnhancements.GerudoWarriorClothingFix"),
    OPT_SWITCH("Fix the Poacher's Saw softlock", "gEnhancements.FixSawSoftlock"),
};

static const Option kSound[] = {
    // (the sequence players: include/sequence.h)
    OPT_VOLUME("Master volume", "gSettings.Volume.Master", 4, -1),
    OPT_VOLUME("Music", "gSettings.Volume.MainMusic", 10, 0),
    OPT_VOLUME("Second music track", "gSettings.Volume.SubMusic", 10, 3),
    OPT_VOLUME("Sound effects", "gSettings.Volume.SFX", 10, 2),
    OPT_VOLUME("Fanfares", "gSettings.Volume.Fanfare", 10, 1),
};

// on the controls page
static const Option kRumble = OPT_NAMES("Rumble", "gLusx.Rumble", kRumbleNames);

// (six at most: the page doesn't scroll)
static const OptionGroup kOptionGroups[] = {
    { "Saving", kSaving, ARRAY_LEN(kSaving) },
    { "Cutscenes", kSkips, ARRAY_LEN(kSkips) },
    { "Speed-ups", kSpeedUps, ARRAY_LEN(kSpeedUps) },
    { "Gameplay", kGameplay, ARRAY_LEN(kGameplay) },
    { "Display and fixes", kDisplay, ARRAY_LEN(kDisplay) },
    { "Sound", kSound, ARRAY_LEN(kSound) },
};
static const OptionGroup kCheatGroups[] = {
    { "Infinite", kCheatInfinite, ARRAY_LEN(kCheatInfinite) },
    { "Items", kCheatItems, ARRAY_LEN(kCheatItems) },
    { "Link", kCheatLink, ARRAY_LEN(kCheatLink) },
    { "Enemies and world", kCheatWorld, ARRAY_LEN(kCheatWorld) },
};

static volatile int sOpen;       // drawn and holding the game
static volatile unsigned sPad;   // the pads, as last polled
static volatile int sWanted;     // Back was pressed and let go
static int sBackDown, sBackAlone;
static int sClosing;             // closed, but the buttons that closed it are not yet let go
static Page sPage;
static int sCursor, sTop;        // the chosen row; the controls page's first on screen
static int sMainCursor;          // what the main page goes back to
static bool sAdd;                // the input being asked for goes on beside the ones there
static int sConfirmRow;          // the main page's row being confirmed
static int sHeldFor;             // frames a direction has been held
static unsigned sHeld;           // last frame's buttons
static unsigned sPressed;        // every button pressed since the menu opened
static std::string sValues[BUTTON_COUNT]; // what each N64 button is on, for the controls page
static const Option* sList;      // the list page's rows, its title and how many, with a Back row after them
static const char* sListTitle;
static int sListCount;
static const OptionGroup* sGroups; // the Options or Cheats page: its rows, how many, with a Back row after, its title
static int sGroupCount;
static const char* sGroupsTitle;
static int sGroupCursor;         // what the list page goes back to there
static bool sListFromDebug;      // the list page was opened from the Debug page (Warp)
static int sDebugCursor;         // what the pages opened from the Debug page go back to
static int sPinned;              // the list page's first rows that stay put while the rest scroll (Warp's settings)
static int sListRow;             // the row of the rest the cursor was last on
static int sScrollFrames;        // frames the cursor has been on this row, for a long label
static bool sChanged;            // settings to save when the menu closes
static int sStartWarpTest;       // the Debug page's warp test, to start as the menu closes from this one (LusxMenu_Close)
__attribute__((unused)) static int sWarpFrom; // the Debug page's warp test row's number
extern "C" void LusxWarpTest_Start(int first); // ovl_select/z_select.c
extern "C" int LusxWarpTest_Total(void);
extern "C" int LusxWarpTest_Last(void);

static std::shared_ptr<Ship::Controller> Pad1(void) {
    return Ship::Context::GetRawInstance()->GetControlDeck()->GetControllerByPort(0);
}

static std::shared_ptr<Ship::ControllerButtonMapping> MappingFor(const XboxInput& in, CONTROLLERBUTTONS_T mask) {
    if (in.button >= 0) {
        return std::make_shared<Ship::SDLButtonToButtonMapping>(0, mask, in.button);
    }
    return std::make_shared<Ship::SDLAxisDirectionToButtonMapping>(0, mask, in.axis, in.direction);
}

// the rows' text, in kInputs' order
static void ReadMappings(void) {
    auto pad = Pad1();

    for (int b = 0; b < BUTTON_COUNT; b++) {
        auto button = pad->GetButton(kButtons[b].mask);
        std::string& value = sValues[b];

        value.clear();
        for (int i = 0; i < INPUT_COUNT; i++) {
            if (button->GetButtonMappingById(MappingFor(kInputs[i], kButtons[b].mask)->GetButtonMappingId()) != nullptr) {
                value += value.empty() ? "" : ", ";
                value += kInputs[i].name;
            }
        }
        if (value.empty()) {
            value = "None";
        }
    }
}

static void HaveConfig(void) {
    Ship::Context::GetRawInstance()->GetConsoleVariables()->SetInteger(CVAR_PREFIX_CONTROLLERS ".Port1.HasConfig", 1);
}

static void Remap(int row, const XboxInput& in) {
    auto cvars = Ship::Context::GetRawInstance()->GetConsoleVariables();
    auto button = Pad1()->GetButton(kButtons[row].mask);
    auto mapping = MappingFor(in, kButtons[row].mask);

    cvars->HoldSaves(true); // each step of this saves the whole file
    if (!sAdd) {
        button->ClearAllButtonMappingsForDeviceType(Ship::PhysicalDeviceType::SDLGamepad);
    }
    button->AddButtonMapping(mapping);
    mapping->SaveToConfig();
    button->SaveButtonMappingIdsToConfig();
    HaveConfig();
    cvars->HoldSaves(false);
    ReadMappings();
}

static void ResetMappings(void) {
    auto cvars = Ship::Context::GetRawInstance()->GetConsoleVariables();
    auto pad = Pad1();

    cvars->HoldSaves(true);
    for (int b = 0; b < BUTTON_COUNT; b++) {
        auto button = pad->GetButton(kButtons[b].mask);
        button->ClearAllButtonMappingsForDeviceType(Ship::PhysicalDeviceType::SDLGamepad);
        button->AddDefaultMappings(Ship::PhysicalDeviceType::SDLGamepad);
    }
    HaveConfig();
    cvars->HoldSaves(false);
    ReadMappings();
}

// a row's setting as an index into its values
static int OptionIndex(const Option& o) {
    if (o.kind == OPT_PERCENT) {
        const int tenths = (CVarGetInteger(o.cvar, o.fallback * 10) + 5) / 10;
        return tenths < 0 ? 0 : tenths > 10 ? 10 : tenths;
    }
    const int value = CVarGetInteger(o.cvar, o.fallback);
    if (o.values != NULL) {
        for (int i = 0; i < o.count; i++) {
            if (o.values[i] == value) {
                return i;
            }
        }
        return 0;
    }
    return value;
}

// its value's text; `text` is for the ones that have to be made
static const char* OptionText(const Option& o, char* text, size_t size) {
    if (o.kind == OPT_CUSTOM || o.kind == OPT_ACTION) {
        return o.text(o.arg, text, size);
    }
    const int index = OptionIndex(o);

    if (o.kind == OPT_ON_OFF) {
        return index ? "On" : "Off";
    }
    if (o.kind == OPT_PERCENT) {
        snprintf(text, size, "%d%%", index * 10);
        return text;
    }
    return index >= 0 && index < o.count ? o.names[index] : "?";
}

static void ChangeOption(const Option& o, int step) {
    if (o.kind == OPT_CUSTOM || o.kind == OPT_ACTION) { // an edit of the save: a report from a run with them says so
        char mark[80], text[32];
        o.step(o.arg, step);
        snprintf(mark, sizeof(mark), "save edit: %s: %s", o.label, OptionText(o, text, sizeof(text)));
        LusxDebug_Mark(mark);
        return;
    }
    const int count = o.kind == OPT_ON_OFF ? 2 : o.count;
    const int index = (OptionIndex(o) + step + count) % count;

    if (o.kind == OPT_PERCENT) {
        CVarSetInteger(o.cvar, index * 10);
        if (o.seqPlayer >= 0) {
            Audio_SetGameVolume(o.seqPlayer, index / 10.0f);
        }
    } else {
        CVarSetInteger(o.cvar, o.values != NULL ? o.values[index] : index);
    }
    if (o.after != NULL) {
        o.after();
    }
    ShipInit::Init(o.cvar);
    sChanged = true;
    if (&o == &kDebugMode || (sList != NULL && sGroups == kCheatGroups)) { // a report from a run with cheats on says so
        char mark[80], text[32];
        snprintf(mark, sizeof(mark), "cheat: %s: %s", o.label, OptionText(o, text, sizeof(text)));
        LusxDebug_Mark(mark);
    }
}

static void SaveSettings(void) {
    if (sChanged) {
        sChanged = false;
        CVarSave();
    }
}

// the log says what's on at the start of a run too: the settings outlive it
void LusxMenu_LogCheats(void) {
    char text[32];

#if LUSX_DEBUG_TOOLS
    ApplyOverlay(); // the saved Test overlay (and with it, whether the last run's report is shown)
#endif
    // an earlier build's Debug mode was SoH's: not left on with its combos and no row to turn it off
    if (CVarGetInteger(SOH_DEBUG_MODE, 0) != 0) {
        CVarClear(SOH_DEBUG_MODE);
        CVarSave();
    }
    if (OptionIndex(kDebugMode) != 0) {
        SPDLOG_INFO("lus-x: cheat on: {}", kDebugMode.label);
    }
    for (const OptionGroup& group : kCheatGroups) {
        for (int i = 0; i < group.count; i++) {
            const Option& o = group.list[i];
            if (OptionIndex(o) != 0) {
                SPDLOG_INFO("lus-x: cheat on: {}: {}", o.label, OptionText(o, text, sizeof(text)));
            }
        }
    }
}

// what the game is left for: an empty image is the dashboard
static void Leave(const char* image, const char* why) {
    SaveSettings();
    LusxDebug_Mark(why);
    LusxDebug_Stop();
    XLaunchNewImage(image, NULL);
    XLaunchNewImage(NULL, NULL);
}

extern "C" void LusxMenu_Pad(unsigned buttons) {
    sPad = buttons;
    if (sOpen) {
        return;
    }
    if (buttons & LUSX_PAD_BACK) {
        if (!sBackDown) {
            sBackDown = 1;
            sBackAlone = 1;
        }
        // the tester tools' lock test starts with Back too
        if (buttons & (LUSX_PAD_LS | LUSX_PAD_RS)) {
            sBackAlone = 0;
        }
    } else if (sBackDown) {
        sBackDown = 0;
        sWanted = sBackAlone;
    }
}

extern "C" int LusxMenu_Wanted(void) {
    const int wanted = sWanted;
    sWanted = 0;
    return wanted;
}

extern "C" int LusxMenu_IsOpen(void) {
    return sOpen;
}

bool LusxMenu_Open(Fast::GfxRenderingAPI* rapi) {
    if (!Ovl_Load(rapi)) {
        return false;
    }
    sOpen = 1;
    sClosing = 0;
    sPage = PAGE_MAIN;
    sCursor = sMainCursor = 0;
    sTop = 0;
    sHeld = sPad; // what's held already isn't a press
    sPressed = 0;
    sHeldFor = 0;
    LusXbox_PadsQuiet();
    ReadMappings();
    LusxDebug_Mark("menu: opened");
    return true;
}

extern "C" void LusxMenu_Close(void) {
    SaveSettings();
    for (std::string& value : sValues) {
        std::string().swap(value);
    }
    Ovl_Unload();
    sOpen = 0;
    LusxDebug_Mark("menu: closed");
    LusxWarp_Run();
    if (sStartWarpTest > 0) {
        const int first = sStartWarpTest;
        sStartWarpTest = 0;
        LusxDebug_Mark("warp test: started from the menu");
        LusxWarpTest_Start(first);
    }
}

static void GoMain(void) {
    sPage = PAGE_MAIN;
    sCursor = sMainCursor;
}

static void GoList(const Option* list, int count, const char* title) {
    sPage = PAGE_LIST;
    sList = list;
    sListCount = count;
    sListTitle = title;
    sListFromDebug = false;
    sPinned = 0;
    sCursor = sTop = sListRow = 0;
}

// back to the page a list was opened from
static void GoGroups(void) {
    sPage = PAGE_GROUPS;
    sCursor = sGroupCursor;
}

static void GoDebug(void) {
    sPage = PAGE_DEBUG;
    sCursor = sDebugCursor;
    sList = NULL; // (Debug mode's row isn't in a list)
}

static void LeaveList(void) {
    if (sListFromDebug) {
        GoDebug();
    } else {
        GoGroups();
    }
}

// the Options and Cheats pages go back to the main page, Edit save's to the Debug page
static void LeaveGroups(void) {
    if (sGroups == kEditGroups) {
        GoDebug();
    } else {
        GoMain();
    }
}

static void OpenGroups(const OptionGroup* groups, int count, const char* title) {
    sGroups = groups;
    sGroupCount = count;
    sGroupsTitle = title;
    sGroupCursor = 0;
    GoGroups();
}

// Edit save is the save being played, by name (xbox/src/port/lusx_save_edit.cpp; "No save loaded" with none). Warp is an
// area and its entrance from the game's debug warp screen's list (port_nogui.cpp); the warp test, every entrance of
// every area there, one after another (ovl_select/z_select.c), each logged before it loads. They're debug tools: Debug
// mode shows them, as Lus-X's MM.
static bool ShowDebugRows(void) {
    return CVarGetInteger(kDebugMode.cvar, 0) != 0;
}

static bool IsDebugTool(int row) {
#if LUSX_WARP_TEST_ROWS
    if (row == DEBUG_TEST || row == DEBUG_TEST_START) {
        return true;
    }
#endif
    return row == DEBUG_WARP || row == DEBUG_EDIT;
}

static void UpdateDebug(unsigned pressed, int step) {
    if (step != 0) {
        do {
            sCursor = (sCursor + step + DEBUG_COUNT) % DEBUG_COUNT;
        } while (IsDebugTool(sCursor) && !ShowDebugRows());
    }
    if (pressed & LUSX_PAD_B) {
        GoMain();
        return;
    }
    if (sCursor == DEBUG_MODE) {
        if (pressed & (kMenuLeft | kMenuRight)) {
            ChangeOption(kDebugMode, 1);
        }
        return;
    }
    if (sCursor == DEBUG_LOGGING) {
        if (pressed & (kMenuLeft | kMenuRight)) {
            ChangeOption(kLogging, 1);
        }
        return;
    }
#if LUSX_DEBUG_TOOLS
    if (sCursor == DEBUG_OVERLAY) {
        if (pressed & (kMenuLeft | kMenuRight)) {
            ChangeOption(kTestOverlay, 1);
        }
        return;
    }
#endif
#if LUSX_WARP_TEST_ROWS
    if (sCursor == DEBUG_TEST && (pressed & (kMenuLeft | kMenuRight))) {
        const int total = LusxWarpTest_Total();
        sWarpFrom += (pressed & kMenuRight) ? 1 : -1;
        sWarpFrom = sWarpFrom < 1 ? total : sWarpFrom > total ? 1 : sWarpFrom;
        return;
    }
#endif
    if (!(pressed & LUSX_PAD_A)) {
        return;
    }
    sDebugCursor = sCursor;
    if (sCursor == DEBUG_WARP) {
        const Option* list;
        int count, cursor, pinned;

        LusxWarp_Open(&list, &count, &cursor, &pinned);
        GoList(list, count, "Warp");
        // on the age and time first; the list ready on the last warp, for X
        sPinned = pinned;
        sCursor = 0;
        sListRow = cursor;
        sTop = cursor - 2 > pinned ? cursor - 2 : pinned;
        sListFromDebug = true;
#if LUSX_WARP_TEST_ROWS
    } else if (sCursor == DEBUG_TEST || sCursor == DEBUG_TEST_START) {
        sStartWarpTest = sCursor == DEBUG_TEST ? sWarpFrom : 1;
        sClosing = 1;
#endif
    } else if (sCursor == DEBUG_EDIT) {
        if (LusxEdit_Available()) {
            OpenGroups(kEditGroups, kEditGroupCount, "Edit save");
        }
    } else {
        GoMain();
    }
}

static void UpdateMain(unsigned pressed, int step) {
    sCursor = (sCursor + step + ROW_COUNT) % ROW_COUNT;
    if (pressed & LUSX_PAD_B) {
        sClosing = 1;
    }
    if (!(pressed & LUSX_PAD_A)) {
        return;
    }
    sMainCursor = sCursor;
    switch (sCursor) {
        case ROW_RESUME:
            sClosing = 1;
            break;
        case ROW_CONTROLS:
            sPage = PAGE_CONTROLS;
            sCursor = sTop = 0;
            sList = NULL; // (Rumble's row isn't a cheat, whatever list was last open)
            break;
        case ROW_CHEATS:
            OpenGroups(kCheatGroups, ARRAY_LEN(kCheatGroups), "Cheats");
            break;
        case ROW_OPTIONS:
            OpenGroups(kOptionGroups, ARRAY_LEN(kOptionGroups), "Options");
            break;
        case ROW_DEBUG: {
#if LUSX_WARP_TEST_ROWS
            // the warp test's row: the one loading when the game last stopped, which a crash leaves saved
            const int last = LusxWarpTest_Last(), total = LusxWarpTest_Total();
            sWarpFrom = last >= 1 && last <= total ? last : 1;
            sDebugCursor = !ShowDebugRows() ? DEBUG_MODE : last >= 1 ? DEBUG_TEST : DEBUG_WARP;
#else
            sDebugCursor = !ShowDebugRows() ? DEBUG_MODE : DEBUG_WARP;
#endif
            GoDebug();
            break;
        }
        default:
            sPage = PAGE_CONFIRM;
            sConfirmRow = sCursor;
            sCursor = 0; // No: the safe answer first
            break;
    }
}

static void UpdateControls(unsigned pressed, int step) {
    sCursor = (sCursor + step + LIST_COUNT) % LIST_COUNT;
    sTop = sCursor < sTop ? sCursor : sCursor >= sTop + LIST_ROWS ? sCursor - LIST_ROWS + 1 : sTop;
    if (pressed & LUSX_PAD_B) {
        GoMain();
    } else if (sCursor == LIST_RUMBLE) {
        if (pressed & kMenuRight) {
            ChangeOption(kRumble, 1);
        } else if (pressed & kMenuLeft) {
            ChangeOption(kRumble, -1);
        }
    } else if (sCursor >= BUTTON_COUNT) {
        if (pressed & LUSX_PAD_A) {
            if (sCursor == LIST_RESET) {
                ResetMappings();
            } else {
                GoMain();
            }
        }
    } else if (pressed & (LUSX_PAD_A | LUSX_PAD_X)) {
        sAdd = (pressed & LUSX_PAD_A) == 0;
        sPage = PAGE_CAPTURE;
    }
}

static void UpdateGroups(unsigned pressed, int step) {
    sCursor = (sCursor + step + sGroupCount + 1) % (sGroupCount + 1);
    if (pressed & LUSX_PAD_B) {
        LeaveGroups();
    } else if (pressed & LUSX_PAD_A) {
        if (sCursor == sGroupCount) {
            LeaveGroups();
        } else {
            sGroupCursor = sCursor;
            GoList(sGroups[sCursor].list, sGroups[sCursor].count, sGroups[sCursor].title);
        }
    }
}

static void UpdateList(unsigned pressed, int step) {
    const int rows = sListCount + 1; // and Back
    const int shown = LIST_ROWS - sPinned; // the rows that scroll, at once

    if (sCursor < sPinned) {
        // the pinned rows and the rest are apart: up and down go round in each, and only X goes between them, back to
        // the row the list was left on
        if (pressed & LUSX_PAD_X) {
            sCursor = sListRow;
        } else {
            sCursor = (sCursor + step + sPinned) % sPinned;
        }
    } else {
        if (sPinned > 0 && (pressed & LUSX_PAD_X)) {
            sCursor = 0;
        } else {
            const int count = rows - sPinned;
            sCursor = sPinned + (sCursor - sPinned + step + count) % count;
        }
        // the triggers, a page at a time
        if (sCursor >= sPinned && (pressed & LUSX_PAD_LT)) {
            sCursor = sCursor - shown > sPinned ? sCursor - shown : sPinned;
        } else if (sCursor >= sPinned && (pressed & LUSX_PAD_RT)) {
            sCursor = sCursor + shown < rows ? sCursor + shown : rows - 1;
        }
    }
    if (sCursor >= sPinned) {
        sListRow = sCursor;
        sTop = sCursor < sTop ? sCursor : sCursor >= sTop + shown ? sCursor - shown + 1 : sTop;
    }
    if (pressed & LUSX_PAD_B) {
        LeaveList();
    } else if (sCursor == sListCount) {
        if (pressed & LUSX_PAD_A) {
            LeaveList();
        }
    } else if (sList[sCursor].press != NULL && (pressed & LUSX_PAD_A)) {
        char mark[80];

        snprintf(mark, sizeof(mark), "warp: %s", sList[sCursor].label);
        LusxDebug_Mark(mark);
        sList[sCursor].press(sList[sCursor].arg);
        sChanged = true;
        sClosing = 1;
    } else if (sList[sCursor].kind == OPT_ACTION) {
        if (pressed & LUSX_PAD_A) { // done once, on purpose
            ChangeOption(sList[sCursor], 1);
        }
    } else if (pressed & kMenuRight) {
        ChangeOption(sList[sCursor], 1);
    } else if (pressed & kMenuLeft) {
        ChangeOption(sList[sCursor], -1);
    }
}

static void UpdateCapture(unsigned pressed) {
    for (int i = 0; i < INPUT_COUNT; i++) {
        if (pressed & kInputs[i].bit) {
            Remap(sCursor, kInputs[i]);
            sPage = PAGE_CONTROLS;
            return;
        }
    }
}

static void UpdateConfirm(unsigned pressed, int step) {
    sCursor = (sCursor + step + 2) % 2;
    if (pressed & LUSX_PAD_B) {
        GoMain();
    } else if (pressed & LUSX_PAD_A) {
        if (sCursor == 0) {
            GoMain();
        } else if (sConfirmRow == ROW_RESTART) {
            Leave("D:\\default.xbe", "menu: restart");
        } else {
            Leave(NULL, "menu: to the dashboard");
        }
    }
}

extern "C" int LusxMenu_Update(void) {
    static const unsigned kUp = LUSX_PAD_UP | LUSX_PAD_L_UP, kDown = LUSX_PAD_DOWN | LUSX_PAD_L_DOWN;

    SDL_PumpEvents();
    const unsigned now = sPad, pressed = now & ~sHeld;
    sHeld = now;
    sPressed |= pressed;
    if (sClosing) {
        // back to the game only once the buttons pressed here are up: it would take one still held for a press of its
        // own. A button held from before the menu opened doesn't hold it up.
        return (now & sPressed) != 0;
    }
    // Back is the way out from anywhere, and all a capture can't take
    if (pressed & LUSX_PAD_BACK) {
        if (sPage == PAGE_CAPTURE) {
            sPage = PAGE_CONTROLS;
        } else {
            sClosing = 1;
        }
        return 1;
    }
    int step = (pressed & kDown) ? 1 : (pressed & kUp) ? -1 : 0;
    sHeldFor = (now & (kUp | kDown)) ? sHeldFor + 1 : 0;
    if (step == 0 && sHeldFor >= REPEAT_AFTER && (sHeldFor - REPEAT_AFTER) % REPEAT_EVERY == 0) {
        step = (now & kDown) ? 1 : -1;
    }
    switch (sPage) {
        case PAGE_MAIN:
            UpdateMain(pressed, step);
            break;
        case PAGE_CONTROLS:
            UpdateControls(pressed, step);
            break;
        case PAGE_CAPTURE:
            UpdateCapture(pressed);
            break;
        case PAGE_GROUPS:
            UpdateGroups(pressed, step);
            break;
        case PAGE_LIST:
            UpdateList(pressed, step);
            break;
        case PAGE_DEBUG:
            UpdateDebug(pressed, step);
            break;
        case PAGE_CONFIRM:
            UpdateConfirm(pressed, step);
            break;
    }
    return 1;
}

// text with a soft drop shadow under it
static void Text(int font, float x, float y, const char* s, uint32_t colour, int align) {
    Ovl_Text(font, x + 2.0f, y + 3.0f, s, C_TEXT_SHADOW, align);
    Ovl_Text(font, x, y, s, colour, align);
}

static void RowBack(float y, float half, bool chosen) {
    if (chosen) {
        Ovl_RoundRect(ROW_X0 - 3.0f, y - half - 3.0f, ROW_X1 + 3.0f, y + half + 3.0f, 17.0f, C_GOLD_TOP, C_GOLD_BOTTOM);
        Ovl_RoundRect(ROW_X0, y - half, ROW_X1, y + half, 14.0f, C_CHOSEN_TOP, C_CHOSEN_BOTTOM);
    } else {
        Ovl_RoundRect(ROW_X0, y - half, ROW_X1, y + half, 14.0f, C_ROW, C_ROW);
    }
}

// a row of one centred label
static void Row(float y, float half, const char* label, bool chosen) {
    RowBack(y, half, chosen);
    Text(OVL_FONT_BODY, CENTRE_X, y, label, chosen ? C_CHOSEN_VALUE : C_LABEL, OVL_ALIGN_CENTRE);
}

// how far a label `over` too long has scrolled: a second at the start, along it, a second at the end, and again
static float ScrollBy(float over) {
    const int hold = SCROLL_HOLD, along = (int)(over / SCROLL_SPEED) + 1;
    const int at = sScrollFrames % (hold + along + hold);

    return at < hold ? 0.0f : at < hold + along ? (at - hold) * SCROLL_SPEED : over;
}

// a row of a label and its value; `arrows`, chosen: left and right change it, and it says so
static void ValueRow(float y, float half, const char* label, const char* value, bool chosen, bool arrows = false) {
    // what the label has, clear of the value and its arrows
    const float room = VALUE_X - LABEL_X - Ovl_TextWidth(OVL_FONT_SMALL, value) - ARROW_GAP * 3.0f;
    const float over = Ovl_TextWidth(OVL_FONT_BODY, label) - room;

    RowBack(y, half, chosen);
    if (over > 0.0f) {
        // too long: cut off there, and on the chosen row, scrolled along to its end and back
        Ovl_Clip(LABEL_X, LABEL_X + room);
        Text(OVL_FONT_BODY, LABEL_X - (chosen ? ScrollBy(over) : 0.0f), y, label, chosen ? C_CHOSEN : C_LABEL,
             OVL_ALIGN_LEFT);
        Ovl_ClipOff();
    } else {
        Text(OVL_FONT_BODY, LABEL_X, y, label, chosen ? C_CHOSEN : C_LABEL, OVL_ALIGN_LEFT);
    }
    Text(OVL_FONT_SMALL, VALUE_X, y, value, chosen ? C_CHOSEN_VALUE : C_VALUE, OVL_ALIGN_RIGHT);
    if (chosen && arrows) {
        const float l = VALUE_X - Ovl_TextWidth(OVL_FONT_SMALL, value) - ARROW_GAP, r = VALUE_X + ARROW_GAP;

        Ovl_Tri(l, y - ARROW_SIZE, l, y + ARROW_SIZE, l - ARROW_SIZE * 1.2f, y, C_GOLD_TOP);
        Ovl_Tri(r, y - ARROW_SIZE, r + ARROW_SIZE * 1.2f, y, r, y + ARROW_SIZE, C_GOLD_TOP);
    }
}

// a pad button as the pad has it, and what it does: returns the width used
static float Hint(float x, const char* badge, uint32_t badgeColour, const char* what) {
    const float w = BADGE_RADIUS * 2.0f + 8.0f;

    Ovl_Circle(x + BADGE_RADIUS, FOOTER_Y, BADGE_RADIUS, badgeColour);
    Ovl_Text(OVL_FONT_SMALL, x + BADGE_RADIUS, FOOTER_Y, badge, 0xFFFFFFFFu, OVL_ALIGN_CENTRE);
    Ovl_Text(OVL_FONT_SMALL, x + w, FOOTER_Y, what, C_HINT, OVL_ALIGN_LEFT);
    return w + Ovl_TextWidth(OVL_FONT_SMALL, what);
}

static float HintWidth(const char* what) {
    return BADGE_RADIUS * 2.0f + 8.0f + Ovl_TextWidth(OVL_FONT_SMALL, what);
}

// for a row left and right change: two arrows, A and X if they do something too, and B
static void FooterChange(const char* what = "Change", const char* a = NULL, const char* xButton = NULL) {
    static const float kGap = 28.0f, kArrows = 34.0f;
    float x = CENTRE_X - (kArrows + Ovl_TextWidth(OVL_FONT_SMALL, what) + kGap + (a ? HintWidth(a) + kGap : 0.0f) +
                          (xButton ? HintWidth(xButton) + kGap : 0.0f) + HintWidth("Back")) * 0.5f;

    if (a != NULL) {
        x += Hint(x, "A", C_BADGE_A, a) + kGap;
    }
    Ovl_Tri(x + 10.0f, FOOTER_Y - 8.0f, x + 10.0f, FOOTER_Y + 8.0f, x, FOOTER_Y, C_HINT);
    Ovl_Tri(x + 16.0f, FOOTER_Y - 8.0f, x + 26.0f, FOOTER_Y, x + 16.0f, FOOTER_Y + 8.0f, C_HINT);
    Ovl_Text(OVL_FONT_SMALL, x + kArrows, FOOTER_Y, what, C_HINT, OVL_ALIGN_LEFT);
    x += kArrows + Ovl_TextWidth(OVL_FONT_SMALL, what) + kGap;
    if (xButton != NULL) {
        x += Hint(x, "X", C_BADGE_X, xButton) + kGap;
    }
    Hint(x, "B", C_BADGE_B, "Back");
}

static void Footer(const char* a, const char* x, const char* b) {
    static const float kGap = 34.0f;
    // measured first, to centre them
    float at = CENTRE_X - (HintWidth(a) + kGap + (x ? HintWidth(x) + kGap : 0.0f) + HintWidth(b)) * 0.5f;

    at += Hint(at, "A", C_BADGE_A, a) + kGap;
    if (x != NULL) {
        at += Hint(at, "X", C_BADGE_X, x) + kGap;
    }
    Hint(at, "B", C_BADGE_B, b);
}

static void Panel(const char* title) {
    // the game dimmed behind; the panel's shadow, gold rim and body
    Ovl_Rect(0.0f, 0.0f, OVL_W, OVL_H, C_DIM, C_DIM);
    Ovl_RoundRect(PANEL_X0 + 8.0f, PANEL_Y0 + 12.0f, PANEL_X1 + 8.0f, PANEL_Y1 + 12.0f, RADIUS, C_SHADOW, C_SHADOW);
    Ovl_RoundRect(PANEL_X0, PANEL_Y0, PANEL_X1, PANEL_Y1, RADIUS, C_GOLD_TOP, C_GOLD_BOTTOM);
    Ovl_RoundRect(PANEL_X0 + BORDER, PANEL_Y0 + BORDER, PANEL_X1 - BORDER, PANEL_Y1 - BORDER, RADIUS - BORDER,
                  C_PANEL_TOP, C_PANEL_BOTTOM);
    // the title's band: rounded at the top, square where it meets the body
    Ovl_RoundRect(PANEL_X0 + BORDER, PANEL_Y0 + BORDER, PANEL_X1 - BORDER, HEADER_Y1, RADIUS - BORDER, C_HEADER_TOP,
                  C_HEADER_BOTTOM);
    Ovl_Rect(PANEL_X0 + BORDER, HEADER_Y1 - RADIUS, PANEL_X1 - BORDER, HEADER_Y1, C_HEADER_MIDDLE, C_HEADER_BOTTOM);
    Ovl_Rect(PANEL_X0 + BORDER, HEADER_Y1, PANEL_X1 - BORDER, HEADER_Y1 + 3.0f, C_GOLD_TOP, C_GOLD_BOTTOM);
    Text(OVL_FONT_TITLE, CENTRE_X, TITLE_Y, title, C_TITLE, OVL_ALIGN_CENTRE);
}

static void DrawMain(void) {
    static const char* const kRows[ROW_COUNT] = { "Back to the game", "Controls",     "Cheats",           "Options",
                                                  "Debug",            "Restart game", "Exit to dashboard" };

    Panel("Ocarina Of Time-X");
    for (int i = 0; i < ROW_COUNT; i++) {
        Row(ROWS_Y + i * ROW_STEP, ROW_HALF, kRows[i], sCursor == i);
    }
    Footer("Select", NULL, "Back");
}

// more of a list above or below; `first`, the first row that scrolls
static void MoreMarks(int rows, int first = 0) {
    if (sTop > first) {
        const float y = first > 0 ? LIST_Y + first * LIST_STEP - 20.0f : 168.0f;
        Ovl_Tri(CENTRE_X - 12.0f, y, CENTRE_X, y - 10.0f, CENTRE_X + 12.0f, y, C_GOLD_TOP);
    }
    if (sTop + LIST_ROWS - first < rows) {
        const float y = LIST_Y + LIST_ROWS * LIST_STEP - 20.0f;
        Ovl_Tri(CENTRE_X - 12.0f, y, CENTRE_X + 12.0f, y, CENTRE_X, y + 10.0f, C_GOLD_TOP);
    }
}

static void DrawDebug(void) {
    char text[32];
    float y = ROWS_Y;

    Panel("Debug");
    ValueRow(y, ROW_HALF, kDebugMode.label, OptionText(kDebugMode, text, sizeof(text)), sCursor == DEBUG_MODE, true);
    y += ROW_STEP;
    ValueRow(y, ROW_HALF, kLogging.label, OptionText(kLogging, text, sizeof(text)), sCursor == DEBUG_LOGGING, true);
    y += ROW_STEP;
#if LUSX_DEBUG_TOOLS
    ValueRow(y, ROW_HALF, kTestOverlay.label, OptionText(kTestOverlay, text, sizeof(text)), sCursor == DEBUG_OVERLAY,
             true);
    y += ROW_STEP;
#endif
    if (ShowDebugRows()) {
        Row(y, ROW_HALF, "Warp", sCursor == DEBUG_WARP);
        y += ROW_STEP;
#if LUSX_WARP_TEST_ROWS
        snprintf(text, sizeof(text), "#%d of %d", sWarpFrom, LusxWarpTest_Total());
        ValueRow(y, ROW_HALF, "Warp test: from", text, sCursor == DEBUG_TEST, true);
        y += ROW_STEP;
        Row(y, ROW_HALF, "Warp test: from the start", sCursor == DEBUG_TEST_START);
        y += ROW_STEP;
#endif
        if (LusxEdit_Available()) {
            Row(y, ROW_HALF, "Edit save", sCursor == DEBUG_EDIT);
        } else {
            ValueRow(y, ROW_HALF, "Edit save", "No save loaded", sCursor == DEBUG_EDIT);
        }
        y += ROW_STEP;
    }
    Row(y, ROW_HALF, "Back", sCursor == DEBUG_BACK);
    if (sCursor == DEBUG_MODE || sCursor == DEBUG_LOGGING) {
        FooterChange();
#if LUSX_DEBUG_TOOLS
    } else if (sCursor == DEBUG_OVERLAY) {
        FooterChange();
#endif
#if LUSX_WARP_TEST_ROWS
    } else if (sCursor == DEBUG_TEST) {
        FooterChange("Change", "Start");
#endif
    } else {
        Footer("Select", NULL, "Back");
    }
}

static void DrawGroups(void) {
    Panel(sGroupsTitle);
    for (int i = 0; i < sGroupCount; i++) {
        Row(ROWS_Y + i * ROW_STEP, ROW_HALF, sGroups[i].title, sCursor == i);
    }
    Row(ROWS_Y + sGroupCount * ROW_STEP, ROW_HALF, "Back", sCursor == sGroupCount);
    Footer("Select", NULL, "Back");
}

static void DrawList(void) {
    const int rows = sListCount + 1;
    char text[16];

    Panel(sListTitle);
    for (int i = 0; i < LIST_ROWS; i++) {
        // the pinned rows where they are, then the rest from the top of the scroll
        const int row = i < sPinned ? i : sTop + i - sPinned;
        const float y = LIST_Y + i * LIST_STEP;
        const bool chosen = row == sCursor;

        if (row >= rows) {
            break;
        }
        if (row == sListCount) {
            Row(y, LIST_HALF, "Back", chosen);
        } else {
            ValueRow(y, LIST_HALF, sList[row].label, OptionText(sList[row], text, sizeof(text)), chosen,
                     sList[row].kind != OPT_ACTION);
        }
    }
    if (sPinned > 0) {
        const float line = LIST_Y + (sPinned - 0.5f) * LIST_STEP;
        Ovl_Rect(ROW_X0, line - 1.0f, ROW_X1, line + 1.0f, C_GOLD_TOP, C_GOLD_BOTTOM);
    }
    MoreMarks(rows, sPinned);
    if (sCursor == sListCount || sList[sCursor].kind == OPT_ACTION) {
        Footer("Select", NULL, "Back");
    } else if (sList[sCursor].press != NULL) {
        FooterChange("Entrance", "Warp", "Settings");
    } else if (sCursor < sPinned) {
        FooterChange("Change", NULL, "Areas");
    } else {
        FooterChange();
    }
}

static void DrawControls(void) {
    char text[16];

    Panel("Controls");
    for (int i = 0; i < LIST_ROWS && sTop + i < LIST_COUNT; i++) {
        const int row = sTop + i;
        const float y = LIST_Y + i * LIST_STEP;
        const bool chosen = row == sCursor;

        if (row == LIST_RUMBLE) {
            ValueRow(y, LIST_HALF, kRumble.label, OptionText(kRumble, text, sizeof(text)), chosen, true);
        } else if (row == LIST_RESET) {
            Row(y, LIST_HALF, "Reset to defaults", chosen);
        } else if (row == LIST_BACK) {
            Row(y, LIST_HALF, "Back", chosen);
        } else if (chosen && sPage == PAGE_CAPTURE) {
            ValueRow(y, LIST_HALF, kButtons[row].name, "Press a button", true);
        } else {
            ValueRow(y, LIST_HALF, kButtons[row].name, sValues[row].c_str(), chosen);
        }
    }
    MoreMarks(LIST_COUNT);
    if (sPage == PAGE_CAPTURE) {
        Ovl_Text(OVL_FONT_SMALL, CENTRE_X, FOOTER_Y, sAdd ? "Press the button to add. Back cancels."
                                                          : "Press the new button. Back cancels.",
                 C_HINT, OVL_ALIGN_CENTRE);
    } else if (sCursor < BUTTON_COUNT) {
        Footer("Change", "Add another", "Back");
    } else if (sCursor == LIST_RUMBLE) {
        FooterChange();
    } else {
        Footer("Select", NULL, "Back");
    }
}

static void DrawConfirm(void) {
    const bool restart = sConfirmRow == ROW_RESTART;

    Panel(restart ? "Restart game?" : "Exit to dashboard?");
    Text(OVL_FONT_BODY, CENTRE_X, ROWS_Y + 20.0f, "Anything not saved will be lost.", C_LABEL, OVL_ALIGN_CENTRE);
    Row(ROWS_Y + ROW_STEP * 2.0f, ROW_HALF, "No, go back", sCursor == 0);
    Row(ROWS_Y + ROW_STEP * 3.0f, ROW_HALF, restart ? "Yes, restart" : "Yes, exit", sCursor == 1);
    Footer("Select", NULL, "Back");
}

void LusxMenu_Draw(Fast::GfxRenderingAPI* rapi) {
    static int sLastCursor;
    static Page sLastPage;

    if (sClosing) {
        return;
    }
    sScrollFrames = sCursor == sLastCursor && sPage == sLastPage ? sScrollFrames + 1 : 0;
    sLastCursor = sCursor;
    sLastPage = sPage;
    Ovl_Begin();
    switch (sPage) {
        case PAGE_MAIN:
            DrawMain();
            break;
        case PAGE_CONTROLS:
        case PAGE_CAPTURE:
            DrawControls();
            break;
        case PAGE_GROUPS:
            DrawGroups();
            break;
        case PAGE_LIST:
            DrawList();
            break;
        case PAGE_DEBUG:
            DrawDebug();
            break;
        case PAGE_CONFIRM:
            DrawConfirm();
            break;
    }
}
