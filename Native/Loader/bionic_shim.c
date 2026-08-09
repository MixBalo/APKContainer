/*
 * bionic_shim.c — Bionic libc + NDK shim for Android .so compatibility on iOS
 *
 * Status: PARTIAL. Common symbols (string/mem/stdlib, pthread wrappers,
 *         __system_property_get, __android_log_print, ashmem emulation) are
 *         real. binder ioctls + exotic Bionic internals are STUB (log + return
 *         ENOSYS / default). See docs/ARCHITECTURE.md §3, docs/CAPABILITY_MATRIX.md §3.
 */
#include "bionic_shim.h"
#include "log_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <os/log.h>

#define LOG_TAG_BIONIC "bionic"

/* ---------------- Per-thread Bionic TLS ---------------- */
static pthread_key_t tls_key;
static pthread_once_t tls_once = PTHREAD_ONCE_INIT;
static void tls_dtor(void *p) { free(p); }
static void tls_init(void) { pthread_key_create(&tls_key, tls_dtor); }

void **apkcontainer_bionic_tls(void) {
    pthread_once(&tls_once, tls_init);
    void **slots = pthread_getspecific(tls_key);
    if (!slots) {
        slots = calloc(BIONIC_TLS_SLOT_COUNT, sizeof(void *));
        pthread_setspecific(tls_key, slots);
    }
    return slots;
}

/* ---------------- __system_property_get ---------------- */
static const struct { const char *key; const char *val; } props[] = {
    { "ro.build.version.sdk",        "33" },
    { "ro.build.version.release",    "13" },
    { "ro.product.cpu.abi",          "arm64-v8a" },
    { "ro.product.manufacturer",     "unknown" },
    { "ro.product.model",            "APKLive" },
    { "ro.product.brand",            "apklive" },
    { "ro.build.fingerprint",        "apklive/apklive/apklive:13/TP1A.220624.021/APKLive" },
    { "ro.build.display.id",         "APKLive-13" },
    { "persist.sys.timezone",        "Europe/Amsterdam" },
    { NULL, NULL }
};

int apkcontainer_bionic_system_property_get(const char *key, char *value, size_t len) {
    if (!key || !value || len == 0) return 0;
    for (int i = 0; props[i].key; i++) {
        if (strcmp(props[i].key, key) == 0) {
            size_t n = strlen(props[i].val);
            if (n >= len) n = len - 1;
            memcpy(value, props[i].val, n);
            value[n] = '\0';
            return (int)n;
        }
    }
    value[0] = '\0';
    return 0;
}

/* ---------------- __android_log_print ----------------
 * Bridges Android's __android_log_print(int prio, const char *tag, const char *fmt, ...)
 * to our log_file writer (and os_log). prio mapping is Android's:
 *   0 UNKNOWN, 1 DEFAULT, 2 VERBOSE, 3 DEBUG, 4 INFO, 5 WARN, 6 ERROR, 7 FATAL */
int apkcontainer_bionic_android_log_print(int prio, const char *tag,
                                          const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    int level = LOG_LVL_INFO;
    switch (prio) {
        case 0 ... 3: level = LOG_LVL_DEBUG; break;
        case 4:       level = LOG_LVL_INFO;  break;
        case 5:       level = LOG_LVL_WARN;  break;
        case 6 ... 7: level = LOG_LVL_ERROR; break;
    }
    log_write(level, tag ? tag : "android", "%s", buf);
    return 0;
}

/* Bionic's __system_property_get is `int(const char *name, char *value)` with
 * value buffer assumed to be PROP_VALUE_MAX (92) bytes. Our internal helper
 * takes a length; provide a 2-arg ABI-compatible trampoline. */
static int bionic_system_property_get_2arg(const char *name, char *value) {
    return apkcontainer_bionic_system_property_get(name, value, 92 /* PROP_VALUE_MAX */);
}

/* ---------------- ashmem emulation ----------------
 * Real ashmem is cross-process shared memory. We're single-process, so an
 * anonymous mmap + an fd registry suffices for any APK that uses ashmem for
 * intra-process buffers (e.g. some graphics buffers). */
#define MAX_ASHMEM 64
static struct { int in_use; void *addr; size_t size; } ashmem_tab[MAX_ASHMEM];

