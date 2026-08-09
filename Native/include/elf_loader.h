/*
 * elf_loader.h — in-process ELF loader for Android arm64-v8a .so files
 *
 * Status: REAL. PT_LOAD mapping, AARCH64 relocations (RELATIVE/ABS64/GLOB_DAT/
 *         JUMP_SLOT/TLS_DTPMOD/TLS_DTPREL/TLS_TPREL), DT_GNU_HASH + DT_HASH
 *         symbol lookup, DT_INIT/DT_INIT_ARRAY execution, JNI_OnLoad invocation.
 *         Requires TrollStore entitlements (allow-unsigned-executable-memory)
 *         to mprotect code segments PROT_EXEC; otherwise fails with EPERM.
 *
 * Part of APKLive. See docs/ARCHITECTURE.md §3.
 */
#ifndef APKCONTAINER_ELF_LOADER_H
#define APKCONTAINER_ELF_LOADER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <elf.h>   /* Elf64_* types */

typedef struct elf_module {
    char             name[256];        /* basename, e.g. "libunity.so"            */
    void            *base;             /* load base address (mmap'd)              */
    size_t           mapped_size;      /* total mapped bytes                       */
    uint64_t         vaddr_min;        /* virtual offset of base in file          */

    /* .dynamic-derived tables (all relative to base) */
    Elf64_Dyn       *dyn;
    Elf64_Sym       *symtab;           /* DT_SYMTAB                                */
    const char      *strtab;           /* DT_STRTAB                                */
    size_t           strtab_size;
    Elf64_Word      *gnu_hash;         /* DT_GNU_HASH (or NULL)                    */
    Elf64_Word      *hash;             /* DT_HASH (or NULL)                        */
    Elf64_Rela      *rela;             /* DT_RELA                                  */
    size_t           rela_sz;          /* DT_RELASZ                                */
    Elf64_Rela      *jmprel;           /* DT_JMPREL (PLT relocations)              */
    size_t           pltrel_sz;        /* DT_PLTRELSZ                              */
    Elf64_Addr      *init_array;       /* DT_INIT_ARRAY                            */
    size_t           init_array_sz;    /* DT_INIT_ARRAYSZ (bytes)                  */
    Elf64_Addr      *fini_array;       /* DT_FINI_ARRAY                            */
    size_t           fini_array_sz;
    Elf64_Addr       init_fn;          /* DT_INIT (single function)                */
    Elf64_Addr       fini_fn;          /* DT_FINI                                  */

    /* DT_NEEDED sonames (point into strtab) */
    const char     **needed;
    int              needed_count;

    int              jni_onload_called;
    void            *jni_onload;       /* resolved JNI_OnLoad fn ptr, or NULL      */
    int              loaded;           /* 1 once relocations + init run            */
} elf_module_t;

/* Load + link + run DT_INIT_ARRAY + JNI_OnLoad (if present).
 * Returns 0 on success, non-zero errno-like code on failure.
 * Fails with EPERM on non-TrollStore/non-jailbreak (mprotect PROT_EXEC). */
int  apkcontainer_elf_load(const char *path, elf_module_t *out);

/* Resolve a symbol from an already-loaded module via DT_GNU_HASH/DT_HASH. */
void *apkcontainer_elf_sym(elf_module_t *mod, const char *name);

/* Unmap + DT_FINI_ARRAY. */
int  apkcontainer_elf_unload(elf_module_t *mod);

/* Register an internal shim library (e.g. "libc.so" -> our bionic_shim table)
 * so DT_NEEDED resolution finds it instead of hitting disk. */
void apkcontainer_elf_register_shim_lib(const char *soname,
                                        void *(*resolver)(const char *));

/* Register an already-loaded module under its soname so other modules'
 * DT_NEEDED resolution can find sibling .so's (e.g. libunity needing
 * libil2cpp). */
void apkcontainer_elf_register_loaded(const char *soname, elf_module_t *mod);

#ifdef __cplusplus
}
#endif
#endif
