/*
 * SDL.h — minimal SDL2 subset for the PlayStation 4 (Piglet/GLES 2.0) build.
 *
 * The engine's platform layer is written against SDL2 and is the part that
 * carries all of the DKR-specific behaviour (pad mapping, audio ring, timing,
 * save handling). Rather than reimplement that layer, this header maps the
 * small SDL surface it actually uses onto the Orbis APIs:
 *
 *   window / GL  -> EGL + libScePigletv2VSH (GLES 2.0)
 *   input        -> libScePad, published as ONE controller
 *   audio        -> libSceAudioOut at 48 kHz stereo S16
 *   threads      -> pthread (ScePthread, linked with -lpthread)
 *   time         -> CLOCK_MONOTONIC
 *
 * Only what the 255 translation units of the mdkr64 target reference is here;
 * anything else is deliberately absent so that a new dependency fails loudly at
 * compile time instead of silently reaching for a stub.
 */
#ifndef SDL_PS4_SHIM_H
#define SDL_PS4_SHIM_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- version ---------------------------------------------------------- */
typedef struct SDL_version {
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
} SDL_version;

#define SDL_MAJOR_VERSION 2
#define SDL_MINOR_VERSION 26
#define SDL_PATCHLEVEL    0

#define SDL_VERSION(x)                         \
    do {                                       \
        (x)->major = SDL_MAJOR_VERSION;        \
        (x)->minor = SDL_MINOR_VERSION;        \
        (x)->patch = SDL_PATCHLEVEL;           \
    } while (0)

#define SDL_VERSIONNUM(X, Y, Z) ((X) * 1000 + (Y) * 100 + (Z))
#define SDL_COMPILEDVERSION SDL_VERSIONNUM(SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL)
#define SDL_VERSION_ATLEAST(X, Y, Z) (SDL_COMPILEDVERSION >= SDL_VERSIONNUM((X), (Y), (Z)))

/* ---- basic types ------------------------------------------------------ */
typedef int8_t   Sint8;
typedef uint8_t  Uint8;
typedef int16_t  Sint16;
typedef uint16_t Uint16;
typedef int32_t  Sint32;
typedef uint32_t Uint32;
typedef int64_t  Sint64;
typedef uint64_t Uint64;
typedef uint8_t  SDL_bool;
#define SDL_FALSE 0
#define SDL_TRUE  1

typedef int32_t SDL_JoystickID;

typedef struct SDL_Window   SDL_Window;
typedef struct SDL_Joystick SDL_Joystick;
typedef struct SDL_GameController SDL_GameController;
typedef void *SDL_GLContext;
typedef struct SDL_mutex SDL_mutex;
typedef struct SDL_cond  SDL_cond;
typedef struct SDL_Thread SDL_Thread;
typedef void *SDL_TimerID;

/* ---- init flags ------------------------------------------------------- */
#define SDL_INIT_TIMER    0x00000001u
#define SDL_INIT_AUDIO    0x00000010u
#define SDL_INIT_VIDEO    0x00000020u
#define SDL_INIT_JOYSTICK 0x00000200u
#define SDL_INIT_HAPTIC   0x00001000u
#define SDL_INIT_GAMECONTROLLER 0x00002000u
#define SDL_INIT_EVENTS   0x00004000u
#define SDL_INIT_SENSOR   0x00008000u
#define SDL_INIT_EVERYTHING (SDL_INIT_TIMER | SDL_INIT_AUDIO | SDL_INIT_VIDEO | \
                             SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC | \
                             SDL_INIT_GAMECONTROLLER | SDL_INIT_SENSOR)

/* ---- hints ------------------------------------------------------------ */
typedef enum {
    SDL_HINT_DEFAULT,
    SDL_HINT_NORMAL,
    SDL_HINT_OVERRIDE
} SDL_HintPriority;

