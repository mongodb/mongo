/*
Copyright (c) 2019, David Anderson
All rights reserved.
cc
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

/*  This file reads the parts of an Elf
    file appropriate to reading DWARF debugging data.
    Overview:
    _dwarf_elf_nlsetup() Does all elf setup.
        calls _dwarf_elf_access_init()
            calls _dwarf_elf_object_access_internals_init()
                Creates internals record 'M',
                    dwarf_elf_object_access_internals_t
                Sets flags/data in internals record
                Loads elf object data needed later.
                Sets methods struct to access elf object.
        calls _dwarf_object_init_b() Creates Dwarf_Debug, independent
            of any elf code.
        Sets internals record into dbg.
    ----------------------
    _dwarf_destruct_elf_nlaccess(). This frees
        the elf internals record created in
        _dwarf_elf_object_access_internals_init()
        in case of errors during setup or when
        dwarf_finish() is called.  Works safely for
        partially or fully set-up elf internals record.

    Other than in _dwarf_elf_nlsetup() the elf code
    knows nothing about Dwarf_Debug, and the rest of
    libdwarf knows nothing about the content of the
    object-type-specific (for Elf here)
    internals record.
*/

#include <config.h>

#include <stddef.h> /* size_t */
#include <stdlib.h> /* free() malloc() */
#ifdef HAVE_UNISTD_H
#include <unistd.h> /* sysconf */
#endif /* HAVE_UNISTD_H */
#ifdef HAVE_FULL_MMAP
#include <sys/mman.h> /* mmap */
#endif /* HAVE_FULL_MMAP */
#include <stdio.h> /* debug printf */
#include <string.h> /* memset() strdup() strncmp() */

#include "dwarf.h"
#include "libdwarf.h"
#include "dwarf_local_malloc.h"
#include "libdwarf_private.h"
#include "dwarf_base_types.h"
#include "dwarf_opaque.h"
#include "dwarf_error.h" /* for _dwarf_error() declaration */
#include "dwarf_reading.h"
#include "dwarf_memcpy_swap.h"
#include "dwarf_object_read_common.h"
#include "dwarf_object_detector.h"
#include "dwarf_elfstructs.h"
#include "dwarf_elf_defines.h"
#include "dwarf_elf_rel_detector.h"
#include "dwarf_elfread.h"

#ifndef TYP
#define TYP(n,l) char (n)[(l)]
#endif /* TYPE */

#ifdef WORDS_BIGENDIAN
#define READ_UNALIGNED_SAFE(dbg,dest, source, length)  \
    do {                                               \
        Dwarf_Unsigned _ltmp = 0;                      \
        (dbg)->de_copy_word( (((char *)(&_ltmp)) +     \
            sizeof(_ltmp) - length),(source),(length)); \
        dest = _ltmp;                                  \
    } while (0)

#define WRITE_UNALIGNED_LOCAL(dbg,dest,source, srclength,len_out) \
    {                                           \
        (dbg)->de_copy_word((dest),             \
            ((char *)(source)) +(srclength)-(len_out),  \
            (len_out)) ;                         \
    }
#else /* LITTLE ENDIAN */
#define READ_UNALIGNED_SAFE(dbg,dest, source, srclength) \
    do  {                                     \
        Dwarf_Unsigned _ltmp = 0;             \
        dbg->de_copy_word( (char *)(&_ltmp),  \
            (source), (srclength)) ;          \
        dest = _ltmp;                         \
    } while (0)

#define WRITE_UNALIGNED_LOCAL(dbg,dest,source, srclength,len_out) \
    {                               \
        dbg->de_copy_word( (dest) , \
            ((char *)(source)) ,    \
            (len_out)) ;            \
    }
#endif /* *-ENDIAN */

static int
_dwarf_elf_object_access_init(
    int  fd,
    unsigned ftype,
    unsigned endian,
    unsigned offsetsize,
    size_t filesize,
    Dwarf_Obj_Access_Interface_a **binary_interface,
    int *localerrnum);

static Dwarf_Small elf_get_nolibelf_byte_order (void *obj)
{
    dwarf_elf_object_access_internals_t *elf =
        (dwarf_elf_object_access_internals_t*)(obj);
    return (Dwarf_Small)elf->f_endian;
}

static Dwarf_Small elf_get_nolibelf_length_size (void *obj)
{
    dwarf_elf_object_access_internals_t *elf =
        (dwarf_elf_object_access_internals_t*)(obj);
    return elf->f_offsetsize/8;
}

static Dwarf_Small elf_get_nolibelf_pointer_size (void *obj)
{
    dwarf_elf_object_access_internals_t *elf =
        (dwarf_elf_object_access_internals_t*)(obj);
    return elf->f_pointersize/8;
}

static Dwarf_Unsigned elf_get_nolibelf_file_size(void *obj)
{
    dwarf_elf_object_access_internals_t *elf =
        (dwarf_elf_object_access_internals_t*)(obj);
    return elf->f_filesize;
}

