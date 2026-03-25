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
#include <stdint.h>
#include <sys/mman.h>
#include <GL/gl.h>

static int sdl_debug_enable = 0;
#include <pthread.h>
#define LOG_FPRINTF(f,s,...) do{if(sdl_debug_enable)fprintf(f,"[%p] "s,pthread_self(),__VA_ARGS__);}while(0)
#define DEBUGLOG(a)          do{if(sdl_debug_enable)fprintf(stderr,"[%p] %s\n",pthread_self(),(a));}while(0)

#include "SDL_dynapi.h"

#define SDL_DYNAPI_PROC(rc, fn, params, args, ret) \
typedef rc (SDLCALL *SDL_DYNAPIFN_##fn) params;
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
 * each with its own EGLSurface. seat_acquire() gives each thread a
 * dedicated surface, sidestepping EGL_BAD_ACCESS when the main window's
 * surface is owned by another thread.
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

/* ── GL surface / swap ── */

static int (*real_SDL_GL_MakeCurrent)(SDL_Window *, SDL_GLContext) = NULL;

static int my_SDL_GL_MakeCurrent(SDL_Window *window, SDL_GLContext context)
{
    spoof_window = window;
    EGLSeat *seat = seat_acquire();
    if (seat && real_SDL_GL_MakeCurrent(seat->window, context) == 0)
        return 0;
    return real_SDL_GL_MakeCurrent(window, context);
}

static SDL_Window *(*real_SDL_GL_GetCurrentWindow)(void) = NULL;
static SDL_Window *my_SDL_GL_GetCurrentWindow(void) { return spoof_window; }

/* ── Init ── */

static int (*real_SDL_Init)(Uint32) = NULL;
static int my_SDL_Init(Uint32 flags)
{
    static int alreadydone = 0;
    if (!alreadydone) {
        alreadydone = 1;
        jump_table.SDL_GameControllerAddMappingsFromRW(
            jump_table.SDL_RWFromFile("gamecontrollerdb.txt", "rb"), 1);
        jump_table.SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    }
    return real_SDL_Init(flags);
}

/* ── Resolution override ── */

static int override_w = 0;
static int override_h = 0;

static int (*real_SDL_GetDisplayBounds)(int, SDL_Rect *) = NULL;

static int my_SDL_GetDisplayBounds(int displayIndex, SDL_Rect *rect)
{
    int ret = real_SDL_GetDisplayBounds(displayIndex, rect);
    if (ret == 0 && override_w > 0 && override_h > 0) {
        rect->w = override_w;
        rect->h = override_h;
    }
    return ret;
}

/* ── Window management ── */

static SDL_Window *(*real_SDL_CreateWindow)(const char *, int, int, int, int, Uint32) = NULL;
static int is_fullscreen;
static SDL_Window *my_SDL_CreateWindow(const char *title,
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
    if((flags&(SDL_WINDOW_FULLSCREEN|SDL_WINDOW_FULLSCREEN_DESKTOP))!=0){
        is_fullscreen=1;
        flags&=~SDL_WINDOW_FULLSCREEN;
        flags|=SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
    return real_SDL_CreateWindow(title, x, y, w, h, flags);
}
static void (*real_SDL_GetWindowSize)(SDL_Window * window, int *w,int *h);
static void my_SDL_GetWindowSize(SDL_Window * window, int *w,int *h){
    if (override_w > 0 && override_h > 0 && is_fullscreen){
        *w=override_w;
        *h=override_h;
    }
    else{
        real_SDL_GetWindowSize(window,w,h);
    }
}

static void (*real_SDL_SetWindowSize)(SDL_Window * window, int w,int h);
static void my_SDL_SetWindowSize(SDL_Window * window, int w,int h){
    if (!is_fullscreen){
        real_SDL_SetWindowSize(window,w,h);
    }
}

/* ── GL context management ── */

static SDL_GLContext (*real_SDL_GL_CreateContext)(SDL_Window *) = NULL;

static SDL_GLContext my_SDL_GL_CreateContext(SDL_Window *w)
{
    gl_lock();
    int already_have_ctx = gl_primary.initialized;
    gl_unlock();

    int forced_share = 0;
    if (already_have_ctx && jump_table.SDL_GL_GetCurrentContext() != NULL) {
        jump_table.SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
        forced_share = 1;
    }

    SDL_GLContext ctx = real_SDL_GL_CreateContext(w);

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

static void (*real_SDL_GL_DeleteContext)(SDL_GLContext) = NULL;

static void my_SDL_GL_DeleteContext(SDL_GLContext ctx)
{
    gl_lock();
    if (ctx == gl_primary.context) {
        gl_primary.window      = NULL;
        gl_primary.context     = NULL;
        gl_primary.initialized = 0;
        DEBUGLOG("[sdl-hook] anchor GL context deleted — state cleared");
    }
    gl_unlock();
    real_SDL_GL_DeleteContext(ctx);
}

/* ── Thread wrapping ── */

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
    real_SDL_GL_DeleteContext(shared_ctx);
    return rc;
}

static SDL_Thread *(*real_SDL_CreateThread)(int (SDLCALL *)(void *),
                                           const char *, void *) = NULL;

static SDL_Thread *my_SDL_CreateThread(int (SDLCALL *fn)(void *),
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
        return real_SDL_CreateThread(fn, name, data);
    }

    SDL_GLContext caller_ctx = jump_table.SDL_GL_GetCurrentContext();
    SDL_Window   *caller_win = jump_table.SDL_GL_GetCurrentWindow();

    if (!caller_ctx) {
        LOG_FPRINTF(stderr, "[sdl-hook] SDL_CreateThread(\"%s\"): calling thread has no current GL context — passing through\n",
                    name ? name : "?");
        return real_SDL_CreateThread(fn, name, data);
    }

    jump_table.SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    SDL_GLContext shared_ctx = real_SDL_GL_CreateContext(win);
    jump_table.SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);

    if (!shared_ctx) {
        LOG_FPRINTF(stderr, "[sdl-hook] SDL_CreateThread(\"%s\"): shared context creation failed (%s) — passing through\n",
                    name ? name : "?", jump_table.SDL_GetError());
        jump_table.SDL_GL_MakeCurrent(caller_win, caller_ctx);
        return real_SDL_CreateThread(fn, name, data);
    }

    jump_table.SDL_GL_MakeCurrent(caller_win, caller_ctx);

    WrappedThreadData *wd = jump_table.SDL_malloc(sizeof *wd);
    if (!wd) {
        DEBUGLOG("[sdl-hook] OOM in SDL_CreateThread hook");
        real_SDL_GL_DeleteContext(shared_ctx);
        return real_SDL_CreateThread(fn, name, data);
    }

    wd->real_fn       = fn;
    wd->real_userdata = data;
    wd->window        = win;
    wd->shared_ctx    = shared_ctx;

    LOG_FPRINTF(stderr, "[sdl-hook] SDL_CreateThread(\"%s\"): injecting shared GL ctx=%p (share group of caller ctx=%p)\n",
                name ? name : "?", shared_ctx, caller_ctx);

    return real_SDL_CreateThread(wrapped_thread_fn, name, wd);
}

