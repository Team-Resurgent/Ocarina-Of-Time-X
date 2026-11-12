// lusx_save_edit.cpp -- the port menu's Edit save page (Debug, xbox/src/lusx_menu.cpp): the save being played, by name.
// Each change is made the way SoH's PC save editor makes it (soh/Enhancements/debugger/debugSaveEditor.cpp, drawn with
// Dear ImGui, which this build hasn't); the game is stopped while the menu is open, so nothing else is touching any
// of it. Its pages are Lus-X's Majora's Mask one's (General, Items, Quest, Dungeons), with Ocarina of Time's things.
#include <stdio.h>

#include "lusx_options.h"

#include <z64.h>
#include "variables.h"
#include "functions.h"
#include "macros.h"

extern "C" {
extern PlayState* gPlayState;
}

#define ROW(label, arg, step, text) \
    { OPT_CUSTOM, label, NULL, 0, NULL, NULL, 0, -1, NULL, arg, step, text }
#define ACTION(label, step) \
    { OPT_ACTION, label, NULL, 0, NULL, NULL, 0, -1, NULL, 0, step, Select }
#define LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define ITEMS gSaveContext.inventory.items

static int Clamp(int v, int lowest, int highest) {
    return v < lowest ? lowest : v > highest ? highest : v;
}

static int Wrap(int v, int lowest, int highest) {
    return v < lowest ? highest : v > highest ? lowest : v;
}

static const char* Number(char* buffer, size_t size, int n) {
    snprintf(buffer, size, "%d", n);
    return buffer;
}

static const char* Select(int, char*, size_t) {
    return "Press A";
}

static const char* YesNo(bool yes) {
    return yes ? "Yes" : "No";
}

bool LusxEdit_Available(void) {
    return gPlayState != NULL && gSaveContext.gameMode == GAMEMODE_NORMAL && gSaveContext.fileNum != 0xFF;
}

// ---- General

static void StepMaxHearts(int, int direction) {
    gSaveContext.healthCapacity = Clamp(gSaveContext.healthCapacity / 16 + direction, 1, 20) * 16;
    gSaveContext.health = Clamp(gSaveContext.health, 0, gSaveContext.healthCapacity);
}

static const char* TextMaxHearts(int, char* buffer, size_t size) {
    return Number(buffer, size, gSaveContext.healthCapacity / 16);
}

static void StepHealth(int, int direction) {
    gSaveContext.health = Clamp(gSaveContext.health + direction * 16, 16, gSaveContext.healthCapacity);
}

static const char* TextHealth(int, char* buffer, size_t size) {
    return Number(buffer, size, gSaveContext.health / 16);
}

static void StepDefense(int, int) {
    gSaveContext.isDoubleDefenseAcquired = !gSaveContext.isDoubleDefenseAcquired;
    gSaveContext.inventory.defenseHearts = gSaveContext.isDoubleDefenseAcquired ? 20 : 0;
}

static const char* TextDefense(int, char*, size_t) {
    return gSaveContext.isDoubleDefenseAcquired ? "On" : "Off";
}

static void StepMagicLevel(int, int direction) {
    const int level = Clamp(gSaveContext.magicLevel + direction, 0, 2);

    gSaveContext.magicLevel = level;
    gSaveContext.isMagicAcquired = level >= 1;
    gSaveContext.isDoubleMagicAcquired = level == 2;
    gSaveContext.magicCapacity = level * 0x30;
    gSaveContext.magic = Clamp(gSaveContext.magic, 0, gSaveContext.magicCapacity);
}

static const char* TextMagicLevel(int, char*, size_t) {
    static const char* const kNames[] = { "None", "Single", "Double" };
    return kNames[Clamp(gSaveContext.magicLevel, 0, 2)];
}

static void StepMagic(int, int direction) {
    gSaveContext.magic = Clamp(gSaveContext.magic + direction * 4, 0, gSaveContext.magicCapacity);
}

static const char* TextMagic(int, char* buffer, size_t size) {
    return Number(buffer, size, gSaveContext.magic);
}

static void StepRupees(int, int direction) {
    gSaveContext.rupees = Clamp(gSaveContext.rupees + direction * 10, 0, CUR_CAPACITY(UPG_WALLET));
}

static const char* TextRupees(int, char* buffer, size_t size) {
    return Number(buffer, size, gSaveContext.rupees);
}

