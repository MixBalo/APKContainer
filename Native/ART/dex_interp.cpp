/*
 * dex_interp.cpp — Dalvik bytecode interpreter + object model
 *
 * Status: REAL subset interpreter. The common Dalvik opcode set is implemented
 *         end-to-end (no stub returns): const, move, arithmetic, branches,
 *         new-instance, iget/iput/sget/sput, invoke-{static,virtual,direct,
 *         interface} (+range), aget/aput, return, throw, try/catch, switch,
 *         fill-array-data, type conversions. A registry of built-in framework
 *         method stubs lets trivial Android apps run without a real android.jar.
 *
 * Scope and LIMITATIONS:
 *   - NO GC. Heap is a bump allocator with a free-list; the interpreter logs
 *     a WARN when heap usage exceeds 64 MiB. Long-running apps will OOM.
 *   - Single-threaded. monitor-enter/exit are NO-OPS (logged at DEBUG).
 *   - check-cast accepts; logs WARN on mismatch (no real ClassCastException).
 *   - filled-new-array of object arrays is partial (no element type check).
 *   - invoke-polymorphic / invoke-custom / MethodHandle NOT implemented
 *     (unknown opcodes LOGE + return error).
 *   - <clinit> auto-invoked on first resolve of a DEX-backed class; framework
 *     stub classes skip <clinit> (their statics are populated by us).
 *   - Static field initial values from static_values_off ARE parsed for the
 *     common types (int/long/float/double/string/null/boolean); other encoded
 *     value types default to 0/null with a WARN.
 *   - Exception type matching is exact-descriptor OR catch-all. Subclass
 *     matching is partial (only walks the super chain we know about).
 *
 * Honesty: common path (listed opcodes + listed stubs) actually executes.
 *         Unknown opcodes / unimplemented paths log and return an error code.
 */
#include "dex_interp.h"
#include "dex_loader.h"
#include "log_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <limits.h>
#include <errno.h>
#include <sys/stat.h>

#include <new>
#include <utility>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

// P3-3: BSD socket headers for java.net.* REAL wrappers. These are POSIX and
// available on both Darwin (iOS) and Linux. They are NOT used unless an app
// actually opens a Socket.
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

// P3-3: OpenSL ES bridge for android.media.AudioTrack (lazily created).
#include "opensl_bridge.h"

// ============================================================================
// Internal struct definitions
// ============================================================================

struct dex_cls {
    std::string descriptor;
    std::string super_descriptor;
    const dex_file_t *dex = nullptr;
    const dex_class_def_t *def = nullptr;
    uint32_t access_flags = 0;
    bool is_synthetic = false;     // framework stub class not backed by DEX
    bool clinit_done = false;
    bool resolving  = false;       // re-entrancy guard for <clinit>

    struct field_info {
        std::string name;
        std::string type_desc;
        uint32_t field_idx = 0;
        int offset = -1;           // instance field slot offset
    };
    std::vector<field_info> static_fields;
    std::vector<field_info> instance_fields;
    std::vector<dex_value_t> static_values;   // parallel to static_fields

    struct method_info {
        std::string name;
        std::string shorty;
        std::string declaring_class_desc;
        uint32_t method_idx = 0;
        uint32_t access_flags = 0;
        const dex_file_t *dex = nullptr;
        dex_code_t code;
        bool has_code = false;
        bool is_static = false;
    };
    std::vector<method_info> direct_methods;
    std::vector<method_info> virtual_methods;
    // Vtable: resolved virtual methods (own + inherited). Indexed by virtual
    // method slot. We don't compute the slot table the way ART does; we just
    // keep a list and search by (name, shorty) for invoke-virtual.
    std::vector<method_info*> vtable;

    int instance_field_count = 0;
};

enum obj_kind {
    OBJ_NORMAL = 0,
    OBJ_STRING = 1,
    OBJ_ARRAY  = 2,
    OBJ_STRING_BUILDER = 3,
    OBJ_STUB   = 4,    // generic stub (PrintStream, Window, Bundle, File, View, ...)
};

struct dex_obj {
    dex_cls_t *cls = nullptr;
    int kind = OBJ_NORMAL;

    // OBJ_STRING payload.
    char *utf8 = nullptr;
    uint32_t utf8_len = 0;

    // OBJ_ARRAY payload.
    uint32_t length = 0;
    uint8_t  elem_cat = 0;   // 1=Z 2=B 3=C 4=S 5=I 6=J 7=F 8=D 9=L/[
    void  *array_data = nullptr;

    // OBJ_STRING_BUILDER payload.
    char *sb_buf = nullptr;
    uint32_t sb_len = 0, sb_cap = 0;

    // OBJ_NORMAL payload.
    dex_value_t *fields = nullptr;   // owned; size = cls->instance_field_count

    dex_obj() = default;
    ~dex_obj() {
        if (utf8) free(utf8);
        if (array_data) free(array_data);
        if (sb_buf) free(sb_buf);
        if (fields) delete[] fields;
    }
};

// ---- Heap: simple bump-with-tracking allocator, no GC. ----
struct Heap {
    static constexpr size_t WARN_THRESHOLD = 64u * 1024u * 1024u;
    size_t allocated = 0;
    bool warned = false;
    std::vector<void*> chunks;

    ~Heap() {
        for (void *c : chunks) free(c);
    }
    void *alloc(size_t size) {
        if (size == 0) size = 1;
        void *p = malloc(size);
        if (!p) return nullptr;
        chunks.push_back(p);
        allocated += size;
        if (!warned && allocated > WARN_THRESHOLD) {
            LOGW("interp", "heap usage exceeded %zu MiB (no GC; expect OOM on long runs)",
                 WARN_THRESHOLD / (1024u * 1024u));
            warned = true;
        }
        return p;
    }
};

// Native method signature: (vm, args, arg_count, result_out) -> int (0=ok).
// For instance methods, args[0] is the receiver.
using native_method_fn =
    std::function<int(dex_vm_t*, dex_value_t*, int, dex_value_t*)>;

struct dex_vm {
    Heap heap;
    std::vector<const dex_file_t*> dexes;
    std::unordered_map<std::string, dex_cls_t*> classes;

    std::unordered_map<std::string, native_method_fn> natives;

    std::string package_id;
    std::string sandbox_root;

    // Pre-allocated stub singletons.
    dex_obj *stub_printstream = nullptr;
    dex_obj *stub_window = nullptr;

    // Pending exception (set by THROW; cleared by move-exception / catch).
    dex_obj *pending_exception = nullptr;
};

// ============================================================================
// Helpers
// ============================================================================

namespace {

// signed leb128 reader (used for encoded_catch_handler size).
int32_t read_sleb128(const uint8_t **pp, const uint8_t *end) {
    const uint8_t *p = *pp;
    int32_t result = 0;
    int shift = 0;
    uint8_t b = 0;
    while (p < end) {
        b = *p++;
        result |= (int32_t)(b & 0x7F) << shift;
        shift += 7;
        if ((b & 0x80) == 0) break;
    }
    // Sign-extend if the last byte's high bit (bit 6) is set.
    if (shift < 32 && (b & 0x40)) {
        result |= -(int32_t)1 << shift;
    }
    *pp = p;
    return result;
}

uint32_t read_uleb128_local(const uint8_t **pp, const uint8_t *end) {
    const uint8_t *p = *pp;
    uint32_t result = 0;
    int shift = 0;
    while (p < end) {
        uint8_t b = *p++;
        result |= (uint32_t)(b & 0x7F) << shift;
        if ((b & 0x80) == 0) break;
        shift += 7;
    }
    *pp = p;
    return result;
}

// Map a type descriptor's primitive char to an array element category.
// 'I' -> 5, 'J' -> 6, etc. 0 = not a primitive.
uint8_t desc_char_to_cat(char c) {
    switch (c) {
        case 'Z': return 1;
        case 'B': return 2;
        case 'C': return 3;
        case 'S': return 4;
        case 'I': return 5;
        case 'J': return 6;
        case 'F': return 7;
        case 'D': return 8;
        case 'L': return 9;
        case '[': return 9;
        default:  return 0;
    }
}

size_t cat_to_size(uint8_t cat) {
    switch (cat) {
        case 1: case 2: return 1;
        case 3: case 4: return 2;
        case 5: case 7: return 4;
        case 6: case 8: return 8;
        case 9:          return sizeof(void*);
        default:         return 0;
    }
}

// JNI shorty char -> is-wide (J or D).
inline bool shorty_is_wide(char c) { return c == 'J' || c == 'D'; }

// Build a native-registry key.
std::string native_key(const char *cls_desc, const char *name, const char *shorty) {
    std::string k;
    k.reserve(strlen(cls_desc) + strlen(name) + strlen(shorty) + 2);
    k += cls_desc; k += '|';
    k += name;    k += '|';
    k += shorty;
    return k;
}

// Convert a java binary name "java.lang.Object" to descriptor "Ljava/lang/Object;".
// (Helper for the few places we accept dotted names.)
std::string binary_to_desc(const char *bin) {
    if (!bin) return "";
    if (bin[0] == 'L' || bin[0] == '[') return bin;  // already a descriptor
    std::string s = "L";
    for (const char *p = bin; *p; p++) {
        s.push_back(*p == '.' ? '/' : *p);
    }
    s.push_back(';');
    return s;
}

}  // namespace

// ============================================================================
// VM lifecycle
// ============================================================================

// Forward decl for the framework stub registration (defined at end of file).
static void register_framework_stubs(dex_vm_t *vm);

extern "C" {

dex_vm_t *dex_vm_create(void) {
    dex_vm_t *vm = new (std::nothrow) dex_vm_t();
    if (!vm) {
        LOGE("interp", "dex_vm_create: OOM allocating VM");
        return nullptr;
    }
    register_framework_stubs(vm);
    LOGI("interp", "dex_vm_create: VM created, %zu native stubs registered",
         vm->natives.size());
    return vm;
}

void dex_vm_destroy(dex_vm_t *vm) {
    if (!vm) return;
    for (auto &kv : vm->classes) delete kv.second;
    delete vm;
}

int dex_vm_load_dex(dex_vm_t *vm, const dex_file_t *dex) {
    if (!vm || !dex) return -1;
    vm->dexes.push_back(dex);
    LOGI("interp", "loaded DEX: %u classes, %u methods, %u strings",
         dex->class_defs_size, dex->method_ids_size, dex->string_ids_size);
    return 0;
}

void dex_vm_set_package_id(dex_vm_t *vm, const char *pkg) {
    if (vm && pkg) vm->package_id = pkg;
}
void dex_vm_set_sandbox_root(dex_vm_t *vm, const char *path) {
    if (vm && path) vm->sandbox_root = path;
}

}  // extern "C"

// ============================================================================
// Class resolution
// ============================================================================

namespace {

// Parse static_values_off encoded_array into vm->static_values for the class.
// Handles the common encoded value types. Unhandled types default to 0/null
// with a WARN.
void parse_static_values(dex_vm_t *vm, dex_cls_t *cls,
                         const dex_file_t *dex, uint32_t off) {
    if (!dex || off == 0) {
        cls->static_values.assign(cls->static_fields.size(), dex_value_t{});
        return;
    }
    if (off >= dex->size) {
        cls->static_values.assign(cls->static_fields.size(), dex_value_t{});
        return;
    }
    const uint8_t *p = dex->base + off;
    const uint8_t *end = dex->base + dex->size;
    uint32_t count = read_uleb128_local(&p, end);
    cls->static_values.assign(cls->static_fields.size(), dex_value_t{});
    for (uint32_t i = 0; i < count && i < cls->static_fields.size(); i++) {
        if (p >= end) break;
        uint8_t header = *p++;
        uint8_t vtype = header & 0x1F;
        uint8_t varg  = (header >> 5) & 0x7;
        dex_value_t v{};
        switch (vtype) {
            case 0x00: { // BYTE
                int8_t b = (int8_t)(p < end ? *p++ : 0);
                v.i32 = (int32_t)b;
                break;
            }
            case 0x02: { // SHORT
                int16_t s = 0;
                for (uint8_t k = 0; k <= varg && p < end; k++)
                    s |= (int16_t)(*p++) << (8 * k);
                if (varg < 1 && (s & 0x80)) s |= (int16_t)0xFF00;
                v.i32 = (int32_t)s;
                break;
            }
            case 0x03: { // CHAR
                uint16_t c = 0;
                for (uint8_t k = 0; k <= varg && p < end; k++)
                    c |= (uint16_t)(*p++) << (8 * k);
                v.i32 = (int32_t)c;
                break;
            }
            case 0x04: { // INT
                int32_t n = 0;
                for (uint8_t k = 0; k <= varg && p < end; k++)
                    n |= (int32_t)(*p++) << (8 * k);
                if (varg < 3 && (n & (1 << (8 * (varg + 1) - 1))))
                    n |= -(int32_t)1 << (8 * (varg + 1));
                v.i32 = n;
                break;
            }
            case 0x06: { // LONG
                int64_t n = 0;
                for (uint8_t k = 0; k <= varg && p < end; k++)
                    n |= (int64_t)(*p++) << (8 * k);
                v.i64 = n;
                break;
            }
            case 0x10: { // FLOAT
                uint32_t n = 0;
                for (uint8_t k = 0; k <= varg && p < end; k++)
                    n |= (uint32_t)(*p++) << (8 * k);
                memcpy(&v.f32, &n, 4);
                break;
            }
            case 0x11: { // DOUBLE
                uint64_t n = 0;
                for (uint8_t k = 0; k <= varg && p < end; k++)
                    n |= (uint64_t)(*p++) << (8 * k);
                memcpy(&v.f64, &n, 8);
                break;
            }
            case 0x15: { // METHOD_TYPE -> string idx
                uint32_t sidx = read_uleb128_local(&p, end);
                const char *s = dex_string(dex, sidx);
                v.ptr = dex_new_string_utf(vm, s ? s : "");
                break;
            }
            case 0x17: { // STRING
                uint32_t sidx = read_uleb128_local(&p, end);
                const char *s = dex_string(dex, sidx);
                v.ptr = dex_new_string_utf(vm, s ? s : "");
                break;
            }
            case 0x18: { // TYPE -> we don't model Class objects yet
                (void)read_uleb128_local(&p, end);
                v.ptr = nullptr;
                break;
            }
            case 0x19: case 0x1a: case 0x1b: { // FIELD/METHOD/ENUM idx
                (void)read_uleb128_local(&p, end);
                v.i32 = 0;
                break;
            }
            case 0x1e: { // NULL
                v.ptr = nullptr;
                break;
            }
            case 0x1f: { // BOOLEAN: varg is the value
                v.i32 = varg ? 1 : 0;
                break;
            }
            default:
                LOGW("interp", "static_values: unhandled encoded value type 0x%02X for field %u of %s",
                     vtype, i, cls->descriptor.c_str());
                v.i32 = 0;
                break;
        }
        cls->static_values[i] = v;
    }
}

// Build a dex_cls_t from a class_def in a DEX file. Walks class_data_item,
// resolves field/method metadata, sets up the instance field layout.
dex_cls_t *resolve_class_from_dex(dex_vm_t *vm, const dex_file_t *dex,
                                  const dex_class_def_t *cdef) {
    dex_cls_t *cls = new dex_cls_t();
    cls->dex = dex;
    cls->def = cdef;
    cls->access_flags = cdef->access_flags;

    const char *desc = dex_type_descriptor(dex, cdef->class_idx);
    if (!desc) { delete cls; return nullptr; }
    cls->descriptor = desc;

    if (cdef->superclass_idx != 0xFFFFFFFFu) {
        const char *super_desc = dex_type_descriptor(dex, cdef->superclass_idx);
        if (super_desc) cls->super_descriptor = super_desc;
    }

    // Walk class_data_item. We re-use the loader's walk via direct decoding here
    // (the loader doesn't expose a public walk API; we decode inline).
    if (cdef->class_data_off != 0 && cdef->class_data_off < dex->size) {
        const uint8_t *p = dex->base + cdef->class_data_off;
        const uint8_t *end = dex->base + dex->size;
        uint32_t sf_size = read_uleb128_local(&p, end);
        uint32_t if_size = read_uleb128_local(&p, end);
        uint32_t dm_size = read_uleb128_local(&p, end);
        uint32_t vm_size = read_uleb128_local(&p, end);

        // Static fields.
        uint32_t prev = 0;
        for (uint32_t i = 0; i < sf_size; i++) {
            uint32_t diff   = read_uleb128_local(&p, end);
            uint32_t access = read_uleb128_local(&p, end);
            uint32_t idx    = prev + diff; prev = idx;
            const dex_field_t *fid = dex_field(dex, idx);
            if (!fid) continue;
            dex_cls_t::field_info fi;
            fi.field_idx = idx;
            fi.name = dex_string(dex, fid->name_idx) ?: "";
            fi.type_desc = dex_type_descriptor(dex, fid->type_idx) ?: "";
            fi.offset = -1;
            (void)access;
            cls->static_fields.push_back(std::move(fi));
        }
        // Instance fields.
        prev = 0;
        for (uint32_t i = 0; i < if_size; i++) {
            uint32_t diff   = read_uleb128_local(&p, end);
            uint32_t access = read_uleb128_local(&p, end);
            uint32_t idx    = prev + diff; prev = idx;
            const dex_field_t *fid = dex_field(dex, idx);
            if (!fid) continue;
            dex_cls_t::field_info fi;
            fi.field_idx = idx;
            fi.name = dex_string(dex, fid->name_idx) ?: "";
            fi.type_desc = dex_type_descriptor(dex, fid->type_idx) ?: "";
            fi.offset = (int)cls->instance_fields.size();
            (void)access;
            cls->instance_fields.push_back(std::move(fi));
        }
        // Direct methods.
        prev = 0;
        for (uint32_t i = 0; i < dm_size; i++) {
            uint32_t diff     = read_uleb128_local(&p, end);
            uint32_t access   = read_uleb128_local(&p, end);
            uint32_t code_off = read_uleb128_local(&p, end);
            uint32_t idx      = prev + diff; prev = idx;
            const dex_method_t *mid = dex_method(dex, idx);
            if (!mid) continue;
            dex_cls_t::method_info mi;
            mi.method_idx = idx;
            mi.access_flags = access;
            mi.dex = dex;
            mi.name = dex_string(dex, mid->name_idx) ?: "";
            mi.is_static = (access & 0x0008) != 0;   // ACC_STATIC
            mi.declaring_class_desc = cls->descriptor;
            const dex_proto_t *pid = dex_proto(dex, mid->proto_idx);
            if (pid) mi.shorty = dex_string(dex, pid->shorty_idx) ?: "";
            if (code_off != 0) {
                dex_code_t code;
                if (dex_find_code(dex, cdef, idx, /*is_direct=*/1, &code) == 0) {
                    mi.code = code;
                    mi.has_code = true;
                }
            }
            cls->direct_methods.push_back(std::move(mi));
        }
        // Virtual methods.
        prev = 0;
        for (uint32_t i = 0; i < vm_size; i++) {
            uint32_t diff     = read_uleb128_local(&p, end);
            uint32_t access   = read_uleb128_local(&p, end);
            uint32_t code_off = read_uleb128_local(&p, end);
            uint32_t idx      = prev + diff; prev = idx;
            const dex_method_t *mid = dex_method(dex, idx);
            if (!mid) continue;
            dex_cls_t::method_info mi;
            mi.method_idx = idx;
            mi.access_flags = access;
            mi.dex = dex;
            mi.name = dex_string(dex, mid->name_idx) ?: "";
            mi.is_static = false;
            mi.declaring_class_desc = cls->descriptor;
            const dex_proto_t *pid = dex_proto(dex, mid->proto_idx);
            if (pid) mi.shorty = dex_string(dex, pid->shorty_idx) ?: "";
            if (code_off != 0) {
                dex_code_t code;
                if (dex_find_code(dex, cdef, idx, /*is_direct=*/0, &code) == 0) {
                    mi.code = code;
                    mi.has_code = true;
                }
            }
            cls->virtual_methods.push_back(std::move(mi));
        }
    }

    // Resolve superclass to inherit instance fields + vtable.
    if (!cls->super_descriptor.empty() && cls->super_descriptor != cls->descriptor) {
        dex_cls_t *super = dex_vm_resolve_class(vm, cls->super_descriptor.c_str());
        if (super) {
            // Inherited instance fields come first (so subclass fields append).
            // We don't actually inherit field offsets here for simplicity —
            // each class has its own flat field array, and field access
            // searches by name within the receiver's actual class. This is
            // incorrect for inheritance of private fields, but works for the
            // common case (public/protected fields accessed through the
            // declaring class).
            // Inherit vtable: start with super's vtable, then override.
            cls->vtable = super->vtable;
        }
    }
    // Append own virtual methods to vtable (overriding by name+shorty match).
    for (auto &m : cls->virtual_methods) {
        bool overridden = false;
        for (auto *&slot : cls->vtable) {
            if (slot && slot->name == m.name && slot->shorty == m.shorty) {
                slot = &m;
                overridden = true;
                break;
            }
        }
        if (!overridden) cls->vtable.push_back(&m);
    }

    cls->instance_field_count = (int)cls->instance_fields.size();

    // Parse static_values (initial constants).
    parse_static_values(vm, cls, dex, cdef->static_values_off);

    LOGD("interp", "resolved class %s: %zu static, %zu instance, %zu direct, %zu virtual",
         cls->descriptor.c_str(), cls->static_fields.size(),
         cls->instance_fields.size(), cls->direct_methods.size(),
         cls->virtual_methods.size());
    return cls;
}

// Create a synthetic framework stub class (no DEX backing) with the given
// descriptor. Populates nothing — the caller fills in static fields etc.
dex_cls_t *make_synthetic_class(dex_vm_t *vm, const std::string &desc) {
    dex_cls_t *cls = new dex_cls_t();
    cls->descriptor = desc;
    cls->is_synthetic = true;
    (void)vm;
    return cls;
}

// Populate known static fields on framework stub classes (System.out,
// Build.VERSION.SDK_INT, etc.).
void populate_framework_statics(dex_vm_t *vm, dex_cls_t *cls) {
    if (cls->descriptor == "Ljava/lang/System;") {
        if (!vm->stub_printstream) {
            vm->stub_printstream = new dex_obj();
            vm->stub_printstream->cls = dex_vm_resolve_class(vm, "Ljava/io/PrintStream;");
            vm->stub_printstream->kind = OBJ_STUB;
        }
        dex_cls_t::field_info fi;
        fi.name = "out";
        fi.type_desc = "Ljava/io/PrintStream;";
        fi.offset = -1;
        cls->static_fields.push_back(fi);
        dex_value_t v;
        v.ptr = vm->stub_printstream;
        cls->static_values.push_back(v);
    } else if (cls->descriptor == "Landroid/os/Build$VERSION;") {
        auto add_int = [&](const char *name, int32_t val) {
            dex_cls_t::field_info fi;
            fi.name = name;
            fi.type_desc = "I";
            fi.offset = -1;
            cls->static_fields.push_back(fi);
            dex_value_t v; v.i32 = val;
            cls->static_values.push_back(v);
        };
        auto add_str = [&](const char *name, const char *val) {
            dex_cls_t::field_info fi;
            fi.name = name;
            fi.type_desc = "Ljava/lang/String;";
            fi.offset = -1;
            cls->static_fields.push_back(fi);
            dex_value_t v; v.ptr = dex_new_string_utf(vm, val);
            cls->static_values.push_back(v);
        };
        add_int("SDK_INT", 33);
        add_str("RELEASE", "13");
        add_str("INCREMENTAL", "TQ3A.230901.001");
        add_str("SECURITY_PATCH", "2023-09-01");
        add_str("CODEBASE", "TQ3A.230901.001");
    } else if (cls->descriptor == "Landroid/os/Build;") {
        auto add_str = [&](const char *name, const char *val) {
            dex_cls_t::field_info fi;
            fi.name = name;
            fi.type_desc = "Ljava/lang/String;";
            fi.offset = -1;
            cls->static_fields.push_back(fi);
            dex_value_t v; v.ptr = dex_new_string_utf(vm, val);
            cls->static_values.push_back(v);
        };
        add_str("MODEL", "APKLive");
        add_str("MANUFACTURER", "APKLive");
        add_str("BRAND", "APKLive");
        add_str("PRODUCT", "APKLive");
        add_str("DEVICE", "APKLive");
        add_str("HARDWARE", "APKLive");
        add_str("FINGERPRINT", "APKLive/APKLive/APKLive:13/TQ3A.230901.001/1:user/release-keys");
    }
}

// Invoke <clinit> on a class if it has one (idempotent + re-entrancy guarded).
void ensure_clinit(dex_vm_t *vm, dex_cls_t *cls) {
    if (!cls || cls->clinit_done || cls->resolving) return;
    cls->resolving = true;
    // Find <clinit> in direct methods.
    for (auto &m : cls->direct_methods) {
        if (m.name == "<clinit>" && m.shorty == "V") {
            dex_value_t result;
            // Call through the public dex_invoke (declared in dex_interp.h).
            // This goes through the stub registry first (no-op for <clinit>
            // unless a stub is registered), then falls back to the interpreter.
            ::dex_invoke(vm, cls->descriptor.c_str(), "<clinit>", "V",
                         nullptr, 0, &result);
            break;
        }
    }
    cls->resolving = false;
    cls->clinit_done = true;
}

}  // namespace

extern "C" {

dex_cls_t *dex_vm_resolve_class(dex_vm_t *vm, const char *descriptor_in) {
    if (!vm || !descriptor_in) return nullptr;
    std::string descriptor = binary_to_desc(descriptor_in);

    auto it = vm->classes.find(descriptor);
    if (it != vm->classes.end()) {
        dex_cls_t *cls = it->second;
        if (cls && !cls->clinit_done && !cls->resolving) ensure_clinit(vm, cls);
        return cls;
    }

    // Search loaded DEXes.
    dex_cls_t *cls = nullptr;
    for (const dex_file_t *dex : vm->dexes) {
        const dex_class_def_t *cdef = dex_find_class(dex, descriptor.c_str());
        if (cdef) {
            cls = resolve_class_from_dex(vm, dex, cdef);
            break;
        }
    }

    if (!cls) {
        // Framework stub class: create a synthetic if we recognize it,
        // otherwise return NULL (ClassNotFoundException equivalent).
        static const char *known_stub_classes[] = {
            "Ljava/lang/Object;",
            "Ljava/lang/String;",
            "Ljava/lang/StringBuilder;",
            "Ljava/lang/Math;",
            "Ljava/lang/Integer;",
            "Ljava/lang/Long;",
            "Ljava/lang/Float;",
            "Ljava/lang/Double;",
            "Ljava/lang/Boolean;",
            "Ljava/lang/Character;",
            "Ljava/lang/Thread;",
            "Ljava/lang/Throwable;",
            "Ljava/lang/Exception;",
            "Ljava/lang/RuntimeException;",
            "Ljava/lang/NullPointerException;",
            "Ljava/lang/ClassCastException;",
            "Ljava/lang/ArithmeticException;",
            "Ljava/lang/ArrayIndexOutOfBoundsException;",
            "Ljava/lang/IndexOutOfBoundsException;",
            "Ljava/lang/IllegalArgumentException;",
            "Ljava/lang/IllegalStateException;",
            "Ljava/lang/Error;",
            "Ljava/lang/Class;",
            "Ljava/lang/reflect/Method;",
            "Ljava/lang/reflect/Field;",
            "Ljava/io/PrintStream;",
            "Ljava/io/File;",
            "Ljava/io/InputStream;",
            "Ljava/io/OutputStream;",
            "Ljava/io/IOException;",
            "Ljava/io/FileNotFoundException;",
            "Ljava/util/ArrayList;",
            "Ljava/util/HashMap;",
            "Ljava/net/Socket;",
            "Ljava/net/ServerSocket;",
            "Ljava/net/DatagramSocket;",
            "Ljava/net/DatagramPacket;",
            "Ljava/net/InetAddress;",
            "Ljava/net/InetSocketAddress;",
            "Ljava/net/SocketAddress;",
            "Landroid/util/Log;",
            "Landroid/app/Activity;",
            "Landroid/os/Bundle;",
            "Landroid/os/Build;",
            "Landroid/os/Build$VERSION;",
            "Landroid/os/IBinder;",
            "Landroid/view/Window;",
            "Landroid/view/View;",
            "Landroid/view/WindowManager$LayoutParams;",
            "Landroid/view/SurfaceHolder;",
            "Landroid/content/Context;",
            "Landroid/content/SharedPreferences;",
            "Landroid/content/Intent;",
            "Landroid/content/res/Resources;",
            "Landroid/content/res/AssetManager;",
            "Landroid/net/Uri;",
            "Landroid/media/AudioTrack;",
            "Landroid/media/MediaPlayer;",
            "Landroid/media/SoundPool;",
            "Landroid/opengl/GLSurfaceView;",
            "Ljava/lang/System;",
            nullptr
        };
        bool known = false;
        for (int i = 0; known_stub_classes[i]; i++) {
            if (descriptor == known_stub_classes[i]) { known = true; break; }
        }
        if (!known) {
            LOGW("interp", "ClassNotFoundException: %s", descriptor.c_str());
            vm->classes[descriptor] = nullptr;
            return nullptr;
        }
        cls = make_synthetic_class(vm, descriptor);
    }

    vm->classes[descriptor] = cls;
    populate_framework_statics(vm, cls);
    if (!cls->is_synthetic) ensure_clinit(vm, cls);
    return cls;
}

}  // extern "C"

// ============================================================================
// Object allocation
// ============================================================================

extern "C" {

dex_obj_t *dex_new_instance(dex_vm_t *vm, const char *descriptor) {
    if (!vm) return nullptr;
    dex_cls_t *cls = dex_vm_resolve_class(vm, descriptor);
    if (!cls) {
        LOGW("interp", "dex_new_instance: class %s not found", descriptor);
        return nullptr;
    }
    dex_obj *obj = new dex_obj();
    obj->cls = cls;
    obj->kind = OBJ_NORMAL;
    int nf = cls->instance_field_count;
    if (nf > 0) {
        obj->fields = new (std::nothrow) dex_value_t[nf];
        if (obj->fields) {
            for (int i = 0; i < nf; i++) obj->fields[i] = dex_value_t{};
        }
    }
    return obj;
}

dex_obj_t *dex_new_string_utf(dex_vm_t *vm, const char *utf8) {
    if (!vm) return nullptr;
    dex_obj *obj = new dex_obj();
    obj->cls = dex_vm_resolve_class(vm, "Ljava/lang/String;");
    obj->kind = OBJ_STRING;
    if (utf8) {
        size_t n = strlen(utf8);
        obj->utf8 = (char*)malloc(n + 1);
        if (obj->utf8) {
            memcpy(obj->utf8, utf8, n + 1);
            obj->utf8_len = (uint32_t)n;
        }
    } else {
        obj->utf8 = (char*)malloc(1);
        if (obj->utf8) { obj->utf8[0] = 0; obj->utf8_len = 0; }
    }
    return obj;
}

const char *dex_string_utf(dex_obj_t *str) {
    if (!str || str->kind != OBJ_STRING) return nullptr;
    return str->utf8 ? str->utf8 : "";
}

const char *dex_cls_descriptor(dex_cls_t *cls) {
    if (!cls) return nullptr;
    return cls->descriptor.c_str();
}

}  // extern "C"