/* ── Joystick → Xbox 360 GameController wrapper ──
*
* The game uses raw SDL_Joystick API with assumed Xbox 360 layout indices.
* We intercept every joystick call and remap through SDL_GameController
* so any supported pad presents as a fixed Xbox 360 layout.
*/

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
            LOG_FPRINTF(stderr, "[sdl-hook] JoystickOpen(%d): gc=%p joy=%p\n",
                        device_index, (void *)gc, (void *)real_joy);
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

/* ── Event pump ── */

static int (*real_SDL_PollEvent)(SDL_Event *) = NULL;

static int    my_xdelta     = 0;
static int    my_ydelta     = 0;
static int    my_zdelta     = 0;
static Uint32 my_buttonstate = 0;

static int my_SDL_PollEvent(SDL_Event *event)
{
    for (;;) {
        int ret = real_SDL_PollEvent(event);
        if (ret == 0) return 0;

        switch (event->type) {
            case SDL_WINDOWEVENT:
                if (override_w > 0 && override_h > 0 && is_fullscreen) {
                    if (event->window.event == SDL_WINDOWEVENT_RESIZED || event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                        event->window.data1 = override_w;
                        event->window.data2 = override_h;
                    }
                }
                break;

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
                        LOG_FPRINTF(stderr, "[sdl-hook] hotplug REMOVED: iid=%d gc=%p\n",
                                    iid, (void *)joy_table[i].gc);
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

            /* suppress raw joystick state events and controller events —
            * game uses joystick API only, we synthesize from gc */
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

                /* accumulate mouse state for GetRelativeMouseState —
                * 4A broke the ABI by adding a z parameter */
                case SDL_MOUSEMOTION:
                    my_xdelta     += event->motion.xrel;
                    my_ydelta     += event->motion.yrel;
                    my_buttonstate = event->motion.state;
                    break;

                case SDL_MOUSEWHEEL:
                    my_zdelta += event->wheel.y;
                    break;

                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEBUTTONUP:
                    my_buttonstate = jump_table.SDL_GetMouseState(NULL, NULL);
                    break;

                default:
                    break;
        }

        return ret;
    }
}

