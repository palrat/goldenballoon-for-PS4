/*
 * ps4_entry.c — PS4 entry point.
 *
 * The engine already has a native entry (platform/main_pc.c). Under -DMDKR_APP
 * that file exposes mdkr64_headless_main() instead of main(), because the
 * desktop shell owns main(); on a packaged PS4 app nobody owns it, so this file
 * supplies the arguments and hands over.
 *
 * Done before anything else runs:
 *
 *   1. stderr/stdout are redirected into /data/dkr/ps4_boot.log. There is no
 *      terminal on the console, and every layer below logs there.
 *   2. The ROM is found without any setup beyond copying one file: rom.txt,
 *      then /data/dkr/dkr.z64, then any 12 MB .z64/.v64/.n64 in /data/dkr,
 *      then the root (or a dkr/ folder) of a USB stick, which is copied to
 *      /data/dkr/dkr.z64 so the stick is needed only once. The Piglet/Shacc
 *      system modules are taken from the stick the same way, because copying
 *      them over FTP corrupts them. Every failure is shown as a PS4
 *      notification, not only written to the log.
 *   3. /data/dkr/flags.txt can set environment variables (a packaged app has
 *      no other way to receive them), e.g. MDKR_TRACE=2 + MDKR_DL_FRAME=<n>
 *      for a per-draw trace to diff against the desktop, MDKR_CAPTURE /
 *      MDKR_PERF for sdl_ps4.c.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <orbis/libkernel.h>
#include "SDL.h"

#define DATA_DIR      "/data/dkr"
#define LOG_PATH      "/data/dkr/ps4_boot.log"
#define MODULE_DIR    "/data/self/system/common/lib"
#define ROM_SIZE      (12L * 1024 * 1024)
#define DEFAULT_ROM   "/data/dkr/dkr.z64"
#define ROM_OVERRIDE  "/data/dkr/rom.txt"

int mdkr64_headless_main(int argc, char **argv);

/* A SIGSEGV inside a GL entry point that Piglet does not have is an indirect
 * call to address 0; logging si_addr separates that from a genuine fault in
 * game code. Written with open/write so the handler does not depend on stdio
 * state, which may be exactly what died. */
static void ps4_crash_handler(int sig, siginfo_t *info, void *ucontext) {
    (void)ucontext;
    int fd = open("/data/dkr/crash.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        char buf[192];
        int n = snprintf(buf, sizeof(buf),
                         "signal=%d addr=%p (0 means a call through a NULL pointer)\n",
                         sig, info != NULL ? info->si_addr : (void *)0);
        if (n > 0) {
            ssize_t written = write(fd, buf, (size_t)n);
            (void)written;
        }
        /* There is no stack unwind to be had: OpenOrbis' ucontext is opaque, so
         * the register set of the faulting context is not reachable from here.
         * The frame/draw snapshot and the breadcrumb ring answer the same
         * question ("where was it") without one. */
        {
            extern void ps4_crash_dump(int fd);
            ps4_crash_dump(fd);
        }
        close(fd);
    }
    _exit(1);
}

static void install_crash_handler(void) {
    /* A stack overflow raises SIGSEGV with no usable stack, so a handler
     * without an alternate stack dies silently — which is one of the two shapes
     * this port has been dying in. */
#ifndef SIGSTKSZ
#define SIGSTKSZ 32768
#endif
    static char alt_stack[SIGSTKSZ * 4];
    stack_t ss;
    ss.ss_sp = alt_stack;
    ss.ss_size = sizeof(alt_stack);
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    /* Direct union member: the header's sa_sigaction macro is not always
     * expanded in this TU. */
    sa.__sa_handler.__sa_sigaction = ps4_crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

static void redirect_log(void) {
    /* freopen keeps the same FILE objects, so every fprintf(stderr, ...) in the
     * engine and the platform shim lands in the file without changes. */
    FILE *f = freopen(LOG_PATH, "w", stderr);
    if (f != NULL) {
        setvbuf(stderr, NULL, _IOLBF, 0);
    }
    freopen(LOG_PATH, "a", stdout);
    /* Block-buffered stdout loses every printf the moment the process dies —
     * which is precisely the case this log exists for. */
    setvbuf(stdout, NULL, _IOLBF, 0);
}

static int read_rom_override(char *out, size_t cap) {
    FILE *f = fopen(ROM_OVERRIDE, "r");
    if (f == NULL) return 0;
    if (fgets(out, (int)cap, f) == NULL) {
        fclose(f);
        return 0;
    }
    fclose(f);
    size_t n = strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ')) {
        out[--n] = '\0';
    }
    return n > 0;
}

/* Build identity in the log itself. Without this the banner was a constant, so
 * an old package and a new one looked identical in the log — and the only way
 * to tell them apart was hashing the installed package, which costs the user
 * half a minute per cycle. */