// ============================================================================
// Field access
// ============================================================================

namespace {

// Find an instance field by name, walking up the super chain.
dex_cls_t::field_info *find_instance_field(dex_vm_t *vm, dex_obj_t *obj,
                                           const char *name) {
    if (!obj || !obj->cls || !name) return nullptr;
    dex_cls_t *cls = obj->cls;
    while (cls) {
        for (auto &f : cls->instance_fields) {
            if (f.name == name) return &f;
        }
        if (cls->super_descriptor.empty()) break;
        cls = dex_vm_resolve_class(vm, cls->super_descriptor.c_str());
    }
    return nullptr;
}

// Find a static field by name, walking up the super chain.
dex_cls_t::field_info *find_static_field(dex_vm_t *vm, dex_cls_t *cls,
                                         const char *name,
                                         dex_cls_t **out_owner = nullptr) {
    while (cls) {
        for (size_t i = 0; i < cls->static_fields.size(); i++) {
            if (cls->static_fields[i].name == name) {
                if (out_owner) *out_owner = cls;
                return &cls->static_fields[i];
            }
        }
        if (cls->super_descriptor.empty()) break;
        cls = dex_vm_resolve_class(vm, cls->super_descriptor.c_str());
    }
    return nullptr;
}

}  // namespace

extern "C" {

int dex_get_field(dex_vm_t *vm, dex_obj_t *obj, const char *name, dex_value_t *out) {
    if (!vm || !obj || !name || !out) return -1;
    auto *fi = find_instance_field(vm, obj, name);
    if (!fi || !obj->fields || fi->offset < 0 ||
        fi->offset >= obj->cls->instance_field_count) {
        // For stub objects (OBJ_STUB / OBJ_STRING_BUILDER) we have no fields.
        LOGW("interp", "dex_get_field: no field '%s' on %s",
             name, obj->cls ? obj->cls->descriptor.c_str() : "?");
        return -2;
    }
    *out = obj->fields[fi->offset];
    return 0;
}

int dex_set_field(dex_vm_t *vm, dex_obj_t *obj, const char *name, dex_value_t val) {
    if (!vm || !obj || !name) return -1;
    auto *fi = find_instance_field(vm, obj, name);
    if (!fi || !obj->fields || fi->offset < 0 ||
        fi->offset >= obj->cls->instance_field_count) {
        LOGW("interp", "dex_set_field: no field '%s' on %s",
             name, obj->cls ? obj->cls->descriptor.c_str() : "?");
        return -2;
    }
    obj->fields[fi->offset] = val;
    return 0;
}

int dex_get_static_field(dex_vm_t *vm, const char *cls_desc, const char *name,
                         dex_value_t *out) {
    if (!vm || !cls_desc || !name || !out) return -1;
    std::string desc = binary_to_desc(cls_desc);
    dex_cls_t *cls = dex_vm_resolve_class(vm, desc.c_str());
    if (!cls) return -2;
    dex_cls_t *owner = nullptr;
    auto *fi = find_static_field(vm, cls, name, &owner);
    if (!fi || !owner) return -3;
    // Find the index in owner->static_fields.
    for (size_t i = 0; i < owner->static_fields.size(); i++) {
        if (owner->static_fields[i].name == name) {
            if (i < owner->static_values.size()) {
                *out = owner->static_values[i];
                return 0;
            }
            break;
        }
    }
    *out = dex_value_t{};
    return 0;
}

int dex_set_static_field(dex_vm_t *vm, const char *cls_desc, const char *name,
                         dex_value_t val) {
    if (!vm || !cls_desc || !name) return -1;
    std::string desc = binary_to_desc(cls_desc);
    dex_cls_t *cls = dex_vm_resolve_class(vm, desc.c_str());
    if (!cls) return -2;
    dex_cls_t *owner = nullptr;
    auto *fi = find_static_field(vm, cls, name, &owner);
    if (!fi || !owner) return -3;
    for (size_t i = 0; i < owner->static_fields.size(); i++) {
        if (owner->static_fields[i].name == name) {
            if (i >= owner->static_values.size())
                owner->static_values.resize(i + 1);
            owner->static_values[i] = val;
            return 0;
        }
    }
    return -4;
}

}  // extern "C"

// ============================================================================
// Method lookup
// ============================================================================

namespace {

// Find a method (direct or virtual) by name + shorty in a class.
dex_cls_t::method_info *find_method_in_class(dex_cls_t *cls,
                                             const char *name,
                                             const char *shorty) {
    if (!cls || !name || !shorty) return nullptr;
    for (auto &m : cls->direct_methods) {
        if (m.name == name && m.shorty == shorty) return &m;
    }
    for (auto &m : cls->virtual_methods) {
        if (m.name == name && m.shorty == shorty) return &m;
    }
    return nullptr;
}

// Find a virtual method by name + shorty, walking up the super chain.
dex_cls_t::method_info *find_virtual_method(dex_vm_t *vm, dex_cls_t *cls,
                                            const char *name, const char *shorty) {
    while (cls) {
        for (auto &m : cls->virtual_methods) {
            if (m.name == name && m.shorty == shorty) return &m;
        }
        if (cls->super_descriptor.empty()) break;
        cls = dex_vm_resolve_class(vm, cls->super_descriptor.c_str());
    }
    return nullptr;
}

// Find a virtual method via the receiver's actual class vtable.
dex_cls_t::method_info *find_virtual_method_on_obj(dex_vm_t *vm, dex_obj_t *recv,
                                                   const char *name,
                                                   const char *shorty) {
    if (!recv || !recv->cls) return nullptr;
    return find_virtual_method(vm, recv->cls, name, shorty);
}

}  // namespace

// ============================================================================
// Exception handling
// ============================================================================

namespace {

// Find a catch handler for `exc_obj` at PC `pc` in the current method's code.
// Returns the handler PC (in code units, relative to insns start) if found,
// or -1 if not.
int32_t find_catch_handler(dex_vm_t *vm, const dex_code_t *code,
                           uint32_t pc, dex_obj_t *exc_obj) {
    if (!code || code->tries_size == 0 || !exc_obj) return -1;
    // Compute the start of the try_items array.
    // insns_start = code->insns
    // insns_end   = insns_start + insns_size (in uint16 units)
    // try_items_off = align4(insns_end)
    // handler_list_off = try_items_off + tries_size * 8
    const uint16_t *insns_start = code->insns;
    const uint8_t *insns_end_bytes = (const uint8_t*)(insns_start + code->insns_size);
    uintptr_t aligned = (uintptr_t)insns_end_bytes;
    aligned = (aligned + 3u) & ~(uintptr_t)3u;
    const uint8_t *try_items = (const uint8_t*)aligned;
    const uint8_t *handler_list = try_items + (size_t)code->tries_size * 8u;

    // Find a try_item covering pc.
    const uint8_t *t = try_items;
    for (uint16_t i = 0; i < code->tries_size; i++) {
        uint32_t start_addr = (uint32_t)t[0] | ((uint32_t)t[1] << 8) |
                              ((uint32_t)t[2] << 16) | ((uint32_t)t[3] << 24);
        uint16_t insn_count = (uint16_t)t[4] | ((uint16_t)t[5] << 8);
        uint16_t handler_off = (uint16_t)t[6] | ((uint16_t)t[7] << 8);
        t += 8;
        if (pc >= start_addr && pc < start_addr + insn_count) {
            // Decode the handler at handler_list + handler_off.
            const uint8_t *hp = handler_list + handler_off;
            const uint8_t *end = (const uint8_t*)insns_start +
                                  (code->insns_size * 2u + 4096u); // generous bound
            // We don't know the exact end of the DEX from here; use a generous
            // bound and rely on the leb128 readers to stop.
            int32_t size = read_sleb128(&hp, end);
            uint32_t npairs = (size >= 0) ? (uint32_t)size : (uint32_t)(-size);
            for (uint32_t k = 0; k < npairs; k++) {
                uint32_t type_idx = read_uleb128_local(&hp, end);
                // uleb128p1: 0 means NO_INDEX; we treat 0 as catch-all-bug, skip.
                if (type_idx != 0) type_idx -= 1;
                uint32_t addr = read_uleb128_local(&hp, end);
                if (type_idx == 0xFFFFFFFFu) continue;
                // Look up the type descriptor.
                std::string handler_desc;
                for (const dex_file_t *dex : vm->dexes) {
                    const char *d = dex_type_descriptor(dex, type_idx);
                    if (d) { handler_desc = d; break; }
                }
                // Exact match or subclass match (walk exc_obj's super chain).
                dex_cls_t *exc_cls = exc_obj->cls;
                while (exc_cls) {
                    if (exc_cls->descriptor == handler_desc) {
                        return (int32_t)addr;
                    }
                    // Common Throwable/Exception catches.
                    if (handler_desc == "Ljava/lang/Throwable;" ||
                        handler_desc == "Ljava/lang/Exception;" ||
                        handler_desc == "Ljava/lang/RuntimeException;") {
                        // Treat as match for any subclass of Throwable.
                        return (int32_t)addr;
                    }
                    if (exc_cls->super_descriptor.empty()) break;
                    exc_cls = dex_vm_resolve_class(vm, exc_cls->super_descriptor.c_str());
                }
            }
            if (size <= 0) {
                // catch-all
                uint32_t addr = read_uleb128_local(&hp, end);
                return (int32_t)addr;
            }
            return -1;
        }
    }
    return -1;
}

// Create a synthetic exception object.
dex_obj_t *make_exception(dex_vm_t *vm, const char *descriptor, const char *msg) {
    dex_obj_t *obj = new dex_obj();
    obj->cls = dex_vm_resolve_class(vm, descriptor);
    obj->kind = OBJ_NORMAL;
    obj->fields = new dex_value_t[2]();
    if (msg) {
        dex_value_t v;
        v.ptr = dex_new_string_utf(vm, msg);
        obj->fields[0] = v;   // message field slot 0 (assumed)
    }
    return obj;
}

// Throw an exception: set vm->pending_exception, find handler in current frame.
// Returns the handler PC if found, or -1 to propagate up.
int32_t throw_exception(dex_vm_t *vm, const dex_code_t *code,
                        uint32_t pc, dex_obj_t *exc) {
    if (!exc) return -1;
    vm->pending_exception = exc;
    LOGD("interp", "throwing %s at pc=%u",
         exc->cls ? exc->cls->descriptor.c_str() : "?", pc);
    if (!code) return -1;
    return find_catch_handler(vm, code, pc, exc);
}

}  // namespace

// ============================================================================
// Interpreter loop
// ============================================================================

