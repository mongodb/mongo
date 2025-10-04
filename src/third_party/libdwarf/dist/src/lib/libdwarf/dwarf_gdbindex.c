/*

  Copyright (C) 2014-2020 David Anderson. All Rights Reserved.

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
/* see
https://sourceware.org/gdb/onlinedocs/gdb/\
Index-Section-Format.html#Index-Section-Format
*/

#include <config.h>

#include <string.h>  /* memcpy() */

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
#include "dwarf_opaque.h"
#include "dwarf_alloc.h"
#include "dwarf_error.h"
#include "dwarf_util.h"
#include "dwarf_string.h"
#include "dwarf_memcpy_swap.h"
#include "dwarf_gdbindex.h"

/*  The dwarf_util macro READ_UNALIGNED
    cannot be directly used because
    gdb defines the section contents of
    .gdb_index as little-endian always.
*/

#if WORDS_BIGENDIAN   /* meaning on this host */
#define READ_GDBINDEX(dest,desttype, source, length) \
    do {                                             \
        desttype _ltmp = 0;                      \
        _dwarf_memcpy_swap_bytes((((char *)(&_ltmp)) \
            + sizeof(_ltmp) - (length)),             \
            (source), (length)) ;                    \
        (dest) = (desttype)_ltmp;                    \
    } while (0)
#else /* little-endian on this host */
#define READ_GDBINDEX(dest,desttype, source, length) \
    do {                                             \
        desttype _ltmp = 0;                      \
        memcpy(((char *)(&_ltmp)) ,                  \
            (source), (length)) ;                    \
        (dest) = (desttype)_ltmp;                    \
    } while (0)
#endif

struct dwarf_64bitpair {
    gdbindex_64 offset;
    gdbindex_64 length;
};

static void
emit_no_value_msg(Dwarf_Debug dbg,
    int errnum,
    const char * errstr_text,
    Dwarf_Error *error)
{
    _dwarf_error_string(dbg,error,errnum,
        (char *)errstr_text);
}

static void
emit_one_value_msg(Dwarf_Debug dbg,
    int errnum,
    const char * errstr_text,
    Dwarf_Unsigned value,
    Dwarf_Error *error)
{
    dwarfstring m;

    dwarfstring_constructor(&m);
    dwarfstring_append_printf_u(&m,
        (char *)errstr_text,value);
    _dwarf_error_string(dbg,error,errnum,
        dwarfstring_string(&m));
    dwarfstring_destructor(&m);
}

static int
set_base(Dwarf_Debug dbg,
    struct Dwarf_Gdbindex_array_instance_s * hdr,
    Dwarf_Small *start,
    Dwarf_Small *end,
    /*  entrylen is the length of a single struct as seen
        in the object. */
    Dwarf_Unsigned entrylen,
    /* The size of each field in the struct in the object. */
    Dwarf_Unsigned fieldlen,
    enum gdbindex_type_e type,
    Dwarf_Error * error)
{

    if (type == git_std || type == git_cuvec) {
        /*  cuvec is sort of a fake as a simple
            section, but a useful one. */
        Dwarf_Unsigned count = 0;
        if ( end < start) {
            _dwarf_error(dbg, error,DW_DLE_GDB_INDEX_COUNT_ERROR);
            return DW_DLV_ERROR;
        }
        count = end - start;
        count = count / entrylen;
        hdr->dg_type = type;
        hdr->dg_base = start;
        hdr->dg_count = count;
        hdr->dg_entry_length = entrylen;
        hdr->dg_fieldlen = (unsigned)fieldlen;
    } else {
        /* address area. */
        /* 64bit, 64bit, offset. Then 32bit pad. */
        Dwarf_Unsigned count = 0;
        hdr->dg_base = start;
        if ( end < start) {
            _dwarf_error(dbg, error,
                DW_DLE_GDB_INDEX_COUNT_ADDR_ERROR);
            return DW_DLV_ERROR;
        }
        /* entry length includes pad. */
        hdr->dg_entry_length = 2*sizeof(gdbindex_64) +
            DWARF_32BIT_SIZE;
        count = end - start;
        count = count / hdr->dg_entry_length;
        hdr->dg_count = count;
        /*  The dg_fieldlen is a fake, the fields are not
            all the same length. */
        hdr->dg_fieldlen = DWARF_32BIT_SIZE;
        hdr->dg_type = type;
    }
    return DW_DLV_OK;
}