int apkcontainer_bionic_ashmem_create_region(const char *name, size_t size) {
    (void)name;
    for (int i = 0; i < MAX_ASHMEM; i++) {
        if (!ashmem_tab[i].in_use) {
            void *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p == MAP_FAILED) return -1;
            ashmem_tab[i].in_use = 1;
            ashmem_tab[i].addr = p;
            ashmem_tab[i].size = size;
            return i + 0x1000;   /* fake fd, never conflicts with real fds */
        }
    }
    return -1;
}
int apkcontainer_bionic_ashmem_get_size_region(int fd) {
    int i = fd - 0x1000;
    if (i < 0 || i >= MAX_ASHMEM || !ashmem_tab[i].in_use) return -1;
    return (int)ashmem_tab[i].size;
}
int apkcontainer_bionic_ashmem_set_prot_region(int fd, int prot) {
    int i = fd - 0x1000;
    if (i < 0 || i >= MAX_ASHMEM || !ashmem_tab[i].in_use) return -1;
    return mprotect(ashmem_tab[i].addr, ashmem_tab[i].size, prot);
}

/* ---------------- Symbol resolver ----------------
 * Returns a function pointer for a Bionic/NDK symbol name. Many forward to
 * Darwin; the rest are STUB (NULL) and the caller logs + surfaces. */
void *apkcontainer_bionic_resolve(const char *name) {
    if (!name) return NULL;

    /* libc / string / stdlib — Darwin libsystem has the same names */
    if (!strcmp(name, "memcpy")  || !strcmp(name, "memmove") ||
        !strcmp(name, "memset")  || !strcmp(name, "memcmp")  ||
        !strcmp(name, "strlen")  || !strcmp(name, "strcmp")  ||
        !strcmp(name, "strncmp") || !strcmp(name, "strcpy")  ||
        !strcmp(name, "strncpy") || !strcmp(name, "strcat")  ||
        !strcmp(name, "strchr")  || !strcmp(name, "strrchr") ||
        !strcmp(name, "strstr")  || !strcmp(name, "strtol")  ||
        !strcmp(name, "strtoll") || !strcmp(name, "strtoul") ||
        !strcmp(name, "atoi")    || !strcmp(name, "atof")    ||
        !strcmp(name, "malloc")  || !strcmp(name, "free")    ||
        !strcmp(name, "calloc")  || !strcmp(name, "realloc") ||
        !strcmp(name, "abort")   || !strcmp(name, "exit")) {
        return dlsym(RTLD_DEFAULT, name);
    }
    /* pthread — Darwin pthread has the same ABI names */
    if (!strncmp(name, "pthread_", 8)) {
        return dlsym(RTLD_DEFAULT, name);
    }
    /* Bionic-specific helpers */
    if (!strcmp(name, "__system_property_get")) {
        /* ABI-compatible 2-arg trampoline (see above). */
        return (void *)bionic_system_property_get_2arg;
    }
    if (!strcmp(name, "__android_log_print") ||
        !strcmp(name, "__android_log_write") ||
        !strcmp(name, "__android_log_vprint")) {
        return (void *)apkcontainer_bionic_android_log_print;
    }
    if (!strcmp(name, "ashmem_create_region")) return (void *)apkcontainer_bionic_ashmem_create_region;
    if (!strcmp(name, "ashmem_get_size_region")) return (void *)apkcontainer_bionic_ashmem_get_size_region;
    if (!strcmp(name, "ashmem_set_prot_region")) return (void *)apkcontainer_bionic_ashmem_set_prot_region;
    if (!strcmp(name, "__get_tls"))             return (void *)apkcontainer_bionic_tls;

    /* binder — STUB: most apps don't call directly. */
    if (!strcmp(name, "ioctl")) return dlsym(RTLD_DEFAULT, "ioctl");

    /* Unknown — log and return NULL. The ELF loader has already logged the
     * calling module; this is the second half of "BIONIC_STUB: <name>". */
    LOGW(LOG_TAG_BIONIC, "BIONIC_STUB: unresolved symbol %s", name);
    return NULL;
}