/* SDL_GetRelativeMouseState — 4A extended with z parameter, stock SDL has 2 args.
* We track deltas ourselves in my_SDL_PollEvent to avoid calling stock SDL's
* version which would pump the wrong event queue. */
static Uint32 (*real_SDL_GetRelativeMouseState)(int *, int *) = NULL;

static Uint32 my_SDL_GetRelativeMouseState(int *x, int *y, int *z)
{
    if (x) *x = my_xdelta;
    if (y) *y = my_ydelta;
    if (z) *z = my_zdelta;
    my_xdelta = 0;
    my_ydelta = 0;
    my_zdelta = 0;
    return my_buttonstate;
}

/* ── GL scaling ── */

static int      scaling_context_initialized          = 0;
static int      OpenGLLogicalScalingWidth            = 0;
static int      OpenGLLogicalScalingHeight           = 0;
static GLuint   OpenGLLogicalScalingFBO              = 0;
static GLuint   OpenGLLogicalScalingColor            = 0;
static GLuint   OpenGLLogicalScalingDepth            = 0;
static int      OpenGLLogicalScalingSamples          = 0;
static GLuint   OpenGLLogicalScalingMultisampleFBO   = 0;
static GLuint   OpenGLLogicalScalingMultisampleColor = 0;
static GLuint   OpenGLLogicalScalingMultisampleDepth = 0;
static int      WantScaleMethodNearest               = 0;

static void (*my_glGenFramebuffers)(GLsizei, GLuint *)                                                          = NULL;
static void (*my_glDeleteFramebuffers)(GLsizei, const GLuint *)                                                  = NULL;
static void (*my_glBindFramebuffer)(GLenum, GLuint)                                                              = NULL;
static void (*my_glGenRenderbuffers)(GLsizei, GLuint *)                                                          = NULL;
static void (*my_glDeleteRenderbuffers)(GLsizei, const GLuint *)                                                 = NULL;
static void (*my_glBindRenderbuffer)(GLenum, GLuint)                                                             = NULL;
static void (*my_glRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei)                                        = NULL;
static void (*my_glRenderbufferStorageMultisample)(GLenum, GLsizei, GLenum, GLsizei, GLsizei)                    = NULL;
static void (*my_glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint)                                      = NULL;
static GLenum (*my_glCheckFramebufferStatus)(GLenum)                                                             = NULL;
static void (*my_glBlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)  = NULL;
static void (*my_glViewport)(GLint, GLint, GLsizei, GLsizei)                                                     = NULL;
static void (*my_glScissor)(GLint, GLint, GLsizei, GLsizei)                                                      = NULL;
static void (*my_glClear)(GLbitfield)                                                                             = NULL;
static void (*my_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat)                                              = NULL;
static void (*my_glGetIntegerv)(GLenum, GLint *)                                                                  = NULL;
static void (*my_glGetFloatv)(GLenum, GLfloat *)                                                                 = NULL;
static GLboolean (*my_glIsEnabled)(GLenum)                                                                        = NULL;
static void (*my_glEnable)(GLenum)                                                                               = NULL;
static void (*my_glDisable)(GLenum)                                                                              = NULL;
static GLenum (*my_glGetError)(void)                                                                             = NULL;