int
dwarf_gdbindex_header(Dwarf_Debug dbg,
    Dwarf_Gdbindex * gdbindexptr,
    Dwarf_Unsigned * version,
    Dwarf_Unsigned * cu_list_offset,
    Dwarf_Unsigned * types_cu_list_offset,
    Dwarf_Unsigned * address_area_offset,
    Dwarf_Unsigned * symbol_table_offset,
    Dwarf_Unsigned * constant_pool_offset,
    Dwarf_Unsigned * section_size,
    const char    ** section_name,
    Dwarf_Error    * error)
{
    Dwarf_Gdbindex indexptr = 0;
    int res = DW_DLV_ERROR;
    Dwarf_Small *data = 0;
    Dwarf_Small *startdata = 0;
    Dwarf_Unsigned version_in = 0;

    CHECK_DBG(dbg,error,"dwarf_gdbindex_header()");
    if (!dbg->de_debug_gdbindex.dss_size) {
        return DW_DLV_NO_ENTRY;
    }
    if (!dbg->de_debug_gdbindex.dss_data) {
        res = _dwarf_load_section(dbg, &dbg->de_debug_gdbindex,error);
        if (res != DW_DLV_OK) {
            return res;
        }
    }
    data = dbg->de_debug_gdbindex.dss_data;
    if (!data) {
        /*  Should be impossible, dwarf_load_section() would
            return DW_DLV_ERROR if dss_data could not be
            set non-null */
        _dwarf_error_string(dbg, error,
            DW_DLE_ERRONEOUS_GDB_INDEX_SECTION,
            "DW_DLE_ERRONEOUS_GDB_INDEX_SECTION: "
            "We have non-zero (section) dss_size but "
            "null dss_data pointer");
        return DW_DLV_ERROR;
    }
    startdata = data;

    if (dbg->de_debug_gdbindex.dss_size < (DWARF_32BIT_SIZE*6)) {
        _dwarf_error(dbg, error, DW_DLE_ERRONEOUS_GDB_INDEX_SECTION);
        return DW_DLV_ERROR;
    }
    indexptr = (Dwarf_Gdbindex)_dwarf_get_alloc(dbg,
        DW_DLA_GDBINDEX,1);
    if (indexptr == NULL) {
        _dwarf_error_string(dbg, error, DW_DLE_ALLOC_FAIL,
            "DW_DLE_ALLOC_FAIL: allocating Dwarf_Gdbindex");
        return DW_DLV_ERROR;
    }
    READ_GDBINDEX(version_in,Dwarf_Unsigned,
        data, DWARF_32BIT_SIZE);
    indexptr->gi_version = version_in;

    indexptr->gi_dbg = dbg;
    indexptr->gi_section_data = startdata;
    indexptr->gi_section_length = dbg->de_debug_gdbindex.dss_size;
    /*   7 and lower are different format in some way */
    if (indexptr->gi_version != 8 &&
        indexptr->gi_version != 7) {
        emit_one_value_msg(dbg, DW_DLE_ERRONEOUS_GDB_INDEX_SECTION,
            "DW_DLE_ERRONEOUS_GDB_INDEX_SECTION: "
            " version number %u is not"
            " supported",
            indexptr->gi_version,error);
        dwarf_dealloc(dbg,indexptr,DW_DLA_GDBINDEX);
        return DW_DLV_ERROR;
    }
    data += DWARF_32BIT_SIZE;
    READ_GDBINDEX(indexptr->gi_cu_list_offset ,Dwarf_Unsigned,
        data, DWARF_32BIT_SIZE);
    if (indexptr->gi_cu_list_offset > indexptr->gi_section_length) {
        emit_one_value_msg(dbg, DW_DLE_ERRONEOUS_GDB_INDEX_SECTION,
            "DW_DLE_ERRONEOUS_GDB_INDEX_SECTION"
            " cu list offset of %u is too large for the section",
            indexptr->gi_cu_list_offset,error);
        dwarf_dealloc(dbg,indexptr,DW_DLA_GDBINDEX);
        return DW_DLV_ERROR;
    }
    data += DWARF_32BIT_SIZE;
    READ_GDBINDEX(indexptr->gi_types_cu_list_offset ,Dwarf_Unsigned,
        data, DWARF_32BIT_SIZE);
    if (indexptr->gi_types_cu_list_offset >
        indexptr->gi_section_length) {
        emit_one_value_msg(dbg, DW_DLE_ERRONEOUS_GDB_INDEX_SECTION,
            "DW_DLE_ERRONEOUS_GDB_INDEX_SECTION"
            " types cu list offset of %u is too "
            "large for the section",
            indexptr->gi_cu_list_offset,error);
        dwarf_dealloc(dbg,indexptr,DW_DLA_GDBINDEX);
        return DW_DLV_ERROR;
    }
    data += DWARF_32BIT_SIZE;
    READ_GDBINDEX(indexptr->gi_address_area_offset ,Dwarf_Unsigned,
        data, DWARF_32BIT_SIZE);
    if (indexptr->gi_address_area_offset >
        indexptr->gi_section_length) {
        emit_one_value_msg(dbg, DW_DLE_ERRONEOUS_GDB_INDEX_SECTION,
            "DW_DLE_ERRONEOUS_GDB_INDEX_SECTION"
            " address area offset of %u is too "
            "large for the section",
            indexptr->gi_address_area_offset ,error);
        dwarf_dealloc(dbg,indexptr,DW_DLA_GDBINDEX);
        return DW_DLV_ERROR;
    }
    data += DWARF_32BIT_SIZE;
    READ_GDBINDEX(indexptr->gi_symbol_table_offset ,Dwarf_Unsigned,
        data, DWARF_32BIT_SIZE);
    if (indexptr->gi_symbol_table_offset >
        indexptr->gi_section_length) {
        emit_one_value_msg(dbg, DW_DLE_ERRONEOUS_GDB_INDEX_SECTION,
            "DW_DLE_ERRONEOUS_GDB_INDEX_SECTION"
            " symbol table offset of %u is too "
            "large for the section",
            indexptr->gi_symbol_table_offset,error);
        dwarf_dealloc(dbg,indexptr,DW_DLA_GDBINDEX);
        return DW_DLV_ERROR;
    }
    data += DWARF_32BIT_SIZE;
    READ_GDBINDEX(indexptr->gi_constant_pool_offset ,Dwarf_Unsigned,
        data, DWARF_32BIT_SIZE);
    if (indexptr->gi_constant_pool_offset >
        indexptr->gi_section_length) {
        emit_one_value_msg(dbg, DW_DLE_ERRONEOUS_GDB_INDEX_SECTION,
            "DW_DLE_ERRONEOUS_GDB_INDEX_SECTION"
            " constant pool offset of %u is too "
            "large for the section",
            indexptr->gi_constant_pool_offset,error);
        dwarf_dealloc(dbg,indexptr,DW_DLA_GDBINDEX);
        return DW_DLV_ERROR;
    }
    data += DWARF_32BIT_SIZE;

    res = set_base(dbg,&indexptr->gi_culisthdr,
        startdata + indexptr->gi_cu_list_offset,
        startdata + indexptr->gi_types_cu_list_offset,
        2*sizeof(gdbindex_64),
        sizeof(gdbindex_64),
        git_std,error);
    if (res == DW_DLV_ERROR) {
        dwarf_dealloc(dbg,indexptr,DW_DLA_GDBINDEX);
        return res;
    }
    res = set_base(dbg,&indexptr->gi_typesculisthdr,
        startdata+ indexptr->gi_types_cu_list_offset,
        startdata+ indexptr->gi_address_area_offset,
        3*sizeof(gdbindex_64),
        sizeof(gdbindex_64),
        git_std,error);
    if (res == DW_DLV_ERROR) {
        dwarf_dealloc(dbg,indexptr,DW_DLA_GDBINDEX);
        return res;
    }
    res = set_base(dbg,&indexptr->gi_addressareahdr,
        startdata + indexptr->gi_address_area_offset,
        startdata + indexptr->gi_symbol_table_offset,
        3*sizeof(gdbindex_64),
        sizeof(gdbindex_64),
        git_address,error);
    if (res == DW_DLV_ERROR) {
        dwarf_dealloc(dbg,indexptr,DW_DLA_GDBINDEX);
        return res;
    }
    res = set_base(dbg,&indexptr->gi_symboltablehdr,
        startdata + indexptr->gi_symbol_table_offset,
        startdata + indexptr->gi_constant_pool_offset,
        2*DWARF_32BIT_SIZE,
        DWARF_32BIT_SIZE,
        git_std,error);
    if (res == DW_DLV_ERROR) {
        dwarf_dealloc(dbg,indexptr,DW_DLA_GDBINDEX);
        return res;
    }
    res = set_base(dbg,&indexptr->gi_cuvectorhdr,
        startdata + indexptr->gi_constant_pool_offset,
        /*  There is no real single vector size.
            but we'll use the entire rest as if there was. */
        startdata + indexptr->gi_section_length,
        DWARF_32BIT_SIZE,
        DWARF_32BIT_SIZE,
        git_cuvec,error);
    if (res == DW_DLV_ERROR) {
        dwarf_dealloc(dbg,indexptr,DW_DLA_GDBINDEX);
        return res;
    }
    *gdbindexptr          = indexptr;
    *version              = indexptr->gi_version;
    *cu_list_offset       = indexptr->gi_cu_list_offset;
    *types_cu_list_offset = indexptr->gi_types_cu_list_offset;
    *address_area_offset  = indexptr->gi_address_area_offset;
    *symbol_table_offset  = indexptr->gi_symbol_table_offset;
    *constant_pool_offset = indexptr->gi_constant_pool_offset;
    *section_size         = indexptr->gi_section_length;
    *section_name  =        dbg->de_debug_gdbindex.dss_name;
    return DW_DLV_OK;
}

