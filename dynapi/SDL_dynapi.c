#include <SDL2/SDL_thread.h>
#include <SDL2/SDL_video.h>
#define SDL_DYNAMIC_API_ENVVAR "SDL_DYNAMIC_API"
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <fenv.h>
#include <unistd.h>
#include <SDL2/SDL_vulkan.h>
#define SDL_DYNAPI_VERSION 1
#include <dlfcn.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static int sdl_debug_enable = 0;
#include <pthread.h>
#define LOG_FPRINTF(f,s,...) do{if(sdl_debug_enable)fprintf(f,"[%p] "s,pthread_self(),__VA_ARGS__);}while(0)
#define DEBUGLOG(a)          do{if(sdl_debug_enable)fprintf(stderr,"[%p] %s\n",pthread_self(),(a));}while(0)

#include "SDL_dynapi.h"

#define SDL_DYNAPI_PROC(rc, fn, params, args, ret) \
typedef rc (SDLCALL *SDL_DYNAPIFN_##fn) params;\
static rc SDLCALL fn##_DEFAULT params;         \
extern rc SDLCALL fn##_REAL params;
#include "SDL_dynapi_procs.h"
#undef SDL_DYNAPI_PROC

typedef struct {
    #define SDL_DYNAPI_PROC(rc, fn, params, args, ret) SDL_DYNAPIFN_##fn fn;
    #include "SDL_dynapi_procs.h"
    #undef SDL_DYNAPI_PROC
} SDL_DYNAPI_jump_table;

static SDL_DYNAPI_jump_table jump_table, real_jump_table;

static __thread SDL_Window *spoof_window;

typedef struct {
    SDL_Window    *window;
    SDL_GLContext  context;
    int            initialized;
} GLPrimaryState;

static GLPrimaryState  gl_primary      = { NULL, NULL, 0 };
static SDL_mutex      *gl_primary_lock = NULL;

static void gl_lock(void)   { jump_table.SDL_LockMutex(gl_primary_lock);   }
static void gl_unlock(void) { jump_table.SDL_UnlockMutex(gl_primary_lock); }

/*
 * EGL enforces one surface per thread. We pre-create 24 hidden windows,
 * each with its own EGLSurface. mySDL_GL_MakeCurrent tries each one until
 * one succeeds, sidestepping EGL_BAD_ACCESS when the main window's surface
 * is owned by another thread.
 */
#define BUFFER_WINDOW_COUNT 24
typedef struct {
    SDL_Window *window;
    int         occupied;
} EGLSeat;

static EGLSeat            egl_seats[BUFFER_WINDOW_COUNT];
static SDL_mutex         *egl_seat_lock = NULL;
static __thread EGLSeat  *current_seat  = NULL;

static EGLSeat *seat_acquire(void)
{
    if (current_seat) return current_seat;
    jump_table.SDL_LockMutex(egl_seat_lock);
    for (int i = 0; i < BUFFER_WINDOW_COUNT; i++) {
        if (!egl_seats[i].occupied) {
            egl_seats[i].occupied = 1;
            current_seat = &egl_seats[i];
            break;
        }
    }
    jump_table.SDL_UnlockMutex(egl_seat_lock);
    return current_seat;
}

static int (*realSDL_GL_MakeCurrent)(SDL_Window *, SDL_GLContext) = NULL;

static int mySDL_GL_MakeCurrent(SDL_Window *window, SDL_GLContext context)
{
    spoof_window = window;
    EGLSeat *seat = seat_acquire();
    if (seat && realSDL_GL_MakeCurrent(seat->window, context) == 0)
        return 0;
    return realSDL_GL_MakeCurrent(window, context);
}

static SDL_Window *(*realSDL_GL_GetCurrentWindow)(void) = NULL;
static SDL_Window *mySDL_GL_GetCurrentWindow(void) { return spoof_window; }

static void (*realSDL_GL_SwapWindow)(SDL_Window *) = NULL;
static void mySDL_GL_SwapWindow(SDL_Window *window)
{
    SDL_GLContext ctx = jump_table.SDL_GL_GetCurrentContext();
    realSDL_GL_MakeCurrent(spoof_window, ctx);
    realSDL_GL_SwapWindow(spoof_window);
}

static int (*realSDL_Init)(Uint32) = NULL;
static int mySDL_Init(Uint32 flags)
{
    static int alreadydone = 0;
    if (!alreadydone) {
        alreadydone = 1;
        jump_table.SDL_GameControllerAddMappingsFromRW(
            jump_table.SDL_RWFromFile("gamecontrollerdb.txt", "rb"), 1);
    }
    return realSDL_Init(flags);
}

static SDL_Window *(*real_SDL_CreateWindow)(const char *, int, int, int, int, Uint32) = NULL;

static SDL_Window *mySDL_CreateWindow(const char *title,
                                      int x, int y, int w, int h,
                                      Uint32 flags)
{
    static int pool_created = 0;
    if (!pool_created) {
        pool_created = 1;
        for (int i = 0; i < BUFFER_WINDOW_COUNT; i++) {
            egl_seats[i].window   = real_SDL_CreateWindow(
                "Metro EGL workaround",
                SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                5, 5,
                SDL_WINDOW_HIDDEN | SDL_WINDOW_OPENGL);
            egl_seats[i].occupied = 0;
        }
    }
    return real_SDL_CreateWindow(title, x, y, w, h, flags);
}

static SDL_GLContext (*realSDL_GL_CreateContext)(SDL_Window *) = NULL;

static SDL_GLContext mySDL_GL_CreateContext(SDL_Window *w)
{
    gl_lock();
    int already_have_ctx = gl_primary.initialized;
    gl_unlock();

    int forced_share = 0;
    if (already_have_ctx && jump_table.SDL_GL_GetCurrentContext() != NULL) {
        jump_table.SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
        forced_share = 1;
    }

    SDL_GLContext ctx = realSDL_GL_CreateContext(w);

    if (forced_share)
        jump_table.SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);

    if (ctx) {
        gl_lock();
        if (!gl_primary.initialized) {
            gl_primary.window      = w;
            gl_primary.context     = ctx;
            gl_primary.initialized = 1;
            LOG_FPRINTF(stderr, "[sdl-hook] primary GL context recorded (anchor): win=%p ctx=%p\n", (void *)w, ctx);
        } else {
            gl_primary.window  = w;
            gl_primary.context = ctx;
            LOG_FPRINTF(stderr, "[sdl-hook] additional GL context forced into share group: win=%p ctx=%p\n", (void *)w, ctx);
        }
        gl_unlock();
    }
    return ctx;
}

