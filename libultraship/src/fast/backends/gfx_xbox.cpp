// The Xbox's television as Fast3D's window (after UltraX's ux_xbox_video.c).
#ifdef LUS_XBOX

#include "fast/backends/gfx_xbox.h"
#include "ship/Context.h"
#include "ship/resource/ResourceManager.h"

// after the C++ headers: the Xbox's xtl.h defines min and max (and ignores NOMINMAX)
#include <xtl.h>
#include <spdlog/spdlog.h>

extern double gXboxFileSeconds, gXboxBuildSeconds; // ResourceManager.cpp
extern uint32_t gXboxResourceLoads;
extern "C" uint32_t gLusxZipReads;       // lusx_zip.c
extern "C" uint64_t gLusxZipReadCycles;
extern "C" double gLusxCyclesPerMs;      // xbox_main.cpp

static bool sWidescreen;

// the display is 16:9, whatever its lines (the port's menu, xbox/src/lusx_overlay.cpp)
extern "C" int LusXbox_Widescreen(void) {
    return sWidescreen;
}

namespace Fast {

GfxWindowBackendXbox::~GfxWindowBackendXbox() {
}

void GfxWindowBackendXbox::Init(const char* gameName, const char* apiName, bool startFullScreen, uint32_t width,
                                uint32_t height, int32_t posX, int32_t posY) {
    D3DPRESENT_PARAMETERS pp;
    DWORD vflags = XGetVideoFlags();
    bool pal50 = XGetVideoStandard() == XC_VIDEO_STANDARD_PAL_I && !(vflags & XC_VIDEO_FLAGS_PAL_60Hz);
    LPDIRECT3D8 d3d = Direct3DCreate8(D3D_SDK_VERSION);

    if (d3d == NULL) {
        SPDLOG_CRITICAL("GfxWindowBackendXbox: no Direct3D");
        return;
    }
    memset(&pp, 0, sizeof(pp));
    // 720p when the dashboard allows it (HDTV pack, 720p ticked), at 16 bits a pixel: front, back and depth
    // are 3 x 1.8 MB, against 3 x 3.6 MB at 32 (and the N64 drew 16-bit color and depth too). Otherwise
    // 640x480 at 32 bits (3 x 1.2 MB), progressive when 480p is allowed.
    m720p = XGetAVPack() == XC_AV_PACK_HDTV && (vflags & XC_VIDEO_FLAGS_HDTV_720p) != 0;
    if (m720p) {
        mWidth = 1280;
        mHeight = 720;
    }
    pp.BackBufferWidth = mWidth;
    pp.BackBufferHeight = mHeight;
    pp.BackBufferFormat = m720p ? D3DFMT_R5G6B5 : D3DFMT_X8R8G8B8;
    // two back buffers (1.8 MB at 720p 16-bit): the GPU starts the next frame while a flip is pending, so
    // a CPU-heavy frame followed by a GPU-heavy one doesn't cost a whole blank
    pp.BackBufferCount = 2;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = m720p ? D3DFMT_D16 : D3DFMT_D24S8; // the color's size: the NV2A pairs them
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    if (m720p || (vflags & XC_VIDEO_FLAGS_HDTV_480p)) {
        pp.Flags = D3DPRESENTFLAG_PROGRESSIVE;
    }
    // 720p is 16:9 by itself (square pixels); at 480 lines, 16:9 is the TV stretching the picture
    if (m720p || (vflags & XC_VIDEO_FLAGS_WIDESCREEN)) {
        pp.Flags |= D3DPRESENTFLAG_WIDESCREEN;
        mWidescreen = true;
    }
    mRefresh = (pal50 && !(pp.Flags & D3DPRESENTFLAG_PROGRESSIVE)) ? 50 : 60;
    pp.FullScreen_RefreshRateInHz = mRefresh;
    pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    // a tick's three frames at once (~400 KB each in Clock Town): at the default 512 KB the CPU waited up to
    // 5.7 ms a frame for room, instead of going on to the next tick (measured on a console)
    d3d->SetPushBufferSize(1536 * 1024, 32 * 1024);
    HRESULT hr = d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &mDevice);
    if (FAILED(hr) && m720p) {
        SPDLOG_ERROR("GfxWindowBackendXbox: no 720p device ({:08x}); 480 lines instead", (uint32_t)hr);
        m720p = false;
        mWidth = 640;
        mHeight = 480;
        pp.BackBufferWidth = mWidth;
        pp.BackBufferHeight = mHeight;
        pp.BackBufferFormat = D3DFMT_X8R8G8B8;
        pp.AutoDepthStencilFormat = D3DFMT_D24S8;
        pp.Flags = (vflags & XC_VIDEO_FLAGS_HDTV_480p) ? D3DPRESENTFLAG_PROGRESSIVE : 0;
        mWidescreen = (vflags & XC_VIDEO_FLAGS_WIDESCREEN) != 0;
        pp.Flags |= mWidescreen ? D3DPRESENTFLAG_WIDESCREEN : 0;
        mRefresh = (pal50 && !(pp.Flags & D3DPRESENTFLAG_PROGRESSIVE)) ? 50 : 60;
        pp.FullScreen_RefreshRateInHz = mRefresh;
        hr = d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &mDevice);
    }
    if (FAILED(hr)) {
        SPDLOG_CRITICAL("GfxWindowBackendXbox: no Direct3D device ({:08x})", (uint32_t)hr);
        mDevice = nullptr;
        return;
    }
    mDevice->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xFF000000, 1.0f, 0);
    mDevice->Present(NULL, NULL, NULL, NULL);
    sWidescreen = mWidescreen;
    SPDLOG_INFO("GfxWindowBackendXbox: {}x{}{} at {} Hz, {}-bit{}", mWidth, mHeight,
                (pp.Flags & D3DPRESENTFLAG_PROGRESSIVE) ? "p" : "i", mRefresh, m720p ? 16 : 32,
                mWidescreen ? ", 16:9" : "");
    // The tile regions D3D left: Melee-X's frame buffers weren't tiled (pbkit's enable bit), and turning that on was
    // +11% at 720p. Whether ours are, and whether depth is compressed, has only been assumed.
    for (DWORD i = 0; i < 8; i++) {
        D3DTILE tile;
        memset(&tile, 0, sizeof(tile));
        mDevice->GetTile(i, &tile);
        if (tile.pMemory != NULL) {
            SPDLOG_INFO("lus-x: tile {}: {:p}, {} KB, pitch {}, flags {:08x}{}{}", i, tile.pMemory, tile.Size >> 10,
                        tile.Pitch, tile.Flags, (tile.Flags & D3DTILE_FLAGS_ZBUFFER) ? ", depth" : "",
                        (tile.Flags & D3DTILE_FLAGS_ZCOMPRESS) ? ", compressed" : "");
        }
    }
}

