/*
 * graphics_bridge.h — EGL/GLES bridge.
 *
 * Status: REAL (uses software GLES 2.0). All egl*/gl* symbol lookups are
 *         routed through swgl_resolve() (Native/Graphics/swgl.cpp). The
 *         rendered RGBA8 framebuffer is exposed via swgl_get_framebuffer();
 *         Swift reads it after each eglSwapBuffers and uploads to an
 *         MTLTexture of format .bgra8Unorm.
 *
 *         ANGLE is no longer the primary path. If the user has built + linked
 *         ANGLE per BUILD_AND_RUN.md §2, its symbols can still be looked up
 *         via dlsym(RTLD_DEFAULT, name) as a fallback — but the software
 *         rasterizer is preferred.
 */
#ifndef APKCONTAINER_GRAPHICS_BRIDGE_H
#define APKCONTAINER_GRAPHICS_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Attach a CAMetalLayer (as void* to avoid pulling CoreAnimation into C) as
 * the EGL native window for the currently-launching package. Returns 0 on OK. */
int graphics_bridge_attach_layer(void *cametal_layer);

/* Swap buffers for the current context. Called from eglSwapBuffers trampoline. */
int graphics_bridge_swap_buffers(void);

/* EGL entry-point trampolines. These resolve via swgl_resolve() for any
 * egl*/gl* name; if swgl doesn't provide a symbol, fall back to dlsym for
 * ANGLE (when linked). */
void *graphics_bridge_resolve(const char *symbol);   /* egl*/gl* lookup */

/* Optionally set the surface size after attach (e.g. once the CAMetalLayer's
 * drawableSize is known). Safe to call before attach_layer (records the size
 * for the next attach). */
int graphics_bridge_set_surface_size(int w, int h);

/* Teardown for the current package's surface/context. */
int graphics_bridge_teardown(void);

#ifdef __cplusplus
}
#endif
#endif