/* ---- scancodes / keycodes (only the ones the engine tests) ------------ */
typedef enum {
    SDL_SCANCODE_UNKNOWN = 0,
    SDL_SCANCODE_A = 4, SDL_SCANCODE_B, SDL_SCANCODE_C, SDL_SCANCODE_D,
    SDL_SCANCODE_E, SDL_SCANCODE_F, SDL_SCANCODE_G, SDL_SCANCODE_H,
    SDL_SCANCODE_I, SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_L,
    SDL_SCANCODE_M, SDL_SCANCODE_N, SDL_SCANCODE_O, SDL_SCANCODE_P,
    SDL_SCANCODE_Q, SDL_SCANCODE_R, SDL_SCANCODE_S, SDL_SCANCODE_T,
    SDL_SCANCODE_U, SDL_SCANCODE_V, SDL_SCANCODE_W, SDL_SCANCODE_X,
    SDL_SCANCODE_Y, SDL_SCANCODE_Z,
    SDL_SCANCODE_1 = 30, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4,
    SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8,
    SDL_SCANCODE_9, SDL_SCANCODE_0,
    SDL_SCANCODE_RETURN = 40, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_BACKSPACE,
    SDL_SCANCODE_TAB, SDL_SCANCODE_SPACE,
    SDL_SCANCODE_F1 = 58, SDL_SCANCODE_F2, SDL_SCANCODE_F3, SDL_SCANCODE_F4,
    SDL_SCANCODE_F5, SDL_SCANCODE_F6, SDL_SCANCODE_F7, SDL_SCANCODE_F8,
    SDL_SCANCODE_F9, SDL_SCANCODE_F10, SDL_SCANCODE_F11, SDL_SCANCODE_F12,
    SDL_SCANCODE_RIGHT = 79, SDL_SCANCODE_LEFT, SDL_SCANCODE_DOWN,
    SDL_SCANCODE_UP,
    SDL_SCANCODE_LSHIFT = 225, SDL_SCANCODE_RSHIFT = 229
} SDL_Scancode;

#define SDL_NUM_SCANCODES 512

typedef int32_t SDL_Keycode;
#define SDLK_UNKNOWN   0x00000000
#define SDLK_RETURN    '\r'
#define SDLK_ESCAPE    '\033'
#define SDLK_BACKSPACE '\b'
#define SDLK_TAB       '\t'
#define SDLK_SPACE     ' '
#define SDLK_F1  (SDL_Keycode)(1 << 30 | 58)
#define SDLK_F2  (SDL_Keycode)(1 << 30 | 59)
#define SDLK_F9  (SDL_Keycode)(1 << 30 | 66)

typedef uint16_t SDL_Keymod;
#define KMOD_NONE 0x0000

typedef struct SDL_Keysym {
    SDL_Scancode scancode;
    SDL_Keycode sym;
    Uint16 mod;
    Uint32 unused;
} SDL_Keysym;

/* ---- events ----------------------------------------------------------- */
enum {
    SDL_FIRSTEVENT = 0,
    SDL_QUIT = 0x100,
    SDL_WINDOWEVENT = 0x200,
    SDL_KEYDOWN = 0x300,
    SDL_KEYUP,
    SDL_TEXTEDITING,
    SDL_TEXTINPUT,
    SDL_MOUSEMOTION = 0x400,
    SDL_MOUSEBUTTONDOWN,
    SDL_MOUSEBUTTONUP,
    SDL_MOUSEWHEEL,
    SDL_JOYAXISMOTION = 0x600,
    SDL_JOYBALLMOTION,
    SDL_JOYHATMOTION,
    SDL_JOYBUTTONDOWN,
    SDL_JOYBUTTONUP,
    SDL_JOYDEVICEADDED,
    SDL_JOYDEVICEREMOVED,
    SDL_CONTROLLERAXISMOTION = 0x650,
    SDL_CONTROLLERBUTTONDOWN,
    SDL_CONTROLLERBUTTONUP,
    SDL_CONTROLLERDEVICEADDED,
    SDL_CONTROLLERDEVICEREMOVED,
    SDL_CONTROLLERDEVICEREMAPPED,
    SDL_LASTEVENT = 0xFFFF
};

enum {
    SDL_WINDOWEVENT_NONE = 0,
    SDL_WINDOWEVENT_SHOWN,
    SDL_WINDOWEVENT_HIDDEN,
    SDL_WINDOWEVENT_EXPOSED,
    SDL_WINDOWEVENT_MOVED,
    SDL_WINDOWEVENT_RESIZED,
    SDL_WINDOWEVENT_SIZE_CHANGED,
    SDL_WINDOWEVENT_MINIMIZED,
    SDL_WINDOWEVENT_MAXIMIZED,
    SDL_WINDOWEVENT_RESTORED,
    SDL_WINDOWEVENT_ENTER,
    SDL_WINDOWEVENT_LEAVE,
    SDL_WINDOWEVENT_FOCUS_GAINED,
    SDL_WINDOWEVENT_FOCUS_LOST,
    SDL_WINDOWEVENT_CLOSE
};