int
dwarf_gdbindex_culist_array(Dwarf_Gdbindex gdbindexptr,
    Dwarf_Unsigned       * list_length,
    Dwarf_Error * error)
{
    if (!gdbindexptr || !gdbindexptr->gi_dbg) {
        _dwarf_error_string(NULL, error,
            DW_DLE_GDB_INDEX_INDEX_ERROR,
            "DW_DLE_GDB_INDEX_INDEX_ERROR:"
            " passed in NULL inindexptr to"
            " dwarf_gdbindex_culist_array");
        return DW_DLV_ERROR;
    }
    (void)error;
    *list_length = gdbindexptr->gi_culisthdr.dg_count;
    return DW_DLV_OK;
}

/*  entryindex: 0 to list_length-1 */
int
dwarf_gdbindex_culist_entry(Dwarf_Gdbindex gdbindexptr,
    Dwarf_Unsigned   entryindex,
    Dwarf_Unsigned * cu_offset,
    Dwarf_Unsigned * cu_length,
    Dwarf_Error    * error)
{
    Dwarf_Small  * base = 0;
    Dwarf_Small  * endptr = 0;
    Dwarf_Unsigned offset = 0;
    Dwarf_Unsigned length = 0;
    Dwarf_Unsigned max = 0;
    unsigned       fieldlen = 0;

    if (!gdbindexptr || !gdbindexptr->gi_dbg) {
        _dwarf_error_string(NULL, error,
            DW_DLE_GDB_INDEX_INDEX_ERROR,
            "DW_DLE_GDB_INDEX_INDEX_ERROR:"
            " passed in NULL inindexptr to"
            " dwarf_gdbindex_culist_entry");
        return DW_DLV_ERROR;
    }
    max =  gdbindexptr->gi_culisthdr.dg_count;
    fieldlen = gdbindexptr->gi_culisthdr.dg_fieldlen;
    if (entryindex >= max) {
        return DW_DLV_NO_ENTRY;
    }
    endptr = gdbindexptr->gi_section_data +
        gdbindexptr->gi_section_length;

    base = gdbindexptr->gi_culisthdr.dg_base;
    base += entryindex*gdbindexptr->gi_culisthdr.dg_entry_length;
    if ((base + 2*fieldlen) >endptr) {
        Dwarf_Debug    dbg = 0;

        dbg = gdbindexptr->gi_dbg;
        emit_one_value_msg(dbg, DW_DLE_GDB_INDEX_INDEX_ERROR,
            "DW_DLE_GDB_INDEX_INDEX_ERROR:"
            " end offset of data for index %u is past the"
            " end of the section",
            entryindex,error);
        return DW_DLV_ERROR;
    }

    READ_GDBINDEX(offset ,Dwarf_Unsigned,
        base,
        fieldlen);
    READ_GDBINDEX(length ,Dwarf_Unsigned,
        base+ fieldlen,
        fieldlen);
    *cu_offset = offset;
    *cu_length = length;
    return DW_DLV_OK;
}

