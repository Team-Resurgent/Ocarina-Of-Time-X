#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <unordered_map>
#include <map>
#include <list>
#include <cstddef>
#include <vector>
#include <stack>
#include <string>
#include <string_view>
#include <memory>

#include "fast/lus_gbi.h"
#include "fast/types.h"
#ifdef LUS_XBOX
#include "fast/MtxBlends.h"
#include "fast/CutCache.h"
#endif
#include "fast/ucodehandlers.h"
#include "backends/gfx_rendering_api.h"
#include "fast/debug/GfxDebugger.h"

#include "fast/resource/type/Texture.h"
#include "ship/resource/Resource.h"

// TODO figure out why changing these to 640x480 makes the game only render in a quarter of the window
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
#include <compare>
#endif

/*enum {
    CC_0,
    CC_TEXEL0,
    CC_TEXEL1,
    CC_PRIM,
    CC_SHADE,
    CC_ENV,
    CC_TEXEL0A,
    CC_LOD
};*/

enum {
    SHADER_0,
    SHADER_INPUT_1,
    SHADER_INPUT_2,
    SHADER_INPUT_3,
    SHADER_INPUT_4,
    SHADER_INPUT_5,
    SHADER_INPUT_6,
    SHADER_INPUT_7,
    SHADER_TEXEL0,
    SHADER_TEXEL0A,
    SHADER_TEXEL1,
    SHADER_TEXEL1A,
    SHADER_1,
    SHADER_COMBINED,
    SHADER_NOISE
};

#ifdef __cplusplus
enum class ShaderOpts {
    ALPHA,
    FOG,
    TEXTURE_EDGE,
    NOISE,
    _2CYC,
    ALPHA_THRESHOLD,
    INVISIBLE,
    GRAYSCALE,
    TEXEL0_CLAMP_S,
    TEXEL0_CLAMP_T,
    TEXEL1_CLAMP_S,
    TEXEL1_CLAMP_T,
    TEXEL0_MASK,
    TEXEL1_MASK,
    TEXEL0_BLEND,
    TEXEL1_BLEND,
    PRIM_DEPTH,
    PRISM_SHADER, // 16-bit width
    MAX
};

#define SHADER_OPT(opt) ((uint64_t)(1 << static_cast<int>(ShaderOpts::opt)))
#endif

struct ColorCombinerKey {
    uint64_t combine_mode;
    uint64_t options;
    uint64_t shader_id;

#ifdef __cplusplus
    auto operator<=>(const ColorCombinerKey&) const = default;
#endif
};

#define SHADER_MAX_TEXTURES 6
#define SHADER_FIRST_TEXTURE 0
#define SHADER_FIRST_MASK_TEXTURE 2
#define SHADER_FIRST_REPLACEMENT_TEXTURE 4

struct CCFeatures {
    int c[2][2][4];
    bool opt_alpha;
    bool opt_fog;
    bool opt_texture_edge;
    bool opt_noise;
    bool opt_2cyc;
    bool opt_alpha_threshold;
    bool opt_invisible;
    bool opt_grayscale;
    bool opt_prim_depth;
    bool usedTextures[2];
    bool used_masks[2];
    bool used_blend[2];
    bool clamp[2][2];
    int numInputs;
    bool do_single[2][2];
    bool do_multiply[2][2];
    bool do_mix[2][2];
    bool color_alpha_same[2];
    int16_t shader_id;
};

void gfx_cc_get_features(uint64_t shader_id0, uint64_t shader_id1, struct CCFeatures* cc_features);

union Gfx;

namespace Fast {

class GfxRenderingAPI;
class GfxWindowBackend;

constexpr size_t MAX_SEGMENT_POINTERS = 16;
constexpr size_t SHADER_ID_SHIFT = 17;
constexpr int16_t ShaderIdUnmask(int id) {
    return (id >> SHADER_ID_SHIFT) & 0xFFFF;
}

struct GfxExecStack {
    // This is a dlist stack used to handle dlist calls.
    std::stack<F3DGfx*> cmd_stack = {};
    // This is also a dlist stack but a std::vector is used to make it possible
    // to iterate on the elements.
    // The purpose of this is to identify an instruction at a poin in time
    // which would not be possible with just a F3DGfx* because a dlist can be called multiple times
    // what we do instead is store the call path that leads to the instruction (including branches)
    std::vector<const F3DGfx*> gfx_path = {};
    struct CodeDisp {
        const char* file;
        int line;
    };
    // stack for OpenDisp/CloseDisps
    std::vector<CodeDisp> disp_stack{};

