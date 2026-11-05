/*
 * sdl_ps4.c — the SDL2 subset the DKR engine uses, on top of the Orbis APIs.
 *
 * Structure mirrors what the engine expects rather than SDL's internals:
 * one window (the Piglet surface), one controller (libScePad published as a
 * game controller), one audio device (libSceAudioOut), pthread for threads and
 * synchronisation, CLOCK_MONOTONIC for time.
 *
 * Piglet/EGL bring-up follows the recipe that runs on FW 12.02 (SM64 port):
 * preload the system modules from the sandbox random-word path, then walk a
 * ladder of memory splits until scePigletSetConfigurationVSH is accepted and
 * eglGetDisplay succeeds. SCE_PGL_FLAGS_USE_COMPOSITE_EXT is mandatory — without
 * it eglGetDisplay fails for every candidate.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>

#include <orbis/libkernel.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <orbis/Pad.h>
#include <orbis/AudioOut.h>
#include <orbis/Sysmodule.h>
#include <orbis/Pigletv2VSH.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "SDL.h"

/* ====================================================================== */
/* logging                                                                 */
/* ====================================================================== */

/* Milliseconds since the first log line. Every log line carries this, so any
 * future "when did that happen" question is answerable from the log alone. */
static long ps4_uptime_ms(void) {
    static struct timespec t0;
    static int have0;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    if (!have0) { t0 = t; have0 = 1; }
    return (long)(t.tv_sec - t0.tv_sec) * 1000L + (t.tv_nsec - t0.tv_nsec) / 1000000L;
}

/* OpenOrbis' ucontext is opaque (struct __ucontext is only forward-declared),
 * so a real stack unwind is impossible inside the signal handler. A ring of
 * the labels reached, plus the frame counter, answers the
 * same question -- "where was it when it died" -- without an unwinder. Only
 * rare transitions are recorded: recording every frame would overwrite the
 * useful history before it can be read. */
#define PS4_TRAIL_N 24
struct ps4_trail_entry { unsigned seq; unsigned frame; char label[20]; };
static struct ps4_trail_entry s_trail[PS4_TRAIL_N];
static volatile unsigned s_trail_seq;
static volatile unsigned s_crash_frame;

void ps4_crash_trail(const char *label) {
    unsigned i = s_trail_seq % PS4_TRAIL_N;
    s_trail[i].seq = s_trail_seq;
    s_trail[i].frame = s_crash_frame;
    snprintf(s_trail[i].label, sizeof(s_trail[i].label), "%s", label);
    s_trail_seq = s_trail_seq + 1;
}

void ps4_crash_snapshot(unsigned frame) {
    s_crash_frame = frame;
}

/* Best effort by design: called from a signal handler, so it uses raw write()
 * and never touches stdio state that may be what died. */
void ps4_crash_dump(int fd) {
    if (fd < 0) return;
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "frame=%u\n", (unsigned)s_crash_frame);
    if (n > 0) { ssize_t w = write(fd, buf, (size_t)n); (void)w; }

    unsigned total = s_trail_seq;
    unsigned shown = total < PS4_TRAIL_N ? total : PS4_TRAIL_N;
    for (unsigned k = 0; k < shown; k++) {
        unsigned idx = (total - shown + k) % PS4_TRAIL_N;
        n = snprintf(buf, sizeof(buf), "trail %u @frame %u: %s\n",
                     s_trail[idx].seq, s_trail[idx].frame, s_trail[idx].label);
        if (n > 0) { ssize_t w = write(fd, buf, (size_t)n); (void)w; }
    }
}

static void ps4_log(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "[%9.3f] ", (double)ps4_uptime_ms() / 1000.0);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ====================================================================== */
/* error string                                                            */
/* ====================================================================== */
static char s_error[256] = "";

static void set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_error, sizeof(s_error), fmt, ap);
    va_end(ap);
}

const char *SDL_GetError(void) { return s_error; }
void SDL_ClearError(void) { s_error[0] = '\0'; }

/* ====================================================================== */
/* hints                                                                   */
/* ====================================================================== */
#define MAX_HINTS 16
static struct { char name[48]; char value[128]; } s_hints[MAX_HINTS];
static int s_hint_count = 0;

int SDL_SetHintWithPriority(const char *name, const char *value, SDL_HintPriority prio) {
    (void)prio;
    if (name == NULL) return SDL_FALSE;
    for (int i = 0; i < s_hint_count; i++) {
        if (strcmp(s_hints[i].name, name) == 0) {
            snprintf(s_hints[i].value, sizeof(s_hints[i].value), "%s", value ? value : "");
            return SDL_TRUE;
        }
    }
    if (s_hint_count < MAX_HINTS) {
        snprintf(s_hints[s_hint_count].name, sizeof(s_hints[0].name), "%s", name);
        snprintf(s_hints[s_hint_count].value, sizeof(s_hints[0].value), "%s", value ? value : "");
        s_hint_count++;
    }
    return SDL_TRUE;
}

int SDL_SetHint(const char *name, const char *value) {
    return SDL_SetHintWithPriority(name, value, SDL_HINT_NORMAL);
}

const char *SDL_GetHint(const char *name) {
    if (name == NULL) return NULL;
    for (int i = 0; i < s_hint_count; i++) {
        if (strcmp(s_hints[i].name, name) == 0) return s_hints[i].value;
    }
    return NULL;
}

/* ====================================================================== */
/* memory / paths                                                          */
/* ====================================================================== */
void SDL_free(void *mem) { free(mem); }
void *SDL_malloc(size_t n) { return malloc(n); }
void *SDL_calloc(size_t n, size_t s) { return calloc(n, s); }
void *SDL_realloc(void *p, size_t n) { return realloc(p, n); }

char *SDL_GetBasePath(void) {
    /* The engine joins resource names onto this; /data/dkr is the install dir
     * for this port. Trailing slash is part of the SDL contract. */
    char *p = (char *)malloc(16);
    if (p) snprintf(p, 16, "/data/dkr/");
    return p;
}

char *SDL_GetPrefPath(const char *org, const char *app) {
    (void)org;
    (void)app;
    char *p = (char *)malloc(24);
    if (p) snprintf(p, 24, "/data/dkr/save/");
    return p;
}

/* ====================================================================== */
/* time                                                                    */
/* ====================================================================== */
/* libkernel's process clock is the only monotonic source the Orbis headers
 * declare; sceKernelGetProcessTime() is in microseconds. */
static uint64_t now_ns(void) {
    return sceKernelGetProcessTime() * 1000ull;
}

static uint64_t s_start_ns;
static int s_time_ready;

static void time_init(void) {
    if (!s_time_ready) {
        s_start_ns = now_ns();
        s_time_ready = 1;
    }
}

Uint64 SDL_GetPerformanceCounter(void) {
    time_init();
    return now_ns() - s_start_ns;
}

Uint64 SDL_GetPerformanceFrequency(void) { return 1000000000ull; }

Uint32 SDL_GetTicks(void) {
    return (Uint32)(SDL_GetPerformanceCounter() / 1000000ull);
}

Uint64 SDL_GetTicks64(void) {
    return SDL_GetPerformanceCounter() / 1000000ull;
}

void SDL_Delay(Uint32 ms) {
    if (ms == 0) return;
    sceKernelUsleep(ms * 1000u);
}

/* ====================================================================== */
/* threads, mutexes, condition variables, atomics                          */
/* ====================================================================== */
struct SDL_mutex { pthread_mutex_t m; };
struct SDL_cond  { pthread_cond_t c; };
struct SDL_Thread {
    pthread_t handle;
    int (*fn)(void *);
    void *data;
    int result;
    int finished;
};

SDL_mutex *SDL_CreateMutex(void) {
    SDL_mutex *m = (SDL_mutex *)calloc(1, sizeof(*m));
    if (m == NULL) return NULL;
    pthread_mutex_init(&m->m, NULL);
    return m;
}

void SDL_DestroyMutex(SDL_mutex *m) {
    if (m == NULL) return;
    pthread_mutex_destroy(&m->m);
    free(m);
}

int SDL_LockMutex(SDL_mutex *m) { return m ? pthread_mutex_lock(&m->m) : -1; }
int SDL_UnlockMutex(SDL_mutex *m) { return m ? pthread_mutex_unlock(&m->m) : -1; }

SDL_cond *SDL_CreateCond(void) {
    SDL_cond *c = (SDL_cond *)calloc(1, sizeof(*c));
    if (c == NULL) return NULL;
    pthread_cond_init(&c->c, NULL);
    return c;
}

void SDL_DestroyCond(SDL_cond *c) {
    if (c == NULL) return;
    pthread_cond_destroy(&c->c);
    free(c);
}

int SDL_CondWait(SDL_cond *c, SDL_mutex *m) {
    if (c == NULL || m == NULL) return -1;
    return pthread_cond_wait(&c->c, &m->m);
}

