// xbox_gamepad.cpp -- the SDL GameController subset libultraship's controller layer calls
// (xbox/include/SDL2/SDL.h), on the Xbox's XInput; after UltraX's ux_xbox_input.c.
//
// A "device index" is the Nth pad plugged in, in port order; its "instance id" is its port. The
// pads are read once a frame, in SDL_PumpEvents (LUS's ControlDeck calls it before reading its
// mappings), which also notices pads coming and going and tells LUS's device manager. Rumble is
// XInputSetState, one request in flight a pad (its XINPUT_FEEDBACK must stay put until taken).
//
// Also here, as the Xbox has them: SDL's one-off calls (a message box is a log line) and SDL_AddTimer
// (a thread of its own), which LUS's VI emulation uses.
#include <SDL2/SDL.h>

#include <stdio.h>
#include <string.h>
#include <spdlog/spdlog.h>

#include "ship/Context.h"
#include "ship/controller/controldeck/ControlDeck.h"
#include "ship/controller/physicaldevice/ConnectedPhysicalDeviceManager.h"
#include "libultraship/bridge/consolevariablebridge.h"

// after the C++ headers: the Xbox's xtl.h defines min and max (and ignores NOMINMAX)
#include <xtl.h>

#include "lusx_debug.h"
#include "lusx_menu.h"

#define PADS 4
#define PRESSED 0x40 // an analog button counts past this (of 255)

struct _SDL_GameController {
    int port;
    HANDLE h;
    XINPUT_STATE state;
    bool have;          // a state read this frame
    XINPUT_FEEDBACK fb; // rumble: must stay put while pending
    bool busy;
    Uint16 wantLow, wantHigh, sentLow, sentHigh;
    char name[32];
};

static _SDL_GameController sPads[PADS];
static bool sInit;

extern "C" void LusXbox_GamepadInit(void) {
    XDEVICE_PREALLOC_TYPE prealloc[] = { { XDEVICE_TYPE_GAMEPAD, PADS } };

    if (sInit) {
        return;
    }
    for (int i = 0; i < PADS; i++) {
        sPads[i].port = i;
        snprintf(sPads[i].name, sizeof(sPads[i].name), "Xbox Controller (port %d)", i + 1);
    }
    // enumeration is asynchronous: started first thing, waited for a little in LusXbox_GamepadWait
    XInitDevices(sizeof(prealloc) / sizeof(prealloc[0]), prealloc);
    sInit = true;
}

// pads plugged in or out since the last call; true if any changed
static bool Refresh(void) {
    DWORD ins = 0, rem = 0;
    bool changed = false;

    if (!XGetDeviceChanges(XDEVICE_TYPE_GAMEPAD, &ins, &rem)) {
        return false;
    }
    for (int i = 0; i < PADS; i++) {
        _SDL_GameController* p = &sPads[i];
        if ((rem & (1u << i)) && p->h != NULL) {
            XInputClose(p->h);
            p->h = NULL;
            p->busy = false;
            p->have = false;
            p->sentLow = p->sentHigh = p->wantLow = p->wantHigh = 0;
            changed = true;
        }
        if ((ins & (1u << i)) && p->h == NULL) {
            p->h = XInputOpen(XDEVICE_TYPE_GAMEPAD, (DWORD)i, XDEVICE_NO_SLOT, NULL);
            changed = true;
        }
    }
    return changed;
}

extern "C" void LusXbox_GamepadWait(void) {
    DWORD start = GetTickCount();

    for (;;) {
        Refresh();
        for (int i = 0; i < PADS; i++) {
            if (sPads[i].h != NULL) {
                Sleep(50); // a moment for its first report
                return;
            }
        }
        if (GetTickCount() - start > 1000) {
            return;
        }
        Sleep(10);
    }
}

static void RumbleSend(_SDL_GameController* p) {
    if (p->h == NULL) {
        return;
    }
    if (p->busy) {
        if (p->fb.Header.dwStatus == ERROR_IO_PENDING) {
            return;
        }
        p->busy = false;
    }
    if (p->wantLow == p->sentLow && p->wantHigh == p->sentHigh) {
        return;
    }
    memset(&p->fb, 0, sizeof(p->fb));
    p->fb.Rumble.wLeftMotorSpeed = p->wantLow;
    p->fb.Rumble.wRightMotorSpeed = p->wantHigh;
    if (XInputSetState(p->h, &p->fb) == ERROR_IO_PENDING) {
        p->busy = true;
    }
    p->sentLow = p->wantLow;
    p->sentHigh = p->wantHigh;
}