static void StepWallet(int, int direction) {
    Inventory_ChangeUpgrade(UPG_WALLET, Clamp(CUR_UPG_VALUE(UPG_WALLET) + direction, 0, 2));
    gSaveContext.rupees = Clamp(gSaveContext.rupees, 0, CUR_CAPACITY(UPG_WALLET));
}

static const char* TextWallet(int, char*, size_t) {
    static const char* const kNames[] = { "Child's (99)", "Adult's (200)", "Giant's (500)", "Tycoon's (999)" };
    return kNames[CUR_UPG_VALUE(UPG_WALLET) & 3];
}

// the day's clock: 0x10000 a day, midnight 0, noon 0x8000
static void StepTime(int minutes, int direction) {
    gSaveContext.dayTime = (u16)(gSaveContext.dayTime + direction * minutes * 0x10000 / (24 * 60));
}

static const char* TextTime(int, char* buffer, size_t size) {
    const int minutes = (int)((u32)gSaveContext.dayTime * (24 * 60) / 0x10000);
    const int hour = minutes / 60;

    snprintf(buffer, size, "%d:%02d %s", hour % 12 ? hour % 12 : 12, minutes % 60, hour >= 12 ? "pm" : "am");
    return buffer;
}

static const Option kGeneral[] = {
    ROW("Max hearts", 0, StepMaxHearts, TextMaxHearts),
    ROW("Hearts", 0, StepHealth, TextHealth),
    ROW("Double defense", 0, StepDefense, TextDefense),
    ROW("Magic meter", 0, StepMagicLevel, TextMagicLevel),
    ROW("Magic", 0, StepMagic, TextMagic),
    ROW("Rupees", 0, StepRupees, TextRupees),
    ROW("Wallet", 0, StepWallet, TextWallet),
    ROW("Time (1 hour steps)", 60, StepTime, TextTime),
    ROW("Time (15 minute steps)", 15, StepTime, TextTime),
};

// ---- Items

static const char* ItemName(int item) {
    switch (item) {
        case ITEM_NONE: return "None";
        case ITEM_OCARINA_FAIRY: return "Fairy Ocarina";
        case ITEM_OCARINA_TIME: return "Ocarina of Time";
        case ITEM_HOOKSHOT: return "Hookshot";
        case ITEM_LONGSHOT: return "Longshot";
        case ITEM_BOTTLE: return "Empty";
        case ITEM_POTION_RED: return "Red Potion";
        case ITEM_POTION_GREEN: return "Green Potion";
        case ITEM_POTION_BLUE: return "Blue Potion";
        case ITEM_FAIRY: return "Fairy";
        case ITEM_FISH: return "Fish";
        case ITEM_MILK_BOTTLE: return "Milk";
        case ITEM_MILK_HALF: return "Half Milk";
        case ITEM_LETTER_RUTO: return "Ruto's Letter";
        case ITEM_BLUE_FIRE: return "Blue Fire";
        case ITEM_BUG: return "Bugs";
        case ITEM_BIG_POE: return "Big Poe";
        case ITEM_POE: return "Poe";
        case ITEM_WEIRD_EGG: return "Weird Egg";
        case ITEM_CHICKEN: return "Chicken";
        case ITEM_LETTER_ZELDA: return "Zelda's Letter";
        case ITEM_MASK_KEATON: return "Keaton Mask";
        case ITEM_MASK_SKULL: return "Skull Mask";
        case ITEM_MASK_SPOOKY: return "Spooky Mask";
        case ITEM_MASK_BUNNY: return "Bunny Hood";
        case ITEM_MASK_GORON: return "Goron Mask";
        case ITEM_MASK_ZORA: return "Zora Mask";
        case ITEM_MASK_GERUDO: return "Gerudo Mask";
        case ITEM_MASK_TRUTH: return "Mask of Truth";
        case ITEM_SOLD_OUT: return "Sold Out";
        case ITEM_POCKET_EGG: return "Pocket Egg";
        case ITEM_POCKET_CUCCO: return "Pocket Cucco";
        case ITEM_COJIRO: return "Cojiro";
        case ITEM_ODD_MUSHROOM: return "Odd Mushroom";
        case ITEM_ODD_POTION: return "Odd Potion";
        case ITEM_SAW: return "Poacher's Saw";
        case ITEM_SWORD_BROKEN: return "Broken Goron's Sword";
        case ITEM_PRESCRIPTION: return "Prescription";
        case ITEM_FROG: return "Eyeball Frog";
        case ITEM_EYEDROPS: return "World's Finest Eye Drops";
        case ITEM_CLAIM_CHECK: return "Claim Check";
        default: return "Yes"; // a slot with one thing in it
    }
}