#define SDL_RELEASED 0
#define SDL_PRESSED  1

/* Calling-convention marker: empty on Orbis, but the engine spells callbacks
 * with it. */
#ifndef SDLCALL
#define SDLCALL
#endif
#ifndef DECLSPEC
#define DECLSPEC
#endif

typedef struct SDL_CommonEvent { Uint32 type; Uint32 timestamp; } SDL_CommonEvent;
typedef struct SDL_WindowEvent {
    Uint32 type; Uint32 timestamp; Uint32 windowID; Uint8 event; Uint8 padding1;
    Uint8 padding2; Uint8 padding3; Sint32 data1; Sint32 data2;
} SDL_WindowEvent;
typedef struct SDL_KeyboardEvent {
    Uint32 type; Uint32 timestamp; Uint32 windowID; Uint8 state; Uint8 repeat;
    Uint8 padding2; Uint8 padding3; SDL_Keysym keysym;
} SDL_KeyboardEvent;
typedef struct SDL_TextInputEvent {
    Uint32 type; Uint32 timestamp; Uint32 windowID; char text[32];
} SDL_TextInputEvent;
typedef struct SDL_MouseMotionEvent {
    Uint32 type; Uint32 timestamp; Uint32 windowID; Uint32 which; Uint32 state;
    Sint32 x; Sint32 y; Sint32 xrel; Sint32 yrel;
} SDL_MouseMotionEvent;
typedef struct SDL_MouseButtonEvent {
    Uint32 type; Uint32 timestamp; Uint32 windowID; Uint32 which; Uint8 button;
    Uint8 state; Uint8 clicks; Uint8 padding1; Sint32 x; Sint32 y;
} SDL_MouseButtonEvent;
typedef struct SDL_MouseWheelEvent {
    Uint32 type; Uint32 timestamp; Uint32 windowID; Uint32 which;
    Sint32 x; Sint32 y; Uint32 direction; float preciseX; float preciseY;
} SDL_MouseWheelEvent;
typedef struct SDL_JoyAxisEvent {
    Uint32 type; Uint32 timestamp; SDL_JoystickID which; Uint8 axis;
    Uint8 padding1; Uint8 padding2; Uint8 padding3; Sint16 value; Uint16 padding4;
} SDL_JoyAxisEvent;
typedef struct SDL_JoyHatEvent {
    Uint32 type; Uint32 timestamp; SDL_JoystickID which; Uint8 hat; Uint8 value;
    Uint8 padding1; Uint8 padding2;
} SDL_JoyHatEvent;
typedef struct SDL_JoyButtonEvent {
    Uint32 type; Uint32 timestamp; SDL_JoystickID which; Uint8 button; Uint8 state;
    Uint8 padding1; Uint8 padding2;
} SDL_JoyButtonEvent;
typedef struct SDL_JoyDeviceEvent {
    Uint32 type; Uint32 timestamp; Sint32 which;
} SDL_JoyDeviceEvent;
typedef struct SDL_ControllerAxisEvent {
    Uint32 type; Uint32 timestamp; SDL_JoystickID which; Uint8 axis;
    Uint8 padding1; Uint8 padding2; Uint8 padding3; Sint16 value; Uint16 padding4;
} SDL_ControllerAxisEvent;
typedef struct SDL_ControllerButtonEvent {
    Uint32 type; Uint32 timestamp; SDL_JoystickID which; Uint8 button; Uint8 state;
    Uint8 padding1; Uint8 padding2;
} SDL_ControllerButtonEvent;
typedef struct SDL_ControllerDeviceEvent {
    Uint32 type; Uint32 timestamp; Sint32 which;
} SDL_ControllerDeviceEvent;