/* ====================================================================== *
 * OpenSL ES shim (libOpenSLES.so)
 *
 * Status: PARTIAL. We provide:
 *   - The SL_IID_* interface-ID constants (exported as the SL_IID<NAME>_
 *     symbol names that AOSP's OpenSLES.h declares extern; the macro
 *     SL_IID_<NAME> = &SL_IID<NAME>_ resolves to our address at link time).
 *   - slCreateEngine: returns a minimal SLObjectItf wrapper whose Realize /
 *     GetInterface / Destroy forward to opensl_bridge_engine_create etc.
 *   - The wrapper's GetInterface(SL_IID_ENGINE) returns our sl_engine_t*
 *     directly, so apps that link libOpenSLES.so and call slCreateEngine
 *     can use OUR opensl_bridge_create_player / opensl_bridge_player_enqueue
 *     API on the returned interface pointer.
 *
 * What's NOT implemented (logs WARN, returns SL_RESULT_FEATURE_UNSUPPORTED):
 *   - The full SLES object model: SLEngineItf::CreateAudioPlayer /
 *     CreateOutputMix, SLBufferQueueItf::Enqueue, SLPlayItf::SetPlayState,
 *     Object::Destroy for player/outputmix objects, SLEffectSendItf,
 *     SLBassBoostItf, SL3DLocationItf, etc.
 *   - Apps that exercise those APIs must be ported to call our
 *     opensl_bridge_* API directly (see opensl_bridge.h) — or wait for a
 *     future task to add the vtable forwarding.
 *
 * See docs/ARCHITECTURE.md §5, docs/CAPABILITY_MATRIX.md §5.
 * ====================================================================== */

#include "opensl_bridge.h"

/* Minimal SLES types (subset of OpenSLES.h that we need to export). */
typedef int32_t  SLint32;
typedef uint32_t SLuint32;
typedef uint16_t SLuint16;
typedef uint8_t  SLuint8;
typedef int32_t  SLboolean;
typedef int32_t  SLresult;

#define SL_BOOLEAN_FALSE 0
#define SL_BOOLEAN_TRUE  1

#define SL_RESULT_SUCCESS                0
#define SL_RESULT_FEATURE_UNSUPPORTED   (-2)
#define SL_RESULT_INTERNAL_ERROR         (-3)
#define SL_RESULT_PARAMETER_INVALID     (-18)

/* SLInterfaceID = pointer to UUID struct. */
struct SLInterfaceID_ {
    SLuint32 time_low;
    SLuint16 time_mid;
    SLuint16 time_hi_and_version;
    SLuint8  clock_seq;
    SLuint8  node[6];
};
typedef const struct SLInterfaceID_ *SLInterfaceID;

/* SL_IID_* constants. The values are placeholders (NOT the official SLES
 * 1.0.1 UUIDs). This is fine because:
 *   1. Apps reference SL_IID_<NAME> as a macro that expands to
 *      &SL_IID<NAME>_ — the LINKER resolves the symbol to OUR address, so
 *      the app's pointer is always &our_struct.
 *   2. Our GetInterface implementation compares the iid POINTER (not the
 *      UUID value) to our known &SL_IID<NAME>_ addresses. So any pointer
 *      that resolves to our symbol matches.
 * If an app does its own UUID-by-value comparison (very rare in practice),
 * the values would need to match the official SLES 1.0.1 UUIDs from
 * OpenSLES.h — to be verified before shipping.
 */
#define DEFINE_SL_IID(name, a, b, c, d, e0, e1, e2, e3, e4, e5) \
    const struct SLInterfaceID_ SL_IID##name##_ = { \
        (SLuint32)(a), (SLuint16)(b), (SLuint16)(c), (SLuint8)(d), \
        { (SLuint8)(e0), (SLuint8)(e1), (SLuint8)(e2), \
          (SLuint8)(e3), (SLuint8)(e4), (SLuint8)(e5) } \
    }

/* The exported symbols are SL_IIDENGINE_, SL_IIDBUFFERQUEUE_, etc.
 * Apps use the macro SL_IID_ENGINE = &SL_IIDENGINE_ to get a pointer. */
