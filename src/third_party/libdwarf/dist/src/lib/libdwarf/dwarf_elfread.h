/* Copyright (c) 2013-2023, David Anderson All rights reserved.

Redistribution and use in source and binary forms, with
or without modification, are permitted provided that the
following conditions are met:

    Redistributions of source code must retain the above
    copyright notice, this list of conditions and the following
    disclaimer.

    Redistributions in binary form must reproduce the above
    copyright notice, this list of conditions and the following
    disclaimer in the documentation and/or other materials
    provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/
#ifndef READELFOBJ_H
#define READELFOBJ_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/*  Use this for .rel. too. */
struct generic_rela {
    Dwarf_Unsigned gr_offset;
    Dwarf_Unsigned gr_info;
    Dwarf_Unsigned gr_sym; /* From info */
    Dwarf_Unsigned gr_type; /* From info */
    Dwarf_Signed   gr_addend;
    unsigned char  gr_type2; /*MIPS64*/
    unsigned char  gr_type3; /*MIPS64*/
    /*  The following TRUE if .rela. and FALSE if .rel.
        if FALSE, gr_addend will be zero. */
    int            gr_is_rela;
};

/*  The following are generic to simplify handling
    Elf32 and Elf64.  Some fields added where
    the two sizes have different extraction code. */
struct generic_ehdr {
    unsigned char ge_ident[EI_NIDENT];
    Dwarf_Unsigned ge_type;
    Dwarf_Unsigned ge_machine;
    Dwarf_Unsigned ge_version;
    Dwarf_Unsigned ge_entry;
    Dwarf_Unsigned ge_phoff;
    Dwarf_Unsigned ge_shoff;
    Dwarf_Unsigned ge_flags;
    Dwarf_Unsigned ge_ehsize;
    Dwarf_Unsigned ge_phentsize;
    Dwarf_Unsigned ge_phnum;
    Dwarf_Unsigned ge_shentsize;
    Dwarf_Unsigned ge_shnum;
    /*  if ge_shnum >= 0xff00 SHN_LORESERVE
        Once section zero is read we put the sh_size
        member as the true count and set ge_shnum_in_shnum TRUE.
        ge_shnum_extended is TRUE if the object used the extension
        mechanism */
    Dwarf_Bool ge_shnum_in_shnum;
    Dwarf_Bool ge_shnum_extended;

    /* if section num of sec strings >= 0xff SHN_LORESERVE
        this member holds SHN_XINDEX (0xffff) and the real
        section string index is the sh_link value of section
        0.  ge_sstrndx_extended is TRUE if the object used
        the extension mechanism */
    Dwarf_Unsigned ge_shstrndx;
    Dwarf_Bool ge_strndx_in_strndx;
    Dwarf_Bool ge_strndx_extended;
};
struct generic_phdr {
    Dwarf_Unsigned gp_type;
    Dwarf_Unsigned gp_flags;
    Dwarf_Unsigned gp_offset;
    Dwarf_Unsigned gp_vaddr;
    Dwarf_Unsigned gp_paddr;
    Dwarf_Unsigned gp_filesz;
    Dwarf_Unsigned gp_memsz;
    Dwarf_Unsigned gp_align;
};
struct generic_shdr {
    Dwarf_Unsigned gh_secnum;
    Dwarf_Unsigned gh_name;
    const char * gh_namestring;
    Dwarf_Unsigned gh_type;
    Dwarf_Unsigned gh_flags;
    Dwarf_Unsigned gh_addr;
    Dwarf_Unsigned gh_offset;
    Dwarf_Unsigned gh_size;
    Dwarf_Unsigned gh_link;
    /*  Section index (in an SHT_REL or SHT_RELA section)
        of the target section from gh_link. Otherwise 0. */
    Dwarf_Unsigned gh_reloc_target_secnum;
    Dwarf_Unsigned gh_info;
    Dwarf_Unsigned gh_addralign;
    Dwarf_Unsigned gh_entsize;

    /*  Zero unless content read in. Malloc space
        of size gh_size,  in bytes.
        or if load type Dwarf_Alloc_Mmap
        gh_content is a pointer to the user data.
        free() or
        unmap this  this if not null*/
    char *       gh_content;
    /*  Actual load type */
    enum Dwarf_Sec_Alloc_Pref gh_load_type;
    /*  Normally TRUE, meaning do free/munmap in dwarf_finish() */
    Dwarf_Small  gh_was_alloc;
    char *       gh_mmap_realarea;
    Dwarf_Unsigned gh_computed_mmaplen;

    /*  If a .rel or .rela section this will point
        to generic relocation records if such
        have been loaded.
        free() this if not null, always malloc. */
    Dwarf_Unsigned        gh_relcount;
    struct generic_rela * gh_rels;

    /*  For SHT_GROUP based  grouping, which
        group is this section in. 0 unknown,
        1 DW_GROUP_NUMBER_BASE base DWARF,
        2 DW_GROUPNUMBER_DWO  dwo sections, 3+
        are in an SHT_GROUP. GNU uses this.
        set with group number (3+) from SHT_GROUP
        and the flags should have SHF_GROUP set
        if in SHT_GROUP. Must only be in one group? */
    Dwarf_Unsigned gh_section_group_number;

    /*  Content of an SHT_GROUP section as an array
        of integers. [0] is the version, which
        can only be one(1) . Free this on dwarf_finish() */
    Dwarf_Unsigned * gh_sht_group_array;
    /*  Number of elements in the gh_sht_group_array. */
    Dwarf_Unsigned   gh_sht_group_array_count;

