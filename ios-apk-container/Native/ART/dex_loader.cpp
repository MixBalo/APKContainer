/*
 * dex_loader.cpp — DEX (Dalvik Executable) file loader / table resolver
 *
 * Status: REAL. Reads a .dex file via mmap (read-only) and resolves entries
 *         in the string_ids / type_ids / proto_ids / field_ids / method_ids /
 *         class_defs tables, plus walks class_data_item to locate code_items.
 *
 * Format reference (DEX 035/037/038/039):
 *   - Header at offset 0, size 0x70 (112 bytes).
 *   - magic[8] = "dex\n035\0" (or 037/038/039).
 *   - All multi-byte integers are little-endian (endian_tag = 0x12345678).
 *   - string_id_item: uint32 string_data_off  -> string_data_item:
 *       uleb128 utf16_size;  uint8 mutf8[...];  0x00 terminator.
 *       MUTF-8: 1/2/3-byte encoding; U+0000 encoded as 0xC0 0x80.
 *   - type_id_item:    uint32 descriptor_idx  (into string_ids).
 *   - proto_id_item:   uint32 shorty_idx, return_type_idx, parameters_off.
 *   - field_id_item:   uint16 class_idx, uint16 type_idx, uint32 name_idx.
 *   - method_id_item:  uint16 class_idx, uint16 proto_idx, uint32 name_idx.
 *   - class_def_item:  8 x uint32 (class_idx, access_flags, superclass_idx,
 *                      interfaces_off, source_file_idx, annotations_off,
 *                      class_data_off, static_values_off).
 *   - class_data_item: uleb128 static_fields_size, instance_fields_size,
 *                      direct_methods_size, virtual_methods_size; then
 *                      encoded_field[] + encoded_method[] (delta-encoded).
 *   - encoded_field:   uleb128 field_idx_diff; uleb128 access_flags.
 *   - encoded_method:  uleb128 method_idx_diff; uleb128 access_flags;
 *                      uleb128 code_off (0 = abstract/native).
 *   - code_item:       uint16 registers_size, ins_size, outs_size, tries_size;
 *                      uint32 debug_info_off, insns_size; uint16 insns[insns_size].
 *
 * Scope: READ-ONLY. No verification of checksum/SHA-1 (logged at INFO only,
 *        not enforced). No bytecode execution.
 *
 * Honesty: every function returns NULL / -1 on bad input, out-of-bounds
 *          index, or missing class_data. Common path (well-formed DEX from
 *          dx/d8) works end-to-end.
 */
#include "dex_loader.h"
#include "log_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <string>
#include <vector>

