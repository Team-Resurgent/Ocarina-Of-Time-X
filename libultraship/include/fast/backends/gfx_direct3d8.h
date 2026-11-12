#pragma once
#ifdef LUS_XBOX

#include "gfx_rendering_api.h"
#include "fast/interpreter.h"

#include <map>
#include <vector>

struct D3DDevice;
struct D3DTexture;
struct D3DSurface;

namespace Fast {

class GfxWindowBackendXbox;

/**
 * @brief Fast3D on the original Xbox: Direct3D 8 on the NV2A.
 *
 * A Fast3D shader (a color-combiner key) becomes an NV2A register-combiner
 * program: each N64 cycle (a - b) * c + d is two general combiner stages,
 * fog the final combiner. The combiner's inputs, which the interpreter gives
 * per vertex, are packed to colors: inputs 1 and 2 are the vertex's diffuse
 * and specular, 3 and 4 two texture coordinates the pixel stages pass through
 * as colors (texture stages 2 and 3), so a vertex is at most 52 bytes, not
 * the interpreter's 48 floats. A small vertex shader (one for each vertex
 * layout) moves them to where the combiners read them.
 *
 * Textures are converted from the interpreter's RGBA32 to the smallest
 * format that holds them exactly (R5G6B5, A1R5G5B5, A8L8; A8R8G8B8 only for
 * textures with both colour and graded alpha), padded to a power of two and
 * swizzled. Framebuffers other than the screen are linear textures the GPU
 * draws into (addressed in texels, which the vertex shader scales for).
 *
 * Not done (yet): three-point filtering (bilinear instead), the shader's
 * texture clamping inside a texture (the stage's address mode instead), noise
 * (a constant half), grayscale, texture masks/blends, prim depth, more than
 * four combiner inputs.
 */
class GfxRenderingAPID3D8 final : public GfxRenderingAPI {
  public:
    GfxRenderingAPID3D8(GfxWindowBackendXbox* window);
    ~GfxRenderingAPID3D8() override;

    const char* GetName() override;
    int GetMaxTextureSize() override;
    GfxClipParameters GetClipParameters() override;
    void UnloadShader(ShaderProgram* oldPrg) override;
    void LoadShader(ShaderProgram* newPrg) override;
    void ClearShaderCache() override;
    ShaderProgram* CreateAndLoadNewShader(uint64_t shaderId0, uint64_t shaderId1) override;
    ShaderProgram* LookupShader(uint64_t shaderId0, uint64_t shaderId1) override;
    void ShaderGetInfo(ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) override;
    uint32_t NewTexture() override;
    void SelectTexture(int tile, uint32_t textureId) override;
    void UploadTexture(const uint8_t* rgba32Buf, uint32_t width, uint32_t height) override;
    void SetSamplerParameters(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt) override;
    void SetDepthTestAndMask(bool depth_test, bool z_upd) override;
    void SetZmodeDecal(bool decal) override;
    void SetViewport(int x, int y, int width, int height) override;
    void SetScissor(int x, int y, int width, int height) override;
    void SetUseAlpha(bool useAlpha) override;
    void DrawTriangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) override;
    void Init() override;
    void OnResize() override;
    void StartFrame() override;
    void EndFrame() override;
    void FinishRender() override;
    int CreateFramebuffer() override;
    void UpdateFramebufferParameters(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level,
                                     bool opengl_invertY, bool render_target, bool has_depth_buffer,
                                     bool can_extract_depth) override;
    void StartDrawToFramebuffer(int fbId, float noiseScale) override;
    void CopyFramebuffer(int fbDstId, int fbSrcId, int srcX0, int srcY0, int srcX1, int srcY1, int dstX0, int dstY0,
                         int dstX1, int dstY1) override;
    void ClearFramebuffer(bool color, bool depth) override;
    void ClearDepthRegion(int x, int y, int w, int h) override;
    void ReadFramebufferToCPU(int fbId, uint32_t width, uint32_t height, uint16_t* rgba16Buf) override;
    void ReadFramebufferAtReplay(int fbId, uint32_t ulx, uint32_t uly, uint32_t width, uint32_t height,
                                 uint16_t* rgba16Buf, bool byteSwap) override;
    void ResolveMSAAColorBuffer(int fbIdTarger, int fbIdSrc) override;
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
    GetPixelDepth(int fb_id, const std::set<std::pair<float, float>>& coordinates) override;
    void* GetFramebufferTextureId(int fbId) override;
    void SelectTextureFb(int fbId) override;
    void DeleteTexture(uint32_t texId) override;
    void SetTextureFilter(FilteringMode mode) override;
    FilteringMode GetTextureFilter() override;
    void SetSrgbMode() override;
    ImTextureID GetTextureById(int id) override;
    void SetCurrentPrimDepth(float depth) override;
    bool SupportsHwTnl() override {
        return !mHwFailed; // a GPU-transform shader that didn't assemble: the CPU's path from then on
    }
    int HwLitSlots(int numDir, int numPoint) override;
    /** @brief The GPU's transform: raw N64 vertices, transformed, lit, fogged and combined by a vertex shader. */
    void DrawTrianglesHw(const HwBatch& batch) override;
    /**
     * @brief A tick's frame recorded, not drawn: the draws (their vertices packed, the state they bind),
     * the targets, clears, viewports and copies, in order. Replay gives the device all of it, the GPU
     * transform's draws with the interpreter's states as they are now (blended for a frame between ticks).
     * Textures are uploaded as the recording is made and stay until the tick's last replay has drawn.
     */
    void BeginRecording() override;
    void EndRecording() override;
    void Replay(const HwState* states, const HwLights* lights, bool last) override;
    uint32_t RecordedDraws() override {
        return (uint32_t)mRecDraws.size();
    }
    size_t RecordedVertexBytes() override {
        return mRecVerts.size();
    }
    void NextCopyOnceATick(bool once) override {
        mCopyOnceATick = once;
    }
    void BlendRecordedVerts(const HwBlendVert* verts, size_t count, float t) override;
    void SetRecordedClip(uint32_t draw, uint32_t start, uint32_t tris) override {
        if (draw < mRecDraws.size()) {
            mRecDraws[draw].clipStart = start;
            mRecDraws[draw].clipTris = tris;
        }
    }
    void SetClipVerts(const uint8_t* verts) override {
        mClipVerts = verts;
    }