    /*   TRUE if .debug_info .eh_frame etc. */
    char  gh_is_dwarf;
};

struct generic_dynentry {
    Dwarf_Unsigned  gd_tag;
    /*  gd_val stands in for d_ptr and d_val union,
        the union adds nothing in practice since
        we expect ptrsize <= ulongest. */
    Dwarf_Unsigned  gd_val;
    Dwarf_Unsigned  gd_dyn_file_offset;
};

struct generic_symentry {
    Dwarf_Unsigned gs_name;
    Dwarf_Unsigned gs_value;
    Dwarf_Unsigned gs_size;
    Dwarf_Unsigned gs_info;
    Dwarf_Unsigned gs_other;
    Dwarf_Unsigned gs_shndx;
    /* derived */
    Dwarf_Unsigned gs_bind;
    Dwarf_Unsigned gs_type;
};

struct location {
    const char *g_name;
    Dwarf_Unsigned g_offset;
    Dwarf_Unsigned g_count;
    Dwarf_Unsigned g_entrysize;
    Dwarf_Unsigned g_totalsize;
};

typedef struct elf_filedata_s {
    /*  f_ident[0] == 'E' means it is elf and
        elf_filedata_s is the struct involved.
        Other means error/corruption of some kind.
        f_ident[1] is a version number.
        Only version 1 is defined. */
    char           f_ident[8];
    char *         f_path; /* non-null if known. Must be freed */
    int            f_fd;
    unsigned       f_machine; /* EM_* */
    int            f_destruct_close_fd;
    int            f_is_64bit;
    unsigned       f_endian;
    Dwarf_Unsigned f_filesize;
    Dwarf_Unsigned f_flags;
    /* Elf size, not DWARF. 32 or 64 */
    Dwarf_Small    f_offsetsize;
    Dwarf_Small    f_pointersize;
    int            f_ftype;
    int            f_path_source;
    Dwarf_Debug    f_dbg;

    Dwarf_Unsigned f_max_secdata_offset;
    Dwarf_Unsigned f_max_progdata_offset;

    void (*f_copy_word) (void *, const void *, unsigned long);

    struct location      f_loc_ehdr;
    struct generic_ehdr* f_ehdr;

    struct location      f_loc_shdr;
    struct generic_shdr* f_shdr;

    struct location      f_loc_phdr;
    struct generic_phdr* f_phdr;

    char *f_elf_shstrings_data; /* section name strings */
    /* length of currentsection.  Might be zero..*/
    Dwarf_Unsigned  f_elf_shstrings_length;
    /* size of malloc-d space */
    Dwarf_Unsigned  f_elf_shstrings_max;

    /* This is the .dynamic section */
    struct location      f_loc_dynamic;
    struct generic_dynentry * f_dynamic;
    Dwarf_Unsigned f_dynamic_sect_index;

    /* .dynsym, .dynstr */
    struct location      f_loc_dynsym;
    struct generic_symentry* f_dynsym;
    char  *f_dynsym_sect_strings;
    Dwarf_Unsigned f_dynsym_sect_strings_max;
    Dwarf_Unsigned f_dynsym_sect_strings_sect_index;
    Dwarf_Unsigned f_dynsym_sect_index;

    /* .symtab .strtab */
    struct location      f_loc_symtab;
    struct generic_symentry* f_symtab;
    char * f_symtab_sect_strings;
    Dwarf_Unsigned f_symtab_sect_strings_max;
    Dwarf_Unsigned f_symtab_sect_strings_sect_index;
    Dwarf_Unsigned f_symtab_sect_index;

    /* Starts at 3. 0,1,2 used specially. */
    Dwarf_Unsigned f_sg_next_group_number;
    /*  Both the following will be zero unless there
        are explicit Elf groups. */
    Dwarf_Unsigned f_sht_group_type_section_count;
    Dwarf_Unsigned f_shf_group_flag_section_count;
    Dwarf_Unsigned f_dwo_group_section_count;
} dwarf_elf_object_access_internals_t;

int dwarf_construct_elf_access(int fd,
    const char *path,
    dwarf_elf_object_access_internals_t **ep,int *errcode);
int dwarf_destruct_elf_access(
    dwarf_elf_object_access_internals_t *ep,int *errcode);
int _dwarf_load_elf_header(
    dwarf_elf_object_access_internals_t *ep,int *errcode);
int _dwarf_load_elf_sectheaders(
    dwarf_elf_object_access_internals_t* ep,int *errcode);
int _dwarf_load_elf_symtab_symbols(
    dwarf_elf_object_access_internals_t *ep,int *errcode);
int _dwarf_load_elf_symstr(
    dwarf_elf_object_access_internals_t *ep, int *errcode);

/*  These two enums used for type safety in passing
    values. */
enum RelocRela {
    RelocIsRela = 1,
    RelocIsRel = 2
};
enum RelocOffsetSize {
    RelocOffset32 = 4,
    RelocOffset64 = 8
};

int _dwarf_load_elf_relx(dwarf_elf_object_access_internals_t *ep,
    Dwarf_Unsigned secnum,enum RelocRela,int *errcode);

#ifndef EI_NIDENT
#define EI_NIDENT 16
#endif /* EI_NIDENT */

#ifndef SHN_XINDEX
#define SHN_XINDEX 0xffff
#endif /* SHN_XINDEX */

#ifndef SHN_lORESERVE
#define SHN_LORESERVE 0xff00
#endif /* SHN_lORESERVE */

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* READELFOBJ_H */
