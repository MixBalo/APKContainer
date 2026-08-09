/*
 * swgl.cpp — Software OpenGL ES 2.0 implementation.
 *
 * Status: REAL (common path). The common path (eglCreateContext +
 *         eglMakeCurrent + glCreateShader/Program/Link/Use + glUniform* +
 *         glBindBuffer/glBufferData + glVertexAttribPointer +
 *         glBindTexture/glTexImage2D + glDrawArrays(GL_TRIANGLES) +
 *         eglSwapBuffers) actually renders a textured, shaded triangle into
 *         an RGBA8 framebuffer in host memory, which Swift reads via
 *         swgl_get_framebuffer() and uploads to an MTLTexture.
 *
 *         PARTIAL/STUB paths (logged via LOGW, return sensible defaults):
 *         - GL_POINTS, GL_LINES, GL_LINE_STRIP, GL_LINE_LOOP, GL_TRIANGLE_FAN
 *           (only GL_TRIANGLES and GL_TRIANGLE_STRIP are fully implemented;
 *           FAN works but is rare).
 *         - glDrawElements (REAL for GL_TRIANGLES / STRIP / FAN; works).
 *         - FBOs (glGenFramebuffers etc.) — not implemented; logs + no-op.
 *         - Cubemaps (GL_TEXTURE_CUBE_MAP) — not implemented; logs + no-op.
 *         - Mipmaps — only level 0 is sampled; min_filter of
 *           GL_*_MIPMAP_* logs a warning and falls back to level 0.
 *         - glPolygonMode, glCullFace, glFrontFace — accepted, no-op.
 *         - Multi-sample, stencil — not implemented.
 *         - Precision hints — accepted, ignored (we use float32 throughout).
 *
 * Honesty contract: see worklog.md Phase 2 / P2-0. No ANGLE dependency.
 */

#include "swgl.h"
#include "glsl.h"
#include "log_file.h"

#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <algorithm>

/* ===== GL constants we need to recognize ===== */
#define GL_NO_ERROR                  0
#define GL_INVALID_ENUM              0x0500
#define GL_INVALID_VALUE             0x0501
#define GL_INVALID_OPERATION         0x0502
#define GL_OUT_OF_MEMORY             0x0505

#define GL_DEPTH_BUFFER_BIT          0x00000100
#define GL_STENCIL_BUFFER_BIT        0x00000400
#define GL_COLOR_BUFFER_BIT          0x00004000

#define GL_FALSE                     0
#define GL_TRUE                      1

#define GL_BYTE                      0x1400
#define GL_UNSIGNED_BYTE             0x1401
#define GL_SHORT                     0x1402
#define GL_UNSIGNED_SHORT            0x1403
#define GL_INT                       0x1404
#define GL_UNSIGNED_INT              0x1405
#define GL_FLOAT                     0x1406

#define GL_NEAREST                   0x2600
#define GL_LINEAR                    0x2601
#define GL_NEAREST_MIPMAP_NEAREST    0x2700
#define GL_LINEAR_MIPMAP_NEAREST     0x2701
#define GL_NEAREST_MIPMAP_LINEAR     0x2702
#define GL_LINEAR_MIPMAP_LINEAR      0x2703

#define GL_TEXTURE_MAG_FILTER        0x2800
#define GL_TEXTURE_MIN_FILTER        0x2801
#define GL_TEXTURE_WRAP_S            0x2802
#define GL_TEXTURE_WRAP_T            0x2803
#define GL_TEXTURE_WRAP_R            0x8072

#define GL_CLAMP_TO_EDGE             0x812F
#define GL_REPEAT                    0x2901
#define GL_MIRRORED_REPEAT           0x8370

#define GL_RGBA                      0x1908
#define GL_RGB                       0x1907
#define GL_RED                       0x1903
#define GL_LUMINANCE                 0x1909
#define GL_LUMINANCE_ALPHA           0x190A
#define GL_ALPHA                     0x1906
#define GL_BGRA                      0x80E1
#define GL_UNSIGNED_SHORT_4_4_4_4    0x8033
#define GL_UNSIGNED_SHORT_5_5_5_1    0x8034
#define GL_UNSIGNED_SHORT_5_6_5      0x8363

#define GL_TEXTURE_2D                0x0DE1
#define GL_TEXTURE_CUBE_MAP          0x8513

#define GL_ARRAY_BUFFER              0x8892
#define GL_ELEMENT_ARRAY_BUFFER      0x8893

#define GL_STATIC_DRAW               0x88E4
#define GL_DYNAMIC_DRAW              0x88E8
#define GL_STREAM_DRAW               0x88E0

#define GL_TRIANGLES                 0x0004
#define GL_TRIANGLE_STRIP            0x0005
#define GL_TRIANGLE_FAN              0x0006
#define GL_POINTS                    0x0000
#define GL_LINES                     0x0001
#define GL_LINE_STRIP                0x0003
#define GL_LINE_LOOP                 0x0002

#define GL_VERTEX_SHADER             0x8B31
#define GL_FRAGMENT_SHADER           0x8B30

#define GL_COMPILE_STATUS            0x8B81
#define GL_LINK_STATUS               0x8B82
#define GL_INFO_LOG_LENGTH           0x8B84

#define GL_DEPTH_TEST                0x0B71
#define GL_BLEND                     0x0BE2
#define GL_SCISSOR_TEST              0x0C11
#define GL_CULL_FACE                 0x0B44
#define GL_DITHER                    0x0BD0
#define GL_POLYGON_OFFSET_FILL       0x8037
#define GL_SAMPLE_ALPHA_TO_COVERAGE  0x809E
#define GL_SAMPLE_COVERAGE           0x80A0

#define GL_NEVER                     0x0200
#define GL_LESS                      0x0201
#define GL_EQUAL                     0x0202
#define GL_LEQUAL                    0x0203
#define GL_GREATER                   0x0204
#define GL_NOTEQUAL                  0x0205
#define GL_GEQUAL                    0x0206
#define GL_ALWAYS                    0x0207

#define GL_SRC_ALPHA                 0x0302
#define GL_ONE_MINUS_SRC_ALPHA       0x0303
#define GL_ONE                       1
#define GL_ZERO                      0
#define GL_SRC_COLOR                 0x0300
#define GL_ONE_MINUS_SRC_COLOR       0x0301
#define GL_DST_COLOR                 0x0306
#define GL_ONE_MINUS_DST_COLOR       0x0307
#define GL_DST_ALPHA                 0x0304
#define GL_ONE_MINUS_DST_ALPHA       0x0305
#define GL_CONSTANT_COLOR            0x8001
#define GL_ONE_MINUS_CONSTANT_COLOR  0x8002
#define GL_CONSTANT_ALPHA            0x8003
#define GL_ONE_MINUS_CONSTANT_ALPHA  0x8004

#define GL_ACTIVE_TEXTURE            0x84E0
#define GL_TEXTURE0                  0x84C0

#define GL_MAX_VERTEX_ATTRIBS        0x8869
#define MAX_VERTEX_ATTRIBS           16

namespace {

/* ===================== Logging helpers ===================== */
#define SWGL_LOGD(...) LOGD("swgl", __VA_ARGS__)
#define SWGL_LOGI(...) LOGI("swgl", __VA_ARGS__)
#define SWGL_LOGW(...) LOGW("swgl", __VA_ARGS__)
#define SWGL_LOGE(...) LOGE("swgl", __VA_ARGS__)

/* ===================== Resources ===================== */

struct Buffer {
    unsigned int id = 0;
    std::vector<uint8_t> data;
};

struct Texture {
    unsigned int id = 0;
    int target = GL_TEXTURE_2D;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;  /* RGBA8, row-major */
    int min_filter = GL_NEAREST_MIPMAP_LINEAR;
    int mag_filter = GL_LINEAR;
    int wrap_s = GL_CLAMP_TO_EDGE;
    int wrap_t = GL_CLAMP_TO_EDGE;
    int internal_format = GL_RGBA;
};

struct Shader {
    unsigned int id = 0;
    int type = 0;             /* GL_VERTEX_SHADER or GL_FRAGMENT_SHADER */
    std::string source;
    int compile_status = 0;
    std::string info_log;
};

struct ProgramObj {
    unsigned int id = 0;
    unsigned int vs_id = 0;
    unsigned int fs_id = 0;
    int link_status = 0;
    std::string info_log;
    glsl_program_t* glsl = nullptr;     /* created at link time */
};

/* Per-vertex-attrib binding state. */
struct VertexAttrib {
    int enabled = 0;
    int size = 4;
    int type = GL_FLOAT;
    int normalized = 0;
    int stride = 0;
    const void* pointer = nullptr;      /* byte offset into VBO if bound, or raw pointer */
    float generic[4] = {0.0f, 0.0f, 0.0f, 1.0f};   /* used when not enabled or no VBO */
};

/* ===================== Framebuffer ===================== */
struct Framebuffer {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> color;   /* RGBA8, row-major top-down (row 0 = top) */
    std::vector<float>  depth;    /* per-pixel depth, 0..1 */
};

/* ===================== EGL handles (sentinel pointers) ===================== */
/* We only ever have one display/context/surface; use tagged pointers. */
struct EGLDisplayImpl   { int dummy; };
struct EGLContextImpl   { int dummy; };
struct EGLSurfaceImpl   { int dummy; };
struct EGLConfigImpl    { int dummy; };

static EGLDisplayImpl g_display_singleton;
static EGLContextImpl g_context_singleton;
static EGLSurfaceImpl g_surface_singleton;
static EGLConfigImpl  g_config_singleton;

/* ===================== Global state ===================== */
struct GLState {
    int inited = 0;

    /* EGL */
    void* current_display = nullptr;
    void* current_draw_surface = nullptr;
    void* current_read_surface = nullptr;
    void* current_context = nullptr;
    void* attached_layer = nullptr;
    int   attached_w = 0;
    int   attached_h = 0;

    /* Resources */
    unsigned int next_id = 1;
    std::vector<Buffer>   buffers;
    std::vector<Texture>  textures;
    std::vector<Shader>   shaders;
    std::vector<ProgramObj> programs;

    /* Bindings */
    unsigned int bound_array_buffer = 0;
    unsigned int bound_element_array_buffer = 0;
    int active_texture_unit = 0;   /* 0..N */
    unsigned int bound_texture_2d[4] = {0,0,0,0};   /* per-unit */
    unsigned int bound_program = 0;

    /* Vertex attrib arrays */
    VertexAttrib vertex_attribs[MAX_VERTEX_ATTRIBS];

    /* State */
    float clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int viewport_x = 0, viewport_y = 0, viewport_w = 0, viewport_h = 0;
    int scissor_x = 0, scissor_y = 0, scissor_w = 0, scissor_h = 0;
    int scissor_enabled = 0;
    int depth_test_enabled = 0;
    int depth_mask = 1;
    int depth_func = GL_LESS;
    int blend_enabled = 0;
    int blend_sfactor = GL_ONE;
    int blend_dfactor = GL_ZERO;
    int color_mask_r = 1, color_mask_g = 1, color_mask_b = 1, color_mask_a = 1;
    int cull_face_enabled = 0;
    int dither_enabled = 1;   /* accepted, ignored */

    /* Error */
    int error = GL_NO_ERROR;

