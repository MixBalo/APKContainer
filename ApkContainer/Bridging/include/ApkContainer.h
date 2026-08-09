/*
 *  ApkContainer.h
 *  ApkContainer
 *
 *  Bridging header exposing the native C façade (implemented in Native/) to Swift.
 *
 *  Status: IMPLEMENTED. The native side now provides:
 *    - real ELF loader (relocations + DT_INIT_ARRAY + JNI_OnLoad)
 *    - real DEX interpreter (subset Dalvik bytecode + framework stubs)
 *    - real software OpenGL ES 2.0 (swgl) + tiny GLSL ES interpreter
 *    - real JNI bridge (common JNIEnv* / JavaVM* vtable entries)
 *    - real Bionic libc shim (pthread, ashmem, __system_property_get, log)
 *    - per-run log file under <AppSupport>/APKLive/logs/
 *
 *  See docs/ARCHITECTURE.md, docs/CAPABILITY_MATRIX.md, docs/LIMITATIONS.md.
 */

#ifndef APKCONTAINER_H
#define APKCONTAINER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Runtime control --------------------------------------------------- */

/* Provide sandbox + dex + activity paths for a package. Call BEFORE launch().
 * `activity_class` is dotted ("com.example.MainActivity") or a descriptor
 * ("Lcom/example/MainActivity;"); we normalize. */
int apkcontainer_runtime_configure(const char *packageId,
                                   const char *sandbox_root,
                                   const char *classes_dex_path,
                                   const char *activity_class);

/* Launches the APK: load DEX, load .so files, dispatch onCreate/onStart/onResume.
 * Must be preceded by apkcontainer_runtime_configure(). */
int apkcontainer_runtime_launch(const char *packageId);

/* Suspends the running app (Activity.onPause). */
int apkcontainer_runtime_suspend(const char *packageId);

/* Resumes a suspended app (Activity.onResume). */
int apkcontainer_runtime_resume(const char *packageId);

/* Force-quits the running app (Activity.onDestroy + unload .so + free ART). */
int apkcontainer_runtime_force_quit(const char *packageId);

/* ---- Activity lifecycle ------------------------------------------------ */

int apkcontainer_lifecycle_dispatch(const char *packageId, int event);

/* ---- Graphics ---------------------------------------------------------- */

/* Attaches a CAMetalLayer to the native graphics module so swgl can render
 * into it. `cametallayer` is `Unmanaged.passUnretained(layer).toOpaque()`. */
int apkcontainer_graphics_attach_layer(void *cametallayer);

/* Resizes the GL framebuffer. Call when CAMetalLayer.drawableSize changes. */
int apkcontainer_graphics_resize(int width, int height);

/* After eglSwapBuffers, Swift reads the framebuffer and uploads to an MTLTexture.
 * The returned pointer is valid until the next swap. Format is BGRA8 (matches
 * Metal's .bgra8Unorm preferred format) — host-byte-order, B first. */
const void *apkcontainer_get_framebuffer(void);
int         apkcontainer_get_framebuffer_width(void);
int         apkcontainer_get_framebuffer_height(void);

/* ---- Input ------------------------------------------------------------- */

int apkcontainer_input_enqueue_touch(const char *packageId,
                                     int pointerId,
                                     float x,
                                     float y,
                                     float pressure,
                                     int action);

/* ---- Audio ------------------------------------------------------------- */

int apkcontainer_audio_start(void);
int apkcontainer_audio_stop(void);

/* ---- Diagnostics ------------------------------------------------------- */

/* Returns the absolute path of the current run's log file. Swift shows this
 * in the per-app detail view + a global LogViewer. */
const char *apkcontainer_get_log_path(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* APKCONTAINER_H */
