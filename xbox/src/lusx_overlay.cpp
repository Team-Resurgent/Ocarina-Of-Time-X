// lusx_overlay.cpp -- HD drawing for the port's menu (lusx_overlay.h).
//
// The text is Fredoka (xbox/assets, SIL Open Font License), read from the disc and rasterised with stb_truetype each
// time the menu opens: each size at the number of display pixels it will cover, into an atlas, so a glyph is drawn
// texel for pixel. All of it is let go when the menu closes: the game's memory is the game's. Shapes and text go
// through the renderer's own calls, as a draw of the game's would, so its state cache stays true.
#include "lusx_overlay.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <spdlog/spdlog.h>

#include "ship/Context.h"
#include "ship/window/Window.h"
#include "fast/backends/gfx_rendering_api.h"
#include "fast/interpreter.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#define STBTT_assert(x) ((void)0)
#include "third_party/stb_truetype.h"

// after the C++ headers: the Xbox's xtl.h defines min and max (and ignores NOMINMAX)
#include <xtl.h>

extern "C" int LusXbox_Widescreen(void); // gfx_xbox.cpp

#define FONT_PATH "D:\\menu.ttf"
#define FIRST_CHAR 32
#define CHAR_COUNT 95 // printable ASCII
#define MAX_TEXT 96   // characters drawn at once
#define ROUND_STEPS 6 // segments in a rounded corner
#define CIRCLE_STEPS 24
#define VERT_FLOATS 10 // x y z w, u v, r g b a
#define TX_CLAMP 2     // G_TX_CLAMP

// the sizes, in layout pixels (the line's height), and each one's atlas
static const float kFontSize[OVL_FONT_COUNT] = { 34.0f, 54.0f, 26.0f };
static const int kAtlasW[OVL_FONT_COUNT] = { 512, 512, 256 };
static const int kAtlasH[OVL_FONT_COUNT] = { 256, 512, 256 };
// drawn twice, a display pixel apart: the regular weight, made bolder for reading across a room
static const int kFontBold[OVL_FONT_COUNT] = { 1, 1, 0 };

struct OvlFont {
    uint32_t texture; // its id stays for the next opening; the texture itself doesn't
    stbtt_packedchar chars[CHAR_COUNT];
    float ascent, descent; // display pixels
    float scale;           // the size it was made at, in display pixels a layout pixel
};

static OvlFont sFonts[OVL_FONT_COUNT];
static stbtt_fontinfo sInfo;
static unsigned char* sTtf;
static Fast::GfxRenderingAPI* sRapi;
static float sW, sH;           // the display
static float sScaleX, sScaleY; // display pixels a layout pixel
static float sLeft;            // where the layout's left edge is

// a combiner's slot in a shader id: (a - b) * c + d, k being a, b, c, d
static constexpr uint64_t Slot(int alpha, int k, int source) {
    return (uint64_t)source << (alpha * 16 + k * 4);
}
static constexpr uint64_t kFlatShader = Slot(0, 3, SHADER_INPUT_1) | Slot(1, 3, SHADER_INPUT_1);
static constexpr uint64_t kTextShader =
    Slot(0, 3, SHADER_INPUT_1) | Slot(1, 0, SHADER_TEXEL0) | Slot(1, 2, SHADER_INPUT_1);

static void UseShader(uint64_t id) {
    const uint64_t options = SHADER_OPT(ALPHA);
    auto* program = sRapi->LookupShader(id, options);

    if (program == nullptr) {
        program = sRapi->CreateAndLoadNewShader(id, options);
    }
    sRapi->LoadShader(program);
}

static void DisplayMap(void) {
    auto wnd = Ship::Context::GetRawInstance()->GetWindow();

    sW = (float)wnd->GetWidth();
    sH = (float)wnd->GetHeight();
    sScaleY = sH / OVL_H;
    // 16:9 sent in 480 lines is narrower pixels than tall ones
    sScaleX = LusXbox_Widescreen() ? sW / 1280.0f : sW / OVL_W;
    sLeft = (sW - OVL_W * sScaleX) * 0.5f;
}

static float DispX(float x) {
    return sLeft + x * sScaleX;
}

static float DispY(float y) {
    return y * sScaleY;
}