int
dwarf_gdbindex_types_culist_array(Dwarf_Gdbindex gdbindexptr,
    Dwarf_Unsigned       * list_length,
    Dwarf_Error  * error)
{
    if (!gdbindexptr || !gdbindexptr->gi_dbg) {
        _dwarf_error_string(NULL, error,
            DW_DLE_GDB_INDEX_INDEX_ERROR,
            "DW_DLE_GDB_INDEX_INDEX_ERROR:"
            " passed in NULL inindexptr to"
            " dwarf_types_culist_entry");
        return DW_DLV_ERROR;
    }

    (void)error;
    *list_length = gdbindexptr->gi_typesculisthdr.dg_count;
    return DW_DLV_OK;
}

/*  entryindex: 0 to list_length-1 */
int
dwarf_gdbindex_types_culist_entry(Dwarf_Gdbindex gdbindexptr,
    Dwarf_Unsigned   entryindex,
    Dwarf_Unsigned * t_offset,
    Dwarf_Unsigned * t_length,
    Dwarf_Unsigned * t_signature,
    Dwarf_Error    * error)
{
    Dwarf_Unsigned max =  0;
    Dwarf_Small * base = 0;
    Dwarf_Small * endptr = 0;
    Dwarf_Unsigned offset = 0;
    Dwarf_Unsigned length = 0;
    Dwarf_Unsigned signature = 0;
    unsigned fieldlen = 0;

    if (!gdbindexptr || !gdbindexptr->gi_dbg) {
        _dwarf_error_string(NULL, error,
            DW_DLE_GDB_INDEX_INDEX_ERROR,
            "DW_DLE_GDB_INDEX_INDEX_ERROR:"
            " passed in NULL inindexptr to"
            " dwarf_gdbindex_types_culist_entry");
        return DW_DLV_ERROR;
    }
    fieldlen = gdbindexptr->gi_typesculisthdr.dg_fieldlen;
    max =  gdbindexptr->gi_typesculisthdr.dg_count;
    endptr = gdbindexptr->gi_section_data +
        gdbindexptr->gi_section_length;

    if (entryindex >= max) {
        return DW_DLV_NO_ENTRY;
    }
    base = gdbindexptr->gi_typesculisthdr.dg_base;
    base += entryindex*gdbindexptr->gi_typesculisthdr.dg_entry_length;
    if ((base + 3*fieldlen) >endptr) {
        Dwarf_Debug dbg = gdbindexptr->gi_dbg;
        emit_one_value_msg(dbg, DW_DLE_GDB_INDEX_INDEX_ERROR,
            "DW_DLE_GDB_INDEX_INDEX_ERROR:"
            " end offset of data for type index %u is past the"
            " end of the section",
            entryindex,error);
        return DW_DLV_ERROR;
    }
    READ_GDBINDEX(offset ,Dwarf_Unsigned,
        base,
        fieldlen);
    READ_GDBINDEX(length ,Dwarf_Unsigned,
        base+ (1*fieldlen),
        fieldlen);
    READ_GDBINDEX(signature ,Dwarf_Unsigned,
        base+ (2*fieldlen),
        fieldlen);
    *t_offset = offset;
    *t_length = length;
    *t_signature = signature;
    return DW_DLV_OK;
}

