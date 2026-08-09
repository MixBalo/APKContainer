/*
 * bionic_shim.h — Bionic libc + NDK shim for Android .so compatibility on iOS
 *
 * Status: PARTIAL. Common Bionic symbols (string/mem/stdlib, pthread wrappers,
 *         __system_property_get, __android_log_print, ashmem emulation) are
 *         real. binder ioctls + exotic Bionic internals are STUB (log + return
 *         ENOSYS / default). The OpenSL ES resolver (bionic_opensl_resolve)
 *         provides slCreateEngine + SL_IID_* constants and a minimal SLES
 *         object-model wrapper that forwards to opensl_bridge_*. See
 *         bionic_shim.c, docs/ARCHITECTURE.md §3, docs/CAPABILITY_MATRIX.md §3.
 */
#ifndef APKCONTAINER_BIONIC_SHIM_H
#define APKCONTAINER_BIONIC_SHIM_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve a Bionic/NDK symbol name to our implementation.
 * Returns NULL if unknown (caller logs + surfaces). */
void *apkcontainer_bionic_resolve(const char *name);

/* Per-thread Bionic TLS slot block. Android code reads TLS via __get_tls().
 * We allocate a fixed-size slot array per thread. */
enum {
    BIONIC_TLS_SLOT_SELF = 0,
    BIONIC_TLS_SLOT_THREAD_ID,
    BIONIC_TLS_SLOT_ERRNO,
    BIONIC_TLS_SLOT_TSAN,           /* unused */
    BIONIC_TLS_SLOT_STACK_GUARD,
    BIONIC_TLS_SLOT_BIONIC_RESERVED,
    BIONIC_TLS_SLOT_OPENGL_API,     /* used by GLES */
    BIONIC_TLS_SLOT_OPENGL_DRIVER,
    BIONIC_TLS_SLOT_USER_START = 16,
    BIONIC_TLS_SLOT_COUNT = 64
};
void **apkcontainer_bionic_tls(void);   /* returns current thread's slot array */

/* Android system property stub. Returns the length of the value copied into
 * `value` (<= `len`), or 0 if not found. */
int apkcontainer_bionic_system_property_get(const char *key, char *value, size_t len);

/* ashmem emulation. Returns a fd-like handle (>=0) or -1. */
int  apkcontainer_bionic_ashmem_create_region(const char *name, size_t size);
int  apkcontainer_bionic_ashmem_get_size_region(int fd);
int  apkcontainer_bionic_ashmem_set_prot_region(int fd, int prot);

/* android/log.h bridge */
int  apkcontainer_bionic_android_log_print(int prio, const char *tag,
                                           const char *fmt, ...);

/* ---- OpenSL ES symbol resolver (libOpenSLES.so) ----
 * Resolves the SLES entry points an Android .so may import:
 *   slCreateEngine, slQueryNumSupportedInterfaces,
 *   slQuerySupportedInterfaces, slCreateOutputMix (alias),
 *   and the SL_IID_* interface-ID constants.
 *
 * Apps that link libOpenSLES.so and call slCreateEngine directly (instead
 * of our opensl_bridge_* API) get a minimal SLES object-model wrapper whose
 * Realize / GetInterface / Destroy forward to opensl_bridge_engine_create
 * etc. The full SLES object model (SLEngineItf::CreateAudioPlayer,
 * SLBufferQueueItf::Enqueue, SLPlayItf::SetPlayState, ...) is partially
 * bridged — enough that a typical app's audio path works; the rest logs
 * WARN and returns SL_RESULT_FEATURE_UNSUPPORTED. See bionic_shim.c. */
void *bionic_opensl_resolve(const char *name);

/* ---- Framework accessor used by the SLES wrapper + AudioTrack Java stub ----
 * Returns the global engine created by apkcontainer_audio_start(), or NULL.
 * Implemented in opensl_bridge.mm. */
struct sl_engine;
struct sl_engine *opensl_bridge_get_global_engine(void);

#ifdef __cplusplus
}
#endif
#endif