static Dwarf_Unsigned elf_get_nolibelf_section_count (void *obj)
{
    dwarf_elf_object_access_internals_t *elf =
        (dwarf_elf_object_access_internals_t*)(obj);
    return elf->f_loc_shdr.g_count;
}

static int elf_get_nolibelf_section_info(void *obj,
    Dwarf_Unsigned section_index,
    Dwarf_Obj_Access_Section_a *return_section,
    int *error)
{
    dwarf_elf_object_access_internals_t *elf =
        (dwarf_elf_object_access_internals_t*)(obj);

    (void)error;
    if (section_index < elf->f_loc_shdr.g_count) {
        struct generic_shdr *sp = 0;

        sp = elf->f_shdr + section_index;
        return_section->as_addr      = sp->gh_addr;
        return_section->as_type      = sp->gh_type;
        return_section->as_size      = sp->gh_size;
        return_section->as_name      = sp->gh_namestring;
        return_section->as_link      = sp->gh_link;
        return_section->as_info      = sp->gh_info;
        return_section->as_flags     = sp->gh_flags;
        return_section->as_entrysize = sp->gh_entsize;
        return_section->as_offset    = sp->gh_offset;
        return DW_DLV_OK;
    }
    return DW_DLV_NO_ENTRY;
}

/*  This interface does not support mmap. It is malloc only */
static int
elf_load_nolibelf_section (void *obj, Dwarf_Unsigned section_index,
    Dwarf_Small **return_data, int *errorc)
{
    /*  Linux kernel read size limit 0x7ffff000,
        Without any good reason, limit our reads
        to a bit less. */
    const Dwarf_Unsigned read_size_limit = 0x7ff00000;
    Dwarf_Unsigned read_offset = 0;
    Dwarf_Unsigned read_size = 0;
    Dwarf_Unsigned remaining_bytes = 0;
    Dwarf_Small *  read_target = 0;
    dwarf_elf_object_access_internals_t *elf =
        (dwarf_elf_object_access_internals_t*)(obj);

    if (0 < section_index &&
        section_index < elf->f_loc_shdr.g_count) {
        int res = 0;

        struct generic_shdr *sp =
            elf->f_shdr + section_index;
        if (sp->gh_content) {
            *return_data = (Dwarf_Small *)sp->gh_content;
            return DW_DLV_OK;
        }
        if (!sp->gh_size) {
            return DW_DLV_NO_ENTRY;
        }
        /*  Guarding against bad values and
            against overflow */
        if (sp->gh_size > elf->f_filesize ||
            sp->gh_offset > elf->f_filesize ||
            (sp->gh_size + sp->gh_offset) >
                elf->f_filesize) {
            *errorc = DW_DLE_ELF_SECTION_ERROR;
            return DW_DLV_ERROR;
        }
        sp->gh_load_type = Dwarf_Alloc_Malloc;
        sp->gh_content = malloc((size_t)sp->gh_size);
        if (!sp->gh_content) {
            *errorc = DW_DLE_ALLOC_FAIL;
            return DW_DLV_ERROR;
        }
        /*  Linux has a 2GB limit on read size.
            So break this into 2gb pieces.  */
        remaining_bytes = sp->gh_size;
        read_size = remaining_bytes;
        read_offset = sp->gh_offset;
        read_target = (Dwarf_Small*)sp->gh_content;
        for ( ; remaining_bytes > 0; read_size = remaining_bytes ) {
            if (read_size > read_size_limit) {
                read_size = read_size_limit;
            }
            res = RRMOA(elf->f_fd,
                (void *)read_target, read_offset,
                read_size,
                elf->f_filesize, errorc);
            if (res != DW_DLV_OK) {
                free(sp->gh_content);
                sp->gh_content = 0;
                return res;
            }
            remaining_bytes -= read_size;
            read_offset += read_size;
            read_target += read_size;
        }
        sp->gh_was_alloc = TRUE;
        sp->gh_load_type = Dwarf_Alloc_Malloc;
        *return_data = (Dwarf_Small *)sp->gh_content;
        return DW_DLV_OK;
    }
    return DW_DLV_NO_ENTRY;
}

#ifdef HAVE_FULL_MMAP
/*  Calls elf_load_nolibelf_section() if
    malloc  is preferred. */