int SDL_CondWaitTimeout(SDL_cond *c, SDL_mutex *m, Uint32 ms) {
    if (c == NULL || m == NULL) return -1;
    struct timespec ts;
    time_t now = time(NULL);
    ts.tv_sec = now + (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    int rc = pthread_cond_timedwait(&c->c, &m->m, &ts);
    return rc == ETIMEDOUT ? 1 : rc;
}

int SDL_CondSignal(SDL_cond *c) { return c ? pthread_cond_signal(&c->c) : -1; }
int SDL_CondBroadcast(SDL_cond *c) { return c ? pthread_cond_broadcast(&c->c) : -1; }

static void *thread_trampoline(void *arg) {
    SDL_Thread *t = (SDL_Thread *)arg;
    t->result = t->fn(t->data);
    t->finished = 1;
    return NULL;
}

SDL_Thread *SDL_CreateThread(int (*fn)(void *), const char *name, void *data) {
    (void)name;
    if (fn == NULL) return NULL;
    SDL_Thread *t = (SDL_Thread *)calloc(1, sizeof(*t));
    if (t == NULL) return NULL;
    t->fn = fn;
    t->data = data;
    if (pthread_create(&t->handle, NULL, thread_trampoline, t) != 0) {
        free(t);
        return NULL;
    }
    return t;
}

void SDL_WaitThread(SDL_Thread *thread, int *status) {
    if (thread == NULL) return;
    pthread_join(thread->handle, NULL);
    if (status != NULL) *status = thread->result;
    free(thread);
}

int SDL_AtomicGet(SDL_atomic_t *a) { return a ? __atomic_load_n(&a->value, __ATOMIC_SEQ_CST) : 0; }
int SDL_AtomicSet(SDL_atomic_t *a, int v) {
    return a ? __atomic_exchange_n(&a->value, v, __ATOMIC_SEQ_CST) : 0;
}
int SDL_AtomicAdd(SDL_atomic_t *a, int v) {
    return a ? __atomic_fetch_add(&a->value, v, __ATOMIC_SEQ_CST) : 0;
}
SDL_bool SDL_AtomicCAS(SDL_atomic_t *a, int oldval, int newval) {
    if (a == NULL) return SDL_FALSE;
    return __atomic_compare_exchange_n(&a->value, &oldval, newval, 0,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
               ? SDL_TRUE : SDL_FALSE;
}
void SDL_MemoryBarrierAcquire(void) { __atomic_thread_fence(__ATOMIC_ACQUIRE); }
void SDL_MemoryBarrierRelease(void) { __atomic_thread_fence(__ATOMIC_RELEASE); }

/* ====================================================================== */
/* Orbis bring-up: modules -> Piglet config ladder -> EGL                  */
/* ====================================================================== */
#define SCREEN_W 1920
#define SCREEN_H 1080

static EGLDisplay s_display = EGL_NO_DISPLAY;
static EGLSurface s_surface = EGL_NO_SURFACE;
static EGLContext s_context = EGL_NO_CONTEXT;
static EGLConfig  s_config  = NULL;
static int s_surface_w = SCREEN_W;
static int s_surface_h = SCREEN_H;
static int s_swap_count = 0;
static int s_swap_interval = 0;
static int s_swap_interval_ignored_logged = 0;

/* Same ladder the SM64 port uses; system/video/flexible in MiB. */
/* Same ladder, same order as the working SM64 port. The first candidate is the
 * one that port actually runs with: 128 MiB system, 512 MiB video. Starting at
 * 16/48 (as this port did) leaves too little video memory for double-buffered
 * 1920x1080 plus depth — the first eglSwapBuffers then waits for a back buffer
 * that cannot be allocated. */
static const struct { uint32_t sys_mb, vid_mb, flex_mb; } kMemorySplits[] = {
    { 128, 512, 236 },
    { 16, 48, 236 },
    { 64, 128, 208 },
    { 64, 128, 64 },
    /* If an updated loader or a background app changed the budget, the only
     * way to find the ceiling is to ask for less and see where it starts to
     * work. These are last-resort sizes, not targets. */
    { 16, 48, 64 },
    { 8, 16, 32 },
    { 4, 8, 16 },
};

/* Defined by the OpenOrbis Piglet header only as a comment; the values the
 * working SM64 port uses. */
#define SCE_PGL_FLAGS_USE_COMPOSITE_EXT  0x8u
#define SCE_PGL_FLAGS_USE_FLEXIBLE_MEMORY 0x80u

static OrbisPglConfig s_pgl;
static OrbisPglWindow s_render_window;

static int load_system_modules(void) {
    static const char *kModules[] = {
        "libSceSysCore.sprx",
        "libSceMbus.sprx",
        "libSceIpmi.sprx",
        "libSceSystemService.sprx",
        "libScePigletv2VSH.sprx",
        "libSceShaccVSH.sprx",
    };

    /* Both candidate directories, in the order the working SM64 port uses: the
     * literal path first. The sandbox random word resolves to a per-title
     * directory that does NOT contain every module — loading Shacc from there
     * returned 0x80020002 while it sat in /data the whole time. */
    char sandbox_base[256];
    sandbox_base[0] = '\0';
    const char *word = sceKernelGetFsSandboxRandomWord();
    if (word != NULL) {
        snprintf(sandbox_base, sizeof(sandbox_base), "/%s/common/lib", word);
    }

    int loaded = 0;
    for (size_t i = 0; i < sizeof(kModules) / sizeof(kModules[0]); i++) {
        char path[512];
        int32_t start_result = 0;
        int handle = -1;

        snprintf(path, sizeof(path), "/data/self/system/common/lib/%s", kModules[i]);
        handle = (int)sceKernelLoadStartModule(path, 0, NULL, 0, NULL, &start_result);

        if (handle < 0 && sandbox_base[0] != '\0') {
            snprintf(path, sizeof(path), "%s/%s", sandbox_base, kModules[i]);
            start_result = 0;
            handle = (int)sceKernelLoadStartModule(path, 0, NULL, 0, NULL, &start_result);
        }

        ps4_log("[ps4] module %-24s -> 0x%08X (start 0x%08X) %s",
                kModules[i], (unsigned)handle, (unsigned)start_result, path);
        if (handle >= 0) loaded++;
    }
    return loaded;
}

/* Direct memory is the pool every GPU allocation (and Piglet's compositor)
 * comes from. When eglGetDisplay fails with "Out of memory", the question is
 * whether the console is actually short or Piglet is misconfigured -- these two
 * numbers answer it without guessing. */
static void ps4_log_memory(const char *when) {
    extern size_t sceKernelGetDirectMemorySize(void);
    /* orbis/libkernel.h prototypes this with the last two arguments by value,
     * which cannot be right for out-parameters; the real ABI takes pointers.
     * Call it through a correctly typed pointer rather than fighting the
     * header, and keep the declaration identical to the header's so it does not
     * conflict. */
    extern int32_t sceKernelAvailableDirectMemorySize(off_t, off_t, size_t, off_t, size_t);
    int32_t (*query)(off_t, off_t, size_t, size_t *, off_t *) =
        (int32_t (*)(off_t, off_t, size_t, size_t *, off_t *))
        (void *)sceKernelAvailableDirectMemorySize;
    size_t total = sceKernelGetDirectMemorySize();
    size_t avail = 0;
    off_t phys = 0;
    int rc = query(0, (off_t)total, 0x4000, &avail, &phys);
    ps4_log("[MEM] %s: direct total=%zu MiB free=%zu MiB (rc=0x%08X)",
            when, total >> 20, avail >> 20, (unsigned)rc);
}

static int piglet_configure(int width, int height) {
    memset(&s_pgl, 0, sizeof(s_pgl));
    s_pgl.size                    = sizeof(s_pgl);
    s_pgl.flags                   = SCE_PGL_FLAGS_USE_COMPOSITE_EXT |
                                    SCE_PGL_FLAGS_USE_FLEXIBLE_MEMORY | 0x60;
    s_pgl.processOrder            = 1;
    s_pgl.systemSharedMemorySize  = 128u << 20;
    s_pgl.videoSharedMemorySize   = 512u << 20;
    s_pgl.maxMappedFlexibleMemory = 236u << 20;
    s_pgl.drawCommandBufferSize   = 4u << 20;
    s_pgl.lcueResourceBufferSize  = 4u << 20;
    s_pgl.dbgPosCmd_0x40          = width;
    s_pgl.dbgPosCmd_0x44          = height;
    s_pgl.unk_0x5C                = 2;

    for (size_t i = 0; i < sizeof(kMemorySplits) / sizeof(kMemorySplits[0]); i++) {
        s_pgl.systemSharedMemorySize  = kMemorySplits[i].sys_mb << 20;
        s_pgl.videoSharedMemorySize   = kMemorySplits[i].vid_mb << 20;
        s_pgl.maxMappedFlexibleMemory = kMemorySplits[i].flex_mb << 20;
        if (!scePigletSetConfigurationVSH(&s_pgl)) {
            ps4_log("[ps4] piglet config %u rejected (sys=%u vid=%u flex=%u MiB)",
                    (unsigned)i, (unsigned)kMemorySplits[i].sys_mb,
                    (unsigned)kMemorySplits[i].vid_mb, (unsigned)kMemorySplits[i].flex_mb);
            continue;
        }
        s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (s_display != EGL_NO_DISPLAY) {
            ps4_log("[ps4] piglet config %u accepted, display %p", (unsigned)i,
                    (void *)s_display);
            return 1;
        }
        ps4_log("[ps4] piglet config %u (sys=%u vid=%u flex=%u MiB): eglGetDisplay failed (0x%04X)",
                (unsigned)i, (unsigned)kMemorySplits[i].sys_mb,
                (unsigned)kMemorySplits[i].vid_mb, (unsigned)kMemorySplits[i].flex_mb,
                (unsigned)eglGetError());
    }
    return 0;
}

static void ps4_heartbeat_start(void);

static int egl_bringup(int width, int height) {
    if (!piglet_configure(width, height)) {
        /* A previous instance that crashed can still hold the compositor's
         * memory for a moment; one delayed retry costs a second and turns
         * "won't start" into "starts a second later" when that is the cause. */
        ps4_log_memory("all splits rejected, retrying in 1 s");
        sceKernelUsleep(1000000);
        ps4_log_memory("retrying");
        if (!piglet_configure(width, height)) {
            set_error("Piglet configuration rejected for every memory split");
            return 0;
        }
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(s_display, &major, &minor)) {
        set_error("eglInitialize failed (0x%04X)", (unsigned)eglGetError());
        return 0;
    }
    ps4_log("[ps4] EGL %d.%d", (int)major, (int)minor);

    /* Both calls exist in the working SM64 port and were missing here.
     * Without eglBindAPI the ES binding is left to the driver's default, and
     * without eglSwapInterval(0) the first eglSwapBuffers waits for a vertical
     * blank that never arrives in this presentation path — the observed hang. */
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        ps4_log("[ps4] eglBindAPI failed (0x%04X)", (unsigned)eglGetError());
    }
    if (!eglSwapInterval(s_display, 0)) {
        ps4_log("[ps4] eglSwapInterval failed (0x%04X)", (unsigned)eglGetError());
    } else {
        ps4_log("[ps4] EGL swap interval 0 (no vsync wait)");
    }

    /* Depth/stencil 24/8: a 3D game needs depth, and some drivers refuse
     * eglChooseConfig outright when nothing is requested. */
    static const EGLint cfg_attribs[] = {
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_SAMPLE_BUFFERS, 0, EGL_SAMPLES, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_NONE
    };
    EGLint num_configs = 0;
    if (!eglChooseConfig(s_display, cfg_attribs, &s_config, 1, &num_configs) ||
        num_configs < 1) {
        set_error("eglChooseConfig failed (0x%04X)", (unsigned)eglGetError());
        return 0;
    }

    memset(&s_render_window, 0, sizeof(s_render_window));
    s_render_window.uID = 0;                 /* must be in [0..7] */
    s_render_window.uWidth = (uint32_t)width;
    s_render_window.uHeight = (uint32_t)height;

    /* EGL_RENDER_BUFFER = EGL_BACK_BUFFER is what the working SM64 port passes:
     * it states that drawing goes to the back buffer and eglSwapBuffers flips
     * it. Without it the swap blocked forever on the first call. */
    static const EGLint surface_attribs[] = {
        EGL_RENDER_BUFFER, EGL_BACK_BUFFER,
        EGL_NONE
    };
    s_surface = eglCreateWindowSurface(s_display, s_config, &s_render_window,
                                       surface_attribs);
    if (s_surface == EGL_NO_SURFACE) {
        set_error("eglCreateWindowSurface failed (0x%04X)", (unsigned)eglGetError());
        return 0;
    }

    static const EGLint ctx_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    s_context = eglCreateContext(s_display, s_config, EGL_NO_CONTEXT, ctx_attribs);
    if (s_context == EGL_NO_CONTEXT) {
        set_error("eglCreateContext failed (0x%04X)", (unsigned)eglGetError());
        return 0;
    }
    if (!eglMakeCurrent(s_display, s_surface, s_surface, s_context)) {
        set_error("eglMakeCurrent failed (0x%04X)", (unsigned)eglGetError());
        return 0;
    }

    eglQuerySurface(s_display, s_surface, EGL_WIDTH, &s_surface_w);
    eglQuerySurface(s_display, s_surface, EGL_HEIGHT, &s_surface_h);

    ps4_log("[ps4] surface %dx%d, context ready", s_surface_w, s_surface_h);
    return 1;
}

int ps4_sdl_backend_init(int width, int height) {
    if (s_context != EGL_NO_CONTEXT) {
        return 1;   /* already brought up */
    }
    time_init();
    load_system_modules();
    if (!egl_bringup(width, height)) {
        ps4_log("[ps4] EGL bring-up failed: %s", SDL_GetError());
        return 0;
    }
    return 1;
}

void ps4_sdl_backend_shutdown(void) {
    if (s_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (s_context != EGL_NO_CONTEXT) eglDestroyContext(s_display, s_context);
        if (s_surface != EGL_NO_SURFACE) eglDestroySurface(s_display, s_surface);
        eglTerminate(s_display);
        s_context = EGL_NO_CONTEXT;
        s_surface = EGL_NO_SURFACE;
        s_display = EGL_NO_DISPLAY;
    }
}

/* ====================================================================== */
/* window + GL                                                             */
/* ====================================================================== */
struct SDL_Window {
    int w, h;
    Uint32 flags;
    char title[64];
};

static SDL_Window s_window;

int SDL_InitSubSystem(Uint32 flags) { (void)flags; return 0; }
void SDL_QuitSubSystem(Uint32 flags) { (void)flags; }

int SDL_Init(Uint32 flags) {
    (void)flags;
    time_init();
    return 0;
}

void SDL_Quit(void) { }

Uint32 SDL_WasInit(Uint32 flags) { (void)flags; return SDL_INIT_EVERYTHING; }
void SDL_SetMainReady(void) { }

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags) {
    (void)x;
    (void)y;
    if (!ps4_sdl_backend_init(w > 0 ? w : SCREEN_W, h > 0 ? h : SCREEN_H)) {
        return NULL;
    }
    memset(&s_window, 0, sizeof(s_window));
    s_window.w = s_surface_w;
    s_window.h = s_surface_h;
    s_window.flags = flags;
    snprintf(s_window.title, sizeof(s_window.title), "%s", title ? title : "mdkr64");
    ps4_log("[ps4] window %dx%d (%s)", s_window.w, s_window.h, s_window.title);
    return &s_window;
}

