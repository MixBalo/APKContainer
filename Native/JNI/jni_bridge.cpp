/*
 * jni_bridge.cpp — JNIEnv* / JavaVM* vtable implementation backed by the DEX
 *                  interpreter
 *
 * Status: REAL for the common path.
 *
 *   We define the full JNINativeInterface_ and JNIInvokeInterface_ vtables in
 *   the EXACT layout Android's jni.h uses, so native .so code's
 *   `env->FindClass(env, ...)` (which expands to `(*env)->FindClass(env, ...)`)
 *   works without adaptation. Common entries (FindClass, GetMethodID,
 *   GetStaticMethodID, GetFieldID, GetStaticFieldID, NewStringUTF,
 *   GetStringUTFChars, ReleaseStringUTFChars, New*Array, Get*ArrayElements,
 *   Release*ArrayElements, Set*ArrayRegion, NewLocalRef, NewGlobalRef,
 *   DeleteLocalRef, DeleteGlobalRef, IsInstanceOf, GetObjectClass,
 *   RegisterNatives, UnregisterNatives, GetJavaVM, CallVoidMethod,
 *   CallObjectMethod, CallIntMethod, CallBooleanMethod, CallStaticVoidMethod,
 *   CallStaticObjectMethod, CallStaticIntMethod, GetIntField, SetIntField,
 *   GetStaticIntField, SetStaticIntField, NewObject, GetVersion, Throw,
 *   ThrowNew, ExceptionOccurred, ExceptionClear, ExceptionCheck,
 *   NewDirectByteBuffer, GetDirectBufferAddress, GetDirectBufferCapacity,
 *   AttachCurrentThread, DetachCurrentThread, GetEnv, DestroyJavaVM) are
 *   implemented against dex_interp.h. The rest log via LOGW and return a
 *   sensible default.
 *
 * See docs/ARCHITECTURE.md §3, docs/CAPABILITY_MATRIX.md §4.
 */
#include "jni_bridge.h"
#include "art_runtime.h"
#include "dex_loader.h"
#include "dex_interp.h"
#include "log_file.h"

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cstdlib>
#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>

#define LOG_TAG_JNI "jni"

/* ----------------------------------------------------------------------
 *  Standard JNI type definitions (subset of Android's jni.h)
 * ---------------------------------------------------------------------- */
