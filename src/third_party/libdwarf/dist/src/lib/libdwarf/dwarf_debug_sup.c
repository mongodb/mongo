/*
Copyright (c) 2020, David Anderson
All rights reserved.

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

/*  This provides access to the DWARF5 .debug_sup section. */

#include <config.h>

#include <string.h> /* strlen() */

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
#include "dwarf_global.h"
#include "dwarf_string.h"

static void
get_sup_fields(Dwarf_Debug dbg,
    struct Dwarf_Section_s **sec_out)
{
    if (IS_INVALID_DBG(dbg)) {
        return;
    }
    *sec_out = &dbg->de_debug_sup;
}

static int
load_sup(Dwarf_Debug dbg,
    Dwarf_Error *error)
{
    struct Dwarf_Section_s * sec = 0;
    int res;

    get_sup_fields(dbg,&sec);
    if (!sec) {
        return DW_DLV_NO_ENTRY;
    }
    res = _dwarf_load_section(dbg,sec,error);
    return res;
}

/* New for DWARF5 in July 2020. */
int
dwarf_get_debug_sup(Dwarf_Debug dbg,
    Dwarf_Half     * version_out,
    Dwarf_Small    * is_supplementary_out,
    char          ** filename_out,
    Dwarf_Unsigned * checksum_len_out,
    Dwarf_Small   ** checksum_out,
    Dwarf_Error    * error)
{
    Dwarf_Unsigned version = 0;
    Dwarf_Small    is_supp = 0;
    char          *filename = 0;
    Dwarf_Unsigned checksum_len = 0;
    Dwarf_Small   *checksum_ptr = 0;
    int            res = 0;
    Dwarf_Small   *data = 0;
    Dwarf_Small   *enddata = 0;
    Dwarf_Unsigned size = 0;

    CHECK_DBG(dbg,error,"dwarf_get_debug_sup()");
    res = load_sup(dbg,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    data = dbg->de_debug_sup.dss_data;
    size = dbg->de_debug_sup.dss_size;
    if (dbg->de_filesize && size > dbg->de_filesize) {
        dwarfstring m;

        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m, "DW_DLE_DEBUG_SUP_ERROR: "
            ".debug_sup section size 0x%x bigger than file "
            "size! Corrupt",
            size);
        _dwarf_error_string(dbg,error,
            DW_DLE_DEBUG_SUP_ERROR,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    enddata = data + size;
    res = _dwarf_read_unaligned_ck_wrapper(dbg,
        &version,
        data,DWARF_HALF_SIZE,
        enddata,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    data+= DWARF_HALF_SIZE;
    if ((data+4) > enddata) {
        dwarfstring m;

        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m, "DW_DLE_DEBUG_SUP_ERROR: "
            " .debug_sup section size 0x%x"
            " too small to be correct! Corrupt",
            size);
        _dwarf_error_string(dbg,error,
            DW_DLE_DEBUG_SUP_ERROR,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }

    is_supp = (Dwarf_Small)*data;
    ++data;
    res  = _dwarf_check_string_valid(dbg,data,data,enddata,
        DW_DLE_DEBUG_SUP_STRING_ERROR,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    filename = (char *)data;

    data += strlen((char *)data) +1;

    res = _dwarf_leb128_uword_wrapper(dbg, &data,enddata,
        &checksum_len,error);
    if (res != DW_DLV_OK) {
        return res;
    }

    if (checksum_len >= size) {
        dwarfstring m;

        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m, "DW_DLE_DEBUG_SUP_ERROR: "
            " .debug_sup checksum length 0x%x"
            " too large to be correct! Corrupt",
            checksum_len);
        _dwarf_error_string(dbg,error,
            DW_DLE_DEBUG_SUP_ERROR,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    if ((data +checksum_len) > enddata) {
        dwarfstring m;

        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m, "DW_DLE_DEBUG_SUP_ERROR: "
            " .debug_sup checksum (length 0x%x) "
            " runs off the end of the section, Corrupt data",
            checksum_len);
        _dwarf_error_string(dbg,error,
            DW_DLE_DEBUG_SUP_ERROR,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    checksum_ptr = data;
    if (version_out) {
        *version_out = (Dwarf_Half)version;
    }
    if (is_supp) {
        *is_supplementary_out = is_supp;
    }
    if (filename_out) {
        *filename_out = filename;
    }
    if (checksum_len_out) {
        *checksum_len_out = checksum_len;
    }
    if (checksum_out) {
        *checksum_out  = checksum_ptr;
    }
    return DW_DLV_OK;
}