    void start(F3DGfx* dlist);
    void stop();
    F3DGfx*& currCmd();
    void openDisp(const char* file, int line);
    void closeDisp();
    const std::vector<CodeDisp>& getDisp() const;
    void branch(F3DGfx* caller);
    void call(F3DGfx* caller, F3DGfx* callee);
    F3DGfx* ret();
};

struct XYWidthHeight {
    int16_t x, y;
    uint32_t width, height;
};

struct GfxDimensions {
    float internal_mul;
    uint32_t width, height;
    float aspect_ratio;
};

struct TextureCacheKey {
    const uint8_t* texture_addr;
    const uint8_t* palette_addrs[2];
    uint8_t fmt, siz;
    uint8_t palette_index;
    uint32_t size_bytes;
#ifdef LUS_XBOX
    // a blended texture's mask and replacement, hashed (0: not one): the mask is applied as it is imported
    // (Interpreter::ImportTexture), so a change to it is a texture of its own
    uint32_t mask_hash = 0;
#endif

    bool operator==(const TextureCacheKey&) const noexcept = default;

    struct Hasher {
        size_t operator()(const TextureCacheKey& key) const noexcept {
            uintptr_t addr = (uintptr_t)key.texture_addr;
            return (size_t)(addr ^ (addr >> 5));
        }
    };
};

typedef std::unordered_map<TextureCacheKey, struct TextureCacheValue, TextureCacheKey::Hasher> TextureCacheMap;
typedef std::pair<const TextureCacheKey, struct TextureCacheValue> TextureCacheNode;

struct TextureCacheValue {
    uint32_t texture_id;
    uint8_t cms, cmt;
    bool linear_filter;

    std::list<struct TextureCacheMapIter>::iterator lru_location;
};

struct TextureCacheMapIter {
    TextureCacheMap::iterator it;
};

struct RGBA {
    uint8_t r, g, b, a;
};

struct LoadedVertex {
    float x, y, z, w;
    float u, v;
    struct RGBA color;
    uint8_t clip_rej;
#ifdef LUS_XBOX
    // loaded raw for the GPU (Interpreter::mHwTnl): x..w, clip_rej and fog's alpha are made only when a
    // CPU path needs them (Interpreter::EnsureTransformed)
    int16_t ob[3];    // the object position
    uint16_t state;   // the transform state it was loaded under (Interpreter::mHwStates)
    uint8_t alpha;    // its own alpha (color.a becomes fog's on a CPU transform)
    bool hw;          // loaded raw
    bool transformed; // x..w made
    uint8_t hwRisk;   // Interpreter::HwVertexRisky's answer: 0 not asked yet, 1 no, 2 yes
    uint8_t hwOut;    // and the planes it is far beyond for all the tick (a bit each): one all three share drops a triangle
    int8_t n[3];      // the normal, and
    uint16_t light;   //   the light set (Interpreter::mHwLights, 1-based) the GPU lights it with; 0: color is
                      //   its shade (Interpreter::EnsureLit lights it on the CPU)
    uint8_t hwFrom;   // a cut's piece: the triangle's corners it was made from (CutFrom)
    bool hwBlend;     // its position moves between ticks by itself, from this one (LusXbox_BlendVertices)
    int16_t hwPrev[3];
#endif
};

struct RawTexMetadata {
    uint16_t width, height;
    float h_byte_scale = 1, v_pixel_scale = 1;
    std::shared_ptr<Fast::Texture> resource;
    Fast::TextureType type;
};

#define MAX_LIGHTS 32
#define MAX_VERTICES 64

struct RSP {
    float modelview_matrix_stack[11][4][4];
    uint8_t modelview_matrix_stack_size;

    float MP_matrix[4][4];
    float P_matrix[4][4];

    F3DLight_t lookat[2];
    F3DLight current_lights[MAX_LIGHTS + 1];
    float current_lights_coeffs[MAX_LIGHTS][3];
    float current_lookat_coeffs[2][3]; // lookat_x, lookat_y
    uint8_t current_num_lights;        // includes ambient light
    bool lights_changed;

