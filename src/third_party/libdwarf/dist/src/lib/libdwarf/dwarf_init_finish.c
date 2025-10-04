/*
  Copyright (C) 2000-2005 Silicon Graphics, Inc. All Rights Reserved.
  Portions Copyright (C) 2008-2010 Arxan Technologies, Inc. All Rights Reserved.
  Portions Copyright (C) 2009-2025 David Anderson. All Rights Reserved.
  Portions Copyright (C) 2010-2012 SN Systems Ltd. All Rights Reserved.

  This program is free software; you can redistribute it
  and/or modify it under the terms of version 2.1 of the
  GNU Lesser General Public License as published by the Free
  Software Foundation.

  This program is distributed in the hope that it would be
  useful, but WITHOUT ANY WARRANTY; without even the implied
  warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
  PURPOSE.

  Further, this software is distributed without any warranty
  that it is free of the rightful claim of any third person
  regarding infringement or the like.  Any license provided
  herein, whether implied or otherwise, applies only to this
  software file.  Patent licenses, if any, provided herein
  do not apply to combinations of this program with other
  software, or any other product whatsoever.

  You should have received a copy of the GNU Lesser General
  Public License along with this program; if not, write the
  Free Software Foundation, Inc., 51 Franklin Street - Fifth
  Floor, Boston MA 02110-1301, USA.

*/

#include <config.h>

#include <stdlib.h> /* calloc() free() */
#include <string.h> /* memset() strcmp() strncmp() strlen() */
#include <stdio.h> /* debugging */

#if defined(_WIN32) && defined(HAVE_STDAFX_H)
#include "stdafx.h"
#endif /* HAVE_STDAFX_H */

#include "dwarf.h"
#include "libdwarf.h"
#include "dwarf_local_malloc.h"
#include "libdwarf_private.h"
#include "dwarf_base_types.h"
#include "dwarf_opaque.h"
#include "dwarf_alloc.h"
#include "dwarf_error.h"
#include "dwarf_util.h"
#include "dwarf_memcpy_swap.h"
#include "dwarf_harmless.h"
#include "dwarf_string.h"
#include "dwarf_secname_ck.h"
#include "dwarf_setup_sections.h"

#ifdef HAVE_ZLIB_H
#include "zlib.h"
#endif
#ifdef HAVE_ZSTD_H
#include "zstd.h"
#endif

#ifndef ELFCOMPRESS_ZLIB
#define ELFCOMPRESS_ZLIB 1
#endif
#ifndef ELFCOMPRESS_ZSTD
#define ELFCOMPRESS_ZSTD 2
#endif

/*  If your mingw elf.h is missing SHT_RELA and you do not
    need SHT_RELA support
    this define should work for you.
    It is the elf value, hopefully it will
    not cause trouble. If does not work, try -1
    or something else
    and let us know what works.  */
#ifndef SHT_RELA
#define SHT_RELA 4
#endif
#ifndef SHT_REL
#define SHT_REL 9
# endif
/*  For COMDAT GROUPS. Guarantees we can compile. We hope. */
#ifndef SHT_GROUP
#define SHT_GROUP 17
#endif

#ifndef SHF_COMPRESSED
/*  This from ubuntu xenial. Is in top of trunk binutils
    as of February 2016. Elf Section Flag */
#define SHF_COMPRESSED (1 << 11)
#endif

/*#define DWARF_DEBUG_LOAD */
#ifdef DWARF_DEBUG_LOAD
static const char * _dwarf_pref_name(enum Dwarf_Sec_Alloc_Pref pref)
{
    switch(pref) {
    case Dwarf_Alloc_Malloc) {
        return "Dwarf_Alloc_Malloc";
    case Dwarf_Alloc_Mmap){
        return "Dwarf_Alloc_Mmap";
    case Dwarf_Alloc_None){
        return "Dwarf_Alloc_None";
    }
    return "Unknown. Error";
}

static void dump_load_data(const char *msg,
    Dwarf_Section sec,
    int line,
    const char *file)
{
    printf("Section Load of %s %s line %d file %s\n",
        sec->dss_name,msg,line,_dwarf_basename(file));
    printf("  preference      : %s\n",_dwarf_pref_name(
        sec->dss_load_preference));
    printf("  data            : %p\n",(void *)sec->dss_data);
    printf("  Actual load     : %s\n",_dwarf_pref_name(
        sec->dss_actual_load_type));
    printf("  secsize         : %lu\n",
        (unsigned long)sec->dss_size);
    printf("  did_decompress  : %u\n",sec->dss_did_decompress);
    printf("  mmap_len        : %lu\n",
        (unsigned long)sec->dss_computed_mmap_len);
    printf("  mmap_offset     : %lu\n",
        (unsigned long)sec->dss_computed_mmap_offset);
    printf("  mmap_data       : %p\n",
        (void *)sec->dss_mmap_realarea);
}

static void
validate_section_load_data(const char *msg,
    Dwarf_Section sec,
    int line,
    const char *file)
{
    enum Dwarf_Sec_Alloc_Pref pref = sec->dss_actual_load_type;
    Dwarf_Unsigned mmap_len = sec->dss_computed_mmap_len;

    dump_load_data(msg,sec,line,file);
    switch(pref) {
    case Dwarf_Alloc_Malloc: {
    } break;
    case Dwarf_Alloc_Mmap: {
        if (!mmap_len) {
            printf("FAIL load data mmap\n");
            /* debugging only */
            exit(1);
        }
    } break;
    case Dwarf_Alloc_None: {
    } break;
    default:
        dump_load_data(msg,sec,line,file);
        printf("FAIL load data mmap\n");
        /* debugging only */
        exit(1);
    } /* end switch */
}
#endif /* DWARF_DEBUG_LOAD  */

/* This static is copied to the dbg on dbg init
   so that the static need not be referenced at
   run time, preserving better locality of
   reference.
   Value is 0 means do the string check.
   Value non-zero means do not do the check.
*/
static Dwarf_Small _dwarf_assume_string_in_bounds;
static Dwarf_Small _dwarf_apply_relocs = 1;

/*  Call this after calling dwarf_init but before doing anything else.
    It applies to all objects, not just the current object.  */
int
dwarf_set_reloc_application(int apply)
{
    int oldval = _dwarf_apply_relocs;
    _dwarf_apply_relocs = (Dwarf_Small)apply;
    return oldval;
}

int
dwarf_set_stringcheck(int newval)
{
    int oldval = _dwarf_assume_string_in_bounds;

    _dwarf_assume_string_in_bounds = (Dwarf_Small)newval;
    return oldval;
}

/*  Unifies the basic duplicate/empty testing and section
    data setting to one place. */
static int
get_basic_section_data(Dwarf_Debug dbg,
    struct Dwarf_Section_s *secdata,
    struct Dwarf_Obj_Access_Section_a_s *doas,
    Dwarf_Unsigned section_index,
    unsigned group_number,
    Dwarf_Error* error,
    int duperr, int emptyerr )
{
    /*  There is an elf convention that section index 0  is reserved,
        and that section is always empty.
        Non-elf object formats must honor that by ensuring that
        (when they assign numbers to 'sections' or
        'section-like-things')
        they never assign a real section section-number
        0 to dss_index. */
    if (secdata->dss_index != 0) {
        DWARF_DBG_ERROR(dbg, duperr, DW_DLV_ERROR);
    }
    if (doas->as_size == 0) {
        /*  As of 2018 it seems impossible to detect
            (via dwarfdump) whether emptyerr has any
            practical effect, whether TRUE or FALSE.  */
        if (emptyerr == 0 ) {
            /*  Allow empty section. */
            return DW_DLV_OK;
        }
        /* Know no reason to allow section */
        DWARF_DBG_ERROR(dbg, emptyerr, DW_DLV_ERROR);
    }
    secdata->dss_index = section_index;
    secdata->dss_size  = doas->as_size;
    secdata->dss_group_number = group_number;
    secdata->dss_addr  = doas->as_addr;
    secdata->dss_link  = doas->as_link;
    secdata->dss_flags = doas->as_flags;
    if (secdata->dss_flags & SHF_COMPRESSED) {
        secdata->dss_shf_compressed = TRUE;
    }
    secdata->dss_entrysize = doas->as_entrysize;
    secdata->dss_addralign = doas->as_addralign;
    return DW_DLV_OK;
}