    /* Framebuffer */
    Framebuffer fb;
};

static GLState g;

void set_error(int e) {
    if (g.error == GL_NO_ERROR) {
        g.error = e;
        SWGL_LOGW("set GL error 0x%x", e);
    }
}

/* ===== Resource lookup helpers ===== */
Buffer* find_buffer(unsigned int id) {
    for (auto& b : g.buffers) if (b.id == id) return &b;
    return nullptr;
}
Texture* find_texture(unsigned int id) {
    for (auto& t : g.textures) if (t.id == id) return &t;
    return nullptr;
}
Shader* find_shader(unsigned int id) {
    for (auto& s : g.shaders) if (s.id == id) return &s;
    return nullptr;
}
ProgramObj* find_program(unsigned int id) {
    for (auto& p : g.programs) if (p.id == id) return &p;
    return nullptr;
}

unsigned int alloc_id() { return g.next_id++; }

/* Allocate / resize framebuffer to match the attached output. */
static void fb_resize(int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (g.fb.width == w && g.fb.height == h && !g.fb.color.empty()) return;
    g.fb.width = w;
    g.fb.height = h;
    g.fb.color.assign((size_t)w * h * 4, 0);
    g.fb.depth.assign((size_t)w * h, 1.0f);
    /* Default viewport to the full framebuffer. */
    if (g.viewport_w == 0 || g.viewport_h == 0) {
        g.viewport_x = 0;
        g.viewport_y = 0;
        g.viewport_w = w;
        g.viewport_h = h;
    }
}

/* ===== glsl_texture_view builder ===== */
static int build_texture_view(unsigned int tex_id, glsl_texture_view& out) {
    Texture* t = find_texture(tex_id);
    if (!t || t->width <= 0 || t->height <= 0) return -1;
    out.width = t->width;
    out.height = t->height;
    out.rgba = t->rgba.data();
    out.min_filter = t->min_filter;
    out.mag_filter = t->mag_filter;
    out.wrap_s = t->wrap_s;
    out.wrap_t = t->wrap_t;
    return 0;
}

/* ===================== Rasterizer ===================== */

struct VertexOutput {
    float clip[4];                /* clip-space position */
    float inv_w;                  /* 1/clip[3] (valid if clip[3] != 0) */
    float screen_x, screen_y;     /* screen-space (top-down) coords */
    float ndc_z;                  /* depth 0..1 */
    std::vector<float> varyings;  /* flat varying array (varying_count * 4 floats) */
};

/* Convert a GL type to byte size. */
static int type_size(int type) {
    switch (type) {
        case GL_BYTE: case GL_UNSIGNED_BYTE:  return 1;
        case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
        case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: return 4;
        default: return 0;
    }
}

/* Read one component of a vertex attrib from raw bytes. */
static float read_attrib_component(const void* base, int index, int type, int normalized) {
    const uint8_t* bytes = (const uint8_t*)base;
    switch (type) {
        case GL_FLOAT:         return ((const float*)base)[index];
        case GL_UNSIGNED_BYTE: {
            uint8_t v = bytes[index];
            return normalized ? (v / 255.0f) : (float)v;
        }
        case GL_BYTE: {
            int8_t v = (int8_t)bytes[index];
            return normalized ? (v < 0 ? (v + 256.0f) / 255.0f : v / 255.0f) : (float)v;
        }
        case GL_SHORT: {
            int16_t v = ((const int16_t*)base)[index];
            return normalized ? (v < 0 ? (v + 32768.0f) / 32767.0f : (float)v / 32767.0f) : (float)v;
        }
        case GL_UNSIGNED_SHORT: {
            uint16_t v = ((const uint16_t*)base)[index];
            return normalized ? (v / 65535.0f) : (float)v;
        }
        case GL_INT: {
            int32_t v = ((const int32_t*)base)[index];
            return normalized ? (v < 0 ? (v + 2147483648.0f) / 2147483647.0f : (float)v / 2147483647.0f) : (float)v;
        }
        case GL_UNSIGNED_INT: {
            uint32_t v = ((const uint32_t*)base)[index];
            return normalized ? (v / 4294967295.0f) : (float)v;
        }
        default: return 0.0f;
    }
}

/* Gather attribs for one vertex and run the vertex shader. */
static void run_vertex(ProgramObj* prog, int vertex_index, VertexOutput& out, int varying_count) {
    glsl_attrib attribs[MAX_VERTEX_ATTRIBS];
    int n_attribs = 0;

    /* For each enabled attrib array, gather its data. */
    for (int i = 0; i < MAX_VERTEX_ATTRIBS; i++) {
        VertexAttrib& va = g.vertex_attribs[i];
        if (!va.enabled) continue;

        /* Find the attribute in the program by location i. */
        /* We just pass the index; glsl_run_vertex will look up the program's
         * attribute by index. */
        glsl_attrib& a = attribs[n_attribs++];
        a.index = i;
        a.size = va.size;
        a.type = va.type;
        a.normalized = va.normalized;

        const void* base = nullptr;
        if (g.bound_array_buffer != 0) {
            Buffer* buf = find_buffer(g.bound_array_buffer);
            if (!buf || buf->data.empty()) continue;
            size_t offset = (size_t)va.pointer + (size_t)vertex_index * (va.stride ? va.stride : (va.size * type_size(va.type)));
            if (offset + va.size * type_size(va.type) > buf->data.size()) continue;
            base = buf->data.data() + offset;
        } else if (va.pointer != nullptr) {
            /* No VBO: pointer is a raw pointer into client memory (which our
             * APK doesn't really have — but we accept it). */
            base = (const uint8_t*)va.pointer + (size_t)vertex_index * (va.stride ? va.stride : (va.size * type_size(va.type)));
        } else {
            /* No VBO and no pointer: skip; the generic value will be used
             * (added below). */
            continue;
        }
        a.value = base;
    }

    /* Also include any generic attribs (set via glVertexAttrib4f) for slots
     * that aren't enabled but are referenced by the shader. We add them
     * with a pointer to the generic value. */
    for (int i = 0; i < MAX_VERTEX_ATTRIBS; i++) {
        if (g.vertex_attribs[i].enabled) continue;
        /* Only add if the program uses this attrib slot. */
        glsl_attrib a;
        a.index = i;
        a.size = 4;
        a.type = GL_FLOAT;
        a.normalized = 0;
        a.value = g.vertex_attribs[i].generic;
        /* Append; glsl_run_vertex will only use it if the program references it. */
        if (n_attribs < MAX_VERTEX_ATTRIBS) {
            attribs[n_attribs++] = a;
        }
    }

    out.varyings.assign(varying_count * 4, 0.0f);
    glsl_run_vertex(prog->glsl, attribs, n_attribs, out.clip, out.varyings.data(), varying_count);

    /* Perspective divide + viewport transform. */
    float w = out.clip[3];
    if (w == 0.0f) w = 1e-9f;
    out.inv_w = 1.0f / w;
    float ndc_x = out.clip[0] / w;
    float ndc_y = out.clip[1] / w;
    float ndc_z_val = out.clip[2] / w;
    out.ndc_z = (ndc_z_val + 1.0f) * 0.5f;
    if (out.ndc_z < 0.0f) out.ndc_z = 0.0f;
    if (out.ndc_z > 1.0f) out.ndc_z = 1.0f;

    /* Viewport transform. We store framebuffer row 0 at the top, so flip y. */
    out.screen_x = (ndc_x + 1.0f) * 0.5f * g.viewport_w + g.viewport_x;
    /* GL viewport.y is from bottom; our framebuffer row 0 is at the top.
     * screen_y (top-down) = fb_height - 1 - ((ndc_y + 1) * 0.5 * viewport_h + viewport_y)
     *                     = fb_height - 1 - viewport_y - (ndc_y + 1) * 0.5 * viewport_h */
    out.screen_y = (float)g.fb.height - 1.0f - g.viewport_y - (ndc_y + 1.0f) * 0.5f * g.viewport_h;
}

/* Sutherland-Hodgman clip against the near plane (clip.z + clip.w >= 0).
 * Returns the clipped polygon (3 or 4 vertices) in `out`. */
static void clip_near(const VertexOutput* in, int n_in, std::vector<VertexOutput>& out) {
    out.clear();
    if (n_in == 0) return;
    auto is_inside = [](const VertexOutput& v) {
        return v.clip[2] + v.clip[3] >= 0.0f;
    };
    auto interp = [](const VertexOutput& a, const VertexOutput& b) {
        /* t such that (a.clip.z + a.clip.w) + t * ((b.clip.z+b.clip.w) - (a.clip.z+a.clip.w)) = 0 */
        float da = a.clip[2] + a.clip[3];
        float db = b.clip[2] + b.clip[3];
        float t = (da == db) ? 0.0f : da / (da - db);
        VertexOutput r;
        for (int i = 0; i < 4; i++) r.clip[i] = a.clip[i] + t * (b.clip[i] - a.clip[i]);
        float w = r.clip[3];
        if (w == 0.0f) w = 1e-9f;
        r.inv_w = 1.0f / w;
        /* Re-derive screen coords + ndc_z from the interpolated clip position
         * (interpolating them linearly in clip space would be wrong). */
        float ndc_x = r.clip[0] / w;
        float ndc_y = r.clip[1] / w;
        float ndc_z_val = r.clip[2] / w;
        r.ndc_z = (ndc_z_val + 1.0f) * 0.5f;
        if (r.ndc_z < 0.0f) r.ndc_z = 0.0f;
        if (r.ndc_z > 1.0f) r.ndc_z = 1.0f;
        r.screen_x = (ndc_x + 1.0f) * 0.5f * g.viewport_w + g.viewport_x;
        r.screen_y = (float)g.fb.height - 1.0f - g.viewport_y - (ndc_y + 1.0f) * 0.5f * g.viewport_h;
        /* Interpolate varyings linearly in clip space (correct for
         * perspective-correct interpolation downstream). */
        r.varyings.assign(a.varyings.size(), 0.0f);
        for (size_t i = 0; i < a.varyings.size(); i++) {
            r.varyings[i] = a.varyings[i] + t * (b.varyings[i] - a.varyings[i]);
        }
        return r;
    };
    for (int i = 0; i < n_in; i++) {
        const VertexOutput& cur = in[i];
        const VertexOutput& nxt = in[(i + 1) % n_in];
        bool cur_in = is_inside(cur);
        bool nxt_in = is_inside(nxt);
        if (cur_in) out.push_back(cur);
        if (cur_in != nxt_in) {
            out.push_back(interp(cur, nxt));
        }
    }
}

/* Bind textures to the program's sampler units. */
static void bind_sampler_textures(ProgramObj* prog) {
    if (!prog->glsl) return;
    /* For each texture unit, if there's a bound texture, plug it in. */
    for (int unit = 0; unit < 4; unit++) {
        if (g.bound_texture_2d[unit] != 0) {
            glsl_texture_view view;
            if (build_texture_view(g.bound_texture_2d[unit], view) == 0) {
                glsl_bind_texture_to_unit(prog->glsl, unit, &view);
            } else {
                glsl_bind_texture_to_unit(prog->glsl, unit, nullptr);
            }
        } else {
            glsl_bind_texture_to_unit(prog->glsl, unit, nullptr);
        }
    }
}

/* Rasterize one triangle (3 vertices). */
static void raster_triangle(const VertexOutput& v0, const VertexOutput& v1, const VertexOutput& v2,
                            ProgramObj* prog, int varying_count) {
    /* Compute bounding box in screen space (top-down). */
    float min_x = std::min({v0.screen_x, v1.screen_x, v2.screen_x});
    float max_x = std::max({v0.screen_x, v1.screen_x, v2.screen_x});
    float min_y = std::min({v0.screen_y, v1.screen_y, v2.screen_y});
    float max_y = std::max({v0.screen_y, v1.screen_y, v2.screen_y});

    int x0 = (int)floorf(min_x);
    int x1 = (int)ceilf(max_x);
    int y0 = (int)floorf(min_y);
    int y1 = (int)ceilf(max_y);

    /* Clamp to framebuffer. */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g.fb.width)  x1 = g.fb.width;
    if (y1 > g.fb.height) y1 = g.fb.height;

    /* Clamp to viewport (GL convention: origin bottom-left; convert to
     * top-down row range). */
    {
        int vx0 = g.viewport_x;
        int vx1 = g.viewport_x + g.viewport_w;
        int vy_top = g.fb.height - (g.viewport_y + g.viewport_h);
        int vy_bot = g.fb.height - g.viewport_y;
        if (vx0 > x0) x0 = vx0;
        if (vx1 < x1) x1 = vx1;
        if (vy_top > y0) y0 = vy_top;
        if (vy_bot < y1) y1 = vy_bot;
    }

    /* Scissor clamp. */
    if (g.scissor_enabled) {
        /* scissor_x/y/w/h are in GL convention (origin bottom-left).
         * Convert to top-down row range. */
        int sx0 = g.scissor_x;
        int sx1 = g.scissor_x + g.scissor_w;
        int sy_top = g.fb.height - (g.scissor_y + g.scissor_h);
        int sy_bot = g.fb.height - g.scissor_y;
        int sy0 = sy_top;
        int sy1 = sy_bot;
        if (sx0 > x0) x0 = sx0;
        if (sx1 < x1) x1 = sx1;
        if (sy0 > y0) y0 = sy0;
        if (sy1 < y1) y1 = sy1;
    }

    if (x0 >= x1 || y0 >= y1) return;

    /* Edge functions. Using signed-area (counter-clockwise = positive). */
    /* Area = 0.5 * ((v1-v0) × (v2-v0)) — but we use 2x Area for barycentric. */
    float area = (v1.screen_x - v0.screen_x) * (v2.screen_y - v0.screen_y) -
                 (v1.screen_y - v0.screen_y) * (v2.screen_x - v0.screen_x);
    if (area == 0.0f) return;
    float inv_area = 1.0f / area;

    /* Precompute 1/w for perspective-correct interpolation. */
    float iw0 = v0.inv_w;
    float iw1 = v1.inv_w;
    float iw2 = v2.inv_w;

    /* For each pixel in the bbox. */
    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
            /* Pixel center at (px + 0.5, py + 0.5). */
            float cx = px + 0.5f;
            float cy = py + 0.5f;

            /* Barycentric coords (signed area / total area). */
            float w0 = ((v1.screen_x - cx) * (v2.screen_y - cy) -
                        (v1.screen_y - cy) * (v2.screen_x - cx)) * inv_area;
            float w1 = ((v2.screen_x - cx) * (v0.screen_y - cy) -
                        (v2.screen_y - cy) * (v0.screen_x - cx)) * inv_area;
            float w2 = 1.0f - w0 - w1;

            /* Inside test (use the same sign as `area`). */
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
                /* Allow tiny epsilon for edge pixels. */
                if (w0 < -0.0001f || w1 < -0.0001f || w2 < -0.0001f) continue;
            }

            /* Depth interpolation (linear in screen space — GL convention). */
            float depth = w0 * v0.ndc_z + w1 * v1.ndc_z + w2 * v2.ndc_z;
            if (depth < 0.0f) depth = 0.0f;
            if (depth > 1.0f) depth = 1.0f;

            /* Depth test. */
            size_t pix_idx = (size_t)py * g.fb.width + px;
            if (g.depth_test_enabled) {
                float stored = g.fb.depth[pix_idx];
                int pass = 0;
                switch (g.depth_func) {
                    case GL_NEVER:    pass = 0; break;
                    case GL_LESS:     pass = (depth <  stored); break;
                    case GL_EQUAL:    pass = (depth == stored); break;
                    case GL_LEQUAL:   pass = (depth <= stored); break;
                    case GL_GREATER:  pass = (depth >  stored); break;
                    case GL_NOTEQUAL: pass = (depth != stored); break;
                    case GL_GEQUAL:   pass = (depth >= stored); break;
                    case GL_ALWAYS:   pass = 1; break;
                }
                if (!pass) continue;
            }

            /* Perspective-correct varying interpolation. */
            float pcw = w0 * iw0 + w1 * iw1 + w2 * iw2;
            if (pcw == 0.0f) pcw = 1e-9f;
            float inv_pcw = 1.0f / pcw;

            /* Interpolated varyings (perspective-correct). */
            float varyings[256] = {0};
            int total = varying_count * 4;
            if (total > 256) total = 256;
            for (int i = 0; i < total; i++) {
                float v = w0 * iw0 * v0.varyings[i] +
                          w1 * iw1 * v1.varyings[i] +
                          w2 * iw2 * v2.varyings[i];
                varyings[i] = v * inv_pcw;
            }

            /* gl_FragCoord: (x, y, z, 1/w) where x/y are window coords
             * (origin bottom-left), z is depth 0..1, and 1/w is the
             * reciprocal of the interpolated clip-space w. The interpolated
             * clip-space w satisfies 1/w_interp = sum(bary_i / w_clip_i) = pcw,
             * so 1/w_interp = pcw. */
            float frag_coord[4] = {
                (float)px + 0.5f,
                (float)(g.fb.height - 1 - py) + 0.5f,
                depth,
                pcw
            };

            /* Run the fragment shader. */
            float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            int discard = glsl_run_fragment(prog->glsl, varyings, varying_count, frag_coord, color);
            if (discard) continue;

            /* Clamp color. */
            for (int i = 0; i < 4; i++) {
                if (color[i] < 0.0f) color[i] = 0.0f;
                if (color[i] > 1.0f) color[i] = 1.0f;
            }

            /* Blend. */
            if (g.blend_enabled) {
                uint8_t* dst = &g.fb.color[pix_idx * 4];
                float dst_r = dst[0] / 255.0f;
                float dst_g = dst[1] / 255.0f;
                float dst_b = dst[2] / 255.0f;
                float dst_a = dst[3] / 255.0f;

                /* Source factor. */
                float sr, sg, sb, sa;
                switch (g.blend_sfactor) {
                    case GL_ZERO:                sr = sg = sb = sa = 0.0f; break;
                    case GL_ONE:                 sr = sg = sb = sa = 1.0f; break;
                    case GL_SRC_ALPHA:           sr = sg = sb = sa = color[3]; break;
                    case GL_ONE_MINUS_SRC_ALPHA: sr = sg = sb = sa = 1.0f - color[3]; break;
                    case GL_SRC_COLOR:           sr = color[0]; sg = color[1]; sb = color[2]; sa = color[3]; break;
                    case GL_ONE_MINUS_SRC_COLOR: sr = 1-color[0]; sg = 1-color[1]; sb = 1-color[2]; sa = 1-color[3]; break;
                    case GL_DST_COLOR:           sr = dst_r; sg = dst_g; sb = dst_b; sa = dst_a; break;
                    case GL_ONE_MINUS_DST_COLOR: sr = 1-dst_r; sg = 1-dst_g; sb = 1-dst_b; sa = 1-dst_a; break;
                    case GL_DST_ALPHA:           sr = sg = sb = sa = dst_a; break;
                    case GL_ONE_MINUS_DST_ALPHA: sr = sg = sb = sa = 1.0f - dst_a; break;
                    default:                     sr = sg = sb = sa = 1.0f; break;
                }
                /* Dest factor. */
                float dr, dg, db, da;
                switch (g.blend_dfactor) {
                    case GL_ZERO:                dr = dg = db = da = 0.0f; break;
                    case GL_ONE:                 dr = dg = db = da = 1.0f; break;
                    case GL_SRC_ALPHA:           dr = dg = db = da = color[3]; break;
                    case GL_ONE_MINUS_SRC_ALPHA: dr = dg = db = da = 1.0f - color[3]; break;
                    case GL_SRC_COLOR:           dr = color[0]; dg = color[1]; db = color[2]; da = color[3]; break;
                    case GL_ONE_MINUS_SRC_COLOR: dr = 1-color[0]; dg = 1-color[1]; db = 1-color[2]; da = 1-color[3]; break;
                    case GL_DST_COLOR:           dr = dst_r; dg = dst_g; db = dst_b; da = dst_a; break;
                    case GL_ONE_MINUS_DST_COLOR: dr = 1-dst_r; dg = 1-dst_g; db = 1-dst_b; da = 1-dst_a; break;
                    case GL_DST_ALPHA:           dr = dg = db = da = dst_a; break;
                    case GL_ONE_MINUS_DST_ALPHA: dr = dg = db = da = 1.0f - dst_a; break;
                    default:                     dr = dg = db = da = 0.0f; break;
                }
                color[0] = color[0] * sr + dst_r * dr;
                color[1] = color[1] * sg + dst_g * dg;
                color[2] = color[2] * sb + dst_b * db;
                color[3] = color[3] * sa + dst_a * da;
                for (int i = 0; i < 4; i++) {
                    if (color[i] < 0.0f) color[i] = 0.0f;
                    if (color[i] > 1.0f) color[i] = 1.0f;
                }
            }

            /* Color mask + write. */
            uint8_t* dst = &g.fb.color[pix_idx * 4];
            if (g.color_mask_r) dst[0] = (uint8_t)(color[0] * 255.0f + 0.5f);
            if (g.color_mask_g) dst[1] = (uint8_t)(color[1] * 255.0f + 0.5f);
            if (g.color_mask_b) dst[2] = (uint8_t)(color[2] * 255.0f + 0.5f);
            if (g.color_mask_a) dst[3] = (uint8_t)(color[3] * 255.0f + 0.5f);

            /* Depth write. */
            if (g.depth_mask) {
                g.fb.depth[pix_idx] = depth;
            }
        }
    }
}