int
dwarf_gdbindex_addressarea(Dwarf_Gdbindex gdbindexptr,
    Dwarf_Unsigned            * list_length,
    Dwarf_Error     * error)
{
    if (!gdbindexptr || !gdbindexptr->gi_dbg) {
        _dwarf_error_string(NULL, error,
            DW_DLE_GDB_INDEX_INDEX_ERROR,
            "DW_DLE_GDB_INDEX_INDEX_ERROR:"
            " passed in NULL inindexptr to"
            " dwarf_gdbindex_addressarea");
        return DW_DLV_ERROR;
    }
    (void)error;
    *list_length = gdbindexptr->gi_addressareahdr.dg_count;
    return DW_DLV_OK;
}

/*    entryindex: 0 to addressarea_list_length-1 */
int
dwarf_gdbindex_addressarea_entry(
    Dwarf_Gdbindex   gdbindexptr,
    Dwarf_Unsigned   entryindex,
    Dwarf_Unsigned * low_address,
    Dwarf_Unsigned * high_address,
    Dwarf_Unsigned * cu_index,
    Dwarf_Error    * error)
{
    Dwarf_Unsigned max = 0;
    Dwarf_Small * base = 0;
    Dwarf_Small * endptr = 0;
    Dwarf_Unsigned lowaddr = 0;
    Dwarf_Unsigned highaddr = 0;
    Dwarf_Unsigned cuindex = 0;
    Dwarf_Unsigned fieldslen = 0;

    if (!gdbindexptr || !gdbindexptr->gi_dbg) {
        _dwarf_error_string(NULL, error,
            DW_DLE_GDB_INDEX_INDEX_ERROR,
            "DW_DLE_GDB_INDEX_INDEX_ERROR:"
            " passed in NULL inindexptr to"
            " dwarf_gdbindex_addressarea_entry");
        return DW_DLV_ERROR;
    }
    max =  gdbindexptr->gi_addressareahdr.dg_count;
    if (entryindex >= max) {
        _dwarf_error(gdbindexptr->gi_dbg, error,
            DW_DLE_GDB_INDEX_INDEX_ERROR);
        return DW_DLV_ERROR;
    }
    base = gdbindexptr->gi_addressareahdr.dg_base;
    base += entryindex*gdbindexptr->gi_addressareahdr.dg_entry_length;
    endptr = gdbindexptr->gi_section_data +
        gdbindexptr->gi_section_length;
    fieldslen = 2*sizeof(gdbindex_64) + DWARF_32BIT_SIZE;
    if ((base + fieldslen) > endptr) {
        Dwarf_Debug dbg = gdbindexptr->gi_dbg;
        emit_one_value_msg(dbg, DW_DLE_GDB_INDEX_INDEX_ERROR,
            "DW_DLE_GDB_INDEX_INDEX_ERROR:"
            " end offset of data for "
            " dwarf_gdbindex_addressarea_entry %u is past the"
            " end of the section",
            entryindex,error);
        return DW_DLV_ERROR;
    }

    READ_GDBINDEX(lowaddr ,Dwarf_Unsigned,
        base,
        sizeof(gdbindex_64));
    READ_GDBINDEX(highaddr ,Dwarf_Unsigned,
        base+ (1*sizeof(gdbindex_64)),
        sizeof(gdbindex_64));
    READ_GDBINDEX(cuindex ,Dwarf_Unsigned,
        base+ (2*sizeof(gdbindex_64)),
        DWARF_32BIT_SIZE);
    *low_address = lowaddr;
    *high_address = highaddr;
    *cu_index = cuindex;
    return DW_DLV_OK;
}

