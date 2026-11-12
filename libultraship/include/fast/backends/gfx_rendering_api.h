#pragma once

#include <stdint.h>

#include <unordered_map>
#include <set>
#include "imconfig.h"

namespace Fast {
struct ShaderProgram;

struct GfxClipParameters {
    bool z_is_from_0_to_1;
    bool invertY;
};

enum FilteringMode { FILTER_THREE_POINT, FILTER_LINEAR, FILTER_NONE };

// A hash function used to hash a: pair<float, float>
struct hash_pair_ff {
    size_t operator()(const std::pair<float, float>& p) const {
        const auto hash1 = std::hash<float>{}(p.first);
        const auto hash2 = std::hash<float>{}(p.second);

        // If hash1 == hash2, their XOR is zero.
        return (hash1 != hash2) ? hash1 ^ hash2 : hash1;
    }
};

class GfxRenderingAPI {
  public:
    virtual ~GfxRenderingAPI() = default;
    virtual const char* GetName() = 0;
    virtual int GetMaxTextureSize() = 0;
    virtual GfxClipParameters GetClipParameters() = 0;
    virtual void UnloadShader(ShaderProgram* oldPrg) = 0;
    virtual void LoadShader(ShaderProgram* newPrg) = 0;
    virtual void ClearShaderCache() = 0;
    virtual ShaderProgram* CreateAndLoadNewShader(uint64_t shaderId0, uint64_t shaderId1) = 0;
    virtual ShaderProgram* LookupShader(uint64_t shaderId0, uint64_t shaderId1) = 0;
    virtual void ShaderGetInfo(ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) = 0;
    virtual uint32_t NewTexture() = 0;
    virtual void SelectTexture(int tile, uint32_t textureId) = 0;
    virtual void UploadTexture(const uint8_t* rgba32Buf, uint32_t width, uint32_t height) = 0;
    virtual void SetSamplerParameters(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt) = 0;
    virtual void SetDepthTestAndMask(bool depth_test, bool z_upd) = 0;
    virtual void SetZmodeDecal(bool decal) = 0;
    virtual void SetViewport(int x, int y, int width, int height) = 0;
    virtual void SetScissor(int x, int y, int width, int height) = 0;
    virtual void SetUseAlpha(bool useAlpha) = 0;
    virtual void DrawTriangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) = 0;
    virtual void Init() = 0;
    virtual void OnResize() = 0;
    virtual void StartFrame() = 0;
    virtual void EndFrame() = 0;
    virtual void FinishRender() = 0;
    virtual int CreateFramebuffer() = 0;
    virtual void UpdateFramebufferParameters(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level,
                                             bool opengl_invertY, bool render_target, bool has_depth_buffer,
                                             bool can_extract_depth) = 0;
    virtual void StartDrawToFramebuffer(int fbId, float noiseScale) = 0;
    virtual void CopyFramebuffer(int fbDstId, int fbSrcId, int srcX0, int srcY0, int srcX1, int srcY1, int dstX0,
                                 int dstY0, int dstX1, int dstY1) = 0;
    virtual void ClearFramebuffer(bool color, bool depth) = 0;
    virtual void ClearDepthRegion(int x, int y, int w, int h) {
        // Default: full depth clear. Backends that support scissored depth clears
        // (e.g. OpenGL) should override for a more precise partial clear.
        ClearFramebuffer(false, true);
    }
    virtual void ReadFramebufferToCPU(int fbId, uint32_t width, uint32_t height, uint16_t* rgba16Buf) = 0;
    virtual void ResolveMSAAColorBuffer(int fbIdTarger, int fbIdSrc) = 0;
    virtual std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
    GetPixelDepth(int fb_id, const std::set<std::pair<float, float>>& coordinates) = 0;
    virtual void* GetFramebufferTextureId(int fbId) = 0;
    virtual void SelectTextureFb(int fbId) = 0;
    virtual void DeleteTexture(uint32_t texId) = 0;
    virtual void SetTextureFilter(FilteringMode mode) = 0;
    virtual FilteringMode GetTextureFilter() = 0;
    virtual void SetSrgbMode() = 0;
    virtual ImTextureID GetTextureById(int id) = 0;
    virtual void SetCurrentPrimDepth(float depth) = 0;

#ifdef LUS_XBOX
    /**
     * @brief The GPU's transform (the Xbox's NV2A vertex shaders): the interpreter hands over the
     * N64's vertices as loaded, and the state each was loaded under, instead of transformed ones.
     */
    struct HwState {
        float mp[4][4];                       // the MP matrix (row vectors: pos = ob * mp)
        float aspect, fogMul, fogOffset, fog; // x's widescreen factor; fog's N64 parameters, and on (1)
        float mv[4][4];                       // the modelview (lighting's), rows
        float nscale;                         // 1 / its scale, or 0: not a rotation and a uniform scale
    };
    // the lights a lit batch's vertices were loaded under (the N64's: directions in the view's space,
    // positions too; colours 0..255)
    static constexpr int kHwMaxDirLights = 7, kHwMaxPointLights = 3;
    struct HwLights {
        int numDir, numPoint;
        float ambient[3];
        float dir[kHwMaxDirLights][3]; // unit
        float dirCol[kHwMaxDirLights][3];
        float pos[kHwMaxPointLights][3];
        float posCol[kHwMaxPointLights][3];
        float att[kHwMaxPointLights][2]; // attenuation: 1 + floor(dist) * [0] + floor(dist)^2 * [1]
    };
    // a vertex's slot is ob's w: slot * the stride (a lit batch's slots carry the modelview too)
    static constexpr int kHwStride = 5, kHwLitStride = 9, kHwMaxSlots = 16;
    struct HwBatch {
        const uint8_t* verts; // 16 bytes a vertex: ob x, y, z and slot * stride (shorts); u, v (shorts); shade
        size_t tris;          //   (a D3DCOLOR, alpha the vertex's own; lit, rgb the normal + 128)
        const HwState* states; // the interpreter's; slotStates says which are the slots
        int numStates;
        const HwLights* lights; // lit on the GPU, or null
        float texAffine[2][4];  // a texture's coordinate: u * [0] + [2], v * [1] + [3] (normalized)
        float inputC[4][4];     // a combiner input: C + shade * S, per channel
        float inputS[4][4];
        float shadeAlphaOne;    // shade's alpha reads 1 (fog in the blender)
        uint32_t fogColor;      // D3DCOLOR
        int cull;               // 0 none, 1 the screen's clockwise, 2 counter-clockwise
        int lightSet;           // the interpreter's light set (1-based), 0 unlit
        const uint16_t* slotStates; // each slot's index in states
        uint8_t lodMask;        // an input's channels that read the LOD fraction, made from the vertex's w:
                                //   bit 2j its colour, 2j + 1 its alpha (its C and S then hold the formula)
    };
    /** @brief A recorded vertex's position between ticks: `offset` its bytes into the recording's vertices. */
    struct HwBlendVert {
        uint32_t offset;
        int16_t prev[3], cur[3];
    };
    /** @brief The next CopyFramebuffer is the game's "once a frame": a tick's, not each of its replays'. */
    virtual void NextCopyOnceATick(bool once) {
    }
    /** @brief The bytes of vertices recorded so far: where the next draw's start. */
    virtual size_t RecordedVertexBytes() {
        return 0;
    }
    /** @brief These vertices, at `t` of the way from their last tick's positions to this one's. */
    virtual void BlendRecordedVerts(const HwBlendVert* verts, size_t count, float t) {
    }
    virtual bool SupportsHwTnl() {
        return false;
    }
    /** @brief The slots a batch lit with these lights has; 0 if the GPU can't light it (the CPU does). */
    virtual int HwLitSlots(int numDir, int numPoint) {
        return 0;
    }
    virtual void DrawTrianglesHw(const HwBatch& batch) {
    }
    /**
     * @brief A tick's frame recorded instead of drawn: the display list is run once, and each of the
     * tick's frames is the recording replayed (Replay) with the interpreter's states made again from
     * blended matrices. `last`: the tick's last replay (what it retired is released after it).
     */
    // a read back to the CPU, made in its place among the tick's drawing, not when the list is recorded: a rectangle
    // of the frame at the N64's 320x240, left where it lies in a buffer of that size
    virtual void ReadFramebufferAtReplay(int fbId, uint32_t ulx, uint32_t uly, uint32_t width, uint32_t height,
                                         uint16_t* rgba16Buf, bool byteSwap) {
    }
    virtual void BeginRecording() {
    }
    virtual void EndRecording() {
    }
    virtual void Replay(const HwState* states, const HwLights* lights, bool last) {
    }
    // the draws recorded so far; and a frame's extra triangles for one of them (bytes into SetClipVerts' array),
    // drawn after its own in the next replay
    virtual uint32_t RecordedDraws() {
        return 0;
    }
    virtual void SetRecordedClip(uint32_t draw, uint32_t start, uint32_t tris) {
    }
    virtual void SetClipVerts(const uint8_t* verts) {
    }
#endif

  protected:
    int8_t mCurrentDepthTest = 0;
    int8_t mCurrentDepthMask = 0;
    int8_t mCurrentZmodeDecal = 0;
    int8_t mLastDepthTest = -1;
    int8_t mLastDepthMask = -1;
    int8_t mLastZmodeDecal = -1;
    bool mSrgbMode = false;
    float mCurrentPrimDepth = 0.0f;
    bool mPrimDepthDirty = true;
};
} // namespace Fast