// a pad as the menu reads it (lusx_menu.h)
static unsigned MenuButtons(const XINPUT_GAMEPAD& g) {
    static const struct {
        int analog;
        unsigned bit;
    } kAnalog[] = {
        { XINPUT_GAMEPAD_A, LUSX_PAD_A },         { XINPUT_GAMEPAD_B, LUSX_PAD_B },
        { XINPUT_GAMEPAD_X, LUSX_PAD_X },         { XINPUT_GAMEPAD_Y, LUSX_PAD_Y },
        { XINPUT_GAMEPAD_BLACK, LUSX_PAD_BLACK }, { XINPUT_GAMEPAD_WHITE, LUSX_PAD_WHITE },
        { XINPUT_GAMEPAD_LEFT_TRIGGER, LUSX_PAD_LT }, { XINPUT_GAMEPAD_RIGHT_TRIGGER, LUSX_PAD_RT },
    };
    static const struct {
        unsigned button, bit;
    } kDigital[] = {
        { XINPUT_GAMEPAD_START, LUSX_PAD_START },     { XINPUT_GAMEPAD_BACK, LUSX_PAD_BACK },
        { XINPUT_GAMEPAD_LEFT_THUMB, LUSX_PAD_LS },   { XINPUT_GAMEPAD_RIGHT_THUMB, LUSX_PAD_RS },
        { XINPUT_GAMEPAD_DPAD_UP, LUSX_PAD_UP },      { XINPUT_GAMEPAD_DPAD_DOWN, LUSX_PAD_DOWN },
        { XINPUT_GAMEPAD_DPAD_LEFT, LUSX_PAD_LEFT },  { XINPUT_GAMEPAD_DPAD_RIGHT, LUSX_PAD_RIGHT },
    };
    const int push = 20000; // of 32767: a stick pushed, not resting off centre
    unsigned b = 0;

    for (const auto& a : kAnalog) {
        b |= g.bAnalogButtons[a.analog] > PRESSED ? a.bit : 0;
    }
    for (const auto& d : kDigital) {
        b |= (g.wButtons & d.button) ? d.bit : 0;
    }
    b |= g.sThumbRY > push ? LUSX_PAD_R_UP : g.sThumbRY < -push ? LUSX_PAD_R_DOWN : 0;
    b |= g.sThumbRX > push ? LUSX_PAD_R_RIGHT : g.sThumbRX < -push ? LUSX_PAD_R_LEFT : 0;
    b |= g.sThumbLY > push ? LUSX_PAD_L_UP : g.sThumbLY < -push ? LUSX_PAD_L_DOWN : 0;
    b |= g.sThumbLX > push ? LUSX_PAD_L_RIGHT : g.sThumbLX < -push ? LUSX_PAD_L_LEFT : 0;
    return b;
}

void SDL_PumpEvents(void) {
    static bool sToldDeck; // SDL reports the pads already in as "added" at start: LusXbox_GamepadWait saw them

    if (!sInit) {
        return;
    }
    if (Refresh() || !sToldDeck) {
        auto ctx = Ship::Context::GetRawInstance();
        if (ctx != nullptr && ctx->GetControlDeck() != nullptr) {
            ctx->GetControlDeck()->GetConnectedPhysicalDeviceManager()->RefreshConnectedSDLGamepads();
            sToldDeck = true;
        }
    }
    unsigned buttons = 0, menu = 0;
    int white = 0, black = 0;
    for (int i = 0; i < PADS; i++) {
        _SDL_GameController* p = &sPads[i];
        p->have = p->h != NULL && XInputGetState(p->h, &p->state) == ERROR_SUCCESS;
        RumbleSend(p);
        if (p->have) {
            buttons |= p->state.Gamepad.wButtons;
            white |= p->state.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_WHITE] > PRESSED;
            black |= p->state.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_BLACK] > PRESSED;
            menu |= MenuButtons(p->state.Gamepad);
        }
    }
    LusxDebug_Pad(buttons, white, black);
    LusxMenu_Pad(menu);
}

extern "C" void LusXbox_PadsQuiet(void) {
    for (int i = 0; i < PADS; i++) {
        sPads[i].wantLow = sPads[i].wantHigh = 0;
        RumbleSend(&sPads[i]);
    }
}

static _SDL_GameController* Nth(int device_index) {
    for (int i = 0, k = 0; i < PADS; i++) {
        if (sPads[i].h != NULL && k++ == device_index) {
            return &sPads[i];
        }
    }
    return NULL;
}

