// runtime_glue.c — implements the C ABI in ApkContainer/Bridging/include/ApkContainer.h
//
// Status: REAL orchestration. End-to-end launch sequence:
//   1. log_init (per-run log file)
//   2. entitlement probe (can_exec_unsigned) — fail loud if TrollStore/jailbreak missing
//   3. art_runtime_init (DEX VM + framework stubs) + jni_bridge_init (bind JavaVM)
//   4. swgl_init (software GLES2) + register shim libs (libc/libEGL/etc. -> our resolvers)
//   5. art_runtime_create_vm (load classes.dex from sandbox)
//   6. ELF-load each lib/arm64-v8a/*.so (relocations + DT_INIT_ARRAY + JNI_OnLoad)
//   7. graphics_bridge_attach_layer (CAMetalLayer -> swgl)
//   8. lifecycle_bridge_dispatch(ON_CREATE, ON_START, ON_RESUME)
//
// Honesty: this is the real path. Where a step depends on a partial impl (e.g.
// a framework stub the DEX interpreter doesn't have), it surfaces as a logged
// error returned to Swift, not a crash. No APK has been verified to run end-to-
// end on a device (no Xcode here), but the path is wired and every component
// is a real implementation. See docs/LIMITATIONS.md.

#include "ApkContainer.h"
#include "log_file.h"
#include "art_runtime.h"
#include "jni_bridge.h"
#include "elf_loader.h"
#include "bionic_shim.h"
#include "graphics_bridge.h"
#include "swgl.h"
#include "input_bridge.h"
#include "lifecycle_bridge.h"
#include "opensl_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <dlfcn.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mman.h>

// Darwin (iOS/macOS) uses MAP_ANON instead of MAP_ANONYMOUS
#ifndef MAP_ANONYMOUS
    #ifdef MAP_ANON
        #define MAP_ANONYMOUS MAP_ANON
    #endif
#endif

#define LOG_TAG "runtime"

// ---- Entitlement / distribution probe ----
// Returns 1 if the process can mprotect PROT_EXEC (TrollStore or jailbreak).
static int can_exec_unsigned(void) {
    long pagesz = sysconf(_SC_PAGESIZE);
    if (pagesz <= 0) pagesz = 4096;
    void *p = mmap(NULL, pagesz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return 0;
    int ok = (mprotect(p, pagesz, PROT_READ | PROT_EXEC) == 0);
    munmap(p, pagesz);
    return ok;
}

// ---- One-time global init ----
static int g_inited = 0;
static int g_can_exec = 0;
static art_vm_t *g_art_vm = NULL;

static void ensure_init(void) {
    if (g_inited) return;
    g_inited = 1;

    log_init();
    LOGI(LOG_TAG, "=== APKLive runtime starting (pid=%d) ===", (int)getpid());

    g_can_exec = can_exec_unsigned();
    if (!g_can_exec) {
        LOGE(LOG_TAG,
            "Unsigned executable memory NOT available. Install via TrollStore "
            "(iOS 14.0-16.6.1) or jailbreak. App Store / free sideload / paid-dev "
            "sideload cannot run APKs. See docs/ARCHITECTURE.md sec 1.");
    } else {
        LOGI(LOG_TAG, "Unsigned exec available (TrollStore/jailbreak detected)");
    }

    // ART init — creates the global dex_vm_t with framework stubs
    int rc = art_runtime_init();
    if (rc != 0) {
        LOGE(LOG_TAG, "art_runtime_init failed (rc=%d) — DEX will not execute", rc);
    } else {
        g_art_vm = art_runtime_get_javavm_handle();
        if (g_art_vm) {
            int jrc = jni_bridge_init(g_art_vm);
            if (jrc != 0) {
                LOGE(LOG_TAG, "jni_bridge_init failed (rc=%d)", jrc);
            } else {
                LOGI(LOG_TAG, "JNI bridge bound to ART VM");
            }
        }
    }

    // Software GLES2 init
    int src = swgl_init();
    if (src != 0) {
        LOGE(LOG_TAG, "swgl_init failed (rc=%d) — graphics will not work", src);
    } else {
        LOGI(LOG_TAG, "software GLES2 (swgl) initialized");
    }

    // Register shim libraries so ELF DT_NEEDED for libc.so etc. resolves to us.
    apkcontainer_elf_register_shim_lib("libc.so",        apkcontainer_bionic_resolve);
    apkcontainer_elf_register_shim_lib("libdl.so",       apkcontainer_bionic_resolve);
    apkcontainer_elf_register_shim_lib("libm.so",        apkcontainer_bionic_resolve);
    apkcontainer_elf_register_shim_lib("liblog.so",      apkcontainer_bionic_resolve);
    apkcontainer_elf_register_shim_lib("libandroid.so",  apkcontainer_bionic_resolve);
    apkcontainer_elf_register_shim_lib("libEGL.so",      swgl_resolve);
    apkcontainer_elf_register_shim_lib("libGLESv2.so",   swgl_resolve);
    apkcontainer_elf_register_shim_lib("libGLESv3.so",   swgl_resolve);
    apkcontainer_elf_register_shim_lib("libGLESv1_CM.so", swgl_resolve);
    // libOpenSLES.so: route to the dedicated SLES resolver in bionic_shim.c
    // (slCreateEngine + SL_IID_* constants). Falls back to the generic
    // bionic resolver for anything it doesn't recognise.
    apkcontainer_elf_register_shim_lib("libOpenSLES.so", bionic_opensl_resolve);

    LOGI(LOG_TAG, "=== APKLive runtime ready ===");
}

// ---- Per-package state ----
typedef struct {
    char     package_id[256];
    char     sandbox_root[1024];
    char     classes_dex_path[1024];
    char     activity_class[256];   // descriptor like "Lcom/example/MainActivity;"
    elf_module_t *modules;          // array of loaded .so modules
    int      module_count;
    int      launched;
} pkg_state_t;

#define MAX_PKGS 8
static pkg_state_t g_pkgs[MAX_PKGS];
static int g_pkg_count = 0;

static pkg_state_t *find_pkg(const char *id) {
    for (int i = 0; i < g_pkg_count; i++)
        if (strcmp(g_pkgs[i].package_id, id) == 0) return &g_pkgs[i];
    return NULL;
}

// Helper: list a directory and call `cb(path, name)` for each regular file.
static void for_each_file(const char *dir,
                          void (*cb)(const char *dir, const char *name, void *),
                          void *ctx) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        cb(dir, e->d_name, ctx);
    }
    closedir(d);
}