int
dwarf_gdbindex_symboltable_array(Dwarf_Gdbindex gdbindexptr,
    Dwarf_Unsigned            * list_length,
    Dwarf_Error     * error)
{
    if (!gdbindexptr || !gdbindexptr->gi_dbg) {
        _dwarf_error_string(NULL, error,
            DW_DLE_GDB_INDEX_INDEX_ERROR,
            "DW_DLE_GDB_INDEX_INDEX_ERROR:"
            " passed in NULL inindexptr to"
            " dwarf_gdbindex_symboltable_array");
        return DW_DLV_ERROR;
    }
    (void)error;
    *list_length = gdbindexptr->gi_symboltablehdr.dg_count;
    return DW_DLV_OK;
}

/*  entryindex: 0 to symtab_list_length-1 */
int
dwarf_gdbindex_symboltable_entry(
    Dwarf_Gdbindex   gdbindexptr,
    Dwarf_Unsigned   entryindex,
    Dwarf_Unsigned * string_offset,
    Dwarf_Unsigned * cu_vector_offset,
    Dwarf_Error    * error)
{
    Dwarf_Unsigned max = 0;
    Dwarf_Small * base = 0;
    Dwarf_Small * endptr = 0;
    Dwarf_Unsigned symoffset = 0;
    Dwarf_Unsigned cuoffset = 0;
    unsigned fieldlen = 0;

    if (!gdbindexptr || !gdbindexptr->gi_dbg) {
        _dwarf_error_string(NULL, error,
            DW_DLE_GDB_INDEX_INDEX_ERROR,
            "DW_DLE_GDB_INDEX_INDEX_ERROR:"
            " passed in NULL inindexptr to"
            " dwarf_gdbindex_symboltable_entry");
        return DW_DLV_ERROR;
    }
    max =  gdbindexptr->gi_symboltablehdr.dg_count;
    fieldlen = gdbindexptr->gi_symboltablehdr.dg_fieldlen;
    if (entryindex >= max) {
        return DW_DLV_NO_ENTRY;
    }
    base = gdbindexptr->gi_symboltablehdr.dg_base;
    base += entryindex*gdbindexptr->gi_symboltablehdr.dg_entry_length;
    endptr = gdbindexptr->gi_section_data +
        gdbindexptr->gi_section_length;

    if (( base + 2*fieldlen) >endptr) {
        Dwarf_Debug dbg = gdbindexptr->gi_dbg;
        emit_one_value_msg(dbg, DW_DLE_GDB_INDEX_INDEX_ERROR,
            "DW_DLE_GDB_INDEX_INDEX_ERROR:"
            " end offset of data for symboltable entry "
            "%u is past the"
            " end of the section",
            entryindex,error);
        return DW_DLV_ERROR;
    }
    READ_GDBINDEX(symoffset ,Dwarf_Unsigned,
        base,
        fieldlen);
    READ_GDBINDEX(cuoffset ,Dwarf_Unsigned,
        base + fieldlen,
        fieldlen);
    *string_offset = symoffset;
    *cu_vector_offset = cuoffset;
    return DW_DLV_OK;
}