int SDL_NumJoysticks(void) {
    int n = 0;
    for (int i = 0; i < PADS; i++) {
        n += sPads[i].h != NULL;
    }
    return n;
}

SDL_bool SDL_IsGameController(int device_index) {
    return Nth(device_index) != NULL ? SDL_TRUE : SDL_FALSE;
}

SDL_GameController* SDL_GameControllerOpen(int device_index) {
    return Nth(device_index);
}

void SDL_GameControllerClose(SDL_GameController* gamecontroller) {
}

const char* SDL_GameControllerName(SDL_GameController* gamecontroller) {
    return gamecontroller ? gamecontroller->name : NULL;
}

SDL_Joystick* SDL_GameControllerGetJoystick(SDL_GameController* gamecontroller) {
    return gamecontroller;
}

SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick* joystick) {
    return joystick ? joystick->port : -1;
}

SDL_JoystickGUID SDL_JoystickGetDeviceGUID(int device_index) {
    SDL_JoystickGUID g;
    _SDL_GameController* p = Nth(device_index);
    memset(&g, 0, sizeof(g));
    if (p != NULL) {
        memcpy(g.data, "XBOXPAD", 7);
        g.data[7] = (Uint8)('1' + p->port);
    }
    return g;
}

void SDL_JoystickGetGUIDString(SDL_JoystickGUID guid, char* pszGUID, int cbGUID) {
    static const char kHex[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 16 && i * 2 + 2 < cbGUID; i++) {
        pszGUID[i * 2] = kHex[guid.data[i] >> 4];
        pszGUID[i * 2 + 1] = kHex[guid.data[i] & 15];
    }
    if (cbGUID > 0) {
        pszGUID[i * 2 < cbGUID ? i * 2 : cbGUID - 1] = '\0';
    }
}