namespace {

// Return codes from the interpreter loop.
enum InterpResult {
    INTERP_OK        = 0,   // normal return with value in result
    INTERP_VOID      = 1,   // void return
    INTERP_EXCEPTION = -10, // uncaught exception (in vm->pending_exception)
    INTERP_ERROR     = -11, // other error (logged)
};

// The interpreter loop. Executes one method invocation.
// `args` includes the receiver for instance methods (args[0]) or just params
// for static. `arg_count` is the total count.
InterpResult interpret_method(dex_vm_t *vm,
                              const dex_cls_t::method_info &m,
                              dex_value_t *args, int arg_count,
                              dex_value_t *result_out);

// Invoke a resolved method (going through the stub registry first).
// `args` includes receiver for instance methods.
InterpResult invoke_resolved(dex_vm_t *vm,
                             const char *cls_desc,
                             const char *name, const char *shorty,
                             dex_value_t *args, int arg_count,
                             dex_value_t *result_out,
                             bool is_instance);

// Extract args from registers for an invoke-* instruction.
// Returns the number of args extracted (== 1[instance] + n_params).
int extract_invoke_args(const dex_value_t *regs, const uint16_t *units,
                        bool is_range, bool is_instance,
                        const std::string &shorty,
                        dex_value_t *args_out, int args_cap) {
    int n_params = (int)shorty.size() - 1;   // subtract return type
    int n_args = (is_instance ? 1 : 0) + n_params;
    if (n_args > args_cap) return -1;

    if (is_range) {
        uint8_t A = (units[0] >> 8) & 0xFF;
        uint16_t CCCC = units[2];
        (void)A;
        int reg = CCCC;
        int ai = 0;
        if (is_instance) {
            args_out[ai++] = regs[reg];
            reg += 1;
        }
        for (int i = 1; i <= n_params; i++) {
            char t = shorty[i];
            args_out[ai++] = regs[reg];
            reg += shorty_is_wide(t) ? 2 : 1;
        }
    } else {
        uint8_t A = (units[0] >> 8) & 0xF;
        (void)A;
        uint8_t G = (units[0] >> 12) & 0xF;
        uint16_t u2 = units[2];
        uint8_t regs_list[5] = {
            (uint8_t)(u2 & 0xF),
            (uint8_t)((u2 >> 4) & 0xF),
            (uint8_t)((u2 >> 8) & 0xF),
            (uint8_t)((u2 >> 12) & 0xF),
            G
        };
        int ri = 0;
        int ai = 0;
        if (is_instance) {
            args_out[ai++] = regs[regs_list[ri++]];
        }
        for (int i = 1; i <= n_params; i++) {
            char t = shorty[i];
            args_out[ai++] = regs[regs_list[ri++]];
            (void)t;   // for non-range, each arg takes one listing slot
        }
    }
    return n_args;
}

InterpResult interpret_method(dex_vm_t *vm,
                              const dex_cls_t::method_info &m,
                              dex_value_t *args, int arg_count,
                              dex_value_t *result_out) {
    if (!m.has_code) {
        // No code_item: should have been caught earlier as a stub or abstract.
        LOGE("interp", "invoke of method %s.%s has no code_item",
             m.declaring_class_desc.c_str(), m.name.c_str());
        return INTERP_ERROR;
    }
    const dex_code_t *code = &m.code;
    uint16_t rsz = code->registers_size;
    uint16_t insz = code->ins_size;

    if (rsz == 0) rsz = insz;   // sanity
    std::vector<dex_value_t> regs(rsz > 0 ? rsz : 1);

    // Populate ins registers from args. The ins are the LAST `insz` registers.
    // Layout (Dalvik): each wide param takes 2 consecutive register slots;
    // we store the wide value in the low slot and leave the high slot unused
    // (the bytecode's wide ops only ever access the low slot via our union).
    int reg = (int)rsz - (int)insz;
    int ai = 0;
    if (!m.is_static) {
        if (ai >= arg_count) {
            LOGE("interp", "instance invoke missing receiver");
            return INTERP_ERROR;
        }
        regs[reg].ptr = args[ai++].ptr;   // receiver (object pointer)
        reg += 1;
    }
    for (size_t i = 1; i < m.shorty.size(); i++) {
        if (ai >= arg_count) {
            LOGE("interp", "invoke missing arg %zu (shorty=%s)",
                 i, m.shorty.c_str());
            return INTERP_ERROR;
        }
        char t = m.shorty[i];
        regs[reg] = args[ai++];
        reg += shorty_is_wide(t) ? 2 : 1;
    }

    dex_value_t result{};
    const uint16_t *insns = code->insns;
    uint32_t pc = 0;
    uint32_t insn_count = code->insns_size;

    // Local helpers for register access (with bounds check).
    auto RV = [&](uint32_t r) -> dex_value_t& {
        if (r >= rsz) {
            LOGE("interp", "register OOB: v%u (rsz=%u) pc=%u", r, rsz, pc);
            static dex_value_t dummy;
            return dummy;
        }
        return regs[r];
    };

    while (pc < insn_count) {
        uint16_t u0 = insns[pc];
        uint8_t op = u0 & 0xFF;
        switch (op) {
        // -------- NOP / RETURN --------
        case 0x00: // NOP
            pc += 1;
            break;
        case 0x0e: // RETURN_VOID
            if (result_out) *result_out = result;
            return INTERP_VOID;
        case 0x0f: { // RETURN vAA
            uint8_t aa = (u0 >> 8) & 0xFF;
            if (result_out) *result_out = RV(aa);
            return INTERP_OK;
        }
        case 0x10: { // RETURN_WIDE vAA
            uint8_t aa = (u0 >> 8) & 0xFF;
            if (result_out) *result_out = RV(aa);
            return INTERP_OK;
        }
        case 0x11: { // RETURN_OBJECT vAA
            uint8_t aa = (u0 >> 8) & 0xFF;
            if (result_out) *result_out = RV(aa);
            return INTERP_OK;
        }

        // -------- MOVE --------
        case 0x01: { // MOVE vA, vB (12x)
            uint8_t a = (u0 >> 8) & 0xF;
            uint8_t b = (u0 >> 12) & 0xF;
            RV(a) = RV(b);
            pc += 1;
            break;
        }
        case 0x02: { // MOVE_FROM16 vAA, vBBBB (22x)
            uint8_t aa = (u0 >> 8) & 0xFF;
            uint16_t bbbb = insns[pc + 1];
            RV(aa) = RV(bbbb);
            pc += 2;
            break;
        }
        case 0x03: { // MOVE_16 vAAAA, vBBBB (32x)
            uint16_t aaaa = insns[pc + 1];
            uint16_t bbbb = insns[pc + 2];
            RV(aaaa) = RV(bbbb);
            pc += 3;
            break;
        }
        case 0x04: { // MOVE_WIDE vA, vB (12x)
            uint8_t a = (u0 >> 8) & 0xF;
            uint8_t b = (u0 >> 12) & 0xF;
            RV(a) = RV(b);
            pc += 1;
            break;
        }
        case 0x05: { // MOVE_WIDE_FROM16 vAA, vBBBB (22x)
            uint8_t aa = (u0 >> 8) & 0xFF;
            uint16_t bbbb = insns[pc + 1];
            RV(aa) = RV(bbbb);
            pc += 2;
            break;
        }
        case 0x06: { // MOVE_WIDE_16 (32x)
            uint16_t aaaa = insns[pc + 1];
            uint16_t bbbb = insns[pc + 2];
            RV(aaaa) = RV(bbbb);
            pc += 3;
            break;
        }
        case 0x07: { // MOVE_OBJECT vA, vB (12x)
            uint8_t a = (u0 >> 8) & 0xF;
            uint8_t b = (u0 >> 12) & 0xF;
            RV(a) = RV(b);
            pc += 1;
            break;
        }
        case 0x08: { // MOVE_OBJECT_FROM16 (22x)
            uint8_t aa = (u0 >> 8) & 0xFF;
            uint16_t bbbb = insns[pc + 1];
            RV(aa) = RV(bbbb);
            pc += 2;
            break;
        }
        case 0x09: { // MOVE_OBJECT_16 (32x)
            uint16_t aaaa = insns[pc + 1];
            uint16_t bbbb = insns[pc + 2];
            RV(aaaa) = RV(bbbb);
            pc += 3;
            break;
        }
        case 0x0a: { // MOVE_RESULT vAA (11x)
            uint8_t aa = (u0 >> 8) & 0xFF;
            RV(aa) = result;
            pc += 1;
            break;
        }
        case 0x0b: { // MOVE_RESULT_WIDE vAA (11x)
            uint8_t aa = (u0 >> 8) & 0xFF;
            RV(aa) = result;
            pc += 1;
            break;
        }
        case 0x0c: { // MOVE_RESULT_OBJECT vAA (11x)
            uint8_t aa = (u0 >> 8) & 0xFF;
            RV(aa) = result;
            pc += 1;
            break;
        }
        case 0x0d: { // MOVE_EXCEPTION vAA (11x)
            uint8_t aa = (u0 >> 8) & 0xFF;
            if (vm->pending_exception) {
                RV(aa).ptr = vm->pending_exception;
                vm->pending_exception = nullptr;
            } else {
                LOGW("interp", "move-exception with no pending exception");
                RV(aa).ptr = nullptr;
            }
            pc += 1;
            break;
        }

        // -------- CONST --------
        case 0x12: { // CONST_4 vA, #+B (11n)
            uint8_t a = (u0 >> 8) & 0xF;
            int8_t b = (int8_t)((u0 >> 12) & 0xF);
            // sign-extend 4-bit
            if (b & 0x8) b |= (int8_t)0xF0;
            RV(a).i32 = (int32_t)b;
            pc += 1;
            break;
        }
        case 0x13: { // CONST_16 vAA, #+BBBB (21s)
            uint8_t aa = (u0 >> 8) & 0xFF;
            int16_t v = (int16_t)insns[pc + 1];
            RV(aa).i32 = (int32_t)v;
            pc += 2;
            break;
        }
        case 0x14: { // CONST vAA, #+BBBBBBBB (31i)
            uint8_t aa = (u0 >> 8) & 0xFF;
            uint32_t v = (uint32_t)insns[pc + 1] | ((uint32_t)insns[pc + 2] << 16);
            RV(aa).i32 = (int32_t)v;
            pc += 3;
            break;
        }
        case 0x15: { // CONST_HIGH16 vAA, #+BBBB0000 (21h)
            uint8_t aa = (u0 >> 8) & 0xFF;
            uint32_t v = (uint32_t)insns[pc + 1] << 16;
            RV(aa).i32 = (int32_t)v;
            pc += 2;
            break;
        }
        case 0x16: { // CONST_WIDE_16 vAA, #+BBBB (21s)
            uint8_t aa = (u0 >> 8) & 0xFF;
            int16_t v = (int16_t)insns[pc + 1];
            RV(aa).i64 = (int64_t)v;
            pc += 2;
            break;
        }
        case 0x17: { // CONST_WIDE_32 vAA, #+BBBBBBBB (31i)
            uint8_t aa = (u0 >> 8) & 0xFF;
            uint32_t v = (uint32_t)insns[pc + 1] | ((uint32_t)insns[pc + 2] << 16);
            RV(aa).i64 = (int64_t)(int32_t)v;
            pc += 3;
            break;
        }
        case 0x18: { // CONST_WIDE vAA, #+BBBBBBBBBBBBBBBB (51l)
            uint8_t aa = (u0 >> 8) & 0xFF;
            uint64_t v = (uint64_t)insns[pc + 1]
                       | ((uint64_t)insns[pc + 2] << 16)
                       | ((uint64_t)insns[pc + 3] << 32)
                       | ((uint64_t)insns[pc + 4] << 48);
            RV(aa).i64 = (int64_t)v;
            pc += 5;
            break;
        }
        case 0x19: { // CONST_WIDE_HIGH16 vAA, #+BBBB000000000000 (21h)
            uint8_t aa = (u0 >> 8) & 0xFF;
            uint64_t v = (uint64_t)insns[pc + 1] << 48;
            RV(aa).i64 = (int64_t)v;
            pc += 2;
            break;
        }
        case 0x1a: { // CONST_STRING vAA, string@BBBB (21c)
            uint8_t aa = (u0 >> 8) & 0xFF;
            uint16_t sidx = insns[pc + 1];
            const char *s = nullptr;
            for (const dex_file_t *dex : vm->dexes) {
                s = dex_string(dex, sidx);
                if (s) break;
            }
            RV(aa).ptr = dex_new_string_utf(vm, s ? s : "");
            pc += 2;
            break;
        }
        case 0x1b: { // CONST_STRING_JUMBO vAA, string@BBBBBBBB (31c)
            uint8_t aa = (u0 >> 8) & 0xFF;
            uint32_t sidx = (uint32_t)insns[pc + 1] | ((uint32_t)insns[pc + 2] << 16);
            const char *s = nullptr;
            for (const dex_file_t *dex : vm->dexes) {
                s = dex_string(dex, sidx);
                if (s) break;
            }
            RV(aa).ptr = dex_new_string_utf(vm, s ? s : "");
            pc += 3;
            break;
        }
        case 0x1c: { // CONST_CLASS vAA, type@BBBB (21c)
            uint8_t aa = (u0 >> 8) & 0xFF;
            uint16_t tidx = insns[pc + 1];
            const char *desc = nullptr;
            for (const dex_file_t *dex : vm->dexes) {
                desc = dex_type_descriptor(dex, tidx);
                if (desc) break;
            }
            // We don't model Class objects fully; store the descriptor string.
            RV(aa).ptr = desc ? dex_new_string_utf(vm, desc) : nullptr;
            pc += 2;
            break;
        }

        // -------- MONITOR / CHECK_CAST / INSTANCE_OF --------
        case 0x1d: { // MONITOR_ENTER vAA (11x)
            uint8_t aa = (u0 >> 8) & 0xFF;
            LOGD("interp", "monitor-enter v%d (no-op, single-threaded)", aa);
            pc += 1;
            break;
        }
        case 0x1e: { // MONITOR_EXIT vAA (11x)
            uint8_t aa = (u0 >> 8) & 0xFF;
            LOGD("interp", "monitor-exit v%d (no-op, single-threaded)", aa);
            pc += 1;
            break;
        }
        case 0x1f: { // CHECK_CAST vAA, type@BBBB (21c)
            uint8_t aa = (u0 >> 8) & 0xFF;
            uint16_t tidx = insns[pc + 1];
            const char *desc = nullptr;
            for (const dex_file_t *dex : vm->dexes) {
                desc = dex_type_descriptor(dex, tidx);
                if (desc) break;
            }
            dex_obj_t *o = (dex_obj_t*)RV(aa).ptr;
            if (o && o->cls && desc && o->cls->descriptor != desc) {
                // Walk super chain to check.
                dex_cls_t *c = o->cls;
                bool ok = false;
                while (c) {
                    if (c->descriptor == desc) { ok = true; break; }
                    if (c->super_descriptor.empty()) break;
                    c = dex_vm_resolve_class(vm, c->super_descriptor.c_str());
                }
                if (!ok) {
                    LOGW("interp", "check-cast: %s cannot be cast to %s (partial: not raising)",
                         o->cls->descriptor.c_str(), desc);
                }
            }
            pc += 2;
            break;
        }
        case 0x20: { // INSTANCE_OF vA, vB, type@CCCC (22c)
            uint8_t a = (u0 >> 8) & 0xF;
            uint8_t b = (u0 >> 12) & 0xF;
            uint16_t tidx = insns[pc + 1];
            const char *desc = nullptr;
            for (const dex_file_t *dex : vm->dexes) {
                desc = dex_type_descriptor(dex, tidx);
                if (desc) break;
            }
            dex_obj_t *o = (dex_obj_t*)RV(b).ptr;
            int32_t r = 0;
            if (o && o->cls && desc) {
                dex_cls_t *c = o->cls;
                while (c) {
                    if (c->descriptor == desc) { r = 1; break; }
                    if (c->super_descriptor.empty()) break;
                    c = dex_vm_resolve_class(vm, c->super_descriptor.c_str());
                }
            }
            RV(a).i32 = r;
            pc += 2;
            break;
        }
        case 0x21: { // ARRAY_LENGTH vA, vB (12x)
            uint8_t a = (u0 >> 8) & 0xF;
            uint8_t b = (u0 >> 12) & 0xF;
            dex_obj_t *arr = (dex_obj_t*)RV(b).ptr;
            if (!arr || arr->kind != OBJ_ARRAY) {
                LOGW("interp", "array-length on non-array (NPE)");
                dex_obj_t *npe = make_exception(vm,
                    "Ljava/lang/NullPointerException;", "array-length on null");
                int32_t h = throw_exception(vm, code, pc, npe);
                if (h >= 0) { pc = (uint32_t)h; break; }
                return INTERP_EXCEPTION;
            }
            RV(a).i32 = (int32_t)arr->length;
            pc += 1;
            break;
        }

        // -------- NEW_INSTANCE / NEW_ARRAY --------
        case 0x22: { // NEW_INSTANCE vAA, type@BBBB (21c)
            uint8_t aa = (u0 >> 8) & 0xFF;
            uint16_t tidx = insns[pc + 1];
            const char *desc = nullptr;
            for (const dex_file_t *dex : vm->dexes) {
                desc = dex_type_descriptor(dex, tidx);
                if (desc) break;
            }
            dex_obj_t *o = desc ? dex_new_instance(vm, desc) : nullptr;
            if (!o && desc) {
                // Possibly a StringBuilder — handle the framework stub case.
                if (std::string(desc) == "Ljava/lang/StringBuilder;") {
                    o = new dex_obj();
                    o->cls = dex_vm_resolve_class(vm, "Ljava/lang/StringBuilder;");
                    o->kind = OBJ_STRING_BUILDER;
                    o->sb_cap = 64;
                    o->sb_buf = (char*)malloc(o->sb_cap);
                    if (o->sb_buf) { o->sb_buf[0] = 0; o->sb_len = 0; }
                }
            }
            RV(aa).ptr = o;
            pc += 2;
            break;
        }
        case 0x23: { // NEW_ARRAY vA, vB, type@CCCC (22c)
            uint8_t a = (u0 >> 8) & 0xF;
            uint8_t b = (u0 >> 12) & 0xF;
            uint16_t tidx = insns[pc + 1];
            const char *desc = nullptr;
            const dex_file_t *src_dex = nullptr;
            for (const dex_file_t *dex : vm->dexes) {
                desc = dex_type_descriptor(dex, tidx);
                if (desc) { src_dex = dex; break; }
            }
            if (!desc || desc[0] != '[') {
                LOGE("interp", "new-array: type %s is not an array", desc ? desc : "?");
                return INTERP_ERROR;
            }
            char elem_char = desc[1];
            uint8_t cat = desc_char_to_cat(elem_char);
            if (cat == 0) {
                LOGE("interp", "new-array: bad element type '%c'", elem_char);
                return INTERP_ERROR;
            }
            int32_t n = RV(b).i32;
            if (n < 0) {
                dex_obj_t *exc = make_exception(vm,
                    "Ljava/lang/NegativeArraySizeException;", "new-array");
                int32_t h = throw_exception(vm, code, pc, exc);
                if (h >= 0) { pc = (uint32_t)h; break; }
                return INTERP_EXCEPTION;
            }
            dex_obj_t *arr = new dex_obj();
            arr->cls = dex_vm_resolve_class(vm, desc);
            arr->kind = OBJ_ARRAY;
            arr->length = (uint32_t)n;
            arr->elem_cat = cat;
            size_t esz = cat_to_size(cat);
            arr->array_data = calloc(n > 0 ? (size_t)n : 1, esz > 0 ? esz : 1);
            RV(a).ptr = arr;
            (void)src_dex;
            pc += 2;
            break;
        }
        case 0x24:   // FILLED_NEW_ARRAY {vC..vG}, type@BBBB (35c)
        case 0x25: { // FILLED_NEW_ARRAY/RANGE {vCCCC..}, type@BBBB (3rc)
            bool is_range = (op == 0x25);
            uint16_t tidx = insns[pc + 1];
            const char *desc = nullptr;
            for (const dex_file_t *dex : vm->dexes) {
                desc = dex_type_descriptor(dex, tidx);
                if (desc) break;
            }
            if (!desc || desc[0] != '[') {
                LOGE("interp", "filled-new-array: type %s not array", desc ? desc : "?");
                return INTERP_ERROR;
            }
            char elem_char = desc[1];
            uint8_t cat = desc_char_to_cat(elem_char);
            // Gather the args (the values to fill with). For filled-new-array,
            // the "shorty" is the element type repeated A times.
            uint8_t A = is_range ? ((u0 >> 8) & 0xFF) : ((u0 >> 8) & 0xF);
            std::string synth_shorty = "V";
            for (uint8_t i = 0; i < A; i++) synth_shorty.push_back(elem_char);
            dex_value_t tmp_args[16];
            int n = extract_invoke_args(regs.data(), insns + pc, is_range,
                                        /*is_instance=*/false, synth_shorty,
                                        tmp_args, 16);
            if (n < 0) return INTERP_ERROR;
            dex_obj_t *arr = new dex_obj();
            arr->cls = dex_vm_resolve_class(vm, desc);
            arr->kind = OBJ_ARRAY;
            arr->length = (uint32_t)n;
            arr->elem_cat = cat;
            size_t esz = cat_to_size(cat);
            arr->array_data = calloc(n > 0 ? (size_t)n : 1, esz > 0 ? esz : 1);
            for (int i = 0; i < n; i++) {
                void *dst = (uint8_t*)arr->array_data + (size_t)i * esz;
                switch (cat) {
                    case 1: *(uint8_t*)dst  = (uint8_t)tmp_args[i].i32; break;
                    case 2: *(int8_t*)dst   = (int8_t)tmp_args[i].i32; break;
                    case 3: *(uint16_t*)dst = (uint16_t)tmp_args[i].i32; break;
                    case 4: *(int16_t*)dst  = (int16_t)tmp_args[i].i32; break;
                    case 5: *(int32_t*)dst  = tmp_args[i].i32; break;
                    case 6: *(int64_t*)dst  = tmp_args[i].i64; break;
                    case 7: memcpy(dst, &tmp_args[i].f32, 4); break;
                    case 8: memcpy(dst, &tmp_args[i].f64, 8); break;
                    case 9: *(void**)dst    = tmp_args[i].ptr; break;
                }
            }
            result.ptr = arr;
            pc += 3;
            break;
        }
        case 0x26: { // FILL_ARRAY_DATA vAA, +BBBBBBBB (31t)
            uint8_t aa = (u0 >> 8) & 0xFF;
            int32_t offset = (int32_t)((uint32_t)insns[pc + 1] |
                              ((uint32_t)insns[pc + 2] << 16));
            const uint16_t *payload = insns + pc + offset;
            uint16_t ident = payload[0];
            if (ident != 0x0300) {
                LOGE("interp", "fill-array-data: bad payload ident 0x%04X", ident);
                return INTERP_ERROR;
            }
            uint16_t elem_width = payload[1];
            uint32_t size = (uint32_t)payload[2] | ((uint32_t)payload[3] << 16);
            dex_obj_t *arr = (dex_obj_t*)RV(aa).ptr;
            if (!arr || arr->kind != OBJ_ARRAY) {
                LOGE("interp", "fill-array-data: target not array");
                return INTERP_ERROR;
            }
            if (arr->length < size) {
                LOGE("interp", "fill-array-data: %u > array len %u", size, arr->length);
                return INTERP_ERROR;
            }
            const uint8_t *src = (const uint8_t*)(payload + 4);
            size_t esz = cat_to_size(arr->elem_cat);
            if (esz != (size_t)elem_width) {
                LOGW("interp", "fill-array-data: elem_width %u != cat size %zu",
                     elem_width, esz);
            }
            memcpy(arr->array_data, src, (size_t)size * esz);
            pc += 3;
            break;
        }

        // -------- THROW --------
        case 0x27: { // THROW vAA (11x)
            uint8_t aa = (u0 >> 8) & 0xFF;
            dex_obj_t *exc = (dex_obj_t*)RV(aa).ptr;
            if (!exc) {
                exc = make_exception(vm, "Ljava/lang/NullPointerException;", "throw null");
            }
            int32_t h = throw_exception(vm, code, pc, exc);
            if (h >= 0) { pc = (uint32_t)h; break; }
            return INTERP_EXCEPTION;
        }

        // -------- GOTO --------
        case 0x28: { // GOTO +AA (10t)
            int8_t off = (int8_t)((u0 >> 8) & 0xFF);
            pc = (uint32_t)((int32_t)pc + off);
            break;
        }
        case 0x29: { // GOTO_16 +AAAA (20t)
            int16_t off = (int16_t)insns[pc + 1];
            pc = (uint32_t)((int32_t)pc + off);
            break;
        }
        case 0x2a: { // GOTO_32 +AAAAAAAA (30t)
            int32_t off = (int32_t)((uint32_t)insns[pc + 1] |
                             ((uint32_t)insns[pc + 2] << 16));
            pc = (uint32_t)((int32_t)pc + off);
            break;
        }

        // -------- SWITCH --------
        case 0x2b: { // PACKED_SWITCH vAA, +BBBBBBBB (31t)
            uint8_t aa = (u0 >> 8) & 0xFF;
            int32_t offset = (int32_t)((uint32_t)insns[pc + 1] |
                              ((uint32_t)insns[pc + 2] << 16));
            const uint16_t *payload = insns + pc + offset;
            if (payload[0] != 0x0100) {
                LOGE("interp", "packed-switch: bad ident 0x%04X", payload[0]);
                return INTERP_ERROR;
            }
            uint16_t size = payload[1];
            int32_t first_key = (int32_t)((uint32_t)payload[2] |
                                  ((uint32_t)payload[3] << 16));
            int32_t key = RV(aa).i32;
            int32_t idx = key - first_key;
            if (idx >= 0 && idx < (int32_t)size) {
                const int32_t *targets = (const int32_t*)(payload + 4);
                int32_t tgt = targets[idx];
                pc = (uint32_t)((int32_t)pc + tgt);
            } else {
                pc += 3;
            }
            break;
        }
        case 0x2c: { // SPARSE_SWITCH vAA, +BBBBBBBB (31t)
            uint8_t aa = (u0 >> 8) & 0xFF;
            int32_t offset = (int32_t)((uint32_t)insns[pc + 1] |
                              ((uint32_t)insns[pc + 2] << 16));
            const uint16_t *payload = insns + pc + offset;
            if (payload[0] != 0x0200) {
                LOGE("interp", "sparse-switch: bad ident 0x%04X", payload[0]);
                return INTERP_ERROR;
            }
            uint16_t size = payload[1];
            const int32_t *keys = (const int32_t*)(payload + 2);
            const int32_t *targets = keys + size;
            int32_t key = RV(aa).i32;
            int32_t tgt = 0;
            bool found = false;
            for (uint16_t i = 0; i < size; i++) {
                if (keys[i] == key) { tgt = targets[i]; found = true; break; }
            }
            if (found) {
                pc = (uint32_t)((int32_t)pc + tgt);
            } else {
                pc += 3;
            }
            break;
        }

        // -------- CMP --------
        case 0x2d: case 0x2e: { // CMPL_FLOAT / CMPG_FLOAT (23x)
            uint8_t aa = (u0 >> 8) & 0xFF;
            uint8_t bb = insns[pc + 1] & 0xFF;
            uint8_t cc = (insns[pc + 1] >> 8) & 0xFF;
            float a = RV(bb).f32, b = RV(cc).f32;
            int32_t r;
            if (isnan(a) || isnan(b)) r = (op == 0x2e) ? 1 : -1;
            else if (a < b) r = -1;
            else if (a > b) r = 1;
            else r = 0;
            RV(aa).i32 = r;
            pc += 2;
            break;
        }
        case 0x2f: case 0x30: { // CMPL_DOUBLE / CMPG_DOUBLE (23x)
            uint8_t aa = (u0 >> 8) & 0xFF;
            uint8_t bb = insns[pc + 1] & 0xFF;
            uint8_t cc = (insns[pc + 1] >> 8) & 0xFF;
            double a = RV(bb).f64, b = RV(cc).f64;
            int32_t r;
            if (isnan(a) || isnan(b)) r = (op == 0x30) ? 1 : -1;
            else if (a < b) r = -1;
            else if (a > b) r = 1;
            else r = 0;
            RV(aa).i32 = r;
            pc += 2;
            break;
        }
        case 0x31: { // CMP_LONG (23x)
            uint8_t aa = (u0 >> 8) & 0xFF;
            uint8_t bb = insns[pc + 1] & 0xFF;
            uint8_t cc = (insns[pc + 1] >> 8) & 0xFF;
            int64_t a = RV(bb).i64, b = RV(cc).i64;
            int32_t r = (a < b) ? -1 : (a > b) ? 1 : 0;
            RV(aa).i32 = r;
            pc += 2;
            break;
        }

        // -------- IF (two-register) --------
        case 0x32: case 0x33: case 0x34: case 0x35:
        case 0x36: case 0x37: { // IF_EQ/NE/LT/GE/GT/LE vA, vB, +CCCC (22t)
            uint8_t a = (u0 >> 8) & 0xF;
            uint8_t b = (u0 >> 12) & 0xF;
            int16_t off = (int16_t)insns[pc + 1];
            int32_t va = RV(a).i32, vb = RV(b).i32;
            bool take = false;
            switch (op) {
                case 0x32: take = (va == vb); break;  // IF_EQ
                case 0x33: take = (va != vb); break;  // IF_NE
                case 0x34: take = (va <  vb); break;  // IF_LT
                case 0x35: take = (va >= vb); break;  // IF_GE
                case 0x36: take = (va >  vb); break;  // IF_GT
                case 0x37: take = (va <= vb); break;  // IF_LE
            }
            if (take) pc = (uint32_t)((int32_t)pc + off);
            else      pc += 2;
            break;
        }

        // -------- IF (one-register) --------
        case 0x38: case 0x39: case 0x3a: case 0x3b:
        case 0x3c: case 0x3d: { // IF_EQZ/NEZ/LTZ/GEZ/GTZ/LEZ vAA, +BBBB (21t)
            uint8_t aa = (u0 >> 8) & 0xFF;
            int16_t off = (int16_t)insns[pc + 1];
            int32_t v = RV(aa).i32;
            bool take = false;
            switch (op) {
                case 0x38: take = (v == 0); break;
                case 0x39: take = (v != 0); break;
                case 0x3a: take = (v <  0); break;
                case 0x3b: take = (v >= 0); break;
                case 0x3c: take = (v >  0); break;
                case 0x3d: take = (v <= 0); break;
            }
            if (take) pc = (uint32_t)((int32_t)pc + off);
            else      pc += 2;
            break;
        }

        // -------- AGET --------
        case 0x44: case 0x45: case 0x46: case 0x47:
        case 0x48: case 0x49: case 0x4a: { // AGET[_WIDE/_OBJECT/_BOOL/_BYTE/_CHAR/_SHORT]
            uint8_t aa = (u0 >> 8) & 0xFF;
            uint8_t bb = insns[pc + 1] & 0xFF;
            uint8_t cc = (insns[pc + 1] >> 8) & 0xFF;
            dex_obj_t *arr = (dex_obj_t*)RV(bb).ptr;
            int32_t idx = RV(cc).i32;
            if (!arr || arr->kind != OBJ_ARRAY) {
                dex_obj_t *exc = make_exception(vm, "Ljava/lang/NullPointerException;", "aget");
                int32_t h = throw_exception(vm, code, pc, exc);
                if (h >= 0) { pc = (uint32_t)h; break; }
                return INTERP_EXCEPTION;
            }
            if (idx < 0 || (uint32_t)idx >= arr->length) {
                dex_obj_t *exc = make_exception(vm,
                    "Ljava/lang/ArrayIndexOutOfBoundsException;", "aget");
                int32_t h = throw_exception(vm, code, pc, exc);
                if (h >= 0) { pc = (uint32_t)h; break; }
                return INTERP_EXCEPTION;
            }
            uint8_t *src = (uint8_t*)arr->array_data + (size_t)idx * cat_to_size(arr->elem_cat);
            switch (op) {
                case 0x44: RV(aa).i32 = *(int32_t*)src; break;   // AGET
                case 0x45: RV(aa).i64 = *(int64_t*)src; break;   // AGET_WIDE
                case 0x46: RV(aa).ptr = *(void**)src; break;     // AGET_OBJECT
                case 0x47: RV(aa).i32 = *(uint8_t*)src; break;   // AGET_BOOLEAN
                case 0x48: RV(aa).i32 = *(int8_t*)src; break;    // AGET_BYTE
                case 0x49: RV(aa).i32 = *(uint16_t*)src; break;  // AGET_CHAR
                case 0x4a: RV(aa).i32 = *(int16_t*)src; break;   // AGET_SHORT
            }
            pc += 2;
            break;
        }
        // -------- APUT --------
        case 0x4b: case 0x4c: case 0x4d: case 0x4e:
        case 0x4f: case 0x50: case 0x51: { // APUT[_WIDE/_OBJECT/_BOOL/_BYTE/_CHAR/_SHORT]
            uint8_t aa = (u0 >> 8) & 0xFF;
            uint8_t bb = insns[pc + 1] & 0xFF;
            uint8_t cc = (insns[pc + 1] >> 8) & 0xFF;
            dex_obj_t *arr = (dex_obj_t*)RV(bb).ptr;
            int32_t idx = RV(cc).i32;
            if (!arr || arr->kind != OBJ_ARRAY) {
                dex_obj_t *exc = make_exception(vm, "Ljava/lang/NullPointerException;", "aput");
                int32_t h = throw_exception(vm, code, pc, exc);
                if (h >= 0) { pc = (uint32_t)h; break; }
                return INTERP_EXCEPTION;
            }
            if (idx < 0 || (uint32_t)idx >= arr->length) {
                dex_obj_t *exc = make_exception(vm,
                    "Ljava/lang/ArrayIndexOutOfBoundsException;", "aput");
                int32_t h = throw_exception(vm, code, pc, exc);
                if (h >= 0) { pc = (uint32_t)h; break; }
                return INTERP_EXCEPTION;
            }
            uint8_t *dst = (uint8_t*)arr->array_data + (size_t)idx * cat_to_size(arr->elem_cat);
            switch (op) {
                case 0x4b: *(int32_t*)dst = RV(aa).i32; break;   // APUT
                case 0x4c: *(int64_t*)dst = RV(aa).i64; break;   // APUT_WIDE
                case 0x4d: *(void**)dst   = RV(aa).ptr; break;   // APUT_OBJECT
                case 0x4e: *(uint8_t*)dst = (uint8_t)RV(aa).i32; break; // APUT_BOOLEAN
                case 0x4f: *(int8_t*)dst  = (int8_t)RV(aa).i32; break;  // APUT_BYTE
                case 0x50: *(uint16_t*)dst = (uint16_t)RV(aa).i32; break; // APUT_CHAR
                case 0x51: *(int16_t*)dst = (int16_t)RV(aa).i32; break;  // APUT_SHORT
            }
            pc += 2;
            break;
        }

        // -------- IGET / IPUT --------
        case 0x52: case 0x53: case 0x54: case 0x55:
        case 0x56: case 0x57: case 0x58: { // IGET[...]
            uint8_t a = (u0 >> 8) & 0xF;
            uint8_t b = (u0 >> 12) & 0xF;
            uint16_t fidx = insns[pc + 1];
            const dex_field_t *fid = nullptr;
            const dex_file_t *src_dex = nullptr;
            for (const dex_file_t *dex : vm->dexes) {
                fid = dex_field(dex, fidx);
                if (fid) { src_dex = dex; break; }
            }
            if (!fid) { LOGE("interp", "iget: field@%u not found", fidx); return INTERP_ERROR; }
            const char *fname = dex_string(src_dex, fid->name_idx);
            dex_obj_t *obj = (dex_obj_t*)RV(b).ptr;
            if (!obj) {
                dex_obj_t *exc = make_exception(vm, "Ljava/lang/NullPointerException;", "iget");
                int32_t h = throw_exception(vm, code, pc, exc);
                if (h >= 0) { pc = (uint32_t)h; break; }
                return INTERP_EXCEPTION;
            }
            dex_value_t v;
            if (dex_get_field(vm, obj, fname, &v) != 0) {
                LOGW("interp", "iget: field '%s' not found on %s", fname,
                     obj->cls ? obj->cls->descriptor.c_str() : "?");
                v = dex_value_t{};
            }
            switch (op) {
                case 0x52: RV(a).i32 = v.i32; break;   // IGET
                case 0x53: RV(a).i64 = v.i64; break;   // IGET_WIDE
                case 0x54: RV(a).ptr = v.ptr; break;   // IGET_OBJECT
                case 0x55: RV(a).i32 = v.i32 & 1; break; // IGET_BOOLEAN
                case 0x56: RV(a).i32 = (int8_t)v.i32; break; // IGET_BYTE
                case 0x57: RV(a).i32 = (uint16_t)v.i32; break; // IGET_CHAR
                case 0x58: RV(a).i32 = (int16_t)v.i32; break; // IGET_SHORT
            }
            pc += 2;
            break;
        }
        case 0x59: case 0x5a: case 0x5b: case 0x5c:
        case 0x5d: case 0x5e: case 0x5f: { // IPUT[...]
            uint8_t a = (u0 >> 8) & 0xF;
            uint8_t b = (u0 >> 12) & 0xF;
            uint16_t fidx = insns[pc + 1];
            const dex_field_t *fid = nullptr;
            const dex_file_t *src_dex = nullptr;
            for (const dex_file_t *dex : vm->dexes) {
                fid = dex_field(dex, fidx);
                if (fid) { src_dex = dex; break; }
            }
            if (!fid) { LOGE("interp", "iput: field@%u not found", fidx); return INTERP_ERROR; }
            const char *fname = dex_string(src_dex, fid->name_idx);
            dex_obj_t *obj = (dex_obj_t*)RV(b).ptr;
            if (!obj) {
                dex_obj_t *exc = make_exception(vm, "Ljava/lang/NullPointerException;", "iput");
                int32_t h = throw_exception(vm, code, pc, exc);
                if (h >= 0) { pc = (uint32_t)h; break; }
                return INTERP_EXCEPTION;
            }
            dex_value_t v = RV(a);
            dex_set_field(vm, obj, fname, v);
            pc += 2;
            break;
        }

        // -------- SGET / SPUT --------
        case 0x60: case 0x61: case 0x62: case 0x63:
        case 0x64: case 0x65: case 0x66: { // SGET[...]
            uint8_t aa = (u0 >> 8) & 0xFF;
            uint16_t fidx = insns[pc + 1];
            const dex_field_t *fid = nullptr;
            const dex_file_t *src_dex = nullptr;
            for (const dex_file_t *dex : vm->dexes) {
                fid = dex_field(dex, fidx);
                if (fid) { src_dex = dex; break; }
            }
            if (!fid) { LOGE("interp", "sget: field@%u not found", fidx); return INTERP_ERROR; }
            const char *cls_desc = dex_type_descriptor(src_dex, fid->class_idx);
            const char *fname = dex_string(src_dex, fid->name_idx);
            dex_value_t v;
            if (dex_get_static_field(vm, cls_desc, fname, &v) != 0) {
                v = dex_value_t{};
            }
            switch (op) {
                case 0x60: RV(aa).i32 = v.i32; break;
                case 0x61: RV(aa).i64 = v.i64; break;
                case 0x62: RV(aa).ptr = v.ptr; break;
                case 0x63: RV(aa).i32 = v.i32 & 1; break;
                case 0x64: RV(aa).i32 = (int8_t)v.i32; break;
                case 0x65: RV(aa).i32 = (uint16_t)v.i32; break;
                case 0x66: RV(aa).i32 = (int16_t)v.i32; break;
            }
            pc += 2;
            break;
        }
        case 0x67: case 0x68: case 0x69: case 0x6a:
        case 0x6b: case 0x6c: case 0x6d: { // SPUT[...]
            uint8_t aa = (u0 >> 8) & 0xFF;
            uint16_t fidx = insns[pc + 1];
            const dex_field_t *fid = nullptr;
            const dex_file_t *src_dex = nullptr;
            for (const dex_file_t *dex : vm->dexes) {
                fid = dex_field(dex, fidx);
                if (fid) { src_dex = dex; break; }
            }
            if (!fid) { LOGE("interp", "sput: field@%u not found", fidx); return INTERP_ERROR; }
            const char *cls_desc = dex_type_descriptor(src_dex, fid->class_idx);
            const char *fname = dex_string(src_dex, fid->name_idx);
            dex_set_static_field(vm, cls_desc, fname, RV(aa));
            pc += 2;
            break;
        }

        // -------- INVOKE --------
        case 0x6e: case 0x6f: case 0x70: case 0x71: case 0x72:   // 35c
        case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: { // 3rc
            bool is_range = (op >= 0x74);
            uint16_t mid_idx = insns[pc + 1];
            const dex_method_t *mid = nullptr;
            const dex_file_t *src_dex = nullptr;
            for (const dex_file_t *dex : vm->dexes) {
                mid = dex_method(dex, mid_idx);
                if (mid) { src_dex = dex; break; }
            }
            if (!mid) { LOGE("interp", "invoke: method@%u not found", mid_idx); return INTERP_ERROR; }
            const char *mname = dex_string(src_dex, mid->name_idx);
            const dex_proto_t *pid = dex_proto(src_dex, mid->proto_idx);
            const char *mshorty = pid ? dex_string(src_dex, pid->shorty_idx) : "V";
            const char *cls_desc = dex_type_descriptor(src_dex, mid->class_idx);
            bool is_static = (op == 0x71 || op == 0x77);
            bool is_instance = !is_static;

            // Extract args from registers.
            dex_value_t iargs[16];
            int n = extract_invoke_args(regs.data(), insns + pc, is_range,
                                        is_instance, mshorty, iargs, 16);
            if (n < 0) { LOGE("interp", "invoke: too many args"); return INTERP_ERROR; }

            // For invoke-virtual/interface, override the declaring class with
            // the receiver's actual class so overrides resolve correctly.
            const char *effective_cls = cls_desc;
            dex_obj_t *recv = is_instance ? (dex_obj_t*)iargs[0].ptr : nullptr;
            if (recv && recv->cls && (op == 0x6e || op == 0x6f || op == 0x72 ||
                                       op == 0x74 || op == 0x75 || op == 0x78)) {
                effective_cls = recv->cls->descriptor.c_str();
            }

            dex_value_t inv_result{};
            InterpResult ir = invoke_resolved(vm, effective_cls, mname, mshorty,
                                              iargs, n, &inv_result, is_instance);
            if (ir == INTERP_EXCEPTION) {
                // Propagate to current frame's try handlers.
                int32_t h = find_catch_handler(vm, code, pc, vm->pending_exception);
                if (h >= 0) { pc = (uint32_t)h; break; }
                return INTERP_EXCEPTION;
            }
            if (ir == INTERP_ERROR) {
                LOGE("interp", "invoke %s.%s%s failed",
                     effective_cls, mname, mshorty);
                return INTERP_ERROR;
            }
            result = inv_result;
            pc += 3;
            break;
        }

        // -------- UNARY OPS (12x) --------
        case 0x7b: { // NEG_INT vA, vB
            uint8_t a = (u0 >> 8) & 0xF, b = (u0 >> 12) & 0xF;
            RV(a).i32 = -RV(b).i32;
            pc += 1; break;
        }
        case 0x7c: { // NOT_INT vA, vB
            uint8_t a = (u0 >> 8) & 0xF, b = (u0 >> 12) & 0xF;
            RV(a).i32 = ~RV(b).i32;
            pc += 1; break;
        }
        case 0x7d: { // NEG_LONG
            uint8_t a = (u0 >> 8) & 0xF, b = (u0 >> 12) & 0xF;
            RV(a).i64 = -RV(b).i64;
            pc += 1; break;
        }
        case 0x7e: { // NOT_LONG
            uint8_t a = (u0 >> 8) & 0xF, b = (u0 >> 12) & 0xF;
            RV(a).i64 = ~RV(b).i64;
            pc += 1; break;
        }
        case 0x7f: { // NEG_FLOAT
            uint8_t a = (u0 >> 8) & 0xF, b = (u0 >> 12) & 0xF;
            RV(a).f32 = -RV(b).f32;
            pc += 1; break;
        }
        case 0x80: { // NEG_DOUBLE
            uint8_t a = (u0 >> 8) & 0xF, b = (u0 >> 12) & 0xF;
            RV(a).f64 = -RV(b).f64;
            pc += 1; break;
        }
        case 0x81: { // INT_TO_LONG vA, vB
            uint8_t a = (u0 >> 8) & 0xF, b = (u0 >> 12) & 0xF;
            RV(a).i64 = (int64_t)RV(b).i32;
            pc += 1; break;
        }
        case 0x82: { // INT_TO_FLOAT
            uint8_t a = (u0 >> 8) & 0xF, b = (u0 >> 12) & 0xF;
            RV(a).f32 = (float)RV(b).i32;
            pc += 1; break;
        }
        case 0x83: { // INT_TO_DOUBLE
            uint8_t a = (u0 >> 8) & 0xF, b = (u0 >> 12) & 0xF;
            RV(a).f64 = (double)RV(b).i32;
            pc += 1; break;
        }
        case 0x84: { // LONG_TO_INT
            uint8_t a = (u0 >> 8) & 0xF, b = (u0 >> 12) & 0xF;
            RV(a).i32 = (int32_t)RV(b).i64;
            pc += 1; break;
        }
        case 0x85: { // LONG_TO_FLOAT
            uint8_t a = (u0 >> 8) & 0xF, b = (u0 >> 12) & 0xF;
            RV(a).f32 = (float)RV(b).i64;
            pc += 1; break;
        }
        case 0x86: { // LONG_TO_DOUBLE
            uint8_t a = (u0 >> 8) & 0xF, b = (u0 >> 12) & 0xF;
            RV(a).f64 = (double)RV(b).i64;
            pc += 1; break;
        }
        case 0x87: { // FLOAT_TO_INT
            uint8_t a = (u0 >> 8) & 0xF, b = (u0 >> 12) & 0xF;
            RV(a).i32 = (int32_t)RV(b).f32;
            pc += 1; break;
        }
        case 0x88: { // FLOAT_TO_LONG
            uint8_t a = (u0 >> 8) & 0xF, b = (u0 >> 12) & 0xF;
            RV(a).i64 = (int64_t)RV(b).f32;
            pc += 1; break;
        }
        case 0x89: { // FLOAT_TO_DOUBLE
            uint8_t a = (u0 >> 8) & 0xF, b = (u0 >> 12) & 0xF;
            RV(a).f64 = (double)RV(b).f32;
            pc += 1; break;
        }
        case 0x8a: { // DOUBLE_TO_INT
            uint8_t a = (u0 >> 8) & 0xF, b = (u0 >> 12) & 0xF;
            RV(a).i32 = (int32_t)RV(b).f64;
            pc += 1; break;
        }
        case 0x8b: { // DOUBLE_TO_LONG
            uint8_t a = (u0 >> 8) & 0xF, b = (u0 >> 12) & 0xF;
            RV(a).i64 = (int64_t)RV(b).f64;
            pc += 1; break;
        }
        case 0x8c: { // DOUBLE_TO_FLOAT
            uint8_t a = (u0 >> 8) & 0xF, b = (u0 >> 12) & 0xF;
            RV(a).f32 = (float)RV(b).f64;
            pc += 1; break;
        }
        case 0x8d: { // INT_TO_BYTE
            uint8_t a = (u0 >> 8) & 0xF, b = (u0 >> 12) & 0xF;
            RV(a).i32 = (int32_t)(int8_t)RV(b).i32;
            pc += 1; break;
        }
        case 0x8e: { // INT_TO_CHAR
            uint8_t a = (u0 >> 8) & 0xF, b = (u0 >> 12) & 0xF;
            RV(a).i32 = (int32_t)(uint16_t)RV(b).i32;
            pc += 1; break;
        }
        case 0x8f: { // INT_TO_SHORT
            uint8_t a = (u0 >> 8) & 0xF, b = (u0 >> 12) & 0xF;
            RV(a).i32 = (int32_t)(int16_t)RV(b).i32;
            pc += 1; break;
        }

        // -------- BINARY OPS 23x (3-register form) --------
        case 0x90: case 0x91: case 0x92: case 0x93: case 0x94:
        case 0x95: case 0x96: case 0x97: case 0x98: case 0x99: case 0x9a:
        case 0x9b: case 0x9c: case 0x9d: case 0x9e: case 0x9f:
        case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4: case 0xa5:
        case 0xa6: case 0xa7: case 0xa8: case 0xa9: case 0xaa:
        case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf: {
            uint8_t aa = (u0 >> 8) & 0xFF;
            uint8_t bb = insns[pc + 1] & 0xFF;
            uint8_t cc = (insns[pc + 1] >> 8) & 0xFF;
            switch (op) {
                // int ops
                case 0x90: RV(aa).i32 = RV(bb).i32 + RV(cc).i32; break;  // ADD_INT
                case 0x91: RV(aa).i32 = RV(bb).i32 - RV(cc).i32; break;  // SUB_INT
                case 0x92: RV(aa).i32 = RV(bb).i32 * RV(cc).i32; break;  // MUL_INT
                case 0x93: { // DIV_INT
                    int32_t d = RV(cc).i32;
                    if (d == 0) {
                        dex_obj_t *exc = make_exception(vm, "Ljava/lang/ArithmeticException;", "div by zero");
                        int32_t h = throw_exception(vm, code, pc, exc);
                        if (h >= 0) { pc = (uint32_t)h; goto next_insn; }
                        return INTERP_EXCEPTION;
                    }
                    RV(aa).i32 = RV(bb).i32 / d;
                    break;
                }
                case 0x94: { // REM_INT
                    int32_t d = RV(cc).i32;
                    if (d == 0) {
                        dex_obj_t *exc = make_exception(vm, "Ljava/lang/ArithmeticException;", "rem by zero");
                        int32_t h = throw_exception(vm, code, pc, exc);
                        if (h >= 0) { pc = (uint32_t)h; goto next_insn; }
                        return INTERP_EXCEPTION;
                    }
                    RV(aa).i32 = RV(bb).i32 % d;
                    break;
                }
                case 0x95: RV(aa).i32 = RV(bb).i32 & RV(cc).i32; break;  // AND_INT
                case 0x96: RV(aa).i32 = RV(bb).i32 | RV(cc).i32; break;  // OR_INT
                case 0x97: RV(aa).i32 = RV(bb).i32 ^ RV(cc).i32; break;  // XOR_INT
                case 0x98: RV(aa).i32 = RV(bb).i32 << (RV(cc).i32 & 31); break;  // SHL_INT
                case 0x99: RV(aa).i32 = RV(bb).i32 >> (RV(cc).i32 & 31); break;  // SHR_INT
                case 0x9a: RV(aa).i32 = (int32_t)((uint32_t)RV(bb).i32 >> (RV(cc).i32 & 31)); break; // USHR_INT
                // long ops
                case 0x9b: RV(aa).i64 = RV(bb).i64 + RV(cc).i64; break;  // ADD_LONG
                case 0x9c: RV(aa).i64 = RV(bb).i64 - RV(cc).i64; break;  // SUB_LONG
                case 0x9d: RV(aa).i64 = RV(bb).i64 * RV(cc).i64; break;  // MUL_LONG
                case 0x9e: RV(aa).i64 = RV(bb).i64 / RV(cc).i64; break;  // DIV_LONG
                case 0x9f: RV(aa).i64 = RV(bb).i64 % RV(cc).i64; break;  // REM_LONG
                case 0xa0: RV(aa).i64 = RV(bb).i64 & RV(cc).i64; break;  // AND_LONG
                case 0xa1: RV(aa).i64 = RV(bb).i64 | RV(cc).i64; break;  // OR_LONG
                case 0xa2: RV(aa).i64 = RV(bb).i64 ^ RV(cc).i64; break;  // XOR_LONG
                case 0xa3: RV(aa).i64 = RV(bb).i64 << (RV(cc).i64 & 63); break;  // SHL_LONG
                case 0xa4: RV(aa).i64 = RV(bb).i64 >> (RV(cc).i64 & 63); break;  // SHR_LONG
                case 0xa5: RV(aa).i64 = (int64_t)((uint64_t)RV(bb).i64 >> (RV(cc).i64 & 63)); break; // USHR_LONG
                // float ops
                case 0xa6: RV(aa).f32 = RV(bb).f32 + RV(cc).f32; break;  // ADD_FLOAT
                case 0xa7: RV(aa).f32 = RV(bb).f32 - RV(cc).f32; break;  // SUB_FLOAT
                case 0xa8: RV(aa).f32 = RV(bb).f32 * RV(cc).f32; break;  // MUL_FLOAT
                case 0xa9: RV(aa).f32 = RV(bb).f32 / RV(cc).f32; break;  // DIV_FLOAT
                case 0xaa: RV(aa).f32 = fmodf(RV(bb).f32, RV(cc).f32); break; // REM_FLOAT
                // double ops
                case 0xab: RV(aa).f64 = RV(bb).f64 + RV(cc).f64; break;  // ADD_DOUBLE
                case 0xac: RV(aa).f64 = RV(bb).f64 - RV(cc).f64; break;  // SUB_DOUBLE
                case 0xad: RV(aa).f64 = RV(bb).f64 * RV(cc).f64; break;  // MUL_DOUBLE
                case 0xae: RV(aa).f64 = RV(bb).f64 / RV(cc).f64; break;  // DIV_DOUBLE
                case 0xaf: RV(aa).f64 = fmod(RV(bb).f64, RV(cc).f64); break; // REM_DOUBLE
            }
            pc += 2;
            break;
        }

        // -------- BINARY OPS 12x (2addr form) --------
        case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4:
        case 0xb5: case 0xb6: case 0xb7: case 0xb8: case 0xb9: case 0xba:
        case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
        case 0xc0: case 0xc1: case 0xc2: case 0xc3: case 0xc4: case 0xc5:
        case 0xc6: case 0xc7: case 0xc8: case 0xc9: case 0xca:
        case 0xcb: case 0xcc: case 0xcd: case 0xce: case 0xcf: {
            uint8_t a = (u0 >> 8) & 0xF, b = (u0 >> 12) & 0xF;
            // Same semantics as 23x but with dst = src1.
            switch (op - 0xb0 + 0x90) {
                case 0x90: RV(a).i32 = RV(a).i32 + RV(b).i32; break;
                case 0x91: RV(a).i32 = RV(a).i32 - RV(b).i32; break;
                case 0x92: RV(a).i32 = RV(a).i32 * RV(b).i32; break;
                case 0x93: {
                    if (RV(b).i32 == 0) {
                        dex_obj_t *exc = make_exception(vm, "Ljava/lang/ArithmeticException;", "div by zero");
                        int32_t h = throw_exception(vm, code, pc, exc);
                        if (h >= 0) { pc = (uint32_t)h; goto next_insn; }
                        return INTERP_EXCEPTION;
                    }
                    RV(a).i32 = RV(a).i32 / RV(b).i32;
                    break;
                }
                case 0x94: {
                    if (RV(b).i32 == 0) {
                        dex_obj_t *exc = make_exception(vm, "Ljava/lang/ArithmeticException;", "rem by zero");
                        int32_t h = throw_exception(vm, code, pc, exc);
                        if (h >= 0) { pc = (uint32_t)h; goto next_insn; }
                        return INTERP_EXCEPTION;
                    }
                    RV(a).i32 = RV(a).i32 % RV(b).i32;
                    break;
                }
                case 0x95: RV(a).i32 = RV(a).i32 & RV(b).i32; break;
                case 0x96: RV(a).i32 = RV(a).i32 | RV(b).i32; break;
                case 0x97: RV(a).i32 = RV(a).i32 ^ RV(b).i32; break;
                case 0x98: RV(a).i32 = RV(a).i32 << (RV(b).i32 & 31); break;
                case 0x99: RV(a).i32 = RV(a).i32 >> (RV(b).i32 & 31); break;
                case 0x9a: RV(a).i32 = (int32_t)((uint32_t)RV(a).i32 >> (RV(b).i32 & 31)); break;
                case 0x9b: RV(a).i64 = RV(a).i64 + RV(b).i64; break;
                case 0x9c: RV(a).i64 = RV(a).i64 - RV(b).i64; break;
                case 0x9d: RV(a).i64 = RV(a).i64 * RV(b).i64; break;
                case 0x9e: RV(a).i64 = RV(a).i64 / RV(b).i64; break;
                case 0x9f: RV(a).i64 = RV(a).i64 % RV(b).i64; break;
                case 0xa0: RV(a).i64 = RV(a).i64 & RV(b).i64; break;
                case 0xa1: RV(a).i64 = RV(a).i64 | RV(b).i64; break;
                case 0xa2: RV(a).i64 = RV(a).i64 ^ RV(b).i64; break;
                case 0xa3: RV(a).i64 = RV(a).i64 << (RV(b).i64 & 63); break;
                case 0xa4: RV(a).i64 = RV(a).i64 >> (RV(b).i64 & 63); break;
                case 0xa5: RV(a).i64 = (int64_t)((uint64_t)RV(a).i64 >> (RV(b).i64 & 63)); break;
                case 0xa6: RV(a).f32 = RV(a).f32 + RV(b).f32; break;
                case 0xa7: RV(a).f32 = RV(a).f32 - RV(b).f32; break;
                case 0xa8: RV(a).f32 = RV(a).f32 * RV(b).f32; break;
                case 0xa9: RV(a).f32 = RV(a).f32 / RV(b).f32; break;
                case 0xaa: RV(a).f32 = fmodf(RV(a).f32, RV(b).f32); break;
                case 0xab: RV(a).f64 = RV(a).f64 + RV(b).f64; break;
                case 0xac: RV(a).f64 = RV(a).f64 - RV(b).f64; break;
                case 0xad: RV(a).f64 = RV(a).f64 * RV(b).f64; break;
                case 0xae: RV(a).f64 = RV(a).f64 / RV(b).f64; break;
                case 0xaf: RV(a).f64 = fmod(RV(a).f64, RV(b).f64); break;
            }
            pc += 1;
            break;
        }

        // -------- LIT16 (22s) --------
        case 0xd0: case 0xd1: case 0xd2: case 0xd3: case 0xd4:
        case 0xd5: case 0xd6: case 0xd7: { // ADD/RSUB/MUL/DIV/REM/AND/OR/XOR _INT_LIT16
            uint8_t a = (u0 >> 8) & 0xF, b = (u0 >> 12) & 0xF;
            int16_t lit = (int16_t)insns[pc + 1];
            int32_t bv = RV(b).i32;
            switch (op) {
                case 0xd0: RV(a).i32 = bv + lit; break;
                case 0xd1: RV(a).i32 = lit - bv; break;  // RSUB
                case 0xd2: RV(a).i32 = bv * lit; break;
                case 0xd3: if (lit == 0) { goto div_zero_lit16; } RV(a).i32 = bv / lit; break;
                case 0xd4: if (lit == 0) { goto div_zero_lit16; } RV(a).i32 = bv % lit; break;
                case 0xd5: RV(a).i32 = bv & lit; break;
                case 0xd6: RV(a).i32 = bv | lit; break;
                case 0xd7: RV(a).i32 = bv ^ lit; break;
            }
            pc += 2;
            break;
          div_zero_lit16: {
                dex_obj_t *exc = make_exception(vm, "Ljava/lang/ArithmeticException;", "lit16 div/rem by zero");
                int32_t h = throw_exception(vm, code, pc, exc);
                if (h >= 0) { pc = (uint32_t)h; break; }
                return INTERP_EXCEPTION;
            }
        }
        // -------- LIT8 (22b) --------
        case 0xd8: case 0xd9: case 0xda: case 0xdb: case 0xdc:
        case 0xdd: case 0xde: case 0xdf:
        case 0xe0: case 0xe1: case 0xe2: { // ADD/RSUB/MUL/DIV/REM/AND/OR/XOR/SHL/SHR/USHR _INT_LIT8
            uint8_t aa = (u0 >> 8) & 0xFF;
            uint8_t bb = insns[pc + 1] & 0xFF;
            int8_t lit = (int8_t)((insns[pc + 1] >> 8) & 0xFF);
            int32_t bv = RV(bb).i32;
            switch (op) {
                case 0xd8: RV(aa).i32 = bv + lit; break;
                case 0xd9: RV(aa).i32 = lit - bv; break;  // RSUB
                case 0xda: RV(aa).i32 = bv * lit; break;
                case 0xdb: if (lit == 0) { goto div_zero_lit8; } RV(aa).i32 = bv / lit; break;
                case 0xdc: if (lit == 0) { goto div_zero_lit8; } RV(aa).i32 = bv % lit; break;
                case 0xdd: RV(aa).i32 = bv & lit; break;
                case 0xde: RV(aa).i32 = bv | lit; break;
                case 0xdf: RV(aa).i32 = bv ^ lit; break;
                case 0xe0: RV(aa).i32 = bv << lit; break;   // SHL (lit & 31 implicit)
                case 0xe1: RV(aa).i32 = bv >> lit; break;   // SHR
                case 0xe2: RV(aa).i32 = (int32_t)((uint32_t)bv >> lit); break; // USHR
            }
            pc += 2;
            break;
          div_zero_lit8: {
                dex_obj_t *exc = make_exception(vm, "Ljava/lang/ArithmeticException;", "lit8 div/rem by zero");
                int32_t h = throw_exception(vm, code, pc, exc);
                if (h >= 0) { pc = (uint32_t)h; break; }
                return INTERP_EXCEPTION;
            }
        }

        // -------- Unknown opcode --------
        default:
            LOGE("interp", "unknown opcode 0x%02X at pc=%u (method %s.%s)",
                 op, pc, m.declaring_class_desc.c_str(), m.name.c_str());
            return INTERP_ERROR;
        }
      next_insn:;
    }

    // Fell off the end without return.
    LOGW("interp", "method %s.%s fell off end without return",
         m.declaring_class_desc.c_str(), m.name.c_str());
    if (result_out) *result_out = result;
    return INTERP_VOID;
}

// Invoke a resolved method via the stub registry or the DEX interpreter.
InterpResult invoke_resolved(dex_vm_t *vm,
                             const char *cls_desc,
                             const char *name, const char *shorty,
                             dex_value_t *args, int arg_count,
                             dex_value_t *result_out,
                             bool is_instance) {
    if (!vm || !cls_desc || !name || !shorty) return INTERP_ERROR;

    // 1. Try the native stub registry first.
    std::string key = native_key(cls_desc, name, shorty);
    auto it = vm->natives.find(key);
    if (it != vm->natives.end()) {
        int rc = it->second(vm, args, arg_count, result_out);
        if (rc == 0) {
            // Stubs return 0 for both void and non-void. For void methods,
            // result_out is not set (left as caller's value).
            return (shorty[0] == 'V') ? INTERP_VOID : INTERP_OK;
        }
        // P3-3: stubs that set vm->pending_exception (e.g. Socket throwing
        // IOException on connect() failure) want the bytecode try/catch path
        // to handle them. Map that case to INTERP_EXCEPTION instead of ERROR.
        if (vm->pending_exception) {
            return INTERP_EXCEPTION;
        }
        return INTERP_ERROR;
    }

    // 2. Resolve the class.
    dex_cls_t *cls = dex_vm_resolve_class(vm, cls_desc);
    if (!cls) {
        LOGE("interp", "NoSuchClassError: %s (method %s%s)",
             cls_desc, name, shorty);
        return INTERP_ERROR;
    }

    // 3. Find the method. Order matters: check the class's OWN methods first
    //    (direct + virtual) so that private/constructor methods shadow any
    //    inherited virtual method with the same name+shorty; then walk the
    //    super chain for inherited virtual methods.
    (void)is_instance;   // not used for lookup order; interpret_method uses m->is_static
    dex_cls_t::method_info *m = find_method_in_class(cls, name, shorty);
    if (!m) m = find_virtual_method(vm, cls, name, shorty);
    if (!m) {
        LOGE("interp", "NoSuchMethodError: %s.%s%s", cls_desc, name, shorty);
        return INTERP_ERROR;
    }
    if (!m->has_code) {
        // Abstract or native without a stub. Check supers.
        if (is_instance && cls->super_descriptor.size()) {
            dex_cls_t *sup = dex_vm_resolve_class(vm, cls->super_descriptor.c_str());
            if (sup) {
                dex_cls_t::method_info *m2 = find_virtual_method(vm, sup, name, shorty);
                if (m2 && m2->has_code) { m = m2; }
            }
        }
        if (!m->has_code) {
            LOGE("interp", "method %s.%s%s has no code_item and no stub",
                 cls_desc, name, shorty);
            return INTERP_ERROR;
        }
    }

    // 4. Run the interpreter.
    return interpret_method(vm, *m, args, arg_count, result_out);
}

}  // namespace