// what a slot can hold, nothing first: as the save editor's valid items (gItemSlots), the bottles' four slots one
static int SlotItems(int slot, u8* items) {
    const bool bottle = slot >= SLOT_BOTTLE_1 && slot <= SLOT_BOTTLE_4;
    const int wanted = bottle ? SLOT_BOTTLE_1 : slot;
    int n = 0;

    items[n++] = ITEM_NONE;
    for (int i = 0; i < LEN(gItemSlots); i++) {
        if (gItemSlots[i] == wanted && i != ITEM_SOLD_OUT) {
            items[n++] = (u8)i;
        }
    }
    return n;
}

static int AmmoLimit(int item) {
    int limit;

    switch (item) {
        case ITEM_STICK:
            limit = CUR_CAPACITY(UPG_STICKS);
            limit = limit > 0 ? limit : 10;
            break;
        case ITEM_NUT:
            limit = CUR_CAPACITY(UPG_NUTS);
            limit = limit > 0 ? limit : 20;
            break;
        case ITEM_BOMB:
            limit = CUR_CAPACITY(UPG_BOMB_BAG);
            break;
        case ITEM_BOW:
            limit = CUR_CAPACITY(UPG_QUIVER);
            break;
        case ITEM_SLINGSHOT:
            limit = CUR_CAPACITY(UPG_BULLET_BAG);
            break;
        case ITEM_BOMBCHU:
            limit = 50;
            break;
        case ITEM_BEAN:
            limit = 10;
            break;
        default:
            limit = 0;
            break;
    }
    return Clamp(limit, 0, 99);
}

// an item that's counted, full, on being given; the bag, quiver or capacity it needs first
static void Fill(int item) {
    if (item == ITEM_BOW && CUR_UPG_VALUE(UPG_QUIVER) == 0) {
        Inventory_ChangeUpgrade(UPG_QUIVER, 1);
    } else if (item == ITEM_SLINGSHOT && CUR_UPG_VALUE(UPG_BULLET_BAG) == 0) {
        Inventory_ChangeUpgrade(UPG_BULLET_BAG, 1);
    } else if (item == ITEM_BOMB && CUR_UPG_VALUE(UPG_BOMB_BAG) == 0) {
        Inventory_ChangeUpgrade(UPG_BOMB_BAG, 1);
    } else if (item == ITEM_STICK && CUR_UPG_VALUE(UPG_STICKS) == 0) {
        Inventory_ChangeUpgrade(UPG_STICKS, 1);
    } else if (item == ITEM_NUT && CUR_UPG_VALUE(UPG_NUTS) == 0) {
        Inventory_ChangeUpgrade(UPG_NUTS, 1);
    }
    if (AmmoLimit(item) > 0) {
        AMMO(item) = AmmoLimit(item);
    }
}

// as the save editor's SyncButtonItemsForSlot: a C or D-pad button with the slot on it shows what it now holds
static void SyncButtons(int slot) {
    for (int i = 1; i < (int)ARRAY_COUNT(gSaveContext.equips.buttonItems); i++) {
        if (gSaveContext.equips.cButtonSlots[i - 1] == slot) {
            gSaveContext.equips.buttonItems[i] = ITEMS[slot];
            Interface_LoadItemIcon1(gPlayState, (u16)i);
        }
    }
}

static void StepSlot(int slot, int direction) {
    u8 items[64];
    const int n = SlotItems(slot, items);
    const int current = ITEMS[slot];
    int at = 0;

    for (int i = 0; i < n; i++) {
        if (items[i] == current) {
            at = i;
        }
    }
    const int next = items[(at + direction + n) % n];
    if (next == current) {
        return;
    }
    if (next == ITEM_NONE) {
        if (AmmoLimit(current) > 0) {
            AMMO(current) = 0;
        }
        Inventory_DeleteItem(current, slot);
        return;
    }
    ITEMS[slot] = next;
    if (current == ITEM_NONE) {
        Fill(next);
    }
    SyncButtons(slot);
}

static const char* TextSlot(int slot, char*, size_t) {
    return ItemName(ITEMS[slot]);
}

static void StepAmmo(int item, int direction) {
    const int by = AmmoLimit(item) >= 20 ? 5 : 1;

    AMMO(item) = Clamp(AMMO(item) + direction * by, 0, AmmoLimit(item));
}