static void (*realSDL_GL_DeleteContext)(SDL_GLContext) = NULL;

static void mySDL_GL_DeleteContext(SDL_GLContext ctx)
{
    gl_lock();
    if (ctx == gl_primary.context) {
        gl_primary.window      = NULL;
        gl_primary.context     = NULL;
        gl_primary.initialized = 0;
        DEBUGLOG("[sdl-hook] anchor GL context deleted — state cleared");
    }
    gl_unlock();
    realSDL_GL_DeleteContext(ctx);
}

typedef struct {
    int          (SDLCALL *real_fn)(void *);
    void          *real_userdata;
    SDL_Window    *window;
    SDL_GLContext  shared_ctx;
} WrappedThreadData;

static int SDLCALL wrapped_thread_fn(void *arg)
{
    WrappedThreadData *d = (WrappedThreadData *)arg;
    int          (SDLCALL *real_fn)(void *) = d->real_fn;
    void          *real_userdata            = d->real_userdata;
    SDL_Window    *window                   = d->window;
    SDL_GLContext  shared_ctx               = d->shared_ctx;
    jump_table.SDL_free(d);

    if (jump_table.SDL_GL_MakeCurrent(window, shared_ctx) != 0) {
        LOG_FPRINTF(stderr, "[sdl-hook] SDL_GL_MakeCurrent failed on thread: %s\n",
                    jump_table.SDL_GetError());
    } else {
        LOG_FPRINTF(stderr, "[sdl-hook] shared GL context current on thread: win=%p ctx=%p\n",
                    (void *)window, shared_ctx);
    }

    int rc = real_fn(real_userdata);

    jump_table.SDL_GL_MakeCurrent(window, NULL);
    realSDL_GL_DeleteContext(shared_ctx);
    return rc;
}

static SDL_Thread *(*realSDL_CreateThread)(int (SDLCALL *)(void *),
                                           const char *, void *) = NULL;