void GfxWindowBackendXbox::Close() {
    mIsRunning = false;
}

void GfxWindowBackendXbox::SetKeyboardCallbacks(bool (*onKeyDown)(int scancode), bool (*onKeyUp)(int scancode),
                                                void (*onAllKeysUp)()) {
    mOnKeyDown = onKeyDown;
    mOnKeyUp = onKeyUp;
}

void GfxWindowBackendXbox::SetMouseCallbacks(bool (*onMouseButtonDown)(int btn), bool (*onMouseButtonUp)(int btn)) {
    mOnMouseButtonDown = onMouseButtonDown;
    mOnMouseButtonUp = onMouseButtonUp;
}

void GfxWindowBackendXbox::SetFullscreenChangedCallback(void (*onFullscreenChanged)(bool is_now_fullscreen)) {
    mOnFullscreenChanged = onFullscreenChanged;
}

void GfxWindowBackendXbox::SetFullscreen(bool fullscreen) {
}

void GfxWindowBackendXbox::GetActiveWindowRefreshRate(uint32_t* refreshRate) {
    *refreshRate = mRefresh;
}

void GfxWindowBackendXbox::SetCursorVisibility(bool visibility) {
}

void GfxWindowBackendXbox::SetMousePos(int32_t posX, int32_t posY) {
}

