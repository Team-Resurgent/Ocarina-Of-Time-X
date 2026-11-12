// Fast3D's renderer on the original Xbox: Direct3D 8 and the NV2A's register combiners
// (gfx_direct3d8.h). The state cache, the fenced texture retirement and the combiner
// encoding follow UltraX's ux_gpu_d3d8.c / ux_rdp_hw.c.
#ifdef LUS_XBOX

#include "fast/backends/gfx_direct3d8.h"
#include "fast/backends/gfx_xbox.h"

#include <xtl.h>
#include <xgraphics.h>

#include <string>
#include <xmmintrin.h>
#include <spdlog/spdlog.h>

#include "fast/lus_gbi.h"

extern "C" double LusXbox_Seconds(void);       // gfx_xbox.cpp
extern "C" void LusXbox_AddWait(double seconds);
extern "C" void LusXbox_Where(const char* where); // xbox_main.cpp: where a hang is said to be
extern "C" double gLusxCyclesPerMs;            // xbox_main.cpp

// 0: a replay doesn't prefetch the next draw (Replay)
#ifndef LUSX_PREFETCH_DRAWS
#define LUSX_PREFETCH_DRAWS 1
#endif
// of a draw's vertices (the average is ~700 bytes): the prefetches beyond the four the CPU keeps in flight only queue
static constexpr size_t kPrefetchVertexBytes = 1024;

// a constant register compared bit for bit, inline: memcmp is a real call here, and this runs for every register
// of every draw of every frame
typedef uint32_t __attribute__((may_alias)) LusxWord;
static inline bool Same16(const void* a, const void* b) {
    const LusxWord* x = (const LusxWord*)a;
    const LusxWord* y = (const LusxWord*)b;
    return ((x[0] ^ y[0]) | (x[1] ^ y[1]) | (x[2] ^ y[2]) | (x[3] ^ y[3])) == 0;
}

namespace Fast {

namespace {

// vertex shader input registers (the declaration's)
enum { VR_POS = 0, VR_UV0 = 1, VR_UV1 = 2, VR_COLOR = 3, VR_FOG = 7 };

uint32_t Pot(uint32_t v) {
    uint32_t p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

uint8_t ToByte(float f) {
    int v = (int)(f * 255.0f + 0.5f);
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

// an RGBA of floats (0..1) as a D3DCOLOR: scaled and clamped four at once, then each converted (rounding);
// SSE1 only, the Pentium III having no SSE2 (nor MMX intrinsics, clang making those of SSE2)
inline DWORD PackColor(const float* rgba) {
    __m128 v = _mm_mul_ps(_mm_loadu_ps(rgba), _mm_set1_ps(255.0f));
    v = _mm_min_ps(_mm_max_ps(v, _mm_setzero_ps()), _mm_set1_ps(255.0f));
    DWORD r = (DWORD)_mm_cvtss_si32(v);
    DWORD g = (DWORD)_mm_cvtss_si32(_mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1)));
    DWORD b = (DWORD)_mm_cvtss_si32(_mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 2, 2, 2)));
    DWORD a = (DWORD)_mm_cvtss_si32(_mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 3, 3, 3)));
    return (a << 24) | (r << 16) | (g << 8) | b;
}

// an N64 combiner source as an NV2A combiner input; `alpha`: for the alpha half
DWORD CombinerInput(int src, bool alpha, int cycle) {
    DWORD ch = alpha ? PS_CHANNEL_ALPHA : PS_CHANNEL_RGB;

    switch (src) {
        case SHADER_0:
            return PS_REGISTER_ZERO;
        case SHADER_1:
            return PS_REGISTER_ONE;
        case SHADER_INPUT_1:
            return PS_REGISTER_V0 | ch;
        case SHADER_INPUT_2:
            return PS_REGISTER_V1 | ch;
        case SHADER_INPUT_3:
            return PS_REGISTER_T2 | ch;
        case SHADER_INPUT_4:
            return PS_REGISTER_T3 | ch;
        case SHADER_TEXEL0:
            return PS_REGISTER_T0 | ch;
        case SHADER_TEXEL0A:
            return PS_REGISTER_T0 | PS_CHANNEL_ALPHA;
        case SHADER_TEXEL1:
            return PS_REGISTER_T1 | ch;
        case SHADER_TEXEL1A:
            return PS_REGISTER_T1 | PS_CHANNEL_ALPHA;
        case SHADER_COMBINED:
            return cycle != 0 ? (PS_REGISTER_R0 | ch) : PS_REGISTER_ZERO;
        case SHADER_NOISE:
            return PS_REGISTER_ONE_HALF;
        default:
            return PS_REGISTER_ZERO; // inputs 5..7: not carried (see the header)
    }
}

// one stage computing a half's formula outright, if it can: d alone, or a * c
bool OneStage(const int f[4], bool alpha, int cycle, DWORD* inputs) {
    if (f[2] == SHADER_0) {
        *inputs = PS_COMBINERINPUTS(CombinerInput(f[3], alpha, cycle), PS_REGISTER_ONE, PS_REGISTER_ZERO,
                                    PS_REGISTER_ZERO);
        return true;
    }
    if (f[1] == SHADER_0 && f[3] == SHADER_0) {
        *inputs = PS_COMBINERINPUTS(CombinerInput(f[0], alpha, cycle), CombinerInput(f[2], alpha, cycle),
                                    PS_REGISTER_ZERO, PS_REGISTER_ZERO);
        return true;
    }
    return false;
}

// (a - b) into R1, then R1 * c + d into R0
void TwoStages(const int f[4], bool alpha, int cycle, DWORD* first, DWORD* second) {
    DWORD ch = alpha ? PS_CHANNEL_ALPHA : PS_CHANNEL_RGB;

    *first = PS_COMBINERINPUTS(CombinerInput(f[0], alpha, cycle), PS_REGISTER_ONE, CombinerInput(f[1], alpha, cycle),
                               PS_REGISTER_NEGATIVE_ONE);
    *second = PS_COMBINERINPUTS(PS_REGISTER_R1 | PS_INPUTMAPPING_SIGNED_IDENTITY | ch,
                                CombinerInput(f[2], alpha, cycle), CombinerInput(f[3], alpha, cycle),
                                PS_REGISTER_ONE);
}

const DWORD kAddress[4] = { D3DTADDRESS_WRAP, D3DTADDRESS_MIRROR, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP };

} // namespace

GfxRenderingAPID3D8::GfxRenderingAPID3D8(GfxWindowBackendXbox* window) : mWindow(window) {
}

GfxRenderingAPID3D8::~GfxRenderingAPID3D8() {
}

const char* GfxRenderingAPID3D8::GetName() {
    return "Direct3D 8 (Xbox)";
}

// The interpreter allocates its upload buffer as this squared, four bytes a texel: 512 is 1 MB.
// N64 textures are far smaller (TMEM is 4 KB); only a port's own images could be larger.
int GfxRenderingAPID3D8::GetMaxTextureSize() {
    return 512;
}

GfxClipParameters GfxRenderingAPID3D8::GetClipParameters() {
    return { true, false };
}

void GfxRenderingAPID3D8::UnloadShader(ShaderProgram* oldPrg) {
}

void GfxRenderingAPID3D8::LoadShader(ShaderProgram* newPrg) {
    mProgram = reinterpret_cast<Program*>(newPrg);
}

void GfxRenderingAPID3D8::ClearShaderCache() {
    for (auto& [key, p] : mPrograms) {
        mDev->DeletePixelShader(p.ps);
    }
    mPrograms.clear();
    mProgram = nullptr;
    mBoundPs = 0;
}

uint32_t GfxRenderingAPID3D8::VertexShaderFor(const Program& p) {
    uint32_t key = (p.usedTextures[0] ? 1 : 0) | (p.usedTextures[1] ? 2 : 0) | (p.colors << 2) | (p.fog ? 32 : 0);
    auto it = mVertexShaders.find(key);
    if (it != mVertexShaders.end()) {
        return it->second;
    }

    DWORD decl[16];
    int n = 0;
    std::string src = "xvs.1.1\nmov oPos, v0\n";
    decl[n++] = D3DVSD_STREAM(0);
    decl[n++] = D3DVSD_REG(VR_POS, D3DVSDT_FLOAT4);
    if (p.usedTextures[0]) {
        decl[n++] = D3DVSD_REG(VR_UV0, D3DVSDT_FLOAT2);
        src += "mul oT0.xy, v1.xy, c0.xy\n"; // c0: texture 0's scale (padding, or texels)
    }
    if (p.usedTextures[1]) {
        decl[n++] = D3DVSD_REG(VR_UV1, D3DVSDT_FLOAT2);
        src += "mul oT1.xy, v2.xy, c1.xy\n";
    }
    static const char* kColorOut[4] = { "oD0", "oD1", "oT2", "oT3" };
    for (int i = 0; i < p.colors; i++) {
        decl[n++] = D3DVSD_REG(VR_COLOR + i, D3DVSDT_D3DCOLOR);
        src += std::string("mov ") + kColorOut[i] + ", v" + std::to_string(VR_COLOR + i) + "\n";
    }
    if (p.fog) {
        decl[n++] = D3DVSD_REG(VR_FOG, D3DVSDT_FLOAT1);
        src += "mov oFog.x, v7.x\n";
    }
    decl[n++] = D3DVSD_END();

    LPXGBUFFER code = nullptr, errors = nullptr;
    DWORD handle = 0;
    HRESULT hr =
        XGAssembleShader("fast3d.vsh", src.data(), (UINT)src.size(), 0, NULL, &code, &errors, NULL, NULL, NULL, NULL);
    if (FAILED(hr) || code == nullptr) {
        SPDLOG_ERROR("GfxRenderingAPID3D8: vertex shader {:x} did not assemble: {}", key,
                     errors ? (const char*)errors->GetBufferPointer() : "?");
    } else if (FAILED(mDev->CreateVertexShader(decl, (const DWORD*)code->GetBufferPointer(), &handle, 0))) {
        SPDLOG_ERROR("GfxRenderingAPID3D8: vertex shader {:x} not made", key);
        handle = 0;
    }
    if (code) {
        code->Release();
    }
    if (errors) {
        errors->Release();
    }
    mVertexShaders[key] = handle;
    return handle;
}

// A lit batch's constants: c12 (255, -128, 4, 0), c13 the ambient colour, c14 (1, 1, 2, 0) (the distance's
// weights), then two for each directional light (its unit direction, its colour / 127) and three for each
// point light (its position, colour, attenuation's (a, b)); the slots after them, 9 each.
static int HwLitBase(int numDir, int numPoint) {
    return 15 + 2 * numDir + 3 * numPoint;
}

int GfxRenderingAPID3D8::HwLitSlots(int numDir, int numPoint) {
    // the shader's instructions (136 the NV2A's): ~55 without lights, 3 a directional light, 24 a point light
    if (mHwLitFailed || numDir > kHwMaxDirLights || numPoint > kHwMaxPointLights || 3 * numDir + 24 * numPoint > 78) {
        return 0;
    }
    int slots = (96 - HwLitBase(numDir, numPoint)) / kHwLitStride;
    return slots > kHwMaxSlots ? kHwMaxSlots : slots;
}