#ifdef MDKR_BUILD_STAMP
#define BUILD_TAG MDKR_BUILD_STAMP
#else
#define BUILD_TAG "dev"
#endif

/* Flags from a file instead of the environment: a packaged app has no way to
 * set variables, and one build per experiment costs a full install cycle. With
 * /data/dkr/flags.txt read at startup, several hypotheses can be tried on the
 * console by editing one file and relaunching. */
static void load_flag_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return;
    }
    char line[256];
    int applied = 0;
    while (fgets(line, (int)sizeof(line), f) != NULL) {
        char *hash = strchr(line, '#');
        if (hash != NULL) *hash = '\0';
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' || line[len-1] == ' ')) {
            line[--len] = '\0';
        }
        char *eq = strchr(line, '=');
        if (eq == NULL || line[0] == '\0') continue;
        *eq = '\0';
        setenv(line, eq + 1, 1);
        applied++;
        fprintf(stderr, "[flags] %s=%s\n", line, eq + 1);
    }
    fclose(f);
    fprintf(stderr, "[flags] %d flag(s) from %s\n", applied, path);
}

/* A notification is the only thing a player sees: the log needs FTP to read. */
static void ps4_notify(const char *text) {
    OrbisNotificationRequest req;
    memset(&req, 0, sizeof(req));
    req.type = NotificationRequest;
    req.targetId = -1;
    snprintf(req.message, sizeof(req.message), "%s", text);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
    fprintf(stderr, "[ps4] notify: %s\n", text);
    fflush(stderr);
}

static long file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) ? (long)st.st_size : -1;
}

static void mkdir_p(const char *path) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p != '\0'; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0777); *p = '/'; }
    }
    mkdir(tmp, 0777);
}

static int copy_file(const char *src, const char *dst) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.part", dst);
    FILE *in = fopen(src, "rb");
    if (in == NULL) return 0;
    FILE *out = fopen(tmp, "wb");
    if (out == NULL) { fclose(in); return 0; }
    static char buf[1 << 16];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
    }
    if (ferror(in)) ok = 0;
    fclose(in);
    if (fclose(out) != 0) ok = 0;
    if (!ok || rename(tmp, dst) != 0) { unlink(tmp); return 0; }
    return 1;
}

static int is_rom_name(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot != NULL && (strcasecmp(dot, ".z64") == 0 || strcasecmp(dot, ".v64") == 0 ||
                           strcasecmp(dot, ".n64") == 0);
}

/* First 12 MB ROM image in dir (hidden files skipped: macOS writes ._name). */
static int find_rom_in(const char *dir, char *out, size_t cap) {
    DIR *d = opendir(dir);
    if (d == NULL) return 0;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || !is_rom_name(e->d_name)) continue;
        snprintf(out, cap, "%s/%s", dir, e->d_name);
        found = file_size(out) == ROM_SIZE;
    }
    closedir(d);
    return found;
}

/* USB sticks mount at /mnt/usb0../mnt/usb7; look in the root and in dkr/. */
static int find_on_usb(const char *name, char *out, size_t cap) {
    for (int i = 0; i < 8; i++) {
        char dir[64];
        snprintf(dir, sizeof(dir), "/mnt/usb%d", i);
        if (name == NULL) {
            if (find_rom_in(dir, out, cap)) return 1;
            snprintf(dir, sizeof(dir), "/mnt/usb%d/dkr", i);
            if (find_rom_in(dir, out, cap)) return 1;
        } else {
            snprintf(out, cap, "%s/%s", dir, name);
            if (file_size(out) > 0) return 1;
            snprintf(out, cap, "%s/dkr/%s", dir, name);
            if (file_size(out) > 0) return 1;
        }
    }
    return 0;
}

/* Returns 1 with the ROM path in rom; *copied is set when it came off a stick. */
static int locate_rom(char *rom, size_t cap, int *copied) {
    *copied = 0;
    if (read_rom_override(rom, cap) && file_size(rom) > 0) return 1;
    snprintf(rom, cap, "%s", DEFAULT_ROM);
    if (file_size(rom) > 0) return 1;
    if (find_rom_in(DATA_DIR, rom, cap)) return 1;
    char usb[512];
    if (find_on_usb(NULL, usb, sizeof(usb))) {
        fprintf(stderr, "[ps4] copying ROM from %s\n", usb);
        ps4_notify("Diddy Kong Racing: copying the ROM from the USB stick ...");
        if (copy_file(usb, DEFAULT_ROM)) {
            snprintf(rom, cap, "%s", DEFAULT_ROM);
            *copied = 1;
            return 1;
        }
        ps4_notify("Diddy Kong Racing: could not copy the ROM from the USB stick.");
        return 0;
    }
    return 0;
}

