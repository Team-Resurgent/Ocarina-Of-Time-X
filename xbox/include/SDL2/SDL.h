/* SDL2/SDL.h -- Lus-X: NOT SDL. The Xbox build has no SDL. libultraship's
 * controller layer (its mapping classes, their factories, the connected-device
 * manager) is written against SDL's GameController API; this declares just
 * that subset, and xbox/src/xbox_gamepad.cpp implements it on the Xbox's own
 * XInput, so the mapping/remapping code and its saved config (which store
 * SDL's button and axis numbers) build and work unchanged.
 *
 * Everything else LUS took from SDL is an Xbox backend behind LUS's own
 * interfaces: the window (Fast::GfxWindowBackend), the renderer
 * (Fast::GfxRenderingAPI), the sound (Ship::AudioPlayer). The few one-off
 * calls left (message boxes, hints, the VI timer) are here too, as the Xbox
 * has them: a message box is a log line. */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t Uint8;
typedef int8_t Sint8;
typedef uint16_t Uint16;
typedef int16_t Sint16;
typedef uint32_t Uint32;
typedef int32_t Sint32;
typedef uint64_t Uint64;
typedef int64_t Sint64;
typedef enum { SDL_FALSE = 0, SDL_TRUE = 1 } SDL_bool;

/* SDL2's numbering: LUS saves mappings by these numbers */
typedef enum {
    SDL_CONTROLLER_BUTTON_INVALID = -1,
    SDL_CONTROLLER_BUTTON_A,
    SDL_CONTROLLER_BUTTON_B,
    SDL_CONTROLLER_BUTTON_X,
    SDL_CONTROLLER_BUTTON_Y,
    SDL_CONTROLLER_BUTTON_BACK,
    SDL_CONTROLLER_BUTTON_GUIDE,
    SDL_CONTROLLER_BUTTON_START,
    SDL_CONTROLLER_BUTTON_LEFTSTICK,
    SDL_CONTROLLER_BUTTON_RIGHTSTICK,
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  /* the Xbox pad's White */
    SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, /* the Xbox pad's Black */
    SDL_CONTROLLER_BUTTON_DPAD_UP,
    SDL_CONTROLLER_BUTTON_DPAD_DOWN,
    SDL_CONTROLLER_BUTTON_DPAD_LEFT,
    SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
    SDL_CONTROLLER_BUTTON_MISC1,
    SDL_CONTROLLER_BUTTON_PADDLE1,
    SDL_CONTROLLER_BUTTON_PADDLE2,
    SDL_CONTROLLER_BUTTON_PADDLE3,
    SDL_CONTROLLER_BUTTON_PADDLE4,
    SDL_CONTROLLER_BUTTON_TOUCHPAD,
    SDL_CONTROLLER_BUTTON_MAX
} SDL_GameControllerButton;

typedef enum {
    SDL_CONTROLLER_AXIS_INVALID = -1,
    SDL_CONTROLLER_AXIS_LEFTX,
    SDL_CONTROLLER_AXIS_LEFTY,
    SDL_CONTROLLER_AXIS_RIGHTX,
    SDL_CONTROLLER_AXIS_RIGHTY,
    SDL_CONTROLLER_AXIS_TRIGGERLEFT,
    SDL_CONTROLLER_AXIS_TRIGGERRIGHT,
    SDL_CONTROLLER_AXIS_MAX
} SDL_GameControllerAxis;

typedef enum { SDL_SENSOR_INVALID = -1, SDL_SENSOR_UNKNOWN, SDL_SENSOR_ACCEL, SDL_SENSOR_GYRO } SDL_SensorType;

#define SDL_JOYSTICK_AXIS_MAX 32767
#define SDL_JOYSTICK_AXIS_MIN -32768

/* an Xbox pad (xbox_gamepad.cpp); a "joystick" is the same pad */
typedef struct _SDL_GameController SDL_GameController;
typedef struct _SDL_GameController SDL_Joystick;
typedef int32_t SDL_JoystickID;
typedef struct {
    Uint8 data[16];
} SDL_JoystickGUID;