void SDL_DestroyWindow(SDL_Window *w) {
    (void)w;
    ps4_sdl_backend_shutdown();
}

void SDL_ShowWindow(SDL_Window *w) { (void)w; }
void SDL_HideWindow(SDL_Window *w) { (void)w; }
void SDL_RaiseWindow(SDL_Window *w) { (void)w; }
void SDL_RestoreWindow(SDL_Window *w) { (void)w; }
void SDL_MinimizeWindow(SDL_Window *w) { (void)w; }

void SDL_GetWindowSize(SDL_Window *w, int *wout, int *hout) {
    if (wout) *wout = w ? w->w : s_surface_w;
    if (hout) *hout = w ? w->h : s_surface_h;
}

void SDL_GetWindowSizeInPixels(SDL_Window *w, int *wout, int *hout) {
    SDL_GetWindowSize(w, wout, hout);
}

Uint32 SDL_GetWindowFlags(SDL_Window *w) { return w ? w->flags : 0; }
int SDL_GetWindowDisplayIndex(SDL_Window *w) { (void)w; return 0; }

int SDL_GetCurrentDisplayMode(int displayIndex, SDL_DisplayMode *mode) {
    (void)displayIndex;
    if (mode == NULL) return -1;
    mode->w = s_surface_w;
    mode->h = s_surface_h;
    mode->refresh_rate = 60;
    mode->format = 0;
    mode->driverdata = NULL;
    return 0;
}

int SDL_GetDesktopDisplayMode(int displayIndex, SDL_DisplayMode *mode) {
    return SDL_GetCurrentDisplayMode(displayIndex, mode);
}

int SDL_GetWindowWMInfo(SDL_Window *w, SDL_SysWMinfo *info) {
    (void)w;
    (void)info;
    return 0;
}

int SDL_GL_SetAttribute(int attr, int value) { (void)attr; (void)value; return 0; }

int SDL_GL_GetAttribute(int attr, int *value) {
    (void)attr;
    if (value) *value = 0;
    return 0;
}

SDL_GLContext SDL_GL_CreateContext(SDL_Window *w) {
    (void)w;
    return s_context != EGL_NO_CONTEXT ? (SDL_GLContext)s_context : NULL;
}

void SDL_GL_DeleteContext(SDL_GLContext ctx) { (void)ctx; }
SDL_GLContext SDL_GL_GetCurrentContext(void) { return (SDL_GLContext)s_context; }
SDL_Window *SDL_GL_GetCurrentWindow(void) { return &s_window; }

/* Core GL entry points that eglGetProcAddress may not hand out. The engine logs
 * how many pointers it resolved, and the count is the check. */