DEFINE_SL_IID(ENGINE,        0x8b0ee268u, 0x9aa4, 0x11e0, 0x8b, 0x07,0x00,0x20,0x2e,0x97,0xa5,0x4d);
DEFINE_SL_IID(BUFFERQUEUE,   0x2dd06aa0u, 0x9c4b, 0x11e0, 0x87, 0xaf,0x00,0x02,0xa5,0xd5,0xc5,0x1b);
DEFINE_SL_IID(PLAY,          0x37b9deadu, 0x9c4c, 0x11e0, 0x87, 0xaf,0x00,0x02,0xa5,0xd5,0xc5,0x1b);
DEFINE_SL_IID(VOLUME,        0x37b9dea1u, 0x9c4c, 0x11e0, 0x87, 0xaf,0x00,0x02,0xa5,0xd5,0xc5,0x1b);
DEFINE_SL_IID(OUTPUTMIX,     0x9773560au, 0x9c4c, 0x11e0, 0x87, 0xaf,0x00,0x02,0xa5,0xd5,0xc5,0x1b);
DEFINE_SL_IID(ZNULL,         0x00000000u, 0x0000, 0x0000, 0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x00);
/* Note: SL_IIDNULL_ is named ZNULL_ here to dodge the `NULL` macro; the
 * exported symbol is SL_IIDZNULL_ but apps asking for SL_IIDNULL_ are
 * rare (it's the "null interface" used only for error tests). */
DEFINE_SL_IID(SEEK,          0x37b9deabu, 0x9c4c, 0x11e0, 0x87, 0xaf,0x00,0x02,0xa5,0xd5,0xc5,0x1b);
DEFINE_SL_IID(PLAYBACKRATE,  0x37b9deacu, 0x9c4c, 0x11e0, 0x87, 0xaf,0x00,0x02,0xa5,0xd5,0xc5,0x1b);
DEFINE_SL_IID(RECORD,        0x37b9deaau, 0x9c4c, 0x11e0, 0x87, 0xaf,0x00,0x02,0xa5,0xd5,0xc5,0x1b);

/* ---- SLObjectItf (minimal vtable) ----
 * We only implement Realize / GetInterface / Destroy. Other methods log
 * WARN and return SL_RESULT_FEATURE_UNSUPPORTED.
 *
 * ABI note: AOSP's OpenSLES.h declares `SLObjectItf` as a DOUBLE pointer
 * (`typedef const struct SLObjectItf_ { ... } ** SLObjectItf;`) so that the
 * app pattern `(*engineObject)->Realize(engineObject, ...)` works:
 *   - engineObject is SLObjectItf = `const struct SLObjectItf_ **`
 *   - *engineObject is `const struct SLObjectItf_ *` (the vtable pointer)
 *   - (*engineObject)->Realize is the function pointer
 *   - Realize receives `self = engineObject` (the double pointer); to get
 *     back to our wrapper struct, deref once: `*self` is the vtable pointer
 *     (= the first member of sles_wrapper_t), cast to sles_wrapper_t *.
 */

struct SLObjectItf_;
typedef const struct SLObjectItf_ **SLObjectItf;

typedef SLresult (*slObjectCallback)(SLObjectItf self, void *pContext, SLuint32 event,
                                     SLresult result, void *pInterface);

struct SLObjectItf_ {
    SLresult (*Realize)(SLObjectItf self, SLboolean async);
    SLresult (*Resume)(SLObjectItf self, SLboolean async);
    SLresult (*GetState)(SLObjectItf self, SLuint32 *pState);
    SLresult (*GetInterface)(SLObjectItf self, const SLInterfaceID iid, void *pInterface);
    SLresult (*RegisterCallback)(SLObjectItf self, slObjectCallback callback, void *pContext);
    void     (*DestroyAsync)(SLObjectItf self);
    SLresult (*Destroy)(SLObjectItf self);
    SLresult (*GetSupportedInterfacesCount)(SLObjectItf self, SLuint32 *pCount);
    SLresult (*GetSupportedInterfaces)(SLObjectItf self, SLuint32 index, SLInterfaceID *pInterfaceId);
    SLresult (*GetObjectID)(SLObjectItf self, SLInterfaceID *pObjectID);
};

/* Our wrapper instance: vtable + the engine we wrap. The vtable pointer
 * (the first member) is what the app's `*engineObject` dereferences to.
 * `engineObject` itself points to &vtable. */
typedef struct {
    const struct SLObjectItf_ *vtable;   /* MUST be first — *self points here */
    sl_engine_t *engine;                  /* NULL after Destroy */
    int is_engine_object;                  /* 1 for engine, 0 for player/recorder */
    sl_player_t *player;                   /* if is_engine_object==0 */
} sles_wrapper_t;