// The GPU transform's vertex shader, for a program's layout (and lights). A vertex: v0 the N64's object
// position and its state's slot * the stride (shorts), v1 its u, v (shorts), v2 its shade (lit or not on
// the CPU; alpha its own), or lit here, its normal + 128. Constants: c0, c1 the textures' coordinate scale
// (.xy) and offset (.zw), padding included; c2 (0, 0.001, 32767, 255): fog's guards; c3..c6 the combiner
// inputs' constant parts and c7..c10 their shade masks; c11 (1/255, shade alpha is 1, 0.5, 1); a slot: its
// MP rows, then (aspect, fog's multiplier and offset, fog on), and lit, the modelview's rows (row 0's w
// 1 / its scale), at c12 + 5 * slot, or after the lights (HwLitBase) + 9 * slot. As GfxSpVertex and
// GfxSpTri1 make them on the CPU: the position (x scaled for widescreen, z to D3D's 0..w and clamped there:
// the other backends' depth clamp), the lighting, fog (the N64's, w guarded the same way), the coordinates,
// and each input as C + shade * S.
uint32_t GfxRenderingAPID3D8::HwVertexShaderFor(const Program& p, const HwLights* lights, uint8_t lodMask) {
    const int numDir = lights ? lights->numDir : 0, numPoint = lights ? lights->numPoint : 0;
    uint32_t key = (p.usedTextures[0] ? 1 : 0) | (p.usedTextures[1] ? 2 : 0) | (p.colors << 2) | (p.fog ? 32 : 0) |
                   (lights ? 64 | (numDir << 7) | (numPoint << 10) : 0) | ((uint32_t)lodMask << 13);
    auto it = mHwVertexShaders.find(key);
    if (it != mHwVertexShaders.end()) {
        return it->second;
    }

    const int base = lights ? HwLitBase(numDir, numPoint) : 12;
    auto slot = [&](int off) { return "c[a0.x + " + std::to_string(base + off) + "]"; };
    auto cr = [](int n) { return "c" + std::to_string(n); };
    DWORD decl[] = { D3DVSD_STREAM(0), D3DVSD_REG(0, D3DVSDT_SHORT4), D3DVSD_REG(1, D3DVSDT_SHORT2),
                     D3DVSD_REG(2, D3DVSDT_D3DCOLOR), D3DVSD_END() };
    std::string src = "xvs.1.1\n"
                      "mov a0.x, v0.w\n";
    if (lights) {
        // GfxSpVertex's lighting, into r4's rgb: the ambient, each directional light by the normal through
        // the modelview over its scale (the light's direction taken back through it, normalized, on the
        // CPU), each point light by MM's formula (the distance's z twice, the direction to it in the model's
        // space clamped, the attenuation by the floored distance)
        src += "mov r10, c12\n"
               "mad r8, v2, r10.x, r10.y\n" // the normal, -128..127
               "mul r9, r8.x, " + slot(5) + "\n"
               "mad r9, r8.y, " + slot(6) + ", r9\n"
               "mad r9, r8.z, " + slot(7) + ", r9\n"
               "mov r10, " + slot(5) + "\n"
               "mul r9.xyz, r9, r10.w\n"
               "mov r4, c13\n";
        for (int i = 0; i < numDir; i++) {
            src += "dp3 r6.x, r9, " + cr(15 + 2 * i) + "\n"
                   "max r6.x, r6.x, c2.x\n"
                   "mad r4.xyz, r6.x, " + cr(16 + 2 * i) + ", r4\n";
        }
        if (numPoint > 0) {
            src += "mul r11, v0.x, " + slot(5) + "\n"
                   "mad r11, v0.y, " + slot(6) + ", r11\n"
                   "mad r11, v0.z, " + slot(7) + ", r11\n"
                   "add r11, r11, " + slot(8) + "\n"; // the vertex in the view's space
        }
        for (int j = 0; j < numPoint; j++) {
            int cp = 15 + 2 * numDir + 3 * j;
            src += "add r0, " + cr(cp) + ", -r11\n"
                   "mul r1, r0, r0\n"
                   "dp3 r1.x, r1, c14\n" // the distance squared
                   "dp3 r2.x, r0, " + slot(5) + "\n"
                   "dp3 r2.y, r0, " + slot(6) + "\n"
                   "dp3 r2.z, r0, " + slot(7) + "\n"
                   "rcp r1.y, r1.x\n"
                   "mul r1.y, r1.y, c12.z\n"
                   "mul r2.xyz, r2, r1.y\n"
                   "max r2.xyz, r2, -c11.w\n"
                   "min r2.xyz, r2, c11.w\n"
                   "dp3 r2.w, r2, r8\n"
                   "min r2.w, r2.w, c11.w\n"
                   "rsq r1.z, r1.x\n"
                   "mul r1.z, r1.x, r1.z\n" // the distance
                   "expp r3.y, r1.z\n"
                   "add r1.w, r1.z, -r3.y\n" // floored
                   "mov r5, " + cr(cp + 2) + "\n"
                   "mad r3.x, r1.w, r5.y, r5.x\n"
                   "mad r3.x, r3.x, r1.w, c11.w\n"
                   "rcp r3.x, r3.x\n"
                   "mul r2.w, r2.w, r3.x\n"
                   "max r2.w, r2.w, c2.x\n"
                   "mad r4.xyz, r2.w, " + cr(cp + 1) + ", r4\n";
        }
        src += "min r4.xyz, r4, c2.w\n"
               "mul r4.xyz, r4, c11.x\n";
    }
    src += "mul r0, v0.x, " + slot(0) + "\n"
           "mad r0, v0.y, " + slot(1) + ", r0\n"
           "mad r0, v0.z, " + slot(2) + ", r0\n"
           "add r0, r0, " + slot(3) + "\n"
           "mov r1, " + slot(4) + "\n";
    src +=            // fog: z / w (w kept off 0, a negative 1/w taken as 32767) * mul + offset, 0..255
                      "max r2.x, r0.w, -r0.w\n"
                      "slt r2.y, r2.x, c2.y\n"
                      "add r2.z, c2.y, -r0.w\n"
                      "mad r2.z, r2.y, r2.z, r0.w\n"
                      "rcp r2.w, r2.z\n"
                      "slt r3.x, r2.w, c2.x\n"
                      "add r3.y, c2.z, -r2.w\n"
                      "mad r2.w, r3.x, r3.y, r2.w\n"
                      "mul r3.z, r0.z, r2.w\n"
                      "mad r3.z, r3.z, r1.y, r1.z\n"
                      "max r3.z, r3.z, c2.x\n"
                      "min r3.z, r3.z, c2.w\n"
                      "mul r3.z, r3.z, c11.x\n"
                      // the vertex's alpha, or fog's where its state had fog on; shade's alpha, or 1
                      "add r3.w, r3.z, -v2.w\n"
                      "mad r3.w, r1.w, r3.w, v2.w\n"
                      "add r4.w, c11.w, -r3.w\n"
                      "mad r4.w, c11.y, r4.w, r3.w\n"
                      ;
    if (!lights) {
        src += "mov r4.xyz, v2.xyz\n";
    }
    src +=            // the position: x for widescreen, z to 0..w
                      "mul r5.x, r0.x, r1.x\n"
                      "add r5.z, r0.z, r0.w\n"
                      "mul r5.z, r5.z, c11.z\n"
                      "max r5.z, r5.z, c2.x\n"
                      "min r5.z, r5.z, r0.w\n"
                      "mov r5.y, r0.y\n"
                      "mov r5.w, r0.w\n"
                      "mov oPos, r5\n";
    // (one constant register an instruction: vs.1.1's rule)
    if (p.usedTextures[0]) {
        src += "mov r6, c0\n"
               "mad oT0.xy, v1.xy, r6.xy, r6.zw\n";
    }
    if (p.usedTextures[1]) {
        src += "mov r6, c1\n"
               "mad oT1.xy, v1.xy, r6.xy, r6.zw\n";
    }
    static const char* kColorOut[4] = { "oD0", "oD1", "oT2", "oT3" };
    for (int i = 0; i < p.colors; i++) {
        const int lod = (lodMask >> (2 * i)) & 3;
        if (lod == 0) {
            src += "mul r7, r4, c" + std::to_string(7 + i) + "\n" + "add " + kColorOut[i] + ", r7, c" +
                   std::to_string(3 + i) + "\n";
            continue;
        }
        // the LOD fraction's channels: the vertex's w through the input's S and C (w / 3000 - 1, as the CPU
        // path's "hack"), clamped to 0..1; the others as above
        const char* m = lod == 3 ? "" : lod == 1 ? ".xyz" : ".w";
        src += "mul r7, r4, c" + std::to_string(7 + i) + "\n" + "add r7, r7, c" + std::to_string(3 + i) + "\n" +
               "mul r8" + m + ", r0.w, c" + std::to_string(7 + i) + "\n" + "add r7" + m + ", r8, c" +
               std::to_string(3 + i) + "\n" + "max r7" + m + ", r7, c2.x\n" + "min r7" + m + ", r7, c11.w\n" +
               "mov " + kColorOut[i] + ", r7\n";
    }
    if (p.fog) {
        src += "add oFog.x, c11.w, -r3.w\n"; // D3D's: 1 is no fog
    }

    LPXGBUFFER code = nullptr, errors = nullptr;
    DWORD handle = 0;
    HRESULT hr =
        XGAssembleShader("fast3d_hw.vsh", src.data(), (UINT)src.size(), 0, NULL, &code, &errors, NULL, NULL, NULL, NULL);
    if (FAILED(hr) || code == nullptr) {
        SPDLOG_ERROR("GfxRenderingAPID3D8: GPU-transform vertex shader {:x} did not assemble: {}", key,
                     errors ? (const char*)errors->GetBufferPointer() : "?");
    } else if (FAILED(mDev->CreateVertexShader(decl, (const DWORD*)code->GetBufferPointer(), &handle, 0))) {
        SPDLOG_ERROR("GfxRenderingAPID3D8: GPU-transform vertex shader {:x} not made", key);
        handle = 0;
    }
    if (code) {
        code->Release();
    }
    if (errors) {
        errors->Release();
    }
    if (handle == 0) {
        // the CPU's path from the next frame: for lighting only, if a lit shader
        (lights ? mHwLitFailed : mHwFailed) = true;
    }
    mHwVertexShaders[key] = handle;
    return handle;
}