void GfxWindowBackendXbox::GetMousePos(int32_t* x, int32_t* y) {
    *x = *y = 0;
}

void GfxWindowBackendXbox::GetMouseDelta(int32_t* x, int32_t* y) {
    *x = *y = 0;
}

void GfxWindowBackendXbox::GetMouseWheel(float* x, float* y) {
    *x = *y = 0.0f;
}

bool GfxWindowBackendXbox::GetMouseState(uint32_t btn) {
    return false;
}

void GfxWindowBackendXbox::SetMouseCapture(bool capture) {
}

bool GfxWindowBackendXbox::IsMouseCaptured() {
    return false;
}

void GfxWindowBackendXbox::GetDimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) {
    *width = mWidth;
    *height = mHeight;
    *posX = 0;
    *posY = 0;
}

void GfxWindowBackendXbox::SetDimensions(uint32_t width, uint32_t height, int32_t posX, int32_t posY) {
}

Ship::WindowRect GfxWindowBackendXbox::GetPrimaryMonitorRect() {
    return { 0, 0, (int32_t)mWidth, (int32_t)mHeight };
}

void GfxWindowBackendXbox::HandleEvents() {
    // the pads are polled where LUS reads them (ControlDeck); there are no other events
}

bool GfxWindowBackendXbox::IsFrameReady() {
    return true;
}

extern "C" void LusXbox_LogMemory(const char* when); // xbox_main.cpp
extern "C" void LusXbox_Presented(void);              // xbox_main.cpp's watchdog

// for 2Ship's frame-rate governor (BenPort.cpp): a clock, and the time spent waiting for blanks
static double sWaitSeconds;

extern "C" double LusXbox_Seconds(void) {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return (double)t.QuadPart / (double)f.QuadPart;
}

extern "C" double LusXbox_WaitSeconds(void) {
    return sWaitSeconds;
}

// time spent waiting for the GPU elsewhere (the depth read's fence): the governor's, not a frame's cost
extern "C" void LusXbox_AddWait(double seconds) {
    sWaitSeconds += seconds;
}

// every five seconds: frames shown, the share of the time the CPU spent making them (not waiting
// for a blank), free memory, and the renderer's counters
void GfxWindowBackendXbox::LogStats() {
    static bool sFirstLogged;
    double now = GetTime();
    if (!sFirstLogged) {
        sFirstLogged = true;
        LusXbox_LogMemory("first frame");
        mStatsStart = now;
        mCpuSeconds = 0.0;
        return;
    }
    if (now - mStatsStart < 5.0) {
        return;
    }
    double secs = now - mStatsStart;
    SPDLOG_INFO("lus-x: {:.1f} frames/s (target {}), CPU busy {:.0f}%, {:.1f} blanks waited a frame, Present {:.1f} ms "
                "a frame",
                mPresents / secs, mTargetFps, mCpuSeconds * 100.0 / secs,
                mPresents ? (double)mWaitedBlanks / mPresents : 0.0,
                mPresents ? mPresentSeconds * 1000.0 / mPresents : 0.0);
    mPresentSeconds = 0.0;
    LusXbox_LogMemory("now");
    auto ctx = Ship::Context::GetRawInstance();
    if (ctx != nullptr && ctx->GetResourceManager() != nullptr) {
        SPDLOG_INFO("lus-x: {} resources cached; {} loaded: {:.0f} ms reading ({} from the disc, {:.0f} ms of it), "
                    "{:.0f} ms building",
                    ctx->GetResourceManager()->CachedResourceCount(), gXboxResourceLoads, gXboxFileSeconds * 1000.0,
                    gLusxZipReads, gLusxZipReadCycles / gLusxCyclesPerMs, gXboxBuildSeconds * 1000.0);
        gXboxFileSeconds = gXboxBuildSeconds = 0.0;
        gXboxResourceLoads = 0;
        gLusxZipReads = 0;
        gLusxZipReadCycles = 0;
    }
    if (mStats) {
        mStats(mStatsArg);
    }
    mStatsStart = now;
    mCpuSeconds = 0.0;
    mPresents = 0;
    mWaitedBlanks = 0;
}

