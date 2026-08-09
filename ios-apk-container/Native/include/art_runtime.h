/*
 * art_runtime.h — ART (Dalvik) interpreter wrapper, interpreter-only
 *
 * Status: REAL. Backed by the DEX loader (Native/ART/dex_loader.cpp) and
 *         the subset Dalvik bytecode interpreter (Native/ART/dex_interp.cpp).
 *         The interpreter executes the common Dalvik opcode set + a registry
 *         of built-in framework method stubs. Trivial Android apps (single
 *         Activity, log/println, basic String/StringBuilder/Math/Integer,
 *         Activity lifecycle no-ops, GLSurfaceView Java-side no-ops) load
 *         and execute. Apps touching unimplemented framework classes or
 *         uncommon bytecode paths fail with honest error codes + logs.
 *
 *         This is NOT full AOSP ART. No JIT/AOT, no GC, no real ClassLoader,
 *         no Reflection, no JNI native code execution from within DEX. The
 *         JNI bridge (Native/JNI) routes native calls separately.
 *
 * Deviation from earlier stub: `art_runtime_create_vm` now takes a
 * pre-extracted `classes_dex_path` instead of an `apk_path`. The Swift
 * ApkInstaller extracts classes.dex to the sandbox before calling us.
 * Multi-dex (classes2.dex etc.) is supported via additional
 * `art_runtime_load_dex` calls.
 */
#ifndef APKCONTAINER_ART_RUNTIME_H
#define APKCONTAINER_ART_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct art_vm       art_vm_t;        /* == dex_vm_t  (opaque)        */
typedef struct art_env      art_env_t;       /* unused in v1                  */
typedef struct art_class    art_class_t;     /* == dex_cls_t  (opaque)       */
typedef struct art_method   art_method_t;    /* unused in v1                  */
typedef struct art_object   art_object_t;    /* == dex_obj_t  (opaque)       */

/* One-time runtime init (creates the global dex_vm_t with framework stubs).
 * Returns 0 on success. Idempotent. */
int  art_runtime_init(void);

/* Per-package VM instance. `classes_dex_path` is a pre-extracted classes.dex
 * file (Swift ApkInstaller extracts it from the APK into the sandbox).
 * `sandbox_root` is used for java.io paths and SharedPreferences backing.
 * `package_id` is returned by Context.getPackageName().
 * On success, *out_vm is a dex_vm_t* cast to art_vm_t*. */
int  art_runtime_create_vm(const char *package_id,
                           const char *classes_dex_path,
                           const char *sandbox_root,
                           art_vm_t **out_vm);

/* Load + link an additional DEX file (e.g. classes2.dex) into the given VM. */
int  art_runtime_load_dex(art_vm_t *vm, const char *dex_path);

/* Find a class by binary name. Accepts either a dotted binary name
 * ("com.example.Foo") or a descriptor ("Lcom/example/Foo;").
 * On success, *out_cls is a dex_cls_t* cast to art_class_t*. */
int  art_runtime_find_class(art_vm_t *vm, const char *binary_name,
                            art_class_t **out_cls);

/* Invoke the launcher Activity's lifecycle entry points.
 * `activity_class` is the binary name or descriptor of the Activity subclass.
 * `event` is 0=onCreate ... 5=onDestroy.
 * For onCreate (event=0), a stub Bundle is passed as the argument. */
int  art_runtime_dispatch_activity(art_vm_t *vm, const char *activity_class,
                                   int event);

/* Hand our VM* to the JNI bridge so native .so code can RegisterNatives.
 * Returns NULL if art_runtime_init has not succeeded. */
art_vm_t *art_runtime_get_javavm_handle(void);

/* Tear down a per-package VM (unloads dex + frees heap). */
int  art_runtime_destroy_vm(art_vm_t *vm);

#ifdef __cplusplus
}
#endif
#endif