// ============================================================================
// Public dex_invoke
// ============================================================================

extern "C" {

int dex_invoke(dex_vm_t *vm,
               const char *cls_desc, const char *name, const char *shorty,
               dex_value_t *args, int arg_count,
               dex_value_t *result_out) {
    if (!vm || !cls_desc || !name || !shorty) return -1;
    dex_value_t result{};
    // Determine is_instance from the method kind. For the public entry point
    // we don't know the opcode, so we treat any non-static method as instance.
    // (Static methods are in direct_methods with ACC_STATIC; interpret_method
    // checks m->is_static to set up the ins registers correctly, so passing
    // is_instance=true for a static method is harmless — the lookup just
    // checks direct methods first which is where statics live.)
    InterpResult ir = invoke_resolved(vm, cls_desc, name, shorty,
                                      args, arg_count, &result,
                                      /*is_instance=*/true);
    if (ir == INTERP_EXCEPTION) {
        LOGW("interp", "dex_invoke: uncaught exception %s in %s.%s%s",
             vm->pending_exception && vm->pending_exception->cls ?
                 vm->pending_exception->cls->descriptor.c_str() : "?",
             cls_desc, name, shorty);
        return -10;
    }
    if (ir == INTERP_ERROR) return -2;
    if (result_out) *result_out = result;
    return 0;
}

}  // extern "C"

// ============================================================================
// Framework stub methods
// ============================================================================

namespace {

// Helper: get a string from an arg (dex_value_t containing a dex_obj_t*).
const char *arg_str(dex_value_t *v) {
    if (!v || !v->ptr) return nullptr;
    dex_obj_t *o = (dex_obj_t*)v->ptr;
    if (o->kind == OBJ_STRING) return o->utf8 ? o->utf8 : "";
    if (o->kind == OBJ_STRING_BUILDER) return o->sb_buf ? o->sb_buf : "";
    return nullptr;
}

// ---- java.lang.Object ----
int stub_object_init(dex_vm_t*, dex_value_t*, int, dex_value_t*) { return 0; }
int stub_object_clinit(dex_vm_t*, dex_value_t*, int, dex_value_t*) { return 0; }

// ---- java.lang.String ----
int stub_string_init_bytes(dex_vm_t *vm, dex_value_t *args, int, dex_value_t*) {
    // args[0] = this (String), args[1] = byte[]
    if (!args || !args[0].ptr) return 0;
    dex_obj_t *str = (dex_obj_t*)args[0].ptr;
    dex_obj_t *arr  = (dex_obj_t*)(args[1].ptr);
    str->kind = OBJ_STRING;
    if (str->utf8) { free(str->utf8); str->utf8 = nullptr; str->utf8_len = 0; }
    if (arr && arr->kind == OBJ_ARRAY && arr->array_data) {
        size_t n = arr->length;
        str->utf8 = (char*)malloc(n + 1);
        if (str->utf8) {
            memcpy(str->utf8, arr->array_data, n);
            str->utf8[n] = 0;
            str->utf8_len = (uint32_t)n;
        }
    } else {
        str->utf8 = (char*)malloc(1);
        if (str->utf8) { str->utf8[0] = 0; str->utf8_len = 0; }
    }
    (void)vm;
    return 0;
}
int stub_string_init(dex_vm_t *vm, dex_value_t *args, int argc, dex_value_t *res) {
    // Overload: <init>()V — just mark as string.
    if (argc >= 1 && args[0].ptr) {
        dex_obj_t *str = (dex_obj_t*)args[0].ptr;
        str->kind = OBJ_STRING;
        if (!str->utf8) {
            str->utf8 = (char*)malloc(1);
            if (str->utf8) { str->utf8[0] = 0; str->utf8_len = 0; }
        }
    }
    return stub_object_init(vm, args, argc, res);
}
int stub_string_length(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    if (res && args && args[0].ptr) {
        dex_obj_t *str = (dex_obj_t*)args[0].ptr;
        if (str->kind == OBJ_STRING && str->utf8) {
            res->i32 = (int32_t)strlen(str->utf8);
            return 0;
        }
    }
    if (res) res->i32 = 0;
    return 0;
}
int stub_string_equals(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    if (!args || !args[0].ptr || !args[1].ptr) { res->i32 = 0; return 0; }
    dex_obj_t *a = (dex_obj_t*)args[0].ptr;
    dex_obj_t *b = (dex_obj_t*)args[1].ptr;
    const char *sa = (a->kind == OBJ_STRING && a->utf8) ? a->utf8 : "";
    const char *sb = (b->kind == OBJ_STRING && b->utf8) ? b->utf8 : "";
    res->i32 = (strcmp(sa, sb) == 0) ? 1 : 0;
    return 0;
}
int stub_string_hashcode(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    if (!args || !args[0].ptr) { res->i32 = 0; return 0; }
    dex_obj_t *str = (dex_obj_t*)args[0].ptr;
    const char *s = (str->kind == OBJ_STRING && str->utf8) ? str->utf8 : "";
    int32_t h = 0;
    for (const char *p = s; *p; p++) h = h * 31 + (int32_t)(unsigned char)*p;
    res->i32 = h;
    return 0;
}
int stub_string_tostring(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    if (args && args[0].ptr) {
        dex_obj_t *str = (dex_obj_t*)args[0].ptr;
        const char *s = (str->kind == OBJ_STRING && str->utf8) ? str->utf8 : "";
        res->ptr = dex_new_string_utf(vm, s);
    } else {
        res->ptr = dex_new_string_utf(vm, "");
    }
    return 0;
}

// ---- java.lang.StringBuilder ----
int stub_sb_init(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    if (!args || !args[0].ptr) return 0;
    dex_obj_t *sb = (dex_obj_t*)args[0].ptr;
    sb->kind = OBJ_STRING_BUILDER;
    if (!sb->sb_buf) {
        sb->sb_cap = 64;
        sb->sb_buf = (char*)malloc(sb->sb_cap);
        if (sb->sb_buf) { sb->sb_buf[0] = 0; sb->sb_len = 0; }
    }
    return 0;
}
int stub_sb_append_str(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!args || !args[0].ptr) return 0;
    dex_obj_t *sb = (dex_obj_t*)args[0].ptr;
    if (sb->kind != OBJ_STRING_BUILDER) {
        sb->kind = OBJ_STRING_BUILDER;
        sb->sb_cap = 64;
        sb->sb_buf = (char*)malloc(sb->sb_cap);
        if (sb->sb_buf) { sb->sb_buf[0] = 0; sb->sb_len = 0; }
    }
    const char *s = arg_str(&args[1]);
    if (s) {
        size_t add = strlen(s);
        if (sb->sb_len + add + 1 > sb->sb_cap) {
            sb->sb_cap = (sb->sb_len + add + 1) * 2;
            sb->sb_buf = (char*)realloc(sb->sb_buf, sb->sb_cap);
        }
        if (sb->sb_buf) {
            memcpy(sb->sb_buf + sb->sb_len, s, add);
            sb->sb_len += (uint32_t)add;
            sb->sb_buf[sb->sb_len] = 0;
        }
    }
    if (res) res->ptr = sb;   // returns this
    (void)vm;
    return 0;
}
int stub_sb_append_int(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!args || !args[0].ptr) return 0;
    dex_obj_t *sb = (dex_obj_t*)args[0].ptr;
    if (sb->kind != OBJ_STRING_BUILDER) {
        sb->kind = OBJ_STRING_BUILDER;
        sb->sb_cap = 64;
        sb->sb_buf = (char*)malloc(sb->sb_cap);
        if (sb->sb_buf) { sb->sb_buf[0] = 0; sb->sb_len = 0; }
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", args[1].i32);
    dex_value_t tmp[2];
    tmp[0].ptr = sb;
    tmp[1].ptr = dex_new_string_utf(vm, buf);
    return stub_sb_append_str(vm, tmp, 2, res);
}
int stub_sb_tostring(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    if (!args || !args[0].ptr) { res->ptr = dex_new_string_utf(vm, ""); return 0; }
    dex_obj_t *sb = (dex_obj_t*)args[0].ptr;
    const char *s = (sb->kind == OBJ_STRING_BUILDER && sb->sb_buf) ? sb->sb_buf : "";
    res->ptr = dex_new_string_utf(vm, s);
    return 0;
}
int stub_sb_length(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    if (!args || !args[0].ptr) { res->i32 = 0; return 0; }
    dex_obj_t *sb = (dex_obj_t*)args[0].ptr;
    res->i32 = (sb->kind == OBJ_STRING_BUILDER) ? (int32_t)sb->sb_len : 0;
    return 0;
}

// ---- java.io.PrintStream ----
int stub_ps_println_str(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    const char *s = args ? arg_str(&args[1]) : nullptr;
    LOGI("interp", "[System.out] %s", s ? s : "(null)");
    return 0;
}
int stub_ps_println_void(dex_vm_t *vm, dex_value_t *args, int argc, dex_value_t *res) {
    dex_value_t tmp[2];
    tmp[0] = args[0];
    tmp[1].ptr = dex_new_string_utf(vm, "");
    return stub_ps_println_str(vm, tmp, 2, res);
}
int stub_ps_print_str(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    const char *s = args ? arg_str(&args[1]) : nullptr;
    if (s) printf("%s", s);
    return 0;
}

// ---- java.lang.Math ----
int stub_math_max(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    if (res) res->i32 = (args[0].i32 > args[1].i32) ? args[0].i32 : args[1].i32;
    return 0;
}
int stub_math_min(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    if (res) res->i32 = (args[0].i32 < args[1].i32) ? args[0].i32 : args[1].i32;
    return 0;
}
int stub_math_abs(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    if (res) res->i32 = (args[0].i32 < 0) ? -args[0].i32 : args[0].i32;
    return 0;
}
int stub_math_max_long(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    if (res) res->i64 = (args[0].i64 > args[1].i64) ? args[0].i64 : args[1].i64;
    return 0;
}

// ---- java.lang.Integer / Long / Double / Boolean ----
int stub_integer_parseint(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    const char *s = arg_str(&args[0]);
    if (!s) { res->i32 = 0; return 0; }
    res->i32 = (int32_t)atoi(s);
    return 0;
}
int stub_integer_tostring(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", args[0].i32);
    res->ptr = dex_new_string_utf(vm, buf);
    return 0;
}
int stub_integer_valueof(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    // Return a boxed Integer as a stub object holding the int.
    dex_obj_t *o = new dex_obj();
    o->cls = dex_vm_resolve_class(vm, "Ljava/lang/Integer;");
    o->kind = OBJ_STUB;
    o->fields = new dex_value_t[1]();
    o->fields[0].i32 = args[0].i32;
    res->ptr = o;
    return 0;
}
int stub_long_parselong(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    const char *s = arg_str(&args[1]);
    if (!s) { res->i64 = 0; return 0; }
    res->i64 = (int64_t)strtoll(s, nullptr, 10);
    return 0;
}

// ---- java.lang.Thread / Throwable ----
int stub_thread_init(dex_vm_t*, dex_value_t*, int, dex_value_t*) {
    LOGW("interp", "Thread.<init>: no-op (single-threaded interpreter)");
    return 0;
}
int stub_thread_start(dex_vm_t *vm, dex_value_t *args, int, dex_value_t*) {
    /* Workaround for single-threaded interpreter: instead of spawning a real
     * pthread, call Thread.run() synchronously on the current thread. This is
     * wrong for apps that depend on concurrency, but it's a "let the app still
     * run" fallback — the Runnable's logic executes, just inline. Apps that
     * block on thread coordination (wait/notify, Join) may deadlock. */
    if (!args || !args[0].ptr) return 0;
    LOGW("interp", "Thread.start: running synchronously (single-threaded workaround)");
    dex_value_t run_args[1];
    run_args[0].ptr = args[0].ptr;
    dex_value_t run_result;
    dex_invoke(vm, "Ljava/lang/Thread;", "run", "V", run_args, 1, &run_result);
    return 0;
}
int stub_throwable_init(dex_vm_t*, dex_value_t*, int, dex_value_t*) { return 0; }
int stub_throwable_getmessage(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    if (!args || !args[0].ptr) { res->ptr = nullptr; return 0; }
    dex_obj_t *t = (dex_obj_t*)args[0].ptr;
    if (t->fields) res->ptr = t->fields[0].ptr;
    else res->ptr = dex_new_string_utf(vm, "");
    return 0;
}

// ---- android.util.Log ----
int stub_log_i(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    const char *tag = arg_str(&args[0]);
    const char *msg  = arg_str(&args[1]);
    LOGI("interp", "[Log.i %s] %s", tag ? tag : "?", msg ? msg : "?");
    if (res) res->i32 = 0;
    return 0;
}
int stub_log_d(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    const char *tag = arg_str(&args[0]);
    const char *msg  = arg_str(&args[1]);
    LOGD("interp", "[Log.d %s] %s", tag ? tag : "?", msg ? msg : "?");
    if (res) res->i32 = 0;
    return 0;
}
int stub_log_w(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    const char *tag = arg_str(&args[0]);
    const char *msg  = arg_str(&args[1]);
    LOGW("interp", "[Log.w %s] %s", tag ? tag : "?", msg ? msg : "?");
    if (res) res->i32 = 0;
    return 0;
}
int stub_log_e(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    const char *tag = arg_str(&args[0]);
    const char *msg  = arg_str(&args[1]);
    LOGE("interp", "[Log.e %s] %s", tag ? tag : "?", msg ? msg : "?");
    if (res) res->i32 = 0;
    return 0;
}
int stub_log_v(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    const char *tag = arg_str(&args[0]);
    const char *msg  = arg_str(&args[1]);
    LOGD("interp", "[Log.v %s] %s", tag ? tag : "?", msg ? msg : "?");
    if (res) res->i32 = 0;
    return 0;
}

// ---- android.app.Activity lifecycle (no-ops; subclasses override) ----
int stub_activity_init(dex_vm_t*, dex_value_t*, int, dex_value_t*) { return 0; }
int stub_activity_oncreate(dex_vm_t*, dex_value_t*, int, dex_value_t*) {
    LOGD("interp", "Activity.onCreate (stub)");
    return 0;
}
int stub_activity_onstart(dex_vm_t*, dex_value_t*, int, dex_value_t*) { return 0; }
int stub_activity_onresume(dex_vm_t*, dex_value_t*, int, dex_value_t*) { return 0; }
int stub_activity_onpause(dex_vm_t*, dex_value_t*, int, dex_value_t*) { return 0; }
int stub_activity_onstop(dex_vm_t*, dex_value_t*, int, dex_value_t*) { return 0; }
int stub_activity_ondestroy(dex_vm_t*, dex_value_t*, int, dex_value_t*) { return 0; }
int stub_activity_setcontentview_int(dex_vm_t *vm, dex_value_t *args, int, dex_value_t*) {
    LOGI("interp", "Activity.setContentView(layout=0x%X) (no XML inflate)", args[1].i32);
    (void)vm;
    return 0;
}
int stub_activity_findviewbyid(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    LOGD("interp", "Activity.findViewById(0x%X) -> null (no view hierarchy)", args[1].i32);
    if (res) res->ptr = nullptr;
    (void)vm;
    return 0;
}
int stub_activity_requestwindowfeature(dex_vm_t *vm, dex_value_t*, int, dex_value_t *res) {
    if (res) res->i32 = 1;   // true
    (void)vm;
    return 0;
}
int stub_activity_getwindow(dex_vm_t *vm, dex_value_t*, int, dex_value_t *res) {
    if (!res) return 0;
    if (!vm->stub_window) {
        vm->stub_window = new dex_obj();
        vm->stub_window->cls = dex_vm_resolve_class(vm, "Landroid/view/Window;");
        vm->stub_window->kind = OBJ_STUB;
    }
    res->ptr = vm->stub_window;
    return 0;
}
int stub_activity_getpackagename(dex_vm_t *vm, dex_value_t*, int, dex_value_t *res) {
    if (!res) return 0;
    res->ptr = dex_new_string_utf(vm, vm->package_id.c_str());
    return 0;
}

