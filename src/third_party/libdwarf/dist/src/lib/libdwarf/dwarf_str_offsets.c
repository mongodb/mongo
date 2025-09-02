/*
    Copyright (C) 2018-2020 David Anderson. All Rights Reserved.

    This program is free software; you can redistribute it
    and/or modify it under the terms of version 2.1 of the
    GNU Lesser General Public License as published by the
    Free Software Foundation.

    This program is distributed in the hope that it would
    be useful, but WITHOUT ANY WARRANTY; without even the
    implied warranty of MERCHANTABILITY or FITNESS FOR A
    PARTICULAR PURPOSE.

    Further, this software is distributed without any warranty
    that it is free of the rightful claim of any third person
    regarding infringement or the like.  Any license provided
    herein, whether implied or otherwise, applies only to
    this software file.  Patent licenses, if any, provided
    herein do not apply to combinations of this program with
    other software, or any other product whatsoever.

    You should have received a copy of the GNU Lesser General
    Public License along with this program; if not, write
    the Free Software Foundation, Inc., 51 Franklin Street -
    Fifth Floor, Boston MA 02110-1301, USA.
*/

#include <config.h>

#include <stddef.h> /* NULL size_t */
#ifdef HAVE_STDINT_H
#include <stdint.h> /* uintptr_t */
#endif /* HAVE_STDINT_H */

#if defined(_WIN32) && defined(HAVE_STDAFX_H)
#include "stdafx.h"
#endif /* HAVE_STDAFX_H */
#include <stdio.h> /* printf */

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
#include "dwarf_str_offsets.h"

#define STR_OFFSETS_MAGIC 0x2feed2

#define VALIDATE_SOT(xsot)                                \
    if (!(xsot)) {                                          \
        _dwarf_error(NULL,error,DW_DLE_STR_OFFSETS_NULLARGUMENT);\
        return DW_DLV_ERROR;                              \
    }                                                     \
    if (!(xsot)->so_dbg) {                                \
        _dwarf_error(NULL,error,DW_DLE_STR_OFFSETS_NULL_DBG);\
        return DW_DLV_ERROR;                              \
    }                                                     \
    if ((xsot)->so_magic_value !=  STR_OFFSETS_MAGIC) {   \
        _dwarf_error((xsot)->so_dbg,error,                \
        DW_DLE_STR_OFFSETS_NO_MAGIC);                     \
        return DW_DLV_ERROR;                              \
    }

#if 0 /* dump_bytes */
static void
dump_bytes(char * msg,Dwarf_Small * start, long len)
{
    Dwarf_Small *end = start + len;
    Dwarf_Small *cur = start;

    printf("%s starting at %p \n",msg,(void*)start);
    for (; cur < end; cur++) {
        printf("%02x ", *cur);
    }
    printf("\n");
}
#endif /*0*/