static int
elf_load_nolibelf_section_a (void* obj,
    Dwarf_Unsigned    dw_section_index,
    enum Dwarf_Sec_Alloc_Pref *dw_alloc_type,
    Dwarf_Small   **return_data_ptr,
    Dwarf_Unsigned *return_data_len,
    Dwarf_Small   **return_mmap_base_ptr,
    Dwarf_Unsigned *return_mmap_offset,
    Dwarf_Unsigned *return_mmap_len,
    int            *errc)
{
    int res  = 0;
    enum Dwarf_Sec_Alloc_Pref alloc_type_in = *dw_alloc_type;
    switch (alloc_type_in) {
    case Dwarf_Alloc_Malloc: {
        /* Does NOT alter *return_data_len */
        res = elf_load_nolibelf_section(obj,dw_section_index,
            return_data_ptr,errc);
        *return_mmap_base_ptr = 0;
        *return_mmap_offset = 0;
        *return_mmap_len = 0;
        *dw_alloc_type = Dwarf_Alloc_Malloc;
        /* *return_data_len =  not set */
        return res;
    }
    case Dwarf_Alloc_Mmap: {
        Dwarf_Unsigned secoffset = 0;
        Dwarf_Unsigned seclen = 0;
        Dwarf_Small   *realarea = (void*)-1;
        Dwarf_Unsigned computed_mmaplen = 0;
        Dwarf_Unsigned computed_mmapend = 0;
        long           pagesize = sysconf(_SC_PAGESIZE);
        Dwarf_Unsigned upagesize = 0;
        Dwarf_Unsigned pagesizebits = 0;
        Dwarf_Unsigned pageoff = 0;
        dwarf_elf_object_access_internals_t *elf =
            (dwarf_elf_object_access_internals_t*)(obj);
        void *         mmptr = 0;

        /*  pagesize is guaranteed to be a multiple of 2,
            and will be >= 512 and is usually 4096.
            this helps coverityscan know that sutracting one
            from pagesize will not result in an
            anomalous number. */
        if (pagesize < 200L || pagesize > (128L*1024L*1024L)) {
            /*  verifying the value of pagesize to help fix
                coverity scan CID  531843 */
            *errc = DW_DLE_SYSCONF_VALUE_UNUSABLE;
            return DW_DLV_ERROR;
        }
        upagesize = (Dwarf_Unsigned)pagesize;
        pagesizebits = upagesize -1;
        if (0 < dw_section_index &&
            dw_section_index < elf->f_loc_shdr.g_count) {
            Dwarf_Unsigned pageadjust = 0;

            struct generic_shdr *sp =
                elf->f_shdr + dw_section_index;
            if (sp->gh_content) {
                *return_data_ptr = (Dwarf_Small *)sp->gh_content;
                return DW_DLV_OK;
            }
            seclen = sp->gh_size;
            if (!seclen) {
                return DW_DLV_NO_ENTRY;
            }
            /*  Guarding against bad values and
                against overflow */
            if (sp->gh_size > elf->f_filesize ||
                sp->gh_offset > elf->f_filesize ||
                (sp->gh_size + sp->gh_offset) >
                    elf->f_filesize) {
                *errc = DW_DLE_ELF_SECTION_ERROR;
                return DW_DLV_ERROR;
            }
            secoffset = sp->gh_offset;
            pageoff = secoffset & ~pagesizebits;
            /*  coverity scan CID 581843. Guarding
                against possible overflow complaint
                in computing computed_mmaplen. */
            computed_mmaplen = seclen;
            pageadjust = secoffset - pageoff;
            computed_mmaplen += pageadjust;
            if (computed_mmaplen > elf->f_filesize) {
                *errc = DW_DLE_ELF_SECTION_ERROR;
                return DW_DLV_ERROR;
            }
            computed_mmaplen += pagesizebits;
            if (computed_mmaplen > elf->f_filesize) {
                *errc = DW_DLE_ELF_SECTION_ERROR;
                return DW_DLV_ERROR;
            }
            computed_mmaplen &= ~pagesizebits;
            if (computed_mmaplen > elf->f_filesize) {
                /*  impossible */
                *errc = DW_DLE_ELF_SECTION_ERROR;
                return DW_DLV_ERROR;
            }
            if (computed_mmaplen < seclen) {
                /*  unsigned arith overflowed? */
                *errc = DW_DLE_ELF_SECTION_ERROR;
                return DW_DLV_ERROR;
            }
            computed_mmapend = computed_mmaplen+pageoff;
            /*  mmap tiny is formally ok, but since we
                are doing mmap per_section we do not
                want overlaps with other mmap.
                Overlap seems to fail. */
            if (seclen < (Dwarf_Unsigned)(4096*2) ||
                computed_mmaplen >= elf->f_filesize ||
                computed_mmapend >= elf->f_filesize ||
                /* overflow likely? */
                computed_mmaplen < seclen) {
                /* Does NOT alter *return_data_len */
                res = elf_load_nolibelf_section(obj,
                    dw_section_index,
                    return_data_ptr,errc);
                *return_mmap_base_ptr = 0;
                *return_mmap_offset = 0;
                *return_mmap_len = 0;
                *dw_alloc_type = Dwarf_Alloc_Malloc;
                /* *return_data_len =  not set */
                return res;
            }
            /*  Coverity Scan CID 531843. Possible overflow
                computing computed_mmaplen.  This is
                a false positive,  Marked as such
                in coverity scan 16 July 2025. */
            mmptr = mmap(0, (size_t)computed_mmaplen,
                PROT_READ|PROT_WRITE, MAP_PRIVATE,
                elf->f_fd,(off_t)pageoff);
            if (mmptr == (void *)-1) {
                *errc = DW_DLE_ELF_SECTION_ERROR;
                return DW_DLV_ERROR;
            }
            sp->gh_load_type = Dwarf_Alloc_Mmap;
            realarea = (Dwarf_Small*)mmptr;
            sp->gh_mmap_realarea = (char*)realarea;
            sp->gh_computed_mmaplen = computed_mmaplen;
            sp->gh_content   = (char *)realarea +
                (secoffset&pagesizebits);
            sp->gh_was_alloc = TRUE;
            *return_data_ptr = (Dwarf_Small *)sp->gh_content;
            *dw_alloc_type =  sp->gh_load_type;
            *return_data_len = seclen;
            *return_mmap_base_ptr = realarea;
            *return_mmap_offset = pageoff;
            *return_mmap_len = computed_mmaplen;
            return DW_DLV_OK;
        }
        return DW_DLV_NO_ENTRY;
    }
    break;
    case Dwarf_Alloc_None:
    default:
    break;
    } /* end switch */
    *errc = DW_DLE_ELF_SECTION_ERROR;
    return DW_DLV_ERROR;
}
#endif /* HAVE_FULL_MMAP */

