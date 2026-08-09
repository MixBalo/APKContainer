/*
 * dex_loader.h — DEX (Dalvik Executable) file loader / table resolver
 *
 * Status: REAL. Parses the DEX header, string/type/proto/field/method/class
 *         tables, and walks the class_data_item to locate code_items.
 *         Implemented against the .dex format spec (Android 9+ versions
 *         035/037/038/039). See Native/ART/dex_loader.cpp for details.
 *
 * Scope: this loader only READS DEX. It does NOT verify checksums/SHA-1
 *        (logged at INFO only) and does NOT execute bytecode. Bytecode
 *        execution is the job of dex_interp.h / dex_interp.cpp.
 */
#ifndef APKCONTAINER_DEX_LOADER_H
#define APKCONTAINER_DEX_LOADER_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct dex_string { const char *data; uint32_t utf16_len; } dex_string_t;
typedef struct dex_type   { uint32_t descriptor_idx; } dex_type_t;
typedef struct dex_proto  { uint32_t shorty_idx; uint32_t return_type_idx; uint32_t parameters_off; } dex_proto_t;
typedef struct dex_field  { uint16_t class_idx; uint16_t type_idx; uint32_t name_idx; } dex_field_t;
typedef struct dex_method { uint16_t class_idx; uint16_t proto_idx; uint32_t name_idx; } dex_method_t;
typedef struct dex_class_def {
    uint32_t class_idx; uint32_t access_flags; uint32_t superclass_idx;
    uint32_t interfaces_off; uint32_t source_file_idx; uint32_t annotations_off;
    uint32_t class_data_off; uint32_t static_values_off;
} dex_class_def_t;

typedef struct dex_code {
    uint16_t registers_size; uint16_t ins_size; uint16_t outs_size;
    uint16_t tries_size; uint32_t debug_info_off; uint32_t insns_size;
    const uint16_t *insns;   // points into mapped DEX
} dex_code_t;

typedef struct dex_file {
    const uint8_t *base; size_t size;
    const char *header_magic;
    uint32_t string_ids_size; uint32_t string_ids_off;
    uint32_t type_ids_size;   uint32_t type_ids_off;
    uint32_t proto_ids_size;  uint32_t proto_ids_off;
    uint32_t field_ids_size;  uint32_t field_ids_off;
    uint32_t method_ids_size; uint32_t method_ids_off;
    uint32_t class_defs_size; uint32_t class_defs_off;
    uint32_t data_size;       uint32_t data_off;
} dex_file_t;

int  dex_open(const char *path, dex_file_t *out);
void dex_close(dex_file_t *dex);

// Resolvers — return NULL / -1 on not-found
const char *dex_string(const dex_file_t *dex, uint32_t idx);          // raw UTF-8
const char *dex_type_descriptor(const dex_file_t *dex, uint32_t idx);
const dex_field_t  *dex_field(const dex_file_t *dex, uint32_t idx);
const dex_method_t *dex_method(const dex_file_t *dex, uint32_t idx);
const dex_proto_t  *dex_proto(const dex_file_t *dex, uint32_t idx);
const dex_class_def_t *dex_class_def(const dex_file_t *dex, uint32_t idx);

// Find a class def by binary descriptor, e.g. "Lcom/example/MainActivity;"
const dex_class_def_t *dex_find_class(const dex_file_t *dex, const char *descriptor);

// Locate a code_item for a method_idx, walking class_data_item. Returns 0 on success.
int dex_find_code(const dex_file_t *dex, const dex_class_def_t *cls,
                  uint32_t method_idx, int is_direct, dex_code_t *out_code);

// Find a method by name+shorty within a class. Returns method_idx or -1.
int32_t dex_find_method(const dex_file_t *dex, const dex_class_def_t *cls,
                        const char *name, const char *shorty);

// Find a field by name within a class. Returns field_idx or -1.
int32_t dex_find_field(const dex_file_t *dex, const dex_class_def_t *cls, const char *name);

#ifdef __cplusplus
}
#endif
#endif