void GfxRenderingAPID3D8::DrawTrianglesHw(const HwBatch& b) {
    if (mProgram == nullptr || b.numStates <= 0) {
        return;
    }
    if (mRecording) {
        RecordDraw(true, b.verts, b.tris * 3 * 16, b.tris, b.fogColor, &b);
        return;
    }
    // (all of a batch can be triangles watched for cutting, and none of them in sight this frame)
    const uint32_t clipTris = mReplayDraw != nullptr ? mReplayDraw->clipTris : 0;
    if (b.tris == 0 && clipTris == 0) {
        return;
    }
    if (!mInScene) {
        mDev->BeginScene();
        mInScene = true;
    }
    const uint64_t stateStart = __builtin_ia32_rdtsc();
    ApplyDrawState(true, b.cull, b.lights, b.lodMask);
    const uint64_t constStart = __builtin_ia32_rdtsc();
    mStateCycles += constStart - stateStart;
    if (mBoundVs == 0) {
        return; // the shader didn't assemble (logged; the CPU's path from the next frame)
    }
    if (mProgram->cc.opt_fog && b.fogColor != mBoundFogColor) {
        mDev->SetRenderState(D3DRS_FOGCOLOR, b.fogColor);
        mBoundFogColor = b.fogColor;
    }
    float c[96][4];
    for (int t = 0; t < 2; t++) {
        c[t][0] = b.texAffine[t][0] * mStageScale[t][0];
        c[t][1] = b.texAffine[t][1] * mStageScale[t][1];
        c[t][2] = b.texAffine[t][2] * mStageScale[t][0];
        c[t][3] = b.texAffine[t][3] * mStageScale[t][1];
    }
    c[2][0] = 0.0f;
    c[2][1] = 0.001f;
    c[2][2] = 32767.0f;
    c[2][3] = 255.0f;
    memcpy(c[3], b.inputC, sizeof(b.inputC));
    memcpy(c[7], b.inputS, sizeof(b.inputS));
    c[11][0] = 1.0f / 255.0f;
    c[11][1] = b.shadeAlphaOne;
    c[11][2] = 0.5f;
    c[11][3] = 1.0f;
    int base = 12, stride = kHwStride, n = b.numStates;
    if (b.lights != nullptr) {
        const HwLights& l = *b.lights;
        base = HwLitBase(l.numDir, l.numPoint);
        stride = kHwLitStride;
        int maxSlots = HwLitSlots(l.numDir, l.numPoint);
        n = n > maxSlots ? maxSlots : n;
        const float c12[4] = { 255.0f, -128.0f, 4.0f, 0.0f }, c14[4] = { 1.0f, 1.0f, 2.0f, 0.0f };
        memcpy(c[12], c12, sizeof(c12));
        c[13][0] = l.ambient[0];
        c[13][1] = l.ambient[1];
        c[13][2] = l.ambient[2];
        c[13][3] = 0.0f;
        memcpy(c[14], c14, sizeof(c14));
        for (int i = 0; i < l.numDir; i++) {
            for (int k = 0; k < 3; k++) {
                c[15 + 2 * i][k] = l.dir[i][k];
                c[16 + 2 * i][k] = l.dirCol[i][k] * (1.0f / 127.0f); // the normal's 127
            }
            c[15 + 2 * i][3] = c[16 + 2 * i][3] = 0.0f;
        }
        for (int j = 0; j < l.numPoint; j++) {
            int cp = 15 + 2 * l.numDir + 3 * j;
            for (int k = 0; k < 3; k++) {
                c[cp][k] = l.pos[j][k];
                c[cp + 1][k] = l.posCol[j][k];
            }
            c[cp][3] = c[cp + 1][3] = 0.0f;
            c[cp + 2][0] = l.att[j][0];
            c[cp + 2][1] = l.att[j][1];
            c[cp + 2][2] = c[cp + 2][3] = 0.0f;
        }
    } else {
        n = n > kHwMaxSlots ? kHwMaxSlots : n;
    }
    // a slot that holds the same state as in this frame's last draw of this layout is as it was: constants were
    // the biggest part of a frame's draws in a log from a 733 MHz machine (3-3.75 ms a frame)
    if (mReplayDraw == nullptr || base != mSlotBase || stride != mSlotStride) {
        mSlotCount = 0;
    }
    uint32_t stale = 0;
    for (int k = 0; k < n; k++) {
        if (k < mSlotCount && mSlotState[k] == b.slotStates[k]) {
            continue;
        }
        stale |= 1u << k;
        mSlotState[k] = b.slotStates[k];
        const HwState& s = b.states[b.slotStates[k]];
        float(*slot)[4] = &c[base + stride * k];
        memcpy(slot, s.mp, sizeof(s.mp));
        slot[4][0] = s.aspect;
        slot[4][1] = s.fogMul;
        slot[4][2] = s.fogOffset;
        slot[4][3] = s.fog;
        if (b.lights != nullptr) {
            memcpy(slot + 5, s.mv, sizeof(s.mv));
            slot[5][3] = s.nscale;
        }
    }
    // only the registers that changed (most draws change a texture's and the combiner's few; the slots of
    // a room's or a body's draws are the same): every register sent is 16 bytes of push buffer the GPU
    // reads, and an emulator handles one method at a time
    auto send = [&](int from, int to) {
        for (int k = from; k < to;) {
            if (mVsConstKnown[k] && Same16(mVsConst[k], c[k])) {
                k++;
                continue;
            }
            int e = k + 1;
            while (e < to && !(mVsConstKnown[e] && Same16(mVsConst[e], c[e]))) {
                e++;
            }
            mDev->SetVertexShaderConstantFast(k, c[k], e - k); // (mVsConst is our copy: D3D's isn't needed)
            memcpy(mVsConst[k], c[k], sizeof(c[0]) * (e - k));
            memset(mVsConstKnown + k, 1, e - k);
            mConstsSent += e - k;
            k = e;
        }
    };
    send(0, base);
    for (int k = 0; k < n;) {
        if ((stale & (1u << k)) == 0) {
            k++;
            continue;
        }
        int e = k + 1;
        while (e < n && (stale & (1u << e)) != 0) {
            e++;
        }
        send(base + stride * k, base + stride * e);
        k = e;
    }
    mSlotBase = base;
    mSlotStride = stride;
    mSlotCount = n > mSlotCount ? n : mSlotCount;
    mBoundScale[0][0] = mBoundScale[1][0] = -1.0f; // the CPU path's c0, c1 to be set again
    const uint64_t drawStart = __builtin_ia32_rdtsc();
    mConstCycles += drawStart - constStart;
    if (b.tris > 0) {
        mDev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)b.tris, b.verts, 16);
    }
    if (clipTris > 0) {
        mDev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, clipTris, mClipVerts + mReplayDraw->clipStart, 16);
    }
    const uint64_t drawCycles = __builtin_ia32_rdtsc() - drawStart;
    mDrawCycles += drawCycles;
    if (drawCycles > gLusxCyclesPerMs * 0.1) { // 100 us, far over a copy's time: it waited for room in the push buffer
        mPushWaits++;
        mPushWaitCycles += drawCycles;
    }
    mDraws++;
    mTris += (uint32_t)b.tris + clipTris;
    mHwDraws++;
    mHwTris += b.tris + clipTris;
}

ShaderProgram* GfxRenderingAPID3D8::CreateAndLoadNewShader(uint64_t shaderId0, uint64_t shaderId1) {
    Program& p = mPrograms[std::make_pair(shaderId0, shaderId1)];
    CCFeatures& cc = p.cc;
    D3DPIXELSHADERDEF def;
    int stages = 0;

    gfx_cc_get_features(shaderId0, shaderId1, &cc);
    p.id0 = shaderId0;
    p.id1 = shaderId1;
    p.numInputs = (uint8_t)cc.numInputs;
    p.colors = (uint8_t)(cc.numInputs < 4 ? cc.numInputs : 4);
    p.usedTextures[0] = cc.usedTextures[0];
    p.usedTextures[1] = cc.usedTextures[1];
    p.fog = cc.opt_fog;
    p.numFloats = 4;
    for (int t = 0; t < 2; t++) {
        if (cc.usedTextures[t]) {
            p.numFloats += 2 + (cc.clamp[t][0] ? 1 : 0) + (cc.clamp[t][1] ? 1 : 0);
        }
    }
    p.numFloats += (cc.opt_fog ? 4 : 0) + (cc.opt_grayscale ? 4 : 0) + cc.numInputs * (cc.opt_alpha ? 4 : 3);
    p.stride = (uint8_t)(16 + (p.usedTextures[0] ? 8 : 0) + (p.usedTextures[1] ? 8 : 0) + p.colors * 4 +
                         (p.fog ? 4 : 0));
    if (cc.numInputs > 4) {
        SPDLOG_WARN("GfxRenderingAPID3D8: combiner {:016x}/{:x} has {} inputs; the NV2A path carries 4", shaderId0,
                    shaderId1, cc.numInputs);
    }

    memset(&def, 0, sizeof(def));
    for (int c = 0; c < (cc.opt_2cyc ? 2 : 1); c++) {
        DWORD rgb1, alpha1;
        bool oneRgb = OneStage(cc.c[c][0], false, c, &rgb1);
        bool oneAlpha = OneStage(cc.c[c][1], true, c, &alpha1);

        if (oneRgb && oneAlpha) {
            def.PSRGBInputs[stages] = rgb1;
            def.PSAlphaInputs[stages] = alpha1;
            def.PSRGBOutputs[stages] =
                PS_COMBINEROUTPUTS(PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, PS_REGISTER_R0, PS_COMBINEROUTPUT_IDENTITY);
            def.PSAlphaOutputs[stages] = def.PSRGBOutputs[stages];
            stages++;
        } else {
            DWORD rgb2, alpha2;

            TwoStages(cc.c[c][0], false, c, &rgb1, &rgb2);
            TwoStages(cc.c[c][1], true, c, &alpha1, &alpha2);
            def.PSRGBInputs[stages] = rgb1;
            def.PSAlphaInputs[stages] = alpha1;
            def.PSRGBOutputs[stages] =
                PS_COMBINEROUTPUTS(PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, PS_REGISTER_R1, PS_COMBINEROUTPUT_IDENTITY);
            def.PSAlphaOutputs[stages] = def.PSRGBOutputs[stages];
            stages++;
            def.PSRGBInputs[stages] = rgb2;
            def.PSAlphaInputs[stages] = alpha2;
            def.PSRGBOutputs[stages] =
                PS_COMBINEROUTPUTS(PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, PS_REGISTER_R0, PS_COMBINEROUTPUT_IDENTITY);
            def.PSAlphaOutputs[stages] = def.PSRGBOutputs[stages];
            stages++;
        }
    }
    // fog in the final combiner: f * R0 + (1 - f) * fog colour (D3D's f: 1 is no fog)
    if (cc.opt_fog) {
        def.PSFinalCombinerInputsABCD =
            PS_COMBINERINPUTS(PS_REGISTER_FOG | PS_CHANNEL_ALPHA, PS_REGISTER_R0 | PS_CHANNEL_RGB,
                              PS_REGISTER_FOG | PS_CHANNEL_RGB, PS_REGISTER_ZERO);
    } else {
        def.PSFinalCombinerInputsABCD =
            PS_COMBINERINPUTS(PS_REGISTER_ZERO, PS_REGISTER_ZERO, PS_REGISTER_R0 | PS_CHANNEL_RGB, PS_REGISTER_ZERO);
    }
    def.PSFinalCombinerInputsEFG = PS_COMBINERINPUTS(
        PS_REGISTER_ZERO, PS_REGISTER_ZERO, cc.opt_alpha ? (PS_REGISTER_R0 | PS_CHANNEL_ALPHA) : PS_REGISTER_ONE, 0);
    def.PSCombinerCount = PS_COMBINERCOUNT(stages, PS_COMBINERCOUNT_MUX_MSB | PS_COMBINERCOUNT_SAME_C0 |
                                                       PS_COMBINERCOUNT_SAME_C1);
    def.PSTextureModes = PS_TEXTUREMODES(cc.usedTextures[0] ? PS_TEXTUREMODES_PROJECT2D : PS_TEXTUREMODES_NONE,
                                         cc.usedTextures[1] ? PS_TEXTUREMODES_PROJECT2D : PS_TEXTUREMODES_NONE,
                                         p.colors >= 3 ? PS_TEXTUREMODES_PASSTHRU : PS_TEXTUREMODES_NONE,
                                         p.colors >= 4 ? PS_TEXTUREMODES_PASSTHRU : PS_TEXTUREMODES_NONE);
    DWORD ps = 0;
    if (FAILED(mDev->CreatePixelShader(&def, &ps))) {
        SPDLOG_ERROR("GfxRenderingAPID3D8: pixel shader for {:016x}/{:x} not made", shaderId0, shaderId1);
        ps = 0;
    }
    p.ps = ps;
    p.vs = VertexShaderFor(p);
    mPrograms_++;
    mProgram = &p;
    return reinterpret_cast<ShaderProgram*>(&p);
}

ShaderProgram* GfxRenderingAPID3D8::LookupShader(uint64_t shaderId0, uint64_t shaderId1) {
    auto it = mPrograms.find(std::make_pair(shaderId0, shaderId1));
    return it == mPrograms.end() ? nullptr : reinterpret_cast<ShaderProgram*>(&it->second);
}

void GfxRenderingAPID3D8::ShaderGetInfo(ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) {
    Program* p = reinterpret_cast<Program*>(prg);
    *numInputs = p->numInputs;
    usedTextures[0] = p->usedTextures[0];
    usedTextures[1] = p->usedTextures[1];
}

uint32_t GfxRenderingAPID3D8::NewTexture() {
    mTextures.emplace_back();
    return (uint32_t)(mTextures.size() - 1);
}

void GfxRenderingAPID3D8::SelectTexture(int tile, uint32_t textureId) {
    mCurrentTile = tile;
    mCurrentTextureIds[tile & 1] = textureId;
}