static void scaling_load_gl_procs(void)
{
    #define GLPROC(fn) my_##fn = (void *)jump_table.SDL_GL_GetProcAddress(#fn)
    GLPROC(glGenFramebuffers);
    GLPROC(glDeleteFramebuffers);
    GLPROC(glBindFramebuffer);
    GLPROC(glGenRenderbuffers);
    GLPROC(glDeleteRenderbuffers);
    GLPROC(glBindRenderbuffer);
    GLPROC(glRenderbufferStorage);
    GLPROC(glRenderbufferStorageMultisample);
    GLPROC(glFramebufferRenderbuffer);
    GLPROC(glCheckFramebufferStatus);
    GLPROC(glBlitFramebuffer);
    GLPROC(glViewport);
    GLPROC(glScissor);
    GLPROC(glClear);
    GLPROC(glClearColor);
    GLPROC(glGetIntegerv);
    GLPROC(glGetFloatv);
    GLPROC(glIsEnabled);
    GLPROC(glEnable);
    GLPROC(glDisable);
    GLPROC(glGetError);
    #undef GLPROC
}

static SDL_Rect GetOpenGLLogicalScalingViewport(int pw, int ph)
{
    SDL_Rect r;
    float want = (float)OpenGLLogicalScalingWidth  / (float)OpenGLLogicalScalingHeight;
    float real = (float)pw / (float)ph;

    if (jump_table.SDL_fabs(want - real) < 0.0001f) {
        r.x = 0; r.y = 0; r.w = pw; r.h = ph;
    } else if (want > real) {
        float scale = (float)pw / OpenGLLogicalScalingWidth;
        r.x = 0; r.w = pw;
        r.h = (int)jump_table.SDL_floor(OpenGLLogicalScalingHeight * scale);
        r.y = (ph - r.h) / 2;
    } else {
        float scale = (float)ph / OpenGLLogicalScalingHeight;
        r.y = 0; r.h = ph;
        r.w = (int)jump_table.SDL_floor(OpenGLLogicalScalingWidth * scale);
        r.x = (pw - r.w) / 2;
    }
    return r;
}