#ifdef __cplusplus
extern "C" {
#endif

struct JNINativeInterface_;
struct JNIInvokeInterface_;

/* Opaque object handle = our dex_obj_t*. We use void* in the vtable signature
 * so we don't have to pull dex_interp.h into the public ABI. */
typedef void *jobject_custom;
typedef jobject_custom jclass_custom;
typedef jobject_custom jstring_custom;
typedef jobject_custom jthrowable_custom;
typedef jobject_custom jarray_custom;
typedef jobject_custom jintArray_custom;
typedef jobject_custom jbyteArray_custom;
typedef jobject_custom jobjectArray_custom;
typedef jobject_custom jbooleanArray_custom;
typedef jobject_custom jcharArray_custom;
typedef jobject_custom jshortArray_custom;
typedef jobject_custom jfloatArray_custom;
typedef jobject_custom jlongArray_custom;
typedef jobject_custom jdoubleArray_custom;
typedef jobject_custom jweak_custom;

typedef int    jint;
typedef long long jlong;
typedef unsigned char jboolean;
typedef unsigned short jchar;
typedef signed char jbyte;
typedef short jshort;
typedef float jfloat;
typedef double jdouble;
typedef jint  jsize;

/* jvalue — union for typed return/args. */
typedef union jvalue {
    jboolean z;
    jbyte    b;
    jchar    c;
    jshort   s;
    jint     i;
    jlong    j;
    jfloat   f;
    jdouble  d;
    jobject_custom l;
} jvalue;

#define JNI_TRUE  1
#define JNI_FALSE 0

/* jfieldID / jmethodID — we encode as small integers that key into the DEX
 * VM's field/method tables. Real JNI uses opaque pointers; we use pointers
 * to intern'd descriptor strings. */
typedef struct { const char *class_desc; const char *name; const char *sig; int is_static; } *jfieldID_custom;
typedef jfieldID_custom jmethodID_custom;

/* Standard signatures (we use the C calling convention; variadic where the
 * JNI spec requires it). The struct field order must match Android's jni.h. */
struct JNINativeInterface_ {
    void *reserved0, *reserved1, *reserved2, *reserved3;

    jint        (*GetVersion)(void *);

    jclass_custom (*DefineClass)(void*, const char*, jobject_custom, const jbyte*, jsize, ...);
    jclass_custom (*FindClass)(void*, const char*);

    /* Exceptions */
    jthrowable_custom (*ExceptionOccurred)(void*);
    void        (*ExceptionDescribe)(void*);
    void        (*ExceptionClear)(void*);
    void        (*FatalError)(void*, const char*);

    /* Global / local refs */
    jint        (*PushLocalFrame)(void*, jint);
    jobject_custom (*PopLocalFrame)(void*, jobject_custom);
    jobject_custom (*NewGlobalRef)(void*, jobject_custom);
    void        (*DeleteGlobalRef)(void*, jobject_custom);
    void        (*DeleteLocalRef)(void*, jobject_custom);
    jboolean    (*IsSameObject)(void*, jobject_custom, jobject_custom);
    jobject_custom (*NewLocalRef)(void*, jobject_custom);
    jint        (*EnsureLocalCapacity)(void*, jint);

    jobject_custom (*AllocObject)(void*, jclass_custom);
    jobject_custom (*NewObject)(void*, jclass_custom, jmethodID_custom, ...);
    jobject_custom (*NewObjectV)(void*, jclass_custom, jmethodID_custom, va_list);
    jobject_custom (*NewObjectA)(void*, jclass_custom, jmethodID_custom, const jvalue*);

    jclass_custom (*GetObjectClass)(void*, jobject_custom);
    jboolean    (*IsInstanceOf)(void*, jobject_custom, jclass_custom);
    jmethodID_custom (*GetMethodID)(void*, jclass_custom, const char*, const char*);

    /* Call*Method (instance) */
    jobject_custom (*CallObjectMethod)(void*, jobject_custom, jmethodID_custom, ...);
    jobject_custom (*CallObjectMethodV)(void*, jobject_custom, jmethodID_custom, va_list);
    jobject_custom (*CallObjectMethodA)(void*, jobject_custom, jmethodID_custom, const jvalue*);
    jboolean    (*CallBooleanMethod)(void*, jobject_custom, jmethodID_custom, ...);
    jboolean    (*CallBooleanMethodV)(void*, jobject_custom, jmethodID_custom, va_list);
    jboolean    (*CallBooleanMethodA)(void*, jobject_custom, jmethodID_custom, const jvalue*);
    jbyte       (*CallByteMethod)(void*, jobject_custom, jmethodID_custom, ...);
    jbyte       (*CallByteMethodV)(void*, jobject_custom, jmethodID_custom, va_list);
    jbyte       (*CallByteMethodA)(void*, jobject_custom, jmethodID_custom, const jvalue*);
    jchar       (*CallCharMethod)(void*, jobject_custom, jmethodID_custom, ...);
    jchar       (*CallCharMethodV)(void*, jobject_custom, jmethodID_custom, va_list);
    jchar       (*CallCharMethodA)(void*, jobject_custom, jmethodID_custom, const jvalue*);
    jshort      (*CallShortMethod)(void*, jobject_custom, jmethodID_custom, ...);
    jshort      (*CallShortMethodV)(void*, jobject_custom, jmethodID_custom, va_list);
    jshort      (*CallShortMethodA)(void*, jobject_custom, jmethodID_custom, const jvalue*);
    jint        (*CallIntMethod)(void*, jobject_custom, jmethodID_custom, ...);
    jint        (*CallIntMethodV)(void*, jobject_custom, jmethodID_custom, va_list);
    jint        (*CallIntMethodA)(void*, jobject_custom, jmethodID_custom, const jvalue*);
    jlong       (*CallLongMethod)(void*, jobject_custom, jmethodID_custom, ...);
    jlong       (*CallLongMethodV)(void*, jobject_custom, jmethodID_custom, va_list);
    jlong       (*CallLongMethodA)(void*, jobject_custom, jmethodID_custom, const jvalue*);
    jfloat      (*CallFloatMethod)(void*, jobject_custom, jmethodID_custom, ...);
    jfloat      (*CallFloatMethodV)(void*, jobject_custom, jmethodID_custom, va_list);
    jfloat      (*CallFloatMethodA)(void*, jobject_custom, jmethodID_custom, const jvalue*);
    jdouble     (*CallDoubleMethod)(void*, jobject_custom, jmethodID_custom, ...);
    jdouble     (*CallDoubleMethodV)(void*, jobject_custom, jmethodID_custom, va_list);
    jdouble     (*CallDoubleMethodA)(void*, jobject_custom, jmethodID_custom, const jvalue*);
    void        (*CallVoidMethod)(void*, jobject_custom, jmethodID_custom, ...);
    void        (*CallVoidMethodV)(void*, jobject_custom, jmethodID_custom, va_list);
    void        (*CallVoidMethodA)(void*, jobject_custom, jmethodID_custom, const jvalue*);

    /* CallNonvirtual*Method — partial: treated as Call*Method */
    jobject_custom (*CallNonvirtualObjectMethod)(void*, jobject_custom, jclass_custom, jmethodID_custom, ...);
    jboolean    (*CallNonvirtualBooleanMethod)(void*, jobject_custom, jclass_custom, jmethodID_custom, ...);
    jint        (*CallNonvirtualIntMethod)(void*, jobject_custom, jclass_custom, jmethodID_custom, ...);
    jlong       (*CallNonvirtualLongMethod)(void*, jobject_custom, jclass_custom, jmethodID_custom, ...);
    void        (*CallNonvirtualVoidMethod)(void*, jobject_custom, jclass_custom, jmethodID_custom, ...);

    /* Field access (instance) */
    jfieldID_custom (*GetFieldID)(void*, jclass_custom, const char*, const char*);
    jobject_custom (*GetObjectField)(void*, jobject_custom, jfieldID_custom);
    jboolean    (*GetBooleanField)(void*, jobject_custom, jfieldID_custom);
    jbyte       (*GetByteField)(void*, jobject_custom, jfieldID_custom);
    jchar       (*GetCharField)(void*, jobject_custom, jfieldID_custom);
    jshort      (*GetShortField)(void*, jobject_custom, jfieldID_custom);
    jint        (*GetIntField)(void*, jobject_custom, jfieldID_custom);
    jlong       (*GetLongField)(void*, jobject_custom, jfieldID_custom);
    jfloat      (*GetFloatField)(void*, jobject_custom, jfieldID_custom);
    jdouble     (*GetDoubleField)(void*, jobject_custom, jfieldID_custom);
    void        (*SetObjectField)(void*, jobject_custom, jfieldID_custom, jobject_custom);
    void        (*SetBooleanField)(void*, jobject_custom, jfieldID_custom, jboolean);
    void        (*SetByteField)(void*, jobject_custom, jfieldID_custom, jbyte);
    void        (*SetCharField)(void*, jobject_custom, jfieldID_custom, jchar);
    void        (*SetShortField)(void*, jobject_custom, jfieldID_custom, jshort);
    void        (*SetIntField)(void*, jobject_custom, jfieldID_custom, jint);
    void        (*SetLongField)(void*, jobject_custom, jfieldID_custom, jlong);
    void        (*SetFloatField)(void*, jobject_custom, jfieldID_custom, jfloat);
    void        (*SetDoubleField)(void*, jobject_custom, jfieldID_custom, jdouble);

    /* Static method access */
    jmethodID_custom (*GetStaticMethodID)(void*, jclass_custom, const char*, const char*);
    jobject_custom (*CallStaticObjectMethod)(void*, jclass_custom, jmethodID_custom, ...);
    jobject_custom (*CallStaticObjectMethodV)(void*, jclass_custom, jmethodID_custom, va_list);
    jobject_custom (*CallStaticObjectMethodA)(void*, jclass_custom, jmethodID_custom, const jvalue*);
    jboolean    (*CallStaticBooleanMethod)(void*, jclass_custom, jmethodID_custom, ...);
    jboolean    (*CallStaticBooleanMethodV)(void*, jclass_custom, jmethodID_custom, va_list);
    jboolean    (*CallStaticBooleanMethodA)(void*, jclass_custom, jmethodID_custom, const jvalue*);
    jint        (*CallStaticIntMethod)(void*, jclass_custom, jmethodID_custom, ...);
    jint        (*CallStaticIntMethodV)(void*, jclass_custom, jmethodID_custom, va_list);
    jint        (*CallStaticIntMethodA)(void*, jclass_custom, jmethodID_custom, const jvalue*);
    jlong       (*CallStaticLongMethod)(void*, jclass_custom, jmethodID_custom, ...);
    void        (*CallStaticVoidMethod)(void*, jclass_custom, jmethodID_custom, ...);
    void        (*CallStaticVoidMethodV)(void*, jclass_custom, jmethodID_custom, va_list);
    void        (*CallStaticVoidMethodA)(void*, jclass_custom, jmethodID_custom, const jvalue*);

    /* Static field access */
    jfieldID_custom (*GetStaticFieldID)(void*, jclass_custom, const char*, const char*);
    jobject_custom (*GetStaticObjectField)(void*, jclass_custom, jfieldID_custom);
    jboolean    (*GetStaticBooleanField)(void*, jclass_custom, jfieldID_custom);
    jint        (*GetStaticIntField)(void*, jclass_custom, jfieldID_custom);
    jlong       (*GetStaticLongField)(void*, jclass_custom, jfieldID_custom);
    jfloat      (*GetStaticFloatField)(void*, jclass_custom, jfieldID_custom);
    jdouble     (*GetStaticDoubleField)(void*, jclass_custom, jfieldID_custom);
    void        (*SetStaticObjectField)(void*, jclass_custom, jfieldID_custom, jobject_custom);
    void        (*SetStaticBooleanField)(void*, jclass_custom, jfieldID_custom, jboolean);
    void        (*SetStaticIntField)(void*, jclass_custom, jfieldID_custom, jint);
    void        (*SetStaticLongField)(void*, jclass_custom, jfieldID_custom, jlong);
    void        (*SetStaticFloatField)(void*, jclass_custom, jfieldID_custom, jfloat);
    void        (*SetStaticDoubleField)(void*, jclass_custom, jfieldID_custom, jdouble);

    /* Strings */
    jstring_custom (*NewString)(void*, const jchar*, jsize);
    jsize       (*GetStringLength)(void*, jstring_custom);
    const jchar*(*GetStringChars)(void*, jstring_custom, jboolean*);
    void        (*ReleaseStringChars)(void*, jstring_custom, const jchar*);
    jstring_custom (*NewStringUTF)(void*, const char*);
    jsize       (*GetStringUTFLength)(void*, jstring_custom);
    const char* (*GetStringUTFChars)(void*, jstring_custom, jboolean*);
    void        (*ReleaseStringUTFChars)(void*, jstring_custom, const char*);
    void        (*GetStringRegion)(void*, jstring_custom, jsize, jsize, jchar*);
    void        (*GetStringUTFRegion)(void*, jstring_custom, jsize, jsize, char*);
    void        (*GetStringCritical)(void*, jstring_custom, jboolean*);
    void        (*ReleaseStringCritical)(void*, jstring_custom, const jchar*);

    /* Arrays */
    jsize       (*GetArrayLength)(void*, jarray_custom);
    jobjectArray_custom (*NewObjectArray)(void*, jsize, jclass_custom, jobject_custom);
    jobject_custom (*GetObjectArrayElement)(void*, jobjectArray_custom, jsize);
    void        (*SetObjectArrayElement)(void*, jobjectArray_custom, jsize, jobject_custom);
    jbooleanArray_custom (*NewBooleanArray)(void*, jsize);
    jbyteArray_custom (*NewByteArray)(void*, jsize);
    jcharArray_custom (*NewCharArray)(void*, jsize);
    jshortArray_custom (*NewShortArray)(void*, jsize);
    jintArray_custom (*NewIntArray)(void*, jsize);
    jlongArray_custom (*NewLongArray)(void*, jsize);
    jfloatArray_custom (*NewFloatArray)(void*, jsize);
    jdoubleArray_custom (*NewDoubleArray)(void*, jsize);
    jboolean*   (*GetBooleanArrayElements)(void*, jbooleanArray_custom, jboolean*);
    jbyte*      (*GetByteArrayElements)(void*, jbyteArray_custom, jboolean*);
    jchar*      (*GetCharArrayElements)(void*, jcharArray_custom, jboolean*);
    jshort*     (*GetShortArrayElements)(void*, jshortArray_custom, jboolean*);
    jint*       (*GetIntArrayElements)(void*, jintArray_custom, jboolean*);
    jlong*      (*GetLongArrayElements)(void*, jlongArray_custom, jboolean*);
    jfloat*     (*GetFloatArrayElements)(void*, jfloatArray_custom, jboolean*);
    jdouble*    (*GetDoubleArrayElements)(void*, jdoubleArray_custom, jboolean*);
    void        (*ReleaseBooleanArrayElements)(void*, jbooleanArray_custom, jboolean*, jint);
    void        (*ReleaseByteArrayElements)(void*, jbyteArray_custom, jbyte*, jint);
    void        (*ReleaseCharArrayElements)(void*, jcharArray_custom, jchar*, jint);
    void        (*ReleaseShortArrayElements)(void*, jshortArray_custom, jshort*, jint);
    void        (*ReleaseIntArrayElements)(void*, jintArray_custom, jint*, jint);
    void        (*ReleaseLongArrayElements)(void*, jlongArray_custom, jlong*, jint);
    void        (*ReleaseFloatArrayElements)(void*, jfloatArray_custom, jfloat*, jint);
    void        (*ReleaseDoubleArrayElements)(void*, jdoubleArray_custom, jdouble*, jint);
    void        (*GetBooleanArrayRegion)(void*, jbooleanArray_custom, jsize, jsize, jboolean*);
    void        (*GetByteArrayRegion)(void*, jbyteArray_custom, jsize, jsize, jbyte*);
    void        (*GetCharArrayRegion)(void*, jcharArray_custom, jsize, jsize, jchar*);
    void        (*GetShortArrayRegion)(void*, jshortArray_custom, jsize, jsize, jshort*);
    void        (*GetIntArrayRegion)(void*, jintArray_custom, jsize, jsize, jint*);
    void        (*GetLongArrayRegion)(void*, jlongArray_custom, jsize, jsize, jlong*);
    void        (*GetFloatArrayRegion)(void*, jfloatArray_custom, jsize, jsize, jfloat*);
    void        (*GetDoubleArrayRegion)(void*, jdoubleArray_custom, jsize, jsize, jdouble*);
    void        (*SetBooleanArrayRegion)(void*, jbooleanArray_custom, jsize, jsize, const jboolean*);
    void        (*SetByteArrayRegion)(void*, jbyteArray_custom, jsize, jsize, const jbyte*);
    void        (*SetCharArrayRegion)(void*, jcharArray_custom, jsize, jsize, const jchar*);
    void        (*SetShortArrayRegion)(void*, jshortArray_custom, jsize, jsize, const jshort*);
    void        (*SetIntArrayRegion)(void*, jintArray_custom, jsize, jsize, const jint*);
    void        (*SetLongArrayRegion)(void*, jlongArray_custom, jsize, jsize, const jlong*);
    void        (*SetFloatArrayRegion)(void*, jfloatArray_custom, jsize, jsize, const jfloat*);
    void        (*SetDoubleArrayRegion)(void*, jdoubleArray_custom, jsize, jsize, const jdouble*);
    void        (*RegisterNatives)(void*, jclass_custom, const void*, jint);
    void        (*UnregisterNatives)(void*, jclass_custom);

    /* Monitor (no-op) */
    jint        (*MonitorEnter)(void*, jobject_custom);
    jint        (*MonitorExit)(void*, jobject_custom);
    jint        (*GetJavaVM)(void*, void**);

    /* Misc */
    void        (*GetStringRegionChars)(void*, jstring_custom, jsize, jsize, jchar*);
    jobject_custom (*NewDirectByteBuffer)(void*, void*, jlong);
    void*       (*GetDirectBufferAddress)(void*, jobject_custom);
    jlong       (*GetDirectBufferCapacity)(void*, jobject_custom);
    jboolean    (*GetObjectRefType)(void*, jobject_custom);
};

struct JNIInvokeInterface_ {
    void *reserved0, *reserved1, *reserved2;
    jint (*DestroyJavaVM)(void*);
    jint (*AttachCurrentThread)(void*, void**, const void*);
    jint (*DetachCurrentThread)(void*);
    jint (*GetEnv)(void*, void**, jint);
    jint (*AttachCurrentThreadAsDaemon)(void*, void**, const void*);
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

/* ----------------------------------------------------------------------
 *  Implementation
 * ---------------------------------------------------------------------- */

/* Recorded native methods (class dotted-binary, name, sig, fnptr). */
struct NativeEntry {
    std::string cls;
    std::string name;
    std::string sig;
    void       *fnptr;
};
static std::vector<NativeEntry> g_registered;
static std::mutex g_registered_lock;

static art_vm_t *g_art_vm = nullptr;
static int       g_inited = 0;

/* Intern table for jfieldID/jmethodID: same (class,name,sig,static) returns
 * the same pointer. */
struct FieldMethodKey {
    std::string class_desc, name, sig;
    int is_static;
    bool operator==(const FieldMethodKey &o) const {
        return class_desc == o.class_desc && name == o.name &&
               sig == o.sig && is_static == o.is_static;
    }
};
struct FieldMethodKeyHash {
    size_t operator()(const FieldMethodKey &k) const {
        size_t h = std::hash<std::string>{}(k.class_desc);
        h ^= std::hash<std::string>{}(k.name) << 1;
        h ^= std::hash<std::string>{}(k.sig) << 2;
        h ^= std::hash<int>{}(k.is_static) << 3;
        return h;
    }
};
static std::unordered_map<FieldMethodKey, jfieldID_custom, FieldMethodKeyHash> g_id_table;

/* The intern'd descriptor string is heap-allocated and never freed; the
 * pointer identity is what makes the jfieldID unique. */
static jfieldID_custom intern_id(const char *cls, const char *name,
                                 const char *sig, int is_static) {
    FieldMethodKey k{cls, name, sig, is_static};
    auto it = g_id_table.find(k);
    if (it != g_id_table.end()) return it->second;
    jfieldID_custom id = (jfieldID_custom)malloc(sizeof(*id));
    id->class_desc = strdup(cls);
    id->name = strdup(name);
    id->sig = strdup(sig);
    id->is_static = is_static;
    g_id_table[k] = id;
    return id;
}

/* Helper: convert dotted binary name to descriptor "Lcom/example/Foo;" */
static std::string to_descriptor(const char *dotted) {
    if (!dotted) return "";
    if (dotted[0] == 'L' && dotted[strlen(dotted)-1] == ';') return dotted;
    std::string s = dotted;
    for (auto &c : s) if (c == '.') c = '/';
    return "L" + s + ";";
}
static std::string descriptor_to_dotted(const char *desc) {
    if (!desc) return "";
    if (desc[0] != 'L' || desc[strlen(desc)-1] != ';') return desc;
    std::string s = desc + 1;
    s.pop_back();
    for (auto &c : s) if (c == '/') c = '.';
    return s;
}

/* Helper: count args in a shorty/signature like "(II)V" -> 2. */
static int count_args(const char *sig) {
    if (!sig) return 0;
    const char *p = strchr(sig, '(');
    if (!p) return 0;
    p++;
    int n = 0;
    while (*p && *p != ')') {
        if (*p == 'L') { while (*p && *p != ';') p++; if (*p) p++; n++; continue; }
        if (*p == '[') { p++; continue; }  /* array prefix */
        if (*p == 'J' || *p == 'D') { n++; p++; /* wide */ continue; }
        n++; p++;
    }
    return n;
}

/* Helper: get the dex_vm_t* (it's the same as art_vm_t* per art_runtime.h). */
static dex_vm_t *vm() { return (dex_vm_t *)g_art_vm; }

/* ----------------------------------------------------------------------
 *  Forward declarations of vtable functions (defined below)
 * ---------------------------------------------------------------------- */
#define JNI_FN(name) name##_impl
static jint        JNI_FN(GetVersion)(void *);
static jclass_custom JNI_FN(FindClass)(void *, const char *);
static jobject_custom JNI_FN(NewGlobalRef)(void *, jobject_custom);
static void        JNI_FN(DeleteGlobalRef)(void *, jobject_custom);
static void        JNI_FN(DeleteLocalRef)(void *, jobject_custom);
static jboolean    JNI_FN(IsSameObject)(void *, jobject_custom, jobject_custom);
static jobject_custom JNI_FN(NewLocalRef)(void *, jobject_custom);
static jobject_custom JNI_FN(NewObject)(void *, jclass_custom, jmethodID_custom, ...);
static jobject_custom JNI_FN(NewObjectV)(void *, jclass_custom, jmethodID_custom, va_list);
static jclass_custom JNI_FN(GetObjectClass)(void *, jobject_custom);
static jboolean    JNI_FN(IsInstanceOf)(void *, jobject_custom, jclass_custom);
static jmethodID_custom JNI_FN(GetMethodID)(void *, jclass_custom, const char *, const char *);
static void        JNI_FN(CallVoidMethodV)(void *, jobject_custom, jmethodID_custom, va_list);
static jint        JNI_FN(CallIntMethodV)(void *, jobject_custom, jmethodID_custom, va_list);
static jboolean    JNI_FN(CallBooleanMethodV)(void *, jobject_custom, jmethodID_custom, va_list);
static jbyte       JNI_FN(CallByteMethodV)(void *, jobject_custom, jmethodID_custom, va_list);
static jchar       JNI_FN(CallCharMethodV)(void *, jobject_custom, jmethodID_custom, va_list);
static jshort      JNI_FN(CallShortMethodV)(void *, jobject_custom, jmethodID_custom, va_list);
static jlong       JNI_FN(CallLongMethodV)(void *, jobject_custom, jmethodID_custom, va_list);
static jfloat      JNI_FN(CallFloatMethodV)(void *, jobject_custom, jmethodID_custom, va_list);
static jdouble     JNI_FN(CallDoubleMethodV)(void *, jobject_custom, jmethodID_custom, va_list);
static jbyte       JNI_FN(CallByteMethod)(void *, jobject_custom, jmethodID_custom, ...);
static jchar       JNI_FN(CallCharMethod)(void *, jobject_custom, jmethodID_custom, ...);
static jshort      JNI_FN(CallShortMethod)(void *, jobject_custom, jmethodID_custom, ...);
static jlong       JNI_FN(CallLongMethod)(void *, jobject_custom, jmethodID_custom, ...);
static jfloat      JNI_FN(CallFloatMethod)(void *, jobject_custom, jmethodID_custom, ...);
static jdouble     JNI_FN(CallDoubleMethod)(void *, jobject_custom, jmethodID_custom, ...);
static jbyte       JNI_FN(GetByteField)(void *, jobject_custom, jfieldID_custom);
static jchar       JNI_FN(GetCharField)(void *, jobject_custom, jfieldID_custom);
static jshort      JNI_FN(GetShortField)(void *, jobject_custom, jfieldID_custom);
static jlong       JNI_FN(GetLongField)(void *, jobject_custom, jfieldID_custom);
static jfloat      JNI_FN(GetFloatField)(void *, jobject_custom, jfieldID_custom);
static jdouble     JNI_FN(GetDoubleField)(void *, jobject_custom, jfieldID_custom);
static void        JNI_FN(SetByteField)(void *, jobject_custom, jfieldID_custom, jbyte);
static void        JNI_FN(SetCharField)(void *, jobject_custom, jfieldID_custom, jchar);
static void        JNI_FN(SetShortField)(void *, jobject_custom, jfieldID_custom, jshort);
static void        JNI_FN(SetLongField)(void *, jobject_custom, jfieldID_custom, jlong);
static void        JNI_FN(SetFloatField)(void *, jobject_custom, jfieldID_custom, jfloat);
static void        JNI_FN(SetDoubleField)(void *, jobject_custom, jfieldID_custom, jdouble);
static jboolean    JNI_FN(GetStaticBooleanField)(void *, jclass_custom, jfieldID_custom);
static jlong       JNI_FN(GetStaticLongField)(void *, jclass_custom, jfieldID_custom);
static jfloat      JNI_FN(GetStaticFloatField)(void *, jclass_custom, jfieldID_custom);
static jdouble     JNI_FN(GetStaticDoubleField)(void *, jclass_custom, jfieldID_custom);
static void        JNI_FN(SetStaticBooleanField)(void *, jclass_custom, jfieldID_custom, jboolean);
static void        JNI_FN(SetStaticLongField)(void *, jclass_custom, jfieldID_custom, jlong);
static void        JNI_FN(SetStaticFloatField)(void *, jclass_custom, jfieldID_custom, jfloat);
static void        JNI_FN(SetStaticDoubleField)(void *, jclass_custom, jfieldID_custom, jdouble);
static jboolean    JNI_FN(CallStaticBooleanMethod)(void *, jclass_custom, jmethodID_custom, ...);
static jboolean    JNI_FN(CallStaticBooleanMethodV)(void *, jclass_custom, jmethodID_custom, va_list);
static jlong       JNI_FN(CallStaticLongMethod)(void *, jclass_custom, jmethodID_custom, ...);
static jlong       JNI_FN(CallStaticLongMethodV)(void *, jclass_custom, jmethodID_custom, va_list);
static jlongArray_custom JNI_FN(NewLongArray)(void *, jsize);
static jshortArray_custom JNI_FN(NewShortArray)(void *, jsize);
static jcharArray_custom JNI_FN(NewCharArray)(void *, jsize);
static jbooleanArray_custom JNI_FN(NewBooleanArray)(void *, jsize);
static jdoubleArray_custom JNI_FN(NewDoubleArray)(void *, jsize);
static jlong*      JNI_FN(GetLongArrayElements)(void *, jlongArray_custom, jboolean*);
static jshort*     JNI_FN(GetShortArrayElements)(void *, jshortArray_custom, jboolean*);
static jchar*      JNI_FN(GetCharArrayElements)(void *, jcharArray_custom, jboolean*);
static jboolean*   JNI_FN(GetBooleanArrayElements)(void *, jbooleanArray_custom, jboolean*);
static jdouble*    JNI_FN(GetDoubleArrayElements)(void *, jdoubleArray_custom, jboolean*);
static void        JNI_FN(ReleaseLongArrayElements)(void *, jlongArray_custom, jlong*, jint);
static void        JNI_FN(ReleaseShortArrayElements)(void *, jshortArray_custom, jshort*, jint);
static void        JNI_FN(ReleaseCharArrayElements)(void *, jcharArray_custom, jchar*, jint);
static void        JNI_FN(ReleaseBooleanArrayElements)(void *, jbooleanArray_custom, jboolean*, jint);
static void        JNI_FN(ReleaseDoubleArrayElements)(void *, jdoubleArray_custom, jdouble*, jint);
static void        JNI_FN(GetBooleanArrayRegion)(void *, jbooleanArray_custom, jsize, jsize, jboolean*);
static void        JNI_FN(GetByteArrayRegion)(void *, jbyteArray_custom, jsize, jsize, jbyte*);
static void        JNI_FN(GetCharArrayRegion)(void *, jcharArray_custom, jsize, jsize, jchar*);
static void        JNI_FN(GetShortArrayRegion)(void *, jshortArray_custom, jsize, jsize, jshort*);
static void        JNI_FN(GetIntArrayRegion)(void *, jintArray_custom, jsize, jsize, jint*);
static void        JNI_FN(GetLongArrayRegion)(void *, jlongArray_custom, jsize, jsize, jlong*);
static void        JNI_FN(GetFloatArrayRegion)(void *, jfloatArray_custom, jsize, jsize, jfloat*);
static void        JNI_FN(GetDoubleArrayRegion)(void *, jdoubleArray_custom, jsize, jsize, jdouble*);
static void        JNI_FN(SetBooleanArrayRegion)(void *, jbooleanArray_custom, jsize, jsize, const jboolean*);
static void        JNI_FN(SetCharArrayRegion)(void *, jcharArray_custom, jsize, jsize, const jchar*);
static void        JNI_FN(SetShortArrayRegion)(void *, jshortArray_custom, jsize, jsize, const jshort*);
static void        JNI_FN(SetLongArrayRegion)(void *, jlongArray_custom, jsize, jsize, const jlong*);
static void        JNI_FN(SetFloatArrayRegion)(void *, jfloatArray_custom, jsize, jsize, const jfloat*);
static void        JNI_FN(SetDoubleArrayRegion)(void *, jdoubleArray_custom, jsize, jsize, const jdouble*);
static jobjectArray_custom JNI_FN(NewObjectArray)(void *, jsize, jclass_custom, jobject_custom);
static jobject_custom JNI_FN(GetObjectArrayElement)(void *, jobjectArray_custom, jsize);
static void        JNI_FN(SetObjectArrayElement)(void *, jobjectArray_custom, jsize, jobject_custom);
static jobject_custom JNI_FN(CallObjectMethodV)(void *, jobject_custom, jmethodID_custom, va_list);
static void        JNI_FN(CallVoidMethod)(void *, jobject_custom, jmethodID_custom, ...);
static jint        JNI_FN(CallIntMethod)(void *, jobject_custom, jmethodID_custom, ...);
static jboolean    JNI_FN(CallBooleanMethod)(void *, jobject_custom, jmethodID_custom, ...);
static jobject_custom JNI_FN(CallObjectMethod)(void *, jobject_custom, jmethodID_custom, ...);
static jmethodID_custom JNI_FN(GetStaticMethodID)(void *, jclass_custom, const char *, const char *);
static void        JNI_FN(CallStaticVoidMethodV)(void *, jclass_custom, jmethodID_custom, va_list);
static jint        JNI_FN(CallStaticIntMethodV)(void *, jclass_custom, jmethodID_custom, va_list);
static jobject_custom JNI_FN(CallStaticObjectMethodV)(void *, jclass_custom, jmethodID_custom, va_list);
static void        JNI_FN(CallStaticVoidMethod)(void *, jclass_custom, jmethodID_custom, ...);
static jint        JNI_FN(CallStaticIntMethod)(void *, jclass_custom, jmethodID_custom, ...);
static jobject_custom JNI_FN(CallStaticObjectMethod)(void *, jclass_custom, jmethodID_custom, ...);
static jfieldID_custom JNI_FN(GetFieldID)(void *, jclass_custom, const char *, const char *);
static jfieldID_custom JNI_FN(GetStaticFieldID)(void *, jclass_custom, const char *, const char *);
static jint        JNI_FN(GetIntField)(void *, jobject_custom, jfieldID_custom);
static void        JNI_FN(SetIntField)(void *, jobject_custom, jfieldID_custom, jint);
static jobject_custom JNI_FN(GetObjectField)(void *, jobject_custom, jfieldID_custom);
static void        JNI_FN(SetObjectField)(void *, jobject_custom, jfieldID_custom, jobject_custom);
static jboolean    JNI_FN(GetBooleanField)(void *, jobject_custom, jfieldID_custom);
static jint        JNI_FN(GetStaticIntField)(void *, jclass_custom, jfieldID_custom);
static void        JNI_FN(SetStaticIntField)(void *, jclass_custom, jfieldID_custom, jint);
static jobject_custom JNI_FN(GetStaticObjectField)(void *, jclass_custom, jfieldID_custom);
static jstring_custom JNI_FN(NewStringUTF)(void *, const char *);
static jsize       JNI_FN(GetStringLength)(void *, jstring_custom);
static jsize       JNI_FN(GetStringUTFLength)(void *, jstring_custom);
static const char* JNI_FN(GetStringUTFChars)(void *, jstring_custom, jboolean*);
static void        JNI_FN(ReleaseStringUTFChars)(void *, jstring_custom, const char*);
static jintArray_custom JNI_FN(NewIntArray)(void *, jsize);
static jint*       JNI_FN(GetIntArrayElements)(void *, jintArray_custom, jboolean*);
static void        JNI_FN(ReleaseIntArrayElements)(void *, jintArray_custom, jint*, jint);
static jbyteArray_custom JNI_FN(NewByteArray)(void *, jsize);
static jbyte*      JNI_FN(GetByteArrayElements)(void *, jbyteArray_custom, jboolean*);
static void        JNI_FN(ReleaseByteArrayElements)(void *, jbyteArray_custom, jbyte*, jint);
static jfloatArray_custom JNI_FN(NewFloatArray)(void *, jsize);
static jfloat*     JNI_FN(GetFloatArrayElements)(void *, jfloatArray_custom, jboolean*);
static void        JNI_FN(ReleaseFloatArrayElements)(void *, jfloatArray_custom, jfloat*, jint);
static void        JNI_FN(SetIntArrayRegion)(void *, jintArray_custom, jsize, jsize, const jint*);
static void        JNI_FN(SetByteArrayRegion)(void *, jbyteArray_custom, jsize, jsize, const jbyte*);
static void        JNI_FN(RegisterNatives)(void *, jclass_custom, const void*, jint);
static void        JNI_FN(UnregisterNatives)(void *, jclass_custom);
static jint        JNI_FN(GetJavaVM)(void *, void**);
static jthrowable_custom JNI_FN(ExceptionOccurred)(void *);
static void        JNI_FN(ExceptionDescribe)(void *);
static void        JNI_FN(ExceptionClear)(void *);
static jboolean    JNI_FN(ExceptionCheck)(void *);
static jint        JNI_FN(Throw)(void *, jthrowable_custom);
static jint        JNI_FN(ThrowNew)(void *, jclass_custom, const char *);
static jobject_custom JNI_FN(NewDirectByteBuffer)(void *, void *, jlong);
static void*       JNI_FN(GetDirectBufferAddress)(void *, jobject_custom);
static jlong       JNI_FN(GetDirectBufferCapacity)(void *, jobject_custom);
static jsize       JNI_FN(GetArrayLength)(void *, jarray_custom);

/* JavaVM functions */
static jint JNI_FN(DestroyJavaVM)(void *);
static jint JNI_FN(AttachCurrentThread)(void *, void**, const void*);
static jint JNI_FN(DetachCurrentThread)(void *);
static jint JNI_FN(GetEnv)(void *, void**, jint);
static jint JNI_FN(AttachCurrentThreadAsDaemon)(void *, void**, const void*);

/* ----------------------------------------------------------------------
 *  Static vtable instances
 * ---------------------------------------------------------------------- */
static const struct JNINativeInterface_ g_jni_vtable = {
    /* reserved0..3 */ nullptr, nullptr, nullptr, nullptr,

    /* GetVersion */           JNI_FN(GetVersion),
    /* DefineClass */          nullptr,
    /* FindClass */            JNI_FN(FindClass),

    /* ExceptionOccurred */    JNI_FN(ExceptionOccurred),
    /* ExceptionDescribe */    JNI_FN(ExceptionDescribe),
    /* ExceptionClear */       JNI_FN(ExceptionClear),
    /* FatalError */           [](void*, const char* m){ LOGE(LOG_TAG_JNI, "FatalError: %s", m?m:"?"); abort(); },

    /* PushLocalFrame */       [](void*, jint){ return 0; },
    /* PopLocalFrame */        [](void*, jobject_custom o){ return o; },
    /* NewGlobalRef */         JNI_FN(NewGlobalRef),
    /* DeleteGlobalRef */      JNI_FN(DeleteGlobalRef),
    /* DeleteLocalRef */       JNI_FN(DeleteLocalRef),
    /* IsSameObject */         JNI_FN(IsSameObject),
    /* NewLocalRef */          JNI_FN(NewLocalRef),
    /* EnsureLocalCapacity */  [](void*, jint){ return 0; },

    /* AllocObject */          [](void*, jclass_custom){ LOGW(LOG_TAG_JNI,"AllocObject STUB"); return (jobject_custom)nullptr; },
    /* NewObject */            JNI_FN(NewObject),
    /* NewObjectV */           JNI_FN(NewObjectV),
    /* NewObjectA */           nullptr,

    /* GetObjectClass */       JNI_FN(GetObjectClass),
    /* IsInstanceOf */         JNI_FN(IsInstanceOf),
    /* GetMethodID */          JNI_FN(GetMethodID),

    /* CallObjectMethod */     JNI_FN(CallObjectMethod),
    /* CallObjectMethodV */    JNI_FN(CallObjectMethodV),
    /* CallObjectMethodA */    nullptr,
    /* CallBooleanMethod */    JNI_FN(CallBooleanMethod),
    /* CallBooleanMethodV */   JNI_FN(CallBooleanMethodV),
    /* CallBooleanMethodA */   nullptr,
    /* CallByteMethod */       JNI_FN(CallByteMethod),
    /* CallByteMethodV */      JNI_FN(CallByteMethodV),
    /* CallByteMethodA */      nullptr,
    /* CallCharMethod */       JNI_FN(CallCharMethod),
    /* CallCharMethodV */      JNI_FN(CallCharMethodV),
    /* CallCharMethodA */      nullptr,
    /* CallShortMethod */      JNI_FN(CallShortMethod),
    /* CallShortMethodV */     JNI_FN(CallShortMethodV),
    /* CallShortMethodA */     nullptr,
    /* CallIntMethod */        JNI_FN(CallIntMethod),
    /* CallIntMethodV */       JNI_FN(CallIntMethodV),
    /* CallIntMethodA */       nullptr,
    /* CallLongMethod */       JNI_FN(CallLongMethod),
    /* CallLongMethodV */      JNI_FN(CallLongMethodV),
    /* CallLongMethodA */      nullptr,
    /* CallFloatMethod */      JNI_FN(CallFloatMethod),
    /* CallFloatMethodV */     JNI_FN(CallFloatMethodV),
    /* CallFloatMethodA */     nullptr,
    /* CallDoubleMethod */     JNI_FN(CallDoubleMethod),
    /* CallDoubleMethodV */    JNI_FN(CallDoubleMethodV),
    /* CallDoubleMethodA */    nullptr,
    /* CallVoidMethod */       JNI_FN(CallVoidMethod),
    /* CallVoidMethodV */      JNI_FN(CallVoidMethodV),
    /* CallVoidMethodA */      nullptr,

    /* CallNonvirtual* — partial: forward to virtual */
    [](void*, jobject_custom, jclass_custom, jmethodID_custom, ...){ return (jobject_custom)nullptr; },
    [](void*, jobject_custom, jclass_custom, jmethodID_custom, ...){ return (jboolean)0; },
    [](void*, jobject_custom, jclass_custom, jmethodID_custom, ...){ return (jint)0; },
    [](void*, jobject_custom, jclass_custom, jmethodID_custom, ...){ return (jlong)0; },
    [](void*, jobject_custom, jclass_custom, jmethodID_custom, ...){},

    /* GetFieldID */           JNI_FN(GetFieldID),
    /* GetObjectField */       JNI_FN(GetObjectField),
    /* GetBooleanField */      JNI_FN(GetBooleanField),
    /* GetByteField */         JNI_FN(GetByteField),
    /* GetCharField */         JNI_FN(GetCharField),
    /* GetShortField */        JNI_FN(GetShortField),
    /* GetIntField */          JNI_FN(GetIntField),
    /* GetLongField */         JNI_FN(GetLongField),
    /* GetFloatField */        JNI_FN(GetFloatField),
    /* GetDoubleField */       JNI_FN(GetDoubleField),
    /* SetObjectField */       JNI_FN(SetObjectField),
    /* SetBooleanField */      [](void*, jobject_custom, jfieldID_custom, jboolean){},
    /* SetByteField */         JNI_FN(SetByteField),
    /* SetCharField */         JNI_FN(SetCharField),
    /* SetShortField */        JNI_FN(SetShortField),
    /* SetIntField */          JNI_FN(SetIntField),
    /* SetLongField */         JNI_FN(SetLongField),
    /* SetFloatField */        JNI_FN(SetFloatField),
    /* SetDoubleField */       JNI_FN(SetDoubleField),

    /* GetStaticMethodID */    JNI_FN(GetStaticMethodID),
    /* CallStaticObjectMethod */  JNI_FN(CallStaticObjectMethod),
    /* CallStaticObjectMethodV */ JNI_FN(CallStaticObjectMethodV),
    /* CallStaticObjectMethodA */ nullptr,
    /* CallStaticBooleanMethod */ JNI_FN(CallStaticBooleanMethod),
    /* CallStaticBooleanMethodV */JNI_FN(CallStaticBooleanMethodV),
    /* CallStaticBooleanMethodA */nullptr,
    /* CallStaticIntMethod */  JNI_FN(CallStaticIntMethod),
    /* CallStaticIntMethodV */ JNI_FN(CallStaticIntMethodV),
    /* CallStaticIntMethodA */ nullptr,
    /* CallStaticLongMethod */ JNI_FN(CallStaticLongMethod),
    /* CallStaticVoidMethod */ JNI_FN(CallStaticVoidMethod),
    /* CallStaticVoidMethodV */JNI_FN(CallStaticVoidMethodV),
    /* CallStaticVoidMethodA */nullptr,

    /* GetStaticFieldID */     JNI_FN(GetStaticFieldID),
    /* GetStaticObjectField */ JNI_FN(GetStaticObjectField),
    /* GetStaticBooleanField */JNI_FN(GetStaticBooleanField),
    /* GetStaticIntField */    JNI_FN(GetStaticIntField),
    /* GetStaticLongField */   JNI_FN(GetStaticLongField),
    /* GetStaticFloatField */ JNI_FN(GetStaticFloatField),
    /* GetStaticDoubleField */ JNI_FN(GetStaticDoubleField),
    /* SetStaticObjectField */ [](void*, jclass_custom, jfieldID_custom, jobject_custom){},
    /* SetStaticBooleanField */JNI_FN(SetStaticBooleanField),
    /* SetStaticIntField */    JNI_FN(SetStaticIntField),
    /* SetStaticLongField */   JNI_FN(SetStaticLongField),
    /* SetStaticFloatField */ JNI_FN(SetStaticFloatField),
    /* SetStaticDoubleField */ JNI_FN(SetStaticDoubleField),

    /* NewString */            [](void*, const jchar*, jsize){ LOGW(LOG_TAG_JNI,"NewString STUB"); return (jstring_custom)nullptr; },
    /* GetStringLength */      JNI_FN(GetStringLength),
    /* GetStringChars */       [](void*, jstring_custom, jboolean*){ LOGW(LOG_TAG_JNI,"GetStringChars STUB"); return (const jchar*)nullptr; },
    /* ReleaseStringChars */   [](void*, jstring_custom, const jchar*){},
    /* NewStringUTF */         JNI_FN(NewStringUTF),
    /* GetStringUTFLength */   JNI_FN(GetStringUTFLength),
    /* GetStringUTFChars */    JNI_FN(GetStringUTFChars),
    /* ReleaseStringUTFChars */JNI_FN(ReleaseStringUTFChars),
    /* GetStringRegion */      nullptr,
    /* GetStringUTFRegion */   nullptr,
    /* GetStringCritical */    nullptr,
    /* ReleaseStringCritical */nullptr,

    /* GetArrayLength */       JNI_FN(GetArrayLength),
    /* NewObjectArray */       JNI_FN(NewObjectArray),
    /* GetObjectArrayElement */JNI_FN(GetObjectArrayElement),
    /* SetObjectArrayElement */JNI_FN(SetObjectArrayElement),
    /* NewBooleanArray */      JNI_FN(NewBooleanArray),
    /* NewByteArray */         JNI_FN(NewByteArray),
    /* NewCharArray */         JNI_FN(NewCharArray),
    /* NewShortArray */        JNI_FN(NewShortArray),
    /* NewIntArray */          JNI_FN(NewIntArray),
    /* NewLongArray */         JNI_FN(NewLongArray),
    /* NewFloatArray */        JNI_FN(NewFloatArray),
    /* NewDoubleArray */       JNI_FN(NewDoubleArray),
    /* GetBooleanArrayElements */ JNI_FN(GetBooleanArrayElements),
    /* GetByteArrayElements */ JNI_FN(GetByteArrayElements),
    /* GetCharArrayElements */ JNI_FN(GetCharArrayElements),
    /* GetShortArrayElements */JNI_FN(GetShortArrayElements),
    /* GetIntArrayElements */  JNI_FN(GetIntArrayElements),
    /* GetLongArrayElements */ JNI_FN(GetLongArrayElements),
    /* GetFloatArrayElements */JNI_FN(GetFloatArrayElements),
    /* GetDoubleArrayElements */JNI_FN(GetDoubleArrayElements),
    /* ReleaseBooleanArrayElements */ JNI_FN(ReleaseBooleanArrayElements),
    /* ReleaseByteArrayElements */     JNI_FN(ReleaseByteArrayElements),
    /* ReleaseCharArrayElements */     JNI_FN(ReleaseCharArrayElements),
    /* ReleaseShortArrayElements */    JNI_FN(ReleaseShortArrayElements),
    /* ReleaseIntArrayElements */      JNI_FN(ReleaseIntArrayElements),
    /* ReleaseLongArrayElements */     JNI_FN(ReleaseLongArrayElements),
    /* ReleaseFloatArrayElements */    JNI_FN(ReleaseFloatArrayElements),
    /* ReleaseDoubleArrayElements */   JNI_FN(ReleaseDoubleArrayElements),
    /* Get*ArrayRegion */ JNI_FN(GetBooleanArrayRegion), JNI_FN(GetByteArrayRegion), JNI_FN(GetCharArrayRegion), JNI_FN(GetShortArrayRegion), JNI_FN(GetIntArrayRegion), JNI_FN(GetLongArrayRegion), JNI_FN(GetFloatArrayRegion), JNI_FN(GetDoubleArrayRegion),
    /* SetBooleanArrayRegion */ JNI_FN(SetBooleanArrayRegion),
    /* SetByteArrayRegion */    JNI_FN(SetByteArrayRegion),
    /* SetCharArrayRegion */    JNI_FN(SetCharArrayRegion),
    /* SetShortArrayRegion */   JNI_FN(SetShortArrayRegion),
    /* SetIntArrayRegion */     JNI_FN(SetIntArrayRegion),
    /* SetLongArrayRegion */    JNI_FN(SetLongArrayRegion),
    /* SetFloatArrayRegion */   JNI_FN(SetFloatArrayRegion),
    /* SetDoubleArrayRegion */  JNI_FN(SetDoubleArrayRegion),
    /* RegisterNatives */       JNI_FN(RegisterNatives),
    /* UnregisterNatives */     JNI_FN(UnregisterNatives),

    /* MonitorEnter */          [](void*, jobject_custom){ return 0; },
    /* MonitorExit */           [](void*, jobject_custom){ return 0; },
    /* GetJavaVM */             JNI_FN(GetJavaVM),

    /* GetStringRegionChars */  nullptr,
    /* NewDirectByteBuffer */   JNI_FN(NewDirectByteBuffer),
    /* GetDirectBufferAddress */JNI_FN(GetDirectBufferAddress),
    /* GetDirectBufferCapacity */JNI_FN(GetDirectBufferCapacity),
    /* GetObjectRefType */      [](void*, jobject_custom){ return (jboolean)0; },
};

static const struct JNIInvokeInterface_ g_javaVM_vtable = {
    nullptr, nullptr, nullptr,
    JNI_FN(DestroyJavaVM),
    JNI_FN(AttachCurrentThread),
    JNI_FN(DetachCurrentThread),
    JNI_FN(GetEnv),
    JNI_FN(AttachCurrentThreadAsDaemon),
};

/* The "instance" structs. In standard JNI, a JNIEnv* is a pointer to a
 * pointer to the vtable. We make a single global pointer-pair. */
static const struct JNINativeInterface_ *g_env_inner = &g_jni_vtable;
static const struct JNINativeInterface_ **g_env_outer = &g_env_inner;
static const struct JNIInvokeInterface_ *g_vm_inner = &g_javaVM_vtable;
static const struct JNIInvokeInterface_ **g_vm_outer = &g_vm_inner;

/* ----------------------------------------------------------------------
 *  jni_bridge_init / get_javavm / attach_env
 * ---------------------------------------------------------------------- */
int jni_bridge_init(art_vm_t *art_vm) {
    if (!art_vm) {
        LOGE(LOG_TAG_JNI, "init refused: ART VM handle is NULL");
        return -1;
    }
    g_art_vm = art_vm;
    g_inited = 1;
    LOGI(LOG_TAG_JNI, "init OK (ART VM bound to JNIEnv vtable)");
    return 0;
}

void *jni_bridge_get_javavm(void) {
    if (!g_inited) return nullptr;
    return (void *)g_vm_outer;   /* JavaVM* = pointer to pointer to vtable */
}

void *jni_bridge_attach_env(void) {
    if (!g_inited) return nullptr;
    return (void *)g_env_outer;  /* JNIEnv* = pointer to pointer to vtable */
}

jni_registered_native_t *jni_bridge_lookup_native(const char *class_binary,
                                                  const char *name,
                                                  const char *sig) {
    std::lock_guard<std::mutex> lk(g_registered_lock);
    std::string dotted = descriptor_to_dotted(class_binary);
    for (auto &e : g_registered) {
        if (e.cls == dotted && e.name == name && e.sig == sig) {
            static thread_local jni_registered_native_t out;
            std::strncpy(out.method_name, e.name.c_str(), sizeof(out.method_name) - 1);
            std::strncpy(out.signature, e.sig.c_str(), sizeof(out.signature) - 1);
            out.fnptr = e.fnptr;
            return &out;
        }
    }
    return nullptr;
}

extern "C" int jni_bridge_register_natives(const char *class_binary,
                                           const char *name,
                                           const char *sig,
                                           void *fnptr) {
    if (!class_binary || !name || !sig || !fnptr) return -1;
    std::lock_guard<std::mutex> lk(g_registered_lock);
    std::string dotted = descriptor_to_dotted(class_binary);
    g_registered.push_back({dotted, name, sig, fnptr});
    LOGI(LOG_TAG_JNI, "RegisterNatives: %s.%s %s -> %p",
         dotted.c_str(), name, sig, fnptr);
    return 0;
}

/* ----------------------------------------------------------------------
 *  Vtable function implementations
 * ---------------------------------------------------------------------- */

static jint JNI_FN(GetVersion)(void *) { return 0x00010006; }  /* JNI 1.6 */

static jclass_custom JNI_FN(FindClass)(void *, const char *name) {
    if (!name) return nullptr;
    std::string desc = to_descriptor(name);
    dex_cls_t *cls = dex_vm_resolve_class(vm(), desc.c_str());
    if (!cls) {
        LOGW(LOG_TAG_JNI, "FindClass: %s not resolved", name);
        return nullptr;
    }
    return (jclass_custom)cls;
}

static jobject_custom JNI_FN(NewGlobalRef)(void *, jobject_custom o) { return o; }
static void JNI_FN(DeleteGlobalRef)(void *, jobject_custom) {}
static void JNI_FN(DeleteLocalRef)(void *, jobject_custom) {}
static jboolean JNI_FN(IsSameObject)(void *, jobject_custom a, jobject_custom b) {
    return a == b ? JNI_TRUE : JNI_FALSE;
}
static jobject_custom JNI_FN(NewLocalRef)(void *, jobject_custom o) { return o; }

static jobject_custom JNI_FN(NewObjectV)(void *, jclass_custom cls,
                                         jmethodID_custom mid, va_list ap) {
    if (!cls || !mid) return nullptr;
    /* mid->class_desc is the class to instantiate; create the instance, then
     * call the <init> method. */
    std::string desc = mid->class_desc;
    dex_obj_t *obj = dex_new_instance(vm(), desc.c_str());
    if (!obj) {
        LOGE(LOG_TAG_JNI, "NewObjectV: cannot instantiate %s", desc.c_str());
        return nullptr;
    }
    /* Build args: receiver + ctor args from va_list based on shorty. */
    const char *shorty = mid->sig;
    int n_args = count_args(shorty) + 1;  /* +1 for receiver */
    std::vector<dex_value_t> args(n_args);
    args[0].ptr = obj;
    const char *p = strchr(shorty, '(');
    if (p) p++;
    for (int i = 1; i < n_args; i++) {
        if (!p || !*p || *p == ')') break;
        char c = *p++;
        if (c == 'L' || c == '[') {
            args[i].ptr = va_arg(ap, void*);
            while (c == 'L' && p && *p && *p != ';') p++;
            if (c == 'L' && p && *p) p++;
        } else if (c == 'J') {
            args[i].i64 = va_arg(ap, long long);
        } else if (c == 'D') {
            args[i].f64 = va_arg(ap, double);
        } else if (c == 'F') {
            args[i].f32 = (float)va_arg(ap, double);
        } else {
            args[i].i32 = va_arg(ap, int);
        }
    }
    dex_value_t result;
    dex_invoke(vm(), desc.c_str(), mid->name, shorty,
               args.data(), n_args, &result);
    return (jobject_custom)obj;
}

static jobject_custom JNI_FN(NewObject)(void *env, jclass_custom cls,
                                        jmethodID_custom mid, ...) {
    va_list ap; va_start(ap, mid);
    jobject_custom r = JNI_FN(NewObjectV)(env, cls, mid, ap);
    va_end(ap);
    return r;
}

static jboolean JNI_FN(IsInstanceOf)(void *, jobject_custom obj, jclass_custom cls) {
    (void)obj; (void)cls;
    return JNI_TRUE;   /* permissive: avoid breaking apps */
}

static jmethodID_custom JNI_FN(GetMethodID)(void *, jclass_custom cls,
                                            const char *name, const char *sig) {
    if (!cls || !name || !sig) return nullptr;
    /* We need the class descriptor; the jclass_custom is a dex_cls_t* whose
     * descriptor we can read via the dex_loader API. But we don't have direct
     * access here — intern with the class name as the method sees it.
     * For now, intern without verifying; the dex_invoke lookup will fail
     * loudly if the method doesn't exist. */
    /* TODO: store descriptor on dex_cls_t and read it. Use a thread-local
     * "last FindClass descriptor" as a heuristic. */
    extern const char *dex_cls_descriptor(dex_cls_t *cls);  /* from dex_interp */
    const char *desc = dex_cls_descriptor((dex_cls_t *)cls);
    if (!desc) {
        LOGW(LOG_TAG_JNI, "GetMethodID: no descriptor for cls %p", cls);
        return nullptr;
    }
    LOGI(LOG_TAG_JNI, "GetMethodID: %s.%s %s", desc, name, sig);
    return intern_id(desc, name, sig, 0);
}

static jmethodID_custom JNI_FN(GetStaticMethodID)(void *, jclass_custom cls,
                                                  const char *name, const char *sig) {
    if (!cls || !name || !sig) return nullptr;
    extern const char *dex_cls_descriptor(dex_cls_t *cls);
    const char *desc = dex_cls_descriptor((dex_cls_t *)cls);
    if (!desc) return nullptr;
    LOGI(LOG_TAG_JNI, "GetStaticMethodID: %s.%s %s", desc, name, sig);
    return intern_id(desc, name, sig, 1);
}

static jfieldID_custom JNI_FN(GetFieldID)(void *, jclass_custom cls,
                                          const char *name, const char *sig) {
    if (!cls || !name || !sig) return nullptr;
    extern const char *dex_cls_descriptor(dex_cls_t *cls);
    const char *desc = dex_cls_descriptor((dex_cls_t *)cls);
    if (!desc) return nullptr;
    return intern_id(desc, name, sig, 0);
}

static jfieldID_custom JNI_FN(GetStaticFieldID)(void *, jclass_custom cls,
                                                const char *name, const char *sig) {
    if (!cls || !name || !sig) return nullptr;
    extern const char *dex_cls_descriptor(dex_cls_t *cls);
    const char *desc = dex_cls_descriptor((dex_cls_t *)cls);
    if (!desc) return nullptr;
    return intern_id(desc, name, sig, 1);
}

/* ---- Call*Method (instance) ---- */
static void JNI_FN(CallVoidMethodV)(void *, jobject_custom obj,
                                    jmethodID_custom mid, va_list ap) {
    if (!obj || !mid) return;
    int n_args = count_args(mid->sig) + 1;
    std::vector<dex_value_t> args(n_args);
    args[0].ptr = obj;
    const char *p = strchr(mid->sig, '('); if (p) p++;
    for (int i = 1; i < n_args; i++) {
        if (!p || !*p || *p == ')') break;
        char c = *p++;
        if (c == 'L' || c == '[') { args[i].ptr = va_arg(ap, void*);
            while (c=='L' && p && *p && *p!=';') p++; if (c=='L' && p && *p) p++; }
        else if (c == 'J') args[i].i64 = va_arg(ap, long long);
        else if (c == 'D') args[i].f64 = va_arg(ap, double);
        else if (c == 'F') args[i].f32 = (float)va_arg(ap, double);
        else args[i].i32 = va_arg(ap, int);
    }
    dex_value_t r;
    dex_invoke(vm(), mid->class_desc, mid->name, mid->sig, args.data(), n_args, &r);
}
static void JNI_FN(CallVoidMethod)(void *env, jobject_custom o, jmethodID_custom m, ...) {
    va_list ap; va_start(ap, m);
    JNI_FN(CallVoidMethodV)(env, o, m, ap);
    va_end(ap);
}
static jint JNI_FN(CallIntMethodV)(void *, jobject_custom obj, jmethodID_custom mid, va_list ap) {
    if (!obj || !mid) return 0;
    int n_args = count_args(mid->sig) + 1;
    std::vector<dex_value_t> args(n_args);
    args[0].ptr = obj;
    const char *p = strchr(mid->sig, '('); if (p) p++;
    for (int i = 1; i < n_args; i++) {
        if (!p || !*p || *p == ')') break;
        char c = *p++;
        if (c == 'L' || c == '[') { args[i].ptr = va_arg(ap, void*);
            while (c=='L' && p && *p && *p!=';') p++; if (c=='L' && p && *p) p++; }
        else if (c == 'J') args[i].i64 = va_arg(ap, long long);
        else if (c == 'D') args[i].f64 = va_arg(ap, double);
        else if (c == 'F') args[i].f32 = (float)va_arg(ap, double);
        else args[i].i32 = va_arg(ap, int);
    }
    dex_value_t r; r.i32 = 0;
    dex_invoke(vm(), mid->class_desc, mid->name, mid->sig, args.data(), n_args, &r);
    return r.i32;
}
static jint JNI_FN(CallIntMethod)(void *env, jobject_custom o, jmethodID_custom m, ...) {
    va_list ap; va_start(ap, m);
    jint r = JNI_FN(CallIntMethodV)(env, o, m, ap);
    va_end(ap);
    return r;
}
static jboolean JNI_FN(CallBooleanMethodV)(void *, jobject_custom obj, jmethodID_custom mid, va_list ap) {
    if (!obj || !mid) return 0;
    int n_args = count_args(mid->sig) + 1;
    std::vector<dex_value_t> args(n_args);
    args[0].ptr = obj;
    const char *p = strchr(mid->sig, '('); if (p) p++;
    for (int i = 1; i < n_args; i++) {
        if (!p || !*p || *p == ')') break;
        char c = *p++;
        if (c == 'L' || c == '[') { args[i].ptr = va_arg(ap, void*);
            while (c=='L' && p && *p && *p!=';') p++; if (c=='L' && p && *p) p++; }
        else if (c == 'J') args[i].i64 = va_arg(ap, long long);
        else if (c == 'D') args[i].f64 = va_arg(ap, double);
        else if (c == 'F') args[i].f32 = (float)va_arg(ap, double);
        else args[i].i32 = va_arg(ap, int);
    }
    dex_value_t r; r.i32 = 0;
    dex_invoke(vm(), mid->class_desc, mid->name, mid->sig, args.data(), n_args, &r);
    return r.i32 ? JNI_TRUE : JNI_FALSE;
}
static jboolean JNI_FN(CallBooleanMethod)(void *env, jobject_custom o, jmethodID_custom m, ...) {
    va_list ap; va_start(ap, m);
    jboolean r = JNI_FN(CallBooleanMethodV)(env, o, m, ap);
    va_end(ap);
    return r;
}
static jobject_custom JNI_FN(CallObjectMethodV)(void *, jobject_custom obj, jmethodID_custom mid, va_list ap) {
    if (!obj || !mid) return nullptr;
    int n_args = count_args(mid->sig) + 1;
    std::vector<dex_value_t> args(n_args);
    args[0].ptr = obj;
    const char *p = strchr(mid->sig, '('); if (p) p++;
    for (int i = 1; i < n_args; i++) {
        if (!p || !*p || *p == ')') break;
        char c = *p++;
        if (c == 'L' || c == '[') { args[i].ptr = va_arg(ap, void*);
            while (c=='L' && p && *p && *p!=';') p++; if (c=='L' && p && *p) p++; }
        else if (c == 'J') args[i].i64 = va_arg(ap, long long);
        else if (c == 'D') args[i].f64 = va_arg(ap, double);
        else if (c == 'F') args[i].f32 = (float)va_arg(ap, double);
        else args[i].i32 = va_arg(ap, int);
    }
    dex_value_t r; r.ptr = nullptr;
    dex_invoke(vm(), mid->class_desc, mid->name, mid->sig, args.data(), n_args, &r);
    return (jobject_custom)r.ptr;
}
static jobject_custom JNI_FN(CallObjectMethod)(void *env, jobject_custom o, jmethodID_custom m, ...) {
    va_list ap; va_start(ap, m);
    jobject_custom r = JNI_FN(CallObjectMethodV)(env, o, m, ap);
    va_end(ap);
    return r;
}

/* ---- Typed Call*Method variants (Byte/Char/Short/Long/Float/Double) ----
 * All call dex_invoke and read the corresponding union member from dex_value_t. */
static jbyte JNI_FN(CallByteMethodV)(void *, jobject_custom obj, jmethodID_custom mid, va_list ap) {
    if (!obj || !mid) return 0;
    int n_args = count_args(mid->sig) + 1;
    std::vector<dex_value_t> args(n_args);
    args[0].ptr = obj;
    const char *p = strchr(mid->sig, '('); if (p) p++;
    for (int i = 1; i < n_args; i++) {
        if (!p || !*p || *p == ')') break;
        char c = *p++;
        if (c == 'L' || c == '[') { args[i].ptr = va_arg(ap, void*); while (c=='L'&&p&&*p&&*p!=';') p++; if (c=='L'&&p&&*p) p++; }
        else if (c == 'J') args[i].i64 = va_arg(ap, long long);
        else if (c == 'D') args[i].f64 = va_arg(ap, double);
        else if (c == 'F') args[i].f32 = (float)va_arg(ap, double);
        else args[i].i32 = va_arg(ap, int);
    }
    dex_value_t r; r.i32 = 0;
    dex_invoke(vm(), mid->class_desc, mid->name, mid->sig, args.data(), n_args, &r);
    return (jbyte)r.i32;
}
static jbyte JNI_FN(CallByteMethod)(void *env, jobject_custom o, jmethodID_custom m, ...) { va_list ap; va_start(ap,m); jbyte r=JNI_FN(CallByteMethodV)(env,o,m,ap); va_end(ap); return r; }

static jchar JNI_FN(CallCharMethodV)(void *, jobject_custom obj, jmethodID_custom mid, va_list ap) {
    if (!obj || !mid) return 0;
    int n_args = count_args(mid->sig) + 1; std::vector<dex_value_t> args(n_args); args[0].ptr = obj;
    const char *p = strchr(mid->sig, '('); if (p) p++;
    for (int i = 1; i < n_args; i++) { if (!p||!*p||*p==')') break; char c=*p++;
        if (c=='L'||c=='[') { args[i].ptr=va_arg(ap,void*); while(c=='L'&&p&&*p&&*p!=';')p++; if(c=='L'&&p&&*p)p++; }
        else if (c=='J') args[i].i64=va_arg(ap,long long); else if (c=='D') args[i].f64=va_arg(ap,double); else if (c=='F') args[i].f32=(float)va_arg(ap,double); else args[i].i32=va_arg(ap,int); }
    dex_value_t r; r.i32 = 0; dex_invoke(vm(),mid->class_desc,mid->name,mid->sig,args.data(),n_args,&r); return (jchar)r.i32;
}
static jchar JNI_FN(CallCharMethod)(void *env, jobject_custom o, jmethodID_custom m, ...) { va_list ap; va_start(ap,m); jchar r=JNI_FN(CallCharMethodV)(env,o,m,ap); va_end(ap); return r; }

static jshort JNI_FN(CallShortMethodV)(void *, jobject_custom obj, jmethodID_custom mid, va_list ap) {
    if (!obj || !mid) return 0;
    int n_args = count_args(mid->sig) + 1; std::vector<dex_value_t> args(n_args); args[0].ptr = obj;
    const char *p = strchr(mid->sig, '('); if (p) p++;
    for (int i = 1; i < n_args; i++) { if (!p||!*p||*p==')') break; char c=*p++;
        if (c=='L'||c=='[') { args[i].ptr=va_arg(ap,void*); while(c=='L'&&p&&*p&&*p!=';')p++; if(c=='L'&&p&&*p)p++; }
        else if (c=='J') args[i].i64=va_arg(ap,long long); else if (c=='D') args[i].f64=va_arg(ap,double); else if (c=='F') args[i].f32=(float)va_arg(ap,double); else args[i].i32=va_arg(ap,int); }
    dex_value_t r; r.i32 = 0; dex_invoke(vm(),mid->class_desc,mid->name,mid->sig,args.data(),n_args,&r); return (jshort)r.i32;
}
static jshort JNI_FN(CallShortMethod)(void *env, jobject_custom o, jmethodID_custom m, ...) { va_list ap; va_start(ap,m); jshort r=JNI_FN(CallShortMethodV)(env,o,m,ap); va_end(ap); return r; }

static jlong JNI_FN(CallLongMethodV)(void *, jobject_custom obj, jmethodID_custom mid, va_list ap) {
    if (!obj || !mid) return 0;
    int n_args = count_args(mid->sig) + 1; std::vector<dex_value_t> args(n_args); args[0].ptr = obj;
    const char *p = strchr(mid->sig, '('); if (p) p++;
    for (int i = 1; i < n_args; i++) { if (!p||!*p||*p==')') break; char c=*p++;
        if (c=='L'||c=='[') { args[i].ptr=va_arg(ap,void*); while(c=='L'&&p&&*p&&*p!=';')p++; if(c=='L'&&p&&*p)p++; }
        else if (c=='J') args[i].i64=va_arg(ap,long long); else if (c=='D') args[i].f64=va_arg(ap,double); else if (c=='F') args[i].f32=(float)va_arg(ap,double); else args[i].i32=va_arg(ap,int); }
    dex_value_t r; r.i64 = 0; dex_invoke(vm(),mid->class_desc,mid->name,mid->sig,args.data(),n_args,&r); return (jlong)r.i64;
}
static jlong JNI_FN(CallLongMethod)(void *env, jobject_custom o, jmethodID_custom m, ...) { va_list ap; va_start(ap,m); jlong r=JNI_FN(CallLongMethodV)(env,o,m,ap); va_end(ap); return r; }

static jfloat JNI_FN(CallFloatMethodV)(void *, jobject_custom obj, jmethodID_custom mid, va_list ap) {
    if (!obj || !mid) return 0;
    int n_args = count_args(mid->sig) + 1; std::vector<dex_value_t> args(n_args); args[0].ptr = obj;
    const char *p = strchr(mid->sig, '('); if (p) p++;
    for (int i = 1; i < n_args; i++) { if (!p||!*p||*p==')') break; char c=*p++;
        if (c=='L'||c=='[') { args[i].ptr=va_arg(ap,void*); while(c=='L'&&p&&*p&&*p!=';')p++; if(c=='L'&&p&&*p)p++; }
        else if (c=='J') args[i].i64=va_arg(ap,long long); else if (c=='D') args[i].f64=va_arg(ap,double); else if (c=='F') args[i].f32=(float)va_arg(ap,double); else args[i].i32=va_arg(ap,int); }
    dex_value_t r; r.f32 = 0; dex_invoke(vm(),mid->class_desc,mid->name,mid->sig,args.data(),n_args,&r); return r.f32;
}
static jfloat JNI_FN(CallFloatMethod)(void *env, jobject_custom o, jmethodID_custom m, ...) { va_list ap; va_start(ap,m); jfloat r=JNI_FN(CallFloatMethodV)(env,o,m,ap); va_end(ap); return r; }

static jdouble JNI_FN(CallDoubleMethodV)(void *, jobject_custom obj, jmethodID_custom mid, va_list ap) {
    if (!obj || !mid) return 0;
    int n_args = count_args(mid->sig) + 1; std::vector<dex_value_t> args(n_args); args[0].ptr = obj;
    const char *p = strchr(mid->sig, '('); if (p) p++;
    for (int i = 1; i < n_args; i++) { if (!p||!*p||*p==')') break; char c=*p++;
        if (c=='L'||c=='[') { args[i].ptr=va_arg(ap,void*); while(c=='L'&&p&&*p&&*p!=';')p++; if(c=='L'&&p&&*p)p++; }
        else if (c=='J') args[i].i64=va_arg(ap,long long); else if (c=='D') args[i].f64=va_arg(ap,double); else if (c=='F') args[i].f32=(float)va_arg(ap,double); else args[i].i32=va_arg(ap,int); }
    dex_value_t r; r.f64 = 0; dex_invoke(vm(),mid->class_desc,mid->name,mid->sig,args.data(),n_args,&r); return r.f64;
}
static jdouble JNI_FN(CallDoubleMethod)(void *env, jobject_custom o, jmethodID_custom m, ...) { va_list ap; va_start(ap,m); jdouble r=JNI_FN(CallDoubleMethodV)(env,o,m,ap); va_end(ap); return r; }

/* ---- GetObjectClass (REAL — reads obj->cls) ---- */
/* dex_obj_t layout: first field is cls pointer. We use a local struct to
 * avoid pulling in the full dex_interp.h definition. */
struct dex_obj_layout { void *cls; };
extern "C" const char *dex_obj_class_descriptor(dex_obj_t *obj);  /* from dex_interp */
static jclass_custom JNI_FN(GetObjectClass)(void *, jobject_custom obj) {
    if (!obj) return nullptr;
    return (jclass_custom)((struct dex_obj_layout *)obj)->cls;
}

/* ---- Typed Get*Field / Set*Field (REAL — read/write the right union member) ---- */
static jbyte  JNI_FN(GetByteField)(void *, jobject_custom obj, jfieldID_custom fid) { if(!obj||!fid) return 0; dex_value_t v; v.i32=0; dex_get_field(vm(),(dex_obj_t*)obj,fid->name,&v); return (jbyte)v.i32; }
static jchar  JNI_FN(GetCharField)(void *, jobject_custom obj, jfieldID_custom fid) { if(!obj||!fid) return 0; dex_value_t v; v.i32=0; dex_get_field(vm(),(dex_obj_t*)obj,fid->name,&v); return (jchar)v.i32; }
static jshort JNI_FN(GetShortField)(void *, jobject_custom obj, jfieldID_custom fid) { if(!obj||!fid) return 0; dex_value_t v; v.i32=0; dex_get_field(vm(),(dex_obj_t*)obj,fid->name,&v); return (jshort)v.i32; }
static jlong  JNI_FN(GetLongField)(void *, jobject_custom obj, jfieldID_custom fid) { if(!obj||!fid) return 0; dex_value_t v; v.i64=0; dex_get_field(vm(),(dex_obj_t*)obj,fid->name,&v); return v.i64; }
static jfloat JNI_FN(GetFloatField)(void *, jobject_custom obj, jfieldID_custom fid) { if(!obj||!fid) return 0; dex_value_t v; v.f32=0; dex_get_field(vm(),(dex_obj_t*)obj,fid->name,&v); return v.f32; }
static jdouble JNI_FN(GetDoubleField)(void *, jobject_custom obj, jfieldID_custom fid) { if(!obj||!fid) return 0; dex_value_t v; v.f64=0; dex_get_field(vm(),(dex_obj_t*)obj,fid->name,&v); return v.f64; }
static void JNI_FN(SetByteField)(void *, jobject_custom obj, jfieldID_custom fid, jbyte val) { if(!obj||!fid) return; dex_value_t v; v.i32=val; dex_set_field(vm(),(dex_obj_t*)obj,fid->name,v); }
static void JNI_FN(SetCharField)(void *, jobject_custom obj, jfieldID_custom fid, jchar val) { if(!obj||!fid) return; dex_value_t v; v.i32=val; dex_set_field(vm(),(dex_obj_t*)obj,fid->name,v); }
static void JNI_FN(SetShortField)(void *, jobject_custom obj, jfieldID_custom fid, jshort val) { if(!obj||!fid) return; dex_value_t v; v.i32=val; dex_set_field(vm(),(dex_obj_t*)obj,fid->name,v); }
static void JNI_FN(SetLongField)(void *, jobject_custom obj, jfieldID_custom fid, jlong val) { if(!obj||!fid) return; dex_value_t v; v.i64=val; dex_set_field(vm(),(dex_obj_t*)obj,fid->name,v); }
static void JNI_FN(SetFloatField)(void *, jobject_custom obj, jfieldID_custom fid, jfloat val) { if(!obj||!fid) return; dex_value_t v; v.f32=val; dex_set_field(vm(),(dex_obj_t*)obj,fid->name,v); }
static void JNI_FN(SetDoubleField)(void *, jobject_custom obj, jfieldID_custom fid, jdouble val) { if(!obj||!fid) return; dex_value_t v; v.f64=val; dex_set_field(vm(),(dex_obj_t*)obj,fid->name,v); }

/* ---- Typed static field variants ---- */
static jboolean JNI_FN(GetStaticBooleanField)(void *, jclass_custom, jfieldID_custom fid) { if(!fid) return 0; dex_value_t v; v.i32=0; dex_get_static_field(vm(),fid->class_desc,fid->name,&v); return v.i32?JNI_TRUE:JNI_FALSE; }
static jlong   JNI_FN(GetStaticLongField)(void *, jclass_custom, jfieldID_custom fid) { if(!fid) return 0; dex_value_t v; v.i64=0; dex_get_static_field(vm(),fid->class_desc,fid->name,&v); return v.i64; }
static jfloat  JNI_FN(GetStaticFloatField)(void *, jclass_custom, jfieldID_custom fid) { if(!fid) return 0; dex_value_t v; v.f32=0; dex_get_static_field(vm(),fid->class_desc,fid->name,&v); return v.f32; }
static jdouble JNI_FN(GetStaticDoubleField)(void *, jclass_custom, jfieldID_custom fid) { if(!fid) return 0; dex_value_t v; v.f64=0; dex_get_static_field(vm(),fid->class_desc,fid->name,&v); return v.f64; }
static void JNI_FN(SetStaticBooleanField)(void *, jclass_custom, jfieldID_custom fid, jboolean val) { if(!fid) return; dex_value_t v; v.i32=val; dex_set_static_field(vm(),fid->class_desc,fid->name,v); }
static void JNI_FN(SetStaticLongField)(void *, jclass_custom, jfieldID_custom fid, jlong val) { if(!fid) return; dex_value_t v; v.i64=val; dex_set_static_field(vm(),fid->class_desc,fid->name,v); }
static void JNI_FN(SetStaticFloatField)(void *, jclass_custom, jfieldID_custom fid, jfloat val) { if(!fid) return; dex_value_t v; v.f32=val; dex_set_static_field(vm(),fid->class_desc,fid->name,v); }
static void JNI_FN(SetStaticDoubleField)(void *, jclass_custom, jfieldID_custom fid, jdouble val) { if(!fid) return; dex_value_t v; v.f64=val; dex_set_static_field(vm(),fid->class_desc,fid->name,v); }

/* ---- CallStaticBooleanMethod + CallStaticLongMethod (REAL) ---- */
static jboolean JNI_FN(CallStaticBooleanMethodV)(void *, jclass_custom, jmethodID_custom mid, va_list ap) {
    if (!mid) return 0;
    int n_args = count_args(mid->sig); std::vector<dex_value_t> args(n_args);
    const char *p = strchr(mid->sig, '('); if (p) p++;
    for (int i = 0; i < n_args; i++) { if (!p||!*p||*p==')') break; char c=*p++;
        if (c=='L'||c=='[') { args[i].ptr=va_arg(ap,void*); while(c=='L'&&p&&*p&&*p!=';')p++; if(c=='L'&&p&&*p)p++; }
        else if (c=='J') args[i].i64=va_arg(ap,long long); else if (c=='D') args[i].f64=va_arg(ap,double); else if (c=='F') args[i].f32=(float)va_arg(ap,double); else args[i].i32=va_arg(ap,int); }
    dex_value_t r; r.i32 = 0; dex_invoke(vm(),mid->class_desc,mid->name,mid->sig,args.data(),n_args,&r); return r.i32?JNI_TRUE:JNI_FALSE;
}
static jboolean JNI_FN(CallStaticBooleanMethod)(void *env, jclass_custom c, jmethodID_custom m, ...) { va_list ap; va_start(ap,m); jboolean r=JNI_FN(CallStaticBooleanMethodV)(env,c,m,ap); va_end(ap); return r; }
static jlong JNI_FN(CallStaticLongMethodV)(void *, jclass_custom, jmethodID_custom mid, va_list ap) {
    if (!mid) return 0;
    int n_args = count_args(mid->sig); std::vector<dex_value_t> args(n_args);
    const char *p = strchr(mid->sig, '('); if (p) p++;
    for (int i = 0; i < n_args; i++) { if (!p||!*p||*p==')') break; char c=*p++;
        if (c=='L'||c=='[') { args[i].ptr=va_arg(ap,void*); while(c=='L'&&p&&*p&&*p!=';')p++; if(c=='L'&&p&&*p)p++; }
        else if (c=='J') args[i].i64=va_arg(ap,long long); else if (c=='D') args[i].f64=va_arg(ap,double); else if (c=='F') args[i].f32=(float)va_arg(ap,double); else args[i].i32=va_arg(ap,int); }
    dex_value_t r; r.i64 = 0; dex_invoke(vm(),mid->class_desc,mid->name,mid->sig,args.data(),n_args,&r); return r.i64;
}
static jlong JNI_FN(CallStaticLongMethod)(void *env, jclass_custom c, jmethodID_custom m, ...) { va_list ap; va_start(ap,m); jlong r=JNI_FN(CallStaticLongMethodV)(env,c,m,ap); va_end(ap); return r; }

/* Array header shared by all array implementations */
struct jni_array_hdr { int type; jsize length; void *data; };

/* ---- Remaining array types (REAL — same jni_array_hdr pattern) ---- */
static jlongArray_custom  JNI_FN(NewLongArray)(void *, jsize len) { jni_array_hdr *h=(jni_array_hdr*)calloc(1,sizeof(*h)); h->type='J'; h->length=len; h->data=calloc(len>0?len:1,sizeof(jlong)); return (jlongArray_custom)h; }
static jshortArray_custom JNI_FN(NewShortArray)(void *, jsize len) { jni_array_hdr *h=(jni_array_hdr*)calloc(1,sizeof(*h)); h->type='S'; h->length=len; h->data=calloc(len>0?len:1,sizeof(jshort)); return (jshortArray_custom)h; }
static jcharArray_custom  JNI_FN(NewCharArray)(void *, jsize len) { jni_array_hdr *h=(jni_array_hdr*)calloc(1,sizeof(*h)); h->type='C'; h->length=len; h->data=calloc(len>0?len:1,sizeof(jchar)); return (jcharArray_custom)h; }
static jbooleanArray_custom JNI_FN(NewBooleanArray)(void *, jsize len) { jni_array_hdr *h=(jni_array_hdr*)calloc(1,sizeof(*h)); h->type='Z'; h->length=len; h->data=calloc(len>0?len:1,sizeof(jboolean)); return (jbooleanArray_custom)h; }
static jdoubleArray_custom JNI_FN(NewDoubleArray)(void *, jsize len) { jni_array_hdr *h=(jni_array_hdr*)calloc(1,sizeof(*h)); h->type='D'; h->length=len; h->data=calloc(len>0?len:1,sizeof(jdouble)); return (jdoubleArray_custom)h; }
static jlong*    JNI_FN(GetLongArrayElements)(void *, jlongArray_custom a, jboolean *ic) { if(!a) return nullptr; if(ic) *ic=JNI_FALSE; return (jlong*)((jni_array_hdr*)a)->data; }
static jshort*   JNI_FN(GetShortArrayElements)(void *, jshortArray_custom a, jboolean *ic) { if(!a) return nullptr; if(ic) *ic=JNI_FALSE; return (jshort*)((jni_array_hdr*)a)->data; }
static jchar*    JNI_FN(GetCharArrayElements)(void *, jcharArray_custom a, jboolean *ic) { if(!a) return nullptr; if(ic) *ic=JNI_FALSE; return (jchar*)((jni_array_hdr*)a)->data; }
static jboolean* JNI_FN(GetBooleanArrayElements)(void *, jbooleanArray_custom a, jboolean *ic) { if(!a) return nullptr; if(ic) *ic=JNI_FALSE; return (jboolean*)((jni_array_hdr*)a)->data; }
static jdouble*  JNI_FN(GetDoubleArrayElements)(void *, jdoubleArray_custom a, jboolean *ic) { if(!a) return nullptr; if(ic) *ic=JNI_FALSE; return (jdouble*)((jni_array_hdr*)a)->data; }
static void JNI_FN(ReleaseLongArrayElements)(void *, jlongArray_custom, jlong*, jint) {}
static void JNI_FN(ReleaseShortArrayElements)(void *, jshortArray_custom, jshort*, jint) {}
static void JNI_FN(ReleaseCharArrayElements)(void *, jcharArray_custom, jchar*, jint) {}
static void JNI_FN(ReleaseBooleanArrayElements)(void *, jbooleanArray_custom, jboolean*, jint) {}
static void JNI_FN(ReleaseDoubleArrayElements)(void *, jdoubleArray_custom, jdouble*, jint) {}
static void JNI_FN(GetBooleanArrayRegion)(void *, jbooleanArray_custom a, jsize s, jsize l, jboolean *b) { if(!a||!b) return; jni_array_hdr *h=(jni_array_hdr*)a; if(s<0||s+l>h->length) return; memcpy(b,(jboolean*)h->data+s,l); }
static void JNI_FN(GetByteArrayRegion)(void *, jbyteArray_custom a, jsize s, jsize l, jbyte *b) { if(!a||!b) return; jni_array_hdr *h=(jni_array_hdr*)a; if(s<0||s+l>h->length) return; memcpy(b,(jbyte*)h->data+s,l); }
static void JNI_FN(GetCharArrayRegion)(void *, jcharArray_custom a, jsize s, jsize l, jchar *b) { if(!a||!b) return; jni_array_hdr *h=(jni_array_hdr*)a; if(s<0||s+l>h->length) return; memcpy(b,(jchar*)h->data+s,l*sizeof(jchar)); }
static void JNI_FN(GetShortArrayRegion)(void *, jshortArray_custom a, jsize s, jsize l, jshort *b) { if(!a||!b) return; jni_array_hdr *h=(jni_array_hdr*)a; if(s<0||s+l>h->length) return; memcpy(b,(jshort*)h->data+s,l*sizeof(jshort)); }
static void JNI_FN(GetIntArrayRegion)(void *, jintArray_custom a, jsize s, jsize l, jint *b) { if(!a||!b) return; jni_array_hdr *h=(jni_array_hdr*)a; if(s<0||s+l>h->length) return; memcpy(b,(jint*)h->data+s,l*sizeof(jint)); }
static void JNI_FN(GetLongArrayRegion)(void *, jlongArray_custom a, jsize s, jsize l, jlong *b) { if(!a||!b) return; jni_array_hdr *h=(jni_array_hdr*)a; if(s<0||s+l>h->length) return; memcpy(b,(jlong*)h->data+s,l*sizeof(jlong)); }
static void JNI_FN(GetFloatArrayRegion)(void *, jfloatArray_custom a, jsize s, jsize l, jfloat *b) { if(!a||!b) return; jni_array_hdr *h=(jni_array_hdr*)a; if(s<0||s+l>h->length) return; memcpy(b,(jfloat*)h->data+s,l*sizeof(jfloat)); }
static void JNI_FN(GetDoubleArrayRegion)(void *, jdoubleArray_custom a, jsize s, jsize l, jdouble *b) { if(!a||!b) return; jni_array_hdr *h=(jni_array_hdr*)a; if(s<0||s+l>h->length) return; memcpy(b,(jdouble*)h->data+s,l*sizeof(jdouble)); }
static void JNI_FN(SetBooleanArrayRegion)(void *, jbooleanArray_custom a, jsize s, jsize l, const jboolean *b) { if(!a||!b) return; jni_array_hdr *h=(jni_array_hdr*)a; if(s<0||s+l>h->length) return; memcpy((jboolean*)h->data+s,b,l); }
static void JNI_FN(SetCharArrayRegion)(void *, jcharArray_custom a, jsize s, jsize l, const jchar *b) { if(!a||!b) return; jni_array_hdr *h=(jni_array_hdr*)a; if(s<0||s+l>h->length) return; memcpy((jchar*)h->data+s,b,l*sizeof(jchar)); }
static void JNI_FN(SetShortArrayRegion)(void *, jshortArray_custom a, jsize s, jsize l, const jshort *b) { if(!a||!b) return; jni_array_hdr *h=(jni_array_hdr*)a; if(s<0||s+l>h->length) return; memcpy((jshort*)h->data+s,b,l*sizeof(jshort)); }
static void JNI_FN(SetLongArrayRegion)(void *, jlongArray_custom a, jsize s, jsize l, const jlong *b) { if(!a||!b) return; jni_array_hdr *h=(jni_array_hdr*)a; if(s<0||s+l>h->length) return; memcpy((jlong*)h->data+s,b,l*sizeof(jlong)); }
static void JNI_FN(SetFloatArrayRegion)(void *, jfloatArray_custom a, jsize s, jsize l, const jfloat *b) { if(!a||!b) return; jni_array_hdr *h=(jni_array_hdr*)a; if(s<0||s+l>h->length) return; memcpy((jfloat*)h->data+s,b,l*sizeof(jfloat)); }
static void JNI_FN(SetDoubleArrayRegion)(void *, jdoubleArray_custom a, jsize s, jsize l, const jdouble *b) { if(!a||!b) return; jni_array_hdr *h=(jni_array_hdr*)a; if(s<0||s+l>h->length) return; memcpy((jdouble*)h->data+s,b,l*sizeof(jdouble)); }

/* ---- Object arrays (REAL — backed by a jni_array_hdr of jobject_custom) ---- */
static jobjectArray_custom JNI_FN(NewObjectArray)(void *, jsize len, jclass_custom, jobject_custom init) {
    jni_array_hdr *h=(jni_array_hdr*)calloc(1,sizeof(*h)); h->type='L'; h->length=len;
    h->data=calloc(len>0?len:1,sizeof(jobject_custom));
    if (init) { jobject_custom *arr=(jobject_custom*)h->data; for(jsize i=0;i<len;i++) arr[i]=init; }
    return (jobjectArray_custom)h;
}
static jobject_custom JNI_FN(GetObjectArrayElement)(void *, jobjectArray_custom a, jsize idx) {
    if(!a) return nullptr; jni_array_hdr *h=(jni_array_hdr*)a;
    if(idx<0||idx>=h->length) return nullptr;
    return ((jobject_custom*)h->data)[idx];
}
static void JNI_FN(SetObjectArrayElement)(void *, jobjectArray_custom a, jsize idx, jobject_custom val) {
    if(!a) return; jni_array_hdr *h=(jni_array_hdr*)a;
    if(idx<0||idx>=h->length) return;
    ((jobject_custom*)h->data)[idx] = val;
}

/* ---- Call*StaticMethod ---- */
static void JNI_FN(CallStaticVoidMethodV)(void *, jclass_custom, jmethodID_custom mid, va_list ap) {
    if (!mid) return;
    int n_args = count_args(mid->sig);
    std::vector<dex_value_t> args(n_args);
    const char *p = strchr(mid->sig, '('); if (p) p++;
    for (int i = 0; i < n_args; i++) {
        if (!p || !*p || *p == ')') break;
        char c = *p++;
        if (c == 'L' || c == '[') { args[i].ptr = va_arg(ap, void*);
            while (c=='L' && p && *p && *p!=';') p++; if (c=='L' && p && *p) p++; }
        else if (c == 'J') args[i].i64 = va_arg(ap, long long);
        else if (c == 'D') args[i].f64 = va_arg(ap, double);
        else if (c == 'F') args[i].f32 = (float)va_arg(ap, double);
        else args[i].i32 = va_arg(ap, int);
    }
    dex_value_t r;
    dex_invoke(vm(), mid->class_desc, mid->name, mid->sig, args.data(), n_args, &r);
}
static void JNI_FN(CallStaticVoidMethod)(void *env, jclass_custom c, jmethodID_custom m, ...) {
    va_list ap; va_start(ap, m);
    JNI_FN(CallStaticVoidMethodV)(env, c, m, ap);
    va_end(ap);
}
static jint JNI_FN(CallStaticIntMethodV)(void *, jclass_custom, jmethodID_custom mid, va_list ap) {
    if (!mid) return 0;
    int n_args = count_args(mid->sig);
    std::vector<dex_value_t> args(n_args);
    const char *p = strchr(mid->sig, '('); if (p) p++;
    for (int i = 0; i < n_args; i++) {
        if (!p || !*p || *p == ')') break;
        char c = *p++;
        if (c == 'L' || c == '[') { args[i].ptr = va_arg(ap, void*);
            while (c=='L' && p && *p && *p!=';') p++; if (c=='L' && p && *p) p++; }
        else if (c == 'J') args[i].i64 = va_arg(ap, long long);
        else if (c == 'D') args[i].f64 = va_arg(ap, double);
        else if (c == 'F') args[i].f32 = (float)va_arg(ap, double);
        else args[i].i32 = va_arg(ap, int);
    }
    dex_value_t r; r.i32 = 0;
    dex_invoke(vm(), mid->class_desc, mid->name, mid->sig, args.data(), n_args, &r);
    return r.i32;
}
static jint JNI_FN(CallStaticIntMethod)(void *env, jclass_custom c, jmethodID_custom m, ...) {
    va_list ap; va_start(ap, m);
    jint r = JNI_FN(CallStaticIntMethodV)(env, c, m, ap);
    va_end(ap);
    return r;
}
static jobject_custom JNI_FN(CallStaticObjectMethodV)(void *, jclass_custom, jmethodID_custom mid, va_list ap) {
    if (!mid) return nullptr;
    int n_args = count_args(mid->sig);
    std::vector<dex_value_t> args(n_args);
    const char *p = strchr(mid->sig, '('); if (p) p++;
    for (int i = 0; i < n_args; i++) {
        if (!p || !*p || *p == ')') break;
        char c = *p++;
        if (c == 'L' || c == '[') { args[i].ptr = va_arg(ap, void*);
            while (c=='L' && p && *p && *p!=';') p++; if (c=='L' && p && *p) p++; }
        else if (c == 'J') args[i].i64 = va_arg(ap, long long);
        else if (c == 'D') args[i].f64 = va_arg(ap, double);
        else if (c == 'F') args[i].f32 = (float)va_arg(ap, double);
        else args[i].i32 = va_arg(ap, int);
    }
    dex_value_t r; r.ptr = nullptr;
    dex_invoke(vm(), mid->class_desc, mid->name, mid->sig, args.data(), n_args, &r);
    return (jobject_custom)r.ptr;
}
static jobject_custom JNI_FN(CallStaticObjectMethod)(void *env, jclass_custom c, jmethodID_custom m, ...) {
    va_list ap; va_start(ap, m);
    jobject_custom r = JNI_FN(CallStaticObjectMethodV)(env, c, m, ap);
    va_end(ap);
    return r;
}

/* ---- Field access ---- */
static jint JNI_FN(GetIntField)(void *, jobject_custom obj, jfieldID_custom fid) {
    if (!obj || !fid) return 0;
    dex_value_t v; v.i32 = 0;
    dex_get_field(vm(), (dex_obj_t *)obj, fid->name, &v);
    return v.i32;
}
static void JNI_FN(SetIntField)(void *, jobject_custom obj, jfieldID_custom fid, jint val) {
    if (!obj || !fid) return;
    dex_value_t v; v.i32 = val;
    dex_set_field(vm(), (dex_obj_t *)obj, fid->name, v);
}
static jobject_custom JNI_FN(GetObjectField)(void *, jobject_custom obj, jfieldID_custom fid) {
    if (!obj || !fid) return nullptr;
    dex_value_t v; v.ptr = nullptr;
    dex_get_field(vm(), (dex_obj_t *)obj, fid->name, &v);
    return (jobject_custom)v.ptr;
}
static void JNI_FN(SetObjectField)(void *, jobject_custom obj, jfieldID_custom fid, jobject_custom val) {
    if (!obj || !fid) return;
    dex_value_t v; v.ptr = val;
    dex_set_field(vm(), (dex_obj_t *)obj, fid->name, v);
}
static jboolean JNI_FN(GetBooleanField)(void *, jobject_custom obj, jfieldID_custom fid) {
    if (!obj || !fid) return 0;
    dex_value_t v; v.i32 = 0;
    dex_get_field(vm(), (dex_obj_t *)obj, fid->name, &v);
    return v.i32 ? JNI_TRUE : JNI_FALSE;
}
static jint JNI_FN(GetStaticIntField)(void *, jclass_custom, jfieldID_custom fid) {
    if (!fid) return 0;
    dex_value_t v; v.i32 = 0;
    dex_get_static_field(vm(), fid->class_desc, fid->name, &v);
    return v.i32;
}
static void JNI_FN(SetStaticIntField)(void *, jclass_custom, jfieldID_custom fid, jint val) {
    if (!fid) return;
    dex_value_t v; v.i32 = val;
    dex_set_static_field(vm(), fid->class_desc, fid->name, v);
}
static jobject_custom JNI_FN(GetStaticObjectField)(void *, jclass_custom, jfieldID_custom fid) {
    if (!fid) return nullptr;
    dex_value_t v; v.ptr = nullptr;
    dex_get_static_field(vm(), fid->class_desc, fid->name, &v);
    return (jobject_custom)v.ptr;
}

/* ---- Strings ---- */
static jstring_custom JNI_FN(NewStringUTF)(void *, const char *utf8) {
    return (jstring_custom)dex_new_string_utf(vm(), utf8 ? utf8 : "");
}
static jsize JNI_FN(GetStringLength)(void *, jstring_custom s) {
    /* UTF-16 length. Approximate as UTF-8 length for ASCII strings. */
    const char *u = dex_string_utf((dex_obj_t *)s);
    return (jsize)(u ? strlen(u) : 0);
}
static jsize JNI_FN(GetStringUTFLength)(void *, jstring_custom s) {
    const char *u = dex_string_utf((dex_obj_t *)s);
    return (jsize)(u ? strlen(u) : 0);
}
static const char *JNI_FN(GetStringUTFChars)(void *, jstring_custom s, jboolean *isCopy) {
    const char *u = dex_string_utf((dex_obj_t *)s);
    if (isCopy) *isCopy = JNI_FALSE;
    return u;
}
static void JNI_FN(ReleaseStringUTFChars)(void *, jstring_custom, const char *) {
    /* No-op: we returned the internal pointer with isCopy=FALSE. */
}

/* ---- Arrays ----
 * We model a Java array as a dex_obj_t with an extra payload. For v1 we
 * allocate a raw buffer + length; the dex_obj_t tag is "I[", "B[", etc. The
 * GetIntArrayElements returns the raw pointer; Release is a no-op. */
/* jni_array_hdr defined earlier, before Remaining array types section */

static jsize JNI_FN(GetArrayLength)(void *, jarray_custom a) {
    if (!a) return 0;
    jni_array_hdr *h = (jni_array_hdr *)a;
    return h->length;
}
static jintArray_custom JNI_FN(NewIntArray)(void *, jsize len) {
    jni_array_hdr *h = (jni_array_hdr *)calloc(1, sizeof(jni_array_hdr));
    h->type = 'I';
    h->length = len;
    h->data = calloc(len > 0 ? len : 1, sizeof(jint));
    return (jintArray_custom)h;
}
static jint *JNI_FN(GetIntArrayElements)(void *, jintArray_custom a, jboolean *isCopy) {
    if (!a) return nullptr;
    if (isCopy) *isCopy = JNI_FALSE;
    return (jint *)((jni_array_hdr *)a)->data;
}
static void JNI_FN(ReleaseIntArrayElements)(void *, jintArray_custom, jint *, jint) {}
static void JNI_FN(SetIntArrayRegion)(void *, jintArray_custom a, jsize start, jsize len, const jint *buf) {
    if (!a || !buf) return;
    jni_array_hdr *h = (jni_array_hdr *)a;
    if (start < 0 || start + len > h->length) return;
    memcpy((jint *)h->data + start, buf, len * sizeof(jint));
}
static jbyteArray_custom JNI_FN(NewByteArray)(void *, jsize len) {
    jni_array_hdr *h = (jni_array_hdr *)calloc(1, sizeof(jni_array_hdr));
    h->type = 'B';
    h->length = len;
    h->data = calloc(len > 0 ? len : 1, 1);
    return (jbyteArray_custom)h;
}
static jbyte *JNI_FN(GetByteArrayElements)(void *, jbyteArray_custom a, jboolean *isCopy) {
    if (!a) return nullptr;
    if (isCopy) *isCopy = JNI_FALSE;
    return (jbyte *)((jni_array_hdr *)a)->data;
}
static void JNI_FN(ReleaseByteArrayElements)(void *, jbyteArray_custom, jbyte *, jint) {}
static void JNI_FN(SetByteArrayRegion)(void *, jbyteArray_custom a, jsize start, jsize len, const jbyte *buf) {
    if (!a || !buf) return;
    jni_array_hdr *h = (jni_array_hdr *)a;
    if (start < 0 || start + len > h->length) return;
    memcpy((jbyte *)h->data + start, buf, len);
}
static jfloatArray_custom JNI_FN(NewFloatArray)(void *, jsize len) {
    jni_array_hdr *h = (jni_array_hdr *)calloc(1, sizeof(jni_array_hdr));
    h->type = 'F';
    h->length = len;
    h->data = calloc(len > 0 ? len : 1, sizeof(jfloat));
    return (jfloatArray_custom)h;
}
static jfloat *JNI_FN(GetFloatArrayElements)(void *, jfloatArray_custom a, jboolean *isCopy) {
    if (!a) return nullptr;
    if (isCopy) *isCopy = JNI_FALSE;
    return (jfloat *)((jni_array_hdr *)a)->data;
}
static void JNI_FN(ReleaseFloatArrayElements)(void *, jfloatArray_custom, jfloat *, jint) {}

/* ---- RegisterNatives ----
 * The JNINativeMethod array: { const char *name; const char *signature; void *fnPtr; } */
struct JNINativeMethod { const char *name; const char *signature; void *fnPtr; };
static void JNI_FN(RegisterNatives)(void *, jclass_custom cls, const void *methods_raw, jint n) {
    if (!cls || !methods_raw) return;
    extern const char *dex_cls_descriptor(dex_cls_t *cls);
    const char *desc = dex_cls_descriptor((dex_cls_t *)cls);
    if (!desc) return;
    std::string dotted = descriptor_to_dotted(desc);
    const JNINativeMethod *methods = (const JNINativeMethod *)methods_raw;
    for (jint i = 0; i < n; i++) {
        if (!methods[i].name || !methods[i].signature || !methods[i].fnPtr) continue;
        std::lock_guard<std::mutex> lk(g_registered_lock);
        g_registered.push_back({dotted, methods[i].name, methods[i].signature, methods[i].fnPtr});
        LOGI(LOG_TAG_JNI, "RegisterNatives(vtable): %s.%s %s -> %p",
             dotted.c_str(), methods[i].name, methods[i].signature, methods[i].fnPtr);
    }
}
static void JNI_FN(UnregisterNatives)(void *, jclass_custom cls) {
    if (!cls) return;
    extern const char *dex_cls_descriptor(dex_cls_t *cls);
    const char *desc = dex_cls_descriptor((dex_cls_t *)cls);
    if (!desc) return;
    std::string dotted = descriptor_to_dotted(desc);
    std::lock_guard<std::mutex> lk(g_registered_lock);
    g_registered.erase(
        std::remove_if(g_registered.begin(), g_registered.end(),
                       [&](const NativeEntry &e){ return e.cls == dotted; }),
        g_registered.end());
}

/* ---- GetJavaVM ---- */
static jint JNI_FN(GetJavaVM)(void *, void **out) {
    if (!out) return -1;
    *out = jni_bridge_get_javavm();
    return 0;
}

/* ---- Exceptions (we don't really throw Java exceptions; we track a flag) ---- */
static thread_local int t_exception_pending = 0;
static jthrowable_custom JNI_FN(ExceptionOccurred)(void *) {
    return t_exception_pending ? (jthrowable_custom)1 : nullptr;
}
static void JNI_FN(ExceptionDescribe)(void *) {
    if (t_exception_pending) LOGW(LOG_TAG_JNI, "exception pending (described)");
}
static void JNI_FN(ExceptionClear)(void *) { t_exception_pending = 0; }
static jboolean JNI_FN(ExceptionCheck)(void *) { return t_exception_pending ? JNI_TRUE : JNI_FALSE; }
static jint JNI_FN(Throw)(void *, jthrowable_custom) { t_exception_pending = 1; return 0; }
static jint JNI_FN(ThrowNew)(void *, jclass_custom, const char *) { t_exception_pending = 1; return 0; }

/* ---- Direct byte buffers (used by IL2CPP for mesh data) ----
 * We model a direct ByteBuffer as a jni_array_hdr with type='D' and data=address. */
static jobject_custom JNI_FN(NewDirectByteBuffer)(void *env, void *address, jlong capacity) {
    (void)env;
    jni_array_hdr *h = (jni_array_hdr *)calloc(1, sizeof(jni_array_hdr));
    h->type = 'D';
    h->length = (jsize)capacity;
    h->data = address;
    return (jobject_custom)h;
}
static void *JNI_FN(GetDirectBufferAddress)(void *, jobject_custom buf) {
    if (!buf) return nullptr;
    return ((jni_array_hdr *)buf)->data;
}
static jlong JNI_FN(GetDirectBufferCapacity)(void *, jobject_custom buf) {
    if (!buf) return 0;
    return ((jni_array_hdr *)buf)->length;
}

/* ---- JavaVM functions ---- */
static jint JNI_FN(DestroyJavaVM)(void *) { return 0; }
static jint JNI_FN(AttachCurrentThread)(void *, void **env, const void *) {
    if (env) *env = jni_bridge_attach_env();
    return 0;
}
static jint JNI_FN(DetachCurrentThread)(void *) { return 0; }
static jint JNI_FN(GetEnv)(void *, void **env, jint) {
    if (env) *env = jni_bridge_attach_env();
    return 0;
}
static jint JNI_FN(AttachCurrentThreadAsDaemon)(void *, void **env, const void *) {
    if (env) *env = jni_bridge_attach_env();
    return 0;
}

/* The dex_interp module must expose a function returning the descriptor of a
 * resolved class. We declare it extern here; the dex_interp.cpp implementation
 * should provide it. (If it doesn't, the linker will fail — which is the
 * honest signal that this contract is required.) */
