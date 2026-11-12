#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// What a triangle's cut at a clamped tile's bounds came to (Interpreter::HwClampSplit), kept for the next time: a
// scene's triangles are cut the same way tick after tick. A cut depends on the triangle's raw vertices and the bounds
// and on nothing else, so that is the key, and nothing here can go out of date: a triangle whose colours or
// coordinates moved is simply another key. What does depend on the tick (a vertex's state and lights, whether a frame
// could put it behind the eye) isn't kept: a piece's vertex says which of the triangle's it came from, and takes them
// from there.
//
// Only what the batch reads is key and kept: its alpha, and its normal if lit on the GPU, else its colour. A loaded
// vertex's other one is whatever last used that slot (GfxSpVertex sets one or the other), and color.a can be fog's:
// in the key they made a static triangle new every tick the camera moved (15-20% hits, the block refilled every 3
// ticks), though each cut they did find was right.

// a piece's corner
struct CutVert {
    int16_t ob[3];
    uint8_t alpha;
    uint8_t from; // CutFrom's bits
    float u, v;
    uint8_t color[4];
    int8_t n[3];
    uint8_t pad;
};
static_assert(sizeof(CutVert) == 24, "CutVert: compared bytewise, so no hidden padding");

// LoadedVertex::hwFrom: the triangle's corners a vertex was made from, the one it inherits from, and if it's a new one
enum CutFrom : uint8_t { kCutFromMask = 7, kCutFromRootShift = 3, kCutFromRootMask = 3, kCutFromLerped = 0x20 };

template <typename V> inline void CutMark(V* poly) {
    for (int i = 0; i < 3; i++) {
        poly[i].hwFrom = (uint8_t)((1 << i) | (i << kCutFromRootShift));
    }
}

// (`lit`: the triangle's, all its corners' alike: HwClampTriangle's cut is lit on the CPU first if they differ)
template <typename V> inline void CutSave(CutVert& o, const V& s, bool lit) {
    memset(&o, 0, sizeof(o));
    memcpy(o.ob, s.ob, sizeof(o.ob));
    o.alpha = s.alpha;
    o.from = s.hwFrom;
    o.u = s.u;
    o.v = s.v;
    if (lit) {
        memcpy(o.n, s.n, sizeof(o.n));
    } else {
        o.color[0] = s.color.r;
        o.color[1] = s.color.g;
        o.color[2] = s.color.b;
    }
}

// A piece's corner as the cut would make it from `poly`: what isn't kept is its root corner's, as HwLerp's copy gives
// it, and a lerped corner's risk is HwLerp's rule over all it came from (1 only if each of those is 1; 0 otherwise, for
// a frame to ask again), with nothing known beyond the planes.
template <typename V> inline void CutRestore(V& d, const CutVert& s, const V* poly, bool lit) {
    d = poly[(s.from >> kCutFromRootShift) & kCutFromRootMask];
    memcpy(d.ob, s.ob, sizeof(s.ob));
    d.alpha = s.alpha;
    d.hwFrom = s.from;
    d.u = s.u;
    d.v = s.v;
    if (lit) {
        memcpy(d.n, s.n, sizeof(s.n));
    } else {
        d.color.r = s.color[0];
        d.color.g = s.color[1];
        d.color.b = s.color[2];
    }
    if ((s.from & kCutFromLerped) != 0) {
        bool safe = true;
        for (int i = 0; i < 3; i++) {
            if ((s.from & (1 << i)) != 0 && poly[i].hwRisk != 1) {
                safe = false;
            }
        }
        d.hwRisk = safe ? 1 : 0;
        d.hwOut = 0;
    }
}

// 64 bits of a triangle and its bounds. A hash and not the bytes: an entry is ~300 bytes against the 90 of a key, and
// more entries matter more than two triangles in 2^64 meeting
struct CutKey {
    uint32_t a = 0x811C9DC5u, b = 0x9E3779B9u;