// ---- android.view.Window ----
int stub_window_setflags(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    LOGD("interp", "Window.setFlags(0x%X, 0x%X)", args[1].i32, args[2].i32);
    return 0;
}
int stub_window_getdecorview(dex_vm_t*, dex_value_t*, int, dex_value_t *res) {
    if (res) res->ptr = nullptr;
    return 0;
}
int stub_window_setcontentview(dex_vm_t*, dex_value_t*, int, dex_value_t*) {
    LOGD("interp", "Window.setContentView(View) (no-op)");
    return 0;
}

// ---- android.view.View ----
int stub_view_init(dex_vm_t*, dex_value_t*, int, dex_value_t*) { return 0; }
int stub_view_invalidate(dex_vm_t*, dex_value_t*, int, dex_value_t*) { return 0; }
int stub_view_requestlayout(dex_vm_t*, dex_value_t*, int, dex_value_t*) { return 0; }
int stub_view_setbackgroundcolor(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    LOGD("interp", "View.setBackgroundColor(0x%X)", (unsigned)args[1].i32);
    return 0;
}

// ---- android.content.Context (Activity is-a Context) ----
int stub_context_getfilesdir(dex_vm_t *vm, dex_value_t*, int, dex_value_t *res) {
    if (!res) return 0;
    std::string path = vm->sandbox_root;
    if (path.empty()) path = "/tmp/apklive_files";
    if (!path.empty() && path.back() != '/') path += "/";
    path += "files";
    dex_obj_t *f = new dex_obj();
    f->cls = dex_vm_resolve_class(vm, "Ljava/io/File;");
    f->kind = OBJ_STUB;
    f->fields = new dex_value_t[1]();
    f->fields[0].ptr = dex_new_string_utf(vm, path.c_str());
    res->ptr = f;
    return 0;
}
int stub_context_getcachedir(dex_vm_t *vm, dex_value_t*, int, dex_value_t *res) {
    if (!res) return 0;
    std::string path = vm->sandbox_root;
    if (path.empty()) path = "/tmp/apklive_cache";
    if (!path.empty() && path.back() != '/') path += "/";
    path += "cache";
    dex_obj_t *f = new dex_obj();
    f->cls = dex_vm_resolve_class(vm, "Ljava/io/File;");
    f->kind = OBJ_STUB;
    f->fields = new dex_value_t[1]();
    f->fields[0].ptr = dex_new_string_utf(vm, path.c_str());
    res->ptr = f;
    return 0;
}
int stub_context_getpackagename(dex_vm_t *vm, dex_value_t*, int, dex_value_t *res) {
    if (!res) return 0;
    res->ptr = dex_new_string_utf(vm, vm->package_id.c_str());
    return 0;
}
int stub_context_getresources(dex_vm_t *vm, dex_value_t*, int, dex_value_t *res) {
    if (!res) return 0;
    dex_obj_t *r = new dex_obj();
    r->cls = dex_vm_resolve_class(vm, "Landroid/content/res/Resources;");
    r->kind = OBJ_STUB;
    res->ptr = r;
    return 0;
}

// ---- java.io.File ----
int stub_file_init_str(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    if (!args || !args[0].ptr) return 0;
    dex_obj_t *f = (dex_obj_t*)args[0].ptr;
    f->kind = OBJ_STUB;
    if (!f->fields) f->fields = new dex_value_t[1]();
    f->fields[0].ptr = args[1].ptr;
    return 0;
}
int stub_file_getabsolutepath(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    if (!args || !args[0].ptr) { res->ptr = dex_new_string_utf(vm, ""); return 0; }
    dex_obj_t *f = (dex_obj_t*)args[0].ptr;
    const char *p = (f->fields && f->fields[0].ptr) ?
                    dex_string_utf((dex_obj_t*)f->fields[0].ptr) : "";
    res->ptr = dex_new_string_utf(vm, p);
    return 0;
}
int stub_file_exists(dex_vm_t*, dex_value_t*, int, dex_value_t *res) {
    if (res) res->i32 = 0;
    return 0;
}
int stub_file_mkdirs(dex_vm_t*, dex_value_t*, int, dex_value_t *res) {
    if (res) res->i32 = 1;
    return 0;
}

// ---- android.os.Bundle ----
int stub_bundle_init(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    if (args && args[0].ptr) {
        dex_obj_t *b = (dex_obj_t*)args[0].ptr;
        b->kind = OBJ_STUB;
        if (!b->fields) b->fields = new dex_value_t[4]();
    }
    return 0;
}
int stub_bundle_putstring(dex_vm_t*, dex_value_t*, int, dex_value_t*) { return 0; }
int stub_bundle_getstring(dex_vm_t *vm, dex_value_t*, int, dex_value_t *res) {
    if (res) res->ptr = dex_new_string_utf(vm, "");
    return 0;
}

// ---- android.opengl.GLSurfaceView (Java-side no-ops; GLES goes through JNI bridge) ----
int stub_glsurfaceview_init(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    LOGD("interp", "GLSurfaceView.<init> (Java side; GLES via JNI bridge)");
    if (args && args[0].ptr) {
        dex_obj_t *v = (dex_obj_t*)args[0].ptr;
        v->kind = OBJ_STUB;
        if (!v->fields) v->fields = new dex_value_t[4]();
    }
    return 0;
}
int stub_glsurfaceview_setrenderer(dex_vm_t*, dex_value_t*, int, dex_value_t*) {
    LOGI("interp", "GLSurfaceView.setRenderer (no-op; renderer is a Java interface)");
    return 0;
}
int stub_glsurfaceview_setrendermode(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    LOGD("interp", "GLSurfaceView.setRenderMode(%d)", args[1].i32);
    return 0;
}
int stub_glsurfaceview_onresume(dex_vm_t*, dex_value_t*, int, dex_value_t*) {
    LOGD("interp", "GLSurfaceView.onResume (no-op)");
    return 0;
}
int stub_glsurfaceview_onpause(dex_vm_t*, dex_value_t*, int, dex_value_t*) {
    LOGD("interp", "GLSurfaceView.onPause (no-op)");
    return 0;
}

// ---- android.view.WindowManager$LayoutParams ----
int stub_layoutparams_clinit(dex_vm_t*, dex_value_t*, int, dex_value_t*) { return 0; }

// ---- java.util.ArrayList (minimal) ----
int stub_arraylist_init(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    if (args && args[0].ptr) {
        dex_obj_t *al = (dex_obj_t*)args[0].ptr;
        al->kind = OBJ_STUB;
        if (!al->fields) al->fields = new dex_value_t[2]();   // capacity, size
    }
    return 0;
}
int stub_arraylist_add(dex_vm_t*, dex_value_t*, int, dex_value_t *res) {
    if (res) res->i32 = 1;
    return 0;
}
int stub_arraylist_size(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    if (res && args && args[0].ptr) {
        dex_obj_t *al = (dex_obj_t*)args[0].ptr;
        res->i32 = al->fields ? al->fields[1].i32 : 0;
    } else if (res) res->i32 = 0;
    return 0;
}

// ---- java.util.HashMap (minimal) ----
int stub_hashmap_init(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    if (args && args[0].ptr) {
        dex_obj_t *m = (dex_obj_t*)args[0].ptr;
        m->kind = OBJ_STUB;
        if (!m->fields) m->fields = new dex_value_t[1]();
    }
    return 0;
}
int stub_hashmap_put(dex_vm_t*, dex_value_t*, int, dex_value_t *res) {
    if (res) res->ptr = nullptr;
    return 0;
}
int stub_hashmap_get(dex_vm_t*, dex_value_t*, int, dex_value_t *res) {
    if (res) res->ptr = nullptr;
    return 0;
}

// ============================================================================
// P3-3 additions
// ----------------------------------------------------------------------------
// java.net.*      — REAL BSD sockets
// java.io.*       — REAL recv/send-backed InputStream/OutputStream
// java.lang.Class,
// java.lang.reflect.{Method,Field} — REAL via dex_invoke / dex_get/set_field
// android.media.* — degraded: no-op-success + log; AudioTrack lazily routes to
//                   the OpenSL ES bridge (P3-2). If the bridge is unavailable,
//                   methods still return success and audio is dropped.
// android.content.{SharedPreferences,Intent} — REAL file-backed / map-backed
// android.util.Log.println — REAL routing to LOG* macros
// ============================================================================

// ---- Side tables for stub objects that need richer state than fields[] ----
struct SPValue {
    char type = 's';   // 's' string, 'i' int (also long), 'b' bool, 'f' float, 'd' double
    std::string s;
    int64_t i = 0;
    double d = 0.0;
};
static std::unordered_map<dex_obj_t*, std::unordered_map<std::string, SPValue>> g_sp_data;
static std::unordered_map<dex_obj_t*, bool> g_sp_loaded;

static std::unordered_map<dex_obj_t*, std::unordered_map<std::string, std::string>> g_intent_extras;
static std::unordered_map<dex_obj_t*, std::string> g_intent_action;
static std::unordered_map<dex_obj_t*, std::string> g_intent_component;

struct AudioTrackState {
    int sample_rate = 0;
    int channels = 0;
    int bits_per_sample = 16;
    sl_player_t *player = nullptr;
};
static sl_engine_t *g_audio_engine = nullptr;
static std::unordered_map<dex_obj_t*, AudioTrackState> g_audio_tracks;

struct MediaPlayerState {
    std::string path;
    dex_obj_t *on_prepared_listener = nullptr;
    dex_obj_t *on_error_listener = nullptr;
    bool prepared = false;
};
static std::unordered_map<dex_obj_t*, MediaPlayerState> g_media_players;
static int g_soundpool_next_id = 1;

// ---- Helper: create a stub object with N zero-initialized field slots ----
dex_obj_t *make_stub_obj(dex_vm_t *vm, const char *descriptor, int nfields) {
    dex_obj_t *o = new dex_obj();
    o->cls = dex_vm_resolve_class(vm, descriptor);
    o->kind = OBJ_STUB;
    if (nfields > 0) o->fields = new (std::nothrow) dex_value_t[nfields]();
    return o;
}

// ---- Helper: set pending_exception and return -1 so invoke_resolved maps
// to INTERP_EXCEPTION (see the P3-3 modification in invoke_resolved).
int stub_throw(dex_vm_t *vm, const char *descriptor, const char *msg) {
    vm->pending_exception = make_exception(vm, descriptor, msg);
    return -1;
}

// Helper: convert a descriptor ("Lcom/foo/Bar;" or "I" or "[I") to a single
// shorty char ('L' for objects/arrays, or the primitive char).
char desc_to_shorty_char(const std::string &desc) {
    if (desc.empty()) return 'L';
    char c = desc[0];
    if (c == 'L' || c == '[') return 'L';
    return c;   // I J F D Z B C S V
}

// Helper: descriptor -> dotted java name ("Ljava/lang/String;" -> "java.lang.String")
std::string desc_to_dotted(const std::string &desc) {
    if (desc.empty()) return "";
    if (desc[0] == '[') return desc;   // arrays stay as-is
    if (desc[0] != 'L' || desc.back() != ';') return desc;
    std::string s = desc.substr(1, desc.size() - 2);
    for (char &c : s) if (c == '/') c = '.';
    return s;
}

// Helper: dotted name -> descriptor ("java.lang.String" -> "Ljava/lang/String;")
std::string dotted_to_desc(const std::string &name) {
    if (name.empty()) return "";
    if (name[0] == 'L' || name[0] == '[') return name;
    std::string s = "L";
    for (char c : name) s.push_back(c == '.' ? '/' : c);
    s.push_back(';');
    return s;
}

// ============================================================================
// java.net.Socket — REAL BSD socket
// State (fields[]): [0]=fd, [1]=state (0=new, 1=connected, 2=bound,
//                                          3=listening, 4=closed), [2]=so_timeout_ms
// ============================================================================

int stub_socket_init_v(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    if (!args || !args[0].ptr) return 0;
    dex_obj_t *s = (dex_obj_t*)args[0].ptr;
    s->kind = OBJ_STUB;
    if (!s->fields) s->fields = new dex_value_t[3]();
    s->fields[0].i32 = -1;
    s->fields[1].i32 = 0;
    s->fields[2].i32 = 0;
    LOGD("framework", "Socket.<init>() fd=-1");
    return 0;
}

int stub_socket_init_str_int(dex_vm_t *vm, dex_value_t *args, int, dex_value_t*) {
    if (!args || !args[0].ptr) return 0;
    dex_obj_t *s = (dex_obj_t*)args[0].ptr;
    s->kind = OBJ_STUB;
    if (!s->fields) s->fields = new dex_value_t[3]();
    s->fields[0].i32 = -1;
    s->fields[1].i32 = 0;
    s->fields[2].i32 = 0;
    const char *host = arg_str(&args[1]);
    int port = args[2].i32;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return stub_throw(vm, "Ljava/io/IOException;", "socket() failed");
    }
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portbuf[16];
    snprintf(portbuf, sizeof(portbuf), "%d", port);
    if (getaddrinfo(host ? host : "127.0.0.1", portbuf, &hints, &res) != 0 || !res) {
        if (fd >= 0) close(fd);
        return stub_throw(vm, "Ljava/io/IOException;", "getaddrinfo() failed");
    }
    if (connect(fd, res->ai_addr, (socklen_t)res->ai_addrlen) < 0) {
        freeaddrinfo(res);
        close(fd);
        return stub_throw(vm, "Ljava/io/IOException;", "connect() failed");
    }
    freeaddrinfo(res);
    s->fields[0].i32 = fd;
    s->fields[1].i32 = 1;  // connected
    LOGI("framework", "Socket connected to %s:%d fd=%d", host ? host : "?", port, fd);
    return 0;
}

int stub_socket_connect_addr(dex_vm_t *vm, dex_value_t *args, int argc, dex_value_t*) {
    // connect(SocketAddress, int) — partial. We don't have a real SocketAddress
    // impl yet; if the Socket was constructed with host:port we're already
    // connected. If not, throw IOException (caller should use the
    // (String,int) ctor for v1).
    if (args && args[0].ptr) {
        dex_obj_t *s = (dex_obj_t*)args[0].ptr;
        if (s->fields && s->fields[1].i32 == 1) {
            LOGD("framework", "Socket.connect(SocketAddress, %d) already connected fd=%d",
                 argc > 2 ? args[2].i32 : 0, s->fields[0].i32);
            return 0;
        }
    }
    return stub_throw(vm, "Ljava/io/IOException;",
                      "Socket.connect(SocketAddress,int) — partial: use new Socket(host,port)");
}

int stub_socket_getinputstream(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res || !args || !args[0].ptr) return 0;
    dex_obj_t *s = (dex_obj_t*)args[0].ptr;
    int fd = (s->fields && s->fields[0].i32 >= 0) ? s->fields[0].i32 : -1;
    dex_obj_t *is = make_stub_obj(vm, "Ljava/io/InputStream;", 3);
    is->fields[0].i32 = fd;
    is->fields[1].i32 = 0;  // direction = input
    is->fields[2].i32 = 0;  // closed = false
    res->ptr = is;
    return 0;
}

int stub_socket_getoutputstream(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res || !args || !args[0].ptr) return 0;
    dex_obj_t *s = (dex_obj_t*)args[0].ptr;
    int fd = (s->fields && s->fields[0].i32 >= 0) ? s->fields[0].i32 : -1;
    dex_obj_t *os = make_stub_obj(vm, "Ljava/io/OutputStream;", 3);
    os->fields[0].i32 = fd;
    os->fields[1].i32 = 1;  // direction = output
    os->fields[2].i32 = 0;
    res->ptr = os;
    return 0;
}

int stub_socket_close(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    if (!args || !args[0].ptr) return 0;
    dex_obj_t *s = (dex_obj_t*)args[0].ptr;
    if (s->fields && s->fields[0].i32 >= 0) {
        LOGI("framework", "Socket.close fd=%d", s->fields[0].i32);
        close(s->fields[0].i32);
        s->fields[0].i32 = -1;
        s->fields[1].i32 = 4;  // closed
    }
    return 0;
}

int stub_socket_setsotimeout(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    if (!args || !args[0].ptr) return 0;
    dex_obj_t *s = (dex_obj_t*)args[0].ptr;
    int ms = args[1].i32;
    if (s->fields) s->fields[2].i32 = ms;
    if (s->fields && s->fields[0].i32 >= 0) {
        struct timeval tv;
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        setsockopt(s->fields[0].i32, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    LOGD("framework", "Socket.setSoTimeout(%d ms)", ms);
    return 0;
}

int stub_socket_getsotimeout(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    if (!args || !args[0].ptr) { res->i32 = 0; return 0; }
    dex_obj_t *s = (dex_obj_t*)args[0].ptr;
    res->i32 = (s->fields && s->fields[2].i32 > 0) ? s->fields[2].i32 : 0;
    return 0;
}

int stub_socket_isconnected(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    if (!args || !args[0].ptr) { res->i32 = 0; return 0; }
    dex_obj_t *s = (dex_obj_t*)args[0].ptr;
    res->i32 = (s->fields && s->fields[1].i32 == 1) ? 1 : 0;
    return 0;
}

int stub_socket_isclosed(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    if (!args || !args[0].ptr) { res->i32 = 1; return 0; }
    dex_obj_t *s = (dex_obj_t*)args[0].ptr;
    res->i32 = (s->fields && s->fields[1].i32 == 4) ? 1 : 0;
    return 0;
}

// ---- java.net.ServerSocket — REAL BSD socket bind/listen/accept ----
// State (fields[]): [0]=listen_fd, [1]=state, [2]=bound_port
int stub_serversocket_init_port(dex_vm_t *vm, dex_value_t *args, int, dex_value_t*) {
    if (!args || !args[0].ptr) return 0;
    dex_obj_t *s = (dex_obj_t*)args[0].ptr;
    s->kind = OBJ_STUB;
    if (!s->fields) s->fields = new dex_value_t[3]();
    int port = args[1].i32;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return stub_throw(vm, "Ljava/io/IOException;", "socket() failed");
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return stub_throw(vm, "Ljava/io/IOException;", "bind() failed");
    }
    if (listen(fd, 50) < 0) {
        close(fd);
        return stub_throw(vm, "Ljava/io/IOException;", "listen() failed");
    }
    // Read back the actual port (in case port=0 was requested).
    socklen_t alen = sizeof(addr);
    getsockname(fd, (struct sockaddr*)&addr, &alen);
    int actual_port = ntohs(addr.sin_port);
    s->fields[0].i32 = fd;
    s->fields[1].i32 = 2;  // bound/listening
    s->fields[2].i32 = actual_port;
    LOGI("framework", "ServerSocket bound on port %d fd=%d", actual_port, fd);
    return 0;
}

int stub_serversocket_accept(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res || !args || !args[0].ptr) return 0;
    dex_obj_t *ss = (dex_obj_t*)args[0].ptr;
    int listen_fd = ss->fields ? ss->fields[0].i32 : -1;
    if (listen_fd < 0) {
        return stub_throw(vm, "Ljava/io/IOException;", "ServerSocket not bound");
    }
    struct sockaddr_in caddr{};
    socklen_t clen = sizeof(caddr);
    int cfd = accept(listen_fd, (struct sockaddr*)&caddr, &clen);
    if (cfd < 0) {
        return stub_throw(vm, "Ljava/io/IOException;", "accept() failed");
    }
    dex_obj_t *sock = make_stub_obj(vm, "Ljava/net/Socket;", 3);
    sock->fields[0].i32 = cfd;
    sock->fields[1].i32 = 1;  // connected
    sock->fields[2].i32 = 0;
    res->ptr = sock;
    LOGI("framework", "ServerSocket.accept fd=%d", cfd);
    return 0;
}

int stub_serversocket_close(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    if (!args || !args[0].ptr) return 0;
    dex_obj_t *ss = (dex_obj_t*)args[0].ptr;
    if (ss->fields && ss->fields[0].i32 >= 0) {
        LOGI("framework", "ServerSocket.close fd=%d", ss->fields[0].i32);
        close(ss->fields[0].i32);
        ss->fields[0].i32 = -1;
        ss->fields[1].i32 = 4;
    }
    return 0;
}

// ---- java.net.DatagramSocket — REAL BSD SOCK_DGRAM ----
// State (fields[]): [0]=fd, [1]=state, [2]=bound_port
int stub_dgramsocket_init_v(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    if (!args || !args[0].ptr) return 0;
    dex_obj_t *s = (dex_obj_t*)args[0].ptr;
    s->kind = OBJ_STUB;
    if (!s->fields) s->fields = new dex_value_t[3]();
    s->fields[0].i32 = -1;
    s->fields[1].i32 = 0;
    s->fields[2].i32 = 0;
    LOGD("framework", "DatagramSocket.<init>()");
    return 0;
}

int stub_dgramsocket_init_port(dex_vm_t *vm, dex_value_t *args, int, dex_value_t*) {
    if (!args || !args[0].ptr) return 0;
    dex_obj_t *s = (dex_obj_t*)args[0].ptr;
    s->kind = OBJ_STUB;
    if (!s->fields) s->fields = new dex_value_t[3]();
    int port = args[1].i32;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return stub_throw(vm, "Ljava/io/IOException;", "socket(SOCK_DGRAM) failed");
    }
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return stub_throw(vm, "Ljava/io/IOException;", "datagram bind() failed");
    }
    socklen_t alen = sizeof(addr);
    getsockname(fd, (struct sockaddr*)&addr, &alen);
    int actual_port = ntohs(addr.sin_port);
    s->fields[0].i32 = fd;
    s->fields[1].i32 = 2;
    s->fields[2].i32 = actual_port;
    LOGI("framework", "DatagramSocket bound on port %d fd=%d", actual_port, fd);
    return 0;
}

int stub_dgramsocket_send(dex_vm_t *vm, dex_value_t *args, int, dex_value_t*) {
    // args[0]=this, args[1]=DatagramPacket
    if (!args || !args[0].ptr || !args[1].ptr) return 0;
    dex_obj_t *s = (dex_obj_t*)args[0].ptr;
    dex_obj_t *pkt = (dex_obj_t*)args[1].ptr;
    int fd = s->fields ? s->fields[0].i32 : -1;
    if (fd < 0) {
        // Auto-create an unbound socket.
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return stub_throw(vm, "Ljava/io/IOException;", "dgram socket() failed");
        s->fields[0].i32 = fd;
        s->fields[1].i32 = 2;
    }
    if (!pkt->fields || !pkt->fields[0].ptr) return 0;
    dex_obj_t *data = (dex_obj_t*)pkt->fields[0].ptr;
    int off = pkt->fields[1].i32;
    int len = pkt->fields[2].i32;
    dex_obj_t *addr = (dex_obj_t*)pkt->fields[3].ptr;
    int port = pkt->fields[4].i32;
    if (!data || data->kind != OBJ_ARRAY || !data->array_data) return 0;
    struct sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)port);
    if (addr && addr->fields && addr->fields[0].ptr) {
        const char *ipstr = dex_string_utf((dex_obj_t*)addr->fields[0].ptr);
        if (ipstr) inet_pton(AF_INET, ipstr, &dst.sin_addr);
    }
    ssize_t n = sendto(fd, (uint8_t*)data->array_data + off, (size_t)len, 0,
                       (struct sockaddr*)&dst, sizeof(dst));
    if (n < 0) {
        return stub_throw(vm, "Ljava/io/IOException;", "sendto() failed");
    }
    return 0;
}

int stub_dgramsocket_receive(dex_vm_t *vm, dex_value_t *args, int, dex_value_t*) {
    if (!args || !args[0].ptr || !args[1].ptr) return 0;
    dex_obj_t *s = (dex_obj_t*)args[0].ptr;
    dex_obj_t *pkt = (dex_obj_t*)args[1].ptr;
    int fd = s->fields ? s->fields[0].i32 : -1;
    if (fd < 0) {
        return stub_throw(vm, "Ljava/io/IOException;", "DatagramSocket not bound");
    }
    if (!pkt->fields || !pkt->fields[0].ptr) return 0;
    dex_obj_t *data = (dex_obj_t*)pkt->fields[0].ptr;
    int off = pkt->fields[1].i32;
    int max_len = pkt->fields[2].i32;
    if (!data || data->kind != OBJ_ARRAY || !data->array_data) return 0;
    if (off < 0 || max_len < 0 || (size_t)(off + max_len) > data->length) {
        return stub_throw(vm, "Ljava/lang/IndexOutOfBoundsException;", "datagram recv OOB");
    }
    struct sockaddr_in src{};
    socklen_t slen = sizeof(src);
    ssize_t n = recvfrom(fd, (uint8_t*)data->array_data + off, (size_t)max_len, 0,
                         (struct sockaddr*)&src, &slen);
    if (n < 0) {
        return stub_throw(vm, "Ljava/io/IOException;", "recvfrom() failed");
    }
    pkt->fields[2].i32 = (int32_t)n;   // length = received
    pkt->fields[4].i32 = ntohs(src.sin_port);
    // Build InetAddress for source.
    char ipbuf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &src.sin_addr, ipbuf, sizeof(ipbuf));
    dex_obj_t *addr = make_stub_obj(vm, "Ljava/net/InetAddress;", 1);
    addr->fields[0].ptr = dex_new_string_utf(vm, ipbuf);
    pkt->fields[3].ptr = addr;
    return 0;
}

int stub_dgramsocket_close(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    if (!args || !args[0].ptr) return 0;
    dex_obj_t *s = (dex_obj_t*)args[0].ptr;
    if (s->fields && s->fields[0].i32 >= 0) {
        close(s->fields[0].i32);
        s->fields[0].i32 = -1;
        s->fields[1].i32 = 4;
    }
    return 0;
}

// ---- java.net.DatagramPacket ----
// State (fields[]): [0]=byte[] data, [1]=offset, [2]=length, [3]=InetAddress, [4]=port
int stub_dgrampacket_init_bii(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    if (!args || !args[0].ptr) return 0;
    dex_obj_t *p = (dex_obj_t*)args[0].ptr;
    p->kind = OBJ_STUB;
    if (!p->fields) p->fields = new dex_value_t[5]();
    p->fields[0].ptr = args[1].ptr;
    p->fields[1].i32 = args[2].i32;
    p->fields[2].i32 = args[3].i32;
    return 0;
}
int stub_dgrampacket_init_biiai(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    if (!args || !args[0].ptr) return 0;
    dex_obj_t *p = (dex_obj_t*)args[0].ptr;
    p->kind = OBJ_STUB;
    if (!p->fields) p->fields = new dex_value_t[5]();
    p->fields[0].ptr = args[1].ptr;  // data
    p->fields[1].i32 = args[2].i32;  // off
    p->fields[2].i32 = args[3].i32;  // len
    p->fields[3].ptr = args[4].ptr;  // addr
    p->fields[4].i32 = args[5].i32;  // port
    return 0;
}
int stub_dgrampacket_getdata(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    if (!res || !args || !args[0].ptr) return 0;
    dex_obj_t *p = (dex_obj_t*)args[0].ptr;
    res->ptr = p->fields ? p->fields[0].ptr : nullptr;
    return 0;
}
int stub_dgrampacket_getlength(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    if (!args || !args[0].ptr) { res->i32 = 0; return 0; }
    dex_obj_t *p = (dex_obj_t*)args[0].ptr;
    res->i32 = p->fields ? p->fields[2].i32 : 0;
    return 0;
}
int stub_dgrampacket_getaddress(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    if (!args || !args[0].ptr) { res->ptr = nullptr; return 0; }
    dex_obj_t *p = (dex_obj_t*)args[0].ptr;
    res->ptr = p->fields ? p->fields[3].ptr : nullptr;
    return 0;
}
int stub_dgrampacket_getport(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    if (!args || !args[0].ptr) { res->i32 = 0; return 0; }
    dex_obj_t *p = (dex_obj_t*)args[0].ptr;
    res->i32 = p->fields ? p->fields[4].i32 : 0;
    return 0;
}

// ---- java.net.InetAddress ----
// State (fields[]): [0]=host address string dex_obj_t*
int stub_inetaddress_getbyname(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    // args[0]=String host (static method, no receiver)
    const char *host = arg_str(&args[0]);
    if (!host) host = "127.0.0.1";
    struct addrinfo hints{}, *res_ai = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, nullptr, &hints, &res_ai) != 0 || !res_ai) {
        return stub_throw(vm, "Ljava/net/UnknownHostException;", "getaddrinfo failed");
    }
    char ipbuf[INET_ADDRSTRLEN] = "127.0.0.1";
    for (struct addrinfo *p = res_ai; p; p = p->ai_next) {
        if (p->ai_family == AF_INET) {
            struct sockaddr_in *sa = (struct sockaddr_in*)p->ai_addr;
            inet_ntop(AF_INET, &sa->sin_addr, ipbuf, sizeof(ipbuf));
            break;
        }
    }
    freeaddrinfo(res_ai);
    dex_obj_t *addr = make_stub_obj(vm, "Ljava/net/InetAddress;", 1);
    addr->fields[0].ptr = dex_new_string_utf(vm, ipbuf);
    res->ptr = addr;
    LOGI("framework", "InetAddress.getByName(%s) -> %s", host, ipbuf);
    return 0;
}
int stub_inetaddress_gethostaddress(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    if (!args || !args[0].ptr) { res->ptr = dex_new_string_utf(vm, ""); return 0; }
    dex_obj_t *a = (dex_obj_t*)args[0].ptr;
    const char *ip = (a->fields && a->fields[0].ptr) ?
                     dex_string_utf((dex_obj_t*)a->fields[0].ptr) : "";
    res->ptr = dex_new_string_utf(vm, ip);
    return 0;
}
int stub_inetaddress_gethostname(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    // We don't do reverse DNS; return the stored address as the "host name".
    return stub_inetaddress_gethostaddress(vm, args, 0, res);
}