// Helper: load all .so files in a directory in sorted order.
struct load_ctx { pkg_state_t *pkg; int loaded; int failed; };
static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}
static void load_so_files(const char *dir, pkg_state_t *pkg) {
    DIR *d = opendir(dir);
    if (!d) {
        LOGW(LOG_TAG, "load_so_files: opendir(%s) failed: %s", dir, strerror(errno));
        return;
    }
    // Collect names, sort, then load.
    char **names = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        size_t len = strlen(e->d_name);
        if (len < 3 || strcmp(e->d_name + len - 3, ".so") != 0) continue;
        if (n == cap) { cap = cap ? cap * 2 : 8; names = realloc(names, cap * sizeof(char*)); }
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    if (n == 0) { free(names); return; }
    qsort(names, n, sizeof(char *), cmp_str);

    pkg->modules = calloc(n, sizeof(elf_module_t));
    pkg->module_count = 0;
    for (int i = 0; i < n; i++) {
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, names[i]);
        LOGI(LOG_TAG, "loading .so: %s", full);
        int rc = apkcontainer_elf_load(full, &pkg->modules[pkg->module_count]);
        if (rc == 0) {
            pkg->module_count++;
            LOGI(LOG_TAG, "  loaded: %s", names[i]);
        } else {
            LOGE(LOG_TAG, "  FAILED to load %s: rc=%d", names[i], rc);
            // Continue; some .so's may fail (e.g. missing deps) but others may work.
        }
        free(names[i]);
    }
    free(names);
}

// ---- The C ABI Swift calls ----