/* Decompose a triangle list / strip / fan into individual triangles and
 * rasterize them. */
static void draw_primitives(int mode, const std::vector<VertexOutput>& verts, int varying_count) {
    ProgramObj* prog = find_program(g.bound_program);
    if (!prog || !prog->glsl) {
        SWGL_LOGW("draw: no program bound");
        return;
    }
    bind_sampler_textures(prog);

    if (mode == GL_TRIANGLES) {
        for (size_t i = 0; i + 2 < verts.size(); i += 3) {
            /* Clip against near plane. */
            const VertexOutput* in[3] = {&verts[i], &verts[i+1], &verts[i+2]};
            std::vector<VertexOutput> tmp;
            tmp.reserve(3);
            for (int i = 0; i < 3; i++) tmp.push_back(*in[i]);
            std::vector<VertexOutput> clipped;
            clip_near(tmp.data(), 3, clipped);
            if (clipped.size() < 3) continue;
            /* Fan-triangulate the clipped polygon. */
            for (size_t j = 1; j + 1 < clipped.size(); j++) {
                raster_triangle(clipped[0], clipped[j], clipped[j+1], prog, varying_count);
            }
        }
    } else if (mode == GL_TRIANGLE_STRIP) {
        for (size_t i = 0; i + 2 < verts.size(); i++) {
            /* Alternate winding to keep the same facing. */
            const VertexOutput* in[3];
            if (i & 1) {
                in[0] = &verts[i];
                in[1] = &verts[i+2];
                in[2] = &verts[i+1];
            } else {
                in[0] = &verts[i];
                in[1] = &verts[i+1];
                in[2] = &verts[i+2];
            }
            std::vector<VertexOutput> tmp;
            tmp.reserve(3);
            for (int i = 0; i < 3; i++) tmp.push_back(*in[i]);
            std::vector<VertexOutput> clipped;
            clip_near(tmp.data(), 3, clipped);
            if (clipped.size() < 3) continue;
            for (size_t j = 1; j + 1 < clipped.size(); j++) {
                raster_triangle(clipped[0], clipped[j], clipped[j+1], prog, varying_count);
            }
        }
    } else if (mode == GL_TRIANGLE_FAN) {
        for (size_t i = 1; i + 1 < verts.size(); i++) {
            const VertexOutput* in[3] = {&verts[0], &verts[i], &verts[i+1]};
            std::vector<VertexOutput> tmp;
            tmp.reserve(3);
            for (int i = 0; i < 3; i++) tmp.push_back(*in[i]);
            std::vector<VertexOutput> clipped;
            clip_near(tmp.data(), 3, clipped);
            if (clipped.size() < 3) continue;
            for (size_t j = 1; j + 1 < clipped.size(); j++) {
                raster_triangle(clipped[0], clipped[j], clipped[j+1], prog, varying_count);
            }
        }
    } else if (mode == GL_POINTS || mode == GL_LINES ||
               mode == GL_LINE_STRIP || mode == GL_LINE_LOOP) {
        SWGL_LOGW("draw mode 0x%x not implemented (only TRIANGLES/STRIP/FAN)", mode);
    } else {
        SWGL_LOGW("draw mode 0x%x unknown", mode);
    }
}

/* ===================== Texture upload helpers ===================== */

/* Convert an arbitrary pixel format/type to RGBA8. */
static void convert_to_rgba8(const void* pixels, int width, int height,
                             int format, int type,
                             std::vector<uint8_t>& out) {
    out.assign((size_t)width * height * 4, 0);
    if (!pixels) return;
    size_t npix = (size_t)width * height;

    auto put = [&](size_t i, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        out[i*4 + 0] = r;
        out[i*4 + 1] = g;
        out[i*4 + 2] = b;
        out[i*4 + 3] = a;
    };

    if (type == GL_UNSIGNED_BYTE) {
        const uint8_t* p = (const uint8_t*)pixels;
        if (format == GL_RGBA) {
            memcpy(out.data(), p, npix * 4);
        } else if (format == GL_RGB) {
            for (size_t i = 0; i < npix; i++) {
                put(i, p[i*3], p[i*3+1], p[i*3+2], 255);
            }
        } else if (format == GL_RED || format == GL_LUMINANCE || format == GL_ALPHA) {
            int chan = (format == GL_ALPHA) ? 3 : 0;
            for (size_t i = 0; i < npix; i++) {
                uint8_t v = p[i];
                uint8_t r = (chan == 0) ? v : 0;
                uint8_t g = (chan == 0) ? v : 0;
                uint8_t b = (chan == 0) ? v : 0;
                uint8_t a = (chan == 3) ? v : 255;
                put(i, r, g, b, a);
            }
        } else if (format == GL_LUMINANCE_ALPHA) {
            for (size_t i = 0; i < npix; i++) {
                uint8_t l = p[i*2];
                uint8_t a = p[i*2+1];
                put(i, l, l, l, a);
            }
        } else if (format == GL_BGRA) {
            for (size_t i = 0; i < npix; i++) {
                put(i, p[i*4+2], p[i*4+1], p[i*4+0], p[i*4+3]);
            }
        } else {
            SWGL_LOGW("tex_image_2d: unsupported format 0x%x (UB)", format);
        }
    } else if (type == GL_UNSIGNED_SHORT_5_6_5) {
        const uint16_t* p = (const uint16_t*)pixels;
        for (size_t i = 0; i < npix; i++) {
            uint16_t v = p[i];
            uint8_t r = (v >> 11) & 0x1F; r = (r << 3) | (r >> 2);
            uint8_t g = (v >> 5)  & 0x3F; g = (g << 2) | (g >> 4);
            uint8_t b = (v)       & 0x1F; b = (b << 3) | (b >> 2);
            put(i, r, g, b, 255);
        }
    } else if (type == GL_UNSIGNED_SHORT_4_4_4_4) {
        const uint16_t* p = (const uint16_t*)pixels;
        for (size_t i = 0; i < npix; i++) {
            uint16_t v = p[i];
            uint8_t r = (v >> 12) & 0xF; r = (r << 4) | r;
            uint8_t g = (v >> 8)  & 0xF; g = (g << 4) | g;
            uint8_t b = (v >> 4)  & 0xF; b = (b << 4) | b;
            uint8_t a = (v)       & 0xF; a = (a << 4) | a;
            put(i, r, g, b, a);
        }
    } else if (type == GL_UNSIGNED_SHORT_5_5_5_1) {
        const uint16_t* p = (const uint16_t*)pixels;
        for (size_t i = 0; i < npix; i++) {
            uint16_t v = p[i];
            uint8_t r = (v >> 11) & 0x1F; r = (r << 3) | (r >> 2);
            uint8_t g = (v >> 6)  & 0x1F; g = (g << 3) | (g >> 2);
            uint8_t b = (v >> 1)  & 0x1F; b = (b << 3) | (b >> 2);
            uint8_t a = (v & 1) ? 255 : 0;
            put(i, r, g, b, a);
        }
    } else if (type == GL_FLOAT) {
        const float* p = (const float*)pixels;
        if (format == GL_RGBA) {
            for (size_t i = 0; i < npix; i++) {
                auto conv = [](float f) { return (uint8_t)(f < 0 ? 0 : (f > 1 ? 255 : f * 255.0f + 0.5f)); };
                put(i, conv(p[i*4]), conv(p[i*4+1]), conv(p[i*4+2]), conv(p[i*4+3]));
            }
        } else {
            SWGL_LOGW("tex_image_2d: unsupported format 0x%x (FLOAT)", format);
        }
    } else {
        SWGL_LOGW("tex_image_2d: unsupported type 0x%x", type);
    }
}

}  // namespace

/* ===================== C API ===================== */

/* Forward declarations of extra entry points defined at the bottom of this
 * file (so swgl_resolve can reference them). Not in swgl.h (internal). */
extern "C" {
const char *swgl_gl_get_string(int name);
void swgl_gl_get_integer_v(int pname, int *params);
void swgl_gl_pixel_store_i(int pname, int param);
void swgl_gl_delete_program(unsigned int program);
void swgl_gl_delete_shader(unsigned int shader);
void swgl_gl_bind_attrib_location(unsigned int program, unsigned int index, const char *name);
void swgl_gl_generate_mipmap(int target);
void swgl_gl_uniform_1iv(int location, int count, const int *value);
void swgl_gl_gen_vertex_arrays_oes(int n, unsigned int *arrays);
void swgl_gl_bind_vertex_array_oes(unsigned int array);
void swgl_gl_delete_vertex_arrays_oes(int n, const unsigned int *arrays);
void swgl_gl_get_active_attrib(unsigned int program, unsigned int index, int bufsize,
                               int *length, int *size, int *type, char *name);
void swgl_gl_get_active_uniform(unsigned int program, unsigned int index, int bufsize,
                                int *length, int *size, int *type, char *name);
void swgl_gl_release_shader_compiler(void);
void swgl_gl_shader_binary(int n, const unsigned int *shaders, int binaryformat,
                           const void *binary, int length);
void swgl_gl_finish(void);
void swgl_gl_flush(void);
void swgl_gl_polygon_offset(float factor, float units);
void swgl_gl_line_width(float width);
void swgl_gl_front_face(int mode);
void swgl_gl_cull_face(int mode);
void swgl_gl_sample_coverage(float value, unsigned char invert);
void swgl_gl_depth_rangef(float n, float f);
void swgl_gl_hint(int target, int mode);
void swgl_gl_disable(int cap);
void swgl_gl_blend_func(int sfactor, int dfactor);
void swgl_gl_depth_func(int func);
void swgl_gl_depth_mask(unsigned char flag);
void swgl_gl_color_mask(unsigned char r, unsigned char g_, unsigned char b, unsigned char a);
unsigned int swgl_gl_create_shader(int type);
void swgl_gl_shader_source(unsigned int shader, int count, const char *const*string, const int *length);
void swgl_gl_compile_shader(unsigned int shader);
void swgl_gl_get_shader_iv(unsigned int shader, int pname, int *params);
void swgl_gl_get_shader_info_log(unsigned int shader, int max_len, int *len, char *info_log);
unsigned int swgl_gl_create_program(void);
void swgl_gl_attach_shader(unsigned int program, unsigned int shader);
void swgl_gl_link_program(unsigned int program);
void swgl_gl_get_program_iv(unsigned int program, int pname, int *params);
}  /* end forward decls */

