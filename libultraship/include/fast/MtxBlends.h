#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "fast/types.h"

// A tick's blended matrices, by the address its display list names each at: what FrameInterpolation makes and a
// replay's states read. The one table is kept from tick to tick, so a tick costs the heap nothing (a map's nodes
// were a malloc and a free for every matrix of every tick), and the entries lie in the order they're made.
// Slots hold an entry's index + 1, in 16 bits: 64K matrices would be 4 MB of the game's display list pool.
class MtxBlends {
  public:
    MtxBlends() : mSlots(kFirstSlots, 0) {
    }

    // every tick's start: an old entry would match a reused Mtx
    void Clear() {
        if (!mEntries.empty()) {
            std::memset(mSlots.data(), 0, mSlots.size() * sizeof(mSlots[0]));
            mEntries.clear();
        }
    }

    size_t Size() const {
        return mEntries.size();
    }

    // the matrix to fill in for this address (a tick's frames blend the same ones: its entry again)
    MtxF* Put(const void* key) {
        if ((mEntries.size() + 1) * 2 > mSlots.size()) {
            Grow();
        }
        const uint32_t mask = (uint32_t)mSlots.size() - 1;
        uint32_t i = SlotOf(key);
        for (; mSlots[i] != 0; i = (i + 1) & mask) {
            Entry& e = mEntries[mSlots[i] - 1];
            if (e.key == key) {
                return &e.mf;
            }
        }
        mEntries.emplace_back();
        mEntries.back().key = key;
        mSlots[i] = (uint16_t)mEntries.size();
        return &mEntries.back().mf;
    }

    const MtxF* Find(const void* key) const {
        const uint32_t mask = (uint32_t)mSlots.size() - 1;
        for (uint32_t i = SlotOf(key); mSlots[i] != 0; i = (i + 1) & mask) {
            const Entry& e = mEntries[mSlots[i] - 1];
            if (e.key == key) {
                return &e.mf;
            }
        }
        return nullptr;
    }

  private:
    static constexpr size_t kFirstSlots = 4096;

    struct Entry {
        const void* key;
        MtxF mf;
    };

    uint32_t SlotOf(const void* key) const {
        return (((uint32_t)(uintptr_t)key >> 3) * 2654435761u) >> mShift;
    }

    void Grow() {
        mSlots.assign(mSlots.size() * 2, 0);
        mShift--;
        const uint32_t mask = (uint32_t)mSlots.size() - 1;
        for (size_t e = 0; e < mEntries.size(); e++) {
            uint32_t i = SlotOf(mEntries[e].key);
            while (mSlots[i] != 0) {
                i = (i + 1) & mask;
            }
            mSlots[i] = (uint16_t)(e + 1);
        }
    }

    std::vector<Entry> mEntries;
    std::vector<uint16_t> mSlots; // a power of two, never more than half full
    uint32_t mShift = 20;         // 32 - log2(the slots)
};