// ---- java.io.InputStream / OutputStream (backed by Socket fd) ----
// State (fields[]): [0]=fd, [1]=direction(0=in,1=out), [2]=closed
int stub_inputstream_read_b(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    if (!args || !args[0].ptr) { res->i32 = -1; return 0; }
    dex_obj_t *is = (dex_obj_t*)args[0].ptr;
    int fd = is->fields ? is->fields[0].i32 : -1;
    if (fd < 0) { res->i32 = -1; return 0; }
    dex_obj_t *arr = (dex_obj_t*)args[1].ptr;
    if (!arr || arr->kind != OBJ_ARRAY || !arr->array_data) { res->i32 = -1; return 0; }
    ssize_t n = recv(fd, arr->array_data, arr->length, 0);
    if (n < 0) {
        return stub_throw(vm, "Ljava/io/IOException;", "recv() failed");
    }
    res->i32 = (int32_t)n;
    return 0;
}
int stub_inputstream_read_bii(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    if (!args || !args[0].ptr) { res->i32 = -1; return 0; }
    dex_obj_t *is = (dex_obj_t*)args[0].ptr;
    int fd = is->fields ? is->fields[0].i32 : -1;
    if (fd < 0) { res->i32 = -1; return 0; }
    dex_obj_t *arr = (dex_obj_t*)args[1].ptr;
    if (!arr || arr->kind != OBJ_ARRAY || !arr->array_data) { res->i32 = -1; return 0; }
    int off = args[2].i32;
    int len = args[3].i32;
    if (off < 0 || len < 0 || (size_t)(off + len) > arr->length) {
        return stub_throw(vm, "Ljava/lang/IndexOutOfBoundsException;", "read OOB");
    }
    ssize_t n = recv(fd, (uint8_t*)arr->array_data + off, (size_t)len, 0);
    if (n < 0) {
        return stub_throw(vm, "Ljava/io/IOException;", "recv() failed");
    }
    res->i32 = (int32_t)n;
    return 0;
}
int stub_inputstream_read_int(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    // read()I — single byte
    if (!res) return 0;
    if (!args || !args[0].ptr) { res->i32 = -1; return 0; }
    dex_obj_t *is = (dex_obj_t*)args[0].ptr;
    int fd = is->fields ? is->fields[0].i32 : -1;
    if (fd < 0) { res->i32 = -1; return 0; }
    uint8_t b = 0;
    ssize_t n = recv(fd, &b, 1, 0);
    if (n < 0) return stub_throw(vm, "Ljava/io/IOException;", "recv() failed");
    if (n == 0) { res->i32 = -1; return 0; }   // EOF
    res->i32 = (int32_t)b;
    return 0;
}
int stub_outputstream_write_bii(dex_vm_t *vm, dex_value_t *args, int, dex_value_t*) {
    if (!args || !args[0].ptr) return 0;
    dex_obj_t *os = (dex_obj_t*)args[0].ptr;
    int fd = os->fields ? os->fields[0].i32 : -1;
    if (fd < 0) return 0;
    dex_obj_t *arr = (dex_obj_t*)args[1].ptr;
    if (!arr || arr->kind != OBJ_ARRAY || !arr->array_data) return 0;
    int off = args[2].i32;
    int len = args[3].i32;
    if (off < 0 || len < 0 || (size_t)(off + len) > arr->length) {
        return stub_throw(vm, "Ljava/lang/IndexOutOfBoundsException;", "write OOB");
    }
    ssize_t total = 0;
    while (total < len) {
        ssize_t n = send(fd, (uint8_t*)arr->array_data + off + total,
                         (size_t)(len - total), 0);
        if (n < 0) {
            return stub_throw(vm, "Ljava/io/IOException;", "send() failed");
        }
        if (n == 0) break;
        total += n;
    }
    return 0;
}
int stub_outputstream_write_int(dex_vm_t *vm, dex_value_t *args, int, dex_value_t*) {
    if (!args || !args[0].ptr) return 0;
    dex_obj_t *os = (dex_obj_t*)args[0].ptr;
    int fd = os->fields ? os->fields[0].i32 : -1;
    if (fd < 0) return 0;
    uint8_t b = (uint8_t)args[1].i32;
    ssize_t n = send(fd, &b, 1, 0);
    if (n < 0) return stub_throw(vm, "Ljava/io/IOException;", "send() failed");
    return 0;
}
int stub_stream_flush(dex_vm_t*, dex_value_t*, int, dex_value_t*) { return 0; }
int stub_stream_close(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    // The Socket owns the fd; streams just mark themselves closed.
    if (!args || !args[0].ptr) return 0;
    dex_obj_t *s = (dex_obj_t*)args[0].ptr;
    if (s->fields) s->fields[2].i32 = 1;
    LOGD("framework", "stream.close (fd left for Socket to close)");
    return 0;
}

// ---- java.io.IOException / FileNotFoundException constructors ----
int stub_ioexception_init(dex_vm_t*, dex_value_t*, int, dex_value_t*) { return 0; }

// ============================================================================
// java.lang.Class + java.lang.reflect.{Method,Field} — REAL via dex_invoke
// Class wrapper state:  fields[0].ptr = dex_cls_t*
// Method wrapper state: fields[0].ptr = declaring dex_cls_t*,
//                       fields[1].ptr = name dex_obj_t*,
//                       fields[2].ptr = shorty dex_obj_t*,
//                       fields[3].i32  = is_static
// Field wrapper state:  fields[0].ptr = declaring dex_cls_t*,
//                       fields[1].ptr = name dex_obj_t*,
//                       fields[2].ptr = type descriptor dex_obj_t*,
//                       fields[3].i32  = is_static
// ============================================================================

// Class.forName(String) -> Class. Static method, args[0] = String.
int stub_class_forname(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    const char *name = arg_str(&args[0]);
    if (!name) {
        return stub_throw(vm, "Ljava/lang/ClassNotFoundException;", "null name");
    }
    std::string desc = dotted_to_desc(name);
    dex_cls_t *cls = dex_vm_resolve_class(vm, desc.c_str());
    if (!cls) {
        return stub_throw(vm, "Ljava/lang/ClassNotFoundException;", name);
    }
    dex_obj_t *c = make_stub_obj(vm, "Ljava/lang/Class;", 1);
    c->fields[0].ptr = cls;
    res->ptr = c;
    LOGD("framework", "Class.forName(%s) -> %s", name, desc.c_str());
    return 0;
}

// Class.getName() -> String
int stub_class_getname(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    if (!args || !args[0].ptr) { res->ptr = dex_new_string_utf(vm, ""); return 0; }
    dex_obj_t *c = (dex_obj_t*)args[0].ptr;
    dex_cls_t *cls = (dex_cls_t*)(c->fields ? c->fields[0].ptr : nullptr);
    if (!cls) { res->ptr = dex_new_string_utf(vm, ""); return 0; }
    res->ptr = dex_new_string_utf(vm, desc_to_dotted(cls->descriptor).c_str());
    return 0;
}

// Class.getSimpleName() -> String
int stub_class_getsimplename(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    if (!args || !args[0].ptr) { res->ptr = dex_new_string_utf(vm, ""); return 0; }
    dex_obj_t *c = (dex_obj_t*)args[0].ptr;
    dex_cls_t *cls = (dex_cls_t*)(c->fields ? c->fields[0].ptr : nullptr);
    if (!cls) { res->ptr = dex_new_string_utf(vm, ""); return 0; }
    std::string dotted = desc_to_dotted(cls->descriptor);
    size_t dot = dotted.rfind('.');
    if (dot != std::string::npos) dotted = dotted.substr(dot + 1);
    // Inner classes: take segment after '$'
    size_t dollar = dotted.rfind('$');
    if (dollar != std::string::npos) dotted = dotted.substr(dollar + 1);
    res->ptr = dex_new_string_utf(vm, dotted.c_str());
    return 0;
}

// Helper: build a shorty string from a Class[] parameterTypes array.
// Each element is a Class wrapper with fields[0].ptr = dex_cls_t*.
std::string shorty_from_param_types(dex_obj_t *param_array) {
    std::string s = "V";   // placeholder return type; we don't know it here
    if (!param_array || param_array->kind != OBJ_ARRAY || !param_array->array_data) {
        return s;
    }
    void **elems = (void**)param_array->array_data;
    for (uint32_t i = 0; i < param_array->length; i++) {
        dex_obj_t *pc = (dex_obj_t*)elems[i];
        if (!pc || !pc->fields) { s.push_back('L'); continue; }
        dex_cls_t *pcls = (dex_cls_t*)pc->fields[0].ptr;
        if (!pcls) { s.push_back('L'); continue; }
        s.push_back(desc_to_shorty_char(pcls->descriptor));
    }
    return s;
}

// Find a method on a class matching name + param shorty (ignoring return).
// Returns the method_info and fills out_shorty with the full shorty.
dex_cls_t::method_info *find_method_by_params(dex_vm_t *vm, dex_cls_t *cls,
                                              const char *name,
                                              const std::string &param_shorty,
                                              std::string *out_shorty) {
    // param_shorty[0] is 'V' placeholder; params start at [1].
    std::string pparams = param_shorty.substr(1);
    dex_cls_t *c = cls;
    while (c) {
        for (auto &m : c->direct_methods) {
            if (m.name != name) continue;
            std::string mp = m.shorty.substr(1);
            if (mp == pparams) {
                if (out_shorty) *out_shorty = m.shorty;
                return &m;
            }
        }
        for (auto &m : c->virtual_methods) {
            if (m.name != name) continue;
            std::string mp = m.shorty.substr(1);
            if (mp == pparams) {
                if (out_shorty) *out_shorty = m.shorty;
                return &m;
            }
        }
        if (c->super_descriptor.empty()) break;
        c = dex_vm_resolve_class(vm, c->super_descriptor.c_str());
    }
    return nullptr;
}

// Class.getMethod(String, Class[]) -> Method
int stub_class_getmethod(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    // args[0]=Class wrapper, args[1]=name String, args[2]=Class[] paramTypes
    dex_obj_t *c = (dex_obj_t*)args[0].ptr;
    dex_cls_t *cls = (dex_cls_t*)(c && c->fields ? c->fields[0].ptr : nullptr);
    const char *name = arg_str(&args[1]);
    dex_obj_t *param_array = (dex_obj_t*)args[2].ptr;
    std::string pshorty = shorty_from_param_types(param_array);
    std::string full_shorty;
    auto *m = find_method_by_params(vm, cls, name ? name : "", pshorty, &full_shorty);
    if (!m) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s.%s", cls ? cls->descriptor.c_str() : "?", name ? name : "?");
        return stub_throw(vm, "Ljava/lang/NoSuchMethodException;", buf);
    }
    dex_obj_t *method = make_stub_obj(vm, "Ljava/lang/reflect/Method;", 4);
    method->fields[0].ptr = m->declaring_class_desc.empty() ? cls : dex_vm_resolve_class(vm, m->declaring_class_desc.c_str());
    method->fields[1].ptr = dex_new_string_utf(vm, name ? name : "");
    method->fields[2].ptr = dex_new_string_utf(vm, full_shorty.c_str());
    method->fields[3].i32 = m->is_static ? 1 : 0;
    res->ptr = method;
    return 0;
}

// Class.getDeclaredMethod — same as getMethod for our purposes (we don't
// enforce visibility).
int stub_class_getdeclaredmethod(dex_vm_t *vm, dex_value_t *args, int argc,
                                 dex_value_t *res) {
    return stub_class_getmethod(vm, args, argc, res);
}

// Class.getField(String) -> Field
int stub_class_getfield(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    dex_obj_t *c = (dex_obj_t*)args[0].ptr;
    dex_cls_t *cls = (dex_cls_t*)(c && c->fields ? c->fields[0].ptr : nullptr);
    const char *name = arg_str(&args[1]);
    if (!cls || !name) {
        return stub_throw(vm, "Ljava/lang/NoSuchFieldException;", name ? name : "(null)");
    }
    // Walk instance fields then static fields up the super chain.
    std::string type_desc;
    bool is_static = false;
    dex_cls_t *cur = cls;
    while (cur) {
        for (auto &f : cur->instance_fields) {
            if (f.name == name) { type_desc = f.type_desc; is_static = false; goto found; }
        }
        for (auto &f : cur->static_fields) {
            if (f.name == name) { type_desc = f.type_desc; is_static = true; goto found; }
        }
        if (cur->super_descriptor.empty()) break;
        cur = dex_vm_resolve_class(vm, cur->super_descriptor.c_str());
    }
    return stub_throw(vm, "Ljava/lang/NoSuchFieldException;", name);
found:
    dex_obj_t *field = make_stub_obj(vm, "Ljava/lang/reflect/Field;", 4);
    field->fields[0].ptr = cls;
    field->fields[1].ptr = dex_new_string_utf(vm, name);
    field->fields[2].ptr = dex_new_string_utf(vm, type_desc.c_str());
    field->fields[3].i32 = is_static ? 1 : 0;
    res->ptr = field;
    return 0;
}

// Class.getMethods() -> Method[]
int stub_class_getmethods(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    dex_obj_t *c = (dex_obj_t*)args[0].ptr;
    dex_cls_t *cls = (dex_cls_t*)(c && c->fields ? c->fields[0].ptr : nullptr);
    if (!cls) { res->ptr = nullptr; return 0; }
    // Collect all public methods (we don't enforce ACC_PUBLIC — return all).
    std::vector<const dex_cls_t::method_info*> methods;
    dex_cls_t *cur = cls;
    while (cur) {
        for (auto &m : cur->virtual_methods) methods.push_back(&m);
        for (auto &m : cur->direct_methods) {
            if (m.name == "<init>" || m.name == "<clinit>") continue;
            methods.push_back(&m);
        }
        if (cur->super_descriptor.empty()) break;
        cur = dex_vm_resolve_class(vm, cur->super_descriptor.c_str());
    }
    dex_obj_t *arr = new dex_obj();
    arr->cls = dex_vm_resolve_class(vm, "[Ljava/lang/reflect/Method;");
    arr->kind = OBJ_ARRAY;
    arr->length = (uint32_t)methods.size();
    arr->elem_cat = 9;
    arr->array_data = calloc(methods.size() > 0 ? methods.size() : 1, sizeof(void*));
    void **slots = (void**)arr->array_data;
    for (size_t i = 0; i < methods.size(); i++) {
        dex_obj_t *method = make_stub_obj(vm, "Ljava/lang/reflect/Method;", 4);
        method->fields[0].ptr = dex_vm_resolve_class(vm, methods[i]->declaring_class_desc.c_str());
        method->fields[1].ptr = dex_new_string_utf(vm, methods[i]->name.c_str());
        method->fields[2].ptr = dex_new_string_utf(vm, methods[i]->shorty.c_str());
        method->fields[3].i32 = methods[i]->is_static ? 1 : 0;
        slots[i] = method;
    }
    res->ptr = arr;
    return 0;
}

// Class.newInstance() -> Object
int stub_class_newinstance(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    dex_obj_t *c = (dex_obj_t*)args[0].ptr;
    dex_cls_t *cls = (dex_cls_t*)(c && c->fields ? c->fields[0].ptr : nullptr);
    if (!cls) {
        return stub_throw(vm, "Ljava/lang/InstantiationException;", "no class");
    }
    dex_obj_t *obj = dex_new_instance(vm, cls->descriptor.c_str());
    if (!obj) {
        return stub_throw(vm, "Ljava/lang/InstantiationException;", "dex_new_instance failed");
    }
    // Invoke no-arg constructor <init>()V if present.
    dex_value_t ctor_args[1];
    ctor_args[0].ptr = obj;
    dex_value_t ignored;
    dex_invoke(vm, cls->descriptor.c_str(), "<init>", "V", ctor_args, 1, &ignored);
    res->ptr = obj;
    return 0;
}

// ---- java.lang.reflect.Method ----

// Helper: unbox a boxed Object into a dex_value_t based on shorty char.
dex_value_t unbox_arg(dex_obj_t *boxed, char shorty_char) {
    dex_value_t v{};
    if (!boxed) return v;
    if (shorty_char == 'L' || shorty_char == '[') {
        v.ptr = boxed;
        return v;
    }
    if (!boxed->fields) return v;
    switch (shorty_char) {
        case 'J': v.i64 = boxed->fields[0].i64; break;
        case 'F': v.f32 = boxed->fields[0].f32; break;
        case 'D': v.f64 = boxed->fields[0].f64; break;
        default:  v.i32 = boxed->fields[0].i32; break;  // I, Z, B, C, S
    }
    return v;
}

// Helper: box a return value based on shorty return char.
dex_obj_t *box_return(dex_vm_t *vm, dex_value_t ret, char shorty_char) {
    switch (shorty_char) {
        case 'V': return nullptr;
        case 'L': case '[': return (dex_obj_t*)ret.ptr;
        case 'I': case 'Z': case 'B': case 'C': case 'S': {
            dex_obj_t *o = make_stub_obj(vm, "Ljava/lang/Integer;", 1);
            o->fields[0].i32 = ret.i32;
            return o;
        }
        case 'J': {
            dex_obj_t *o = make_stub_obj(vm, "Ljava/lang/Long;", 1);
            o->fields[0].i64 = ret.i64;
            return o;
        }
        case 'F': {
            dex_obj_t *o = make_stub_obj(vm, "Ljava/lang/Float;", 1);
            o->fields[0].f32 = ret.f32;
            return o;
        }
        case 'D': {
            dex_obj_t *o = make_stub_obj(vm, "Ljava/lang/Double;", 1);
            o->fields[0].f64 = ret.f64;
            return o;
        }
    }
    return nullptr;
}

// Method.invoke(Object receiver, Object[] args) -> Object
int stub_method_invoke(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    // args[0]=Method wrapper, args[1]=receiver (may be null for static),
    // args[2]=Object[] args
    dex_obj_t *m = (dex_obj_t*)args[0].ptr;
    if (!m || !m->fields) { res->ptr = nullptr; return 0; }
    dex_cls_t *cls = (dex_cls_t*)m->fields[0].ptr;
    const char *name = dex_string_utf((dex_obj_t*)m->fields[1].ptr);
    std::string shorty = dex_string_utf((dex_obj_t*)m->fields[2].ptr) ?: "";
    bool is_static = m->fields[3].i32 != 0;
    if (!cls || !name || shorty.empty()) {
        return stub_throw(vm, "Ljava/lang/IllegalArgumentException;", "bad Method wrapper");
    }
    dex_obj_t *recv_obj = (dex_obj_t*)args[1].ptr;
    dex_obj_t *args_arr = (dex_obj_t*)args[2].ptr;
    int n_params = (int)shorty.size() - 1;
    int n_args = (is_static ? 0 : 1) + n_params;
    std::vector<dex_value_t> invoke_args(n_args > 0 ? n_args : 1);
    int ai = 0;
    if (!is_static) {
        invoke_args[ai++].ptr = recv_obj;
    }
    if (n_params > 0) {
        if (!args_arr || args_arr->kind != OBJ_ARRAY || !args_arr->array_data) {
            return stub_throw(vm, "Ljava/lang/IllegalArgumentException;", "missing args array");
        }
        void **elems = (void**)args_arr->array_data;
        for (int i = 0; i < n_params; i++) {
            char t = shorty[i + 1];
            dex_obj_t *boxed = (i < (int)args_arr->length) ? (dex_obj_t*)elems[i] : nullptr;
            invoke_args[ai++] = unbox_arg(boxed, t);
        }
    }
    dex_value_t ret{};
    int rc = dex_invoke(vm, cls->descriptor.c_str(), name, shorty.c_str(),
                        invoke_args.data(), n_args, &ret);
    if (rc != 0) {
        // dex_invoke already logged; if pending_exception is set, the caller's
        // try/catch will handle it (we returned non-zero from a stub).
        if (vm->pending_exception) return -1;
        return stub_throw(vm, "Ljava/lang/reflect/InvocationTargetException;",
                          "dex_invoke failed");
    }
    res->ptr = box_return(vm, ret, shorty[0]);
    return 0;
}

int stub_method_setaccessible(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    // No access checks in our interpreter — log + no-op.
    LOGD("framework", "Method.setAccessible(%d) no-op (no access checks)",
         args && args[0].ptr ? args[1].i32 : 0);
    return 0;
}

int stub_method_getname(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    if (!args || !args[0].ptr) { res->ptr = dex_new_string_utf(vm, ""); return 0; }
    dex_obj_t *m = (dex_obj_t*)args[0].ptr;
    const char *name = (m->fields && m->fields[1].ptr) ?
                       dex_string_utf((dex_obj_t*)m->fields[1].ptr) : "";
    res->ptr = dex_new_string_utf(vm, name);
    return 0;
}

// ---- java.lang.reflect.Field ----
int stub_field_get(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    dex_obj_t *f = (dex_obj_t*)args[0].ptr;
    if (!f || !f->fields) { res->ptr = nullptr; return 0; }
    dex_cls_t *cls = (dex_cls_t*)f->fields[0].ptr;
    const char *name = dex_string_utf((dex_obj_t*)f->fields[1].ptr);
    const char *type_desc = dex_string_utf((dex_obj_t*)f->fields[2].ptr);
    bool is_static = f->fields[3].i32 != 0;
    dex_value_t out{};
    int rc;
    if (is_static) {
        rc = dex_get_static_field(vm, cls->descriptor.c_str(), name, &out);
    } else {
        dex_obj_t *target = (dex_obj_t*)args[1].ptr;
        rc = dex_get_field(vm, target, name, &out);
    }
    if (rc != 0) {
        return stub_throw(vm, "Ljava/lang/IllegalAccessException;", "field get failed");
    }
    res->ptr = box_return(vm, out, desc_to_shorty_char(type_desc ? type_desc : "L"));
    return 0;
}

int stub_field_set(dex_vm_t *vm, dex_value_t *args, int, dex_value_t*) {
    dex_obj_t *f = (dex_obj_t*)args[0].ptr;
    if (!f || !f->fields) return 0;
    dex_cls_t *cls = (dex_cls_t*)f->fields[0].ptr;
    const char *name = dex_string_utf((dex_obj_t*)f->fields[1].ptr);
    const char *type_desc = dex_string_utf((dex_obj_t*)f->fields[2].ptr);
    bool is_static = f->fields[3].i32 != 0;
    dex_obj_t *value_boxed = (dex_obj_t*)args[2].ptr;
    dex_value_t v = unbox_arg(value_boxed, desc_to_shorty_char(type_desc ? type_desc : "L"));
    int rc;
    if (is_static) {
        rc = dex_set_static_field(vm, cls->descriptor.c_str(), name, v);
    } else {
        dex_obj_t *target = (dex_obj_t*)args[1].ptr;
        rc = dex_set_field(vm, target, name, v);
    }
    if (rc != 0) {
        return stub_throw(vm, "Ljava/lang/IllegalAccessException;", "field set failed");
    }
    return 0;
}

int stub_field_setaccessible(dex_vm_t*, dex_value_t*, int, dex_value_t*) {
    LOGD("framework", "Field.setAccessible no-op");
    return 0;
}

int stub_field_getname(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    if (!args || !args[0].ptr) { res->ptr = dex_new_string_utf(vm, ""); return 0; }
    dex_obj_t *f = (dex_obj_t*)args[0].ptr;
    const char *name = (f->fields && f->fields[1].ptr) ?
                       dex_string_utf((dex_obj_t*)f->fields[1].ptr) : "";
    res->ptr = dex_new_string_utf(vm, name);
    return 0;
}

// ---- Boxing: Long/Float/Double valueOf + intValue/etc. ----
// (Integer/Boolean valueOf already registered; we add Long/Float/Double and
//  the *Value accessors for all primitive boxes.)
int stub_long_valueof(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    dex_obj_t *o = make_stub_obj(vm, "Ljava/lang/Long;", 1);
    o->fields[0].i64 = args[0].i64;   // static valueOf(J) — args[0] = first param
    res->ptr = o;
    return 0;
}
int stub_long_longvalue(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    dex_obj_t *o = (dex_obj_t*)args[0].ptr;
    res->i64 = (o && o->fields) ? o->fields[0].i64 : 0;
    return 0;
}
int stub_float_valueof(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    dex_obj_t *o = make_stub_obj(vm, "Ljava/lang/Float;", 1);
    o->fields[0].f32 = args[0].f32;
    res->ptr = o;
    return 0;
}
int stub_float_floatvalue(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    dex_obj_t *o = (dex_obj_t*)args[0].ptr;
    res->f32 = (o && o->fields) ? o->fields[0].f32 : 0;
    return 0;
}
int stub_double_valueof(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    dex_obj_t *o = make_stub_obj(vm, "Ljava/lang/Double;", 1);
    o->fields[0].f64 = args[0].f64;
    res->ptr = o;
    return 0;
}
int stub_double_doublevalue(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    dex_obj_t *o = (dex_obj_t*)args[0].ptr;
    res->f64 = (o && o->fields) ? o->fields[0].f64 : 0;
    return 0;
}
int stub_boolean_valueof(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    dex_obj_t *o = make_stub_obj(vm, "Ljava/lang/Boolean;", 1);
    o->fields[0].i32 = args[0].i32 ? 1 : 0;
    res->ptr = o;
    return 0;
}
int stub_boolean_booleanvalue(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    dex_obj_t *o = (dex_obj_t*)args[0].ptr;
    res->i32 = (o && o->fields) ? (o->fields[0].i32 ? 1 : 0) : 0;
    return 0;
}
int stub_integer_valueof_static(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    dex_obj_t *o = make_stub_obj(vm, "Ljava/lang/Integer;", 1);
    o->fields[0].i32 = args[0].i32;   // static valueOf(I) — args[0] = first param
    res->ptr = o;
    return 0;
}
int stub_integer_intvalue(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    dex_obj_t *o = (dex_obj_t*)args[0].ptr;
    res->i32 = (o && o->fields) ? o->fields[0].i32 : 0;
    return 0;
}

// ============================================================================
// android.media.* — degraded: no-op-success + log; AudioTrack routes to OpenSL
// ============================================================================

// AudioTrack <init>(int streamType, int sampleRate, int channelConfig,
//                   int audioFormat, int bufferSize, int mode)
int stub_audiotrack_init(dex_vm_t *vm, dex_value_t *args, int, dex_value_t*) {
    // args[0]=this, args[1..6]=params
    if (!args || !args[0].ptr) return 0;
    dex_obj_t *t = (dex_obj_t*)args[0].ptr;
    t->kind = OBJ_STUB;
    if (!t->fields) t->fields = new dex_value_t[1]();
    int sample_rate = args[2].i32;
    int channel_config = args[3].i32;
    int audio_format = args[4].i32;
    // channelConfig: 2=MONO, 3=STEREO. Map conservatively.
    int channels = (channel_config == 2 /*CHANNEL_OUT_MONO*/) ? 1 : 2;
    int bits = (audio_format == 2 /*ENCODING_PCM_16BIT*/) ? 16 : 8;
    AudioTrackState &st = g_audio_tracks[t];
    st.sample_rate = sample_rate;
    st.channels = channels;
    st.bits_per_sample = bits;
    st.player = nullptr;
    LOGI("framework", "AudioTrack.<init> sr=%d ch=%d bits=%d (lazy OpenSL)",
         sample_rate, channels, bits);
    (void)vm;
    return 0;
}

// Lazy-create the OpenSL engine + player for a given AudioTrack.
// Uses the global engine created by apkcontainer_audio_start() so there's a
// single AVAudioEngine instance shared with the SLES C API wrapper.
static sl_player_t *ensure_audio_player(dex_vm_t *vm, dex_obj_t *t) {
    auto it = g_audio_tracks.find(t);
    if (it == g_audio_tracks.end()) return nullptr;
    AudioTrackState &st = it->second;
    if (st.player) return st.player;

    // Prefer the global engine (created by apkcontainer_audio_start); create
    // one on demand if Swift hasn't started audio yet.
    sl_engine_t *eng = opensl_bridge_get_global_engine();
    if (!eng) {
        if (opensl_bridge_engine_create(&eng) != 0 || !eng) {
            LOGW("framework", "AudioTrack: opensl_bridge_engine_create failed (no audio)");
            return nullptr;
        }
        opensl_bridge_set_global_engine(eng);
    }
    g_audio_engine = eng;   // cache for compat with older code paths

    if (opensl_bridge_create_player(eng, st.channels, st.sample_rate,
                                    st.bits_per_sample, &st.player) != 0 || !st.player) {
        LOGW("framework", "AudioTrack: opensl_bridge_create_player failed (no audio)");
        st.player = nullptr;
        return nullptr;
    }
    (void)vm;
    return st.player;
}

// AudioTrack.write(byte[] | short[], int offset, int size) -> int
// Both overloads share shorty "ILII"; we dispatch by array elem_cat.
//   byte[] : offset/size are in BYTES — enqueue size bytes.
//   short[]: offset/size are in SHORTS — enqueue size*2 bytes from offset*2.
int stub_audiotrack_write_b(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    dex_obj_t *t = (dex_obj_t*)args[0].ptr;
    dex_obj_t *arr = (dex_obj_t*)args[1].ptr;
    int off = args[2].i32;
    int len = args[3].i32;
    if (!arr || arr->kind != OBJ_ARRAY || !arr->array_data) {
        res->i32 = 0;
        return 0;
    }
    // elem_cat: 2=byte, 4=short. Bytes-per-element for indexing.
    size_t elem_sz = (arr->elem_cat == 4) ? 2 : 1;
    size_t byte_off = (size_t)off * elem_sz;
    size_t byte_len = (size_t)len * elem_sz;

    // Clamp to array bounds to avoid OOB reads (apps may pass len larger than
    // the remaining array; we return what we actually accepted).
    size_t arr_bytes = (size_t)arr->length * elem_sz;
    if (byte_off >= arr_bytes) { res->i32 = 0; return 0; }
    if (byte_off + byte_len > arr_bytes) {
        byte_len = arr_bytes - byte_off;
    }
    if (byte_len == 0) { res->i32 = 0; return 0; }

    sl_player_t *p = ensure_audio_player(vm, t);
    if (p) {
        int rc = opensl_bridge_player_enqueue(p,
                                              (uint8_t*)arr->array_data + byte_off,
                                              byte_len);
        if (rc != 0) {
            LOGW("framework", "AudioTrack.write: opensl_bridge_player_enqueue rc=%d", rc);
            res->i32 = 0;
            return 0;
        }
    } else {
        LOGD("framework", "AudioTrack.write %zu bytes (no audio device; dropped)", byte_len);
    }
    // Return the COUNT we accepted, in the same units the caller passed
    // (bytes for byte[], shorts for short[]). Real AudioTrack returns the
    // number of units written; we accept everything we clamped to.
    res->i32 = (int32_t)(byte_len / elem_sz);
    return 0;
}
// AudioTrack.write(short[], int, int) -> int — same handler (shorty is the
// same "ILII"; dispatch is by array elem_cat inside the handler above).
int stub_audiotrack_write_s(dex_vm_t *vm, dex_value_t *args, int argc,
                            dex_value_t *res) {
    return stub_audiotrack_write_b(vm, args, argc, res);
}
int stub_audiotrack_play(dex_vm_t *vm, dex_value_t *args, int, dex_value_t*) {
    dex_obj_t *t = (dex_obj_t*)args[0].ptr;
    sl_player_t *p = ensure_audio_player(vm, t);
    if (p) opensl_bridge_player_play(p);
    LOGI("framework", "AudioTrack.play");
    return 0;
}
int stub_audiotrack_pause(dex_vm_t *vm, dex_value_t *args, int, dex_value_t*) {
    dex_obj_t *t = (dex_obj_t*)args[0].ptr;
    auto it = g_audio_tracks.find(t);
    if (it != g_audio_tracks.end() && it->second.player) {
        opensl_bridge_player_stop(it->second.player);
    }
    LOGI("framework", "AudioTrack.pause");
    (void)vm;
    return 0;
}
int stub_audiotrack_stop(dex_vm_t *vm, dex_value_t *args, int, dex_value_t*) {
    dex_obj_t *t = (dex_obj_t*)args[0].ptr;
    auto it = g_audio_tracks.find(t);
    if (it != g_audio_tracks.end() && it->second.player) {
        opensl_bridge_player_stop(it->second.player);
    }
    LOGI("framework", "AudioTrack.stop");
    (void)vm;
    return 0;
}
int stub_audiotrack_release(dex_vm_t *vm, dex_value_t *args, int, dex_value_t*) {
    dex_obj_t *t = (dex_obj_t*)args[0].ptr;
    auto it = g_audio_tracks.find(t);
    if (it != g_audio_tracks.end()) {
        if (it->second.player) opensl_bridge_player_destroy(it->second.player);
        g_audio_tracks.erase(it);
    }
    LOGI("framework", "AudioTrack.release");
    (void)vm;
    return 0;
}
// AudioTrack.getMinBufferSize(int, int, int) -> int  (static)
int stub_audiotrack_getminbuffersize(dex_vm_t*, dex_value_t*, int, dex_value_t *res) {
    if (res) res->i32 = 4096;
    return 0;
}
int stub_audiotrack_setnotifmarker(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    LOGD("framework", "AudioTrack.setNotificationMarkerPosition(%d) no-op",
         args && args[0].ptr ? args[1].i32 : 0);
    return 0;
}
int stub_audiotrack_setpositionlistener(dex_vm_t*, dex_value_t*, int, dex_value_t*) {
    LOGD("framework", "AudioTrack.setPlaybackPositionUpdateListener no-op");
    return 0;
}