static void
add_relx_data_to_secdata( struct Dwarf_Section_s *secdata,
    struct Dwarf_Obj_Access_Section_a_s *doas,
    Dwarf_Unsigned section_index,
    int is_rela)
{
    secdata->dss_reloc_index = section_index;
    secdata->dss_reloc_size = doas->as_size;
    secdata->dss_reloc_entrysize = doas->as_entrysize;
    secdata->dss_reloc_addr = doas->as_addr;
    secdata->dss_reloc_symtab = doas->as_link;
    secdata->dss_reloc_link = doas->as_link;
    secdata->dss_is_rela = (char)is_rela;
}

#if 0 /* dump_bytes */
static void
dump_bytes(const char *msg,Dwarf_Small * start, long len)
{
    Dwarf_Small *end = start + len;
    Dwarf_Small *cur = start;

    printf("dump_bytes: %s ",msg);
    for (; cur < end; cur++) {
        printf("%02x",*cur);
    }
    printf("\n");
}

static int
all_sig8_bits_zero(Dwarf_Sig8 *val)
{
    unsigned u = 0;
    for ( ; u < sizeof(*val); ++u) {
        if (val->signature[u] != 0) {
            return FALSE;
        }
    }
    return TRUE;
}
#endif /*0*/

static int
is_section_name_known_already(Dwarf_Debug dbg, const char *scn_name)
{
    unsigned i = 0;
    for ( ; i < dbg->de_debug_sections_total_entries; ++i) {
        struct Dwarf_dbg_sect_s *section = &dbg->de_debug_sections[i];
        if (!strcmp(scn_name, section->ds_name)) {
            /*  The caller will declare this a duplicate, an error. */
            return DW_DLV_OK;
        }
    }
    /*  This is normal, we expect we've not accepted
        scn_name already. */
    return DW_DLV_NO_ENTRY;
}

/*  Given an Elf ptr, set up dbg with pointers
    to all the Dwarf data sections.
    Return NULL on error.

    This function is also responsible for determining
    whether the given object contains Dwarf information
    or not.  The test currently used is that it contains
    either a .debug_info or a .debug_frame section.  If
    not, it returns DW_DLV_NO_ENTRY causing dwarf_init() also to
    return DW_DLV_NO_ENTRY.  Earlier, we had thought of using only
    the presence/absence of .debug_info to test, but we
    added .debug_frame since there could be stripped objects
    that have only a .debug_frame section for exception
    processing.
    DW_DLV_NO_ENTRY or DW_DLV_OK or DW_DLV_ERROR

    This does not allow for section-groups in object files,
    for which many .debug_info (and other DWARF) sections may exist.

    We process. .rela (SHT_RELA) and .rel (SHT_REL)
    sections because with .rela the referencing section
    offset value is zero whereas with .rel the
    referencing section value is already correct for
    the object itself.  In other words, we do it because
    of the definition of .rela relocations in Elf.

    However!  In some cases clang emits  a .rel section (at least
    for .rel.debug_info) where symtab entries have an st_value
    that must be treated like an addend: the compiler did not
    bother to backpatch the DWARF information for these.
*/

/*  For an object file with an incorrect rela section name,
    readelf prints correct debug information,
    as the tool takes the section type instead
    of the section name. So check the
    section name but test section type. */
static int
is_a_relx_section(const char *scn_name,int type,int *is_rela)
{
    if (type == SHT_RELA) {
        *is_rela = TRUE;
        return TRUE;
    }
    if (_dwarf_startswith(scn_name,".rela.")) {
        *is_rela = TRUE;
        return TRUE;
    }
    if (_dwarf_startswith(scn_name,".rel.")) {
        *is_rela = FALSE;
        return TRUE;
    }
    if (type == SHT_REL) {
        *is_rela = FALSE;
        return TRUE;
    }
    *is_rela = FALSE;
    return FALSE;
}

/*  ASSERT: names like .debug_ or .zdebug_ never passed in here! */
static int
is_a_special_section_semi_dwarf(const char *scn_name)
{
    if (!strcmp(scn_name,".strtab") ||
        !strcmp(scn_name,".symtab")) {
        return TRUE;
    }
    /*  It's not one of these special sections referenced in
        the test. */
    return FALSE;
}

static int
this_section_dwarf_relevant(const char *scn_name,
    int type,
    int *is_rela)
{
    /* A small helper function for _dwarf_setup(). */
    if (_dwarf_startswith(scn_name, ".zdebug_") ||
        _dwarf_startswith(scn_name, ".debug_")) {
        /* standard debug */
        return TRUE;
    }
    if (_dwarf_ignorethissection(scn_name)) {
        return FALSE;
    }
    /* Now check if a special section could be
        in a section_group, but though seems unlikely. */
    if (!strcmp(scn_name, ".eh_frame")) {
        /*  This is not really a group related file, but
            it is harmless to consider it such. */
        return TRUE;
    }
    if (!strcmp(scn_name, ".gnu_debuglink")) {
        /*  This is not a group or DWARF related file, but
            it is useful for split dwarf. */
        return TRUE;
    }
    if (!strcmp(scn_name, ".note.gnu.build-id")) {
        /*  This is not a group or DWARF related file, but
            it is useful for split dwarf. */
        return TRUE;
    }
    if (!strcmp(scn_name, ".gdb_index")) {
        return TRUE;
    }
    if (is_a_special_section_semi_dwarf(scn_name)) {
        return TRUE;
    }
    if (is_a_relx_section(scn_name,type,is_rela)) {
        return TRUE;
    }
    /*  All sorts of sections are of no interest: .text
        .rel. and many others. */
    return FALSE;
}

/*  This assumes any non-Elf object files have no SHT_GROUP
    sections. So this code will not be invoked on non-Elf objects.
    One supposes this is unlikely to match any non-Elf
    version of COMDAT. */
static int
insert_sht_list_in_group_map(Dwarf_Debug dbg,
    struct Dwarf_Obj_Access_Section_a_s *doas,
    unsigned comdat_group_number,
    unsigned section_number,
    Dwarf_Unsigned section_count,
    struct Dwarf_Obj_Access_Interface_a_s * obj,
    unsigned *did_add_map,
    Dwarf_Error *error)
{
    struct Dwarf_Section_s secdata;
    Dwarf_Small * data = 0;
    int           res = 0;
    Dwarf_Small*  secend = 0;

    memset(&secdata,0,sizeof(secdata));
    secdata.dss_size =      doas->as_size;
    secdata.dss_entrysize = doas->as_entrysize;
    secdata.dss_group_number = 1; /* arbitrary. */
    secdata.dss_index     = section_number;
    secdata.dss_name      = ".group";
    secdata.dss_standard_name = ".group";
    secdata.dss_number = section_number;
    secdata.dss_ignore_reloc_group_sec = TRUE;
    res = _dwarf_load_section(dbg,&secdata,error);
    if (res != DW_DLV_OK) {
        _dwarf_malloc_section_free(&secdata);
        return res;
    }
    if (!secdata.dss_data) {
        _dwarf_error(dbg,error,DW_DLE_GROUP_INTERNAL_ERROR);
        return DW_DLV_ERROR;
    }
    if (doas->as_entrysize != 4) {
        _dwarf_malloc_section_free(&secdata);
        _dwarf_error(dbg,error,DW_DLE_GROUP_INTERNAL_ERROR);
        return DW_DLV_ERROR;
    }
    /*  So now pick up the data in dss_data.
        It is an array of 32 bit fields.
        Entry zero is just a constant 1.
        Each additional is a section number. */
    data = secdata.dss_data;
    secend = data + secdata.dss_size;
    {
        Dwarf_Unsigned i = 1;
        Dwarf_Unsigned count = doas->as_size/doas->as_entrysize;
        Dwarf_Unsigned  fval = 0;

        /*  The fields treatments with  regard
            to endianness is unclear.  In any case a single
            bit should be on, as 0x01000000
            without any endiannes swapping.
            Or so it seems given limited evidence.
            We read with length checking and allow the
            reader to byte swap and then fix things.
            At least one test case has big-endian
            data but little-endian SHT_GROUP data. */
        if ((data+DWARF_32BIT_SIZE) > secend) {
            /* Duplicates the check in READ_UNALIGNED_CK
                so we can free allocated memory bere. */
            _dwarf_malloc_section_free(&secdata);
            _dwarf_error(dbg,error,DW_DLE_GROUP_INTERNAL_ERROR);
            return DW_DLV_ERROR;
        }
        READ_UNALIGNED_CK(dbg,fval,Dwarf_Unsigned,
            data,
            DWARF_32BIT_SIZE,
            error,
            secend);
        if (fval != 1 && fval != 0x1000000) {
            /*  Could be corrupted elf object. */
            _dwarf_malloc_section_free(&secdata);
            _dwarf_error(dbg,error,DW_DLE_GROUP_INTERNAL_ERROR);
            return DW_DLV_ERROR;
        }

        data = data + doas->as_entrysize;
        for (i = 1 ; i < count ; ++i) {
            Dwarf_Unsigned  val = 0;

            if ((data+DWARF_32BIT_SIZE) > secend) {
                /* Duplicates the check in READ_UNALIGNED_CK
                    so we can free allocated memory bere. */
                _dwarf_malloc_section_free(&secdata);
                _dwarf_error(dbg,error,DW_DLE_GROUP_INTERNAL_ERROR);
                return DW_DLV_ERROR;
            }
            READ_UNALIGNED_CK(dbg,val,Dwarf_Unsigned,
                data,
                DWARF_32BIT_SIZE,
                error,
                secend);
            if (val > section_count) {
                /*  Might be confused endianness by
                    the compiler generating the SHT_GROUP.
                    This is pretty horrible. */
                Dwarf_Unsigned valr = 0;
                _dwarf_memcpy_swap_bytes(&valr,&val,
                    DWARF_32BIT_SIZE);
                if (valr > section_count) {
                    _dwarf_malloc_section_free(&secdata);
                    _dwarf_error(dbg,error,
                        DW_DLE_GROUP_INTERNAL_ERROR);
                    return DW_DLV_ERROR;
                }
                /* Ok. Yes, ugly. */
                val = valr;
            }
            {
                /*  Ensure this group entry DWARF relevant before
                    adding to group map */
                struct Dwarf_Obj_Access_Section_a_s doasx;
                int resx = DW_DLV_ERROR;
                int err = 0;
                int is_rela = FALSE;

                memset(&doasx,0,sizeof(doasx));
                resx = obj->ai_methods->
                    om_get_section_info(obj->ai_object,
                    val,
                    &doasx, &err);
                if (resx == DW_DLV_NO_ENTRY){
                    /*  Should we really ignore this? */
                    continue;
                }
                if (resx == DW_DLV_ERROR){
                    _dwarf_malloc_section_free(&secdata);
                    _dwarf_error(dbg,error,err);
                    return resx;
                }
                if (!this_section_dwarf_relevant(doasx.as_name,
                    (int)doasx.as_type,&is_rela) ) {
                    continue;
                }
                data += DWARF_32BIT_SIZE;
                *did_add_map = TRUE;
                res = _dwarf_insert_in_group_map(dbg,
                    (unsigned)comdat_group_number,
                    (unsigned)val,
                    doasx.as_name,
                    error);
                if (res != DW_DLV_OK) {
                    _dwarf_malloc_section_free(&secdata);
                    return res;
                }
            }
        }
    }
    _dwarf_malloc_section_free(&secdata);
    return DW_DLV_OK;
}

