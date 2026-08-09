/*
 * glsl.h — GLSL ES 1.0 subset parser + AST interpreter (INTERNAL C ABI).
 *
 * Status: REAL (common path). Implements the GLSL ES 1.0 subset listed in
 *         worklog.md Phase 2 / P2-3: float/vec2..4/mat2..4/int/ivec2..4/bool/
 *         sampler2D types; attribute/uniform/varying/const qualifiers;
 *         gl_Position/gl_FragColor/gl_FragCoord/gl_PointCoord built-ins;
 *         + - * / unary -/! == != < > <= >= && || = swizzles (read+write) [];
 *         built-in fns: mix clamp min max abs sqrt length normalize dot cross
 *         reflect texture2D pow sin cos radians degrees; if/else (no for/while);
 *         preprocessor: #version 100 + precision lines (others ignored).
 *
 *         NOT implemented: user-defined functions, structs, arrays of non-float,
 *         for/while loops, gl_PointSize semantics (slot exists but unused),
 *         dynamic indexing of non-const arrays, integer-only ops on ivec.
 *         Where the spec'd common path uses mat4*vec4, vec4*vec4, texture2D,
 *         swizzles, and basic arithmetic, those are all REAL.
 *
 * This header is internal to swgl.cpp; it is NOT installed in the public
 * include path. The public GL ABI is in swgl.h.
 */
#ifndef APKCONTAINER_GLSL_H
#define APKCONTAINER_GLSL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GLSL_VOID = 0,
    GLSL_FLOAT,
    GLSL_VEC2, GLSL_VEC3, GLSL_VEC4,
    GLSL_MAT2, GLSL_MAT3, GLSL_MAT4,
    GLSL_INT,
    GLSL_IVEC2, GLSL_IVEC3, GLSL_IVEC4,
    GLSL_BOOL,
    GLSL_SAMPLER2D,
    GLSL_UNKNOWN
} glsl_type_kind_t;

/* Returns the component count for a vector/matrix type (1 for float/int/bool,
 * 4 for vec4, 16 for mat4, etc.). Matrices are stored row-major in flat
 * floats: mat4 = 16 floats, mat3 = 9 floats (NOT 12). */
int glsl_type_components(glsl_type_kind_t t);

/* A read-only view of a texture owned by swgl. */
typedef struct {
    int width;
    int height;
    const uint8_t *rgba;   /* RGBA8, row-major, tightly packed, no padding */
    int min_filter;        /* GL_NEAREST=0x2600, GL_LINEAR=0x2601 */
    int mag_filter;
    int wrap_s;            /* GL_CLAMP_TO_EDGE=0x812F, GL_REPEAT=0x2901 */
    int wrap_t;
} glsl_texture_view;

typedef struct glsl_program glsl_program_t;

/* One vertex attrib passed to glsl_run_vertex. `value` points to raw bytes;
 * the interpreter reads `size` components of the given `type`. */
typedef struct {
    int index;          /* matches the index returned by glsl_get_attrib_location */
    int size;           /* 1, 2, 3, or 4 components */
    int type;           /* GL_FLOAT=0x1406, GL_UNSIGNED_BYTE=0x1401, GL_BYTE=0x1400,
                           GL_SHORT=0x1402, GL_UNSIGNED_SHORT=0x1403, GL_FIXED=0x140C,
                           GL_INT=0x1404, GL_UNSIGNED_INT=0x1405 */
    int normalized;     /* 1 if integer types should be mapped to 0..1 / -1..1 */
    const void *value;  /* raw bytes; we read size * sizeof(type) bytes */
} glsl_attrib;

/* Compile both shaders. Always returns a non-NULL program object even if
 * parsing failed; check glsl_link() / glsl_get_*_info for the actual status.
 * vs_src and fs_src are NUL-terminated C strings. */
glsl_program_t *glsl_compile(const char *vs_src, const char *fs_src);
void glsl_destroy(glsl_program_t *p);

/* Link the VS+FS together (resolve shared varyings, assign uniform/attrib
 * slots). Returns 0 on success, non-zero + fills info_log on failure. */
int glsl_link(glsl_program_t *p, char *info_log, int log_len);

/* Per-shader compile info (set during glsl_compile). */
int         glsl_get_vs_compile_status(glsl_program_t *p);
int         glsl_get_fs_compile_status(glsl_program_t *p);
const char *glsl_get_vs_info_log(glsl_program_t *p);
const char *glsl_get_fs_info_log(glsl_program_t *p);
int         glsl_get_link_status(glsl_program_t *p);

/* Look up uniform/attrib by name. Returns -1 if not found or not linked. */
int glsl_get_uniform_location(glsl_program_t *p, const char *name);
int glsl_get_attrib_location(glsl_program_t *p, const char *name);

/* Returns the number of varyings the VS writes and the FS reads. Each
 * varying occupies up to 4 floats in the packed varying array passed to
 * glsl_run_vertex / glsl_run_fragment. */
int glsl_get_varying_count(glsl_program_t *p);
/* Returns the component count (1..4) for varying slot i. */
int glsl_get_varying_components(glsl_program_t *p, int slot);

/* Set a uniform value. `type` is glsl_type_kind_t; `count` is the number of
 * elements (for non-array uniforms, count=1). `value` points to the raw
 * values: float-typed in IEEE-754, int-typed as 32-bit ints.
 * For SAMPLER2D, type=GLSL_INT and value is a single int = the texture unit
 * index (0..N). Returns 0 on success. */
int glsl_set_uniform(glsl_program_t *p, int location, int type, int count, const void *value);

/* Bind a texture view to a texture-unit slot in the program. The rasterizer
 * in swgl.cpp calls this before running the fragment shader for every sampler
 * the program declares: it reads the sampler's uniform value (a unit index,
 * set via glUniform1i) and plugs in the currently-bound GL_TEXTURE_2D for
 * that unit. Returns 0 on success. */
int glsl_bind_texture_to_unit(glsl_program_t *p, int unit, const glsl_texture_view *tex);

/* Run the vertex shader for one vertex. attribs is a packed array of n
 * glsl_attrib structs. out_position is a vec4 (clip space, before perspective
 * divide). out_varyings is a flat float array of size >= varying_count*4.
 * Returns 0 on success, non-zero on interpreter error. */
int glsl_run_vertex(glsl_program_t *p, const glsl_attrib *attribs, int n,
                    float *out_position,
                    float *out_varyings, int varying_count);

/* Run the fragment shader for one pixel. varyings is the interpolated flat
 * float array of varying_count*4 floats. frag_coord is vec4 (x, y, z, 1/w)
 * where x/y are pixel coords (origin lower-left), z is depth 0..1, and 1/w
 * is the reciprocal of the clip-space w (used by the program if it wants to
 * un-perspective-correct varyings — most shaders don't). out_color is vec4
 * RGBA 0..1. Returns 0 on success, non-zero on interpreter error. */
int glsl_run_fragment(glsl_program_t *p, const float *varyings, int varying_count,
                      const float *frag_coord,
                      float *out_color);

#ifdef __cplusplus
}
#endif

#endif /* APKCONTAINER_GLSL_H */