#define MATCH_REL_SEC(i_,s_,r_)  \
if ((i_) == (s_).dss_index) { \
    *(r_) = &(s_);            \
    return DW_DLV_OK;    \
}

static int
find_section_to_relocate(Dwarf_Debug dbg,Dwarf_Unsigned section_index,
    struct Dwarf_Section_s **relocatablesec, int *errorc)
{
    MATCH_REL_SEC(section_index,dbg->de_debug_info,relocatablesec);
    MATCH_REL_SEC(section_index,dbg->de_debug_abbrev,relocatablesec);
    MATCH_REL_SEC(section_index,dbg->de_debug_line,relocatablesec);
    MATCH_REL_SEC(section_index,dbg->de_debug_loc,relocatablesec);
    MATCH_REL_SEC(section_index,dbg->de_debug_aranges,relocatablesec);
    MATCH_REL_SEC(section_index,dbg->de_debug_macinfo,relocatablesec);
    MATCH_REL_SEC(section_index,dbg->de_debug_pubnames,
        relocatablesec);
    MATCH_REL_SEC(section_index,dbg->de_debug_names,
        relocatablesec);
    MATCH_REL_SEC(section_index,dbg->de_debug_ranges,relocatablesec);
    MATCH_REL_SEC(section_index,dbg->de_debug_frame,
        relocatablesec);
    MATCH_REL_SEC(section_index,dbg->de_debug_frame_eh_gnu,
        relocatablesec);
    MATCH_REL_SEC(section_index,dbg->de_debug_pubtypes,
        relocatablesec);
    MATCH_REL_SEC(section_index,dbg->de_debug_funcnames,
        relocatablesec);
    MATCH_REL_SEC(section_index,dbg->de_debug_typenames,
        relocatablesec);
    MATCH_REL_SEC(section_index,dbg->de_debug_varnames,
        relocatablesec);
    MATCH_REL_SEC(section_index,dbg->de_debug_weaknames,
        relocatablesec);
    MATCH_REL_SEC(section_index,dbg->de_debug_types,relocatablesec);
    MATCH_REL_SEC(section_index,dbg->de_debug_macro,relocatablesec);
    MATCH_REL_SEC(section_index,dbg->de_debug_rnglists,
        relocatablesec);
    MATCH_REL_SEC(section_index,dbg->de_debug_loclists,
        relocatablesec);
    MATCH_REL_SEC(section_index,dbg->de_debug_aranges,
        relocatablesec);
    MATCH_REL_SEC(section_index,dbg->de_debug_sup,
        relocatablesec);
    MATCH_REL_SEC(section_index,dbg->de_debug_str_offsets,
        relocatablesec);
    MATCH_REL_SEC(section_index,dbg->de_debug_addr,relocatablesec);
    MATCH_REL_SEC(section_index,dbg->de_debug_gnu_pubnames,
        relocatablesec);
    MATCH_REL_SEC(section_index,dbg->de_debug_gnu_pubtypes,
        relocatablesec);
    /* dbg-> de_debug_tu_index,reloctablesec); */
    /* dbg-> de_debug_cu_index,reloctablesec); */
    /* dbg-> de_debug_gdbindex,reloctablesec); */
    /* dbg-> de_debug_str,syms); */
    /* de_elf_symtab,syms); */
    /* de_elf_strtab,syms); */
    *errorc = DW_DLE_RELOC_SECTION_MISMATCH;
    return DW_DLV_ERROR;
}