int
dwarf_open_str_offsets_table_access(Dwarf_Debug dbg,
    Dwarf_Str_Offsets_Table * table_data,
    Dwarf_Error             * error)
{
    int res = 0;
    Dwarf_Str_Offsets_Table local_table_data = 0;
    Dwarf_Small *offsets_start_ptr = 0;
    Dwarf_Unsigned sec_size = 0;

    CHECK_DBG(dbg,error,"dwarf_open_str_offsets_table_access()");
    if (!table_data) {
        _dwarf_error(dbg,error,DW_DLE_STR_OFFSETS_NULLARGUMENT);
        return DW_DLV_ERROR;
    }
    /*  Considered testing for *table_data being NULL, but
        not doing such a test. */

    res = _dwarf_load_section(dbg, &dbg->de_debug_str_offsets,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    offsets_start_ptr = dbg->de_debug_str_offsets.dss_data;
    if (!offsets_start_ptr) {
        return DW_DLV_NO_ENTRY;
    }
    sec_size = dbg->de_debug_str_offsets.dss_size;
    local_table_data = (Dwarf_Str_Offsets_Table)_dwarf_get_alloc(dbg,
        DW_DLA_STR_OFFSETS,1);
    if (!local_table_data) {
        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
        return DW_DLV_ERROR;
    }
    local_table_data->so_dbg = dbg;
    local_table_data->so_magic_value  = STR_OFFSETS_MAGIC;
    local_table_data->so_section_start_ptr = offsets_start_ptr;
    local_table_data->so_section_end_ptr = offsets_start_ptr +
        sec_size;
    local_table_data->so_section_size = sec_size;
    local_table_data->so_next_table_offset = 0;
    local_table_data->so_wasted_section_bytes = 0;
    /*  get_alloc zeroed all the bits, no need to repeat that here. */
    *table_data = local_table_data;
    return DW_DLV_OK;
}

int
dwarf_close_str_offsets_table_access(
    Dwarf_Str_Offsets_Table  table_data,
    Dwarf_Error             * error)
{
    Dwarf_Debug dbg = 0;

    VALIDATE_SOT(table_data)
    dbg = table_data->so_dbg;
    table_data->so_magic_value = 0xdead;
    dwarf_dealloc(dbg,table_data, DW_DLA_STR_OFFSETS);
    return DW_DLV_OK;
}

int
dwarf_str_offsets_value_by_index(Dwarf_Str_Offsets_Table sot,
    Dwarf_Unsigned index,
    Dwarf_Unsigned *stroffset,
    Dwarf_Error *error)
{
    Dwarf_Small *entryptr = 0;
    Dwarf_Unsigned val = 0;
    Dwarf_Unsigned entryoffset = 0;
    Dwarf_Unsigned secsize = 0;
    Dwarf_Debug dbg = 0;
    Dwarf_Small *end_ptr = 0;
/* so_section_size */

    VALIDATE_SOT(sot)
    dbg = sot->so_dbg;
    secsize = dbg->de_debug_str_offsets.dss_size;
    if (index >= sot->so_array_entry_count) {
        return DW_DLV_NO_ENTRY;
    }
    entryoffset = sot->so_table_start_offset +
        sot->so_lcl_offset_to_array;
    entryoffset += index*sot->so_array_entry_size;
    entryptr = sot->so_section_start_ptr + entryoffset;
    if (entryoffset > secsize ||
        (entryoffset+sot->so_array_entry_size) > secsize) {
        _dwarf_error_string(dbg,error,
            DW_DLE_STR_OFFSETS_ARRAY_INDEX_WRONG,
            "DW_DLE_STR_OFFSETS_ARRAY_INDEX_WRONG: "
            "A libdwarf internal bug. Report to the maintainers");
        return DW_DLV_ERROR;
    }
    end_ptr = entryptr+sot->so_array_entry_size;
    READ_UNALIGNED_CK(dbg, val, Dwarf_Unsigned,
        entryptr, sot->so_array_entry_size,error,end_ptr);
    *stroffset = val;
    return DW_DLV_OK;
}

/* The minimum possible area .debug_str_offsets header . */
#define MIN_HEADER_LENGTH  8

/*  New April 2018.
    Beginning at starting_offset zero,
    returns data about the first table found.
    The value *next_table_offset is the value
    of the next table (if any), one byte past
    the end of the table whose data is returned..
    Returns DW_DLV_NO_ENTRY if the starting offset
    is past the end of valid data.

    There is no guarantee that there are no non-0 nonsense
    bytes in the section outside of useful tables,
    so this can fail and return nonsense or
    DW_DLV_ERROR  if such garbage exists.
*/

static int
is_all_zeroes(Dwarf_Small*start,
    Dwarf_Small*end)
{
    if (start >= end) {
        /*  We should not get here, this is just
            a defensive test. */
        return TRUE;
    }
    for ( ; start < end; ++start) {
        if (!*start) {
            /* There is some garbage here. */
            return FALSE;
        }
    }
    /* All just zero bytes. */
    return TRUE;
}

static void
emit_invalid_dw5tab(Dwarf_Debug dbg,
    Dwarf_Unsigned headerlen,
    Dwarf_Error *error)
{
    dwarfstring m;

    dwarfstring_constructor(&m);
    dwarfstring_append_printf_u(&m,
        "DW_DLE_SECTION_SIZE_ERROR: "
        "header length 0x%x is too small "
        "to be a real .debug_str_offsets "
        "DWARF5 section",
        headerlen);
    _dwarf_error_string(dbg,error,
        DW_DLE_SECTION_SIZE_ERROR,
        dwarfstring_string(&m));
    dwarfstring_destructor(&m);
}
static void
emit_offsets_array_msg(Dwarf_Debug dbg,
    Dwarf_Unsigned tab_length,
    Dwarf_Unsigned secsize,
    Dwarf_Error *error)
{
    dwarfstring m;

    dwarfstring_constructor(&m);
    dwarfstring_append_printf_u(&m,
        "DW_DLE_STR_OFFSETS_ARRAY_SIZE: "
        " total length 0x%x  with table offset larger than ",
        tab_length);
    dwarfstring_append_printf_u(&m,
        ".debug_str_offsets section size of 0x%x."
        " Perhaps the section is a GNU DWARF4"
        " extension with a different format.",
        secsize);
    _dwarf_error_string(dbg,error,
        DW_DLE_STR_OFFSETS_ARRAY_SIZE,
        dwarfstring_string(&m));
    dwarfstring_destructor(&m);
}

static void
emit_wrong_version(Dwarf_Debug dbg,
    Dwarf_Half version,
    Dwarf_Error *error)
{
    dwarfstring m;

    dwarfstring_constructor(&m);
    dwarfstring_append_printf_u(&m,
        "DW_DLE_STR_OFFSETS_VERSION_WRONG: "
        "%u. Only version 5 is supported "
        "when reading .debug_str_offsets."
        " Perhaps the section is a GNU DWARF4"
        " extension with a different format.",
        version);
    _dwarf_error_string(dbg,error,
        DW_DLE_STR_OFFSETS_VERSION_WRONG,
        dwarfstring_string(&m));
    dwarfstring_destructor(&m);
}

int
_dwarf_trial_read_dwarf_five_hdr(Dwarf_Debug dbg,
    Dwarf_Unsigned  new_table_offset,
    Dwarf_Unsigned  secsize,
    Dwarf_Unsigned *table_local_offset_of_array,
    Dwarf_Unsigned *total_table_length,
    Dwarf_Unsigned *length_out,
    Dwarf_Half     *local_offset_size_out,
    Dwarf_Half     *local_extension_size_out,
    Dwarf_Half     *version_out,
    Dwarf_Half     *padding_out,
    Dwarf_Error    *error)
{
    Dwarf_Unsigned length = 0; /* length following the
        local_offset_size + local_extension_size */
    Dwarf_Unsigned local_offset_size = 0;
    Dwarf_Unsigned local_extension_size = 0;
    Dwarf_Unsigned tab_length = 0;
    Dwarf_Unsigned array_local_offset = 0;
    Dwarf_Half     version = 0;
    Dwarf_Half     padding = 0;
    Dwarf_Unsigned globloff = 0;
    Dwarf_Unsigned net_end_offset = 0;
    Dwarf_Small   *secstart = dbg->de_debug_str_offsets.dss_data;
    Dwarf_Small   *table_start_ptr = secstart + new_table_offset;
    Dwarf_Small   *secendptr = secstart+secsize;

    READ_AREA_LENGTH_CK(dbg,length,Dwarf_Unsigned,
        table_start_ptr,local_offset_size,
        local_extension_size,error,
        secsize,secendptr);
    /*  The 'length' part of any header is
        local_extension_size + local_offset_size.
        The length of an offset in the section is just
        local_offset_size.
        Standard DWARF2 sums to 4.
        Standard DWARF3,4,5 sums to 4 or 12.
        Nonstandard SGI IRIX 64bit dwarf sums to 8 (SGI IRIX
        was all DWARF2 and could not have a .debug_str_offsets
        section).
        The header includes 2 bytes of version and two bytes
        of padding. */
    if (length < 4) {
        /*  Usually DW4-style .debug_str_offsets
            starts off with a zero value to ref the
            base string in .debug_str. Should not apply here.
            Any tiny value is guaranteed not to be a legal
            DWARF5 .debug_str_offsets section. */
        emit_invalid_dw5tab(dbg,length,error);
        return DW_DLV_ERROR;
    }
    globloff = new_table_offset;
    array_local_offset = local_extension_size +local_offset_size
        +4; /* 4 for the two Dwarf_Half in the table header */
    tab_length = local_extension_size +local_offset_size +
        length;

    net_end_offset = tab_length+globloff;

    if (length > secsize  ||
        array_local_offset > secsize ||
        tab_length > secsize ||
        net_end_offset > secsize) {
        emit_offsets_array_msg(dbg,tab_length,
            secsize,error);
        return DW_DLV_ERROR;
    }
    /*  table_start_ptr is incremented past
        the length data by read-area-length-ck. */
    READ_UNALIGNED_CK(dbg, version, Dwarf_Half,
        table_start_ptr, DWARF_HALF_SIZE,
        error,secendptr);
    table_start_ptr += DWARF_HALF_SIZE;
    if (version != DW_STR_OFFSETS_VERSION5) {
        emit_wrong_version(dbg,version,error);
        return DW_DLV_ERROR;
    }
    READ_UNALIGNED_CK(dbg, padding, Dwarf_Half,
        table_start_ptr, DWARF_HALF_SIZE,
        error,secendptr);

    /*  padding should be zero, but we are
        not checking it here at present. */
    *table_local_offset_of_array = array_local_offset;
    *total_table_length = tab_length;
    *length_out = length;
    *local_offset_size_out = (Dwarf_Half)local_offset_size;
    *local_extension_size_out = (Dwarf_Half)local_extension_size;
    *version_out = version;
    *padding_out = padding;
    return DW_DLV_OK;
}

/*  Used by code reading attributes/forms and
    also by code reading the raw .debug_str_offsets
    section, hence the code allows for
    output arguments to be zero.
    If cucontext is null it means the call part
    of trying to print the section without
    accessing any context. dwarfdump option
    --print-str-offsets.
    New 30 August 2020. */
int
_dwarf_read_str_offsets_header(Dwarf_Str_Offsets_Table sot,
    Dwarf_CU_Context cucontext,
    /* Followed by return values/error */
    Dwarf_Unsigned *length_out,
    Dwarf_Half     *offset_size_out,
    Dwarf_Half     *extension_size_out,
    Dwarf_Half     *version_out,
    Dwarf_Half     *padding_out,
    Dwarf_Unsigned *local_offset_to_array_out,
    Dwarf_Unsigned *total_table_length_out,
    Dwarf_Error *error)
{
    Dwarf_Unsigned length            = 0;
    Dwarf_Half local_offset_size = 0;
    Dwarf_Half local_extension_size = 0;
    Dwarf_Half version               = 0;
    Dwarf_Half padding               = 0;
    int            res = 0;
    Dwarf_Bool     is_dwarf_five = TRUE;
    Dwarf_Debug    dbg = sot->so_dbg;
    Dwarf_Unsigned secsize  = sot->so_section_size;
    Dwarf_Unsigned table_local_offset_of_array = 0;
    Dwarf_Unsigned total_table_length = 0;
    Dwarf_Unsigned globaltaboff = 0;

    globaltaboff = sot->so_next_table_offset;
    if (cucontext) {
        if (cucontext->cc_str_offsets_tab_present) {
            /*  cu_context has what it needs already and we do
                not need the rest of what the interface
                provides */
            return DW_DLV_OK;
        }
    }
    res = _dwarf_trial_read_dwarf_five_hdr(dbg,
        globaltaboff,
        secsize,
        &table_local_offset_of_array,
        &total_table_length,
        /* Length is the length field from the table involved */
        &length,
        &local_offset_size,
        &local_extension_size,
        &version,
        &padding,
        error);
    if (res != DW_DLV_OK) {
        if (res == DW_DLV_ERROR && error) {
            dwarf_dealloc_error(dbg,*error);
            *error = 0;
        }
        /*  If it's really DWARF5 but with a serious
            problem  this will think...NOT 5! */
        is_dwarf_five = FALSE;
    }
    if ( !is_dwarf_five) {
        length = secsize;
        /*  This is possibly
            GNU Dwarf4 extension .debug_str_offsets.
            It could also just be corrupted data.
            offset size is not going to be 8.
            de_length_size is a guess and not set at this point .
            We assume the data starts with the array
            of string offsets,
            and all in one simple array from start to end
            of section. */
        table_local_offset_of_array = 0;
        total_table_length = secsize;
        length = secsize;
        local_offset_size = 4;
        local_extension_size = 0;
        version = DW_STR_OFFSETS_VERSION4;
        padding = 0;
    }

    if (length_out) {
        *length_out = length;
    }
    if (offset_size_out) {
        *offset_size_out = local_offset_size;
    }
    if (extension_size_out) {
        *extension_size_out = local_extension_size;
    }
    if (version_out) {
        *version_out = version;
    }
    if (padding_out) {
        *padding_out = padding;
    }
    if (cucontext) {
        cucontext->cc_str_offsets_header_offset = globaltaboff;
        cucontext->cc_str_offsets_tab_present = TRUE;
        cucontext->cc_str_offsets_tab_to_array =
            table_local_offset_of_array;
        cucontext->cc_str_offsets_version   = version;
        cucontext->cc_str_offsets_offset_size  = local_offset_size;
        cucontext->cc_str_offsets_table_size   = total_table_length;
    }
    if (local_offset_to_array_out) {
        *local_offset_to_array_out = table_local_offset_of_array;
    }
    if (total_table_length_out) {
        *total_table_length_out = total_table_length;
    }
    return DW_DLV_OK;
}

int
dwarf_next_str_offsets_table(Dwarf_Str_Offsets_Table sot,
    Dwarf_Unsigned *unit_length_out,
    Dwarf_Unsigned *unit_length_offset_out,
    Dwarf_Unsigned *table_start_offset_out,
    Dwarf_Half     *entry_size_out,
    Dwarf_Half     *version_out,
    Dwarf_Half     *padding_out,
    Dwarf_Unsigned *table_value_count_out,
    Dwarf_Error    * error)
{

    Dwarf_Small *table_header_ptr = 0;
    Dwarf_Unsigned table_header_offset  = 0;
    Dwarf_Unsigned table_end_offset   = 0;
    Dwarf_Unsigned array_start_offset = 0;
    Dwarf_Unsigned length        = 0;
    Dwarf_Half local_length_size = 0;
    Dwarf_Half local_extension_size = 0;
    Dwarf_Half version           = 0;
    Dwarf_Half padding           = 0;
    Dwarf_Unsigned local_offset_to_array= 0;
    Dwarf_Unsigned  total_table_length = 0;
    Dwarf_Debug dbg = sot->so_dbg;
    int res = 0;

    VALIDATE_SOT(sot)

    table_header_offset = sot->so_next_table_offset;
    if (table_header_offset >= sot->so_section_size) {
        return DW_DLV_NO_ENTRY;
    }
    table_header_ptr = sot->so_section_start_ptr +
        table_header_offset;
    if (table_header_offset >= sot->so_section_size) {
        if (table_header_ptr == sot->so_section_end_ptr) {
            /* At end of section. Done. */
            return DW_DLV_NO_ENTRY;
        } else {
            /* bogus table offset. */
            Dwarf_Unsigned len = 0;
            dwarfstring m;

            len = (sot->so_section_end_ptr >= table_header_ptr)?
                (Dwarf_Unsigned)(uintptr_t)sot->so_section_end_ptr -
                (Dwarf_Unsigned)(uintptr_t)table_header_ptr:
                (Dwarf_Unsigned)0xffffffff;
            dwarfstring_constructor(&m);
            dwarfstring_append_printf_i(&m,
                "DW_DLE_STR_OFFSETS_EXTRA_BYTES: "
                "Table Offset is %"   DW_PR_DSd
                " bytes past end of section",len);
            _dwarf_error_string(dbg,error,
                DW_DLE_STR_OFFSETS_EXTRA_BYTES,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
        }
    }

    if ((table_header_ptr + MIN_HEADER_LENGTH) >
        sot->so_section_end_ptr) {

        /*  We have a too-short section it appears.
            Should we generate error? Or ignore?
            As of March 10 2020 we check for garbage
            bytes in-section. */
        dwarfstring m;
        Dwarf_Small *hend = 0;
        Dwarf_Unsigned len = 0;

        if (is_all_zeroes(table_header_ptr,sot->so_section_end_ptr)){
            return DW_DLV_NO_ENTRY;
        }
        hend = table_header_ptr + MIN_HEADER_LENGTH;
        len = (hend >= sot->so_section_end_ptr)?
            (Dwarf_Unsigned)(uintptr_t)sot->so_section_end_ptr -
            (Dwarf_Unsigned)(uintptr_t)table_header_ptr:
            (Dwarf_Unsigned)0xffffffff;
        dwarfstring_constructor(&m);
        dwarfstring_append_printf_i(&m,
            "DW_DLE_STR_OFFSETS_EXTRA_BYTES: "
            "Table Offset plus a minimal header is %"
            DW_PR_DSd
            " bytes past end of section"
            " and some bytes in-section are non-zero",len);
        _dwarf_error_string(dbg,error,
            DW_DLE_STR_OFFSETS_EXTRA_BYTES,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }

    res = _dwarf_read_str_offsets_header(sot,
        0 /* cu_context */,
        &length,
        &local_length_size,
        &local_extension_size,
        &version,
        &padding,
        &local_offset_to_array,
        &total_table_length, error);
    if (res != DW_DLV_OK) {
        return res;
    }
    if (version == DW_STR_OFFSETS_VERSION5) {
        /*  header_length includes length field plus 4 for
            the rest of the header. Don't double count!
            length includes space for 4 bytes of the header!  */
        array_start_offset = table_header_offset +
            local_offset_to_array;
        table_end_offset = array_start_offset + total_table_length;
    } else {
        Dwarf_Unsigned space_left = 0;

        /* leave table header offset as-is */
        space_left = sot->so_section_size - table_header_offset;
        array_start_offset = table_header_offset;
        table_end_offset = table_header_offset + space_left;
    }
    {
        Dwarf_Unsigned entrycount = 0;
        Dwarf_Unsigned entrybytes = 0;

        entrybytes = table_end_offset - array_start_offset;
        if (entrybytes % local_length_size) {
            _dwarf_error_string(dbg,error,
                DW_DLE_STR_OFFSETS_ARRAY_SIZE,
                "DW_DLE_STR_OFFSETS_ARRAY_SIZE "
                " array size not a multiple of the size of "
                "a single entry");
            return DW_DLV_ERROR;
        }
        entrycount = entrybytes/local_length_size;

        /* On the next table loop this will be the new table offset */
        sot->so_next_table_offset = table_end_offset;

        sot->so_table_end_offset = table_end_offset;
        sot->so_table_start_offset = table_header_offset;
        sot->so_table_end_offset = table_header_offset+
            total_table_length;
        sot->so_array_entry_count = entrycount;
        sot->so_array_entry_size = local_length_size;
        sot->so_table_count += 1;

        /*  The data length  in bytes following the unit_length field
            of the header. includes any other header bytes
            and the table space */
        *unit_length_out = length;

        /*  Where the unit_length field starts in the section. */
        *unit_length_offset_out = sot->so_table_start_offset;

        /*  Where the table of offsets starts in the section. */
        *table_start_offset_out = sot->so_table_start_offset;

        /*   Entrysize: 4 or 8 */
        *entry_size_out  = local_length_size;

        /*   Version is 5. */
        *version_out  = version;

        /*   This is supposed to be zero. */
        *padding_out  = padding;

        /*  How many entry_size entries are in the array. */
        *table_value_count_out = entrycount;
        return DW_DLV_OK;
    }
}

/*  Meant to be called after all tables accessed using
    dwarf_next_str_offsets_table(). */
int
dwarf_str_offsets_statistics(Dwarf_Str_Offsets_Table table_data,
    Dwarf_Unsigned * wasted_byte_count,
    Dwarf_Unsigned * table_count,
    Dwarf_Error    * error)
{
    VALIDATE_SOT(table_data)
    if (wasted_byte_count) {
        *wasted_byte_count = table_data->so_wasted_section_bytes;
    }
    if (table_count) {
        *table_count = table_data->so_table_count;
    }
    return DW_DLV_OK;
}
