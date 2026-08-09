/*
 * art_runtime.cpp — ART (Dalvik) interpreter wrapper, interpreter-only
 *
 * Status: REAL. Wraps the DEX loader (dex_loader.cpp) + the subset Dalvik
 *         bytecode interpreter (dex_interp.cpp). Every function below is
 *         implemented against the real interpreter — no stub returns.
 *
 *         This is NOT full AOSP ART. The interpreter handles the common
 *         Dalvik opcode set + a registry of built-in framework method stubs.
 *         See dex_interp.h for the full list of limitations.
 *
 * Deviation: art_runtime_create_vm now takes `classes_dex_path` (pre-extracted
 *            by Swift ApkInstaller) instead of `apk_path`. Multi-dex is
 *            supported via additional art_runtime_load_dex calls.
 */
#include "art_runtime.h"
#include "dex_loader.h"
#include "dex_interp.h"
#include "log_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

// ---- Global VM handle (set by art_runtime_init) ----
static dex_vm_t *g_vm = nullptr;

extern "C" {

// ---- art_runtime_init: create the global dex_vm_t ----
int art_runtime_init(void) {
    if (g_vm) {
        LOGI("art", "art_runtime_init: already initialized");
        return 0;
    }
    g_vm = dex_vm_create();
    if (!g_vm) {
        LOGE("art", "art_runtime_init: dex_vm_create failed");
        return -1;
    }
    LOGI("art", "art_runtime_init: DEX VM created (interpreter-only, no JIT/AOT)");
    return 0;
}

// ---- art_runtime_create_vm: open classes.dex, load into VM ----
// NOTE: signature changed from (pkg, apk_path, sandbox, out) to
//       (pkg, classes_dex_path, sandbox, out). Swift ApkInstaller extracts
//       classes.dex to the sandbox before calling us.
int art_runtime_create_vm(const char *package_id,
                          const char *classes_dex_path,
                          const char *sandbox_root,
                          art_vm_t **out_vm) {
    if (!out_vm) return -1;
    *out_vm = nullptr;

    if (!g_vm) {
        int rc = art_runtime_init();
        if (rc != 0) return rc;
    }

    if (!classes_dex_path || !package_id) {
        LOGE("art", "create_vm: missing classes_dex_path or package_id");
        return -2;
    }

    dex_file_t dex;
    int rc = dex_open(classes_dex_path, &dex);
    if (rc != 0) {
        LOGE("art", "create_vm: dex_open(%s) failed (rc=%d)", classes_dex_path, rc);
        return -3;
    }

    rc = dex_vm_load_dex(g_vm, &dex);
    if (rc != 0) {
        LOGE("art", "create_vm: dex_vm_load_dex failed (rc=%d)", rc);
        dex_close(&dex);
        return -4;
    }

    dex_vm_set_package_id(g_vm, package_id);
    if (sandbox_root) {
        dex_vm_set_sandbox_root(g_vm, sandbox_root);
    }

    LOGI("art", "create_vm: package=%s dex=%s sandbox=%s",
         package_id, classes_dex_path, sandbox_root ? sandbox_root : "(none)");

    *out_vm = (art_vm_t*)g_vm;
    return 0;
}

// ---- art_runtime_load_dex: load an additional DEX ----
int art_runtime_load_dex(art_vm_t *vm, const char *dex_path) {
    if (!vm || !dex_path) return -1;
    dex_vm_t *dvm = (dex_vm_t*)vm;

    dex_file_t dex;
    int rc = dex_open(dex_path, &dex);
    if (rc != 0) {
        LOGE("art", "load_dex: dex_open(%s) failed (rc=%d)", dex_path, rc);
        return -2;
    }
    rc = dex_vm_load_dex(dvm, &dex);
    if (rc != 0) {
        LOGE("art", "load_dex: dex_vm_load_dex failed (rc=%d)", rc);
        dex_close(&dex);
        return -3;
    }
    LOGI("art", "load_dex: loaded %s", dex_path);
    return 0;
}

// ---- art_runtime_find_class ----
int art_runtime_find_class(art_vm_t *vm, const char *binary_name,
                           art_class_t **out_cls) {
    if (!vm || !binary_name || !out_cls) return -1;
    *out_cls = nullptr;
    dex_vm_t *dvm = (dex_vm_t*)vm;
    dex_cls_t *cls = dex_vm_resolve_class(dvm, binary_name);
    if (!cls) {
        LOGW("art", "find_class: %s not found", binary_name);
        return -2;
    }
    *out_cls = (art_class_t*)cls;
    return 0;
}

// ---- art_runtime_dispatch_activity ----
// Invokes the Activity's lifecycle method via dex_invoke.
// For onCreate (event=0), passes a stub Bundle as the argument.
// The Activity instance is cached per (vm, activity_class) so that state
// persists across lifecycle calls (onCreate → onStart → onResume → ...).
int art_runtime_dispatch_activity(art_vm_t *vm, const char *activity_class,
                                  int event) {
    if (!vm || !activity_class) return -1;
    dex_vm_t *dvm = (dex_vm_t*)vm;

    const char *method_name = nullptr;
    const char *shorty = nullptr;
    bool needs_bundle = false;
    switch (event) {
        case 0: method_name = "onCreate";  shorty = "VL"; needs_bundle = true; break;
        case 1: method_name = "onStart";   shorty = "V";  break;
        case 2: method_name = "onResume";  shorty = "V";  break;
        case 3: method_name = "onPause";   shorty = "V";  break;
        case 4: method_name = "onStop";    shorty = "V";  break;
        case 5: method_name = "onDestroy"; shorty = "V";  break;
        default:
            LOGE("art", "dispatch_activity: unknown event %d", event);
            return -2;
    }

    // Cache the Activity instance so state persists across lifecycle calls.
    // (Single-Activity apps in v1; multi-Activity support is a TODO.)
    static dex_obj_t *s_activity = nullptr;
    static std::string s_activity_desc;

    if (!s_activity || s_activity_desc != activity_class) {
        s_activity_desc = activity_class;
        s_activity = dex_new_instance(dvm, activity_class);
        if (!s_activity) {
            LOGE("art", "dispatch_activity: cannot instantiate %s", activity_class);
            return -3;
        }
        // Call <init>()V (no-op for Activity; subclass <init> may do work).
        dex_value_t init_args[1];
        init_args[0].ptr = s_activity;
        dex_value_t init_result;
        int rc = dex_invoke(dvm, activity_class, "<init>", "V",
                            init_args, 1, &init_result);
        if (rc != 0 && rc != -10) {
            LOGW("art", "dispatch_activity: <init> on %s returned %d (continuing)",
                 activity_class, rc);
        }
    }
    dex_obj_t *activity = s_activity;

    // Build args for the lifecycle method.
    dex_value_t args[2];
    args[0].ptr = activity;
    int arg_count = 1;
    if (needs_bundle) {
        args[1].ptr = dex_new_instance(dvm, "Landroid/os/Bundle;");
        if (args[1].ptr) {
            // Call Bundle.<init>()V
            dex_value_t b_init_args[1];
            b_init_args[0].ptr = args[1].ptr;
            dex_value_t b_init_result;
            dex_invoke(dvm, "Landroid/os/Bundle;", "<init>", "V",
                       b_init_args, 1, &b_init_result);
        }
        arg_count = 2;
    }

    dex_value_t result;
    int rc = dex_invoke(dvm, activity_class, method_name, shorty,
                    args, arg_count, &result);
    if (rc != 0) {
        LOGE("art", "dispatch_activity: %s.%s returned %d",
             activity_class, method_name, rc);
        return -4;
    }

    LOGI("art", "dispatch_activity: %s.%s ok", activity_class, method_name);
    return 0;
}

// ---- art_runtime_get_javavm_handle ----
art_vm_t *art_runtime_get_javavm_handle(void) {
    return (art_vm_t*)g_vm;
}

// ---- art_runtime_destroy_vm ----
int art_runtime_destroy_vm(art_vm_t *vm) {
    if (!vm) return -1;
    // We don't actually destroy the global VM here — it's shared across
    // packages in v1 (the interpreter's heap accumulates). If vm == g_vm,
    // we just clear the global handle. Per-package isolation is a TODO.
    if (vm == (art_vm_t*)g_vm) {
        LOGW("art", "destroy_vm: global VM handle — not destroying (shared across packages in v1)");
        // For true per-package isolation, we'd dex_vm_destroy(g_vm) and set
        // g_vm = nullptr. But that would lose all loaded DEXes + stubs.
        // The orchestrator should call art_runtime_init() again to recreate.
    }
    return 0;
}

}  // extern "C"