static const char* TextAmmo(int item, char* buffer, size_t size) {
    return Number(buffer, size, AMMO(item));
}

static void StepBag(int upgrade, int direction) {
    Inventory_ChangeUpgrade(upgrade, Clamp(CUR_UPG_VALUE(upgrade) + direction, 0, 3));
    for (int item : { ITEM_STICK, ITEM_NUT, ITEM_BOMB, ITEM_BOW, ITEM_SLINGSHOT }) {
        AMMO(item) = Clamp(AMMO(item), 0, AmmoLimit(item));
    }
}

static const char* TextBag(int upgrade, char* buffer, size_t size) {
    if (CUR_UPG_VALUE(upgrade) == 0) {
        return "None";
    }
    return Number(buffer, size, CUR_CAPACITY(upgrade));
}

static void GiveAllItems(int, int) {
    u8 items[64];

    for (int slot = SLOT_STICK; slot <= SLOT_TRADE_CHILD; slot++) {
        if (ITEMS[slot] == ITEM_NONE) {
            const bool bottle = slot >= SLOT_BOTTLE_1 && slot <= SLOT_BOTTLE_4;
            ITEMS[slot] = bottle ? ITEM_BOTTLE : items[SlotItems(slot, items) - 1];
            SyncButtons(slot);
        }
    }
    for (int upgrade : { UPG_QUIVER, UPG_BOMB_BAG, UPG_BULLET_BAG, UPG_STICKS, UPG_NUTS }) {
        Inventory_ChangeUpgrade(upgrade, 3);
    }
    for (int item : { ITEM_STICK, ITEM_NUT, ITEM_BOMB, ITEM_BOW, ITEM_SLINGSHOT, ITEM_BOMBCHU, ITEM_BEAN }) {
        Fill(item);
    }
}

static const Option kItems[] = {
    ACTION("Give all items", GiveAllItems),
    ROW("Deku Sticks", SLOT_STICK, StepSlot, TextSlot),
    ROW("Stick count", ITEM_STICK, StepAmmo, TextAmmo),
    ROW("Stick capacity", UPG_STICKS, StepBag, TextBag),
    ROW("Deku Nuts", SLOT_NUT, StepSlot, TextSlot),
    ROW("Nut count", ITEM_NUT, StepAmmo, TextAmmo),
    ROW("Nut capacity", UPG_NUTS, StepBag, TextBag),
    ROW("Bombs", SLOT_BOMB, StepSlot, TextSlot),
    ROW("Bomb count", ITEM_BOMB, StepAmmo, TextAmmo),
    ROW("Bomb Bag", UPG_BOMB_BAG, StepBag, TextBag),
    ROW("Fairy Bow", SLOT_BOW, StepSlot, TextSlot),
    ROW("Arrows", ITEM_BOW, StepAmmo, TextAmmo),
    ROW("Quiver", UPG_QUIVER, StepBag, TextBag),
    ROW("Fire Arrow", SLOT_ARROW_FIRE, StepSlot, TextSlot),
    ROW("Ice Arrow", SLOT_ARROW_ICE, StepSlot, TextSlot),
    ROW("Light Arrow", SLOT_ARROW_LIGHT, StepSlot, TextSlot),
    ROW("Fairy Slingshot", SLOT_SLINGSHOT, StepSlot, TextSlot),
    ROW("Deku Seeds", ITEM_SLINGSHOT, StepAmmo, TextAmmo),
    ROW("Bullet Bag", UPG_BULLET_BAG, StepBag, TextBag),
    ROW("Ocarina", SLOT_OCARINA, StepSlot, TextSlot),
    ROW("Bombchus", SLOT_BOMBCHU, StepSlot, TextSlot),
    ROW("Bombchu count", ITEM_BOMBCHU, StepAmmo, TextAmmo),
    ROW("Hookshot", SLOT_HOOKSHOT, StepSlot, TextSlot),
    ROW("Boomerang", SLOT_BOOMERANG, StepSlot, TextSlot),
    ROW("Lens of Truth", SLOT_LENS, StepSlot, TextSlot),
    ROW("Magic Beans", SLOT_BEAN, StepSlot, TextSlot),
    ROW("Bean count", ITEM_BEAN, StepAmmo, TextAmmo),
    ROW("Megaton Hammer", SLOT_HAMMER, StepSlot, TextSlot),
    ROW("Din's Fire", SLOT_DINS_FIRE, StepSlot, TextSlot),
    ROW("Farore's Wind", SLOT_FARORES_WIND, StepSlot, TextSlot),
    ROW("Nayru's Love", SLOT_NAYRUS_LOVE, StepSlot, TextSlot),
    ROW("Bottle 1", SLOT_BOTTLE_1, StepSlot, TextSlot),
    ROW("Bottle 2", SLOT_BOTTLE_2, StepSlot, TextSlot),
    ROW("Bottle 3", SLOT_BOTTLE_3, StepSlot, TextSlot),
    ROW("Bottle 4", SLOT_BOTTLE_4, StepSlot, TextSlot),
    ROW("Child trade item", SLOT_TRADE_CHILD, StepSlot, TextSlot),
    ROW("Adult trade item", SLOT_TRADE_ADULT, StepSlot, TextSlot),
};