/*  Split dwarf CUs can be in an object with non-split
    or split may be in a separate object.
    If all in one object the default is to deal with group_number
    and ignore DW_GROUPNUMBER_DWO.
    If only .dwo the default is DW_GROUPNUMBER_DWO(2).
    Otherwise use DW_GROUP_NUMBER_BASE(1).

    If there are COMDAT SHT_GROUP sections, these
    are assigned group numbers 3-N as needed.

    At present this makes the assumption that COMDAT group
    (ie, SHT_GROUP) sections
    have lower section numbers than the sections COMDAT refers to.
    It is not clear whether this is guaranteed, COMDAT is not
    an official Elf thing and documentation is scarce.
    In the 1990's SGI folks and others formed a committee
    and attempted to get COMDAT and a feature allowing section
    numbers  greater than 16 bits into Elf, but there was no
    group that was able to approve such things.

    This is called once at dbg init  time.
*/

static int
determine_target_group(Dwarf_Unsigned section_count,
    struct Dwarf_Obj_Access_Interface_a_s * obj,
    unsigned *group_number_out,
    Dwarf_Debug dbg,
    Dwarf_Error *error)
{
    unsigned obj_section_index = 0;
    int found_group_one = 0;
    int found_group_two = 0;
    struct Dwarf_Group_Data_s *grp = 0;
    unsigned comdat_group_next = 3;
    unsigned lowest_comdat_groupnum = 0;

    grp = &dbg->de_groupnumbers;
    grp->gd_number_of_groups = 0;
    grp->gd_number_of_sections = (unsigned int)section_count;
    if (grp->gd_map) {
        _dwarf_error(dbg,error,DW_DLE_GROUP_INTERNAL_ERROR);
        return DW_DLV_OK;
    }
    for (obj_section_index = 0; obj_section_index < section_count;
        ++obj_section_index) {

        struct Dwarf_Obj_Access_Section_a_s doas;
        int res = DW_DLV_ERROR;
        int err = 0;
        const char *scn_name = 0;
        unsigned groupnumber = 0;
        unsigned mapgroupnumber = 0;
        int is_rela = FALSE;

        memset(&doas,0,sizeof(doas));
        res = obj->ai_methods->om_get_section_info(obj->ai_object,
            obj_section_index,
            &doas, &err);
        if (res == DW_DLV_NO_ENTRY){
            return res;
        }
        if (res == DW_DLV_ERROR){
            _dwarf_error(dbg, error,err);
            return res;
        }

        if (doas.as_type == SHT_GROUP) {
            /*  See assumptions in function comment above. */
            unsigned did_add_map = 0;
            /*  Add to our map. Here we
                are assuming SHT_GROUP records come first.
                Till proven wrong. */
            res = insert_sht_list_in_group_map(dbg,&doas,
                comdat_group_next,
                obj_section_index,
                section_count,
                obj,
                &did_add_map,error);
            if (res != DW_DLV_OK) {
                return res;
            }
            if (!lowest_comdat_groupnum) {
                lowest_comdat_groupnum = comdat_group_next;
            }
            if (did_add_map) {
                ++grp->gd_number_of_groups;
                ++comdat_group_next;
            }
            continue;
        }
        scn_name = doas.as_name;
        if (!this_section_dwarf_relevant(scn_name,
            (int)doas.as_type,
            &is_rela) ) {
            continue;
        }

        /*  Now at a 'normal' section, though we do not
            quite know what group it is. */

        res = _dwarf_section_get_target_group_from_map(dbg,
            obj_section_index,&groupnumber,error);
        if (res == DW_DLV_OK ) {
            /*  groupnumber is set. Fall through.
                All COMDAT group should get here. */
            mapgroupnumber = groupnumber;
        } else if (res == DW_DLV_ERROR) {
            return res;
        } else { /* DW_DLV_NO_ENTRY */
            /* Normal non-COMDAT. groupnumber is zero.  */
        }

        /* BUILDING_MAP.  See also BUILDING_SECTIONS, SETUP_SECTION */
        if (!groupnumber) {
            res =_dwarf_dwo_groupnumber_given_name(scn_name,
                &groupnumber);
            /* DW_DLV_ERROR impossible here. */
            if (res == DW_DLV_OK) {
                /* groupnumber set 2 */
            } else {
                /*  This is what it has to be.
                    .rela in here too.  */
                groupnumber = DW_GROUPNUMBER_BASE;
            }
        }
        if (is_a_relx_section(scn_name,(int)doas.as_type,
            &is_rela)) {
            continue;
        }

        /*  ASSERT: groupnumber non-zero now */
        if (!is_a_special_section_semi_dwarf(scn_name)) {
            if (mapgroupnumber) {
                /* Already in group map */
                continue;
            }
            /* !mapgroupnumber */
            res = _dwarf_insert_in_group_map(dbg,
                (unsigned)groupnumber,
                (unsigned)obj_section_index,
                scn_name,
                error);
            if (res != DW_DLV_OK) {
                return res;
            }
            if (groupnumber == 1) {
                found_group_one++;
            } else if (groupnumber == 2) {
                found_group_two++;
            } else { /* fall through to continue */ }
            continue;
        }
    }
    if (found_group_two) {
        ++grp->gd_number_of_groups;
    }
    if (found_group_one) {
        *group_number_out = DW_GROUPNUMBER_BASE;
        ++grp->gd_number_of_groups;
    } else {
        if (found_group_two) {
            *group_number_out = DW_GROUPNUMBER_DWO;
        } else {
            if (lowest_comdat_groupnum) {
                *group_number_out = lowest_comdat_groupnum;
            } else {
                *group_number_out = DW_GROUPNUMBER_BASE;
            }
        }
    }
    return DW_DLV_OK;
}