void GfxRenderingAPID3D8::Retire(D3DTexture* tex) {
    if (tex == nullptr) {
        return;
    }
    for (int s = 0; s < 2; s++) {
        if (mBoundTex[s] == tex) {
            mDev->SetTexture(s, NULL);
            mBoundTex[s] = nullptr;
        }
    }
    // a tick's recording may still draw with it: its fence is the tick's last replay's (EndFrame)
    mRetired.push_back({ tex, mTickOpen ? 0u : mDev->InsertFence() });
}

void GfxRenderingAPID3D8::ReleaseRetired(bool all) {
    size_t k = 0;
    if (all) {
        mDev->BlockUntilIdle();
    }
    for (size_t i = 0; i < mRetired.size(); i++) {
        if (all || (mRetired[i].fence != 0 && !mDev->IsFencePending(mRetired[i].fence))) {
            mRetired[i].tex->Release();
        } else {
            mRetired[k++] = mRetired[i];
        }
    }
    mRetired.resize(k);
}

// A texture's pixels through a conversion, the last column and row repeated out to the padded size
template <typename T, typename F>
static inline void ConvertPixels(const uint32_t* px, uint32_t width, uint32_t height, uint32_t pw, uint32_t ph,
                                 uint8_t* dst, F convert) {
    T* out = (T*)dst;
    for (uint32_t y = 0; y < height; y++, px += width, out += pw) {
        for (uint32_t x = 0; x < width; x++) {
            out[x] = convert(px[x]);
        }
        for (uint32_t x = width; x < pw; x++) {
            out[x] = out[width - 1];
        }
    }
    for (uint32_t y = height; y < ph; y++, out += pw) {
        memcpy(out, out - pw, pw * sizeof(T));
    }
}

// A cut-out texture's transparent texels take the colour of the solid ones beside them. Filtering mixes a transparent
// texel's colour into the edge next to it, and the game's textures leave any colour there (cyan, white): an outline in
// that colour around the shape. Only transparent texels change, so the wrap at the edges does no harm.
static void BleedCutoutEdges(uint16_t* px, uint32_t pw, uint32_t ph) {
    for (uint32_t y = 0; y < ph; y++) {
        uint16_t* row = px + y * pw;
        for (uint32_t x = 0; x < pw; x++) {
            uint32_t r = 0, g = 0, b = 0, n = 0;
            if (row[x] & 0x8000) {
                continue;
            }
            for (uint32_t dy = ph - 1; dy <= ph + 1; dy++) {
                const uint16_t* beside = px + ((y + dy) & (ph - 1)) * pw;
                for (uint32_t dx = pw - 1; dx <= pw + 1; dx++) {
                    const uint32_t v = beside[(x + dx) & (pw - 1)];
                    if (v & 0x8000) {
                        r += v & 0x7C00;
                        g += v & 0x03E0;
                        b += v & 0x001F;
                        n++;
                    }
                }
            }
            if (n != 0) {
                row[x] = (uint16_t)(((r / n) & 0x7C00) | ((g / n) & 0x03E0) | (b / n));
            }
        }
    }
}

// RGBA32 to the smallest Xbox format that holds it exactly, padded to a power of two
// (the edges repeated, for clamping) and swizzled
void GfxRenderingAPID3D8::UploadTexture(const uint8_t* rgba, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || mDev == nullptr) {
        return;
    }
    Texture& t = mTextures[mCurrentTextureIds[mCurrentTile & 1]];
    uint32_t n = width * height;
    // a pixel a word (R the low byte), no branches: alpha all 255, alpha only 0 or 255 (a + 1 is 1 or 256), R=G=B
    const uint32_t* px = (const uint32_t*)rgba;
    uint32_t alphas = 0xFFFFFFFFu, partial = 0, colour = 0;

    for (uint32_t i = 0; i < n; i++) {
        const uint32_t v = px[i];
        alphas &= v;
        partial |= (v >> 24) + 1;
        colour |= v ^ (v >> 8);
    }
    const bool opaque = (alphas >> 24) == 0xFF, binary = (partial & 0xFE) == 0, gray = (colour & 0xFFFF) == 0;
    D3DFORMAT fmt;
    uint32_t bpp;
    if (gray && opaque) {
        fmt = D3DFMT_L8;
        bpp = 1;
    } else if (gray) {
        fmt = D3DFMT_A8L8;
        bpp = 2;
    } else if (opaque) {
        fmt = D3DFMT_R5G6B5;
        bpp = 2;
    } else if (binary) {
        fmt = D3DFMT_A1R5G5B5;
        bpp = 2;
    } else {
        fmt = D3DFMT_A8R8G8B8;
        bpp = 4;
    }
    uint32_t pw = Pot(width), ph = Pot(height);
    mConvert.resize((size_t)pw * ph * bpp);
    uint8_t* dst = mConvert.data();
    switch (fmt) {
        case D3DFMT_L8:
            ConvertPixels<uint8_t>(px, width, height, pw, ph, dst, [](uint32_t v) { return (uint8_t)v; });
            break;
        case D3DFMT_A8L8:
            ConvertPixels<uint16_t>(px, width, height, pw, ph, dst,
                                    [](uint32_t v) { return (uint16_t)((v & 0xFF) | ((v >> 16) & 0xFF00)); });
            break;
        case D3DFMT_R5G6B5:
            ConvertPixels<uint16_t>(px, width, height, pw, ph, dst, [](uint32_t v) {
                return (uint16_t)(((v & 0xF8) << 8) | ((v >> 5) & 0x7E0) | ((v >> 19) & 0x1F));
            });
            break;
        case D3DFMT_A1R5G5B5:
            ConvertPixels<uint16_t>(px, width, height, pw, ph, dst, [](uint32_t v) {
                return (uint16_t)(((v >> 24) ? 0x8000 : 0) | ((v & 0xF8) << 7) | ((v >> 6) & 0x3E0) |
                                  ((v >> 19) & 0x1F));
            });
            BleedCutoutEdges((uint16_t*)dst, pw, ph);
            break;
        default:
            ConvertPixels<uint32_t>(px, width, height, pw, ph, dst, [](uint32_t v) {
                return (v & 0xFF00FF00u) | ((v & 0xFF) << 16) | ((v >> 16) & 0xFF);
            });
            break;
    }
    D3DTexture* tex = nullptr;
    if (FAILED(mDev->CreateTexture(pw, ph, 1, 0, fmt, 0, &tex))) {
        SPDLOG_ERROR("GfxRenderingAPID3D8: no {}x{} texture (out of memory?)", pw, ph);
        return;
    }
    D3DLOCKED_RECT lr;
    tex->LockRect(0, &lr, NULL, 0);
    XGSwizzleRect(mConvert.data(), pw * bpp, NULL, lr.pBits, pw, ph, NULL, bpp);
    tex->UnlockRect(0);
    Retire(t.tex);
    t.tex = tex;
    t.w = (uint16_t)width;
    t.h = (uint16_t)height;
    t.pw = (uint16_t)pw;
    t.ph = (uint16_t)ph;
    t.bytes = pw * ph * bpp;
    t.linear = false;
    mUploads++;
    mUploadBytes += pw * ph * bpp;
    // the big ones, for the memory budget (the first 40)
    static uint32_t sBigLogged;
    if (t.bytes >= 64 * 1024 && sBigLogged < 40) {
        sBigLogged++;
        SPDLOG_INFO("GfxRenderingAPID3D8: big texture {}x{} (as {}x{}, {}-bit): {} KB", width, height, pw, ph, bpp * 8,
                    t.bytes >> 10);
    }
}

void GfxRenderingAPID3D8::SetSamplerParameters(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt) {
    Texture& t = mTextures[mCurrentTextureIds[sampler & 1]];
    t.filter = linear_filter;
    t.addrU = (uint8_t)((cms & G_TX_CLAMP) ? 2 : (cms & G_TX_MIRROR) ? 1 : 0);
    t.addrV = (uint8_t)((cmt & G_TX_CLAMP) ? 2 : (cmt & G_TX_MIRROR) ? 1 : 0);
}

void GfxRenderingAPID3D8::SetDepthTestAndMask(bool depth_test, bool z_upd) {
    mCurrentDepthTest = depth_test;
    mCurrentDepthMask = z_upd;
}

void GfxRenderingAPID3D8::SetZmodeDecal(bool decal) {
    mCurrentZmodeDecal = decal;
}

// Fast3D's rectangles count y from the bottom, as OpenGL does
void GfxRenderingAPID3D8::SetViewport(int x, int y, int width, int height) {
    D3DVIEWPORT8 vp;
    int top = (int)mTargetH - y - height;
    int x1 = x + width, y1 = top + height;

    if (mRecording) {
        mRecCmds.push_back({ REC_VIEWPORT, { x, y, width, height }, 0.0f });
        return;
    }
    x = x < 0 ? 0 : x;
    top = top < 0 ? 0 : top;
    x1 = x1 > (int)mTargetW ? (int)mTargetW : x1;
    y1 = y1 > (int)mTargetH ? (int)mTargetH : y1;
    if (x1 <= x || y1 <= top) {
        return;
    }
    vp.X = x;
    vp.Y = top;
    vp.Width = x1 - x;
    vp.Height = y1 - top;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    mDev->SetViewport(&vp);
}

void GfxRenderingAPID3D8::SetScissor(int x, int y, int width, int height) {
    D3DRECT r;
    if (mRecording) {
        mRecCmds.push_back({ REC_SCISSOR, { x, y, width, height }, 0.0f });
        return;
    }
    r.x1 = x;
    r.y1 = (int)mTargetH - y - height;
    r.x2 = x + width;
    r.y2 = (int)mTargetH - y;
    mDev->SetScissors(1, FALSE, &r);
}

void GfxRenderingAPID3D8::SetUseAlpha(bool useAlpha) {
    mUseAlpha = useAlpha;
}

#define RS(state, cached, value)                     \
    do {                                             \
        int v_ = (int)(value);                       \
        if ((cached) != v_) {                        \
            mDev->SetRenderState((state), (DWORD)v_); \
            (cached) = v_;                           \
        }                                            \
    } while (0)