// ---- Equipment: what's owned (the pause menu's equipment page), as the save editor's toggles; `arg` the type and
// the item, type * 4 + item

#define EQUIP_ARG(type, item) ((type) * 4 + (item))

static void StepOwned(int arg, int) {
    gSaveContext.inventory.equipment ^= OWNED_EQUIP_FLAG(arg / 4, arg % 4);
}

static const char* TextOwned(int arg, char*, size_t) {
    return YesNo(CHECK_OWNED_EQUIP(arg / 4, arg % 4) != 0);
}

// the third sword: the Giant's Knife, Biggoron's (the knife, with bgsFlag), the broken knife
static int ThirdSword(void) {
    if (CHECK_OWNED_EQUIP(EQUIP_TYPE_SWORD, EQUIP_INV_SWORD_BROKENGIANTKNIFE)) {
        return 3;
    }
    if (CHECK_OWNED_EQUIP(EQUIP_TYPE_SWORD, EQUIP_INV_SWORD_BIGGORON)) {
        return gSaveContext.bgsFlag ? 2 : 1;
    }
    return 0;
}

static void StepThirdSword(int, int direction) {
    const int next = Wrap(ThirdSword() + direction, 0, 3);

    gSaveContext.inventory.equipment &= ~(OWNED_EQUIP_FLAG(EQUIP_TYPE_SWORD, EQUIP_INV_SWORD_BIGGORON) |
                                          OWNED_EQUIP_FLAG(EQUIP_TYPE_SWORD, EQUIP_INV_SWORD_BROKENGIANTKNIFE));
    gSaveContext.bgsFlag = next == 2;
    if (next == 1 || next == 2) {
        gSaveContext.inventory.equipment |= OWNED_EQUIP_FLAG(EQUIP_TYPE_SWORD, EQUIP_INV_SWORD_BIGGORON);
        gSaveContext.swordHealth = 8; // (the knife's: the game's at its getting it)
    } else if (next == 3) {
        gSaveContext.inventory.equipment |= OWNED_EQUIP_FLAG(EQUIP_TYPE_SWORD, EQUIP_INV_SWORD_BROKENGIANTKNIFE);
        gSaveContext.swordHealth = 0;
    }
}

static const char* TextThirdSword(int, char*, size_t) {
    static const char* const kNames[] = { "None", "Giant's Knife", "Biggoron's Sword", "Broken Knife" };
    return kNames[ThirdSword()];
}

static void StepUpgrade(int upgrade, int direction) {
    Inventory_ChangeUpgrade(upgrade, Clamp(CUR_UPG_VALUE(upgrade) + direction, 0, upgrade == UPG_SCALE ? 2 : 3));
}

static const char* TextUpgrade(int upgrade, char*, size_t) {
    static const char* const kStrength[] = { "None", "Goron's Bracelet", "Silver Gauntlets", "Golden Gauntlets" };
    static const char* const kScale[] = { "None", "Silver Scale", "Golden Scale", "Golden Scale" };
    const int value = CUR_UPG_VALUE(upgrade) & 3;

    return upgrade == UPG_SCALE ? kScale[value] : kStrength[value];
}

static void GiveAllEquipment(int, int) {
    for (int type = EQUIP_TYPE_SWORD; type <= EQUIP_TYPE_BOOTS; type++) {
        for (int item = 0; item < 3; item++) {
            gSaveContext.inventory.equipment |= OWNED_EQUIP_FLAG(type, item);
        }
    }
    gSaveContext.inventory.equipment &= ~OWNED_EQUIP_FLAG(EQUIP_TYPE_SWORD, EQUIP_INV_SWORD_BROKENGIANTKNIFE);
    gSaveContext.bgsFlag = 1;
    gSaveContext.swordHealth = 8;
    Inventory_ChangeUpgrade(UPG_STRENGTH, 3);
    Inventory_ChangeUpgrade(UPG_SCALE, 2);
}