static SDL_Thread *mySDL_CreateThread(int (SDLCALL *fn)(void *),
                                        const char *name, void *data)
{
    gl_lock();
    SDL_Window    *win  = gl_primary.window;
    SDL_GLContext  pctx = gl_primary.context;
    int            init = gl_primary.initialized;
    gl_unlock();

    if (!init || !win || !pctx) {
        LOG_FPRINTF(stderr, "[sdl-hook] SDL_CreateThread(\"%s\") before GL context — passing through\n",
                    name ? name : "?");
        return realSDL_CreateThread(fn, name, data);
    }

    SDL_GLContext caller_ctx = jump_table.SDL_GL_GetCurrentContext();
    SDL_Window   *caller_win = jump_table.SDL_GL_GetCurrentWindow();

    if (!caller_ctx) {
        LOG_FPRINTF(stderr, "[sdl-hook] SDL_CreateThread(\"%s\"): calling thread has no current GL context — passing through\n",
                    name ? name : "?");
        return realSDL_CreateThread(fn, name, data);
    }

    jump_table.SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    SDL_GLContext shared_ctx = realSDL_GL_CreateContext(win);
    jump_table.SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);

    if (!shared_ctx) {
        LOG_FPRINTF(stderr, "[sdl-hook] SDL_CreateThread(\"%s\"): shared context creation failed (%s) — passing through\n",
                    name ? name : "?", jump_table.SDL_GetError());
        jump_table.SDL_GL_MakeCurrent(caller_win, caller_ctx);
        return realSDL_CreateThread(fn, name, data);
    }

    jump_table.SDL_GL_MakeCurrent(caller_win, caller_ctx);

    WrappedThreadData *wd = jump_table.SDL_malloc(sizeof *wd);
    if (!wd) {
        DEBUGLOG("[sdl-hook] OOM in SDL_CreateThread hook");
        realSDL_GL_DeleteContext(shared_ctx);
        return realSDL_CreateThread(fn, name, data);
    }

    wd->real_fn       = fn;
    wd->real_userdata = data;
    wd->window        = win;
    wd->shared_ctx    = shared_ctx;

    LOG_FPRINTF(stderr, "[sdl-hook] SDL_CreateThread(\"%s\"): injecting shared GL ctx=%p (share group of caller ctx=%p)\n",
                name ? name : "?", shared_ctx, caller_ctx);

    return realSDL_CreateThread(wrapped_thread_fn, name, wd);
}

/* Joystick → Xbox 360 GameController wrapper
*
* The game uses raw SDL_Joystick API with assumed Xbox 360 layout indices.
* We intercept every joystick call and remap through SDL_GameController
* so any supported pad presents as a fixed Xbox 360 layout.*/
#define MAX_JOYSTICKS 8

typedef struct {
    SDL_Joystick       *joy;
    SDL_GameController *gc;
    int                 device_index;
} JoyEntry;

static JoyEntry   joy_table[MAX_JOYSTICKS];
static SDL_mutex *joy_lock = NULL;

static void joy_lock_acquire(void) { jump_table.SDL_LockMutex(joy_lock);   }
static void joy_lock_release(void) { jump_table.SDL_UnlockMutex(joy_lock); }

static JoyEntry *joy_find(SDL_Joystick *joy)
{
    for (int i = 0; i < MAX_JOYSTICKS; i++)
        if (joy_table[i].joy == joy) return &joy_table[i];
        return NULL;
}

static int (*real_SDL_NumJoysticks)(void) = NULL;
static int my_SDL_NumJoysticks(void)
{
    int total = real_SDL_NumJoysticks();
    int count = 0;
    for (int i = 0; i < total; i++)
        if (jump_table.SDL_IsGameController(i)) count++;
        return count;
}

static SDL_Joystick *my_SDL_JoystickOpen(int device_index)
{
    if (!jump_table.SDL_IsGameController(device_index)) return NULL;

    joy_lock_acquire();
    for (int i = 0; i < MAX_JOYSTICKS; i++) {
        if (!joy_table[i].joy) {
            SDL_GameController *gc = jump_table.SDL_GameControllerOpen(device_index);
            if (!gc) { joy_lock_release(); return NULL; }
            SDL_Joystick *real_joy = jump_table.SDL_GameControllerGetJoystick(gc);
            joy_table[i].joy          = real_joy;
            joy_table[i].gc           = gc;
            joy_table[i].device_index = device_index;
            LOG_FPRINTF(stderr, "[sdl-hook] JoystickOpen(%d): gc=%p joy=%p\n", device_index, (void *)gc, (void *)real_joy);
            joy_lock_release();
            return real_joy;
        }
    }
    joy_lock_release();
    return NULL;
}

static void my_SDL_JoystickClose(SDL_Joystick *joy)
{
    joy_lock_acquire();
    JoyEntry *e = joy_find(joy);
    if (e) {
        if (e->gc) jump_table.SDL_GameControllerClose(e->gc);
        e->joy          = NULL;
        e->gc           = NULL;
        e->device_index = -1;
    }
    joy_lock_release();
}