// `hw`: for the GPU transform's draw (its vertex shader, the batch's cull; the stage scales are left in
// mStageScale for its constants); else the CPU path's (no culling: the interpreter culled)
void GfxRenderingAPID3D8::ApplyDrawState(bool hw, int cull, const HwLights* lights, uint8_t lodMask) {
    const CCFeatures& cc = mProgram->cc;
    bool hasDepth = mCurrentFb == 0 || mFramebuffers[mCurrentFb].hasDepth;
    bool zTest = mCurrentDepthTest && hasDepth;
    bool zWrite = mCurrentDepthMask && hasDepth;

    RS(D3DRS_ZENABLE, mBoundZEnable, (zTest || zWrite) ? D3DZB_TRUE : D3DZB_FALSE);
    RS(D3DRS_ZWRITEENABLE, mBoundZWrite, zWrite);
    RS(D3DRS_ZFUNC, mBoundZFunc, zTest ? (mCurrentZmodeDecal ? D3DCMP_LESSEQUAL : D3DCMP_LESS) : D3DCMP_ALWAYS);
    // A decal (a dirt road on the grass, a drawing on a wall) lies on what's under it, triangles that don't match, so
    // the two fight at a slant. The offset as the PC backends' "no vanishing paths" mode: by the slope, height / 100.
    // D3DRS_ZBIAS's slope is fixed, and with 720p's 16-bit depth let the grass through.
    const int zBias = mCurrentZmodeDecal ? (int)mTargetH : 0;
    if (mBoundZBias != zBias) {
        const float slope = -(float)zBias / 100.0f, units = -2.0f;
        DWORD slopeBits, unitsBits;
        memcpy(&slopeBits, &slope, sizeof(slopeBits));
        memcpy(&unitsBits, &units, sizeof(unitsBits));
        mDev->SetRenderState(D3DRS_POLYGONOFFSETZSLOPESCALE, slopeBits);
        mDev->SetRenderState(D3DRS_POLYGONOFFSETZOFFSET, unitsBits);
        mDev->SetRenderState(D3DRS_SOLIDOFFSETENABLE, zBias != 0);
        mBoundZBias = zBias;
    }
    // a texture edge is cut, not blended (the PC shaders make its alpha 1): blended, its filtered edge mixed with what
    // was behind it so far, the sky, and wrote depth, so the wall drawn after left a sky-coloured outline
    RS(D3DRS_ALPHABLENDENABLE, mBoundBlend, cc.opt_alpha && !cc.opt_texture_edge);
    int ref = cc.opt_texture_edge ? 48 : cc.opt_alpha_threshold ? 8 : -1;
    // A blended draw with no depth to write: the pixels too faint to change the picture (under one step of a 16-bit
    // screen) needn't read and write the screen. Smoke is mostly these, in big quads near the camera, many deep: a
    // Goron's roll on ice at 720p took a tick from 50 ms to 94 with the CPU idle.
    if (ref < 0 && cc.opt_alpha && !zWrite) {
        ref = 8;
    }
    RS(D3DRS_ALPHATESTENABLE, mBoundAlphaTest, ref >= 0);
    if (ref >= 0) {
        RS(D3DRS_ALPHAREF, mBoundAlphaRef, ref);
    }
    RS(D3DRS_COLORWRITEENABLE, mBoundColorWrite, cc.opt_invisible ? 0 : D3DCOLORWRITEENABLE_ALL);
    RS(D3DRS_FOGENABLE, mBoundFog, cc.opt_fog);
    RS(D3DRS_CULLMODE, mBoundCull, cull == 1 ? D3DCULL_CW : cull == 2 ? D3DCULL_CCW : D3DCULL_NONE);

    if (mBoundPs != mProgram->ps) {
        mDev->SetPixelShader(mProgram->ps);
        mBoundPs = mProgram->ps;
    }
    uint32_t vs;
    if (!hw) {
        vs = mProgram->vs;
    } else if (lodMask != 0) {
        vs = HwVertexShaderFor(*mProgram, lights, lodMask); // rare (the LOD fraction): its own map lookup
    } else if (lights) {
        // the program's lit shaders by their light counts (a map lookup a lit draw otherwise)
        uint32_t& slot = mProgram->hwvsLit[lights->numDir * (kHwMaxPointLights + 1) + lights->numPoint];
        if (slot == 0) {
            slot = HwVertexShaderFor(*mProgram, lights, 0);
        }
        vs = slot;
    } else {
        vs = mProgram->hwvs ? mProgram->hwvs : (mProgram->hwvs = HwVertexShaderFor(*mProgram, nullptr, 0));
    }
    if (hw && vs == 0) {
        mBoundVs = 0; // DrawTrianglesHw draws nothing
    } else if (vs != 0 && mBoundVs != vs) {
        mDev->SetVertexShader(vs);
        mBoundVs = vs;
    }
    for (int s = 0; s < 2; s++) {
        mStageScale[s][0] = mStageScale[s][1] = 1.0f;
        if (!mProgram->usedTextures[s]) {
            continue;
        }
        TexBind b;
        if (mReplayDraw != nullptr) {
            b = mReplayDraw->tex[s];
        } else {
            ResolveTexture(s, b);
        }
        if (b.tex == nullptr) {
            // a stage the shader samples is never left empty (a texture deleted, or none yet): the
            // NV2A's result is undefined, and xemu aborts ("Unhandled texture dimensions")
            if (mBoundTex[s] != mDummyTex) {
                mDev->SetTexture(s, mDummyTex);
                mBoundTex[s] = mDummyTex;
            }
            continue;
        }
        if (mBoundTex[s] != b.tex) {
            mDev->SetTexture(s, b.tex);
            mBoundTex[s] = b.tex;
            mTexSwitches++;
        }
        if (mBoundAddrU[s] != b.addrU) {
            mDev->SetTextureStageState(s, D3DTSS_ADDRESSU, kAddress[b.addrU]);
            mBoundAddrU[s] = b.addrU;
        }
        if (mBoundAddrV[s] != b.addrV) {
            mDev->SetTextureStageState(s, D3DTSS_ADDRESSV, kAddress[b.addrV]);
            mBoundAddrV[s] = b.addrV;
        }
        if (mBoundFilter[s] != b.filter) {
            mDev->SetTextureStageState(s, D3DTSS_MINFILTER, b.filter);
            mDev->SetTextureStageState(s, D3DTSS_MAGFILTER, b.filter);
            mBoundFilter[s] = b.filter;
        }
        float su = b.su, sv = b.sv;
        mStageScale[s][0] = su;
        mStageScale[s][1] = sv;
        if (hw) {
            mBoundScale[s][0] = -1.0f; // c0 and c1 are the batch's now
        } else if (mBoundScale[s][0] != su || mBoundScale[s][1] != sv) {
            float c[4] = { su, sv, 0.0f, 0.0f };
            mDev->SetVertexShaderConstantFast(s, c, 1);
            memcpy(mVsConst[s], c, sizeof(c));
            mVsConstKnown[s] = true;
            mBoundScale[s][0] = su;
            mBoundScale[s][1] = sv;
        }
    }
}

// a stage's texture as a draw now binds it (none: a texture deleted, or none yet)
void GfxRenderingAPID3D8::ResolveTexture(int s, TexBind& b) {
    if (mCurrentTextureIds[s] >= mTextures.size() || mTextures[mCurrentTextureIds[s]].tex == nullptr) {
        b = {};
        return;
    }
    const Texture& t = mTextures[mCurrentTextureIds[s]];
    b.tex = t.tex;
    b.addrU = (uint8_t)(t.linear ? 2 : t.addrU);
    b.addrV = (uint8_t)(t.linear ? 2 : t.addrV);
    b.filter = (uint8_t)((t.filter && mFilter != FILTER_NONE) ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    // the vertex shader's texture-coordinate scale: the padding, or a linear texture's texels
    b.su = t.linear ? (float)t.w : (float)t.w / (float)(t.pw ? t.pw : 1);
    b.sv = t.linear ? (float)t.h : (float)t.h / (float)(t.ph ? t.ph : 1);
}

// a draw into the recording: its vertices copied, and what it binds
void GfxRenderingAPID3D8::RecordDraw(bool hw, const uint8_t* verts, size_t bytes, size_t tris, uint32_t fogColor,
                                     const HwBatch* b) {
    DrawRec r;
    r.prog = mProgram;
    for (int s = 0; s < 2; s++) {
        if (mProgram->usedTextures[s]) {
            ResolveTexture(s, r.tex[s]);
        } else {
            r.tex[s] = {};
        }
    }
    r.zTest = mCurrentDepthTest;
    r.zWrite = mCurrentDepthMask;
    r.decal = mCurrentZmodeDecal;
    r.hw = hw;
    r.tris = (uint32_t)tris;
    r.clipStart = r.clipTris = 0;
    r.fogColor = fogColor;
    if (b != nullptr) {
        r.batch = *b;
        r.batch.numStates = b->numStates > kHwMaxSlots ? kHwMaxSlots : b->numStates;
        memcpy(r.slots, b->slotStates, r.batch.numStates * sizeof(r.slots[0]));
    }
    r.start = (uint32_t)mRecVerts.size();
    mRecVerts.insert(mRecVerts.end(), verts, verts + bytes);
    mRecCmds.push_back({ REC_DRAW, { (int)mRecDraws.size() }, 0.0f });
    mRecDraws.push_back(r);
}

// (the recording is copied to the GPU as each frame draws it: changing it here is for this frame on)
void GfxRenderingAPID3D8::BlendRecordedVerts(const HwBlendVert* verts, size_t count, float t) {
    const int32_t f = (int32_t)(t * 256.0f);
    uint8_t* base = mRecVerts.data();

    for (size_t i = 0; i < count; i++) {
        const HwBlendVert& v = verts[i];
        int16_t* out = (int16_t*)(base + v.offset);
        for (int k = 0; k < 3; k++) {
            out[k] = (int16_t)(v.prev[k] + (((v.cur[k] - v.prev[k]) * f + 128) >> 8));
        }
    }
}

// a recorded draw to the device, with the interpreter's states as they are now
void GfxRenderingAPID3D8::DrawRecorded(DrawRec& r, const HwState* states, const HwLights* lights) {
    mProgram = r.prog;
    mCurrentDepthTest = r.zTest;
    mCurrentDepthMask = r.zWrite;
    mCurrentZmodeDecal = r.decal;
    mReplayDraw = &r;
    const uint8_t* verts = mRecVerts.data() + r.start;
    if (r.hw) {
        HwBatch& b = r.batch;
        b.verts = verts;
        b.states = states;
        b.slotStates = r.slots;
        b.lights = b.lightSet != 0 ? &lights[b.lightSet - 1] : nullptr;
        DrawTrianglesHw(b);
    } else {
        if (!mInScene) {
            mDev->BeginScene();
            mInScene = true;
        }
        if (mProgram->cc.opt_fog && r.fogColor != mBoundFogColor) {
            mDev->SetRenderState(D3DRS_FOGCOLOR, r.fogColor);
            mBoundFogColor = r.fogColor;
        }
        ApplyDrawState();
        mDev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)r.tris, verts, mProgram->stride);
        mDraws++;
        mTris += r.tris;
    }
    mReplayDraw = nullptr;
}

void GfxRenderingAPID3D8::BeginRecording() {
    if (mRecCmds.capacity() == 0) {
        // (vertices stay in main memory, DrawPrimitiveUP copying them for each frame: tried from a vertex
        // buffer written once a tick, a replay took 3 times as long under xemu)
        mRecVerts.reserve(512 * 1024); // Clock Town's frame is ~350 KB
        mRecCmds.reserve(2048);
        mRecDraws.reserve(1024);
    }
    mRecVerts.clear();
    mRecCmds.clear();
    mRecDraws.clear();
    mRecording = true;
    mTickOpen = true;
}

void GfxRenderingAPID3D8::EndRecording() {
    mRecording = false;
}