typedef union SDL_Event {
    Uint32 type;
    SDL_CommonEvent common;
    SDL_WindowEvent window;
    SDL_KeyboardEvent key;
    SDL_TextInputEvent text;
    SDL_MouseMotionEvent motion;
    SDL_MouseButtonEvent button;
    SDL_MouseWheelEvent wheel;
    SDL_JoyAxisEvent jaxis;
    SDL_JoyHatEvent jhat;
    SDL_JoyButtonEvent jbutton;
    SDL_JoyDeviceEvent jdevice;
    SDL_ControllerAxisEvent caxis;
    SDL_ControllerButtonEvent cbutton;
    SDL_ControllerDeviceEvent cdevice;
    Uint8 padding[64];
} SDL_Event;

/* ---- controller / joystick enums -------------------------------------- */
typedef enum {
    SDL_CONTROLLER_BUTTON_INVALID = -1,
    SDL_CONTROLLER_BUTTON_A = 0,
    SDL_CONTROLLER_BUTTON_B,
    SDL_CONTROLLER_BUTTON_X,
    SDL_CONTROLLER_BUTTON_Y,
    SDL_CONTROLLER_BUTTON_BACK,
    SDL_CONTROLLER_BUTTON_GUIDE,
    SDL_CONTROLLER_BUTTON_START,
    SDL_CONTROLLER_BUTTON_LEFTSTICK,
    SDL_CONTROLLER_BUTTON_RIGHTSTICK,
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
    SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
    SDL_CONTROLLER_BUTTON_DPAD_UP,
    SDL_CONTROLLER_BUTTON_DPAD_DOWN,
    SDL_CONTROLLER_BUTTON_DPAD_LEFT,
    SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
    SDL_CONTROLLER_BUTTON_MAX
} SDL_GameControllerButton;

typedef enum {
    SDL_CONTROLLER_AXIS_INVALID = -1,
    SDL_CONTROLLER_AXIS_LEFTX = 0,
    SDL_CONTROLLER_AXIS_LEFTY,
    SDL_CONTROLLER_AXIS_RIGHTX,
    SDL_CONTROLLER_AXIS_RIGHTY,
    SDL_CONTROLLER_AXIS_TRIGGERLEFT,
    SDL_CONTROLLER_AXIS_TRIGGERRIGHT,
    SDL_CONTROLLER_AXIS_MAX
} SDL_GameControllerAxis;

typedef enum {
    SDL_JOYSTICK_TYPE_UNKNOWN = 0,
    SDL_JOYSTICK_TYPE_GAMECONTROLLER = 1
} SDL_JoystickType;

typedef struct SDL_JoystickGUID { Uint8 data[16]; } SDL_JoystickGUID;

/* ---- window / GL ------------------------------------------------------ */
enum {
    SDL_WINDOW_FULLSCREEN = 0x00000001,
    SDL_WINDOW_OPENGL     = 0x00000002,
    SDL_WINDOW_SHOWN      = 0x00000004,
    SDL_WINDOW_HIDDEN     = 0x00000008,
    SDL_WINDOW_BORDERLESS = 0x00000010,
    SDL_WINDOW_RESIZABLE  = 0x00000020,
    SDL_WINDOW_MINIMIZED  = 0x00000040,
    SDL_WINDOW_MAXIMIZED  = 0x00000080,
    SDL_WINDOW_INPUT_GRABBED = 0x00000100,
    SDL_WINDOW_ALLOW_HIGHDPI = 0x00002000
};

enum {
    SDL_GL_RED_SIZE, SDL_GL_GREEN_SIZE, SDL_GL_BLUE_SIZE, SDL_GL_ALPHA_SIZE,
    SDL_GL_BUFFER_SIZE, SDL_GL_DOUBLEBUFFER, SDL_GL_DEPTH_SIZE,
    SDL_GL_STENCIL_SIZE, SDL_GL_ACCUM_RED_SIZE, SDL_GL_ACCUM_GREEN_SIZE,
    SDL_GL_ACCUM_BLUE_SIZE, SDL_GL_ACCUM_ALPHA_SIZE, SDL_GL_STEREO,
    SDL_GL_MULTISAMPLEBUFFERS, SDL_GL_MULTISAMPLESAMPLES, SDL_GL_ACCELERATED_VISUAL,
    SDL_GL_RETAINED_BACKING, SDL_GL_CONTEXT_MAJOR_VERSION, SDL_GL_CONTEXT_MINOR_VERSION,
    SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_SHARE_WITH_CURRENT_CONTEXT,
    SDL_GL_FRAMEBUFFER_SRGB_CAPABLE, SDL_GL_CONTEXT_RELEASE_BEHAVIOR,
    SDL_GL_CONTEXT_RESET_NOTIFICATION, SDL_GL_CONTEXT_NO_ERROR,
    SDL_GL_FLOATBUFFERS, SDL_GL_EGL_PLATFORM
};

