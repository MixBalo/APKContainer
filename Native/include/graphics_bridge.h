// graphics_bridge.h — EGL/GLES bridge.
//
// Status: REAL (uses software GLES 2.0). All egl* / gl* symbol lookups are
//         routed through swgl_resolve() (Native/Graphics/swgl.cpp). The
//         rendered RGBA8 framebuffer is exposed via swgl_get_framebuffer();
//         Swift reads it after each eglSwapBuffers and uploads to an
//         MTLTexture of format .bgra8Unorm.
//
//         ANGLE is no longer the primary path. If the user has built + linked
//         ANGLE per BUILD_AND_RUN.md section 2, its symbols can still be looked up
//         via dlsym(RTLD_DEFAULT, name) as a fallback — but the software
//         rasterizer is preferred.

#ifndef GRAPHICS_BRIDGE_H
#define GRAPHICS_BRIDGE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Resolve an egl* / gl* name; if swgl doesn't provide a symbol, fall back to dlsym for
// anything else (e.g. ANGLE).
void *graphics_bridge_resolve(const char *symbol);   // egl* / gl* lookup

int graphics_bridge_attach_layer(void *cametallayer);

#ifdef __cplusplus
}
#endif

#endif