/* Piglet (Sony's OpenGL ES) needs these two system modules. FTP uploads break
 * them, so take them from a USB stick when they are not installed yet. */
static void install_modules_from_usb(void) {
    static const char *const kModules[] = { "libScePigletv2VSH.sprx", "libSceShaccVSH.sprx" };
    for (size_t i = 0; i < sizeof(kModules) / sizeof(kModules[0]); i++) {
        char dst[256], src[512];
        snprintf(dst, sizeof(dst), "%s/%s", MODULE_DIR, kModules[i]);
        if (file_size(dst) > 4096) continue;
        if (!find_on_usb(kModules[i], src, sizeof(src))) continue;
        mkdir_p(MODULE_DIR);
        fprintf(stderr, "[ps4] installing %s from %s: %s\n", kModules[i], src,
                copy_file(src, dst) ? "ok" : "FAILED");
    }
}

static int modules_present(void) {
    return file_size(MODULE_DIR "/libScePigletv2VSH.sprx") > 4096 &&
           file_size(MODULE_DIR "/libSceShaccVSH.sprx") > 4096;
}

int main(void) {
    mkdir(DATA_DIR, 0777);
    redirect_log();
    install_crash_handler();

    /* The engine installs its own SIGSEGV handler before gfx init; that one
     * uses a backtrace that Orbis does not provide and logs through a dead
     * buffer. Keep ours. */
    /* The sine/arctan tables are filled by a constructor on the desktop. In
     * this build that constructor never exists: clang -cc1 runs without
     * -fgnuc-version, PRinternal/macros.h then defines __attribute__(x) away,
     * and .init_array in mdkr64.elf is empty (size 0). Without the tables every
     * camera basis is a zero matrix (world invisible, HUD fine), so fill them
     * explicitly. */
    {
        extern void mdkr_native_init_math_tables(void);
        mdkr_native_init_math_tables();
    }
    /* Compile the previous session's shader programs during start-up rather
     * than the first time each material appears. */
    setenv("MDKR_SHADER_WARM", "1", 0);

    load_flag_file("/data/dkr/flags.txt");
    fflush(stderr);

    fprintf(stderr, "=== mdkr64 (PS4) build " BUILD_TAG " ===\n");
    install_modules_from_usb();
    if (!modules_present()) {
        /* Not fatal by itself: some firmwares may resolve the modules from the
         * system path. If the start fails, the message below names them. */
        fprintf(stderr, "[ps4] Piglet/Shacc modules not found in " MODULE_DIR "\n");
    }

    static char rom[512];
    int rom_copied = 0;
    if (!locate_rom(rom, sizeof(rom), &rom_copied)) {
        ps4_notify("Diddy Kong Racing: no ROM found. Put your ROM (.z64, .v64 or .n64, "
                   "12 MB) on a USB stick and start the game again.");
        return 1;
    }
    fprintf(stderr, "[ps4] rom=%s size=%ld bytes\n", rom, file_size(rom));
    fflush(stderr);

    char *argv[] = {
        (char *)"mdkr64",
        (char *)"--rom",
        rom,
        (char *)"--window-size",
        /* 16:9, exactly like the desktop. The race camera's 1920x1440
         * viewport at y=-180 inside a 1080-high frame is DKR's own framing
         * with Hor+ (the desktop at 1920x1080 emits the identical
         * G_MOVEMEM VIEWPORT), not an error. The 1440x1080 window used before
         * cut the sides off and pillarboxed the picture. */
        (char *)"1920x1080",
        /* RenderScale 1: supersampling needs the scene target, which is off
         * on ES 2.0 (gfx_opengl_scene_target_enabled), so a higher scale has
         * nothing to resolve into. */
        (char *)"--video-set",
        (char *)"Video.RenderScale=1",
        NULL
    };
    /* Counted, not written down: a literal 5 here silently dropped the
     * --video-set pair for every build up to 03.05. */
    int argc = (int)(sizeof(argv) / sizeof(argv[0])) - 1;
    int rc = mdkr64_headless_main(argc, argv);
    fprintf(stderr, "[ps4] engine returned %d\n", rc);
    fflush(stderr);
    if (rc != 0) {
        if (!modules_present()) {
            ps4_notify("Diddy Kong Racing could not start: libScePigletv2VSH.sprx and "
                       "libSceShaccVSH.sprx are missing. Put them on the USB stick.");
        } else {
            ps4_notify("Diddy Kong Racing could not start. Check the ROM: it must be the "
                       "US or European version, Rev A (12 MB). Details: /data/dkr/ps4_boot.log");
        }
        /* A ROM copied off the stick in this run is the likely culprit; remove it
         * so the next start looks at the stick again instead of reusing it. */
        if (rom_copied) unlink(DEFAULT_ROM);
    }
    return rc;
}