extern "C" {

int swgl_init(void) {
    if (g.inited) return 0;
    g.inited = 1;
    SWGL_LOGI("swgl_init: software GLES 2.0 ready");
    /* Default framebuffer size until swgl_attach_output is called. */
    fb_resize(720, 1280);
    return 0;
}

void swgl_shutdown(void) {
    g.buffers.clear();
    g.textures.clear();
    g.shaders.clear();
    for (auto& p : g.programs) {
        if (p.glsl) glsl_destroy(p.glsl);
    }
    g.programs.clear();
    g.fb.color.clear();
    g.fb.depth.clear();
    g.inited = 0;
}

void *swgl_resolve(const char *name) {
    if (!name) return nullptr;
    /* EGL */
    if (strcmp(name, "eglGetDisplay") == 0)         return (void*)swgl_egl_get_display;
    if (strcmp(name, "eglInitialize") == 0)         return (void*)swgl_egl_initialize;
    if (strcmp(name, "eglChooseConfig") == 0)       return (void*)swgl_egl_choose_config;
    if (strcmp(name, "eglCreateWindowSurface") == 0) return (void*)swgl_egl_create_window_surface;
    if (strcmp(name, "eglCreateContext") == 0)      return (void*)swgl_egl_create_context;
    if (strcmp(name, "eglMakeCurrent") == 0)        return (void*)swgl_egl_make_current;
    if (strcmp(name, "eglSwapBuffers") == 0)        return (void*)swgl_egl_swap_buffers;
    if (strcmp(name, "eglDestroySurface") == 0)     return (void*)swgl_egl_destroy_surface;
    if (strcmp(name, "eglDestroyContext") == 0)     return (void*)swgl_egl_destroy_context;
    if (strcmp(name, "eglTerminate") == 0)          return (void*)swgl_egl_terminate;
    if (strcmp(name, "eglGetError") == 0)           return (void*)swgl_gl_get_error;
    /* GL */
    if (strcmp(name, "glClearColor") == 0)          return (void*)swgl_gl_clear_color;
    if (strcmp(name, "glClear") == 0)               return (void*)swgl_gl_clear;
    if (strcmp(name, "glViewport") == 0)            return (void*)swgl_gl_viewport;
    if (strcmp(name, "glScissor") == 0)             return (void*)swgl_gl_scissor;
    if (strcmp(name, "glEnable") == 0)              return (void*)swgl_gl_enable;
    if (strcmp(name, "glDisable") == 0)             return (void*)swgl_gl_disable;
    if (strcmp(name, "glBlendFunc") == 0)           return (void*)swgl_gl_blend_func;
    if (strcmp(name, "glDepthFunc") == 0)           return (void*)swgl_gl_depth_func;
    if (strcmp(name, "glDepthMask") == 0)           return (void*)swgl_gl_depth_mask;
    if (strcmp(name, "glColorMask") == 0)           return (void*)swgl_gl_color_mask;
    if (strcmp(name, "glCreateShader") == 0)        return (void*)swgl_gl_create_shader;
    if (strcmp(name, "glShaderSource") == 0)        return (void*)swgl_gl_shader_source;
    if (strcmp(name, "glCompileShader") == 0)       return (void*)swgl_gl_compile_shader;
    if (strcmp(name, "glGetShaderiv") == 0)         return (void*)swgl_gl_get_shader_iv;
    if (strcmp(name, "glGetShaderInfoLog") == 0)    return (void*)swgl_gl_get_shader_info_log;
    if (strcmp(name, "glCreateProgram") == 0)       return (void*)swgl_gl_create_program;
    if (strcmp(name, "glAttachShader") == 0)        return (void*)swgl_gl_attach_shader;
    if (strcmp(name, "glLinkProgram") == 0)         return (void*)swgl_gl_link_program;
    if (strcmp(name, "glGetProgramiv") == 0)        return (void*)swgl_gl_get_program_iv;
    if (strcmp(name, "glGetProgramInfoLog") == 0)   return (void*)swgl_gl_get_program_info_log;
    if (strcmp(name, "glUseProgram") == 0)          return (void*)swgl_gl_use_program;
    if (strcmp(name, "glGetAttribLocation") == 0)   return (void*)swgl_gl_get_attrib_location;
    if (strcmp(name, "glGetUniformLocation") == 0)  return (void*)swgl_gl_get_uniform_location;
    if (strcmp(name, "glUniform1i") == 0)           return (void*)swgl_gl_uniform_1i;
    if (strcmp(name, "glUniform1f") == 0)           return (void*)swgl_gl_uniform_1f;
    if (strcmp(name, "glUniform2f") == 0)           return (void*)swgl_gl_uniform_2f;
    if (strcmp(name, "glUniform3f") == 0)           return (void*)swgl_gl_uniform_3f;
    if (strcmp(name, "glUniform4f") == 0)           return (void*)swgl_gl_uniform_4f;
    if (strcmp(name, "glUniformMatrix4fv") == 0)    return (void*)swgl_gl_uniform_matrix_4fv;
    if (strcmp(name, "glUniform1fv") == 0)          return (void*)swgl_gl_uniform_1fv;
    if (strcmp(name, "glUniform2fv") == 0)          return (void*)swgl_gl_uniform_2fv;
    if (strcmp(name, "glUniform3fv") == 0)          return (void*)swgl_gl_uniform_3fv;
    if (strcmp(name, "glUniform4fv") == 0)          return (void*)swgl_gl_uniform_4fv;
    if (strcmp(name, "glGenBuffers") == 0)          return (void*)swgl_gl_gen_buffers;
    if (strcmp(name, "glBindBuffer") == 0)          return (void*)swgl_gl_bind_buffer;
    if (strcmp(name, "glBufferData") == 0)          return (void*)swgl_gl_buffer_data;
    if (strcmp(name, "glDeleteBuffers") == 0)       return (void*)swgl_gl_delete_buffers;
    if (strcmp(name, "glGenTextures") == 0)         return (void*)swgl_gl_gen_textures;
    if (strcmp(name, "glBindTexture") == 0)         return (void*)swgl_gl_bind_texture;
    if (strcmp(name, "glTexImage2D") == 0)          return (void*)swgl_gl_tex_image_2d;
    if (strcmp(name, "glTexSubImage2D") == 0)       return (void*)swgl_gl_tex_sub_image_2d;
    if (strcmp(name, "glTexParameteri") == 0)       return (void*)swgl_gl_tex_parameter_i;
    if (strcmp(name, "glActiveTexture") == 0)       return (void*)swgl_gl_active_texture;
    if (strcmp(name, "glDeleteTextures") == 0)      return (void*)swgl_gl_delete_textures;
    if (strcmp(name, "glEnableVertexAttribArray") == 0) return (void*)swgl_gl_enable_vertex_attrib_array;
    if (strcmp(name, "glDisableVertexAttribArray") == 0) return (void*)swgl_gl_disable_vertex_attrib_array;
    if (strcmp(name, "glVertexAttribPointer") == 0) return (void*)swgl_gl_vertex_attrib_pointer;
    if (strcmp(name, "glVertexAttrib4f") == 0)      return (void*)swgl_gl_vertex_attrib_4f;
    if (strcmp(name, "glDrawArrays") == 0)          return (void*)swgl_gl_draw_arrays;
    if (strcmp(name, "glDrawElements") == 0)        return (void*)swgl_gl_draw_elements;
    if (strcmp(name, "glGetError") == 0)            return (void*)swgl_gl_get_error;
    /* Extra commonly-aliased entry points (not in swgl.h). */
    if (strcmp(name, "glGetString") == 0)           return (void*)swgl_gl_get_string;
    if (strcmp(name, "glGetIntegerv") == 0)         return (void*)swgl_gl_get_integer_v;
    if (strcmp(name, "glPixelStorei") == 0)         return (void*)swgl_gl_pixel_store_i;
    if (strcmp(name, "glDeleteProgram") == 0)       return (void*)swgl_gl_delete_program;
    if (strcmp(name, "glDeleteShader") == 0)        return (void*)swgl_gl_delete_shader;
    if (strcmp(name, "glBindAttribLocation") == 0)  return (void*)swgl_gl_bind_attrib_location;
    if (strcmp(name, "glGenerateMipmap") == 0)      return (void*)swgl_gl_generate_mipmap;
    if (strcmp(name, "glUniform1iv") == 0)          return (void*)swgl_gl_uniform_1iv;
    if (strcmp(name, "glGenVertexArraysOES") == 0)  return (void*)swgl_gl_gen_vertex_arrays_oes;
    if (strcmp(name, "glBindVertexArrayOES") == 0)  return (void*)swgl_gl_bind_vertex_array_oes;
    if (strcmp(name, "glDeleteVertexArraysOES") == 0) return (void*)swgl_gl_delete_vertex_arrays_oes;
    if (strcmp(name, "glGetActiveAttrib") == 0)     return (void*)swgl_gl_get_active_attrib;
    if (strcmp(name, "glGetActiveUniform") == 0)    return (void*)swgl_gl_get_active_uniform;
    if (strcmp(name, "glReleaseShaderCompiler") == 0) return (void*)swgl_gl_release_shader_compiler;
    if (strcmp(name, "glShaderBinary") == 0)        return (void*)swgl_gl_shader_binary;
    if (strcmp(name, "glFinish") == 0)              return (void*)swgl_gl_finish;
    if (strcmp(name, "glFlush") == 0)               return (void*)swgl_gl_flush;
    if (strcmp(name, "glPolygonOffset") == 0)       return (void*)swgl_gl_polygon_offset;
    if (strcmp(name, "glLineWidth") == 0)           return (void*)swgl_gl_line_width;
    if (strcmp(name, "glFrontFace") == 0)           return (void*)swgl_gl_front_face;
    if (strcmp(name, "glCullFace") == 0)            return (void*)swgl_gl_cull_face;
    if (strcmp(name, "glSampleCoverage") == 0)      return (void*)swgl_gl_sample_coverage;
    if (strcmp(name, "glDepthRangef") == 0)         return (void*)swgl_gl_depth_rangef;
    if (strcmp(name, "glHint") == 0)                return (void*)swgl_gl_hint;
    /* --- GLES 3 compat shim entry points --- */
    if (strcmp(name, "glGetStringi") == 0)          return (void*)swgl_gl_get_stringi;
    if (strcmp(name, "glGenVertexArrays") == 0)     return (void*)swgl_gl_gen_vertex_arrays;
    if (strcmp(name, "glBindVertexArray") == 0)     return (void*)swgl_gl_bind_vertex_array;
    if (strcmp(name, "glDeleteVertexArrays") == 0)  return (void*)swgl_gl_delete_vertex_arrays;
    if (strcmp(name, "glIsVertexArray") == 0)       return (void*)swgl_gl_is_vertex_array;
    if (strcmp(name, "glBufferSubData") == 0)       return (void*)swgl_gl_buffer_sub_data;
    if (strcmp(name, "glMapBufferRange") == 0)      return (void*)swgl_gl_map_buffer_range;
    if (strcmp(name, "glUnmapBuffer") == 0)         return (void*)swgl_gl_unmap_buffer;
    if (strcmp(name, "glFlushMappedBufferRange") == 0) return (void*)swgl_gl_flush_mapped_buffer_range;
    if (strcmp(name, "glCopyBufferSubData") == 0)   return (void*)swgl_gl_copy_buffer_sub_data;
    if (strcmp(name, "glReadPixels") == 0)          return (void*)swgl_gl_read_pixels;
    if (strcmp(name, "glGenFramebuffers") == 0)     return (void*)swgl_gl_gen_framebuffers;
    if (strcmp(name, "glBindFramebuffer") == 0)     return (void*)swgl_gl_bind_framebuffer;
    if (strcmp(name, "glCheckFramebufferStatus") == 0) return (void*)swgl_gl_check_framebuffer_status;
    if (strcmp(name, "glFramebufferTexture2D") == 0) return (void*)swgl_gl_framebuffer_texture_2d;
    if (strcmp(name, "glDeleteFramebuffers") == 0)  return (void*)swgl_gl_delete_framebuffers;
    if (strcmp(name, "glGenRenderbuffers") == 0)    return (void*)swgl_gl_gen_renderbuffers;
    if (strcmp(name, "glBindRenderbuffer") == 0)    return (void*)swgl_gl_bind_renderbuffer;
    if (strcmp(name, "glRenderbufferStorage") == 0) return (void*)swgl_gl_renderbuffer_storage;
    if (strcmp(name, "glFramebufferRenderbuffer") == 0) return (void*)swgl_gl_framebuffer_renderbuffer;
    if (strcmp(name, "glDeleteRenderbuffers") == 0) return (void*)swgl_gl_delete_renderbuffers;
    if (strcmp(name, "glBlitFramebuffer") == 0)     return (void*)swgl_gl_blit_framebuffer;
    if (strcmp(name, "glTransformFeedbackVaryings") == 0) return (void*)swgl_gl_transform_feedback_varyings;
    if (strcmp(name, "glBeginTransformFeedback") == 0) return (void*)swgl_gl_begin_transform_feedback;
    if (strcmp(name, "glEndTransformFeedback") == 0) return (void*)swgl_gl_end_transform_feedback;
    if (strcmp(name, "glPauseTransformFeedback") == 0) return (void*)swgl_gl_pause_transform_feedback;
    if (strcmp(name, "glResumeTransformFeedback") == 0) return (void*)swgl_gl_resume_transform_feedback;
    if (strcmp(name, "glBindBufferBase") == 0)      return (void*)swgl_gl_bind_buffer_base;
    if (strcmp(name, "glBindBufferRange") == 0)     return (void*)swgl_gl_bind_buffer_range;
    if (strcmp(name, "glGenQueries") == 0)          return (void*)swgl_gl_gen_queries;
    if (strcmp(name, "glDeleteQueries") == 0)       return (void*)swgl_gl_delete_queries;
    if (strcmp(name, "glBeginQuery") == 0)          return (void*)swgl_gl_begin_query;
    if (strcmp(name, "glEndQuery") == 0)            return (void*)swgl_gl_end_query;
    if (strcmp(name, "glGetQueryiv") == 0)          return (void*)swgl_gl_get_query_iv;
    if (strcmp(name, "glGetQueryObjectiv") == 0)    return (void*)swgl_gl_get_query_object_iv;
    if (strcmp(name, "glGetQueryObjectuiv") == 0)   return (void*)swgl_gl_get_query_object_uiv;
    if (strcmp(name, "glGetUniformBlockIndex") == 0) return (void*)swgl_gl_get_uniform_block_index;
    if (strcmp(name, "glGetActiveUniformBlockiv") == 0) return (void*)swgl_gl_get_active_uniform_block_iv;
    if (strcmp(name, "glUniformBlockBinding") == 0) return (void*)swgl_gl_uniform_block_binding;
    if (strcmp(name, "glGetProgramResourceIndex") == 0) return (void*)swgl_gl_get_program_resource_index;
    if (strcmp(name, "glGetProgramResourceiv") == 0) return (void*)swgl_gl_get_program_resource_iv;
    if (strcmp(name, "glGetProgramResourceName") == 0) return (void*)swgl_gl_get_program_resource_name;
    if (strcmp(name, "glShaderStorageBlockBinding") == 0) return (void*)swgl_gl_shader_storage_block_binding;
    if (strcmp(name, "glBindImageTexture") == 0)    return (void*)swgl_gl_bind_image_texture;
    if (strcmp(name, "glDispatchCompute") == 0)     return (void*)swgl_gl_dispatch_compute;
    if (strcmp(name, "glDispatchComputeIndirect") == 0) return (void*)swgl_gl_dispatch_compute_indirect;
    if (strcmp(name, "glMemoryBarrier") == 0)       return (void*)swgl_gl_memory_barrier;
    if (strcmp(name, "glBarrier") == 0)             return (void*)swgl_gl_barrier;
    if (strcmp(name, "glUniform1ui") == 0)          return (void*)swgl_gl_uniform_1ui;
    if (strcmp(name, "glUniform2ui") == 0)          return (void*)swgl_gl_uniform_2ui;
    if (strcmp(name, "glUniform3ui") == 0)          return (void*)swgl_gl_uniform_3ui;
    if (strcmp(name, "glUniform4ui") == 0)          return (void*)swgl_gl_uniform_4ui;
    if (strcmp(name, "glUniform1uiv") == 0)         return (void*)swgl_gl_uniform_1uiv;
    if (strcmp(name, "glUniform2uiv") == 0)         return (void*)swgl_gl_uniform_2uiv;
    if (strcmp(name, "glUniform3uiv") == 0)         return (void*)swgl_gl_uniform_3uiv;
    if (strcmp(name, "glUniform4uiv") == 0)         return (void*)swgl_gl_uniform_4uiv;
    if (strcmp(name, "glUniformMatrix2fv") == 0)    return (void*)swgl_gl_uniform_matrix_2fv;
    if (strcmp(name, "glUniformMatrix3fv") == 0)    return (void*)swgl_gl_uniform_matrix_3fv;
    if (strcmp(name, "glUniformMatrix2x3fv") == 0)  return (void*)swgl_gl_uniform_matrix_2x3fv;
    if (strcmp(name, "glUniformMatrix3x2fv") == 0)  return (void*)swgl_gl_uniform_matrix_3x2fv;
    if (strcmp(name, "glUniformMatrix2x4fv") == 0)  return (void*)swgl_gl_uniform_matrix_2x4fv;
    if (strcmp(name, "glUniformMatrix4x2fv") == 0)  return (void*)swgl_gl_uniform_matrix_4x2fv;
    if (strcmp(name, "glUniformMatrix3x4fv") == 0)  return (void*)swgl_gl_uniform_matrix_3x4fv;
    if (strcmp(name, "glUniformMatrix4x3fv") == 0)  return (void*)swgl_gl_uniform_matrix_4x3fv;
    if (strcmp(name, "glVertexAttribIPointer") == 0) return (void*)swgl_gl_vertex_attrib_i_pointer;
    if (strcmp(name, "glVertexAttribI4i") == 0)     return (void*)swgl_gl_vertex_attrib_i4i;
    if (strcmp(name, "glVertexAttribI4ui") == 0)    return (void*)swgl_gl_vertex_attrib_i4ui;
    if (strcmp(name, "glVertexAttribDivisor") == 0) return (void*)swgl_gl_vertex_attrib_divisor;
    if (strcmp(name, "glDrawArraysInstanced") == 0) return (void*)swgl_gl_draw_arrays_instanced;
    if (strcmp(name, "glDrawElementsInstanced") == 0) return (void*)swgl_gl_draw_elements_instanced;
    if (strcmp(name, "glDrawRangeElements") == 0)   return (void*)swgl_gl_draw_range_elements;
    if (strcmp(name, "glTexStorage2D") == 0)        return (void*)swgl_gl_tex_storage_2d;
    if (strcmp(name, "glTexStorage3D") == 0)        return (void*)swgl_gl_tex_storage_3d;
    if (strcmp(name, "glTexSubImage3D") == 0)       return (void*)swgl_gl_tex_sub_image_3d;
    if (strcmp(name, "glCompressedTexImage2D") == 0) return (void*)swgl_gl_compressed_tex_image_2d;
    if (strcmp(name, "glCompressedTexSubImage2D") == 0) return (void*)swgl_gl_compressed_tex_sub_image_2d;
    if (strcmp(name, "glCopyTexImage2D") == 0)      return (void*)swgl_gl_copy_tex_image_2d;
    if (strcmp(name, "glCopyTexSubImage2D") == 0)   return (void*)swgl_gl_copy_tex_sub_image_2d;
    if (strcmp(name, "glTexImage3D") == 0)          return (void*)swgl_gl_tex_image_3d;
    if (strcmp(name, "glTexParameterf") == 0)       return (void*)swgl_gl_tex_parameter_f;
    if (strcmp(name, "glTexParameterfv") == 0)      return (void*)swgl_gl_tex_parameter_fv;
    if (strcmp(name, "glFenceSync") == 0)           return (void*)swgl_gl_fence_sync;
    if (strcmp(name, "glIsSync") == 0)              return (void*)swgl_gl_is_sync;
    if (strcmp(name, "glDeleteSync") == 0)          return (void*)swgl_gl_delete_sync;
    if (strcmp(name, "glClientWaitSync") == 0)      return (void*)swgl_gl_client_wait_sync;
    if (strcmp(name, "glWaitSync") == 0)            return (void*)swgl_gl_wait_sync;
    if (strcmp(name, "glMultiDrawArrays") == 0)     return (void*)swgl_gl_multi_draw_arrays;
    if (strcmp(name, "glMultiDrawElements") == 0)   return (void*)swgl_gl_multi_draw_elements;
    if (strcmp(name, "glGenSamplers") == 0)         return (void*)swgl_gl_gen_samplers;
    if (strcmp(name, "glBindSampler") == 0)         return (void*)swgl_gl_bind_sampler;
    if (strcmp(name, "glSamplerParameteri") == 0)   return (void*)swgl_gl_sampler_parameter_i;
    if (strcmp(name, "glDeleteSamplers") == 0)      return (void*)swgl_gl_delete_samplers;
    if (strcmp(name, "glInvalidateFramebuffer") == 0) return (void*)swgl_gl_invalidate_framebuffer;
    if (strcmp(name, "glInvalidateSubFramebuffer") == 0) return (void*)swgl_gl_invalidate_subFramebuffer;
    if (strcmp(name, "glDebugMessageCallback") == 0) return (void*)swgl_gl_debug_message_callback;
    if (strcmp(name, "glPushDebugGroup") == 0)      return (void*)swgl_gl_push_debug_group;
    if (strcmp(name, "glPopDebugGroup") == 0)       return (void*)swgl_gl_pop_debug_group;
    if (strcmp(name, "glObjectLabel") == 0)         return (void*)swgl_gl_object_label;
    /* Other commonly-aliased names (glGetString etc.) — return NULL. */
    return nullptr;
}

/* ===== EGL ===== */

void *swgl_egl_get_display(void *native_display) {
    (void)native_display;
    return (void*)&g_display_singleton;
}

int swgl_egl_initialize(void *dpy, int *major, int *minor) {
    if (dpy != (void*)&g_display_singleton) {
        SWGL_LOGW("egl_initialize: bad display");
        return 0;
    }
    if (major) *major = 1;
    if (minor) *minor = 4;
    return 1;
}

int swgl_egl_choose_config(void *dpy, const int *attrib_list,
                           void **configs, int config_size, int *num_config) {
    (void)dpy; (void)attrib_list; (void)config_size;
    if (num_config) *num_config = 1;
    if (configs && config_size >= 1) {
        configs[0] = (void*)&g_config_singleton;
    }
    return 1;
}

void *swgl_egl_create_window_surface(void *dpy, void *config,
                                     void *native_window, const int *attrib_list) {
    (void)dpy; (void)config; (void)attrib_list;
    /* The native_window is the CAMetalLayer pointer Swift passed via
     * swgl_attach_output. We just record it for completeness. */
    g.attached_layer = native_window;
    return (void*)&g_surface_singleton;
}

void *swgl_egl_create_context(void *dpy, void *config,
                              void *share_ctx, const int *attrib_list) {
    (void)dpy; (void)config; (void)share_ctx; (void)attrib_list;
    return (void*)&g_context_singleton;
}

int swgl_egl_make_current(void *dpy, void *draw, void *read, void *ctx) {
    g.current_display = dpy;
    g.current_draw_surface = draw;
    g.current_read_surface = read;
    g.current_context = ctx;
    return 1;
}

int swgl_egl_swap_buffers(void *dpy, void *surface) {
    (void)dpy; (void)surface;
    /* Mark framebuffer as "dirty" — Swift polls swgl_get_framebuffer() after
     * each swap. We don't clear the framebuffer; the app calls glClear for
     * that. */
    SWGL_LOGD("swap_buffers: framebuffer marked dirty (%dx%d)",
              g.fb.width, g.fb.height);
    return 1;
}

int swgl_egl_destroy_surface(void *dpy, void *surface) {
    (void)dpy; (void)surface;
    return 1;
}

int swgl_egl_destroy_context(void *dpy, void *ctx) {
    (void)dpy; (void)ctx;
    return 1;
}

int swgl_egl_terminate(void *dpy) {
    (void)dpy;
    return 1;
}

int swgl_attach_output(void *cametal_layer_ptr, int width, int height) {
    g.attached_layer = cametal_layer_ptr;
    if (width <= 0 || height <= 0) {
        width = 720;
        height = 1280;
    }
    g.attached_w = width;
    g.attached_h = height;
    fb_resize(width, height);
    SWGL_LOGI("attach_output: layer=%p %dx%d", cametal_layer_ptr, width, height);
    return 0;
}

const void *swgl_get_framebuffer(void) {
    return g.fb.color.data();
}
int swgl_get_framebuffer_width(void)  { return g.fb.width; }
int swgl_get_framebuffer_height(void) { return g.fb.height; }

/* ===== GL state ===== */

void swgl_gl_clear_color(float r, float g_, float b, float a) {
    g.clear_color[0] = r;
    g.clear_color[1] = g_;
    g.clear_color[2] = b;
    g.clear_color[3] = a;
}

void swgl_gl_clear(int mask) {
    if (mask & GL_COLOR_BUFFER_BIT) {
        uint8_t r = (uint8_t)(g.clear_color[0] * 255.0f + 0.5f);
        uint8_t gg = (uint8_t)(g.clear_color[1] * 255.0f + 0.5f);
        uint8_t b = (uint8_t)(g.clear_color[2] * 255.0f + 0.5f);
        uint8_t a = (uint8_t)(g.clear_color[3] * 255.0f + 0.5f);
        uint32_t pixel = ((uint32_t)r) | ((uint32_t)gg << 8) |
                         ((uint32_t)b << 16) | ((uint32_t)a << 24);
        uint32_t* dst = (uint32_t*)g.fb.color.data();
        size_t npix = (size_t)g.fb.width * g.fb.height;
        for (size_t i = 0; i < npix; i++) dst[i] = pixel;
    }
    if (mask & GL_DEPTH_BUFFER_BIT) {
        std::fill(g.fb.depth.begin(), g.fb.depth.end(), 1.0f);
    }
    if (mask & GL_STENCIL_BUFFER_BIT) {
        /* No stencil buffer implemented. */
    }
}

void swgl_gl_viewport(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    g.viewport_x = x;
    g.viewport_y = y;
    g.viewport_w = w;
    g.viewport_h = h;
}

void swgl_gl_scissor(int x, int y, int w, int h) {
    g.scissor_x = x;
    g.scissor_y = y;
    g.scissor_w = w;
    g.scissor_h = h;
}

void swgl_gl_enable(int cap) {
    switch (cap) {
        case GL_DEPTH_TEST:    g.depth_test_enabled = 1; break;
        case GL_BLEND:         g.blend_enabled = 1; break;
        case GL_SCISSOR_TEST:  g.scissor_enabled = 1; break;
        case GL_CULL_FACE:     g.cull_face_enabled = 1; break;
        case GL_DITHER:        g.dither_enabled = 1; break;
        default:
            SWGL_LOGW("glEnable: cap 0x%x not implemented", cap);
            break;
    }
}

void swgl_gl_disable(int cap) {
    switch (cap) {
        case GL_DEPTH_TEST:    g.depth_test_enabled = 0; break;
        case GL_BLEND:         g.blend_enabled = 0; break;
        case GL_SCISSOR_TEST:  g.scissor_enabled = 0; break;
        case GL_CULL_FACE:     g.cull_face_enabled = 0; break;
        case GL_DITHER:        g.dither_enabled = 0; break;
        default:
            SWGL_LOGW("glDisable: cap 0x%x not implemented", cap);
            break;
    }
}

void swgl_gl_blend_func(int sfactor, int dfactor) {
    g.blend_sfactor = sfactor;
    g.blend_dfactor = dfactor;
}

void swgl_gl_depth_func(int func) {
    g.depth_func = func;
}

void swgl_gl_depth_mask(unsigned char flag) {
    g.depth_mask = flag ? 1 : 0;
}

void swgl_gl_color_mask(unsigned char r, unsigned char g_, unsigned char b, unsigned char a) {
    g.color_mask_r = r ? 1 : 0;
    g.color_mask_g = g_ ? 1 : 0;
    g.color_mask_b = b ? 1 : 0;
    g.color_mask_a = a ? 1 : 0;
}

/* ===== Shaders / Programs ===== */

unsigned int swgl_gl_create_shader(int type) {
    if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER) {
        set_error(GL_INVALID_ENUM);
        return 0;
    }
    Shader s;
    s.id = alloc_id();
    s.type = type;
    g.shaders.push_back(s);
    return s.id;
}