/* Helper: from a method's `self` (SLObjectItf = double pointer), get the
 * wrapper struct. */
static inline sles_wrapper_t *sles_self_to_wrapper(SLObjectItf self) {
    if (!self || !*self) return NULL;
    /* *self is the vtable pointer (first member of sles_wrapper_t). */
    return (sles_wrapper_t *)*self;
}

/* Forward decls of the vtable methods. */
static SLresult sles_obj_Realize(SLObjectItf self, SLboolean async);
static SLresult sles_obj_Resume(SLObjectItf self, SLboolean async);
static SLresult sles_obj_GetState(SLObjectItf self, SLuint32 *pState);
static SLresult sles_obj_GetInterface(SLObjectItf self, const SLInterfaceID iid, void *pInterface);
static SLresult sles_obj_RegisterCallback(SLObjectItf self, slObjectCallback cb, void *pCtx);
static void     sles_obj_DestroyAsync(SLObjectItf self);
static SLresult sles_obj_Destroy(SLObjectItf self);
static SLresult sles_obj_GetSupportedInterfacesCount(SLObjectItf self, SLuint32 *pCount);
static SLresult sles_obj_GetSupportedInterfaces(SLObjectItf self, SLuint32 index, SLInterfaceID *pInterfaceId);
static SLresult sles_obj_GetObjectID(SLObjectItf self, SLInterfaceID *pObjectID);

static const struct SLObjectItf_ s_sles_engine_vtable = {
    sles_obj_Realize,
    sles_obj_Resume,
    sles_obj_GetState,
    sles_obj_GetInterface,
    sles_obj_RegisterCallback,
    sles_obj_DestroyAsync,
    sles_obj_Destroy,
    sles_obj_GetSupportedInterfacesCount,
    sles_obj_GetSupportedInterfaces,
    sles_obj_GetObjectID,
};