/*  Returns DW_DLV_OK if it works, else DW_DLV_ERROR.
    The caller may decide to ignore the errors or report them. */
static int
update_entry(Dwarf_Debug dbg,
    dwarf_elf_object_access_internals_t*obj,
    struct generic_rela *rela,
    Dwarf_Small *target_section,
    Dwarf_Unsigned target_section_size,
    int *errorc)
{
    unsigned int type = 0;
    unsigned int sym_idx = 0;
    Dwarf_Unsigned offset = 0;
    Dwarf_Signed addend = 0;
    Dwarf_Half reloc_size = 0;
    Dwarf_Half machine  = (Dwarf_Half)obj->f_machine;
    struct generic_symentry *symp = 0;
    int is_rela = rela->gr_is_rela;

    offset = rela->gr_offset;
    addend = rela->gr_addend;
    type = (unsigned int)rela->gr_type;
    sym_idx = (unsigned int)rela->gr_sym;
    if (sym_idx >= obj->f_loc_symtab.g_count) {
        *errorc = DW_DLE_RELOC_SECTION_SYMBOL_INDEX_BAD;
        return DW_DLV_ERROR;
    }
    symp = obj->f_symtab + sym_idx;
    if (offset >= target_section_size) {
        /*  If offset really big, any add will overflow.
            So lets stop early if offset is corrupt. */
        *errorc = DW_DLE_RELOC_INVALID;
        return DW_DLV_ERROR;
    }
    /* Determine relocation size */
    if (_dwarf_is_32bit_abs_reloc(type, machine)) {
        reloc_size = 4;
    } else if (_dwarf_is_64bit_abs_reloc(type, machine)) {
        reloc_size = 8;
    } else if (!type) {
        /*  There is nothing to do. , this is the case such as
            R_AARCH64_NONE and R_X86_64_NONE and the other machine
            cases have it too. Most object files do not have
            any relocation records of type R_<machine>_NONE.  */
        return DW_DLV_OK;
    } else {
        *errorc = DW_DLE_RELOC_SECTION_RELOC_TARGET_SIZE_UNKNOWN;
        return DW_DLV_ERROR;
    }
    if ( (offset + reloc_size) < offset) {
        /* Another check for overflow. */
        *errorc = DW_DLE_RELOC_INVALID;
        return DW_DLV_ERROR;
    }
    if ( (offset + reloc_size) > target_section_size) {
        *errorc = DW_DLE_RELOC_INVALID;
        return DW_DLV_ERROR;
    }
    /*  Assuming we do not need to do a READ_UNALIGNED here
        at target_section + offset and add its value to
        outval.  Some ABIs say no read (for example MIPS),
        but if some do then which ones? */
    {   /* .rel. (addend is 0), or .rela. */
        Dwarf_Small *targ = target_section+offset;
        Dwarf_Unsigned presentval = 0;
        Dwarf_Unsigned outval = 0;

        if (!is_rela) {
            READ_UNALIGNED_SAFE(dbg,presentval,
                targ,(unsigned long)reloc_size);
        }
        /*  There is no addend in .rel.
            Normally presentval is correct
            and st_value will be zero.
            But a few compilers have
            presentval zero and st_value set. */
        outval = presentval + symp->gs_value + addend;
        WRITE_UNALIGNED_LOCAL(dbg,targ,
            &outval,sizeof(outval),(unsigned long)reloc_size);
    }
    return DW_DLV_OK;
}

/*  Somewhat arbitrarily, we attempt to apply all the
    relocations we can
    and still notify the caller of at least one error if we found
    any errors.  */

static int
apply_rela_entries(
    Dwarf_Debug dbg,
    /* Section_index of the relocation section, .rela entries  */
    Dwarf_Unsigned r_section_index,
    dwarf_elf_object_access_internals_t*obj,
    /* relocatablesec is the .debug_info(etc)  in Dwarf_Debug */
    struct Dwarf_Section_s * relocatablesec,
    int *errorc)
{
    int return_res = DW_DLV_OK;
    struct generic_shdr * rels_shp = 0;
    Dwarf_Unsigned relcount;
    Dwarf_Unsigned i = 0;

    if (r_section_index >= obj->f_loc_shdr.g_count) {
        *errorc = DW_DLE_SECTION_INDEX_BAD;
        return DW_DLV_ERROR;
    }
    rels_shp = obj->f_shdr + r_section_index;
    relcount = rels_shp->gh_relcount;
    if (obj->f_ehdr->ge_type != ET_REL) {
        /* No relocations to do */
        return DW_DLV_OK;
    }
    if (!relcount) {
        /*  Nothing to do. */
        return DW_DLV_OK;
    }
    if (!rels_shp->gh_rels) {
        /*  something wrong. */
        *errorc = DW_DLE_RELOCS_ERROR;
        return DW_DLV_ERROR;
    }
    for (i = 0; i < relcount; i++) {
        int res = update_entry(dbg,obj,
            rels_shp->gh_rels+i,
            relocatablesec->dss_data,
            relocatablesec->dss_size,
            errorc);
        if (res != DW_DLV_OK) {
            /* We try to keep going, not stop. */
            return_res = res;
        }
    }
    return return_res;
}