void GfxRenderingAPID3D8::Replay(const HwState* states, const HwLights* lights, bool last) {
    mRecording = false;
    mSlotCount = 0; // a frame's own matrices
    if (mFrameFence != 0) { // did the GPU finish all that was queued while the CPU made this frame?
        mReplays++;
        mGpuIdle += !mDev->IsFencePending(mFrameFence);
    }
    ReadWantedDepths(); // (the tick's first: nothing of this tick has reached the depth buffer yet)
#if LUSX_PREFETCH_DRAWS
    // The next draw's record and the start of its vertices on their way while this one's state is worked out. The
    // Pentium III has no hardware prefetcher, and a frame's vertices (~350 KB) don't fit its 128 KB L2, so a replay
    // takes them from memory, a miss a 32-byte line, inside D3D's copy. NTA: they're read once a frame, and the L2
    // is better left to the code. (Prime-X's memcpy, for the same misses. xemu has no caches: this only shows on a
    // console, in the "triangles" of the GPU's log line.)
    const auto prefetchDraw = [this](size_t draw) {
        // the record a draw further on: this one's is read here to find its vertices, so it had to be asked for
        // last time, or that read waits for the very miss this is to hide
        if (draw + 1 < mRecDraws.size()) {
            const char* rec = (const char*)&mRecDraws[draw + 1];
            for (size_t i = 0; i < sizeof(DrawRec); i += 32) {
                _mm_prefetch(rec + i, _MM_HINT_NTA);
            }
        }
        if (draw >= mRecDraws.size()) {
            return;
        }
        const DrawRec& r = mRecDraws[draw];
        const char* verts = (const char*)mRecVerts.data() + r.start;
        const size_t bytes = (size_t)r.tris * 3 * (r.hw ? 16 : r.prog->stride);
        const size_t lines = bytes < kPrefetchVertexBytes ? bytes : kPrefetchVertexBytes;
        for (size_t i = 0; i < lines; i += 32) {
            _mm_prefetch(verts + i, _MM_HINT_NTA);
        }
    };
#endif
    for (RecCmd& c : mRecCmds) {
        switch (c.kind) {
            case REC_DRAW:
#if LUSX_PREFETCH_DRAWS
                prefetchDraw((size_t)c.v[0] + 1);
#endif
                DrawRecorded(mRecDraws[c.v[0]], states, lights);
                break;
            case REC_TARGET:
                StartDrawToFramebuffer(c.v[0], c.f);
                break;
            case REC_CLEAR:
                ClearFramebuffer(c.v[0] != 0, c.v[1] != 0);
                break;
            case REC_CLEAR_DEPTH:
                ClearDepthRegion(c.v[0], c.v[1], c.v[2], c.v[3]);
                break;
            case REC_VIEWPORT:
                SetViewport(c.v[0], c.v[1], c.v[2], c.v[3]);
                break;
            case REC_SCISSOR:
                SetScissor(c.v[0], c.v[1], c.v[2], c.v[3]);
                break;
            case REC_COPY:
                // (motion blur's, the whole screen: the game asks for it once a tick, and each frame did it again)
                if (c.f == 0.0f || last) {
                    CopyFramebuffer(c.v[0], c.v[1], c.v[2], c.v[3], c.v[4], c.v[5], c.v[6], c.v[7], c.v[8], c.v[9]);
                }
                break;
            case REC_READ:
                if (c.v[5] == 0) { // once a tick, not once a replay: it waits for the GPU
                    c.v[5] = 1;
                    ReadFramebufferAtReplay(c.v[0], (uint32_t)c.v[6], (uint32_t)c.v[7], (uint32_t)c.v[1],
                                            (uint32_t)c.v[2], (uint16_t*)(uintptr_t)c.v[3], c.v[4] != 0);
                }
                break;
        }
    }
    if (last) {
        mTickOpen = false; // EndFrame fences what was retired meanwhile
    }
}

// a vertex between a and b: the floats (position, coordinates, fog) and the colors' bytes
void GfxRenderingAPID3D8::ClampLerp(uint8_t* dst, const uint8_t* a, const uint8_t* b, float t) {
    const Program& p = *mProgram;
    int floats = 4 + (p.usedTextures[0] ? 2 : 0) + (p.usedTextures[1] ? 2 : 0);
    const float* fa = (const float*)a;
    const float* fb = (const float*)b;
    float* fd = (float*)dst;

    for (int i = 0; i < floats; i++) {
        fd[i] = fa[i] + (fb[i] - fa[i]) * t;
    }
    int c0 = floats * 4, c1 = c0 + p.colors * 4;
    for (int i = c0; i < c1; i++) {
        dst[i] = (uint8_t)(a[i] + ((int)b[i] - (int)a[i]) * t + 0.5f);
    }
    if (p.cc.opt_fog) {
        float fa2 = *(const float*)(a + c1), fb2 = *(const float*)(b + c1);
        *(float*)(dst + c1) = fa2 + (fb2 - fa2) * t;
    }
}

// cut a polygon where steps[0]'s coordinate crosses its bound, pin the piece beyond to the bound, and go
// on with the rest of the steps; the last step's pieces go out as triangle fans
void GfxRenderingAPID3D8::ClampSplit(const ClampPoly& poly, const ClampStep* steps, int numSteps) {
    const size_t stride = mProgram->stride;

    if (poly.n < 3) {
        return;
    }
    if (numSteps == 0) {
        for (int i = 1; i + 1 < poly.n; i++) {
            mClamped.insert(mClamped.end(), poly.v, poly.v + stride);
            mClamped.insert(mClamped.end(), poly.v + i * stride, poly.v + (i + 2) * stride);
        }
        return;
    }
    const ClampStep& s = steps[0];
    float d[kClampMaxVerts];
    bool anyIn = false, anyOut = false;
    for (int i = 0; i < poly.n; i++) {
        float u = ((const float*)(poly.v + i * stride))[s.at];
        d[i] = s.upper ? s.bound - u : u - s.bound;
        anyIn |= d[i] > 0.0f;
        anyOut |= d[i] < 0.0f;
    }
    if (!anyOut) {
        ClampSplit(poly, steps + 1, numSteps - 1);
        return;
    }
    ClampPoly inside, beyond;
    inside.n = beyond.n = 0;
    for (int i = 0; i < poly.n; i++) {
        int j = (i + 1) % poly.n;
        const uint8_t* vi = poly.v + i * stride;
        if (d[i] >= 0.0f && inside.n < kClampMaxVerts) {
            memcpy(inside.v + inside.n++ * stride, vi, stride);
        }
        if (d[i] <= 0.0f && beyond.n < kClampMaxVerts) {
            memcpy(beyond.v + beyond.n++ * stride, vi, stride);
        }
        if ((d[i] > 0.0f && d[j] < 0.0f) || (d[i] < 0.0f && d[j] > 0.0f)) {
            uint8_t x[kClampMaxStride];
            ClampLerp(x, vi, poly.v + j * stride, d[i] / (d[i] - d[j]));
            if (inside.n < kClampMaxVerts) {
                memcpy(inside.v + inside.n++ * stride, x, stride);
            }
            if (beyond.n < kClampMaxVerts) {
                memcpy(beyond.v + beyond.n++ * stride, x, stride);
            }
        }
    }
    for (int i = 0; i < beyond.n; i++) {
        ((float*)(beyond.v + i * stride))[s.at] = s.bound;
    }
    if (anyIn) {
        ClampSplit(inside, steps + 1, numSteps - 1);
    }
    ClampSplit(beyond, steps + 1, numSteps - 1);
}

void GfxRenderingAPID3D8::DrawTriangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    if (mProgram == nullptr || mProgram->vs == 0 || buf_vbo_num_tris == 0) {
        return;
    }
    const Program& p = *mProgram;
    const CCFeatures& cc = p.cc;
    size_t verts = buf_vbo_num_tris * 3;
    mVertices.resize(verts * p.stride);
    uint8_t* out = mVertices.data();
    const float* in = buf_vbo;
    ClampStep steps[8];
    int numSteps = 0;
    uint32_t fogColor = mBoundFogColor;

    for (size_t v = 0; v < verts; v++, in += p.numFloats) {
        float* o = (float*)out;
        int k = 4;
        o[0] = in[0];
        o[1] = in[1];
        o[2] = in[2];
        o[3] = in[3];
        o += 4;
        for (int t = 0; t < 2; t++) {
            if (!cc.usedTextures[t]) {
                continue;
            }
            o[0] = in[k];
            o[1] = in[k + 1];
            // the interpreter's clamp to the tile (a clamped tile that also mirrors or wraps): its upper
            // bound follows the coordinates; the same for all of a draw's vertices
            if (v == 0) {
                const Texture& tx = mTextures[mCurrentTextureIds[t] < mTextures.size() ? mCurrentTextureIds[t] : 0];
                int c = k + 2;
                for (int a = 0; a < 2; a++) {
                    if (cc.clamp[t][a]) {
                        int size = a == 0 ? tx.w : tx.h;
                        int at = (int)(o - (float*)out) + a;
                        steps[numSteps++] = { at, in[c], true };
                        steps[numSteps++] = { at, size > 0 ? 0.5f / size : 0.0f, false };
                        c++;
                    }
                }
            }
            o += 2;
            k += 2 + (cc.clamp[t][0] ? 1 : 0) + (cc.clamp[t][1] ? 1 : 0);
        }
        float fog = 1.0f;
        if (cc.opt_fog) {
            if (v == 0) {
                fogColor = 0xFF000000u | (ToByte(in[k]) << 16) | (ToByte(in[k + 1]) << 8) | ToByte(in[k + 2]);
            }
            fog = 1.0f - in[k + 3];
            k += 4;
        }
        if (cc.opt_grayscale) {
            k += 4;
        }
        DWORD* col = (DWORD*)o;
        for (int j = 0; j < p.numInputs; j++) {
            if (j < 4) {
                // with no alpha, the inputs are 3 floats apart: the 4th read is the next one's, replaced
                col[j] = cc.opt_alpha ? PackColor(in + k) : (PackColor(in + k) | 0xFF000000u);
            }
            k += cc.opt_alpha ? 4 : 3;
        }
        o = (float*)(col + p.colors);
        if (cc.opt_fog) {
            *o++ = fog;
        }
        out += p.stride;
    }
    // depth clamp for the triangles wholly beyond the near (or far) plane: z pinned to it, as
    // D3DDCC_CLAMP would (the HUD's quads, z 0 under an ortho projection with its near plane at 10:
    // z -0.2 here); not every NV2A emulator honours that render state
    for (size_t i = 0; i < buf_vbo_num_tris; i++) {
        float* v0 = (float*)(mVertices.data() + (i * 3) * p.stride);
        float* v1 = (float*)((uint8_t*)v0 + p.stride);
        float* v2 = (float*)((uint8_t*)v1 + p.stride);
        if (v0[2] < 0.0f && v1[2] < 0.0f && v2[2] < 0.0f) {
            v0[2] = v1[2] = v2[2] = 0.0f;
        } else if (v0[2] > v0[3] && v1[2] > v1[3] && v2[2] > v2[3]) {
            v0[2] = v0[3];
            v1[2] = v1[3];
            v2[2] = v2[3];
        }
    }
    const uint8_t* draw = mVertices.data();
    if (numSteps > 0) {
        // ps.1.x can't clamp a coordinate: the triangles are cut where it crosses the tile's bounds,
        // and the pieces beyond have it pinned there (exact: the attributes are linear in clip space)
        mClamped.clear();
        for (size_t i = 0; i < buf_vbo_num_tris; i++) {
            ClampPoly poly;
            poly.n = 3;
            memcpy(poly.v, mVertices.data() + i * 3 * p.stride, 3 * p.stride);
            ClampSplit(poly, steps, numSteps);
        }
        buf_vbo_num_tris = mClamped.size() / (3 * p.stride);
        draw = mClamped.data();
        if (buf_vbo_num_tris == 0) {
            return;
        }
    }
    if (mRecording) {
        RecordDraw(false, draw, buf_vbo_num_tris * 3 * p.stride, buf_vbo_num_tris, fogColor, nullptr);
        return;
    }
    if (!mInScene) {
        mDev->BeginScene();
        mInScene = true;
    }
    if (cc.opt_fog && fogColor != mBoundFogColor) {
        mDev->SetRenderState(D3DRS_FOGCOLOR, fogColor);
        mBoundFogColor = fogColor;
    }
    ApplyDrawState();
    mDev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)buf_vbo_num_tris, draw, p.stride);
    mDraws++;
    mTris += (uint32_t)buf_vbo_num_tris;
}