#define SDL_GL_CONTEXT_PROFILE_MASK          22
#define SDL_GL_CONTEXT_PROFILE_CORE          0x0001
#define SDL_GL_CONTEXT_PROFILE_COMPATIBILITY 0x0002
#define SDL_GL_CONTEXT_PROFILE_ES            0x0004
#define SDL_GL_CONTEXT_DEBUG_FLAG            0x0001

typedef struct SDL_DisplayMode {
    Uint32 format;
    int w, h, refresh_rate;
    void *driverdata;
} SDL_DisplayMode;

typedef struct SDL_SysWMinfo {
    SDL_version version;
    int subsystem;
    union {
        struct { void *display; void *window; } x11;
        struct { void *display; void *surface; } wl;
        struct { void *hinstance; void *window; } win;
    } info;
} SDL_SysWMinfo;

#define SDL_SYSWM_UNKNOWN  0
#define SDL_SYSWM_WINDOWS 1
#define SDL_SYSWM_X11     2
#define SDL_SYSWM_WAYLAND 3
#define SDL_SYSWM_JAVA    4

/* ---- audio ------------------------------------------------------------ */
typedef Uint16 SDL_AudioFormat;
#define AUDIO_U8     0x0008
#define AUDIO_S8     0x8008
#define AUDIO_U16LSB 0x0010
#define AUDIO_S16LSB 0x8010
#define AUDIO_S32LSB 0x8020
#define AUDIO_F32LSB 0x8120
#define AUDIO_S16SYS AUDIO_S16LSB

/* The engine requests exactly one format; the shim converts if it asks for
 * something else, so the struct only has to be compatible. */
#define SDL_AUDIO_ALLOW_FREQUENCY_CHANGE 0x00000001
#define SDL_AUDIO_ALLOW_FORMAT_CHANGE    0x00000002
#define SDL_AUDIO_ALLOW_CHANNELS_CHANGE  0x00000004
#define SDL_AUDIO_ALLOW_SAMPLES_CHANGE   0x00000008
#define SDL_AUDIO_ALLOW_ANY_CHANGE (SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | \
                                    SDL_AUDIO_ALLOW_FORMAT_CHANGE | \
                                    SDL_AUDIO_ALLOW_CHANNELS_CHANGE | \
                                    SDL_AUDIO_ALLOW_SAMPLES_CHANGE)

typedef struct SDL_AudioSpec {
    int freq;
    SDL_AudioFormat format;
    Uint8 channels;
    Uint8 silence;
    Uint16 samples;
    Uint16 padding;
    Uint32 size;
    void (*callback)(void *userdata, Uint8 *stream, int len);
    void *userdata;
} SDL_AudioSpec;

typedef Uint32 SDL_AudioDeviceID;

/* ---- misc ------------------------------------------------------------- */
typedef struct { int value; } SDL_atomic_t;

/* ---- function surface ------------------------------------------------- */
int SDL_Init(Uint32 flags);
int SDL_InitSubSystem(Uint32 flags);
void SDL_Quit(void);
void SDL_QuitSubSystem(Uint32 flags);
Uint32 SDL_WasInit(Uint32 flags);
void SDL_SetMainReady(void);
const char *SDL_GetError(void);
void SDL_ClearError(void);
int SDL_SetHintWithPriority(const char *name, const char *value, SDL_HintPriority prio);
int SDL_SetHint(const char *name, const char *value);
const char *SDL_GetHint(const char *name);
char *SDL_GetBasePath(void);
char *SDL_GetPrefPath(const char *org, const char *app);
void SDL_free(void *mem);
void *SDL_malloc(size_t n);
void *SDL_calloc(size_t n, size_t s);
void *SDL_realloc(void *p, size_t n);