static const Option kEquipment[] = {
    ACTION("Give all equipment", GiveAllEquipment),
    ROW("Kokiri Sword", EQUIP_ARG(EQUIP_TYPE_SWORD, EQUIP_INV_SWORD_KOKIRI), StepOwned, TextOwned),
    ROW("Master Sword", EQUIP_ARG(EQUIP_TYPE_SWORD, EQUIP_INV_SWORD_MASTER), StepOwned, TextOwned),
    ROW("Third sword", 0, StepThirdSword, TextThirdSword),
    ROW("Deku Shield", EQUIP_ARG(EQUIP_TYPE_SHIELD, EQUIP_INV_SHIELD_DEKU), StepOwned, TextOwned),
    ROW("Hylian Shield", EQUIP_ARG(EQUIP_TYPE_SHIELD, EQUIP_INV_SHIELD_HYLIAN), StepOwned, TextOwned),
    ROW("Mirror Shield", EQUIP_ARG(EQUIP_TYPE_SHIELD, EQUIP_INV_SHIELD_MIRROR), StepOwned, TextOwned),
    ROW("Kokiri Tunic", EQUIP_ARG(EQUIP_TYPE_TUNIC, EQUIP_INV_TUNIC_KOKIRI), StepOwned, TextOwned),
    ROW("Goron Tunic", EQUIP_ARG(EQUIP_TYPE_TUNIC, EQUIP_INV_TUNIC_GORON), StepOwned, TextOwned),
    ROW("Zora Tunic", EQUIP_ARG(EQUIP_TYPE_TUNIC, EQUIP_INV_TUNIC_ZORA), StepOwned, TextOwned),
    ROW("Kokiri Boots", EQUIP_ARG(EQUIP_TYPE_BOOTS, EQUIP_INV_BOOTS_KOKIRI), StepOwned, TextOwned),
    ROW("Iron Boots", EQUIP_ARG(EQUIP_TYPE_BOOTS, EQUIP_INV_BOOTS_IRON), StepOwned, TextOwned),
    ROW("Hover Boots", EQUIP_ARG(EQUIP_TYPE_BOOTS, EQUIP_INV_BOOTS_HOVER), StepOwned, TextOwned),
    ROW("Strength", UPG_STRENGTH, StepUpgrade, TextUpgrade),
    ROW("Scale", UPG_SCALE, StepUpgrade, TextUpgrade),
};

// ---- Quest

static void StepQuest(int item, int) {
    gSaveContext.inventory.questItems ^= gBitFlags[item];
}

static const char* TextQuest(int item, char*, size_t) {
    return YesNo(CHECK_QUEST_ITEM(item) != 0);
}

// the pieces towards the next heart: the top four bits
static void StepHeartPieces(int, int direction) {
    const u32 count = (u32)Wrap((int)((gSaveContext.inventory.questItems & 0xF0000000) >> 28) + direction, 0, 3);

    gSaveContext.inventory.questItems = (gSaveContext.inventory.questItems & ~0xF0000000) | (count << 28);
}

static const char* TextHeartPieces(int, char* buffer, size_t size) {
    return Number(buffer, size, (int)((gSaveContext.inventory.questItems & 0xF0000000) >> 28));
}

// the count, and the token on the quest page with one got
static void StepTokens(int, int direction) {
    gSaveContext.inventory.gsTokens = Clamp(gSaveContext.inventory.gsTokens + direction, 0, 100);
    if (gSaveContext.inventory.gsTokens > 0) {
        gSaveContext.inventory.questItems |= gBitFlags[QUEST_SKULL_TOKEN];
    }
}

static const char* TextTokens(int, char* buffer, size_t size) {
    return Number(buffer, size, gSaveContext.inventory.gsTokens);
}

static void GiveAllQuest(int, int) {
    for (int i = QUEST_MEDALLION_FOREST; i <= QUEST_GERUDO_CARD; i++) {
        gSaveContext.inventory.questItems |= gBitFlags[i];
    }
}