static SDL_bool my_SDL_JoystickGetAttached(SDL_Joystick *joy)
{
    joy_lock_acquire();
    JoyEntry *e = joy_find(joy);
    SDL_bool result = (e && e->gc) ? SDL_TRUE : SDL_FALSE;
    joy_lock_release();
    return result;
}

static void my_SDL_JoystickUpdate(void)
{
    jump_table.SDL_GameControllerUpdate();
}

static const char *my_SDL_JoystickName(SDL_Joystick *joy)
{
    joy_lock_acquire();
    JoyEntry *e = joy_find(joy);
    int has_gc = e && e->gc;
    joy_lock_release();
    return has_gc ? "Xbox 360 Controller" : NULL;
}

static int my_SDL_JoystickNumAxes(SDL_Joystick *joy)
{
    joy_lock_acquire();
    JoyEntry *e = joy_find(joy);
    int has_gc = e && e->gc;
    joy_lock_release();
    return has_gc ? 6 : 0;
}

static int my_SDL_JoystickNumButtons(SDL_Joystick *joy)
{
    joy_lock_acquire();
    JoyEntry *e = joy_find(joy);
    int has_gc = e && e->gc;
    joy_lock_release();
    return has_gc ? 11 : 0;
}

static int my_SDL_JoystickNumHats(SDL_Joystick *joy)
{
    joy_lock_acquire();
    JoyEntry *e = joy_find(joy);
    int has_gc = e && e->gc;
    joy_lock_release();
    return has_gc ? 1 : 0;
}

static Sint16 my_SDL_JoystickGetAxis(SDL_Joystick *joy, int axis)
{
    joy_lock_acquire();
    JoyEntry *e = joy_find(joy);
    SDL_GameController *gc = e ? e->gc : NULL;
    joy_lock_release();

    if (!gc) return 0;

    static const SDL_GameControllerAxis axis_map[6] = {
        SDL_CONTROLLER_AXIS_LEFTX,        /* 0 */
        SDL_CONTROLLER_AXIS_LEFTY,        /* 1 */
        SDL_CONTROLLER_AXIS_TRIGGERLEFT,  /* 2 */
        SDL_CONTROLLER_AXIS_RIGHTX,       /* 3 */
        SDL_CONTROLLER_AXIS_RIGHTY,       /* 4 */
        SDL_CONTROLLER_AXIS_TRIGGERRIGHT, /* 5 */
    };
    if (axis < 0 || axis >= 6) return 0;

    Sint16 val = jump_table.SDL_GameControllerGetAxis(gc, axis_map[axis]);

    /* Triggers: SDL GameController returns 0..32767 (0=rest).
     *      Game expects raw Xbox range -32768..32767 (-32768=rest). */
    if (axis == 2 || axis == 5) {
        int remapped = (int)val * 2 - 32767;
        if (remapped < -32768) remapped = -32768;
        return (Sint16)remapped;
    }

    return val;
}

static Uint8 my_SDL_JoystickGetButton(SDL_Joystick *joy, int button)
{
    joy_lock_acquire();
    JoyEntry *e = joy_find(joy);
    SDL_GameController *gc = e ? e->gc : NULL;
    joy_lock_release();

    if (!gc) return 0;

    static const SDL_GameControllerButton btn_map[11] = {
        SDL_CONTROLLER_BUTTON_A,
        SDL_CONTROLLER_BUTTON_B,
        SDL_CONTROLLER_BUTTON_X,
        SDL_CONTROLLER_BUTTON_Y,
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
        SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        SDL_CONTROLLER_BUTTON_BACK,
        SDL_CONTROLLER_BUTTON_START,
        SDL_CONTROLLER_BUTTON_GUIDE,
        SDL_CONTROLLER_BUTTON_LEFTSTICK,
        SDL_CONTROLLER_BUTTON_RIGHTSTICK,
    };
    if (button < 0 || button >= 11) return 0;
    return jump_table.SDL_GameControllerGetButton(gc, btn_map[button]);
}