#define PS4_GL(name) extern void *name(void);
extern void glActiveTexture(unsigned);
extern void glBindTexture(unsigned, unsigned);
extern void glBlendFunc(unsigned, unsigned);
extern void glClear(unsigned);
extern void glClearColor(float, float, float, float);
extern void glClearDepthf(float);
extern void glColorMask(unsigned char, unsigned char, unsigned char, unsigned char);
extern void glCompileShader(unsigned);
extern unsigned glCreateProgram(void);
extern unsigned glCreateShader(unsigned);
extern void glCullFace(unsigned);
extern void glDeleteBuffers(int, const unsigned *);
extern void glDeleteProgram(unsigned);
extern void glDeleteShader(unsigned);
extern void glDeleteTextures(int, const unsigned *);
extern void glDepthFunc(unsigned);
extern void glDepthMask(unsigned char);
extern void glDisable(unsigned);
extern void glDrawArrays(unsigned, int, int);
extern void glDrawElements(unsigned, int, unsigned, const void *);
extern void glEnable(unsigned);
extern void glEnableVertexAttribArray(unsigned);
extern void glDisableVertexAttribArray(unsigned);
extern void glGenBuffers(int, unsigned *);
extern void glGenTextures(int, unsigned *);
extern int  glGetAttribLocation(unsigned, const char *);
extern unsigned glGetError(void);
extern void glGetIntegerv(unsigned, int *);
extern void glGetProgramiv(unsigned, unsigned, int *);
extern void glGetProgramInfoLog(unsigned, int, int *, char *);
extern void glGetShaderiv(unsigned, unsigned, int *);
extern void glGetShaderInfoLog(unsigned, int, int *, char *);
extern const unsigned char *glGetString(unsigned);
extern int  glGetUniformLocation(unsigned, const char *);
extern void glLinkProgram(unsigned);
extern void glPixelStorei(unsigned, int);
extern void glReadPixels(int, int, int, int, unsigned, unsigned, void *);
extern void glScissor(int, int, int, int);
extern void glShaderSource(unsigned, int, const char *const *, const int *);
extern void glTexImage2D(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
extern void glTexParameteri(unsigned, unsigned, int);
extern void glTexSubImage2D(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
extern void glUniform1i(int, int);
extern void glUniform1f(int, float);
extern void glUniform2f(int, float, float);
extern void glUniform3f(int, float, float, float);
extern void glUniform4f(int, float, float, float, float);
extern void glUniformMatrix4fv(int, int, unsigned char, const float *);
extern void glUseProgram(unsigned);
extern void glVertexAttribPointer(unsigned, int, unsigned, unsigned char, int, const void *);
extern void glViewport(int, int, int, int);
extern void glBindBuffer(unsigned, unsigned);
extern void glBufferData(unsigned, long, const void *, unsigned);
extern void glBufferSubData(unsigned, long, long, const void *);
extern void glGenFramebuffers(int, unsigned *);
extern void glBindFramebuffer(unsigned, unsigned);
extern void glFramebufferTexture2D(unsigned, unsigned, unsigned, unsigned, int);
extern void glDeleteFramebuffers(int, const unsigned *);
extern unsigned glCheckFramebufferStatus(unsigned);
extern void glGenRenderbuffers(int, unsigned *);
extern void glBindRenderbuffer(unsigned, unsigned);
extern void glRenderbufferStorage(unsigned, unsigned, int, int);
extern void glFramebufferRenderbuffer(unsigned, unsigned, unsigned, unsigned);
extern void glDeleteRenderbuffers(int, const unsigned *);
extern void glBlendFuncSeparate(unsigned, unsigned, unsigned, unsigned);
extern void glBlendEquation(unsigned);
extern void glBlendEquationSeparate(unsigned, unsigned);
extern void glDepthRangef(float, float);
extern void glPolygonOffset(float, float);
extern void glLineWidth(float);
extern void glFrontFace(unsigned);
extern void glFinish(void);
extern void glFlush(void);
extern void glGetFloatv(unsigned, float *);
extern void glGetBooleanv(unsigned, unsigned char *);
extern unsigned char glIsEnabled(unsigned);
extern void glTexParameterf(unsigned, unsigned, float);
extern void glGenerateMipmap(unsigned);
extern void glAttachShader(unsigned, unsigned);
extern void glBindAttribLocation(unsigned, unsigned, const char *);
extern void glStencilFunc(unsigned, int, unsigned);
extern void glStencilOp(unsigned, unsigned, unsigned);
extern void glStencilMask(unsigned);
extern void glCopyTexSubImage2D(unsigned, int, int, int, int, int, int, int);
extern void glVertexAttrib1f(unsigned, float);
extern void glVertexAttrib2f(unsigned, float, float);
extern void glVertexAttrib3f(unsigned, float, float, float);
extern void glVertexAttrib4f(unsigned, float, float, float, float);
extern void glUniform1fv(int, int, const float *);
extern void glUniform2fv(int, int, const float *);
extern void glUniform3fv(int, int, const float *);
extern void glUniform4fv(int, int, const float *);
extern void glActiveTextureStub(void);

/* Forward declarations: the lookup table below is defined before the stub
 * bodies (which live next to SDL_GL_GetProcAddress). */

void ps4_glTexImage2D(unsigned target, int level, int internalformat, int width,
                      int height, int border, unsigned format, unsigned type,
                      const void *pixels);
void ps4_glGetTexLevelParameteriv(unsigned target, int level, unsigned pname, int *params);
void ps4_glReadBuffer(unsigned mode);
void ps4_glDrawBuffer(unsigned mode);
void ps4_glPolygonMode(unsigned face, unsigned mode);
void ps4_glRenderbufferStorageMultisample(unsigned target, int samples,
                                          unsigned internalformat, int width, int height);
void ps4_glFramebufferTextureLayer(unsigned target, unsigned attachment,
                                   unsigned texture, int level, int layer);
void ps4_glBlitFramebuffer(int srcX0, int srcY0, int srcX1, int srcY1,
                           int dstX0, int dstY0, int dstX1, int dstY1,
                           unsigned mask, unsigned filter);
void ps4_glTexImage3D(unsigned target, int level, int internalformat, int width,
                      int height, int depth, int border, unsigned format,
                      unsigned type, const void *pixels);

static const struct { const char *name; void *fn; } kCoreGL[] = {
    { "glActiveTexture", (void *)glActiveTexture },
    { "glBindTexture", (void *)glBindTexture },
    { "glBlendFunc", (void *)glBlendFunc },
    { "glClear", (void *)glClear },
    { "glClearColor", (void *)glClearColor },
    { "glClearDepthf", (void *)glClearDepthf },
    { "glColorMask", (void *)glColorMask },
    { "glCompileShader", (void *)glCompileShader },
    { "glCreateProgram", (void *)glCreateProgram },
    { "glCreateShader", (void *)glCreateShader },
    { "glCullFace", (void *)glCullFace },
    { "glDeleteBuffers", (void *)glDeleteBuffers },
    { "glDeleteProgram", (void *)glDeleteProgram },
    { "glDeleteShader", (void *)glDeleteShader },
    { "glDeleteTextures", (void *)glDeleteTextures },
    { "glDepthFunc", (void *)glDepthFunc },
    { "glDepthMask", (void *)glDepthMask },
    { "glDisable", (void *)glDisable },
    { "glDisableVertexAttribArray", (void *)glDisableVertexAttribArray },
    { "glDrawArrays", (void *)glDrawArrays },
    { "glDrawElements", (void *)glDrawElements },
    { "glEnable", (void *)glEnable },
    { "glEnableVertexAttribArray", (void *)glEnableVertexAttribArray },
    { "glFinish", (void *)glFinish },
    { "glFlush", (void *)glFlush },
    { "glFrontFace", (void *)glFrontFace },
    { "glGenBuffers", (void *)glGenBuffers },
    { "glGenTextures", (void *)glGenTextures },
    { "glGetAttribLocation", (void *)glGetAttribLocation },
    { "glGetBooleanv", (void *)glGetBooleanv },
    { "glGetError", (void *)glGetError },
    { "glGetFloatv", (void *)glGetFloatv },
    { "glGetIntegerv", (void *)glGetIntegerv },
    { "glGetProgramInfoLog", (void *)glGetProgramInfoLog },
    { "glGetProgramiv", (void *)glGetProgramiv },
    { "glGetShaderInfoLog", (void *)glGetShaderInfoLog },
    { "glGetShaderiv", (void *)glGetShaderiv },
    { "glGetString", (void *)glGetString },
    { "glGetUniformLocation", (void *)glGetUniformLocation },
    { "glIsEnabled", (void *)glIsEnabled },
    { "glLineWidth", (void *)glLineWidth },
    { "glLinkProgram", (void *)glLinkProgram },
    { "glPixelStorei", (void *)glPixelStorei },
    { "glPolygonOffset", (void *)glPolygonOffset },
    { "glReadPixels", (void *)glReadPixels },
    { "glScissor", (void *)glScissor },
    { "glShaderSource", (void *)glShaderSource },
    { "glTexImage2D", (void *)ps4_glTexImage2D },
    { "glTexParameteri", (void *)glTexParameteri },
    { "glTexParameterf", (void *)glTexParameterf },
    { "glTexSubImage2D", (void *)glTexSubImage2D },
    { "glUniform1f", (void *)glUniform1f },
    { "glUniform1i", (void *)glUniform1i },
    { "glUniform2f", (void *)glUniform2f },
    { "glUniform3f", (void *)glUniform3f },
    { "glUniform4f", (void *)glUniform4f },
    { "glUniformMatrix4fv", (void *)glUniformMatrix4fv },
    { "glUseProgram", (void *)glUseProgram },
    { "glVertexAttribPointer", (void *)glVertexAttribPointer },
    { "glViewport", (void *)glViewport },
    { "glBindBuffer", (void *)glBindBuffer },
    { "glBufferData", (void *)glBufferData },
    { "glBufferSubData", (void *)glBufferSubData },
    { "glGenFramebuffers", (void *)glGenFramebuffers },
    { "glBindFramebuffer", (void *)glBindFramebuffer },
    { "glFramebufferTexture2D", (void *)glFramebufferTexture2D },
    { "glDeleteFramebuffers", (void *)glDeleteFramebuffers },
    { "glCheckFramebufferStatus", (void *)glCheckFramebufferStatus },
    { "glGenRenderbuffers", (void *)glGenRenderbuffers },
    { "glBindRenderbuffer", (void *)glBindRenderbuffer },
    { "glRenderbufferStorage", (void *)glRenderbufferStorage },
    { "glFramebufferRenderbuffer", (void *)glFramebufferRenderbuffer },
    { "glDeleteRenderbuffers", (void *)glDeleteRenderbuffers },
    { "glBlendFuncSeparate", (void *)glBlendFuncSeparate },
    { "glBlendEquation", (void *)glBlendEquation },
    { "glBlendEquationSeparate", (void *)glBlendEquationSeparate },
    { "glDepthRangef", (void *)glDepthRangef },
    { "glGenerateMipmap", (void *)glGenerateMipmap },
    { "glReadBuffer", (void *)ps4_glReadBuffer },
    { "glDrawBuffer", (void *)ps4_glDrawBuffer },
    { "glPolygonMode", (void *)ps4_glPolygonMode },
    { "glRenderbufferStorageMultisample", (void *)ps4_glRenderbufferStorageMultisample },
    { "glFramebufferTextureLayer", (void *)ps4_glFramebufferTextureLayer },
    { "glBlitFramebuffer", (void *)ps4_glBlitFramebuffer },
    { "glTexImage3D", (void *)ps4_glTexImage3D },
    { "glGetTexLevelParameteriv", (void *)ps4_glGetTexLevelParameteriv },

    { "glAttachShader", (void *)glAttachShader },
    { "glBindAttribLocation", (void *)glBindAttribLocation },
    { "glStencilFunc", (void *)glStencilFunc },
    { "glStencilOp", (void *)glStencilOp },
    { "glStencilMask", (void *)glStencilMask },
    { "glCopyTexSubImage2D", (void *)glCopyTexSubImage2D },
    { "glVertexAttrib1f", (void *)glVertexAttrib1f },
    { "glVertexAttrib2f", (void *)glVertexAttrib2f },
    { "glVertexAttrib3f", (void *)glVertexAttrib3f },
    { "glVertexAttrib4f", (void *)glVertexAttrib4f },
    { "glUniform1fv", (void *)glUniform1fv },
    { "glUniform2fv", (void *)glUniform2fv },
    { "glUniform3fv", (void *)glUniform3fv },
    { "glUniform4fv", (void *)glUniform4fv },

};

/* ====================================================================== */
/* GL functions Piglet does not export                                    */
/* ====================================================================== */
/* eglGetProcAddress on Piglet hands out extensions only, and several entry
 * points the renderer reaches for belong to ES 3 or to desktop GL. Left NULL
 * they are indirect calls into nothing — that is a SIGSEGV with no log line,
 * which is exactly how this port first died right after "GL ready".
 *
 * Each stub either degrades the optional path or forwards to an ES 2.0
 * equivalent; none of them is a real implementation of the missing feature. */

#define PS4_TEX_TRACK 4096
static int s_tex_w[PS4_TEX_TRACK];
static int s_tex_h[PS4_TEX_TRACK];

static void ps4_note_texture_size(int level, int w, int h) {
    if (level != 0) return;
    GLint bound = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    if (bound >= 0 && bound < PS4_TEX_TRACK) {
        s_tex_w[bound] = w;
        s_tex_h[bound] = h;
    }
}

void ps4_glTexImage2D(unsigned target, int level, int internalformat, int width,
                      int height, int border, unsigned format, unsigned type,
                      const void *pixels) {
    ps4_note_texture_size(level, width, height);
    glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
}

void ps4_glGetTexLevelParameteriv(unsigned target, int level, unsigned pname, int *params) {
    (void)target;
    if (params == NULL) return;
    GLint bound = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    int w = 0, h = 0;
    if (bound >= 0 && bound < PS4_TEX_TRACK && level == 0) {
        w = s_tex_w[bound];
        h = s_tex_h[bound];
    }
    switch (pname) {
        case 0x1000 /* GL_TEXTURE_WIDTH  */: *params = w; break;
        case 0x1001 /* GL_TEXTURE_HEIGHT */: *params = h; break;
        case 0x1003 /* GL_TEXTURE_INTERNAL_FORMAT */: *params = 0x1908 /* GL_RGBA */; break;
        case 0x1004 /* GL_TEXTURE_BORDER */: *params = 0; break;
        default: *params = 0; break;
    }
}

void ps4_glReadBuffer(unsigned mode) { (void)mode; }
void ps4_glDrawBuffer(unsigned mode) { (void)mode; }
void ps4_glPolygonMode(unsigned face, unsigned mode) { (void)face; (void)mode; }

/* No multisampled renderbuffers on ES 2.0: allocate single-sample storage so
 * the attachment is still complete. */
void ps4_glRenderbufferStorageMultisample(unsigned target, int samples,
                                          unsigned internalformat, int width, int height) {
    (void)samples;
    glRenderbufferStorage(target, internalformat, width, height);
}

/* 3D textures, layered attachments and framebuffer blits do not exist here.
 * A no-op leaves the caller with an incomplete attachment, which its own error
 * path handles — a NULL call would take the process down. */
void ps4_glFramebufferTextureLayer(unsigned target, unsigned attachment,
                                   unsigned texture, int level, int layer) {
    (void)target; (void)attachment; (void)texture; (void)level; (void)layer;
}

void ps4_glBlitFramebuffer(int srcX0, int srcY0, int srcX1, int srcY1,
                           int dstX0, int dstY0, int dstX1, int dstY1,
                           unsigned mask, unsigned filter) {
    (void)srcX0; (void)srcY0; (void)srcX1; (void)srcY1;
    (void)dstX0; (void)dstY0; (void)dstX1; (void)dstY1;
    (void)mask; (void)filter;
}

void ps4_glTexImage3D(unsigned target, int level, int internalformat, int width,
                      int height, int depth, int border, unsigned format,
                      unsigned type, const void *pixels) {
    (void)target; (void)level; (void)internalformat; (void)width; (void)height;
    (void)depth; (void)border; (void)format; (void)type; (void)pixels;
}

void *SDL_GL_GetProcAddress(const char *proc) {
    if (proc == NULL) return NULL;
    void *p = (void *)eglGetProcAddress(proc);
    if (p != NULL) return p;
    for (size_t i = 0; i < sizeof(kCoreGL) / sizeof(kCoreGL[0]); i++) {
        if (strcmp(kCoreGL[i].name, proc) == 0) return kCoreGL[i].fn;
    }
    ps4_log("[ps4] GL entry point unresolved: %s", proc);
    return NULL;
}

int SDL_GL_GetDrawableSize(SDL_Window *w, int *wout, int *hout) {
    (void)w;
    if (wout) *wout = s_surface_w;
    if (hout) *hout = s_surface_h;
    return 0;
}

int SDL_GL_MakeCurrent(SDL_Window *w, SDL_GLContext ctx) {
    (void)w;
    if (s_display == EGL_NO_DISPLAY) return -1;
    return eglMakeCurrent(s_display, s_surface, s_surface, (EGLContext)ctx) ? 0 : -1;
}

/* Framebuffer capture (MDKR_CAPTURE): the pixels are read here and written as
 * a plain PPM, which can be pulled over FTP and compared with the desktop. */
static void ps4_capture_ppm(unsigned seq) {
    if (s_surface_w <= 0 || s_surface_h <= 0) {
        return;
    }
    size_t w = (size_t)s_surface_w, h = (size_t)s_surface_h;
    unsigned char *px = (unsigned char *)malloc(w * h * 4);
    if (px == NULL) {
        return;
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, (int)w, (int)h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    GLenum err = glGetError();
    char path[128];
    snprintf(path, sizeof(path), "/data/dkr/shot_%03u.ppm", seq);
    FILE *f = fopen(path, "wb");
    if (f != NULL) {
        fprintf(f, "P6\n%zu %zu\n255\n", w, h);
        for (size_t y = h; y-- > 0; ) {
            for (size_t x = 0; x < w; x++) {
                fwrite(&px[(y * w + x) * 4], 1, 3, f);
            }
        }
        fclose(f);
    }
    free(px);
    ps4_log("[ps4] capture %s (err=0x%x)", path, (unsigned)err);
}

/* Optional instruments, read once from the environment (flags.txt):
 *   MDKR_CAPTURE=1  touching /data/dkr/capture_now takes one PPM shot of the
 *                   next frame (costs ~2.3 s per shot, so never on by default)
 *   MDKR_PERF=1     one fps/frame-time line every five seconds, plus every
 *                   swap that took 50 ms or more
 * With neither set the swap path does nothing but swap. */
static int s_opt_capture = -1;
static int s_opt_perf = -1;

static void ps4_swap_perf(unsigned swaps, long swap_ms) {
    static long s_prev_ms = -1;
    static long s_win_start;
    static unsigned s_win_frames;
    static long s_worst_ms;

    if (swap_ms >= 50) {
        ps4_log("[SLOWSWAP] frame=%u swap=%ldms", swaps, swap_ms);
    }
    long now = ps4_uptime_ms();
    if (s_win_start == 0) s_win_start = now;
    if (s_prev_ms >= 0 && now - s_prev_ms > s_worst_ms) s_worst_ms = now - s_prev_ms;
    s_prev_ms = now;
    s_win_frames++;
    if (now - s_win_start >= 5000) {
        double secs = (double)(now - s_win_start) / 1000.0;
        fprintf(stderr, "[PERF] fps=%.1f frame=%.1fms worst=%ldms\n",
                (double)s_win_frames / secs,
                secs * 1000.0 / (double)(s_win_frames ? s_win_frames : 1),
                s_worst_ms);
        fflush(stderr);
        s_win_start = now;
        s_win_frames = 0;
        s_worst_ms = 0;
    }
}

void SDL_GL_SwapWindow(SDL_Window *w) {
    (void)w;
    static unsigned swaps = 0;
    if (s_display == EGL_NO_DISPLAY || s_surface == EGL_NO_SURFACE) {
        return;
    }
    swaps++;
    if (swaps == 1) {
        /* Compile every program the last session used here, where a stall
         * costs nothing -- instead of the first time each material appears
         * in a race. */
        extern void gfx_gl_shader_warm_cache(void);
        gfx_gl_shader_warm_cache();
        ps4_crash_trail("gl-context-ready");
        s_opt_capture = getenv("MDKR_CAPTURE") != NULL ? 1 : 0;
        s_opt_perf = getenv("MDKR_PERF") != NULL ? 1 : 0;
    }
    if (s_opt_capture && access("/data/dkr/capture_now", F_OK) == 0) {
        unlink("/data/dkr/capture_now");
        ps4_capture_ppm(swaps);
    }

    struct timespec sw0, sw1;
    if (s_opt_perf) clock_gettime(CLOCK_MONOTONIC, &sw0);
    EGLBoolean ok = eglSwapBuffers(s_display, s_surface);
    s_swap_count++;
    /* Hide the launch splash only once frames are actually being flipped,
     * exactly like the working SM64 port does after its third swap. */
    if (s_swap_count == 3) {
        int hide = sceSystemServiceHideSplashScreen();
        ps4_log("[ps4] splash hidden after 3 swaps -> 0x%08X", (unsigned)hide);
    }
    if (s_opt_perf) {
        clock_gettime(CLOCK_MONOTONIC, &sw1);
        ps4_swap_perf(swaps, (sw1.tv_sec - sw0.tv_sec) * 1000L +
                                 (sw1.tv_nsec - sw0.tv_nsec) / 1000000L);
    }
    ps4_crash_snapshot(swaps);
    if (!ok) {
        ps4_log("[ps4] eglSwapBuffers failed #%u eglErr=0x%04X", swaps,
                (unsigned)eglGetError());
    }
}

int SDL_GL_SetSwapInterval(int interval) {
    /* Deliberately ignored, exactly like the working SoH port does it.
     *
     * With a swap interval of 1 eglSwapBuffers() never returns on a real
     * console: Piglet waits for a vertical blank that this presentation path
     * does not deliver. The evidence trail in this port is unambiguous — the
     * start-up probe swaps 40 times cleanly while the interval is still 0, and
     * the very first engine swap after "[PRESENT-MODE] requestedSwap=1" blocks
     * forever. Frame pacing is the engine's own job (it is built around a
     * cooperative frame pacer), so the interval stays 0. */
    if (!s_swap_interval_ignored_logged) {
        s_swap_interval_ignored_logged = 1;
        ps4_log("[ps4] SDL_GL_SetSwapInterval(%d) ignored; Piglet stays at interval 0",
                interval);
    }
    s_swap_interval = 0;
    if (s_display != EGL_NO_DISPLAY) {
        eglSwapInterval(s_display, 0);
    }
    return 0;
}

int SDL_GL_GetSwapInterval(void) { return s_swap_interval; }

/* Metal: the shared sources reference these, Orbis never calls them. */
void *SDL_Metal_CreateView(SDL_Window *w) { (void)w; return NULL; }
void SDL_Metal_DestroyView(void *view) { (void)view; }
void *SDL_Metal_GetLayer(void *view) { (void)view; return NULL; }
int SDL_Metal_GetDrawableSize(SDL_Window *w, int *wout, int *hout) {
    return SDL_GL_GetDrawableSize(w, wout, hout);
}

/* ====================================================================== */
/* input: one Pad, published as a game controller                          */
/* ====================================================================== */
#define PAD_INSTANCE 1

struct SDL_Joystick {
    SDL_JoystickID id;
    int opened;
    Sint16 axis[SDL_CONTROLLER_AXIS_MAX];
    Uint8  button[SDL_CONTROLLER_BUTTON_MAX];
};

struct SDL_GameController {
    SDL_Joystick joy;
    int attached;
    char name[32];
};

static SDL_GameController s_pad;
static int s_pad_open = -1;
static int s_pad_user = -1;
static int s_pad_inited = 0;

#define EVENT_QUEUE_LEN 256
static SDL_Event s_events[EVENT_QUEUE_LEN];
static int s_ev_head = 0, s_ev_tail = 0;
static SDL_mutex *s_ev_mutex = NULL;

static void queue_event(const SDL_Event *e) {
    if (s_ev_mutex) SDL_LockMutex(s_ev_mutex);
    int next = (s_ev_tail + 1) % EVENT_QUEUE_LEN;
    if (next != s_ev_head) {
        s_events[s_ev_tail] = *e;
        s_ev_tail = next;
    }
    if (s_ev_mutex) SDL_UnlockMutex(s_ev_mutex);
}

static int dequeue_event(SDL_Event *out) {
    if (s_ev_mutex) SDL_LockMutex(s_ev_mutex);
    int have = s_ev_head != s_ev_tail;
    if (have) {
        *out = s_events[s_ev_head];
        s_ev_head = (s_ev_head + 1) % EVENT_QUEUE_LEN;
    }
    if (s_ev_mutex) SDL_UnlockMutex(s_ev_mutex);
    return have;
}

static int s_pad_disabled_logged = 0;

/* Reading the pad is deferred out of the startup path.
 *
 * scePadInit/sceUserServiceGetInitialUser/scePadOpen ran inside
 * SDL_GameControllerOpen and killed the process there with no signal and no
 * log line past that point. Input is not needed to prove the renderer works,
 * so the controller is published as neutral and the Orbis pad is opened later
 * (or not at all) — the game then has to be driven by the engine's own default
 * input until the pad path is reintroduced carefully, with
 * sceUserServiceInitialize() ahead of the user query.
 */
static int pad_open(void) {
    /* Every step logs before it runs, so a fault names the call that died
     * instead of the whole sequence. Deliberately does NOT call
     * sceUserServiceInitialize: that is what killed the process in earlier
     * attempts, and the pad can be opened without it. */
    int uid_rc, handle;
    OrbisUserServiceUserId uid = 0;

    /* libScePad has to be in the process before scePadInit; nothing in this
     * port loaded it, which is why the very first call died. The internal
     * module ids are the ones the working ports use (0x80000011 user service,
     * 0x80000024 pad). Each step logs before it runs. */
    ps4_log("[pad] 0a: load internal user service");
    ps4_log("[pad] 0b: module -> 0x%08X",
            (unsigned)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_USER_SERVICE));
    ps4_log("[pad] 0c: load internal pad");
    ps4_log("[pad] 0d: module -> 0x%08X",
            (unsigned)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_PAD));

    ps4_log("[pad] 1: scePadInit");
    ps4_log("[pad] 2: scePadInit rc=0x%08X", (unsigned)scePadInit());

    /* The service has to be initialised after its module is loaded; the earlier
     * attempt crashed because the module was missing, not because of the
     * parameter block (which is a single uint32 priority). */
    {
        OrbisUserServiceInitializeParams usp;
        memset(&usp, 0, sizeof(usp));
        usp.priority = 0x100;   /* SCE_KERNEL_PRIO_FIFO_DEFAULT */
        ps4_log("[pad] 2b: sceUserServiceInitialize(priority=0x%X)", (unsigned)usp.priority);
        ps4_log("[pad] 2c: rc=0x%08X", (unsigned)sceUserServiceInitialize(&usp));
    }

    ps4_log("[pad] 3: sceUserServiceGetInitialUser");
    uid_rc = sceUserServiceGetInitialUser(&uid);
    ps4_log("[pad] 4: GetInitialUser rc=0x%08X uid=0x%08X", (unsigned)uid_rc,
            (unsigned)uid);
    if (uid_rc != 0 || uid == 0) {
        /* The system user is the id the working SM64 audio path uses; on a
         * homebrew app it is the only one that reliably answers. */
        uid = 0xFF;
        ps4_log("[pad] 4b: falling back to system user 0x%08X", (unsigned)uid);
    }

    ps4_log("[pad] 5: scePadOpen(uid=0x%08X)", (unsigned)uid);
    handle = scePadOpen(uid, ORBIS_PAD_PORT_TYPE_STANDARD, 0, NULL);
    ps4_log("[pad] 6: scePadOpen -> 0x%08X", (unsigned)handle);
    if (handle == ORBIS_PAD_ERROR_ALREADY_OPENED) {
        handle = scePadGetHandle(uid, ORBIS_PAD_PORT_TYPE_STANDARD, 0);
        ps4_log("[pad] 7: scePadGetHandle -> 0x%08X", (unsigned)handle);
    }
    if (handle < 0 && uid != 0xFF) {
        ps4_log("[pad] 8: retry with system user");
        handle = scePadOpen(0xFF, ORBIS_PAD_PORT_TYPE_STANDARD, 0, NULL);
        ps4_log("[pad] 9: scePadOpen(0xFF) -> 0x%08X", (unsigned)handle);
    }

    s_pad_open = handle;
    if (handle < 0) {
        ps4_log("[ps4] pad unavailable, controller stays neutral");
        return 0;
    }
    ps4_log("[ps4] pad opened, handle 0x%08X", (unsigned)handle);
    return 1;
}

typedef struct {
    int button;
    int orbis;
} PadButtonMap;

static const PadButtonMap kPadButtons[] = {
    { SDL_CONTROLLER_BUTTON_A, ORBIS_PAD_BUTTON_CROSS },
    { SDL_CONTROLLER_BUTTON_B, ORBIS_PAD_BUTTON_CIRCLE },
    { SDL_CONTROLLER_BUTTON_X, ORBIS_PAD_BUTTON_SQUARE },
    { SDL_CONTROLLER_BUTTON_Y, ORBIS_PAD_BUTTON_TRIANGLE },
    { SDL_CONTROLLER_BUTTON_BACK, ORBIS_PAD_BUTTON_L1 },
    { SDL_CONTROLLER_BUTTON_START, ORBIS_PAD_BUTTON_OPTIONS },
    { SDL_CONTROLLER_BUTTON_LEFTSTICK, ORBIS_PAD_BUTTON_L3 },
    { SDL_CONTROLLER_BUTTON_RIGHTSTICK, ORBIS_PAD_BUTTON_R3 },
    { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, ORBIS_PAD_BUTTON_L2 },
    { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, ORBIS_PAD_BUTTON_R2 },
    { SDL_CONTROLLER_BUTTON_DPAD_UP, ORBIS_PAD_BUTTON_UP },
    { SDL_CONTROLLER_BUTTON_DPAD_DOWN, ORBIS_PAD_BUTTON_DOWN },
    { SDL_CONTROLLER_BUTTON_DPAD_LEFT, ORBIS_PAD_BUTTON_LEFT },
    { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, ORBIS_PAD_BUTTON_RIGHT },
};

static Sint16 scale_axis(uint8_t raw) {
    /* Orbis reports 0..255 with 128 as centre; SDL wants -32768..32767. */
    int v = ((int)raw - 128) * 258;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (Sint16)v;
}

static void publish_axis(int axis, Sint16 value) {
    if (s_pad.joy.axis[axis] == value) return;
    s_pad.joy.axis[axis] = value;
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = SDL_CONTROLLERAXISMOTION;
    e.caxis.timestamp = SDL_GetTicks();
    e.caxis.which = s_pad.joy.id;
    e.caxis.axis = (Uint8)axis;
    e.caxis.value = value;
    queue_event(&e);
    e.type = SDL_JOYAXISMOTION;
    e.jaxis.which = s_pad.joy.id;
    e.jaxis.axis = (Uint8)axis;
    e.jaxis.value = value;
    queue_event(&e);
}

static void publish_button(int button, int pressed) {
    Uint8 state = pressed ? SDL_PRESSED : SDL_RELEASED;
    if (s_pad.joy.button[button] == state) return;
    s_pad.joy.button[button] = state;
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = pressed ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
    e.cbutton.timestamp = SDL_GetTicks();
    e.cbutton.which = s_pad.joy.id;
    e.cbutton.button = (Uint8)button;
    e.cbutton.state = state;
    queue_event(&e);
    e.type = pressed ? SDL_JOYBUTTONDOWN : SDL_JOYBUTTONUP;
    e.jbutton.which = s_pad.joy.id;
    e.jbutton.button = (Uint8)button;
    e.jbutton.state = state;
    queue_event(&e);
}

static int s_pad_published = 0;

void ps4_sdl_backend_pump_pad(void) {
    if (!s_pad_published) {
        s_pad_published = 1;
        if (!pad_open()) {
            /* Publish a neutral controller so the engine's input layer has a
             * device to bind, but never touch libScePad here. */
            s_pad.joy.id = PAD_INSTANCE;
            snprintf(s_pad.name, sizeof(s_pad.name), "PS4 Controller (neutral)");
            s_pad.attached = 1;
            SDL_Event dev;
            memset(&dev, 0, sizeof(dev));
            dev.type = SDL_CONTROLLERDEVICEADDED;
            dev.cdevice.which = 0;
            queue_event(&dev);
            ps4_log("[ps4] controller published (neutral, no pad reads)");
            return;
        }
        s_pad.joy.id = PAD_INSTANCE;
        snprintf(s_pad.name, sizeof(s_pad.name), "PS4 Controller");
        s_pad.attached = 1;
        SDL_Event e;
        memset(&e, 0, sizeof(e));
        e.type = SDL_CONTROLLERDEVICEADDED;
        e.cdevice.which = 0;
        queue_event(&e);
        memset(&e, 0, sizeof(e));
        e.type = SDL_JOYDEVICEADDED;
        e.jdevice.which = 0;
        queue_event(&e);
        ps4_log("[ps4] controller published");
    }
    if (s_pad_open < 0) return;

    OrbisPadData data;
    memset(&data, 0, sizeof(data));
    if (scePadReadState(s_pad_open, &data) != 0 || !data.connected) {
        return;
    }
    for (size_t i = 0; i < sizeof(kPadButtons) / sizeof(kPadButtons[0]); i++) {
        publish_button(kPadButtons[i].button,
                       (data.buttons & kPadButtons[i].orbis) != 0);
    }
    publish_axis(SDL_CONTROLLER_AXIS_LEFTX, scale_axis(data.leftStick.x));
    publish_axis(SDL_CONTROLLER_AXIS_LEFTY, scale_axis(data.leftStick.y));
    publish_axis(SDL_CONTROLLER_AXIS_RIGHTX, scale_axis(data.rightStick.x));
    publish_axis(SDL_CONTROLLER_AXIS_RIGHTY, scale_axis(data.rightStick.y));
    publish_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, scale_axis(data.analogButtons.l2));
    publish_axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, scale_axis(data.analogButtons.r2));
}