    uint32_t geometry_mode;
    int16_t fog_mul, fog_offset;

    uint32_t extra_geometry_mode;

    struct {
        // U0.16
        uint16_t s, t;
    } texture_scaling_factor;

    struct LoadedVertex loaded_vertices[MAX_VERTICES + 4];
};

struct RDP {
    const uint8_t* palettes[2];
    // Original DRAM source address of the most recent TLUT load per palette half.
    // Used in texture cache keys instead of palettes[] (which always points to staging).
    const uint8_t* palette_dram_addr[2];
    // CI4 palette staging buffer: N64 TMEM holds up to 16 CI4 palettes (16 entries x 2 bytes each = 32 bytes per
    // palette). palettes[0] covers indices 0-7 (256 bytes), palettes[1] covers 8-15 (256 bytes). GfxDpLoadTlut copies
    // TLUT data here at the correct offset so multi-palette CI4 models work.
    uint8_t palette_staging[2][256];
    struct {
        const uint8_t* addr;
        uint8_t siz;
        uint32_t width;
        uint32_t tex_flags;
        struct RawTexMetadata raw_tex_metadata;
    } texture_to_load;
    struct {
        const uint8_t* addr;
        uint32_t orig_size_bytes;
        uint32_t size_bytes;
        uint32_t full_image_line_size_bytes;
        uint32_t line_size_bytes;
        uint32_t tex_flags;
        struct RawTexMetadata raw_tex_metadata;
        bool masked;
        bool blended;
    } loaded_texture[2];
    struct {
        uint8_t fmt;
        uint8_t siz;
        uint8_t cms, cmt;
        uint8_t masks, maskt; // mask exponents from SetTile; 2^mask is the WRAP/MIRROR period
        uint8_t shifts, shiftt;
        float uls, ult, lrs, lrt;
        uint16_t tmem; // 0-511, in 64-bit word units
        uint32_t line_size_bytes;
        uint8_t palette;
        uint8_t tmem_index; // 0 or 1 for offset 0 kB or offset 2 kB, respectively
    } texture_tile[8];
    bool textures_changed[2];

    uint8_t first_tile_index;

    uint32_t other_mode_l, other_mode_h;
    uint64_t combine_mode;
    bool grayscale;

    uint8_t prim_lod_fraction;
    uint16_t prim_depth;
    struct RGBA env_color, prim_color, fog_color, blend_color, fill_color, grayscale_color;

    // Chroma key parameters (G_SETKEYR / G_SETKEYGB)
    struct RGBA key_center;
    struct RGBA key_scale;
    int16_t convert_k[6]; // YUV convert coefficients (G_SETCONVERT) — K0-K5

    struct XYWidthHeight viewport, scissor;
    bool viewport_or_scissor_changed;
    void* z_buf_address;
    void* color_image_address;
};

typedef enum Attribute {
    MTX_PROJECTION,
    MTX_LOAD,
    MTX_PUSH,
    MTX_NOPUSH,
    CULL_FRONT,
    CULL_BACK,
    CULL_BOTH,
    MV_VIEWPORT,
    MV_LIGHT,
} Attribute;

extern GfxExecStack g_exec_stack;

struct GfxTextureCache {
    TextureCacheMap map;
    std::list<TextureCacheMapIter> lru;
    std::vector<uint32_t> free_texture_ids;
};

struct ColorCombiner {
    uint64_t shader_id0;
    uint64_t shader_id1;
    bool usedTextures[2];
    struct ShaderProgram* prg[16];
    uint8_t shader_input_mapping[2][7];
};

struct RenderingState {
    uint8_t depth_test_and_mask; // 1: depth test, 2: depth mask
    bool decal_mode;
    bool alpha_blend;
    struct XYWidthHeight viewport, scissor;
    struct ShaderProgram* mShaderProgram;
    TextureCacheNode* mTextures[SHADER_MAX_TEXTURES];
};

struct FBInfo {
    uint32_t orig_width, orig_height;       // Original shape
    uint32_t applied_width, applied_height; // Up-scaled for the viewport
    uint32_t native_width, native_height;   // Max "native" size of the screen, used for up-scaling
    bool resize;                            // Scale to match the viewport
    bool forceFixedAspect;                  // Preserve aspect ratio even if resize is true
};

struct MaskedTextureEntry {
    uint8_t* mask;
    uint8_t* replacementData;
};

class Interpreter {
  public:
    Interpreter();
    ~Interpreter();