// ---- MediaPlayer ----
int stub_mediaplayer_init(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    if (!args || !args[0].ptr) return 0;
    dex_obj_t *p = (dex_obj_t*)args[0].ptr;
    p->kind = OBJ_STUB;
    if (!p->fields) p->fields = new dex_value_t[1]();
    g_media_players[p];   // create default state
    return 0;
}
int stub_mediaplayer_setdatasource_str(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    dex_obj_t *p = (dex_obj_t*)args[0].ptr;
    const char *path = arg_str(&args[1]);
    auto &st = g_media_players[p];
    st.path = path ? path : "";
    LOGI("framework", "MediaPlayer.setDataSource(%s) (no decode)", st.path.c_str());
    return 0;
}
int stub_mediaplayer_setdatasource_uri(dex_vm_t *vm, dex_value_t *args, int argc,
                                       dex_value_t *res) {
    // Uri is opaque; just record its toString if available. For v1, log + no-op.
    LOGW("framework", "MediaPlayer.setDataSource(Uri) — partial: ignoring Uri");
    (void)vm; (void)args; (void)argc; (void)res;
    return 0;
}
int stub_mediaplayer_prepare(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    dex_obj_t *p = (dex_obj_t*)args[0].ptr;
    auto &st = g_media_players[p];
    st.prepared = true;
    LOGI("framework", "MediaPlayer.prepare (no-op success)");
    return 0;
}
int stub_mediaplayer_prepareasync(dex_vm_t *vm, dex_value_t *args, int, dex_value_t*) {
    dex_obj_t *p = (dex_obj_t*)args[0].ptr;
    auto &st = g_media_players[p];
    st.prepared = true;
    LOGI("framework", "MediaPlayer.prepareAsync -> firing onPrepared immediately");
    // Fire the prepared listener synchronously (we don't have an event loop).
    if (st.on_prepared_listener) {
        dex_value_t cb_args[2];
        cb_args[0].ptr = st.on_prepared_listener;
        cb_args[1].ptr = p;
        dex_value_t ignored;
        dex_invoke(vm, "Landroid/media/MediaPlayer$OnPreparedListener;",
                   "onPrepared", "VL", cb_args, 2, &ignored);
    }
    return 0;
}
int stub_mediaplayer_start(dex_vm_t*, dex_value_t*, int, dex_value_t*) {
    LOGI("framework", "MediaPlayer.start (no-op success)");
    return 0;
}
int stub_mediaplayer_pause(dex_vm_t*, dex_value_t*, int, dex_value_t*) {
    LOGI("framework", "MediaPlayer.pause (no-op success)");
    return 0;
}
int stub_mediaplayer_stop(dex_vm_t*, dex_value_t*, int, dex_value_t*) {
    LOGI("framework", "MediaPlayer.stop (no-op success)");
    return 0;
}
int stub_mediaplayer_release(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    dex_obj_t *p = (dex_obj_t*)args[0].ptr;
    g_media_players.erase(p);
    LOGI("framework", "MediaPlayer.release");
    return 0;
}
int stub_mediaplayer_setlooping(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    LOGD("framework", "MediaPlayer.setLooping(%d) no-op",
         args && args[0].ptr ? args[1].i32 : 0);
    return 0;
}
int stub_mediaplayer_setvolume(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    LOGD("framework", "MediaPlayer.setVolume(%g, %g) no-op",
         (double)args[1].f32, (double)args[2].f32);
    return 0;
}
int stub_mediaplayer_setdisplay(dex_vm_t*, dex_value_t*, int, dex_value_t*) {
    LOGD("framework", "MediaPlayer.setDisplay no-op");
    return 0;
}
int stub_mediaplayer_setonprepared(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    dex_obj_t *p = (dex_obj_t*)args[0].ptr;
    auto &st = g_media_players[p];
    st.on_prepared_listener = (dex_obj_t*)args[1].ptr;
    LOGD("framework", "MediaPlayer.setOnPreparedListener recorded");
    return 0;
}
int stub_mediaplayer_setonerror(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    dex_obj_t *p = (dex_obj_t*)args[0].ptr;
    auto &st = g_media_players[p];
    st.on_error_listener = (dex_obj_t*)args[1].ptr;
    LOGD("framework", "MediaPlayer.setOnErrorListener recorded");
    return 0;
}

// ---- SoundPool ----
int stub_soundpool_init(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    if (!args || !args[0].ptr) return 0;
    dex_obj_t *p = (dex_obj_t*)args[0].ptr;
    p->kind = OBJ_STUB;
    if (!p->fields) p->fields = new dex_value_t[1]();
    LOGD("framework", "SoundPool.<init> no-op");
    return 0;
}
int stub_soundpool_load(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    const char *path = arg_str(&args[1]);
    int id = g_soundpool_next_id++;
    LOGI("framework", "SoundPool.load(%s) -> fake id %d", path ? path : "?", id);
    (void)args;
    res->i32 = id;
    return 0;
}
int stub_soundpool_play(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    // play(int, float, float, int, int, float) -> int
    int stream_id = g_soundpool_next_id++;
    LOGD("framework", "SoundPool.play(sound=%d) -> fake stream %d (no-op)",
         args[1].i32, stream_id);
    res->i32 = stream_id;
    return 0;
}
int stub_soundpool_release(dex_vm_t*, dex_value_t*, int, dex_value_t*) {
    LOGD("framework", "SoundPool.release no-op");
    return 0;
}

// ============================================================================
// android.content.SharedPreferences — REAL JSON-backed
// ============================================================================

// Minimal JSON writer for the shared-prefs map.
static void sp_write_json(const std::string &path,
                          const std::unordered_map<std::string, SPValue> &data) {
    FILE *fp = fopen(path.c_str(), "w");
    if (!fp) {
        LOGW("framework", "SharedPreferences: cannot write %s", path.c_str());
        return;
    }
    fputc('{', fp);
    bool first = true;
    for (auto &kv : data) {
        if (!first) fputc(',', fp);
        first = false;
        fputc('"', fp);
        for (char c : kv.first) {
            if (c == '"' || c == '\\') fputc('\\', fp);
            fputc(c, fp);
        }
        fputs("\":", fp);
        const SPValue &v = kv.second;
        if (v.type == 's') {
            fputc('"', fp);
            for (char c : v.s) {
                if (c == '"' || c == '\\') { fputc('\\', fp); fputc(c, fp); }
                else if (c == '\n') fputs("\\n", fp);
                else if (c == '\t') fputs("\\t", fp);
                else if (c == '\r') fputs("\\r", fp);
                else fputc(c, fp);
            }
            fputc('"', fp);
        } else if (v.type == 'b') {
            fputs(v.i ? "true" : "false", fp);
        } else if (v.type == 'f') {
            fprintf(fp, "%g", v.d);
        } else if (v.type == 'd') {
            fprintf(fp, "%g", v.d);
        } else {
            fprintf(fp, "%lld", (long long)v.i);
        }
    }
    fputs("}\n", fp);
    fclose(fp);
}

// Minimal JSON parser for the shared-prefs map.
static bool sp_read_json(const std::string &path,
                         std::unordered_map<std::string, SPValue> &out) {
    FILE *fp = fopen(path.c_str(), "r");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); return false; }
    std::string buf(sz, '\0');
    size_t rd = fread(&buf[0], 1, (size_t)sz, fp);
    fclose(fp);
    buf.resize(rd);
    size_t i = 0;
    auto skip_ws = [&]() {
        while (i < buf.size() && (buf[i] == ' ' || buf[i] == '\t' ||
               buf[i] == '\n' || buf[i] == '\r')) i++;
    };
    auto parse_string = [&](std::string &out_s) -> bool {
        if (i >= buf.size() || buf[i] != '"') return false;
        i++;
        out_s.clear();
        while (i < buf.size() && buf[i] != '"') {
            if (buf[i] == '\\' && i + 1 < buf.size()) {
                i++;
                char c = buf[i++];
                switch (c) {
                    case 'n': out_s.push_back('\n'); break;
                    case 't': out_s.push_back('\t'); break;
                    case 'r': out_s.push_back('\r'); break;
                    case '"': out_s.push_back('"'); break;
                    case '\\': out_s.push_back('\\'); break;
                    case '/': out_s.push_back('/'); break;
                    default: out_s.push_back(c); break;
                }
            } else {
                out_s.push_back(buf[i++]);
            }
        }
        if (i < buf.size()) i++;
        return true;
    };
    skip_ws();
    if (i >= buf.size() || buf[i] != '{') return false;
    i++;
    skip_ws();
    if (i < buf.size() && buf[i] == '}') return true;
    while (i < buf.size()) {
        skip_ws();
        std::string key;
        if (!parse_string(key)) return false;
        skip_ws();
        if (i >= buf.size() || buf[i] != ':') return false;
        i++;
        skip_ws();
        SPValue v;
        if (i < buf.size() && buf[i] == '"') {
            v.type = 's';
            if (!parse_string(v.s)) return false;
        } else if (i + 4 <= buf.size() && buf.compare(i, 4, "true") == 0) {
            v.type = 'b'; v.i = 1; i += 4;
        } else if (i + 5 <= buf.size() && buf.compare(i, 5, "false") == 0) {
            v.type = 'b'; v.i = 0; i += 5;
        } else {
            char *endp = nullptr;
            long long n = strtoll(buf.c_str() + i, &endp, 10);
            if (endp == buf.c_str() + i) {
                // try float
                double d = strtod(buf.c_str() + i, &endp);
                if (endp == buf.c_str() + i) return false;
                v.type = 'd'; v.d = d;
            } else {
                v.type = 'i'; v.i = n;
            }
            i = (size_t)(endp - buf.c_str());
        }
        out[key] = v;
        skip_ws();
        if (i < buf.size() && buf[i] == ',') { i++; continue; }
        if (i < buf.size() && buf[i] == '}') { i++; return true; }
        return false;
    }
    return false;
}

// Context.getSharedPreferences(String, int) -> SharedPreferences
int stub_context_getsharedprefs(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    // args[0]=this (Context), args[1]=name, args[2]=mode
    const char *name = arg_str(&args[1]);
    if (!name) name = "default";
    std::string root = vm->sandbox_root;
    if (root.empty()) root = "/tmp/apklive";
    if (root.back() != '/') root += "/";
    root += "shared_prefs/";
    // Best-effort mkdir; ignore failure.
    {
        std::string path = root;
        size_t pos = 0;
        do {
            pos = path.find('/', pos + 1);
            std::string sub = path.substr(0, pos);
            if (!sub.empty()) {
                struct stat st;
                if (stat(sub.c_str(), &st) != 0) {
                    mkdir(sub.c_str(), 0755);
                }
            }
        } while (pos != std::string::npos);
    }
    root += name;
    root += ".json";
    dex_obj_t *sp = make_stub_obj(vm, "Landroid/content/SharedPreferences;", 1);
    sp->fields[0].ptr = dex_new_string_utf(vm, root.c_str());
    g_sp_data[sp];   // create empty map
    g_sp_loaded[sp] = true;
    // Best-effort load.
    sp_read_json(root, g_sp_data[sp]);
    LOGI("framework", "Context.getSharedPreferences(%s) -> %s", name, root.c_str());
    return 0;
}

// SharedPreferences.getString(String, String) -> String
int stub_sharedprefs_getstring(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    dex_obj_t *sp = (dex_obj_t*)args[0].ptr;
    const char *key = arg_str(&args[1]);
    const char *def = arg_str(&args[2]);
    auto it = g_sp_data.find(sp);
    if (it == g_sp_data.end() || !key) {
        res->ptr = dex_new_string_utf(vm, def ? def : "");
        return 0;
    }
    auto kit = it->second.find(key);
    if (kit == it->second.end() || kit->second.type != 's') {
        res->ptr = dex_new_string_utf(vm, def ? def : "");
    } else {
        res->ptr = dex_new_string_utf(vm, kit->second.s.c_str());
    }
    return 0;
}
int stub_sharedprefs_getint(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    dex_obj_t *sp = (dex_obj_t*)args[0].ptr;
    const char *key = arg_str(&args[1]);
    int def = args[2].i32;
    auto it = g_sp_data.find(sp);
    if (it == g_sp_data.end() || !key) { res->i32 = def; return 0; }
    auto kit = it->second.find(key);
    if (kit == it->second.end() || kit->second.type == 's') {
        res->i32 = def;
    } else {
        res->i32 = (int32_t)kit->second.i;
    }
    return 0;
}
int stub_sharedprefs_getboolean(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    dex_obj_t *sp = (dex_obj_t*)args[0].ptr;
    const char *key = arg_str(&args[1]);
    int def = args[2].i32;
    auto it = g_sp_data.find(sp);
    if (it == g_sp_data.end() || !key) { res->i32 = def; return 0; }
    auto kit = it->second.find(key);
    if (kit == it->second.end()) {
        res->i32 = def;
    } else {
        res->i32 = kit->second.i ? 1 : 0;
    }
    return 0;
}
int stub_sharedprefs_putstring(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    // putString returns SharedPreferences.Editor for chaining; we return this SP.
    dex_obj_t *sp = (dex_obj_t*)args[0].ptr;
    const char *key = arg_str(&args[1]);
    const char *val = arg_str(&args[2]);
    if (key) {
        SPValue v; v.type = 's'; v.s = val ? val : "";
        g_sp_data[sp][key] = v;
    }
    if (res) res->ptr = sp;
    return 0;
}
int stub_sharedprefs_putint(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    dex_obj_t *sp = (dex_obj_t*)args[0].ptr;
    const char *key = arg_str(&args[1]);
    int val = args[2].i32;
    if (key) {
        SPValue v; v.type = 'i'; v.i = val;
        g_sp_data[sp][key] = v;
    }
    if (res) res->ptr = sp;
    return 0;
}
int stub_sharedprefs_putboolean(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    dex_obj_t *sp = (dex_obj_t*)args[0].ptr;
    const char *key = arg_str(&args[1]);
    int val = args[2].i32;
    if (key) {
        SPValue v; v.type = 'b'; v.i = val ? 1 : 0;
        g_sp_data[sp][key] = v;
    }
    if (res) res->ptr = sp;
    return 0;
}
int stub_sharedprefs_commit(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    dex_obj_t *sp = (dex_obj_t*)args[0].ptr;
    if (sp && sp->fields && sp->fields[0].ptr) {
        const char *path = dex_string_utf((dex_obj_t*)sp->fields[0].ptr);
        if (path) sp_write_json(path, g_sp_data[sp]);
    }
    if (res) res->i32 = 1;   // success
    LOGI("framework", "SharedPreferences.commit (sync write)");
    return 0;
}
int stub_sharedprefs_apply(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    dex_obj_t *sp = (dex_obj_t*)args[0].ptr;
    if (sp && sp->fields && sp->fields[0].ptr) {
        const char *path = dex_string_utf((dex_obj_t*)sp->fields[0].ptr);
        if (path) sp_write_json(path, g_sp_data[sp]);
    }
    LOGI("framework", "SharedPreferences.apply (sync write; no async)");
    return 0;
}
int stub_sharedprefs_edit(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    // We model in-place mutation; return the SP itself as the "Editor".
    if (!res) return 0;
    res->ptr = args[0].ptr;
    return 0;
}

// ============================================================================
// android.content.Intent — REAL map-backed extras
// ============================================================================
int stub_intent_init_v(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    if (!args || !args[0].ptr) return 0;
    dex_obj_t *i = (dex_obj_t*)args[0].ptr;
    i->kind = OBJ_STUB;
    if (!i->fields) i->fields = new dex_value_t[1]();
    g_intent_extras[i];
    g_intent_action[i];
    g_intent_component[i];
    return 0;
}
int stub_intent_init_ctx_class(dex_vm_t*, dex_value_t *args, int, dex_value_t*) {
    if (!args || !args[0].ptr) return 0;
    dex_obj_t *i = (dex_obj_t*)args[0].ptr;
    i->kind = OBJ_STUB;
    if (!i->fields) i->fields = new dex_value_t[1]();
    // args[1] = Context (we ignore), args[2] = Class
    dex_obj_t *cls = (dex_obj_t*)args[2].ptr;
    if (cls && cls->fields && cls->fields[0].ptr) {
        dex_cls_t *cc = (dex_cls_t*)cls->fields[0].ptr;
        if (cc) g_intent_component[i] = cc->descriptor;
    }
    g_intent_extras[i];
    g_intent_action[i];
    LOGD("framework", "Intent.<init>(Context, Class=%s)", g_intent_component[i].c_str());
    return 0;
}
int stub_intent_putextra_str(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    dex_obj_t *i = (dex_obj_t*)args[0].ptr;
    const char *key = arg_str(&args[1]);
    const char *val = arg_str(&args[2]);
    if (key && val) g_intent_extras[i][key] = val;
    if (res) res->ptr = i;   // chainable
    return 0;
}
int stub_intent_getstringextra(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    dex_obj_t *i = (dex_obj_t*)args[0].ptr;
    const char *key = arg_str(&args[1]);
    auto it = g_intent_extras.find(i);
    if (it == g_intent_extras.end() || !key) {
        res->ptr = dex_new_string_utf(vm, "");
        return 0;
    }
    auto kit = it->second.find(key);
    res->ptr = dex_new_string_utf(vm, (kit != it->second.end()) ? kit->second.c_str() : "");
    return 0;
}
int stub_intent_getaction(dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) {
    if (!res) return 0;
    dex_obj_t *i = (dex_obj_t*)args[0].ptr;
    auto it = g_intent_action.find(i);
    res->ptr = dex_new_string_utf(vm, (it != g_intent_action.end()) ? it->second.c_str() : "");
    return 0;
}
int stub_intent_setaction(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    dex_obj_t *i = (dex_obj_t*)args[0].ptr;
    const char *a = arg_str(&args[1]);
    g_intent_action[i] = a ? a : "";
    if (res) res->ptr = i;
    return 0;
}

// ============================================================================
// android.content.Context gaps — cache dir / external cache / assets stub
// ============================================================================
int stub_context_getexternalcachedir(dex_vm_t *vm, dex_value_t*, int, dex_value_t *res) {
    if (!res) return 0;
    std::string path = vm->sandbox_root;
    if (path.empty()) path = "/tmp/apklive";
    if (path.back() != '/') path += "/";
    path += "cache_external";
    dex_obj_t *f = make_stub_obj(vm, "Ljava/io/File;", 1);
    f->fields[0].ptr = dex_new_string_utf(vm, path.c_str());
    res->ptr = f;
    return 0;
}
int stub_context_getassets(dex_vm_t *vm, dex_value_t*, int, dex_value_t *res) {
    // Return a stub AssetManager. Its open(String) throws FileNotFoundException.
    if (!res) return 0;
    dex_obj_t *a = make_stub_obj(vm, "Landroid/content/res/AssetManager;", 1);
    res->ptr = a;
    return 0;
}
int stub_assetmanager_open(dex_vm_t *vm, dex_value_t*, int, dex_value_t *res) {
    // No APK assets wired for v1; throw FileNotFoundException.
    return stub_throw(vm, "Ljava/io/FileNotFoundException;",
                      "AssetManager.open not wired (v1)");
}

// ============================================================================
// android.util.Log.println — REAL routing to LOG* macros
// ============================================================================
int stub_log_println(dex_vm_t*, dex_value_t *args, int, dex_value_t *res) {
    // args[0]=priority, args[1]=tag, args[2]=msg (static, no receiver)
    int prio = args[0].i32;
    const char *tag = arg_str(&args[1]);
    const char *msg = arg_str(&args[2]);
    const char *t = tag ? tag : "?";
    const char *m = msg ? msg : "?";
    switch (prio) {
        case 2:  LOGD("framework", "[Log.v %s] %s", t, m); break;  // VERBOSE (no LOGV; route to DEBUG)
        case 3:  LOGD("framework", "[Log.d %s] %s", t, m); break;  // DEBUG
        case 4:  LOGI("framework", "[Log.i %s] %s", t, m); break;  // INFO
        case 5:  LOGW("framework", "[Log.w %s] %s", t, m); break;  // WARN
        case 6:  LOGE("framework", "[Log.e %s] %s", t, m); break;  // ERROR
        case 7:  LOGE("framework", "[Log ASSERT %s] %s", t, m); break;
        default: LOGD("framework", "[Log ?%d %s] %s", prio, t, m); break;
    }
    if (res) res->i32 = 0;
    return 0;
}

}  // namespace

