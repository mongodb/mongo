/*
  Copyright (C) 2014-2021 David Anderson. All Rights Reserved.

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

/*  The file and functions have  'xu' because
    the .debug_cu_index and .debug_tu_index
    sections have the same layout and this deals with both.

    This is DebugFission, part of DWARF5.

    It allows fast section access in a .dwp object file
    with debug-information to locate offsets
    within and between sections.

    See the DWARF5 Standard: section 7.3.5 and
    examples in Appendix F.3.

    A note about the index field from the index table.
    See DWARF5 7.5.3.5.
    The index table array index values are [1,S)
    These value ae used to call functions requesting
    values from the offset table and size table.

    Inside the code in this file we subtract 1 and use
    0 origin as that is how we arranged the
    table access here.
    A zero in the index table is an unused signature
    table signature and unused index.

    By subtracting one and arranging things properly
    in the offset table and size table we can refer
    to the tables in an identical simple fashion
    These tables are thus U rows and N columns.
    Technically the Offset table physically
    row zero is a separate set of numbers translating
    the column number to a DW_SECT* value
    so callers can request specific bases(offsets)
    and sizes from the offset and size tables.
    But we change things a little internally so both
    tables look zero-origin.
*/

#include <config.h>

#include <string.h>  /* memcmp() memcpy() strcmp() */
#include <stdio.h>  /*  for printf (debugging) */

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
#include "dwarf_xu_index.h"
#include "dwarf_string.h"

#define  HASHSIGNATURELEN 8

/* zerohashkey used as all-zero-bits for comparison. */
static const Dwarf_Sig8 zerohashkey;

#if 0 /* dump_bytes */
static void
dump_bytes(char * msg,Dwarf_Small * start, long len)
{
    Dwarf_Small *end = start + len;
    Dwarf_Small *cur = start;

    printf("%s %p: ",msg, (void *)start);
    for (; cur < end; cur++) {
        printf("%02x ", *cur);
    }
    printf("\n");
}
#endif /*0*/

/*  Precondition: headerline_offset + N*32 is within
    the section. */
static int
fill_in_offsets_headerline(Dwarf_Debug dbg,
    Dwarf_Xu_Index_Header xuhdr,
    Dwarf_Unsigned headerline_offset,
    Dwarf_Unsigned num_sects,
    Dwarf_Error *error)
{
    Dwarf_Small *section_start = xuhdr->gx_section_data;
    Dwarf_Small *section_end = xuhdr->gx_section_data+
        xuhdr->gx_section_length;
    Dwarf_Small *data = 0;
    unsigned i = 0;

    data = section_start +headerline_offset;
    for ( ; i < num_sects ; ++i) {
        Dwarf_Unsigned v = 0;

        READ_UNALIGNED_CK(dbg,v, Dwarf_Unsigned,
            data,SIZEOFT32,
            error,section_end);
        data += SIZEOFT32;
        if (v > DW_SECT_RNGLISTS) {
            dwarfstring s;

            dwarfstring_constructor(&s);
            dwarfstring_append_printf_u(&s,
                "ERROR: DW_DLE_XU_NAME_COL_ERROR  The "
                "section number of %u ",v);
            dwarfstring_append(&s," is too high. "
                "Sections 1-8 are listed in "
                "DWARF5 Table 7.1.");
            _dwarf_error_string(dbg, error, DW_DLE_XU_NAME_COL_ERROR,
                dwarfstring_string(&s));
            dwarfstring_destructor(&s);
            return DW_DLV_ERROR;
        }
        xuhdr->gx_section_id[i] = (unsigned long)v;
    }
    return DW_DLV_OK;
}