    void Init(GfxWindowBackend* wapi, class GfxRenderingAPI* rapi, const char* game_name, bool start_in_fullscreen,
              uint32_t width, uint32_t height, uint32_t posX, uint32_t posY);
    void Destroy();
    void SetGfxDebugger(std::shared_ptr<GfxDebugger> debugger);
    std::shared_ptr<GfxDebugger> GetGfxDebugger() const;
    void GetDimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY);
    GfxRenderingAPI* GetCurrentRenderingAPI();
    void StartFrame();
    void RunGuiOnly();
    void Run(Gfx* commands, const std::unordered_map<Mtx*, MtxF>& mtx_replacements);
#ifdef LUS_XBOX
    /** @brief One of a tick's frames from the recording Run made: its states' matrices blended (HwRebuildStates). */
    void Replay(const MtxBlends* blends, bool last); // (null: the tick's own matrices)
    /** @brief Something else used the renderer's textures (the port's menu): the tiles are imported again. */
    void ForgetBoundTextures();
#endif
    void EndFrame();
    void HandleWindowEvents();
    bool IsFrameReady();
    bool ViewportMatchesRendererResolution();
    int GetTargetFps();
    void SetTargetFps(int fps);
    void SetMaxFrameLatency(int latency);
    int CreateFrameBuffer(uint32_t width, uint32_t height, uint32_t native_width, uint32_t native_height,
                          uint8_t resize, bool forceFixedAspect = false);
    void SetFrameBuffer(int fb, float noiseScale);
    void CopyFrameBuffer(int fb_dst_id, int fb_src_id, bool copyOnce, bool* hasCopiedPtr);
    void ResetFrameBuffer();
    void AdjustPixelDepthCoordinates(float& x, float& y);
    void GetPixelDepthPrepare(float x, float y);
    uint16_t GetPixelDepth(float x, float y);
    void RegisterBlendedTexture(const char* name, uint8_t* mask, uint8_t* replacement);
    void UnregisterBlendedTexture(const char* name);

    // Register a CPU address as a mirror of a GPU framebuffer texture.
    // When ImportTexture encounters this address, it uses SelectTextureFb instead
    // of reading from CPU memory — giving full GPU resolution with no readback.
    void RegisterFbTexture(const void* cpuAddr, int fbId);
    void UnregisterFbTexture(const void* cpuAddr);

    void SetNativeDimensions(float width, float height);
    void SetResolutionMultiplier(float multiplier);
    void SetMsaaLevel(uint32_t level);
    void GetCurDimensions(uint32_t* width, uint32_t* height);

    // private: TODO make these private
    void Flush();
    ShaderProgram* LookupOrCreateShaderProgram(uint64_t id0, uint64_t id1);
    ColorCombiner* LookupOrCreateColorCombiner(const ColorCombinerKey& key);
    void ShaderCacheClear();
    void TextureCacheClear();
    std::shared_ptr<Ship::IResource> ResolveResourceCached(const char* path);
    bool TextureCacheLookup(int i, const TextureCacheKey& key);
    void TextureCacheDelete(const uint8_t* origAddr);
    void ImportTextureRgba16(int tile, bool importReplacement);
    void ImportTextureRgba32(int tile, bool importReplacement);
    void ImportTextureIA4(int tile, bool importReplacement);
    void ImportTextureIA8(int tile, bool importReplacement);
    void ImportTextureIA16(int tile, bool importReplacement);
    void ImportTextureI4(int tile, bool importReplacement);
    void ImportTextureI8(int tile, bool importReplacement);
    void ImportTextureCi4(int tile, bool importReplacement);
    void ImportTextureCi8(int tile, bool importReplacement);
    void ImportTextureRaw(int tile, bool importReplacement);
    void ImportTextureImg(int tile, bool importReplacement);
    void ImportTexture(int i, int tile, bool importReplacement);
    void DecodeTexture(int tile, bool importReplacement, uint8_t fmt, uint8_t siz, uint32_t texFlags);
    void UploadImported(const uint8_t* rgba, uint32_t width, uint32_t height);
    void ImportTextureMask(int i, int tile);
    void CalculateNormalDir(const F3DLight_t*, float coeffs[3]);
    // Opt-in memoization of OTR texture-path resolution, keyed by display-list
    // pointer and dropped with the texture cache. Safe for ports whose display
    // lists carry stable path pointers; off by default.
    void SetResolvedResourceCacheEnabled(bool enabled);