/* keyboard state: the engine polls it, the pad is the real input here. */
static Uint8 s_key_state[512];

const Uint8 *SDL_GetKeyboardState(int *numkeys) {
    if (numkeys) *numkeys = (int)sizeof(s_key_state);
    return s_key_state;
}

int SDL_NumJoysticks(void) { return 1; }
SDL_bool SDL_IsGameController(int device_index) { return device_index == 0 ? SDL_TRUE : SDL_FALSE; }
SDL_bool SDL_JoystickIsVirtual(int device_index) { (void)device_index; return SDL_FALSE; }

SDL_GameController *SDL_GameControllerOpen(int device_index) {
    (void)device_index;
    ps4_sdl_backend_pump_pad();
    s_pad.attached = 1;
    return &s_pad;
}

void SDL_GameControllerClose(SDL_GameController *gc) { (void)gc; }
SDL_bool SDL_GameControllerGetAttached(SDL_GameController *gc) {
    return (gc && gc->attached) ? SDL_TRUE : SDL_FALSE;
}
SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *gc) {
    return gc ? &gc->joy : NULL;
}
const char *SDL_GameControllerName(SDL_GameController *gc) {
    return gc ? gc->name : "PS4 Controller";
}
SDL_bool SDL_GameControllerHasRumble(SDL_GameController *gc) { (void)gc; return SDL_TRUE; }

