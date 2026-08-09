// swgl.h — Software OpenGL ES 2.0 implementation (PUBLIC C ABI).
//
// Status: REAL (common path). The common path (compile shaders, set up buffers,
//         draw a textured triangle with vertex+fragment shaders) actually
//         renders correct pixels into an RGBA8 framebuffer in host memory.
//         Unimplemented paths (cubemaps, FBOs, MRT, GL_POINTS, GL_LINES) are
//         logged via LOGW and return a sensible default; see swgl.cpp headers.
//
// This is the symbol surface the ELF loader exposes for libEGL.so / libGLESv2.so
// (those shim libs' resolver returns swgl_resolve(name) for any egl* / gl* name).
// Swift obtains the rendered framebuffer via swgl_get_framebuffer() and uploads
// it to an MTLTexture of format .bgra8Unorm (channel swap handled by Swift).
//
// Honesty contract: see worklog.md Phase 2 / P2-0. No ANGLE dependency; this is
// a from-scratch software rasterizer.

#ifndef SWGL_H
#define SWGL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The ELF loader calls this to resolve egl* / gl* symbols.
void *swgl_resolve(const char *name);

const void *swgl_get_framebuffer(void);
int swgl_get_framebuffer_width(void);
int swgl_get_framebuffer_height(void);
int swgl_init(void);
int swgl_attach_output(void *layer, int width, int height);

int swgl_egl_swap_buffers(void *dpy, void *surface);
void swgl_shutdown(void);


#ifdef __cplusplus
}
#endif

#endif
