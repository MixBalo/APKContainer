/*
 * dex_interp.h — Dalvik bytecode interpreter + object model
 *
 * Status: REAL subset interpreter. Implements the common Dalvik opcode set
 *         (const, move, arithmetic, branches, new-instance, iget/iput/sget/sput,
 *         invoke-{static,virtual,direct,interface}, aget/aput, return, throw,
 *         try/catch) plus a registry of built-in framework method stubs so
 *         trivial Android apps (single Activity, log/println, basic String/
 *         StringBuilder, Math.max/min/abs, Integer.parseInt, android.util.Log,
 *         Activity lifecycle no-ops, GLSurfaceView Java-side no-ops) load and
 *         execute without a real android.jar DEX.
 *
 * Scope and LIMITATIONS (read before relying on this):
 *   - NO GC. Heap is a bump allocator with a free-list; the interpreter logs
 *     a WARN when heap usage exceeds 64 MiB. Long-running apps will OOM.
 *   - NO real Java Reflection, ClassLoader, JNI native code execution from
 *     within DEX (JNI_OnLoad runs through the separate jni_bridge).
 *   - monitor-enter / monitor-execute are NO-OPS (logged at DEBUG). The
 *     interpreter is single-threaded; concurrent execution is undefined.
 *   - check-cast accepts; logs a WARN if the runtime class would mismatch
 *     (partial — actual ClassCastException is not raised unless THROW fires).
 *   - filled-new-array of object arrays is partial (creates the array but
 *     does not perform element type checking).
 *   - invoke-polymorphic / invoke-custom / MethodHandle are NOT implemented
 *     (unknown-opcode path: LOGE + return error).
 *   - Framework classes NOT in the registered stub list (see dex_interp.cpp
 *     register_framework_stubs()) will surface as either ClassNotFoundException
 *     (no DEX entry, no stub) or NoSuchMethodError (DEX class present but
 *     method has no code_item and no stub). Both are logged.
 *
 * Object model:
 *   - dex_obj_t: heap object. Has a class pointer, a kind tag (NORMAL, STRING,
 *     ARRAY, STRING_BUILDER, STUB), and either instance fields or special
 *     payload (string utf8 buffer / array data / SB buffer).
 *   - dex_cls_t: resolved class. Has descriptor, super, instance field layout,
 *     vtable (virtual methods), direct methods, static fields storage, and a
 *     pointer back to its source dex_file.
 */
#ifndef APKCONTAINER_DEX_INTERP_H
#define APKCONTAINER_DEX_INTERP_H
#include "dex_loader.h"
#ifdef __cplusplus
extern "C" {
#endif

// Opaque VM/Env
typedef struct dex_vm  dex_vm_t;
typedef struct dex_obj dex_obj_t;     // a heap-allocated object/array
typedef struct dex_cls dex_cls_t;     // a resolved class

typedef union dex_value {
    int32_t  i32;
    int64_t  i64;
    float    f32;
    double   f64;
    void    *ptr;        // dex_obj_t* or char* (for strings)
} dex_value_t;

// VM lifecycle
dex_vm_t *dex_vm_create(void);
void      dex_vm_destroy(dex_vm_t *vm);

// Load a DEX file into the VM (multiple allowed). Returns 0 on success.
int  dex_vm_load_dex(dex_vm_t *vm, const dex_file_t *dex);

// Resolve a class by descriptor; creates internal class struct.
dex_cls_t *dex_vm_resolve_class(dex_vm_t *vm, const char *descriptor);

// Return the descriptor ("Lcom/example/Foo;") of a resolved class.
const char *dex_cls_descriptor(dex_cls_t *cls);

// Allocate a new instance. Returns NULL on failure.
dex_obj_t *dex_new_instance(dex_vm_t *vm, const char *descriptor);

// Invoke a method by (class descriptor, name, shorty). args are the receiver
// (NULL for static) + arguments. result_out gets the return value (for non-void).
// shorty is the JNI-style shorty, e.g. "V" void, "IL" (int, Object) etc.
int  dex_invoke(dex_vm_t *vm,
                const char *cls_desc, const char *name, const char *shorty,
                dex_value_t *args, int arg_count,
                dex_value_t *result_out);

// Field access (instance + static) — used by JNI bridge
int  dex_get_static_field(dex_vm_t *vm, const char *cls, const char *name,
                          dex_value_t *out);
int  dex_set_static_field(dex_vm_t *vm, const char *cls, const char *name,
                          dex_value_t val);
int  dex_get_field(dex_vm_t *vm, dex_obj_t *obj, const char *name, dex_value_t *out);
int  dex_set_field(dex_vm_t *vm, dex_obj_t *obj, const char *name, dex_value_t val);

// String allocation
dex_obj_t *dex_new_string_utf(dex_vm_t *vm, const char *utf8);
const char *dex_string_utf(dex_obj_t *str);

// Per-VM configuration used by framework stubs (System.out's PrintStream,
// Context.getPackageName, Context.getFilesDir, etc.).
void dex_vm_set_package_id(dex_vm_t *vm, const char *pkg);
void dex_vm_set_sandbox_root(dex_vm_t *vm, const char *path);

#ifdef __cplusplus
}
#endif
#endif