int SDL_GameControllerRumble(SDL_GameController *gc, Uint16 low, Uint16 high, Uint32 ms) {
    (void)gc; (void)low; (void)high; (void)ms;
    return 0;
}

int SDL_GameControllerAddMappingsFromFile(const char *file) { (void)file; return 0; }

SDL_Joystick *SDL_JoystickFromInstanceID(SDL_JoystickID id) {
    return id == s_pad.joy.id ? &s_pad.joy : NULL;
}

SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick *joy) { return joy ? joy->id : -1; }
SDL_JoystickID SDL_JoystickGetDeviceInstanceID(int device_index) {
    (void)device_index;
    return s_pad.joy.id;
}

SDL_JoystickGUID SDL_JoystickGetGUID(SDL_Joystick *joy) {
    (void)joy;
    SDL_JoystickGUID g;
    memset(&g, 0, sizeof(g));
    g.data[0] = 0x03;   /* pretend to be a HID gamepad */
    return g;
}

void SDL_JoystickGetGUIDString(SDL_JoystickGUID guid, char *out, int cb) {
    if (out == NULL || cb <= 0) return;
    static const char hex[] = "0123456789abcdef";
    int n = cb / 3;
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) {
        out[i * 3 + 0] = hex[(guid.data[i] >> 4) & 0xF];
        out[i * 3 + 1] = hex[guid.data[i] & 0xF];
        out[i * 3 + 2] = (i == n - 1) ? '\0' : ':';
    }
    out[cb - 1] = '\0';
}