static int InitializeOpenGLScaling(int logical_w, int logical_h)
{
    if (!my_glGenFramebuffers)
        scaling_load_gl_procs();

    if (OpenGLLogicalScalingFBO &&
        logical_w == OpenGLLogicalScalingWidth &&
        logical_h == OpenGLLogicalScalingHeight &&
        scaling_context_initialized)
        return SDL_TRUE;

    if (OpenGLLogicalScalingFBO) {
        my_glDeleteFramebuffers(1, &OpenGLLogicalScalingFBO);
        my_glDeleteRenderbuffers(1, &OpenGLLogicalScalingColor);
        my_glDeleteRenderbuffers(1, &OpenGLLogicalScalingDepth);
        OpenGLLogicalScalingFBO = OpenGLLogicalScalingColor = OpenGLLogicalScalingDepth = 0;
        if (OpenGLLogicalScalingMultisampleFBO) {
            my_glDeleteFramebuffers(1, &OpenGLLogicalScalingMultisampleFBO);
            my_glDeleteRenderbuffers(1, &OpenGLLogicalScalingMultisampleColor);
            my_glDeleteRenderbuffers(1, &OpenGLLogicalScalingMultisampleDepth);
            OpenGLLogicalScalingMultisampleFBO = OpenGLLogicalScalingMultisampleColor = OpenGLLogicalScalingMultisampleDepth = 0;
        }
    }

    int alpha_size = 0, depth_size = 0, stencil_size = 0;
    jump_table.SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE,   &alpha_size);
    jump_table.SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE,   &depth_size);
    jump_table.SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &stencil_size);

    my_glGenFramebuffers(1, &OpenGLLogicalScalingFBO);
    my_glGenRenderbuffers(1, &OpenGLLogicalScalingColor);
    my_glGenRenderbuffers(1, &OpenGLLogicalScalingDepth);
    my_glBindFramebuffer(GL_FRAMEBUFFER, OpenGLLogicalScalingFBO);

    my_glBindRenderbuffer(GL_RENDERBUFFER, OpenGLLogicalScalingColor);
    if (OpenGLLogicalScalingSamples > 0)
        my_glRenderbufferStorageMultisample(GL_RENDERBUFFER, OpenGLLogicalScalingSamples,
                                            alpha_size > 0 ? GL_RGBA8 : GL_RGB8, logical_w, logical_h);
        else
            my_glRenderbufferStorage(GL_RENDERBUFFER,
                                    alpha_size > 0 ? GL_RGBA8 : GL_RGB8, logical_w, logical_h);
            my_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, OpenGLLogicalScalingColor);

        if (depth_size > 0 || stencil_size > 0) {
            my_glBindRenderbuffer(GL_RENDERBUFFER, OpenGLLogicalScalingDepth);
            if (OpenGLLogicalScalingSamples > 0)
                my_glRenderbufferStorageMultisample(GL_RENDERBUFFER, OpenGLLogicalScalingSamples,
                                                    GL_DEPTH24_STENCIL8, logical_w, logical_h);
                else
                    my_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, logical_w, logical_h);
            if (depth_size > 0)
                my_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,   GL_RENDERBUFFER, OpenGLLogicalScalingDepth);
            if (stencil_size > 0)
                my_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, OpenGLLogicalScalingDepth);
        }
        my_glBindRenderbuffer(GL_RENDERBUFFER, 0);

        if (my_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
            my_glGetError() != GL_NO_ERROR) {
            my_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        my_glDeleteRenderbuffers(1, &OpenGLLogicalScalingColor);
        my_glDeleteRenderbuffers(1, &OpenGLLogicalScalingDepth);
        my_glDeleteFramebuffers(1, &OpenGLLogicalScalingFBO);
        OpenGLLogicalScalingFBO = OpenGLLogicalScalingColor = OpenGLLogicalScalingDepth = 0;
        DEBUGLOG("[sdl-hook] scaling FBO incomplete — scaling disabled");
        return SDL_FALSE;
            }

            my_glViewport(0, 0, logical_w, logical_h);
            my_glScissor(0, 0, logical_w, logical_h);
            my_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            my_glBindFramebuffer(GL_FRAMEBUFFER, 0);

            OpenGLLogicalScalingWidth   = logical_w;
            OpenGLLogicalScalingHeight  = logical_h;
            scaling_context_initialized = 1;
            LOG_FPRINTF(stderr, "[sdl-hook] scaling FBO initialized: %dx%d\n", logical_w, logical_h);
            return SDL_TRUE;
}

static void (*real_SDL_GL_SwapWindow)(SDL_Window *) = NULL;