    void GfxSpMatrix(uint8_t params, const int32_t* addr);
    void GfxSpPopMatrix(uint32_t count);
    void GfxSpVertex(size_t numVertices, size_t destIndex, const F3DVtx* vertices);
    void GfxSpModifyVertex(uint16_t vtxIdx, uint8_t where, uint32_t val);
    void GfxSpTri1(uint8_t vtx1Idx, uint8_t vtx2Idx, uint8_t vtx3Idx, bool isRect);
    void GfxSpGeometryMode(uint32_t clear, uint32_t set);
    void GfxSpExtraGeometryMode(uint32_t clear, uint32_t set);
    void GfxSpMovememF3dex2(uint8_t index, uint8_t offset, const void* data);
    void GfxSpMovememF3d(uint8_t index, uint8_t offset, const void* data);
    void GfxSpMovewordF3dex2(uint8_t index, uint16_t offset, uintptr_t data);
    void GfxSpMovewordF3d(uint8_t index, uint16_t offset, uintptr_t data);
    void GfxSpTexture(uint16_t sc, uint16_t tc, uint8_t level, uint8_t tile, uint8_t on);
    void GfxDpSetScissor(uint32_t mode, uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry);
    void GfxDpSetTextureImage(uint32_t format, uint32_t size, uint32_t width, const char* texPath, uint32_t texFlags,
                              RawTexMetadata rawTexMetdata, const void* addr);
#ifdef LUS_XBOX
    void GfxDpSetTextureImage(uint32_t size, uint32_t width, const std::shared_ptr<Fast::Texture>& texture);
#endif
    void GfxDpSetTile(uint8_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint8_t tile, uint32_t palette,
                      uint32_t cmt, uint32_t maskt, uint32_t shiftt, uint32_t cms, uint32_t masks, uint32_t shifts);
    void GfxDpSetTileSize(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt);
    void GfxDpLoadTlut(uint8_t tile, uint32_t high_index);
    void GfxDpLoadBlock(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt);
    void GfxDpLoadTile(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt);
    void GfxDpSetCombineMode(uint32_t rgb, uint32_t alpha, uint32_t rgb_cyc2, uint32_t alpha_cyc2);
    void GfxDpSetGrayscaleColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void GfxDpSetEnvColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void GfxDpSetPrimColor(uint8_t m, uint8_t r, uint8_t l, uint8_t g, uint8_t b, uint8_t a);
    void GfxDpSetFogColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void GfxDpSetBlendColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void GfxDpSetFillColor(uint32_t pickedColor);
    void GfxDrawRectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry);
    void GfxDpTextureRectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile, int16_t uls,
                               int16_t ult, int16_t dsdx, int16_t dtdy, bool flip);
    void GfxDpImageRectangle(int32_t tile, int32_t w, int32_t h, int32_t ulx, int32_t uly, int16_t uls, int16_t ult,
                             int32_t lrx, int32_t lry, int16_t lrs, int16_t lrt);
    void GfxDpFillRectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry);
    void GfxDpSetZImage(void* zBufAddr);
    void GfxDpSetColorImage(uint32_t format, uint32_t size, uint32_t width, void* address);
    void GfxSpSetOtherMode(uint32_t shift, uint32_t num_bits, uint64_t mode);
    void GfxDpSetOtherMode(uint32_t h, uint32_t l);

    void Gfxs2dexBgCopy(F3DuObjBg* bg);
    void Gfxs2dexBg1cyc(F3DuObjBg* bg);
    void Gfxs2dexRecyCopy(F3DuObjSprite* spr);

    void AdjustWidthHeightForScale(uint32_t& width, uint32_t& height, uint32_t nativeWidth,
                                   uint32_t nativeHeight) const;
    float AdjXForAspectRatio(float x) const;
    void AdjustVIewportOrScissor(XYWidthHeight* area);
    void CalcAndSetViewport(const F3DVp_t* viewport);

    void SpReset();
    void* SegAddr(uintptr_t w1);

    static const char* CCMUXtoStr(uint32_t ccmux);
    static const char* ACMUXtoStr(uint32_t acmux);
    static void GenerateCC(ColorCombiner* comb, const ColorCombinerKey& key);
    static std::string_view GetBaseTexturePath(std::string_view path);
    static void NormalizeVector(float v[3]);
    static void TransposedMatrixMul(float res[3], const float a[3], const float b[4][4]);
    static void MatrixMul(float res[4][4], const float a[4][4], const float b[4][4]);

    RSP* mRsp;
    RDP* mRdp;
    RenderingState mRenderingState{};

    GfxTextureCache mTextureCache{};