    /** @brief One line of counters for the log (draws, state changes, textures). */
    void LogStats();

  private:
    struct Program {
        uint64_t id0, id1;
        CCFeatures cc;
        uint32_t ps;      // pixel shader handle
        uint32_t vs;      // vertex shader handle (shared by programs with the same layout)
        uint32_t hwvs;    // the GPU transform's, unlit (made at its first use), and
        uint32_t hwvsLit[(kHwMaxDirLights + 1) * (kHwMaxPointLights + 1)] = {}; // lit, by the light counts
        uint8_t numInputs;
        uint8_t colors;   // inputs packed as colors (at most 4)
        bool usedTextures[2];
        bool fog;
        uint8_t numFloats; // the interpreter's floats a vertex
        uint8_t stride;    // ours, bytes
    };
    struct Texture {
        D3DTexture* tex = nullptr;
        uint16_t w = 0, h = 0, pw = 0, ph = 0;
        uint32_t bytes = 0;  // of an uploaded texture (for the stats)
        bool linear = false; // a framebuffer's: addressed in texels
        bool filter = false;
        uint8_t addrU = 1, addrV = 1;
    };
    struct Framebuffer {
        D3DTexture* tex = nullptr;
        D3DSurface* color = nullptr;
        D3DSurface* depth = nullptr;
        uint32_t w = 0, h = 0;
        bool hasDepth = false;
        bool sharesScreenDepth = false; // depth is the screen's (StartDrawToFramebuffer)
        uint32_t texId = 0;             // its entry in mTextures, for sampling
    };

    // the recording (BeginRecording): a stage's texture as a draw binds it, a draw, and the commands
    struct TexBind {
        D3DTexture* tex;
        uint8_t addrU, addrV, filter;
        float su, sv;
    };
    struct DrawRec {
        Program* prog;
        TexBind tex[2];
        int8_t zTest, zWrite, decal;
        bool hw;
        uint32_t start;  // its vertices: the bytes into mRecVerts
        uint32_t tris;
        uint32_t clipStart, clipTris; // this frame's extra ones, in mClipVerts (SetRecordedClip)
        uint32_t fogColor;
        uint16_t slots[kHwMaxSlots];
        HwBatch batch;   // the GPU transform's: as it came, the vertices, states and lights filled at a replay
    };
    enum RecKind : uint8_t {
        REC_DRAW,
        REC_TARGET,
        REC_CLEAR,
        REC_CLEAR_DEPTH,
        REC_VIEWPORT,
        REC_SCISSOR,
        REC_COPY,
        REC_READ
    };
    struct RecCmd {
        RecKind kind;
        int v[10];
        float f;
    };

    uint32_t VertexShaderFor(const Program& p);
    uint32_t HwVertexShaderFor(const Program& p, const HwLights* lights, uint8_t lodMask);
    void Retire(D3DTexture* tex);
    void ReleaseRetired(bool all);
    void ResolveTexture(int stage, TexBind& b);
    void RecordDraw(bool hw, const uint8_t* verts, size_t bytes, size_t tris, uint32_t fogColor, const HwBatch* b);
    void DrawRecorded(DrawRec& r, const HwState* states, const HwLights* lights);
    void ReadWantedDepths();
    void ReadFramebufferRect(int fbId, uint32_t fullW, uint32_t fullH, uint32_t ulx, uint32_t uly, uint32_t width,
                             uint32_t height, uint16_t* rgba16Buf);
    void ApplyDrawState(bool hw = false, int cull = 0, const HwLights* lights = nullptr, uint8_t lodMask = 0);
    void SetFullViewport();
    D3DSurface* TargetColor(int fbId);
    D3DSurface* TargetDepth(int fbId);
    void EnsureFramebuffer(int fbId);