static void my_SDL_GL_SwapWindow(SDL_Window *window)
{
    SDL_GLContext ctx = jump_table.SDL_GL_GetCurrentContext();

    if (override_w > 0 && override_h > 0 && is_fullscreen) {
        if (!scaling_context_initialized)
            InitializeOpenGLScaling(override_w, override_h);

        if (scaling_context_initialized && OpenGLLogicalScalingFBO) {
            int physical_w = 0, physical_h = 0;
            jump_table.SDL_GL_GetDrawableSize(spoof_window, &physical_w, &physical_h);

            SDL_Rect dstrect = GetOpenGLLogicalScalingViewport(physical_w, physical_h);
            GLenum filter = WantScaleMethodNearest ? GL_NEAREST : GL_LINEAR;

            GLboolean has_scissor = my_glIsEnabled(GL_SCISSOR_TEST);
            if (has_scissor) my_glDisable(GL_SCISSOR_TEST);

            GLfloat cc[4];
            my_glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);

            /* game rendered to FBO 0 at logical res — copy to scaling FBO */
            my_glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            my_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, OpenGLLogicalScalingFBO);
            my_glBlitFramebuffer(
                0, 0, override_w, override_h,
                0, 0, override_w, override_h,
                GL_COLOR_BUFFER_BIT, GL_NEAREST);

            /* blit scaling FBO to screen with aspect ratio correction */
            my_glBindFramebuffer(GL_READ_FRAMEBUFFER, OpenGLLogicalScalingFBO);
            my_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            my_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            my_glClear(GL_COLOR_BUFFER_BIT);
            my_glBlitFramebuffer(
                0, 0, override_w, override_h,
                dstrect.x, dstrect.y, dstrect.x + dstrect.w, dstrect.y + dstrect.h,
                GL_COLOR_BUFFER_BIT, filter);

            my_glClearColor(cc[0], cc[1], cc[2], cc[3]);
            if (has_scissor) my_glEnable(GL_SCISSOR_TEST);
            my_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
    }

    real_SDL_GL_MakeCurrent(spoof_window, ctx);
    real_SDL_GL_SwapWindow(spoof_window);
}

static int (*real_SDL_SetWindowFullscreen)(SDL_Window * window,Uint32 flags);
static int my_SDL_SetWindowFullscreen(SDL_Window * window,Uint32 flags){
    if(flags!=0){
        flags=SDL_WINDOW_FULLSCREEN_DESKTOP;
        is_fullscreen=1;
    }
    else{
        is_fullscreen=0;
    }
    return real_SDL_SetWindowFullscreen(window,flags);
}

/* ── GL proc address hook (shader sanitizer + future hooks) ── */

#include "shadersanitizer.h"

static void *(*real_SDL_GL_GetProcAddress)(const char *) = NULL;

static void *my_SDL_GL_GetProcAddress(const char *proc)
{
    void *ret = real_SDL_GL_GetProcAddress(proc);
    if (strcmp(proc, "glShaderSource") == 0) {
        if (!real_glShaderSource)
            real_glShaderSource = ret;
        /* Some drivers reject mid-shader #extension directives (non-standard).
        * Mesa uses an ad-hoc binary-name workaround; we sanitize instead. */
        return (void *)my_glShaderSource;
    }
    return ret;
}

/* ── Vibration patches ──
*
* cinput_manager_posix stubs out vibration entirely.
* We patch three functions:
*   allow_option_vibration @ 0xd43b80 — always return true
*   activate_vibration     @ 0xd43b20 — redirect to SDL_GameControllerRumble
*   deactivate_vibration   @ 0xd43900 — redirect to stop rumble
*/

static void patch_write(void *dst, void *src, size_t len)
{
    uintptr_t page = (uintptr_t)dst & ~(4095UL);
    mprotect((void *)page, 4096, PROT_READ | PROT_WRITE | PROT_EXEC);
    memcpy(dst, src, len);
    mprotect((void *)page, 4096, PROT_READ | PROT_EXEC);
}

static void patch_nop(void *dst, size_t len)
{
    uintptr_t page = (uintptr_t)dst & ~(4095UL);
    mprotect((void *)page, 4096, PROT_READ | PROT_WRITE | PROT_EXEC);
    memset(dst, 0x90, len);
    mprotect((void *)page, 4096, PROT_READ | PROT_EXEC);
}