void GfxWindowBackendXbox::SwapBuffersBegin() {
    D3DFIELD_STATUS fs;
    uint32_t fps = mTargetFps > 0 ? mTargetFps : 60;
    uint32_t blanks = (mRefresh + fps / 2) / fps;

    if (mDevice == nullptr) {
        return;
    }
    double start = GetTime();
    if (mPresentStart != 0.0) {
        mCpuSeconds += start - mPresentStart; // the frame's making, since the last present returned
    }
    // the game's own rate (20 fps for MM: every third blank), never faster than the display
    if (blanks < 1) {
        blanks = 1;
    }
    // 40 fps: a tick's two frames in its three blanks, a short one and a long one
    const bool paced = blanks > 1 || fps == 40;
    if (fps == 40) {
        mPaceLong = !mPaceLong;
        blanks = mPaceLong ? 2 : 1;
    }
    double waitStart = GetTime();
    if (!paced) {
        // every blank: the hardware flips one queued frame a blank (D3DPRESENT_INTERVAL_ONE), so a frame only
        // waits here when it would be more than two ahead of the display. Waiting for a blank before each
        // present put a tick's first frame, with all the tick's work, behind 2 blanks of its own: 60 fps
        // needed the first frame in 33 ms, not the tick in 50
        mDevice->GetDisplayFieldStatus(&fs);
        mNextFlipBlank = (int32_t)(mNextFlipBlank - fs.VBlankCount) > 0 ? mNextFlipBlank + 1 : fs.VBlankCount + 1;
        while ((int32_t)(mNextFlipBlank - fs.VBlankCount) > 2) {
            mDevice->BlockUntilVerticalBlank();
            mWaitedBlanks++;
            mDevice->GetDisplayFieldStatus(&fs);
        }
    } else {
        for (;;) {
            mDevice->GetDisplayFieldStatus(&fs);
            // mLastPresentVBlank is the blank of the last present: the next one is due `blanks` later
            if (fs.VBlankCount - mLastPresentVBlank >= blanks) {
                break;
            }
            mDevice->BlockUntilVerticalBlank();
            mWaitedBlanks++;
        }
    }
    double presentStart = GetTime();
    mDevice->Present(NULL, NULL, NULL, NULL); // blocks while no back buffer is free
    double presented = GetTime();
    mPresentSeconds += presented - presentStart;
    sWaitSeconds += presented - waitStart; // (the governor's: all of it the display's pace, not the frame's cost)
    mDevice->GetDisplayFieldStatus(&fs);
    mLastPresentVBlank = fs.VBlankCount;
    mPresentStart = GetTime();
    mPresents++;
    LusXbox_Presented();
    LogStats();
}

void GfxWindowBackendXbox::SwapBuffersEnd() {
}

double GfxWindowBackendXbox::GetTime() {
    LARGE_INTEGER t, f;

    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return (double)t.QuadPart / (double)f.QuadPart;
}

int GfxWindowBackendXbox::GetTargetFps() {
    return mTargetFps;
}

void GfxWindowBackendXbox::SetTargetFps(int fps) {
    mTargetFps = fps;
}

void GfxWindowBackendXbox::SetMaxFrameLatency(int latency) {
}

const char* GfxWindowBackendXbox::GetKeyName(int scancode) {
    return "";
}

bool GfxWindowBackendXbox::CanDisableVsync() {
    return false;
}

bool GfxWindowBackendXbox::IsRunning() {
    return mIsRunning;
}

void GfxWindowBackendXbox::Destroy() {
}

bool GfxWindowBackendXbox::IsFullscreen() {
    return true;
}

} // namespace Fast

#endif