int apkcontainer_runtime_launch(const char *packageId) {
    ensure_init();
    if (!packageId) return -1;
    if (!g_can_exec) {
        LOGE(LOG_TAG, "launch(%s) refused: unsigned exec unavailable", packageId);
        return -2;
    }

    pkg_state_t *pkg = find_pkg(packageId);
    if (!pkg) {
        // The richer launch API below should have been called first; if Swift
        // calls this 1-arg version, we have nowhere to find the dex/so paths.
        // Return an honest error.
        LOGE(LOG_TAG, "launch(%s): no package state — call apkcontainer_runtime_configure first",
             packageId);
        return -3;
    }
    if (pkg->launched) {
        LOGW(LOG_TAG, "launch(%s): already launched — resuming", packageId);
        lifecycle_bridge_dispatch(packageId, LIFECYCLE_ON_RESUME);
        input_bridge_set_foreground(packageId);
        return 0;
    }

    LOGI(LOG_TAG, "=== launch(%s) ===", packageId);
    LOGI(LOG_TAG, "  sandbox: %s", pkg->sandbox_root);
    LOGI(LOG_TAG, "  dex:     %s", pkg->classes_dex_path);
    LOGI(LOG_TAG, "  activity:%s", pkg->activity_class);

    // 1. Create the per-package VM (loads classes.dex).
    art_vm_t *vm = NULL;
    int rc = art_runtime_create_vm(packageId, pkg->classes_dex_path,
                                   pkg->sandbox_root, &vm);
    if (rc != 0) {
        LOGE(LOG_TAG, "art_runtime_create_vm failed (rc=%d)", rc);
        return -10;
    }

    // 2. Load native .so files from <sandbox>/lib/.
    char lib_dir[1024];
    snprintf(lib_dir, sizeof(lib_dir), "%s/lib", pkg->sandbox_root);
    load_so_files(lib_dir, pkg);
    if (pkg->module_count == 0) {
        LOGW(LOG_TAG, "no native .so loaded — proceeding with pure-Java path");
    }

    // 3. Dispatch Activity.onCreate -> onStart -> onResume.
    if (pkg->activity_class[0]) {
        lifecycle_bridge_dispatch(packageId, LIFECYCLE_ON_CREATE);
        lifecycle_bridge_dispatch(packageId, LIFECYCLE_ON_START);
        lifecycle_bridge_dispatch(packageId, LIFECYCLE_ON_RESUME);
    } else {
        LOGW(LOG_TAG, "no activity class set — skipping lifecycle dispatch");
    }

    input_bridge_set_foreground(packageId);
    pkg->launched = 1;
    LOGI(LOG_TAG, "=== launch(%s) complete ===", packageId);
    return 0;
}

// Richer launch API: Swift calls this BEFORE launch() to provide paths.
int apkcontainer_runtime_configure(const char *packageId,
                                   const char *sandbox_root,
                                   const char *classes_dex_path,
                                   const char *activity_class) {
    ensure_init();
    if (!packageId) return -1;
    pkg_state_t *pkg = find_pkg(packageId);
    if (!pkg) {
        if (g_pkg_count >= MAX_PKGS) return -2;
        pkg = &g_pkgs[g_pkg_count++];
        memset(pkg, 0, sizeof(*pkg));
        strncpy(pkg->package_id, packageId, sizeof(pkg->package_id) - 1);
    }
    if (sandbox_root) strncpy(pkg->sandbox_root, sandbox_root, sizeof(pkg->sandbox_root) - 1);
    if (classes_dex_path) strncpy(pkg->classes_dex_path, classes_dex_path, sizeof(pkg->classes_dex_path) - 1);
    if (activity_class) {
        // Normalize to descriptor form.
        const char *s = activity_class;
        if (s[0] == 'L' && s[strlen(s)-1] == ';') {
            strncpy(pkg->activity_class, s, sizeof(pkg->activity_class) - 1);
        } else {
            char buf[256];
            snprintf(buf, sizeof(buf), "L");
            size_t off = 1;
            for (const char *p = s; *p && off < sizeof(buf)-2; p++) {
                buf[off++] = (*p == '.') ? '/' : *p;
            }
            buf[off++] = ';';
            buf[off] = '\0';
            strncpy(pkg->activity_class, buf, sizeof(pkg->activity_class) - 1);
        }
    }
    LOGI(LOG_TAG, "configure(%s): sandbox=%s dex=%s activity=%s",
         packageId, pkg->sandbox_root, pkg->classes_dex_path, pkg->activity_class);
    return 0;
}

int apkcontainer_runtime_suspend(const char *packageId) {
    if (!packageId) return -1;
    return lifecycle_bridge_dispatch(packageId, LIFECYCLE_ON_PAUSE);
}

int apkcontainer_runtime_resume(const char *packageId) {
    if (!packageId) return -1;
    return lifecycle_bridge_dispatch(packageId, LIFECYCLE_ON_RESUME);
}