    void Add(uint32_t w) {
        a = (a ^ w) * 0x01000193u;
        a ^= a >> 15;
        b = (b + w) * 0x85EBCA6Bu;
        b ^= b >> 13;
    }
    void AddFloat(float f) {
        uint32_t w;
        memcpy(&w, &f, sizeof(w));
        Add(w);
    }
    template <typename V> void AddVertex(const V& s, bool lit) {
        Add((uint32_t)(uint16_t)s.ob[0] | ((uint32_t)(uint16_t)s.ob[1] << 16));
        Add((uint32_t)(uint16_t)s.ob[2] | ((uint32_t)s.alpha << 16) | (lit ? 1u << 24 : 0u));
        AddFloat(s.u);
        AddFloat(s.v);
        if (lit) {
            Add((uint32_t)(uint8_t)s.n[0] | ((uint32_t)(uint8_t)s.n[1] << 8) | ((uint32_t)(uint8_t)s.n[2] << 16));
        } else {
            Add((uint32_t)s.color.r | ((uint32_t)s.color.g << 8) | ((uint32_t)s.color.b << 16));
        }
    }
    // (a step's bound, and which bound it is)
    void AddStep(bool axisV, bool upper, float bound) {
        Add((axisV ? 1u : 0u) | (upper ? 2u : 0u));
        AddFloat(bound);
    }
};

// The entries, in one block with a table of chains: a full block starts again, which costs a few ticks of cutting.
template <uint32_t kBytes, uint32_t kBuckets> class CutCacheOf {
    static_assert((kBuckets & (kBuckets - 1)) == 0, "a power of two");

  public:
    struct Found {
        const CutVert* verts;
        const uint8_t* sizes; // each piece's corners
        uint32_t polys, numVerts;
    };

    CutCacheOf() {
        Wipe();
    }

    bool Find(const CutKey& key, Found* out) const {
        for (uint32_t at = mHeads[key.a & (kBuckets - 1)]; at != 0;) {
            Entry e;
            memcpy(&e, mBlock + at, sizeof(e));
            if (e.a == key.a && e.b == key.b) {
                out->verts = (const CutVert*)(mBlock + at + sizeof(Entry));
                out->sizes = mBlock + at + sizeof(Entry) + e.verts * sizeof(CutVert);
                out->polys = e.polys;
                out->numVerts = e.verts;
                return true;
            }
            at = e.next;
        }
        return false;
    }

    void Put(const CutKey& key, const CutVert* verts, uint32_t numVerts, const uint8_t* sizes, uint32_t polys) {
        const uint32_t bytes = (uint32_t)(sizeof(Entry) + numVerts * sizeof(CutVert) + ((polys + 3) & ~3u));
        if (bytes > kBytes - kFirst) {
            return;
        }
        if (mUsed + bytes > kBytes) {
            Wipe();
            mWipes++;
        }
        const uint32_t at = mUsed;
        uint32_t& head = mHeads[key.a & (kBuckets - 1)];
        Entry e = { key.a, key.b, head, (uint16_t)polys, (uint16_t)numVerts };
        memcpy(mBlock + at, &e, sizeof(e));
        memcpy(mBlock + at + sizeof(Entry), verts, numVerts * sizeof(CutVert));
        memcpy(mBlock + at + sizeof(Entry) + numVerts * sizeof(CutVert), sizes, polys);
        head = at;
        mUsed += bytes;
    }

    uint32_t UsedBytes() const {
        return mUsed - kFirst;
    }
    uint32_t TakeWipes() {
        const uint32_t w = mWipes;
        mWipes = 0;
        return w;
    }

  private:
    struct Entry {
        uint32_t a, b, next;
        uint16_t polys, verts;
    };
    static constexpr uint32_t kFirst = 8; // (0 ends a chain; 8: CutVert's floats stay aligned)

    void Wipe() {
        memset(mHeads, 0, sizeof(mHeads));
        mUsed = kFirst;
    }

    uint32_t mHeads[kBuckets];
    uint32_t mUsed;
    uint32_t mWipes = 0;
    alignas(8) uint8_t mBlock[kBytes];
};

// the size the user agreed: 256 KB in all (Nov 5 2026), for up to about 900 cuts at a scene's worst
typedef CutCacheOf<256 * 1024 - 2048 * sizeof(uint32_t) - 16, 2048> CutCache;
static_assert(sizeof(CutCache) <= 256 * 1024, "the budget");
