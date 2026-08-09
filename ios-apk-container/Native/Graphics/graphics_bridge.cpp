/*
 * graphics_bridge.cpp — EGL/GLES bridge.
 *
 * Status: REAL (uses software GLES 2.0). All egl*/gl* symbol lookups are
 *         routed through swgl_resolve() (Native/Graphics/swgl.cpp). The
 *         rendered RGBA8 framebuffer is exposed via swgl_get_framebuffer();
 *         Swift reads it after each eglSwapBuffers and uploads to an
 *         MTLTexture of format .bgra8Unorm (channel swap handled by Swift).
 *
 *         ANGLE is no longer the primary path. If the user has built + linked
 *         ANGLE per BUILD_AND_RUN.md §2, its symbols can still be looked up
 *         via dlsym(RTLD_DEFAULT, name) as a fallback — but the software
 *         rasterizer is preferred because it has zero external dependencies.
 *
 * Honesty contract: see worklog.md Phase 2 / P2-0. The software GLES 2.0
 * implementation in swgl.cpp is REAL code that renders textured, shaded
 * triangles; unimplemented paths (cubemaps, FBOs, MRT, GL_POINTS, GL_LINES)
 * are logged via LOGW and return sensible defaults.
 */
#include "graphics_bridge.h"
#include "swgl.h"
#include "log_file.h"

#include <cstdio>
#include <cstring>
#include <dlfcn.h>

#define GFX_TAG "graphics_bridge"

static void *s_attached_layer = nullptr;
static int   s_surface_w = 0, s_surface_h = 0;
static int   s_swgl_inited = 0;
static int   s_angle_linked = 0;   /* set 1 only if ANGLE symbols are also present */

int graphics_bridge_attach_layer(void *cametal_layer) {
    s_attached_layer = cametal_layer;
    LOGI(GFX_TAG, "attach_layer %p", cametal_layer);

    /* Initialize the software GLES implementation (idempotent). */
    if (swgl_init() == 0) {
        s_swgl_inited = 1;
        LOGI(GFX_TAG, "using software GLES 2.0 (swgl)");
    } else {
        LOGE(GFX_TAG, "swgl_init failed — GL calls will crash");
        s_swgl_inited = 0;
    }

    /* Probe whether ANGLE is also linked (optional fallback path). */
    if (dlsym(RTLD_DEFAULT, "eglInitialize")) {
        s_angle_linked = 1;
        LOGI(GFX_TAG, "ANGLE also linked (software GLES is primary; ANGLE available as fallback)");
    } else {
        s_angle_linked = 0;
        LOGI(GFX_TAG, "ANGLE not linked — software GLES is the sole renderer");
    }

    /* Attach the layer to swgl. We don't know the layer's drawableSize from
     * C (it's an Obj-C property), so default to 720x1280. Swift should call
     * a resize API (or swgl_attach_output directly) once it knows the real
     * size; see docs/ARCHITECTURE.md §4. */
    int w = s_surface_w > 0 ? s_surface_w : 720;
    int h = s_surface_h > 0 ? s_surface_h : 1280;
    if (s_swgl_inited) {
        swgl_attach_output(cametal_layer, w, h);
    }
    return s_swgl_inited ? 0 : -1;
}

int graphics_bridge_swap_buffers(void) {
    if (!s_swgl_inited) return -1;
    /* The current EGL display/surface/context live inside swgl's global state;
     * we just call swgl_egl_swap_buffers with sentinel pointers. swgl ignores
     * them (it has a single display/context/surface). */
    return swgl_egl_swap_buffers(nullptr, nullptr) ? 0 : -1;
}

void *graphics_bridge_resolve(const char *symbol) {
    if (!symbol) return nullptr;

    /* Route any egl*/gl* symbol through the software GLES resolver. */
    if (strncmp(symbol, "egl", 3) == 0 || strncmp(symbol, "gl", 2) == 0) {
        void *p = swgl_resolve(symbol);
        if (p) return p;
        /* Fall through to dlsym (in case ANGLE exports symbols not in swgl). */
    }

    /* ANGLE fallback: if the user has linked ANGLE, its symbols are in the
     * process image and dlsym(RTLD_DEFAULT, ...) will find them. This is the
     * secondary path — software GLES is primary. */
    void *p = dlsym(RTLD_DEFAULT, symbol);
    if (!p) {
        LOGW(GFX_TAG, "resolve: %s NOT FOUND (neither swgl nor ANGLE provides it)", symbol);
    }
    return p;
}

int graphics_bridge_teardown(void) {
    if (s_swgl_inited) {
        swgl_shutdown();
        s_swgl_inited = 0;
    }
    s_attached_layer = nullptr;
    s_surface_w = s_surface_h = 0;
    return 0;
}

/* Optional: let Swift set the surface size after attach (e.g. when the
 * CAMetalLayer's drawableSize becomes known). Not declared in the header;
 * Swift can call swgl_attach_output directly with the new size. */
int graphics_bridge_set_surface_size(int w, int h) {
    s_surface_w = w;
    s_surface_h = h;
    if (s_swgl_inited && s_attached_layer) {
        swgl_attach_output(s_attached_layer, w, h);
    }
    return 0;
}