int
dwarf_gdbindex_cuvector_length(Dwarf_Gdbindex gdbindexptr,
    Dwarf_Unsigned   cuvector_offset,
    Dwarf_Unsigned * innercount,
    Dwarf_Error    * error)
{
    Dwarf_Small *base = 0;
    Dwarf_Small *endptr = 0;
    Dwarf_Unsigned fieldlen = 0;
    Dwarf_Unsigned val = 0;

    if (!gdbindexptr || !gdbindexptr->gi_dbg) {
        _dwarf_error_string(NULL, error,
            DW_DLE_GDB_INDEX_INDEX_ERROR,
            "DW_DLE_GDB_INDEX_INDEX_ERROR:"
            " passed in NULL indexptr to"
            " dwarf_gdbindex_cuvector_length");
        return DW_DLV_ERROR;
    }

    base = gdbindexptr->gi_cuvectorhdr.dg_base;
    endptr = gdbindexptr->gi_section_data +
        gdbindexptr->gi_section_length;
    fieldlen = gdbindexptr->gi_cuvectorhdr.dg_entry_length;
    base += cuvector_offset;
    if (( base + fieldlen) >endptr) {
        Dwarf_Debug dbg = gdbindexptr->gi_dbg;
        emit_no_value_msg(dbg, DW_DLE_GDB_INDEX_INDEX_ERROR,
            "DW_DLE_GDB_INDEX_INDEX_ERROR:"
            " end offset of count of gdbindex cuvector "
            " is past the"
            " end of the section",
            error);
        return DW_DLV_ERROR;
    }
    READ_GDBINDEX(val,Dwarf_Unsigned,
        base,
        fieldlen);
    *innercount = val;
    return DW_DLV_OK;
}

int
dwarf_gdbindex_cuvector_inner_attributes(Dwarf_Gdbindex gdbindexptr,
    Dwarf_Unsigned   cuvector_offset,
    Dwarf_Unsigned   innerindex,
    /* The attr_value is a field of bits. For expanded version
        use  dwarf_gdbindex_instance_expand_value() */
    Dwarf_Unsigned * attributes,
    Dwarf_Error    * error)
{
    Dwarf_Small *base = 0;
    Dwarf_Small *endptr =  0;
    Dwarf_Unsigned fieldlen = 0;
    Dwarf_Unsigned val = 0;

    if (!gdbindexptr || !gdbindexptr->gi_dbg) {
        _dwarf_error_string(NULL, error,
            DW_DLE_GDB_INDEX_INDEX_ERROR,
            "DW_DLE_GDB_INDEX_INDEX_ERROR:"
            " passed in NULL indexptr to"
            " dwarf_gdbindex_cuvector_length");
        return DW_DLV_ERROR;
    }
    base = gdbindexptr->gi_cuvectorhdr.dg_base;
    base += cuvector_offset;
    endptr = gdbindexptr->gi_section_data +
        gdbindexptr->gi_section_length;
    fieldlen = gdbindexptr->gi_cuvectorhdr.dg_entry_length;
    /*  The initial 4 bytes is not part of the array,
        it is some sort of count.  Get past it.*/
    base += fieldlen;
    base += fieldlen*innerindex;
    if ((base+fieldlen) >= endptr) {
        Dwarf_Debug dbg = gdbindexptr->gi_dbg;
        emit_one_value_msg(dbg, DW_DLE_GDB_INDEX_INDEX_ERROR,
            "DW_DLE_GDB_INDEX_INDEX_ERROR:"
            " end offset of data for cuvector_inner_attribute "
            "%u is past the"
            " end of the section",
            innerindex,error);
        return DW_DLV_ERROR;
    }

    READ_GDBINDEX(val ,Dwarf_Unsigned,
        base,
        fieldlen);
    *attributes = val;
    return DW_DLV_OK;
}