// a vertex at a display position
static float* Vert(float* v, float dx, float dy, uint32_t colour, bool textured, float s = 0.0f, float t = 0.0f) {
    *v++ = dx / sW * 2.0f - 1.0f;
    *v++ = 1.0f - dy / sH * 2.0f;
    *v++ = 0.0f;
    *v++ = 1.0f;
    if (textured) {
        *v++ = s;
        *v++ = t;
    }
    *v++ = (float)((colour >> 16) & 0xFF) / 255.0f;
    *v++ = (float)((colour >> 8) & 0xFF) / 255.0f;
    *v++ = (float)(colour & 0xFF) / 255.0f;
    *v++ = (float)(colour >> 24) / 255.0f;
    return v;
}

static uint32_t Mix(uint32_t a, uint32_t b, float t) {
    uint32_t out = 0;

    for (int shift = 0; shift < 32; shift += 8) {
        const float ca = (float)((a >> shift) & 0xFF), cb = (float)((b >> shift) & 0xFF);
        out |= ((uint32_t)(ca + (cb - ca) * t + 0.5f) & 0xFF) << shift;
    }
    return out;
}

static void DrawFlat(float* verts, size_t floats) {
    UseShader(kFlatShader);
    sRapi->DrawTriangles(verts, floats, floats / (8 * 3));
}

void Ovl_Begin(void) {
    DisplayMap();
    sRapi->StartDrawToFramebuffer(0, 1.0f);
    sRapi->SetViewport(0, 0, (int)sW, (int)sH);
    sRapi->SetScissor(0, 0, (int)sW, (int)sH);
    sRapi->SetDepthTestAndMask(false, false);
    sRapi->SetZmodeDecal(false);
    sRapi->SetUseAlpha(true);
}

void Ovl_Clip(float x0, float x1) {
    const int l = (int)DispX(x0), r = (int)DispX(x1);

    sRapi->SetScissor(l, 0, r - l, (int)sH);
}

void Ovl_ClipOff(void) {
    sRapi->SetScissor(0, 0, (int)sW, (int)sH);
}

void Ovl_Rect(float x0, float y0, float x1, float y1, uint32_t top, uint32_t bottom) {
    float q[6 * 8], *v = q;
    // across the whole layout: across the whole display, a 16:9 one too
    const bool whole = x0 <= 0.0f && x1 >= OVL_W;
    const float l = whole ? 0.0f : floorf(DispX(x0) + 0.5f), r = whole ? sW : floorf(DispX(x1) + 0.5f);
    const float t = floorf(DispY(y0) + 0.5f), b = floorf(DispY(y1) + 0.5f);

    v = Vert(v, l, t, top, false);
    v = Vert(v, r, t, top, false);
    v = Vert(v, r, b, bottom, false);
    v = Vert(v, l, t, top, false);
    v = Vert(v, r, b, bottom, false);
    v = Vert(v, l, b, bottom, false);
    DrawFlat(q, v - q);
}

void Ovl_RoundRect(float x0, float y0, float x1, float y1, float radius, uint32_t top, uint32_t bottom) {
    // the outline, corner by corner, then a fan from the middle
    static const float kCorner[4][2] = { { 1, -1 }, { 1, 1 }, { -1, 1 }, { -1, -1 } };
    float px[4 * (ROUND_STEPS + 1)], py[4 * (ROUND_STEPS + 1)];
    float tris[3 * 4 * (ROUND_STEPS + 1) * 8], *v = tris;
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, h = y1 - y0;
    int n = 0;

    if (radius * 2.0f > x1 - x0) {
        radius = (x1 - x0) * 0.5f;
    }
    if (radius * 2.0f > h) {
        radius = h * 0.5f;
    }
    for (int c = 0; c < 4; c++) {
        // the corner's centre, and its quarter turn: top right, bottom right, bottom left, top left
        const float ox = kCorner[c][0] > 0 ? x1 - radius : x0 + radius;
        const float oy = kCorner[c][1] > 0 ? y1 - radius : y0 + radius;
        const float a0 = -1.5707963f + c * 1.5707963f;

        for (int s = 0; s <= ROUND_STEPS; s++) {
            const float a = a0 + 1.5707963f * s / ROUND_STEPS;
            px[n] = ox + cosf(a) * radius;
            py[n] = oy + sinf(a) * radius;
            n++;
        }
    }
    for (int i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        v = Vert(v, DispX(cx), DispY(cy), Mix(top, bottom, (cy - y0) / h), false);
        v = Vert(v, DispX(px[i]), DispY(py[i]), Mix(top, bottom, (py[i] - y0) / h), false);
        v = Vert(v, DispX(px[j]), DispY(py[j]), Mix(top, bottom, (py[j] - y0) / h), false);
    }
    DrawFlat(tris, v - tris);
}

