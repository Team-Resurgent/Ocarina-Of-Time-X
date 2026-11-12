// sohx_normal_only.cpp -- Ship of Harkinian, the normal game only: what the randomizer's remaining code calls in the
// files tools/build.py leaves out (SOH_NORMAL_ONLY_EXCLUDE). File select offers no randomizer quest on the Xbox
// (FileSelectEnhancements.cpp), so no seed is ever made, and nothing here is reached by a normal save: these are the
// pieces that make or show a seed.
#include "soh/Enhancements/randomizer/location_access.h"
#include "soh/Enhancements/randomizer/static_data.h"
#include "soh/Enhancements/randomizer/randomizer_tracker_windows.h"

// location_access/*: the world graph's areas, filled in only to make a seed, shuffle entrances, or show the check
// tracker. Empty: the graph stays without them.
void RegionTable_Init_Root() {
}
void RegionTable_Init_KokiriForest() {
}
void RegionTable_Init_LostWoods() {
}
void RegionTable_Init_SacredForestMeadow() {
}
void RegionTable_Init_HyruleField() {
}
void RegionTable_Init_LakeHylia() {
}
void RegionTable_Init_LonLonRanch() {
}
void RegionTable_Init_Market() {
}
void RegionTable_Init_TempleOfTime() {
}
void RegionTable_Init_CastleGrounds() {
}
void RegionTable_Init_Kakariko() {
}
void RegionTable_Init_Graveyard() {
}
void RegionTable_Init_DeathMountainTrail() {
}
void RegionTable_Init_GoronCity() {
}
void RegionTable_Init_DeathMountainCrater() {
}
void RegionTable_Init_ZoraRiver() {
}
void RegionTable_Init_ZorasDomain() {
}
void RegionTable_Init_ZorasFountain() {
}
void RegionTable_Init_GerudoValley() {
}
void RegionTable_Init_GerudoFortress() {
}
void RegionTable_Init_HauntedWasteland() {
}
void RegionTable_Init_DesertColossus() {
}
void RegionTable_Init_DekuTree() {
}
void RegionTable_Init_DodongosCavern() {
}
void RegionTable_Init_JabuJabusBelly() {
}
void RegionTable_Init_ForestTemple() {
}
void RegionTable_Init_FireTemple() {
}
void RegionTable_Init_WaterTemple() {
}
void RegionTable_Init_SpiritTemple() {
}
void RegionTable_Init_ShadowTemple() {
}
void RegionTable_Init_BottomOfTheWell() {
}
void RegionTable_Init_IceCavern() {
}
void RegionTable_Init_ThievesHideout() {
}
void RegionTable_Init_GerudoTrainingGround() {
}
void RegionTable_Init_GanonsCastle() {
}

// 3drando/hint_list*: the hint texts a seed's gossip stones are given. Empty: no seed, no hints.
void Rando::StaticData::HintTable_Init() {
}

// randomizer_tracker_windows.cpp: the trackers' floating windows (a GUI, never drawn here)
namespace Trackers {
bool BeginFloatWindows(const std::string& UniqueName, bool* open, const Color_RGBA8& bgCol,
                       TrackerWindowType windowType, bool draggable, ImGuiWindowFlags flags) {
    return false;
}

void EndFloatWindows() {
}
} // namespace Trackers