static void my_activate_vibration(void *this)
{
    float cam_left   = *(float *)((char *)this + 0x5f0);
    float cam_right  = *(float *)((char *)this + 0x5f4);
    float game_left  = *(float *)((char *)this + 0x5f8);
    float game_right = *(float *)((char *)this + 0x5fc);
    float koef       = *(float *)((char *)this + 0x600);

    float left  = (cam_left  + game_left)  * koef;
    float right = (cam_right + game_right) * koef;

    if (left  < 0.0f) left  = 0.0f;
    if (right < 0.0f) right = 0.0f;
    if (left  > 1.0f) left  = 1.0f;
    if (right > 1.0f) right = 1.0f;

    joy_lock_acquire();
    for (int i = 0; i < MAX_JOYSTICKS; i++) {
        if (joy_table[i].gc)
            jump_table.SDL_GameControllerRumble(joy_table[i].gc,
                                                (Uint16)(left  * 65535.0f),
                                                (Uint16)(right * 65535.0f),
                                                1000);
    }
    joy_lock_release();
}

static void my_deactivate_vibration(void *this)
{
    joy_lock_acquire();
    for (int i = 0; i < MAX_JOYSTICKS; i++) {
        if (joy_table[i].gc)
            jump_table.SDL_GameControllerRumble(joy_table[i].gc, 0, 0, 0);
    }
    joy_lock_release();
}

static void my_set_vibration_game(void *this, float left, float right)
{
    *(float *)((char *)this + 0x5f8) = left;
    *(float *)((char *)this + 0x5fc) = right;
    my_activate_vibration(this);
}

static void my_set_vibration_camera(void *this, float left, float right)
{
    *(float *)((char *)this + 0x5f0) = left;
    *(float *)((char *)this + 0x5f4) = right;
    my_activate_vibration(this);
}

static void my_set_vibration_koef(void *this, float koef)
{
    *(float *)((char *)this + 0x600) = koef;
    my_activate_vibration(this);
}

static void apply_vibration_patches(void)
{
    /* allow_option_vibration: mov al, 1; ret */
    static const uint8_t always_true[] = { 0xb0, 0x01, 0xc3 };
    patch_write((void *)0xd43b80, (void *)always_true, sizeof always_true);

    /* activate_vibration: mov rax, imm64; jmp rax */
    uint8_t jmp_activate[12] = { 0x48, 0xb8, 0,0,0,0,0,0,0,0, 0xff, 0xe0 };
    *(uintptr_t *)(jmp_activate + 2) = (uintptr_t)my_activate_vibration;
    patch_write((void *)0xd43b20, jmp_activate, sizeof jmp_activate);

    /* deactivate_vibration (posix override): mov rax, imm64; jmp rax */
    uint8_t jmp_deactivate[12] = { 0x48, 0xb8, 0,0,0,0,0,0,0,0, 0xff, 0xe0 };
    *(uintptr_t *)(jmp_deactivate + 2) = (uintptr_t)my_deactivate_vibration;
    patch_write((void *)0xd43900, jmp_deactivate, sizeof jmp_deactivate);

    uint8_t jmp_set_game[12] = { 0x48, 0xb8, 0,0,0,0,0,0,0,0, 0xff, 0xe0 };
    *(uintptr_t *)(jmp_set_game + 2) = (uintptr_t)my_set_vibration_game;
    patch_write((void *)0xd43ab0, jmp_set_game, sizeof jmp_set_game);

    uint8_t jmp_set_camera[12] = { 0x48, 0xb8, 0,0,0,0,0,0,0,0, 0xff, 0xe0 };
    *(uintptr_t *)(jmp_set_camera + 2) = (uintptr_t)my_set_vibration_camera;
    patch_write((void *)0xd43a90, jmp_set_camera, sizeof jmp_set_camera);

    uint8_t jmp_koef[12] = { 0x48, 0xb8, 0,0,0,0,0,0,0,0, 0xff, 0xe0 };
    *(uintptr_t *)(jmp_koef + 2) = (uintptr_t)my_set_vibration_koef;
    patch_write((void *)0xd43ae0, jmp_koef, sizeof jmp_koef);

    DEBUGLOG("[sdl-hook] vibration patches applied");
}