static const Option kQuest[] = {
    ACTION("Give all quest items", GiveAllQuest),
    ROW("Kokiri Emerald", QUEST_KOKIRI_EMERALD, StepQuest, TextQuest),
    ROW("Goron Ruby", QUEST_GORON_RUBY, StepQuest, TextQuest),
    ROW("Zora Sapphire", QUEST_ZORA_SAPPHIRE, StepQuest, TextQuest),
    ROW("Forest Medallion", QUEST_MEDALLION_FOREST, StepQuest, TextQuest),
    ROW("Fire Medallion", QUEST_MEDALLION_FIRE, StepQuest, TextQuest),
    ROW("Water Medallion", QUEST_MEDALLION_WATER, StepQuest, TextQuest),
    ROW("Spirit Medallion", QUEST_MEDALLION_SPIRIT, StepQuest, TextQuest),
    ROW("Shadow Medallion", QUEST_MEDALLION_SHADOW, StepQuest, TextQuest),
    ROW("Light Medallion", QUEST_MEDALLION_LIGHT, StepQuest, TextQuest),
    ROW("Zelda's Lullaby", QUEST_SONG_LULLABY, StepQuest, TextQuest),
    ROW("Epona's Song", QUEST_SONG_EPONA, StepQuest, TextQuest),
    ROW("Saria's Song", QUEST_SONG_SARIA, StepQuest, TextQuest),
    ROW("Sun's Song", QUEST_SONG_SUN, StepQuest, TextQuest),
    ROW("Song of Time", QUEST_SONG_TIME, StepQuest, TextQuest),
    ROW("Song of Storms", QUEST_SONG_STORMS, StepQuest, TextQuest),
    ROW("Minuet of Forest", QUEST_SONG_MINUET, StepQuest, TextQuest),
    ROW("Bolero of Fire", QUEST_SONG_BOLERO, StepQuest, TextQuest),
    ROW("Serenade of Water", QUEST_SONG_SERENADE, StepQuest, TextQuest),
    ROW("Requiem of Spirit", QUEST_SONG_REQUIEM, StepQuest, TextQuest),
    ROW("Nocturne of Shadow", QUEST_SONG_NOCTURNE, StepQuest, TextQuest),
    ROW("Prelude of Light", QUEST_SONG_PRELUDE, StepQuest, TextQuest),
    ROW("Stone of Agony", QUEST_STONE_OF_AGONY, StepQuest, TextQuest),
    ROW("Gerudo's Card", QUEST_GERUDO_CARD, StepQuest, TextQuest),
    ROW("Heart pieces", 0, StepHeartPieces, TextHeartPieces),
    ROW("Gold Skulltula tokens", 0, StepTokens, TextTokens),
};

// ---- Dungeons: `arg` is the dungeon (its scene), and for the dungeon items the item too

#define DUNGEON_ARG(dungeon, item) ((dungeon) | ((item) << 5))

static void StepDungeonItem(int arg, int) {
    gSaveContext.inventory.dungeonItems[arg & 31] ^= gBitFlags[arg >> 5];
}

static const char* TextDungeonItem(int arg, char*, size_t) {
    return YesNo(CHECK_DUNGEON_ITEM(arg >> 5, arg & 31) != 0);
}

// the most small keys a dungeon has (the save editor asks the randomizer's dungeon table; these are the game's)
static int MostKeys(int dungeon) {
    switch (dungeon) {
        case SCENE_FOREST_TEMPLE: return 5;
        case SCENE_FIRE_TEMPLE: return 8;
        case SCENE_WATER_TEMPLE: return 6;
        case SCENE_SPIRIT_TEMPLE: return 5;
        case SCENE_SHADOW_TEMPLE: return 5;
        case SCENE_BOTTOM_OF_THE_WELL: return 3;
        case SCENE_GERUDO_TRAINING_GROUND: return 9;
        case SCENE_THIEVES_HIDEOUT: return 4;
        case SCENE_INSIDE_GANONS_CASTLE: return 2;
        default: return 0;
    }
}

static void StepKeys(int dungeon, int direction) {
    // (-1: none got yet)
    const int keys = Clamp(gSaveContext.inventory.dungeonKeys[dungeon] + direction, -1, MostKeys(dungeon));

    gSaveContext.inventory.dungeonKeys[dungeon] = (s8)keys;
    gSaveContext.ship.stats.dungeonKeys[dungeon] = (u8)keys;
}

static const char* TextKeys(int dungeon, char* buffer, size_t size) {
    const int keys = gSaveContext.inventory.dungeonKeys[dungeon];

    return keys < 0 ? "None yet" : Number(buffer, size, keys);
}