int SDL_JoystickAttachVirtual(int type, int naxes, int nbuttons, int nhats) {
    (void)type; (void)naxes; (void)nbuttons; (void)nhats;
    return 0;
}
int SDL_JoystickDetachVirtual(int device_index) { (void)device_index; return 0; }

int SDL_JoystickSetVirtualAxis(SDL_Joystick *joy, int axis, Sint16 value) {
    if (joy == NULL || axis < 0 || axis >= SDL_CONTROLLER_AXIS_MAX) return -1;
    publish_axis(axis, value);
    return 0;
}

int SDL_JoystickSetVirtualButton(SDL_Joystick *joy, int button, Uint8 value) {
    if (joy == NULL || button < 0 || button >= SDL_CONTROLLER_BUTTON_MAX) return -1;
    publish_button(button, value == SDL_PRESSED);
    return 0;
}

int SDL_JoystickSetVirtualHat(SDL_Joystick *joy, int hat, Uint8 value) {
    (void)joy; (void)hat; (void)value;
    return 0;
}

int SDL_PushEvent(SDL_Event *e) {
    if (e == NULL) return -1;
    queue_event(e);
    return 1;
}

void SDL_PumpEvents(void) { ps4_sdl_backend_pump_pad(); }

int SDL_PollEvent(SDL_Event *e) {
    if (e == NULL) return 0;
    ps4_sdl_backend_pump_pad();
    if (dequeue_event(e)) return 1;
    e->type = 0;
    return 0;
}

int SDL_WaitEventTimeout(SDL_Event *e, int timeout) {
    Uint32 start = SDL_GetTicks();
    for (;;) {
        if (SDL_PollEvent(e)) return 1;
        if (timeout >= 0 && (int)(SDL_GetTicks() - start) >= timeout) return 0;
        SDL_Delay(1);
    }
}

/* ====================================================================== */
/* audio: libSceAudioOut at 48 kHz stereo S16                              */
/* ====================================================================== */
#define AUDIO_RATE_HW    48000
#define AUDIO_FRAMES_HW  256
#define AUDIO_CHANNELS   2

typedef struct {
    int open;
    int paused;
    int freq;            /* requested rate */
    SDL_AudioSpec spec;
    int32_t handle;      /* int32_t on purpose: error codes are negative */
    SDL_Thread *thread;
    int running;
    SDL_mutex *lock;
    float resample_pos;
    int16_t *src_buf;    /* one callback round, converted to S16 stereo */
    int src_frames;
    int16_t hw_buf[AUDIO_FRAMES_HW * AUDIO_CHANNELS];
} Ps4Audio;

static Ps4Audio s_audio;