#ifdef LUS_XBOX
    // keyed by the pointer, with the path it held then: a pointer's path can change (the message box's
    // font slots are rewritten with each character's glyph path), and is compared at each lookup
    std::unordered_map<const void*, std::pair<std::string, std::shared_ptr<Ship::IResource>>> mResolvedResourceCache;
#else
    std::unordered_map<const void*, std::shared_ptr<Ship::IResource>> mResolvedResourceCache;
#endif
#ifdef LUS_XBOX
    // on: G_SETTIMG's path lookups were ~8 us each on the Xbox; cleared with the texture cache (a new scene)
    bool mResolvedResourceCacheEnabled = true;
#else
    bool mResolvedResourceCacheEnabled = false;
#endif
    std::map<ColorCombinerKey, ColorCombiner> mColorCombinerPool; // color_combiner_pool;
    std::map<ColorCombinerKey, ColorCombiner>::iterator mPrevCombiner = mColorCombinerPool.end();
    uint8_t* mTexUploadBuffer = nullptr;
#ifdef LUS_XBOX
    // a blended texture's import (ImportTexture): its replacement decoded into mXboxBlend first (CAPTURE),
    // then the texture with the mask applied (MASK)
    enum { XBOX_UPLOAD_PLAIN, XBOX_UPLOAD_CAPTURE, XBOX_UPLOAD_MASK } mXboxUpload = XBOX_UPLOAD_PLAIN;
    const uint8_t* mXboxMask = nullptr;
    uint32_t mXboxMaskWidth = 0, mXboxMaskHeight = 0;
    std::vector<uint8_t> mXboxBlend;
    uint32_t mXboxBlendWidth = 0, mXboxBlendHeight = 0;
    bool XboxMaskSize(int tile, uint32_t* width, uint32_t* height);
#endif

    GfxDimensions mGfxCurrentWindowDimensions{}; // gfx_current_window_dimensions;
    int32_t mCurWindowPosX{};
    int32_t mCurWindowPosY{};
    GfxDimensions mCurDimensions{};        // gfx_current_dimensions;
    GfxDimensions mPrvDimensions{};        // gfx_prev_dimensions;
    XYWidthHeight mGameWindowViewport{};   // gfx_current_game_window_viewport;
    XYWidthHeight mNativeDimensions{};     // gfx_native_dimensions;
    XYWidthHeight mPrevNativeDimensions{}; // gfx_prev_native_dimensions;
    uintptr_t mGfxFrameBuffer{};

    unsigned int mMsaaLevel = 1;
    bool mDroppedFrame{};
    float* mBufVbo; // 3 vertices in a triangle and 32 floats per vtx
    size_t mBufVboLen{};
    size_t mBufVboNumTris{};