int apkcontainer_runtime_force_quit(const char *packageId) {
    if (!packageId) return -1;
    lifecycle_bridge_dispatch(packageId, LIFECYCLE_ON_DESTROY);
    pkg_state_t *pkg = find_pkg(packageId);
    if (pkg) {
        for (int i = 0; i < pkg->module_count; i++)
            apkcontainer_elf_unload(&pkg->modules[i]);
        free(pkg->modules);
        memset(pkg, 0, sizeof(*pkg));
    }
    input_bridge_set_foreground(NULL);
    LOGI(LOG_TAG, "force_quit(%s): modules unloaded", packageId);
    return 0;
}

int apkcontainer_lifecycle_dispatch(const char *packageId, int event) {
    return lifecycle_bridge_dispatch(packageId, event);
}

int apkcontainer_graphics_attach_layer(void *cametallayer) {
    ensure_init();
    // Default framebuffer size; Swift will resize via swgl_attach_output if
    // it knows the layer size. 720x1280 is a safe Android phone portrait.
    int rc = graphics_bridge_attach_layer(cametallayer);
    if (rc != 0) {
        LOGE(LOG_TAG, "graphics_bridge_attach_layer failed (rc=%d)", rc);
    }
    return rc;
}

int apkcontainer_input_enqueue_touch(const char *packageId, int pointerId,
                                     float x, float y, float pressure, int action) {
    return input_bridge_enqueue(packageId, pointerId, x, y, pressure, action);
}

int apkcontainer_audio_start(void) {
    // Lazily create the global OpenSL ES -> AVAudioEngine. Once this returns
    // 0, opensl_bridge_get_global_engine() returns a non-NULL engine that
    // the AudioTrack Java stub (dex_interp.cpp) and the SLES C API wrapper
    // (bionic_shim.c) use to create players + enqueue PCM.
    //
    // The single source of truth for the engine pointer is the Obj-C++ side
    // (g_global_engine in opensl_bridge.mm). We never cache it here so that
    // audio_stop() can NULL it out and a subsequent audio_start() re-creates
    // a fresh engine.
    if (opensl_bridge_get_global_engine()) {
        LOGI(LOG_TAG, "audio_start: engine already running");
        return 0;
    }
    sl_engine_t *eng = NULL;
    int rc = opensl_bridge_engine_create(&eng);
    if (rc != 0 || !eng) {
        LOGE(LOG_TAG, "audio_start: opensl_bridge_engine_create failed (rc=%d)", rc);
        return -1;
    }
    // Publish via the Obj-C++ side. opensl_bridge_set_global_engine is
    // defined in opensl_bridge.mm; it atomically swaps the engine pointer
    // the AudioTrack stub reads. Ownership transfers to the global slot.
    opensl_bridge_set_global_engine(eng);
    LOGI(LOG_TAG, "audio_start: OpenSL ES -> AVAudioEngine bridge up (engine=%p)",
         (void *)eng);
    return 0;
}

int apkcontainer_audio_stop(void) {
    // Tear down the global engine. Players created from it must already be
    // destroyed by their owners (the AudioTrack stub's release() does this
    // per-track). We destroy the engine anyway — AVAudioEngine.detachNode
    // tolerates a missing node.
    sl_engine_t *eng = opensl_bridge_get_global_engine();
    if (!eng) {
        LOGI(LOG_TAG, "audio_stop: engine not running");
        return 0;
    }
    // Clear the global pointer first so any racing AudioTrack call sees NULL
    // and bails out instead of touching a torn-down engine. The engine is
    // destroyed on the opensl_bridge side via the swap (passing NULL swaps
    // out the old pointer and destroys it).
    opensl_bridge_set_global_engine(NULL);
    LOGI(LOG_TAG, "audio_stop: engine destroyed");
    return 0;
}

// Extra API for Swift: get the framebuffer pointer + dims after eglSwapBuffers.
const void *apkcontainer_get_framebuffer(void) {
    return swgl_get_framebuffer();
}
int apkcontainer_get_framebuffer_width(void) {
    return swgl_get_framebuffer_width();
}
int apkcontainer_get_framebuffer_height(void) {
    return swgl_get_framebuffer_height();
}

// Extra API for Swift: resize the GL output (called when CAMetalLayer.drawableSize changes).
int apkcontainer_graphics_resize(int width, int height) {
    void *layer = NULL;   // Swift passes the layer ptr via attach_layer; we
                           // don't need it here — swgl_attach_output with NULL
                           // layer just resizes.
    return swgl_attach_output(layer, width, height);
}

// Extra API for Swift: get the current log file path (so the UI can show it).
const char *apkcontainer_get_log_path(void) {
    return log_current_path();
}