void GfxRenderingAPID3D8::Init() {
    mDev = mWindow->GetDevice();
    uint32_t w, h;
    int32_t x, y;
    mWindow->GetDimensions(&w, &h, &x, &y);
    mTargetW = w;
    mTargetH = h;
    mFramebuffers.resize(1); // 0: the screen
    mFramebuffers[0].w = mTargetW;
    mFramebuffers[0].h = mTargetH;
    mFramebuffers[0].hasDepth = true;
    if (mDev == nullptr) {
        return;
    }
    // kept: what's bound later may be a framebuffer's (TargetDepth)
    mDev->GetDepthStencilSurface(&mScreenDepth);
    mDev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    mDev->SetRenderState(D3DRS_LIGHTING, FALSE);
    mDev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    mDev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    mDev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    mDev->SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
    mDev->SetRenderState(D3DRS_SPECULARENABLE, TRUE);
    mDev->SetRenderState(D3DRS_DITHERENABLE, TRUE);
    // depth clamped, not clipped, as the other backends have it (GL_DEPTH_CLAMP, DepthClipEnable off):
    // the HUD's quads (the clock's sun and moon, the beating heart) are drawn at z 0 with an ortho
    // projection whose near plane is 10, just outside the depth range, and were culled (DrawTriangles
    // also pins those triangles' z, for where this state isn't honoured)
    mDev->SetRenderState(D3DRS_DEPTHCLIPCONTROL, D3DDCC_CLAMP);
    // white, for a sampled stage with no texture (ApplyDrawState)
    if (SUCCEEDED(mDev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, 0, &mDummyTex))) {
        D3DLOCKED_RECT lr;
        mDummyTex->LockRect(0, &lr, NULL, 0);
        *(uint32_t*)lr.pBits = 0xFFFFFFFFu;
        mDummyTex->UnlockRect(0);
    }
    mWindow->SetStatsCallback([](void* self) { static_cast<GfxRenderingAPID3D8*>(self)->LogStats(); }, this);
    SPDLOG_INFO("GfxRenderingAPID3D8: up");
}

void GfxRenderingAPID3D8::OnResize() {
}

void GfxRenderingAPID3D8::StartFrame() {
    if (mRecording) {
        return; // the replays' frames start (Replay)
    }
    ReleaseRetired(false);
}

void GfxRenderingAPID3D8::EndFrame() {
    if (mInScene) {
        mDev->EndScene();
        mInScene = false;
    }
    mFrameFence = mDev->InsertFence(); // the frame's drawing done when it's passed (GetPixelDepth)
    if (!mTickOpen) {
        for (Retired& r : mRetired) {
            if (r.fence == 0) {
                r.fence = mFrameFence; // retired while a recording could still draw with it
            }
        }
    }
}

void GfxRenderingAPID3D8::FinishRender() {
}

int GfxRenderingAPID3D8::CreateFramebuffer() {
    if (mFramebuffers.empty()) {
        mFramebuffers.resize(1);
    }
    mFramebuffers.emplace_back();
    return (int)mFramebuffers.size() - 1;
}

void GfxRenderingAPID3D8::UpdateFramebufferParameters(int fb_id, uint32_t width, uint32_t height,
                                                      uint32_t msaa_level, bool opengl_invertY, bool render_target,
                                                      bool has_depth_buffer, bool can_extract_depth) {
    if (fb_id < 0 || fb_id >= (int)mFramebuffers.size()) {
        return;
    }
    Framebuffer& fb = mFramebuffers[fb_id];
    if (fb_id == 0) {
        return; // the screen: its size is the window's
    }
    if (fb.w == width && fb.h == height && fb.hasDepth == has_depth_buffer) {
        return;
    }
    // made again when next drawn into (most a port declares are never drawn into here)
    if (fb.tex != nullptr) {
        if (fb.color) {
            fb.color->Release();
        }
        Retire(fb.tex);
        if (fb.depth) {
            mDev->BlockUntilIdle();
            fb.depth->Release();
        }
        fb.tex = nullptr;
        fb.color = fb.depth = nullptr; // (a shared screen depth: only this target's reference went)
        fb.sharesScreenDepth = false;
        mTextures[fb.texId].tex = nullptr;
    }
    fb.w = width;
    fb.h = height;
    fb.hasDepth = has_depth_buffer;
}

void GfxRenderingAPID3D8::EnsureFramebuffer(int fbId) {
    Framebuffer& fb = mFramebuffers[fbId];
    if (fbId == 0 || fb.tex != nullptr || fb.w == 0 || fb.h == 0) {
        return;
    }
    // the screen's format: CopyRects copies between surfaces of one format
    D3DFORMAT fmt = mWindow->Is16Bit() ? D3DFMT_LIN_R5G6B5 : D3DFMT_LIN_X8R8G8B8;
    if (FAILED(mDev->CreateTexture(fb.w, fb.h, 1, 0, fmt, 0, &fb.tex))) {
        SPDLOG_ERROR("GfxRenderingAPID3D8: no {}x{} framebuffer", fb.w, fb.h);
        fb.tex = nullptr;
        return;
    }
    fb.tex->GetSurfaceLevel(0, &fb.color);
    // no depth buffer yet: most framebuffers are only copied into (2Ship's pause and blur effects);
    // one is made when the game first draws into the framebuffer (StartDrawToFramebuffer)
    if (fb.texId == 0) {
        fb.texId = NewTexture();
    }
    Texture& t = mTextures[fb.texId];
    t.tex = fb.tex;
    t.w = t.pw = (uint16_t)fb.w;
    t.h = t.ph = (uint16_t)fb.h;
    t.linear = true;
    t.filter = true;
    SPDLOG_INFO("GfxRenderingAPID3D8: framebuffer {}: {}x{}", fbId, fb.w, fb.h);
}

D3DSurface* GfxRenderingAPID3D8::TargetColor(int fbId) {
    D3DSurface* s = nullptr;
    if (fbId == 0) {
        mDev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &s);
        return s; // a reference: the caller releases it
    }
    EnsureFramebuffer(fbId);
    s = mFramebuffers[fbId].color;
    if (s) {
        s->AddRef();
    }
    return s;
}

D3DSurface* GfxRenderingAPID3D8::TargetDepth(int fbId) {
    D3DSurface* s = nullptr;
    if (fbId == 0) {
        if (mScreenDepth != nullptr) {
            mScreenDepth->AddRef();
            return mScreenDepth;
        }
        mDev->GetDepthStencilSurface(&s);
        return s;
    }
    s = mFramebuffers[fbId].depth;
    if (s) {
        s->AddRef();
    }
    return s;
}

void GfxRenderingAPID3D8::StartDrawToFramebuffer(int fbId, float noiseScale) {
    if (fbId < 0 || fbId >= (int)mFramebuffers.size()) {
        return;
    }
    D3DSurface* color = TargetColor(fbId);
    if (color == nullptr) {
        return;
    }
    Framebuffer& fb = mFramebuffers[fbId];
    if (fbId != 0 && fb.hasDepth && fb.depth == nullptr && mScreenDepth != nullptr && fb.w == mFramebuffers[0].w &&
        fb.h == mFramebuffers[0].h) {
        // drawn into, and the screen's size: the screen's depth buffer, not 1.8 MB more at 720p (SoH's pause menu
        // Link is one, drawn at the start of the frame's work list, before the game's own frame clears depth). Its
        // depth is cleared again on the way back to the screen (below).
        fb.depth = mScreenDepth;
        fb.depth->AddRef();
        fb.sharesScreenDepth = true;
        SPDLOG_INFO("GfxRenderingAPID3D8: framebuffer {}: the screen's depth buffer", fbId);
    } else if (fbId != 0 && fb.hasDepth && fb.depth == nullptr) {
        // drawn into: now it needs its depth buffer
        D3DFORMAT zfmt = mWindow->Is16Bit() ? D3DFMT_LIN_D16 : D3DFMT_LIN_D24S8; // the color's size
        fb.sharesScreenDepth = false;
        if (FAILED(mDev->CreateDepthStencilSurface(fb.w, fb.h, zfmt, D3DMULTISAMPLE_NONE, &fb.depth))) {
            fb.depth = nullptr;
            fb.hasDepth = false;
        } else {
            SPDLOG_INFO("GfxRenderingAPID3D8: framebuffer {}: depth buffer", fbId);
        }
    }
    D3DSurface* depth = TargetDepth(fbId);
    if (mRecording) {
        // kept for the replay; the target's size is what the recorded viewports and clears take
        mRecCmds.push_back({ REC_TARGET, { fbId }, noiseScale });
        color->Release();
        if (depth) {
            depth->Release();
        }
        mCurrentFb = fbId;
        mTargetW = mFramebuffers[fbId].w;
        mTargetH = mFramebuffers[fbId].h;
        return;
    }
    if (!mInScene) {
        mDev->BeginScene();
        mInScene = true;
    }
    mDev->SetRenderTarget(color, depth);
    color->Release();
    if (depth) {
        depth->Release();
    }
    const int left = mCurrentFb;
    mCurrentFb = fbId;
    mTargetW = mFramebuffers[fbId].w; // 0's is the screen's (Init)
    mTargetH = mFramebuffers[fbId].h;
    // back from a target that drew into the screen's depth: none of its depth is left for the screen's draws
    if (fbId == 0 && left > 0 && left < (int)mFramebuffers.size() && mFramebuffers[left].sharesScreenDepth) {
        ClearFramebuffer(false, true);
    }
}

void GfxRenderingAPID3D8::SetFullViewport() {
    D3DVIEWPORT8 vp = { 0, 0, mTargetW, mTargetH, 0.0f, 1.0f };
    D3DRECT r = { 0, 0, (LONG)mTargetW, (LONG)mTargetH };
    mDev->SetViewport(&vp);
    mDev->SetScissors(1, FALSE, &r);
}

void GfxRenderingAPID3D8::ClearFramebuffer(bool color, bool depth) {
    if (mRecording) {
        mRecCmds.push_back({ REC_CLEAR, { color ? 1 : 0, depth ? 1 : 0 }, 0.0f });
        return;
    }
    DWORD flags = (color ? D3DCLEAR_TARGET : 0);
    if (depth && (mCurrentFb == 0 || mFramebuffers[mCurrentFb].depth != nullptr)) {
        flags |= D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL;
    }
    if (flags == 0) {
        return;
    }
    D3DVIEWPORT8 old;
    mDev->GetViewport(&old);
    SetFullViewport();
    mDev->Clear(0, NULL, flags, 0xFF000000, 1.0f, 0);
    mDev->SetViewport(&old);
}