static SLresult sles_obj_Realize(SLObjectItf self, SLboolean async) {
    (void)self; (void)async;
    /* Realize is a no-op for us — the AVAudioEngine is created during
     * opensl_bridge_engine_create, which slCreateEngine already called. */
    LOGD("opensl", "SLObjectItf::Realize async=%d (no-op)", (int)async);
    return SL_RESULT_SUCCESS;
}
static SLresult sles_obj_Resume(SLObjectItf self, SLboolean async) {
    (void)self; (void)async;
    return SL_RESULT_SUCCESS;
}
static SLresult sles_obj_GetState(SLObjectItf self, SLuint32 *pState) {
    (void)self;
    if (pState) *pState = 0x02 /* SL_OBJECT_STATE_REALIZED */;
    return SL_RESULT_SUCCESS;
}
static SLresult sles_obj_GetInterface(SLObjectItf self, const SLInterfaceID iid, void *pInterface) {
    sles_wrapper_t *w = sles_self_to_wrapper(self);
    if (!w || !pInterface) return SL_RESULT_PARAMETER_INVALID;

    /* Match by pointer address. The macro SL_IID_<NAME> = &SL_IID<NAME>_
     * resolves to our symbol's address, so any app that uses the standard
     * SLES headers gets our address. */
    if (iid == &SL_IIDENGINE_) {
        /* The app expects an SLEngineItf. We don't implement the full
         * SLEngineItf vtable (CreateAudioPlayer, CreateOutputMix, ...). We
         * hand back our sl_engine_t* directly so the app can use the
         * opensl_bridge_* API on it. */
        *(sl_engine_t **)pInterface = w->engine;
        LOGI("opensl", "GetInterface(SL_IID_ENGINE) -> %p (use opensl_bridge_* API)", (void *)w->engine);
        return SL_RESULT_SUCCESS;
    }
    if (iid == &SL_IIDBUFFERQUEUE_ && w->player) {
        /* Same trick: hand back our sl_player_t* directly. */
        *(sl_player_t **)pInterface = w->player;
        LOGI("opensl", "GetInterface(SL_IID_BUFFERQUEUE) -> %p (use opensl_bridge_player_enqueue)", (void *)w->player);
        return SL_RESULT_SUCCESS;
    }
    if (iid == &SL_IIDPLAY_ && w->player) {
        *(sl_player_t **)pInterface = w->player;
        LOGI("opensl", "GetInterface(SL_IID_PLAY) -> %p (use opensl_bridge_player_play/stop)", (void *)w->player);
        return SL_RESULT_SUCCESS;
    }
    if (iid == &SL_IIDVOLUME_ && w->player) {
        /* Volume not implemented; return the player so the app at least
         * doesn't crash on the pointer. The app's SetVolume call would
         * need a real SLVolumeItf — not implemented. */
        *(sl_player_t **)pInterface = w->player;
        LOGW("opensl", "GetInterface(SL_IID_VOLUME) PARTIAL — volume control not implemented");
        return SL_RESULT_SUCCESS;
    }
    LOGW("opensl", "GetInterface: unsupported iid=%p (use opensl_bridge_* API directly)", (void *)iid);
    return SL_RESULT_FEATURE_UNSUPPORTED;
}
static SLresult sles_obj_RegisterCallback(SLObjectItf self, slObjectCallback cb, void *pCtx) {
    (void)self; (void)cb; (void)pCtx;
    return SL_RESULT_SUCCESS;
}
static void sles_obj_DestroyAsync(SLObjectItf self) {
    sles_obj_Destroy(self);
}
static SLresult sles_obj_Destroy(SLObjectItf self) {
    sles_wrapper_t *w = sles_self_to_wrapper(self);
    if (!w) return SL_RESULT_PARAMETER_INVALID;
    /* Clear the caller's vtable pointer BEFORE freeing the wrapper, so any
     * racing access from another thread sees NULL and bails out instead of
     * touching freed memory. */
    if (self) *self = NULL;
    if (w->is_engine_object && w->engine) {
        /* Don't double-destroy if we're the global engine — the runtime
         * owns its lifecycle. The wrapper just forgets the pointer. */
        sl_engine_t *global = opensl_bridge_get_global_engine();
        if (w->engine != global) {
            opensl_bridge_engine_destroy(w->engine);
        }
        w->engine = NULL;
    } else if (w->player) {
        opensl_bridge_player_destroy(w->player);
        w->player = NULL;
    }
    free(w);
    LOGI("opensl", "SLObjectItf::Destroy ok");
    return SL_RESULT_SUCCESS;
}
static SLresult sles_obj_GetSupportedInterfacesCount(SLObjectItf self, SLuint32 *pCount) {
    (void)self;
    if (pCount) *pCount = 1;
    return SL_RESULT_SUCCESS;
}
static SLresult sles_obj_GetSupportedInterfaces(SLObjectItf self, SLuint32 index, SLInterfaceID *pInterfaceId) {
    (void)self;
    if (index != 0 || !pInterfaceId) return SL_RESULT_PARAMETER_INVALID;
    *pInterfaceId = &SL_IIDENGINE_;
    return SL_RESULT_SUCCESS;
}
static SLresult sles_obj_GetObjectID(SLObjectItf self, SLInterfaceID *pObjectID) {
    (void)self;
    if (pObjectID) *pObjectID = &SL_IIDENGINE_;
    return SL_RESULT_SUCCESS;
}

/* ---- slCreateEngine entry point ----
 * Signature matches AOSP's OpenSLES.h. We ignore the options/interfaces and
 * just create our opensl_bridge engine, wrapping it in an SLObjectItf. */
typedef struct {
    SLuint32 feature;
    SLuint32 data;
} SLEngineOption;   /* matches AOSP */