/*  Find the section data in dbg and find all the relevant
    sections.  Then do relocations.

    section_index is the index of a .debug_info (for example)
    so we have to find the section(s) with relocations
    targeting section_index.
    Normally there is exactly one such, though.
*/
static int
elf_relocations_nolibelf(void* obj_in,
    Dwarf_Unsigned section_index,
    Dwarf_Debug dbg,
    int* errorc)
{
    int res = DW_DLV_ERROR;
    dwarf_elf_object_access_internals_t*obj = 0;
    struct Dwarf_Section_s * relocatablesec = 0;
    Dwarf_Unsigned section_with_reloc_records = 0;

    if (section_index == 0) {
        return DW_DLV_NO_ENTRY;
    }
    obj = (dwarf_elf_object_access_internals_t*)obj_in;

    /*  The section to relocate must already be loaded into memory.
        This just turns section_index into a pointer
        to a de_debug_info or other  section record in
        Dwarf_Debug. */
    res = find_section_to_relocate(dbg, section_index,
        &relocatablesec, errorc);
    if (res != DW_DLV_OK) {
        return res;
    }
    /*  Now we know the  Dwarf_Section_s section
        we need to relocate.
        So lets find the rela section(s) targeting this.
    */

    /*  Sun and possibly others do not always set
        sh_link in .debug_* sections.
        So we cannot do full  consistency checks.
        FIXME: This approach assumes there is only one
        relocation section applying to section section_index! */
    section_with_reloc_records = relocatablesec->dss_reloc_index;
    if (!section_with_reloc_records) {
        /* Something is wrong. */
        *errorc = DW_DLE_RELOC_SECTION_MISSING_INDEX;
        return DW_DLV_ERROR;
    }
    /* The relocations, if they exist, have been loaded. */
    /* The symtab was already loaded. */
    if (!obj->f_symtab || !obj->f_symtab_sect_strings) {
        *errorc = DW_DLE_DEBUG_SYMTAB_ERR;
        return DW_DLV_ERROR;
    }
    if (obj->f_symtab_sect_index != relocatablesec->dss_reloc_link) {
        /* Something is wrong. */
        *errorc = DW_DLE_RELOC_MISMATCH_RELOC_INDEX;
        return DW_DLV_ERROR;
    }
    /* We have all the data we need in memory. */
    /*  Now we apply the relocs in section_with_reloc_records to the
        target, relocablesec */
    res = apply_rela_entries(dbg,
        section_with_reloc_records,
        obj, relocatablesec,errorc);
    return res;
}

static void
_dwarf_destruct_elf_nlaccess(void * obj)
{
    struct Dwarf_Obj_Access_Interface_a_s *aip =
        (struct Dwarf_Obj_Access_Interface_a_s *)obj;
    dwarf_elf_object_access_internals_t *ep = 0;
    struct generic_shdr *shp = 0;
    Dwarf_Unsigned shcount = 0;
    Dwarf_Unsigned i = 0;

    ep = (dwarf_elf_object_access_internals_t *)aip->ai_object;
    free(ep->f_ehdr);
    shp = ep->f_shdr;
    shcount = ep->f_loc_shdr.g_count;
    for (i = 0; i < shcount; ++i,++shp) {
        enum Dwarf_Sec_Alloc_Pref alloc = shp->gh_load_type;
        free(shp->gh_rels);
        shp->gh_rels = 0;
        switch(alloc) {
        case Dwarf_Alloc_Malloc:
            if (shp->gh_was_alloc) {
                free(shp->gh_content);
            }
            break;
#ifdef HAVE_FULL_MMAP
        case Dwarf_Alloc_Mmap: {
            if (shp->gh_was_alloc) {
                munmap(shp->gh_mmap_realarea,
                    (size_t)shp->gh_computed_mmaplen);
                /* If returned non-zero unmap failed */
            }
        } break;
#endif /* HAVE_FULL_MMAP */
        default: break;
            /*  something disastrously wrong. No free/mmap */
        } /* end switch on alloc */
        shp->gh_content = 0;
        shp->gh_mmap_realarea = (char *)-1;
        shp->gh_computed_mmaplen = 0;
        free(shp->gh_sht_group_array);
        shp->gh_sht_group_array = 0;
        shp->gh_sht_group_array_count = 0;
    }
    free(ep->f_shdr);
    ep->f_shdr = 0;
    ep->f_loc_shdr.g_count = 0;
    free(ep->f_phdr);
    ep->f_phdr = 0;
    free(ep->f_elf_shstrings_data);
    ep->f_elf_shstrings_data = 0;
    ep->f_elf_shstrings_max = 0;
    free(ep->f_dynamic);
    ep->f_dynamic = 0;
    free(ep->f_symtab_sect_strings);
    ep->f_symtab_sect_strings = 0;
    free(ep->f_dynsym_sect_strings);
    ep->f_dynsym_sect_strings = 0;
    free(ep->f_symtab);
    ep->f_symtab = 0;
    free(ep->f_dynsym);
    ep->f_dynsym = 0;

    /* if TRUE close f_fd on destruct.*/
    if (ep->f_destruct_close_fd) {
        _dwarf_closer(ep->f_fd);
    }
    ep->f_ident[0] = 'X';
    free(ep->f_path);
    ep->f_path = 0;
    free(ep);
    ep = 0;
    free(aip);
    aip = 0;
}