void GfxRenderingAPID3D8::ClearDepthRegion(int x, int y, int w, int h) {
    if (mRecording) {
        mRecCmds.push_back({ REC_CLEAR_DEPTH, { x, y, w, h }, 0.0f });
        return;
    }
    if (mCurrentFb != 0 && mFramebuffers[mCurrentFb].depth == nullptr) {
        return;
    }
    D3DRECT r = { x, (LONG)mTargetH - y - h, x + w, (LONG)mTargetH - y };
    mDev->Clear(1, &r, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
}

// a GPU copy of the source's rectangle; the sizes are taken as equal (no stretching on the NV2A's
// blitter): the smaller of the two is copied
void GfxRenderingAPID3D8::CopyFramebuffer(int fbDstId, int fbSrcId, int srcX0, int srcY0, int srcX1, int srcY1,
                                          int dstX0, int dstY0, int dstX1, int dstY1) {
    if (fbDstId < 0 || fbSrcId < 0 || fbDstId >= (int)mFramebuffers.size() ||
        fbSrcId >= (int)mFramebuffers.size() || fbDstId == fbSrcId) {
        return;
    }
    if (mRecording) {
        mRecCmds.push_back({ REC_COPY,
                             { fbDstId, fbSrcId, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1 },
                             mCopyOnceATick ? 1.0f : 0.0f });
        return;
    }
    D3DSurface* src = TargetColor(fbSrcId);
    D3DSurface* dst = TargetColor(fbDstId);
    if (src && dst) {
        uint32_t sh = mFramebuffers[fbSrcId].h;
        uint32_t dh = mFramebuffers[fbDstId].h;
        int w = (std::min)(std::abs(srcX1 - srcX0), std::abs(dstX1 - dstX0));
        int h = (std::min)(std::abs(srcY1 - srcY0), std::abs(dstY1 - dstY0));
        RECT r = { (std::min)(srcX0, srcX1), (LONG)sh - (std::max)(srcY0, srcY1), 0, 0 };
        r.right = r.left + w;
        r.bottom = r.top + h;
        POINT pt = { (std::min)(dstX0, dstX1), (LONG)dh - (std::max)(dstY0, dstY1) };
        if (r.top < 0) {
            r.top = 0;
        }
        if (pt.y < 0) {
            pt.y = 0;
        }
        mDev->CopyRects(src, &r, 1, dst, &pt);
    }
    if (src) {
        src->Release();
    }
    if (dst) {
        dst->Release();
    }
}

void GfxRenderingAPID3D8::ReadFramebufferAtReplay(int fbId, uint32_t ulx, uint32_t uly, uint32_t width, uint32_t height,
                                                  uint16_t* rgba16Buf, bool byteSwap) {
    const uint32_t fullW = 320, fullH = 240; // the N64's frame: what the game's buffers hold

    if (mRecording) {
        // nothing of the tick is drawn yet: done in its place, on the tick's first replay. What reads the buffer has
        // it a tick later: the Deku bubble's picture is a tick old, the Pictograph's photo is read in a later state.
        mRecCmds.push_back({ REC_READ,
                             { fbId, (int)width, (int)height, (int)(uintptr_t)rgba16Buf, byteSwap ? 1 : 0, 0, (int)ulx,
                               (int)uly },
                             0.0f });
        return;
    }
    LusXbox_Where("reading a framebuffer back");
    ReadFramebufferRect(fbId, fullW, fullH, ulx, uly, width, height, rgba16Buf);
    if (byteSwap) {
        for (uint32_t j = uly; j < uly + height; j++) {
            uint16_t* row = rgba16Buf + j * fullW;
            for (uint32_t i = ulx; i < ulx + width; i++) {
                row[i] = (uint16_t)((row[i] << 8) | (row[i] >> 8));
            }
        }
    }
}

void GfxRenderingAPID3D8::ReadFramebufferToCPU(int fbId, uint32_t width, uint32_t height, uint16_t* rgba16Buf) {
    ReadFramebufferRect(fbId, width, height, 0, 0, width, height, rgba16Buf);
}

// A rectangle of the frame scaled to fullW by fullH, left where it lies in a buffer of that size. A CPU read of the
// frame is slow, pixel by pixel, so only what's wanted is read.
// (the middle 4:3 of a wider framebuffer: what 2Ship's own scaling to the N64's size keeps)
void GfxRenderingAPID3D8::ReadFramebufferRect(int fbId, uint32_t fullW, uint32_t fullH, uint32_t ulx, uint32_t uly,
                                              uint32_t width, uint32_t height, uint16_t* rgba16Buf) {
    D3DSurface* s = TargetColor(fbId);
    if (s == nullptr) {
        return;
    }
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT lr;
    s->GetDesc(&desc);
    mDev->BlockUntilIdle();
    bool is16 = desc.Format == D3DFMT_R5G6B5 || desc.Format == D3DFMT_LIN_R5G6B5;
    const uint32_t srcW = desc.Width * 3 > desc.Height * 4 ? desc.Height * 4 / 3 : desc.Width;
    const uint32_t srcX = (desc.Width - srcW) / 2;
    if (SUCCEEDED(s->LockRect(&lr, NULL, D3DLOCK_READONLY))) {
        for (uint32_t j = uly; j < uly + height; j++) {
            const uint8_t* row = (const uint8_t*)lr.pBits + (j * desc.Height / fullH) * lr.Pitch;
            for (uint32_t i = ulx; i < ulx + width; i++) {
                uint32_t r, g, b;
                if (is16) { // R5G6B5 to the N64's RGBA5551
                    uint32_t px = ((const uint16_t*)row)[srcX + i * srcW / fullW];
                    r = px >> 11;
                    g = (px >> 6) & 0x1F;
                    b = px & 0x1F;
                } else {
                    uint32_t px = ((const uint32_t*)row)[srcX + i * srcW / fullW];
                    r = (((px >> 16) & 0xFF) + 4) * 0x1F / 0xFF;
                    g = (((px >> 8) & 0xFF) + 4) * 0x1F / 0xFF;
                    b = ((px & 0xFF) + 4) * 0x1F / 0xFF;
                }
                rgba16Buf[i + j * fullW] = (uint16_t)((r << 11) | (g << 6) | (b << 1) | 1);
            }
        }
        s->UnlockRect();
    }
    s->Release();
}

void GfxRenderingAPID3D8::ResolveMSAAColorBuffer(int fbIdTarger, int fbIdSrc) {
}

std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
GfxRenderingAPID3D8::GetPixelDepth(int fb_id, const std::set<std::pair<float, float>>& coordinates) {
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff> res;

    // The game asks once a tick, before its drawing (the sun's and lens flares' visibility), for the depth
    // the last frame left. That frame may be behind others queued for the display, and waiting for it here
    // stalled the game until they had flipped. The depth buffer stays whole until the tick's first replay
    // though, and the GPU has caught up by then: the read is made there (ReadWantedDepths), and the game
    // gets a tick-old answer now, from the nearest point read (these points move little between ticks)
    for (const auto& c : coordinates) {
        uint16_t depth = 0;
        float best = 1e30f;
        for (const auto& known : mLastDepths) {
            const float dx = known.first.first - c.first, dy = known.first.second - c.second;
            if (dx * dx + dy * dy < best) {
                best = dx * dx + dy * dy;
                depth = known.second;
            }
        }
        res.emplace(c, depth);
    }
    mDepthWanted.insert(coordinates.begin(), coordinates.end());
    mDepthWantedFb = fb_id;
    return res;
}

// what GetPixelDepth was asked this tick, from the depth buffer as the last frame left it
void GfxRenderingAPID3D8::ReadWantedDepths() {
    if (mDepthWanted.empty()) {
        return;
    }
    const int fb = mDepthWantedFb;
    D3DSurface* s = (fb >= 0 && fb < (int)mFramebuffers.size()) ? TargetDepth(fb) : nullptr;
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT lr;

    if (s != nullptr) {
        // Waiting for the last frame here empties the GPU's queue, every tick that a torch's glow or the sun asks: a
        // tick then missed a blank and ran late, though its costs fit. Still drawing, the old answers stand (a few
        // ticks late hardly shows); only every 8th tick waits, so they can't go stale.
        static uint32_t sSkipped;
        if (mFrameFence != 0 && mDev->IsFencePending(mFrameFence)) {
            if (++sSkipped < 8) {
                s->Release();
                mDepthWanted.clear();
                return;
            }
            const double start = LusXbox_Seconds();
            mDev->BlockOnFence(mFrameFence);
            LusXbox_AddWait(LusXbox_Seconds() - start);
        }
        sSkipped = 0;
        s->GetDesc(&desc);
    }
    // the screen's depth buffer is in tiled memory: a lock without D3DLOCK_TILED reads it "jumbled" (the
    // XDK's word); a framebuffer's own (CreateDepthStencilSurface) is linear
    const DWORD lockFlags = D3DLOCK_READONLY | D3DLOCK_NOOVERWRITE | (fb == 0 ? D3DLOCK_TILED : 0);
    static uint32_t sReads;
    mLastDepths.clear();
    if (s != nullptr && SUCCEEDED(s->LockRect(&lr, NULL, lockFlags))) {
        bool is16 = desc.Format == D3DFMT_D16 || desc.Format == D3DFMT_LIN_D16;
        for (const auto& c : mDepthWanted) {
            int x = (int)c.first, y = (int)desc.Height - 1 - (int)c.second;
            double z = 1.0; // 0..1
            if (x >= 0 && y >= 0 && x < (int)desc.Width && y < (int)desc.Height) {
                const uint8_t* row = (const uint8_t*)lr.pBits + y * lr.Pitch;
                z = is16 ? ((const uint16_t*)row)[x] / 65535.0 : (((const uint32_t*)row)[x] >> 8) / 16777215.0;
            }
            mLastDepths.emplace(c, (uint16_t)(z * 65532.0));
            if (sReads % 200 == 0) {
                SPDLOG_INFO("lus-x: depth read at ({}, {}) of {}x{} (format {:#x}, pitch {}): {}", x, y, desc.Width,
                            desc.Height, (unsigned)desc.Format, lr.Pitch, (uint16_t)(z * 65532.0));
            }
        }
        sReads++;
        s->UnlockRect();
    } else if (sReads++ % 200 == 0) {
        SPDLOG_INFO("lus-x: depth read: no surface or the lock failed (framebuffer {})", fb);
    }
    if (s != nullptr) {
        s->Release();
    }
    mDepthWanted.clear();
}

void* GfxRenderingAPID3D8::GetFramebufferTextureId(int fbId) {
    if (fbId <= 0 || fbId >= (int)mFramebuffers.size()) {
        return nullptr;
    }
    EnsureFramebuffer(fbId);
    return mFramebuffers[fbId].tex;
}

void GfxRenderingAPID3D8::SelectTextureFb(int fbId) {
    if (fbId <= 0 || fbId >= (int)mFramebuffers.size()) {
        return;
    }
    EnsureFramebuffer(fbId);
    SelectTexture(0, mFramebuffers[fbId].texId);
}

void GfxRenderingAPID3D8::DeleteTexture(uint32_t texId) {
    if (texId < mTextures.size()) {
        Retire(mTextures[texId].tex);
        mTextures[texId].tex = nullptr;
    }
}

void GfxRenderingAPID3D8::SetTextureFilter(FilteringMode mode) {
    mFilter = mode;
    for (int s = 0; s < 2; s++) {
        mBoundFilter[s] = -1;
    }
}

FilteringMode GfxRenderingAPID3D8::GetTextureFilter() {
    return mFilter;
}

void GfxRenderingAPID3D8::SetSrgbMode() {
}

ImTextureID GfxRenderingAPID3D8::GetTextureById(int id) {
    return (id >= 0 && id < (int)mTextures.size()) ? (ImTextureID)mTextures[id].tex : nullptr;
}

void GfxRenderingAPID3D8::SetCurrentPrimDepth(float depth) {
    mCurrentPrimDepth = depth;
}

void GfxRenderingAPID3D8::LogStats() {
    uint32_t live = 0, liveBytes = 0, live32 = 0;
    for (const auto& t : mTextures) {
        live += t.tex != nullptr;
        if (t.tex != nullptr && !t.linear) {
            liveBytes += t.bytes;
            live32 += t.bytes == (uint32_t)t.pw * t.ph * 4 ? t.bytes : 0;
        }
    }
    uint64_t hwDraws = mHwDraws - mHwDrawsLogged;
    SPDLOG_INFO("GfxRenderingAPID3D8: {} draws ({} the GPU transform's, {:.1f} constants sent each), {} triangles, {} "
                "uploads ({} KB), {} texture switches; {} programs, {} textures live ({} KB, {} KB of it 32-bit), {} "
                "retiring",
                mDraws, hwDraws, hwDraws ? (double)mConstsSent / hwDraws : 0.0, mTris, mUploads, mUploadBytes >> 10,
                mTexSwitches, mPrograms.size(), live, liveBytes >> 10, live32 >> 10, mRetired.size());
    const double perFrame = mReplays ? gLusxCyclesPerMs * mReplays : gLusxCyclesPerMs;
    SPDLOG_INFO("GfxRenderingAPID3D8: the GPU: idle at the start of {} of {} frames; {} waits for the push buffer, "
                "{:.2f} ms a frame; a frame's draws: state {:.2f} ms, constants {:.2f} ms, triangles {:.2f} ms",
                mGpuIdle, mReplays, mPushWaits, mPushWaitCycles / perFrame, mStateCycles / perFrame,
                mConstCycles / perFrame, mDrawCycles / perFrame);
    mGpuIdle = mReplays = mPushWaits = 0;
    mPushWaitCycles = mStateCycles = mConstCycles = mDrawCycles = 0;
    mDraws = mTris = mUploads = mUploadBytes = mTexSwitches = mConstsSent = 0;
    mHwDrawsLogged = mHwDraws;
}

} // namespace Fast

#endif