static Uint8 my_SDL_JoystickGetHat(SDL_Joystick *joy, int hat)
{
    joy_lock_acquire();
    JoyEntry *e = joy_find(joy);
    SDL_GameController *gc = e ? e->gc : NULL;
    joy_lock_release();

    if (!gc) return SDL_HAT_CENTERED;
    if (hat != 0) return SDL_HAT_CENTERED;

    Uint8 val = SDL_HAT_CENTERED;
    if (jump_table.SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP))
        val |= SDL_HAT_UP;
    if (jump_table.SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN))
        val |= SDL_HAT_DOWN;
    if (jump_table.SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT))
        val |= SDL_HAT_LEFT;
    if (jump_table.SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))
        val |= SDL_HAT_RIGHT;
    return val;
}
/*
* SDL_JOYDEVICEADDED fires when a new joystick is plugged in.
* The game has already opened its joystick list at startup so it may not
* handle hotplug itself. We open the new device into our table so that
* if the game calls JoystickOpen for it later, the gc is ready.
* SDL_JOYDEVICEREMOVED closes any gc we have open for that instance id.
*
* We also suppress SDL_CONTROLLERDEVICE* events — the game only speaks
* SDL_JOYSTICK* and duplicate device events confuse some engines.
*/
static int (*real_SDL_PollEvent)(SDL_Event *) = NULL;

static int my_SDL_PollEvent(SDL_Event *event)
{
    for (;;) {
        int ret = real_SDL_PollEvent(event);
        if (ret == 0) return 0;

        switch (event->type) {
            case SDL_JOYDEVICEADDED: {
                int idx = event->jdevice.which;
                if (!jump_table.SDL_IsGameController(idx)) continue;
                LOG_FPRINTF(stderr, "[sdl-hook] hotplug ADDED: idx=%d\n", idx);
                break; /* game will call JoystickOpen itself */
            }

            case SDL_JOYDEVICEREMOVED: {
                SDL_JoystickID iid = event->jdevice.which;
                joy_lock_acquire();
                for (int i = 0; i < MAX_JOYSTICKS; i++) {
                    if (joy_table[i].joy &&
                        jump_table.SDL_JoystickInstanceID(joy_table[i].joy) == iid) {
                        LOG_FPRINTF(stderr, "[sdl-hook] hotplug REMOVED: iid=%d gc=%p\n", iid, (void *)joy_table[i].gc);
                    if (joy_table[i].gc) {
                        jump_table.SDL_GameControllerClose(joy_table[i].gc);
                        joy_table[i].gc = NULL;
                    }
                    /* leave joy entry — game will call JoystickClose */
                    break;
                    }
                }
                joy_lock_release();
                break;
            }
            /* Suppress controller events — game uses joystick API only */
            case SDL_JOYAXISMOTION:
            case SDL_JOYBALLMOTION:
            case SDL_JOYHATMOTION:
            case SDL_JOYBUTTONDOWN:
            case SDL_JOYBUTTONUP:
            case SDL_CONTROLLERDEVICEADDED:
            case SDL_CONTROLLERDEVICEREMOVED:
            case SDL_CONTROLLERDEVICEREMAPPED:
            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP:
            case SDL_CONTROLLERAXISMOTION:
                continue;

            default:
                break;
        }

        return ret;
    }
}

static Uint32 (*real_SDL_GetRelativeMouseState)(int *, int *) = NULL;
static Uint32 my_SDL_GetRelativeMouseState(int *x, int *y, int *z)
{
    Uint32 buttons = real_SDL_GetRelativeMouseState(x, y);
    if (z) *z = 0; //WTF WAS 4A THINKING?????
    return buttons;
}

