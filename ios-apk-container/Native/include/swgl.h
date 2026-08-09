/*
 * swgl.h — Software OpenGL ES 2.0 implementation (PUBLIC C ABI).
 *
 * Status: REAL (common path). The common path (compile shaders, set up buffers,
 *         draw a textured triangle with vertex+fragment shaders) actually
 *         renders correct pixels into an RGBA8 framebuffer in host memory.
 *         Unimplemented paths (cubemaps, FBOs, MRT, GL_POINTS, GL_LINES) are
 *         logged via LOGW and return a sensible default; see swgl.cpp headers.
 *
 * This is the symbol surface the ELF loader exposes for libEGL.so / libGLESv2.so
 * (those shim libs' resolver returns swgl_resolve(name) for any egl*/gl* name).
 * Swift obtains the rendered framebuffer via swgl_get_framebuffer() and uploads
 * it to an MTLTexture of format .bgra8Unorm (channel swap handled by Swift).
 *
 * Honesty contract: see worklog.md Phase 2 / P2-0. No ANGLE dependency; this is
 * a from-scratch software rasterizer.
 */
#ifndef APKCONTAINER_SWGL_H
#define APKCONTAINER_SWGL_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* One-time init. Idempotent. Returns 0 on success. */
int  swgl_init(void);
void swgl_shutdown(void);

/* The ELF loader calls this to resolve egl*/gl* symbols.
 * Returns NULL if unknown. */
void *swgl_resolve(const char *name);

/* --- EGL (subset) --- */
/* eglGetDisplay(EGLNativeDisplayType) -> EGLDisplay */
void *swgl_egl_get_display(void *native_display);
int   swgl_egl_initialize(void *dpy, int *major, int *minor);
int   swgl_egl_choose_config(void *dpy, const int *attrib_list,
                             void **configs, int config_size, int *num_config);
void *swgl_egl_create_window_surface(void *dpy, void *config,
                                     void *native_window, const int *attrib_list);
void *swgl_egl_create_context(void *dpy, void *config,
                              void *share_ctx, const int *attrib_list);
int   swgl_egl_make_current(void *dpy, void *draw, void *read, void *ctx);
int   swgl_egl_swap_buffers(void *dpy, void *surface);
int   swgl_egl_destroy_surface(void *dpy, void *surface);
int   swgl_egl_destroy_context(void *dpy, void *ctx);
int   swgl_egl_terminate(void *dpy);

/* Attach a CAMetalLayer-backed pixel buffer. Swift calls this with the
 * layer's address (as void*) and a width/height. eglSwapBuffers will mark
 * the framebuffer dirty; Swift then reads via swgl_get_framebuffer() and
 * uploads to its MTLTexture. Width/height of 0 → default 720x1280. */
int   swgl_attach_output(void *cametal_layer_ptr, int width, int height);

/* Get a pointer to the current RGBA8 framebuffer (host-byte-order, R first).
 * Swift reads this and uploads to an MTLTexture of format .bgra8Unorm (note
 * the channel swap; we expose .bgra to match Metal's preferred format). */
const void *swgl_get_framebuffer(void);
int         swgl_get_framebuffer_width(void);
int         swgl_get_framebuffer_height(void);

/* --- GL ES 2.0 entry points (subset). Implement enough to draw a textured
 *     triangle with vertex+fragment shaders. --- */
void swgl_gl_clear_color(float r, float g, float b, float a);
void swgl_gl_clear(int mask);             /* GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT */
void swgl_gl_viewport(int x, int y, int w, int h);
void swgl_gl_scissor(int x, int y, int w, int h);
void swgl_gl_enable(int cap);
void swgl_gl_disable(int cap);
void swgl_gl_blend_func(int sfactor, int dfactor);
void swgl_gl_depth_func(int func);
void swgl_gl_depth_mask(unsigned char flag);
void swgl_gl_color_mask(unsigned char r, unsigned char g, unsigned char b, unsigned char a);

unsigned int swgl_gl_create_shader(int type);    /* GL_VERTEX_SHADER=0x8B31, GL_FRAGMENT_SHADER=0x8B30 */
void swgl_gl_shader_source(unsigned int shader, int count, const char *const*string, const int *length);
void swgl_gl_compile_shader(unsigned int shader);
void swgl_gl_get_shader_iv(unsigned int shader, int pname, int *params);   /* COMPILE_STATUS, INFO_LOG_LENGTH */
void swgl_gl_get_shader_info_log(unsigned int shader, int max_len, int *len, char *info_log);
unsigned int swgl_gl_create_program(void);
void swgl_gl_attach_shader(unsigned int program, unsigned int shader);
void swgl_gl_link_program(unsigned int program);
void swgl_gl_get_program_iv(unsigned int program, int pname, int *params);
void swgl_gl_get_program_info_log(unsigned int program, int max_len, int *len, char *info_log);
void swgl_gl_use_program(unsigned int program);
int  swgl_gl_get_attrib_location(unsigned int program, const char *name);
int  swgl_gl_get_uniform_location(unsigned int program, const char *name);
void swgl_gl_uniform_1i(int location, int v0);
void swgl_gl_uniform_1f(int location, float v0);
void swgl_gl_uniform_2f(int location, float x, float y);
void swgl_gl_uniform_3f(int location, float x, float y, float z);
void swgl_gl_uniform_4f(int location, float x, float y, float z, float w);
void swgl_gl_uniform_matrix_4fv(int location, int count, unsigned char transpose, const float *value);
void swgl_gl_uniform_1fv(int location, int count, const float *value);
void swgl_gl_uniform_2fv(int location, int count, const float *value);
void swgl_gl_uniform_3fv(int location, int count, const float *value);
void swgl_gl_uniform_4fv(int location, int count, const float *value);

void swgl_gl_gen_buffers(int n, unsigned int *buffers);
void swgl_gl_bind_buffer(int target, unsigned int buffer);
void swgl_gl_buffer_data(int target, long size, const void *data, int usage);
void swgl_gl_delete_buffers(int n, const unsigned int *buffers);

void swgl_gl_gen_textures(int n, unsigned int *textures);
void swgl_gl_bind_texture(int target, unsigned int texture);
void swgl_gl_tex_image_2d(int target, int level, int internalformat, int width, int height, int border, int format, int type, const void *pixels);
void swgl_gl_tex_sub_image_2d(int target, int level, int xoffset, int yoffset, int width, int height, int format, int type, const void *pixels);
void swgl_gl_tex_parameter_i(int target, int pname, int param);
void swgl_gl_active_texture(int unit);
void swgl_gl_delete_textures(int n, const unsigned int *textures);

void swgl_gl_enable_vertex_attrib_array(unsigned int index);
void swgl_gl_disable_vertex_attrib_array(unsigned int index);
void swgl_gl_vertex_attrib_pointer(unsigned int index, int size, int type, unsigned char normalized, int stride, const void *pointer);
void swgl_gl_vertex_attrib_4f(unsigned int index, float x, float y, float z, float w);

void swgl_gl_draw_arrays(int mode, int first, int count);
void swgl_gl_draw_elements(int mode, int count, int type, const void *indices);

int  swgl_gl_get_error(void);   /* returns last error; clears it */

#ifdef __cplusplus
}
#endif
#endif
