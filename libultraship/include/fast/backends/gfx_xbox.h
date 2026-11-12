#pragma once
#ifdef LUS_XBOX

#include "gfx_window_manager_api.h"

struct D3DDevice;

namespace Fast {

/**
 * @brief The original Xbox as a "window": the television.
 *
 * Init() makes the Direct3D 8 device in the dashboard's video mode: 1280x720
 * at 16 bits a pixel when 720p is allowed, else 640x480 at 32 (progressive
 * when 480p is allowed, 50 Hz only on a PAL console without PAL60); the
 * renderer (GfxRenderingAPID3D8) draws through it.
 * Frames are paced by the display's vertical blanks: a present waits until
 * 60/target-fps blanks have passed since the last one. No keyboard or mouse.
 */
class GfxWindowBackendXbox final : public GfxWindowBackend {
  public:
    GfxWindowBackendXbox() = default;
    ~GfxWindowBackendXbox() override;

    void Init(const char* gameName, const char* apiName, bool startFullScreen, uint32_t width, uint32_t height,
              int32_t posX, int32_t posY) override;
    void Close() override;
    void SetKeyboardCallbacks(bool (*onKeyDown)(int scancode), bool (*onKeyUp)(int scancode),
                              void (*onAllKeysUp)()) override;
    void SetMouseCallbacks(bool (*onMouseButtonDown)(int btn), bool (*onMouseButtonUp)(int btn)) override;
    void SetFullscreenChangedCallback(void (*onFullscreenChanged)(bool is_now_fullscreen)) override;
    void SetFullscreen(bool fullscreen) override;
    void GetActiveWindowRefreshRate(uint32_t* refreshRate) override;
    void SetCursorVisibility(bool visibility) override;
    void SetMousePos(int32_t posX, int32_t posY) override;
    void GetMousePos(int32_t* x, int32_t* y) override;
    void GetMouseDelta(int32_t* x, int32_t* y) override;
    void GetMouseWheel(float* x, float* y) override;
    bool GetMouseState(uint32_t btn) override;
    void SetMouseCapture(bool capture) override;
    bool IsMouseCaptured() override;
    void GetDimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) override;
    void SetDimensions(uint32_t width, uint32_t height, int32_t posX, int32_t posY) override;
    Ship::WindowRect GetPrimaryMonitorRect() override;
    void HandleEvents() override;
    bool IsFrameReady() override;
    void SwapBuffersBegin() override;
    void SwapBuffersEnd() override;
    double GetTime() override;
    int GetTargetFps() override;
    void SetTargetFps(int fps) override;
    void SetMaxFrameLatency(int latency) override;
    const char* GetKeyName(int scancode) override;
    bool CanDisableVsync() override;
    bool IsRunning() override;
    void Destroy() override;
    bool IsFullscreen() override;

    D3DDevice* GetDevice() const {
        return mDevice;
    }
    /** @brief true when the display is 16:9 (480 lines sent anamorphic). */
    bool IsWidescreen() const {
        return mWidescreen;
    }
    /** @brief true when the screen is 16 bits a pixel (720p: R5G6B5 color, D16 depth). */
    bool Is16Bit() const {
        return m720p;
    }
    /** @brief Called with the five-second performance line (the renderer adds its counters). */
    void SetStatsCallback(void (*fn)(void*), void* arg) {
        mStats = fn;
        mStatsArg = arg;
    }

  private:
    void LogStats();

    D3DDevice* mDevice = nullptr;
    uint32_t mWidth = 640, mHeight = 480, mRefresh = 60;
    bool mWidescreen = false;
    bool m720p = false;
    uint32_t mLastPresentVBlank = 0;
    bool mPaceLong = false; // 40 fps: the next frame is the long one of a tick's two
    uint32_t mNextFlipBlank = 0; // at 60: the blank the last queued frame flips on
    uint32_t mPresents = 0, mWaitedBlanks = 0;
    double mStatsStart = 0.0, mCpuSeconds = 0.0, mPresentStart = 0.0, mPresentSeconds = 0.0;
    void (*mStats)(void*) = nullptr;
    void* mStatsArg = nullptr;
};

} // namespace Fast

#endif