#ifdef LUS_XBOX
    // The GPU's transform (GfxRenderingAPI::HwBatch): vertices are loaded raw with the transform state
    // they were loaded under; a triangle the GPU can draw goes to the batch, others are transformed on
    // the CPU (EnsureTransformed) and take the path above
    bool mHwTnl = false;                         // the backend does it
    std::vector<GfxRenderingAPI::HwState> mHwStates; // this frame's, a vertex's `state` indexes it
    uint16_t mHwSlotState[16];                   // the batch's slots: their mHwStates index
    int mHwSlots = 0;
    std::vector<uint8_t> mHwVerts;               // the batch's vertices, 16 bytes each
    size_t mHwTris = 0;
    GfxRenderingAPI::HwBatch mHwBatch{};         // the batch's constants (verts, tris, states filled at Flush)
    uint64_t mHwTriangles = 0, mHwLitTriangles = 0, mCpuTriangles = 0; // counted for the profile
    // the last triangle went to the batch and no command that sets state has run since (gfx_step): the
    // next one goes there too, without working its state out again
    bool mHwFast = false;
    // bumped by the state setters that found a real change (combiner, colours, other modes, tiles, geometry
    // mode, texture): a re-issue of the same values (MM's material blocks do it constantly) keeps mHwFast
    uint32_t mHwStateGen = 0;
    // bumped by what the vertices' GPU state and lights depend on (matrices, geometry mode, fog, lights,
    // the framebuffer): GfxSpVertex rebuilds its HwState and light set only when it changed
    uint32_t mHwVtxVersion = 1, mHwVtxCachedVersion = 0;
    uint16_t mHwVtxCachedState = 0, mHwVtxCachedLight = 0;
    bool mHwVtxCachedDecal = false;
    std::vector<GfxRenderingAPI::HwLights> mHwLights; // this frame's light sets, a vertex's `light` - 1
    int mHwStride = GfxRenderingAPI::kHwStride;        // the batch's slot stride and slots (lit: fewer)
    int mHwSlotMax = GfxRenderingAPI::kHwMaxSlots;
    // how a state's matrices were made, as the display list's G_MTX commands gave them: a replayed frame
    // makes them again from the blended matrices (HwRebuildStates); a chain with no room keeps the frame's
    struct HwRecipe {
        const int32_t* p[2];  // the projection: a load, then at most one multiply
        const int32_t* mv[4]; // the modelview: a load, then multiplies
        uint8_t np, nmv;      // 0: not made from the display list's matrices (the state keeps its own)
        bool lit;             // mv is read (lighting)
        bool decal;           // its depth pushed nearer (HwPushDecal)
        bool tri;             // a triangle's own state (HwTriState): its corners, each under its own state
        uint16_t triState[3];
        int16_t triOb[3][3];
    };
    std::vector<HwRecipe> mHwRecipes; // one for each of mHwStates
    const int32_t* mHwRecipeP[2] = {};
    const int32_t* mHwRecipeMv[11][4] = {}; // the modelview stack's levels
    uint8_t mHwRecipeNp = 0, mHwRecipeNmv[11] = {};
    void LoadMtx(float m[4][4], const int32_t* addr, const std::unordered_map<Mtx*, MtxF>& mtx_replacements);
    void LoadMtx(float m[4][4], const int32_t* addr, const MtxBlends* blends);
    void HwRebuildStates(const MtxBlends* blends);
    void HwFillTriState(GfxRenderingAPI::HwState& s, const HwRecipe& r) const;
    uint16_t HwLightSet();
    void EnsureLit(LoadedVertex* v);
    void EnsureTransformed(LoadedVertex* v);
    bool HwTriangle(LoadedVertex* const v[3], int cull, const struct ColorCombiner* comb, bool use_alpha,
                    bool use_fog, const bool usedTextures[2], uint8_t numInputs, const uint32_t effective_tile[2],
                    const uint32_t tex_width[2], const uint32_t tex_height[2], bool is_rect);
    void HwEmit(LoadedVertex* const v[3]);
    void HwSetBatchLights(uint16_t lightSet);
    // A triangle the GPU can't be given whole in some frame of the tick: see HwClipTriangles
    struct HwClipTri {
        uint32_t bytes;      // its place in the batch, until the batch is flushed
        uint32_t draw;       // then the recorded draw it goes with
        uint16_t state;      // what places it
        int16_t verts[3][8]; // as HwEmit made them
    };
    // neighbours of one draw and one state, and the box around them: most go together, behind the camera
    struct HwClipGroup {
        uint32_t first, count;
        int16_t lo[3], hi[3];
    };
    std::vector<HwClipTri> mHwClipTris;
    std::vector<HwClipGroup> mHwClipGroups;
    std::vector<uint8_t> mHwStateUsed; // a state a draw reads: the others aren't made again for each frame
    size_t mHwClipPending = 0;         // the first of them still in the batch
    // vertices that move between ticks by themselves (LusXbox_BlendVertices): in the batch, and in the recording
    std::vector<GfxRenderingAPI::HwBlendVert> mHwBlendBatch, mHwBlendRecorded;
    std::vector<uint8_t> mHwClipVerts; // a frame's: what it made of them
    bool HwVertexRisky(LoadedVertex* v);
    bool HwSpanRisky(LoadedVertex* const v[3]);
    void HwClipTriangles();
    void HwFlush();
    // a triangle that reaches a clamped tile's bounds, cut there in object space (the coordinates are
    // affine there too), the pieces beyond pinned to the bound: on the GPU's path with the rest
    struct HwClampStep {
        bool axisV, upper;
        float bound; // in the vertex's coordinate units
    };
    struct HwClampAxis {
        bool axisV;
        float lo, hi; // the range where the GPU's wrap or mirror reads what the clamp would
    };
    struct HwTriArgs {
        int cull;
        const struct ColorCombiner* comb;
        bool use_alpha, use_fog;
        bool usedTextures[2];
        uint8_t numInputs;
        uint32_t effective_tile[2], tex_width[2], tex_height[2];
    };
    // the state's clamped tiles (GfxSpTri1 works them out; mHwFast's triangles reuse them)
    HwClampAxis mHwClampAxis[4];
    HwClampStep mHwClampStep[8];
    int mHwClampAxes = 0, mHwClampSteps = 0;
    const HwTriArgs* mHwCutArgs = nullptr; // a cut's first piece sets the batch's constants; then null
    bool HwClampReached(LoadedVertex* const v[3]) const;
    bool HwSameXform(uint16_t a, uint16_t b) const;
    bool HwTriState(LoadedVertex* poly);
    bool HwClampTriangle(LoadedVertex* const v[3], const HwTriArgs* a);
    bool HwClampSplit(const LoadedVertex* poly, int n, const HwClampStep* steps, int numSteps);
    bool HwCutCached(LoadedVertex* poly);
    bool HwCutEmit(const LoadedVertex* poly, int n);
