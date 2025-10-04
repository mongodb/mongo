/*
  Portions Copyright (C) 2017-2021 David Anderson. All Rights Reserved.

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

/*  This provides access to the DWARF5 .debug_names section. */

#include <config.h>

#include <stdlib.h> /* calloc() free() */
#include <stdio.h>
#include <string.h> /* memcpy */
#if defined(_WIN32) && defined(HAVE_STDAFX_H)
#include "stdafx.h"
#endif /* HAVE_STDAFX_H */
#ifdef HAVE_STDINT_H
#include <stdint.h> /* uintptr_t */
#endif /* HAVE_STDINT_H */

#include "dwarf.h"
#include "libdwarf.h"
#include "dwarf_local_malloc.h"
#include "libdwarf_private.h"
#include "dwarf_base_types.h"
#include "dwarf_safe_strcpy.h"
#include "dwarf_opaque.h"
#include "dwarf_alloc.h"
#include "dwarf_error.h"
#include "dwarf_util.h"
#include "dwarf_global.h"
#include "dwarf_debugnames.h"
#include "dwarf_string.h"

#if 0 /* dump_bytes */
static void
dump_bytes(char * msg,Dwarf_Small * start, long len)
{
    Dwarf_Small *end = start + len;
    Dwarf_Small *cur = start;

    printf("%s(%ld bytes):",msg,len);
    for (; cur < end; cur++) {
        printf("%02x ", *cur);
    }
    printf("\n");
}
#endif /*0*/
#if 0 /* dump_abbrev_record() */
static void
dump_abbrev_record(const char *msg,
    Dwarf_Unsigned abcode,
    struct Dwarf_D_Abbrev_s *abb,
    int line)
{
    unsigned long i = 0;

    printf("%s: code %lu abpairs %lu line %d \n",
        msg,(unsigned long)abcode,
        (unsigned long)abb->da_pairs_count,line);
    for (i=0; i < abb->da_pairs_count ; ++i) {
        printf("  [%lu] 0x%02lx 0x%02lx\n",
            i,
            (unsigned long)abb->da_idxattr[i],
            (unsigned long)abb->da_form[i]);
    }
}
#endif

/*  Encapsulates DECODE_LEB128_UWORD_LEN_CK
    so the caller can free resources
    in case of problems.
    This updates *lp to point to next byte
*/
static int
_dwarf_read_uleb_ck(Dwarf_Small **lp,
    Dwarf_Unsigned *out_p,
    Dwarf_Debug dbg,
    Dwarf_Error *err,
    Dwarf_Small *lpend)
{
    Dwarf_Small *inptr = *lp;
    Dwarf_Unsigned abcode = 0;

    /* The macro updates inptr */
    DECODE_LEB128_UWORD_CK(inptr,
        abcode,dbg,err,lpend);
    *lp = inptr;
    *out_p = abcode;
    return DW_DLV_OK;
}

/*  Encapsulates DECODE_LEB128_UWORD_CK
    so the caller can free resources
    in case of problems. */
static int
read_uword_ab(Dwarf_Small **lp,
    Dwarf_Unsigned *out_p,
    Dwarf_Debug dbg,
    Dwarf_Error *err,
    Dwarf_Small *lpend)

{
    Dwarf_Small *inptr = *lp;
    Dwarf_Unsigned out = 0;

    /* The macro updates inptr */
    DECODE_LEB128_UWORD_CK(inptr,
        out, dbg,err,lpend);
    *lp = inptr;
    *out_p = out;
    return DW_DLV_OK;
}

static void
free_temp_abbrevs( struct Dwarf_D_Abbrev_s * firstab)
{
    struct Dwarf_D_Abbrev_s *curab = firstab;

    while (curab) {
        struct Dwarf_D_Abbrev_s *nxtab = 0;

        nxtab = curab->da_next;
        curab->da_next = 0;
        free(curab);
        curab = nxtab;
    }
}

static int
fill_in_abbrevs_table(Dwarf_Dnames_Head dn,
    Dwarf_Error * error)
{
    Dwarf_Unsigned ablen =  dn->dn_abbrev_table_size;
    Dwarf_Small   *abdata = dn->dn_abbrevs;
    Dwarf_Small   *tabend = abdata + ablen;
    Dwarf_Small   *abcur = 0;
    Dwarf_Unsigned code = 0;
    Dwarf_Unsigned tag = 0;
    int            foundabend = FALSE;
    Dwarf_Unsigned abcount = 0;
    struct Dwarf_D_Abbrev_s  *curdab = 0;
    struct Dwarf_D_Abbrev_s  *firstdab = 0;
    struct Dwarf_D_Abbrev_s **lastabp = &firstdab;
    Dwarf_Debug    dbg = dn->dn_dbg;

    for (abcur = abdata; abcur < tabend; ) {
        Dwarf_Unsigned attr = 0;
        Dwarf_Unsigned form = 0;
        Dwarf_Small   *inner = 0;
        Dwarf_Unsigned inroffset = 0;
        int            res = 0;

        inroffset = abcur - abdata;
        res = read_uword_ab(&abcur,&code,dbg,error,tabend);
        if (res != DW_DLV_OK) {
            free_temp_abbrevs(firstdab);
            return res;
        }
        if (code == 0) {
            foundabend = TRUE;
            break;
        }

        res = read_uword_ab(&abcur,&tag,dbg,error,tabend);
        if (res != DW_DLV_OK) {
            free_temp_abbrevs(firstdab);
            return res;
        }
        inner = abcur;
        curdab = (struct Dwarf_D_Abbrev_s *)calloc(1,
            sizeof(struct Dwarf_D_Abbrev_s));
        if (!curdab) {
            free_temp_abbrevs(firstdab);
            firstdab = 0;
            _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
            return DW_DLV_ERROR;
        }
        curdab->da_abbrev_offset = inroffset;
        curdab->da_abbrev_code = code;
        curdab->da_tag = tag;
        abcount++;
        for (;;) {
            res = read_uword_ab(&inner,&attr,dbg,error,tabend);
            if (res != DW_DLV_OK) {
                free_temp_abbrevs(curdab);
                free_temp_abbrevs(firstdab);
                firstdab = 0;
                return res;
            }
            res = read_uword_ab(&inner,&form,dbg,error,tabend);
            if (res != DW_DLV_OK) {
                free_temp_abbrevs(curdab);
                free_temp_abbrevs(firstdab);
                firstdab = 0;
                return res;
            }
            if ( curdab->da_pairs_count ==
                ABB_PAIRS_MAX) {
                free_temp_abbrevs(curdab);
                free_temp_abbrevs(firstdab);
                _dwarf_error_string(dbg,error,
                    DW_DLE_DEBUG_NAMES_ABBREV_CORRUPTION,
                    "DW_DLE_DEBUG_NAMES_ABBREV_CORRUPTION: "
                    "Impossible: too many idxattr/form pairs"
                    "corrupt abbrevs");
                return DW_DLV_ERROR;
            }
            curdab->da_idxattr[curdab->da_pairs_count] =
                (Dwarf_Half)attr;
            curdab->da_form[curdab->da_pairs_count] =
                (Dwarf_Half)form;
            curdab->da_pairs_count++;
            if (!attr && !form) {
                /*  We put the terminator into the pairs list.
                    done for this pair set */
                break;
            }
        }

        /* Building singly linked list.  without if stmt. */
        abcur = inner;
        *lastabp = curdab;
        lastabp = &curdab->da_next;
    }
    if (!foundabend) {
        free_temp_abbrevs(firstdab);
        _dwarf_error_string(dbg, error,
            DW_DLE_DEBUG_NAMES_ABBREV_CORRUPTION,
            "DW_DLE_DEBUG_NAMES_ABBREV_CORRUPTION: Never found"
            "abbrev final NUL byte");
        return DW_DLV_ERROR;
    }
    if ( abcur < tabend) {
        unsigned padcount = 0;

        for (; abcur < tabend; ++abcur) {
            ++padcount;
            if (*abcur) {
                dwarfstring  m;

                dwarfstring_constructor(&m);
                dwarfstring_append_printf_u(&m,
                    "WARNING NON NULL debug_names "
                    "abbreviation pad. "
                    "padcount %u at ",padcount);
                dwarfstring_append_printf_u(&m,
                    ".debug_names sec_offset 0x%lx",
                    (uintptr_t)(abcur - dn->dn_section_data));
                dwarf_insert_harmless_error(dbg,
                    dwarfstring_string(&m));
                dwarfstring_destructor(&m);
            }
        }
    }
    {
        Dwarf_Unsigned ct = 0;
        struct Dwarf_D_Abbrev_s *tmpa = 0;

        dn->dn_abbrev_instances =
            (struct Dwarf_D_Abbrev_s *)calloc(
            abcount,sizeof(struct Dwarf_D_Abbrev_s));
        if (!dn->dn_abbrev_instances) {
            free_temp_abbrevs(firstdab);
            _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
            return DW_DLV_ERROR;
        }
        dn->dn_abbrev_instance_count = abcount;
        tmpa = firstdab;
        for (ct = 0; ct < abcount; ++ct) {
            struct Dwarf_D_Abbrev_s *tmpb =tmpa->da_next;
            /*  da_next no longer means anything */
            dn->dn_abbrev_instances[ct] = *tmpa;
            dn->dn_abbrev_instances[ct].da_next = 0;
            free(tmpa);
            tmpa = tmpb;
        }
        tmpa = 0;
        firstdab = 0;
        /*  Now the list has turned into an
            array. We can ignore
            the list aspect. */
    }
    return DW_DLV_OK;
}
static int
read_uword_val(Dwarf_Debug dbg,
    Dwarf_Small **ptr_in,
    Dwarf_Small *endptr,
    int   errcode,
    Dwarf_Unsigned *val_out,
    Dwarf_Unsigned area_length,
    Dwarf_Error *error)
{
    Dwarf_Unsigned val = 0;
    Dwarf_Small *ptr = *ptr_in;

    READ_UNALIGNED_CK(dbg, val, Dwarf_Unsigned,
        ptr, DWARF_32BIT_SIZE,
        error,endptr);
    ptr += DWARF_32BIT_SIZE;
    if (ptr >= endptr) {
        _dwarf_error(dbg, error,errcode);
        return DW_DLV_ERROR;
    }
    /*  Some of the fields are not length fields, but
        if non-zero the size will be longer than
        the value, so we do the following
        overall sanity check to avoid overflows. */
    if (val > area_length) {
        _dwarf_error(dbg, error,errcode);
        return DW_DLV_ERROR;
    }
    *val_out = val;
    *ptr_in = ptr;
    return DW_DLV_OK;
}