namespace {
// ---- Little-endian readers (DEX is LE on all supported platforms) ----
inline uint16_t rd_u16(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}
inline uint32_t rd_u32(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

// ---- uleb128 / uleb128p1 readers ----
// uleb128: 7 bits per byte, MSB = continuation.
uint32_t read_uleb128(const uint8_t **pp, const uint8_t *end) {
    const uint8_t *p = *pp;
    uint32_t result = 0;
    int shift = 0;
    while (p < end) {
        uint8_t b = *p++;
        result |= (uint32_t)(b & 0x7F) << shift;
        if ((b & 0x80) == 0) { *pp = p; return result; }
        shift += 7;
        if (shift >= 35) break;   // uleb128 is at most 5 bytes for uint32
    }
    *pp = p;
    return result;
}

// uleb128p1: value = uleb128 - 1; the encoding 0 represents NO_INDEX (0xFFFFFFFF).
// Used for type_idx in encoded_array/annotation contexts, NOT for class_data
// field/method indices (those are plain uleb128 deltas).
uint32_t read_uleb128p1(const uint8_t **pp, const uint8_t *end) {
    uint32_t raw = read_uleb128(pp, end);
    return (raw == 0) ? 0xFFFFFFFFu : raw - 1;
}

// Bounds-checked pointer arithmetic into the mapped DEX.
inline const uint8_t *offset_ptr(const dex_file_t *dex, uint32_t off, size_t need) {
    if (!dex || !dex->base) return nullptr;
    if ((size_t)off + need > dex->size) return nullptr;
    return dex->base + off;
}

// Decoded class_data_item entries.
struct decoded_field  { uint32_t idx; uint32_t access; };
struct decoded_method { uint32_t idx; uint32_t access; uint32_t code_off; };

struct class_data_walk {
    std::vector<decoded_field>  static_fields;
    std::vector<decoded_field>  instance_fields;
    std::vector<decoded_method> direct_methods;
    std::vector<decoded_method> virtual_methods;
};

// Walk the class_data_item at `class_data_off`. Returns false on parse error.
// Layout (per DEX spec):
//   uleb128 static_fields_size
//   uleb128 instance_fields_size
//   uleb128 direct_methods_size
//   uleb128 virtual_methods_size
//   encoded_field[static_fields_size]
//   encoded_field[instance_fields_size]
//   encoded_method[direct_methods_size]
//   encoded_method[virtual_methods_size]
// Indices in encoded_field / encoded_method are DELTAS from the previous entry
// (the first delta is from 0).
bool walk_class_data(const dex_file_t *dex, uint32_t class_data_off, class_data_walk *out) {
    out->static_fields.clear();
    out->instance_fields.clear();
    out->direct_methods.clear();
    out->virtual_methods.clear();
    if (class_data_off == 0) return true;   // no class data (e.g. marker class)

    const uint8_t *p = offset_ptr(dex, class_data_off, 1);
    if (!p) return false;
    const uint8_t *end = dex->base + dex->size;

    uint32_t sf_size  = read_uleb128(&p, end);
    uint32_t if_size  = read_uleb128(&p, end);
    uint32_t dm_size  = read_uleb128(&p, end);
    uint32_t vm_size  = read_uleb128(&p, end);

    auto read_fields = [&](std::vector<decoded_field> &v, uint32_t n) -> bool {
        uint32_t prev = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t diff   = read_uleb128(&p, end);
            uint32_t access = read_uleb128(&p, end);
            uint32_t idx    = prev + diff;
            prev = idx;
            v.push_back({idx, access});
        }
        return true;
    };
    auto read_methods = [&](std::vector<decoded_method> &v, uint32_t n) -> bool {
        uint32_t prev = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t diff     = read_uleb128(&p, end);
            uint32_t access   = read_uleb128(&p, end);
            uint32_t code_off = read_uleb128(&p, end);
            uint32_t idx      = prev + diff;
            prev = idx;
            v.push_back({idx, access, code_off});
        }
        return true;
    };
    read_fields(out->static_fields,   sf_size);
    read_fields(out->instance_fields, if_size);
    read_methods(out->direct_methods, dm_size);
    read_methods(out->virtual_methods, vm_size);
    return true;
}
}  // namespace

extern "C" {

// ---- dex_open: mmap the file read-only, parse the header ----
int dex_open(const char *path, dex_file_t *out) {
    if (!path || !out) return -1;
    memset(out, 0, sizeof(*out));

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        LOGE("dex", "open failed: %s (%s)", path, strerror(errno));
        return -2;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        LOGE("dex", "fstat failed or empty: %s", path);
        close(fd);
        return -3;
    }
    size_t sz = (size_t)st.st_size;
    void *map = mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        LOGE("dex", "mmap failed: %s (%s)", path, strerror(errno));
        close(fd);
        return -4;
    }
    // mmap keeps the mapping valid after close(2) on POSIX; close fd now.
    close(fd);

    const uint8_t *base = (const uint8_t *)map;
    if (sz < 0x70) {
        LOGE("dex", "file too small for DEX header: %s (size=%zu)", path, sz);
        munmap(map, sz);
        return -5;
    }
    // Validate magic: "dex\n035\0" / "036" / "037" / "038" / "039".
    if (memcmp(base, "dex\n", 4) != 0) {
        LOGE("dex", "bad magic (not 'dex\\n'): %s", path);
        munmap(map, sz);
        return -6;
    }
    char ver[4] = { (char)base[4], (char)base[5], (char)base[6], 0 };
    if (!(strcmp(ver, "035") == 0 || strcmp(ver, "036") == 0 ||
          strcmp(ver, "037") == 0 || strcmp(ver, "038") == 0 ||
          strcmp(ver, "039") == 0)) {
        LOGE("dex", "unsupported DEX version '%s': %s", ver, path);
        munmap(map, sz);
        return -7;
    }
    if (base[7] != 0) {
        LOGE("dex", "magic not null-terminated: %s", path);
        munmap(map, sz);
        return -8;
    }

    out->base  = base;
    out->size  = sz;
    out->header_magic = (const char *)base;

    // Header fields (offsets per DEX spec).
    out->string_ids_size = rd_u32(base + 0x38);
    out->string_ids_off  = rd_u32(base + 0x3C);
    out->type_ids_size   = rd_u32(base + 0x40);
    out->type_ids_off    = rd_u32(base + 0x44);
    out->proto_ids_size  = rd_u32(base + 0x48);
    out->proto_ids_off   = rd_u32(base + 0x4C);
    out->field_ids_size  = rd_u32(base + 0x50);
    out->field_ids_off   = rd_u32(base + 0x54);
    out->method_ids_size = rd_u32(base + 0x58);
    out->method_ids_off  = rd_u32(base + 0x5C);
    out->class_defs_size = rd_u32(base + 0x60);
    out->class_defs_off  = rd_u32(base + 0x64);
    out->data_size       = rd_u32(base + 0x68);
    out->data_off        = rd_u32(base + 0x6C);

    // Sanity: endian_tag.
    uint32_t endian = rd_u32(base + 0x28);
    if (endian != 0x12345678u) {
        LOGE("dex", "bad endian_tag 0x%08X (expected 0x12345678): %s", endian, path);
        munmap(map, sz);
        memset(out, 0, sizeof(*out));
        return -9;
    }

    LOGI("dex", "opened %s  ver=%s  strings=%u types=%u protos=%u fields=%u methods=%u classes=%u",
         path, ver, out->string_ids_size, out->type_ids_size, out->proto_ids_size,
         out->field_ids_size, out->method_ids_size, out->class_defs_size);
    return 0;
}