int
_dwarf_elf_nlsetup(int fd,
    char *true_path,
    unsigned ftype,
    unsigned endian,
    unsigned offsetsize,
    size_t filesize,
    unsigned groupnumber,
    Dwarf_Handler errhand,
    Dwarf_Ptr errarg,
    Dwarf_Debug *dbg,Dwarf_Error *error)
{
    Dwarf_Obj_Access_Interface_a *binary_interface = 0;
    dwarf_elf_object_access_internals_t *intfc = 0;
    int res = DW_DLV_OK;
    int localerrnum = 0;

    res = _dwarf_elf_object_access_init(
        fd,
        ftype,endian,offsetsize,filesize,
        &binary_interface,
        &localerrnum);
    if (res != DW_DLV_OK) {
        if (res == DW_DLV_NO_ENTRY) {
            return res;
        }
        _dwarf_error(NULL, error, localerrnum);
        return DW_DLV_ERROR;
    }
    /*  allocates and initializes Dwarf_Debug,
        generic code */
    res = dwarf_object_init_b(binary_interface, errhand, errarg,
        groupnumber, dbg, error);
    if (res != DW_DLV_OK){
        _dwarf_destruct_elf_nlaccess(binary_interface);
        return res;
    }
    intfc = binary_interface->ai_object;
    intfc->f_path = strdup(true_path);
    (*dbg)->de_obj_machine = intfc->f_machine;
    (*dbg)->de_obj_type = intfc->f_ftype; /* ET_REL etc */
    (*dbg)->de_obj_flags = intfc->f_flags;
    return res;
}

/*  dwarf_elf_access method table for use with non-libelf.
    See also the methods table in dwarf_elf_access.c for libelf.
*/
static Dwarf_Obj_Access_Methods_a const elf_nlmethods = {
    elf_get_nolibelf_section_info,
    elf_get_nolibelf_byte_order,
    elf_get_nolibelf_length_size,
    elf_get_nolibelf_pointer_size,
    elf_get_nolibelf_file_size,
    elf_get_nolibelf_section_count,
    elf_load_nolibelf_section,
    elf_relocations_nolibelf,
#ifdef HAVE_FULL_MMAP
    elf_load_nolibelf_section_a,
#else
    0 /* Not allowing mmap */,
#endif
    _dwarf_destruct_elf_nlaccess
};