static int
_dwarf_setup(Dwarf_Debug dbg, Dwarf_Error * error)
{
    const char    *scn_name = 0;
    struct Dwarf_Obj_Access_Interface_a_s * obj = 0;
    int            resn = 0;
    struct Dwarf_Section_s **sections = 0;
    Dwarf_Small    endianness = 0;
    Dwarf_Unsigned section_count = 0;
    unsigned       default_group_number = 0;
    unsigned       foundDwarf = FALSE;
    Dwarf_Unsigned obj_section_index = 0;

    dbg->de_assume_string_in_bounds =
        _dwarf_assume_string_in_bounds;
    /* First make an arbitrary assumption. */
    dbg->de_copy_word = _dwarf_memcpy_noswap_bytes;
    obj = dbg->de_obj_file;
    endianness = obj->ai_methods->om_get_byte_order(obj->ai_object);
    /* Then adjust any changes we need. */
#ifdef WORDS_BIGENDIAN
    dbg->de_big_endian_object = 1;
    if (endianness == DW_END_little) {
        dbg->de_big_endian_object = 0;
        dbg->de_copy_word = _dwarf_memcpy_swap_bytes;
    }
#else /* little endian */
    dbg->de_big_endian_object = 0;
    if (endianness == DW_END_big ) {
        dbg->de_big_endian_object = 1;
        dbg->de_copy_word = _dwarf_memcpy_swap_bytes;
    }
#endif /* !WORDS_BIGENDIAN */

    /*  The following de_length_size is Not Too Significant.
        Only used one calculation, and an approximate one
        at that. */
    dbg->de_length_size = obj->ai_methods->
        om_get_length_size(obj->ai_object);
    dbg->de_pointer_size =
        obj->ai_methods->om_get_pointer_size(obj->ai_object);
    section_count = obj->ai_methods->
        om_get_section_count(obj->ai_object);
    resn = determine_target_group(section_count,obj,
        &default_group_number,dbg,error);
    if (resn == DW_DLV_ERROR) {
        return DW_DLV_ERROR;
    }
    if (dbg->de_groupnumber == DW_GROUPNUMBER_ANY) {
        dbg->de_groupnumber = default_group_number;
    }
    /*  Allocate space to record references to debug sections
        that can be referenced by RELA sections in
        the 'sh_info' field. */
    sections = (struct Dwarf_Section_s **)calloc(section_count + 1,
        sizeof(struct Dwarf_Section_s *));
    if (!sections) {
        /* Impossible case, we hope. Give up. */
        _dwarf_error(dbg, error, DW_DLE_SECTION_ERROR);
        return DW_DLV_ERROR;
    }

    /*  We can skip index 0 when considering ELF files, but not other
        object types.  Indeed regardless of the object type we should
        skip section 0 here.
        This is a convention.  We depend on it.
        Non-elf object access code should
        (in itself) understand we will index beginning at 1 and adjust
        itself to deal with this Elf convention.    Without this
        convention various parts of the code in this file won't
        work correctly.
        A dss_index of 0 must not be used, even though we start at 0
        here.  So the get_section_info() must adapt to the situation
        (the elf version does automatically as a result of Elf having
        a section zero with zero length and an empty name). */

    /* ASSERT: all group map entries set up. */

    for (obj_section_index = 0; obj_section_index < section_count;
        ++obj_section_index) {

        struct Dwarf_Obj_Access_Section_a_s doas;
        int res = DW_DLV_ERROR;
        int err = 0;
        unsigned groupnumber = 0;
        unsigned mapgroupnumber = 0;
        int is_rela = FALSE;

        res = _dwarf_section_get_target_group_from_map(dbg,
            (unsigned int)obj_section_index, &groupnumber,error);
        if (res == DW_DLV_OK ) {
            /* groupnumber is set. Fall through */
            mapgroupnumber = groupnumber;
        } else if (res == DW_DLV_ERROR) {
            free(sections);
            return res;
        } else { /* DW_DLV_NO_ENTRY */
            /* fall through, a BASE or DWO group, possibly */
        }
        memset(&doas,0,sizeof(doas));

        res = obj->ai_methods->om_get_section_info(obj->ai_object,
            obj_section_index,
            &doas, &err);
        if (res == DW_DLV_NO_ENTRY){
            free(sections);
            return res;
        }
        if (res == DW_DLV_ERROR){
            free(sections);
            DWARF_DBG_ERROR(dbg, err, DW_DLV_ERROR);
        }
        scn_name = doas.as_name;
        if (!groupnumber) {
            /* This finds dwo sections, group 2 */
            res = _dwarf_dwo_groupnumber_given_name(scn_name,
                &groupnumber);
            if (res == DW_DLV_NO_ENTRY) {
                /* No, must be group 1 */
                groupnumber = DW_GROUPNUMBER_BASE;
            }
        }
        if (!this_section_dwarf_relevant(scn_name,
            (int)doas.as_type,
            &is_rela) ) {
            continue;
        }
        if (!is_a_relx_section(scn_name,(int)doas.as_type,
            &is_rela)
            && !is_a_special_section_semi_dwarf(scn_name)) {
            /*  We do these actions only for group-related
                sections.  Do for .debug_info etc,
                never for .strtab or .rela.*
                We already tested for relevance, so that part
                is not news. */
            if (mapgroupnumber == dbg->de_groupnumber) {
                /*  OK. Mapped. Part of the group.. This will
                    catch the cases where there are versions of
                    a section in multiple COMDATs and in BASE
                    an DWO to get the right one */
            } else {
                /* This section not mapped into this group. */
                if (groupnumber == 1 && dbg->de_groupnumber > 2 &&
                    !_dwarf_section_in_group_by_name(dbg,scn_name,
                        dbg->de_groupnumber)) {
                    /* Load the section (but as group 1) */
                } else {
                    continue;
                }
            }
        }
        /* BUILDING_SECTIONS.  See also BUILDING_MAP, SETUP_SECTION */
        {
            /*  Build up the sections table and the
                de_debug* etc pointers in Dwarf_Debug. */
            struct Dwarf_dbg_sect_s *section = 0;
            int found_match = FALSE;

            res = is_section_name_known_already(dbg,scn_name);
            if (res == DW_DLV_OK) {
#if 0 /* Removed check for section duplication */
                /* DUPLICATE */
                DWARF_DBG_ERROR(dbg, DW_DLE_SECTION_DUPLICATION,
                    DW_DLV_ERROR);
                /* Metrowerks does this nonsense */
#endif
                continue;
            }
            if (res == DW_DLV_ERROR) {
                free(sections);
                DWARF_DBG_ERROR(dbg, err, DW_DLV_ERROR);
            }
            /* No entry: new-to-us section, the normal case. */
            res = _dwarf_enter_section_in_de_debug_sections_array(dbg,
                scn_name, obj_section_index, groupnumber,&err);
            if (res == DW_DLV_OK) {
                section = &dbg->de_debug_sections[
                    dbg->de_debug_sections_total_entries-1];
                res = get_basic_section_data(dbg,
                    section->ds_secdata, &doas,
                    obj_section_index,
                    groupnumber,
                    error,
                    section->ds_duperr,
                    section->ds_emptyerr);
                if (res != DW_DLV_OK) {
                    free(sections);
                    return res;
                }
                sections[obj_section_index] = section->ds_secdata;
                foundDwarf += section->ds_have_dwarf;
                found_match = TRUE;
                /*  Normal section set up.
                    Fall through. */
            } else if (res == DW_DLV_NO_ENTRY) {
                /*  We get here for relocation sections.
                    Fall through. */
            } else {
                free(sections);
                DWARF_DBG_ERROR(dbg, err, DW_DLV_ERROR);
            }

            if (!found_match) {
                /*  For an object file with incorrect rel[a]
                    section name, the 'readelf' tool,
                    prints correct debug information,
                    as the tool takes the section type instead
                    of the section name. If the current section
                    is a RELA one and the 'sh_info'
                    refers to a debug section, add the
                    relocation data. */
                if (is_a_relx_section(scn_name,
                    (int)doas.as_type, &is_rela)) {
                    if ( doas.as_info < section_count) {
                        if (sections[doas.as_info]) {
                            add_relx_data_to_secdata(
                                sections[doas.as_info],
                                &doas,
                                obj_section_index,is_rela);
                        }
                    } else {
                        /* Something is wrong with the ELF file. */
                        free(sections);
                        DWARF_DBG_ERROR(dbg, DW_DLE_ELF_SECT_ERR,
                            DW_DLV_ERROR);
                    }
                }
            }
            /* Fetch next section */
        }
    }

    /* Free table with section information. */
    free(sections);
    if (foundDwarf) {
        return DW_DLV_OK;
    }
    return DW_DLV_NO_ENTRY;
}