void swgl_gl_shader_source(unsigned int shader, int count, const char *const*string, const int *length) {
    Shader* s = find_shader(shader);
    if (!s) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    s->source.clear();
    for (int i = 0; i < count; i++) {
        if (length && length[i] >= 0) {
            s->source.append(string[i], length[i]);
        } else {
            s->source.append(string[i]);
        }
    }
}

void swgl_gl_compile_shader(unsigned int shader) {
    Shader* s = find_shader(shader);
    if (!s) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    /* Compile by calling glsl_compile with the actual source and an empty
     * other-shader source. We then check the appropriate compile status. */
    const char* vs_src = s->type == GL_VERTEX_SHADER ? s->source.c_str() : "";
    const char* fs_src = s->type == GL_FRAGMENT_SHADER ? s->source.c_str() : "";
    glsl_program_t* p = glsl_compile(vs_src, fs_src);
    if (s->type == GL_VERTEX_SHADER) {
        s->compile_status = glsl_get_vs_compile_status(p);
        s->info_log = glsl_get_vs_info_log(p);
    } else {
        s->compile_status = glsl_get_fs_compile_status(p);
        s->info_log = glsl_get_fs_info_log(p);
    }
    if (!s->compile_status) {
        SWGL_LOGW("compile_shader %u failed: %s", shader, s->info_log.c_str());
    }
    glsl_destroy(p);
}

void swgl_gl_get_shader_iv(unsigned int shader, int pname, int *params) {
    Shader* s = find_shader(shader);
    if (!s || !params) {
        if (params) *params = 0;
        return;
    }
    switch (pname) {
        case GL_COMPILE_STATUS:
            *params = s->compile_status;
            break;
        case GL_INFO_LOG_LENGTH:
            *params = (int)s->info_log.size() + 1;
            break;
        default:
            *params = 0;
            SWGL_LOGW("get_shader_iv: pname 0x%x not implemented", pname);
            break;
    }
}

void swgl_gl_get_shader_info_log(unsigned int shader, int max_len, int *len, char *info_log) {
    Shader* s = find_shader(shader);
    if (!s || !info_log || max_len <= 0) {
        if (len) *len = 0;
        return;
    }
    int n = (int)s->info_log.size();
    if (n >= max_len) n = max_len - 1;
    memcpy(info_log, s->info_log.data(), n);
    info_log[n] = 0;
    if (len) *len = n;
}

unsigned int swgl_gl_create_program(void) {
    ProgramObj p;
    p.id = alloc_id();
    g.programs.push_back(p);
    return p.id;
}

void swgl_gl_attach_shader(unsigned int program, unsigned int shader) {
    ProgramObj* p = find_program(program);
    Shader* s = find_shader(shader);
    if (!p || !s) {
        set_error(GL_INVALID_OPERATION);
        return;
    }
    if (s->type == GL_VERTEX_SHADER)   p->vs_id = shader;
    else                              p->fs_id = shader;
}

void swgl_gl_link_program(unsigned int program) {
    ProgramObj* p = find_program(program);
    if (!p) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    Shader* vs = find_shader(p->vs_id);
    Shader* fs = find_shader(p->fs_id);
    if (!vs || !fs) {
        p->link_status = 0;
        p->info_log = "missing vertex or fragment shader";
        return;
    }
    if (p->glsl) {
        glsl_destroy(p->glsl);
        p->glsl = nullptr;
    }
    p->glsl = glsl_compile(vs->source.c_str(), fs->source.c_str());
    char logbuf[1024];
    int rc = glsl_link(p->glsl, logbuf, sizeof(logbuf));
    if (rc != 0) {
        p->link_status = 0;
        p->info_log = logbuf;
        SWGL_LOGW("link_program %u failed: %s", program, p->info_log.c_str());
        return;
    }
    p->link_status = 1;
    p->info_log.clear();
    SWGL_LOGI("link_program %u ok", program);
}

void swgl_gl_get_program_iv(unsigned int program, int pname, int *params) {
    ProgramObj* p = find_program(program);
    if (!p || !params) {
        if (params) *params = 0;
        return;
    }
    switch (pname) {
        case GL_LINK_STATUS:
            *params = p->link_status;
            break;
        case GL_INFO_LOG_LENGTH:
            *params = (int)p->info_log.size() + 1;
            break;
        default:
            *params = 0;
            SWGL_LOGW("get_program_iv: pname 0x%x not implemented", pname);
            break;
    }
}

void swgl_gl_get_program_info_log(unsigned int program, int max_len, int *len, char *info_log) {
    ProgramObj* p = find_program(program);
    if (!p || !info_log || max_len <= 0) {
        if (len) *len = 0;
        return;
    }
    int n = (int)p->info_log.size();
    if (n >= max_len) n = max_len - 1;
    memcpy(info_log, p->info_log.data(), n);
    info_log[n] = 0;
    if (len) *len = n;
}

void swgl_gl_use_program(unsigned int program) {
    if (program == 0) {
        g.bound_program = 0;
        return;
    }
    ProgramObj* p = find_program(program);
    if (!p) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    if (!p->link_status) {
        SWGL_LOGW("use_program %u: not linked", program);
    }
    g.bound_program = program;
}

int swgl_gl_get_attrib_location(unsigned int program, const char *name) {
    ProgramObj* p = find_program(program);
    if (!p || !p->glsl || !p->link_status) return -1;
    return glsl_get_attrib_location(p->glsl, name);
}

