// lusx_options.h -- the rows of the port's menu (xbox/src/lusx_menu.cpp), and the Edit Save page's, which are in the
// game's group (xbox/src/port/lusx_save_edit.cpp) as they need its headers.
#pragma once

#include <stddef.h>

enum OptionKind {
    OPT_ON_OFF,  // an integer setting, 0 or 1
    OPT_LIST,    // an integer setting: one of `names`, its value `values[i]`, or i with no `values`
    OPT_PERCENT, // an integer setting, 0 to 100 in tens (SoH's volumes); `fallback` is its default in tenths
    OPT_CUSTOM,  // no setting: `step` changes something, by a press of left (-1) or right (1), `text` shows it
    OPT_ACTION,  // the same, but something done, by A
};

struct Option {
    OptionKind kind;
    const char* label;
    const char* cvar;
    int fallback; // the setting's value when it was never set: SoH's default for it
    const char* const* names;
    const int* values;
    int count;
    int seqPlayer;       // a volume: the audio engine's player it scales, or -1
    void (*after)(void); // what else the change needs
    int arg;             // OPT_CUSTOM's: what `step` and `text` are told
    void (*step)(int arg, int direction);
    const char* (*text)(int arg, char* buffer, size_t size);
    void (*press)(int arg); // A does this and closes the menu
};

// a page of lists: a row for each
struct OptionGroup {
    const char* title;
    const Option* list;
    int count;
};

// the Edit Save page, for when a save is being played
extern const OptionGroup kEditGroups[];
extern const int kEditGroupCount;
bool LusxEdit_Available(void);

// the Warp page: its rows, how many, the row to start on (the last warp) and how many stay put while the areas scroll;
// the warp itself, once the menu has closed
void LusxWarp_Open(const Option** list, int* count, int* cursor, int* pinned);
void LusxWarp_Run(void);