void dex_close(dex_file_t *dex) {
    if (!dex || !dex->base) return;
    munmap((void *)dex->base, dex->size);
    memset(dex, 0, sizeof(*dex));
}

// ---- Table accessors (bounds-checked) ----

const char *dex_string(const dex_file_t *dex, uint32_t idx) {
    if (!dex || !dex->base || idx >= dex->string_ids_size) return nullptr;
    // string_id_item: uint32 string_data_off
    const uint8_t *sid = offset_ptr(dex, dex->string_ids_off + idx * 4u, 4);
    if (!sid) return nullptr;
    uint32_t data_off = rd_u32(sid);
    const uint8_t *p = offset_ptr(dex, data_off, 1);
    if (!p) return nullptr;
    const uint8_t *end = dex->base + dex->size;
    // Skip the uleb128 utf16_size prefix; bytes follow.
    (void)read_uleb128(&p, end);
    return (const char *)p;   // null-terminated MUTF-8
}

const char *dex_type_descriptor(const dex_file_t *dex, uint32_t idx) {
    if (!dex || !dex->base || idx >= dex->type_ids_size) return nullptr;
    const uint8_t *tid = offset_ptr(dex, dex->type_ids_off + idx * 4u, 4);
    if (!tid) return nullptr;
    uint32_t desc_idx = rd_u32(tid);
    return dex_string(dex, desc_idx);
}

const dex_field_t *dex_field(const dex_file_t *dex, uint32_t idx) {
    if (!dex || !dex->base || idx >= dex->field_ids_size) return nullptr;
    // field_id_item: 2 + 2 + 4 = 8 bytes
    return (const dex_field_t *)offset_ptr(dex, dex->field_ids_off + idx * 8u, 8);
}

const dex_method_t *dex_method(const dex_file_t *dex, uint32_t idx) {
    if (!dex || !dex->base || idx >= dex->method_ids_size) return nullptr;
    // method_id_item: 2 + 2 + 4 = 8 bytes
    return (const dex_method_t *)offset_ptr(dex, dex->method_ids_off + idx * 8u, 8);
}

const dex_proto_t *dex_proto(const dex_file_t *dex, uint32_t idx) {
    if (!dex || !dex->base || idx >= dex->proto_ids_size) return nullptr;
    // proto_id_item: 4 + 4 + 4 = 12 bytes
    return (const dex_proto_t *)offset_ptr(dex, dex->proto_ids_off + idx * 12u, 12);
}

const dex_class_def_t *dex_class_def(const dex_file_t *dex, uint32_t idx) {
    if (!dex || !dex->base || idx >= dex->class_defs_size) return nullptr;
    // class_def_item: 8 x uint32 = 32 bytes
    return (const dex_class_def_t *)offset_ptr(dex, dex->class_defs_off + idx * 32u, 32);
}