int SDL_NumJoysticks(void);
SDL_bool SDL_IsGameController(int device_index);
SDL_GameController* SDL_GameControllerOpen(int device_index);
void SDL_GameControllerClose(SDL_GameController* gamecontroller);
const char* SDL_GameControllerName(SDL_GameController* gamecontroller);
SDL_Joystick* SDL_GameControllerGetJoystick(SDL_GameController* gamecontroller);
SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick* joystick);
SDL_JoystickGUID SDL_JoystickGetDeviceGUID(int device_index);
void SDL_JoystickGetGUIDString(SDL_JoystickGUID guid, char* pszGUID, int cbGUID);
Uint8 SDL_GameControllerGetButton(SDL_GameController* gamecontroller, SDL_GameControllerButton button);
Sint16 SDL_GameControllerGetAxis(SDL_GameController* gamecontroller, SDL_GameControllerAxis axis);
SDL_bool SDL_GameControllerHasRumble(SDL_GameController* gamecontroller);
int SDL_GameControllerRumble(SDL_GameController* gamecontroller, Uint16 low_frequency_rumble,
                             Uint16 high_frequency_rumble, Uint32 duration_ms);
SDL_bool SDL_GameControllerHasLED(SDL_GameController* gamecontroller);
int SDL_JoystickSetLED(SDL_Joystick* joystick, Uint8 red, Uint8 green, Uint8 blue);
SDL_bool SDL_GameControllerHasSensor(SDL_GameController* gamecontroller, SDL_SensorType type);
int SDL_GameControllerSetSensorEnabled(SDL_GameController* gamecontroller, SDL_SensorType type, SDL_bool enabled);
int SDL_GameControllerGetSensorData(SDL_GameController* gamecontroller, SDL_SensorType type, float* data,
                                    int num_values);
int SDL_GameControllerAddMappingsFromFile(const char* file);
void SDL_PumpEvents(void);

/* the rest LUS and 2Ship call */
#define SDL_INIT_GAMECONTROLLER 0x00002000u
#define SDL_HINT_JOYSTICK_THREAD "SDL_JOYSTICK_THREAD"
#define SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS"
int SDL_Init(Uint32 flags);
SDL_bool SDL_SetHint(const char* name, const char* value);
const char* SDL_GetError(void);
void SDL_free(void* mem);
char* SDL_GetPrefPath(const char* org, const char* app);
int SDL_OpenURL(const char* url);
#define SDL_memcmp memcmp

typedef Uint32 (*SDL_TimerCallback)(Uint32 interval, void* param);
typedef int SDL_TimerID;
/* a thread of its own, calling back every interval ms */
SDL_TimerID SDL_AddTimer(Uint32 interval, SDL_TimerCallback callback, void* param);

typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer; /* (named in libultraship's Fast3dGui.h, which SoH's files include) */
typedef enum {
    SDL_MESSAGEBOX_ERROR = 0x10,
    SDL_MESSAGEBOX_WARNING = 0x20,
    SDL_MESSAGEBOX_INFORMATION = 0x40
} SDL_MessageBoxFlags;
typedef enum {
    SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT = 1,
    SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT = 2
} SDL_MessageBoxButtonFlags;
typedef struct {
    Uint32 flags;
    int buttonid;
    const char* text;
} SDL_MessageBoxButtonData;
typedef struct {
    Uint32 flags;
    SDL_Window* window;
    const char* title;
    const char* message;
    int numbuttons;
    const SDL_MessageBoxButtonData* buttons;
    const void* colorScheme;
} SDL_MessageBoxData;
/* to the debug channel; ShowMessageBox answers with the default button */
int SDL_ShowSimpleMessageBox(Uint32 flags, const char* title, const char* message, SDL_Window* window);
int SDL_ShowMessageBox(const SDL_MessageBoxData* messageboxdata, int* buttonid);

#ifdef __cplusplus
}
#endif