/*  There is one table per CU and one per TU, and each
    table refers to the associated other DWARF data
    for that CU or TU.
    See DW_SECT_*

    In DWARF4 the type units are in .debug_types
    In DWARF5 the type units are in .debug_info.
*/

static int
load_debugfission_tables(Dwarf_Debug dbg,Dwarf_Error *error)
{
    int i = 0;
    if (dbg->de_debug_cu_index.dss_size ==0 &&
        dbg->de_debug_tu_index.dss_size ==0) {
        /*  This is the normal case.
            No debug fission. Not a .dwp object. */
        return DW_DLV_NO_ENTRY;
    }

    for (i = 0; i < 2; ++i) {
        Dwarf_Xu_Index_Header xuptr = 0;
        struct Dwarf_Section_s* dwsect = 0;
        Dwarf_Unsigned version = 0;
        Dwarf_Unsigned number_of_cols /* L */ = 0;
        Dwarf_Unsigned number_of_CUs /* N */ = 0;
        Dwarf_Unsigned number_of_slots /* M */ = 0;
        const char *secname = 0;
        int res = 0;
        const char *type = 0;

        if (i == 0) {
            dwsect = &dbg->de_debug_cu_index;
            type = "cu";
        } else {
            dwsect = &dbg->de_debug_tu_index;
            type = "tu";
        }
        if ( !dwsect->dss_size ) {
            continue;
        }
        res = dwarf_get_xu_index_header(dbg,type,
            &xuptr,&version,&number_of_cols,
            &number_of_CUs,&number_of_slots,
            &secname,error);
        if (res == DW_DLV_NO_ENTRY) {
            continue;
        }
        if (res != DW_DLV_OK) {
            return res;
        }
        if (i == 0) {
            dbg->de_cu_hashindex_data = xuptr;
        } else {
            dbg->de_tu_hashindex_data = xuptr;
        }
    }
    return DW_DLV_OK;
}

/*
    Use a Dwarf_Obj_Access_Interface to kick things off.
    All other init routines eventually use this one.
    The returned Dwarf_Debug contains a copy of *obj
    the callers copy of *obj may be freed whenever the caller
    wishes.

    New March 2017. Enables dealing with DWARF5 split
    dwarf more fully.  */
int
dwarf_object_init_b(Dwarf_Obj_Access_Interface_a* obj,
    Dwarf_Handler errhand,
    Dwarf_Ptr errarg,
    unsigned groupnumber,
    Dwarf_Debug* ret_dbg,
    Dwarf_Error* error)
{
    Dwarf_Debug dbg = 0;
    int setup_result = DW_DLV_OK;
    Dwarf_Unsigned filesize = 0;

    if (!ret_dbg) {
        DWARF_DBG_ERROR(NULL,DW_DLE_DWARF_INIT_DBG_NULL,
            DW_DLV_ERROR);
    }
    /*  Non-null *ret_dbg will cause problems dealing with
        DW_DLV_ERROR */
    *ret_dbg = 0;
    filesize = obj->ai_methods->om_get_filesize(obj->ai_object);
    /*  Initializes  Dwarf_Debug struct and returns
        a pointer to that empty record.
        Filesize is to set up a sensible default hash tree
        size. */
    dbg = _dwarf_get_debug(filesize);
    if (IS_INVALID_DBG(dbg)) {
        dwarf_finish(dbg);
        dbg = 0;
        DWARF_DBG_ERROR(dbg, DW_DLE_DBG_ALLOC, DW_DLV_ERROR);
    }
    dbg->de_errhand = errhand;
    dbg->de_errarg = errarg;
    dbg->de_frame_rule_initial_value = DW_FRAME_REG_INITIAL_VALUE;
    dbg->de_frame_reg_rules_entry_count = DW_FRAME_LAST_REG_NUM;
    dbg->de_frame_cfa_col_number = DW_FRAME_CFA_COL3;
    dbg->de_frame_same_value_number = DW_FRAME_SAME_VAL;
    dbg->de_frame_undefined_value_number  = DW_FRAME_UNDEFINED_VAL;
    dbg->de_dbg = dbg;
    /*  See  dwarf_set_tied_dbg()  dwarf_get_tied_dbg()
        and comments in dwarf_opaque.h*/
    dbg->de_primary_dbg = dbg;
    dbg->de_secondary_dbg = 0;
    dbg->de_errors_dbg = dbg;

    dbg->de_obj_file = obj;
    dbg->de_filesize = filesize;
    dbg->de_groupnumber = groupnumber;
    setup_result = _dwarf_setup(dbg, error);
    if (setup_result == DW_DLV_OK) {
        int fission_result = load_debugfission_tables(dbg,error);
        /*  In most cases we get
            setup_result == DW_DLV_NO_ENTRY here
            as having debugfission (.dwp objects)
            is fairly rare. */
        if (fission_result == DW_DLV_ERROR) {
            /*  Something is very wrong. */
            setup_result = fission_result;
        }
        if (setup_result == DW_DLV_OK) {
            _dwarf_harmless_init(&dbg->de_harmless_errors,
                DW_HARMLESS_ERROR_CIRCULAR_LIST_DEFAULT_SIZE);
            *ret_dbg = dbg;
            /*  This is the normal return. */
            return setup_result;
        }
    }
    if (setup_result == DW_DLV_NO_ENTRY) {
        _dwarf_free_all_of_one_debug(dbg);
        dbg = 0;
        /*  ASSERT: _dwarf_free_all_of_one_debug() never returns
            DW_DLV_ERROR */
        return setup_result;
    }
    /*  An error of some sort. Report it as well as
        possible.
        ASSERT: setup_result == DW_DLV_ERROR
        here  */
    {
        Dwarf_Unsigned myerr = 0;
        dwarfstring msg;

        dwarfstring_constructor(&msg);
        /* We cannot use any _dwarf_setup()
            error here as
            we are freeing dbg, making that error (setup
            as part of dbg) stale.
            Hence we have to make a new error without a dbg.
            But error might be NULL and the init call
            error-handler function might be set.
        */
        if (error && *error) {
            /*  Preserve our _dwarf_setup error number, but
                this does not apply if error NULL. */
            /* *error safe */
            myerr = dwarf_errno(*error);
            /* *error safe */
            dwarfstring_append(&msg,dwarf_errmsg(*error));
            /*  deallocate the soon-stale error pointer. */
            dwarf_dealloc_error(dbg,*error);
            /* *error safe */
            *error = 0;
        }
        /*  The status we want to return  here is of _dwarf_setup,
            not of the  _dwarf_free_all_of_one_debug(dbg) call.
            So use a local status variable for the free.  */
        _dwarf_free_all_of_one_debug(dbg);
        dbg = 0;
        if (myerr) {
            /*  Use the _dwarf_setup error number.
                If error is NULL the following will issue
                a message on stderr, as without
                dbg there is no error-handler function.
                */
            _dwarf_error_string(dbg,error,myerr,
                dwarfstring_string(&msg));
            dwarfstring_destructor(&msg);
        } /* else return quietly, a serious error
            was already reported. */
    }
    return setup_result;
}

/*  A finish routine that is completely unaware of ELF.

    Frees all memory that was not previously freed by
    dwarf_dealloc.
    NEVER returns DW_DLV_ERROR;

    Aside from certain categories.  */
int
dwarf_object_finish(Dwarf_Debug dbg)
{
    int res = 0;
    /* do not use CHECK_DBG */
    _dwarf_harmless_cleanout(&dbg->de_harmless_errors);
    res = _dwarf_free_all_of_one_debug(dbg);
    /*  see dwarf_error.h dwarf_error.c  Relevant
        to trying and failing to open/read corrupt
        object files.  */
    return res;
}

#if defined(HAVE_ZLIB) && defined(HAVE_ZSTD)