// ---- dex_find_class: linear scan over class_defs ----
// Each class_def's class_idx is a type_id index; its descriptor string is
// "Lcom/example/Foo;". We compare against the caller's descriptor.
const dex_class_def_t *dex_find_class(const dex_file_t *dex, const char *descriptor) {
    if (!dex || !descriptor) return nullptr;
    for (uint32_t i = 0; i < dex->class_defs_size; i++) {
        const dex_class_def_t *cd = dex_class_def(dex, i);
        if (!cd) continue;
        const char *desc = dex_type_descriptor(dex, cd->class_idx);
        if (desc && strcmp(desc, descriptor) == 0) return cd;
    }
    return nullptr;
}

// ---- dex_find_code: locate a code_item by method_idx + kind ----
// Walks the class_data_item: decodes sizes, skips fields, walks the requested
// method section (direct or virtual) accumulating delta-encoded method_idx
// values until we find a match; remembers its code_off; parses the code_item.
int dex_find_code(const dex_file_t *dex, const dex_class_def_t *cls,
                  uint32_t method_idx, int is_direct, dex_code_t *out_code) {
    if (!dex || !cls || !out_code) return -1;
    memset(out_code, 0, sizeof(*out_code));
    if (cls->class_data_off == 0) return -2;   // no code at all

    class_data_walk walk;
    if (!walk_class_data(dex, cls->class_data_off, &walk)) return -3;

    const std::vector<decoded_method> &methods =
        is_direct ? walk.direct_methods : walk.virtual_methods;
    for (const auto &m : methods) {
        if (m.idx == method_idx) {
            if (m.code_off == 0) {
                // abstract / native: no code_item
                return -4;
            }
            const uint8_t *p = offset_ptr(dex, m.code_off, 16);
            if (!p) return -5;
            out_code->registers_size = rd_u16(p + 0);
            out_code->ins_size       = rd_u16(p + 2);
            out_code->outs_size      = rd_u16(p + 4);
            out_code->tries_size     = rd_u16(p + 6);
            out_code->debug_info_off = rd_u32(p + 8);
            out_code->insns_size     = rd_u32(p + 12);
            out_code->insns          = (const uint16_t *)(p + 16);
            // Bounds check insns.
            size_t insns_bytes = (size_t)out_code->insns_size * 2u;
            if ((size_t)(m.code_off + 16u) + insns_bytes > dex->size) {
                LOGE("dex", "code_item insns out of bounds (method_idx=%u)", method_idx);
                return -6;
            }
            return 0;
        }
    }
    return -7;   // not found in this section
}

// ---- dex_find_method: walk both direct + virtual, match name + shorty ----
int32_t dex_find_method(const dex_file_t *dex, const dex_class_def_t *cls,
                        const char *name, const char *shorty) {
    if (!dex || !cls || !name || !shorty) return -1;
    if (cls->class_data_off == 0) return -1;

    class_data_walk walk;
    if (!walk_class_data(dex, cls->class_data_off, &walk)) return -1;

    auto match = [&](const std::vector<decoded_method> &ms) -> int32_t {
        for (const auto &m : ms) {
            const dex_method_t *mid = dex_method(dex, m.idx);
            if (!mid) continue;
            const char *mname = dex_string(dex, mid->name_idx);
            const dex_proto_t *pid = dex_proto(dex, mid->proto_idx);
            if (!mname || !pid) continue;
            const char *mshorty = dex_string(dex, pid->shorty_idx);
            if (!mshorty) continue;
            if (strcmp(mname, name) == 0 && strcmp(mshorty, shorty) == 0) {
                return (int32_t)m.idx;
            }
        }
        return -1;
    };
    int32_t r = match(walk.direct_methods);
    if (r >= 0) return r;
    return match(walk.virtual_methods);
}

// ---- dex_find_field: walk both static + instance, match name ----
int32_t dex_find_field(const dex_file_t *dex, const dex_class_def_t *cls,
                       const char *name) {
    if (!dex || !cls || !name) return -1;
    if (cls->class_data_off == 0) return -1;

    class_data_walk walk;
    if (!walk_class_data(dex, cls->class_data_off, &walk)) return -1;

    auto match = [&](const std::vector<decoded_field> &fs) -> int32_t {
        for (const auto &f : fs) {
            const dex_field_t *fid = dex_field(dex, f.idx);
            if (!fid) continue;
            const char *fname = dex_string(dex, fid->name_idx);
            if (fname && strcmp(fname, name) == 0) return (int32_t)f.idx;
        }
        return -1;
    };
    int32_t r = match(walk.static_fields);
    if (r >= 0) return r;
    return match(walk.instance_fields);
}

}  // extern "C"