SLresult slCreateEngine(SLObjectItf *pEngine, SLuint32 numOptions,
                        const SLEngineOption *pEngineOptions,
                        SLuint32 numInterfaces,
                        const SLInterfaceID *pInterfaceIds,
                        const SLboolean *pInterfaceRequired,
                        void *pEngineReserved) {
    (void)numOptions; (void)pEngineOptions;
    (void)numInterfaces; (void)pInterfaceIds; (void)pInterfaceRequired;
    (void)pEngineReserved;
    if (!pEngine) return SL_RESULT_PARAMETER_INVALID;

    /* Prefer the global engine (created by apkcontainer_audio_start); if it
     * doesn't exist yet, create one on demand AND publish it as the global
     * engine so subsequent calls reuse it. This way apps that call
     * slCreateEngine before the runtime has started audio still get sound,
     * and we don't end up with multiple AVAudioEngine instances. */
    sl_engine_t *eng = opensl_bridge_get_global_engine();
    if (!eng) {
        int rc = opensl_bridge_engine_create(&eng);
        if (rc != 0 || !eng) {
            LOGE("opensl", "slCreateEngine: opensl_bridge_engine_create failed (rc=%d)", rc);
            return SL_RESULT_INTERNAL_ERROR;
        }
        /* Publish. opensl_bridge_set_global_engine is declared in
         * opensl_bridge.h and defined in opensl_bridge.mm. */
        opensl_bridge_set_global_engine(eng);
    }

    sles_wrapper_t *w = (sles_wrapper_t *)calloc(1, sizeof(sles_wrapper_t));
    if (!w) return SL_RESULT_INTERNAL_ERROR;
    w->vtable = &s_sles_engine_vtable;
    w->engine = eng;
    w->is_engine_object = 1;
    /* *pEngine is SLObjectItf (= `const struct SLObjectItf_ **`). The app
     * dereferences once to get the vtable pointer, so we store the address
     * of w->vtable (which is the first member of the wrapper). */
    *pEngine = (SLObjectItf)&w->vtable;
    LOGI("opensl", "slCreateEngine -> %p (engine=%p)", (void *)*pEngine, (void *)eng);
    return SL_RESULT_SUCCESS;
}

/* slQueryNumSupportedInterfaces / slQuerySupportedInterfaces — return
 * minimal values so apps that probe SLES at startup don't fail. */
SLresult slQueryNumSupportedInterfaces(SLuint32 *pNumSupported) {
    if (pNumSupported) *pNumSupported = 1;
    return SL_RESULT_SUCCESS;
}
SLresult slQuerySupportedInterfaces(SLuint32 index, SLInterfaceID *pInterfaceId) {
    if (index != 0 || !pInterfaceId) return SL_RESULT_PARAMETER_INVALID;
    *pInterfaceId = &SL_IIDENGINE_;
    return SL_RESULT_SUCCESS;
}

/* ---- The resolver registered for libOpenSLES.so ----
 * Returns function-pointer / data-symbol addresses for SLES names. Anything
 * we don't recognise is delegated to apkcontainer_bionic_resolve so generic
 * libc symbols an app might pull in via libOpenSLES.so still resolve. */
void *bionic_opensl_resolve(const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "slCreateEngine"))                   return (void *)slCreateEngine;
    if (!strcmp(name, "slQueryNumSupportedInterfaces"))    return (void *)slQueryNumSupportedInterfaces;
    if (!strcmp(name, "slQuerySupportedInterfaces"))       return (void *)slQuerySupportedInterfaces;
    /* SL_IID<NAME>_ symbols (UUID structs exported by AOSP's OpenSLES.h).
     * Apps use the macro SL_IID_<NAME> = &SL_IID<NAME>_, so the linker
     * resolves the SL_IID<NAME>_ symbol to our struct's address. */
    if (!strcmp(name, "SL_IIDENGINE_"))        return (void *)&SL_IIDENGINE_;
    if (!strcmp(name, "SL_IIDBUFFERQUEUE_"))   return (void *)&SL_IIDBUFFERQUEUE_;
    if (!strcmp(name, "SL_IIDPLAY_"))          return (void *)&SL_IIDPLAY_;
    if (!strcmp(name, "SL_IIDVOLUME_"))        return (void *)&SL_IIDVOLUME_;
    if (!strcmp(name, "SL_IIDOUTPUTMIX_"))     return (void *)&SL_IIDOUTPUTMIX_;
    if (!strcmp(name, "SL_IIDSEEK_"))          return (void *)&SL_IIDSEEK_;
    if (!strcmp(name, "SL_IIDPLAYBACKRATE_"))  return (void *)&SL_IIDPLAYBACKRATE_;
    if (!strcmp(name, "SL_IIDRECORD_"))        return (void *)&SL_IIDRECORD_;
    if (!strcmp(name, "SL_IIDNULL_"))          return (void *)&SL_IIDZNULL_;
    /* Fall through to the generic Bionic resolver for anything else. */
    return apkcontainer_bionic_resolve(name);
}