#define SDL_DYNAPI_PROC(rc, fn, params, args, ret) \
static rc wrapper_##fn params { \
    if (real_jump_table.fn == NULL) { \
        fprintf(stderr, "Symbol %s not found.\n", "" #fn ""); \
        exit(1); \
    } \
    if (sdl_debug_enable) { \
        fprintf(stderr, "[%p] %s\n", pthread_self(), "" #fn ""); \
    } \
    ret real_jump_table.fn args; \
}
#include "SDL_dynapi_procs.h"
#undef SDL_DYNAPI_PROC

static Sint32 initialize_jumptable(Uint32 apiver, void *table, Uint32 tablesize)
{
    SDL_DYNAPI_jump_table *output_jump_table = (SDL_DYNAPI_jump_table *)table;

    if (apiver != SDL_DYNAPI_VERSION)   return -1;
    if (tablesize > sizeof(jump_table)) return -1;

    void *sdlhandle = dlopen("libSDL2-2.0.so.0", RTLD_LOCAL | RTLD_NOW);
    if (!sdlhandle) { fputs("Could not load SDL2.\n", stderr); exit(1); }

    if (getenv("SDL_LOG_CALLS_DEBUG")) sdl_debug_enable = 1;

    #define SDL_DYNAPI_PROC(rc, fn, params, args, ret) \
    do { \
        real_jump_table.fn = dlsym(sdlhandle, #fn); \
        jump_table.fn = (void *)wrapper_##fn; \
    } while(0);
    #include "SDL_dynapi_procs.h"
    #undef SDL_DYNAPI_PROC

    gl_primary_lock = jump_table.SDL_CreateMutex();
    if (!gl_primary_lock) { fputs("[sdl-hook] gl_primary_lock failed\n", stderr); exit(1); }

    egl_seat_lock = jump_table.SDL_CreateMutex();
    if (!egl_seat_lock) { fputs("[sdl-hook] egl_seat_lock failed\n", stderr); exit(1); }

    joy_lock = jump_table.SDL_CreateMutex();
    if (!joy_lock) { fputs("[sdl-hook] joy_lock failed\n", stderr); exit(1); }

    /* SDL_Init */
    realSDL_Init        = jump_table.SDL_Init;
    jump_table.SDL_Init = mySDL_Init;

    /* Window management */
    real_SDL_CreateWindow        = jump_table.SDL_CreateWindow;
    jump_table.SDL_CreateWindow  = mySDL_CreateWindow;

    /* GL context management */
    realSDL_GL_CreateContext        = jump_table.SDL_GL_CreateContext;
    jump_table.SDL_GL_CreateContext = mySDL_GL_CreateContext;

    realSDL_GL_DeleteContext        = jump_table.SDL_GL_DeleteContext;
    jump_table.SDL_GL_DeleteContext = mySDL_GL_DeleteContext;

    /* GL surface / swap */
    realSDL_GL_MakeCurrent        = jump_table.SDL_GL_MakeCurrent;
    jump_table.SDL_GL_MakeCurrent = mySDL_GL_MakeCurrent;

    realSDL_GL_GetCurrentWindow        = jump_table.SDL_GL_GetCurrentWindow;
    jump_table.SDL_GL_GetCurrentWindow = mySDL_GL_GetCurrentWindow;

    realSDL_GL_SwapWindow        = jump_table.SDL_GL_SwapWindow;
    jump_table.SDL_GL_SwapWindow = mySDL_GL_SwapWindow;

    /* Thread creation */
    realSDL_CreateThread        = jump_table.SDL_CreateThread;
    jump_table.SDL_CreateThread = mySDL_CreateThread;

    /* Event pump */
    real_SDL_PollEvent        = jump_table.SDL_PollEvent;
    jump_table.SDL_PollEvent  = my_SDL_PollEvent;

    /* Joystick → GameController remapping */

    real_SDL_NumJoysticks              = jump_table.SDL_NumJoysticks;
    jump_table.SDL_NumJoysticks        = my_SDL_NumJoysticks;
    jump_table.SDL_JoystickOpen        = my_SDL_JoystickOpen;
    jump_table.SDL_JoystickClose       = my_SDL_JoystickClose;
    jump_table.SDL_JoystickGetAttached = my_SDL_JoystickGetAttached;
    jump_table.SDL_JoystickUpdate      = my_SDL_JoystickUpdate;
    jump_table.SDL_JoystickName        = my_SDL_JoystickName;
    jump_table.SDL_JoystickNumAxes     = my_SDL_JoystickNumAxes;
    jump_table.SDL_JoystickNumButtons  = my_SDL_JoystickNumButtons;
    jump_table.SDL_JoystickNumHats     = my_SDL_JoystickNumHats;
    jump_table.SDL_JoystickGetAxis     = my_SDL_JoystickGetAxis;
    jump_table.SDL_JoystickGetButton   = my_SDL_JoystickGetButton;
    jump_table.SDL_JoystickGetHat      = my_SDL_JoystickGetHat;

    real_SDL_GetRelativeMouseState       = jump_table.SDL_GetRelativeMouseState;
    jump_table.SDL_GetRelativeMouseState = (void*)my_SDL_GetRelativeMouseState;
    if (output_jump_table != &jump_table)
        jump_table.SDL_memcpy(output_jump_table, &jump_table, tablesize);

    return 0;
}

typedef Sint32 (SDLCALL *SDL_DYNAPI_ENTRYFN)(Uint32 apiver, void *table, Uint32 tablesize);

Sint32 SDL_DYNAPI_entry(Uint32 apiver, void *table, Uint32 tablesize)
{
    unsetenv(SDL_DYNAMIC_API_ENVVAR);
    return initialize_jumptable(apiver, table, tablesize);
}