/*  For now, check using global offset, not pointer */
#define VALIDATEOFFSET(dnm,used,m)                  \
    do {                                            \
        if ((used) >= (dnm)->dn_next_set_offset) {  \
            _dwarf_error_string(dbg, error,         \
                DW_DLE_DEBUG_NAMES_HEADER_ERROR,    \
                "DW_DLE_DEBUG_NAMES_HEADER_ERROR: " \
                m);                                 \
            return DW_DLV_ERROR;                    \
        }                                           \
    } while(0)

/*  Reading a .debug_names Name Index header */
static int
read_a_name_table_header(Dwarf_Dnames_Head dn,
    Dwarf_Unsigned  starting_offset,
    Dwarf_Unsigned  remaining_space,
    Dwarf_Small    *curptr_in,
    Dwarf_Unsigned *usedspace_out,
    Dwarf_Unsigned *next_offset,
    Dwarf_Small    *end_section,
    Dwarf_Error    *error)
{
    Dwarf_Unsigned area_length = 0;
    Dwarf_Unsigned area_max_offset = 0;
    unsigned initial_length = 0; /*offset_size+local_ext */
    int offset_size = 0;
    int local_extension_size = 0;
    Dwarf_Small *end_dnames = 0; /* 1 past local table */
    Dwarf_Half version = 0;
    Dwarf_Half padding = 0;
    Dwarf_Unsigned comp_unit_count = 0;
    Dwarf_Unsigned local_type_unit_count = 0;
    Dwarf_Unsigned foreign_type_unit_count = 0;
    Dwarf_Unsigned bucket_count = 0;
    Dwarf_Unsigned name_count = 0;
    Dwarf_Unsigned abbrev_table_size = 0; /* bytes */
    Dwarf_Unsigned entry_pool_size = 0; /* bytes */
    Dwarf_Unsigned augmentation_string_size = 0; /* bytes */
    Dwarf_Unsigned usedspace = 0;
    Dwarf_Unsigned totaloffset = 0;
    int res = 0;
    const char *str_utf8 = 0; /* Augmentation string */
    Dwarf_Small *curptr = 0;
    Dwarf_Debug dbg = dn->dn_dbg;

    curptr = curptr_in;
    usedspace = 0;
    totaloffset = starting_offset;
    /* 1 */
    READ_AREA_LENGTH_CK(dbg, area_length, Dwarf_Unsigned,
        curptr, offset_size,
        local_extension_size,error,
        remaining_space,end_section);
    initial_length =  offset_size+local_extension_size;

    /* curptr now points past the length field */
    area_max_offset = area_length + initial_length;
    usedspace = initial_length;
    totaloffset += initial_length;
    dn->dn_offset_size = (Dwarf_Half)offset_size;
    /* Two stage length test so overflow is caught. */
    if (area_length > remaining_space ||
        (area_length +offset_size +local_extension_size) >
        remaining_space) {
        _dwarf_error_string(dbg, error,
            DW_DLE_DEBUG_NAMES_HEADER_ERROR,
            "DW_DLE_DEBUG_NAMES_HEADER_ERROR: "
            "The index table runs off the end of the .debug_names "
            "section. Corrupt data.");
        return DW_DLV_ERROR;
    }
    if (area_max_offset > remaining_space) {
        _dwarf_error_string(dbg, error,
            DW_DLE_DEBUG_NAMES_HEADER_ERROR,
            "DW_DLE_DEBUG_NAMES_HEADER_ERROR: "
            "The index table runs off the end of the .debug_names "
            "section... Corrupt data.");
        return DW_DLV_ERROR;
    }
    end_dnames = curptr + area_length;
    dn->dn_unit_length = area_length + local_extension_size;
    dn->dn_indextable_data_end = end_dnames;
    dn->dn_next_set_offset = area_length
        + initial_length +
        + starting_offset;

    /* 2 */
    READ_UNALIGNED_CK(dbg, version, Dwarf_Half,
        curptr, DWARF_HALF_SIZE,
        error,end_dnames);
    curptr += DWARF_HALF_SIZE;
    usedspace += DWARF_HALF_SIZE;
    totaloffset += DWARF_HALF_SIZE;
    if (curptr >= end_dnames) {
        _dwarf_error(dbg, error,DW_DLE_DEBUG_NAMES_HEADER_ERROR);
        return DW_DLV_ERROR;
    }
    if (version != DWARF_DNAMES_VERSION5) {
        _dwarf_error(dbg, error, DW_DLE_VERSION_STAMP_ERROR);
        return DW_DLV_ERROR;
    }
    /* 3 */
    READ_UNALIGNED_CK(dbg, padding, Dwarf_Half,
        curptr, DWARF_HALF_SIZE,
        error,end_dnames);
    curptr += DWARF_HALF_SIZE;
    usedspace += DWARF_HALF_SIZE;
    totaloffset += DWARF_HALF_SIZE;
    if (curptr >= end_dnames) {
        _dwarf_error(dbg, error,DW_DLE_DEBUG_NAMES_HEADER_ERROR);
        return DW_DLV_ERROR;
    }
    if (padding) {
        _dwarf_error(dbg, error,DW_DLE_DEBUG_NAMES_HEADER_ERROR);
        return DW_DLV_ERROR;
    }
    /* 4 */
    res = read_uword_val(dbg, &curptr,
        end_dnames, DW_DLE_DEBUG_NAMES_HEADER_ERROR,
        &comp_unit_count,area_max_offset,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    if (comp_unit_count > dn->dn_section_size) {
        _dwarf_error_string(dbg,error,
            DW_DLE_DEBUG_NAMES_ERROR,
            "DW_DLE_DEBUG_NAMES_ERROR comp_unit_count too large");
        return DW_DLV_ERROR;
    }
    dn->dn_comp_unit_count = comp_unit_count;
    usedspace += SIZEOFT32;
    totaloffset +=  SIZEOFT32;
    /* 5 */
    res = read_uword_val(dbg, &curptr,
        end_dnames, DW_DLE_DEBUG_NAMES_HEADER_ERROR,
        &local_type_unit_count,area_max_offset,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    if (local_type_unit_count >  dn->dn_section_size) {
        _dwarf_error_string(dbg,error,
            DW_DLE_DEBUG_NAMES_ERROR,
            "DW_DLE_DEBUG_NAMES_ERROR local_type_unit_count large");
        return DW_DLV_ERROR;
    }
    dn->dn_local_type_unit_count = local_type_unit_count;
    usedspace += SIZEOFT32;
    totaloffset +=  SIZEOFT32;

    /* 6 */
    res = read_uword_val(dbg, &curptr,
        end_dnames, DW_DLE_DEBUG_NAMES_HEADER_ERROR,
        &foreign_type_unit_count,area_max_offset,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    if (foreign_type_unit_count >  dn->dn_section_size) {
        _dwarf_error_string(dbg,error,
            DW_DLE_DEBUG_NAMES_ERROR,
            "DW_DLE_DEBUG_NAMES_ERROR: "
            "foreign_type_unit_count large");
        return DW_DLV_ERROR;
    }
    dn->dn_foreign_type_unit_count = foreign_type_unit_count;
    usedspace += SIZEOFT32;
    totaloffset += SIZEOFT32;
    /* 7 */
    res = read_uword_val(dbg, &curptr,
        end_dnames, DW_DLE_DEBUG_NAMES_HEADER_ERROR,
        &bucket_count,area_max_offset,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    if (bucket_count > dn->dn_section_size) {
        _dwarf_error_string(dbg,error,
            DW_DLE_DEBUG_NAMES_ERROR,
            "DW_DLE_DEBUG_NAMES_ERROR bucketcount too large");
        return DW_DLV_ERROR;
    }
    dn->dn_bucket_count = bucket_count;
    usedspace += SIZEOFT32;
    totaloffset += SIZEOFT32;
    /*  name_count gives the size of
        the string-offsets and entry-offsets arrays,
        and if hashes present, the size of the hashes
        array. */
    /* 8 */
    res = read_uword_val(dbg, &curptr,
        end_dnames, DW_DLE_DEBUG_NAMES_HEADER_ERROR,
        &name_count,area_max_offset,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    dn->dn_name_count = name_count;
    if (name_count > dn->dn_section_size) {
        _dwarf_error_string(dbg,error,
            DW_DLE_DEBUG_NAMES_ERROR,
            "DW_DLE_DEBUG_NAMES_ERROR name_count too large");
        return DW_DLV_ERROR;
    }
    usedspace += SIZEOFT32;
    totaloffset += SIZEOFT32;

    /*  abbrev_table */
    /* 9 */
    res = read_uword_val(dbg, &curptr,
        end_dnames, DW_DLE_DEBUG_NAMES_HEADER_ERROR,
        &abbrev_table_size,area_max_offset,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    if (abbrev_table_size > dn->dn_section_size) {
        _dwarf_error_string(dbg,error,
            DW_DLE_DEBUG_NAMES_ERROR,
            "DW_DLE_DEBUG_NAMES_ERROR abbrev_table_size too large");
        return DW_DLV_ERROR;
    }
    dn->dn_abbrev_table_size = abbrev_table_size;
    usedspace += SIZEOFT32;
    totaloffset += SIZEOFT32;

    /* 10 */
    res = read_uword_val(dbg, &curptr,
        end_dnames, DW_DLE_DEBUG_NAMES_HEADER_ERROR,
        &augmentation_string_size,area_max_offset,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    if (augmentation_string_size > dn->dn_section_size) {
        _dwarf_error_string(dbg,error,
            DW_DLE_DEBUG_NAMES_ERROR,
            "DW_DLE_DEBUG_NAMES_ERROR augmentation string too long");
        return DW_DLV_ERROR;
    }
    usedspace += SIZEOFT32;
    totaloffset += SIZEOFT32;

    str_utf8 = (const char *) curptr;
    totaloffset += augmentation_string_size;
    usedspace += augmentation_string_size;
    curptr += augmentation_string_size;
    dn->dn_augmentation_string_size = augmentation_string_size;
    if (curptr >= end_dnames) {
        _dwarf_error(dbg, error,DW_DLE_DEBUG_NAMES_HEADER_ERROR);
        return DW_DLV_ERROR;
    }

    dn->dn_version = version;
    dn->dn_abbrev_table_size = abbrev_table_size;
    if (augmentation_string_size) {
        /*  11. len: The string size includes zero or
            more NUL bytes to be a multiple of
            four bytes long. */
        Dwarf_Unsigned len = augmentation_string_size;
        const char *cp = 0;
        const char *cpend = 0;
        Dwarf_Unsigned finallen = 0;

        dn->dn_augmentation_string = calloc(1,
            augmentation_string_size + 1);
        /* Ensures a final NUL byte */
        _dwarf_safe_strcpy(dn->dn_augmentation_string,
            len +1,str_utf8, len);
        /* This validates a zero length string too. */
        /* in LLVM0700 there is no NUL terminator there:
            See DWARF5 page 144 and also ISSUE
            200505.4 */
        cp = dn->dn_augmentation_string;
        cpend = cp + len;
        /* It's null terminated now, so find the true length */
        for ( ; cp<cpend; ++cp) {
            if (!*cp) {
                break;
            }
            ++finallen;
        }
        if (len%4) {
            dwarfstring m;

            dwarfstring_constructor(&m);
            dwarfstring_append_printf_u(&m,
                "DW_DLE_DEBUG_NAMES_PAD_NON_ZERO: "
                "The augmentation_string_size "
                " is %u, not a multiple of four",len);
            _dwarf_error_string(dbg, error,
                DW_DLE_DEBUG_NAMES_PAD_NON_ZERO,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
        if (finallen != len) {
            cp = str_utf8 + finallen;
            cpend = str_utf8+len;
            /*  Ensure that there is no corruption in
                the padding. */
            for ( ; cp < cpend; ++cp) {
                if (*cp) {
                    _dwarf_error_string(dbg, error,
                        DW_DLE_DEBUG_NAMES_PAD_NON_ZERO,
                        "DW_DLE_DEBUG_NAMES_PAD_NON_ZERO: "
                        "padding in augmentation string not zeros");
                    return DW_DLV_ERROR;
                }
            }
        }
    }

    /*  Now we deal with the arrays following the header. */
    dn->dn_cu_list = curptr;
    dn->dn_cu_list_offset = usedspace;
    curptr +=  dn->dn_offset_size * comp_unit_count;
    usedspace += dn->dn_offset_size * comp_unit_count ;
    totaloffset += dn->dn_offset_size * comp_unit_count ;
    VALIDATEOFFSET(dn,totaloffset,"comp_unit array error");

    dn->dn_local_tu_list = curptr;
    dn->dn_local_tu_list_offset = usedspace;
    curptr +=  dn->dn_offset_size *local_type_unit_count;
    usedspace += dn->dn_offset_size* local_type_unit_count;
    totaloffset += dn->dn_offset_size* local_type_unit_count;
    VALIDATEOFFSET(dn,totaloffset,"local_type__unit array error");

    dn->dn_foreign_tu_list = curptr;
    dn->dn_foreign_tu_list_offset = usedspace;
    curptr += sizeof(Dwarf_Sig8)     * foreign_type_unit_count;
    usedspace += sizeof(Dwarf_Sig8)  * foreign_type_unit_count;
    totaloffset += sizeof(Dwarf_Sig8)* foreign_type_unit_count;
    VALIDATEOFFSET(dn,totaloffset,"foreign_type__unit array error");

    dn->dn_buckets_offset = usedspace;
    dn->dn_buckets = curptr;
    curptr +=  SIZEOFT32 * bucket_count ;
    usedspace += SIZEOFT32 * bucket_count;
    totaloffset += SIZEOFT32 * bucket_count;
    VALIDATEOFFSET(dn,totaloffset," bucket array error");

    if (comp_unit_count == 1) {
        Dwarf_Small *ptrx     = dn->dn_cu_list;
        Dwarf_Small *endptr   = dn->dn_foreign_tu_list;
        Dwarf_Unsigned unit_entry_size = dn->dn_offset_size;
        Dwarf_Unsigned offsetval = 0;

        READ_UNALIGNED_CK(dbg, offsetval, Dwarf_Unsigned,
            ptrx,(unsigned long) unit_entry_size,
            error,endptr);
        dn->dn_single_cu = TRUE;
        dn->dn_single_cu_offset = offsetval;
    }
    dn->dn_hash_table = curptr;
    dn->dn_hash_table_offset = usedspace;
    if (bucket_count) {
        curptr +=  SIZEOFT32 * name_count;
        usedspace += SIZEOFT32 * name_count;
        totaloffset += SIZEOFT32 * name_count;
    }
    VALIDATEOFFSET(dn,totaloffset,"hashes array error");

    dn->dn_string_offsets = curptr;
    dn->dn_string_offsets_offset = usedspace;
    curptr +=  dn->dn_offset_size * name_count;
    usedspace += dn->dn_offset_size * name_count;
    totaloffset += dn->dn_offset_size * name_count;
    VALIDATEOFFSET(dn,totaloffset,"string offsets array error");

    dn->dn_entry_offsets = curptr;
    dn->dn_entry_offsets_offset = usedspace;
    curptr +=  dn->dn_offset_size * name_count;
    usedspace += dn->dn_offset_size * name_count;
    totaloffset += dn->dn_offset_size * name_count;
    VALIDATEOFFSET(dn,totaloffset,"entry offsets array error");

    dn->dn_abbrevs = curptr;
    dn->dn_abbrevs_offset = totaloffset;
    VALIDATEOFFSET(dn,totaloffset,"abbrev table error");
    curptr +=  dn->dn_abbrev_table_size;
    usedspace +=  dn->dn_abbrev_table_size;
    totaloffset +=  dn->dn_abbrev_table_size;
    VALIDATEOFFSET(dn,totaloffset,"abbrev table error");

    dn->dn_entry_pool = curptr;
    dn->dn_entry_pool_offset = totaloffset;
    if (dn->dn_next_set_offset < totaloffset) {
        _dwarf_error_string(dbg, error,
            DW_DLE_DEBUG_NAMES_HEADER_ERROR,
            "DW_DLE_DEBUG_NAMES_HEADER_ERROR: "
            "Abbrev total wrong, exceeds "
            "the room available.");
        return DW_DLV_ERROR;
    }

    entry_pool_size = dn->dn_next_set_offset - totaloffset;
    dn->dn_entry_pool_size = entry_pool_size;
    curptr +=   entry_pool_size;
    usedspace += entry_pool_size;
    totaloffset += entry_pool_size;

    if (totaloffset != dn->dn_next_set_offset)   {
        _dwarf_error_string(dbg, error,
            DW_DLE_DEBUG_NAMES_HEADER_ERROR,
            "DW_DLE_DEBUG_NAMES_HEADER_ERROR: "
            "Final total offset does not match base "
            "calculation. Logic error.");
        return DW_DLV_ERROR;
    }

    *usedspace_out = usedspace;
    *next_offset = dn->dn_next_set_offset;
    res = fill_in_abbrevs_table(dn,error);
    if (res != DW_DLV_OK) {
        free(dn->dn_augmentation_string);
        dn->dn_augmentation_string = 0;
        return res;
    }
    return DW_DLV_OK;
}

#define FAKE_LAST_USED 0xffffffff

/*  There may be one debug index for an entire object file,
    for multiple CUs or there can be individual indexes
    for some CUs.
    see DWARF5 6.1.1.3 Per_CU versus Per-Module Indexes.
    The initial of these tables starts at offset 0.
    If the starting-offset is too high for the section
    return DW_DLV_NO_ENTRY */
int
dwarf_dnames_header(Dwarf_Debug dbg,
    Dwarf_Off           starting_offset,
    Dwarf_Dnames_Head * dn_out,
    Dwarf_Off         * offset_of_next_table,
    Dwarf_Error       * error)
{
    Dwarf_Unsigned    remaining    = 0;
    Dwarf_Dnames_Head dn           = 0;
    Dwarf_Unsigned    section_size = 0;
    Dwarf_Unsigned    usedspace    = 0;
    Dwarf_Unsigned    next_offset  = 0;
    Dwarf_Small      *start_section= 0;
    Dwarf_Small      *end_section  = 0;
    Dwarf_Small      *curptr       = 0;
    int              res           = 0;

    CHECK_DBG(dbg,error,"dwarf_dnames_header()");
    res = _dwarf_load_section(dbg, &dbg->de_debug_names, error);
    if (res != DW_DLV_OK) {
        return res;
    }
    section_size = dbg->de_debug_names.dss_size;
    if (!section_size){
        return DW_DLV_NO_ENTRY;
    }
    res = _dwarf_load_section(dbg, &dbg->de_debug_str,error);
    if (res == DW_DLV_ERROR) {
        return res;
    }
    if (starting_offset >= section_size) {
        return DW_DLV_NO_ENTRY;
    }
    start_section = dbg->de_debug_names.dss_data;
    curptr = start_section + starting_offset;
    end_section = start_section + section_size;
    dn =  (Dwarf_Dnames_Head)_dwarf_get_alloc(dbg,
        DW_DLA_DNAMES_HEAD, 1);
    if (!dn) {
        _dwarf_error_string(dbg, error, DW_DLE_ALLOC_FAIL,
            "DW_DLE_ALLOC_FAIL: dwarf_get_alloc of "
            "a Dwarf_Dnames_Head record failed.");
        return DW_DLV_ERROR;
    }
    dn->dn_magic = DWARF_DNAMES_MAGIC;
    dn->dn_section_data = start_section;
    dn->dn_section_size = section_size;
    dn->dn_section_end = end_section;
    dn->dn_dbg = dbg;
    dn->dn_section_offset = starting_offset;
    dn->dn_indextable_data = curptr;
    remaining = dn->dn_section_size - starting_offset;
    res = read_a_name_table_header(dn,
        starting_offset,
        remaining,
        curptr,
        &usedspace,
        &next_offset,
        dn->dn_section_end,
        error);
    if (res == DW_DLV_ERROR) {
        dwarf_dealloc_dnames(dn);
        return res;
    }
    if (res == DW_DLV_NO_ENTRY) {
        /*  Impossible. A bug. Or possibly
            a bunch of zero pad? */
        dwarf_dealloc_dnames(dn);
        return res;
    }
    if (usedspace > section_size) {
        dwarf_dealloc_dnames(dn);
        _dwarf_error_string(dbg, error,DW_DLE_DEBUG_NAMES_OFF_END,
            "DW_DLE_DEBUG_NAMES_OFF_END: "
            " used space > section size");
        return DW_DLV_ERROR;
    }
    remaining -= usedspace;
    if (remaining && remaining < 15) {
        /*  No more content in here, just padding. Check for zero
            in padding. */
        curptr += usedspace;
        for ( ; curptr < end_section; ++curptr) {
            if (*curptr) {
                /*  Fixing Coverity Scan CID 581830
                    resource leak if just call dealloc,
                    so call local dealloc to do it. */
                dwarf_dealloc_dnames(dn);
                /*  One could argue this is a harmless error,
                    but for now assume it is real corruption. */
                _dwarf_error_string(dbg, error,
                    DW_DLE_DEBUG_NAMES_PAD_NON_ZERO,
                    "DW_DLE_DEBUG_NAMES_PAD_NON_ZERO: "
                    "space at end of valid tables not zeros");
                return DW_DLV_ERROR;
            }
        }
    }
    *dn_out = dn;
    *offset_of_next_table = next_offset;
    return DW_DLV_OK;
}

/*  Frees all the space in dn. It's up to you
    to to "dn = 0;" after the call. */
static void
_dwarf_internal_dwarf_dealloc_dnames(Dwarf_Dnames_Head dn)
{
    if (!dn || dn->dn_magic != DWARF_DNAMES_MAGIC) {
        return;
    }
    free(dn->dn_augmentation_string);
    dn->dn_augmentation_string = 0;
    free(dn->dn_bucket_array);
    dn->dn_bucket_array = 0;
    free(dn->dn_abbrev_instances);
    dn->dn_abbrev_instances = 0;
    dn->dn_abbrev_instance_count = 0;
    dn->dn_magic = 0;
}

void
dwarf_dealloc_dnames(Dwarf_Dnames_Head dn)
{
    if (!dn || dn->dn_magic != DWARF_DNAMES_MAGIC) {
        return;
    }
    _dwarf_internal_dwarf_dealloc_dnames(dn);
    /*  Now dn_magic 0 so the dwarf_dealloc
        will just do the base record free. */
    dwarf_dealloc(dn->dn_dbg,dn,DW_DLA_DNAMES_HEAD);
}
/*  Frees any Dwarf_Dnames_Head_s data content that is directly
    mallocd, unless such is already done. */
void
_dwarf_dnames_destructor(void *m)
{
    Dwarf_Dnames_Head dn = (Dwarf_Dnames_Head)m;
    if (!dn || dn->dn_magic != DWARF_DNAMES_MAGIC) {
        return;
    }
    _dwarf_internal_dwarf_dealloc_dnames(dn);
}

/*  These are the sizes/counts applicable a particular
    names table (most likely the only one) in the
    .debug_names section, numbers from the section
    Dwarf_Dnames header.
    DWARF5 section 6.1.1.2 Structure of the Name Header. */
int dwarf_dnames_sizes(Dwarf_Dnames_Head dn,
    /* The counts are entry counts, not byte sizes. */
    Dwarf_Unsigned * comp_unit_count,
    Dwarf_Unsigned * local_type_unit_count,
    Dwarf_Unsigned * foreign_type_unit_count,
    Dwarf_Unsigned * bucket_count,
    Dwarf_Unsigned * name_count,

    /* The following are counted in bytes */
    Dwarf_Unsigned * abbrev_table_size,
    Dwarf_Unsigned * entry_pool_size,
    Dwarf_Unsigned * augmentation_string_size,
    char          ** augmentation_string,
    Dwarf_Unsigned * section_size,
    Dwarf_Half     * table_version,
    Dwarf_Half     * offset_size,
    Dwarf_Error *    error)
{
    if (!dn || dn->dn_magic != DWARF_DNAMES_MAGIC) {
        _dwarf_error_string(NULL, error,DW_DLE_DBG_NULL,
            "DW_DLE_DBG_NULL: A call to dwarf_dnames_sizes() "
            "has a NULL Dwarf_Dnames_Head or an improper one.");
        return DW_DLV_ERROR;
    }
    if (comp_unit_count) {
        *comp_unit_count = dn->dn_comp_unit_count;
    }
    if (local_type_unit_count) {
        *local_type_unit_count = dn->dn_local_type_unit_count;
    }
    if (foreign_type_unit_count) {
        *foreign_type_unit_count = dn->dn_foreign_type_unit_count;
    }
    if (bucket_count) {
        *bucket_count = dn->dn_bucket_count;
    }
    if (name_count) {
        *name_count = dn->dn_name_count;
    }
    if (abbrev_table_size) {
        *abbrev_table_size = dn->dn_abbrev_table_size;
    }
    if (entry_pool_size) {
        *entry_pool_size = dn->dn_entry_pool_size;
    }
    if (augmentation_string_size) {
        *augmentation_string_size = dn->dn_augmentation_string_size;
    }
    if (augmentation_string) {
        *augmentation_string = dn->dn_augmentation_string;
    }
    if (section_size) {
        *section_size = dn->dn_section_size;
    }
    if (table_version) {
        *table_version = dn->dn_version;
    }
    if (offset_size) {
        *offset_size = dn->dn_offset_size;
    }
    return DW_DLV_OK;
}

/*  Useful for investigating errors in libdwarf or the
    .debug_names section. */
int
dwarf_dnames_offsets(Dwarf_Dnames_Head dn,
    Dwarf_Unsigned * header_offset,
    Dwarf_Unsigned * cu_table_offset,
    Dwarf_Unsigned * tu_local_offset,
    Dwarf_Unsigned * foreign_tu_offset,
    Dwarf_Unsigned * bucket_offset,
    Dwarf_Unsigned * hashes_offset,
    Dwarf_Unsigned * stringoffsets_offset,
    Dwarf_Unsigned * entryoffsets_offset,
    Dwarf_Unsigned * abbrev_table_offset,
    Dwarf_Unsigned * entry_pool_offset,
    Dwarf_Error *    error)
{
    if (!dn || dn->dn_magic != DWARF_DNAMES_MAGIC) {
        _dwarf_error_string(NULL, error,DW_DLE_DBG_NULL,
            "DW_DLE_DBG_NULL: A call to dwarf_dnames_offsets() "
            "has a NULL Dwarf_Dnames_Head or an improper one.");
        return DW_DLV_ERROR;
    }
    if (header_offset) {
        *header_offset = dn->dn_section_offset;
    }
    if (cu_table_offset) {
        *cu_table_offset = dn->dn_cu_list_offset;
    }
    if (tu_local_offset) {
        *tu_local_offset = dn->dn_local_tu_list_offset;
    }
    if (foreign_tu_offset) {
        *foreign_tu_offset = dn->dn_foreign_tu_list_offset;
    }
    if (bucket_offset) {
        *bucket_offset = dn->dn_buckets_offset;
    }
    if (hashes_offset) {
        *hashes_offset = dn->dn_hash_table_offset;
    }
    if (stringoffsets_offset) {
        *stringoffsets_offset = dn->dn_string_offsets_offset;
    }
    if (entryoffsets_offset) {
        *entryoffsets_offset = dn->dn_entry_offsets_offset;
    }
    if (abbrev_table_offset) {
        *abbrev_table_offset = dn->dn_abbrevs_offset;
    }
    if (entry_pool_offset) {
        *entry_pool_offset = dn->dn_entry_pool_offset;
    }
    return DW_DLV_OK;
}

/*  The "tu" case covers both local type units
    and foreign type units.
    This table is indexed starting at 0.
*/
int
dwarf_dnames_cu_table(Dwarf_Dnames_Head dn,
    const char        * type /* "cu", "tu" */,
    Dwarf_Unsigned      index_number,
    Dwarf_Unsigned    * offset,
    Dwarf_Sig8        * sig,
    Dwarf_Error       * error)
{
    Dwarf_Debug    dbg             = 0;
    Dwarf_Unsigned unit_count      = 0;
    Dwarf_Unsigned total_count     = 0;
    Dwarf_Unsigned unit_entry_size = 0;
    Dwarf_Small  * unit_ptr        = 0;
    Dwarf_Unsigned foreign_count   = 0;
    Dwarf_Bool     offset_case     = TRUE;

    if (!dn || dn->dn_magic != DWARF_DNAMES_MAGIC) {
        _dwarf_error_string(NULL, error,DW_DLE_DEBUG_NAMES_ERROR,
            "DW_DLE_DEBUG_NAMES_ERROR: "
            " Dwarf_Dnames_Head is NULL or invalid pointer"
            "calling dwarf_dnames_cu_table()");
        return DW_DLV_ERROR;
    }

    dbg = dn->dn_dbg;
    if (type[0] == 'c') {
        unit_ptr = dn->dn_cu_list;
        unit_entry_size = dn->dn_offset_size;
        unit_count = dn->dn_comp_unit_count;
        total_count = unit_count;
        offset_case = TRUE;
    } else if (type[0] == 't') {
        unit_count = dn->dn_local_type_unit_count;
        foreign_count = dn->dn_foreign_type_unit_count;
        total_count = unit_count + foreign_count;
        if (index_number < dn->dn_local_type_unit_count) {
            unit_ptr = dn->dn_local_tu_list;
            unit_entry_size = dn->dn_offset_size;
            offset_case = TRUE;
        } else {
            unit_ptr = dn->dn_foreign_tu_list;
            unit_entry_size = sizeof(Dwarf_Sig8);
            offset_case = FALSE;
        }
    } else {
        _dwarf_error_string(dbg,error,DW_DLE_DEBUG_NAMES_ERROR,
            "DW_DLE_DEBUG_NAMES_ERROR: "
            "type string is not start with cu or tu"
            "so invalid call to dwarf_dnames_cu_table()");
        return DW_DLV_ERROR;
    }
    if (index_number >= total_count) {
        return DW_DLV_NO_ENTRY;
    }
    if (offset_case) {
        /* CU or TU ref */
        Dwarf_Unsigned offsetval = 0;
        Dwarf_Small *ptr = unit_ptr +
            (index_number) *unit_entry_size;
        Dwarf_Small *endptr = dn->dn_indextable_data_end;

        READ_UNALIGNED_CK(dbg, offsetval, Dwarf_Unsigned,
            ptr, (unsigned long)unit_entry_size,
            error,endptr);
        if (offset) {
            *offset = offsetval;
        }
        return DW_DLV_OK;
    }
    {
        Dwarf_Small *ptr =  unit_ptr +
            (index_number -unit_count) *unit_entry_size;
        if (sig) {
            memcpy(sig,ptr,sizeof(*sig));
        }
    }
    return DW_DLV_OK;
}

/*  Each bucket gives the index of the first of
    the name entries for the bucket
    index_of_entry returns the name table entry.
    indexcount returns the number of name table entries
    (representing name collisions) for this bucket. */
static int
_dwarf_initialize_bucket_details(Dwarf_Dnames_Head dn,
    Dwarf_Error *  error)
{
    Dwarf_Debug dbg = 0;
    Dwarf_Unsigned i = 0;
    struct Dwarf_DN_Bucket_s *curbucket = 0;

    dbg = dn->dn_dbg;
    if (dn->dn_bucket_array) {
        return DW_DLV_OK;
    }
    if (!dn->dn_bucket_count) {
        return DW_DLV_NO_ENTRY;
    }
    dn->dn_bucket_array = (struct Dwarf_DN_Bucket_s*)
        calloc(dn->dn_bucket_count,sizeof(struct Dwarf_DN_Bucket_s));
    if (!dn->dn_bucket_array) {
        _dwarf_error_string(dbg,error,DW_DLE_ALLOC_FAIL,
            "DW_DLE_ALLOC_FAIL: "
            ".debug_names bucket array could not be allocated");
        return DW_DLV_ERROR;
    }

    curbucket = dn->dn_bucket_array ;
    for (i = 0 ; i < dn->dn_bucket_count; ++i) {
        Dwarf_Unsigned offsetval = 0;
        Dwarf_Small *ptr = dn->dn_buckets +
            i * DWARF_32BIT_SIZE;
        Dwarf_Small *endptr = dn->dn_buckets+
            dn->dn_bucket_count*DWARF_32BIT_SIZE;

        READ_UNALIGNED_CK(dbg, offsetval, Dwarf_Unsigned,
            ptr, DWARF_32BIT_SIZE,
            error,endptr);
        curbucket = dn->dn_bucket_array +i;
        curbucket->db_nameindex = offsetval;
    }
    for (i = 0; i < dn->dn_bucket_count; ) {
        Dwarf_Unsigned j = 0;

        curbucket = dn->dn_bucket_array+i;
        if (!curbucket->db_nameindex) {
            ++i;
            continue;
        }
        for (j = i+1; j < dn->dn_bucket_count; ++j) {
            struct Dwarf_DN_Bucket_s *partial =
                dn->dn_bucket_array+j;

            if (partial->db_nameindex) {
                curbucket->db_collisioncount =
                    partial->db_nameindex - curbucket->db_nameindex;
                    i = j;
                    break;
            }
        }
        if (j >= dn->dn_bucket_count) {
            /* Ran off end */
            curbucket->db_collisioncount =
                dn->dn_name_count - curbucket->db_nameindex;
            if (!curbucket->db_collisioncount) {
                curbucket->db_collisioncount = 1;
            }
            break;
        }
    }
    return DW_DLV_OK;
}

int
dwarf_dnames_bucket(Dwarf_Dnames_Head dn,
    Dwarf_Unsigned      bucket_number,
    Dwarf_Unsigned    * name_index,
    Dwarf_Unsigned    * collision_count,
    Dwarf_Error *       error)
{
    struct Dwarf_DN_Bucket_s *cur = 0;
    int res = 0;

    if (!dn || dn->dn_magic != DWARF_DNAMES_MAGIC) {
        _dwarf_error_string(NULL, error,DW_DLE_DBG_NULL,
            "DW_DLE_DBG_NULL: bad Head argument to "
            "dwarf_dnames_bucket");
        return DW_DLV_ERROR;
    }
    if (bucket_number >= dn->dn_bucket_count) {
        return DW_DLV_NO_ENTRY;
    }
    res  = _dwarf_initialize_bucket_details(dn,error);
    if (res == DW_DLV_ERROR) {
        return res;
    }
    if (!dn->dn_bucket_array) {
        return DW_DLV_NO_ENTRY;
    }
    cur = dn->dn_bucket_array + bucket_number;
    *name_index = cur->db_nameindex;
    *collision_count = cur->db_collisioncount;
    return DW_DLV_OK;
}
static int
get_hash_value_number(Dwarf_Dnames_Head dn,
    Dwarf_Unsigned name_index,
    Dwarf_Unsigned *hash_value,
    Dwarf_Error *error)
{
    Dwarf_Unsigned hash_val   = 0;
    int res                   = 0;
    Dwarf_Small * hashesentry = 0;
    Dwarf_Small *end          = 0;

    if (!dn->dn_bucket_count) {
        return DW_DLV_NO_ENTRY;
    }
    hashesentry = dn->dn_hash_table + (name_index-1)*DWARF_32BIT_SIZE;
    end = dn->dn_hash_table + DWARF_32BIT_SIZE*(dn->dn_name_count+1);
    res = read_uword_val(dn->dn_dbg,
        &hashesentry,
        end,
        DW_DLE_DEBUG_NAMES_OFF_END,
        &hash_val,
        0xffffffff, /* hashes fit in 32 bits */
        error);
    if (res != DW_DLV_OK) {
        return res;
    }
    *hash_value = hash_val;
    return DW_DLV_OK;
}

static int
get_bucket_number(Dwarf_Dnames_Head dn,
    Dwarf_Unsigned name_index,
    Dwarf_Unsigned *bucket_num)
{
    Dwarf_Unsigned i = 0;

    if (!dn->dn_bucket_count) {
        return DW_DLV_NO_ENTRY;
    }
    if (!dn->dn_bucket_array) {
        return DW_DLV_NO_ENTRY;
    }
    /*  Binary search would be better FIXME */
    for (i = 0; i < dn->dn_bucket_count; ++i) {
        Dwarf_Unsigned bindx = 0;
        Dwarf_Unsigned ccount = 0;
        Dwarf_Unsigned lastbindx = 0;
        struct Dwarf_DN_Bucket_s *cur = dn->dn_bucket_array +i;

        bindx = cur->db_nameindex;
        ccount = cur->db_collisioncount;
        lastbindx = ccount + bindx -1;
        if (name_index > lastbindx) {
            continue;
        }
        if (!bindx ) {
            /* empty bucket */
            continue;
        }
        if (bindx == name_index) {
            *bucket_num = i;
            return DW_DLV_OK;
        }
        if (name_index <= lastbindx) {
            *bucket_num = i;
            return DW_DLV_OK;
        }
    }
    /*  ? is it an error? */
    return DW_DLV_NO_ENTRY;
}

/*  We try to protect against bogus arguments,
    but all we can do is check for null.  */
int
dwarf_dnames_abbrevtable(Dwarf_Dnames_Head dn,
    Dwarf_Unsigned index,
    Dwarf_Unsigned *abbrev_offset,
    Dwarf_Unsigned *abbrev_code,
    Dwarf_Unsigned *abbrev_tag,
    Dwarf_Unsigned array_size,
    Dwarf_Half     * idxattr_array,
    Dwarf_Half     * form_array,
    Dwarf_Unsigned * attr_count)
{
    struct Dwarf_D_Abbrev_s *ab      = 0;
    Dwarf_Unsigned abnumber          = 0;
    Dwarf_Unsigned abmax             = 0;

    if (!dn) {
        return DW_DLV_NO_ENTRY;
    }
    if (!idxattr_array || !form_array) {
        return DW_DLV_NO_ENTRY;
    }
    if (index >= dn->dn_abbrev_instance_count) {
        return DW_DLV_NO_ENTRY;
    }
    ab = dn->dn_abbrev_instances + index;
    if (abbrev_offset) {
        *abbrev_offset = ab->da_abbrev_offset;
    }
    if (abbrev_code) {
        *abbrev_code = ab->da_abbrev_code;
    }
    if (abbrev_tag) {
        *abbrev_tag  = ab->da_tag;
    }
    abmax       = ab->da_pairs_count;
    if (attr_count) {
        *attr_count = abmax;
    }
    if (array_size < abmax) {
        abmax = array_size;
    }
    for ( ; abnumber < abmax; ++abnumber) {
        idxattr_array[abnumber] = ab->da_idxattr[abnumber];
        form_array[abnumber] = ab->da_form[abnumber];
    }
    return DW_DLV_OK;
}

static int
_dwarf_read_abbrev_code_from_pool(Dwarf_Dnames_Head dn,
    Dwarf_Unsigned  entrypooloffset,
    Dwarf_Unsigned *code,
    Dwarf_Error    *error)
{
    Dwarf_Small *epool = 0;
    Dwarf_Small *end   = 0;
    Dwarf_Debug dbg    = 0;
    int res            = 0;

    epool = dn->dn_entry_pool;
    end = epool + dn->dn_entry_pool_size;
    if (entrypooloffset >= dn->dn_entry_pool_size) {
        _dwarf_error_string(dbg,error,DW_DLE_DEBUG_NAMES_ERROR,
            "DW_DLE_DEBUG_NAMES_ERROR: "
            "The entry pool offset from the names table "
            " is out of bounds.");
        return DW_DLV_ERROR;
    }
    epool += entrypooloffset;
    dbg = dn->dn_dbg;
    res = _dwarf_read_uleb_ck(&epool,
        code,dbg,error,end);
    if (res == DW_DLV_ERROR) {
        return res;
    }
    return DW_DLV_OK;
}

/*  Starting out with a simple linear search.
    Better to use a dwarf_tsearch function. */
static int
_dwarf_find_abbrev_for_code(Dwarf_Dnames_Head dn,
    Dwarf_Unsigned code,
    struct Dwarf_D_Abbrev_s **abbrevdata,
    Dwarf_Error *error)
{
    Dwarf_Unsigned i = 0;
    struct Dwarf_D_Abbrev_s *ap =0;

    ap = dn->dn_abbrev_instances;
    for (i = 0; i < dn->dn_abbrev_instance_count; ++i,++ap) {
        if (ap->da_abbrev_code == code) {
            *abbrevdata = ap;
            return DW_DLV_OK;
        }
    }
    {
        Dwarf_Debug dbg = 0;
        dwarfstring m;

        dbg = dn->dn_dbg;
        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            "DW_DLE_DEBUG_NAMES_ERROR: "
            "The abbrev code %u",code);
        dwarfstring_append_printf_u(&m,
            "(0x%x) ",code);
        dwarfstring_append(&m,
            "from the entry pool "
            "is absent from the abbrev table.");
        _dwarf_error_string(dbg,error,DW_DLE_DEBUG_NAMES_ERROR,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
    }
    return DW_DLV_ERROR;
}

static int
_dwarf_fill_in_attr_form(Dwarf_Dnames_Head dn,
    struct Dwarf_D_Abbrev_s *abbrevdata,
    Dwarf_Half *idxattr_array,
    Dwarf_Half *form_array,
    Dwarf_Unsigned array_size,
    Dwarf_Error *error)
{
    Dwarf_Unsigned limit = abbrevdata->da_pairs_count;
    Dwarf_Unsigned i = 0;
    Dwarf_Debug dbg = 0;

    if (limit > array_size) {
        limit = array_size;
    }
    for ( ; i < limit ; ++i) {
        idxattr_array[i] = abbrevdata->da_idxattr[i];
        form_array[i] = abbrevdata->da_form[i];
    }
    if (i < limit) {
        dwarfstring m;

        dbg= dn->dn_dbg;
        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            "DW_DLE_DEBUG_NAMES_ERROR: "
            "At attr-form pair ",i);
        dwarfstring_append_printf_u(&m,
            ", with attr-form max of %u",
            abbrevdata->da_pairs_count);
        dwarfstring_append_printf_u(&m,
            ", and limit of of %u",
            limit);
        dwarfstring_append(&m,
            " something is very wrong. "
            "a pairs pointer is null");
        _dwarf_error_string(dbg,error,DW_DLE_DEBUG_NAMES_ERROR,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
    }
    return DW_DLV_OK;

}

/*  Each Name Table entry, one at a time.
    It is not an error if array_size is zero or
    small. Check the returned attr_count to
    know now much of array filled in and
    if the array you provided is
    large enough. Possibly 40 is
    sufficient.
    name indexes start at 1.  */
int
dwarf_dnames_name(Dwarf_Dnames_Head dn,
    Dwarf_Unsigned      name_index,
    Dwarf_Unsigned    * bucket_number,
    Dwarf_Unsigned    * hash_value,
    Dwarf_Unsigned    * offset_to_debug_str,
    char *            * ptrtostr ,
    Dwarf_Unsigned    * offset_in_entrypool,
    Dwarf_Unsigned    * abbrev_code,
    Dwarf_Half        * abbrev_tag,
    Dwarf_Unsigned      array_size,
    Dwarf_Half        * idxattr_array,
    Dwarf_Half        * form_array,
    Dwarf_Unsigned    * attr_count,
    Dwarf_Error *       error)
{
    Dwarf_Debug dbg                     = 0;
    int res                             = 0;
    Dwarf_Unsigned entrypooloffset      = 0;
    Dwarf_Unsigned debugstroffset       = 0;
    Dwarf_Small * strpointer            = 0;
    struct Dwarf_D_Abbrev_s *abbrevdata = 0;
    Dwarf_Unsigned code                 = 0;

    if (!dn || dn->dn_magic != DWARF_DNAMES_MAGIC) {
        _dwarf_error_string(NULL, error,DW_DLE_DBG_NULL,
            "DW_DLE_DBG_NULL: "
            "a call to dwarf_dnames_name() "
            "Passes in a NULL or uninitialized pointer");
        return DW_DLV_ERROR;
    }
    dbg = dn->dn_dbg;
    if (IS_INVALID_DBG(dbg)) {
        _dwarf_error_string(NULL, error,DW_DLE_DBG_NULL,
            "DW_DLE_DBG_NULL: "
            "a call to dwarf_dnames_name() "
            "finds a NULL Dwarf_Debug in a Dwarf_Dnames_Head");
        return DW_DLV_ERROR;
    }
    if (!name_index || name_index > dn->dn_name_count) {
        return DW_DLV_NO_ENTRY;
    }
    res = get_bucket_number(dn,name_index,bucket_number);
    if (res == DW_DLV_OK) {
        res = get_hash_value_number(dn,name_index,hash_value,error);
        if (res == DW_DLV_ERROR) {
            return res;
        }
    }
    {
        Dwarf_Small *ptr = dn->dn_string_offsets +
            (name_index-1) * dn->dn_offset_size;
        Dwarf_Small *endptr = dn->dn_abbrevs;

        READ_UNALIGNED_CK(dbg, debugstroffset, Dwarf_Unsigned,
            ptr, dn->dn_offset_size,
            error,endptr);
        if (offset_to_debug_str) {
            *offset_to_debug_str = debugstroffset;
        }
    }
    /* Get str ptr from .debug_str */
    {
        Dwarf_Small *secdataptr = 0;
        Dwarf_Unsigned secdatalen = 0;
        Dwarf_Small *secend = 0;
        int res_s = 0;

        secdataptr = (Dwarf_Small *)dbg->de_debug_str.dss_data;
        secdatalen = dbg->de_debug_str.dss_size;
        secend = secdataptr+secdatalen;
        strpointer = secdataptr +debugstroffset;
        res_s = _dwarf_check_string_valid(dbg,
            secdataptr,strpointer,secend,
            DW_DLE_FORM_STRING_BAD_STRING,error);
        if (res_s != DW_DLV_OK) {
            return res_s;
        }
        if (ptrtostr) {
            *ptrtostr = (char *)strpointer;
        }
    }

    {
        Dwarf_Small *ptr = dn->dn_entry_offsets +
            (name_index-1) * dn->dn_offset_size;
        Dwarf_Small *endptr = dn->dn_abbrevs;
        /*  offsets relative to the start of the
            entry_pool */

        READ_UNALIGNED_CK(dbg, entrypooloffset, Dwarf_Unsigned,
            ptr, dn->dn_offset_size,
            error,endptr);
        if (entrypooloffset >= dn->dn_entry_pool_size) {
            _dwarf_error_string(dbg, error,DW_DLE_DEBUG_NAMES_ERROR,
                "DW_DLE_DEBUG_NAMES_ERROR: "
                "The entrypool offset read is larger than"
                "the entrypool size");
            return DW_DLV_ERROR;
        }
        if (offset_in_entrypool) {
            *offset_in_entrypool = entrypooloffset;
        }
    }
    /*   Find abbrev code at the given entry offset */
    res = _dwarf_read_abbrev_code_from_pool(dn,
        entrypooloffset, &code,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    res = _dwarf_find_abbrev_for_code(dn,code,&abbrevdata,error);
    if (res == DW_DLV_ERROR) {
        return res;
    }
    if (res == DW_DLV_NO_ENTRY) {
        return res;
    }

    if (abbrev_code) {
        *abbrev_code =  code;
    }
    if (code && abbrev_tag) {
        *abbrev_tag =  (Dwarf_Half)abbrevdata->da_tag;
    }
    if (code) {
        if (attr_count) {
            *attr_count = abbrevdata->da_pairs_count;
        }
        res = _dwarf_fill_in_attr_form(dn,abbrevdata,idxattr_array,
            form_array, array_size,error);
        if (res == DW_DLV_ERROR) {
            return res;
        }
    }
    return DW_DLV_OK;
}

/*  If abbrev_code returned is zero there is no tag returned
    and we are at the end of the entry pool set for this name
    entry.
        abbrev code, tag
        attr,form
        ...
        0,0
        ... repeat like the above
        0
*/

static int
_dwarf_internal_abbrev_by_code(Dwarf_Dnames_Head dn,
    Dwarf_Unsigned  abbrev_code,
    Dwarf_Half     *tag,
    Dwarf_Unsigned *index_of_abbrev,
    Dwarf_Unsigned *number_of_attr_form_entries)
{
    Dwarf_Unsigned n = 0;
    struct Dwarf_D_Abbrev_s * abbrev = 0;

    abbrev = dn->dn_abbrev_instances;
    for (n = 0; n < dn->dn_abbrev_instance_count; ++n,++abbrev) {
        if (abbrev_code == abbrev->da_abbrev_code) {
            if (tag) {
                *tag = (Dwarf_Half)abbrev->da_tag;
            }
            if (index_of_abbrev) {
                *index_of_abbrev = n;
            }
            if (number_of_attr_form_entries) {
                *number_of_attr_form_entries = abbrev->da_pairs_count;
            }
            return DW_DLV_OK;
        }
    }
    /*  Something is wrong, not found! */
    return DW_DLV_NO_ENTRY;
}

/*  This, combined with dwarf_dnames_entrypool_values(),
    lets one examine as much or as little of an entrypool
    as one wants to by alternately calling these two
    functions.
*/

int dwarf_dnames_entrypool(Dwarf_Dnames_Head dn,
    Dwarf_Unsigned  offset_in_entrypool,
    Dwarf_Unsigned *abbrev_code,
    Dwarf_Half     *tag,
    Dwarf_Unsigned *value_count,
    Dwarf_Unsigned *index_of_abbrev,
    Dwarf_Unsigned *offset_of_initial_value,
    Dwarf_Error    *error)
{
    Dwarf_Debug dbg = 0;
    int res = 0;
    Dwarf_Small *entrypool = 0;
    Dwarf_Small *endentrypool = 0;
    Dwarf_Unsigned abcode = 0;
    Dwarf_Unsigned leblen = 0;
    if (!dn || dn->dn_magic != DWARF_DNAMES_MAGIC) {
        _dwarf_error(NULL, error,DW_DLE_DBG_NULL);
        return DW_DLV_ERROR;
    }

    dbg = dn->dn_dbg;
    if (offset_in_entrypool >= dn->dn_entry_pool_size) {
        _dwarf_error(NULL, error,DW_DLE_DEBUG_NAMES_ENTRYPOOL_OFFSET);
        return DW_DLV_ERROR;
    }
    endentrypool = dn->dn_entry_pool +dn->dn_entry_pool_size;
    entrypool = dn->dn_entry_pool + offset_in_entrypool;

    DECODE_LEB128_UWORD_LEN_CK(entrypool,abcode,leblen,
        dbg,error,endentrypool);

    res = _dwarf_internal_abbrev_by_code(dn,
        abcode,
        tag, index_of_abbrev,
        value_count);
    if (res != DW_DLV_OK) {
        /* Never DW_DLV_ERROR (so far) */
        return res;
    }
    *offset_of_initial_value = offset_in_entrypool + leblen;
    *abbrev_code = abcode;
    return DW_DLV_OK;
}

static int
isformrefval(Dwarf_Debug dbg,Dwarf_Half form,
    Dwarf_Small *poolptr,
    Dwarf_Small *endpool,
    Dwarf_Unsigned *val,
    Dwarf_Unsigned *bytesread,
    Dwarf_Error *error)
{
    Dwarf_Unsigned localval = 0;

    if (poolptr >= endpool) {
        dwarfstring m;
        const char * formname = 0;

        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            "DW_DLE_DEBUG_NAMES_OFF_END: "
            "Reading data of form 0x%02x ",form);
        dwarf_get_FORM_name((unsigned int)form,&formname);
        dwarfstring_append_printf_s(&m,
            "%s from entrypool would read "
            "off the end of the pool",
            (char *)formname);
        _dwarf_error_string(dbg,error,
            DW_DLE_DEBUG_NAMES_OFF_END,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    switch(form) {
    case DW_FORM_ref1:
        *val = *poolptr;
        *bytesread = 1;
        break;
    case DW_FORM_ref2:
        READ_UNALIGNED_CK(dbg, localval, Dwarf_Unsigned,
            poolptr, DWARF_HALF_SIZE,
            error,endpool);
        *bytesread = DWARF_HALF_SIZE;
        *val = localval;
        break;
    case DW_FORM_ref4:
        READ_UNALIGNED_CK(dbg, localval, Dwarf_Unsigned,
            poolptr, DWARF_32BIT_SIZE,
            error,endpool);
        *bytesread = DWARF_32BIT_SIZE;
        *val = localval;
        break;
    case DW_FORM_ref8:
    default:
        return DW_DLV_NO_ENTRY;
    }
    return DW_DLV_OK;
}
/*  Caller, knowing array size needed, passes in arrays
    it allocates of for idx, form, offset-size-values,
    and signature values.  Caller must examine idx-number
    and form to decide, for each array element, whether
    the offset or the signature contains the value.
    So this returns all the values for the abbrev code.
    And points via offset_of_next to the next abbrev code.

    While an array of structs would be easier for the caller
    to allocate than parallel arrays, public structs have
    turned out to be difficult to work with as interfaces
    (as formats change over time).
    */
int dwarf_dnames_entrypool_values(Dwarf_Dnames_Head dn,
    Dwarf_Unsigned   index_of_abbrev,
    Dwarf_Unsigned   offset_in_entrypool_of_values,
    Dwarf_Unsigned   array_sizes,
    Dwarf_Half     * array_dw_idx_number,
    Dwarf_Half     * array_form,
    Dwarf_Unsigned * array_of_offsets,
    Dwarf_Sig8     * array_of_signatures,
    Dwarf_Bool     * single_cu,
    Dwarf_Unsigned * single_cu_offset,
    /*  offset of the next entrypool entry. */
    Dwarf_Unsigned *    offset_of_next_entrypool,
    Dwarf_Error *       error)
{
    struct Dwarf_D_Abbrev_s * abbrev = 0;
    Dwarf_Debug               dbg = 0;
    unsigned                  n = 0;
    int                       res = 0;
    Dwarf_Unsigned            abcount = 0;
    Dwarf_Unsigned            pooloffset =
        offset_in_entrypool_of_values;
    Dwarf_Small             * endpool = 0;
    Dwarf_Small             * poolptr = 0;
    Dwarf_Unsigned            bytesread = 0;

    if (!dn || dn->dn_magic != DWARF_DNAMES_MAGIC) {
        _dwarf_error_string(NULL, error,DW_DLE_DBG_NULL,
            "DW_DLE_DBG_NULL: bad Head argument to "
            "dwarf_dnames_entrypool_values");
        return DW_DLV_ERROR;
    }
    dbg = dn->dn_dbg;
    endpool = dn->dn_entry_pool + dn->dn_entry_pool_size;
    if (pooloffset >= dn->dn_entry_pool_size) {
        /* make error or harmless error? */
        return DW_DLV_NO_ENTRY;
    }

    if (index_of_abbrev >= dn->dn_abbrev_instance_count) {
        /* make error or harmless error? */
        return DW_DLV_NO_ENTRY;
    }

    poolptr = dn->dn_entry_pool + pooloffset;
    abbrev = dn->dn_abbrev_instances + index_of_abbrev;
    abcount = abbrev->da_pairs_count;
    if (!abcount) {
        return DW_DLV_NO_ENTRY;
    }
    if (abcount > array_sizes) {
        dwarfstring m;

        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            "DW_DLE_DEBUG_NAMES_OFF_END: "
            "The size of the pair of arrays "
            "passed to dwarf_dnames_entrypool_values "
            "is %u",
            array_sizes);
        dwarfstring_append_printf_u(&m,
            " but the entry requires %u entries.",
            abcount);
        _dwarf_error_string(dbg,error,
            DW_DLE_DEBUG_NAMES_OFF_END,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    if (abcount > dn->dn_entry_pool_size) {
        /*  Just looking for a horrible giant value */
        dwarfstring m;

        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            "DW_DLE_DEBUG_NAMES_ABBREV_CORRUPTION: "
            "The abbrev count for this entry pool entry"
            " is %u, impossibly large. Corrupt data",
            abcount);
        _dwarf_error_string(dbg,error,
            DW_DLE_DEBUG_NAMES_ABBREV_CORRUPTION,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    for (n = 0; n < abcount ; ++n) {
        Dwarf_Half idxtype =  0;
        Dwarf_Half form = 0;

        idxtype = abbrev->da_idxattr[n];
        form = abbrev->da_form[n];
        array_dw_idx_number[n] = idxtype;
        array_form[n] = form;

        if (!idxtype && !form) {
            break;
        }

        if (form == DW_FORM_data8 && idxtype == DW_IDX_type_hash) {
            if ((poolptr + sizeof(Dwarf_Sig8)) > endpool){
                _dwarf_error(dbg,error,
                    DW_DLE_DEBUG_NAMES_ENTRYPOOL_OFFSET);
                return DW_DLV_ERROR;
            }
            bytesread = sizeof(Dwarf_Sig8);
            memcpy(array_of_signatures+n,
                poolptr,bytesread);
            poolptr += bytesread;
            pooloffset += bytesread;
            continue;
        } else if (_dwarf_allow_formudata(form)) {
            Dwarf_Unsigned val = 0;
            res = _dwarf_formudata_internal(dbg,0,form,poolptr,
                endpool,&val,&bytesread,error);
            if (res != DW_DLV_OK) {
                return res;
            }
            poolptr += bytesread;
            pooloffset += bytesread;

            array_of_offsets[n] = val;
            continue;
        } else if (form == DW_FORM_flag_present) {
            array_of_offsets[n] = 1;
            /* No change to poolptr or pooloffset */
            continue;
        } else {
            Dwarf_Unsigned val = 0;
            res =isformrefval(dbg,form,poolptr,
                endpool,&val,&bytesread,error);
            if (res == DW_DLV_ERROR) {
                return res;
            }
            if (res == DW_DLV_OK) {
                poolptr += bytesread;
                if (poolptr >= endpool) {
                    _dwarf_error_string(dbg,error,
                        DW_DLE_DEBUG_NAMES_ENTRYPOOL_OFFSET,
                        "DW_DLE_DEBUG_NAMES_ENTRYPOOL_OFFSET:"
                        " a DW_FORM_ref* would read past end"
                        " of the entrypool");
                    return DW_DLV_ERROR;
                }
                pooloffset += bytesread;
                array_of_offsets[n] = val;
                continue;
            }
        }
        /*  There is some mistake/omission in our code here or in
            the data. */
        {
        dwarfstring m;
        const char *name = "<unexpected form>";

        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            "DW_DLE_DEBUG_NAMES_UNHANDLED_FORM: Form 0x%x",
            form);
        dwarf_get_FORM_name(form,&name);
        dwarfstring_append_printf_s(&m,
            " %s is not currently supported for .debug_names ",
            (char *)name);
        _dwarf_error_string(dbg,error,
            DW_DLE_DEBUG_NAMES_UNHANDLED_FORM,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        }
        return DW_DLV_ERROR;
    }
    if ( dn->dn_single_cu) {
        if (single_cu && single_cu_offset) {
            *single_cu = dn->dn_single_cu;
            *single_cu_offset = dn->dn_single_cu_offset;
        }
    }
    *offset_of_next_entrypool = pooloffset;
    return DW_DLV_OK;
}