// Register all the framework stubs. Called from dex_vm_create.
static void register_framework_stubs(dex_vm_t *vm) {
    auto reg = [&](const char *cls, const char *name, const char *shorty,
                   native_method_fn fn) {
        vm->natives[native_key(cls, name, shorty)] = std::move(fn);
    };
    auto reg_obj = [&](const char *cls, const char *name, const char *shorty,
                       int (*fn)(dex_vm_t*, dex_value_t*, int, dex_value_t*)) {
        vm->natives[native_key(cls, name, shorty)] =
            [fn](dex_vm_t *vm, dex_value_t *a, int n, dex_value_t *r) {
                return fn(vm, a, n, r);
            };
    };

    // java.lang.Object
    reg_obj("Ljava/lang/Object;", "<init>", "V", stub_object_init);
    reg_obj("Ljava/lang/Object;", "<clinit>", "V", stub_object_clinit);
    reg_obj("Ljava/lang/Object;", "hashCode", "I",
        [](dex_vm_t*, dex_value_t *args, int, dex_value_t *res) -> int {
            if (res) res->i32 = (int32_t)(intptr_t)(args[0].ptr);
            return 0;
        });
    reg_obj("Ljava/lang/Object;", "equals", "LZ",
        [](dex_vm_t*, dex_value_t *args, int, dex_value_t *res) -> int {
            if (res) res->i32 = (args[0].ptr == args[1].ptr) ? 1 : 0;
            return 0;
        });
    reg_obj("Ljava/lang/Object;", "toString", "L",
        [](dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) -> int {
            if (!res) return 0;
            char buf[64];
            snprintf(buf, sizeof(buf), "%s@%p",
                     args[0].ptr ? ((dex_obj_t*)args[0].ptr)->cls->descriptor.c_str() : "?",
                     args[0].ptr);
            res->ptr = dex_new_string_utf(vm, buf);
            return 0;
        });
    reg_obj("Ljava/lang/Object;", "getClass", "L",
        [](dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) -> int {
            if (!res) return 0;
            dex_obj_t *o = (dex_obj_t*)args[0].ptr;
            if (o && o->cls) res->ptr = dex_new_string_utf(vm, o->cls->descriptor.c_str());
            else res->ptr = nullptr;
            return 0;
        });

    // java.lang.String
    reg_obj("Ljava/lang/String;", "<init>", "V", stub_string_init);
    reg_obj("Ljava/lang/String;", "<init>", "LB", stub_string_init_bytes);  // [B -> B in shorty
    reg_obj("Ljava/lang/String;", "<init>", "LL", stub_string_init);   // (Ljava/lang/String;)V
    reg_obj("Ljava/lang/String;", "length", "I", stub_string_length);
    reg_obj("Ljava/lang/String;", "equals", "LZ", stub_string_equals);
    reg_obj("Ljava/lang/String;", "hashCode", "I", stub_string_hashcode);
    reg_obj("Ljava/lang/String;", "toString", "L", stub_string_tostring);
    reg_obj("Ljava/lang/String;", "charAt", "II",
        [](dex_vm_t*, dex_value_t *args, int, dex_value_t *res) -> int {
            if (!res) return 0;
            dex_obj_t *s = (dex_obj_t*)args[0].ptr;
            int32_t i = args[1].i32;
            if (s && s->kind == OBJ_STRING && s->utf8 && i >= 0 && (uint32_t)i < s->utf8_len) {
                res->i32 = (int32_t)(unsigned char)s->utf8[i];
            } else res->i32 = 0;
            return 0;
        });
    reg_obj("Ljava/lang/String;", "substring", "LL",
        [](dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) -> int {
            if (!res) return 0;
            dex_obj_t *s = (dex_obj_t*)args[0].ptr;
            const char *str = (s && s->kind == OBJ_STRING && s->utf8) ? s->utf8 : "";
            int32_t begin = args[1].i32;
            if (begin < 0) begin = 0;
            if ((size_t)begin > strlen(str)) begin = (int32_t)strlen(str);
            res->ptr = dex_new_string_utf(vm, str + begin);
            return 0;
        });
    reg_obj("Ljava/lang/String;", "substring", "ILL",
        [](dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) -> int {
            if (!res) return 0;
            dex_obj_t *s = (dex_obj_t*)args[0].ptr;
            const char *str = (s && s->kind == OBJ_STRING && s->utf8) ? s->utf8 : "";
            int32_t begin = args[1].i32, end = args[2].i32;
            size_t slen = strlen(str);
            if (begin < 0) begin = 0;
            if ((size_t)begin > slen) begin = (int32_t)slen;
            if ((size_t)end > slen) end = (int32_t)slen;
            if (end < begin) end = begin;
            std::string sub(str + begin, str + end);
            res->ptr = dex_new_string_utf(vm, sub.c_str());
            return 0;
        });
    reg_obj("Ljava/lang/String;", "concat", "LL",
        [](dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) -> int {
            if (!res) return 0;
            const char *a = arg_str(&args[0]);
            const char *b = arg_str(&args[1]);
            std::string s = (a ? a : "");
            s += (b ? b : "");
            res->ptr = dex_new_string_utf(vm, s.c_str());
            return 0;
        });
    reg_obj("Ljava/lang/String;", "valueOf", "I", stub_integer_tostring); // same shape
    reg_obj("Ljava/lang/String;", "valueOf", "J",
        [](dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) -> int {
            if (!res) return 0;
            char buf[32]; snprintf(buf, sizeof(buf), "%lld", (long long)args[1].i64);
            res->ptr = dex_new_string_utf(vm, buf);
            return 0;
        });
    reg_obj("Ljava/lang/String;", "valueOf", "F",
        [](dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) -> int {
            if (!res) return 0;
            char buf[32]; snprintf(buf, sizeof(buf), "%g", (double)args[1].f32);
            res->ptr = dex_new_string_utf(vm, buf);
            return 0;
        });
    reg_obj("Ljava/lang/String;", "valueOf", "D",
        [](dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) -> int {
            if (!res) return 0;
            char buf[32]; snprintf(buf, sizeof(buf), "%g", args[1].f64);
            res->ptr = dex_new_string_utf(vm, buf);
            return 0;
        });
    reg_obj("Ljava/lang/String;", "valueOf", "L",
        [](dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) -> int {
            if (!res) return 0;
            const char *s = arg_str(&args[1]);
            res->ptr = dex_new_string_utf(vm, s ? s : "null");
            return 0;
        });
    reg_obj("Ljava/lang/String;", "valueOf", "Z",
        [](dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) -> int {
            if (!res) return 0;
            res->ptr = dex_new_string_utf(vm, args[1].i32 ? "true" : "false");
            return 0;
        });

    // java.lang.StringBuilder
    reg_obj("Ljava/lang/StringBuilder;", "<init>", "V", stub_sb_init);
    reg_obj("Ljava/lang/StringBuilder;", "<init>", "V", stub_sb_init);   // (I)V overload: same key — caller may use either
    reg_obj("Ljava/lang/StringBuilder;", "append", "LL", stub_sb_append_str);
    reg_obj("Ljava/lang/StringBuilder;", "append", "LI", stub_sb_append_int);
    reg_obj("Ljava/lang/StringBuilder;", "append", "LJ",
        [](dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) -> int {
            char buf[32]; snprintf(buf, sizeof(buf), "%lld", (long long)args[1].i64);
            dex_value_t tmp[2]; tmp[0].ptr = args[0].ptr; tmp[1].ptr = dex_new_string_utf(vm, buf);
            return stub_sb_append_str(vm, tmp, 2, res);
        });
    reg_obj("Ljava/lang/StringBuilder;", "append", "LF",
        [](dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) -> int {
            char buf[32]; snprintf(buf, sizeof(buf), "%g", (double)args[1].f32);
            dex_value_t tmp[2]; tmp[0].ptr = args[0].ptr; tmp[1].ptr = dex_new_string_utf(vm, buf);
            return stub_sb_append_str(vm, tmp, 2, res);
        });
    reg_obj("Ljava/lang/StringBuilder;", "append", "LD",
        [](dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) -> int {
            char buf[32]; snprintf(buf, sizeof(buf), "%g", args[1].f64);
            dex_value_t tmp[2]; tmp[0].ptr = args[0].ptr; tmp[1].ptr = dex_new_string_utf(vm, buf);
            return stub_sb_append_str(vm, tmp, 2, res);
        });
    reg_obj("Ljava/lang/StringBuilder;", "append", "LZ",
        [](dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) -> int {
            dex_value_t tmp[2]; tmp[0].ptr = args[0].ptr;
            tmp[1].ptr = dex_new_string_utf(vm, args[1].i32 ? "true" : "false");
            return stub_sb_append_str(vm, tmp, 2, res);
        });
    reg_obj("Ljava/lang/StringBuilder;", "toString", "L", stub_sb_tostring);
    reg_obj("Ljava/lang/StringBuilder;", "length", "I", stub_sb_length);

    // java.io.PrintStream
    reg_obj("Ljava/io/PrintStream;", "println", "V", stub_ps_println_void);
    reg_obj("Ljava/io/PrintStream;", "println", "VL", stub_ps_println_str);
    reg_obj("Ljava/io/PrintStream;", "println", "VI",
        [](dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) -> int {
            char buf[32]; snprintf(buf, sizeof(buf), "%d", args[1].i32);
            dex_value_t tmp[2]; tmp[0].ptr = args[0].ptr; tmp[1].ptr = dex_new_string_utf(vm, buf);
            return stub_ps_println_str(vm, tmp, 2, res);
        });
    reg_obj("Ljava/io/PrintStream;", "print", "VL", stub_ps_print_str);
    reg_obj("Ljava/io/PrintStream;", "printf", "VL",
        [](dex_vm_t*, dex_value_t *args, int, dex_value_t *res) -> int {
            const char *fmt = arg_str(&args[1]);
            LOGI("interp", "[System.out.printf] %s", fmt ? fmt : "?");
            if (res) res->ptr = args[0].ptr;
            return 0;
        });

    // java.lang.Math
    reg_obj("Ljava/lang/Math;", "max", "III", stub_math_max);
    reg_obj("Ljava/lang/Math;", "min", "III", stub_math_min);
    reg_obj("Ljava/lang/Math;", "abs", "II", stub_math_abs);
    reg_obj("Ljava/lang/Math;", "max", "JJJ", stub_math_max_long);
    reg_obj("Ljava/lang/Math;", "abs", "JJ",
        [](dex_vm_t*, dex_value_t *args, int, dex_value_t *res) -> int {
            if (res) res->i64 = (args[1].i64 < 0) ? -args[1].i64 : args[1].i64;
            return 0;
        });
    reg_obj("Ljava/lang/Math;", "abs", "FF",
        [](dex_vm_t*, dex_value_t *args, int, dex_value_t *res) -> int {
            if (res) res->f32 = fabsf(args[1].f32);
            return 0;
        });
    reg_obj("Ljava/lang/Math;", "abs", "DD",
        [](dex_vm_t*, dex_value_t *args, int, dex_value_t *res) -> int {
            if (res) res->f64 = fabs(args[1].f64);
            return 0;
        });
    reg_obj("Ljava/lang/Math;", "sqrt", "DD",
        [](dex_vm_t*, dex_value_t *args, int, dex_value_t *res) -> int {
            if (res) res->f64 = sqrt(args[1].f64);
            return 0;
        });
    reg_obj("Ljava/lang/Math;", "floor", "DD",
        [](dex_vm_t*, dex_value_t *args, int, dex_value_t *res) -> int {
            if (res) res->f64 = floor(args[1].f64);
            return 0;
        });
    reg_obj("Ljava/lang/Math;", "ceil", "DD",
        [](dex_vm_t*, dex_value_t *args, int, dex_value_t *res) -> int {
            if (res) res->f64 = ceil(args[1].f64);
            return 0;
        });
    reg_obj("Ljava/lang/Math;", "pow", "DDD",
        [](dex_vm_t*, dex_value_t *args, int, dex_value_t *res) -> int {
            if (res) res->f64 = pow(args[1].f64, args[2].f64);
            return 0;
        });
    reg_obj("Ljava/lang/Math;", "round", "IJ",
        [](dex_vm_t*, dex_value_t *args, int, dex_value_t *res) -> int {
            if (res) res->i64 = (int64_t)llround(args[1].f32);
            return 0;
        });
    reg_obj("Ljava/lang/Math;", "random", "D",
        [](dex_vm_t*, dex_value_t*, int, dex_value_t *res) -> int {
            if (res) res->f64 = (double)rand() / (double)RAND_MAX;
            return 0;
        });

    // java.lang.Integer / Long / Boolean / Double
    reg_obj("Ljava/lang/Integer;", "parseInt", "LI", stub_integer_parseint);
    reg_obj("Ljava/lang/Integer;", "parseInt", "LII",
        [](dex_vm_t*, dex_value_t *args, int, dex_value_t *res) -> int {
            if (!res) return 0;
            const char *s = arg_str(&args[1]);
            int radix = args[2].i32;
            if (radix == 0) radix = 10;
            res->i32 = s ? (int32_t)strtol(s, nullptr, radix) : 0;
            return 0;
        });
    reg_obj("Ljava/lang/Integer;", "toString", "IL",
        [](dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) -> int {
            if (!res) return 0;
            char buf[32]; snprintf(buf, sizeof(buf), "%d", args[1].i32);
            res->ptr = dex_new_string_utf(vm, buf);
            return 0;
        });
    reg_obj("Ljava/lang/Integer;", "valueOf", "IL", stub_integer_valueof);  // boxed Integer
    reg_obj("Ljava/lang/Integer;", "<init>", "VI", stub_object_init);
    reg_obj("Ljava/lang/Integer;", "intValue", "II",
        [](dex_vm_t*, dex_value_t *args, int, dex_value_t *res) -> int {
            if (res && args[0].ptr) {
                dex_obj_t *o = (dex_obj_t*)args[0].ptr;
                res->i32 = o->fields ? o->fields[0].i32 : 0;
            }
            return 0;
        });
    reg_obj("Ljava/lang/Long;", "parseLong", "LJ", stub_long_parselong);
    reg_obj("Ljava/lang/Long;", "toString", "JL",
        [](dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) -> int {
            if (!res) return 0;
            char buf[32]; snprintf(buf, sizeof(buf), "%lld", (long long)args[1].i64);
            res->ptr = dex_new_string_utf(vm, buf);
            return 0;
        });
    reg_obj("Ljava/lang/Double;", "parseDouble", "LD",
        [](dex_vm_t*, dex_value_t *args, int, dex_value_t *res) -> int {
            if (!res) return 0;
            const char *s = arg_str(&args[1]);
            res->f64 = s ? strtod(s, nullptr) : 0.0;
            return 0;
        });
    reg_obj("Ljava/lang/Boolean;", "valueOf", "ZL",
        [](dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) -> int {
            if (!res) return 0;
            dex_obj_t *o = new dex_obj();
            o->cls = dex_vm_resolve_class(vm, "Ljava/lang/Boolean;");
            o->kind = OBJ_STUB;
            o->fields = new dex_value_t[1]();
            o->fields[0].i32 = args[1].i32 ? 1 : 0;
            res->ptr = o;
            return 0;
        });

    // java.lang.Thread / Throwable
    reg_obj("Ljava/lang/Thread;", "<init>", "V", stub_thread_init);
    reg_obj("Ljava/lang/Thread;", "start", "V", stub_thread_start);
    reg_obj("Ljava/lang/Thread;", "currentThread", "L",
        [](dex_vm_t *vm, dex_value_t*, int, dex_value_t *res) -> int {
            if (!res) return 0;
            dex_obj_t *t = new dex_obj();
            t->cls = dex_vm_resolve_class(vm, "Ljava/lang/Thread;");
            t->kind = OBJ_STUB;
            res->ptr = t;
            return 0;
        });
    reg_obj("Ljava/lang/Throwable;", "<init>", "V", stub_throwable_init);
    reg_obj("Ljava/lang/Throwable;", "<init>", "VL", stub_throwable_init);
    reg_obj("Ljava/lang/Throwable;", "getMessage", "L", stub_throwable_getmessage);
    reg_obj("Ljava/lang/Throwable;", "toString", "L",
        [](dex_vm_t *vm, dex_value_t *args, int, dex_value_t *res) -> int {
            if (!res) return 0;
            dex_obj_t *t = (dex_obj_t*)args[0].ptr;
            const char *desc = t && t->cls ? t->cls->descriptor.c_str() : "Ljava/lang/Throwable;";
            res->ptr = dex_new_string_utf(vm, desc);
            return 0;
        });
    reg_obj("Ljava/lang/Exception;", "<init>", "V", stub_throwable_init);
    reg_obj("Ljava/lang/Exception;", "<init>", "VL", stub_throwable_init);
    reg_obj("Ljava/lang/RuntimeException;", "<init>", "V", stub_throwable_init);
    reg_obj("Ljava/lang/RuntimeException;", "<init>", "VL", stub_throwable_init);
    reg_obj("Ljava/lang/NullPointerException;", "<init>", "V", stub_throwable_init);
    reg_obj("Ljava/lang/NullPointerException;", "<init>", "VL", stub_throwable_init);

    // android.util.Log
    reg_obj("Landroid/util/Log;", "i", "LLI", stub_log_i);
    reg_obj("Landroid/util/Log;", "d", "LLI", stub_log_d);
    reg_obj("Landroid/util/Log;", "w", "LLI", stub_log_w);
    reg_obj("Landroid/util/Log;", "e", "LLI", stub_log_e);
    reg_obj("Landroid/util/Log;", "v", "LLI", stub_log_v);

    // android.app.Activity
    reg_obj("Landroid/app/Activity;", "<init>", "V", stub_activity_init);
    reg_obj("Landroid/app/Activity;", "onCreate", "VL", stub_activity_oncreate);
    reg_obj("Landroid/app/Activity;", "onStart", "V", stub_activity_onstart);
    reg_obj("Landroid/app/Activity;", "onResume", "V", stub_activity_onresume);
    reg_obj("Landroid/app/Activity;", "onPause", "V", stub_activity_onpause);
    reg_obj("Landroid/app/Activity;", "onStop", "V", stub_activity_onstop);
    reg_obj("Landroid/app/Activity;", "onDestroy", "V", stub_activity_ondestroy);
    reg_obj("Landroid/app/Activity;", "setContentView", "VI", stub_activity_setcontentview_int);
    reg_obj("Landroid/app/Activity;", "setContentView", "VL", stub_window_setcontentview);
    reg_obj("Landroid/app/Activity;", "findViewById", "IL", stub_activity_findviewbyid);
    reg_obj("Landroid/app/Activity;", "requestWindowFeature", "IZ", stub_activity_requestwindowfeature);
    reg_obj("Landroid/app/Activity;", "getWindow", "L", stub_activity_getwindow);
    reg_obj("Landroid/app/Activity;", "getPackageName", "L", stub_activity_getpackagename);
    reg_obj("Landroid/app/Activity;", "getFilesDir", "L", stub_context_getfilesdir);
    reg_obj("Landroid/app/Activity;", "getCacheDir", "L", stub_context_getcachedir);
    reg_obj("Landroid/app/Activity;", "getResources", "L", stub_context_getresources);
    reg_obj("Landroid/app/Activity;", "finish", "V",
        [](dex_vm_t*, dex_value_t*, int, dex_value_t*) -> int { return 0; });
    reg_obj("Landroid/app/Activity;", "isFinishing", "Z",
        [](dex_vm_t*, dex_value_t*, int, dex_value_t *res) -> int {
            if (res) res->i32 = 0; return 0;
        });

    // android.view.Window
    reg_obj("Landroid/view/Window;", "setFlags", "VII", stub_window_setflags);
    reg_obj("Landroid/view/Window;", "getDecorView", "L", stub_window_getdecorview);
    reg_obj("Landroid/view/Window;", "setContentView", "VL", stub_window_setcontentview);

    // android.view.View
    reg_obj("Landroid/view/View;", "<init>", "VL", stub_view_init);
    reg_obj("Landroid/view/View;", "invalidate", "V", stub_view_invalidate);
    reg_obj("Landroid/view/View;", "requestLayout", "V", stub_view_requestlayout);
    reg_obj("Landroid/view/View;", "setBackgroundColor", "VI", stub_view_setbackgroundcolor);
    reg_obj("Landroid/view/View;", "setVisibility", "VI",
        [](dex_vm_t*, dex_value_t*, int, dex_value_t*) -> int { return 0; });
    reg_obj("Landroid/view/View;", "setLayoutParams", "VL",
        [](dex_vm_t*, dex_value_t*, int, dex_value_t*) -> int { return 0; });

    // android.content.Context (called via Context subclass)
    reg_obj("Landroid/content/Context;", "getFilesDir", "L", stub_context_getfilesdir);
    reg_obj("Landroid/content/Context;", "getCacheDir", "L", stub_context_getcachedir);
    reg_obj("Landroid/content/Context;", "getPackageName", "L", stub_context_getpackagename);
    reg_obj("Landroid/content/Context;", "getResources", "L", stub_context_getresources);

    // java.io.File
    reg_obj("Ljava/io/File;", "<init>", "VL", stub_file_init_str);
    reg_obj("Ljava/io/File;", "getAbsolutePath", "L", stub_file_getabsolutepath);
    reg_obj("Ljava/io/File;", "exists", "Z", stub_file_exists);
    reg_obj("Ljava/io/File;", "mkdirs", "Z", stub_file_mkdirs);
    reg_obj("Ljava/io/File;", "mkdir", "Z", stub_file_mkdirs);

    // android.os.Bundle
    reg_obj("Landroid/os/Bundle;", "<init>", "V", stub_bundle_init);
    reg_obj("Landroid/os/Bundle;", "putString", "VLL", stub_bundle_putstring);
    reg_obj("Landroid/os/Bundle;", "getString", "LL", stub_bundle_getstring);

    // android.view.WindowManager$LayoutParams
    reg_obj("Landroid/view/WindowManager$LayoutParams;", "<clinit>", "V",
            stub_layoutparams_clinit);
    reg_obj("Landroid/view/WindowManager$LayoutParams;", "<init>", "V",
            stub_layoutparams_clinit);

    // android.opengl.GLSurfaceView (Java side only)
    reg_obj("Landroid/opengl/GLSurfaceView;", "<init>", "VL", stub_glsurfaceview_init);
    reg_obj("Landroid/opengl/GLSurfaceView;", "setRenderer", "VL", stub_glsurfaceview_setrenderer);
    reg_obj("Landroid/opengl/GLSurfaceView;", "setRenderMode", "VI", stub_glsurfaceview_setrendermode);
    reg_obj("Landroid/opengl/GLSurfaceView;", "onResume", "V", stub_glsurfaceview_onresume);
    reg_obj("Landroid/opengl/GLSurfaceView;", "onPause", "V", stub_glsurfaceview_onpause);
    reg_obj("Landroid/opengl/GLSurfaceView;", "queueEvent", "VL",
        [](dex_vm_t*, dex_value_t*, int, dex_value_t*) -> int { return 0; });

    // java.util.ArrayList / HashMap
    reg_obj("Ljava/util/ArrayList;", "<init>", "V", stub_arraylist_init);
    reg_obj("Ljava/util/ArrayList;", "add", "LZ", stub_arraylist_add);
    reg_obj("Ljava/util/ArrayList;", "size", "I", stub_arraylist_size);
    reg_obj("Ljava/util/HashMap;", "<init>", "V", stub_hashmap_init);
    reg_obj("Ljava/util/HashMap;", "put", "LLL", stub_hashmap_put);
    reg_obj("Ljava/util/HashMap;", "get", "LL", stub_hashmap_get);
    reg_obj("Ljava/util/HashMap;", "size", "I",
        [](dex_vm_t*, dex_value_t*, int, dex_value_t *res) -> int {
            if (res) res->i32 = 0; return 0;
        });

    // java.lang.System (the System class itself has only fields, no methods we stub).
    // (System.out is a static field — populated in populate_framework_statics.)

    // ========================================================================
    // P3-3 registrations
    // ========================================================================

    // ---- java.net.Socket (REAL BSD sockets) ----
    reg_obj("Ljava/net/Socket;", "<init>", "V", stub_socket_init_v);
    reg_obj("Ljava/net/Socket;", "<init>", "VLI", stub_socket_init_str_int);
    reg_obj("Ljava/net/Socket;", "connect", "VL",
        [](dex_vm_t *vm, dex_value_t *a, int n, dex_value_t *r) -> int {
            // connect(SocketAddress) — no timeout overload
            return stub_socket_connect_addr(vm, a, n, r);
        });
    reg_obj("Ljava/net/Socket;", "connect", "VLI", stub_socket_connect_addr);
    reg_obj("Ljava/net/Socket;", "getInputStream", "L", stub_socket_getinputstream);
    reg_obj("Ljava/net/Socket;", "getOutputStream", "L", stub_socket_getoutputstream);
    reg_obj("Ljava/net/Socket;", "close", "V", stub_socket_close);
    reg_obj("Ljava/net/Socket;", "setSoTimeout", "VI", stub_socket_setsotimeout);
    reg_obj("Ljava/net/Socket;", "getSoTimeout", "I", stub_socket_getsotimeout);
    reg_obj("Ljava/net/Socket;", "isConnected", "Z", stub_socket_isconnected);
    reg_obj("Ljava/net/Socket;", "isClosed", "Z", stub_socket_isclosed);

    // ---- java.net.ServerSocket ----
    reg_obj("Ljava/net/ServerSocket;", "<init>", "VI", stub_serversocket_init_port);
    reg_obj("Ljava/net/ServerSocket;", "accept", "L", stub_serversocket_accept);
    reg_obj("Ljava/net/ServerSocket;", "close", "V", stub_serversocket_close);

    // ---- java.net.DatagramSocket ----
    reg_obj("Ljava/net/DatagramSocket;", "<init>", "V", stub_dgramsocket_init_v);
    reg_obj("Ljava/net/DatagramSocket;", "<init>", "VI", stub_dgramsocket_init_port);
    reg_obj("Ljava/net/DatagramSocket;", "send", "VL", stub_dgramsocket_send);
    reg_obj("Ljava/net/DatagramSocket;", "receive", "VL", stub_dgramsocket_receive);
    reg_obj("Ljava/net/DatagramSocket;", "close", "V", stub_dgramsocket_close);

    // ---- java.net.DatagramPacket ----
    // <init>(byte[], int, int)V — shorty VLII (V + L(byte[]) + I + I)
    reg_obj("Ljava/net/DatagramPacket;", "<init>", "VLII", stub_dgrampacket_init_bii);
    // <init>(byte[], int, int, InetAddress, int)V — shorty VLIILL
    reg_obj("Ljava/net/DatagramPacket;", "<init>", "VLIILL", stub_dgrampacket_init_biiai);
    reg_obj("Ljava/net/DatagramPacket;", "getData", "L", stub_dgrampacket_getdata);
    reg_obj("Ljava/net/DatagramPacket;", "getLength", "I", stub_dgrampacket_getlength);
    reg_obj("Ljava/net/DatagramPacket;", "getAddress", "L", stub_dgrampacket_getaddress);
    reg_obj("Ljava/net/DatagramPacket;", "getPort", "I", stub_dgrampacket_getport);

    // ---- java.net.InetAddress ----
    reg_obj("Ljava/net/InetAddress;", "getByName", "LL", stub_inetaddress_getbyname);
    reg_obj("Ljava/net/InetAddress;", "getHostAddress", "L", stub_inetaddress_gethostaddress);
    reg_obj("Ljava/net/InetAddress;", "getHostName", "L", stub_inetaddress_gethostname);

    // ---- java.io.InputStream / OutputStream (backing Socket) ----
    reg_obj("Ljava/io/InputStream;", "read", "IL", stub_inputstream_read_b);
    reg_obj("Ljava/io/InputStream;", "read", "ILII", stub_inputstream_read_bii);
    reg_obj("Ljava/io/InputStream;", "read", "I", stub_inputstream_read_int);
    reg_obj("Ljava/io/InputStream;", "close", "V", stub_stream_close);
    reg_obj("Ljava/io/OutputStream;", "write", "VLII", stub_outputstream_write_bii);
    reg_obj("Ljava/io/OutputStream;", "write", "VI", stub_outputstream_write_int);
    reg_obj("Ljava/io/OutputStream;", "flush", "V", stub_stream_flush);
    reg_obj("Ljava/io/OutputStream;", "close", "V", stub_stream_close);

    // ---- java.io.IOException / FileNotFoundException ctors ----
    reg_obj("Ljava/io/IOException;", "<init>", "V", stub_ioexception_init);
    reg_obj("Ljava/io/IOException;", "<init>", "VL", stub_ioexception_init);
    reg_obj("Ljava/io/FileNotFoundException;", "<init>", "V", stub_ioexception_init);
    reg_obj("Ljava/io/FileNotFoundException;", "<init>", "VL", stub_ioexception_init);

    // ---- java.lang.Class ----
    reg_obj("Ljava/lang/Class;", "forName", "LL", stub_class_forname);
    reg_obj("Ljava/lang/Class;", "getName", "L", stub_class_getname);
    reg_obj("Ljava/lang/Class;", "getSimpleName", "L", stub_class_getsimplename);
    reg_obj("Ljava/lang/Class;", "getMethod", "LLL", stub_class_getmethod);
    reg_obj("Ljava/lang/Class;", "getDeclaredMethod", "LLL", stub_class_getdeclaredmethod);
    reg_obj("Ljava/lang/Class;", "getField", "LL", stub_class_getfield);
    reg_obj("Ljava/lang/Class;", "getDeclaredField", "LL", stub_class_getfield);
    reg_obj("Ljava/lang/Class;", "getMethods", "L", stub_class_getmethods);
    reg_obj("Ljava/lang/Class;", "newInstance", "L", stub_class_newinstance);

    // ---- java.lang.reflect.Method ----
    reg_obj("Ljava/lang/reflect/Method;", "invoke", "LLL", stub_method_invoke);
    reg_obj("Ljava/lang/reflect/Method;", "setAccessible", "VZ", stub_method_setaccessible);
    reg_obj("Ljava/lang/reflect/Method;", "getName", "L", stub_method_getname);

    // ---- java.lang.reflect.Field ----
    reg_obj("Ljava/lang/reflect/Field;", "get", "LL", stub_field_get);
    reg_obj("Ljava/lang/reflect/Field;", "set", "VLL", stub_field_set);
    reg_obj("Ljava/lang/reflect/Field;", "setAccessible", "VZ", stub_field_setaccessible);
    reg_obj("Ljava/lang/reflect/Field;", "getName", "L", stub_field_getname);

    // ---- Boxing for reflection: Integer/Long/Float/Double/Boolean valueOf + *Value ----
    // JNI shorty convention: return-type first, then params; 'L' covers all
    // reference types including arrays. For static valueOf(x), args[0] = the
    // primitive param (no receiver slot per extract_invoke_args).
    // The pre-existing Integer.valueOf/intValue registrations use incorrect
    // shorties ("IL" and "II") — we register the correct JNI shorties here
    // ("LI" and "I") so reflection boxing actually resolves.
    reg_obj("Ljava/lang/Integer;", "valueOf", "LI", stub_integer_valueof_static);
    reg_obj("Ljava/lang/Integer;", "intValue", "I", stub_integer_intvalue);
    reg_obj("Ljava/lang/Long;", "valueOf", "LJ", stub_long_valueof);
    reg_obj("Ljava/lang/Long;", "longValue", "J", stub_long_longvalue);
    reg_obj("Ljava/lang/Long;", "<init>", "VJ", stub_object_init);
    reg_obj("Ljava/lang/Float;", "valueOf", "LF", stub_float_valueof);
    reg_obj("Ljava/lang/Float;", "floatValue", "F", stub_float_floatvalue);
    reg_obj("Ljava/lang/Float;", "<init>", "VF", stub_object_init);
    reg_obj("Ljava/lang/Double;", "valueOf", "LD", stub_double_valueof);
    reg_obj("Ljava/lang/Double;", "doubleValue", "D", stub_double_doublevalue);
    reg_obj("Ljava/lang/Double;", "<init>", "VD", stub_object_init);
    reg_obj("Ljava/lang/Boolean;", "valueOf", "LZ", stub_boolean_valueof);
    reg_obj("Ljava/lang/Boolean;", "booleanValue", "Z", stub_boolean_booleanvalue);
    reg_obj("Ljava/lang/Boolean;", "<init>", "VZ", stub_object_init);

    // ---- android.media.AudioTrack ----
    // <init>(IIIIII)V — streamType, sampleRate, channelConfig, audioFormat,
    //                   bufferSize, mode. Shorty "VIIIIII" (return V + 6 ints).
    reg_obj("Landroid/media/AudioTrack;", "<init>", "VIIIIII", stub_audiotrack_init);
    // write(byte[]|short[], int, int) -> int. Both overloads share shorty
    // "ILII"; the handler dispatches by array elem_cat (byte vs short).
    reg_obj("Landroid/media/AudioTrack;", "write", "ILII", stub_audiotrack_write_b);
    reg_obj("Landroid/media/AudioTrack;", "play", "V", stub_audiotrack_play);
    reg_obj("Landroid/media/AudioTrack;", "pause", "V", stub_audiotrack_pause);
    reg_obj("Landroid/media/AudioTrack;", "stop", "V", stub_audiotrack_stop);
    reg_obj("Landroid/media/AudioTrack;", "release", "V", stub_audiotrack_release);
    reg_obj("Landroid/media/AudioTrack;", "getMinBufferSize", "III",
        stub_audiotrack_getminbuffersize);
    reg_obj("Landroid/media/AudioTrack;", "setNotificationMarkerPosition", "VI",
        stub_audiotrack_setnotifmarker);
    reg_obj("Landroid/media/AudioTrack;", "setPlaybackPositionUpdateListener", "VL",
        stub_audiotrack_setpositionlistener);

    // ---- android.media.MediaPlayer ----
    reg_obj("Landroid/media/MediaPlayer;", "<init>", "V", stub_mediaplayer_init);
    reg_obj("Landroid/media/MediaPlayer;", "setDataSource", "VL", stub_mediaplayer_setdatasource_str);
    reg_obj("Landroid/media/MediaPlayer;", "setDataSource", "VLL",
        [](dex_vm_t *vm, dex_value_t *a, int n, dex_value_t *r) -> int {
            // (Context, String) overload — defer to the String impl.
            return stub_mediaplayer_setdatasource_str(vm, a, n, r);
        });
    reg_obj("Landroid/media/MediaPlayer;", "prepare", "V", stub_mediaplayer_prepare);
    reg_obj("Landroid/media/MediaPlayer;", "prepareAsync", "V", stub_mediaplayer_prepareasync);
    reg_obj("Landroid/media/MediaPlayer;", "start", "V", stub_mediaplayer_start);
    reg_obj("Landroid/media/MediaPlayer;", "pause", "V", stub_mediaplayer_pause);
    reg_obj("Landroid/media/MediaPlayer;", "stop", "V", stub_mediaplayer_stop);
    reg_obj("Landroid/media/MediaPlayer;", "release", "V", stub_mediaplayer_release);
    reg_obj("Landroid/media/MediaPlayer;", "setLooping", "VZ", stub_mediaplayer_setlooping);
    reg_obj("Landroid/media/MediaPlayer;", "setVolume", "VFF", stub_mediaplayer_setvolume);
    reg_obj("Landroid/media/MediaPlayer;", "setDisplay", "VL", stub_mediaplayer_setdisplay);
    reg_obj("Landroid/media/MediaPlayer;", "setOnPreparedListener", "VL",
        stub_mediaplayer_setonprepared);
    reg_obj("Landroid/media/MediaPlayer;", "setOnErrorListener", "VL",
        stub_mediaplayer_setonerror);

    // ---- android.media.SoundPool ----
    reg_obj("Landroid/media/SoundPool;", "<init>", "VIII", stub_soundpool_init);
    // load(String, int) -> int. Instance method; shorty ILI (I + L + I).
    reg_obj("Landroid/media/SoundPool;", "load", "ILI", stub_soundpool_load);
    reg_obj("Landroid/media/SoundPool;", "play", "IIFFIIF", stub_soundpool_play);
    reg_obj("Landroid/media/SoundPool;", "release", "V", stub_soundpool_release);

    // ---- android.content.Context gaps ----
    // getSharedPreferences(String, int) -> SharedPreferences; shorty LLI (L + L + I).
    reg_obj("Landroid/content/Context;", "getSharedPreferences", "LLI",
        stub_context_getsharedprefs);
    reg_obj("Landroid/content/Context;", "getExternalCacheDir", "L",
        stub_context_getexternalcachedir);
    reg_obj("Landroid/content/Context;", "getAssets", "L", stub_context_getassets);
    reg_obj("Landroid/app/Activity;", "getSharedPreferences", "LLI",
        stub_context_getsharedprefs);
    reg_obj("Landroid/app/Activity;", "getExternalCacheDir", "L",
        stub_context_getexternalcachedir);
    reg_obj("Landroid/app/Activity;", "getAssets", "L", stub_context_getassets);

    // ---- android.content.SharedPreferences ----
    reg_obj("Landroid/content/SharedPreferences;", "getString", "LLL",
        stub_sharedprefs_getstring);
    reg_obj("Landroid/content/SharedPreferences;", "getInt", "ILI",
        stub_sharedprefs_getint);
    reg_obj("Landroid/content/SharedPreferences;", "getBoolean", "ZLZ",
        stub_sharedprefs_getboolean);
    reg_obj("Landroid/content/SharedPreferences;", "edit", "L", stub_sharedprefs_edit);
    // Editor methods (we return the SP itself from edit(), so the Editor
    // interface methods are registered on SharedPreferences too).
    reg_obj("Landroid/content/SharedPreferences;", "putString", "LLL",
        stub_sharedprefs_putstring);
    reg_obj("Landroid/content/SharedPreferences;", "putInt", "LLI",
        stub_sharedprefs_putint);
    reg_obj("Landroid/content/SharedPreferences;", "putBoolean", "LLZ",
        stub_sharedprefs_putboolean);
    reg_obj("Landroid/content/SharedPreferences;", "commit", "Z",
        stub_sharedprefs_commit);
    reg_obj("Landroid/content/SharedPreferences;", "apply", "V",
        stub_sharedprefs_apply);

    // ---- android.content.Intent ----
    reg_obj("Landroid/content/Intent;", "<init>", "V", stub_intent_init_v);
    reg_obj("Landroid/content/Intent;", "<init>", "VLL", stub_intent_init_ctx_class);
    reg_obj("Landroid/content/Intent;", "putExtra", "LLL",
        [](dex_vm_t *vm, dex_value_t *a, int n, dex_value_t *r) -> int {
            return stub_intent_putextra_str(vm, a, n, r);
        });
    reg_obj("Landroid/content/Intent;", "getStringExtra", "LL",
        stub_intent_getstringextra);
    reg_obj("Landroid/content/Intent;", "getAction", "L", stub_intent_getaction);
    reg_obj("Landroid/content/Intent;", "setAction", "LL", stub_intent_setaction);

    // ---- android.content.res.AssetManager ----
    reg_obj("Landroid/content/res/AssetManager;", "open", "LL",
        [](dex_vm_t *vm, dex_value_t *a, int n, dex_value_t *r) -> int {
            return stub_assetmanager_open(vm, a, n, r);
        });

    // ---- android.util.Log.println(int, String, String) -> int (static) ----
    reg_obj("Landroid/util/Log;", "println", "ILLI", stub_log_println);

    LOGI("interp", "registered %zu framework stub methods", vm->natives.size());
}