#endif
    GfxWindowBackend* mWapi = nullptr;
    GfxRenderingAPI* mRapi = nullptr;
    std::shared_ptr<GfxDebugger> mGfxDebugger;

    uintptr_t mSegmentPointers[MAX_SEGMENT_POINTERS]{};

    bool mFbActive{};
    bool mRendersToFb{}; // game_renders_to_framebuffer;
    std::map<int, FBInfo>::iterator mActiveFrameBuffer;
    std::map<int, FBInfo> mFrameBuffers;

    int mGameFb{};             // game_framebuffer;
    int mGameFbMsaaResolved{}; // game_framebuffer_msaa_resolved;

    std::set<std::pair<float, float>> mGetPixelDepthPending; // get_pixel_depth_pending;
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff> mGetPixelDepthCached; // get_pixel_depth_cached;
    std::map<std::string, MaskedTextureEntry, std::less<>> mMaskedTextures;
    std::unordered_map<uintptr_t, int> mFbTextures; // CPU addr -> GPU FB id

    const std::unordered_map<Mtx*, MtxF>* mCurMtxReplacements;
    bool mMarkerOn; // This was originally a debug feature. Now it seems to control s2dex?
    std::unordered_map<size_t, const char*> mShaders;

    typedef size_t ShaderId;
    std::stack<ShaderId> mShaderStack;
    size_t mShadersIndex;
    int mInterpolationIndex;
    int mInterpolationIndexTarget;
    // Interpolation factor for the current rendered frame within a game tick:
    // 0 = previous tick, 1 = current tick. Set by the port before each
    // DrawAndRunGraphicsCommands call, like mInterpolationIndex.
    float mInterpolationT = 1.0f;
};

void gfx_set_target_ucode(UcodeHandlers ucode);
void gfx_push_current_dir(char* path);
int32_t gfx_check_image_signature(const char* imgData);
const char* gfx_get_shader(int16_t id);
const char* GfxGetOpcodeName(int8_t opcode);

} // namespace Fast

extern "C" void gfx_texture_cache_clear();
extern "C" void gfx_shader_cache_clear();
extern "C" int gfx_create_framebuffer(uint32_t width, uint32_t height, uint32_t native_width, uint32_t native_height,
                                      uint8_t resize, bool forceFixedAspect = false);