/* ── Jump table wrappers (generated) ── */

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

/* ── Entry point ── */

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

    apply_vibration_patches();

    /* resolution override */
    const char *res = getenv("METRO_RESOLUTION_OVERRIDE");
    if (res) {
        sscanf(res, "%dx%d", &override_w, &override_h);
        LOG_FPRINTF(stderr, "[sdl-hook] logical resolution override: %dx%d\n", override_w, override_h);
    }

    /* SDL_Init */
    real_SDL_Init        = jump_table.SDL_Init;
    jump_table.SDL_Init = my_SDL_Init;

    /* Window management */
    real_SDL_CreateWindow       = jump_table.SDL_CreateWindow;
    jump_table.SDL_CreateWindow = my_SDL_CreateWindow;

    /* GL context management */
    real_SDL_GL_CreateContext        = jump_table.SDL_GL_CreateContext;
    jump_table.SDL_GL_CreateContext = my_SDL_GL_CreateContext;

    real_SDL_GL_DeleteContext        = jump_table.SDL_GL_DeleteContext;
    jump_table.SDL_GL_DeleteContext = my_SDL_GL_DeleteContext;

    /* GL surface / swap */
    real_SDL_GL_MakeCurrent        = jump_table.SDL_GL_MakeCurrent;
    jump_table.SDL_GL_MakeCurrent = my_SDL_GL_MakeCurrent;

    real_SDL_GL_GetCurrentWindow        = jump_table.SDL_GL_GetCurrentWindow;
    jump_table.SDL_GL_GetCurrentWindow = my_SDL_GL_GetCurrentWindow;

    real_SDL_GL_SwapWindow        = jump_table.SDL_GL_SwapWindow;
    jump_table.SDL_GL_SwapWindow = my_SDL_GL_SwapWindow;

    /* Thread creation */
    real_SDL_CreateThread        = jump_table.SDL_CreateThread;
    jump_table.SDL_CreateThread = my_SDL_CreateThread;

    /* Event pump */
    real_SDL_PollEvent       = jump_table.SDL_PollEvent;
    jump_table.SDL_PollEvent = my_SDL_PollEvent;

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

    /* 4A broke the ABI — GetRelativeMouseState has an extra z parameter */
    real_SDL_GetRelativeMouseState       = jump_table.SDL_GetRelativeMouseState;
    jump_table.SDL_GetRelativeMouseState = (void *)my_SDL_GetRelativeMouseState;

    /* GL proc address (shader sanitizer) */
    real_SDL_GL_GetProcAddress        = jump_table.SDL_GL_GetProcAddress;
    jump_table.SDL_GL_GetProcAddress  = my_SDL_GL_GetProcAddress;

    /* Display bounds override */
    real_SDL_GetDisplayBounds        = jump_table.SDL_GetDisplayBounds;
    jump_table.SDL_GetDisplayBounds  = my_SDL_GetDisplayBounds;

    real_SDL_GetWindowSize      = jump_table.SDL_GetWindowSize;
    jump_table.SDL_GetWindowSize  = my_SDL_GetWindowSize;

    real_SDL_SetWindowFullscreen=jump_table.SDL_SetWindowFullscreen;
    jump_table.SDL_SetWindowFullscreen=my_SDL_SetWindowFullscreen;

    real_SDL_SetWindowSize=jump_table.SDL_SetWindowSize;
    jump_table.SDL_SetWindowSize=my_SDL_SetWindowSize;

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