Uint32 SDL_GetTicks(void);
Uint64 SDL_GetTicks64(void);
Uint64 SDL_GetPerformanceCounter(void);
Uint64 SDL_GetPerformanceFrequency(void);
void SDL_Delay(Uint32 ms);

SDL_Thread *SDL_CreateThread(int (*fn)(void *), const char *name, void *data);
void SDL_WaitThread(SDL_Thread *thread, int *status);
SDL_mutex *SDL_CreateMutex(void);
void SDL_DestroyMutex(SDL_mutex *m);
int SDL_LockMutex(SDL_mutex *m);
int SDL_UnlockMutex(SDL_mutex *m);
SDL_cond *SDL_CreateCond(void);
void SDL_DestroyCond(SDL_cond *c);
int SDL_CondWait(SDL_cond *c, SDL_mutex *m);
int SDL_CondWaitTimeout(SDL_cond *c, SDL_mutex *m, Uint32 ms);
int SDL_CondSignal(SDL_cond *c);
int SDL_CondBroadcast(SDL_cond *c);

int SDL_AtomicGet(SDL_atomic_t *a);
int SDL_AtomicSet(SDL_atomic_t *a, int v);
int SDL_AtomicAdd(SDL_atomic_t *a, int v);
SDL_bool SDL_AtomicCAS(SDL_atomic_t *a, int oldval, int newval);
void SDL_MemoryBarrierAcquire(void);
void SDL_MemoryBarrierRelease(void);

#define SDL_WINDOWPOS_UNDEFINED_MASK 0x1FFF0000u
#define SDL_WINDOWPOS_UNDEFINED_DISPLAY(X) (SDL_WINDOWPOS_UNDEFINED_MASK | (X))
#define SDL_WINDOWPOS_UNDEFINED SDL_WINDOWPOS_UNDEFINED_DISPLAY(0)
#define SDL_WINDOWPOS_ISUNDEFINED(X) (((X) & 0xFFFF0000u) == SDL_WINDOWPOS_UNDEFINED_MASK)
#define SDL_WINDOWPOS_CENTERED_MASK 0x2FFF0000u
#define SDL_WINDOWPOS_CENTERED_DISPLAY(X) (SDL_WINDOWPOS_CENTERED_MASK | (X))
#define SDL_WINDOWPOS_CENTERED SDL_WINDOWPOS_CENTERED_DISPLAY(0)

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags);
void SDL_DestroyWindow(SDL_Window *w);
void SDL_ShowWindow(SDL_Window *w);
void SDL_HideWindow(SDL_Window *w);
void SDL_RaiseWindow(SDL_Window *w);
void SDL_RestoreWindow(SDL_Window *w);
void SDL_MinimizeWindow(SDL_Window *w);
void SDL_GetWindowSize(SDL_Window *w, int *wout, int *hout);
void SDL_GetWindowSizeInPixels(SDL_Window *w, int *wout, int *hout);
Uint32 SDL_GetWindowFlags(SDL_Window *w);
int SDL_GetWindowDisplayIndex(SDL_Window *w);
int SDL_GetCurrentDisplayMode(int displayIndex, SDL_DisplayMode *mode);
int SDL_GetDesktopDisplayMode(int displayIndex, SDL_DisplayMode *mode);
int SDL_GetWindowWMInfo(SDL_Window *w, SDL_SysWMinfo *info);

int SDL_GL_SetAttribute(int attr, int value);
int SDL_GL_GetAttribute(int attr, int *value);
SDL_GLContext SDL_GL_CreateContext(SDL_Window *w);
void SDL_GL_DeleteContext(SDL_GLContext ctx);
SDL_GLContext SDL_GL_GetCurrentContext(void);
SDL_Window *SDL_GL_GetCurrentWindow(void);
void *SDL_GL_GetProcAddress(const char *proc);
int SDL_GL_GetDrawableSize(SDL_Window *w, int *wout, int *hout);
int SDL_GL_MakeCurrent(SDL_Window *w, SDL_GLContext ctx);
void SDL_GL_SwapWindow(SDL_Window *w);
int SDL_GL_SetSwapInterval(int interval);
int SDL_GL_GetSwapInterval(void);

/* Metal entry points exist in the shared backend sources but are never called
 * on Orbis; they are here so the TUs link. */
