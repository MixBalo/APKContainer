// elf_shim.h — Minimal ELF64 definitions for iOS builds where <elf.h> is unavailable.
// This covers the types and constants used by elf_loader.h.

#ifndef ELF_SHIM_H
#define ELF_SHIM_H

#include <stdint.h>

/* ELF types */
typedef uint64_t Elf64_Addr;
typedef uint16_t Elf64_Half;
typedef uint64_t Elf64_Off;
typedef int32_t  Elf64_Sword;
typedef uint32_t Elf64_Word;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;

/* e_ident indices and values */
#define EI_NIDENT 16

/* ELF magic string (convenience macro) */
#define ELFMAG    "\177ELF"
#define SELFMAG   4

/* Special section indices */
#define SHN_UNDEF     0
#define SHN_COMMON    0xFFF2

/* Extra AARCH64 relocation types used by the loader */
#define R_AARCH64_COPY       1024
#define R_AARCH64_IRELATIVE  1032

#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EV_CURRENT 1

/* e_type */
#define ET_DYN 3
#define ET_EXEC 2

/* e_machine */
#define EM_AARCH64 183

/* Program header types */
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define PT_GNU_STACK 0x6474e551
#define PT_GNU_RELRO 0x6474e552

/* Program header flags */
#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

/* Section header types */
#define SHT_NULL     0
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_HASH     5
#define SHT_DYNAMIC  6
#define SHT_NOBITS   8
#define SHT_REL      9
#define SHT_DYNSYM   11
#define SHT_INIT_ARRAY 14
#define SHT_FINI_ARRAY 15

/* Dynamic entry tags */
#define DT_NULL         0
#define DT_NEEDED       1
#define DT_PLTRELSZ     2
#define DT_PLTGOT       3
#define DT_HASH         4
#define DT_STRTAB       5
#define DT_SYMTAB       6
#define DT_RELA         7
#define DT_RELASZ       8
#define DT_RELAENT      9
#define DT_STRSZ        10
#define DT_SYMENT       11
#define DT_INIT         12
#define DT_FINI         13
#define DT_SONAME       14
#define DT_RPATH        15
#define DT_SYMBOLIC     16
#define DT_REL          17
#define DT_RELSZ        18
#define DT_RELENT       19
#define DT_PLTREL       20
#define DT_DEBUG        21
#define DT_TEXTREL      22
#define DT_JMPREL       23
#define DT_BIND_NOW     24
#define DT_INIT_ARRAY   25
#define DT_FINI_ARRAY   26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_RUNPATH      29
#define DT_FLAGS        30
#define DT_GNU_HASH     0x6ffffef5
#define DT_VERSYM       0x6ffffff0
#define DT_VERDEF       0x6ffffffc
#define DT_VERDEFNUM    0x6ffffffd

/* Relocation types for AARCH64 */
#define R_AARCH64_NONE            0
#define R_AARCH64_ABS64         257
#define R_AARCH64_GLOB_DAT      1025
#define R_AARCH64_JUMP_SLOT     1026
#define R_AARCH64_RELATIVE      1027
#define R_AARCH64_TLS_DTPMOD64  1028
#define R_AARCH64_TLS_DTPREL64  1029
#define R_AARCH64_TLS_TPREL64   1030
#define R_AARCH64_TLSDESC       1031

/* ELF header */
typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf64_Half    e_type;
    Elf64_Half    e_machine;
    Elf64_Word    e_version;
    Elf64_Addr    e_entry;
    Elf64_Off     e_phoff;
    Elf64_Off     e_shoff;
    Elf64_Word    e_flags;
    Elf64_Half    e_ehsize;
    Elf64_Half    e_phentsize;
    Elf64_Half    e_phnum;
    Elf64_Half    e_shentsize;
    Elf64_Half    e_shnum;
    Elf64_Half    e_shstrndx;
} Elf64_Ehdr;

/* Program header */
typedef struct {
    Elf64_Word  p_type;
    Elf64_Word  p_flags;
    Elf64_Off   p_offset;
    Elf64_Addr  p_vaddr;
    Elf64_Addr  p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
} Elf64_Phdr;

/* Section header */
typedef struct {
    Elf64_Word  sh_name;
    Elf64_Word  sh_type;
    Elf64_Xword sh_flags;
    Elf64_Addr  sh_addr;
    Elf64_Off   sh_offset;
    Elf64_Xword sh_size;
    Elf64_Word  sh_link;
    Elf64_Word  sh_info;
    Elf64_Xword sh_addralign;
    Elf64_Xword sh_entsize;
} Elf64_Shdr;

/* Symbol table entry */
typedef struct {
    Elf64_Word  st_name;
    unsigned char st_info;
    unsigned char st_other;
    Elf64_Half  st_shndx;
    Elf64_Addr  st_value;
    Elf64_Xword st_size;
} Elf64_Sym;

/* Relocation entry with addend */
typedef struct {
    Elf64_Addr  r_offset;
    Elf64_Xword r_info;
    Elf64_Sxword r_addend;
} Elf64_Rela;

/* Dynamic section entry */
typedef struct {
    Elf64_Sxword d_tag;
    union {
        Elf64_Xword d_val;
        Elf64_Addr  d_ptr;
    } d_un;
} Elf64_Dyn;

/* GNU hash bloom filter word size */
#define ELF64_R_SYM(i)    ((i) >> 32)
#define ELF64_R_TYPE(i)   ((i) & 0xffffffff)
#define ELF64_R_INFO(s,t) (((Elf64_Xword)(s) << 32) + ((t) & 0xffffffff))

#endif /* ELF_SHIM_H */