#if 0 /* Dropping heuristic check. Not reliable. */
static int
check_uncompr_inflation(Dwarf_Debug dbg,
    Dwarf_Error *error,
    Dwarf_Unsigned uncompressed_len,
    Dwarf_Unsigned srclen,
    Dwarf_Unsigned max_inflated_len,
    const char *libname)
{
    char buf[100];

    buf[0] = 0;
    if (srclen > 50)  {
        /*  If srclen not super tiny lets check the following. */
        if (uncompressed_len < (srclen/2)) {
            dwarfstring m;

            dwarfstring_constructor_static(&m,buf,sizeof(buf));
            dwarfstring_append_printf_s(&m,
            /*  Violates the approximate invariant about
                compression not actually inflating. */
                "DW_DLE_ZLIB_UNCOMPRESS_ERROR:"
                " The %s compressed section  is"
                "absurdly small. Corrupt dwarf",(char *)libname);
            _dwarf_error_string(dbg, error,
                DW_DLE_ZLIB_UNCOMPRESS_ERROR,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
    }
    if (max_inflated_len < srclen) {
        /*  The calculation overflowed or compression
            inflated the data. */
        dwarfstring m;

        dwarfstring_constructor_static(&m,buf,sizeof(buf));
        dwarfstring_append_printf_s(&m,
            /*  Violates the approximate invariant about
                compression not actually inflating. */
            "DW_DLE_ZLIB_UNCOMPRESS_ERROR:"
            " The %s compressed section  is"
            " absurdly large so arithmentic overflow."
            " So corrupt dwarf",(char *)libname);
        _dwarf_error_string(dbg, error,
            DW_DLE_ZLIB_UNCOMPRESS_ERROR,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    if (uncompressed_len > max_inflated_len) {
        dwarfstring m;

        dwarfstring_constructor_static(&m,buf,sizeof(buf));
        /*  This has happened to a specific
            set of gcc options, though we have no
            test case with this issue. */
        dwarfstring_append_printf_s(&m,
            "DW_DLE_ZLIB_UNCOMPRESS_ERROR"
            " The %s compressed section ",(char *)libname);
        dwarfstring_append_printf_u(&m,
            "(length %u)",srclen);
        dwarfstring_append_printf_u(&m,
            " uncompresses to %u bytes which seems"
            " absurdly large given the input section.",
            uncompressed_len);
        _dwarf_error_string(dbg, error,
            DW_DLE_ZLIB_UNCOMPRESS_ERROR,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    return DW_DLV_OK;
}
#endif /* 0 */

/*  case 1:
    The input stream is assumed to contain
    the four letters
    ZLIB
    Followed by 8 bytes of the size of the
    uncompressed stream. Presented as
    a big-endian binary number.
    Following that is the stream to decompress.

    case 2,3:
    The section flag bit  SHF_COMPRESSED (1 << 11)
    must be set.
    we then do the equivalent of reading a
        Elf32_External_Chdr
    or
        Elf64_External_Chdr
    to get the type (which must be 1 (zlib) or 2 (zstd))
    and the decompressed_length.
    Then what follows the implicit Chdr is decompressed.

    */

#if 0 /* Dropping heuristic check. Not reliable. */
/*  ALLOWED_ZLIB_INFLATION is a heuristic, not necessarily right.
    The test case klingler2/compresseddebug.amd64 actually
    inflates about 8 times.  */
#define ALLOWED_ZLIB_INFLATION 32
#define ALLOWED_ZSTD_INFLATION 32
#endif /* 0 */

static int
do_decompress(Dwarf_Debug dbg,
    struct Dwarf_Section_s *section,
    Dwarf_Error * error)
{
    Dwarf_Small *basesrc = section->dss_data;
    Dwarf_Small *src = basesrc;
    Dwarf_Small *dest = 0;
    Dwarf_Unsigned destlen = 0;
    Dwarf_Unsigned srclen = section->dss_size;
    Dwarf_Unsigned flags = section->dss_flags;
    Dwarf_Small *endsection = 0;
    int zstdcompress = FALSE;
    Dwarf_Unsigned uncompressed_len = 0;

    endsection = basesrc + section->dss_size;
    if ((basesrc + 12) > endsection) {
        _dwarf_error_string(dbg, error,DW_DLE_ZLIB_SECTION_SHORT,
            "DW_DLE_ZLIB_SECTION_SHORT"
            "Section too short to be either zlib or zstd related");
        return DW_DLV_ERROR;
    }
    section->dss_compressed_length = srclen;
    if (!strncmp("ZLIB",(const char *)src,4)) {
        unsigned i = 0;
        unsigned l = 8;
        unsigned char *c = src+4;
        for ( ; i < l; ++i,c++) {
            uncompressed_len <<= 8;
            uncompressed_len += *c;
        }
        src = src + 12;
        srclen -= 12;
        section->dss_uncompressed_length = uncompressed_len;
        section->dss_ZLIB_compressed = TRUE;
    } else  if (flags & SHF_COMPRESSED) {
        /*  The prefix is a struct:
            unsigned int type; followed by pad if following are 64bit!
            size-of-target-address size
            size-of-target-address
        */
        Dwarf_Small *ptr    = (Dwarf_Small *)src;
        Dwarf_Unsigned type = 0;
        Dwarf_Unsigned size = 0;
        /* Dwarf_Unsigned addralign = 0; */
        unsigned fldsize    = dbg->de_pointer_size;
        unsigned structsize = 3* fldsize;
        READ_UNALIGNED_CK(dbg,type,Dwarf_Unsigned,ptr,
            DWARF_32BIT_SIZE,
            error,endsection);
        ptr += fldsize;
        READ_UNALIGNED_CK(dbg,size,Dwarf_Unsigned,ptr,fldsize,
            error,endsection);
        switch(type) {
        case ELFCOMPRESS_ZLIB:
            break;
        case ELFCOMPRESS_ZSTD:
            zstdcompress = TRUE;
            break;
        default: {
            char buf[100];
            dwarfstring m;

            dwarfstring_constructor_static(&m,buf,sizeof(buf));
            dwarfstring_append_printf_u(&m,
                "DW_DLE_ZDEBUG_INPUT_FORMAT_ODD"
                " The SHF_COMPRESSED type field is 0x%x, neither"
                " zlib (1) or zstd(2). Corrupt dwarf.", type);
            _dwarf_error_string(dbg, error,
                DW_DLE_ZDEBUG_INPUT_FORMAT_ODD,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
        }
        uncompressed_len = size;
        section->dss_uncompressed_length = uncompressed_len;
        src    += structsize;
        srclen -= structsize;
        section->dss_shf_compressed = TRUE;
    } else {
        _dwarf_error_string(dbg, error,
            DW_DLE_ZDEBUG_INPUT_FORMAT_ODD,
            "DW_DLE_ZDEBUG_INPUT_FORMAT_ODD"
            " The compressed section is not properly formatted");
        return DW_DLV_ERROR;
    }
#if 0 /* Dropping heuristic check. Not reliable. */
    /*  The heuristics are unreliable. Turned off now  */
    if (!zstdcompress) {
        /*  According to zlib.net zlib essentially never expands
            the data when compressing.  There is no statement
            about  any effective limit in the compression factor
            though we, here, assume  such a limit to check
            for sanity in the object file.
            These tests are heuristics.  */
        int res = 0;
        Dwarf_Unsigned max_inflated_len =
            srclen*ALLOWED_ZLIB_INFLATION;

        res = check_uncompr_inflation(dbg,
            error, uncompressed_len, srclen,max_inflated_len,
            "zlib");
        if (res != DW_DLV_OK) {
            return res;
        }
    }
    if (zstdcompress) {
        /*  According to zlib.net zlib essentially never expands
            the data when compressing.  There is no statement
            about  any effective limit in the compression factor
            though we, here, assume  such a limit to check
            for sanity in the object file.
            These tests are heuristics.  */
        int res = 0;
        Dwarf_Unsigned max_inflated_len =
            srclen*ALLOWED_ZSTD_INFLATION;
        res = check_uncompr_inflation(dbg,
            error, uncompressed_len, srclen,max_inflated_len,
            "zstd");
        if (res != DW_DLV_OK) {
            return res;
        }
    }
#endif /* 0 */
    if ((src +srclen) > endsection) {
        _dwarf_error_string(dbg, error,
            DW_DLE_ZLIB_SECTION_SHORT,
            "DW_DLE_ZDEBUG_ZLIB_SECTION_SHORT"
            " The zstd or zlib compressed section  is"
            " longer than the section"
            " length. So corrupt dwarf");
        return DW_DLV_ERROR;
    }
    destlen = uncompressed_len;
    dest = malloc(destlen);
    if (!dest) {
        _dwarf_error_string(dbg, error,
            DW_DLE_ALLOC_FAIL,
            "DW_DLE_ALLOC_FAIL"
            " The zstd or zlib uncompressed space"
            " malloc failed: out of memory");
        return DW_DLV_ERROR;
    }
    /*  uncompress is a zlib function. */
    if (!zstdcompress) {
        int res = 0;
        uLongf dlen = destlen;

        res = uncompress(dest,&dlen,src,srclen);
        if (res == Z_BUF_ERROR) {
            free(dest);
            DWARF_DBG_ERROR(dbg, DW_DLE_ZLIB_BUF_ERROR, DW_DLV_ERROR);
        } else if (res == Z_MEM_ERROR) {
            free(dest);
            DWARF_DBG_ERROR(dbg, DW_DLE_ALLOC_FAIL, DW_DLV_ERROR);
        } else if (res != Z_OK) {
            free(dest);
            /* Probably Z_DATA_ERROR. */
            DWARF_DBG_ERROR(dbg, DW_DLE_ZLIB_DATA_ERROR,
                DW_DLV_ERROR);
        }
    }
    if (zstdcompress) {
        size_t zsize =
            ZSTD_decompress(dest,destlen,src,srclen);
        if (zsize != destlen) {
            free(dest);
            _dwarf_error_string(dbg, error,
                DW_DLE_ZLIB_DATA_ERROR,
                "DW_DLE_ZLIB_DATA_ERROR"
                " The zstd ZSTD_decompress() failed.");
            return DW_DLV_ERROR;
        }
    }
    /* Z_OK */
    _dwarf_malloc_section_free(section);
    section->dss_data = dest;
    section->dss_size = destlen;
    section->dss_was_alloc= TRUE;
    section->dss_actual_load_type= Dwarf_Alloc_Malloc;
    section->dss_did_decompress = TRUE;
    return DW_DLV_OK;
}
#endif /* HAVE_ZLIB && HAVE_ZSTD */

/*  Load the ELF section with the specified index and set its
    dss_data pointer to the memory where it was loaded.
    This is problematic for mmap use, as more needs
    to be recorded in the section data to munmap.
*/
int
_dwarf_load_section(Dwarf_Debug dbg,
    Dwarf_Section section,
    Dwarf_Error  *error)
{
    int res  = DW_DLV_ERROR;
    struct Dwarf_Obj_Access_Interface_a_s *o = 0;
    int            errc = 0;
    Dwarf_Unsigned data_len = 0;
    Dwarf_Small   *mmap_real_area = 0;
    Dwarf_Unsigned mmap_offset = 0;
    Dwarf_Unsigned mmap_len = 0;
    Dwarf_Small   *data_ptr = 0;
    enum Dwarf_Sec_Alloc_Pref pref =
        _dwarf_determine_section_allocation_type();
    enum Dwarf_Sec_Alloc_Pref finaltype = pref;

    /* check to see if the section is already loaded */
    if (section->dss_data !=  NULL) {
        return DW_DLV_OK;
    }
    o = dbg->de_obj_file;
    /*  There is an elf convention that section index 0
        is reserved, and that section is always empty.
        Non-elf object formats must honor
        that by ensuring that (when they
        assign numbers to 'sections' or
        'section-like-things') they never
        assign a real section section-number
        0 to dss_index.

        There is also a convention for 'bss' that that section
        and its like sections have no data but do have a size.
        That is never true of DWARF sections  */
    data_len = section->dss_size;
#ifdef HAVE_FULL_MMAP
    if (o->ai_methods->om_load_section_a) {
        res = o->ai_methods->om_load_section_a(o->ai_object,
            section->dss_index,
            &finaltype,
            &data_ptr, &data_len,
            &mmap_real_area,&mmap_offset,&mmap_len,
            &errc);
    } else
#endif /* HAVE_FULL_MMAP */
    {
        if (o->ai_methods->om_load_section) {
            res = o->ai_methods->om_load_section(o->ai_object,
                section->dss_index,
                &data_ptr,
                &errc);
            finaltype = Dwarf_Alloc_Malloc;
        } else {
            _dwarf_error_string(dbg, error,
                DW_DLE_SECTION_ERROR,
                "DW_DLE_SECTION_ERROR: "
                " struct Dwarf_Obj_Access_Interface_a_s "
                "is missing an om_load_section function "
                "pointer. Corrupt user setup.");
            return DW_DLV_ERROR;
        }
    }
    if (res == DW_DLV_ERROR) {
        DWARF_DBG_ERROR(dbg, errc, DW_DLV_ERROR);
    }
#if 0 /* Not changing error, to disruptive of regression tests. */
Hold off on this, keep old error for the moment
    if (res == DW_DLV_ERROR) {
        _dwarf_error_string(dbg, error,
            errc," Error in attempting to load section into"
            " memory, possibly corrupt DWARF.");
        return res;
    }
#endif
    if (res == DW_DLV_NO_ENTRY) {
        /*  Gets this for section->dss_index 0.
            Which by ELF definition is a section index
            which is not used (reserved by Elf to
            mean no-section-index).
            Otherwise NULL dss_data gets error.
            BSS would legitimately have no data, but
            no DWARF related section could possibly be bss.
            We also get it if the section is present but
            zero-size. */
        return res;
    }
    section->dss_was_alloc = FALSE;
    section->dss_computed_mmap_offset = mmap_offset;
    section->dss_computed_mmap_len = mmap_len;
    section->dss_mmap_realarea = mmap_real_area;
    section->dss_size = data_len;
    section->dss_data = data_ptr;
    section->dss_load_preference = pref;
    section->dss_actual_load_type = finaltype;

    if (section->dss_ignore_reloc_group_sec) {
        /* Neither zdebug nor reloc apply to .group sections. */
        return res;
    }
    if ((section->dss_zdebug_requires_decompress ||
        section->dss_shf_compressed ||
        section->dss_ZLIB_compressed) &&
        !section->dss_did_decompress) {
        if (!section->dss_data) {
            /*  Impossible. This makes no sense.
                Corrupt object. */
            DWARF_DBG_ERROR(dbg, DW_DLE_COMPRESSED_EMPTY_SECTION,
                DW_DLV_ERROR);
        }
#if defined(HAVE_ZLIB) && defined(HAVE_ZSTD)
        /*  This handles both malloc and mmap case.
            Possibly updating dss_was_malloc if required,
            and setting pref to Dwarf_Alloc_Malloc if
            required. */
        res = do_decompress(dbg,section,error);
        if (res != DW_DLV_OK) {
            return res;
        }
#else /* !defined(HAVE_ZLIB) && defined(HAVE_ZSTD) */
        _dwarf_error_string(dbg, error,
            DW_DLE_ZDEBUG_REQUIRES_ZLIB,
            "DW_DLE_ZDEBUG_REQUIRES_ZLIB: "
            " zlib and zstd are missing, cannot"
            " decompress section.");
        return DW_DLV_ERROR;
#endif /* defined(HAVE_ZLIB) && defined(HAVE_ZSTD) */
        section->dss_did_decompress = TRUE;
        section->dss_actual_load_type = Dwarf_Alloc_Malloc;
        section->dss_was_alloc = TRUE;
    }
    if (_dwarf_apply_relocs == 0) {
        return res;
    }
    if (section->dss_reloc_size == 0) {
        return res;
    }
    if (!o->ai_methods->om_relocate_a_section) {
        return res;
    }
    /*apply relocations */
    res = o->ai_methods->om_relocate_a_section(o->ai_object,
        section->dss_index, dbg, &errc);
    if (res == DW_DLV_ERROR) {
        DWARF_DBG_ERROR(dbg, errc, DW_DLV_ERROR);
    }
    return res;
}

/* This is a hack so clients can verify offsets.
   Added (without so many sections to report)  April 2005
   so that debugger can detect broken offsets
   (which happened in an IRIX  -64 executable larger than 2GB
    using MIPSpro 7.3.1.3 compilers. A couple .debug_pubnames
    offsets were wrong.).
*/
/*  Now with sections new to DWARF5 */
int
dwarf_get_section_max_offsets_d(Dwarf_Debug dbg,
    Dwarf_Unsigned * debug_info_size,
    Dwarf_Unsigned * debug_abbrev_size,
    Dwarf_Unsigned * debug_line_size,
    Dwarf_Unsigned * debug_loc_size,
    Dwarf_Unsigned * debug_aranges_size,
    Dwarf_Unsigned * debug_macinfo_size,
    Dwarf_Unsigned * debug_pubnames_size,
    Dwarf_Unsigned * debug_str_size,
    Dwarf_Unsigned * debug_frame_size,
    Dwarf_Unsigned * debug_ranges_size,
    Dwarf_Unsigned * debug_typenames_size,
    Dwarf_Unsigned * debug_types_size,
    Dwarf_Unsigned * debug_macro_size,
    Dwarf_Unsigned * debug_str_offsets_size,
    Dwarf_Unsigned * debug_sup_size,
    Dwarf_Unsigned * debug_cu_index_size,
    Dwarf_Unsigned * debug_tu_index_size,
    Dwarf_Unsigned * debug_names_size,
    Dwarf_Unsigned * debug_loclists_size,
    Dwarf_Unsigned * debug_rnglists_size)
{
    if (IS_INVALID_DBG(dbg)) {
        return DW_DLV_NO_ENTRY;
    }
    if (debug_info_size) {
        *debug_info_size = dbg->de_debug_info.dss_size;
    }
    if (debug_abbrev_size) {
        *debug_abbrev_size = dbg->de_debug_abbrev.dss_size;
    }
    if (debug_line_size) {
        *debug_line_size = dbg->de_debug_line.dss_size;
    }
    if (debug_loc_size) {
        *debug_loc_size = dbg->de_debug_loc.dss_size;
    }
    if (debug_aranges_size) {
        *debug_aranges_size = dbg->de_debug_aranges.dss_size;
    }
    if (debug_macinfo_size) {
        *debug_macinfo_size = dbg->de_debug_macinfo.dss_size;
    }
    if (debug_pubnames_size) {
        *debug_pubnames_size = dbg->de_debug_pubnames.dss_size;
    }
    if (debug_str_size) {
        *debug_str_size = dbg->de_debug_str.dss_size;
    }
    if (debug_frame_size) {
        *debug_frame_size = dbg->de_debug_frame.dss_size;
    }
    if (debug_ranges_size) {
        *debug_ranges_size = dbg->de_debug_ranges.dss_size;
    }
    if (debug_typenames_size) {
        *debug_typenames_size = dbg->de_debug_typenames.dss_size;
    }
    if (debug_types_size) {
        *debug_types_size = dbg->de_debug_types.dss_size;
    }
    if (debug_macro_size) {
        *debug_macro_size = dbg->de_debug_macro.dss_size;
    }
    if (debug_str_offsets_size) {
        *debug_str_offsets_size = dbg->de_debug_str_offsets.dss_size;
    }
    if (debug_sup_size) {
        *debug_sup_size = dbg->de_debug_sup.dss_size;
    }
    if (debug_cu_index_size) {
        *debug_cu_index_size = dbg->de_debug_cu_index.dss_size;
    }
    if (debug_tu_index_size) {
        *debug_tu_index_size = dbg->de_debug_tu_index.dss_size;
    }
    if (debug_names_size) {
        *debug_names_size = dbg->de_debug_names.dss_size;
    }
    if (debug_loclists_size) {
        *debug_loclists_size = dbg->de_debug_loclists.dss_size;
    }
    if (debug_rnglists_size) {
        *debug_rnglists_size = dbg->de_debug_rnglists.dss_size;
    }
    return DW_DLV_OK;
}

const struct Dwarf_Obj_Access_Section_a_s zerodoas;
/*  Given a section name, get its size and address */
int
dwarf_get_section_info_by_name(Dwarf_Debug dbg,
    const char *section_name,
    Dwarf_Addr *section_addr,
    Dwarf_Unsigned *section_size,
    Dwarf_Error * error)
{
    return dwarf_get_section_info_by_name_a(dbg,
        section_name,
        section_addr,
        section_size,
        0,0,
        error);
}
int
dwarf_get_section_info_by_name_a(Dwarf_Debug dbg,
    const char *section_name,
    Dwarf_Addr *section_addr,
    Dwarf_Unsigned *section_size,
    Dwarf_Unsigned *section_flags,
    Dwarf_Unsigned *section_offset,
    Dwarf_Error * error)
{
    struct Dwarf_Obj_Access_Interface_a_s * obj = 0;
    Dwarf_Unsigned section_count = 0;
    Dwarf_Unsigned section_index = 0;
    struct Dwarf_Obj_Access_Section_a_s doas;

    CHECK_DBG(dbg,error,"dwarf_get_section_info_by_name_a()");
    if (section_addr) {
        *section_addr = 0;
    }
    if (section_size) {
        *section_size = 0;
    }
    if (section_flags) {
        *section_flags = 0;
    }
    if (section_offset) {
        *section_offset = 0;
    }
    if (!section_name) {
        _dwarf_error_string(dbg,error,DW_DLE_DBG_NULL,
            "DW_DLE_DBG_NULL: null section_name pointer "
            "passed to "
            "dwarf_get_section_info_by_name_a");
        return DW_DLV_ERROR;
    }
    if (!section_name[0]) {
        return DW_DLV_NO_ENTRY;
    }
    obj = dbg->de_obj_file;
    if (!obj) {
        return DW_DLV_NO_ENTRY;
    }
    section_count = obj->ai_methods->
        om_get_section_count(obj->ai_object);

    /*  We can skip index 0 when considering ELF files, but not other
        object types. */
    for (section_index = 0; section_index < section_count;
        ++section_index) {
        int errnum = 0;
        int res = 0;

        doas = zerodoas;
        res = obj->ai_methods->
            om_get_section_info(obj->ai_object,
            section_index, &doas, &errnum);
        if (res == DW_DLV_ERROR) {
            DWARF_DBG_ERROR(dbg, errnum, DW_DLV_ERROR);
        }
        if (res == DW_DLV_NO_ENTRY) {
            /* This should be impossible */
            continue;
        }
        if (!strcmp(section_name,doas.as_name)) {
            if (section_addr) {
                *section_addr = doas.as_addr;
            }
            if (section_size) {
                *section_size = doas.as_size;
            }
            if (section_flags) {
                *section_flags = doas.as_flags;
            }
            if (section_offset) {
                *section_offset = doas.as_offset;
            }
            return DW_DLV_OK;
        }
    }
    return DW_DLV_NO_ENTRY;
}

/*  Given a section index, get its size and address */
int
dwarf_get_section_info_by_index(Dwarf_Debug dbg,
    int section_index,
    const char **section_name,
    Dwarf_Addr *section_addr,
    Dwarf_Unsigned *section_size,
    Dwarf_Error * error)
{
    return dwarf_get_section_info_by_index_a(dbg,
        section_index,
        section_name,
        section_addr,
        section_size,
        0,0,
        error);
}
int
dwarf_get_section_info_by_index_a(Dwarf_Debug dbg,
    int section_index,
    const char **section_name,
    Dwarf_Addr *section_addr,
    Dwarf_Unsigned *section_size,
    Dwarf_Unsigned *section_flags,
    Dwarf_Unsigned *section_offset,
    Dwarf_Error * error)
{
    Dwarf_Unsigned sectioncount = 0;
    CHECK_DBG(dbg,error,"dwarf_get_section_info_by_index_a()");

    sectioncount = dwarf_get_section_count(dbg);

    if (section_addr) {
        *section_addr = 0;
    }
    if (section_size) {
        *section_size = 0;
    }
    if (section_name) {
        *section_name = 0;
    }
    if (section_flags) {
        *section_flags = 0;
    }
    if (section_offset) {
        *section_offset = 0;
    }
    if (section_index < 0) {
        return DW_DLV_NO_ENTRY;
    }
    /* Check if we have a valid section index */
    if ((Dwarf_Unsigned)section_index < sectioncount){
        int res = 0;
        int err = 0;
        struct Dwarf_Obj_Access_Section_a_s doas;
        struct Dwarf_Obj_Access_Interface_a_s * obj =
            dbg->de_obj_file;
        if (NULL == obj) {
            return DW_DLV_NO_ENTRY;
        }
        res = obj->ai_methods->om_get_section_info(obj->ai_object,
            section_index, &doas, &err);
        if (res == DW_DLV_ERROR){
            DWARF_DBG_ERROR(dbg, err, DW_DLV_ERROR);
        }

        if (section_addr) {
            *section_addr = doas.as_addr;
        }
        if (section_size) {
            *section_size = doas.as_size;
        }
        if (section_name) {
            *section_name = doas.as_name;
        }
        if (section_flags) {
            *section_flags = doas.as_flags;
        }
        if (section_offset) {
            *section_offset = doas.as_offset;
        }
        return DW_DLV_OK;
    }
    return DW_DLV_NO_ENTRY;
}

/*  Get section count */
Dwarf_Unsigned
dwarf_get_section_count(Dwarf_Debug dbg)
{
    struct Dwarf_Obj_Access_Interface_a_s * obj = 0;

    if (IS_INVALID_DBG(dbg)) {
        return 0;
    }
    obj = dbg->de_obj_file;
    if (!obj) {
        /*  -1  */
        return 0;
    }
    return obj->ai_methods->om_get_section_count(obj->ai_object);
}

Dwarf_Cmdline_Options dwarf_cmdline_options = {
    FALSE /* Use quiet mode by default. */
};

/*  Lets libdwarf reflect a command line option, so we can get details
    of some errors printed using libdwarf-internal information. */
void
dwarf_record_cmdline_options(Dwarf_Cmdline_Options options)
{
    dwarf_cmdline_options = options;
}