Uint8 SDL_GameControllerGetButton(SDL_GameController* p, SDL_GameControllerButton button) {
    if (p == NULL || !p->have) {
        return 0;
    }
    const XINPUT_GAMEPAD& g = p->state.Gamepad;
    switch (button) {
        case SDL_CONTROLLER_BUTTON_A:
            return g.bAnalogButtons[XINPUT_GAMEPAD_A] > PRESSED;
        case SDL_CONTROLLER_BUTTON_B:
            return g.bAnalogButtons[XINPUT_GAMEPAD_B] > PRESSED;
        case SDL_CONTROLLER_BUTTON_X:
            return g.bAnalogButtons[XINPUT_GAMEPAD_X] > PRESSED;
        case SDL_CONTROLLER_BUTTON_Y:
            return g.bAnalogButtons[XINPUT_GAMEPAD_Y] > PRESSED;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            return g.bAnalogButtons[XINPUT_GAMEPAD_WHITE] > PRESSED;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            return g.bAnalogButtons[XINPUT_GAMEPAD_BLACK] > PRESSED;
        case SDL_CONTROLLER_BUTTON_BACK:
            return (g.wButtons & XINPUT_GAMEPAD_BACK) != 0;
        case SDL_CONTROLLER_BUTTON_START:
            return (g.wButtons & XINPUT_GAMEPAD_START) != 0;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK:
            return (g.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
            return (g.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            return (g.wButtons & XINPUT_GAMEPAD_DPAD_UP) != 0;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            return (g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            return (g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            return (g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
        default:
            return 0;
    }
}

// SDL's ranges: sticks -32768..32767 with y down, triggers 0..32767
Sint16 SDL_GameControllerGetAxis(SDL_GameController* p, SDL_GameControllerAxis axis) {
    if (p == NULL || !p->have) {
        return 0;
    }
    const XINPUT_GAMEPAD& g = p->state.Gamepad;
    switch (axis) {
        case SDL_CONTROLLER_AXIS_LEFTX:
            return g.sThumbLX;
        case SDL_CONTROLLER_AXIS_LEFTY:
            return (Sint16)(g.sThumbLY == -32768 ? 32767 : -g.sThumbLY);
        case SDL_CONTROLLER_AXIS_RIGHTX:
            return g.sThumbRX;
        case SDL_CONTROLLER_AXIS_RIGHTY:
            return (Sint16)(g.sThumbRY == -32768 ? 32767 : -g.sThumbRY);
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
            return (Sint16)(g.bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] * 32767 / 255);
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
            return (Sint16)(g.bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] * 32767 / 255);
        default:
            return 0;
    }
}

SDL_bool SDL_GameControllerHasRumble(SDL_GameController* p) {
    return p != NULL ? SDL_TRUE : SDL_FALSE;
}

int SDL_GameControllerRumble(SDL_GameController* p, Uint16 low, Uint16 high, Uint32 duration_ms) {
    // the menu's Rumble row: full, medium, low, off
    static const unsigned kPercent[4] = { 100, 66, 33, 0 };
    const int rumble = CVarGetInteger("gLusx.Rumble", 0);

    if (p == NULL) {
        return -1;
    }
    const unsigned percent = kPercent[rumble & 3];
    p->wantLow = (Uint16)(low * percent / 100);
    p->wantHigh = (Uint16)(high * percent / 100);
    RumbleSend(p);
    return 0;
}

SDL_bool SDL_GameControllerHasLED(SDL_GameController* gamecontroller) {
    return SDL_FALSE;
}

int SDL_JoystickSetLED(SDL_Joystick* joystick, Uint8 red, Uint8 green, Uint8 blue) {
    return -1;
}

SDL_bool SDL_GameControllerHasSensor(SDL_GameController* gamecontroller, SDL_SensorType type) {
    return SDL_FALSE;
}

int SDL_GameControllerSetSensorEnabled(SDL_GameController* gamecontroller, SDL_SensorType type, SDL_bool enabled) {
    return -1;
}

int SDL_GameControllerGetSensorData(SDL_GameController* gamecontroller, SDL_SensorType type, float* data,
                                    int num_values) {
    return -1;
}

int SDL_GameControllerAddMappingsFromFile(const char* file) {
    return 0; // the Xbox pad is the only kind there is
}

int SDL_Init(Uint32 flags) {
    LusXbox_GamepadInit();
    return 0;
}

SDL_bool SDL_SetHint(const char* name, const char* value) {
    return SDL_TRUE;
}

const char* SDL_GetError(void) {
    return "";
}

void SDL_free(void* mem) {
    free(mem);
}

char* SDL_GetPrefPath(const char* org, const char* app) {
    return strdup("T:\\");
}

int SDL_OpenURL(const char* url) {
    SPDLOG_INFO("(a link the Xbox cannot open: {})", url ? url : "");
    return -1;
}

int SDL_ShowSimpleMessageBox(Uint32 flags, const char* title, const char* message, SDL_Window* window) {
    SPDLOG_ERROR("{}: {}", title ? title : "", message ? message : "");
    return 0;
}

int SDL_ShowMessageBox(const SDL_MessageBoxData* data, int* buttonid) {
    int choice = data->numbuttons > 0 ? data->buttons[0].buttonid : -1;
    for (int i = 0; i < data->numbuttons; i++) {
        if (data->buttons[i].flags & SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT) {
            choice = data->buttons[i].buttonid;
        }
    }
    SPDLOG_WARN("{}: {} (answered with button {})", data->title ? data->title : "",
                data->message ? data->message : "", choice);
    if (buttonid) {
        *buttonid = choice;
    }
    return 0;
}

// SDL_AddTimer: a thread calling back every `interval` ms, on a schedule that does not drift
struct Timer {
    Uint32 interval;
    SDL_TimerCallback callback;
    void* param;
};

static DWORD WINAPI TimerThread(LPVOID arg) {
    Timer* t = (Timer*)arg;
    LARGE_INTEGER f, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&now);
    double next = (double)now.QuadPart / f.QuadPart * 1000.0;

    for (;;) {
        next += t->interval;
        QueryPerformanceCounter(&now);
        double ms = next - (double)now.QuadPart / f.QuadPart * 1000.0;
        if (ms > 0) {
            Sleep((DWORD)ms);
        }
        Uint32 again = t->callback(t->interval, t->param);
        if (again == 0) {
            break;
        }
        t->interval = again;
    }
    delete t;
    return 0;
}

SDL_TimerID SDL_AddTimer(Uint32 interval, SDL_TimerCallback callback, void* param) {
    // LUS's VI manager (os_vi.cpp) is its only user: a 62.5 Hz thread feeding a message queue no thread
    // reads (the game's osCreateThread is a stub). No thread: the one core is the game's.
    (void)callback;
    (void)param;
    return interval != 0 ? 1 : 0;
    Timer* t = new Timer{ interval, callback, param };
    HANDLE h = CreateThread(NULL, 16 * 1024, TimerThread, t, 0, NULL);
    if (h == NULL) {
        delete t;
        return 0;
    }
    SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL);
    CloseHandle(h);
    return 1;
}
