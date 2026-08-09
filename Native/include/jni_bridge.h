/*
 * jni_bridge.h — JNIEnv* / JavaVM* implementation backed by the DEX interpreter
 *
 * Status: REAL for the common path. The full JNINativeInterface_ vtable (~230
 *         entries) is defined; ~60 entries commonly used by NDK code are
 *         implemented against dex_interp.h; the rest log + return defaults.
 *         Native .so code that calls JNI_OnLoad, FindClass, RegisterNatives,
 *         GetMethodID, GetFieldID, NewStringUTF, Call*Method, Get*Field,
 *         NewGlobalRef, GetJavaVM works. Exotic entries (DefineClass,
 *         ToReflectedMethod, NewDirectByteBuffer is REAL) are marked.
 *
 * See docs/ARCHITECTURE.md §3, docs/CAPABILITY_MATRIX.md §4.
 */
#ifndef APKCONTAINER_JNI_BRIDGE_H
#define APKCONTAINER_JNI_BRIDGE_H

#include <stddef.h>
#include <stdint.h>
#include "art_runtime.h"

/* Standard JNI opaque types — defined here so we don't need Android's jni.h */
#ifdef __cplusplus
extern "C" {
#endif

/* Forward decls of the vtable structs (full definitions in jni_bridge.cpp). */
struct JNINativeInterface_;
struct JNIInvokeInterface_;

typedef const struct JNINativeInterface_ *JNINativeInterface;
typedef const struct JNIInvokeInterface_ *JNIInvokeInterface;

#ifdef __cplusplus
}  /* extern "C" */
#endif

/* In standard JNI, JNIEnv is `const struct JNINativeInterface_ **` (pointer to
 * pointer to vtable). We follow that exact layout so native code's
 * `env->FindClass(env, ...)` (which expands to `(*env)->FindClass(env, ...)`)
 * works without adaptation. */
typedef const struct JNINativeInterface_ **JNIEnv_custom_t;

/* Same for JavaVM. */
typedef const struct JNIInvokeInterface_ **JavaVM_custom_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Build the JavaVM (JNIInvokeInterface) + JNIEnv (JNINativeInterface) vtables
 * and bind them to the ART handle from art_runtime_get_javavm_handle().
 * Idempotent. Returns 0 on success. */
int  jni_bridge_init(art_vm_t *art_vm);

/* Return the JavaVM* to hand to JNI_OnLoad. Layout: pointer to pointer to
 * JNIInvokeInterface vtable. */
void *jni_bridge_get_javavm(void);

/* Return a JNIEnv* for the current thread (AttachCurrentThread semantics).
 * Same layout: pointer to pointer to JNINativeInterface vtable. */
void *jni_bridge_attach_env(void);

/* Recorded native method (from RegisterNatives). */
typedef struct {
    char method_name[128];
    char signature[128];
    void *fnptr;
} jni_registered_native_t;

/* Look up a registered native for a class+name+sig. Returns NULL if none.
 * `class_binary` is dotted ("com.example.Foo"); we store dotted. */
jni_registered_native_t *jni_bridge_lookup_native(const char *class_binary,
                                                  const char *name,
                                                  const char *sig);

/* Called by native .so code via the JNIEnv vtable's RegisterNatives entry. */
int  jni_bridge_register_natives(const char *class_binary,
                                 const char *name,
                                 const char *sig,
                                 void *fnptr);

#ifdef __cplusplus
}
#endif
#endif