/*  On any error this frees internals argument. */
static int
_dwarf_elf_object_access_internals_init(
    dwarf_elf_object_access_internals_t * internals,
    int  fd,
    unsigned ftype,
    unsigned endian,
    unsigned offsetsize,
    size_t filesize,
    int *errcode)
{
    dwarf_elf_object_access_internals_t * intfc = internals;
    Dwarf_Unsigned i  = 0;
    struct Dwarf_Obj_Access_Interface_a_s *localdoas;
    int res = 0;

    /*  Must malloc as _dwarf_destruct_elf_access()
        forces that due to other uses. */
    localdoas = (struct Dwarf_Obj_Access_Interface_a_s *)
        malloc(sizeof(struct Dwarf_Obj_Access_Interface_a_s));
    if (!localdoas) {
        free(internals);
        *errcode = DW_DLE_ALLOC_FAIL;
        return DW_DLV_ERROR;
    }
    memset(localdoas,0,sizeof(struct Dwarf_Obj_Access_Interface_a_s));
    /*  E is used with libelf. F with this elf reader. */
    intfc->f_ident[0]    = 'F';
    intfc->f_ident[1]    = '1';
    intfc->f_fd          = fd;
    intfc->f_is_64bit    = ((offsetsize==64)?TRUE:FALSE);
    intfc->f_offsetsize  = (Dwarf_Small)offsetsize;
    intfc->f_pointersize = (Dwarf_Small)offsetsize;
    intfc->f_filesize    = filesize;
    intfc->f_ftype       = ftype;
    intfc->f_destruct_close_fd = FALSE;

#ifdef WORDS_BIGENDIAN
    if (endian == DW_END_little ) {
        intfc->f_copy_word = _dwarf_memcpy_swap_bytes;
        intfc->f_endian = DW_END_little;
    } else {
        intfc->f_copy_word = _dwarf_memcpy_noswap_bytes;
        intfc->f_endian = DW_END_big;
    }
#else  /* LITTLE ENDIAN */
    if (endian == DW_END_little ) {
        intfc->f_copy_word = _dwarf_memcpy_noswap_bytes;
        intfc->f_endian = DW_END_little;
    } else {
        intfc->f_copy_word = _dwarf_memcpy_swap_bytes;
        intfc->f_endian = DW_END_big;
    }
#endif /* LITTLE- BIG-ENDIAN */
    /*  The following sets f_machine. */
    res = _dwarf_load_elf_header(intfc,errcode);
    if (res != DW_DLV_OK) {
        localdoas->ai_object = intfc;
        localdoas->ai_methods = 0;
        _dwarf_destruct_elf_nlaccess((void *)localdoas);
        localdoas = 0;
        return res;
    }
    /* Not loading progheaders */
    res = _dwarf_load_elf_sectheaders(intfc,errcode);
    if (res != DW_DLV_OK) {
        localdoas->ai_object = intfc;
        localdoas->ai_methods = 0;
        _dwarf_destruct_elf_nlaccess((void *)localdoas);
        localdoas = 0;
        return res;
    }
    /* We are not looking at symbol strings for now. */
    res = _dwarf_load_elf_symstr(intfc,errcode);
    if (res == DW_DLV_ERROR) {
        localdoas->ai_object = intfc;
        localdoas->ai_methods = 0;
        _dwarf_destruct_elf_nlaccess((void *)localdoas);
        localdoas = 0;
        return res;
    }
    res  = _dwarf_load_elf_symtab_symbols(intfc,errcode);
    if (res == DW_DLV_ERROR) {
        localdoas->ai_object = intfc;
        localdoas->ai_methods = 0;
        _dwarf_destruct_elf_nlaccess((void *)localdoas);
        localdoas = 0;
        return res;
    }
    for ( i = 1; i < intfc->f_loc_shdr.g_count; ++i) {
        struct generic_shdr *shp = 0;
        Dwarf_Unsigned section_type = 0;
        enum RelocRela localrel = RelocIsRela;

        shp = intfc->f_shdr +i;
        section_type = shp->gh_type;
        if (!shp->gh_namestring) {
            /*  A serious error which we ignore here
                as it will be caught elsewhere
                if necessary. */
            continue;
        } else if (section_type == SHT_REL) {
            localrel = RelocIsRel;
        } else if (section_type == SHT_RELA) {
            localrel = RelocIsRela;
        } else if (!strncmp(".rel.",shp->gh_namestring,5)) {
            localrel = RelocIsRel;
        } else if (!strncmp(".rela.",shp->gh_namestring,6)) {
            localrel = RelocIsRela;
        } else {
            continue;
        }
        /*  ASSERT: local rel is either RelocIsRel or
            RelocIsRela. Never any other value. */
        /*  Possibly we should check if the target section
            is one we care about before loading rela
            FIXME */
        res = _dwarf_load_elf_relx(intfc,i,localrel,errcode);
        if (res == DW_DLV_ERROR) {
            localdoas->ai_object = intfc;
            localdoas->ai_methods = 0;
            _dwarf_destruct_elf_nlaccess((void *)localdoas);
            localdoas = 0;
            return res;
        }
    }
    free(localdoas);
    localdoas = 0;
    return DW_DLV_OK;
}

static int
_dwarf_elf_object_access_init(
    int  fd,
    unsigned ftype,
    unsigned endian,
    unsigned offsetsize,
    size_t filesize,
    Dwarf_Obj_Access_Interface_a **binary_interface,
    int *localerrnum)
{

    int res = 0;
    dwarf_elf_object_access_internals_t *internals = 0;
    Dwarf_Obj_Access_Interface_a *intfc = 0;

    internals = malloc(sizeof(dwarf_elf_object_access_internals_t));
    if (!internals) {
        *localerrnum = DW_DLE_ALLOC_FAIL;
        /* Impossible case, we hope. Give up. */
        return DW_DLV_ERROR;
    }
    memset(internals,0,sizeof(*internals));
    res = _dwarf_elf_object_access_internals_init(internals,
        fd,
        ftype, endian, offsetsize, filesize,
        localerrnum);
    if (res != DW_DLV_OK){
        return res;
    }

    intfc = malloc(sizeof(Dwarf_Obj_Access_Interface_a));
    if (!intfc) {
        /* Impossible case, we hope. Give up. */
        free(internals);
        *localerrnum = DW_DLE_ALLOC_FAIL;
        return DW_DLV_ERROR;
    }
    memset(intfc,0,sizeof(*intfc));

    /* Initialize the interface struct */
    intfc->ai_object = internals;
    intfc->ai_methods = &elf_nlmethods;
    *binary_interface = intfc;
    return DW_DLV_OK;
}