/*  Read in a cu or tu section and
    return overview information.

    For libdwarf-internal lookups
    dwarf_init*() calls
    dwarf_get_xu_index_header() when
    the object file is opened and
    dwarf_dealloc_xu_header() is called
    by dwarf_finish(), there is
    no need for users to do this.

    If one wants to call the various
    tu/cu functions oneself (possibly to print the
    .debug_cu_index or .debug_tu_index sections).
    then you will need to call dwarf_get_xu_index_header()
    and eventually dwarf_dealloc_xu_header().

    The libdwarf-internal data is kept in Dwarf_Debug
    fields de_cu_hashindex_data/de_tu_hashindex_data.
*/
int
dwarf_get_xu_index_header(Dwarf_Debug dbg,
    /* Pass in section_type "tu" or "cu" */
    const char     *section_type,
    Dwarf_Xu_Index_Header *xuptr,
    Dwarf_Unsigned *version,
    Dwarf_Unsigned *number_of_columns, /* L section count.*/
    Dwarf_Unsigned *number_of_CUs,     /* U unit count    */
    Dwarf_Unsigned *number_of_slots,   /* S slot count    */
    /*  Standard says S > U DWARF5 sec 7.3.5.3 */
    const char    **section_name,
    Dwarf_Error    *error)
{
    Dwarf_Xu_Index_Header indexptr = 0;
    int res = DW_DLV_ERROR;
    struct Dwarf_Section_s *sect = 0;
    Dwarf_Unsigned local_version = 0;
    Dwarf_Unsigned num_secs  = 0;
    Dwarf_Unsigned num_CUs  = 0;
    Dwarf_Unsigned num_slots  = 0;
    Dwarf_Small   *data = 0;
    Dwarf_Unsigned tables_end_offset = 0;
    Dwarf_Unsigned hash_tab_offset = 0;
    Dwarf_Unsigned indexes_tab_offset = 0;
    Dwarf_Unsigned section_offsets_tab_offset = 0;
    Dwarf_Unsigned section_offsets_headerline_offset = 0;
    Dwarf_Unsigned section_sizes_tab_offset = 0;
    unsigned datalen32 = SIZEOFT32;
    Dwarf_Small *section_end = 0;

    CHECK_DBG(dbg,error,"dwarf_get_xu_index_header()");
    if (!section_type || !xuptr) {
        _dwarf_error_string(0,error,DW_DLE_XU_TYPE_ARG_ERROR,
            "DW_DLE_XU_TYPE_ARG_ERROR: section type or header "
            "return pointer is not valid");
        return DW_DLV_ERROR;
    }

    if (!strcmp(section_type,"cu") ) {
        sect = &dbg->de_debug_cu_index;
    } else if (!strcmp(section_type,"tu") ) {
        sect = &dbg->de_debug_tu_index;
    } else {
        _dwarf_error_string(dbg, error, DW_DLE_XU_TYPE_ARG_ERROR,
            "DW_DLE_XU_TYPE_ARG_ERROR, Passed in section type "
            "is neither \"tu\" nor \"cu\"");
        return DW_DLV_ERROR;
    }
    if (!sect->dss_size) {
        return DW_DLV_NO_ENTRY;
    }
    if (!sect->dss_data) {
        res = _dwarf_load_section(dbg, sect,error);
        if (res != DW_DLV_OK) {
            return res;
        }
    }

    data = sect->dss_data;
    section_end = data + sect->dss_size;

    if (sect->dss_size < (4*datalen32) ) {
        dwarfstring m;

        dwarfstring_constructor(&m);
        dwarfstring_append_printf_s(&m,
            "DW_DLE_ERRONEOUS_XU_INDEX_SECTION: "
            "The size of the %s ",
            (char *)section_type);
        dwarfstring_append_printf_u(&m,
            "is just %u bytes, much to small to be "
            " a correct section",
            sect->dss_size);
        _dwarf_error_string(dbg, error,
            DW_DLE_ERRONEOUS_XU_INDEX_SECTION,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    READ_UNALIGNED_CK(dbg,local_version, Dwarf_Unsigned,
        data,datalen32,
        error,section_end);
    data += datalen32;
    if (local_version < DW_CU_VERSION2 ||
        local_version > DW_CU_VERSION5) {
        /*  A GNU extension allowed debug_fission to
            apply to DWARF4, and even down to...DWARF2! */
        dwarfstring m;

        dwarfstring_constructor(&m);

        dwarfstring_append_printf_u(&m,"DW_DLE_XU_NAME_COL_ERROR: "
            "The CU/TU Index Section version is %u ",local_version);
        _dwarf_error_string(dbg,error,DW_DLE_XU_NAME_COL_ERROR,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }

    /* reading N, section_count */
    READ_UNALIGNED_CK(dbg,num_secs, Dwarf_Unsigned,
        data,datalen32,
        error,section_end);
    if (num_secs > DW_SECT_RNGLISTS) {
        dwarfstring m;

        dwarfstring_constructor(&m);
        dwarfstring_append_printf_s(&m,
            "DW_DLE_XU_NAME_COL_ERROR: "
            " %s index section header ",
            (char *)section_type);
        dwarfstring_append_printf_u(&m,
            "shows N, the sections count, "
            "as %u but only values "
            " 1 through 8 (DW_SECT_RNGLISTS) are valid.",
            num_secs);
        _dwarf_error_string(dbg,error,DW_DLE_XU_NAME_COL_ERROR,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    data += datalen32;
    /*  Reading U, unit_count, compilation-units or type-units
        depending on cu or tu */
    READ_UNALIGNED_CK(dbg,num_CUs, Dwarf_Unsigned,
        data,datalen32,
        error,section_end);
    data += datalen32;
    if (num_CUs >= sect->dss_size ||
        (num_CUs/datalen32) >= sect->dss_size) {
        _dwarf_error_string(dbg,error,DW_DLE_XU_NAME_COL_ERROR,
            "dbg,error,DW_DLE_XU_NAME_COL_ERROR: "
            "The CU/TU Index Section header unit count "
            "is too large to be real. Corrupt DWARF\n");
        return DW_DLV_ERROR;
    }
    /* reading S, slot_count, slots in the hash table */
    READ_UNALIGNED_CK(dbg,num_slots, Dwarf_Unsigned,
        data,datalen32,
        error,section_end);
    hash_tab_offset = datalen32*4;
    indexes_tab_offset = hash_tab_offset +
        (num_slots * HASHSIGNATURELEN);
    /*  Look for corrupt section data. */
    if (num_slots > sect->dss_size) {
        dwarfstring m;

        dwarfstring_constructor(&m);
        dwarfstring_append_printf_s(&m,
            "DW_DLE_ERRONEOUS_XU_INDEX_SECTION: "
            "The size of the %s ",(char *)section_type);
        dwarfstring_append_printf_u(&m,
            " is just %u bytes,",sect->dss_size);
        dwarfstring_append_printf_u(&m,
            "while the number of slots (S) is %u. "
            "which is clearly wrong",num_slots );
        _dwarf_error_string(dbg, error,
            DW_DLE_ERRONEOUS_XU_INDEX_SECTION,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    if ( (4*num_slots) > sect->dss_size) {
        dwarfstring m;

        dwarfstring_constructor(&m);
        dwarfstring_append_printf_s(&m,
            "DW_DLE_ERRONEOUS_XU_INDEX_SECTION: "
            "The size of the %s ",(char *)section_type);
        dwarfstring_append_printf_u(&m,
            " is just %u bytes,",sect->dss_size);
        dwarfstring_append_printf_u(&m,
            "while the number of slots bytes (S) is at least %u. "
            "which is clearly wrong",num_slots*4);
        _dwarf_error_string(dbg, error,
            DW_DLE_ERRONEOUS_XU_INDEX_SECTION,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }

    /*  This offset is to  1 row of N columns, each 32bit. */
    section_offsets_headerline_offset = indexes_tab_offset +
        (num_slots *datalen32);
    /*  Now we can make the real table part index normally.
        This offset is to  U row of N columns, each 32bit. */
    section_offsets_tab_offset = section_offsets_headerline_offset
        + (num_secs*datalen32);
    if ( num_secs > sect->dss_size) {
        dwarfstring m;

        dwarfstring_constructor(&m);
        dwarfstring_append_printf_s(&m,
            "DW_DLE_ERRONEOUS_XU_INDEX_SECTION: "
            "The size of the %s ",(char *)section_type);
        dwarfstring_append_printf_u(&m,
            " is just %u bytes,",sect->dss_size);
        dwarfstring_append_printf_u(&m,
            "while the number of sections/columns (S) is %u. "
            "which is clearly wrong",num_secs );
        _dwarf_error_string(dbg, error,
            DW_DLE_ERRONEOUS_XU_INDEX_SECTION,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    if ( (datalen32*num_secs) > sect->dss_size) {
        dwarfstring m;

        dwarfstring_constructor(&m);
        dwarfstring_append_printf_s(&m,
            "DW_DLE_ERRONEOUS_XU_INDEX_SECTION: "
            "The size of the %s ",(char *)section_type);
        dwarfstring_append_printf_u(&m,
            " is just %u bytes,",sect->dss_size);
        dwarfstring_append_printf_u(&m,
            "while the number of sections/columns bytes (S)"
            " is at least %u. "
            "which is clearly wrong",num_secs*4);
        _dwarf_error_string(dbg, error,
            DW_DLE_ERRONEOUS_XU_INDEX_SECTION,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    section_sizes_tab_offset = section_offsets_tab_offset +
        (num_CUs *num_secs* datalen32) ;
    tables_end_offset = section_sizes_tab_offset +
        (num_CUs * num_secs * datalen32);
    if ( tables_end_offset > sect->dss_size) {
        /* Something is badly wrong here. */
        dwarfstring m;

        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,"ERROR: "
            "DW_DLE_ERRONEOUS_XU_INDEX_SECTION as the end offset "
            "0x%lx is greater than ",tables_end_offset);
        dwarfstring_append_printf_u(&m,"the section size "
            "0x%lx.",sect->dss_size);
        _dwarf_error_string(dbg, error,
            DW_DLE_ERRONEOUS_XU_INDEX_SECTION,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    indexptr = (Dwarf_Xu_Index_Header)
        _dwarf_get_alloc(dbg,DW_DLA_XU_INDEX,1);
    if (indexptr == NULL) {
        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
        return DW_DLV_ERROR;
    }
    /*  Only "cu" or "tu" allowed, that is checked above.
        But for safety we just copy the allowed bytes*/
    indexptr->gx_type[0] = section_type[0];
    indexptr->gx_type[1] = section_type[1];
    indexptr->gx_type[2] = 0;
    indexptr->gx_dbg = dbg;
    indexptr->gx_section_length = sect->dss_size;
    indexptr->gx_section_data   = sect->dss_data;
    indexptr->gx_section_name   = sect->dss_name;
    indexptr->gx_version        = local_version;
    indexptr->gx_column_count_sections = num_secs;
    indexptr->gx_units_in_index = num_CUs;
    indexptr->gx_slots_in_hash  = num_slots;
    indexptr->gx_hash_table_offset  =  hash_tab_offset;
    indexptr->gx_index_table_offset = indexes_tab_offset;
    indexptr->gx_section_offsets_headerline_offset=
        section_offsets_headerline_offset;
    indexptr->gx_section_offsets_offset= section_offsets_tab_offset;
    indexptr->gx_section_sizes_offset  = section_sizes_tab_offset;
    res = fill_in_offsets_headerline(dbg,indexptr,
        section_offsets_headerline_offset,
        num_secs,error);
    if (res != DW_DLV_OK) {
        dwarf_dealloc(dbg,indexptr,DW_DLA_XU_INDEX);
        return res;
    }
    *xuptr             =     indexptr;
    *version           = indexptr->gx_version;
    *number_of_columns = indexptr->gx_column_count_sections;
    *number_of_CUs     = indexptr->gx_units_in_index;
    *number_of_slots   = indexptr->gx_slots_in_hash;
    *section_name      = indexptr->gx_section_name;
    return DW_DLV_OK;
}

int dwarf_get_xu_index_section_type(Dwarf_Xu_Index_Header xuhdr,
    /*  the function returns a pointer to
        the immutable string "tu" or "cu" via
        this arg. Do not free.  */
    const char **typename,
    /*  the function returns a pointer to
        the immutable section name. Do not free.
        .debug_cu_index or .debug_tu_index */
    const char **sectionname,
    Dwarf_Error *error)
{
    (void)error;
    *typename    = &xuhdr->gx_type[0];
    *sectionname = xuhdr->gx_section_name;
    return DW_DLV_OK;
}

/*  Index values 0 to S-1 are valid. */
int dwarf_get_xu_hash_entry(Dwarf_Xu_Index_Header xuhdr,
    Dwarf_Unsigned  index,
    /* returns the hash value. 64 bits. */
    Dwarf_Sig8     *hash_value,

    /* returns the index into rows of offset/size tables. */
    Dwarf_Unsigned *index_to_sections,
    Dwarf_Error    *error)
{
    Dwarf_Debug dbg = xuhdr->gx_dbg;
    Dwarf_Small *hashtab = xuhdr->gx_section_data +
        xuhdr->gx_hash_table_offset;
    Dwarf_Small *indextab = xuhdr->gx_section_data +
        xuhdr->gx_index_table_offset;
    Dwarf_Small *indexentry = 0;
    Dwarf_Small *hashentry = 0;
    Dwarf_Unsigned indexval = 0;
    Dwarf_Small *section_end = xuhdr->gx_section_data +
        xuhdr->gx_section_length;

    if (!xuhdr->gx_slots_in_hash) {
        _dwarf_error_string(dbg, error,  DW_DLE_XU_HASH_ROW_ERROR,
            "DW_DLE_XU_HASH_ROW_ERROR the number of slots is zero"
            " which seems wrong.");
        return DW_DLV_ERROR;
    }
    if (index >= xuhdr->gx_slots_in_hash) {
        return DW_DLV_NO_ENTRY;
    }
    hashentry = hashtab + (index * HASHSIGNATURELEN);
    memcpy(hash_value,hashentry,HASHSIGNATURELEN);
    indexentry = indextab + (index * SIZEOFT32);
    if (indexentry >= section_end ||
        (indexentry+SIZEOFT32) > section_end) {
        dwarfstring m;

        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            "DW_DLE_XU_HASH_ROW_ERROR the index passed in, "
            " %u, results in entry not fitting"
            " in the hash table (past end of section)",index);
        _dwarf_error_string(dbg, error,  DW_DLE_XU_HASH_ROW_ERROR,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    READ_UNALIGNED_CK(dbg,indexval,Dwarf_Unsigned, indexentry,
        SIZEOFT32,
        error,section_end);
    if (indexval > xuhdr->gx_units_in_index) {
        _dwarf_error(dbg, error,  DW_DLE_XU_HASH_INDEX_ERROR);
        return DW_DLV_ERROR;
    }
    *index_to_sections = indexval;
    return DW_DLV_OK;
}

static const char * dwp_secnames[] = {
"No name for zero",
"DW_SECT_INFO"        /* 1 */ /*".debug_info.dwo"*/,
"DW_SECT_TYPES"       /* 2 */ /*".debug_types.dwo"*/,
"DW_SECT_ABBREV"      /* 3 */ /*".debug_abbrev.dwo"*/,
"DW_SECT_LINE"        /* 4 */ /*".debug_line.dwo"*/,
"DW_SECT_LOC"         /* 5 */ /*".debug_loclists.dwo"*/,
"DW_SECT_STR_OFFSETS" /* 6 */ /*".debug_str_offsets.dwo"*/,
"DW_SECT_MACRO"       /* 7 */ /*".debug_macro.dwo"*/,
"DW_SECT_RNGLISTS"       /* 8 */ /*".debug_rnglists.dwo"*/,
"No name > 8",
};

static int
_dwarf_find_xu_target_sec_size(Dwarf_Debug dbg,
    Dwarf_Xu_Index_Header xuhdr,
    Dwarf_Unsigned  column_index,
    Dwarf_Unsigned  *target_sec_size,
    Dwarf_Error *error)
{
    Dwarf_Unsigned sec_num = 0;
    struct  Dwarf_Section_s *secptr = 0;

    if ( column_index >= xuhdr->gx_column_count_sections) {
        dwarfstring s;

        dwarfstring_constructor(&s);
        dwarfstring_append_printf_u(&s,
            "ERROR: DW_DLE_XU_NAME_COL_ERROR as the "
            "column index of %u ",column_index);
        dwarfstring_append_printf_u(&s," is too high. "
            "There are %u sections.",
            xuhdr->gx_column_count_sections);
        _dwarf_error_string(dbg, error, DW_DLE_XU_NAME_COL_ERROR,
            dwarfstring_string(&s));
        dwarfstring_destructor(&s);
        return DW_DLV_ERROR;
    }
    sec_num = xuhdr->gx_section_id[column_index];
    switch(sec_num) {
    case DW_SECT_INFO:
        secptr = &dbg->de_debug_info;
        break;
    case DW_SECT_TYPES:
        secptr = &dbg->de_debug_types;
        break;
    case DW_SECT_ABBREV:
        secptr = &dbg->de_debug_abbrev;
        break;
    case DW_SECT_LINE:
        secptr = &dbg->de_debug_line;
        break;
    case DW_SECT_LOCLISTS:
        /* May not be loaded */
        if (xuhdr->gx_version < DW_CU_VERSION5) {
            secptr = &dbg->de_debug_loc;
        } else {
            secptr = &dbg->de_debug_loclists;
        }
        break;
    case DW_SECT_STR_OFFSETS:
        /* May not be loaded */
        secptr = &dbg->de_debug_str_offsets;
        break;
    case DW_SECT_MACRO:
        /* May not be loaded */
        secptr = &dbg->de_debug_str_offsets;
        break;
    case DW_SECT_RNGLISTS:
        /* May not be loaded */
        if (xuhdr->gx_version < DW_CU_VERSION5) {
            secptr = &dbg->de_debug_ranges;
        } else {
            secptr = &dbg->de_debug_rnglists;
        }
        break;
    default:
        {
            dwarfstring s;

            dwarfstring_constructor(&s);
            dwarfstring_append_printf_u(&s,
                "DW_DLE_XU_NAME_COL_ERROR the .debug_cu/tu_index "
                "section has an unexpected DW_SECT  number "
                "of %u.",sec_num);
            _dwarf_error_string(dbg,error,DW_DLE_XU_NAME_COL_ERROR,
                dwarfstring_string(&s));
            dwarfstring_destructor(&s);
            return DW_DLV_ERROR;
        }
    }
    if (!secptr->dss_size) {
        int res = 0;

        /*  Perhaps the section not yet loaded. */
        res = _dwarf_load_section(dbg,secptr,error);
        if (res == DW_DLV_ERROR) {
            dwarf_dealloc_error(dbg,*error);
            *error = 0;
            return DW_DLV_NO_ENTRY;
        } else {
            if (res == DW_DLV_NO_ENTRY) {
                /*  Sometimes GNU extension ( DWARF2-4)
                    is simply missing a section named in fission */
                return res;
            }
        }
    }
    *target_sec_size =secptr->dss_size;
    return DW_DLV_OK;
}

/*  Row 0 of the Table of Section Offsets,
    columns 0 to N-1,  are the section id's,
    and names, such as DW_SECT_INFO (ie, 1)  */
int
dwarf_get_xu_section_names(Dwarf_Xu_Index_Header xuhdr,
    Dwarf_Unsigned  column_index,
    Dwarf_Unsigned *number,
    const char    **name,
    Dwarf_Error    *error)
{
    Dwarf_Unsigned sec_num = 0;
    Dwarf_Debug dbg = 0;
    if (!xuhdr) {
        _dwarf_error_string(0,error,DW_DLE_XU_TYPE_ARG_ERROR,
            "DW_DLE_XU_TYPE_ARG_ERROR: "
            "Dwarf_Xu_Index_Header is NULL");
        return DW_DLV_ERROR;
    }
    dbg = xuhdr->gx_dbg;
/* FIXME */
    CHECK_DBG(dbg,error,"dwarf_get_xu_section_names()");
    if ( column_index >= xuhdr->gx_column_count_sections) {
        dwarfstring s;

        dwarfstring_constructor(&s);
        dwarfstring_append_printf_u(&s,
            "ERROR: DW_DLE_XU_NAME_COL_ERROR as the "
            "column index of %u ",column_index);
        dwarfstring_append_printf_u(&s," is too high. "
            "There are %u sections.",
            xuhdr->gx_column_count_sections);
        _dwarf_error_string(dbg, error, DW_DLE_XU_NAME_COL_ERROR,
            dwarfstring_string(&s));
        dwarfstring_destructor(&s);
        return DW_DLV_ERROR;
    }
    sec_num = xuhdr->gx_section_id[column_index];
    if (sec_num < 1) {
        return DW_DLV_NO_ENTRY;
    }
    *number = sec_num;
    *name =  dwp_secnames[sec_num];
    return DW_DLV_OK;
}

static Dwarf_Bool
_dwarf_valid_col_index(Dwarf_Unsigned i,Dwarf_Unsigned count)
{
    if (i > count) {
        return FALSE;
    }
    return TRUE;
}
/*  Rows 0 to U-1
    col 0 to L-1
    are section offset and length values from
    the Table of Section Offsets and Table of Section Sizes.
    The formally the table of section offsets is a header
    line of the section offsets we subtract 1 from
    the incoming irow_index as our tables (irow_index and
    column_index are now zero origin. */
int
dwarf_get_xu_section_offset(Dwarf_Xu_Index_Header xuhdr,
    Dwarf_Unsigned  irow_index  /* one-origin */,
    Dwarf_Unsigned  column_index/* zero-origin */,
    Dwarf_Unsigned *sec_offset,
    Dwarf_Unsigned *sec_size,
    Dwarf_Error    *error)
{
    /* We use zero origin in the arrays, Users see
        one origin from the hash table. */
    Dwarf_Debug    dbg = 0;
    /* get to base of tables first. */
    Dwarf_Small   *offsetrow =  0;
    Dwarf_Small   *sizerow =  0;
    Dwarf_Small   *offsetentry = 0;
    Dwarf_Small   *sizeentry =  0;
    Dwarf_Unsigned offset = 0;
    Dwarf_Unsigned size = 0;
    Dwarf_Unsigned column_count = 0;
    Dwarf_Small   *section_end = 0;
    Dwarf_Unsigned row_index = irow_index-1;
    Dwarf_Unsigned secsize = 0;
    /*  These 4 are to help in sanity checks.*/
    Dwarf_Unsigned ofrow = 0;
    Dwarf_Unsigned ofentry = 0;
    Dwarf_Unsigned sirow = 0;
    Dwarf_Unsigned sientry = 0;
    int            res     = 0;
    Dwarf_Unsigned target_sec_size;

    if (!xuhdr) {
        _dwarf_error_string(0,error,DW_DLE_XU_TYPE_ARG_ERROR,
            "DW_DLE_XU_TYPE_ARG_ERROR: "
            "Dwarf_Xu_Index_Header pointer is null");
        return DW_DLV_ERROR;
    }
    dbg = xuhdr->gx_dbg;
    CHECK_DBG(dbg,error,"dwarf_get_xu_section_offset()");
    sizerow =  xuhdr->gx_section_sizes_offset +
        xuhdr->gx_section_data;
    offsetrow =  xuhdr->gx_section_offsets_offset +
        xuhdr->gx_section_data;
    column_count = xuhdr->gx_column_count_sections;
    secsize =  xuhdr->gx_section_length;
    section_end = xuhdr->gx_section_data +
        xuhdr->gx_section_length;
    if (!_dwarf_valid_col_index(column_index,column_count)) {
        dwarfstring s;

        dwarfstring_constructor(&s);
        dwarfstring_append_printf_u(&s,"DW_DLE_XU_TYPE_ARG_ERROR: "
            " dwarf_get_xu_section_offset was passed an invalid"
            " cu/tu column index of %u",column_index);
        _dwarf_error_string(dbg,error,DW_DLE_XU_TYPE_ARG_ERROR,
            dwarfstring_string(&s));
        dwarfstring_destructor(&s);
        return DW_DLV_ERROR;
    }

    if (!irow_index) {
        dwarfstring s;

        dwarfstring_constructor(&s);
        dwarfstring_append(&s,
            "ERROR: DW_DLE_ERRONEOUS_XU_INDEX_SECTION "
            "The row index passed to dwarf_get_xu_section_offset() "
            "is zero, which is not a valid row in "
            " the offset-table or the size table as we think"
            " of them as 1-origin.");
        _dwarf_error_string(dbg, error, DW_DLE_XU_NAME_COL_ERROR,
            dwarfstring_string(&s));
        dwarfstring_destructor(&s);
        return DW_DLV_ERROR;
    }
    /*  Having subtracted 1, check the zero-origin value */
    if (row_index >= xuhdr->gx_units_in_index) {
        dwarfstring s;

        dwarfstring_constructor(&s);
        dwarfstring_append_printf_u(&s,
            "ERROR: DW_DLE_XU_NAME_COL_ERROR as the "
            "row index of %u ",row_index);
        dwarfstring_append_printf_u(&s," is too high. "
            "Valid units must be < %u ",xuhdr->gx_units_in_index);
        _dwarf_error_string(dbg, error, DW_DLE_XU_NAME_COL_ERROR,
            dwarfstring_string(&s));
        dwarfstring_destructor(&s);
        return DW_DLV_ERROR;
    }

    if (column_index >= column_count) {
        dwarfstring s;

        dwarfstring_constructor(&s);
        dwarfstring_append_printf_u(&s,
            "ERROR: DW_DLE_XU_NAME_COL_ERROR as the "
            "column index of %u ",column_index);
        dwarfstring_append_printf_u(&s," is too high. "
            "Valid column indexes  must be < %u ",
            column_count);
        _dwarf_error_string(dbg, error, DW_DLE_XU_NAME_COL_ERROR,
            dwarfstring_string(&s));
        dwarfstring_destructor(&s);
        return DW_DLV_ERROR;
    }
    /*  As noted above we have hidden the extra initial
        row from the offsets table so it is just
        0 to U-1. */
    ofrow = row_index*column_count * SIZEOFT32;
    ofentry = column_index *  SIZEOFT32;
    offsetrow = offsetrow + ofrow;
    offsetentry = offsetrow + ofentry;
    if (ofrow >= secsize) {
        _dwarf_error_string(dbg,error,DW_DLE_XU_NAME_COL_ERROR,
            "DW_DLE_XU_NAME_COL_ERROR row offset past end of"
            " section.");
        return DW_DLV_ERROR;
    }
    if (ofentry >= secsize) {
        _dwarf_error_string(dbg,error,DW_DLE_XU_NAME_COL_ERROR,
            "DW_DLE_XU_NAME_COL_ERROR entry offset past end of"
            " section.");
        return DW_DLV_ERROR;
    }

    sirow =  row_index*column_count * SIZEOFT32;
    sientry = column_index *  SIZEOFT32;
    sizerow = sizerow + sirow;
    sizeentry = sizerow + sientry;
    if (sirow > secsize) {
        _dwarf_error_string(dbg,error,DW_DLE_XU_NAME_COL_ERROR,
            "DW_DLE_XU_NAME_COL_ERROR row size past end of"
            " section.");
        return DW_DLV_ERROR;
    }
    if (sientry > secsize) {
        _dwarf_error_string(dbg,error,DW_DLE_XU_NAME_COL_ERROR,
            "DW_DLE_XU_NAME_COL_ERROR entry size larger than"
            " section.");
        return DW_DLV_ERROR;
    }
    {
        READ_UNALIGNED_CK(dbg,offset,Dwarf_Unsigned,
            offsetentry,
            SIZEOFT32,error,section_end);
        offsetentry += SIZEOFT32;

        READ_UNALIGNED_CK(dbg,size,Dwarf_Unsigned,
            sizeentry,
            SIZEOFT32,error,section_end);
        sizeentry += SIZEOFT32;
    }
    /*  These are offset-in and size-of a section which
        is separate from .debug_cu/tu_index sections.
        _cu_/_tu_ */
    if (!size) {
        /*  If zero, this entry makes no contribution
            to a section in the package file.
            DWARF5 Sec 7.3.5.3   page 194. */
        *sec_offset = offset;
        *sec_size =  size;
        return DW_DLV_OK;
    }
    res = _dwarf_find_xu_target_sec_size(dbg, xuhdr,
        column_index,
        &target_sec_size,error);
    if (res == DW_DLV_ERROR) {
        return res;
    }
    if (res == DW_DLV_NO_ENTRY) {
        /*  These are not precise tests.  */
        if (offset > dbg->de_filesize) {
            _dwarf_error_string(dbg,error,DW_DLE_XU_NAME_COL_ERROR,
                "DW_DLE_XU_NAME_COL_ERROR area offset larger than"
                " file size.");
            return DW_DLV_ERROR;
        }
        if (size > dbg->de_filesize) {
            _dwarf_error_string(dbg,error,DW_DLE_XU_NAME_COL_ERROR,
                "DW_DLE_XU_NAME_COL_ERROR area size larger than"
                " file size.");
            return DW_DLV_ERROR;
        }
    } else {
        if (offset && (offset >= target_sec_size)) {
            dwarfstring m;

            dwarfstring_constructor(&m);
            dwarfstring_append_printf_u(&m,
                "DW_DLE_XU_NAME_COL_ERROR area offset 0x%x ",offset);
            dwarfstring_append_printf_u(&m," larger than"
                " target section size of 0x%x.",target_sec_size);
            _dwarf_error_string(dbg,error,DW_DLE_XU_NAME_COL_ERROR,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
        if (size > target_sec_size) {
            dwarfstring m;
            dwarfstring_constructor(&m);

            dwarfstring_append_printf_u(&m,
                "DW_DLE_XU_NAME_COL_ERROR area size 0x%x ",size);
            dwarfstring_append_printf_u(&m," larger than"
                " target section size of 0x%x.",target_sec_size);
            _dwarf_error_string(dbg,error,DW_DLE_XU_NAME_COL_ERROR,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
    }
    *sec_offset = offset;
    *sec_size =  size;
    return DW_DLV_OK;
}

static int
_dwarf_search_fission_for_key(Dwarf_Debug dbg,
    Dwarf_Xu_Index_Header xuhdr,
    Dwarf_Sig8 *key_in,
    Dwarf_Unsigned * percu_index_out,
    Dwarf_Error *error)
{
    Dwarf_Unsigned key = 0;
    Dwarf_Unsigned slots =  xuhdr->gx_slots_in_hash;
    Dwarf_Sig8 hashentry_key;
    Dwarf_Unsigned percu_index = 0;
    Dwarf_Unsigned h = 0;

    (void)dbg;
    hashentry_key = zerohashkey;
    /*  Look for corrupt section data. */
    if (slots > xuhdr->gx_section_length) {
        dwarfstring s;

        dwarfstring_constructor(&s);
        dwarfstring_append_printf_u(&s,
            "ERROR: DW_DLE_XU_NAME_COL_ERROR as the "
            "slots count of %u ",slots);
        dwarfstring_append_printf_u(&s," is too high. "
            "given the section length of %u\n",
            xuhdr->gx_section_length);
        _dwarf_error_string(dbg, error, DW_DLE_XU_NAME_COL_ERROR,
            dwarfstring_string(&s));
        dwarfstring_destructor(&s);
        return DW_DLV_ERROR;
    }
    if (!slots) {
        return DW_DLV_NO_ENTRY;
    }
    if ( (4*slots) > xuhdr->gx_section_length) {
        dwarfstring s;

        dwarfstring_constructor(&s);
        dwarfstring_append_printf_u(&s,
            "ERROR: DW_DLE_XU_NAME_COL_ERROR as the "
            "slots count *4 of %u ",slots*4);
        dwarfstring_append_printf_u(&s," is too high. "
            "given the section length of %u\n",
            xuhdr->gx_section_length);
        _dwarf_error_string(dbg, error, DW_DLE_XU_NAME_COL_ERROR,
            dwarfstring_string(&s));
        dwarfstring_destructor(&s);
        return DW_DLV_ERROR;
    }
    if (sizeof(key) != sizeof(*key_in)) {
        /* The hash won't work right in this case */
        dwarfstring s;

        dwarfstring_constructor(&s);
        dwarfstring_append_printf_u(&s,
            "DW_DLE_XU_HASH_ROW_ERROR "
            "We require 8 byte keys, not %u byte keys",
            sizeof(key));
        _dwarf_error_string(dbg, error, DW_DLE_XU_HASH_ROW_ERROR,
            dwarfstring_string(&s));
        dwarfstring_destructor(&s);
        return DW_DLV_ERROR;
    }
    ASNARL(key,key_in,sizeof(*key_in));
    /*  If we built a lookup based on hash key
        this loop would not be necessary. */
    for (h = 0; h < slots; ++h) {
        int res = 0;

        res = dwarf_get_xu_hash_entry(xuhdr,
            h,&hashentry_key,
            &percu_index,error);
        if (res != DW_DLV_OK) {
            return res;
        }
        if (percu_index == 0 &&
            !memcmp(&hashentry_key,&zerohashkey,sizeof(Dwarf_Sig8))) {
            continue;
        }
        if (!memcmp(key_in,&hashentry_key,sizeof(Dwarf_Sig8))) {
            /* FOUND */
            *percu_index_out = percu_index;
            return  DW_DLV_OK;
        }
    }
    return DW_DLV_NO_ENTRY;
}

/*  Slow. Consider tsearch. */
/*  For type units and for CUs. */
/*  We're finding an index entry refers
    to a global offset in some CU
    and hence is unique in the target. */
static int
_dwarf_search_fission_for_offset(Dwarf_Debug dbg,
    Dwarf_Xu_Index_Header xuhdr,
    Dwarf_Unsigned offset,
    Dwarf_Unsigned dfp_sect_num, /* DW_SECT_INFO or TYPES */
    Dwarf_Unsigned * percu_index_out,
    Dwarf_Sig8 * key_out,
    Dwarf_Error *error)
{
    Dwarf_Unsigned i = 0;
    Dwarf_Unsigned m = 0;
    Dwarf_Unsigned secnum_index = 0;
    Dwarf_Bool     found_secnum = FALSE;
    int res = 0;

    for ( i = 0; i< xuhdr->gx_column_count_sections; i++) {
        /*  We could put the secnums array into xuhdr
            if looping here is too slow. */
        const char *name = 0;
        Dwarf_Unsigned num = 0;
        res = dwarf_get_xu_section_names(xuhdr,i,&num,&name,error);
        if (res != DW_DLV_OK) {
            return res;
        }
        if (num == dfp_sect_num) {
            secnum_index = i;
            found_secnum = TRUE;
            break;
        }
    }
    if (FALSE == found_secnum) {
        _dwarf_error(dbg,error,DW_DLE_FISSION_SECNUM_ERR);
        return DW_DLV_ERROR;
    }
    for ( m = 0; m < xuhdr->gx_slots_in_hash; ++m) {
        Dwarf_Sig8 hash;
        Dwarf_Unsigned indexn = 0;
        Dwarf_Unsigned sec_offset = 0;
        Dwarf_Unsigned sec_size = 0;

        res = dwarf_get_xu_hash_entry(xuhdr,m,&hash,&indexn,error);
        if (res != DW_DLV_OK) {
            return res;
        }
        if (indexn == 0 &&
            !memcmp(&hash,&zerohashkey,sizeof(Dwarf_Sig8))) {
            /* Empty slot. */
            continue;
        }

        res = dwarf_get_xu_section_offset(xuhdr,
            indexn,secnum_index,&sec_offset,&sec_size,error);
        if (res != DW_DLV_OK) {
            return res;
        }
        if (sec_offset != offset) {
            continue;
        }
        *percu_index_out = indexn;
        *key_out = hash;
        return DW_DLV_OK;
    }
    return DW_DLV_NO_ENTRY;
}

static int
_dwarf_get_xuhdr(Dwarf_Debug dbg,
    const char *sigtype,
    Dwarf_Xu_Index_Header *xuout,
    Dwarf_Error *error)
{
    if (!strcmp(sigtype,"tu")) {
        if (!dbg->de_tu_hashindex_data) {
            return DW_DLV_NO_ENTRY;
        }
        *xuout = dbg->de_tu_hashindex_data;
    } else if (!strcmp(sigtype,"cu")) {
        if (!dbg->de_cu_hashindex_data) {
            return DW_DLV_NO_ENTRY;
        }
        *xuout = dbg->de_cu_hashindex_data;
    } else {
        _dwarf_error(dbg,error,DW_DLE_SIG_TYPE_WRONG_STRING);
        return DW_DLV_ERROR;
    }
    return DW_DLV_OK;

}

static int
transform_xu_to_dfp(Dwarf_Xu_Index_Header xuhdr,
    Dwarf_Unsigned percu_index,
    Dwarf_Sig8 *key,
    const char *sig_type,
    Dwarf_Debug_Fission_Per_CU *  percu_out,
    Dwarf_Error *error)
{
    Dwarf_Unsigned i = 0;
    Dwarf_Unsigned l = 0;
    Dwarf_Unsigned n = 1;
    Dwarf_Unsigned max_cols =
        xuhdr->gx_column_count_sections;/* L */
    Dwarf_Unsigned secnums[DW_FISSION_SECT_COUNT];
    int res = 0;

    for ( i = 0; i< max_cols; i++) {
        /*  We could put the secnums array into xuhdr
            if recreating it is too slow. */
        const char *name = 0;
        Dwarf_Unsigned num = 0;
        res = dwarf_get_xu_section_names(xuhdr,i,&num,&name,error);
        if (res != DW_DLV_OK) {
            return res;
        }
        secnums[i] = num;
    }
    n = percu_index;
    for (l = 0; l < max_cols; ++l) {  /* L */
        Dwarf_Unsigned sec_off = 0;
        Dwarf_Unsigned sec_size = 0;
        Dwarf_Unsigned l_as_sect = secnums[l];
        res = dwarf_get_xu_section_offset(xuhdr,n,l,
            &sec_off,&sec_size,error);
        if (res != DW_DLV_OK) {
            return res;
        }
        percu_out->pcu_offset[l_as_sect] = sec_off;
        percu_out->pcu_size[l_as_sect] = sec_size;
    }
    percu_out->pcu_type = sig_type;
    percu_out->pcu_index = percu_index;
    percu_out->pcu_hash = *key;
    return DW_DLV_OK;
}

/*  This should only be called for a CU, never a TU.
    For a TU the type hash is known while reading
    the TU Header.  Not so for a CU. */
int
_dwarf_get_debugfission_for_offset(Dwarf_Debug dbg,
    Dwarf_Off    offset_wanted,
    const char * key_type, /*  "cu" or "tu" */
    struct Dwarf_Debug_Fission_Per_CU_s *  percu_out,
    Dwarf_Error *error)
{
    Dwarf_Xu_Index_Header xuhdr = 0;
    int sres = 0;
    Dwarf_Unsigned percu_index = 0;
    Dwarf_Unsigned sect_index_base = 0;
    Dwarf_Sig8 key;

    CHECK_DBG(dbg,error,"_dwarf_get_debugfission_for_offset()");
    sect_index_base = DW_SECT_INFO;
    key = zerohashkey;
    sres = _dwarf_get_xuhdr(dbg,key_type, &xuhdr,error);
    if (sres != DW_DLV_OK) {
        return sres;
    }
    sres = _dwarf_search_fission_for_offset(dbg,
        xuhdr,offset_wanted, sect_index_base, &percu_index,
        &key,
        error);
    if (sres != DW_DLV_OK) {
        return sres;
    }
    sres = transform_xu_to_dfp(xuhdr,percu_index,&key,
        key_type,percu_out, error);
    return sres;

}
int
dwarf_get_debugfission_for_key(Dwarf_Debug dbg,
    Dwarf_Sig8  *key  /* pointer to hash signature */,
    const char  *key_type  /*  "cu" or "tu" */,
    Dwarf_Debug_Fission_Per_CU *  percu_out,
    Dwarf_Error *error)
{
    int sres = 0;
    Dwarf_Unsigned percu_index = 0;
    Dwarf_Xu_Index_Header xuhdr = 0;

    CHECK_DBG(dbg,error,"dwarf_get_debugfission_for_key()");
    if (!key || !key_type || !percu_out) {
        _dwarf_error_string(0,error,DW_DLE_XU_TYPE_ARG_ERROR,
            "DW_DLE_XU_TYPE_ARG_ERROR: dw_key, dw_keytype, or "
            "Dwarf_Debug_Fission_Per_CU pointer* to return "
            "is not valid");
        return DW_DLV_ERROR;
    }

    sres = _dwarf_load_debug_info(dbg,error);
    if (sres == DW_DLV_ERROR) {
        return sres;
    }
    sres = _dwarf_load_debug_types(dbg,error);
    if (sres == DW_DLV_ERROR) {
        return sres;
    }
    /*  Returns already existing xuhdr */
    sres = _dwarf_get_xuhdr(dbg,key_type, &xuhdr,error);
    if (sres != DW_DLV_OK) {
        return sres;
    }
    /*  Search in that xu data. */
    sres = _dwarf_search_fission_for_key(dbg,
        xuhdr,key,&percu_index,error);
    if (sres != DW_DLV_OK) {
        return sres;
    }
    sres = transform_xu_to_dfp(xuhdr,percu_index,key,
        key_type,percu_out,error);
    return sres;
}

void
dwarf_dealloc_xu_header(Dwarf_Xu_Index_Header indexptr)
{
    if (indexptr) {
        Dwarf_Debug dbg = indexptr->gx_dbg;
        dwarf_dealloc(dbg,indexptr,DW_DLA_XU_INDEX);
    }
}