void Ovl_Circle(float cx, float cy, float radius, uint32_t colour) {
    float tris[3 * CIRCLE_STEPS * 8], *v = tris;

    for (int i = 0; i < CIRCLE_STEPS; i++) {
        const float a0 = 6.2831853f * i / CIRCLE_STEPS, a1 = 6.2831853f * (i + 1) / CIRCLE_STEPS;
        v = Vert(v, DispX(cx), DispY(cy), colour, false);
        v = Vert(v, DispX(cx + cosf(a0) * radius), DispY(cy + sinf(a0) * radius), colour, false);
        v = Vert(v, DispX(cx + cosf(a1) * radius), DispY(cy + sinf(a1) * radius), colour, false);
    }
    DrawFlat(tris, v - tris);
}

void Ovl_Tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t colour) {
    float t[3 * 8], *v = t;

    v = Vert(v, DispX(x0), DispY(y0), colour, false);
    v = Vert(v, DispX(x1), DispY(y1), colour, false);
    v = Vert(v, DispX(x2), DispY(y2), colour, false);
    DrawFlat(t, v - t);
}

static bool ReadFont(void) {
    HANDLE f = CreateFileA(FONT_PATH, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD size, got = 0;

    if (f == INVALID_HANDLE_VALUE) {
        SPDLOG_ERROR("lus-x: menu: no {}", FONT_PATH);
        return false;
    }
    size = GetFileSize(f, NULL);
    sTtf = (unsigned char*)malloc(size);
    if (sTtf == NULL || !ReadFile(f, sTtf, size, &got, NULL) || got != size) {
        SPDLOG_ERROR("lus-x: menu: {} not read", FONT_PATH);
        free(sTtf);
        sTtf = NULL;
        CloseHandle(f);
        return false;
    }
    CloseHandle(f);
    return true;
}

bool Ovl_Load(Fast::GfxRenderingAPI* rapi) {
    sRapi = rapi;
    DisplayMap();
    if (sTtf != NULL) {
        return true;
    }
    if (!ReadFont()) {
        return false;
    }
    if (!stbtt_InitFont(&sInfo, sTtf, stbtt_GetFontOffsetForIndex(sTtf, 0))) {
        SPDLOG_ERROR("lus-x: menu: the font did not load");
        Ovl_Unload();
        return false;
    }
    unsigned char* alpha = (unsigned char*)malloc(512 * 512);
    uint32_t* rgba = (uint32_t*)malloc(512 * 512 * 4);
    bool ok = alpha != NULL && rgba != NULL;
    int ascent, descent, gap;

    stbtt_GetFontVMetrics(&sInfo, &ascent, &descent, &gap);
    for (int f = 0; ok && f < OVL_FONT_COUNT; f++) {
        OvlFont* font = &sFonts[f];
        const int w = kAtlasW[f], h = kAtlasH[f];
        const float px = kFontSize[f] * sScaleY;
        const float em = stbtt_ScaleForPixelHeight(&sInfo, px);
        stbtt_pack_context pc;

        memset(alpha, 0, (size_t)w * h);
        ok = stbtt_PackBegin(&pc, alpha, w, h, 0, 1, NULL) != 0;
        if (!ok) {
            break;
        }
        if (!stbtt_PackFontRange(&pc, sTtf, 0, px, FIRST_CHAR, CHAR_COUNT, font->chars)) {
            SPDLOG_WARN("lus-x: menu: font {} did not all fit its atlas", f);
        }
        stbtt_PackEnd(&pc);
        // white glyphs whose alpha is their coverage, which the vertex colour then tints
        for (int i = 0; i < w * h; i++) {
            rgba[i] = ((uint32_t)alpha[i] << 24) | 0xFFFFFFu;
        }
        if (font->texture == 0) {
            font->texture = rapi->NewTexture();
        }
        rapi->SelectTexture(0, font->texture);
        rapi->UploadTexture((const uint8_t*)rgba, w, h);
        rapi->SetSamplerParameters(0, false, TX_CLAMP, TX_CLAMP);
        font->ascent = ascent * em;
        font->descent = descent * em;
        font->scale = sScaleY;
    }
    free(alpha);
    free(rgba);
    if (!ok) {
        SPDLOG_ERROR("lus-x: menu: no memory for the font");
        Ovl_Unload();
    }
    return ok;
}

void Ovl_Unload(void) {
    for (int f = 0; f < OVL_FONT_COUNT; f++) {
        if (sFonts[f].texture != 0) {
            sRapi->DeleteTexture(sFonts[f].texture);
        }
    }
    free(sTtf);
    sTtf = NULL;
}

float Ovl_TextWidth(int font, const char* text) {
    float x = 0.0f;
    int prev = 0;

    if (sTtf == NULL) {
        return 0.0f;
    }
    const float em = stbtt_ScaleForPixelHeight(&sInfo, kFontSize[font] * sFonts[font].scale);
    for (; *text; text++) {
        const int c = (unsigned char)*text;

        if (c < FIRST_CHAR || c >= FIRST_CHAR + CHAR_COUNT) {
            continue;
        }
        if (prev) {
            x += stbtt_GetCodepointKernAdvance(&sInfo, prev, c) * em;
        }
        x += sFonts[font].chars[c - FIRST_CHAR].xadvance;
        prev = c;
    }
    return x / sFonts[font].scale; // display pixels, back to the layout's
}

void Ovl_Text(int font, float x, float y, const char* text, uint32_t colour, int align) {
    // static: too big for a small stack, and the menu draws on one thread
    static float verts[6 * MAX_TEXT * VERT_FLOATS];
    OvlFont* f = &sFonts[font];
    float* v = verts;
    int n = 0, prev = 0;

    if (sTtf == NULL) {
        return;
    }
    const float width = Ovl_TextWidth(font, text);
    if (align == OVL_ALIGN_CENTRE) {
        x -= width * 0.5f;
    } else if (align == OVL_ALIGN_RIGHT) {
        x -= width;
    }
    // the glyphs are made square; a display whose pixels aren't has them narrowed as they're drawn
    const float squeeze = sScaleX / sScaleY;
    const float em = stbtt_ScaleForPixelHeight(&sInfo, kFontSize[font] * f->scale);
    const float base = floorf(DispY(y) + (f->ascent + f->descent) * 0.5f + 0.5f);
    float pen = floorf(DispX(x) + 0.5f);

    for (; *text && n < MAX_TEXT; text++) {
        const int c = (unsigned char)*text;
        stbtt_aligned_quad q;
        float gx = 0.0f, gy = 0.0f;

        if (c < FIRST_CHAR || c >= FIRST_CHAR + CHAR_COUNT) {
            continue;
        }
        if (prev) {
            pen += floorf(stbtt_GetCodepointKernAdvance(&sInfo, prev, c) * em * squeeze + 0.5f);
        }
        prev = c;
        stbtt_GetPackedQuad(f->chars, kAtlasW[font], kAtlasH[font], c - FIRST_CHAR, &gx, &gy, &q, 1);
        const float x0 = pen + q.x0 * squeeze, x1 = pen + q.x1 * squeeze;
        if (q.x1 > q.x0) {
            v = Vert(v, x0, base + q.y0, colour, true, q.s0, q.t0);
            v = Vert(v, x1, base + q.y0, colour, true, q.s1, q.t0);
            v = Vert(v, x1, base + q.y1, colour, true, q.s1, q.t1);
            v = Vert(v, x0, base + q.y0, colour, true, q.s0, q.t0);
            v = Vert(v, x1, base + q.y1, colour, true, q.s1, q.t1);
            v = Vert(v, x0, base + q.y1, colour, true, q.s0, q.t1);
            n++;
        }
        pen += floorf(f->chars[c - FIRST_CHAR].xadvance * squeeze + 0.5f);
    }
    if (n == 0) {
        return;
    }
    UseShader(kTextShader);
    sRapi->SelectTexture(0, f->texture);
    // texel for pixel: unfiltered, unless the glyphs are being narrowed
    sRapi->SetSamplerParameters(0, squeeze != 1.0f, TX_CLAMP, TX_CLAMP);
    sRapi->DrawTriangles(verts, (size_t)n * 6 * VERT_FLOATS, (size_t)n * 2);
    if (kFontBold[font]) {
        const float step = 2.0f / sW; // one display pixel on, in clip space
        for (int i = 0; i < n * 6; i++) {
            verts[i * VERT_FLOATS] += step;
        }
        sRapi->DrawTriangles(verts, (size_t)n * 6 * VERT_FLOATS, (size_t)n * 2);
    }
}