int swgl_gl_get_uniform_location(unsigned int program, const char *name) {
    ProgramObj* p = find_program(program);
    if (!p || !p->glsl || !p->link_status) return -1;
    /* GLSL uniform names can have a "[0]" suffix for arrays; strip it. */
    std::string n = name;
    size_t bracket = n.find('[');
    if (bracket != std::string::npos) n = n.substr(0, bracket);
    int loc = glsl_get_uniform_location(p->glsl, n.c_str());
    return loc;
}

/* ===== Uniforms ===== */

void swgl_gl_uniform_1i(int location, int v0) {
    ProgramObj* p = find_program(g.bound_program);
    if (!p || !p->glsl) return;
    if (location < 0) return;
    glsl_set_uniform(p->glsl, location, GLSL_INT, 1, &v0);
}

void swgl_gl_uniform_1f(int location, float v0) {
    ProgramObj* p = find_program(g.bound_program);
    if (!p || !p->glsl) return;
    if (location < 0) return;
    glsl_set_uniform(p->glsl, location, GLSL_FLOAT, 1, &v0);
}

void swgl_gl_uniform_2f(int location, float x, float y) {
    ProgramObj* p = find_program(g.bound_program);
    if (!p || !p->glsl) return;
    if (location < 0) return;
    float v[2] = {x, y};
    glsl_set_uniform(p->glsl, location, GLSL_VEC2, 1, v);
}

void swgl_gl_uniform_3f(int location, float x, float y, float z) {
    ProgramObj* p = find_program(g.bound_program);
    if (!p || !p->glsl) return;
    if (location < 0) return;
    float v[3] = {x, y, z};
    glsl_set_uniform(p->glsl, location, GLSL_VEC3, 1, v);
}

void swgl_gl_uniform_4f(int location, float x, float y, float z, float w) {
    ProgramObj* p = find_program(g.bound_program);
    if (!p || !p->glsl) return;
    if (location < 0) return;
    float v[4] = {x, y, z, w};
    glsl_set_uniform(p->glsl, location, GLSL_VEC4, 1, v);
}

void swgl_gl_uniform_matrix_4fv(int location, int count, unsigned char transpose, const float *value) {
    ProgramObj* p = find_program(g.bound_program);
    if (!p || !p->glsl) return;
    if (location < 0 || !value) return;
    if (transpose) {
        /* Transpose in-place into a temp buffer. */
        std::vector<float> tmp(16 * count);
        for (int c = 0; c < count; c++) {
            const float* src = value + c * 16;
            float* dst = tmp.data() + c * 16;
            for (int i = 0; i < 4; i++)
                for (int j = 0; j < 4; j++)
                    dst[i * 4 + j] = src[j * 4 + i];
        }
        glsl_set_uniform(p->glsl, location, GLSL_MAT4, count, tmp.data());
    } else {
        glsl_set_uniform(p->glsl, location, GLSL_MAT4, count, value);
    }
}

void swgl_gl_uniform_1fv(int location, int count, const float *value) {
    ProgramObj* p = find_program(g.bound_program);
    if (!p || !p->glsl) return;
    if (location < 0 || !value) return;
    glsl_set_uniform(p->glsl, location, GLSL_FLOAT, count, value);
}

void swgl_gl_uniform_2fv(int location, int count, const float *value) {
    ProgramObj* p = find_program(g.bound_program);
    if (!p || !p->glsl) return;
    if (location < 0 || !value) return;
    glsl_set_uniform(p->glsl, location, GLSL_VEC2, count, value);
}

void swgl_gl_uniform_3fv(int location, int count, const float *value) {
    ProgramObj* p = find_program(g.bound_program);
    if (!p || !p->glsl) return;
    if (location < 0 || !value) return;
    glsl_set_uniform(p->glsl, location, GLSL_VEC3, count, value);
}

void swgl_gl_uniform_4fv(int location, int count, const float *value) {
    ProgramObj* p = find_program(g.bound_program);
    if (!p || !p->glsl) return;
    if (location < 0 || !value) return;
    glsl_set_uniform(p->glsl, location, GLSL_VEC4, count, value);
}

/* ===== Buffers ===== */

void swgl_gl_gen_buffers(int n, unsigned int *buffers) {
    if (!buffers || n <= 0) return;
    for (int i = 0; i < n; i++) {
        Buffer b;
        b.id = alloc_id();
        g.buffers.push_back(b);
        buffers[i] = b.id;
    }
}

void swgl_gl_bind_buffer(int target, unsigned int buffer) {
    if (target == GL_ARRAY_BUFFER) {
        g.bound_array_buffer = buffer;
    } else if (target == GL_ELEMENT_ARRAY_BUFFER) {
        g.bound_element_array_buffer = buffer;
    } else {
        SWGL_LOGW("bind_buffer: target 0x%x not implemented", target);
    }
}

void swgl_gl_buffer_data(int target, long size, const void *data, int usage) {
    (void)usage;
    unsigned int* bound = nullptr;
    if (target == GL_ARRAY_BUFFER)             bound = &g.bound_array_buffer;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) bound = &g.bound_element_array_buffer;
    else {
        SWGL_LOGW("buffer_data: target 0x%x not implemented", target);
        return;
    }
    Buffer* b = find_buffer(*bound);
    if (!b) {
        set_error(GL_INVALID_OPERATION);
        return;
    }
    if (size < 0) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    b->data.assign((size_t)size, 0);
    if (data && size > 0) {
        memcpy(b->data.data(), data, (size_t)size);
    }
}

void swgl_gl_delete_buffers(int n, const unsigned int *buffers) {
    if (!buffers || n <= 0) return;
    for (int i = 0; i < n; i++) {
        unsigned int id = buffers[i];
        if (g.bound_array_buffer == id) g.bound_array_buffer = 0;
        if (g.bound_element_array_buffer == id) g.bound_element_array_buffer = 0;
        for (auto it = g.buffers.begin(); it != g.buffers.end(); ++it) {
            if (it->id == id) { g.buffers.erase(it); break; }
        }
    }
}

/* ===== Textures ===== */

void swgl_gl_gen_textures(int n, unsigned int *textures) {
    if (!textures || n <= 0) return;
    for (int i = 0; i < n; i++) {
        Texture t;
        t.id = alloc_id();
        g.textures.push_back(t);
        textures[i] = t.id;
    }
}

void swgl_gl_bind_texture(int target, unsigned int texture) {
    if (target != GL_TEXTURE_2D) {
        SWGL_LOGW("bind_texture: target 0x%x not implemented (only TEXTURE_2D)", target);
        return;
    }
    g.bound_texture_2d[g.active_texture_unit] = texture;
}

void swgl_gl_tex_image_2d(int target, int level, int internalformat,
                          int width, int height, int border,
                          int format, int type, const void *pixels) {
    (void)internalformat; (void)border;
    if (target != GL_TEXTURE_2D) {
        SWGL_LOGW("tex_image_2d: target 0x%x not implemented", target);
        return;
    }
    if (level != 0) {
        SWGL_LOGW("tex_image_2d: level %d not sampled (only level 0 used)", level);
        /* We accept the upload but it'll be ignored at sample time. */
    }
    Texture* t = find_texture(g.bound_texture_2d[g.active_texture_unit]);
    if (!t) {
        set_error(GL_INVALID_OPERATION);
        return;
    }
    t->width = width;
    t->height = height;
    t->internal_format = format;
    convert_to_rgba8(pixels, width, height, format, type, t->rgba);
}

void swgl_gl_tex_sub_image_2d(int target, int level, int xoffset, int yoffset,
                              int width, int height, int format, int type,
                              const void *pixels) {
    (void)level;
    if (target != GL_TEXTURE_2D) return;
    Texture* t = find_texture(g.bound_texture_2d[g.active_texture_unit]);
    if (!t) return;
    std::vector<uint8_t> sub;
    convert_to_rgba8(pixels, width, height, format, type, sub);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int dx = xoffset + x;
            int dy = yoffset + y;
            if (dx < 0 || dy < 0 || dx >= t->width || dy >= t->height) continue;
            size_t dst = ((size_t)dy * t->width + dx) * 4;
            size_t src = ((size_t)y * width + x) * 4;
            for (int c = 0; c < 4; c++) t->rgba[dst + c] = sub[src + c];
        }
    }
}

void swgl_gl_tex_parameter_i(int target, int pname, int param) {
    if (target != GL_TEXTURE_2D) return;
    Texture* t = find_texture(g.bound_texture_2d[g.active_texture_unit]);
    if (!t) return;
    switch (pname) {
        case GL_TEXTURE_MIN_FILTER:
            if (param == GL_NEAREST || param == GL_LINEAR) {
                t->min_filter = param;
            } else {
                SWGL_LOGW("tex_parameter_i: min_filter 0x%x falls back to level 0", param);
                t->min_filter = GL_LINEAR;
            }
            break;
        case GL_TEXTURE_MAG_FILTER:
            t->mag_filter = (param == GL_NEAREST) ? GL_NEAREST : GL_LINEAR;
            break;
        case GL_TEXTURE_WRAP_S:
            t->wrap_s = (param == GL_REPEAT) ? GL_REPEAT : GL_CLAMP_TO_EDGE;
            break;
        case GL_TEXTURE_WRAP_T:
            t->wrap_t = (param == GL_REPEAT) ? GL_REPEAT : GL_CLAMP_TO_EDGE;
            break;
        default:
            SWGL_LOGW("tex_parameter_i: pname 0x%x not implemented", pname);
            break;
    }
}

void swgl_gl_active_texture(int unit) {
    if (unit < GL_TEXTURE0 || unit > GL_TEXTURE0 + 3) {
        SWGL_LOGW("active_texture: unit 0x%x out of range (only 0..3)", unit);
        return;
    }
    g.active_texture_unit = unit - GL_TEXTURE0;
}

void swgl_gl_delete_textures(int n, const unsigned int *textures) {
    if (!textures || n <= 0) return;
    for (int i = 0; i < n; i++) {
        unsigned int id = textures[i];
        for (int u = 0; u < 4; u++) {
            if (g.bound_texture_2d[u] == id) g.bound_texture_2d[u] = 0;
        }
        for (auto it = g.textures.begin(); it != g.textures.end(); ++it) {
            if (it->id == id) { g.textures.erase(it); break; }
        }
    }
}

/* ===== Vertex attribs ===== */

void swgl_gl_enable_vertex_attrib_array(unsigned int index) {
    if (index >= MAX_VERTEX_ATTRIBS) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    g.vertex_attribs[index].enabled = 1;
}

void swgl_gl_disable_vertex_attrib_array(unsigned int index) {
    if (index >= MAX_VERTEX_ATTRIBS) return;
    g.vertex_attribs[index].enabled = 0;
}

void swgl_gl_vertex_attrib_pointer(unsigned int index, int size, int type,
                                   unsigned char normalized, int stride, const void *pointer) {
    if (index >= MAX_VERTEX_ATTRIBS) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    if (size < 1 || size > 4) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    VertexAttrib& va = g.vertex_attribs[index];
    va.size = size;
    va.type = type;
    va.normalized = normalized ? 1 : 0;
    va.stride = stride;
    va.pointer = pointer;
}

void swgl_gl_vertex_attrib_4f(unsigned int index, float x, float y, float z, float w) {
    if (index >= MAX_VERTEX_ATTRIBS) return;
    VertexAttrib& va = g.vertex_attribs[index];
    va.generic[0] = x;
    va.generic[1] = y;
    va.generic[2] = z;
    va.generic[3] = w;
}

/* ===== Drawing ===== */

void swgl_gl_draw_arrays(int mode, int first, int count) {
    if (count <= 0) return;
    ProgramObj* prog = find_program(g.bound_program);
    if (!prog || !prog->glsl || !prog->link_status) {
        SWGL_LOGW("draw_arrays: no program bound or not linked");
        return;
    }
    int varying_count = glsl_get_varying_count(prog->glsl);

    std::vector<VertexOutput> verts((size_t)count);
    for (int i = 0; i < count; i++) {
        run_vertex(prog, first + i, verts[i], varying_count);
    }
    draw_primitives(mode, verts, varying_count);
}

void swgl_gl_draw_elements(int mode, int count, int type, const void *indices) {
    if (count <= 0) return;
    ProgramObj* prog = find_program(g.bound_program);
    if (!prog || !prog->glsl || !prog->link_status) {
        SWGL_LOGW("draw_elements: no program bound or not linked");
        return;
    }
    int varying_count = glsl_get_varying_count(prog->glsl);

    /* Get the index data. */
    const uint8_t* idx_bytes = nullptr;
    if (g.bound_element_array_buffer != 0) {
        Buffer* buf = find_buffer(g.bound_element_array_buffer);
        if (!buf) return;
        size_t offset = (size_t)indices;
        if (offset >= buf->data.size()) return;
        idx_bytes = buf->data.data() + offset;
    } else {
        idx_bytes = (const uint8_t*)indices;
    }
    if (!idx_bytes) return;

    std::vector<VertexOutput> verts((size_t)count);
    for (int i = 0; i < count; i++) {
        int vertex_index = 0;
        switch (type) {
            case GL_UNSIGNED_BYTE:  vertex_index = ((const uint8_t*)idx_bytes)[i]; break;
            case GL_UNSIGNED_SHORT: vertex_index = ((const uint16_t*)idx_bytes)[i]; break;
            case GL_UNSIGNED_INT:   vertex_index = ((const uint32_t*)idx_bytes)[i]; break;
            default:
                set_error(GL_INVALID_ENUM);
                return;
        }
        run_vertex(prog, vertex_index, verts[i], varying_count);
    }
    draw_primitives(mode, verts, varying_count);
}

int swgl_gl_get_error(void) {
    int e = g.error;
    g.error = GL_NO_ERROR;
    return e;
}

/* ===== Extra commonly-aliased GL entry points (not in swgl.h but
 *       exported via swgl_resolve so trivial APKs don't crash on lookup). ===== */

#define GL_VENDOR                   0x1F00
#define GL_RENDERER                 0x1F01
#define GL_VERSION                  0x1F02
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_EXTENSIONS               0x1F03

#define GL_MAX_TEXTURE_SIZE         0x0D33
#define GL_MAX_TEXTURE_IMAGE_UNITS  0x8872
#define GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS 0x8B4C
#define GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS 0x8B4D
#define GL_MAX_VARYING_VECTORS      0x8DFC
#define GL_MAX_VERTEX_UNIFORM_VECTORS 0x8DFB
#define GL_MAX_FRAGMENT_UNIFORM_VECTORS 0x8DFD
#define GL_MAX_RENDERBUFFER_SIZE    0x84E8
#define GL_NUM_SHADER_BINARY_FORMATS 0x8DF9
#define GL_SHADER_BINARY_FORMATS    0x8DF8
#define GL_UNPACK_ALIGNMENT         0x0CF5
#define GL_PACK_ALIGNMENT           0x0D05

/* GLES 3 compat: we advertise 3.0 in the version string so apps that gate
 * on GLES 3 at startup (e.g. Unity with GLES3 player setting) boot. Real GLES
 * 3 rendering needs ANGLE; swgl provides GLES 2 rendering + no-op-success
 * stubs for GLES 3-only entry points (glTransformFeedbackVaryings, glBeginQuery,
 * glDispatchCompute, etc.) so the app doesn't crash. See docs/LIMITATIONS.md. */