static void convert_to_s16(const Uint8 *src, int16_t *dst, int frames, const SDL_AudioSpec *spec) {
    /* Only the conversions the engine can request: S16 stereo (pass-through),
     * U8 stereo and F32 stereo. Everything else falls back to silence. */
    const int ch = spec->channels > 0 ? spec->channels : 2;
    for (int i = 0; i < frames; i++) {
        for (int c = 0; c < AUDIO_CHANNELS; c++) {
            int sc = c < ch ? c : ch - 1;
            float v = 0.0f;
            switch (spec->format) {
                case AUDIO_S16LSB: {
                    const int16_t *s16 = (const int16_t *)src;
                    v = (float)s16[i * ch + sc] / 32768.0f;
                    break;
                }
                case AUDIO_U8: {
                    v = ((float)src[i * ch + sc] - 128.0f) / 128.0f;
                    break;
                }
                case AUDIO_F32LSB: {
                    const float *f32 = (const float *)src;
                    v = f32[i * ch + sc];
                    break;
                }
                default:
                    v = 0.0f;
                    break;
            }
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            dst[i * AUDIO_CHANNELS + c] = (int16_t)(v * 32767.0f);
        }
    }
}

/* Correct streaming resampler. The previous version filled up to 256 output
 * frames and then broke out mid-block when the source round ran out, leaving
 * the tail of the block filled with the previous samples -- heard as crackle
 * and distortion. This one keeps a source FIFO, refills it from the callback
 * whenever the next block needs more, always emits a complete block, and keeps
 * the fractional read position across blocks. */
#define AFIFO_FRAMES 16384
static int16_t s_fifo[AFIFO_FRAMES * AUDIO_CHANNELS];
static int s_fifo_frames;      /* frames currently in the FIFO */
static double s_read_pos;      /* fractional read position, frames */

static int audio_thread(void *arg) {
    Ps4Audio *a = (Ps4Audio *)arg;
    int cb_frames = a->spec.samples > 0 ? a->spec.samples : 1024;
    const int src_ch = a->spec.channels > 0 ? a->spec.channels : 2;
    const int src_bps = (a->spec.format == AUDIO_F32LSB) ? 4 : 2;
    const size_t scratch_bytes = (size_t)cb_frames * (size_t)src_ch * (size_t)src_bps;
    Uint8 *scratch = (Uint8 *)malloc(scratch_bytes);
    int16_t *conv = (int16_t *)malloc((size_t)cb_frames * AUDIO_CHANNELS * sizeof(int16_t));
    const double step = (double)a->freq / (double)AUDIO_RATE_HW;

    if (scratch == NULL || conv == NULL) {
        free(scratch);
        free(conv);
        ps4_log("[audio] thread out of memory");
        return 0;
    }
    s_fifo_frames = 0;
    s_read_pos = 0.0;

    while (a->running) {
        if (a->paused || !a->spec.callback) {
            SDL_Delay(5);
            continue;
        }
        SDL_AudioSpec spec = a->spec;

        /* Make sure the FIFO holds every source frame this block will read. */
        int needed = (int)(s_read_pos + step * (double)AUDIO_FRAMES_HW) + 2;
        int guard = 0;
        while (s_fifo_frames < needed && s_fifo_frames < AFIFO_FRAMES && guard++ < 64) {
            spec.callback(spec.userdata, scratch, (Uint32)scratch_bytes);
            convert_to_s16(scratch, conv, cb_frames, &spec);
            int room = AFIFO_FRAMES - s_fifo_frames;
            int take = cb_frames < room ? cb_frames : room;
            memcpy(&s_fifo[s_fifo_frames * AUDIO_CHANNELS], conv,
                   (size_t)take * AUDIO_CHANNELS * sizeof(int16_t));
            s_fifo_frames += take;
            if (take < cb_frames) break;   /* FIFO full: play it down first */
        }

        /* Emit one complete hardware block. */
        for (int out = 0; out < AUDIO_FRAMES_HW; out++) {
            double pos = s_read_pos + step * (double)out;
            int i0 = (int)pos;
            if (i0 < 0) i0 = 0;
            if (i0 >= s_fifo_frames) i0 = s_fifo_frames - 1;
            int i1 = (i0 + 1 < s_fifo_frames) ? i0 + 1 : i0;
            double frac = pos - (double)i0;
            if (frac < 0.0) frac = 0.0;
            if (frac > 1.0) frac = 1.0;
            for (int c = 0; c < AUDIO_CHANNELS; c++) {
                double s0 = s_fifo[i0 * AUDIO_CHANNELS + c];
                double s1 = s_fifo[i1 * AUDIO_CHANNELS + c];
                a->hw_buf[out * AUDIO_CHANNELS + c] = (int16_t)(s0 + (s1 - s0) * frac);
            }
        }
        s_read_pos += step * (double)AUDIO_FRAMES_HW;

        /* Drop the integer part that has been consumed. */
        int drop = (int)s_read_pos;
        if (drop > 0) {
            if (drop > s_fifo_frames) drop = s_fifo_frames;
            memmove(s_fifo, &s_fifo[drop * AUDIO_CHANNELS],
                    (size_t)(s_fifo_frames - drop) * AUDIO_CHANNELS * sizeof(int16_t));
            s_fifo_frames -= drop;
            s_read_pos -= (double)drop;
        }

        sceAudioOutOutput(a->handle, a->hw_buf);
    }

    free(scratch);
    free(conv);
    return 0;
}

SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int iscapture,
                                      const SDL_AudioSpec *desired,
                                      SDL_AudioSpec *obtained, int allowed_changes) {
    (void)device; (void)iscapture; (void)allowed_changes;
    if (desired == NULL) return 0;

    /* Bring-up staging: the renderer comes first. libSceAudioOut is brought up
     * only after a frame has been presented, because a crash there would hide
     * whether the picture works — and the same class of call (a libkernel
     * service touched before it is ready) already killed the input path.
     * The device is reported as valid and stays silent. */
    memset(&s_audio, 0, sizeof(s_audio));
    s_audio.spec = *desired;
    s_audio.freq = desired->freq > 0 ? desired->freq : 32000;
    s_audio.handle = -1;

    if (obtained != NULL) {
        *obtained = *desired;
        obtained->format = AUDIO_S16LSB;
        obtained->channels = AUDIO_CHANNELS;
    }

    /* libSceAudioOut has to be in the process before sceAudioOutInit, the same
     * way libScePad had to be for the controller. Every step logs its result:
     * the audio path is the last subsystem that has never run, and a silent
     * device would otherwise look the same as a broken one. */
    ps4_log("[audio] 1: load module");
    ps4_log("[audio] 2: module -> 0x%08X",
            (unsigned)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_AUDIOOUT));
    ps4_log("[audio] 3: sceAudioOutInit");
    ps4_log("[audio] 4: init -> 0x%08X", (unsigned)sceAudioOutInit());

    /* Verified on this firmware in the SM64 port: the MAIN port only opens for
     * the SYSTEM user 0xFF. Passing the initial user (0x10000000) fails with
     * 0x809B0001, which reads like "no audio" but is an invalid user id. The
     * initial user is only tried afterwards, as a fallback. */
    {
        s_audio.handle = sceAudioOutOpen(0xFF, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0,
                                         AUDIO_FRAMES_HW, AUDIO_RATE_HW,
                                         ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
        ps4_log("[audio] 5: open(uid=0xFF) -> 0x%08X", (unsigned)s_audio.handle);
        if (s_audio.handle < 0) {
            OrbisUserServiceUserId uid = 0;
            if (sceUserServiceGetInitialUser(&uid) == 0 && uid != 0) {
                s_audio.handle = sceAudioOutOpen(uid, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0,
                                                 AUDIO_FRAMES_HW, AUDIO_RATE_HW,
                                                 ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
                ps4_log("[audio] 5b: retry uid=0x%08X -> 0x%08X",
                        (unsigned)uid, (unsigned)s_audio.handle);
            }
        }
    }
    ps4_crash_trail("audio-open");
    ps4_log("[audio] 6: handle -> 0x%08X", (unsigned)s_audio.handle);

    s_audio.open = 1;
    s_audio.paused = 1;      /* SDL_PauseAudioDevice(dev, 0) starts playback */
    if (s_audio.handle >= 0) {
        s_audio.lock = SDL_CreateMutex();
        s_audio.running = 1;
        s_audio.thread = SDL_CreateThread(audio_thread, "ps4audio", &s_audio);
        ps4_log("[audio] 7: thread -> %s",
                s_audio.thread != NULL ? "started" : "FAILED");
    } else {
        ps4_log("[audio] device unavailable, staying silent");
    }
    return 1;
}

void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on) {
    (void)dev;
    s_audio.paused = pause_on;
}

void SDL_CloseAudioDevice(SDL_AudioDeviceID dev) {
    (void)dev;
    if (!s_audio.open) return;
    s_audio.running = 0;
    if (s_audio.thread) SDL_WaitThread(s_audio.thread, NULL);
    s_audio.thread = NULL;
    if (s_audio.handle >= 0) sceAudioOutClose(s_audio.handle);
    free(s_audio.src_buf);
    s_audio.src_buf = NULL;
    s_audio.open = 0;
}

void SDL_LockAudioDevice(SDL_AudioDeviceID dev) { (void)dev; if (s_audio.lock) SDL_LockMutex(s_audio.lock); }
void SDL_UnlockAudioDevice(SDL_AudioDeviceID dev) { (void)dev; if (s_audio.lock) SDL_UnlockMutex(s_audio.lock); }
Uint32 SDL_GetQueuedAudioSize(SDL_AudioDeviceID dev) { (void)dev; return 0; }
void SDL_ClearQueuedAudio(SDL_AudioDeviceID dev) { (void)dev; }

void ps4_install_fault_handler(void) { }

static void ps4_heartbeat_start(void) { }