#define MAP_COMPASS(name, dungeon)                                                       \
    ROW(name " map", DUNGEON_ARG(dungeon, DUNGEON_MAP), StepDungeonItem, TextDungeonItem), \
    ROW(name " compass", DUNGEON_ARG(dungeon, DUNGEON_COMPASS), StepDungeonItem, TextDungeonItem)
#define BOSS_KEY(name, dungeon) \
    ROW(name " boss key", DUNGEON_ARG(dungeon, DUNGEON_KEY_BOSS), StepDungeonItem, TextDungeonItem)
#define KEYS(name, dungeon) ROW(name " small keys", dungeon, StepKeys, TextKeys)

static void GiveAllDungeon(int, int) {
    for (int dungeon = SCENE_DEKU_TREE; dungeon <= SCENE_INSIDE_GANONS_CASTLE; dungeon++) {
        if (dungeon <= SCENE_ICE_CAVERN) {
            gSaveContext.inventory.dungeonItems[dungeon] |= gBitFlags[DUNGEON_MAP] | gBitFlags[DUNGEON_COMPASS];
        }
        if ((dungeon >= SCENE_FOREST_TEMPLE && dungeon <= SCENE_SHADOW_TEMPLE) || dungeon == SCENE_GANONS_TOWER) {
            gSaveContext.inventory.dungeonItems[dungeon] |= gBitFlags[DUNGEON_KEY_BOSS];
        }
        if (MostKeys(dungeon) > 0) {
            gSaveContext.inventory.dungeonKeys[dungeon] = (s8)MostKeys(dungeon);
            gSaveContext.ship.stats.dungeonKeys[dungeon] = (u8)MostKeys(dungeon);
        }
    }
}

static const Option kDungeons[] = {
    ACTION("Give all maps, compasses and keys", GiveAllDungeon),
    MAP_COMPASS("Deku Tree", SCENE_DEKU_TREE),
    MAP_COMPASS("Dodongo's Cavern", SCENE_DODONGOS_CAVERN),
    MAP_COMPASS("Jabu-Jabu", SCENE_JABU_JABU),
    MAP_COMPASS("Forest Temple", SCENE_FOREST_TEMPLE),
    BOSS_KEY("Forest Temple", SCENE_FOREST_TEMPLE),
    KEYS("Forest Temple", SCENE_FOREST_TEMPLE),
    MAP_COMPASS("Fire Temple", SCENE_FIRE_TEMPLE),
    BOSS_KEY("Fire Temple", SCENE_FIRE_TEMPLE),
    KEYS("Fire Temple", SCENE_FIRE_TEMPLE),
    MAP_COMPASS("Water Temple", SCENE_WATER_TEMPLE),
    BOSS_KEY("Water Temple", SCENE_WATER_TEMPLE),
    KEYS("Water Temple", SCENE_WATER_TEMPLE),
    MAP_COMPASS("Spirit Temple", SCENE_SPIRIT_TEMPLE),
    BOSS_KEY("Spirit Temple", SCENE_SPIRIT_TEMPLE),
    KEYS("Spirit Temple", SCENE_SPIRIT_TEMPLE),
    MAP_COMPASS("Shadow Temple", SCENE_SHADOW_TEMPLE),
    BOSS_KEY("Shadow Temple", SCENE_SHADOW_TEMPLE),
    KEYS("Shadow Temple", SCENE_SHADOW_TEMPLE),
    MAP_COMPASS("Bottom of the Well", SCENE_BOTTOM_OF_THE_WELL),
    KEYS("Bottom of the Well", SCENE_BOTTOM_OF_THE_WELL),
    MAP_COMPASS("Ice Cavern", SCENE_ICE_CAVERN),
    KEYS("Gerudo Training Ground", SCENE_GERUDO_TRAINING_GROUND),
    KEYS("Thieves' Hideout", SCENE_THIEVES_HIDEOUT),
    KEYS("Ganon's Castle", SCENE_INSIDE_GANONS_CASTLE),
    BOSS_KEY("Ganon's Tower", SCENE_GANONS_TOWER),
};

extern const OptionGroup kEditGroups[] = {
    { "General", kGeneral, LEN(kGeneral) },
    { "Items", kItems, LEN(kItems) },
    { "Equipment", kEquipment, LEN(kEquipment) },
    { "Quest", kQuest, LEN(kQuest) },
    { "Dungeons", kDungeons, LEN(kDungeons) },
};
extern const int kEditGroupCount = LEN(kEditGroups);