int
dwarf_gdbindex_cuvector_instance_expand_value(
    Dwarf_Gdbindex gdbindexptr,
    Dwarf_Unsigned   value,
    Dwarf_Unsigned * cu_index,
    Dwarf_Unsigned * symbol_kind,
    Dwarf_Unsigned * is_static,
    Dwarf_Error    * error)
{
    if (!gdbindexptr || !gdbindexptr->gi_dbg)  {
        _dwarf_error_string(NULL, error, DW_DLE_DBG_NULL,
            "DW_DLE_DBG_NULL: The call to "
            "dwarf_gdbindex_cuvector_instance_expand_value"
            " provides no dbg pointer");
        return DW_DLV_ERROR;
    }
    *cu_index =    value         & 0xffffff;
    *symbol_kind = (value >> 28) & 0x7;
    *is_static =   (value >> 31) & 1;
    return DW_DLV_OK;

}

/*  The strings in the pool follow (in memory) the cu index
    set and are NUL terminated. */
int
dwarf_gdbindex_string_by_offset(Dwarf_Gdbindex gdbindexptr,
    Dwarf_Unsigned   stringoffsetinpool,
    const char    ** string_ptr,
    Dwarf_Error   *  error)
{
    Dwarf_Small *pooldata = 0;
    Dwarf_Small *section_end = 0;
    Dwarf_Small *stringitself = 0;
    Dwarf_Debug dbg = 0;
    Dwarf_Unsigned fulloffset = 0;
    int res = 0;

    if (!gdbindexptr || !gdbindexptr->gi_dbg) {
        emit_no_value_msg(NULL,DW_DLE_GDB_INDEX_INDEX_ERROR,
            "DW_DLE_GDB_INDEX_INDEX_ERROR: "
            "The gdbindex pointer to "
            "dwarf_gdbindex_string_by_offset()"
            " is NULL",error);
        return DW_DLV_ERROR;
    }
    dbg = gdbindexptr->gi_dbg;
    if (IS_INVALID_DBG(dbg)) {
        emit_no_value_msg(NULL,DW_DLE_GDB_INDEX_INDEX_ERROR,
            "DW_DLE_GDB_INDEX_INDEX_ERROR: "
            "The gdbindex Dwarf_Debug in"
            "dwarf_gdbindex_string_by_offset()"
            " is NULL",error);
        return DW_DLV_ERROR;
    }
    section_end = gdbindexptr->gi_section_data +
        gdbindexptr->gi_section_length;
    fulloffset = gdbindexptr->gi_constant_pool_offset
        + stringoffsetinpool;
    stringitself = gdbindexptr->gi_section_data + fulloffset;
    if (stringitself > section_end) {
        emit_one_value_msg(dbg,DW_DLE_GDBINDEX_STRING_ERROR,
            "DW_DLE_GDBINDEX_STRING_ERROR: "
            "The dwarf_gdbindex_string_by_offset() "
            "string starts past the end of the "
            "section at section_offset 0x%"
            DW_PR_XZEROS DW_PR_DUx  ".",
            fulloffset,
            error);
        return DW_DLV_ERROR;
    }
    res = _dwarf_check_string_valid(dbg,pooldata,
        stringitself, section_end,
        DW_DLE_GDBINDEX_STRING_ERROR,
        error);
    if (res != DW_DLV_OK) {
        return res;
    }
    *string_ptr = (const char *)stringitself;
    return DW_DLV_OK;
}

void
dwarf_dealloc_gdbindex(Dwarf_Gdbindex indexptr)
{
    if (indexptr) {
        Dwarf_Debug dbg = indexptr->gi_dbg;
        dwarf_dealloc(dbg,indexptr,DW_DLA_GDBINDEX);
    }
}