const char *swgl_gl_get_string(int name) {
    switch (name) {
        case GL_VENDOR:                   return "APKLive";
        case GL_RENDERER:                 return "APKLive Software GLES 3.0 (compat shim over 2.0)";
        case GL_VERSION:                  return "OpenGL ES 3.0 APKLive-swgl-1.0";
        case GL_SHADING_LANGUAGE_VERSION: return "OpenGL ES GLSL ES 3.00";
        case GL_EXTENSIONS:               return "GL_OES_vertex_array_object GL_OES_texture_npot GL_EXT_texture_format_BGRA8888";
        default:
            SWGL_LOGW("get_string: name 0x%x not implemented", name);
            return "";
    }
}

/* GLES 3: glGetStringi — query extensions by index. We advertise 3 extensions
 * (see GL_EXTENSIONS above). */
const char *swgl_gl_get_stringi(int name, unsigned int index) {
    if (name == GL_EXTENSIONS) {
        switch (index) {
            case 0: return "GL_OES_vertex_array_object";
            case 1: return "GL_OES_texture_npot";
            case 2: return "GL_EXT_texture_format_BGRA8888";
            default: return "";
        }
    }
    return "";
}

void swgl_gl_get_integer_v(int pname, int *params) {
    if (!params) return;
    switch (pname) {
        case GL_MAX_TEXTURE_SIZE:                *params = 4096; break;
        case GL_MAX_TEXTURE_IMAGE_UNITS:         *params = 4; break;
        case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS:  *params = 4; break;
        case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS:*params = 4; break;
        case GL_MAX_VARYING_VECTORS:             *params = 16; break;
        case GL_MAX_VERTEX_UNIFORM_VECTORS:      *params = 256; break;
        case GL_MAX_FRAGMENT_UNIFORM_VECTORS:    *params = 256; break;
        case GL_MAX_RENDERBUFFER_SIZE:           *params = 4096; break;
        case GL_NUM_SHADER_BINARY_FORMATS:       *params = 0; break;
        case GL_UNPACK_ALIGNMENT:                *params = 4; break;
        case GL_PACK_ALIGNMENT:                  *params = 4; break;
        case GL_MAX_VERTEX_ATTRIBS:              *params = MAX_VERTEX_ATTRIBS; break;
        /* GLES 3 compat: advertise 3.0 + caps that make version-gating apps happy. */
        case 0x821D /* GL_MAJOR_VERSION */:       *params = 3; break;
        case 0x821E /* GL_MINOR_VERSION */:       *params = 0; break;
        case 0x821F /* GL_NUM_EXTENSIONS */:      *params = 3; break;
        case 0x8B49 /* GL_MAX_VERTEX_UNIFORM_BLOCKS */:       *params = 12; break;
        case 0x8B4A /* GL_MAX_FRAGMENT_UNIFORM_BLOCKS */:    *params = 12; break;
        case 0x8B4B /* GL_MAX_COMBINED_UNIFORM_BLOCKS */:    *params = 24; break;
        case 0x8A2A /* GL_MAX_UNIFORM_BLOCK_SIZE */:         *params = 16384; break;
        case 0x906D /* GL_MAX_COMBINED_SHADER_OUTPUT_RESOURCES */: *params = 8; break;
        case 0x8DFB /* GL_MAX_VERTEX_UNIFORM_VECTORS */:     *params = 256; break;
        case 0x92D3 /* GL_MAX_COMPUTE_WORK_GROUP_COUNT */:  *params = 1; break;
        case 0x91BE /* GL_MAX_COMPUTE_WORK_GROUP_SIZE */:   *params = 1; break;
        default:
            *params = 0;
            SWGL_LOGW("get_integerv: pname 0x%x not implemented", pname);
            break;
    }
}

void swgl_gl_pixel_store_i(int pname, int param) {
    (void)pname; (void)param;
    /* Accepted, ignored (we always use tight packing). */
}

void swgl_gl_delete_program(unsigned int program) {
    for (auto it = g.programs.begin(); it != g.programs.end(); ++it) {
        if (it->id == program) {
            if (it->glsl) glsl_destroy(it->glsl);
            if (g.bound_program == program) g.bound_program = 0;
            g.programs.erase(it);
            return;
        }
    }
}

void swgl_gl_delete_shader(unsigned int shader) {
    for (auto it = g.shaders.begin(); it != g.shaders.end(); ++it) {
        if (it->id == shader) { g.shaders.erase(it); return; }
    }
}

void swgl_gl_bind_attrib_location(unsigned int program, unsigned int index, const char *name) {
    /* STUB: explicit attrib location binding is accepted but ignored. The
     * natural location (declaration order in the shader source) is used by
     * glGetAttribLocation. Apps that rely on fixed locations via
     * glBindAttribLocation may bind to the wrong slot. The trivial test APK
     * (which uses glGetAttribLocation) is unaffected. */
    (void)program; (void)index; (void)name;
    SWGL_LOGW("bind_attrib_location: explicit locations not implemented (using natural order)");
}

void swgl_gl_generate_mipmap(int target) {
    (void)target;
    /* No-op: we only sample level 0. */
}

void swgl_gl_uniform_1iv(int location, int count, const int *value) {
    ProgramObj* p = find_program(g.bound_program);
    if (!p || !p->glsl) return;
    if (location < 0 || !value) return;
    glsl_set_uniform(p->glsl, location, GLSL_INT, count, value);
}

/* VAO stubs (GLES2 doesn't have VAOs but some apps alias OES extensions). */
void swgl_gl_gen_vertex_arrays_oes(int n, unsigned int *arrays) {
    if (!arrays || n <= 0) return;
    /* VAOs are no-ops for us; just hand out fake IDs. */
    for (int i = 0; i < n; i++) arrays[i] = alloc_id();
}
void swgl_gl_bind_vertex_array_oes(unsigned int array) { (void)array; }
void swgl_gl_delete_vertex_arrays_oes(int n, const unsigned int *arrays) { (void)n; (void)arrays; }

void swgl_gl_get_active_attrib(unsigned int program, unsigned int index, int bufsize,
                               int *length, int *size, int *type, char *name) {
    (void)program; (void)index; (void)bufsize; (void)length; (void)size; (void)type; (void)name;
    SWGL_LOGW("get_active_attrib: not fully implemented");
}

void swgl_gl_get_active_uniform(unsigned int program, unsigned int index, int bufsize,
                                int *length, int *size, int *type, char *name) {
    (void)program; (void)index; (void)bufsize; (void)length; (void)size; (void)type; (void)name;
    SWGL_LOGW("get_active_uniform: not fully implemented");
}

void swgl_gl_release_shader_compiler(void) { /* no-op */ }
void swgl_gl_shader_binary(int n, const unsigned int *shaders, int binaryformat,
                           const void *binary, int length) {
    (void)n; (void)shaders; (void)binaryformat; (void)binary; (void)length;
    SWGL_LOGW("shader_binary: not implemented (no shader binary formats supported)");
}

void swgl_gl_finish(void) { /* no-op */ }
void swgl_gl_flush(void) { /* no-op */ }
void swgl_gl_polygon_offset(float factor, float units) { (void)factor; (void)units; }
void swgl_gl_line_width(float width) { (void)width; }
void swgl_gl_front_face(int mode) { (void)mode; }
void swgl_gl_cull_face(int mode) { (void)mode; }
void swgl_gl_sample_coverage(float value, unsigned char invert) { (void)value; (void)invert; }

/* glDepthRangef */
void swgl_gl_depth_rangef(float n, float f) {
    (void)n; (void)f;
    /* We always use 0..1; accepted, ignored. */
}

/* glHint */
#define GL_GENERATE_MIPMAP_HINT 0x8192
void swgl_gl_hint(int target, int mode) { (void)target; (void)mode; }

/* ====================================================================
 *  GLES 3 compatibility shim
 *
 *  We advertise GLES 3.0 in glGetString(GL_VERSION) so apps that gate on
 *  version at startup boot. The entry points below are either:
 *    (a) mapped to existing GLES 2 equivalents (VAO, glTexStorage2D → glTexImage2D),
 *    (b) real-but-minimal (glReadPixels, glBufferSubData, glMapBufferRange), or
 *    (c) no-op-success stubs (transform feedback, queries, compute, FBO depth)
 *  so the app doesn't crash. Real GLES 3 rendering needs ANGLE. See docs/LIMITATIONS.md.
 * ==================================================================== */

/* --- Vertex Array Objects (map to the OES stubs that already exist) --- */
void swgl_gl_gen_vertex_arrays(int n, unsigned int *arrays) { swgl_gl_gen_vertex_arrays_oes(n, arrays); }
void swgl_gl_bind_vertex_array(unsigned int array) { swgl_gl_bind_vertex_array_oes(array); }
void swgl_gl_delete_vertex_arrays(int n, const unsigned int *arrays) { swgl_gl_delete_vertex_arrays_oes(n, arrays); }
unsigned char swgl_gl_is_vertex_array(unsigned int array) { (void)array; return 1; }

/* --- glGetStringi (GLES 3 extension query by index) --- */
/* declared above; re-exported via swgl_resolve below. */

/* --- Buffer sub-data + map (real; common in GLES 3 apps for streaming) --- */
void swgl_gl_buffer_sub_data(int target, long offset, long size, const void *data) {
    unsigned int buf = (target == 0x8892 /* GL_ARRAY_BUFFER */) ? g.bound_array_buffer : g.bound_element_array_buffer;
    Buffer* b = find_buffer(buf);
    if (!b || !data || offset < 0 || size < 0 || (size_t)(offset + size) > b->data.size()) {
        SWGL_LOGW("buffer_sub_data: bad args (buf=%u offset=%ld size=%ld bsize=%zu)", buf, offset, size, b ? b->data.size() : 0);
        return;
    }
    memcpy(b->data.data() + offset, data, size);
}

void *swgl_gl_map_buffer_range(int target, long offset, long length, int access) {
    (void)access;
    unsigned int buf = (target == 0x8892) ? g.bound_array_buffer : g.bound_element_array_buffer;
    Buffer* b = find_buffer(buf);
    if (!b || offset < 0 || length < 0 || (size_t)(offset + length) > b->data.size()) return nullptr;
    return b->data.data() + offset;
}

unsigned char swgl_gl_unmap_buffer(int target) {
    (void)target;
    return 1; /* GL_TRUE */
}

void swgl_gl_flush_mapped_buffer_range(int target, long offset, long length) { (void)target; (void)offset; (void)length; }
void swgl_gl_copy_buffer_sub_data(int readTarget, int writeTarget, long readOffset, long writeOffset, long size) {
    (void)readTarget; (void)writeTarget; (void)readOffset; (void)writeOffset; (void)size;
    SWGL_LOGW("copy_buffer_sub_data: STUB (no-op)");
}

/* --- glReadPixels (real — read from the color framebuffer) --- */
void swgl_gl_read_pixels(int x, int y, int width, int height, int format, int type, void *pixels) {
    if (!pixels || width <= 0 || height <= 0) return;
    /* We only support GL_RGBA + GL_UNSIGNED_BYTE for now. */
    if (format != 0x1908 /* GL_RGBA */ || type != 0x1401 /* GL_UNSIGNED_BYTE */) {
        SWGL_LOGW("read_pixels: format/type 0x%x/0x%x not supported (only RGBA/UBYTE)", format, type);
        return;
    }
    if (g.fb.color.empty()) return;
    int fbW = g.fb.width, fbH = g.fb.height;
    uint8_t *fb = g.fb.color.data();
    for (int row = 0; row < height; row++) {
        int sy = y + row;
        uint8_t *dstRow = (uint8_t*)pixels + row * width * 4;
        if (sy < 0 || sy >= fbH) { memset(dstRow, 0, width * 4); continue; }
        /* GL origin is bottom-left; our framebuffer is top-down. Flip Y. */
        int flippedSy = fbH - 1 - sy;
        int srcX0 = x < 0 ? 0 : x;
        int copyW = width;
        if (x < 0) copyW += x;
        if (x + width > fbW) copyW = fbW - x;
        if (copyW <= 0) { memset(dstRow, 0, width * 4); continue; }
        int dx0 = x < 0 ? -x : 0;
        memcpy(dstRow + dx0 * 4, fb + (flippedSy * fbW + srcX0) * 4, copyW * 4);
        if (dx0 > 0) memset(dstRow, 0, dx0 * 4);
        if (copyW < width - dx0) memset(dstRow + (dx0 + copyW) * 4, 0, (width - dx0 - copyW) * 4);
    }
}

/* --- FBOs (degraded: pretend the default framebuffer is the only one) --- */
void swgl_gl_gen_framebuffers(int n, unsigned int *framebuffers) {
    if (!framebuffers || n <= 0) return;
    for (int i = 0; i < n; i++) framebuffers[i] = alloc_id();
}
void swgl_gl_bind_framebuffer(int target, unsigned int framebuffer) {
    (void)target; (void)framebuffer;
    /* Pretend framebuffer 0 (default) is always bound. Non-zero IDs are accepted + ignored. */
}
int swgl_gl_check_framebuffer_status(int target) {
    (void)target;
    return 0x8CD5; /* GL_FRAMEBUFFER_COMPLETE */
}
void swgl_gl_framebuffer_texture_2d(int target, int attachment, int textarget, unsigned int texture, int level) {
    (void)target; (void)attachment; (void)textarget; (void)texture; (void)level;
    /* STUB: real FBO-to-texture rendering not supported. Apps rendering to a
     * texture FBO will render to the default framebuffer instead. */
    SWGL_LOGW("framebuffer_texture_2d: STUB (renders to default FB, not the texture)");
}
void swgl_gl_delete_framebuffers(int n, const unsigned int *framebuffers) { (void)n; (void)framebuffers; }
void swgl_gl_gen_renderbuffers(int n, unsigned int *renderbuffers) { if (!renderbuffers || n<=0) return; for (int i=0;i<n;i++) renderbuffers[i]=alloc_id(); }
void swgl_gl_bind_renderbuffer(int target, unsigned int renderbuffer) { (void)target; (void)renderbuffer; }
void swgl_gl_renderbuffer_storage(int target, int internalformat, int width, int height) { (void)target; (void)internalformat; (void)width; (void)height; }
void swgl_gl_framebuffer_renderbuffer(int target, int attachment, int renderbuffertarget, unsigned int renderbuffer) { (void)target; (void)attachment; (void)renderbuffertarget; (void)renderbuffer; }
void swgl_gl_delete_renderbuffers(int n, const unsigned int *renderbuffers) { (void)n; (void)renderbuffers; }
void swgl_gl_blit_framebuffer(int srcX0, int srcY0, int srcX1, int srcY1, int dstX0, int dstY0, int dstX1, int dstY1, unsigned int mask, int filter) {
    (void)srcX0; (void)srcY0; (void)srcX1; (void)srcY1; (void)dstX0; (void)dstY0; (void)dstX1; (void)dstY1; (void)mask; (void)filter;
    SWGL_LOGW("blit_framebuffer: STUB (no-op)");
}

/* --- Transform feedback (no-op success) --- */
void swgl_gl_transform_feedback_varyings(unsigned int program, int count, const char *const*varyings, int bufferMode) {
    (void)program; (void)count; (void)varyings; (void)bufferMode;
    SWGL_LOGW("transform_feedback_varyings: STUB (no transform feedback)");
}
void swgl_gl_begin_transform_feedback(int primitiveMode) { (void)primitiveMode; }
void swgl_gl_end_transform_feedback(void) {}
void swgl_gl_pause_transform_feedback(void) {}
void swgl_gl_resume_transform_feedback(void) {}
void swgl_gl_bind_buffer_base(int target, unsigned int index, unsigned int buffer) { (void)target; (void)index; (void)buffer; }
void swgl_gl_bind_buffer_range(int target, unsigned int index, unsigned int buffer, long offset, long size) { (void)target; (void)index; (void)buffer; (void)offset; (void)size; }

