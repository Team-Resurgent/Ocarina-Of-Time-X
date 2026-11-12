#define NOMINMAX

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#if !defined(_WIN32) && !defined(LUS_XBOX)
#include <dlfcn.h>
#endif

#include <any>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>
#include <list>
#include <stack>
#include "fast/resource/type/Light.h"

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include "fast/debug/GfxDebugger.h"
#include "fast/types.h"
#include <string>

#include "fast/interpreter.h"
#include "fast/lus_gbi.h"
#include "fast/backends/gfx_window_manager_api.h"
#include "fast/backends/gfx_rendering_api.h"

#include "ship/window/gui/Gui.h"
#include "ship/resource/ResourceManager.h"
#include "ship/utils/Utils.h"
#include "ship/Context.h"
#include "ship/config/ConsoleVariable.h"

#include "libultraship/libultra/os.h"

#include <spdlog/fmt/fmt.h>
#ifdef LUS_XBOX
#include <xmmintrin.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

std::stack<std::string> currentDir;

#define SEG_ADDR(seg, addr) (addr | (seg << 24) | 1)
#define SUPPORT_CHECK(x) assert(x)

// SCALE_M_N: upscale/downscale M-bit integer to N-bit
#define SCALE_5_8(VAL_) (((VAL_)*0xFF) / 0x1F)
#define SCALE_8_5(VAL_) ((((VAL_) + 4) * 0x1F) / 0xFF)
#define SCALE_4_8(VAL_) ((VAL_)*0x11)
#define SCALE_8_4(VAL_) ((VAL_) / 0x11)
#define SCALE_3_8(VAL_) ((VAL_)*0x24)
#define SCALE_8_3(VAL_) ((VAL_) / 0x24)

// Based off the current set native dimensions or active framebuffer
#define HALF_SCREEN_WIDTH(activeFb) ((mFbActive ? activeFb->second.orig_width : mNativeDimensions.width) / 2)
#define HALF_SCREEN_HEIGHT(activeFb) ((mFbActive ? activeFb->second.orig_height : mNativeDimensions.height) / 2)

// Ratios for current window dimensions or active framebuffer scaled size
#define RATIO_X(activeFb, dims) \
    ((mFbActive ? activeFb->second.applied_width : dims.width) / (2.0f * HALF_SCREEN_WIDTH(activeFb)))
#define RATIO_Y(activeFb, dims) \
    ((mFbActive ? activeFb->second.applied_height : dims.height) / (2.0f * HALF_SCREEN_HEIGHT(activeFb)))

#ifndef TEXTURE_CACHE_MAX_SIZE
#define TEXTURE_CACHE_MAX_SIZE 1024
#endif

namespace Fast {

static UcodeHandlers ucode_handler_index = ucode_f3dex2;

const static uint32_t f3dex2AttrHandler[] = {
    F3DEX2_G_MTX_PROJECTION, F3DEX2_G_MTX_LOAD,  F3DEX2_G_MTX_PUSH,  F3DEX_G_MTX_NOPUSH,
    F3DEX2_G_CULL_FRONT,     F3DEX2_G_CULL_BACK, F3DEX2_G_CULL_BOTH,
};

const static uint32_t f3dexAttrHandler[] = { F3DEX_G_MTX_PROJECTION, F3DEX_G_MTX_LOAD,   F3DEX_G_MTX_PUSH,
                                             F3DEX_G_MTX_NOPUSH,     F3DEX_G_CULL_FRONT, F3DEX_G_CULL_BACK,
                                             F3DEX_G_CULL_BOTH };

static constexpr std::array ucode_attr_handlers = {
    &f3dexAttrHandler,  // ucode_f3db
    &f3dexAttrHandler,  // ucode_f3d
    &f3dexAttrHandler,  // ucode_f3dex
    &f3dexAttrHandler,  // ucode_f3exb
    &f3dex2AttrHandler, // ucode_f3ex2
    &f3dex2AttrHandler, // ucode_s2dex
};

static uint32_t get_attr(Attribute attr) {
    const auto ucode_map = ucode_attr_handlers[ucode_handler_index];
    // assert(ucode_map->contains(attr) && "Attribute not found in the current ucode handler");
    return (*ucode_map)[attr];
}

static std::string GetPathWithoutFileName(char* filePath) {
    size_t len = strlen(filePath);

    for (size_t i = len - 1; (long)i >= 0; i--) {
        if (filePath[i] == '/' || filePath[i] == '\\') {
            return std::string(filePath).substr(0, i);
        }
    }

    return filePath;
}

constexpr size_t MAX_TRI_BUFFER = 256;

#ifdef LUS_XBOX
// The Xbox's profile of a frame's rendering: the CPU's cycles in each display-list command (they don't
// nest: a display list call only pushes the stack), in the backend's draws and in texture imports (both
// within the commands), logged every 100 frames by Interpreter::Run.
// The per-command part (two rdtsc an op, thousands a frame) is one run in every 100: on an emulator the rdtsc
// itself can cost microseconds, and the profile then measures itself. The line gives that run's whole time, to
// set against the others'.
static bool sXboxProfileRun;
static inline uint64_t XboxCycles() {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
#define XBOX_CYCLES() (sXboxProfileRun ? XboxCycles() : 0)
static uint64_t sXboxOpCycles[256], sXboxDrawCycles, sXboxImportCycles, sXboxRunCycles, sXboxProfiledCycles;
static uint32_t sXboxOpCount[256], sXboxRuns, sXboxDraws;
static uint64_t sXboxHwDrawCycles; // the GPU transform's draws (within the commands' times), and how many
static uint32_t sXboxHwDraws;
static uint32_t sXboxFastTris; // the triangles that went to the batch without their state worked out
static uint32_t sXboxDropped;  // those dropped as recorded, far behind the eye
// the others on the GPU's path: why (state may have changed; a rectangle; other lights; a cut refused), and the
// commands that ended the fast path
static uint32_t sXboxSlowWhy[4], sXboxFastEnders[256];
// the profiled run's cycles in the triangles' slow path and in cutting at the clamp (some of it within the slow
// path); and all runs' triangles cut there and the pieces made
static uint64_t sXboxSlowCycles, sXboxCutCycles;
static uint32_t sXboxCuts, sXboxPieces;
struct XboxTimed {
    uint64_t& total;
    uint64_t start;
    explicit XboxTimed(uint64_t& t) : total(t), start(XBOX_CYCLES()) {
    }
    ~XboxTimed() {
        total += XBOX_CYCLES() - start;
    }
};
// raw triangles the CPU's path took: for the tile's clamp, the blend colour's fog, grayscale, a rectangle,
// or what HwTriangle refused (more than 4 inputs, the LOD fraction); and how many of them it then culled
static uint32_t sXboxCpuWhy[5], sXboxCpuCulled;
static uint32_t sXboxClampWhy[2]; // clamped triangles: cut across limbs (on the GPU), refused the cut
static uint32_t sXboxHwWhy[2];    // HwTriangle's refusals: more than 4 combiner inputs, the LOD fraction

extern "C" double LusXbox_Seconds(void); // gfx_xbox.cpp
static double sXboxRebuildSeconds;       // the replays' HwRebuildStates, for the profile line
static uint64_t sXboxStatesRebuilt, sXboxClipWatched, sXboxClipCut, sXboxClipGone;
static uint32_t sXboxReplays;

// HwClipTriangles: a triangle is watched if a vertex is this near the eye or this far off the screen (in screen
// widths from its centre) at the tick's end, and cut in a frame past the tighter limits. (Game units: a camera
// covers ~30 in a tick.) Five planes leave at most 8 corners: 6 triangles.
static constexpr float kHwRiskNear = 48.0f, kHwRiskGuard = 3.0f, kHwClipNear = 0.75f, kHwClipGuard = 4.0f;
// A triangle with every corner this far behind the eye, or this far past one side of the guard band, at the tick's
// end is out of sight in all the tick's frames. Behind: over three times a camera's travel in the two thirds of a
// tick the frames reach back, and a turn would have to pass 60 degrees in that time. Past a side: a turn of 30
// degrees, and the margin takes the near ones the travel moves most. Only a cut does that, and a cut isn't
// interpolated. It's dropped as it's recorded, and costs nothing after that.
static constexpr float kHwDropBehind = 64.0f, kHwDropSide = 160.0f;
static constexpr size_t kHwTriBytes = 3 * 16, kHwClipMaxBytes = 6 * kHwTriBytes;
static constexpr uint32_t kHwClipGroupTris = 16; // few enough to keep a group's box tight

// Is all of a box of object space beyond one of HwClipTriangles' planes? Each plane is linear in the position, so
// the box's highest value on it says.
static bool HwBoxBeyond(const int16_t* lo, const int16_t* hi, const GfxRenderingAPI::HwState& s) {
    float c[3], e[3], x[4], y[4], w[4], r[4];
    for (int k = 0; k < 3; k++) {
        c[k] = (lo[k] + hi[k]) * 0.5f;
        e[k] = (hi[k] - lo[k]) * 0.5f;
    }
    for (int k = 0; k < 4; k++) {
        x[k] = s.mp[k][0] * s.aspect;
        y[k] = s.mp[k][1];
        w[k] = s.mp[k][3];
    }
    auto beyond = [&c, &e](const float* p) {
        return c[0] * p[0] + c[1] * p[1] + c[2] * p[2] + p[3] + e[0] * fabsf(p[0]) + e[1] * fabsf(p[1]) +
                   e[2] * fabsf(p[2]) < 0.0f;
    };
    memcpy(r, w, sizeof(r));
    r[3] -= kHwClipNear;
    if (beyond(r)) {
        return true; // behind the eye: nearly all of them
    }
    for (int sign = 1; sign >= -1; sign -= 2) {
        for (int k = 0; k < 4; k++) {
            r[k] = sign * x[k] + kHwClipGuard * w[k];
        }
        if (beyond(r)) {
            return true;
        }
        for (int k = 0; k < 4; k++) {
            r[k] = sign * y[k] + kHwClipGuard * w[k];
        }
        if (beyond(r)) {
            return true;
        }
    }
    return false;
}

// The hash commands' resources, by hash: they resolve the same ones every frame, and through the resource
// manager (a path string made, a mutex, its map) that was 7-10 us each, 6-8 ms of a Clock Town frame.
// Hits only; cleared with the texture cache (TextureCacheClear: a new scene).
static std::unordered_map<uint64_t, void*> sXboxRawByHash;
static std::unordered_map<uint64_t, std::shared_ptr<Ship::IResource>> sXboxResourceByHash;

static void* XboxRawPointerByHash(uint64_t hash) {
    auto it = sXboxRawByHash.find(hash);
    if (it != sXboxRawByHash.end()) {
        return it->second;
    }
    void* p = Ship::Context::GetRawInstance()->GetResourceManager()->GetResourceRawPointer(hash);
    if (p != nullptr) {
        sXboxRawByHash.emplace(hash, p);
    }
    return p;
}

static std::shared_ptr<Ship::IResource> XboxResourceByHash(uint64_t hash, const char* path) {
    auto it = sXboxResourceByHash.find(hash);
    if (it != sXboxResourceByHash.end()) {
        return it->second;
    }
    auto res = Ship::Context::GetRawInstance()->GetResourceManager()->LoadResourceProcess(path);
    if (res != nullptr) {
        sXboxResourceByHash.emplace(hash, res);
    }
    return res;
}

// G_SETTIMG_OTR_HASH's texture, by the low bits of its hash: a tick's few hundred textures in a table looked in
// once, where the command took 3.3 us at 733 MHz (the archive's name for the hash, the map above, then the
// shared_ptr copied five times over). A hash that shares a slot takes the long way and takes the slot.
struct XboxTextureSlot {
    uint64_t hash;
    std::shared_ptr<Fast::Texture> texture;
};
static XboxTextureSlot sXboxTextureSlots[1024];
#endif

Interpreter::Interpreter() {
    mRsp = new RSP();
    mRdp = new RDP();
    mBufVbo = new float[MAX_TRI_BUFFER * (32 * 3)];
#ifdef LUS_XBOX
    mHwVerts.resize(MAX_TRI_BUFFER * 3 * 16);
    mHwStates.reserve(1024);
#endif
}

#ifdef LUS_XBOX
void Interpreter::HwFlush() {
    // the watched triangles leave the batch's vertices: each frame draws what it makes of them after the rest
    size_t tris = mHwTris;
    if (mHwClipPending < mHwClipTris.size()) {
        // (the vertices to blend in the batch, moved down with the rest; in a watched triangle, they stay as they are)
        size_t watched = mHwClipPending, removed = 0, keptBlends = 0;
        for (const GfxRenderingAPI::HwBlendVert& b : mHwBlendBatch) {
            while (watched < mHwClipTris.size() && mHwClipTris[watched].bytes + kHwTriBytes <= b.offset) {
                watched++;
                removed += kHwTriBytes;
            }
            if (watched == mHwClipTris.size() || b.offset < mHwClipTris[watched].bytes) {
                mHwBlendBatch[keptBlends] = b;
                mHwBlendBatch[keptBlends++].offset -= (uint32_t)removed;
            }
        }
        mHwBlendBatch.resize(keptBlends);
        uint8_t* verts = mHwVerts.data();
        size_t kept = mHwClipTris[mHwClipPending].bytes;
        const uint32_t draw = mRapi->RecordedDraws();
        for (size_t i = mHwClipPending; i < mHwClipTris.size(); i++) {
            HwClipTri& t = mHwClipTris[i];
            const size_t from = t.bytes + kHwTriBytes;
            const size_t to = i + 1 < mHwClipTris.size() ? mHwClipTris[i + 1].bytes : mHwTris * kHwTriBytes;
            memmove(verts + kept, verts + from, to - from);
            kept += to - from;
            t.draw = draw;
            HwClipGroup* g = mHwClipGroups.empty() ? nullptr : &mHwClipGroups.back();
            if (g == nullptr || g->count == kHwClipGroupTris || mHwClipTris[g->first].draw != draw ||
                mHwClipTris[g->first].state != t.state) {
                mHwClipGroups.push_back({ (uint32_t)i, 0, { INT16_MAX, INT16_MAX, INT16_MAX },
                                          { INT16_MIN, INT16_MIN, INT16_MIN } });
                g = &mHwClipGroups.back();
            }
            g->count++;
            for (int c = 0; c < 3; c++) {
                for (int k = 0; k < 3; k++) {
                    g->lo[k] = std::min(g->lo[k], t.verts[c][k]);
                    g->hi[k] = std::max(g->hi[k], t.verts[c][k]);
                }
            }
        }
        mHwClipPending = mHwClipTris.size();
        tris = kept / kHwTriBytes;
    }
    mHwBatch.verts = mHwVerts.data();
    mHwBatch.tris = tris;
    mHwBatch.states = mHwStates.data();
    mHwBatch.slotStates = mHwSlotState;
    mHwBatch.numStates = mHwSlots;
    mHwBatch.lights = mHwBatch.lightSet != 0 ? &mHwLights[mHwBatch.lightSet - 1] : nullptr;
    uint64_t t0 = XBOX_CYCLES();
    const size_t recordedAt = mRapi->RecordedVertexBytes();
    mRapi->DrawTrianglesHw(mHwBatch);
    sXboxHwDrawCycles += XBOX_CYCLES() - t0;
    sXboxHwDraws++;
    if (!mHwBlendBatch.empty()) {
        if (mRapi->RecordedVertexBytes() != recordedAt) {
            for (GfxRenderingAPI::HwBlendVert& b : mHwBlendBatch) {
                b.offset += (uint32_t)recordedAt;
                mHwBlendRecorded.push_back(b);
            }
        }
        mHwBlendBatch.clear();
    }
    mHwBatch.states = nullptr; // (vectors Run clears, otherwise)
    mHwBatch.lights = nullptr;
    mHwTris = 0;
    mHwSlots = 0;
}

// a triangle for the GPU's batch: its constants (a different set, or the slots full, draws the batch so
// far), its vertices raw. False if the GPU's path can't draw it (the CPU's will).
bool Interpreter::HwTriangle(LoadedVertex* const v[3], int cull, const ColorCombiner* comb, bool use_alpha,
                             bool use_fog, const bool usedTextures[2], uint8_t numInputs,
                             const uint32_t effective_tile[2], const uint32_t tex_width[2],
                             const uint32_t tex_height[2], bool is_rect) {
    if (numInputs > 4) {
        sXboxHwWhy[0]++;
        return false;
    }
    // lit on the GPU only if all three are, with the same lights; else lit here, the batch unlit
    if (v[0]->light != v[1]->light || v[0]->light != v[2]->light) {
        EnsureLit(v[0]);
        EnsureLit(v[1]);
        EnsureLit(v[2]);
    }
    GfxRenderingAPI::HwBatch b{};
    b.cull = cull;
    b.lightSet = v[0]->light;
    for (int t = 0; t < 2; t++) {
        if (!usedTextures[t]) {
            continue;
        }
        // as the CPU path: u / 32, shifted, less the tile's origin (+ 0.5 for a linear filter), over the size
        const auto& tile = mRdp->texture_tile[effective_tile[t]];
        float fs = tile.shifts == 0 ? 1.0f : tile.shifts <= 10 ? 1.0f / (1 << tile.shifts) : (float)(1 << (16 - tile.shifts));
        float ft = tile.shiftt == 0 ? 1.0f : tile.shiftt <= 10 ? 1.0f / (1 << tile.shiftt) : (float)(1 << (16 - tile.shiftt));
        float lin = ((mRdp->other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT && !is_rect) ? 0.5f : 0.0f;
        float iw = 1.0f / tex_width[t], ih = 1.0f / tex_height[t]; // (a divide is ~20 cycles on the P3)
        b.texAffine[t][0] = fs * (1.0f / 32.0f) * iw;
        b.texAffine[t][1] = ft * (1.0f / 32.0f) * ih;
        b.texAffine[t][2] = (lin - tile.uls * 0.25f) * iw;
        b.texAffine[t][3] = (lin - tile.ult * 0.25f) * ih;
    }
    for (int j = 0; j < numInputs; j++) {
        for (int k = 0; k < 2; k++) {
            float* C = b.inputC[j];
            float* S = b.inputS[j];
            if (k == 1 && !use_alpha) {
                C[3] = 1.0f; // as the CPU path packs it
                continue;
            }
            RGBA tmp = {};
            const RGBA* color = &tmp;
            bool shade = false;
            switch (comb->shader_input_mapping[k][j]) {
                case G_CCMUX_PRIMITIVE:
                    color = &mRdp->prim_color;
                    break;
                case G_CCMUX_SHADE:
                    shade = true;
                    break;
                case G_CCMUX_ENVIRONMENT:
                    color = &mRdp->env_color;
                    break;
                case G_CCMUX_PRIMITIVE_ALPHA:
                    tmp.r = tmp.g = tmp.b = mRdp->prim_color.a;
                    break;
                case G_CCMUX_ENV_ALPHA:
                    tmp.r = tmp.g = tmp.b = mRdp->env_color.a;
                    break;
                case G_CCMUX_PRIM_LOD_FRAC:
                    tmp.r = tmp.g = tmp.b = mRdp->prim_lod_fraction;
                    break;
                case G_CCMUX_LOD_FRACTION:
                    if (mRdp->other_mode_l & G_TL_LOD) {
                        // from the vertex's w in the vertex shader: w * S + C, clamped (the CPU path's
                        // (w - 3000) / 3000); Clock Town's ground is drawn so
                        b.lodMask |= (uint8_t)(1 << (2 * j + k));
                        if (k == 0) {
                            C[0] = C[1] = C[2] = -1.0f;
                            S[0] = S[1] = S[2] = 1.0f / 3000.0f;
                        } else {
                            C[3] = -1.0f;
                            S[3] = 1.0f / 3000.0f;
                        }
                        continue;
                    }
                    tmp.r = tmp.g = tmp.b = tmp.a = 255;
                    break;
                case G_CCMUX_KEY_CENTER:
                    color = &mRdp->key_center;
                    break;
                case G_CCMUX_KEY_SCALE:
                    color = &mRdp->key_scale;
                    break;
                case G_CCMUX_CONVERT_K4:
                    tmp.r = tmp.g = tmp.b = mRdp->convert_k[4];
                    break;
                case G_CCMUX_CONVERT_K5:
                    tmp.r = tmp.g = tmp.b = mRdp->convert_k[5];
                    break;
                case G_ACMUX_PRIM_LOD_FRAC:
                    tmp.a = mRdp->prim_lod_fraction;
                    break;
                default:
                    break;
            }
            if (k == 0) {
                S[0] = S[1] = S[2] = shade ? 1.0f : 0.0f;
                C[0] = shade ? 0.0f : color->r * (1.0f / 255.0f);
                C[1] = shade ? 0.0f : color->g * (1.0f / 255.0f);
                C[2] = shade ? 0.0f : color->b * (1.0f / 255.0f);
            } else {
                S[3] = shade ? 1.0f : 0.0f;
                C[3] = shade ? 0.0f : color->a * (1.0f / 255.0f);
            }
        }
    }
    b.shadeAlphaOne = use_fog ? 1.0f : 0.0f; // standard fog: shade's alpha reads 1 (fog is the blender's)
    b.fogColor = 0xFF000000u | ((uint32_t)mRdp->fog_color.r << 16) | ((uint32_t)mRdp->fog_color.g << 8) |
                 mRdp->fog_color.b;

    // the CPU's batch drawn first (the order); the GPU's when its constants differ
    if (mBufVboLen > 0) {
        Flush();
    }
    const size_t off = offsetof(GfxRenderingAPI::HwBatch, texAffine);
    if (mHwTris > 0 && memcmp((const uint8_t*)&b + off, (const uint8_t*)&mHwBatch + off, sizeof(b) - off) != 0) {
        Flush();
    }
    memcpy((uint8_t*)&mHwBatch + off, (const uint8_t*)&b + off, sizeof(b) - off);
    HwSetBatchLights(b.lightSet);
    HwEmit(v);
    return true;
}

void Interpreter::HwSetBatchLights(uint16_t lightSet) {
    mHwBatch.lightSet = lightSet;
    if (lightSet != 0) {
        const auto& l = mHwLights[lightSet - 1];
        mHwStride = GfxRenderingAPI::kHwLitStride;
        mHwSlotMax = std::min(mRapi->HwLitSlots(l.numDir, l.numPoint), GfxRenderingAPI::kHwMaxSlots);
    } else {
        mHwStride = GfxRenderingAPI::kHwStride;
        mHwSlotMax = GfxRenderingAPI::kHwMaxSlots;
    }
}

// a triangle into the batch, under the constants it has: its vertices' states in the slots, the vertices raw
void Interpreter::HwEmit(LoadedVertex* const v[3]) {
    const bool risky = HwVertexRisky(v[0]) | HwVertexRisky(v[1]) | HwVertexRisky(v[2]);
    const bool clip = risky && HwSameXform(v[0]->state, v[1]->state) && HwSameXform(v[0]->state, v[2]->state);
    if (mHwStateUsed.size() < mHwStates.size()) {
        mHwStateUsed.resize(mHwStates.size(), 0);
    }
    mHwStateUsed[v[0]->state] = mHwStateUsed[v[1]->state] = mHwStateUsed[v[2]->state] = 1;
    int slot[3];
    for (int pass = 0; pass < 2; pass++) {
        int fresh = mHwSlots;
        bool full = false;
        for (int i = 0; i < 3 && !full; i++) {
            int k = 0;
            while (k < fresh && mHwSlotState[k] != v[i]->state) {
                k++;
            }
            if (k == fresh) {
                if (fresh >= mHwSlotMax) {
                    full = true;
                    break;
                }
                mHwSlotState[fresh++] = v[i]->state;
            }
            slot[i] = k;
        }
        if (!full) {
            mHwSlots = fresh;
            break;
        }
        Flush(); // slots full: a new batch (Flush keeps the constants)
    }

    const bool lit = mHwBatch.lightSet != 0;
    int16_t* const first = (int16_t*)(mHwVerts.data() + mHwTris * 3 * 16);
    int16_t* out = first;
    for (int i = 0; i < 3; i++, out += 8) {
        out[0] = v[i]->ob[0];
        out[1] = v[i]->ob[1];
        out[2] = v[i]->ob[2];
        out[3] = (int16_t)(slot[i] * mHwStride);
        out[4] = (int16_t)v[i]->u;
        out[5] = (int16_t)v[i]->v;
        if (lit) {
            *(uint32_t*)(out + 6) = ((uint32_t)v[i]->alpha << 24) | ((uint32_t)(uint8_t)(v[i]->n[0] + 128) << 16) |
                                    ((uint32_t)(uint8_t)(v[i]->n[1] + 128) << 8) | (uint8_t)(v[i]->n[2] + 128);
        } else {
            *(uint32_t*)(out + 6) = ((uint32_t)v[i]->alpha << 24) | ((uint32_t)v[i]->color.r << 16) |
                                    ((uint32_t)v[i]->color.g << 8) | v[i]->color.b;
        }
        if (v[i]->hwBlend) {
            GfxRenderingAPI::HwBlendVert b;
            b.offset = (uint32_t)((uint8_t*)out - mHwVerts.data());
            memcpy(b.prev, v[i]->hwPrev, sizeof(b.prev));
            memcpy(b.cur, v[i]->ob, sizeof(b.cur));
            mHwBlendBatch.push_back(b);
        }
    }
    mHwTriangles++;
    mHwLitTriangles += lit ? 1 : 0;
    if (clip) {
        HwClipTri t;
        t.bytes = mHwTris * kHwTriBytes;
        t.draw = 0;
        t.state = v[0]->state;
        memcpy(t.verts, first, sizeof(t.verts));
        mHwClipTris.push_back(t);
    }
    if (++mHwTris == MAX_TRI_BUFFER) {
        Flush();
    }
}

// Could a frame of this tick put the vertex behind the eye, or far off the screen? (With the matrices of the
// tick's end, so with room for how far the frames before it differ. A projection without perspective: never.)
bool Interpreter::HwVertexRisky(LoadedVertex* v) {
    if (v->hwRisk == 0) {
        const auto& s = mHwStates[v->state];
        const float x = v->ob[0], y = v->ob[1], z = v->ob[2];
        const float cw = x * s.mp[0][3] + y * s.mp[1][3] + z * s.mp[2][3] + s.mp[3][3];
        const float cx = (x * s.mp[0][0] + y * s.mp[1][0] + z * s.mp[2][0] + s.mp[3][0]) * s.aspect;
        const float cy = x * s.mp[0][1] + y * s.mp[1][1] + z * s.mp[2][1] + s.mp[3][1];
        const float g = kHwRiskGuard * cw;
        const bool ortho = s.mp[0][3] == 0.0f && s.mp[1][3] == 0.0f && s.mp[2][3] == 0.0f;
        v->hwRisk = !ortho && (cw < kHwRiskNear || cx > g || -cx > g || cy > g || -cy > g) ? 2 : 1;
        v->hwOut = 0;
        if (v->hwRisk == 2) {
            const float d = kHwClipGuard * cw + kHwDropSide;
            v->hwOut = (cw < -kHwDropBehind ? 1 : 0) | (cx < -d ? 2 : 0) | (cx > d ? 4 : 0) | (cy < -d ? 8 : 0) |
                       (cy > d ? 16 : 0);
        }
    }
    return v->hwRisk == 2;
}

// across limbs as well: it has no object space to be cut in until it has a state of its own (HwClampTriangle)
bool Interpreter::HwSpanRisky(LoadedVertex* const v[3]) {
    return (HwVertexRisky(v[0]) | HwVertexRisky(v[1]) | HwVertexRisky(v[2])) &&
           !(HwSameXform(v[0]->state, v[1]->state) && HwSameXform(v[0]->state, v[2]->state));
}

// The NV2A divides by w and then rasterizes, with no clipping of its own to speak of: a triangle with a corner
// behind the eye comes out mirrored (and its winding reversed, so culled as a back face), and corners far off
// the screen overrun the rasterizer. That is the walls, floors and bodies near the camera going see-through.
// So they're cut as the RSP cuts them: each frame, as the matrices are that frame's, against the eye's plane and
// a guard band, in the object space of the state that places them. Most of those watched are behind the camera
// or beside it and go at once, so the GPU never sees them; the rest are drawn after their batch's other triangles.
void Interpreter::HwClipTriangles() {
    struct Corner {
        float ob[3], uv[2], c[4], x, y, w;
    };
    Corner buf[2][12];
    size_t used = 0, groupStart = 0;
    uint32_t groupDraw = UINT32_MAX;
    size_t nextBox = 0;

    for (size_t i = 0; i < mHwClipTris.size(); i++) {
        const HwClipTri& t = mHwClipTris[i];
        if (t.draw != groupDraw) {
            if (groupDraw != UINT32_MAX) {
                mRapi->SetRecordedClip(groupDraw, (uint32_t)groupStart, (uint32_t)((used - groupStart) / kHwTriBytes));
            }
            groupDraw = t.draw;
            groupStart = used;
        }
        if (nextBox < mHwClipGroups.size() && mHwClipGroups[nextBox].first == i) {
            const HwClipGroup& box = mHwClipGroups[nextBox++];
            if (HwBoxBeyond(box.lo, box.hi, mHwStates[t.state])) {
                sXboxClipGone += box.count; // (a box never spans draws)
                i += box.count - 1;
                continue;
            }
        }
        if (used + kHwClipMaxBytes > mHwClipVerts.size()) {
            mHwClipVerts.resize(std::max<size_t>(64 * 1024, mHwClipVerts.size() * 2));
        }
        const auto& s = mHwStates[t.state];
        Corner* in = buf[0];
        Corner* out = buf[1];
        uint32_t outAll = 31, outAny = 0;
        for (int i = 0; i < 3; i++) {
            const int16_t* r = t.verts[i];
            Corner& p = in[i];
            const float x = r[0], y = r[1], z = r[2];
            p.x = (x * s.mp[0][0] + y * s.mp[1][0] + z * s.mp[2][0] + s.mp[3][0]) * s.aspect;
            p.y = x * s.mp[0][1] + y * s.mp[1][1] + z * s.mp[2][1] + s.mp[3][1];
            p.w = x * s.mp[0][3] + y * s.mp[1][3] + z * s.mp[2][3] + s.mp[3][3];
            const float g = kHwClipGuard * p.w;
            const uint32_t code = (p.w < kHwClipNear ? 1 : 0) | (p.x < -g ? 2 : 0) | (p.x > g ? 4 : 0) |
                                  (p.y < -g ? 8 : 0) | (p.y > g ? 16 : 0);
            outAll &= code;
            outAny |= code;
        }
        if (outAll != 0) {
            sXboxClipGone++; // all of it beyond one plane
            continue;
        }
        if (outAny == 0) {
            memcpy(mHwClipVerts.data() + used, t.verts, sizeof(t.verts));
            used += kHwTriBytes;
            continue;
        }
        for (int i = 0; i < 3; i++) {
            const int16_t* r = t.verts[i];
            const uint8_t* c = (const uint8_t*)(r + 6);
            Corner& p = in[i];
            p.ob[0] = r[0];
            p.ob[1] = r[1];
            p.ob[2] = r[2];
            p.uv[0] = r[4];
            p.uv[1] = r[5];
            for (int k = 0; k < 4; k++) {
                p.c[k] = c[k];
            }
        }
        sXboxClipCut++;
        int n = 3;
        for (int plane = 0; plane < 5 && n >= 3; plane++) {
            auto inside = [plane](const Corner& p) {
                const float g = kHwClipGuard * p.w;
                switch (plane) {
                    case 0:
                        return p.w - kHwClipNear;
                    case 1:
                        return g + p.x;
                    case 2:
                        return g - p.x;
                    case 3:
                        return g + p.y;
                    default:
                        return g - p.y;
                }
            };
            int m = 0;
            for (int i = 0; i < n; i++) {
                const Corner& a = in[i];
                const Corner& b = in[i + 1 == n ? 0 : i + 1];
                const float da = inside(a), db = inside(b);
                if (da >= 0.0f) {
                    out[m++] = a;
                }
                if ((da >= 0.0f) != (db >= 0.0f)) {
                    const float f = da / (da - db);
                    const float* pa = (const float*)&a;
                    const float* pb = (const float*)&b;
                    float* pr = (float*)&out[m++];
                    for (size_t k = 0; k < sizeof(Corner) / sizeof(float); k++) {
                        pr[k] = pa[k] + (pb[k] - pa[k]) * f;
                    }
                }
            }
            n = m;
            std::swap(in, out);
        }
        const int tris = n < 3 ? 0 : std::min(n - 2, 6);
        const int16_t slot = t.verts[0][3];
        int16_t* o = (int16_t*)(mHwClipVerts.data() + used);
        used += tris * kHwTriBytes;
        auto round = [](float f) { return (int16_t)_mm_cvt_ss2si(_mm_set_ss(f)); };
        for (int i = 0; i < tris; i++) {
            const Corner* const corners[3] = { &in[0], &in[i + 1], &in[i + 2] };
            for (int k = 0; k < 3; k++, o += 8) {
                const Corner& p = *corners[k];
                o[0] = round(p.ob[0]);
                o[1] = round(p.ob[1]);
                o[2] = round(p.ob[2]);
                o[3] = slot;
                o[4] = round(p.uv[0]);
                o[5] = round(p.uv[1]);
                uint8_t* c = (uint8_t*)(o + 6);
                for (int j = 0; j < 4; j++) {
                    c[j] = (uint8_t)(p.c[j] + 0.5f);
                }
            }
        }
    }
    if (groupDraw != UINT32_MAX) {
        mRapi->SetRecordedClip(groupDraw, (uint32_t)groupStart, (uint32_t)((used - groupStart) / kHwTriBytes));
    }
    mRapi->SetClipVerts(mHwClipVerts.data());
}

// 0: triangles are cut every tick (CutCache.h)
#ifndef LUSX_CUT_CACHE
#define LUSX_CUT_CACHE 1
#endif
static constexpr int kHwCutMaxCorners = 12; // of a piece

#if LUSX_CUT_CACHE
static CutCache sHwCutCache;
static uint32_t sHwCutHits, sHwCutMisses, sHwCutSkipped, sHwCutChecked, sHwCutDiffer;
// the pieces of the cut being made, for the cache (or to compare with it: checkOnly, nothing goes to the batch)
static struct {
    bool on, checkOnly, overflow, lit;
    uint32_t numVerts, numPolys;
    CutVert verts[128];
    uint8_t sizes[48];

    void Begin(bool check, bool isLit) {
        on = true;
        checkOnly = check;
        overflow = false;
        lit = isLit;
        numVerts = numPolys = 0;
    }
    void Add(const LoadedVertex* poly, int n) {
        if (numPolys == sizeof(sizes) || numVerts + n > sizeof(verts) / sizeof(verts[0])) {
            overflow = true;
            return;
        }
        for (int i = 0; i < n; i++) {
            CutSave(verts[numVerts + i], poly[i], lit);
        }
        numVerts += n;
        sizes[numPolys++] = (uint8_t)n;
    }
} sHwCutRec;
#endif

// Vertices the game moves itself, a tick at a time (a skinned body, as Epona's: the limbs bend it on the CPU, into one
// of two buffers in turn, so no matrix says where it was). The game names them as it fills them, and each frame between
// ticks has them part of the way from the other buffer. Good for the tick being drawn: Run forgets them.
struct BlendRange {
    const F3DVtx* cur;
    const F3DVtx* prev;
    uint32_t count;
};
static BlendRange sBlendRanges[128];
static uint32_t sBlendRangeCount;

extern "C" void LusXbox_BlendVertices(const void* cur, const void* prev, uint32_t count) {
    if (sBlendRangeCount < sizeof(sBlendRanges) / sizeof(sBlendRanges[0])) {
        sBlendRanges[sBlendRangeCount++] = { (const F3DVtx*)cur, (const F3DVtx*)prev, count };
    }
}

// a raw vertex between two (the position rounded to the N64's shorts)
static LoadedVertex HwLerp(const LoadedVertex& a, const LoadedVertex& b, float t) {
    // the whole numbers in fixed point: no float and back for each
    const int32_t f = _mm_cvt_ss2si(_mm_set_ss(t * 65536.0f));
    auto mix = [f](int x, int y) { return x + (int)(((int64_t)(y - x) * f + 0x8000) >> 16); };
    LoadedVertex r = a;
    r.hwFrom = (uint8_t)(a.hwFrom | (b.hwFrom & kCutFromMask) | kCutFromLerped); // (a's root: this copy is of a)
    for (int k = 0; k < 3; k++) {
        r.ob[k] = (int16_t)mix(a.ob[k], b.ob[k]);
        r.n[k] = (int8_t)mix(a.n[k], b.n[k]);
        r.hwPrev[k] = (int16_t)mix(a.hwPrev[k], b.hwPrev[k]);
    }
    r.hwBlend = a.hwBlend && b.hwBlend;
    // (the limits are planes: a point between two inside them is inside them)
    r.hwRisk = a.hwRisk == 1 && b.hwRisk == 1 ? 1 : 0;
    r.hwOut = 0;
    r.u = a.u + (b.u - a.u) * t;
    r.v = a.v + (b.v - a.v) * t;
    r.color.r = (uint8_t)mix(a.color.r, b.color.r);
    r.color.g = (uint8_t)mix(a.color.g, b.color.g);
    r.color.b = (uint8_t)mix(a.color.b, b.color.b);
    r.color.a = (uint8_t)mix(a.color.a, b.color.a);
    r.alpha = (uint8_t)mix(a.alpha, b.alpha);
    return r;
}

// a coordinate beyond the tile's clamped range (Interpreter::mHwClampAxis)
bool Interpreter::HwClampReached(LoadedVertex* const v[3]) const {
    for (int a = 0; a < mHwClampAxes; a++) {
        const HwClampAxis& x = mHwClampAxis[a];
        for (int k = 0; k < 3; k++) {
            const float c = x.axisV ? v[k]->v : v[k]->u;
            if (c < x.lo || c > x.hi) {
                return true;
            }
        }
    }
    return false;
}

// the same matrices: a cut lerps in object space (states that differ only in fog, as a mesh's vertices
// loaded under two materials do, are fine: a piece keeps its own)
bool Interpreter::HwSameXform(uint16_t a, uint16_t b) const {
    if (a == b) {
        return true;
    }
    const auto& sa = mHwStates[a];
    const auto& sb = mHwStates[b];
    return memcmp(sa.mp, sb.mp, sizeof(sa.mp)) == 0 && memcmp(sa.mv, sb.mv, sizeof(sa.mv)) == 0;
}

// A triangle whose vertices have different matrices (a body's, across two limbs) has no object space to be
// cut in, but its clip space is linear in the barycentric coordinates: position = b0 P0 + b1 P1 + b2 P2. So
// it gets a state of its own, a matrix whose rows are its three clip-space corners, and barycentric
// coordinates for positions. It is cut like any other, and a frame between ticks moves each corner with its
// own limb (HwRebuildStates makes the rows again). The state is unlit: lit here, at the tick.
static constexpr int kHwBaryOne = 32767;

void Interpreter::HwFillTriState(GfxRenderingAPI::HwState& s, const HwRecipe& r) const {
    constexpr float kScale = 1.0f / kHwBaryOne;
    for (int i = 0; i < 3; i++) {
        const auto& src = mHwStates[r.triState[i]];
        const float x = r.triOb[i][0], y = r.triOb[i][1], z = r.triOb[i][2];
        for (int c = 0; c < 4; c++) {
            s.mp[i][c] = (x * src.mp[0][c] + y * src.mp[1][c] + z * src.mp[2][c] + src.mp[3][c]) * kScale;
        }
    }
    memset(s.mp[3], 0, sizeof(s.mp[3]));
}

bool Interpreter::HwTriState(LoadedVertex* poly) {
    if (mHwStates.size() >= 65535) {
        return false;
    }
    HwRecipe r;
    memset(&r, 0, sizeof(r));
    r.tri = true;
    for (int i = 0; i < 3; i++) {
        r.triState[i] = poly[i].state;
        memcpy(r.triOb[i], poly[i].ob, sizeof(r.triOb[i]));
    }
    GfxRenderingAPI::HwState s = mHwStates[poly[0].state]; // (its aspect and fog)
    HwFillTriState(s, r);
    memset(s.mv, 0, sizeof(s.mv));
    s.nscale = 0.0f;
    mHwStates.push_back(s);
    mHwRecipes.push_back(r);
    mHwStateUsed.resize(mHwStates.size(), 0);
    for (int i = 0; i < 3; i++) {
        mHwStateUsed[r.triState[i]] = 1; // what it's made from each frame
    }
    for (int i = 0; i < 3; i++) {
        poly[i].state = (uint16_t)(mHwStates.size() - 1);
        poly[i].hwRisk = 0;
        poly[i].ob[0] = i == 0 ? kHwBaryOne : 0;
        poly[i].ob[1] = i == 1 ? kHwBaryOne : 0;
        poly[i].ob[2] = i == 2 ? kHwBaryOne : 0;
        poly[i].hwBlend = false; // (its position is a matrix's now)
    }
    return true;
}

// A triangle that reaches the clamp, cut for the GPU's path. `a`: the first piece sets the batch's constants
// (HwTriangle); null: the batch has them already, and every piece goes straight in. False: nothing was
// emitted, the CPU's path takes it.
bool Interpreter::HwClampTriangle(LoadedVertex* const v[3], const HwTriArgs* a) {
    XboxTimed timed(sXboxCutCycles);
    sXboxCuts++;
    LoadedVertex poly[3] = { *v[0], *v[1], *v[2] };
    const bool limbs = !HwSameXform(v[0]->state, v[1]->state) || !HwSameXform(v[0]->state, v[2]->state);
    const bool litHere = limbs || v[0]->light != v[1]->light || v[0]->light != v[2]->light;
    if (litHere) {
        EnsureLit(&poly[0]); // lit here, as HwTriangle would (the pieces' colours blend from these)
        EnsureLit(&poly[1]);
        EnsureLit(&poly[2]);
    }
    if (limbs) {
        if (!HwTriState(poly)) {
            return false;
        }
        sXboxClampWhy[0]++;
    }
    mHwCutArgs = a;
#if LUSX_CUT_CACHE
    // (lit here, its colours are this tick's lights through this tick's matrices: a new key each tick, that would
    // only push out the ones that come back; and a body that moves by itself is a new key each tick too)
    if (!litHere && !poly[0].hwBlend && !poly[1].hwBlend && !poly[2].hwBlend) {
        return HwCutCached(poly);
    }
    sHwCutSkipped++;
#endif
    return HwClampSplit(poly, 3, mHwClampStep, mHwClampSteps);
}

// a piece into the batch, as triangles
bool Interpreter::HwCutEmit(const LoadedVertex* poly, int n) {
    for (int i = 1; i + 1 < n; i++) {
        LoadedVertex* const t[3] = { (LoadedVertex*)&poly[0], (LoadedVertex*)&poly[i], (LoadedVertex*)&poly[i + 1] };
        if (mHwCutArgs != nullptr) {
            const HwTriArgs* a = mHwCutArgs;
            mHwCutArgs = nullptr;
            if (!HwTriangle(t, a->cull, a->comb, a->use_alpha, a->use_fog, a->usedTextures, a->numInputs,
                            a->effective_tile, a->tex_width, a->tex_height, false)) {
                return false;
            }
        } else {
            HwEmit(t);
        }
        sXboxPieces++;
    }
    return true;
}

#if LUSX_CUT_CACHE
// The cut from the cache, or made and kept (CutCache.h says why it can't go out of date). A piece's corners go in
// through the same HwCutEmit, in the same order. One hit in 64 is cut again here and compared: a cut that came out
// differently is in the log (`differ`), and the cache is switched off with LUSX_CUT_CACHE 0.
bool Interpreter::HwCutCached(LoadedVertex* poly) {
    const bool lit = poly[0].light != 0; // (all three's: litHere's otherwise)
    CutKey key;
    CutMark(poly);
    for (int i = 0; i < 3; i++) {
        key.AddVertex(poly[i], lit);
    }
    for (int i = 0; i < mHwClampSteps; i++) {
        key.AddStep(mHwClampStep[i].axisV, mHwClampStep[i].upper, mHwClampStep[i].bound);
    }
    CutCache::Found found;
    if (sHwCutCache.Find(key, &found)) {
        sHwCutHits++;
        if ((sHwCutHits & 63) == 0) {
            sHwCutRec.Begin(true, lit);
            HwClampSplit(poly, 3, mHwClampStep, mHwClampSteps);
            sHwCutRec.on = false;
            sHwCutChecked++;
            sHwCutDiffer += !sHwCutRec.overflow &&
                            (sHwCutRec.numVerts != found.numVerts || sHwCutRec.numPolys != found.polys ||
                             memcmp(sHwCutRec.verts, found.verts, found.numVerts * sizeof(CutVert)) != 0 ||
                             memcmp(sHwCutRec.sizes, found.sizes, found.polys) != 0);
        }
        const CutVert* in = found.verts;
        for (uint32_t p = 0; p < found.polys; p++) {
            LoadedVertex piece[kHwCutMaxCorners];
            const int n = found.sizes[p];
            for (int i = 0; i < n; i++) {
                CutRestore(piece[i], in[i], poly, lit);
            }
            in += n;
            if (!HwCutEmit(piece, n)) {
                return false;
            }
        }
        return true;
    }
    sHwCutMisses++;
    sHwCutRec.Begin(false, lit);
    const bool ok = HwClampSplit(poly, 3, mHwClampStep, mHwClampSteps);
    sHwCutRec.on = false;
    if (ok && !sHwCutRec.overflow) {
        sHwCutCache.Put(key, sHwCutRec.verts, sHwCutRec.numVerts, sHwCutRec.sizes, sHwCutRec.numPolys);
    }
    return ok;
}
#endif

// as the backend's ClampSplit on the CPU path's packed vertices, on raw ones. False: the GPU's path can't
// draw these (only ever the first piece: nothing was emitted).
bool Interpreter::HwClampSplit(const LoadedVertex* poly, int n, const HwClampStep* steps, int numSteps) {
    constexpr int kMax = kHwCutMaxCorners;
    if (n < 3) {
        return true;
    }
    if (numSteps == 0) {
#if LUSX_CUT_CACHE
        if (sHwCutRec.on) {
            sHwCutRec.Add(poly, n);
            if (sHwCutRec.checkOnly) {
                return true;
            }
        }
#endif
        return HwCutEmit(poly, n);
    }
    const HwClampStep& s = steps[0];
    float d[kMax];
    bool anyIn = false, anyOut = false;
    for (int i = 0; i < n; i++) {
        float c = s.axisV ? poly[i].v : poly[i].u;
        d[i] = s.upper ? s.bound - c : c - s.bound;
        anyIn |= d[i] > 0.0f;
        anyOut |= d[i] < 0.0f;
    }
    if (!anyOut) {
        return HwClampSplit(poly, n, steps + 1, numSteps - 1);
    }
    LoadedVertex inside[kMax], beyond[kMax];
    int ni = 0, nb = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        if (d[i] >= 0.0f && ni < kMax) {
            inside[ni++] = poly[i];
        }
        if (d[i] <= 0.0f && nb < kMax) {
            beyond[nb++] = poly[i];
        }
        if ((d[i] > 0.0f && d[j] < 0.0f) || (d[i] < 0.0f && d[j] > 0.0f)) {
            LoadedVertex x = HwLerp(poly[i], poly[j], d[i] / (d[i] - d[j]));
            if (ni < kMax) {
                inside[ni++] = x;
            }
            if (nb < kMax) {
                beyond[nb++] = x;
            }
        }
    }
    for (int i = 0; i < nb; i++) {
        (s.axisV ? beyond[i].v : beyond[i].u) = s.bound;
    }
    if (anyIn && !HwClampSplit(inside, ni, steps + 1, numSteps - 1)) {
        return false;
    }
    return HwClampSplit(beyond, nb, steps + 1, numSteps - 1);
}
#endif

Interpreter::~Interpreter() {
    delete mRsp;
    delete mRdp;
    delete[] mBufVbo;
}

static std::weak_ptr<Interpreter> mInstance;
#ifdef LUS_XBOX
// and a raw one: the display-list handlers ask for it ~10,000 times a frame, and a weak_ptr's lock is two
// locked (atomic) operations; the interpreter lives as long as the window
static Interpreter* sInstanceRaw;
#endif
// Set a cached pointer to the instance so we don't need to go through the window every time
void GfxSetInstance(std::shared_ptr<Interpreter> gfx) {
    mInstance = gfx;
#ifdef LUS_XBOX
    sInstanceRaw = gfx.get();
#endif
}

static inline Interpreter* GfxInstance() {
#ifdef LUS_XBOX
    return sInstanceRaw;
#else
    return mInstance.lock().get();
#endif
}

// N64 prim_depth is 15-bit (0 near, 0x7FFF far).
static constexpr float N64_PRIM_DEPTH_MAX = 32767.0f;

void Interpreter::Flush() {
#ifdef LUS_XBOX
    if (mHwTris > 0) {
        HwFlush(); // the two batches are never both filled (GfxSpTri1): the order is kept
    }
#endif
    if (mBufVboLen > 0) {
        mRapi->SetCurrentPrimDepth((float)mRdp->prim_depth / N64_PRIM_DEPTH_MAX);
#ifdef LUS_XBOX
        uint64_t t0 = XBOX_CYCLES();
        mRapi->DrawTriangles(mBufVbo, mBufVboLen, mBufVboNumTris);
        sXboxDrawCycles += XBOX_CYCLES() - t0;
        sXboxDraws++;
#else
        mRapi->DrawTriangles(mBufVbo, mBufVboLen, mBufVboNumTris);
#endif
        mBufVboLen = 0;
        mBufVboNumTris = 0;
    }
}

ShaderProgram* Interpreter::LookupOrCreateShaderProgram(uint64_t id0, uint64_t id1) {
    ShaderProgram* prg = mRapi->LookupShader(id0, id1);
    if (prg == nullptr) {
        mRapi->UnloadShader(mRenderingState.mShaderProgram);
        prg = mRapi->CreateAndLoadNewShader(id0, id1);
        mRenderingState.mShaderProgram = prg;
    }
    return prg;
}

const char* Interpreter::CCMUXtoStr(uint32_t ccmux) {
    static constexpr std::array tbl = {
        "G_CCMUX_COMBINED",
        "G_CCMUX_TEXEL0",
        "G_CCMUX_TEXEL1",
        "G_CCMUX_PRIMITIVE",
        "G_CCMUX_SHADE",
        "G_CCMUX_ENVIRONMENT",
        "G_CCMUX_1",
        "G_CCMUX_COMBINED_ALPHA",
        "G_CCMUX_TEXEL0_ALPHA",
        "G_CCMUX_TEXEL1_ALPHA",
        "G_CCMUX_PRIMITIVE_ALPHA",
        "G_CCMUX_SHADE_ALPHA",
        "G_CCMUX_ENV_ALPHA",
        "G_CCMUX_LOD_FRACTION",
        "G_CCMUX_PRIM_LOD_FRAC",
        "G_CCMUX_K5",
    };
    if (ccmux > tbl.size()) {
        return "G_CCMUX_0";
    }
    return tbl[ccmux];
}

// Seems unused
const char* Interpreter::ACMUXtoStr(uint32_t acmux) {
    static constexpr std::array tbl = {
        "G_ACMUX_COMBINED or G_ACMUX_LOD_FRACTION",
        "G_ACMUX_TEXEL0",
        "G_ACMUX_TEXEL1",
        "G_ACMUX_PRIMITIVE",
        "G_ACMUX_SHADE",
        "G_ACMUX_ENVIRONMENT",
        "G_ACMUX_1 or G_ACMUX_PRIM_LOD_FRAC",
        "G_ACMUX_0",
    };
    return tbl[acmux];
}

void Interpreter::GenerateCC(ColorCombiner* comb, const ColorCombinerKey& key) {
    const bool is2Cyc = (key.options & SHADER_OPT(_2CYC)) != 0;

    uint8_t c[2][2][4];
    uint64_t shaderId0 = 0;
    uint64_t shaderId1 = key.options;
    uint8_t shaderInputMapping[2][7] = { { 0 } };
    bool usedTextures[2]{};
    for (uint32_t i = 0; i < 2 && (i == 0 || is2Cyc); i++) {
        uint32_t rgbA = (key.combine_mode >> (i * 28)) & 0xf;
        uint32_t rgbB = (key.combine_mode >> (i * 28 + 4)) & 0xf;
        uint32_t rgbC = (key.combine_mode >> (i * 28 + 8)) & 0x1f;
        uint32_t rgbD = (key.combine_mode >> (i * 28 + 13)) & 7;
        uint32_t alphaA = (key.combine_mode >> (i * 28 + 16)) & 7;
        uint32_t alphaB = (key.combine_mode >> (i * 28 + 16 + 3)) & 7;
        uint32_t alphaC = (key.combine_mode >> (i * 28 + 16 + 6)) & 7;
        uint32_t alphaD = (key.combine_mode >> (i * 28 + 16 + 9)) & 7;

        if (rgbA >= 8) {
            rgbA = G_CCMUX_0;
        }
        if (rgbB >= 8) {
            rgbB = G_CCMUX_0;
        }
        if (rgbC >= 16) {
            rgbC = G_CCMUX_0;
        }
        if (rgbD == 7) {
            rgbD = G_CCMUX_0;
        }

        if (rgbA == rgbB || rgbC == G_CCMUX_0) {
            // Normalize
            rgbA = G_CCMUX_0;
            rgbB = G_CCMUX_0;
            rgbC = G_CCMUX_0;
        }
        if (alphaA == alphaB || alphaC == G_ACMUX_0) {
            // Normalize
            alphaA = G_ACMUX_0;
            alphaB = G_ACMUX_0;
            alphaC = G_ACMUX_0;
        }
        if (i == 1) {
            if (rgbA != G_CCMUX_COMBINED && rgbB != G_CCMUX_COMBINED && rgbC != G_CCMUX_COMBINED &&
                rgbD != G_CCMUX_COMBINED) {
                // First cycle RGB not used, so clear it away
                c[0][0][0] = c[0][0][1] = c[0][0][2] = c[0][0][3] = G_CCMUX_0;
            }
            if (rgbC != G_CCMUX_COMBINED_ALPHA && alphaA != G_ACMUX_COMBINED && alphaB != G_ACMUX_COMBINED &&
                alphaD != G_ACMUX_COMBINED) {
                // First cycle ALPHA not used, so clear it away
                c[0][1][0] = c[0][1][1] = c[0][1][2] = c[0][1][3] = G_ACMUX_0;
            }
        }

        c[i][0][0] = rgbA;
        c[i][0][1] = rgbB;
        c[i][0][2] = rgbC;
        c[i][0][3] = rgbD;
        c[i][1][0] = alphaA;
        c[i][1][1] = alphaB;
        c[i][1][2] = alphaC;
        c[i][1][3] = alphaD;
    }
    if (!is2Cyc) {
        for (uint32_t i = 0; i < 2; i++) {
            for (uint32_t k = 0; k < 4; k++) {
                c[1][i][k] = i == 0 ? G_CCMUX_0 : G_ACMUX_0;
            }
        }

        // In 1-cycle mode, TEXEL1 returns the same value as TEXEL0.
        // Remap combiner inputs so the shader samples from the correct slot.
        // Ex: TEXEL1/TEXEL1_ALPHA → TEXEL0/TEXEL0_ALPHA
        for (uint32_t k = 0; k < 4; k++) {
            if (c[0][0][k] == G_CCMUX_TEXEL1)
                c[0][0][k] = G_CCMUX_TEXEL0;
            if (c[0][0][k] == G_CCMUX_TEXEL1_ALPHA)
                c[0][0][k] = G_CCMUX_TEXEL0_ALPHA;
            if (c[0][1][k] == G_ACMUX_TEXEL1)
                c[0][1][k] = G_ACMUX_TEXEL0;
        }
    }
    {
        uint8_t inputNumber[32] = { 0 };
        uint32_t nextInputNumber = SHADER_INPUT_1;
        for (uint32_t i = 0; i < 2 && (i == 0 || is2Cyc); i++) {
            for (uint32_t j = 0; j < 4; j++) {
                // Mux values 6/7/15 are overloaded by slot. Only B (value 6 = CENTER, 7 = K4)
                // and C (value 6 = SCALE, 15 = K5) carry chroma-key/convert inputs; A and D
                // reuse those values for unrelated constants.
                if (j == 1 && c[i][0][j] == G_CCMUX_CENTER) {
                    c[i][0][j] = G_CCMUX_KEY_CENTER;
                } else if (j == 1 && c[i][0][j] == G_CCMUX_K4) {
                    c[i][0][j] = G_CCMUX_CONVERT_K4;
                } else if (j == 2 && c[i][0][j] == G_CCMUX_SCALE) {
                    c[i][0][j] = G_CCMUX_KEY_SCALE;
                } else if (j == 2 && c[i][0][j] == G_CCMUX_K5) {
                    c[i][0][j] = G_CCMUX_CONVERT_K5;
                }
                uint32_t val = 0;
                switch (c[i][0][j]) {
                    case G_CCMUX_0:
                        val = SHADER_0;
                        break;
                    case G_CCMUX_1:
                        val = SHADER_1;
                        break;
                    case G_CCMUX_TEXEL0:
                        val = SHADER_TEXEL0;
                        // Set the opposite texture when reading from the second cycle color options
                        if (i == 0) {
                            usedTextures[0] = true;
                        } else {
                            usedTextures[1] = true;
                        }
                        break;
                    case G_CCMUX_TEXEL1:
                        val = SHADER_TEXEL1;
                        if (i == 0) {
                            usedTextures[1] = true;
                        } else {
                            usedTextures[0] = true;
                        }
                        break;
                    case G_CCMUX_TEXEL0_ALPHA:
                        val = SHADER_TEXEL0A;
                        if (i == 0) {
                            usedTextures[0] = true;
                        } else {
                            usedTextures[1] = true;
                        }
                        break;
                    case G_CCMUX_TEXEL1_ALPHA:
                        val = SHADER_TEXEL1A;
                        if (i == 0) {
                            usedTextures[1] = true;
                        } else {
                            usedTextures[0] = true;
                        }
                        break;
                    case G_CCMUX_NOISE:
                        val = SHADER_NOISE;
                        break;
                    case G_CCMUX_PRIMITIVE:
                    case G_CCMUX_PRIMITIVE_ALPHA:
                    case G_CCMUX_PRIM_LOD_FRAC:
                    case G_CCMUX_SHADE:
                    case G_CCMUX_ENVIRONMENT:
                    case G_CCMUX_ENV_ALPHA:
                    case G_CCMUX_LOD_FRACTION:
                    case G_CCMUX_KEY_CENTER:
                    case G_CCMUX_KEY_SCALE:
                    case G_CCMUX_CONVERT_K4:
                    case G_CCMUX_CONVERT_K5:
                        if (inputNumber[c[i][0][j]] == 0) {
                            shaderInputMapping[0][nextInputNumber - 1] = c[i][0][j];
                            inputNumber[c[i][0][j]] = nextInputNumber++;
                        }
                        val = inputNumber[c[i][0][j]];
                        break;
                    case G_CCMUX_COMBINED:
                        val = SHADER_COMBINED;
                        break;
                    default:
                        fprintf(stderr, "Unsupported ccmux: %d\n", c[i][0][j]);
                        break;
                }
                shaderId0 |= (uint64_t)val << (i * 32 + j * 4);
            }
        }
    }
    {
        uint8_t inputNumber[16] = { 0 };
        uint32_t nextInputNumber = SHADER_INPUT_1;
        for (uint32_t i = 0; i < 2; i++) {
            for (uint32_t j = 0; j < 4; j++) {
                uint32_t val = 0;
                switch (c[i][1][j]) {
                    case G_ACMUX_0:
                        val = SHADER_0;
                        break;
                    case G_ACMUX_TEXEL0:
                        val = SHADER_TEXEL0;
                        // Set the opposite texture when reading from the second cycle color options
                        if (i == 0) {
                            usedTextures[0] = true;
                        } else {
                            usedTextures[1] = true;
                        }
                        break;
                    case G_ACMUX_TEXEL1:
                        val = SHADER_TEXEL1;
                        if (i == 0) {
                            usedTextures[1] = true;
                        } else {
                            usedTextures[0] = true;
                        }
                        break;
                    case G_ACMUX_LOD_FRACTION:
                        // case G_ACMUX_COMBINED: same numerical value
                        if (j != 2) {
                            val = SHADER_COMBINED;
                            break;
                        }
                        c[i][1][j] = G_CCMUX_LOD_FRACTION;
                        [[fallthrough]]; // for G_ACMUX_LOD_FRACTION
                    case G_ACMUX_1:
                        // case G_ACMUX_PRIM_LOD_FRAC: same numerical value
                        if (j != 2) {
                            val = SHADER_1;
                            break;
                        }
                        [[fallthrough]]; // for G_ACMUX_PRIM_LOD_FRAC
                    case G_ACMUX_PRIMITIVE:
                    case G_ACMUX_SHADE:
                    case G_ACMUX_ENVIRONMENT:
                        if (inputNumber[c[i][1][j]] == 0) {
                            shaderInputMapping[1][nextInputNumber - 1] = c[i][1][j];
                            inputNumber[c[i][1][j]] = nextInputNumber++;
                        }
                        val = inputNumber[c[i][1][j]];
                        break;
                }
                shaderId0 |= (uint64_t)val << (i * 32 + 16 + j * 4);
            }
        }
    }
    comb->shader_id0 = shaderId0;
    comb->shader_id1 = shaderId1;
    comb->usedTextures[0] = usedTextures[0];
    comb->usedTextures[1] = usedTextures[1];
    // comb->prg = gfx_lookup_or_create_mShaderProgram(shader_id0, shader_id1);
    memcpy(comb->shader_input_mapping, shaderInputMapping, sizeof(shaderInputMapping));
}

ColorCombiner* Interpreter::LookupOrCreateColorCombiner(const ColorCombinerKey& key) {
    if (mPrevCombiner != mColorCombinerPool.end() && mPrevCombiner->first == key) {
        return &mPrevCombiner->second;
    }
    mPrevCombiner = mColorCombinerPool.find(key);
    if (mPrevCombiner != mColorCombinerPool.end()) {
        return &mPrevCombiner->second;
    }
    Flush();
    mPrevCombiner = mColorCombinerPool.insert(std::make_pair(key, ColorCombiner())).first;
    GenerateCC(&mPrevCombiner->second, key);
    return &mPrevCombiner->second;
}

void Interpreter::SetResolvedResourceCacheEnabled(bool enabled) {
    mResolvedResourceCacheEnabled = enabled;
    if (!enabled) {
        mResolvedResourceCache.clear();
    }
}

// Texture binds resolve the same paths every frame; skip the resource
// manager's string/hash/mutex work by memoizing on the pointer.
std::shared_ptr<Ship::IResource> Interpreter::ResolveResourceCached(const char* path) {
    if (path == nullptr) {
        return nullptr;
    }
    if (!mResolvedResourceCacheEnabled) {
        return Ship::Context::GetRawInstance()->GetResourceManager()->LoadResourceProcess(path);
    }
    auto it = mResolvedResourceCache.find(path);
#ifdef LUS_XBOX
    if (it != mResolvedResourceCache.end() && strcmp(it->second.first.c_str(), path) == 0) {
        return it->second.second;
    }
#else
    if (it != mResolvedResourceCache.end()) {
        return it->second;
    }
#endif
    auto res = Ship::Context::GetRawInstance()->GetResourceManager()->LoadResourceProcess(path);
    // Only memoize a hit. The resource manager caches its own misses, so re-asking
    // for one is cheap, and CacheExternalResource can turn a path that missed into a
    // valid resource at runtime. A memoized null would outlive the resource itself.
    if (res != nullptr) {
#ifdef LUS_XBOX
        mResolvedResourceCache[path] = { path, res };
#else
        mResolvedResourceCache[path] = res;
#endif
    }
    return res;
}

#ifdef LUS_XBOX
extern "C" void LusXbox_Where(const char* where); // xbox_main.cpp's watchdog
#endif

void Interpreter::TextureCacheClear() {
#ifdef LUS_XBOX
    LusXbox_Where("clearing the texture cache");
#endif
    for (const auto& entry : mTextureCache.map) {
#ifdef LUS_XBOX
        // freed now, not when the id is next reused: the evicted textures held MBs on the Xbox
        mRapi->DeleteTexture(entry.second.texture_id);
#endif
        mTextureCache.free_texture_ids.push_back(entry.second.texture_id);
    }
    mTextureCache.map.clear();
    mTextureCache.lru.clear();
    mResolvedResourceCache.clear();
    // Pre-allocate buckets so the map never rehashes during normal operation.
    // Rehashing invalidates all iterators, including those stored in LRU entries.
    mTextureCache.map.reserve(TEXTURE_CACHE_MAX_SIZE);
    // Null rendering-state pointers — they pointed into map nodes that are now freed.
    std::fill(std::begin(mRenderingState.mTextures), std::end(mRenderingState.mTextures), nullptr);
#ifdef LUS_XBOX
    // and the loaded tiles imported again at their next draw: a tile only is when marked changed, and a
    // draw with the tile still loaded bound the texture just deleted (xemu aborts on the empty stage)
    mRdp->textures_changed[0] = true;
    mRdp->textures_changed[1] = true;
    sXboxRawByHash.clear();
    sXboxResourceByHash.clear();
    for (XboxTextureSlot& slot : sXboxTextureSlots) {
        slot.hash = 0;
        slot.texture.reset();
    }
#endif
}

void Interpreter::ShaderCacheClear() {
    mRapi->ClearShaderCache();
}

bool Interpreter::TextureCacheLookup(int i, const TextureCacheKey& key) {
    TextureCacheMap::iterator it = mTextureCache.map.find(key);
    TextureCacheNode** n = &mRenderingState.mTextures[i];

    if (it != mTextureCache.map.end()) {
#ifdef LUS_XBOX
        // the GPU's batch ends only when the bound texture changes (GfxSpTri1 flushed before every
        // import, so every G_SETTILE of a model's repeated texture cut a draw)
        if (*n != &*it) {
            Flush();
        }
#endif
        mRapi->SelectTexture(i, it->second.texture_id);
        *n = &*it;
        mTextureCache.lru.splice(mTextureCache.lru.end(), mTextureCache.lru,
                                 it->second.lru_location); // move to back
        return true;
    }

    if (mTextureCache.map.size() >= TEXTURE_CACHE_MAX_SIZE) {
        // Remove the texture that was least recently used
        it = mTextureCache.lru.front().it;
#ifdef LUS_XBOX
        mRapi->DeleteTexture(it->second.texture_id);
#endif
        mTextureCache.free_texture_ids.push_back(it->second.texture_id);
        for (int j = 0; j < SHADER_MAX_TEXTURES; j++) {
            if (mRenderingState.mTextures[j] == &*it)
                mRenderingState.mTextures[j] = nullptr;
        }
        mTextureCache.map.erase(it);
        mTextureCache.lru.pop_front();
    }

    uint32_t texture_id;
    if (!mTextureCache.free_texture_ids.empty()) {
        texture_id = mTextureCache.free_texture_ids.back();
        mTextureCache.free_texture_ids.pop_back();
    } else {
        texture_id = mRapi->NewTexture();
    }

    it = mTextureCache.map.insert(std::make_pair(key, TextureCacheValue())).first;
    TextureCacheNode* node = &*it;
    node->second.texture_id = texture_id;
    node->second.lru_location = mTextureCache.lru.insert(mTextureCache.lru.end(), { it });

#ifdef LUS_XBOX
    Flush(); // a new texture: the batch so far drawn with the one bound
#endif
    mRapi->SelectTexture(i, texture_id);
    mRapi->SetSamplerParameters(i, false, 0, 0);
    *n = node;
    return false;
}

std::string_view Interpreter::GetBaseTexturePath(std::string_view path) {
    if (path.starts_with(Ship::IResource::gAltAssetPrefix)) {
        return path.substr(Ship::IResource::gAltAssetPrefix.length());
    }

    return path;
}

void Interpreter::TextureCacheDelete(const uint8_t* origAddr) {
    while (mTextureCache.map.bucket_count() > 0) {
        TextureCacheKey key = { origAddr, { 0 }, 0, 0, 0 }; // bucket index only depends on the address
        size_t bucket = mTextureCache.map.bucket(key);
        bool again = false;
        for (auto it = mTextureCache.map.begin(bucket); it != mTextureCache.map.end(bucket); ++it) {
            if (it->first.texture_addr == origAddr) {
                for (int j = 0; j < SHADER_MAX_TEXTURES; j++) {
                    if (mRenderingState.mTextures[j] == &*it)
                        mRenderingState.mTextures[j] = nullptr;
                }
                mTextureCache.lru.erase(it->second.lru_location);
#ifdef LUS_XBOX
                mRapi->DeleteTexture(it->second.texture_id);
#endif
                mTextureCache.free_texture_ids.push_back(it->second.texture_id);
                mTextureCache.map.erase(it->first);
                again = true;
                break;
            }
        }
        if (!again) {
            break;
        }
    }
}

// Pick the per-line byte width for texture decode. Prefer the DRAM stride from
// loaded_texture when it looks like real per-line info (differs from total size).
// Fall back to the TMEM tile stride when loaded sizes match total (LoadBlock with
// width=1, where line_size == full_image_line_size == size).
static uint32_t GetEffectiveLineSize(uint32_t lineSizeBytes, uint32_t fullImageLineSizeBytes, uint32_t sizeBytes,
                                     uint32_t tileLineSizeBytes) {
    if ((lineSizeBytes != sizeBytes || fullImageLineSizeBytes != sizeBytes) && lineSizeBytes > 0) {
        return lineSizeBytes;
    }
    return tileLineSizeBytes;
}

static uint32_t GetTileSizeFromCoordinates(float low, float high) {
    // An unset tile (high <= low) defines no region; return 0 so callers skip the tile-region clamp
    // instead of collapsing the texture to the phantom 1-texel size the +4 formula would yield.
    if (high <= low) {
        return 0;
    }
    return static_cast<uint32_t>(lroundf((high - low + 4.0f) / 4.0f));
}

// Loaded size is HD-scaled but the tile region is raw N64 texels; normalize before comparing.
static bool IsPyramidLike(uint32_t width, uint32_t height, uint32_t tileW, uint32_t tileH, float hByteScale,
                          float vPixelScale) {
    const float hs = hByteScale > 0.0f ? hByteScale : 1.0f;
    const float vs = vPixelScale > 0.0f ? vPixelScale : 1.0f;
    const uint32_t loadedPixels = static_cast<uint32_t>(width / hs) * static_cast<uint32_t>(height / vs);
    const uint32_t renderedPixels = tileW * tileH;
    return renderedPixels > 0 && loadedPixels > renderedPixels && loadedPixels * 8 < renderedPixels * 13; // < 1.625x
}

void Interpreter::ImportTextureRgba16(int tile, bool importReplacement) {
    const RawTexMetadata* metadata = &mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].raw_tex_metadata;
    const uint8_t* addr =
        importReplacement && (metadata->resource != nullptr)
            ? mMaskedTextures.find(GetBaseTexturePath(metadata->resource->GetInitData()->Path))->second.replacementData
            : mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].addr;

    if (addr == nullptr) {
        SPDLOG_ERROR("ImportTextureRgba16: null texture address for tile {}", tile);
        return;
    }

    uint32_t sizeBytes = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].size_bytes;
    uint32_t fullImageLineSizeBytes =
        mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].full_image_line_size_bytes;
    uint32_t line_size_bytes = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].line_size_bytes;

    uint32_t widthBytes = GetEffectiveLineSize(line_size_bytes, fullImageLineSizeBytes, sizeBytes,
                                               mRdp->texture_tile[tile].line_size_bytes);
    uint32_t width = widthBytes / 2;
    uint32_t height = widthBytes > 0 ? sizeBytes / widthBytes : 0;

    // Clamp to the rendered region only when the loaded buffer is ~1.33x of it (mipmap
    // pyramid signature). Window-scrolling tiles have loaded ≈ rendered or loaded >> rendered;
    // skip both. CLAMP wrap mode always opts in.
    uint32_t tile_w = GetTileSizeFromCoordinates(mRdp->texture_tile[tile].uls, mRdp->texture_tile[tile].lrs);
    uint32_t tile_h = GetTileSizeFromCoordinates(mRdp->texture_tile[tile].ult, mRdp->texture_tile[tile].lrt);
    bool pyramidLike = IsPyramidLike(width, height, tile_w, tile_h, metadata->h_byte_scale, metadata->v_pixel_scale);
    bool clampS = (mRdp->texture_tile[tile].cms & G_TX_CLAMP) != 0;
    bool clampT = (mRdp->texture_tile[tile].cmt & G_TX_CLAMP) != 0;
    // A masked axis wraps every 2^mask texels, so trim an over-loaded texture back to that.
    // Skip a mask smaller than the tile region though - that's stale tile state, not a real load.
    uint32_t maskW = mRdp->texture_tile[tile].masks;
    uint32_t maskH = mRdp->texture_tile[tile].maskt;
    if (maskW != 0 && (1u << maskW) >= tile_w && (1u << maskW) < width) {
        width = 1u << maskW;
    }
    if (maskH != 0 && (1u << maskH) >= tile_h && (1u << maskH) < height) {
        height = 1u << maskH;
    }
    if ((pyramidLike || clampS) && tile_w > 0 && tile_w < width) {
        width = tile_w;
    }
    if ((pyramidLike || clampT) && tile_h > 0 && tile_h < height) {
        height = tile_h;
    }

    // A single line of pixels should not equal the entire image (height == 1 non-withstanding)
    if (fullImageLineSizeBytes == sizeBytes) {
        fullImageLineSizeBytes = width * 2;
    }

    uint32_t i = 0;

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t clrIdx = (y * (fullImageLineSizeBytes / 2)) + (x);

            uint16_t col16 = (addr[2 * clrIdx] << 8) | addr[2 * clrIdx + 1];
            uint8_t a = col16 & 1;
            uint8_t r = col16 >> 11;
            uint8_t g = (col16 >> 6) & 0x1f;
            uint8_t b = (col16 >> 1) & 0x1f;
            mTexUploadBuffer[4 * i + 0] = SCALE_5_8(r);
            mTexUploadBuffer[4 * i + 1] = SCALE_5_8(g);
            mTexUploadBuffer[4 * i + 2] = SCALE_5_8(b);
            mTexUploadBuffer[4 * i + 3] = a ? 255 : 0;

            i++;
        }
    }

    UploadImported(mTexUploadBuffer, width, height);
}

void Interpreter::ImportTextureRgba32(int tile, bool importReplacement) {
    const RawTexMetadata* metadata = &mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].raw_tex_metadata;
    const uint8_t* addr =
        importReplacement && (metadata->resource != nullptr)
            ? mMaskedTextures.find(GetBaseTexturePath(metadata->resource->GetInitData()->Path))->second.replacementData
            : mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].addr;

    if (addr == nullptr) {
        SPDLOG_ERROR("ImportTextureRgba32: null texture address for tile {}", tile);
        return;
    }

    uint32_t size_bytes = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].size_bytes;
    uint32_t full_image_line_size_bytes =
        mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].full_image_line_size_bytes;
    uint32_t line_size_bytes = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].line_size_bytes;

    uint32_t widthBytes = GetEffectiveLineSize(line_size_bytes, full_image_line_size_bytes, size_bytes,
                                               mRdp->texture_tile[tile].line_size_bytes * 2);
    uint32_t width = widthBytes / 4;
    uint32_t height = widthBytes > 0 ? size_bytes / widthBytes : 0;

    // Clamp to the rendered region only when the loaded buffer is ~1.33x of it (mipmap
    // pyramid signature). Window-scrolling tiles have loaded ≈ rendered or loaded >> rendered;
    // skip both. CLAMP wrap mode always opts in.
    uint32_t tile_w = GetTileSizeFromCoordinates(mRdp->texture_tile[tile].uls, mRdp->texture_tile[tile].lrs);
    uint32_t tile_h = GetTileSizeFromCoordinates(mRdp->texture_tile[tile].ult, mRdp->texture_tile[tile].lrt);
    bool pyramidLike = IsPyramidLike(width, height, tile_w, tile_h, metadata->h_byte_scale, metadata->v_pixel_scale);
    bool clampS = (mRdp->texture_tile[tile].cms & G_TX_CLAMP) != 0;
    bool clampT = (mRdp->texture_tile[tile].cmt & G_TX_CLAMP) != 0;
    // A masked axis wraps every 2^mask texels, so trim an over-loaded texture back to that.
    // Skip a mask smaller than the tile region though - that's stale tile state, not a real load.
    uint32_t maskW = mRdp->texture_tile[tile].masks;
    uint32_t maskH = mRdp->texture_tile[tile].maskt;
    if (maskW != 0 && (1u << maskW) >= tile_w && (1u << maskW) < width) {
        width = 1u << maskW;
    }
    if (maskH != 0 && (1u << maskH) >= tile_h && (1u << maskH) < height) {
        height = 1u << maskH;
    }
    if ((pyramidLike || clampS) && tile_w > 0 && tile_w < width) {
        width = tile_w;
    }
    if ((pyramidLike || clampT) && tile_h > 0 && tile_h < height) {
        height = tile_h;
    }

    if (full_image_line_size_bytes == size_bytes) {
        full_image_line_size_bytes = width * 4;
    }

    // Copy pixel by pixel, respecting full image stride (handles sub-tile loads)
    uint32_t fullImageStridePixels = full_image_line_size_bytes / 4;
    uint32_t i = 0;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t srcIdx = y * fullImageStridePixels + x;
            mTexUploadBuffer[4 * i + 0] = addr[4 * srcIdx + 0];
            mTexUploadBuffer[4 * i + 1] = addr[4 * srcIdx + 1];
            mTexUploadBuffer[4 * i + 2] = addr[4 * srcIdx + 2];
            mTexUploadBuffer[4 * i + 3] = addr[4 * srcIdx + 3];
            i++;
        }
    }
    UploadImported(mTexUploadBuffer, width, height);
}

void Interpreter::ImportTextureIA4(int tile, bool importReplacement) {
    const RawTexMetadata* metadata = &mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].raw_tex_metadata;
    const uint8_t* addr =
        importReplacement && (metadata->resource != nullptr)
            ? mMaskedTextures.find(GetBaseTexturePath(metadata->resource->GetInitData()->Path))->second.replacementData
            : mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].addr;

    if (addr == nullptr) {
        SPDLOG_ERROR("ImportTextureIA4: null texture address for tile {}", tile);
        return;
    }

    uint32_t sizeBytes = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].size_bytes;
    uint32_t fullImageLineSizeBytes =
        mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].full_image_line_size_bytes;
    uint32_t lineSizeBytes = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].line_size_bytes;

    uint32_t widthBytes = GetEffectiveLineSize(lineSizeBytes, fullImageLineSizeBytes, sizeBytes,
                                               mRdp->texture_tile[tile].line_size_bytes);
    uint32_t width = widthBytes * 2;
    uint32_t height = widthBytes > 0 ? sizeBytes / widthBytes : 0;

    if (fullImageLineSizeBytes == sizeBytes) {
        fullImageLineSizeBytes = widthBytes;
    }

    uint32_t i = 0;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t srcPixelIdx = y * (fullImageLineSizeBytes * 2) + x;
            uint8_t byte = addr[srcPixelIdx / 2];
            uint8_t part = (byte >> (4 - (srcPixelIdx % 2) * 4)) & 0xf;
            uint8_t intensity = part >> 1;
            uint8_t alpha = part & 1;
            mTexUploadBuffer[4 * i + 0] = SCALE_3_8(intensity);
            mTexUploadBuffer[4 * i + 1] = SCALE_3_8(intensity);
            mTexUploadBuffer[4 * i + 2] = SCALE_3_8(intensity);
            mTexUploadBuffer[4 * i + 3] = alpha ? 255 : 0;
            i++;
        }
    }

    UploadImported(mTexUploadBuffer, width, height);
}

void Interpreter::ImportTextureIA8(int tile, bool importReplacement) {
    const RawTexMetadata* metadata = &mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].raw_tex_metadata;
    const uint8_t* addr =
        importReplacement && (metadata->resource != nullptr)
            ? mMaskedTextures.find(GetBaseTexturePath(metadata->resource->GetInitData()->Path))->second.replacementData
            : mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].addr;

    if (addr == nullptr) {
        SPDLOG_ERROR("ImportTextureIA8: null texture address for tile {}", tile);
        return;
    }

    uint32_t sizeBytes = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].size_bytes;
    uint32_t fullImageLineSizeBytes =
        mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].full_image_line_size_bytes;
    uint32_t lineSizeBytes = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].line_size_bytes;

    uint32_t width = GetEffectiveLineSize(lineSizeBytes, fullImageLineSizeBytes, sizeBytes,
                                          mRdp->texture_tile[tile].line_size_bytes);
    uint32_t height = width > 0 ? sizeBytes / width : 0;

    if (fullImageLineSizeBytes == sizeBytes) {
        fullImageLineSizeBytes = width;
    }

    uint32_t i = 0;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t srcIdx = y * fullImageLineSizeBytes + x;
            uint8_t intensity = addr[srcIdx] >> 4;
            uint8_t alpha = addr[srcIdx] & 0xf;
            mTexUploadBuffer[4 * i + 0] = SCALE_4_8(intensity);
            mTexUploadBuffer[4 * i + 1] = SCALE_4_8(intensity);
            mTexUploadBuffer[4 * i + 2] = SCALE_4_8(intensity);
            mTexUploadBuffer[4 * i + 3] = SCALE_4_8(alpha);
            i++;
        }
    }

    UploadImported(mTexUploadBuffer, width, height);
}

void Interpreter::ImportTextureIA16(int tile, bool importReplacement) {
    const RawTexMetadata* metadata = &mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].raw_tex_metadata;
    const uint8_t* addr =
        importReplacement && (metadata->resource != nullptr)
            ? mMaskedTextures.find(GetBaseTexturePath(metadata->resource->GetInitData()->Path))->second.replacementData
            : mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].addr;

    if (addr == nullptr) {
        SPDLOG_ERROR("ImportTextureIA16: null texture address for tile {}", tile);
        return;
    }

    uint32_t size_bytes = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].size_bytes;
    uint32_t full_image_line_size_bytes =
        mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].full_image_line_size_bytes;
    uint32_t line_size_bytes = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].line_size_bytes;

    uint32_t widthBytes = GetEffectiveLineSize(line_size_bytes, full_image_line_size_bytes, size_bytes,
                                               mRdp->texture_tile[tile].line_size_bytes);
    uint32_t width = widthBytes / 2;
    uint32_t height = widthBytes > 0 ? size_bytes / widthBytes : 0;

    // A single line of pixels should not equal the entire image (height == 1 non-withstanding)
    if (full_image_line_size_bytes == size_bytes) {
        full_image_line_size_bytes = width * 2;
    }

    uint32_t i = 0;

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t clrIdx = (y * (full_image_line_size_bytes / 2)) + (x);

            uint8_t intensity = addr[2 * clrIdx];
            uint8_t alpha = addr[2 * clrIdx + 1];
            uint8_t r = intensity;
            uint8_t g = intensity;
            uint8_t b = intensity;
            mTexUploadBuffer[4 * i + 0] = r;
            mTexUploadBuffer[4 * i + 1] = g;
            mTexUploadBuffer[4 * i + 2] = b;
            mTexUploadBuffer[4 * i + 3] = alpha;

            i++;
        }
    }

    UploadImported(mTexUploadBuffer, width, height);
}

void Interpreter::ImportTextureI4(int tile, bool importReplacement) {
    const RawTexMetadata* metadata = &mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].raw_tex_metadata;
    const uint8_t* addr =
        importReplacement && (metadata->resource != nullptr)
            ? mMaskedTextures.find(GetBaseTexturePath(metadata->resource->GetInitData()->Path))->second.replacementData
            : mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].addr;

    if (addr == nullptr) {
        SPDLOG_ERROR("ImportTextureI4: null texture address for tile {}", tile);
        return;
    }

    uint32_t sizeBytes = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].size_bytes;
    uint32_t fullImageLineSizeBytes =
        mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].full_image_line_size_bytes;
    uint32_t lineSizeBytes = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].line_size_bytes;

    uint32_t widthBytes = GetEffectiveLineSize(lineSizeBytes, fullImageLineSizeBytes, sizeBytes,
                                               mRdp->texture_tile[tile].line_size_bytes);
    uint32_t width = widthBytes * 2;
    uint32_t height = widthBytes > 0 ? sizeBytes / widthBytes : 0;

    // A single line of pixels should not equal the entire image (height == 1 non-withstanding)
    if (fullImageLineSizeBytes == sizeBytes) {
        fullImageLineSizeBytes = width / 2;
    }

    uint32_t i = 0;

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t clrIdx = (y * (fullImageLineSizeBytes * 2)) + (x);

            uint8_t byte = addr[clrIdx / 2];
            uint8_t part = (byte >> (4 - (clrIdx % 2) * 4)) & 0xf;
            uint8_t intensity = part;
            uint8_t r = intensity;
            uint8_t g = intensity;
            uint8_t b = intensity;
            uint8_t a = intensity;
            mTexUploadBuffer[4 * i + 0] = SCALE_4_8(r);
            mTexUploadBuffer[4 * i + 1] = SCALE_4_8(g);
            mTexUploadBuffer[4 * i + 2] = SCALE_4_8(b);
            mTexUploadBuffer[4 * i + 3] = SCALE_4_8(a);

            i++;
        }
    }

    UploadImported(mTexUploadBuffer, width, height);
}

void Interpreter::ImportTextureI8(int tile, bool importReplacement) {
    const RawTexMetadata* metadata = &mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].raw_tex_metadata;
    const uint8_t* addr =
        importReplacement && (metadata->resource != nullptr)
            ? mMaskedTextures.find(GetBaseTexturePath(metadata->resource->GetInitData()->Path))->second.replacementData
            : mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].addr;

    if (addr == nullptr) {
        SPDLOG_ERROR("ImportTextureI8: null texture address for tile {}", tile);
        return;
    }

    uint32_t sizeBytes = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].size_bytes;
    uint32_t fullImageLineSizeBytes =
        mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].full_image_line_size_bytes;
    uint32_t lineSizeBytes = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].line_size_bytes;

    uint32_t width = GetEffectiveLineSize(lineSizeBytes, fullImageLineSizeBytes, sizeBytes,
                                          mRdp->texture_tile[tile].line_size_bytes);
    uint32_t height = width > 0 ? sizeBytes / width : 0;

    if (fullImageLineSizeBytes == sizeBytes) {
        fullImageLineSizeBytes = width;
    }

    uint32_t i = 0;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint8_t intensity = addr[y * fullImageLineSizeBytes + x];
            mTexUploadBuffer[4 * i + 0] = intensity;
            mTexUploadBuffer[4 * i + 1] = intensity;
            mTexUploadBuffer[4 * i + 2] = intensity;
            mTexUploadBuffer[4 * i + 3] = intensity;
            i++;
        }
    }

    UploadImported(mTexUploadBuffer, width, height);
}

void Interpreter::ImportTextureCi4(int tile, bool importReplacement) {
    uint32_t fullImageLineSizeBytes =
        mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].full_image_line_size_bytes;
    const RawTexMetadata* metadata = &mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].raw_tex_metadata;
    const uint8_t* addr =
        importReplacement && (metadata->resource != nullptr)
            ? mMaskedTextures.find(GetBaseTexturePath(metadata->resource->GetInitData()->Path))->second.replacementData
            : mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].addr;

    if (addr == nullptr) {
        SPDLOG_ERROR("ImportTextureCi4: null texture address for tile {}", tile);
        return;
    }

    uint32_t sizeBytes = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].size_bytes;
    uint32_t lineSizeBytes = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].line_size_bytes;
    uint32_t palIdx = mRdp->texture_tile[tile].palette; // 0-15

    const uint8_t* palette;

    if (mRdp->palettes[palIdx / 8] == nullptr) {
        SPDLOG_WARN("CI4: null palette slot {} for palIdx={}", palIdx / 8, palIdx);
        return;
    }
    palette = mRdp->palettes[palIdx / 8] + (palIdx % 8) * 16 * 2;

    uint32_t baseLineSizeBytes = GetEffectiveLineSize(lineSizeBytes, fullImageLineSizeBytes, sizeBytes,
                                                      mRdp->texture_tile[tile].line_size_bytes);
    uint32_t resultLineSizeBytes = baseLineSizeBytes;

    if (metadata->h_byte_scale != 1) {
        resultLineSizeBytes *= metadata->h_byte_scale;
    }

    // CI4: 2 pixels per byte
    uint32_t width = resultLineSizeBytes * 2;
    uint32_t height = resultLineSizeBytes > 0 ? sizeBytes / resultLineSizeBytes : 0;

    // Clamp to the rendered region only when the loaded buffer is ~1.33x of it (mipmap
    // pyramid signature). Window-scrolling tiles have loaded ≈ rendered or loaded >> rendered;
    // skip both. CLAMP wrap mode always opts in.
    uint32_t tile_w = GetTileSizeFromCoordinates(mRdp->texture_tile[tile].uls, mRdp->texture_tile[tile].lrs);
    uint32_t tile_h = GetTileSizeFromCoordinates(mRdp->texture_tile[tile].ult, mRdp->texture_tile[tile].lrt);
    bool pyramidLike = IsPyramidLike(width, height, tile_w, tile_h, metadata->h_byte_scale, metadata->v_pixel_scale);
    bool clampS = (mRdp->texture_tile[tile].cms & G_TX_CLAMP) != 0;
    bool clampT = (mRdp->texture_tile[tile].cmt & G_TX_CLAMP) != 0;
    // A masked axis wraps every 2^mask texels, so trim an over-loaded texture back to that.
    // Skip a mask smaller than the tile region though - that's stale tile state, not a real load.
    uint32_t maskW = mRdp->texture_tile[tile].masks;
    uint32_t maskH = mRdp->texture_tile[tile].maskt;
    if (maskW != 0 && (1u << maskW) >= tile_w && (1u << maskW) < width) {
        width = 1u << maskW;
    }
    if (maskH != 0 && (1u << maskH) >= tile_h && (1u << maskH) < height) {
        height = 1u << maskH;
    }
    if ((pyramidLike || clampS) && tile_w > 0 && tile_w < width) {
        width = tile_w;
    }
    if ((pyramidLike || clampT) && tile_h > 0 && tile_h < height) {
        height = tile_h;
    }

    if (fullImageLineSizeBytes == sizeBytes) {
        fullImageLineSizeBytes = resultLineSizeBytes;
    }

    uint32_t i = 0;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t srcPixelIdx = y * (fullImageLineSizeBytes * 2) + x;
            uint8_t byte = addr[srcPixelIdx / 2];
            uint8_t idx = (byte >> (4 - (srcPixelIdx % 2) * 4)) & 0xf;
            uint16_t col16 = (palette[idx * 2] << 8) | palette[idx * 2 + 1]; // Big endian load
            uint8_t a = col16 & 1;
            uint8_t r = col16 >> 11;
            uint8_t g = (col16 >> 6) & 0x1f;
            uint8_t b = (col16 >> 1) & 0x1f;
            mTexUploadBuffer[4 * i + 0] = SCALE_5_8(r);
            mTexUploadBuffer[4 * i + 1] = SCALE_5_8(g);
            mTexUploadBuffer[4 * i + 2] = SCALE_5_8(b);
            mTexUploadBuffer[4 * i + 3] = a ? 255 : 0;
            i++;
        }
    }

    UploadImported(mTexUploadBuffer, width, height);
}

void Interpreter::ImportTextureCi8(int tile, bool importReplacement) {
    const RawTexMetadata* metadata = &mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].raw_tex_metadata;
    const uint8_t* addr =
        importReplacement && (metadata->resource != nullptr)
            ? mMaskedTextures.find(GetBaseTexturePath(metadata->resource->GetInitData()->Path))->second.replacementData
            : mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].addr;

    if (addr == nullptr) {
        SPDLOG_ERROR("ImportTextureCi8: null texture address for tile {}", tile);
        return;
    }

    uint32_t sizeBytes = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].size_bytes;
    uint32_t fullImageLineSizeBytes =
        mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].full_image_line_size_bytes;
    uint32_t lineSizeBytes = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].line_size_bytes;

    if (mRdp->palettes[0] == nullptr || mRdp->palettes[1] == nullptr) {
        SPDLOG_WARN("CI8: null palette (pal0={}, pal1={})", static_cast<const void*>(mRdp->palettes[0]),
                    static_cast<const void*>(mRdp->palettes[1]));
        return;
    }

    for (uint32_t i = 0, j = 0; i < sizeBytes; j += fullImageLineSizeBytes - lineSizeBytes) {
        for (uint32_t k = 0; k < lineSizeBytes; i++, k++, j++) {
            uint8_t idx = addr[j];
            uint16_t col16 = (mRdp->palettes[idx / 128][(idx % 128) * 2] << 8) |
                             mRdp->palettes[idx / 128][(idx % 128) * 2 + 1]; // Big endian load
            uint8_t a = col16 & 1;
            uint8_t r = col16 >> 11;
            uint8_t g = (col16 >> 6) & 0x1f;
            uint8_t b = (col16 >> 1) & 0x1f;
            mTexUploadBuffer[4 * i + 0] = SCALE_5_8(r);
            mTexUploadBuffer[4 * i + 1] = SCALE_5_8(g);
            mTexUploadBuffer[4 * i + 2] = SCALE_5_8(b);
            mTexUploadBuffer[4 * i + 3] = a ? 255 : 0;
        }
    }

    uint32_t baseLineSizeBytes = GetEffectiveLineSize(lineSizeBytes, fullImageLineSizeBytes, sizeBytes,
                                                      mRdp->texture_tile[tile].line_size_bytes);
    uint32_t resultLineSizeBytes = baseLineSizeBytes;
    if (metadata->h_byte_scale != 1) {
        resultLineSizeBytes *= metadata->h_byte_scale;
    }

    uint32_t width = resultLineSizeBytes;
    uint32_t height = resultLineSizeBytes > 0 ? sizeBytes / resultLineSizeBytes : 0;

    // Clamp to the rendered region only when the loaded buffer is ~1.33x of it (mipmap
    // pyramid signature). Window-scrolling tiles have loaded ≈ rendered or loaded >> rendered;
    // skip both. CLAMP wrap mode always opts in.
    uint32_t tile_w = GetTileSizeFromCoordinates(mRdp->texture_tile[tile].uls, mRdp->texture_tile[tile].lrs);
    uint32_t tile_h = GetTileSizeFromCoordinates(mRdp->texture_tile[tile].ult, mRdp->texture_tile[tile].lrt);
    bool pyramidLike = IsPyramidLike(width, height, tile_w, tile_h, metadata->h_byte_scale, metadata->v_pixel_scale);
    bool clampS = (mRdp->texture_tile[tile].cms & G_TX_CLAMP) != 0;
    bool clampT = (mRdp->texture_tile[tile].cmt & G_TX_CLAMP) != 0;
    // A masked axis wraps every 2^mask texels, so trim an over-loaded texture back to that.
    // Skip a mask smaller than the tile region though - that's stale tile state, not a real load.
    uint32_t maskW = mRdp->texture_tile[tile].masks;
    uint32_t maskH = mRdp->texture_tile[tile].maskt;
    if (maskW != 0 && (1u << maskW) >= tile_w && (1u << maskW) < width) {
        width = 1u << maskW;
    }
    if (maskH != 0 && (1u << maskH) >= tile_h && (1u << maskH) < height) {
        height = 1u << maskH;
    }
    if ((pyramidLike || clampS) && tile_w > 0 && tile_w < width) {
        width = tile_w;
    }
    if ((pyramidLike || clampT) && tile_h > 0 && tile_h < height) {
        height = tile_h;
    }

    UploadImported(mTexUploadBuffer, width, height);
}

void Interpreter::ImportTextureImg(int tile, bool importReplacement) {
    const RawTexMetadata* metadata = &mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].raw_tex_metadata;
    const uint8_t* addr =
        importReplacement && (metadata->resource != nullptr)
            ? mMaskedTextures.find(GetBaseTexturePath(metadata->resource->GetInitData()->Path))->second.replacementData
            : mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].addr;

    if (addr == nullptr) {
        SPDLOG_ERROR("ImportTextureImg: null texture address for tile {}", tile);
        return;
    }

    uint16_t width = metadata->width;
    uint16_t height = metadata->height;
    UploadImported(addr, width, height);
}

void Interpreter::ImportTextureRaw(int tile, bool importReplacement) {
    const RawTexMetadata* metadata = &mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].raw_tex_metadata;
    const uint8_t* addr =
        importReplacement && (metadata->resource != nullptr)
            ? mMaskedTextures.find(GetBaseTexturePath(metadata->resource->GetInitData()->Path))->second.replacementData
            : mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].addr;

    if (addr == nullptr) {
        SPDLOG_ERROR("ImportTextureRaw: null texture address for tile {}", tile);
        return;
    }

    uint16_t width = metadata->width;
    uint16_t height = metadata->height;
    Fast::TextureType type = metadata->type;
    std::shared_ptr<Fast::Texture> resource = metadata->resource;

    // if texture type is CI4 or CI8 we need to apply tlut to it
    switch (type) {
        case Fast::TextureType::Palette4bpp:
            ImportTextureCi4(tile, importReplacement);
            return;
        case Fast::TextureType::Palette8bpp:
            ImportTextureCi8(tile, importReplacement);
            return;
        default:
            break;
    }

    uint32_t numLoadedBytes = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].size_bytes;
    uint32_t numOriginallyLoadedBytes = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].orig_size_bytes;

    uint32_t resultOrigLineSize = mRdp->texture_tile[tile].line_size_bytes;
    switch (mRdp->texture_tile[tile].siz) {
        case G_IM_SIZ_32b:
            resultOrigLineSize *= 2;
            break;
    }
    uint32_t resultOrigHeight = numOriginallyLoadedBytes / resultOrigLineSize;
    uint32_t resultNewLineSize = resultOrigLineSize * metadata->h_byte_scale;
    uint32_t resultNewHeight = resultOrigHeight * metadata->v_pixel_scale;

    if (resultNewLineSize == 4 * width && resultNewHeight == height) {
        // Can use the texture directly since it has the correct dimensions
        UploadImported(addr, width, height);
        return;
    }

    uint32_t fullImageLineSizeBytes =
        mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].full_image_line_size_bytes;
    uint32_t line_size_bytes = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].line_size_bytes;

    // Get the resource's true image size
    uint32_t resourceImageSizeBytes = resource->ImageDataSize;
    uint32_t safeFullImageLineSizeBytes = fullImageLineSizeBytes;
    uint32_t safeLineSizeBytes = line_size_bytes;
    uint32_t safeLoadedBytes = numLoadedBytes;

    // Sometimes the texture load commands will specify a size larger than the authentic texture
    // Normally the OOB info is read as garbage, but will cause a crash on some platforms
    // Restrict the bytes to a safe amount
    if (numLoadedBytes > resourceImageSizeBytes) {
        safeLoadedBytes = resourceImageSizeBytes;
        safeLineSizeBytes = resourceImageSizeBytes;
        safeFullImageLineSizeBytes = resourceImageSizeBytes;
    }

    // Safely only copy the amount of bytes the resource can allow
    for (uint32_t i = 0, j = 0; i < safeLoadedBytes; i += safeLineSizeBytes, j += safeFullImageLineSizeBytes) {
        memcpy(mTexUploadBuffer + i, addr + j, safeLineSizeBytes);
    }

    // Set the remaining bytes to load as 0
    if (numLoadedBytes > resourceImageSizeBytes) {
        memset(mTexUploadBuffer + resourceImageSizeBytes, 0, numLoadedBytes - resourceImageSizeBytes);
    }

    // Describe the buffer by what was actually packed (the loaded HD stride)
    uint32_t uploadWidth = safeLineSizeBytes / 4;
    uint32_t uploadHeight = safeLineSizeBytes > 0 ? safeLoadedBytes / safeLineSizeBytes : 0;
    const bool singleLineLoad = uploadHeight <= 1 && resultNewLineSize != 0 && resultNewLineSize < safeLineSizeBytes;
    if (uploadWidth > (uint32_t)mRapi->GetMaxTextureSize() || singleLineLoad) {
        if (safeLoadedBytes == (uint64_t)width * height * 4) {
            // buffer holds the full image, use its real dimensions
            uploadWidth = width;
            uploadHeight = height;
        } else {
            // partial load, fall back to the tile-derived dimensions
            uploadWidth = resultNewLineSize / 4;
            uploadHeight = resultNewHeight;
        }
    }
    UploadImported(mTexUploadBuffer, uploadWidth, uploadHeight);
}

void Interpreter::ImportTexture(int i, int tile, bool importReplacement) {
    uint8_t fmt = mRdp->texture_tile[tile].fmt;
    uint8_t siz = mRdp->texture_tile[tile].siz;
    uint32_t texFlags = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].tex_flags;
    uint32_t tmemIdex = mRdp->texture_tile[tile].tmem_index;
    uint8_t paletteIndex = mRdp->texture_tile[tile].palette;
    uint32_t origSizeBytes = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].orig_size_bytes;

    const RawTexMetadata* metadata = &mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].raw_tex_metadata;
    const uint8_t* origAddr =
        importReplacement && (metadata->resource != nullptr)
            ? mMaskedTextures.find(GetBaseTexturePath(metadata->resource->GetInitData()->Path))->second.replacementData
            : mRdp->loaded_texture[tmemIdex].addr;

    // Check if this texture address is a registered GPU framebuffer mirror.
    // If so, bind the GPU FB directly — full resolution, no CPU readback needed.
    if (origAddr != nullptr && !importReplacement) {
        auto fbIt = mFbTextures.find((uintptr_t)origAddr);
        if (fbIt != mFbTextures.end()) {
            Flush();
            mRapi->SelectTextureFb(fbIt->second);
            mRdp->textures_changed[i] = false;
            return;
        }
    }

    if (origAddr == nullptr) {
        // Try the other TMEM slot -- some multi-tile setups only load one slot
        // and expect both tiles to reference it.
        uint32_t otherTmem = tmemIdex ^ 1;
        origAddr = mRdp->loaded_texture[otherTmem].addr;
        if (origAddr == nullptr) {
            SPDLOG_WARN("ImportTexture: null texture address for tile {} (both TMEM slots empty)", tile);
            return;
        }
        SPDLOG_WARN("ImportTexture: tile {} TMEM slot {} empty, falling back to slot {}", tile, tmemIdex, otherTmem);
        tmemIdex = otherTmem;
        origSizeBytes = mRdp->loaded_texture[otherTmem].orig_size_bytes;
        texFlags = mRdp->loaded_texture[otherTmem].tex_flags;
    }

    // Use palette_dram_addr (the original DRAM source) instead of palettes[]
    // (which always points to the staging buffer) so the same texture drawn
    // with different palettes gets distinct cache entries.
    TextureCacheKey key;
    if (fmt == G_IM_FMT_CI) {
        if (siz == G_IM_SIZ_4b) {
            uint8_t palSlot = paletteIndex / 8;
            key = { origAddr,
                    { palSlot == 0 ? mRdp->palette_dram_addr[0] : nullptr,
                      palSlot == 1 ? mRdp->palette_dram_addr[1] : nullptr },
                    fmt,
                    siz,
                    paletteIndex,
                    origSizeBytes };
        } else {
            // CI8 uses both palette halves
            key = { origAddr,     { mRdp->palette_dram_addr[0], mRdp->palette_dram_addr[1] }, fmt, siz, paletteIndex,
                    origSizeBytes };
        }
    } else {
        key = { origAddr, {}, fmt, siz, paletteIndex, origSizeBytes };
    }

#ifdef LUS_XBOX
    // A blended texture (Gfx_RegisterBlendedTexture: Gohma's body, King Dodongo's and his lava floor, Phantom
    // Ganon's, Ganon's cape and windows, the pause map's): the mask, and its replacement, applied here as it's
    // imported, into the one texture the D3D8 backend draws with. (Its slots are two: the mask's and the
    // replacement's, slots 2 to 5, landed on slot 0 and drew the mask for the texture: all black.) The key has
    // them hashed: changed (Gohma's decay, the lava's waves), it's a texture of its own.
    const MaskedTextureEntry* maskEntry = nullptr;
    uint32_t maskWidth = 0, maskHeight = 0;
    if (!importReplacement && mRdp->loaded_texture[tmemIdex].masked && metadata->resource != nullptr) {
        auto maskIt = mMaskedTextures.find(GetBaseTexturePath(metadata->resource->GetInitData()->Path));
        if (maskIt != mMaskedTextures.end() && maskIt->second.mask != nullptr &&
            XboxMaskSize(tile, &maskWidth, &maskHeight)) {
            maskEntry = &maskIt->second;
            uint32_t hash = 2166136261u; // (FNV-1a)
            for (uint32_t b = 0; b < maskWidth * maskHeight; b++) {
                hash = (hash ^ maskEntry->mask[b]) * 16777619u;
            }
            if (maskEntry->replacementData != nullptr) {
                for (uint32_t b = 0; b < origSizeBytes; b++) {
                    hash = (hash ^ maskEntry->replacementData[b]) * 16777619u;
                }
            }
            key.mask_hash = hash | 1; // (never 0, an unmasked texture's)
        }
    }
#endif

    if (TextureCacheLookup(i, key)) {
        return;
    }

    // Guard against zero-sized textures that would cause divide-by-zero
    // or GPU API errors in UploadTexture.
    if (mRdp->texture_tile[tile].line_size_bytes == 0 || mRdp->loaded_texture[tmemIdex].size_bytes == 0 ||
        origAddr == nullptr) {
        return;
    }

#ifdef LUS_XBOX
    if (maskEntry != nullptr) {
        mXboxBlendWidth = mXboxBlendHeight = 0;
        if (maskEntry->replacementData != nullptr) {
            mXboxUpload = XBOX_UPLOAD_CAPTURE; // (into mXboxBlend, not uploaded)
            DecodeTexture(tile, true, fmt, siz, texFlags);
        }
        mXboxUpload = XBOX_UPLOAD_MASK;
        mXboxMask = maskEntry->mask;
        mXboxMaskWidth = maskWidth;
        mXboxMaskHeight = maskHeight;
        DecodeTexture(tile, false, fmt, siz, texFlags);
        mXboxUpload = XBOX_UPLOAD_PLAIN;
        mXboxMask = nullptr;
        return;
    }
#endif
    DecodeTexture(tile, importReplacement, fmt, siz, texFlags);
}

void Interpreter::DecodeTexture(int tile, bool importReplacement, uint8_t fmt, uint8_t siz, uint32_t texFlags) {
    if ((texFlags & TEX_FLAG_LOAD_AS_IMG) != 0) {
        ImportTextureImg(tile, importReplacement);
        return;
    }

    // if load as raw is set then we load_raw();
    if ((texFlags & TEX_FLAG_LOAD_AS_RAW) != 0) {
        ImportTextureRaw(tile, importReplacement);
        return;
    }

    switch (fmt) {
        case G_IM_FMT_RGBA:
            if (siz == G_IM_SIZ_16b) {
                ImportTextureRgba16(tile, importReplacement);
            } else if (siz == G_IM_SIZ_32b) {
                ImportTextureRgba32(tile, importReplacement);
            } else {
                SPDLOG_ERROR("RGBA Texture that isn't 16 or 32 bit. Size = {}", siz);
                // OTRTODO: Sometimes, seemingly randomly, we end up here. Could be a bad dlist, could be
                // something F3D does not have supported. Further investigation is needed.
            }
            break;
        case G_IM_FMT_IA:
            if (siz == G_IM_SIZ_4b) {
                ImportTextureIA4(tile, importReplacement);
            } else if (siz == G_IM_SIZ_8b) {
                ImportTextureIA8(tile, importReplacement);
            } else if (siz == G_IM_SIZ_16b) {
                ImportTextureIA16(tile, importReplacement);
            } else {
                SPDLOG_ERROR("IA Texture that isn't 4, 8, or 16 bit. Size = {}", siz);
                ;
            }
            break;
        case G_IM_FMT_CI:
            if (siz == G_IM_SIZ_4b) {
                ImportTextureCi4(tile, importReplacement);
            } else if (siz == G_IM_SIZ_8b) {
                ImportTextureCi8(tile, importReplacement);
            } else if (siz == G_IM_SIZ_16b) {
                // CI+16b is hardware-invalid on N64. The tile's fmt is likely
                // stale from a prior draw. Decode as RGBA16 instead.
                ImportTextureRgba16(tile, importReplacement);
            } else if (siz == G_IM_SIZ_32b) {
                ImportTextureRgba32(tile, importReplacement);
            } else {
                SPDLOG_ERROR("CI Texture with unexpected size = {}", siz);
            }
            break;
        case G_IM_FMT_I:
            if (siz == G_IM_SIZ_4b) {
                ImportTextureI4(tile, importReplacement);
            } else if (siz == G_IM_SIZ_8b) {
                ImportTextureI8(tile, importReplacement);
            } else {
                SPDLOG_ERROR("I Texture that isn't 4 or 8 bit. Size = {}", siz);
            }
            break;
        case G_IM_FMT_YUV:
            SPDLOG_ERROR("YUV Textures not supported");
            break;
        default:
            SPDLOG_ERROR("Invalid texture format. Fmt = {}", fmt);
            break;
    }
}

// every decoded texture's upload (the ImportTexture* functions)
void Interpreter::UploadImported(const uint8_t* rgba, uint32_t width, uint32_t height) {
#ifdef LUS_XBOX
    if (mXboxUpload == XBOX_UPLOAD_CAPTURE) {
        mXboxBlend.assign(rgba, rgba + (size_t)width * height * 4);
        mXboxBlendWidth = width;
        mXboxBlendHeight = height;
        return;
    }
    if (mXboxUpload == XBOX_UPLOAD_MASK && mXboxMask != nullptr && mXboxMaskWidth > 0 && mXboxMaskHeight > 0) {
        if (rgba != mTexUploadBuffer) { // (an image's, uploaded as it is)
            memcpy(mTexUploadBuffer, rgba, (size_t)width * height * 4);
        }
        const bool blend = mXboxBlendWidth == width && mXboxBlendHeight == height;
        // the shaders' lerp(texel, replacement or 0, mask): the mask's bytes are 0 or not, the mask scaled to the
        // texture (sampled with the same coordinates)
        for (uint32_t y = 0; y < height; y++) {
            const uint8_t* maskRow = mXboxMask + (size_t)(y * mXboxMaskHeight / height) * mXboxMaskWidth;
            for (uint32_t x = 0; x < width; x++) {
                if (maskRow[x * mXboxMaskWidth / width] == 0) {
                    continue;
                }
                const size_t p = ((size_t)y * width + x) * 4;
                if (blend) {
                    memcpy(mTexUploadBuffer + p, mXboxBlend.data() + p, 4);
                } else {
                    memset(mTexUploadBuffer + p, 0, 4);
                }
            }
        }
        mRapi->UploadTexture(mTexUploadBuffer, width, height);
        return;
    }
#endif
    mRapi->UploadTexture(rgba, width, height);
}

#ifdef LUS_XBOX
// a blended texture's mask's size, a byte a pixel: as ImportTextureMask works it out
bool Interpreter::XboxMaskSize(int tile, uint32_t* width, uint32_t* height) {
    const uint32_t lineBytes = mRdp->texture_tile[tile].line_size_bytes;
    if (lineBytes == 0) {
        return false;
    }
    uint32_t w = lineBytes;
    const uint32_t h = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].orig_size_bytes / lineBytes;
    switch (mRdp->texture_tile[tile].siz) {
        case G_IM_SIZ_4b:
            w *= 2;
            break;
        case G_IM_SIZ_16b:
            w /= 2;
            break;
        case G_IM_SIZ_32b:
            w /= 4;
            break;
        default:
            break;
    }
    *width = w;
    *height = h;
    return w > 0 && h > 0;
}
#endif

void Interpreter::ImportTextureMask(int i, int tile) {
    uint32_t tmemIndex = mRdp->texture_tile[tile].tmem_index;
    RawTexMetadata metadata = mRdp->loaded_texture[tmemIndex].raw_tex_metadata;

    if (metadata.resource == nullptr) {
        return;
    }

    auto maskIter = mMaskedTextures.find(GetBaseTexturePath(metadata.resource->GetInitData()->Path));
    if (maskIter == mMaskedTextures.end()) {
        return;
    }

    const uint8_t* orig_addr = maskIter->second.mask;

    if (orig_addr == nullptr) {
        return;
    }

    TextureCacheKey key = { orig_addr, {}, 0, 0, 0, 0 };

    if (TextureCacheLookup(i, key)) {
        return;
    }

    uint32_t width = mRdp->texture_tile[tile].line_size_bytes;
    uint32_t height = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].orig_size_bytes /
                      mRdp->texture_tile[tile].line_size_bytes;
    switch (mRdp->texture_tile[tile].siz) {
        case G_IM_SIZ_4b:
            width *= 2;
            break;
        case G_IM_SIZ_8b:
        default:
            break;
        case G_IM_SIZ_16b:
            width /= 2;
            break;
        case G_IM_SIZ_32b:
            width /= 4;
            break;
    }

    for (uint32_t texIndex = 0; texIndex < width * height; texIndex++) {
        uint8_t masked = orig_addr[texIndex];
        if (masked) {
            mTexUploadBuffer[4 * texIndex + 0] = 0;
            mTexUploadBuffer[4 * texIndex + 1] = 0;
            mTexUploadBuffer[4 * texIndex + 2] = 0;
            mTexUploadBuffer[4 * texIndex + 3] = 0xFF;
        } else {
            mTexUploadBuffer[4 * texIndex + 0] = 0;
            mTexUploadBuffer[4 * texIndex + 1] = 0;
            mTexUploadBuffer[4 * texIndex + 2] = 0;
            mTexUploadBuffer[4 * texIndex + 3] = 0;
        }
    }

    UploadImported(mTexUploadBuffer, width, height);
}

void Interpreter::NormalizeVector(float v[3]) {
    float s = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= s;
    v[1] /= s;
    v[2] /= s;
}

void Interpreter::TransposedMatrixMul(float res[3], const float a[3], const float b[4][4]) {
    res[0] = a[0] * b[0][0] + a[1] * b[0][1] + a[2] * b[0][2];
    res[1] = a[0] * b[1][0] + a[1] * b[1][1] + a[2] * b[1][2];
    res[2] = a[0] * b[2][0] + a[1] * b[2][1] + a[2] * b[2][2];
}

void Interpreter::MatrixMul(float res[4][4], const float a[4][4], const float b[4][4]) {
#ifdef LUS_XBOX
    // a row of the result at a time, as SSE vectors (the same sums in the same order)
    const __m128 b0 = _mm_loadu_ps(b[0]), b1 = _mm_loadu_ps(b[1]), b2 = _mm_loadu_ps(b[2]), b3 = _mm_loadu_ps(b[3]);
    __m128 row[4];
    for (int i = 0; i < 4; i++) {
        row[i] = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(a[i][0]), b0),
                                                  _mm_mul_ps(_mm_set1_ps(a[i][1]), b1)),
                                       _mm_mul_ps(_mm_set1_ps(a[i][2]), b2)),
                            _mm_mul_ps(_mm_set1_ps(a[i][3]), b3));
    }
    for (int i = 0; i < 4; i++) {
        _mm_storeu_ps(res[i], row[i]); // (after all of them: res may be a or b)
    }
    return;
#endif
    float tmp[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            tmp[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        }
    }
    memcpy(res, tmp, sizeof(tmp));
}

void Interpreter::CalculateNormalDir(const F3DLight_t* light, float coeffs[3]) {
    float light_dir[3] = { light->dir[0] / 127.0f, light->dir[1] / 127.0f, light->dir[2] / 127.0f };

    Interpreter::TransposedMatrixMul(coeffs, light_dir,
                                     mRsp->modelview_matrix_stack[mRsp->modelview_matrix_stack_size - 1]);
    Interpreter::NormalizeVector(coeffs);
}

// a G_MTX command's matrix: the port's replacement for it (a frame between two ticks), else the game's
void Interpreter::LoadMtx(float matrix[4][4], const int32_t* addr,
                          const std::unordered_map<Mtx*, MtxF>& mtx_replacements) {
    if (auto it = mtx_replacements.find((Mtx*)addr); it != mtx_replacements.end()) {
#ifdef LUS_XBOX
        memcpy(matrix, it->second.mf, sizeof(it->second.mf)); // (the GPU's floats: no need of the N64's 16.16 steps)
#else
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                float v = it->second.mf[i][j];
                int as_int = (int)(v * 65536.0f);
                matrix[i][j] = as_int * (1.0f / 65536.0f);
            }
        }
#endif
    } else {
#ifndef GBI_FLOATS
        // Original GBI where fixed point matrices are used
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j += 2) {
                int32_t int_part = addr[i * 2 + j / 2];
                uint32_t frac_part = addr[8 + i * 2 + j / 2];
                matrix[i][j] = (int32_t)((int_part & 0xffff0000) | (frac_part >> 16)) / 65536.0f;
                matrix[i][j + 1] = (int32_t)((int_part << 16) | (frac_part & 0xffff)) / 65536.0f;
            }
        }
#else
        // For a modified GBI where fixed point values are replaced with floats
        memcpy(matrix, addr, sizeof(matrix));
#endif
    }
}

#ifdef LUS_XBOX
// a replay's: the frame's blend of the matrix, else the game's own
void Interpreter::LoadMtx(float matrix[4][4], const int32_t* addr, const MtxBlends* blends) {
    static const std::unordered_map<Mtx*, MtxF> sNone;
    const MtxF* blend = blends != nullptr ? blends->Find(addr) : nullptr;
    if (blend != nullptr) {
        memcpy(matrix, blend->mf, sizeof(blend->mf));
    } else {
        LoadMtx(matrix, addr, sNone);
    }
}
#endif

void Interpreter::GfxSpMatrix(uint8_t parameters, const int32_t* addr) {
    float matrix[4][4];

    LoadMtx(matrix, addr, *mCurMtxReplacements);

    const int8_t mtx_projection = get_attr(MTX_PROJECTION);
    const int8_t mtx_load = get_attr(MTX_LOAD);
    const int8_t mtx_push = get_attr(MTX_PUSH);

#ifdef LUS_XBOX
    mHwVtxVersion++;
#endif
    if (parameters & mtx_projection) {
        if (parameters & mtx_load) {
            memcpy(mRsp->P_matrix, matrix, sizeof(matrix));
        } else {
            MatrixMul(mRsp->P_matrix, matrix, mRsp->P_matrix);
        }
#ifdef LUS_XBOX
        // the recipe (HwRecipe): a load starts it; a multiply with room extends it, without, ends it
        if (parameters & mtx_load) {
            mHwRecipeP[0] = addr;
            mHwRecipeNp = 1;
        } else if (mHwRecipeNp == 1) {
            mHwRecipeP[1] = addr;
            mHwRecipeNp = 2;
        } else {
            mHwRecipeNp = 0;
        }
#endif
    } else { // G_MTX_MODELVIEW
        if ((parameters & mtx_push) && mRsp->modelview_matrix_stack_size < 11) {
            ++mRsp->modelview_matrix_stack_size;
            memcpy(mRsp->modelview_matrix_stack[mRsp->modelview_matrix_stack_size - 1],
                   mRsp->modelview_matrix_stack[mRsp->modelview_matrix_stack_size - 2], sizeof(matrix));
#ifdef LUS_XBOX
            int lv = mRsp->modelview_matrix_stack_size - 1;
            memcpy(mHwRecipeMv[lv], mHwRecipeMv[lv - 1], sizeof(mHwRecipeMv[lv]));
            mHwRecipeNmv[lv] = mHwRecipeNmv[lv - 1];
#endif
        }
        if (parameters & mtx_load) {
            if (mRsp->modelview_matrix_stack_size == 0)
                ++mRsp->modelview_matrix_stack_size;
            memcpy(mRsp->modelview_matrix_stack[mRsp->modelview_matrix_stack_size - 1], matrix, sizeof(matrix));
        } else {
            MatrixMul(mRsp->modelview_matrix_stack[mRsp->modelview_matrix_stack_size - 1], matrix,
                      mRsp->modelview_matrix_stack[mRsp->modelview_matrix_stack_size - 1]);
        }
#ifdef LUS_XBOX
        int lv = mRsp->modelview_matrix_stack_size - 1;
        if (parameters & mtx_load) {
            mHwRecipeMv[lv][0] = addr;
            mHwRecipeNmv[lv] = 1;
        } else if (mHwRecipeNmv[lv] >= 1 && mHwRecipeNmv[lv] < 4) {
            mHwRecipeMv[lv][mHwRecipeNmv[lv]++] = addr;
        } else {
            mHwRecipeNmv[lv] = 0;
        }
#endif
        mRsp->lights_changed = 1;
    }
    MatrixMul(mRsp->MP_matrix, mRsp->modelview_matrix_stack[mRsp->modelview_matrix_stack_size - 1], mRsp->P_matrix);
}

#ifdef LUS_XBOX
// A decal's matrix: its depth a little nearer, in proportion to the distance (1 - z grown by the fraction), at no cost
// in the shader. A dirt road lies exactly on the grass, but the corners of what's cut for the GPU's path (near the
// camera, at a clamp) are whole numbers: up to half a unit off the plane, so a piece of grass could stand above the
// road by more than a depth offset covers.
static inline void HwPushDecal(float mp[4][4]) {
    constexpr float kPush = 1.0f / 32.0f;
    for (int i = 0; i < 4; i++) {
        mp[i][2] = mp[i][2] * (1.0f + kPush) - mp[i][3] * kPush;
    }
}

// The frame's states made again with the port's blended matrices (one of a tick's frames between two
// ticks: Replay). A state whose recipe wasn't kept (a chain of matrices with no room, or none loaded)
// keeps the frame's own: drawn where the tick put it.
void Interpreter::HwRebuildStates(const MtxBlends* blends) {
    float p[4][4], mv[4][4], m[4][4];
    const int32_t* madeP[2] = {}; // what p is made of: nearly every state has the same projection
    uint8_t madeNp = 0;
    for (size_t i = 0; i < mHwStates.size() && i < mHwRecipes.size(); i++) {
        if (i >= mHwStateUsed.size() || mHwStateUsed[i] == 0) {
            continue; // all it placed was dropped: no draw reads it
        }
        sXboxStatesRebuilt++;
        const HwRecipe& r = mHwRecipes[i];
        if (r.tri) {
            HwFillTriState(mHwStates[i], r); // (its corners' states are earlier ones: rebuilt already)
            continue;
        }
        if (r.np == 0 || r.nmv == 0) {
            continue;
        }
        if (r.np != madeNp || r.p[0] != madeP[0] || r.p[1] != madeP[1]) {
            LoadMtx(p, r.p[0], blends);
            for (int k = 1; k < r.np; k++) {
                LoadMtx(m, r.p[k], blends);
                MatrixMul(p, m, p);
            }
            madeNp = r.np;
            madeP[0] = r.p[0];
            madeP[1] = r.p[1];
        }
        LoadMtx(mv, r.mv[0], blends);
        for (int k = 1; k < r.nmv; k++) {
            LoadMtx(m, r.mv[k], blends);
            MatrixMul(mv, m, mv);
        }
        GfxRenderingAPI::HwState& s = mHwStates[i];
        MatrixMul(s.mp, mv, p);
        if (r.decal) {
            HwPushDecal(s.mp);
        }
        if (r.lit) {
            memcpy(s.mv, mv, sizeof(s.mv)); // nscale stays the tick's: a blend of two rotations is near one
        }
    }
}

void Interpreter::Replay(const MtxBlends* blends, bool last) {
    LusXbox_Where("rendering: a replay");
    const double rebuildStart = LusXbox_Seconds();
    HwRebuildStates(blends); // the tick's own matrices again when there are no replacements
    HwClipTriangles();
    sXboxRebuildSeconds += LusXbox_Seconds() - rebuildStart;
    sXboxReplays++;
    sXboxClipWatched += mHwClipTris.size();
    mRapi->StartFrame();
    if (!mHwBlendRecorded.empty()) {
        mRapi->BlendRecordedVerts(mHwBlendRecorded.data(), mHwBlendRecorded.size(),
                                  blends != nullptr ? mInterpolationT : 1.0f);
    }
    mRapi->Replay(mHwStates.data(), mHwLights.data(), last);
}

void Interpreter::ForgetBoundTextures() {
    std::fill(std::begin(mRenderingState.mTextures), std::end(mRenderingState.mTextures), nullptr);
    mRdp->textures_changed[0] = true;
    mRdp->textures_changed[1] = true;
}
#endif

void Interpreter::GfxSpPopMatrix(uint32_t count) {
#ifdef LUS_XBOX
    mHwVtxVersion++;
#endif
    while (count--) {
        if (mRsp->modelview_matrix_stack_size > 0) {
            --mRsp->modelview_matrix_stack_size;
            if (mRsp->modelview_matrix_stack_size > 0) {
                MatrixMul(mRsp->MP_matrix, mRsp->modelview_matrix_stack[mRsp->modelview_matrix_stack_size - 1],
                          mRsp->P_matrix);
            }
        }
    }
    mRsp->lights_changed = true;
}

float Interpreter::AdjXForAspectRatio(float x) const {
    // Skip widescreen adjustment for fixed-size off-screen FBs (HUD elements,
    // small capture buffers), or those which specify a fixed aspect ratio.
    if (mFbActive && mActiveFrameBuffer != mFrameBuffers.end() &&
        (!mActiveFrameBuffer->second.resize || mActiveFrameBuffer->second.forceFixedAspect)) {
        return x;
    } else {
        return x * (4.0f / 3.0f) / ((float)mCurDimensions.width / (float)mCurDimensions.height);
    }
}

// Scale the width and height value based on the ratio of the viewport to the native size
void Interpreter::AdjustWidthHeightForScale(uint32_t& width, uint32_t& height, uint32_t nativeWidth,
                                            uint32_t nativeHeight) const {
    width = round(width * (mCurDimensions.width / (2.0f * (nativeWidth / 2))));
    height = round(height * (mCurDimensions.height / (2.0f * (nativeHeight / 2))));

    if (width == 0) {
        width = 1;
    }
    if (height == 0) {
        height = 1;
    }
}

void Interpreter::GfxSpVertex(size_t n_vertices, size_t dest_index, const F3DVtx* vertices) {
#ifdef LUS_XBOX
    // the position as one SSE vector: x, y, z and w are a row combination of the matrix (~10,000
    // vertices a Clock Town frame, each made again for every interpolated frame)
    const __m128 mp0 = _mm_loadu_ps(mRsp->MP_matrix[0]), mp1 = _mm_loadu_ps(mRsp->MP_matrix[1]);
    const __m128 mp2 = _mm_loadu_ps(mRsp->MP_matrix[2]), mp3 = _mm_loadu_ps(mRsp->MP_matrix[3]);

    // with the GPU's transform: the state these vertices are loaded under (a new one when it changed),
    // and no position, clipping or fog made here (EnsureTransformed, when a CPU path needs them)
    uint16_t hwState = 0, hwLight = 0;
    // these vertices' last tick, if they move by themselves
    const F3DVtx* hwPrev = nullptr;
    for (uint32_t i = 0; mHwTnl && i < sBlendRangeCount; i++) {
        const BlendRange& r = sBlendRanges[i];
        if (vertices >= r.cur && vertices + n_vertices <= r.cur + r.count) {
            hwPrev = r.prev + (vertices - r.cur);
            break;
        }
    }
    const bool hwDecal = (mRdp->other_mode_l & ZMODE_DEC) == ZMODE_DEC; // (a material comes before its G_VTX)
    if (mHwTnl && mHwVtxVersion == mHwVtxCachedVersion && hwDecal == mHwVtxCachedDecal) {
        hwState = mHwVtxCachedState; // nothing the state depends on changed since the last G_VTX
        hwLight = mHwVtxCachedLight;
    } else if (mHwTnl) {
        GfxRenderingAPI::HwState s;
        memcpy(s.mp, mRsp->MP_matrix, sizeof(s.mp));
        if (hwDecal) {
            HwPushDecal(s.mp);
        }
        s.aspect = AdjXForAspectRatio(1.0f);
        s.fogMul = mRsp->fog_mul;
        s.fogOffset = mRsp->fog_offset;
        s.fog = (mRsp->geometry_mode & G_FOG) ? 1.0f : 0.0f;
        s.nscale = 0.0f;
        const bool lighting = (mRsp->geometry_mode & G_LIGHTING) != 0;
        if (lighting) {
            memcpy(s.mv, mRsp->modelview_matrix_stack[mRsp->modelview_matrix_stack_size - 1], sizeof(s.mv));
            // the GPU's directional lighting takes the normal through the modelview and divides by its
            // scale, the same as the CPU's lights taken back through it and normalized only when it is a
            // rotation and a uniform scale (rows as long, at right angles)
            float l0 = s.mv[0][0] * s.mv[0][0] + s.mv[0][1] * s.mv[0][1] + s.mv[0][2] * s.mv[0][2];
            float l1 = s.mv[1][0] * s.mv[1][0] + s.mv[1][1] * s.mv[1][1] + s.mv[1][2] * s.mv[1][2];
            float l2 = s.mv[2][0] * s.mv[2][0] + s.mv[2][1] * s.mv[2][1] + s.mv[2][2] * s.mv[2][2];
            float d01 = s.mv[0][0] * s.mv[1][0] + s.mv[0][1] * s.mv[1][1] + s.mv[0][2] * s.mv[1][2];
            float d02 = s.mv[0][0] * s.mv[2][0] + s.mv[0][1] * s.mv[2][1] + s.mv[0][2] * s.mv[2][2];
            float d12 = s.mv[1][0] * s.mv[2][0] + s.mv[1][1] * s.mv[2][1] + s.mv[1][2] * s.mv[2][2];
            float tol = l0 * 0.002f;
            if (l0 > 0.0f && fabsf(l1 - l0) <= tol && fabsf(l2 - l0) <= tol && fabsf(d01) <= tol &&
                fabsf(d02) <= tol && fabsf(d12) <= tol) {
                s.nscale = 1.0f / sqrtf(l0);
            }
        } else {
            memset(s.mv, 0, sizeof(s.mv)); // not read: kept equal so states still merge
        }
        // (two limbs can have the same matrix at the tick and different ones between ticks: the recipe counts)
        HwRecipe r;
        memset(&r, 0, sizeof(r));
        const int lv = mRsp->modelview_matrix_stack_size - 1;
        r.np = mHwRecipeNp;
        r.nmv = lv >= 0 ? mHwRecipeNmv[lv] : 0;
        r.lit = lighting;
        r.decal = hwDecal;
        memcpy(r.p, mHwRecipeP, r.np * sizeof(r.p[0]));
        if (r.nmv != 0) {
            memcpy(r.mv, mHwRecipeMv[lv], r.nmv * sizeof(r.mv[0]));
        }
        if ((mHwStates.empty() || memcmp(&mHwStates.back(), &s, sizeof(s)) != 0 ||
             memcmp(&mHwRecipes.back(), &r, sizeof(r)) != 0) &&
            mHwStates.size() < 65535) {
            mHwStates.push_back(s);
            mHwRecipes.push_back(r);
        }
        hwState = (uint16_t)(mHwStates.size() - 1);
        // lit on the GPU unless texgen's on (its coordinates are made here) or the lights don't fit
        if (lighting && !(mRsp->geometry_mode & G_TEXTURE_GEN) && mHwStates[hwState].nscale != 0.0f) {
            hwLight = HwLightSet();
        }
        mHwVtxCachedVersion = mHwVtxVersion;
        mHwVtxCachedDecal = hwDecal;
        mHwVtxCachedState = hwState;
        mHwVtxCachedLight = hwLight;
    }
#endif
    for (size_t i = 0; i < n_vertices; i++, dest_index++) {
        const F3DVtx_t* v = &vertices[i].v;
        const F3DVtx_tn* vn = &vertices[i].n;
        struct LoadedVertex* d = &mRsp->loaded_vertices[dest_index];

        if (v == nullptr) {
            return;
        }

#ifdef LUS_XBOX
        float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
        d->hw = mHwTnl;
        d->hwBlend = hwPrev != nullptr;
        if (mHwTnl) {
            d->ob[0] = v->ob[0];
            d->ob[1] = v->ob[1];
            d->ob[2] = v->ob[2];
            if (hwPrev != nullptr) {
                memcpy(d->hwPrev, hwPrev[i].v.ob, sizeof(d->hwPrev));
            }
            d->state = hwState;
            d->alpha = v->cn[3];
            d->transformed = false;
            d->hwRisk = 0;
        } else {
            float pos[4];
            _mm_storeu_ps(pos, _mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps((float)v->ob[0]), mp0),
                                                     _mm_mul_ps(_mm_set1_ps((float)v->ob[1]), mp1)),
                                          _mm_add_ps(_mm_mul_ps(_mm_set1_ps((float)v->ob[2]), mp2), mp3)));
            x = pos[0];
            y = pos[1];
            z = pos[2];
            w = pos[3];
        }
        d->light = hwLight;
        if (hwLight != 0) {
            // lit by the GPU (or EnsureLit): the normal kept
            d->n[0] = vn->n[0];
            d->n[1] = vn->n[1];
            d->n[2] = vn->n[2];
            d->u = (short)(v->tc[0] * mRsp->texture_scaling_factor.s >> 16);
            d->v = (short)(v->tc[1] * mRsp->texture_scaling_factor.t >> 16);
            d->clip_rej = 0;
            d->color.a = v->cn[3];
            continue;
        }
#else
        float x = v->ob[0] * mRsp->MP_matrix[0][0] + v->ob[1] * mRsp->MP_matrix[1][0] +
                  v->ob[2] * mRsp->MP_matrix[2][0] + mRsp->MP_matrix[3][0];
        float y = v->ob[0] * mRsp->MP_matrix[0][1] + v->ob[1] * mRsp->MP_matrix[1][1] +
                  v->ob[2] * mRsp->MP_matrix[2][1] + mRsp->MP_matrix[3][1];
        float z = v->ob[0] * mRsp->MP_matrix[0][2] + v->ob[1] * mRsp->MP_matrix[1][2] +
                  v->ob[2] * mRsp->MP_matrix[2][2] + mRsp->MP_matrix[3][2];
        float w = v->ob[0] * mRsp->MP_matrix[0][3] + v->ob[1] * mRsp->MP_matrix[1][3] +
                  v->ob[2] * mRsp->MP_matrix[2][3] + mRsp->MP_matrix[3][3];
#endif

        float world_pos[3] = { 0.0 };
        if (mRsp->geometry_mode & G_LIGHTING_POSITIONAL) {
            float(*mtx)[4] = mRsp->modelview_matrix_stack[mRsp->modelview_matrix_stack_size - 1];
            world_pos[0] = v->ob[0] * mtx[0][0] + v->ob[1] * mtx[1][0] + v->ob[2] * mtx[2][0] + mtx[3][0];
            world_pos[1] = v->ob[0] * mtx[0][1] + v->ob[1] * mtx[1][1] + v->ob[2] * mtx[2][1] + mtx[3][1];
            world_pos[2] = v->ob[0] * mtx[0][2] + v->ob[1] * mtx[1][2] + v->ob[2] * mtx[2][2] + mtx[3][2];
        }

#ifdef LUS_XBOX
        if (!mHwTnl) { // (the GPU's state carries the aspect: a divide a vertex otherwise)
            x = AdjXForAspectRatio(x);
        }
#else
        x = AdjXForAspectRatio(x);
#endif

        short U = v->tc[0] * mRsp->texture_scaling_factor.s >> 16;
        short V = v->tc[1] * mRsp->texture_scaling_factor.t >> 16;

        if (mRsp->geometry_mode & G_LIGHTING) {
            if (mRsp->lights_changed) {
                for (int i = 0; i < mRsp->current_num_lights - 1; i++) {
                    CalculateNormalDir(&mRsp->current_lights[i].l, mRsp->current_lights_coeffs[i]);
                }
                /*static const Light_t lookat_x = {{0, 0, 0}, 0, {0, 0, 0}, 0, {127, 0, 0}, 0};
                static const Light_t lookat_y = {{0, 0, 0}, 0, {0, 0, 0}, 0, {0, 127, 0}, 0};*/
                CalculateNormalDir(&mRsp->lookat[0], mRsp->current_lookat_coeffs[0]);
                CalculateNormalDir(&mRsp->lookat[1], mRsp->current_lookat_coeffs[1]);
                mRsp->lights_changed = false;
            }

            int r = mRsp->current_lights[mRsp->current_num_lights - 1].l.col[0];
            int g = mRsp->current_lights[mRsp->current_num_lights - 1].l.col[1];
            int b = mRsp->current_lights[mRsp->current_num_lights - 1].l.col[2];

            for (int i = 0; i < mRsp->current_num_lights - 1; i++) {
                float intensity = 0;
                if ((mRsp->geometry_mode & G_LIGHTING_POSITIONAL) && (mRsp->current_lights[i].p.unk3 != 0)) {
                    // Calculate distance from the light to the vertex
                    float dist_vec[3] = { mRsp->current_lights[i].p.pos[0] - world_pos[0],
                                          mRsp->current_lights[i].p.pos[1] - world_pos[1],
                                          mRsp->current_lights[i].p.pos[2] - world_pos[2] };
                    float dist_sq =
                        dist_vec[0] * dist_vec[0] + dist_vec[1] * dist_vec[1] +
                        dist_vec[2] * dist_vec[2] * 2; // The *2 comes from GLideN64, unsure of why it does it
                    float dist = sqrt(dist_sq);

                    // Transform distance vector (which acts as a direction light vector) into model's space
                    float light_model[3];
                    TransposedMatrixMul(light_model, dist_vec,
                                        mRsp->modelview_matrix_stack[mRsp->modelview_matrix_stack_size - 1]);

                    // Calculate intensity for each axis using standard formula for intensity
                    float light_intensity[3];
                    for (int light_i = 0; light_i < 3; light_i++) {
                        light_intensity[light_i] = 4.0f * light_model[light_i] / dist_sq;
                        light_intensity[light_i] = std::clamp(light_intensity[light_i], -1.0f, 1.0f);
                    }

                    // Adjust intensity based on surface normal and sum up total
                    float total_intensity =
                        light_intensity[0] * vn->n[0] + light_intensity[1] * vn->n[1] + light_intensity[2] * vn->n[2];
                    total_intensity = std::clamp(total_intensity, -1.0f, 1.0f);

                    // Attenuate intensity based on attenuation values.
                    // Example formula found at https://ogldev.org/www/tutorial20/tutorial20.html
                    // Specific coefficients for MM's microcode sourced from GLideN64
                    // https://github.com/gonetz/GLideN64/blob/3b43a13a80dfc2eb6357673440b335e54eaa3896/src/gSP.cpp#L636
                    float distf = floorf(dist);
                    float attenuation = (distf * mRsp->current_lights[i].p.unk7 * 2.0f +
                                         distf * distf * mRsp->current_lights[i].p.unkE / 8.0f) /
                                            (float)0xFFFF +
                                        1.0f;
                    intensity = total_intensity / attenuation;
                } else {
                    intensity += vn->n[0] * mRsp->current_lights_coeffs[i][0];
                    intensity += vn->n[1] * mRsp->current_lights_coeffs[i][1];
                    intensity += vn->n[2] * mRsp->current_lights_coeffs[i][2];
#ifdef LUS_XBOX
                    intensity *= 1.0f / 127.0f; // a divide is ~30 cycles on the Pentium III, a vertex and light
#else
                    intensity /= 127.0f;
#endif
                }
                if (intensity > 0.0f) {
                    r += intensity * mRsp->current_lights[i].l.col[0];
                    g += intensity * mRsp->current_lights[i].l.col[1];
                    b += intensity * mRsp->current_lights[i].l.col[2];
                }
            }

            d->color.r = r > 255 ? 255 : r;
            d->color.g = g > 255 ? 255 : g;
            d->color.b = b > 255 ? 255 : b;

            if (mRsp->geometry_mode & G_TEXTURE_GEN) {
                float dotx = 0, doty = 0;
                dotx += vn->n[0] * mRsp->current_lookat_coeffs[0][0];
                dotx += vn->n[1] * mRsp->current_lookat_coeffs[0][1];
                dotx += vn->n[2] * mRsp->current_lookat_coeffs[0][2];
                doty += vn->n[0] * mRsp->current_lookat_coeffs[1][0];
                doty += vn->n[1] * mRsp->current_lookat_coeffs[1][1];
                doty += vn->n[2] * mRsp->current_lookat_coeffs[1][2];

                dotx /= 127.0f;
                doty /= 127.0f;

                dotx = Ship::Math::clamp(dotx, -1.0f, 1.0f);
                doty = Ship::Math::clamp(doty, -1.0f, 1.0f);

                if (mRsp->geometry_mode & G_TEXTURE_GEN_LINEAR) {
                    // Not sure exactly what formula we should use to get accurate values
                    /*dotx = (2.906921f * dotx * dotx + 1.36114f) * dotx;
                    doty = (2.906921f * doty * doty + 1.36114f) * doty;
                    dotx = (dotx + 1.0f) / 4.0f;
                    doty = (doty + 1.0f) / 4.0f;*/
                    dotx = acosf(-dotx) /* M_PI */ * 0.159155f;
                    doty = acosf(-doty) /* M_PI */ * 0.159155f;
                } else {
                    dotx = (dotx + 1.0f) / 4.0f;
                    doty = (doty + 1.0f) / 4.0f;
                }

                U = (int32_t)(dotx * mRsp->texture_scaling_factor.s);
                V = (int32_t)(doty * mRsp->texture_scaling_factor.t);
            }
        } else {
            d->color.r = v->cn[0];
            d->color.g = v->cn[1];
            d->color.b = v->cn[2];
        }

        d->u = U;
        d->v = V;

#ifdef LUS_XBOX
        if (mHwTnl) {
            d->clip_rej = 0;
            d->color.a = v->cn[3]; // fog's, if on, is the GPU's (or EnsureTransformed's)
            continue;
        }
#endif

        // trivial clip rejection
        d->clip_rej = 0;
        if (x < -w) {
            d->clip_rej |= 1; // CLIP_LEFT
        }
        if (x > w) {
            d->clip_rej |= 2; // CLIP_RIGHT
        }
        if (y < -w) {
            d->clip_rej |= 4; // CLIP_BOTTOM
        }
        if (y > w) {
            d->clip_rej |= 8; // CLIP_TOP
        }
        // if (z < -w) d->clip_rej |= 16; // CLIP_NEAR
        if (z > w) {
            d->clip_rej |= 32; // CLIP_FAR
        }

        d->x = x;
        d->y = y;
        d->z = z;
        d->w = w;

        if (mRsp->geometry_mode & G_FOG) {
            if (fabsf(w) < 0.001f) {
                // To avoid division by zero
                w = 0.001f;
            }

            float winv = 1.0f / w;
            if (winv < 0.0f) {
                winv = std::numeric_limits<int16_t>::max();
            }

            float fog_z = z * winv * mRsp->fog_mul + mRsp->fog_offset;
            fog_z = Ship::Math::clamp(fog_z, 0.0f, 255.0f);
            d->color.a = fog_z; // Use alpha variable to store fog factor
        } else {
            d->color.a = v->cn[3];
        }
    }
}

#ifdef LUS_XBOX
// the lights GfxSpVertex would light with, as a set for the GPU (the last one again if the same); 0 if the
// GPU's shader can't take them (the CPU lights the vertices)
uint16_t Interpreter::HwLightSet() {
    GfxRenderingAPI::HwLights l{};
    const bool positional = (mRsp->geometry_mode & G_LIGHTING_POSITIONAL) != 0;
    const int numLights = mRsp->current_num_lights - 1;
    if (numLights < 0) {
        return 0;
    }
    for (int i = 0; i < numLights; i++) {
        const F3DLight& li = mRsp->current_lights[i];
        if (positional && li.p.unk3 != 0) {
            if (l.numPoint == GfxRenderingAPI::kHwMaxPointLights) {
                return 0;
            }
            int k = l.numPoint++;
            for (int c = 0; c < 3; c++) {
                l.pos[k][c] = li.p.pos[c];
                l.posCol[k][c] = li.l.col[c];
            }
            l.att[k][0] = li.p.unk7 * 2.0f / (float)0xFFFF;
            l.att[k][1] = li.p.unkE / 8.0f / (float)0xFFFF;
        } else {
            if (l.numDir == GfxRenderingAPI::kHwMaxDirLights) {
                return 0;
            }
            int k = l.numDir++;
            float d[3] = { li.l.dir[0] / 127.0f, li.l.dir[1] / 127.0f, li.l.dir[2] / 127.0f };
            float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            for (int c = 0; c < 3; c++) {
                l.dir[k][c] = len > 0.0f ? d[c] / len : 0.0f;
                l.dirCol[k][c] = li.l.col[c];
            }
        }
    }
    for (int c = 0; c < 3; c++) {
        l.ambient[c] = mRsp->current_lights[numLights].l.col[c];
    }
    if (mRapi->HwLitSlots(l.numDir, l.numPoint) <= 0) {
        return 0;
    }
    if (mHwLights.empty() || memcmp(&mHwLights.back(), &l, sizeof(l)) != 0) {
        if (mHwLights.size() >= 65535) {
            return 0;
        }
        mHwLights.push_back(l);
    }
    return (uint16_t)mHwLights.size();
}

// a vertex the GPU was to light, lit here as GfxSpVertex does it: for a triangle that can't go to a lit batch
void Interpreter::EnsureLit(LoadedVertex* d) {
    if (d->light == 0) {
        return;
    }
    const GfxRenderingAPI::HwLights& l = mHwLights[d->light - 1];
    const GfxRenderingAPI::HwState& s = mHwStates[d->state];
    d->light = 0;

    int r = (int)l.ambient[0], g = (int)l.ambient[1], b = (int)l.ambient[2];
    for (int i = 0; i < l.numDir; i++) {
        float coeffs[3];
        TransposedMatrixMul(coeffs, l.dir[i], s.mv);
        NormalizeVector(coeffs);
        float intensity = (d->n[0] * coeffs[0] + d->n[1] * coeffs[1] + d->n[2] * coeffs[2]) * (1.0f / 127.0f);
        if (intensity > 0.0f) {
            r += intensity * l.dirCol[i][0];
            g += intensity * l.dirCol[i][1];
            b += intensity * l.dirCol[i][2];
        }
    }
    if (l.numPoint > 0) {
        float world[3];
        for (int j = 0; j < 3; j++) {
            world[j] = d->ob[0] * s.mv[0][j] + d->ob[1] * s.mv[1][j] + d->ob[2] * s.mv[2][j] + s.mv[3][j];
        }
        for (int i = 0; i < l.numPoint; i++) {
            float dist_vec[3] = { l.pos[i][0] - world[0], l.pos[i][1] - world[1], l.pos[i][2] - world[2] };
            float dist_sq = dist_vec[0] * dist_vec[0] + dist_vec[1] * dist_vec[1] + dist_vec[2] * dist_vec[2] * 2;
            float dist = sqrtf(dist_sq);
            float light_model[3];
            TransposedMatrixMul(light_model, dist_vec, s.mv);
            float total = 0.0f;
            for (int c = 0; c < 3; c++) {
                total += std::clamp(4.0f * light_model[c] / dist_sq, -1.0f, 1.0f) * d->n[c];
            }
            total = std::clamp(total, -1.0f, 1.0f);
            float distf = floorf(dist);
            float intensity = total / (distf * l.att[i][0] + distf * distf * l.att[i][1] + 1.0f);
            if (intensity > 0.0f) {
                r += intensity * l.posCol[i][0];
                g += intensity * l.posCol[i][1];
                b += intensity * l.posCol[i][2];
            }
        }
    }
    d->color.r = r > 255 ? 255 : r;
    d->color.g = g > 255 ? 255 : g;
    d->color.b = b > 255 ? 255 : b;
}

// a raw vertex's position, clipping and fog as GfxSpVertex would have made them, under the state it was
// loaded with: for a triangle the GPU's path can't draw, and the commands that read a position
void Interpreter::EnsureTransformed(LoadedVertex* d) {
    if (!d->hw || d->transformed) {
        return;
    }
    d->transformed = true;
    if (d->state >= mHwStates.size()) {
        d->x = d->y = d->z = 0.0f;
        d->w = 1.0f;
        d->clip_rej = 0;
        return;
    }
    const GfxRenderingAPI::HwState& s = mHwStates[d->state];
    float pos[4];
    _mm_storeu_ps(pos, _mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps((float)d->ob[0]), _mm_loadu_ps(s.mp[0])),
                                             _mm_mul_ps(_mm_set1_ps((float)d->ob[1]), _mm_loadu_ps(s.mp[1]))),
                                  _mm_add_ps(_mm_mul_ps(_mm_set1_ps((float)d->ob[2]), _mm_loadu_ps(s.mp[2])),
                                             _mm_loadu_ps(s.mp[3]))));
    float x = pos[0] * s.aspect, y = pos[1], z = pos[2], w = pos[3];

    d->clip_rej = (x < -w ? 1 : 0) | (x > w ? 2 : 0) | (y < -w ? 4 : 0) | (y > w ? 8 : 0) | (z > w ? 32 : 0);
    d->x = x;
    d->y = y;
    d->z = z;
    d->w = w;
    if (s.fog != 0.0f) {
        if (fabsf(w) < 0.001f) {
            w = 0.001f;
        }
        float winv = 1.0f / w;
        if (winv < 0.0f) {
            winv = std::numeric_limits<int16_t>::max();
        }
        d->color.a = Ship::Math::clamp(z * winv * s.fogMul + s.fogOffset, 0.0f, 255.0f);
    } else {
        d->color.a = d->alpha;
    }
}
#endif

void Interpreter::GfxSpModifyVertex(uint16_t vtx_idx, uint8_t where, uint32_t val) {
    SUPPORT_CHECK(where == G_MWO_POINT_ST);

    int16_t s = (int16_t)(val >> 16);
    int16_t t = (int16_t)val;

    LoadedVertex* v = &mRsp->loaded_vertices[vtx_idx];
    v->u = s;
    v->v = t;
}

void Interpreter::GfxSpTri1(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx, bool is_rect) {
    struct LoadedVertex* v1 = &mRsp->loaded_vertices[vtx1_idx];
    struct LoadedVertex* v2 = &mRsp->loaded_vertices[vtx2_idx];
    struct LoadedVertex* v3 = &mRsp->loaded_vertices[vtx3_idx];
    struct LoadedVertex* v_arr[3] = { v1, v2, v3 };

    // if (rand()%2) return;

    const uint32_t cull_both = get_attr(CULL_BOTH);
    const uint32_t cull_front = get_attr(CULL_FRONT);
    const uint32_t cull_back = get_attr(CULL_BACK);

    // trivial rejection and culling on the CPU: true if the triangle isn't drawn
    auto cpuReject = [&]() -> bool {
        if (v1->clip_rej & v2->clip_rej & v3->clip_rej) {
            // The whole triangle lies outside the visible area
            return true;
        }
        if ((mRsp->geometry_mode & cull_both) != 0) {
            float dx1 = v1->x / (v1->w) - v2->x / (v2->w);
            float dy1 = v1->y / (v1->w) - v2->y / (v2->w);
            float dx2 = v3->x / (v3->w) - v2->x / (v2->w);
            float dy2 = v3->y / (v3->w) - v2->y / (v2->w);
            float cross = dx1 * dy2 - dy1 * dx2;

            if ((v1->w < 0) ^ (v2->w < 0) ^ (v3->w < 0)) {
                // If one vertex lies behind the eye, negating cross will give the correct result.
                // If all vertices lie behind the eye, the triangle will be rejected anyway.
                cross = -cross;
            }

            // G_EX_INVERT_CULLING is a LUS extension, not tied to a specific ucode,
            // so apply it regardless of the active microcode handler.
            if ((mRsp->extra_geometry_mode & G_EX_INVERT_CULLING) != 0) {
                cross = -cross;
            }

            auto cull_type = mRsp->geometry_mode & cull_both;

            if (cull_type == cull_front) {
                if (cross <= 0) {
                    return true;
                }
            } else if (cull_type == cull_back) {
                if (cross >= 0) {
                    return true;
                }
            } else if (cull_type == cull_both) {
                // Why is this even an option?
                return true;
            }
        }
        return false;
    };

#ifdef LUS_XBOX
    // loaded raw, the GPU clips and culls: the N64's back faces (cross >= 0 above) are the ones the NV2A
    // calls clockwise (measured: with them taken as counter-clockwise, front faces went, the floors,
    // skyboxes and 2D quads with them)
    bool hwVerts = mHwTnl && v1->hw && v2->hw && v3->hw;
    int hwCull = 0;
    if (hwVerts) {
        auto cull_type = mRsp->geometry_mode & cull_both;
        bool invert = (mRsp->extra_geometry_mode & G_EX_INVERT_CULLING) != 0;
        if (cull_type == cull_both) {
            return;
        } else if (cull_type == cull_back) {
            hwCull = invert ? 2 : 1;
        } else if (cull_type == cull_front) {
            hwCull = invert ? 1 : 2;
        }
        if (!is_rect) {
            HwVertexRisky(v1);
            HwVertexRisky(v2);
            HwVertexRisky(v3);
            if ((v1->hwOut & v2->hwOut & v3->hwOut) != 0) {
                sXboxDropped++; // (no state touched: the next triangle drawn sets what this one would have)
                return;
            }
        }
        if (mHwFast && !is_rect && v1->light == v2->light && v1->light == v3->light) {
            // the state is the last triangle's, which the batch took: this one goes there too, or in a new
            // batch if the lights are all that differ (the other constants don't depend on them)
            if (v1->light != mHwBatch.lightSet) {
                if (mHwTris > 0) {
                    Flush();
                }
                HwSetBatchLights(v1->light);
            }
            if (hwCull != mHwBatch.cull) {
                if (mHwTris > 0) {
                    Flush();
                }
                mHwBatch.cull = hwCull;
            }
            if (!HwClampReached(v_arr) && !HwSpanRisky(v_arr)) {
                HwEmit(v_arr);
                sXboxFastTris++;
                return;
            }
            // cut, into the same batch: unless it spans limbs in a lit batch (it's lit here then: another batch)
            const bool oneXform = HwSameXform(v1->state, v2->state) && HwSameXform(v1->state, v3->state);
            if ((oneXform || mHwBatch.lightSet == 0) && HwClampTriangle(v_arr, nullptr)) {
                sXboxFastTris++;
                return;
            }
        }
        sXboxSlowWhy[!mHwFast ? 0 : is_rect ? 1 : v1->light != mHwBatch.lightSet || v2->light != mHwBatch.lightSet ||
                                                          v3->light != mHwBatch.lightSet ? 2 : 3]++;
    } else {
        EnsureTransformed(v1);
        EnsureTransformed(v2);
        EnsureTransformed(v3);
        if (cpuReject()) {
            return;
        }
        EnsureLit(v1);
        EnsureLit(v2);
        EnsureLit(v3);
    }
    mHwFast = false;
    XboxTimed timed(sXboxSlowCycles);
#else
    if (cpuReject()) {
        return;
    }
#endif

    // depth_test is set when the fragment has a depth value to compare (either from vertex Z via
    // RSP G_ZBUFFER, or from the prim-depth register via G_ZS_PRIM) and Z_CMP is requested.
    bool zbuffer_enabled = (mRsp->geometry_mode & G_ZBUFFER) == G_ZBUFFER;
    bool prim_depth_enabled = (mRdp->other_mode_l & G_ZS_PRIM) != 0;
    bool depth_test = (zbuffer_enabled || prim_depth_enabled) && (mRdp->other_mode_l & Z_CMP) == Z_CMP;
    bool depth_mask = (mRdp->other_mode_l & Z_UPD) == Z_UPD;
    uint8_t depth_test_and_mask = (depth_test ? 1 : 0) | (depth_mask ? 2 : 0);
    if (depth_test_and_mask != mRenderingState.depth_test_and_mask) {
        Flush();
        mRapi->SetDepthTestAndMask(depth_test, depth_mask);
        mRenderingState.depth_test_and_mask = depth_test_and_mask;
    }

    bool zmode_decal = (mRdp->other_mode_l & ZMODE_DEC) == ZMODE_DEC;
    if (zmode_decal != mRenderingState.decal_mode) {
        Flush();
        mRapi->SetZmodeDecal(zmode_decal);
        mRenderingState.decal_mode = zmode_decal;
    }

    if (mRdp->viewport_or_scissor_changed) {
        if (memcmp(&mRdp->viewport, &mRenderingState.viewport, sizeof(mRdp->viewport)) != 0) {
            Flush();
            mRapi->SetViewport(mRdp->viewport.x, mRdp->viewport.y, mRdp->viewport.width, mRdp->viewport.height);
            mRenderingState.viewport = mRdp->viewport;
        }
        if (memcmp(&mRdp->scissor, &mRenderingState.scissor, sizeof(mRdp->scissor)) != 0) {
            Flush();
            mRapi->SetScissor(mRdp->scissor.x, mRdp->scissor.y, mRdp->scissor.width, mRdp->scissor.height);
            mRenderingState.scissor = mRdp->scissor;
        }
        mRdp->viewport_or_scissor_changed = false;
    }

    uint64_t cc_id = mRdp->combine_mode;
    uint64_t cc_options = 0;
    bool use_alpha = ((mRdp->other_mode_l & (3 << 20)) == (G_BL_CLR_MEM << 20) &&
                      (mRdp->other_mode_l & (3 << 16)) == (G_BL_1MA << 16)) ||
                     ((mRdp->other_mode_l & (3 << 22)) == (G_BL_CLR_MEM << 22) &&
                      (mRdp->other_mode_l & (3 << 18)) == (G_BL_1MA << 18));
    uint8_t blend_src = mRdp->other_mode_l >> 30;
    bool use_blend_color = blend_src == G_BL_CLR_BL;
    bool use_fog = blend_src == G_BL_CLR_FOG || use_blend_color;
    bool texture_edge = (mRdp->other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA;
    bool use_noise = (mRdp->other_mode_l & (3U << G_MDSFT_ALPHACOMPARE)) == G_AC_DITHER;
    bool use_2cyc = (mRdp->other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE;
    bool alpha_threshold = (mRdp->other_mode_l & (3U << G_MDSFT_ALPHACOMPARE)) == G_AC_THRESHOLD;
    bool invisible =
        (mRdp->other_mode_l & (3 << 24)) == (G_BL_0 << 24) && (mRdp->other_mode_l & (3 << 20)) == (G_BL_CLR_MEM << 20);
    bool use_grayscale = mRdp->grayscale;
    bool use_prim_depth = (mRdp->other_mode_l & G_ZS_PRIM) != 0;

    if (texture_edge) {
        if (use_alpha) {
            alpha_threshold = true;
            texture_edge = false;
        }
        use_alpha = true;
    }

    if (use_alpha) {
        cc_options |= SHADER_OPT(ALPHA);
    }
    if (use_fog) {
        cc_options |= SHADER_OPT(FOG);
    }
    if (texture_edge) {
        cc_options |= SHADER_OPT(TEXTURE_EDGE);
    }
    if (use_noise) {
        cc_options |= SHADER_OPT(NOISE);
    }
    if (use_2cyc) {
        cc_options |= SHADER_OPT(_2CYC);
    }
    if (alpha_threshold) {
        cc_options |= SHADER_OPT(ALPHA_THRESHOLD);
    }
    if (invisible) {
        cc_options |= SHADER_OPT(INVISIBLE);
    }
    if (use_grayscale) {
        cc_options |= SHADER_OPT(GRAYSCALE);
    }
    if (use_prim_depth) {
        cc_options |= SHADER_OPT(PRIM_DEPTH);
    }

    if (!mShaderStack.empty()) {
        cc_options |= (mShaderStack.top() << SHADER_ID_SHIFT);
    } else {
        cc_options |= -1 << SHADER_ID_SHIFT;
    }

#ifndef LUS_XBOX // (masks applied as textures are imported on the Xbox: no programs of their own)
    if (mRdp->loaded_texture[0].masked) {
        cc_options |= SHADER_OPT(TEXEL0_MASK);
    }
    if (mRdp->loaded_texture[1].masked) {
        cc_options |= SHADER_OPT(TEXEL1_MASK);
    }
    if (mRdp->loaded_texture[0].blended) {
        cc_options |= SHADER_OPT(TEXEL0_BLEND);
    }
    if (mRdp->loaded_texture[1].blended) {
        cc_options |= SHADER_OPT(TEXEL1_BLEND);
    }
#endif

    ColorCombinerKey key{}; // (all of it: the map compares shader_id too, and stack garbage made every key new)
    key.combine_mode = mRdp->combine_mode;
    key.options = cc_options;

    ColorCombiner* comb = LookupOrCreateColorCombiner(key);

    uint32_t tm = 0;
    uint32_t tex_width[2], tex_height[2], tex_width2[2], tex_height2[2];
    uint32_t effective_tile[2];

    for (int i = 0; i < 2; i++) {
        uint32_t tile = mRdp->first_tile_index + i;

        // No LOD support: force both slots to the base mip level.
        if (i == 1 && mRdp->first_tile_index >= 2) {
            tile = mRdp->first_tile_index;
        }
        effective_tile[i] = tile;

        if (comb->usedTextures[i]) {
            if (mRdp->textures_changed[i]) {
#ifdef LUS_XBOX
                // (TextureCacheLookup flushes, when the bound texture changes)
                uint64_t xboxImport = XBOX_CYCLES();
                ImportTexture(i, tile, false);
                sXboxImportCycles += XBOX_CYCLES() - xboxImport;
#else
                Flush();
                ImportTexture(i, tile, false);
#endif
#ifndef LUS_XBOX // (the mask and its replacement: applied in ImportTexture on the Xbox)
                if (mRdp->loaded_texture[i].masked) {
                    ImportTextureMask(SHADER_FIRST_MASK_TEXTURE + i, tile);
                }
                if (mRdp->loaded_texture[i].blended) {
                    ImportTexture(SHADER_FIRST_REPLACEMENT_TEXTURE + i, tile, true);
                }
#endif
                mRdp->textures_changed[i] = false;
            }

            uint8_t cms = mRdp->texture_tile[tile].cms;
            uint8_t cmt = mRdp->texture_tile[tile].cmt;

            uint32_t loaded_line_size = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].line_size_bytes;
            uint32_t loaded_size = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].size_bytes;
            uint32_t loaded_full_line =
                mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].full_image_line_size_bytes;
            uint32_t tex_size_bytes;
            uint32_t line_size;
            const bool usesLoadedSizes =
                (loaded_line_size != loaded_size || loaded_full_line != loaded_size) && loaded_line_size > 0;
            if (usesLoadedSizes) {
                line_size = loaded_line_size;
                tex_size_bytes = loaded_size;
            } else {
                line_size = mRdp->texture_tile[tile].line_size_bytes;
                tex_size_bytes = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].orig_size_bytes;
                // RGBA32: texture_tile stores TMEM-interleaved stride (half of actual DRAM stride).
                if (mRdp->texture_tile[tile].siz == G_IM_SIZ_32b) {
                    line_size *= 2;
                }
            }

            if (line_size == 0) {
                line_size = 1;
            }

            tex_height[i] = tex_size_bytes / line_size;
            switch (mRdp->texture_tile[tile].siz) {
                case G_IM_SIZ_4b:
                    line_size <<= 1;
                    break;
                case G_IM_SIZ_8b:
                    break;
                case G_IM_SIZ_16b:
                    line_size /= G_IM_SIZ_16b_LINE_BYTES;
                    break;
                case G_IM_SIZ_32b:
                    line_size /= 4; // RGBA32: 4 bytes per pixel (line_size is now actual DRAM stride)
                    break;
            }
            tex_width[i] = line_size;

            tex_width2[i] = GetTileSizeFromCoordinates(mRdp->texture_tile[tile].uls, mRdp->texture_tile[tile].lrs);
            tex_height2[i] = GetTileSizeFromCoordinates(mRdp->texture_tile[tile].ult, mRdp->texture_tile[tile].lrt);

            // Same pyramid-like ratio gate as ImportTexture: only clamp when loaded pixels
            // are close to rendered pixels (mipmap), not when much bigger (window scroll).
            const RawTexMetadata& triMeta = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].raw_tex_metadata;
            // loaded_texture sizes are scaled for HD replacements; the tile state and UVs are N64 texels
            if (usesLoadedSizes) {
                const float hbs = triMeta.h_byte_scale > 0.0f ? triMeta.h_byte_scale : 1.0f;
                const float vps = triMeta.v_pixel_scale > 0.0f ? triMeta.v_pixel_scale : 1.0f;
                tex_width[i] = (uint32_t)lroundf(tex_width[i] / hbs);
                tex_height[i] = (uint32_t)lroundf(tex_height[i] / vps);
            }
            bool pyrLike = IsPyramidLike(tex_width[i], tex_height[i], tex_width2[i], tex_height2[i], 1.0f, 1.0f);
            // Same wrap-period trim as the import paths. The >= tex_width2 guard skips a stale
            // mask left by an FB blit (the pause background), which would otherwise tile the FB.
            uint32_t maskW = mRdp->texture_tile[tile].masks;
            uint32_t maskH = mRdp->texture_tile[tile].maskt;
            if (maskW != 0 && (1u << maskW) >= tex_width2[i] && (1u << maskW) < tex_width[i]) {
                tex_width[i] = 1u << maskW;
            }
            if (maskH != 0 && (1u << maskH) >= tex_height2[i] && (1u << maskH) < tex_height[i]) {
                tex_height[i] = 1u << maskH;
            }
            if ((pyrLike || (cms & G_TX_CLAMP)) && tex_width2[i] > 0 && tex_width2[i] < tex_width[i]) {
                tex_width[i] = tex_width2[i];
            }
            if ((pyrLike || (cmt & G_TX_CLAMP)) && tex_height2[i] > 0 && tex_height2[i] < tex_height[i]) {
                tex_height[i] = tex_height2[i];
            }

#ifdef LUS_XBOX
            // Mirror and clamp on a tile no bigger than the mirror's period: the clamp stops the coordinate before the
            // mirror could turn it round, so it's a plain clamp, which the texture stage does by itself. Taken as a
            // mirror, nearly half the triangles of a character's model (the data says, tile by tile) were cut at the
            // tile's edge on the CPU, each tick: 18-26 ms at 733 MHz in the Goron Race and Great Bay.
            if ((cms & G_TX_CLAMP) && (cms & G_TX_MIRROR) && (maskW == 0 || tex_width2[i] <= (1u << maskW))) {
                cms &= ~G_TX_MIRROR;
            }
            if ((cmt & G_TX_CLAMP) && (cmt & G_TX_MIRROR) && (maskH == 0 || tex_height2[i] <= (1u << maskH))) {
                cmt &= ~G_TX_MIRROR;
            }
#endif
            uint32_t tex_width1 = tex_width[i] << (cms & G_TX_MIRROR);
            uint32_t tex_height1 = tex_height[i] << (cmt & G_TX_MIRROR);

            if ((cms & G_TX_CLAMP) && ((cms & G_TX_MIRROR) || tex_width1 != tex_width2[i])) {
                tm |= 1 << 2 * i;
                cms &= ~G_TX_CLAMP;
            }
            if ((cmt & G_TX_CLAMP) && ((cmt & G_TX_MIRROR) || tex_height1 != tex_height2[i])) {
                tm |= 1 << 2 * i + 1;
                cmt &= ~G_TX_CLAMP;
            }

            if (mRenderingState.mTextures[i] == nullptr) {
                continue;
            }

            bool linear_filter = (mRdp->other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT;
            if (linear_filter != mRenderingState.mTextures[i]->second.linear_filter ||
                cms != mRenderingState.mTextures[i]->second.cms || cmt != mRenderingState.mTextures[i]->second.cmt) {
                Flush();

#ifndef LUS_XBOX
                // Set the same sampler params on the blended texture. Needed for opengl.
                if (mRdp->loaded_texture[i].blended) {
                    mRapi->SetSamplerParameters(SHADER_FIRST_REPLACEMENT_TEXTURE + i, linear_filter, cms, cmt);
                }
#endif

                mRapi->SetSamplerParameters(i, linear_filter, cms, cmt);
                mRenderingState.mTextures[i]->second.linear_filter = linear_filter;
                mRenderingState.mTextures[i]->second.cms = cms;
                mRenderingState.mTextures[i]->second.cmt = cmt;
            }
        }
    }

    struct ShaderProgram* prg = comb->prg[tm];
    if (prg == NULL) {
        comb->prg[tm] = prg =
            LookupOrCreateShaderProgram(comb->shader_id0, comb->shader_id1 | tm * SHADER_OPT(TEXEL0_CLAMP_S));
    }
    if (prg != mRenderingState.mShaderProgram) {
        Flush();
        mRapi->UnloadShader(mRenderingState.mShaderProgram);
        mRapi->LoadShader(prg);
        mRenderingState.mShaderProgram = prg;
    }
    if (use_alpha != mRenderingState.alpha_blend) {
        Flush();
        mRapi->SetUseAlpha(use_alpha);
        mRenderingState.alpha_blend = use_alpha;
    }
    uint8_t numInputs;
    bool usedTextures[2];

    mRapi->ShaderGetInfo(prg, &numInputs, usedTextures);

#ifdef LUS_XBOX
    if (hwVerts) {
        // a tile's clamp (tm), in the vertex's units: the range a coordinate stays in where the GPU's wrap or
        // mirror reads the same texels (most of a scene's clamped triangles), and the CPU path's bounds (half
        // a texel in) the others are cut at. They depend on the state alone: the fast path above reuses them
        mHwClampAxes = 0;
        mHwClampSteps = 0;
        const float lin = ((mRdp->other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT) ? 0.5f : 0.0f;
        for (int t = 0; t < 2; t++) {
            if (!usedTextures[t] || !(tm & (3 << 2 * t))) {
                continue;
            }
            const auto& tile = mRdp->texture_tile[effective_tile[t]];
            float fs = tile.shifts == 0 ? 1.0f : tile.shifts <= 10 ? 1.0f / (1 << tile.shifts) : (float)(1 << (16 - tile.shifts));
            float ft = tile.shiftt == 0 ? 1.0f : tile.shiftt <= 10 ? 1.0f / (1 << tile.shiftt) : (float)(1 << (16 - tile.shiftt));
            if (tm & (1 << 2 * t)) {
                float k = 32.0f / fs, o = tile.uls * 0.25f;
                mHwClampAxis[mHwClampAxes++] = { false, o * k, (tex_width2[t] + o) * k };
                mHwClampStep[mHwClampSteps++] = { false, true, (tex_width2[t] - 0.5f - lin + o) * k };
                mHwClampStep[mHwClampSteps++] = { false, false, (0.5f - lin + o) * k };
            }
            if (tm & (2 << 2 * t)) {
                float k = 32.0f / ft, o = tile.ult * 0.25f;
                mHwClampAxis[mHwClampAxes++] = { true, o * k, (tex_height2[t] + o) * k };
                mHwClampStep[mHwClampSteps++] = { true, true, (tex_height2[t] - 0.5f - lin + o) * k };
                mHwClampStep[mHwClampSteps++] = { true, false, (0.5f - lin + o) * k };
            }
        }
        // the GPU's, unless the triangle needs what only the CPU path makes: grayscale, the blend colour's
        // fog, a rectangle. One that reaches the tile's bounds is cut there first, so it stays on the GPU's
        // path and a frame between ticks moves it with the rest: on the CPU's path it stood still while the
        // tick lasted (floors shook, bodies came apart)
        const bool hwShape = !use_grayscale && !use_blend_color && !is_rect;
        const bool reached = HwClampReached(v_arr) || HwSpanRisky(v_arr);
        if (hwShape && !reached &&
            HwTriangle(v_arr, hwCull, comb, use_alpha, use_fog, usedTextures, numInputs, effective_tile, tex_width,
                       tex_height, is_rect)) {
            mHwFast = true;
            return;
        }
        if (hwShape && reached) {
            HwTriArgs a = { hwCull, comb, use_alpha, use_fog, { usedTextures[0], usedTextures[1] }, numInputs,
                            { effective_tile[0], effective_tile[1] }, { tex_width[0], tex_width[1] },
                            { tex_height[0], tex_height[1] } };
            if (HwClampTriangle(v_arr, &a)) {
                mHwFast = true;
                return;
            }
            sXboxClampWhy[1]++;
        }
        // why not the GPU's (for the profile): the first reason that held
        sXboxCpuWhy[tm != 0 ? 0 : use_blend_color ? 1 : use_grayscale ? 2 : is_rect ? 3 : 4]++;
        EnsureTransformed(v1);
        EnsureTransformed(v2);
        EnsureTransformed(v3);
        if (cpuReject()) {
            sXboxCpuCulled++;
            return;
        }
        EnsureLit(v1);
        EnsureLit(v2);
        EnsureLit(v3);
    }
    if (mHwTris > 0) {
        Flush(); // the GPU's batch first: the order
    }
    mCpuTriangles++;
#endif

    struct GfxClipParameters clip_parameters = mRapi->GetClipParameters();

    for (int i = 0; i < 3; i++) {
        float z = v_arr[i]->z, w = v_arr[i]->w;
        if (clip_parameters.z_is_from_0_to_1) {
            z = (z + w) / 2.0f;
        }

        mBufVbo[mBufVboLen++] = v_arr[i]->x;
        mBufVbo[mBufVboLen++] = clip_parameters.invertY ? -v_arr[i]->y : v_arr[i]->y;
        mBufVbo[mBufVboLen++] = z;
        mBufVbo[mBufVboLen++] = w;

        for (int t = 0; t < 2; t++) {
            if (!usedTextures[t]) {
                continue;
            }
            float u = v_arr[i]->u / 32.0f;
            float v = v_arr[i]->v / 32.0f;

            uint32_t uv_tile = effective_tile[t];
            int shifts = mRdp->texture_tile[uv_tile].shifts;
            int shiftt = mRdp->texture_tile[uv_tile].shiftt;
            if (shifts != 0) {
                if (shifts <= 10) {
                    u /= 1 << shifts;
                } else {
                    u *= 1 << (16 - shifts);
                }
            }
            if (shiftt != 0) {
                if (shiftt <= 10) {
                    v /= 1 << shiftt;
                } else {
                    v *= 1 << (16 - shiftt);
                }
            }

            u -= mRdp->texture_tile[uv_tile].uls / 4.0f;
            v -= mRdp->texture_tile[uv_tile].ult / 4.0f;

            if ((mRdp->other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT) {
                // Linear filter adds 0.5f to the coordinates
                if (!is_rect) {
                    u += 0.5f;
                    v += 0.5f;
                }
            }

            mBufVbo[mBufVboLen++] = u / tex_width[t];
            mBufVbo[mBufVboLen++] = v / tex_height[t];

            bool clampS = tm & (1 << 2 * t);
            bool clampT = tm & (1 << 2 * t + 1);

            if (clampS) {
                mBufVbo[mBufVboLen++] = (tex_width2[t] - 0.5f) / tex_width[t];
            }

            if (clampT) {
                mBufVbo[mBufVboLen++] = (tex_height2[t] - 0.5f) / tex_height[t];
            }
        }

        if (use_fog) {
            if (use_blend_color) {
                // Shroud/blend mode: blend toward blend_color using fog alpha as factor
                mBufVbo[mBufVboLen++] = mRdp->blend_color.r / 255.0f;
                mBufVbo[mBufVboLen++] = mRdp->blend_color.g / 255.0f;
                mBufVbo[mBufVboLen++] = mRdp->blend_color.b / 255.0f;
                mBufVbo[mBufVboLen++] = mRdp->fog_color.a / 255.0f;
            } else {
                mBufVbo[mBufVboLen++] = mRdp->fog_color.r / 255.0f;
                mBufVbo[mBufVboLen++] = mRdp->fog_color.g / 255.0f;
                mBufVbo[mBufVboLen++] = mRdp->fog_color.b / 255.0f;
                mBufVbo[mBufVboLen++] = v_arr[i]->color.a / 255.0f; // fog factor (not alpha)
            }
        }

        if (use_grayscale) {
            mBufVbo[mBufVboLen++] = mRdp->grayscale_color.r / 255.0f;
            mBufVbo[mBufVboLen++] = mRdp->grayscale_color.g / 255.0f;
            mBufVbo[mBufVboLen++] = mRdp->grayscale_color.b / 255.0f;
            mBufVbo[mBufVboLen++] = mRdp->grayscale_color.a / 255.0f; // lerp interpolation factor (not alpha)
        }

        for (int j = 0; j < numInputs; j++) {
            RGBA* color;
            RGBA tmp;
            for (int k = 0; k < 1 + (use_alpha ? 1 : 0); k++) {
                switch (comb->shader_input_mapping[k][j]) {
                        // Note: CCMUX constants and ACMUX constants used here have same value, which is why this works
                        // (except LOD fraction).
                    case G_CCMUX_PRIMITIVE:
                        color = &mRdp->prim_color;
                        break;
                    case G_CCMUX_SHADE:
                        color = &v_arr[i]->color;
                        break;
                    case G_CCMUX_ENVIRONMENT:
                        color = &mRdp->env_color;
                        break;
                    case G_CCMUX_PRIMITIVE_ALPHA: {
                        tmp.r = tmp.g = tmp.b = mRdp->prim_color.a;
                        color = &tmp;
                        break;
                    }
                    case G_CCMUX_ENV_ALPHA: {
                        tmp.r = tmp.g = tmp.b = mRdp->env_color.a;
                        color = &tmp;
                        break;
                    }
                    case G_CCMUX_PRIM_LOD_FRAC: {
                        tmp.r = tmp.g = tmp.b = mRdp->prim_lod_fraction;
                        color = &tmp;
                        break;
                    }
                    case G_CCMUX_LOD_FRACTION: {
                        if (mRdp->other_mode_l & G_TL_LOD) {
                            // "Hack" that works for Bowser - Peach painting
                            float distance_frac = (v1->w - 3000.0f) / 3000.0f;
                            if (distance_frac < 0.0f) {
                                distance_frac = 0.0f;
                            }
                            if (distance_frac > 1.0f) {
                                distance_frac = 1.0f;
                            }
                            tmp.r = tmp.g = tmp.b = tmp.a = distance_frac * 255.0f;
                        } else {
                            tmp.r = tmp.g = tmp.b = tmp.a = 255.0f;
                        }
                        color = &tmp;
                        break;
                    }
                    case G_CCMUX_KEY_CENTER:
                        color = &mRdp->key_center;
                        break;
                    case G_CCMUX_KEY_SCALE:
                        color = &mRdp->key_scale;
                        break;
                    case G_CCMUX_CONVERT_K4: {
                        tmp.r = tmp.g = tmp.b = mRdp->convert_k[4];
                        color = &tmp;
                        break;
                    }
                    case G_CCMUX_CONVERT_K5: {
                        tmp.r = tmp.g = tmp.b = mRdp->convert_k[5];
                        color = &tmp;
                        break;
                    }
                    case G_ACMUX_PRIM_LOD_FRAC:
                        tmp.a = mRdp->prim_lod_fraction;
                        color = &tmp;
                        break;
                    default:
                        memset(&tmp, 0, sizeof(tmp));
                        color = &tmp;
                        break;
                }
                if (k == 0) {
                    mBufVbo[mBufVboLen++] = color->r / 255.0f;
                    mBufVbo[mBufVboLen++] = color->g / 255.0f;
                    mBufVbo[mBufVboLen++] = color->b / 255.0f;
                } else {
                    if (use_fog && !use_blend_color && color == &v_arr[i]->color) {
                        // Shade alpha is 100% for standard fog, blend color mode preserves
                        // it since fog alpha is the blend factor
                        mBufVbo[mBufVboLen++] = 1.0f;
                    } else {
                        mBufVbo[mBufVboLen++] = color->a / 255.0f;
                    }
                }
            }
        }

        // struct RGBA *color = &v_arr[i]->color;
        // mBufVbo[mBufVboLen++] = color->r / 255.0f;
        // mBufVbo[mBufVboLen++] = color->g / 255.0f;
        // mBufVbo[mBufVboLen++] = color->b / 255.0f;
        // mBufVbo[mBufVboLen++] = color->a / 255.0f;
    }

    if (++mBufVboNumTris == MAX_TRI_BUFFER) {
        // if (++mBufVbo_num_tris == 1) {
        Flush();
    }
}

void Interpreter::GfxSpGeometryMode(uint32_t clear, uint32_t set) {
#ifdef LUS_XBOX
    uint32_t was = mRsp->geometry_mode;
    mRsp->geometry_mode = (was & ~clear) | set;
    if (mRsp->geometry_mode != was) {
        mHwStateGen++;
        mHwVtxVersion++;
    }
#else
    mRsp->geometry_mode &= ~clear;
    mRsp->geometry_mode |= set;
#endif
}

void Interpreter::GfxSpExtraGeometryMode(uint32_t clear, uint32_t set) {
    mRsp->extra_geometry_mode &= ~clear;
    mRsp->extra_geometry_mode |= set;
#ifdef LUS_XBOX
    mHwStateGen++;
    mHwVtxVersion++;
#endif
}

void Interpreter::AdjustVIewportOrScissor(XYWidthHeight* area) {
    if (!mFbActive) {
        // Adjust the y origin based on the y-inversion for the active framebuffer
        GfxClipParameters clipParameters = mRapi->GetClipParameters();
        if (clipParameters.invertY) {
            area->y -= area->height;
        } else {
            area->y = mNativeDimensions.height - area->y;
        }

        area->width *= RATIO_X(mActiveFrameBuffer, mCurDimensions);
        area->height *= RATIO_Y(mActiveFrameBuffer, mCurDimensions);
        area->x *= RATIO_X(mActiveFrameBuffer, mCurDimensions);
        area->y *= RATIO_Y(mActiveFrameBuffer, mCurDimensions);

        if (!mRendersToFb || (mMsaaLevel > 1 && mCurDimensions.width == mGameWindowViewport.width &&
                              mCurDimensions.height == mGameWindowViewport.height)) {
            area->x += mGameWindowViewport.x;
            area->y += mGfxCurrentWindowDimensions.height - (mGameWindowViewport.y + mGameWindowViewport.height);
        }
    } else {
        area->y = mActiveFrameBuffer->second.orig_height - area->y;

        if (mActiveFrameBuffer->second.resize) {
            area->width *= RATIO_X(mActiveFrameBuffer, mCurDimensions);
            area->height *= RATIO_Y(mActiveFrameBuffer, mCurDimensions);
            area->x *= RATIO_X(mActiveFrameBuffer, mCurDimensions);
            area->y *= RATIO_Y(mActiveFrameBuffer, mCurDimensions);
        }
    }
}

void Interpreter::CalcAndSetViewport(const F3DVp_t* viewport) {
    // 2 bits fraction
    float width = 2.0f * viewport->vscale[0] / 4.0f;
    float height = 2.0f * viewport->vscale[1] / 4.0f;
    float x = (viewport->vtrans[0] / 4.0f) - width / 2.0f;
    float y = ((viewport->vtrans[1] / 4.0f) + height / 2.0f);

    mRdp->viewport.x = x;
    mRdp->viewport.y = y;
    mRdp->viewport.width = width;
    mRdp->viewport.height = height;

    AdjustVIewportOrScissor(&mRdp->viewport);

    mRdp->viewport_or_scissor_changed = true;
}

void Interpreter::GfxSpMovememF3dex2(uint8_t index, uint8_t offset, const void* data) {
#ifdef LUS_XBOX
    mHwVtxVersion++; // (lights, lookat)
#endif
    switch (index) {
        case F3DEX2_G_MV_VIEWPORT:
            CalcAndSetViewport((const F3DVp_t*)data);
            break;
        case F3DEX2_G_MV_LIGHT: {
            int lightidx = offset / 24 - 2;
            if (lightidx >= 0 && lightidx <= MAX_LIGHTS) { // skip lookat
                // NOTE: reads out of bounds if it is an ambient light
                memcpy(mRsp->current_lights + lightidx, data, sizeof(F3DLight));
            } else if (lightidx < 0) {
                memcpy(mRsp->lookat + offset / 24, data, sizeof(F3DLight_t)); // TODO Light?
            }
            break;
        }
    }
}

void Interpreter::GfxSpMovememF3d(uint8_t index, uint8_t offset, const void* data) {
#ifdef LUS_XBOX
    mHwVtxVersion++;
#endif
    switch (index) {
        case F3DEX_G_MV_VIEWPORT:
            CalcAndSetViewport((const F3DVp_t*)data);
            break;
        case F3DEX_G_MV_LOOKATX:
            memcpy(mRsp->lookat + 0, data, sizeof(F3DLight_t));
            break;
        case F3DEX_G_MV_LOOKATY:
            memcpy(mRsp->lookat + 1, data, sizeof(F3DLight_t));
            break;
        case F3DEX_G_MV_L0:
        case F3DEX_G_MV_L1:
        case F3DEX_G_MV_L2:
        case F3DEX_G_MV_L3:
        case F3DEX_G_MV_L4:
        case F3DEX_G_MV_L5:
        case F3DEX_G_MV_L6:
        case F3DEX_G_MV_L7:
            // NOTE: reads out of bounds if it is an ambient light
            memcpy(mRsp->current_lights + (index - F3DEX_G_MV_L0) / 2, data, sizeof(F3DLight_t));
            break;
    }
}

void Interpreter::GfxSpMovewordF3dex2(uint8_t index, uint16_t offset, uintptr_t data) {
#ifdef LUS_XBOX
    mHwVtxVersion++; // (fog, the number of lights)
#endif
    switch (index) {
        case G_MW_NUMLIGHT:
            mRsp->current_num_lights = data / 24 + 1; // add ambient light
            mRsp->lights_changed = true;
            break;
        case G_MW_FOG:
            mRsp->fog_mul = (int16_t)(data >> 16);
            mRsp->fog_offset = (int16_t)data;
            break;
        case G_MW_SEGMENT: {
            int segNumber = offset / 4;
            mSegmentPointers[segNumber] = data;
        } break;
        case G_MW_SEGMENT_INTERP: {
            int segNumber = offset % 16;
            int segIndex = offset / 16;

            if (segIndex == mInterpolationIndex)
                mSegmentPointers[segNumber] = data;
        } break;
    }
}

void Interpreter::GfxSpMovewordF3d(uint8_t index, uint16_t offset, uintptr_t data) {
#ifdef LUS_XBOX
    mHwVtxVersion++;
#endif
    switch (index) {
        case G_MW_NUMLIGHT:
            // Ambient light is included
            // The 31st bit is a flag that lights should be recalculated
            mRsp->current_num_lights = (data - 0x80000000U) / 32;
            mRsp->lights_changed = true;
            break;
        case G_MW_FOG:
            mRsp->fog_mul = (int16_t)(data >> 16);
            mRsp->fog_offset = (int16_t)data;
            break;
        case G_MW_SEGMENT: {
            int segNumber = offset / 4;
            mSegmentPointers[segNumber] = data;
        } break;
        case G_MW_SEGMENT_INTERP: {
            int segNumber = offset % 16;
            int segIndex = offset / 16;

            if (segIndex == mInterpolationIndex)
                mSegmentPointers[segNumber] = data;
        } break;
    }
}

void Interpreter::GfxSpTexture(uint16_t sc, uint16_t tc, uint8_t level, uint8_t tile, uint8_t on) {
#ifdef LUS_XBOX
    if (mRdp->first_tile_index != tile) {
        mHwStateGen++;
    }
#endif
    mRsp->texture_scaling_factor.s = sc;
    mRsp->texture_scaling_factor.t = tc;
    if (mRdp->first_tile_index != tile) {
        mRdp->textures_changed[0] = true;
        mRdp->textures_changed[1] = true;
    }

    mRdp->first_tile_index = tile;
}

void Interpreter::GfxDpSetScissor(uint32_t mode, uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    float x = ulx / 4.0f;
    float y = lry / 4.0f;
    float width = (lrx - ulx) / 4.0f;
    float height = (lry - uly) / 4.0f;

    mRdp->scissor.x = x;
    mRdp->scissor.y = y;
    mRdp->scissor.width = width;
    mRdp->scissor.height = height;

    AdjustVIewportOrScissor(&mRdp->scissor);

    mRdp->viewport_or_scissor_changed = true;
}

void Interpreter::GfxDpSetTextureImage(uint32_t format, uint32_t size, uint32_t width, const char* texPath,
                                       uint32_t texFlags, RawTexMetadata rawTexMetdata, const void* addr) {
    // fprintf(stderr, "GfxDpSetTextureImage: %s (width=%d; size=0x%X)\n",
    //         rawTexMetdata.resource ? rawTexMetdata.resource->GetInitData()->Path.c_str() : nullptr, width, size);
    mRdp->texture_to_load.addr = (const uint8_t*)addr;
    mRdp->texture_to_load.siz = size;
    mRdp->texture_to_load.width = width;
    mRdp->texture_to_load.tex_flags = texFlags;
    mRdp->texture_to_load.raw_tex_metadata = rawTexMetdata;
}

#ifdef LUS_XBOX
// the same from a texture in hand, its one shared_ptr copy the one kept
void Interpreter::GfxDpSetTextureImage(uint32_t size, uint32_t width, const std::shared_ptr<Fast::Texture>& texture) {
    auto& load = mRdp->texture_to_load;
    load.addr = (const uint8_t*)texture->ImageData;
    load.siz = size;
    load.width = width;
    load.tex_flags = texture->Flags;
    load.raw_tex_metadata.width = texture->Width;
    load.raw_tex_metadata.height = texture->Height;
    load.raw_tex_metadata.h_byte_scale = texture->HByteScale;
    load.raw_tex_metadata.v_pixel_scale = texture->VPixelScale;
    load.raw_tex_metadata.type = texture->Type;
    load.raw_tex_metadata.resource = texture;
}
#endif

void Interpreter::GfxDpSetTile(uint8_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint8_t tile, uint32_t palette,
                               uint32_t cmt, uint32_t maskt, uint32_t shiftt, uint32_t cms, uint32_t masks,
                               uint32_t shifts) {
    // OTRTODO:
    // SUPPORT_CHECK(tmem == 0 || tmem == 256);

    if (cms == G_TX_WRAP && masks == G_TX_NOMASK) {
        cms = G_TX_CLAMP;
    }
    if (cmt == G_TX_WRAP && maskt == G_TX_NOMASK) {
        cmt = G_TX_CLAMP;
    }

#ifdef LUS_XBOX
    // the same tile again (a material block re-issued): no change, no import, the GPU's batch goes on
    auto& t = mRdp->texture_tile[tile];
    if (t.palette == palette && t.fmt == fmt && t.siz == siz && t.cms == cms && t.cmt == cmt && t.masks == masks &&
        t.maskt == maskt && t.shifts == shifts && t.shiftt == shiftt && t.line_size_bytes == line * 8 &&
        t.tmem == tmem) {
        return;
    }
    mHwStateGen++;
#endif
    mRdp->texture_tile[tile].palette = palette; // palette should set upper 4 bits of color index in 4b mode
    mRdp->texture_tile[tile].fmt = fmt;
    mRdp->texture_tile[tile].siz = siz;
    mRdp->texture_tile[tile].cms = cms;
    mRdp->texture_tile[tile].cmt = cmt;
    mRdp->texture_tile[tile].masks = masks;
    mRdp->texture_tile[tile].maskt = maskt;
    mRdp->texture_tile[tile].shifts = shifts;
    mRdp->texture_tile[tile].shiftt = shiftt;
    mRdp->texture_tile[tile].line_size_bytes = line * 8;

    mRdp->texture_tile[tile].tmem = tmem;
    // mRdp->texture_tile[tile].tmem_index = tmem / 256; // tmem is the 64-bit word offset, so 256 words means 2 kB

    mRdp->texture_tile[tile].tmem_index =
        tmem != 0; // assume one texture is loaded at address 0 and another texture at any other address

    mRdp->textures_changed[0] = true;
    mRdp->textures_changed[1] = true;
}

void Interpreter::GfxDpSetTileSize(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
#ifdef LUS_XBOX
    auto& t = mRdp->texture_tile[tile];
    if (t.uls == uls && t.ult == ult && t.lrs == lrs && t.lrt == lrt) {
        return;
    }
    mHwStateGen++;
#endif
    mRdp->texture_tile[tile].uls = uls;
    mRdp->texture_tile[tile].ult = ult;
    mRdp->texture_tile[tile].lrs = lrs;
    mRdp->texture_tile[tile].lrt = lrt;
    mRdp->textures_changed[0] = true;
    mRdp->textures_changed[1] = true;
}

void Interpreter::GfxDpLoadTlut(uint8_t tile, uint32_t high_index) {
    SUPPORT_CHECK(mRdp->texture_to_load.siz == G_IM_SIZ_16b);

    uint16_t tmem = mRdp->texture_tile[tile].tmem;
    const uint8_t* src = mRdp->texture_to_load.addr;
    uint32_t entryCount = high_index + 1;
    uint32_t byteCount = entryCount * 2;

    // The bytes of the palette from `from` on, `len` of them, into dst.
    // On the Xbox: a palette's resource can be shorter than the load asks for (a 256-entry load of a smaller
    // palette). The N64 just read on into whatever followed, but here the bytes after it can be an unmapped page,
    // and the copy an access violation (warp test 250, Bazaar shop, on 64 MB). So only what the resource has is
    // copied; the rest of the entries are zero. A palette that isn't a resource (a raw address) is copied as asked.
    size_t avail = SIZE_MAX;
#ifdef LUS_XBOX
    {
        const auto& res = mRdp->texture_to_load.raw_tex_metadata.resource;
        if (res != nullptr && res->ImageData != nullptr) {
            const uint8_t* base = res->ImageData;
            if (src >= base && src <= base + res->ImageDataSize) {
                avail = (size_t)(base + res->ImageDataSize - src);
            }
        }
    }
#endif
    auto copyPalette = [&](uint8_t* dst, size_t from, size_t len) {
        const size_t n = from >= avail ? 0 : std::min(len, avail - from);
        memcpy(dst, src + from, n);
        if (n < len) {
            memset(dst + n, 0, len - n);
            static bool sReported;
            if (!sReported) {
                sReported = true;
                const auto& res = mRdp->texture_to_load.raw_tex_metadata.resource;
                SPDLOG_INFO("lus-x: a palette load asked for {} bytes of {}, which has {}: the rest zero", byteCount,
                            res->GetInitData()->Path, avail);
            }
        }
    };

    if (tmem >= 256) {
        // N64 TMEM palette area starts at tmem word 256. Each CI4 palette = 16 entries = 16 tmem words.
        uint32_t paletteByteOffset = (tmem - 256) * 2;

        if (high_index == 255 && paletteByteOffset == 0) {
            // CI8: full 256-entry palette spanning both halves
            copyPalette(mRdp->palette_staging[0], 0, 256);
            copyPalette(mRdp->palette_staging[1], 256, 256);
            mRdp->palettes[0] = mRdp->palette_staging[0];
            mRdp->palettes[1] = mRdp->palette_staging[1];
            mRdp->palette_dram_addr[0] = src;
            mRdp->palette_dram_addr[1] = src + 256;
        } else if (paletteByteOffset < 256) {
            // Palettes 0-7 range
            uint32_t copyLen = (paletteByteOffset + byteCount <= 256) ? byteCount : (256 - paletteByteOffset);
            copyPalette(mRdp->palette_staging[0] + paletteByteOffset, 0, copyLen);
            mRdp->palettes[0] = mRdp->palette_staging[0];
            mRdp->palette_dram_addr[0] = src;
        } else {
            // Palettes 8-15 range
            uint32_t offset = paletteByteOffset - 256;
            uint32_t copyLen = (offset + byteCount <= 256) ? byteCount : (256 - offset);
            copyPalette(mRdp->palette_staging[1] + offset, 0, copyLen);
            mRdp->palettes[1] = mRdp->palette_staging[1];
            mRdp->palette_dram_addr[1] = src;
        }
    } else {
        // tmem < 256: non-standard location, fall back to direct pointer
        mRdp->palettes[1] = src;
        mRdp->palette_dram_addr[1] = src;
    }
}

void Interpreter::GfxDpLoadBlock(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt) {
    SUPPORT_CHECK(uls == 0);
    SUPPORT_CHECK(ult == 0);

    // The lrs field rather seems to be number of pixels to load
    uint32_t word_size_shift = 0;
    switch (mRdp->texture_to_load.siz) {
        case G_IM_SIZ_4b:
            word_size_shift = -1;
            break;
        case G_IM_SIZ_8b:
            word_size_shift = 0;
            break;
        case G_IM_SIZ_16b:
            word_size_shift = 1;
            break;
        case G_IM_SIZ_32b:
            word_size_shift = 2;
            break;
    }
    uint32_t orig_size_bytes =
        word_size_shift > 0 ? (lrs + 1) << word_size_shift : (lrs + 1) >> (-(int64_t)word_size_shift);
    uint32_t size_bytes = orig_size_bytes;
    if (mRdp->texture_to_load.raw_tex_metadata.h_byte_scale != 1 ||
        mRdp->texture_to_load.raw_tex_metadata.v_pixel_scale != 1) {
        size_bytes *= mRdp->texture_to_load.raw_tex_metadata.h_byte_scale;
        size_bytes *= mRdp->texture_to_load.raw_tex_metadata.v_pixel_scale;
    }
    mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].orig_size_bytes = orig_size_bytes;
    mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].size_bytes = size_bytes;
    // Compute actual per-line DRAM stride from SetTextureImage width when available.
    // The standard gDPLoadTextureBlock macro sets width=1, but manually-built DL
    // commands may set the real pixel width.
    uint32_t actual_line_bytes = size_bytes;
    const RawTexMetadata& blkMeta = mRdp->texture_to_load.raw_tex_metadata;
    bool blkHd = blkMeta.h_byte_scale != 1 || blkMeta.v_pixel_scale != 1;
    if (mRdp->texture_to_load.width > 1 && blkHd && blkMeta.height > 0 && size_bytes % blkMeta.height == 0) {
        // HD-upscaled textures report a sentinel SetTextureImage width, so the per-line stride
        // derived from it is bogus. The resource's stored height is authoritative.
        actual_line_bytes = size_bytes / blkMeta.height;
    } else if (mRdp->texture_to_load.width > 1) {
        uint32_t candidate;
        switch (mRdp->texture_to_load.siz) {
            case G_IM_SIZ_4b:
                candidate = (mRdp->texture_to_load.width + 1) / 2;
                break;
            case G_IM_SIZ_8b:
                candidate = mRdp->texture_to_load.width;
                break;
            case G_IM_SIZ_16b:
                candidate = mRdp->texture_to_load.width * 2;
                break;
            case G_IM_SIZ_32b:
                candidate = mRdp->texture_to_load.width * 4;
                break;
            default:
                candidate = mRdp->texture_to_load.width;
                break;
        }
        if (candidate > 0 && candidate < size_bytes && size_bytes % candidate == 0) {
            actual_line_bytes = candidate;
        }
    }
    mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].line_size_bytes = actual_line_bytes;
    mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].full_image_line_size_bytes = actual_line_bytes;
    // assert(size_bytes <= 4096 && "bug: too big texture");
    mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].tex_flags = mRdp->texture_to_load.tex_flags;
    mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].raw_tex_metadata = mRdp->texture_to_load.raw_tex_metadata;
    mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].addr = mRdp->texture_to_load.addr;
    // fprintf(stderr, "GfxDpLoadBlock: line_size = 0x%x; orig = 0x%x; bpp=%d; lrs=%d\n", size_bytes,
    // orig_size_bytes,
    //         mRdp->texture_to_load.siz, lrs);

    const std::string_view texPath =
        mRdp->texture_to_load.raw_tex_metadata.resource != nullptr
            ? GetBaseTexturePath(mRdp->texture_to_load.raw_tex_metadata.resource->GetInitData()->Path)
            : std::string_view{};
    auto maskedTextureIter = mMaskedTextures.find(texPath);
    if (maskedTextureIter != mMaskedTextures.end()) {
        mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].masked = true;
        mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].blended =
            maskedTextureIter->second.replacementData != nullptr;
    } else {
        mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].masked = false;
        mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].blended = false;
    }

    mRdp->textures_changed[mRdp->texture_tile[tile].tmem_index] = true;
}

void Interpreter::GfxDpLoadTile(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    SUPPORT_CHECK(tile == G_TX_LOADTILE);

    uint32_t word_size_shift = 0;
    switch (mRdp->texture_to_load.siz) {
        case G_IM_SIZ_4b:
            word_size_shift = 0;
            break;
        case G_IM_SIZ_8b:
            word_size_shift = 0;
            break;
        case G_IM_SIZ_16b:
            word_size_shift = 1;
            break;
        case G_IM_SIZ_32b:
            word_size_shift = 2;
            break;
    }

    uint32_t offset_x = uls >> G_TEXTURE_IMAGE_FRAC;
    uint32_t offset_y = ult >> G_TEXTURE_IMAGE_FRAC;
    uint32_t tile_width = ((lrs - uls) >> G_TEXTURE_IMAGE_FRAC) + 1;
    uint32_t tile_height = ((lrt - ult) >> G_TEXTURE_IMAGE_FRAC) + 1;
    uint32_t full_image_width = mRdp->texture_to_load.width;

    uint32_t offset_x_in_bytes = offset_x << word_size_shift;
    uint32_t tile_line_size_bytes = tile_width << word_size_shift;
    uint32_t full_image_line_size_bytes = full_image_width << word_size_shift;

    uint32_t orig_size_bytes = tile_line_size_bytes * tile_height;
    uint32_t size_bytes = orig_size_bytes;
    uint32_t start_offset_bytes = full_image_line_size_bytes * offset_y + offset_x_in_bytes;

    float h_byte_scale = mRdp->texture_to_load.raw_tex_metadata.h_byte_scale;
    float v_pixel_scale = mRdp->texture_to_load.raw_tex_metadata.v_pixel_scale;

    if (h_byte_scale != 1 || v_pixel_scale != 1) {
        start_offset_bytes = h_byte_scale * (v_pixel_scale * offset_y * full_image_line_size_bytes + offset_x_in_bytes);
        size_bytes *= h_byte_scale * v_pixel_scale;
        full_image_line_size_bytes *= h_byte_scale;
        tile_line_size_bytes *= h_byte_scale;
    }

    mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].orig_size_bytes = orig_size_bytes;
    mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].size_bytes = size_bytes;
    mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].full_image_line_size_bytes = full_image_line_size_bytes;
    mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].line_size_bytes = tile_line_size_bytes;

    //    assert(size_bytes <= 4096 && "bug: too big texture");
    mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].tex_flags = mRdp->texture_to_load.tex_flags;
    mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].raw_tex_metadata = mRdp->texture_to_load.raw_tex_metadata;
    mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].addr = mRdp->texture_to_load.addr + start_offset_bytes;

    const std::string_view texPath =
        mRdp->texture_to_load.raw_tex_metadata.resource != nullptr
            ? GetBaseTexturePath(mRdp->texture_to_load.raw_tex_metadata.resource->GetInitData()->Path)
            : std::string_view{};
    auto maskedTextureIter = mMaskedTextures.find(texPath);
    if (maskedTextureIter != mMaskedTextures.end()) {
        mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].masked = true;
        mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].blended =
            maskedTextureIter->second.replacementData != nullptr;
    } else {
        mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].masked = false;
        mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index].blended = false;
    }

    mRdp->texture_tile[tile].uls = uls;
    mRdp->texture_tile[tile].ult = ult;
    mRdp->texture_tile[tile].lrs = lrs;
    mRdp->texture_tile[tile].lrt = lrt;

    mRdp->textures_changed[mRdp->texture_tile[tile].tmem_index] = true;
}

/*static uint8_t color_comb_component(uint32_t v) {
    switch (v) {
        case G_CCMUX_TEXEL0:
            return CC_TEXEL0;
        case G_CCMUX_TEXEL1:
            return CC_TEXEL1;
        case G_CCMUX_PRIMITIVE:
            return CC_PRIM;
        case G_CCMUX_SHADE:
            return CC_SHADE;
        case G_CCMUX_ENVIRONMENT:
            return CC_ENV;
        case G_CCMUX_TEXEL0_ALPHA:
            return CC_TEXEL0A;
        case G_CCMUX_LOD_FRACTION:
            return CC_LOD;
        default:
            return CC_0;
    }
}

static inline uint32_t color_comb(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return color_comb_component(a) |
           (color_comb_component(b) << 3) |
           (color_comb_component(c) << 6) |
           (color_comb_component(d) << 9);
}

static void GfxDpSetCombineMode(uint32_t rgb, uint32_t alpha) {
    mRdp->combine_mode = rgb | (alpha << 12);
}*/

void Interpreter::GfxDpSetCombineMode(uint32_t rgb, uint32_t alpha, uint32_t rgb_cyc2, uint32_t alpha_cyc2) {
    uint64_t mode = rgb | (alpha << 16) | ((uint64_t)rgb_cyc2 << 28) | ((uint64_t)alpha_cyc2 << 44);
#ifdef LUS_XBOX
    if (mode != mRdp->combine_mode) {
        mHwStateGen++;
    }
#endif
    mRdp->combine_mode = mode;
}

static inline uint32_t color_comb(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (a & 0xf) | ((b & 0xf) << 4) | ((c & 0x1f) << 8) | ((d & 7) << 13);
}

static inline uint32_t alpha_comb(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (a & 7) | ((b & 7) << 3) | ((c & 7) << 6) | ((d & 7) << 9);
}

// Sign-extend a 9-bit value (used for G_SETCONVERT K0..K5).
static inline int16_t sign_extend_9(uint32_t v) {
    return (int16_t)((v & 0x100) ? (int32_t)(v | 0xFFFFFE00u) : (int32_t)v);
}

void Interpreter::GfxDpSetGrayscaleColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    mRdp->grayscale_color.r = r;
    mRdp->grayscale_color.g = g;
    mRdp->grayscale_color.b = b;
    mRdp->grayscale_color.a = a;
}

void Interpreter::GfxDpSetEnvColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
#ifdef LUS_XBOX
    if (mRdp->env_color.r == r && mRdp->env_color.g == g && mRdp->env_color.b == b && mRdp->env_color.a == a) {
        return;
    }
    mHwStateGen++;
#endif
    mRdp->env_color.r = r;
    mRdp->env_color.g = g;
    mRdp->env_color.b = b;
    mRdp->env_color.a = a;
}

void Interpreter::GfxDpSetPrimColor(uint8_t m, uint8_t l, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
#ifdef LUS_XBOX
    if (mRdp->prim_lod_fraction == l && mRdp->prim_color.r == r && mRdp->prim_color.g == g &&
        mRdp->prim_color.b == b && mRdp->prim_color.a == a) {
        return;
    }
    mHwStateGen++;
#endif
    mRdp->prim_lod_fraction = l;
    mRdp->prim_color.r = r;
    mRdp->prim_color.g = g;
    mRdp->prim_color.b = b;
    mRdp->prim_color.a = a;
}

void Interpreter::GfxDpSetFogColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
#ifdef LUS_XBOX
    if (mRdp->fog_color.r == r && mRdp->fog_color.g == g && mRdp->fog_color.b == b && mRdp->fog_color.a == a) {
        return;
    }
    mHwStateGen++;
#endif
    mRdp->fog_color.r = r;
    mRdp->fog_color.g = g;
    mRdp->fog_color.b = b;
    mRdp->fog_color.a = a;
}

void Interpreter::GfxDpSetBlendColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    mRdp->blend_color.r = r;
    mRdp->blend_color.g = g;
    mRdp->blend_color.b = b;
    mRdp->blend_color.a = a;
}

void Interpreter::GfxDpSetFillColor(uint32_t packed_color) {
    uint16_t col16 = (uint16_t)packed_color;
    uint32_t r = col16 >> 11;
    uint32_t g = (col16 >> 6) & 0x1f;
    uint32_t b = (col16 >> 1) & 0x1f;
    uint32_t a = col16 & 1;
    mRdp->fill_color.r = SCALE_5_8(r);
    mRdp->fill_color.g = SCALE_5_8(g);
    mRdp->fill_color.b = SCALE_5_8(b);
    mRdp->fill_color.a = a * 255;
}

void Interpreter::GfxDrawRectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    uint32_t saved_other_mode_h = mRdp->other_mode_h;
    uint32_t cycle_type = (mRdp->other_mode_h & (3U << G_MDSFT_CYCLETYPE));

    if (cycle_type == G_CYC_COPY) {
        mRdp->other_mode_h = (mRdp->other_mode_h & ~(3U << G_MDSFT_TEXTFILT)) | G_TF_POINT;
    }

    // U10.2 coordinates
    float ulxf = ulx;
    float ulyf = uly;
    float lrxf = lrx;
    float lryf = lry;

    ulxf = ulxf / (4.0f * HALF_SCREEN_WIDTH(mActiveFrameBuffer)) - 1.0f;
    ulyf = -(ulyf / (4.0f * HALF_SCREEN_HEIGHT(mActiveFrameBuffer))) + 1.0f;
    lrxf = lrxf / (4.0f * HALF_SCREEN_WIDTH(mActiveFrameBuffer)) - 1.0f;
    lryf = -(lryf / (4.0f * HALF_SCREEN_HEIGHT(mActiveFrameBuffer))) + 1.0f;

    ulxf = AdjXForAspectRatio(ulxf);
    lrxf = AdjXForAspectRatio(lrxf);

    struct LoadedVertex* ul = &mRsp->loaded_vertices[MAX_VERTICES + 0];
    struct LoadedVertex* ll = &mRsp->loaded_vertices[MAX_VERTICES + 1];
    struct LoadedVertex* lr = &mRsp->loaded_vertices[MAX_VERTICES + 2];
    struct LoadedVertex* ur = &mRsp->loaded_vertices[MAX_VERTICES + 3];

    ul->x = ulxf;
    ul->y = ulyf;
    ul->z = -1.0f;
    ul->w = 1.0f;

    ll->x = ulxf;
    ll->y = lryf;
    ll->z = -1.0f;
    ll->w = 1.0f;

    lr->x = lrxf;
    lr->y = lryf;
    lr->z = -1.0f;
    lr->w = 1.0f;

    ur->x = lrxf;
    ur->y = ulyf;
    ur->z = -1.0f;
    ur->w = 1.0f;

    // The coordinates for texture rectangle shall bypass the viewport setting
    struct XYWidthHeight default_viewport;
    if (!mFbActive) {
        default_viewport = { 0, (int16_t)mNativeDimensions.height, mNativeDimensions.width, mNativeDimensions.height };
    } else {
        default_viewport = { 0, (int16_t)mActiveFrameBuffer->second.orig_height, mActiveFrameBuffer->second.orig_width,
                             mActiveFrameBuffer->second.orig_height };
    }

    struct XYWidthHeight viewport_saved = mRdp->viewport;
    uint32_t geometry_mode_saved = mRsp->geometry_mode;

    AdjustVIewportOrScissor(&default_viewport);

    mRdp->viewport = default_viewport;
    mRdp->viewport_or_scissor_changed = true;
    mRsp->geometry_mode = 0;

    GfxSpTri1(MAX_VERTICES + 0, MAX_VERTICES + 1, MAX_VERTICES + 3, true);
    GfxSpTri1(MAX_VERTICES + 1, MAX_VERTICES + 2, MAX_VERTICES + 3, true);

    mRsp->geometry_mode = geometry_mode_saved;
    mRdp->viewport = viewport_saved;
    mRdp->viewport_or_scissor_changed = true;

    if (cycle_type == G_CYC_COPY) {
        mRdp->other_mode_h = saved_other_mode_h;
    }
}

void Interpreter::GfxDpTextureRectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile, int16_t uls,
                                        int16_t ult, int16_t dsdx, int16_t dtdy, bool flip) {
    // printf("render %d at %d\n", tile, lrx);
    uint64_t saved_combine_mode = mRdp->combine_mode;
    if ((mRdp->other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_COPY) {
        // Per RDP Command Summary Set Tile's shift s and this dsdx should be set to 4 texels
        // Divide by 4 to get 1 instead
        dsdx >>= 2;

        // Color combiner is turned off in copy mode
        GfxDpSetCombineMode(color_comb(0, 0, 0, G_CCMUX_TEXEL0), alpha_comb(0, 0, 0, G_ACMUX_TEXEL0), 0, 0);

        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }

    // uls and ult are S10.5
    // dsdx and dtdy are S5.10
    // lrx, lry, ulx, uly are U10.2
    // lrs, lrt are S10.5
    if (flip) {
        dsdx = -dsdx;
        dtdy = -dtdy;
    }
    int16_t width = !flip ? lrx - ulx : lry - uly;
    int16_t height = !flip ? lry - uly : lrx - ulx;
    float lrs = ((uls << 7) + dsdx * width) >> 7;
    float lrt = ((ult << 7) + dtdy * height) >> 7;

    LoadedVertex* ul = &mRsp->loaded_vertices[MAX_VERTICES + 0];
    LoadedVertex* ll = &mRsp->loaded_vertices[MAX_VERTICES + 1];
    LoadedVertex* lr = &mRsp->loaded_vertices[MAX_VERTICES + 2];
    LoadedVertex* ur = &mRsp->loaded_vertices[MAX_VERTICES + 3];
    ul->u = uls;
    ul->v = ult;
    lr->u = lrs;
    lr->v = lrt;
    if (!flip) {
        ll->u = uls;
        ll->v = lrt;
        ur->u = lrs;
        ur->v = ult;
    } else {
        ll->u = lrs;
        ll->v = ult;
        ur->u = uls;
        ur->v = lrt;
    }

    uint8_t saved_tile = mRdp->first_tile_index;
    if (saved_tile != tile) {
        mRdp->textures_changed[0] = true;
        mRdp->textures_changed[1] = true;
    }
    mRdp->first_tile_index = tile;

    GfxDrawRectangle(ulx, uly, lrx, lry);
    if (saved_tile != tile) {
        mRdp->textures_changed[0] = true;
        mRdp->textures_changed[1] = true;
    }
    mRdp->first_tile_index = saved_tile;
    mRdp->combine_mode = saved_combine_mode;
}

void Interpreter::GfxDpImageRectangle(int32_t tile, int32_t w, int32_t h, int32_t ulx, int32_t uly, int16_t uls,
                                      int16_t ult, int32_t lrx, int32_t lry, int16_t lrs, int16_t lrt) {

    LoadedVertex* ul = &mRsp->loaded_vertices[MAX_VERTICES + 0];
    LoadedVertex* ll = &mRsp->loaded_vertices[MAX_VERTICES + 1];
    LoadedVertex* lr = &mRsp->loaded_vertices[MAX_VERTICES + 2];
    LoadedVertex* ur = &mRsp->loaded_vertices[MAX_VERTICES + 3];
    ul->u = uls * 32;
    ul->v = ult * 32;
    lr->u = lrs * 32;
    lr->v = lrt * 32;
    ll->u = uls * 32;
    ll->v = lrt * 32;
    ur->u = lrs * 32;
    ur->v = ult * 32;

    // ensure we have the correct texture size, format and starting position
    mRdp->texture_tile[tile].siz = G_IM_SIZ_8b;
    mRdp->texture_tile[tile].fmt = G_IM_FMT_RGBA;
    mRdp->texture_tile[tile].cms = 0;
    mRdp->texture_tile[tile].cmt = 0;
    mRdp->texture_tile[tile].shifts = 0;
    mRdp->texture_tile[tile].shiftt = 0;
    mRdp->texture_tile[tile].uls = 0 * 4;
    mRdp->texture_tile[tile].ult = 0 * 4;
    mRdp->texture_tile[tile].lrs = w * 4;
    mRdp->texture_tile[tile].lrt = h * 4;
    mRdp->texture_tile[tile].line_size_bytes = w << (mRdp->texture_tile[tile].siz >> 1);

    auto& loadtex = mRdp->loaded_texture[mRdp->texture_tile[tile].tmem_index];
    loadtex.raw_tex_metadata = {};
    loadtex.full_image_line_size_bytes = loadtex.line_size_bytes = mRdp->texture_tile[tile].line_size_bytes;
    loadtex.size_bytes = loadtex.orig_size_bytes = loadtex.line_size_bytes * h;

    uint8_t saved_tile = mRdp->first_tile_index;
    if (saved_tile != tile) {
        mRdp->textures_changed[0] = true;
        mRdp->textures_changed[1] = true;
    }
    mRdp->first_tile_index = tile;

    GfxDrawRectangle(ulx, uly, lrx, lry);
    if (saved_tile != tile) {
        mRdp->textures_changed[0] = true;
        mRdp->textures_changed[1] = true;
    }
    mRdp->first_tile_index = saved_tile;
}

void Interpreter::GfxDpFillRectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    if (mRdp->color_image_address == mRdp->z_buf_address) {
        // Fullscreen Z clears are redundant — already done by glClear at frame start.
        bool isFullScreen = (ulx <= 0 && uly <= 0 && lrx >= (int32_t)(mNativeDimensions.width - 1) * 4 &&
                             lry >= (int32_t)(mNativeDimensions.height - 1) * 4);
        if (isFullScreen) {
            return;
        }

        // Partial depth clear (e.g. HUD model regions): clear the actual depth buffer
        // via a scissored depth clear instead of drawing a colored rect to the color buffer.
        Flush();

        // Convert U10.2 coords to pixel coords and add +1 pixel for fill mode
        int32_t expanded_lrx = lrx + (1 << 2);
        int32_t expanded_lry = lry + (1 << 2);
        float x = ulx / 4.0f;
        float y = expanded_lry / 4.0f;
        float w = (expanded_lrx - ulx) / 4.0f;
        float h = (expanded_lry - uly) / 4.0f;
        float halfNativeWidth = (float)HALF_SCREEN_WIDTH(mActiveFrameBuffer);
        x = halfNativeWidth + AdjXForAspectRatio(x - halfNativeWidth);
        w = AdjXForAspectRatio(w);

        struct XYWidthHeight area;
        area.x = (int16_t)x;
        area.y = (int16_t)y;
        area.width = (uint32_t)w;
        area.height = (uint32_t)h;
        AdjustVIewportOrScissor(&area);

        mRapi->ClearDepthRegion(area.x, area.y, area.width, area.height);
        return;
    }
    uint32_t mode = (mRdp->other_mode_h & (3U << G_MDSFT_CYCLETYPE));

    // Expand fullscreen fill rects to cover widescreen viewports.
    // Without this, screen clears and fades only cover the native 4:3 area.
    if (ulx == 0 && uly == 0) {
        bool isFullScreen = (lrx == ((int32_t)(mNativeDimensions.width - 1) * 4) &&
                             lry == ((int32_t)(mNativeDimensions.height - 1) * 4));
        if (isFullScreen) {
            ulx = -1024;
            uly = -1024;
            lrx = 2048;
            lry = 2048;
        }
    }

    if (mode == G_CYC_COPY || mode == G_CYC_FILL) {
        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }

    for (int i = MAX_VERTICES; i < MAX_VERTICES + 4; i++) {
        LoadedVertex* v = &mRsp->loaded_vertices[i];
        v->color = mRdp->fill_color;
    }

    uint64_t saved_combine_mode = mRdp->combine_mode;

    if (mode == G_CYC_FILL) {
        GfxDpSetCombineMode(color_comb(0, 0, 0, G_CCMUX_SHADE), alpha_comb(0, 0, 0, G_ACMUX_SHADE), 0, 0);
    }

    GfxDrawRectangle(ulx, uly, lrx, lry);
    mRdp->combine_mode = saved_combine_mode;
}

void Interpreter::GfxDpSetZImage(void* zBufAddr) {
    mRdp->z_buf_address = zBufAddr;
}

void Interpreter::GfxDpSetColorImage(uint32_t format, uint32_t size, uint32_t width, void* address) {
    mRdp->color_image_address = address;
}

void Interpreter::GfxSpSetOtherMode(uint32_t shift, uint32_t num_bits, uint64_t mode) {
    uint64_t mask = (((uint64_t)1 << num_bits) - 1) << shift;
    uint64_t om = mRdp->other_mode_l | ((uint64_t)mRdp->other_mode_h << 32);
    om = (om & ~mask) | mode;
#ifdef LUS_XBOX
    if (mRdp->other_mode_l != (uint32_t)om || mRdp->other_mode_h != (uint32_t)(om >> 32)) {
        mHwStateGen++;
    }
#endif
    mRdp->other_mode_l = (uint32_t)om;
    mRdp->other_mode_h = (uint32_t)(om >> 32);
}

void Interpreter::GfxDpSetOtherMode(uint32_t h, uint32_t l) {
#ifdef LUS_XBOX
    if (mRdp->other_mode_h != h || mRdp->other_mode_l != l) {
        mHwStateGen++;
    }
#endif
    mRdp->other_mode_h = h;
    mRdp->other_mode_l = l;
}

void Interpreter::Gfxs2dexBgCopy(F3DuObjBg* bg) {
    /*
    bg->b.imageX = 0;
    bg->b.imageW = width * 4;
    bg->b.frameX = frameX * 4;
    bg->b.imageY = 0;
    bg->b.imageH = height * 4;
    bg->b.frameY = frameY * 4;
    bg->b.imagePtr = source;
    bg->b.imageLoad = G_BGLT_LOADTILE;
    bg->b.imageFmt = fmt;
    bg->b.imageSiz = siz;
    bg->b.imagePal = 0;
    bg->b.imageFlip = 0;
    */

    uintptr_t data = (uintptr_t)bg->b.imagePtr;

    uint32_t texFlags = 0;
    RawTexMetadata rawTexMetadata = {};

    if ((bool)gfx_check_image_signature((char*)data)) {
        std::shared_ptr<Fast::Texture> tex = std::static_pointer_cast<Fast::Texture>(
            Ship::Context::GetRawInstance()->GetResourceManager()->LoadResourceProcess((char*)data));
        texFlags = tex->Flags;
        rawTexMetadata.width = tex->Width;
        rawTexMetadata.height = tex->Height;
        rawTexMetadata.h_byte_scale = tex->HByteScale;
        rawTexMetadata.v_pixel_scale = tex->VPixelScale;
        rawTexMetadata.type = tex->Type;
        rawTexMetadata.resource = tex;
        data = (uintptr_t) reinterpret_cast<char*>(tex->ImageData);
    }

    s16 dsdx = 4 << 10;
    s16 uls = bg->b.imageX << 3;
    // Flip flag only flips horizontally
    if (bg->b.imageFlip == G_BG_FLAG_FLIPS) {
        dsdx = -dsdx;
        uls = (bg->b.imageW - bg->b.imageX) << 3;
    }

    SUPPORT_CHECK(bg->b.imageSiz == G_IM_SIZ_16b);
    GfxDpSetTextureImage(G_IM_FMT_RGBA, G_IM_SIZ_16b, 0, nullptr, texFlags, rawTexMetadata, (void*)data);
    GfxDpSetTile(G_IM_FMT_RGBA, G_IM_SIZ_16b, 0, 0, G_TX_LOADTILE, 0, 0, 0, 0, 0, 0, 0);
    GfxDpLoadBlock(G_TX_LOADTILE, 0, 0, (bg->b.imageW * bg->b.imageH >> 4) - 1, 0);
    GfxDpSetTile(bg->b.imageFmt, G_IM_SIZ_16b, bg->b.imageW >> 4, 0, G_TX_RENDERTILE, bg->b.imagePal, 0, 0, 0, 0, 0, 0);
    GfxDpSetTileSize(G_TX_RENDERTILE, 0, 0, bg->b.imageW, bg->b.imageH);
    GfxDpTextureRectangle(bg->b.frameX, bg->b.frameY, bg->b.frameX + bg->b.imageW - 4, bg->b.frameY + bg->b.imageH - 4,
                          G_TX_RENDERTILE, uls, bg->b.imageY << 3, dsdx, 1 << 10, false);
}

void Interpreter::Gfxs2dexBg1cyc(F3DuObjBg* bg) {
    uintptr_t data = (uintptr_t)bg->b.imagePtr;

    uint32_t texFlags = 0;
    RawTexMetadata rawTexMetadata = {};

    if ((bool)gfx_check_image_signature((char*)data)) {
        std::shared_ptr<Fast::Texture> tex = std::static_pointer_cast<Fast::Texture>(
            Ship::Context::GetRawInstance()->GetResourceManager()->LoadResourceProcess((char*)data));
        texFlags = tex->Flags;
        rawTexMetadata.width = tex->Width;
        rawTexMetadata.height = tex->Height;
        rawTexMetadata.h_byte_scale = tex->HByteScale;
        rawTexMetadata.v_pixel_scale = tex->VPixelScale;
        rawTexMetadata.type = tex->Type;
        rawTexMetadata.resource = tex;
        data = (uintptr_t) reinterpret_cast<char*>(tex->ImageData);
    }

    // TODO: Implement bg scaling correctly
    s16 uls = bg->b.imageX >> 2;
    s16 lrs = bg->b.imageW >> 2;

    s16 dsdxRect = 1 << 10;
    s16 ulsRect = bg->b.imageX << 3;
    // Flip flag only flips horizontally
    if (bg->b.imageFlip == G_BG_FLAG_FLIPS) {
        dsdxRect = -dsdxRect;
        ulsRect = (bg->b.imageW - bg->b.imageX) << 3;
    }

    GfxDpSetTextureImage(bg->b.imageFmt, bg->b.imageSiz, bg->b.imageW >> 2, nullptr, texFlags, rawTexMetadata,
                         (void*)data);
    GfxDpSetTile(bg->b.imageFmt, bg->b.imageSiz, 0, 0, G_TX_LOADTILE, 0, 0, 0, 0, 0, 0, 0);
    GfxDpLoadBlock(G_TX_LOADTILE, 0, 0, (bg->b.imageW * bg->b.imageH >> 4) - 1, 0);
    GfxDpSetTile(bg->b.imageFmt, bg->b.imageSiz, (((lrs - uls) * bg->b.imageSiz) + 7) >> 3, 0, G_TX_RENDERTILE,
                 bg->b.imagePal, 0, 0, 0, 0, 0, 0);
    GfxDpSetTileSize(G_TX_RENDERTILE, 0, 0, bg->b.imageW, bg->b.imageH);

    GfxDpTextureRectangle(bg->b.frameX, bg->b.frameY, bg->b.frameW, bg->b.frameH, G_TX_RENDERTILE, ulsRect,
                          bg->b.imageY << 3, dsdxRect, 1 << 10, false);
}

void Interpreter::Gfxs2dexRecyCopy(F3DuObjSprite* spr) {
    s16 dsdx = 4 << 10;
    [[maybe_unused]] s16 uls = spr->s.objX << 3;
    // Flip flag only flips horizontally
    if (spr->s.imageFlags == G_BG_FLAG_FLIPS) {
        dsdx = -dsdx;
        uls = (spr->s.imageW - spr->s.objX) << 3;
    }

    int realX = spr->s.objX >> 2;
    int realY = spr->s.objY >> 2;
    int realW = (((spr->s.imageW)) >> 5);
    int realH = (((spr->s.imageH)) >> 5);
    float realSW = spr->s.scaleW / 1024.0f;
    float realSH = spr->s.scaleH / 1024.0f;

    int testX = (realX + (realW / realSW));
    int testY = (realY + (realH / realSH));

    GfxDpTextureRectangle(realX << 2, realY << 2, testX << 2, testY << 2, G_TX_RENDERTILE,
                          (s32)mRdp->texture_tile[0].uls << 3, (s32)mRdp->texture_tile[0].ult << 3,
                          (float)(1 << 10) * realSW, (float)(1 << 10) * realSH, false);
}

void* Interpreter::SegAddr(uintptr_t w1) {
    // Segmented?
    if (w1 & 1) {
        uint32_t segNum = (uint32_t)(w1 >> 24);

        uint32_t offset = w1 & 0x00FFFFFE;

        if (mSegmentPointers[segNum] != 0) {
            return (void*)(mSegmentPointers[segNum] + offset);
        } else {
            return (void*)w1;
        }
    } else {
        return (void*)w1;
    }
}

#define C0(pos, width) ((cmd->words.w0 >> (pos)) & ((1U << width) - 1))
#define C1(pos, width) ((cmd->words.w1 >> (pos)) & ((1U << width) - 1))

void GfxExecStack::start(F3DGfx* dlist) {
    while (!cmd_stack.empty())
        cmd_stack.pop();
    gfx_path.clear();
    cmd_stack.push(dlist);
    disp_stack.clear();
}

void GfxExecStack::stop() {
    while (!cmd_stack.empty())
        cmd_stack.pop();
    gfx_path.clear();
}

F3DGfx*& GfxExecStack::currCmd() {
    return cmd_stack.top();
}

void GfxExecStack::openDisp(const char* file, int line) {
    disp_stack.push_back({ file, line });
}
void GfxExecStack::closeDisp() {
    disp_stack.pop_back();
}
const std::vector<GfxExecStack::CodeDisp>& GfxExecStack::getDisp() const {
    return disp_stack;
}

void GfxExecStack::branch(F3DGfx* caller) {
    F3DGfx* old = cmd_stack.top();
    cmd_stack.pop();
    cmd_stack.push(nullptr);
    cmd_stack.push(old);

    gfx_path.push_back(caller);
}

void GfxExecStack::call(F3DGfx* caller, F3DGfx* callee) {
    cmd_stack.push(callee);
    gfx_path.push_back(caller);
}

F3DGfx* GfxExecStack::ret() {
    F3DGfx* cmd = cmd_stack.top();

    cmd_stack.pop();
    if (!gfx_path.empty()) {
        gfx_path.pop_back();
    }

    while (cmd_stack.size() > 0 && cmd_stack.top() == nullptr) {
        cmd_stack.pop();
        if (!gfx_path.empty()) {
            gfx_path.pop_back();
        }
    }
    return cmd;
}

void gfx_set_framebuffer(int fb, float noise_scale);
void gfx_reset_framebuffer();
void gfx_copy_framebuffer(int fb_dst_id, int fb_src_id, bool copyOnce, bool* hasCopiedPtr);

// The main type of the handler function. These function will take a pointer to a pointer to a Gfx. It needs to be a
// double pointer because we sometimes need to increment and decrement the underlying pointer Returns false if the
// current opcode should be incremented after the handler ends.
typedef bool (*GfxOpcodeHandlerFunc)(F3DGfx** gfx);

bool gfx_load_ucode_handler_f3dex2(F3DGfx** cmd) {
    Interpreter* gfx = GfxInstance();
    gfx->mRsp->fog_mul = 0;
    gfx->mRsp->fog_offset = 0;
    return false;
}

bool gfx_cull_dl_handler_f3dex2(F3DGfx** cmd) {
    // TODO:
    return false;
}

bool gfx_marker_handler_otr(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    (*cmd0)++;
    F3DGfx* cmd = (*cmd0);
    gfx->mMarkerOn = true;
    return false;
}

bool gfx_invalidate_tex_cache_handler_f3dex2(F3DGfx** cmd) {
    Interpreter* gfx = GfxInstance();
    const uintptr_t texAddr = (*cmd)->words.w1;

    if (texAddr == 0) {
        gfx->TextureCacheClear();
    } else {
        gfx->TextureCacheDelete((const uint8_t*)texAddr);
    }
    return false;
}

bool gfx_noop_handler_f3dex2(F3DGfx** cmd0) {
    F3DGfx* cmd = *cmd0;
    const char* filename = (const char*)(cmd)->words.w1;
    uint32_t p = C0(16, 8);
    uint32_t l = C0(0, 16);
    if (p == 7) {
        g_exec_stack.openDisp(filename, l);
    } else if (p == 8) {
        if (g_exec_stack.disp_stack.size() == 0) {
            SPDLOG_WARN("CLOSE_DISPS without matching open {}:{}", p, l);
        } else {
            g_exec_stack.closeDisp();
        }
    }
    return false;
}

bool gfx_mtx_handler_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;
    uintptr_t mtxAddr = cmd->words.w1;

    gfx->GfxSpMatrix(C0(0, 8) ^ F3DEX2_G_MTX_PUSH, (const int32_t*)gfx->SegAddr(mtxAddr));
    return false;
}
// Seems to be the same for all other non F3DEX2 microcodes...
bool gfx_mtx_handler_f3d(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;
    uintptr_t mtxAddr = cmd->words.w1;

    gfx->GfxSpMatrix(C0(16, 8), (const int32_t*)gfx->SegAddr(cmd->words.w1));
    return false;
}

bool gfx_mtx_otr_filepath_handler_custom_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;
    const char* fileName = (const char*)cmd->words.w1;
    const int32_t* mtx = (const int32_t*)Ship::Context::GetRawInstance()->GetResourceManager()->GetResourceRawPointer(
        (const char*)fileName);

    if (mtx != NULL) {
        gfx->GfxSpMatrix(C0(0, 8) ^ F3DEX2_G_MTX_PUSH, mtx);
    }

    return false;
}

bool gfx_mtx_otr_filepath_handler_custom_f3d(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;
    const char* fileName = (const char*)cmd->words.w1;
    const int32_t* mtx = (const int32_t*)Ship::Context::GetRawInstance()->GetResourceManager()->GetResourceRawPointer(
        (const char*)fileName);

    if (mtx != NULL) {
        gfx->GfxSpMatrix(C0(16, 8), mtx);
    }

    return false;
}

bool gfx_mtx_otr_filepath_handler_custom(F3DGfx** cmd0) {
    if (ucode_handler_index == ucode_f3dex2) {
        return gfx_mtx_otr_filepath_handler_custom_f3dex2(cmd0);
    } else {
        return gfx_mtx_otr_filepath_handler_custom_f3d(cmd0);
    }
}

bool gfx_mtx_otr_handler_custom_f3dex2(F3DGfx** cmd0) {
    (*cmd0)++;
    F3DGfx* cmd = *cmd0;

    const uint64_t hash = ((uint64_t)cmd->words.w0 << 32) + cmd->words.w1;
    const int32_t* mtx =
        (const int32_t*)Ship::Context::GetRawInstance()->GetResourceManager()->GetResourceRawPointer(hash);

    if (mtx != NULL) {
        Interpreter* gfx = GfxInstance();
        cmd--;
        gfx->GfxSpMatrix(C0(0, 8) ^ F3DEX2_G_MTX_PUSH, mtx);
        cmd++;
    }

    return false;
}

bool gfx_mtx_otr_handler_custom_f3d(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    (*cmd0)++;
    F3DGfx* cmd = *cmd0;

    const uint64_t hash = ((uint64_t)cmd->words.w0 << 32) + cmd->words.w1;
    const int32_t* mtx =
        (const int32_t*)Ship::Context::GetRawInstance()->GetResourceManager()->GetResourceRawPointer(hash);
    if (mtx != nullptr) {
        cmd--;
        gfx->GfxSpMatrix(C0(16, 8), mtx);
        cmd++;
    }
    return false;
}

bool gfx_mtx_otr_handler_custom(F3DGfx** cmd0) {
    if (ucode_handler_index == ucode_f3dex2) {
        return gfx_mtx_otr_handler_custom_f3dex2(cmd0);
    } else {
        return gfx_mtx_otr_handler_custom_f3d(cmd0);
    }
}

bool gfx_pop_mtx_handler_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpPopMatrix((uint32_t)(cmd->words.w1 / 64));

    return false;
}

bool gfx_pop_mtx_handler_f3d(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpPopMatrix(1);

    return false;
}

bool gfx_movemem_handler_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpMovememF3dex2(C0(0, 8), C0(8, 8) * 8, gfx->SegAddr(cmd->words.w1));

    return false;
}

bool gfx_movemem_handler_f3d(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpMovememF3d(C0(16, 8), 0, gfx->SegAddr(cmd->words.w1));

    return false;
}

bool gfx_movemem_handler_otr(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    const uint8_t index = C1(24, 8);
    const uint8_t offset = C1(16, 8);
    const uint8_t hasOffset = C1(8, 8);

    (*cmd0)++;

    const uint64_t hash = ((uint64_t)(*cmd0)->words.w0 << 32) + (*cmd0)->words.w1;

    if (ucode_handler_index == ucode_f3dex2) {
        gfx->GfxSpMovememF3dex2(index, offset,
                                Ship::Context::GetRawInstance()->GetResourceManager()->GetResourceRawPointer(hash));
    } else {
        auto light =
            (Fast::LightEntry*)Ship::Context::GetRawInstance()->GetResourceManager()->GetResourceRawPointer(hash);
        uintptr_t data = (uintptr_t)&light->Ambient;
        gfx->GfxSpMovememF3d(index, offset, (void*)(data + (hasOffset == 1 ? 0x8 : 0)));
    }
    return false;
}

bool gfx_push_shader(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;
    const char* path = (const char*)gfx->SegAddr(cmd->words.w1);

    if (!gfx_check_image_signature(path)) {
        SPDLOG_ERROR("G_PUSH_SHADER: Shader is not a valid OTR resource name, unable to register push shader");
        return false;
    }

    path = &path[7];

    size_t shaderId = static_cast<size_t>(-1);
    for (const auto& shader : gfx->mShaders) {
        if (strcmp(shader.second, path) == 0) {
            shaderId = shader.first;
            break;
        }
    }

    if (shaderId == static_cast<size_t>(-1)) {
        shaderId = gfx->mShadersIndex++;
        gfx->mShaders[shaderId] = path;
    }

    gfx->mShaderStack.push(shaderId);

    return false;
}

bool gfx_pop_shader(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    if (!gfx->mShaderStack.empty()) {
        gfx->mShaderStack.pop();
    }

    return false;
}

const char* gfx_get_shader(int16_t id) {
    Interpreter* gfx = GfxInstance();

    for (const std::pair<size_t, const char*>& shader : gfx->mShaders) {
        if (shader.first == id) {
            return shader.second;
        }
    }

    return nullptr; // Use no shader
}

bool gfx_moveword_handler_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpMovewordF3dex2(C0(16, 8), C0(0, 16), cmd->words.w1);

    return false;
}

bool gfx_moveword_handler_f3d(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpMovewordF3d(C0(0, 8), C0(8, 16), cmd->words.w1);

    return false;
}

bool gfx_texture_handler_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpTexture(C1(16, 16), C1(0, 16), C0(11, 3), C0(8, 3), C0(1, 7));

    return false;
}

// Seems to be the same for all other non F3DEX2 microcodes...
bool gfx_texture_handler_f3d(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpTexture(C1(16, 16), C1(0, 16), C0(11, 3), C0(8, 3), C0(0, 8));

    return false;
}

// Almost all versions of the microcode have their own version of this opcode
bool gfx_vtx_handler_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpVertex(C0(12, 8), C0(1, 7) - C0(12, 8), (const F3DVtx*)gfx->SegAddr(cmd->words.w1));

    return false;
}

bool gfx_vtx_handler_f3dex(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;
    gfx->GfxSpVertex(C0(10, 6), C0(17, 7), (const F3DVtx*)gfx->SegAddr(cmd->words.w1));

    return false;
}

bool gfx_vtx_handler_f3d(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpVertex((C0(0, 16)) / sizeof(F3DVtx), C0(16, 4), (const F3DVtx*)gfx->SegAddr(cmd->words.w1));

    return false;
}

bool gfx_vtx_hash_handler_custom(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    // Offset added to the start of the vertices
    const uintptr_t offset = (*cmd0)->words.w1;
    // This is a two-part display list command, so increment the instruction pointer so we can get the CRC64
    // hash from the second
    (*cmd0)++;
    const uint64_t hash = ((uint64_t)(*cmd0)->words.w0 << 32) + (*cmd0)->words.w1;

    // We need to know if the offset is a cached pointer or not. An offset greater than one million is not a
    // real offset, so it must be a real pointer
    if (offset > 0xFFFFF) {
        (*cmd0)--;
        F3DGfx* cmd = *cmd0;
        gfx->GfxSpVertex(C0(12, 8), C0(1, 7) - C0(12, 8), (F3DVtx*)offset);
        (*cmd0)++;
    } else {
#ifdef LUS_XBOX
        F3DVtx* vtx = (F3DVtx*)XboxRawPointerByHash(hash);
#else
        F3DVtx* vtx = (F3DVtx*)Ship::Context::GetRawInstance()->GetResourceManager()->GetResourceRawPointer(hash);
#endif

        if (vtx != NULL) {
            vtx = (F3DVtx*)((char*)vtx + offset);

            (*cmd0)--;
            F3DGfx* cmd = *cmd0;

            // TODO: WTF??
            cmd->words.w1 = (uintptr_t)vtx;

            gfx->GfxSpVertex(C0(12, 8), C0(1, 7) - C0(12, 8), vtx);
            (*cmd0)++;
        }
    }
    return false;
}

bool gfx_vtx_otr_filepath_handler_custom(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;
    char* fileName = (char*)cmd->words.w1;
    (*cmd0)++;
    cmd = *cmd0;
    size_t vtxCnt = cmd->words.w0;
    size_t vtxIdxOff = cmd->words.w1 >> 16;
    size_t vtxDataOff = cmd->words.w1 & 0xFFFF;
    F3DVtx* vtx =
        (F3DVtx*)Ship::Context::GetRawInstance()->GetResourceManager()->GetResourceRawPointer((const char*)fileName);
    vtx += vtxDataOff;

    gfx->GfxSpVertex(vtxCnt, vtxIdxOff, vtx);
    return false;
}

bool gfx_dl_otr_filepath_handler_custom(F3DGfx** cmd0) {
    F3DGfx* cmd = *cmd0;
    char* fileName = (char*)cmd->words.w1;
    F3DGfx* nDL =
        (F3DGfx*)Ship::Context::GetRawInstance()->GetResourceManager()->GetResourceRawPointer((const char*)fileName);

    if (C0(16, 1) == 0 && nDL != nullptr) {
        g_exec_stack.call(*cmd0, nDL);
    } else {
        if (nDL != nullptr) {
            (*cmd0) = nDL;
            g_exec_stack.branch(cmd);
            return true; // shortcut cmd increment
        } else {
            assert(0 && "???");
            // gfx_path.pop_back();
            // cmd = cmd_stack.top();
            // cmd_stack.pop();
        }
    }
    return false;
}

// The original F3D microcode doesn't seem to have this opcode. Glide handles it as part of moveword
bool gfx_modify_vtx_handler_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;
    gfx->GfxSpModifyVertex(C0(1, 15), C0(16, 8), (uint32_t)cmd->words.w1);
    return false;
}

// F3D, F3DEX, and F3DEX2 do the same thing but F3DEX2 has its own opcode number
bool gfx_dl_handler_common(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;
    F3DGfx* subGFX = (F3DGfx*)gfx->SegAddr(cmd->words.w1);
    if (C0(16, 1) == 0) {
        // Push return address
        if (subGFX != nullptr) {
            g_exec_stack.call(*cmd0, subGFX);
        }
    } else {
        (*cmd0) = subGFX;
        g_exec_stack.branch(cmd);
        return true; // shortcut cmd increment
    }
    return false;
}

bool gfx_dl_otr_hash_handler_custom(F3DGfx** cmd0) {
    F3DGfx* cmd = *cmd0;
    if (C0(16, 1) == 0) {
        // Push return address
        (*cmd0)++;

        uint64_t hash = ((uint64_t)(*cmd0)->words.w0 << 32) + (*cmd0)->words.w1;

#ifdef LUS_XBOX
        F3DGfx* gfx = (F3DGfx*)XboxRawPointerByHash(hash);
#else
        F3DGfx* gfx = (F3DGfx*)Ship::Context::GetRawInstance()->GetResourceManager()->GetResourceRawPointer(hash);
#endif

        if (gfx != 0) {
            g_exec_stack.call(cmd, gfx);
        }
    } else {
        Interpreter* gfx = GfxInstance();
        assert(0 && "????");
        (*cmd0) = (F3DGfx*)gfx->SegAddr((*cmd0)->words.w1);
        return true;
    }
    return false;
}
bool gfx_dl_index_handler(F3DGfx** cmd0) {
    // Compute seg addr by converting an index value to a offset value
    // handling 32 vs 64 bit size differences for Gfx
    // adding 1 to trigger the segaddr flow
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = (*cmd0);
    uint8_t segNum = (uint8_t)(cmd->words.w1 >> 24);
    uint32_t index = (uint32_t)(cmd->words.w1 & 0x00FFFFFF);
    uintptr_t segAddr = (segNum << 24) | (index * sizeof(F3DGfx)) + 1;

    F3DGfx* subGFX = (F3DGfx*)gfx->SegAddr(segAddr);
    if (C0(16, 1) == 0) {
        // Push return address
        if (subGFX != nullptr) {
            g_exec_stack.call((*cmd0), subGFX);
        }
    } else {
        (*cmd0) = subGFX;
        g_exec_stack.branch(cmd);
        return true; // shortcut cmd increment
    }
    return false;
}

// TODO handle special OTR opcodes later...
bool gfx_pushcd_handler_custom(F3DGfx** cmd0) {
    gfx_push_current_dir((char*)(*cmd0)->words.w1);
    return false;
}

// TODO handle special OTR opcodes later...
bool gfx_branch_z_otr_handler_f3dex2(F3DGfx** cmd0) {
    // Push return address
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = (*cmd0);

    uint8_t vbidx = (uint8_t)((*cmd0)->words.w0 & 0x00000FFF);
    uint32_t zval = (uint32_t)((*cmd0)->words.w1);

    (*cmd0)++;

#ifdef LUS_XBOX
    gfx->EnsureTransformed(&gfx->mRsp->loaded_vertices[vbidx]); // loaded raw for the GPU: its z made here
#endif
    if (gfx->mRsp->loaded_vertices[vbidx].z <= zval ||
        (gfx->mRsp->extra_geometry_mode & G_EX_ALWAYS_EXECUTE_BRANCH) != 0) {
        uint64_t hash = ((uint64_t)(*cmd0)->words.w0 << 32) + (*cmd0)->words.w1;

#ifdef LUS_XBOX
        F3DGfx* gfx = (F3DGfx*)XboxRawPointerByHash(hash);
#else
        F3DGfx* gfx = (F3DGfx*)Ship::Context::GetRawInstance()->GetResourceManager()->GetResourceRawPointer(hash);
#endif

        if (gfx != 0) {
            (*cmd0) = gfx;
            g_exec_stack.branch(cmd);
            return true; // shortcut cmd increment
        }
    }
    return false;
}

// F3D, F3DEX, and F3DEX2 do the same thing but F3DEX2 has its own opcode number
bool gfx_end_dl_handler_common(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    gfx->mMarkerOn = false;
    g_exec_stack.ret();
    return true;
}

bool gfx_set_prim_depth_handler_rdp(F3DGfx** cmd) {
    Interpreter* gfx = GfxInstance();
    uint32_t w1 = (*cmd)->words.w1;
    gfx->mRdp->prim_depth = (uint16_t)((w1 >> 16) & 0x7FFF); // Mask to 15 bits
    return false;
}

// Only on F3DEX2
bool gfx_geometry_mode_handler_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpGeometryMode(~C0(0, 24), (uint32_t)cmd->words.w1);
    return false;
}

// Only on F3DEX and older
bool gfx_set_geometry_mode_handler_f3d(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpGeometryMode(0, (uint32_t)cmd->words.w1);
    return false;
}

// Only on F3DEX and older
bool gfx_clear_geometry_mode_handler_f3d(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpGeometryMode((uint32_t)cmd->words.w1, 0);
    return false;
}

bool gfx_tri1_otr_handler_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();

    F3DGfx* cmd = *cmd0;
    uint8_t v00 = (uint8_t)(cmd->words.w0 & 0x0000FFFF);
    uint8_t v01 = (uint8_t)(cmd->words.w1 >> 16);
    uint8_t v02 = (uint8_t)(cmd->words.w1 & 0x0000FFFF);
    gfx->GfxSpTri1(v00, v01, v02, false);

    return false;
}

bool gfx_tri1_handler_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpTri1(C0(16, 8) / 2, C0(8, 8) / 2, C0(0, 8) / 2, false);

    return false;
}

bool gfx_tri1_handler_f3dex(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpTri1(C1(17, 7), C1(9, 7), C1(1, 7), false);

    return false;
}

bool gfx_tri1_handler_f3d(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpTri1(C1(16, 8) / 10, C1(8, 8) / 10, C1(0, 8) / 10, false);

    return false;
}

// F3DEX, and F3DEX2 share a tri2 function, however F3DEX has a different quad function.
bool gfx_tri2_handler_f3dex(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpTri1(C0(17, 7), C0(9, 7), C0(1, 7), false);
    gfx->GfxSpTri1(C1(17, 7), C1(9, 7), C1(1, 7), false);
    return false;
}

bool gfx_quad_handler_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpTri1(C0(16, 8) / 2, C0(8, 8) / 2, C0(0, 8) / 2, false);
    gfx->GfxSpTri1(C1(16, 8) / 2, C1(8, 8) / 2, C1(0, 8) / 2, false);
    return false;
}

bool gfx_quad_handler_f3dex(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpTri1(C1(16, 8) / 2, C1(8, 8) / 2, C1(0, 8) / 2, false);
    gfx->GfxSpTri1(C1(16, 8) / 2, C1(0, 8) / 2, C1(24, 8) / 2, false);
    return false;
}

bool gfx_othermode_l_handler_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpSetOtherMode(31 - C0(8, 8) - C0(0, 8), C0(0, 8) + 1, cmd->words.w1);

    return false;
}

bool gfx_othermode_l_handler_f3d(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpSetOtherMode(C0(8, 8), C0(0, 8), cmd->words.w1);

    return false;
}

bool gfx_othermode_h_handler_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpSetOtherMode(63 - C0(8, 8) - C0(0, 8), C0(0, 8) + 1, (uint64_t)cmd->words.w1 << 32);

    return false;
}

// Only on F3DEX and older
bool gfx_set_geometry_mode_handler_f3dex(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpGeometryMode(0, (uint32_t)cmd->words.w1);
    return false;
}

// Only on F3DEX and older
bool gfx_clear_geometry_mode_handler_f3dex(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpGeometryMode((uint32_t)cmd->words.w1, 0);
    return false;
}

bool gfx_othermode_h_handler_f3d(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpSetOtherMode(C0(8, 8) + 32, C0(0, 8), (uint64_t)cmd->words.w1 << 32);

    return false;
}

// A resolved address still in the N64 segmented range (<= 0x0FFFFFFF) usually means SegAddr
// failed to resolve it (segment not set up). Keep it only if it belongs to a loaded module,
// where it's a real low pointer (e.g. a static TLUT) rather than an unresolved segment addr.
static bool IsValidResolvedAddress(uintptr_t addr) {
    if (addr > 0x0FFFFFFF) {
        return true;
    }

    // Still in the N64 segmented range, but might be a false positive (a real low pointer).
#ifdef _WIN32
    // For Windows, check whether the address belongs to a dll.
    HMODULE module = nullptr;
    return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCSTR>(addr), &module) != 0;
#elif defined(LUS_XBOX)
    // On the Xbox everything the title has lies below 64 MB, so the range says nothing; a segmented
    // address SegAddr could not resolve still has its marker bit 0 set.
    return addr != 0 && (addr & 1) == 0;
#else
    // For non-Windows platforms, check whether the address belongs to a loaded object.
    Dl_info info;
    return dladdr(reinterpret_cast<void*>(addr), &info) != 0;
#endif
}

bool gfx_set_timg_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;
    uintptr_t i = (uintptr_t)gfx->SegAddr(cmd->words.w1);

    char* imgData = (char*)i;
    uint32_t texFlags = 0;
    RawTexMetadata rawTexMetdata = {};
    // Default scale factors to 1 for raw N64 textures. OTR textures set these
    // from the resource, but raw textures would leave them at 0.
    rawTexMetdata.h_byte_scale = 1;
    rawTexMetdata.v_pixel_scale = 1;

    if ((i & 1) != 1) {
        if (gfx_check_image_signature(imgData) == 1) {
            std::shared_ptr<Fast::Texture> tex =
                std::static_pointer_cast<Fast::Texture>(gfx->ResolveResourceCached(imgData));

            if (tex == nullptr) {
                (*cmd0)++;
                return false;
            }

            i = (uintptr_t) reinterpret_cast<char*>(tex->ImageData);
            texFlags = tex->Flags;
            rawTexMetdata.width = tex->Width;
            rawTexMetdata.height = tex->Height;
            rawTexMetdata.h_byte_scale = tex->HByteScale;
            rawTexMetdata.v_pixel_scale = tex->VPixelScale;
            rawTexMetdata.type = tex->Type;
            rawTexMetdata.resource = tex;
        }
    }

    if (!IsValidResolvedAddress(i)) {
        return false;
    }

    gfx->GfxDpSetTextureImage(C0(21, 3), C0(19, 2), C0(0, 12) + 1, imgData, texFlags, rawTexMetdata, (void*)i);

    return false;
}

bool gfx_set_timg_otr_hash_handler_custom(F3DGfx** cmd0) {
    uintptr_t addr = (*cmd0)->words.w1;
    (*cmd0)++;
    uint64_t hash = ((uint64_t)(*cmd0)->words.w0 << 32) + (uint64_t)(*cmd0)->words.w1;

#ifdef LUS_XBOX
    XboxTextureSlot& slot = sXboxTextureSlots[(uint32_t)hash % 1024];
    if (slot.hash == hash && slot.texture != nullptr) {
        F3DGfx* cmd = *cmd0 - 1;
        cmd->words.w1 = (uintptr_t)slot.texture->ImageData;
        GfxInstance()->GfxDpSetTextureImage(C0(19, 2), C0(0, 12) + 1, slot.texture);
        // left on the command's second word, as the long way below: gfx_step's ++cmd takes it past. One more here
        // skipped the command after it, the load's G_SETTILE, and textures came out as noise
        return false;
    }
#endif

    const char* fileName =
        Ship::Context::GetRawInstance()->GetResourceManager()->GetArchiveManager()->HashToCString(hash);
    uint32_t texFlags = 0;
    RawTexMetadata rawTexMetadata = {};

    if (fileName == nullptr) {
        (*cmd0)++;
        return false;
    }

#ifdef LUS_XBOX
    std::shared_ptr<Fast::Texture> texture = std::static_pointer_cast<Fast::Texture>(XboxResourceByHash(hash, fileName));
    if (texture != nullptr && texture->ImageData != nullptr) {
        slot.hash = hash;
        slot.texture = texture;
    }
#else
    std::shared_ptr<Fast::Texture> texture = std::static_pointer_cast<Fast::Texture>(
        Ship::Context::GetRawInstance()->GetResourceManager()->LoadResourceProcess(
            Ship::Context::GetRawInstance()->GetResourceManager()->GetArchiveManager()->HashToCString(hash)));
#endif
    if (texture != nullptr) {
        texFlags = texture->Flags;
        rawTexMetadata.width = texture->Width;
        rawTexMetadata.height = texture->Height;
        rawTexMetadata.h_byte_scale = texture->HByteScale;
        rawTexMetadata.v_pixel_scale = texture->VPixelScale;
        rawTexMetadata.type = texture->Type;
        rawTexMetadata.resource = texture;

        // OTRTODO: We have disabled caching for now to fix a texture corruption issue with HD texture
        // support. In doing so, there is a potential performance hit since we are not caching lookups. We
        // need to do proper profiling to see whether or not it is worth it to keep the caching system.

        char* tex = reinterpret_cast<char*>(texture->ImageData);

        if (tex != nullptr) {
            (*cmd0)--;
            uintptr_t oldData = (*cmd0)->words.w1;
            // TODO: wtf??
            (*cmd0)->words.w1 = (uintptr_t)tex;

            // if (ourHash != (uint64_t)-1) {
            //     auto res = ResourceLoad(ourHash);
            // }

            (*cmd0)++;
        }

        (*cmd0)--;
        F3DGfx* cmd = (*cmd0);
        uint32_t fmt = C0(21, 3);
        uint32_t size = C0(19, 2);
        uint32_t width = C0(0, 12) + 1;

        if (tex != NULL) {
            Interpreter* gfx = GfxInstance();
            gfx->GfxDpSetTextureImage(fmt, size, width, fileName, texFlags, rawTexMetadata, tex);
        }
    } else {
        SPDLOG_ERROR("G_SETTIMG_OTR_HASH: Texture is null");
    }

    (*cmd0)++;
    return false;
}

bool gfx_set_timg_otr_filepath_handler_custom(F3DGfx** cmd0) {
    F3DGfx* cmd = *cmd0;
    const char* fileName = (char*)cmd->words.w1;

    uint32_t texFlags = 0;
    RawTexMetadata rawTexMetadata = {};

    std::shared_ptr<Fast::Texture> texture = std::static_pointer_cast<Fast::Texture>(
        Ship::Context::GetRawInstance()->GetResourceManager()->LoadResourceProcess(fileName));
    if (texture != nullptr) {
        Interpreter* gfx = GfxInstance();
        texFlags = texture->Flags;
        rawTexMetadata.width = texture->Width;
        rawTexMetadata.height = texture->Height;
        rawTexMetadata.h_byte_scale = texture->HByteScale;
        rawTexMetadata.v_pixel_scale = texture->VPixelScale;
        rawTexMetadata.type = texture->Type;
        rawTexMetadata.resource = texture;

        uint32_t fmt = C0(21, 3);
        uint32_t size = C0(19, 2);
        uint32_t width = C0(0, 12) + 1;

        gfx->GfxDpSetTextureImage(fmt, size, width, fileName, texFlags, rawTexMetadata,
                                  reinterpret_cast<char*>(texture->ImageData));
    } else {
        SPDLOG_ERROR("G_SETTIMG_OTR_FILEPATH: Texture is null");
    }
    return false;
}

bool gfx_set_fb_handler_custom(F3DGfx** cmd0) {
    F3DGfx* cmd = *cmd0;
    Interpreter* gfx = GfxInstance();
    gfx->Flush();

    if (cmd->words.w1) {
        gfx->SetFrameBuffer((int32_t)cmd->words.w1, 1.0f);
        gfx->mActiveFrameBuffer = gfx->mFrameBuffers.find((int32_t)cmd->words.w1);
        gfx->mFbActive = true;
    } else {
        gfx->ResetFrameBuffer();
        gfx->mFbActive = false;
        gfx->mActiveFrameBuffer = gfx->mFrameBuffers.end();
    }
    return false;
}

bool gfx_reset_fb_handler_custom(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    gfx->Flush();
#ifdef LUS_XBOX
    gfx->mHwVtxVersion++;
#endif
    gfx->mFbActive = false;
    gfx->mActiveFrameBuffer = gfx->mFrameBuffers.end();
    gfx->mRapi->StartDrawToFramebuffer(gfx->mRendersToFb ? gfx->mGameFb : 0,
                                       (float)gfx->mCurDimensions.height / gfx->mNativeDimensions.height);
    // Force viewport and scissor to reapply against the main framebuffer, in case a previous smaller
    // framebuffer truncated the values
    gfx->mRdp->viewport_or_scissor_changed = true;
    gfx->mRenderingState.viewport = {};
    gfx->mRenderingState.scissor = {};
    return false;
}

bool gfx_copy_fb_handler_custom(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;
    bool* hasCopiedPtr = (bool*)cmd->words.w1;

    gfx->Flush();
    gfx->CopyFrameBuffer(C0(11, 11), C0(0, 11), (bool)C0(22, 1), hasCopiedPtr);
    return false;
}

bool gfx_read_fb_handler_custom(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    int32_t width, height;
    [[maybe_unused]] int32_t ulx, uly;
    uint16_t* rgba16Buffer = (uint16_t*)cmd->words.w1;
    int fbId = C0(0, 8);
    bool bswap = C0(8, 1);
    ++(*cmd0);
    cmd = *cmd0;
    // Specifying the upper left origin value is unused and unsupported at the renderer level
    ulx = C0(0, 16);
    uly = C0(16, 16);
    width = C1(0, 16);
    height = C1(16, 16);

    gfx->Flush();
#ifdef LUS_XBOX
    gfx->mRapi->ReadFramebufferAtReplay(fbId, ulx, uly, width, height, rgba16Buffer, bswap);
    return false;
#endif
    gfx->mRapi->ReadFramebufferToCPU(fbId, width, height, rgba16Buffer);

#ifndef IS_BIGENDIAN
    // byteswap the output to BE
    if (bswap) {
        for (size_t i = 0; i < (size_t)width * height; i++) {
            rgba16Buffer[i] = BE16SWAP(rgba16Buffer[i]);
        }
    }
#endif

    return false;
}

bool gfx_register_blended_texture_handler_custom(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    // Flush incase we are replacing a previous blended texture that hasn't been finialized to the GPU
    gfx->Flush();

    char* timg = (char*)cmd->words.w1;

    ++(*cmd0);
    cmd = *cmd0;

    uint8_t* mask = (uint8_t*)cmd->words.w0;
    uint8_t* replacementTex = (uint8_t*)cmd->words.w1;

    if (!gfx_check_image_signature(timg)) {
        SPDLOG_ERROR(
            "OTR_G_REGBLENDEDTEX: Texture is not a valid OTR resource name, unable to register blended texture");
        return false;
    }

    // With no mask, we should clear the blended texture
    if (mask == nullptr) {
        gfx->UnregisterBlendedTexture(timg);
    } else {
        gfx->RegisterBlendedTexture(timg, mask, replacementTex);
    }

    return false;
}

bool gfx_set_timg_fb_handler_custom(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->Flush();
    gfx->mRapi->SelectTextureFb((uint32_t)cmd->words.w1);
    gfx->mRdp->textures_changed[0] = false;
    gfx->mRdp->textures_changed[1] = false;
    return false;
}

bool gfx_set_grayscale_handler_custom(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->mRdp->grayscale = cmd->words.w1;
    return false;
}

bool gfx_load_block_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxDpLoadBlock(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
    return false;
}

bool gfx_load_block_wide_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    uint32_t tile = cmd->words.w0 & 0x7;
    uint32_t lrs = cmd->words.w1;

    (*cmd0)++;
    cmd = *cmd0;

    uint32_t uls = (cmd->words.w0 >> 16) & 0xFFFF;
    uint32_t ult = (cmd->words.w0 >> 0) & 0xFFFF;
    uint32_t dxt = (cmd->words.w1 >> 0) & 0xFFF;

    gfx->GfxDpLoadBlock(tile, uls, ult, lrs, dxt);
    return false;
}

bool gfx_load_tile_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxDpLoadTile(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
    return false;
}

bool gfx_set_tile_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxDpSetTile(C0(21, 3), C0(19, 2), C0(9, 9), C0(0, 9), C1(24, 3), C1(20, 4), C1(18, 2), C1(14, 4), C1(10, 4),
                      C1(8, 2), C1(4, 4), C1(0, 4));
    return false;
}

bool gfx_set_tile_size_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxDpSetTileSize(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
    return false;
}

bool gfx_set_tile_size_interp_handler_rdp(F3DGfx** cmd0) {
    F3DGfx* cmd = *cmd0;
    Interpreter* gfx = GfxInstance();

    if (gfx->mInterpolationIndex == gfx->mInterpolationIndexTarget) {
        int tile = C1(24, 3);
        gfx->GfxDpSetTileSize(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
        ++(*cmd0);
        memcpy(&gfx->mRdp->texture_tile[tile].uls, &(*cmd0)->words.w0, sizeof(float));
        memcpy(&gfx->mRdp->texture_tile[tile].ult, &(*cmd0)->words.w1, sizeof(float));
        ++(*cmd0);
        memcpy(&gfx->mRdp->texture_tile[tile].lrs, &(*cmd0)->words.w0, sizeof(float));
        memcpy(&gfx->mRdp->texture_tile[tile].lrt, &(*cmd0)->words.w1, sizeof(float));
    } else {
        ++(*cmd0);
        ++(*cmd0);
    }

    return false;
}

bool gfx_set_tile_size_lerp_handler_rdp(F3DGfx** cmd0) {
    F3DGfx* cmd = *cmd0;
    Interpreter* gfx = GfxInstance();

    int tile = C1(24, 3);
    float coords[8];
    for (int i = 0; i < 8; i += 2) {
        ++(*cmd0);
        memcpy(&coords[i], &(*cmd0)->words.w0, sizeof(float));
        memcpy(&coords[i + 1], &(*cmd0)->words.w1, sizeof(float));
    }

    float t = gfx->mInterpolationT;
    gfx->mRdp->texture_tile[tile].uls = coords[0] + t * (coords[4] - coords[0]);
    gfx->mRdp->texture_tile[tile].ult = coords[1] + t * (coords[5] - coords[1]);
    gfx->mRdp->texture_tile[tile].lrs = coords[2] + t * (coords[6] - coords[2]);
    gfx->mRdp->texture_tile[tile].lrt = coords[3] + t * (coords[7] - coords[3]);
    gfx->mRdp->textures_changed[0] = true;
    gfx->mRdp->textures_changed[1] = true;

    return false;
}

bool gfx_set_interpolation_index_target(F3DGfx** cmd0) {
    F3DGfx* cmd = *cmd0;
    Interpreter* gfx = GfxInstance();

    gfx->mInterpolationIndexTarget = cmd->words.w1;
    return false;
}

bool gfx_load_tlut_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxDpLoadTlut(C1(24, 3), C1(14, 10));
    return false;
}

bool gfx_set_env_color_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxDpSetEnvColor(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
    return false;
}

bool gfx_set_prim_color_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxDpSetPrimColor(C0(8, 8), C0(0, 8), C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
    return false;
}

bool gfx_set_fog_color_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxDpSetFogColor(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
    return false;
}

// CENTER/SCALE and K4/K5 are wired as combiner inputs, so the standard (A-B)*C+D
// shader path covers their common uses. TODO: chroma-key width/threshold
// gating from G_SETKEYR/GB (wR/wG/wB ignored) and the YUV->RGB matrix K0..K3
// applied during texture sampling.
// G_SETKEYR: w1 = [wR:12 | cR:8 | sR:8]
bool gfx_set_key_r_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->mRdp->key_center.r = C1(8, 8);
    gfx->mRdp->key_scale.r = C1(0, 8);
    return false;
}

// G_SETKEYGB: w0 = [op:8 | wG:12 | _:4 | wB:12], w1 = [cG:8 | sG:8 | cB:8 | sB:8]
bool gfx_set_key_gb_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->mRdp->key_center.g = C1(24, 8);
    gfx->mRdp->key_scale.g = C1(16, 8);
    gfx->mRdp->key_center.b = C1(8, 8);
    gfx->mRdp->key_scale.b = C1(0, 8);
    return false;
}

// G_SETCONVERT: w0 = [op:8 | k0:9 | k1:9 | k2_hi:4], w1 = [k2_lo:5 | k3:9 | k4:9 | k5:9]
// K0..K5 are signed 9-bit values; sign-extend after decoding.
bool gfx_set_convert_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->mRdp->convert_k[0] = sign_extend_9(C0(13, 9));
    gfx->mRdp->convert_k[1] = sign_extend_9(C0(4, 9));
    // k2 is split across w0 and w1
    gfx->mRdp->convert_k[2] = sign_extend_9((C0(0, 4) << 5) | C1(27, 5));
    gfx->mRdp->convert_k[3] = sign_extend_9(C1(18, 9));
    gfx->mRdp->convert_k[4] = sign_extend_9(C1(9, 9));
    gfx->mRdp->convert_k[5] = sign_extend_9(C1(0, 9));
    return false;
}

bool gfx_set_blend_color_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxDpSetBlendColor(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
    return false;
}

bool gfx_set_fill_color_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxDpSetFillColor((uint32_t)cmd->words.w1);
    return false;
}

bool gfx_set_intensity_handler_custom(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxDpSetGrayscaleColor(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
    return false;
}

bool gfx_set_combine_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;

    gfx->GfxDpSetCombineMode(
        color_comb(C0(20, 4), C1(28, 4), C0(15, 5), C1(15, 3)), alpha_comb(C0(12, 3), C1(12, 3), C0(9, 3), C1(9, 3)),
        color_comb(C0(5, 4), C1(24, 4), C0(0, 5), C1(6, 3)), alpha_comb(C1(21, 3), C1(3, 3), C1(18, 3), C1(0, 3)));
    return false;
}

bool gfx_tex_rect_and_flip_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;
    int8_t opcode = (int8_t)(cmd->words.w0 >> 24);
    int32_t lrx, lry, tile, ulx, uly;
    uint32_t uls, ult, dsdx, dtdy;

    lrx = C0(12, 12);
    lry = C0(0, 12);
    tile = C1(24, 3);
    ulx = C1(12, 12);
    uly = C1(0, 12);
    // TODO make sure I don't need to increment cmd0
    ++(*cmd0);
    cmd = *cmd0;
    uls = C1(16, 16);
    ult = C1(0, 16);
    ++(*cmd0);
    cmd = *cmd0;
    dsdx = C1(16, 16);
    dtdy = C1(0, 16);

    gfx->GfxDpTextureRectangle(ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy, opcode == RDP_G_TEXRECTFLIP);
    return false;
}

bool gfx_tex_rect_wide_handler_custom(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;
    int8_t opcode = (int8_t)(cmd->words.w0 >> 24);
    int32_t lrx, lry, tile, ulx, uly;
    uint32_t uls, ult, dsdx, dtdy;

    lrx = static_cast<int32_t>((C0(0, 24) << 8)) >> 8;
    lry = static_cast<int32_t>((C1(0, 24) << 8)) >> 8;
    tile = C1(24, 3);
    ++(*cmd0);
    cmd = *cmd0;
    ulx = static_cast<int32_t>((C0(0, 24) << 8)) >> 8;
    uly = static_cast<int32_t>((C1(0, 24) << 8)) >> 8;
    ++(*cmd0);
    cmd = *cmd0;
    uls = C0(16, 16);
    ult = C0(0, 16);
    dsdx = C1(16, 16);
    dtdy = C1(0, 16);
    gfx->GfxDpTextureRectangle(ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy, opcode == RDP_G_TEXRECTFLIP);
    return false;
}

bool gfx_image_rect_handler_custom(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *cmd0;
    int16_t tile, iw, ih;
    int16_t x0, y0, s0, t0;
    int16_t x1, y1, s1, t1;
    tile = C0(0, 3);
    iw = C1(16, 16);
    ih = C1(0, 16);
    cmd = ++(*cmd0);
    x0 = C0(16, 16);
    y0 = C0(0, 16);
    s0 = C1(16, 16);
    t0 = C1(0, 16);
    cmd = ++(*cmd0);
    x1 = C0(16, 16);
    y1 = C0(0, 16);
    s1 = C1(16, 16);
    t1 = C1(0, 16);
    gfx->GfxDpImageRectangle(tile, iw, ih, x0, y0, s0, t0, x1, y1, s1, t1);

    return false;
}

bool gfx_fill_rect_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *(cmd0);

    gfx->GfxDpFillRectangle(C1(12, 12), C1(0, 12), C0(12, 12), C0(0, 12));
    return false;
}

bool gfx_fill_wide_rect_handler_custom(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *(cmd0);
    int32_t lrx, lry, ulx, uly;

    lrx = (int32_t)(C0(0, 24) << 8) >> 8;
    lry = (int32_t)(C1(0, 24) << 8) >> 8;
    cmd = ++(*cmd0);
    ulx = (int32_t)(C0(0, 24) << 8) >> 8;
    uly = (int32_t)(C1(0, 24) << 8) >> 8;
    gfx->GfxDpFillRectangle(ulx, uly, lrx, lry);

    return false;
}

bool gfx_SetScissor_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *(cmd0);

    gfx->GfxDpSetScissor(C1(24, 2), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
    return false;
}

bool gfx_set_z_img_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *(cmd0);

    gfx->GfxDpSetZImage(gfx->SegAddr(cmd->words.w1));
    return false;
}

bool gfx_set_c_img_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *(cmd0);

    gfx->GfxDpSetColorImage(C0(21, 3), C0(19, 2), C0(0, 11), gfx->SegAddr(cmd->words.w1));
    return false;
}

bool gfx_rdp_set_other_mode_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *(cmd0);

    gfx->GfxDpSetOtherMode(C0(0, 24), (uint32_t)cmd->words.w1);
    return false;
}

bool gfx_bg_copy_handler_s2dex(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *(cmd0);

    if (!gfx->mMarkerOn) {
        gfx->Gfxs2dexBgCopy((F3DuObjBg*)cmd->words.w1); // not gfx->SegAddr here it seems
    }
    return false;
}

bool gfx_bg_1cyc_handler_s2dex(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *(cmd0);

    gfx->Gfxs2dexBg1cyc((F3DuObjBg*)cmd->words.w1);
    return false;
}

bool gfx_obj_rectangle_handler_s2dex(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *(cmd0);

    if (!gfx->mMarkerOn) {
        gfx->Gfxs2dexRecyCopy((F3DuObjSprite*)cmd->words.w1); // not gfx->SegAddr here it seems
    }
    return false;
}

bool gfx_extra_geometry_mode_handler_custom(F3DGfx** cmd0) {
    Interpreter* gfx = GfxInstance();
    F3DGfx* cmd = *(cmd0);

    gfx->GfxSpExtraGeometryMode(~C0(0, 24), (uint32_t)cmd->words.w1);
    return false;
}

bool gfx_stubbed_command_handler(F3DGfx** cmd0) {
    return false;
}

bool gfx_spnoop_command_handler_f3dex2(F3DGfx** cmd0) {
    return false;
}

class UcodeHandler {
  public:
    inline constexpr UcodeHandler(
        std::initializer_list<std::pair<int8_t, std::pair<const char*, GfxOpcodeHandlerFunc>>> initializer) {
        std::fill(std::begin(mHandlers), std::end(mHandlers),
                  std::pair<const char*, GfxOpcodeHandlerFunc>(nullptr, nullptr));

        for (const auto& [opcode, handler] : initializer) {
            mHandlers[static_cast<uint8_t>(opcode)] = handler;
        }
    }

    inline bool contains(int8_t opcode) const {
        return mHandlers[static_cast<uint8_t>(opcode)].first != nullptr;
    }

    inline std::pair<const char*, GfxOpcodeHandlerFunc> at(int8_t opcode) const {
        return mHandlers[static_cast<uint8_t>(opcode)];
    }

  private:
    std::pair<const char*, GfxOpcodeHandlerFunc> mHandlers[std::numeric_limits<uint8_t>::max() + 1];
};

static constexpr UcodeHandler rdpHandlers = {
    { RDP_G_SETTARGETINTERPINDEX,
      { "G_SETTARGETINTERPINDEX", gfx_set_interpolation_index_target } }, // G_SETTARGETINTERPINDEX
    { RDP_G_SETTILESIZE_INTERP,
      { "G_SETTILESIZE_INTERP", gfx_set_tile_size_interp_handler_rdp } },                     // G_SETTILESIZE_INTERP
    { RDP_G_SETTILESIZE_LERP, { "G_SETTILESIZE_LERP", gfx_set_tile_size_lerp_handler_rdp } }, // G_SETTILESIZE_LERP
    { RDP_G_TEXRECT, { "G_TEXRECT", gfx_tex_rect_and_flip_handler_rdp } },                    // G_TEXRECT (-28)
    { RDP_G_TEXRECTFLIP, { "G_TEXRECTFLIP", gfx_tex_rect_and_flip_handler_rdp } },            // G_TEXRECTFLIP (-27)
    { RDP_G_RDPLOADSYNC, { "mRdpLOADSYNC", gfx_stubbed_command_handler } },                   // mRdpLOADSYNC (-26)
    { RDP_G_RDPPIPESYNC, { "mRdpPIPESYNC", gfx_stubbed_command_handler } },                   // mRdpPIPESYNC (-25)
    { RDP_G_RDPTILESYNC, { "mRdpTILESYNC", gfx_stubbed_command_handler } },                   // mRdpPIPESYNC (-24)
    { RDP_G_RDPFULLSYNC, { "mRdpFULLSYNC", gfx_stubbed_command_handler } },                   // mRdpFULLSYNC (-23)
    { RDP_G_SETKEYGB, { "G_SETKEYGB", gfx_set_key_gb_handler_rdp } },                         // G_SETKEYGB (-22)
    { RDP_G_SETKEYR, { "G_SETKEYR", gfx_set_key_r_handler_rdp } },                            // G_SETKEYR (-21)
    { RDP_G_SETCONVERT, { "G_SETCONVERT", gfx_set_convert_handler_rdp } },                    // G_SETCONVERT (-20)
    { RDP_G_SETSCISSOR, { "G_SETSCISSOR", gfx_SetScissor_handler_rdp } },                     // G_SETSCISSOR (-19)
    { RDP_G_SETPRIMDEPTH, { "G_SETPRIMDEPTH", gfx_set_prim_depth_handler_rdp } },             // G_SETPRIMDEPTH (-18)
    { RDP_G_RDPSETOTHERMODE, { "mRdpSETOTHERMODE", gfx_rdp_set_other_mode_rdp } },            // mRdpSETOTHERMODE (-17)
    { RDP_G_LOADTLUT, { "G_LOADTLUT", gfx_load_tlut_handler_rdp } },                          // G_LOADTLUT (-16)
    { RDP_G_SETTILESIZE, { "G_SETTILESIZE", gfx_set_tile_size_handler_rdp } },                // G_SETTILESIZE (-14)
    { RDP_G_LOADBLOCK, { "G_LOADBLOCK", gfx_load_block_handler_rdp } },                       // G_LOADBLOCK (-13)
    { RDP_G_LOADTILE, { "G_LOADTILE", gfx_load_tile_handler_rdp } },                          // G_LOADTILE (-12)
    { RDP_G_SETTILE, { "G_SETTILE", gfx_set_tile_handler_rdp } },                             // G_SETTILE (-11)
    { RDP_G_FILLRECT, { "G_FILLRECT", gfx_fill_rect_handler_rdp } },                          // G_FILLRECT (-10)
    { RDP_G_SETFILLCOLOR, { "G_SETFILLCOLOR", gfx_set_fill_color_handler_rdp } },             // G_SETFILLCOLOR (-9)
    { RDP_G_SETFOGCOLOR, { "G_SETFOGCOLOR", gfx_set_fog_color_handler_rdp } },                // G_SETFOGCOLOR (-8)
    { RDP_G_SETBLENDCOLOR, { "G_SETBLENDCOLOR", gfx_set_blend_color_handler_rdp } },          // G_SETBLENDCOLOR (-7)
    { RDP_G_SETPRIMCOLOR, { "G_SETPRIMCOLOR", gfx_set_prim_color_handler_rdp } },             // G_SETPRIMCOLOR (-6)
    { RDP_G_SETENVCOLOR, { "G_SETENVCOLOR", gfx_set_env_color_handler_rdp } },                // G_SETENVCOLOR (-5)
    { RDP_G_SETCOMBINE, { "G_SETCOMBINE", gfx_set_combine_handler_rdp } },                    // G_SETCOMBINE (-4)
    { RDP_G_SETTIMG, { "G_SETTIMG", gfx_set_timg_handler_rdp } },                             // G_SETTIMG (-3)
    { RDP_G_SETZIMG, { "G_SETZIMG", gfx_set_z_img_handler_rdp } },                            // G_SETZIMG (-2)
    { RDP_G_SETCIMG, { "G_SETCIMG", gfx_set_c_img_handler_rdp } },                            // G_SETCIMG (-1)
};

static constexpr UcodeHandler otrHandlers = {
    { OTR_G_SETTIMG_OTR_HASH,
      { "G_SETTIMG_OTR_HASH", gfx_set_timg_otr_hash_handler_custom } },       // G_SETTIMG_OTR_HASH (0x20)
    { OTR_G_SETFB, { "G_SETFB", gfx_set_fb_handler_custom } },                // G_SETFB (0x21)
    { OTR_G_RESETFB, { "G_RESETFB", gfx_reset_fb_handler_custom } },          // G_RESETFB (0x22)
    { OTR_G_SETTIMG_FB, { "G_SETTIMG_FB", gfx_set_timg_fb_handler_custom } }, // G_SETTIMG_FB (0x23)
    { OTR_G_VTX_OTR_FILEPATH,
      { "G_VTX_OTR_FILEPATH", gfx_vtx_otr_filepath_handler_custom } }, // G_VTX_OTR_FILEPATH (0x24)
    { OTR_G_SETTIMG_OTR_FILEPATH,
      { "G_SETTIMG_OTR_FILEPATH", gfx_set_timg_otr_filepath_handler_custom } }, // G_SETTIMG_OTR_FILEPATH (0x25)
    { OTR_G_TRI1_OTR, { "G_TRI1_OTR", gfx_tri1_otr_handler_f3dex2 } },          // G_TRI1_OTR (0x26)
    { OTR_G_DL_OTR_FILEPATH, { "G_DL_OTR_FILEPATH", gfx_dl_otr_filepath_handler_custom } }, // G_DL_OTR_FILEPATH (0x27)
    { OTR_G_PUSHCD, { "G_PUSHCD", gfx_pushcd_handler_custom } },                            // G_PUSHCD (0x28)
    { OTR_G_MTX_OTR_FILEPATH,
      { "G_MTX_OTR_FILEPATH", gfx_mtx_otr_filepath_handler_custom } },          // G_MTX_OTR_FILEPATH (0x29)
    { OTR_G_DL_OTR_HASH, { "G_DL_OTR_HASH", gfx_dl_otr_hash_handler_custom } }, // G_DL_OTR_HASH (0x31)
    { OTR_G_VTX_OTR_HASH, { "G_VTX_OTR_HASH", gfx_vtx_hash_handler_custom } },  // G_VTX_OTR_HASH (0x32)
    { OTR_G_MARKER, { "G_MARKER", gfx_marker_handler_otr } },                   // G_MARKER (0X33)
    { OTR_G_INVALTEXCACHE, { "G_INVALTEXCACHE", gfx_invalidate_tex_cache_handler_f3dex2 } }, // G_INVALTEXCACHE (0X34)
    { OTR_G_BRANCH_Z_OTR, { "G_BRANCH_Z_OTR", gfx_branch_z_otr_handler_f3dex2 } },           // G_BRANCH_Z_OTR (0x35)
    { OTR_G_MTX_OTR, { "G_MTX_OTR", gfx_mtx_otr_handler_custom } },                          // G_MTX_OTR (0x36)
    { OTR_G_TEXRECT_WIDE, { "G_TEXRECT_WIDE", gfx_tex_rect_wide_handler_custom } },          // G_TEXRECT_WIDE (0x37)
    { OTR_G_FILLWIDERECT, { "G_FILLWIDERECT", gfx_fill_wide_rect_handler_custom } },         // G_FILLWIDERECT (0x38)
    { OTR_G_SETGRAYSCALE, { "G_SETGRAYSCALE", gfx_set_grayscale_handler_custom } },          // G_SETGRAYSCALE (0x39)
    { OTR_G_EXTRAGEOMETRYMODE,
      { "G_EXTRAGEOMETRYMODE", gfx_extra_geometry_mode_handler_custom } }, // G_EXTRAGEOMETRYMODE (0x3a)
    { OTR_G_COPYFB, { "G_COPYFB", gfx_copy_fb_handler_custom } },          // G_COPYFB (0x3b)
    { OTR_G_IMAGERECT, { "G_IMAGERECT", gfx_image_rect_handler_custom } }, // G_IMAGERECT (0x3c)
    { OTR_G_DL_INDEX, { "G_DL_INDEX", gfx_dl_index_handler } },            // G_DL_INDEX (0x3d)
    { OTR_G_READFB, { "G_READFB", gfx_read_fb_handler_custom } },          // G_READFB (0x3e)
    { OTR_G_REGBLENDEDTEX,
      { "G_REGBLENDEDTEX", gfx_register_blended_texture_handler_custom } },         // G_REGBLENDEDTEX (0x3f)
    { OTR_G_SETINTENSITY, { "G_SETINTENSITY", gfx_set_intensity_handler_custom } }, // G_SETINTENSITY (0x40)
    { OTR_G_MOVEMEM_HASH, { "OTR_G_MOVEMEM_HASH", gfx_movemem_handler_otr } },      // OTR_G_MOVEMEM_HASH
    { OTR_G_PUSH_SHADER, { "G_PUSH_SHADER", gfx_push_shader } },
    { OTR_G_POP_SHADER, { "G_POP_SHADER", gfx_pop_shader } },
    { RDP_G_LOADBLOCK_WIDE, { "G_LOADBLOCK_WIDE", gfx_load_block_wide_handler_rdp } }, // RDP_G_LOADBLOCK_WIDE (-15)
    { RDP_G_VTX_WIDE, { "G_VTX_WIDE", gfx_vtx_handler_f3dex2 } },                      // RDP_G_VTX_WIDE (-16)
    { RDP_G_TRI1_WIDE, { "G_TRI1_WIDE", gfx_tri1_handler_f3dex2 } },                   // RDP_G_TRI1_WIDE (-17)
};

static constexpr UcodeHandler f3dex2Handlers = {
    { F3DEX2_G_NOOP, { "G_NOOP", gfx_noop_handler_f3dex2 } },
    { F3DEX2_G_SPNOOP, { "G_SPNOOP", gfx_noop_handler_f3dex2 } },
    { F3DEX2_G_CULLDL, { "G_CULLDL", gfx_cull_dl_handler_f3dex2 } },
    { F3DEX2_G_MTX, { "G_MTX", gfx_mtx_handler_f3dex2 } },
    { F3DEX2_G_POPMTX, { "G_POPMTX", gfx_pop_mtx_handler_f3dex2 } },
    { F3DEX2_G_MOVEMEM, { "G_MOVEMEM", gfx_movemem_handler_f3dex2 } },
    { F3DEX2_G_MOVEWORD, { "G_MOVEWORD", gfx_moveword_handler_f3dex2 } },
    { F3DEX2_G_TEXTURE, { "G_TEXTURE", gfx_texture_handler_f3dex2 } },
    { F3DEX2_G_VTX, { "G_VTX", gfx_vtx_handler_f3dex2 } },
    { F3DEX2_G_MODIFYVTX, { "G_MODIFYVTX", gfx_modify_vtx_handler_f3dex2 } },
    { F3DEX2_G_DL, { "G_DL", gfx_dl_handler_common } },
    { F3DEX2_G_ENDDL, { "G_ENDDL", gfx_end_dl_handler_common } },
    { F3DEX2_G_GEOMETRYMODE, { "G_GEOMETRYMODE", gfx_geometry_mode_handler_f3dex2 } },
    { F3DEX2_G_TRI1, { "G_TRI1", gfx_tri1_handler_f3dex2 } },
    { F3DEX2_G_TRI2, { "G_TRI2", gfx_tri2_handler_f3dex } },
    { F3DEX2_G_QUAD, { "G_QUAD", gfx_quad_handler_f3dex2 } },
    { F3DEX2_G_SETOTHERMODE_L, { "G_SETOTHERMODE_L", gfx_othermode_l_handler_f3dex2 } },
    { F3DEX2_G_SETOTHERMODE_H, { "G_SETOTHERMODE_H", gfx_othermode_h_handler_f3dex2 } },
};

static constexpr UcodeHandler f3dexHandlers = {
    { F3DEX_G_NOOP, { "G_NOOP", gfx_noop_handler_f3dex2 } },
    { F3DEX_G_CULLDL, { "G_CULLDL", gfx_cull_dl_handler_f3dex2 } },
    { F3DEX_G_MTX, { "G_MTX", gfx_mtx_handler_f3d } },
    { F3DEX_G_POPMTX, { "G_POPMTX", gfx_pop_mtx_handler_f3d } },
    { F3DEX_G_MOVEMEM, { "G_POPMEM", gfx_movemem_handler_f3d } },
    { F3DEX_G_MOVEWORD, { "G_MOVEWORD", gfx_moveword_handler_f3d } },
    { F3DEX_G_TEXTURE, { "G_TEXTURE", gfx_texture_handler_f3d } },
    { F3DEX_G_SETOTHERMODE_L, { "G_SETOTHERMODE_L", gfx_othermode_l_handler_f3d } },
    { F3DEX_G_SETOTHERMODE_H, { "G_SETOTHERMODE_H", gfx_othermode_h_handler_f3d } },
    { F3DEX_G_SETGEOMETRYMODE, { "G_SETGEOMETRYMODE", gfx_set_geometry_mode_handler_f3dex } },
    { F3DEX_G_CLEARGEOMETRYMODE, { "G_CLEARGEOMETRYMODE", gfx_clear_geometry_mode_handler_f3dex } },
    { F3DEX_G_VTX, { "G_VTX", gfx_vtx_handler_f3dex } },
    { F3DEX_G_TRI1, { "G_TRI1", gfx_tri1_handler_f3dex } },
    { F3DEX_G_MODIFYVTX, { "G_MODIFYVTX", gfx_modify_vtx_handler_f3dex2 } },
    { F3DEX_G_DL, { "G_DL", gfx_dl_handler_common } },
    { F3DEX_G_ENDDL, { "G_ENDDL", gfx_end_dl_handler_common } },
    { F3DEX_G_TRI2, { "G_TRI2", gfx_tri2_handler_f3dex } },
    { F3DEX_G_SPNOOP, { "G_SPNOOP", gfx_spnoop_command_handler_f3dex2 } },
    { F3DEX_G_RDPHALF_1, { "mRdpHALF_1", gfx_stubbed_command_handler } },
    { F3DEX_G_QUAD, { "G_QUAD", gfx_quad_handler_f3dex } },
};

static constexpr UcodeHandler f3dHandlers = {
    { F3DEX_G_NOOP, { "G_NOOP", gfx_noop_handler_f3dex2 } },
    { F3DEX_G_CULLDL, { "G_CULLDL", gfx_cull_dl_handler_f3dex2 } },
    { F3DEX_G_MTX, { "G_MTX", gfx_mtx_handler_f3d } },
    { F3DEX_G_POPMTX, { "G_POPMTX", gfx_pop_mtx_handler_f3d } },
    { F3DEX_G_MOVEMEM, { "G_POPMEM", gfx_movemem_handler_f3d } },
    { F3DEX_G_MOVEWORD, { "G_MOVEWORD", gfx_moveword_handler_f3d } },
    { F3DEX_G_TEXTURE, { "G_TEXTURE", gfx_texture_handler_f3d } },
    { F3DEX_G_SETOTHERMODE_L, { "G_SETOTHERMODE_L", gfx_othermode_l_handler_f3d } },
    { F3DEX_G_SETOTHERMODE_H, { "G_SETOTHERMODE_H", gfx_othermode_h_handler_f3d } },
    { F3DEX_G_SETGEOMETRYMODE, { "G_SETGEOMETRYMODE", gfx_set_geometry_mode_handler_f3dex } },
    { F3DEX_G_CLEARGEOMETRYMODE, { "G_CLEARGEOMETRYMODE", gfx_clear_geometry_mode_handler_f3dex } },
    { F3DEX_G_VTX, { "G_VTX", gfx_vtx_handler_f3d } },
    { F3DEX_G_TRI1, { "G_TRI1", gfx_tri1_handler_f3d } },
    { F3DEX_G_MODIFYVTX, { "G_MODIFYVTX", gfx_modify_vtx_handler_f3dex2 } },
    { F3DEX_G_DL, { "G_DL", gfx_dl_handler_common } },
    { F3DEX_G_ENDDL, { "G_ENDDL", gfx_end_dl_handler_common } },
    { F3DEX_G_TRI2, { "G_TRI2", gfx_tri2_handler_f3dex } },
    { F3DEX_G_SPNOOP, { "G_SPNOOP", gfx_spnoop_command_handler_f3dex2 } },
    { F3DEX_G_RDPHALF_1, { "mRdpHALF_1", gfx_stubbed_command_handler } },
};

// LUSTODO: These S2DEX commands have different opcode numbers on F3DEX2 vs other ucodes. More research needs to be done
// to see if the implementations are different.
static constexpr UcodeHandler s2dexHandlers = {
    { F3DEX2_G_BG_COPY, { "G_BG_COPY", gfx_bg_copy_handler_s2dex } },
    { F3DEX2_G_BG_1CYC, { "G_BG_1CYC", gfx_bg_1cyc_handler_s2dex } },
    { F3DEX2_G_OBJ_RENDERMODE, { "G_OBJ_RENDERMODE", gfx_stubbed_command_handler } },
    { F3DEX2_G_OBJ_RECTANGLE_R, { "G_OBJ_RECTANGLE_R", gfx_stubbed_command_handler } },
    { F3DEX2_G_OBJ_RECTANGLE, { "G_OBJ_RECTANGLE", gfx_obj_rectangle_handler_s2dex } },
    { F3DEX2_G_DL, { "G_DL", gfx_dl_handler_common } },
    { F3DEX2_G_ENDDL, { "G_ENDDL", gfx_end_dl_handler_common } },
};

static constexpr std::array ucode_handlers = {
    &f3dHandlers,    // ucode_f3db
    &f3dHandlers,    // ucode_f3d
    &f3dexHandlers,  // ucode_f3dex
    &f3dexHandlers,  // ucode_f3dexb
    &f3dex2Handlers, // ucode_f3dex2
    &s2dexHandlers,  // ucode_s2dex
};

const char* GfxGetOpcodeName(int8_t opcode) {
    if (otrHandlers.contains(opcode)) {
        return otrHandlers.at(opcode).first;
    }

    if (rdpHandlers.contains(opcode)) {
        return rdpHandlers.at(opcode).first;
    }

    if (ucode_handler_index < ucode_handlers.size()) {
        if (ucode_handlers[ucode_handler_index]->contains(opcode)) {
            return ucode_handlers[ucode_handler_index]->at(opcode).first;
        } else {
            SPDLOG_CRITICAL("Unhandled OP code: 0x{:X}, for loaded ucode: {}", (uint8_t)opcode,
                            (uint32_t)ucode_handler_index);
        }
    } else {
        SPDLOG_CRITICAL("Unhandled OP code: 0x{:X}, invalid ucode: {}", (uint8_t)opcode, (uint32_t)ucode_handler_index);
    }

    return nullptr;
}

// TODO, implement a system where we can get the current opcode handler by writing to the GWords. If the powers that be
// are OK with that...
static void gfx_set_ucode_handler(UcodeHandlers ucode) {
    // Loaded ucode must be in range of the supported ucode_handlers
    assert(ucode < ucode_max);
    Interpreter* gfx = GfxInstance();
    ucode_handler_index = ucode;

    // Reset some RSP state values upon ucode load to deal with hardware quirks discovered by emulators
    switch (ucode) {
        case ucode_f3d:
        case ucode_f3db:
        case ucode_f3dex:
        case ucode_f3dexb:
        case ucode_f3dex2:
            gfx->mRsp->fog_mul = 0;
            gfx->mRsp->fog_offset = 0;
            break;
        default:
            break;
    }
}

#ifdef LUS_XBOX
static void gfx_step_impl();

// the commands that leave the triangles' state as it was (Interpreter::mHwFast): the triangles, vertices,
// display lists' calls and returns, matrices, syncs and no-ops; any other may set state
static bool XboxHandlerKeepsTriState(uint8_t opcode) {
    int8_t op = (int8_t)opcode;
    GfxOpcodeHandlerFunc h = nullptr;
    if (op == F3DEX2_G_LOAD_UCODE) {
        return false;
    } else if (otrHandlers.contains(op)) {
        h = otrHandlers.at(op).second;
    } else if (rdpHandlers.contains(op)) {
        h = rdpHandlers.at(op).second;
    } else if (ucode_handler_index < ucode_handlers.size() && ucode_handlers[ucode_handler_index]->contains(op)) {
        h = ucode_handlers[ucode_handler_index]->at(op).second;
    }
    return h == gfx_tri1_handler_f3dex2 || h == gfx_tri1_handler_f3dex || h == gfx_tri1_handler_f3d ||
           h == gfx_tri2_handler_f3dex || h == gfx_quad_handler_f3dex2 || h == gfx_quad_handler_f3dex ||
           h == gfx_tri1_otr_handler_f3dex2 || h == gfx_vtx_handler_f3dex2 || h == gfx_vtx_handler_f3dex ||
           h == gfx_vtx_handler_f3d || h == gfx_vtx_hash_handler_custom || h == gfx_vtx_otr_filepath_handler_custom ||
           h == gfx_modify_vtx_handler_f3dex2 || h == gfx_dl_handler_common || h == gfx_end_dl_handler_common ||
           h == gfx_dl_otr_hash_handler_custom || h == gfx_dl_otr_filepath_handler_custom ||
           h == gfx_dl_index_handler || h == gfx_branch_z_otr_handler_f3dex2 || h == gfx_cull_dl_handler_f3dex2 ||
           h == gfx_stubbed_command_handler || h == gfx_noop_handler_f3dex2 || h == gfx_marker_handler_otr ||
           h == gfx_mtx_handler_f3dex2 || h == gfx_mtx_handler_f3d || h == gfx_mtx_otr_handler_custom ||
           h == gfx_mtx_otr_filepath_handler_custom || h == gfx_pop_mtx_handler_f3dex2 ||
           h == gfx_pop_mtx_handler_f3d;
}

// the state setters that say whether they changed anything (Interpreter::mHwStateGen): a re-issue of
// the same values keeps the triangles' fast path
static bool XboxHandlerSaysIfChanged(uint8_t opcode) {
    int8_t op = (int8_t)opcode;
    GfxOpcodeHandlerFunc h = nullptr;
    if (otrHandlers.contains(op)) {
        h = otrHandlers.at(op).second;
    } else if (rdpHandlers.contains(op)) {
        h = rdpHandlers.at(op).second;
    } else if (ucode_handler_index < ucode_handlers.size() && ucode_handlers[ucode_handler_index]->contains(op)) {
        h = ucode_handlers[ucode_handler_index]->at(op).second;
    }
    return h == gfx_set_tile_handler_rdp || h == gfx_set_tile_size_handler_rdp || h == gfx_set_combine_handler_rdp ||
           h == gfx_set_prim_color_handler_rdp || h == gfx_set_env_color_handler_rdp ||
           h == gfx_set_fog_color_handler_rdp || h == gfx_othermode_l_handler_f3dex2 ||
           h == gfx_othermode_h_handler_f3dex2 || h == gfx_rdp_set_other_mode_rdp ||
           h == gfx_geometry_mode_handler_f3dex2 || h == gfx_texture_handler_f3dex2;
}

// the same, worked out once a microcode and opcode (~12,000 commands a frame)
static int XboxOpKind(uint8_t opcode) {
    static uint8_t sKnown[8][256]; // 0 not yet, 1 keeps the state, 2 says if it changed it, 3 may change it
    uint8_t& k = sKnown[ucode_handler_index < 8 ? ucode_handler_index : 7][opcode];
    if (k == 0) {
        k = XboxHandlerKeepsTriState(opcode) ? 1 : XboxHandlerSaysIfChanged(opcode) ? 2 : 3;
    }
    return k;
}

static void gfx_step() {
    uint8_t opcode = (uint8_t)(g_exec_stack.currCmd()->words.w0 >> 24);
    Interpreter* gfx = GfxInstance();
    int kind = XboxOpKind(opcode);
    uint32_t gen = gfx->mHwStateGen;
    const uint64_t t0 = XBOX_CYCLES();
    gfx_step_impl();
    if (kind == 3 || (kind == 2 && gfx->mHwStateGen != gen)) {
        sXboxFastEnders[opcode] += gfx->mHwFast ? 1 : 0;
        gfx->mHwFast = false;
    }
    if (sXboxProfileRun) {
        sXboxOpCycles[opcode] += XboxCycles() - t0;
        sXboxOpCount[opcode]++;
    }
}

static void gfx_step_impl() {
#else
static void gfx_step() {
#endif
    auto& cmd = g_exec_stack.currCmd();
    auto cmd0 = cmd;
    int8_t opcode = (int8_t)(cmd->words.w0 >> 24);

#ifdef USE_GBI_TRACE
    if (cmd->words.trace.valid &&
        Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger("gEnableGFXTrace", 0)) {
#define TRACE                                  \
    "\n====================================\n" \
    " - CMD: {:02X}\n"                         \
    " - Path: {}:{}\n"                         \
    " - W0: {:08X}\n"                          \
    " - W1: {:08X}\n"                          \
    "===================================="
        SPDLOG_INFO(TRACE, (uint8_t)opcode, cmd->words.trace.file, cmd->words.trace.idx, cmd->words.w0, cmd->words.w1);
    }
#endif

    if (opcode == F3DEX2_G_LOAD_UCODE) {
        gfx_set_ucode_handler((UcodeHandlers)(cmd->words.w0 & 0xFFFFFF));
        ++cmd;
        return;
        // Instead of having a handler for each ucode for switching ucode, just check for it early and return.
    }

    if (otrHandlers.contains(opcode)) {
        // OTR filepath handlers expect w1 to be a valid string pointer.
        // Guard against null or N64-segment addresses that would crash in strlen/strncmp.
        if (opcode == OTR_G_VTX_OTR_FILEPATH || opcode == OTR_G_SETTIMG_OTR_FILEPATH ||
            opcode == OTR_G_DL_OTR_FILEPATH || opcode == OTR_G_PUSHCD || opcode == OTR_G_MTX_OTR_FILEPATH) {
            uintptr_t w1 = (uintptr_t)cmd->words.w1;
            if (w1 < 0x10000
#if UINTPTR_MAX > 0xFFFFFFFFu
                // On 64-bit: filter kernel/sentinel addresses.
                || w1 > 0x0000FFFFFFFFFFFFull
#endif
            ) {
                ++g_exec_stack.currCmd();
                return;
            }
        }
        if (otrHandlers.at(opcode).second(&cmd)) {
            return;
        }
    } else if (rdpHandlers.contains(opcode)) {
        if (rdpHandlers.at(opcode).second(&cmd)) {
            return;
        }
    } else if (ucode_handler_index < ucode_handlers.size()) {
        if (ucode_handlers[ucode_handler_index]->contains(opcode)) {
            if (ucode_handlers[ucode_handler_index]->at(opcode).second(&cmd)) {
                return;
            }
        } else {
            SPDLOG_CRITICAL("Unhandled OP code: 0x{:X}, for loaded ucode: {}", (uint8_t)opcode,
                            (uint32_t)ucode_handler_index);
        }
    } else {
        SPDLOG_CRITICAL("Unhandled OP code: 0x{:X}, invalid ucode: {}", (uint8_t)opcode, (uint32_t)ucode_handler_index);
    }

    ++cmd;
}

void Interpreter::SpReset() {
#ifdef LUS_XBOX
    mHwVtxVersion++;
    mHwStateGen++;
    mHwRecipeNp = 0;
    memset(mHwRecipeNmv, 0, sizeof(mHwRecipeNmv));
#endif
    while (!mShaderStack.empty()) {
        mShaderStack.pop();
    }
    mRsp->modelview_matrix_stack_size = 1;
    mRsp->current_num_lights = 2;
    mRsp->lights_changed = true;
    // Transposed from the SDK basis on purpose: lookat[0] is LOOKATX and drives S.
    // Only games that never send a lookat see these, and MK64 matches output with them.
    // TODO: Use ucode DMEM defaults from hardware.
    mRsp->lookat[0].dir[0] = 0;
    mRsp->lookat[0].dir[1] = 127;
    mRsp->lookat[0].dir[2] = 0;
    mRsp->lookat[1].dir[0] = 127;
    mRsp->lookat[1].dir[1] = 0;
    mRsp->lookat[1].dir[2] = 0;
    CalculateNormalDir(&mRsp->lookat[0], mRsp->current_lookat_coeffs[0]);
    CalculateNormalDir(&mRsp->lookat[1], mRsp->current_lookat_coeffs[1]);
}

void Interpreter::RegisterFbTexture(const void* cpuAddr, int fbId) {
    mFbTextures[(uintptr_t)cpuAddr] = fbId;
}

void Interpreter::UnregisterFbTexture(const void* cpuAddr) {
    mFbTextures.erase((uintptr_t)cpuAddr);
}

void Interpreter::GetDimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) {
    mWapi->GetDimensions(width, height, posX, posY);
}

void Interpreter::Init(class GfxWindowBackend* wapi, class GfxRenderingAPI* rapi, const char* game_name,
                       bool start_in_fullscreen, uint32_t width, uint32_t height, uint32_t posX, uint32_t posY) {
    mWapi = wapi;
    mRapi = rapi;
    mWapi->Init(game_name, rapi->GetName(), start_in_fullscreen, width, height, posX, posY);
    mRapi->Init();
    mRapi->UpdateFramebufferParameters(0, width, height, 1, false, true, true, true);
    mCurDimensions.internal_mul =
        Ship::Context::GetRawInstance()->GetConsoleVariables()->GetFloat(CVAR_INTERNAL_RESOLUTION, 1);
    mMsaaLevel = Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(CVAR_MSAA_VALUE, 1);

    mCurDimensions.width = width;
    mCurDimensions.height = height;

    mGameFb = mRapi->CreateFramebuffer();
    mGameFbMsaaResolved = mRapi->CreateFramebuffer();

    mNativeDimensions.width = SCREEN_WIDTH;
    mNativeDimensions.height = SCREEN_HEIGHT;

    for (int i = 0; i < MAX_SEGMENT_POINTERS; i++) {
        mSegmentPointers[i] = 0;
    }

    if (mTexUploadBuffer == nullptr) {
        // We cap texture max to 8k, because why would you need more?
        int max_tex_size = std::min(8192, mRapi->GetMaxTextureSize());
        mTexUploadBuffer = (uint8_t*)malloc(max_tex_size * max_tex_size * 4);
    }

    ucode_handler_index = UcodeHandlers::ucode_f3dex2;

    // Pre-allocate texture cache buckets to prevent rehash-induced iterator invalidation.
    mTextureCache.map.reserve(TEXTURE_CACHE_MAX_SIZE);
}

void Interpreter::Destroy() {
    // TODO: should also destroy rapi, and any other resources acquired in fast3d
    free(mTexUploadBuffer);
    mWapi->Destroy();

    // Texture cache and loaded textures store references to Resources which need to be unreferenced.
    TextureCacheClear();
    mRdp->texture_to_load.raw_tex_metadata.resource = nullptr;
    mRdp->loaded_texture[0].raw_tex_metadata.resource = nullptr;
    mRdp->loaded_texture[1].raw_tex_metadata.resource = nullptr;
}

GfxRenderingAPI* Interpreter::GetCurrentRenderingAPI() {
    return mRapi;
}

void Interpreter::SetGfxDebugger(std::shared_ptr<GfxDebugger> debugger) {
    mGfxDebugger = std::move(debugger);
}

std::shared_ptr<GfxDebugger> Interpreter::GetGfxDebugger() const {
    return mGfxDebugger;
}

void Interpreter::HandleWindowEvents() {
    mWapi->HandleEvents();
}

bool Interpreter::IsFrameReady() {
    return mWapi->IsFrameReady();
}

bool Interpreter::ViewportMatchesRendererResolution() {
#ifdef __APPLE__
    // Always treat the viewport as not matching the render resolution on mac
    // to avoid issues with retina scaling.
    return false;
#else
    if (mCurDimensions.width == mGameWindowViewport.width && mCurDimensions.height == mGameWindowViewport.height) {
        return true;
    }
    return false;
#endif
}

void Interpreter::StartFrame() {
    mWapi->GetDimensions(&mGfxCurrentWindowDimensions.width, &mGfxCurrentWindowDimensions.height, &mCurWindowPosX,
                         &mCurWindowPosY);
    if (mCurDimensions.height == 0) {
        // Avoid division by zero
        mCurDimensions.height = 1;
    }
#ifdef LUS_NO_IMGUI
    // No GUI lays out a game window: the game fills the screen at the screen's resolution, drawn
    // straight into the back buffer (no game framebuffer, no copy).
    mGameWindowViewport.x = 0;
    mGameWindowViewport.y = 0;
    mGameWindowViewport.width = mGfxCurrentWindowDimensions.width;
    mGameWindowViewport.height = mGfxCurrentWindowDimensions.height;
    mCurDimensions.width = mGfxCurrentWindowDimensions.width;
    mCurDimensions.height = mGfxCurrentWindowDimensions.height;
#endif
    mCurDimensions.aspect_ratio = (float)mCurDimensions.width / (float)mCurDimensions.height;

    // Update the framebuffer sizes when the viewport or native dimension changes
    if (mCurDimensions.width != mPrvDimensions.width || mCurDimensions.height != mPrvDimensions.height ||
        mNativeDimensions.width != mPrevNativeDimensions.width ||
        mNativeDimensions.height != mPrevNativeDimensions.height) {

        for (auto& fb : mFrameBuffers) {
            uint32_t width = fb.second.orig_width, height = fb.second.orig_height;
            if (fb.second.resize) {
                AdjustWidthHeightForScale(width, height, fb.second.native_width, fb.second.native_height);
            }
            if (width != fb.second.applied_width || height != fb.second.applied_height) {
                mRapi->UpdateFramebufferParameters(fb.first, width, height, 1, true, true, true, true);
                fb.second.applied_width = width;
                fb.second.applied_height = height;
            }
        }
    }

    mPrvDimensions = mCurDimensions;
    mPrevNativeDimensions = mNativeDimensions;
    if (!ViewportMatchesRendererResolution() || mMsaaLevel > 1) {
        mRendersToFb = true;
        if (!ViewportMatchesRendererResolution()) {
            mRapi->UpdateFramebufferParameters(mGameFb, mCurDimensions.width, mCurDimensions.height, mMsaaLevel, true,
                                               true, true, true);
        } else {
            // MSAA framebuffer needs to be resolved to an equally sized target when complete, which must therefore
            // match the window size
            mRapi->UpdateFramebufferParameters(mGameFb, mGfxCurrentWindowDimensions.width,
                                               mGfxCurrentWindowDimensions.height, mMsaaLevel, false, true, true, true);
        }
        if (mMsaaLevel > 1 && !ViewportMatchesRendererResolution()) {
            mRapi->UpdateFramebufferParameters(mGameFbMsaaResolved, mCurDimensions.width, mCurDimensions.height, 1,
                                               false, false, false, false);
        }
    } else {
        mRendersToFb = false;
    }

    mFbActive = false;
}

GfxExecStack g_exec_stack = {};

void Interpreter::RunGuiOnly() {
    SpReset();

    mGetPixelDepthPending.clear();
    mGetPixelDepthCached.clear();

    mRapi->UpdateFramebufferParameters(0, mGfxCurrentWindowDimensions.width, mGfxCurrentWindowDimensions.height, 1,
                                       false, true, true, !mRendersToFb);
    mRapi->StartFrame();
    mRapi->StartDrawToFramebuffer(mRendersToFb ? mGameFb : 0, (float)mCurDimensions.height / mNativeDimensions.height);
    mRapi->ClearFramebuffer(true, true);
    mRdp->viewport_or_scissor_changed = true;
    mRenderingState.viewport = {};
    mRenderingState.scissor = {};

    Flush();
    mGfxFrameBuffer = 0;

    if (mRendersToFb) {
        mRapi->StartDrawToFramebuffer(0, 1);
        mRapi->ClearFramebuffer(true, true);
        if (mMsaaLevel > 1) {
            if (!ViewportMatchesRendererResolution()) {
                mRapi->ResolveMSAAColorBuffer(mGameFbMsaaResolved, mGameFb);
                mGfxFrameBuffer = (uintptr_t)mRapi->GetFramebufferTextureId(mGameFbMsaaResolved);
            } else {
                mRapi->ResolveMSAAColorBuffer(0, mGameFb);
            }
        } else {
            mGfxFrameBuffer = (uintptr_t)mRapi->GetFramebufferTextureId(mGameFb);
        }
    } else if (mFbActive) {
        // Failsafe reset to main framebuffer to prevent softlocking the renderer
        mFbActive = 0;
        mRapi->StartDrawToFramebuffer(0, 1);

        assert(0 && "active framebuffer was never reset back to original");
    }
}

#ifdef LUS_XBOX
extern "C" double gLusxCyclesPerMs; // xbox_main.cpp

// every 100 frames: ms a frame in the costliest commands, the draws and the imports
static void XboxLogProfile(uint64_t hwTriangles, uint64_t litTriangles, uint64_t cpuTriangles) {
    const double perMs = gLusxCyclesPerMs * sXboxRuns;
    const double opMs = gLusxCyclesPerMs; // the commands, draws and imports: the one profiled run
    int order[256];
    for (int i = 0; i < 256; i++) {
        order[i] = i;
    }
    auto opName = [](int op) {
        return otrHandlers.contains((int8_t)op)   ? otrHandlers.at((int8_t)op).first
               : rdpHandlers.contains((int8_t)op) ? rdpHandlers.at((int8_t)op).first
               : (ucode_handler_index < ucode_handlers.size() &&
                  ucode_handlers[ucode_handler_index]->contains((int8_t)op))
                   ? ucode_handlers[ucode_handler_index]->at((int8_t)op).first
                   : "?";
    };
    std::sort(order, order + 256, [](int a, int b) { return sXboxOpCycles[a] > sXboxOpCycles[b]; });
    std::string line = fmt::format(
        "lus-x: a frame's rendering {:.1f} ms: {} draws {:.1f} and {} GPU-transform draws {:.1f}, texture imports "
        "{:.1f}; triangles {} on the GPU's path ({} lit there, {} without their state worked out), {} on the CPU's",
        sXboxRunCycles / perMs, sXboxDraws / sXboxRuns, sXboxDrawCycles / opMs, sXboxHwDraws / sXboxRuns,
        sXboxHwDrawCycles / opMs, sXboxImportCycles / opMs, hwTriangles / sXboxRuns, litTriangles / sXboxRuns,
        sXboxFastTris / sXboxRuns, cpuTriangles / sXboxRuns);
    line += fmt::format(" (raw ones taken there: {} for the clamp ({} cut refused), {} blend colour, {} grayscale, {} "
                        "rectangles, {} other ({} over 4 inputs, {} LOD); {} of them culled; {} clamped ones across "
                        "limbs on the GPU)",
                        sXboxCpuWhy[0] / sXboxRuns, sXboxClampWhy[1] / sXboxRuns, sXboxCpuWhy[1] / sXboxRuns,
                        sXboxCpuWhy[2] / sXboxRuns, sXboxCpuWhy[3] / sXboxRuns, sXboxCpuWhy[4] / sXboxRuns,
                        sXboxHwWhy[0] / sXboxRuns, sXboxHwWhy[1] / sXboxRuns, sXboxCpuCulled / sXboxRuns,
                        sXboxClampWhy[0] / sXboxRuns);
    if (sXboxReplays != 0) {
        line += fmt::format("; {} states rebuilt a frame and {} triangles watched for cutting ({} out of sight, {} "
                            "cut), {:.2f} ms",
                            sXboxStatesRebuilt / sXboxReplays, sXboxClipWatched / sXboxReplays,
                            sXboxClipGone / sXboxReplays, sXboxClipCut / sXboxReplays,
                            sXboxRebuildSeconds * 1000.0 / sXboxReplays);
    }
    sXboxReplays = 0;
    sXboxStatesRebuilt = 0;
    sXboxClipWatched = 0;
    sXboxClipCut = 0;
    sXboxClipGone = 0;
    sXboxRebuildSeconds = 0.0;
    memset(sXboxCpuWhy, 0, sizeof(sXboxCpuWhy));
    memset(sXboxClampWhy, 0, sizeof(sXboxClampWhy));
    memset(sXboxHwWhy, 0, sizeof(sXboxHwWhy));
    sXboxCpuCulled = 0;
    for (int k = 0; k < 10 && sXboxOpCycles[order[k]] != 0; k++) {
        if (k == 0) {
            line += fmt::format("; commands, in a run of {:.1f} ms:", sXboxProfiledCycles / opMs);
        }
        int op = order[k];
        line += fmt::format(" {} {:.2f} ({}x)", opName(op), sXboxOpCycles[op] / opMs, sXboxOpCount[op]);
    }
    SPDLOG_INFO("{}", line); // (the log cuts long lines: the rest on its own)
    line = fmt::format("lus-x: triangles: {} dropped out of sight; slow path {:.2f} ms, cutting {:.2f} ms ({} cut into "
                       "{}); not fast: {} after a state command, {} rectangles, {} other lights, {} cut refused; "
                       "ended by:",
                       sXboxDropped / sXboxRuns, sXboxSlowCycles / opMs, sXboxCutCycles / opMs, sXboxCuts / sXboxRuns,
                       sXboxPieces / sXboxRuns, sXboxSlowWhy[0] / sXboxRuns, sXboxSlowWhy[1] / sXboxRuns,
                       sXboxSlowWhy[2] / sXboxRuns, sXboxSlowWhy[3] / sXboxRuns);
    std::sort(order, order + 256, [](int a, int b) { return sXboxFastEnders[a] > sXboxFastEnders[b]; });
    for (int k = 0; k < 8 && sXboxFastEnders[order[k]] / sXboxRuns != 0; k++) {
        line += fmt::format(" {} {}", opName(order[k]), sXboxFastEnders[order[k]] / sXboxRuns);
    }
    sXboxDropped = sXboxCuts = sXboxPieces = 0;
    sXboxSlowCycles = sXboxCutCycles = 0;
    memset(sXboxSlowWhy, 0, sizeof(sXboxSlowWhy));
    memset(sXboxFastEnders, 0, sizeof(sXboxFastEnders));
    SPDLOG_INFO("{}", line);
#if LUSX_CUT_CACHE
    SPDLOG_INFO("lus-x: cut cache: {} hits, {} misses, {} lit here a run, {} KB used, {} wipes; {} checked, {} differ",
                sHwCutHits / sXboxRuns, sHwCutMisses / sXboxRuns, sHwCutSkipped / sXboxRuns,
                sHwCutCache.UsedBytes() >> 10, sHwCutCache.TakeWipes(), sHwCutChecked, sHwCutDiffer);
    sHwCutHits = sHwCutMisses = sHwCutSkipped = 0;
#endif
    memset(sXboxOpCycles, 0, sizeof(sXboxOpCycles));
    memset(sXboxOpCount, 0, sizeof(sXboxOpCount));
    sXboxDrawCycles = sXboxImportCycles = sXboxRunCycles = sXboxHwDrawCycles = sXboxProfiledCycles = 0;
    sXboxRuns = sXboxDraws = sXboxHwDraws = sXboxFastTris = 0;
}
#endif

void Interpreter::Run(Gfx* commands, const std::unordered_map<Mtx*, MtxF>& mtx_replacements) {
#ifdef LUS_XBOX
    LusXbox_Where("rendering: the display list");
    uint64_t xboxRunStart = XboxCycles();
    sXboxProfileRun = sXboxRuns == 50;
    mHwTnl = mRapi->SupportsHwTnl();
    mHwStates.clear(); // a frame's: its vertices index them
    mHwRecipes.clear();
    mHwClipTris.clear();
    mHwClipGroups.clear();
    mHwStateUsed.clear();
    mHwClipPending = 0;
    mHwBlendBatch.clear();
    mHwBlendRecorded.clear();
    mHwLights.clear();
    mHwTris = 0;
    mHwSlots = 0;
    mHwFast = false;
    mHwVtxVersion++;
#endif
    SpReset();

    mGetPixelDepthPending.clear();
    mGetPixelDepthCached.clear();

    mCurMtxReplacements = &mtx_replacements;

    mRapi->UpdateFramebufferParameters(0, mGfxCurrentWindowDimensions.width, mGfxCurrentWindowDimensions.height, 1,
                                       false, true, true, !mRendersToFb);
    mRapi->StartFrame();
    mRapi->StartDrawToFramebuffer(mRendersToFb ? mGameFb : 0, (float)mCurDimensions.height / mNativeDimensions.height);
    mRapi->ClearFramebuffer(true, true);
    mRdp->viewport_or_scissor_changed = true;
    mRenderingState.viewport = {};
    mRenderingState.scissor = {};

    auto dbg = mGfxDebugger;
    g_exec_stack.start((F3DGfx*)commands);
    while (!g_exec_stack.cmd_stack.empty()) {
        auto cmd = g_exec_stack.cmd_stack.top();

        if (dbg->IsDebugging()) {
            g_exec_stack.gfx_path.push_back(cmd);
            if (dbg->HasBreakPoint(g_exec_stack.gfx_path)) {
                // On a breakpoint with the active framebuffer still set, we need to reset back to prevent
                // soft locking the renderer
                if (mFbActive) {
                    mFbActive = 0;
                    mRapi->StartDrawToFramebuffer(mRendersToFb ? mGameFb : 0, 1);
                }

                break;
            }
            g_exec_stack.gfx_path.pop_back();
        }
        gfx_step();
    }

#ifdef LUS_XBOX
    LusXbox_Where("rendering: the display list's last draw");
#endif
    Flush();
#ifdef LUS_XBOX
    sBlendRangeCount = 0;
#endif
    mGfxFrameBuffer = 0;
    currentDir = std::stack<std::string>();

    if (mRendersToFb) {
        mRapi->StartDrawToFramebuffer(0, 1);
        mRapi->ClearFramebuffer(true, true);
        if (mMsaaLevel > 1) {
            if (!ViewportMatchesRendererResolution()) {
                mRapi->ResolveMSAAColorBuffer(mGameFbMsaaResolved, mGameFb);
                mGfxFrameBuffer = (uintptr_t)mRapi->GetFramebufferTextureId(mGameFbMsaaResolved);
            } else {
                mRapi->ResolveMSAAColorBuffer(0, mGameFb);
            }
        } else {
            mGfxFrameBuffer = (uintptr_t)mRapi->GetFramebufferTextureId(mGameFb);
        }
    } else if (mFbActive) {
        // Failsafe reset to main framebuffer to prevent softlocking the renderer
        mFbActive = 0;
        mRapi->StartDrawToFramebuffer(0, 1);

        assert(0 && "active framebuffer was never reset back to original");
    }
#ifdef LUS_XBOX
    sXboxRunCycles += XboxCycles() - xboxRunStart;
    if (sXboxProfileRun) {
        sXboxProfiledCycles = XboxCycles() - xboxRunStart;
        sXboxProfileRun = false;
    }
    if (++sXboxRuns == 100) {
        XboxLogProfile(mHwTriangles, mHwLitTriangles, mCpuTriangles);
        mHwTriangles = mHwLitTriangles = mCpuTriangles = 0;
    }
#endif
}

void Interpreter::EndFrame() {
#ifdef LUS_XBOX
    LusXbox_Where("rendering: presenting");
#endif
    mRapi->EndFrame();
    mWapi->SwapBuffersBegin();
    mRapi->FinishRender();
    mWapi->SwapBuffersEnd();
}

void gfx_set_target_ucode(UcodeHandlers ucode) {
    ucode_handler_index = ucode;
}

int Interpreter::GetTargetFps() {
    return mWapi->GetTargetFps();
}

void Interpreter::SetTargetFps(int fps) {
    mWapi->SetTargetFps(fps);
}

void Interpreter::SetMaxFrameLatency(int latency) {
    mWapi->SetMaxFrameLatency(latency);
}

int Interpreter::CreateFrameBuffer(uint32_t width, uint32_t height, uint32_t native_width, uint32_t native_height,
                                   uint8_t resize, bool forceFixedAspect) {
    uint32_t orig_width = width, orig_height = height;
    if (resize) {
        AdjustWidthHeightForScale(width, height, native_width, native_height);
    }

    int fb = mRapi->CreateFramebuffer();
    mRapi->UpdateFramebufferParameters(fb, width, height, 1, true, true, true, true);

    mFrameBuffers[fb] = {
        orig_width, orig_height, width, height, native_width, native_height, static_cast<bool>(resize), forceFixedAspect
    };
    return fb;
}

void Interpreter::SetFrameBuffer(int fb, float noiseScale) {
#ifdef LUS_XBOX
    mHwVtxVersion++; // (the aspect)
#endif
    mRapi->StartDrawToFramebuffer(fb, noiseScale);
    mRapi->ClearFramebuffer(false, true);
}

void Interpreter::CopyFrameBuffer(int fb_dst_id, int fb_src_id, bool copyOnce, bool* hasCopiedPtr) {
    // Do not copy again if we have already copied before
    if (copyOnce && hasCopiedPtr != nullptr && *hasCopiedPtr) {
        return;
    }

    if (fb_src_id == 0 && mRendersToFb) {
        // read from the framebuffer we've been rendering to
        fb_src_id = mGameFb;
    }

    int srcX0, srcY0, srcX1, srcY1;
    int dstX0, dstY0, dstX1, dstY1;

    // When rendering to the main window buffer or MSAA is enabled with a buffer size equal to the view port,
    // then the source coordinates must account for any docked ImGui elements
    if (fb_src_id == 0 || (mMsaaLevel > 1 && mCurDimensions.width == mGameWindowViewport.width &&
                           mCurDimensions.height == mGameWindowViewport.height)) {
        srcX0 = mGameWindowViewport.x;
        srcY0 = mGameWindowViewport.y;
        srcX1 = mGameWindowViewport.x + mGameWindowViewport.width;
        srcY1 = mGameWindowViewport.y + mGameWindowViewport.height;
    } else {
        srcX0 = 0;
        srcY0 = 0;
        srcX1 = mCurDimensions.width;
        srcY1 = mCurDimensions.height;
    }

    dstX0 = 0;
    dstY0 = 0;
    dstX1 = mCurDimensions.width;
    dstY1 = mCurDimensions.height;

#ifdef LUS_XBOX
    mRapi->NextCopyOnceATick(copyOnce);
#endif
    mRapi->CopyFramebuffer(fb_dst_id, fb_src_id, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1);

    // Set the copied pointer if we have one
    if (hasCopiedPtr != nullptr) {
        *hasCopiedPtr = true;
    }
}

void Interpreter::ResetFrameBuffer() {
#ifdef LUS_XBOX
    mHwVtxVersion++;
#endif
    mRapi->StartDrawToFramebuffer(0, (float)mCurDimensions.height / mNativeDimensions.height);
}

void Interpreter::AdjustPixelDepthCoordinates(float& x, float& y) {
    x = x * RATIO_X(mActiveFrameBuffer, mCurDimensions) -
        (mNativeDimensions.width * RATIO_X(mActiveFrameBuffer, mCurDimensions) - mCurDimensions.width) / 2;
    y *= RATIO_Y(mActiveFrameBuffer, mCurDimensions);
    if (!mRendersToFb || (mMsaaLevel > 1 && mCurDimensions.width == mGameWindowViewport.width &&
                          mCurDimensions.height == mGameWindowViewport.height)) {
        x += mGameWindowViewport.x;
        y += mGfxCurrentWindowDimensions.height - (mGameWindowViewport.y + mGameWindowViewport.height);
    }
}

void Interpreter::GetPixelDepthPrepare(float x, float y) {
    AdjustPixelDepthCoordinates(x, y);
    mGetPixelDepthPending.emplace(x, y);
}

uint16_t Interpreter::GetPixelDepth(float x, float y) {
    AdjustPixelDepthCoordinates(x, y);

    if (auto it = mGetPixelDepthCached.find(std::make_pair(x, y)); it != mGetPixelDepthCached.end()) {
        return it->second;
    }

    mGetPixelDepthPending.emplace(x, y);

    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff> res =
        mRapi->GetPixelDepth(mRendersToFb ? mGameFb : 0, mGetPixelDepthPending);
    mGetPixelDepthCached.merge(res);
    mGetPixelDepthPending.clear();

    return mGetPixelDepthCached.find(std::make_pair(x, y))->second;
}

void gfx_push_current_dir(char* path) {
    if (gfx_check_image_signature(path) == 1)
        path = &path[7];

    currentDir.push(GetPathWithoutFileName(path));
}

int32_t gfx_check_image_signature(const char* imgData) {
    uintptr_t i = (uintptr_t)(imgData);

    if ((i & 1) == 1) {
        return 0;
    }

    // Filter addresses that are obviously not valid string pointers before
    // attempting to dereference for the "__OTR__" check.
    if (i == 0 || i < 0x10000) {
        return 0;
    }
#if UINTPTR_MAX > 0xFFFFFFFFu
    // On 64-bit: filter kernel/sentinel addresses. Upper bound covers all
    // user-space layouts (x86_64 47-bit canonical, ARM64 48-bit VA, etc.).
    if (i > 0x0000FFFFFFFFFFFFull) {
        return 0;
    }
#endif

    return Ship::Context::GetRawInstance()->GetResourceManager()->OtrSignatureCheck(imgData);
}

void Interpreter::RegisterBlendedTexture(const char* name, uint8_t* mask, uint8_t* replacement) {
    if (gfx_check_image_signature(name)) {
        name += 7;
    }

    if (gfx_check_image_signature(reinterpret_cast<char*>(replacement))) {
        Fast::Texture* tex = std::static_pointer_cast<Fast::Texture>(
                                 Ship::Context::GetRawInstance()->GetResourceManager()->LoadResourceProcess(
                                     reinterpret_cast<char*>(replacement)))
                                 .get();

        replacement = tex->ImageData;
    }

    mMaskedTextures[name] = MaskedTextureEntry{ mask, replacement };
}

void Interpreter::UnregisterBlendedTexture(const char* name) {
    if (gfx_check_image_signature(name)) {
        name += 7;
    }

    mMaskedTextures.erase(name);
}

// New getters and setters
void Interpreter::SetNativeDimensions(float width, float height) {
    mNativeDimensions.width = width;
    mNativeDimensions.height = height;
}

void Interpreter::SetResolutionMultiplier(float multiplier) {
    mCurDimensions.internal_mul = multiplier;
}

void Interpreter::SetMsaaLevel(uint32_t level) {
    mMsaaLevel = level;
}

void Interpreter::GetCurDimensions(uint32_t* width, uint32_t* height) {
    *width = mCurDimensions.width;
    *height = mCurDimensions.height;
}

} // namespace Fast

void gfx_cc_get_features(uint64_t shader_id0, uint64_t shader_id1, struct CCFeatures* cc_features) {
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) {
            for (int k = 0; k < 4; k++) {
                cc_features->c[i][j][k] = shader_id0 >> i * 32 + j * 16 + k * 4 & 0xf;
            }
        }
    }

    cc_features->opt_alpha = (shader_id1 & SHADER_OPT(ALPHA)) != 0;
    cc_features->opt_fog = (shader_id1 & SHADER_OPT(FOG)) != 0;
    cc_features->opt_texture_edge = (shader_id1 & SHADER_OPT(TEXTURE_EDGE)) != 0;
    cc_features->opt_noise = (shader_id1 & SHADER_OPT(NOISE)) != 0;
    cc_features->opt_2cyc = (shader_id1 & SHADER_OPT(_2CYC)) != 0;
    cc_features->opt_alpha_threshold = (shader_id1 & SHADER_OPT(ALPHA_THRESHOLD)) != 0;
    cc_features->opt_invisible = (shader_id1 & SHADER_OPT(INVISIBLE)) != 0;
    cc_features->opt_grayscale = (shader_id1 & SHADER_OPT(GRAYSCALE)) != 0;
    cc_features->opt_prim_depth = (shader_id1 & SHADER_OPT(PRIM_DEPTH)) != 0;

    cc_features->clamp[0][0] = shader_id1 & SHADER_OPT(TEXEL0_CLAMP_S);
    cc_features->clamp[0][1] = shader_id1 & SHADER_OPT(TEXEL0_CLAMP_T);
    cc_features->clamp[1][0] = shader_id1 & SHADER_OPT(TEXEL1_CLAMP_S);
    cc_features->clamp[1][1] = shader_id1 & SHADER_OPT(TEXEL1_CLAMP_T);

    cc_features->usedTextures[0] = false;
    cc_features->usedTextures[1] = false;
    cc_features->used_masks[0] = false;
    cc_features->used_masks[1] = false;
    cc_features->used_blend[0] = false;
    cc_features->used_blend[1] = false;
    cc_features->numInputs = 0;

    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 4; j++) {
                if (cc_features->c[c][i][j] >= SHADER_INPUT_1 && cc_features->c[c][i][j] <= SHADER_INPUT_7) {
                    if (cc_features->c[c][i][j] > cc_features->numInputs) {
                        cc_features->numInputs = cc_features->c[c][i][j];
                    }
                }
                if (cc_features->c[c][i][j] == SHADER_TEXEL0 || cc_features->c[c][i][j] == SHADER_TEXEL0A) {
                    cc_features->usedTextures[0] = true;
                    if (cc_features->opt_2cyc) {
                        cc_features->usedTextures[1] = true;
                    }
                }
                if (cc_features->c[c][i][j] == SHADER_TEXEL1 || cc_features->c[c][i][j] == SHADER_TEXEL1A) {
                    cc_features->usedTextures[1] = true;
                    if (cc_features->opt_2cyc) {
                        cc_features->usedTextures[0] = true;
                    }
                }
            }
        }
    }

    for (int c = 0; c < 2; c++) {
        cc_features->do_single[c][0] = cc_features->c[c][0][2] == SHADER_0;
        cc_features->do_single[c][1] = cc_features->c[c][1][2] == SHADER_0;
        cc_features->do_multiply[c][0] = cc_features->c[c][0][1] == SHADER_0 && cc_features->c[c][0][3] == SHADER_0;
        cc_features->do_multiply[c][1] = cc_features->c[c][1][1] == SHADER_0 && cc_features->c[c][1][3] == SHADER_0;
        cc_features->do_mix[c][0] = cc_features->c[c][0][1] == cc_features->c[c][0][3];
        cc_features->do_mix[c][1] = cc_features->c[c][1][1] == cc_features->c[c][1][3];
        cc_features->color_alpha_same[c] = (shader_id0 >> c * 32 & 0xffff) == (shader_id0 >> c * 32 + 16 & 0xffff);
    }

    if (cc_features->usedTextures[0] && shader_id1 & SHADER_OPT(TEXEL0_MASK)) {
        cc_features->used_masks[0] = true;
    }
    if (cc_features->usedTextures[1] && shader_id1 & SHADER_OPT(TEXEL1_MASK)) {
        cc_features->used_masks[1] = true;
    }

    if (cc_features->usedTextures[0] && shader_id1 & SHADER_OPT(TEXEL0_BLEND)) {
        cc_features->used_blend[0] = true;
    }
    if (cc_features->usedTextures[1] && shader_id1 & SHADER_OPT(TEXEL1_BLEND)) {
        cc_features->used_blend[1] = true;
    }

    cc_features->shader_id = Fast::ShaderIdUnmask(shader_id1);
}

extern "C" int gfx_create_framebuffer(uint32_t width, uint32_t height, uint32_t native_width, uint32_t native_height,
                                      uint8_t resize, bool forceFixedAspect) {
    return Fast::GfxInstance()->CreateFrameBuffer(width, height, native_width, native_height, resize,
                                                           forceFixedAspect);
}

extern "C" void gfx_texture_cache_clear() {
    Fast::GfxInstance()->TextureCacheClear();
}

extern "C" void gfx_shader_cache_clear() {
    auto instance = Fast::GfxInstance();
    instance->mColorCombinerPool.clear();
    instance->mPrevCombiner = Fast::GfxInstance()->mColorCombinerPool.end();
    instance->mRenderingState.mShaderProgram = nullptr;
    instance->mRapi->ClearShaderCache();
}

extern "C" void gfx_register_fb_texture(const void* cpuAddr, int fbId) {
    Fast::GfxInstance()->RegisterFbTexture(cpuAddr, fbId);
}

extern "C" void gfx_unregister_fb_texture(const void* cpuAddr) {
    Fast::GfxInstance()->UnregisterFbTexture(cpuAddr);
}