void *SDL_Metal_CreateView(SDL_Window *w);
void SDL_Metal_DestroyView(void *view);
void *SDL_Metal_GetLayer(void *view);
int SDL_Metal_GetDrawableSize(SDL_Window *w, int *wout, int *hout);

int SDL_PollEvent(SDL_Event *e);
int SDL_WaitEventTimeout(SDL_Event *e, int timeout);
void SDL_PumpEvents(void);
int SDL_PushEvent(SDL_Event *e);
const Uint8 *SDL_GetKeyboardState(int *numkeys);

int SDL_NumJoysticks(void);
SDL_bool SDL_IsGameController(int device_index);
SDL_GameController *SDL_GameControllerOpen(int device_index);
void SDL_GameControllerClose(SDL_GameController *gc);
SDL_bool SDL_GameControllerGetAttached(SDL_GameController *gc);
SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *gc);
const char *SDL_GameControllerName(SDL_GameController *gc);
SDL_bool SDL_GameControllerHasRumble(SDL_GameController *gc);
int SDL_GameControllerRumble(SDL_GameController *gc, Uint16 low, Uint16 high, Uint32 ms);
int SDL_GameControllerAddMappingsFromFile(const char *file);
SDL_Joystick *SDL_JoystickFromInstanceID(SDL_JoystickID id);
SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick *joy);
SDL_JoystickID SDL_JoystickGetDeviceInstanceID(int device_index);
SDL_JoystickGUID SDL_JoystickGetGUID(SDL_Joystick *joy);
void SDL_JoystickGetGUIDString(SDL_JoystickGUID guid, char *out, int cb);
int SDL_JoystickAttachVirtual(int type, int naxes, int nbuttons, int nhats);
int SDL_JoystickDetachVirtual(int device_index);
SDL_bool SDL_JoystickIsVirtual(int device_index);
int SDL_JoystickSetVirtualAxis(SDL_Joystick *joy, int axis, Sint16 value);
int SDL_JoystickSetVirtualButton(SDL_Joystick *joy, int button, Uint8 value);
int SDL_JoystickSetVirtualHat(SDL_Joystick *joy, int hat, Uint8 value);

SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int iscapture,
                                      const SDL_AudioSpec *desired,
                                      SDL_AudioSpec *obtained, int allowed_changes);
void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on);
void SDL_CloseAudioDevice(SDL_AudioDeviceID dev);
void SDL_LockAudioDevice(SDL_AudioDeviceID dev);
void SDL_UnlockAudioDevice(SDL_AudioDeviceID dev);
Uint32 SDL_GetQueuedAudioSize(SDL_AudioDeviceID dev);
void SDL_ClearQueuedAudio(SDL_AudioDeviceID dev);

/* Orbis-side hooks implemented in sdl_ps4.c, called by the platform glue. */
int  ps4_sdl_backend_init(int width, int height);
void ps4_sdl_backend_shutdown(void);
void ps4_sdl_backend_pump_pad(void);

/* ---- constants the platform layer references but never acts on here ------
 * They exist so its policy code compiles unchanged; Orbis has no window server,
 * no Metal and no alternate audio driver to select.
 */
#define SDL_MAIN_HANDLED 1
#define SDL_AUDIODRIVER                       "orbis"
#define SDL_HINT_AUDIODRIVER                  "SDL_AUDIODRIVER"
#define SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS"
#define SDL_HINT_MAC_BACKGROUND_APP           "SDL_MAC_BACKGROUND_APP"
#define SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN "SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN"
#define SDL_VIDEO_DRIVER_X11                  "x11"
#define SDL_VIDEO_DRIVER_WAYLAND              "wayland"
#define SDL_VIDEO_DRIVER_WINDOWS              "windows"
#define SDL_SYSWM_COCOA                       5
#define SDL_WINDOW_FULLSCREEN_DESKTOP         (SDL_WINDOW_FULLSCREEN | 0x00001000)
#define SDL_WINDOW_METAL                      0x20000000
#define SDL_WINDOWEVENT_DISPLAY_CHANGED       15
typedef void *SDL_MetalView;
/* Only referenced behind dead branches on this platform; declared so the
 * surrounding expressions type-check. */
#define SDL_PrivateJoystickShouldIgnoreEvent(ev) 0

#ifdef __cplusplus
}
#endif

#endif /* SDL_PS4_SHIM_H */