/* --- Queries (no-op success, return 0 results) --- */
void swgl_gl_gen_queries(int n, unsigned int *ids) { if (!ids||n<=0) return; for(int i=0;i<n;i++) ids[i]=alloc_id(); }
void swgl_gl_delete_queries(int n, const unsigned int *ids) { (void)n; (void)ids; }
void swgl_gl_begin_query(int target, unsigned int id) { (void)target; (void)id; }
void swgl_gl_end_query(int target) { (void)target; }
void swgl_gl_get_query_iv(int target, int pname, int *params) { (void)target; (void)pname; if(params) *params = 0; }
void swgl_gl_get_query_object_iv(unsigned int id, int pname, int *params) { (void)id; (void)pname; if(params) *params = 1; /* GL_QUERY_RESULT_AVAILABLE */ }
void swgl_gl_get_query_object_uiv(unsigned int id, int pname, unsigned int *params) { (void)id; (void)pname; if(params) *params = 0; }

/* --- Uniform Buffer Objects / Shader Storage Blocks (no-op success) --- */
unsigned int swgl_gl_get_uniform_block_index(unsigned int program, const char *uniformBlockName) { (void)program; (void)uniformBlockName; return 0; }
void swgl_gl_get_active_uniform_block_iv(unsigned int program, unsigned int uniformBlockIndex, int pname, int *params) { (void)program; (void)uniformBlockIndex; (void)pname; if(params) *params = 0; }
void swgl_gl_uniform_block_binding(unsigned int program, unsigned int uniformBlockIndex, unsigned int uniformBlockBinding) { (void)program; (void)uniformBlockIndex; (void)uniformBlockBinding; }
unsigned int swgl_gl_get_program_resource_index(unsigned int program, int programInterface, const char *name) { (void)program; (void)programInterface; (void)name; return 0; }
void swgl_gl_get_program_resource_iv(unsigned int program, int programInterface, unsigned int index, int propCount, const int *props, int bufSize, int *length, int *params) { (void)program; (void)programInterface; (void)index; (void)propCount; (void)props; (void)bufSize; (void)length; if(params) *params = 0; }
void swgl_gl_get_program_resource_name(unsigned int program, int programInterface, unsigned int index, int bufSize, int *length, char *name) { (void)program; (void)programInterface; (void)index; (void)bufSize; if(length) *length = 0; if(name && bufSize > 0) name[0] = '\0'; }
void swgl_gl_shader_storage_block_binding(unsigned int program, unsigned int storageBlockIndex, unsigned int storageBlockBinding) { (void)program; (void)storageBlockIndex; (void)storageBlockBinding; }
void swgl_gl_bind_image_texture(unsigned int unit, unsigned int texture, int level, unsigned char layered, int layer, int access, int format) { (void)unit; (void)texture; (void)level; (void)layered; (void)layer; (void)access; (void)format; }

/* --- Compute (no-op success — no compute shader support) --- */
void swgl_gl_dispatch_compute(unsigned int num_groups_x, unsigned int num_groups_y, unsigned int num_groups_z) { (void)num_groups_x; (void)num_groups_y; (void)num_groups_z; SWGL_LOGW("dispatch_compute: STUB (no compute)"); }
void swgl_gl_dispatch_compute_indirect(long indirect) { (void)indirect; SWGL_LOGW("dispatch_compute_indirect: STUB (no compute)"); }
void swgl_gl_memory_barrier(unsigned int barriers) { (void)barriers; }
void swgl_gl_barrier(void) {}

/* --- Integer uniforms (store as int; the GLSL interpreter reads them) --- */
void swgl_gl_uniform_1ui(int location, unsigned int v0) { int v = (int)v0; ProgramObj* p=find_program(g.bound_program); if(p&&p->glsl&&location>=0) glsl_set_uniform(p->glsl,location,GLSL_INT,1,&v); }
void swgl_gl_uniform_2ui(int location, unsigned int v0, unsigned int v1) { int v[2]={(int)v0,(int)v1}; ProgramObj* p=find_program(g.bound_program); if(p&&p->glsl&&location>=0) glsl_set_uniform(p->glsl,location,GLSL_INT,2,v); }
void swgl_gl_uniform_3ui(int location, unsigned int v0, unsigned int v1, unsigned int v2) { int v[3]={(int)v0,(int)v1,(int)v2}; ProgramObj* p=find_program(g.bound_program); if(p&&p->glsl&&location>=0) glsl_set_uniform(p->glsl,location,GLSL_INT,3,v); }
void swgl_gl_uniform_4ui(int location, unsigned int v0, unsigned int v1, unsigned int v2, unsigned int v3) { int v[4]={(int)v0,(int)v1,(int)v2,(int)v3}; ProgramObj* p=find_program(g.bound_program); if(p&&p->glsl&&location>=0) glsl_set_uniform(p->glsl,location,GLSL_INT,4,v); }
void swgl_gl_uniform_1uiv(int location, int count, const unsigned int *value) { ProgramObj* p=find_program(g.bound_program); if(p&&p->glsl&&location>=0&&value) glsl_set_uniform(p->glsl,location,GLSL_INT,count,value); }
void swgl_gl_uniform_2uiv(int location, int count, const unsigned int *value) { ProgramObj* p=find_program(g.bound_program); if(p&&p->glsl&&location>=0&&value) glsl_set_uniform(p->glsl,location,GLSL_INT,count*2,value); }
void swgl_gl_uniform_3uiv(int location, int count, const unsigned int *value) { ProgramObj* p=find_program(g.bound_program); if(p&&p->glsl&&location>=0&&value) glsl_set_uniform(p->glsl,location,GLSL_INT,count*3,value); }
void swgl_gl_uniform_4uiv(int location, int count, const unsigned int *value) { ProgramObj* p=find_program(g.bound_program); if(p&&p->glsl&&location>=0&&value) glsl_set_uniform(p->glsl,location,GLSL_INT,count*4,value); }
void swgl_gl_uniform_matrix_2fv(int location, int count, unsigned char transpose, const float *value) { ProgramObj* p=find_program(g.bound_program); if(p&&p->glsl&&location>=0&&value) glsl_set_uniform(p->glsl,location,GLSL_FLOAT,count*4,value); }
void swgl_gl_uniform_matrix_3fv(int location, int count, unsigned char transpose, const float *value) { ProgramObj* p=find_program(g.bound_program); if(p&&p->glsl&&location>=0&&value) glsl_set_uniform(p->glsl,location,GLSL_FLOAT,count*9,value); }
void swgl_gl_uniform_matrix_2x3fv(int location, int count, unsigned char transpose, const float *value) { (void)location;(void)count;(void)transpose;(void)value; }
void swgl_gl_uniform_matrix_3x2fv(int location, int count, unsigned char transpose, const float *value) { (void)location;(void)count;(void)transpose;(void)value; }
void swgl_gl_uniform_matrix_2x4fv(int location, int count, unsigned char transpose, const float *value) { (void)location;(void)count;(void)transpose;(void)value; }
void swgl_gl_uniform_matrix_4x2fv(int location, int count, unsigned char transpose, const float *value) { (void)location;(void)count;(void)transpose;(void)value; }
void swgl_gl_uniform_matrix_3x4fv(int location, int count, unsigned char transpose, const float *value) { (void)location;(void)count;(void)transpose;(void)value; }
void swgl_gl_uniform_matrix_4x3fv(int location, int count, unsigned char transpose, const float *value) { (void)location;(void)count;(void)transpose;(void)value; }

/* --- Integer vertex attribs (no-op success — we treat all attribs as float) --- */
void swgl_gl_vertex_attrib_i_pointer(unsigned int index, int size, int type, int stride, const void *pointer) {
    /* Route to the float pointer path; the rasterizer will reinterpret. */
    swgl_gl_vertex_attrib_pointer(index, size, type, 0 /*normalized*/, stride, pointer);
}
void swgl_gl_vertex_attrib_i4i(unsigned int index, int x, int y, int z, int w) { float v[4]={(float)x,(float)y,(float)z,(float)w}; swgl_gl_vertex_attrib_4f(index,v[0],v[1],v[2],v[3]); }
void swgl_gl_vertex_attrib_i4ui(unsigned int index, unsigned int x, unsigned int y, unsigned int z, unsigned int w) { float v[4]={(float)x,(float)y,(float)z,(float)w}; swgl_gl_vertex_attrib_4f(index,v[0],v[1],v[2],v[3]); }

/* --- Instancing (degraded: draw only 1 instance, ignore divisor) --- */
void swgl_gl_vertex_attrib_divisor(unsigned int index, unsigned int divisor) { (void)index; (void)divisor; }
void swgl_gl_draw_arrays_instanced(int mode, int first, int count, int instancecount) {
    (void)instancecount; /* draw 1 instance — degraded but runs */
    swgl_gl_draw_arrays(mode, first, count);
}
void swgl_gl_draw_elements_instanced(int mode, int count, int type, const void *indices, int instancecount) {
    (void)instancecount;
    swgl_gl_draw_elements(mode, count, type, indices);
}
void swgl_gl_draw_range_elements(int mode, unsigned int start, unsigned int end, int count, int type, const void *indices) {
    (void)start; (void)end;
    swgl_gl_draw_elements(mode, count, type, indices);
}

/* --- Texture storage (map to glTexImage2D) --- */
void swgl_gl_tex_storage_2d(int target, int levels, int internalformat, int width, int height) {
    /* Treat as a single-level glTexImage2D with no initial data. */
    int format = (internalformat == 0x1908 /* GL_RGBA */) ? 0x1908 : 0x1908;
    swgl_gl_tex_image_2d(target, 0, internalformat, width, height, 0, format, 0x1401 /* GL_UNSIGNED_BYTE */, nullptr);
}
void swgl_gl_tex_storage_3d(int target, int levels, int internalformat, int width, int height, int depth) { (void)target;(void)levels;(void)internalformat;(void)width;(void)height;(void)depth; SWGL_LOGW("tex_storage_3d: STUB (no 3D textures)"); }
void swgl_gl_tex_sub_image_3d(int target, int level, int xoffset, int yoffset, int zoffset, int width, int height, int depth, int format, int type, const void *pixels) { (void)target;(void)level;(void)xoffset;(void)yoffset;(void)zoffset;(void)width;(void)height;(void)depth;(void)format;(void)type;(void)pixels; SWGL_LOGW("tex_sub_image_3d: STUB"); }
void swgl_gl_compressed_tex_image_2d(int target, int level, int internalformat, int width, int height, int border, int imageSize, const void *data) {
    (void)target;(void)level;(void)internalformat;(void)width;(void)height;(void)border;(void)imageSize;(void)data;
    SWGL_LOGW("compressed_tex_image_2d: STUB (no compressed texture decode; texture will be blank)");
}
void swgl_gl_compressed_tex_sub_image_2d(int target, int level, int xoffset, int yoffset, int width, int height, int format, int imageSize, const void *data) { (void)target;(void)level;(void)xoffset;(void)yoffset;(void)width;(void)height;(void)format;(void)imageSize;(void)data; }
void swgl_gl_copy_tex_image_2d(int target, int level, int internalformat, int x, int y, int width, int height, int border) { (void)target;(void)level;(void)internalformat;(void)x;(void)y;(void)width;(void)height;(void)border; SWGL_LOGW("copy_tex_image_2d: STUB"); }
void swgl_gl_copy_tex_sub_image_2d(int target, int level, int xoffset, int yoffset, int x, int y, int width, int height) { (void)target;(void)level;(void)xoffset;(void)yoffset;(void)x;(void)y;(void)width;(void)height; }
void swgl_gl_tex_image_3d(int target, int level, int internalformat, int width, int height, int depth, int border, int format, int type, const void *pixels) { (void)target;(void)level;(void)internalformat;(void)width;(void)height;(void)depth;(void)border;(void)format;(void)type;(void)pixels; SWGL_LOGW("tex_image_3d: STUB (no 3D textures)"); }
void swgl_gl_active_texture_texture_unit(int unit) { swgl_gl_active_texture(unit); }  /* alias */
void swgl_gl_tex_parameter_f(int target, int pname, float param) { (void)target;(void)pname;(void)param; }
void swgl_gl_tex_parameter_fv(int target, int pname, const float *params) { (void)target;(void)pname;(void)params; }

/* --- Sync objects (no-op success) --- */
void *swgl_gl_fence_sync(int condition, unsigned int flags) { (void)condition;(void)flags; return (void*)0x1; }
unsigned char swgl_gl_is_sync(void *sync) { (void)sync; return 1; }
void swgl_gl_delete_sync(void *sync) { (void)sync; }
int swgl_gl_client_wait_sync(void *sync, unsigned int flags, unsigned long long timeout) { (void)sync;(void)flags;(void)timeout; return 0x911D /* GL_ALREADY_SIGNALED */; }
void swgl_gl_wait_sync(void *sync, unsigned int flags, unsigned long long timeout) { (void)sync;(void)flags;(void)timeout; }

/* --- Multi-draw (degraded: draw each individually) --- */
void swgl_gl_multi_draw_arrays(int mode, const int *first, const int *count, int drawcount) {
    for (int i = 0; i < drawcount; i++) swgl_gl_draw_arrays(mode, first[i], count[i]);
}
void swgl_gl_multi_draw_elements(int mode, const int *count, int type, const void *const*indices, int drawcount) {
    for (int i = 0; i < drawcount; i++) swgl_gl_draw_elements(mode, count[i], type, indices[i]);
}

/* --- Sampler objects (no-op success — we use texture params directly) --- */
void swgl_gl_gen_samplers(int count, unsigned int *samplers) { if(!samplers||count<=0) return; for(int i=0;i<count;i++) samplers[i]=alloc_id(); }
void swgl_gl_bind_sampler(unsigned int unit, unsigned int sampler) { (void)unit;(void)sampler; }
void swgl_gl_sampler_parameter_i(unsigned int sampler, int pname, int param) { (void)sampler;(void)pname;(void)param; }
void swgl_gl_delete_samplers(int count, const unsigned int *samplers) { (void)count;(void)samplers; }

/* --- glInvalidateFramebuffer (no-op success) --- */
void swgl_gl_invalidate_framebuffer(int target, int numAttachments, const int *attachments) { (void)target;(void)numAttachments;(void)attachments; }
void swgl_gl_invalidate_subFramebuffer(int target, int numAttachments, const int *attachments, int x, int y, int width, int height) { (void)target;(void)numAttachments;(void)attachments;(void)x;(void)y;(void)width;(void)height; }

/* --- Debug output (no-op success) --- */
void swgl_gl_debug_message_callback(int source, int type, int id, int severity, int length, const char *message, const void *userParam) { (void)source;(void)type;(void)id;(void)severity;(void)length;(void)message;(void)userParam; }
void swgl_gl_push_debug_group(int source, int id, int length, const char *message) { (void)source;(void)id;(void)length;(void)message; }
void swgl_gl_pop_debug_group(void) {}
void swgl_gl_object_label(int identifier, unsigned int name, int length, const char *label) { (void)identifier;(void)name;(void)length;(void)label; }

/* --- GLES 3 EGL attribs (EGL 1.5 + context attributes) --- */
/* eglCreateContext with EGL_CONTEXT_MAJOR_VERSION_KHR=3 is accepted by our
 * swgl_egl_create_context (it ignores attribs). No extra function needed. */

}  // extern "C"