    GfxWindowBackendXbox* mWindow;
    D3DDevice* mDev = nullptr;
    D3DSurface* mScreenDepth = nullptr; // the device's own depth buffer (Init): 0's, and a screen-sized target's
    std::map<std::pair<uint64_t, uint64_t>, Program> mPrograms;
    std::map<uint32_t, uint32_t> mVertexShaders; // layout key -> handle
    std::map<uint32_t, uint32_t> mHwVertexShaders; // the GPU transform's, by layout and lights
    float mStageScale[2][2] = { { 1, 1 }, { 1, 1 } }; // a stage's coordinate scale (padding, or texels)
    int mBoundCull = -1;
    bool mHwFailed = false, mHwLitFailed = false;
    uint64_t mHwDraws = 0, mHwTris = 0, mHwDrawsLogged = 0;
    uint32_t mFrameFence = 0; // the last EndFrame's
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff> mLastDepths; // GetPixelDepth's last answers
    uint32_t mConstsSent = 0;
    uint64_t mPushWaitCycles = 0;                      // draws that waited for the GPU, and how long
    uint32_t mPushWaits = 0, mReplays = 0, mGpuIdle = 0;
    uint64_t mStateCycles = 0, mConstCycles = 0, mDrawCycles = 0; // a GPU-transform draw's parts, all frames
    // the states the slots hold in this frame, and their layout: a slot that keeps its state keeps its constants
    uint16_t mSlotState[kHwMaxSlots] = {};
    int mSlotBase = -1, mSlotStride = 0, mSlotCount = 0;
    const uint8_t* mClipVerts = nullptr;
    Program* mProgram = nullptr;
    std::vector<Texture> mTextures;
    std::vector<uint32_t> mFreeTextures;
    std::vector<Framebuffer> mFramebuffers;
    struct Retired {
        D3DTexture* tex;
        uint32_t fence;
    };
    std::vector<Retired> mRetired;
    bool mCopyOnceATick = false; // NextCopyOnceATick's
    bool mRecording = false; // BeginRecording .. EndRecording: the calls are kept, not given to the device
    bool mTickOpen = false;  // a recording made and not all of the tick's replays drawn: what is retired
                             //   meanwhile gets its fence at the last replay's EndFrame
    std::vector<RecCmd> mRecCmds;
    std::vector<DrawRec> mRecDraws;
    std::vector<uint8_t> mRecVerts;
    const DrawRec* mReplayDraw = nullptr; // the draw being replayed (its textures), or none
    std::set<std::pair<float, float>> mDepthWanted; // what GetPixelDepth was asked (ReadWantedDepths)
    int mDepthWantedFb = 0;
    std::vector<uint8_t> mConvert;   // a texture before swizzling
    std::vector<uint8_t> mVertices;  // a draw's vertices, packed
    // the interpreter's clamp to the tile, done on the triangles (DrawTriangles)
    struct ClampStep {
        int at;      // the coordinate's float in a packed vertex
        float bound; // in the coordinate's units
        bool upper;  // a bound from above (else from below)
    };
    static constexpr int kClampMaxVerts = 12, kClampMaxStride = 64;
    struct ClampPoly {
        int n;
        uint8_t v[kClampMaxVerts * kClampMaxStride];
    };
    void ClampSplit(const ClampPoly& poly, const ClampStep* steps, int numSteps);
    void ClampLerp(uint8_t* dst, const uint8_t* a, const uint8_t* b, float t);
    std::vector<uint8_t> mClamped; // the cut triangles
    uint32_t mCurrentTile = 0;
    uint32_t mCurrentTextureIds[2] = { 0, 0 };
    int mCurrentFb = 0;
    uint32_t mTargetW = 640, mTargetH = 480;
    FilteringMode mFilter = FILTER_LINEAR;
    bool mUseAlpha = false;
    bool mInScene = false;

    // what the device has (to set only what changes)
    uint32_t mBoundPs = 0, mBoundVs = 0;
    D3DTexture* mBoundTex[2] = { nullptr, nullptr };
    D3DTexture* mDummyTex = nullptr; // 1x1 white: bound where a sampled stage has no texture
    int mBoundAddrU[2] = { -1, -1 }, mBoundAddrV[2] = { -1, -1 }, mBoundFilter[2] = { -1, -1 };
    float mBoundScale[2][2] = { { -1, -1 }, { -1, -1 } };
    int mBoundBlend = -1, mBoundZEnable = -1, mBoundZWrite = -1, mBoundZFunc = -1, mBoundZBias = -1;
    int mBoundAlphaTest = -1, mBoundAlphaRef = -1, mBoundColorWrite = -1, mBoundFog = -1;
    uint32_t mBoundFogColor = 0;
    float mVsConst[96][4];             // the vertex shader constants the device has (DrawTrianglesHw sends
    bool mVsConstKnown[96] = {};       //   only the ones that change)

    // counters for LogStats
    uint32_t mDraws = 0, mTris = 0, mUploads = 0, mUploadBytes = 0, mPrograms_ = 0, mTexSwitches = 0;
};

} // namespace Fast

#endif
