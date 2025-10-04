/*
Copyright (C) 2022 David Anderson. All Rights Reserved.

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

#include "config.h"
#include <stddef.h> /* NULL size_t */
#include <stdlib.h> /* calloc() free() malloc() */
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
#include "dwarf_die_deliv.h"
#include "dwarf_abbrev.h"
#include "dwarf_error.h"
#include "dwarf_util.h"
#include "dwarf_string.h"

static void
build_alloc_ab_error(Dwarf_Debug dbg,
    Dwarf_Unsigned count,
    const char *fieldname,
    Dwarf_Error *error)
{
    dwarfstring m;

    dwarfstring_constructor(&m);
    dwarfstring_append_printf_s(&m,
        "DW_DLE_ALLOC_FAIL :"
        " Attempt to malloc space for %s ",
        (char *)fieldname);
    dwarfstring_append_printf_u(&m,
        " with %u entries failed.",
        count);
    _dwarf_error_string(dbg,error,DW_DLE_ALLOC_FAIL,
        dwarfstring_string(&m));
    dwarfstring_destructor(&m);
}

/*
    This is a pre-scan of the abbrev/form list.
    We will not handle DW_FORM_indirect here as that
    accesses data outside of the abbrev section.
*/
int
_dwarf_fill_in_attr_form_abtable(Dwarf_CU_Context context,
    Dwarf_Byte_Ptr abbrev_ptr,
    Dwarf_Byte_Ptr abbrev_end,
    Dwarf_Abbrev_List abbrev_list,
    Dwarf_Error *error)
{
    Dwarf_Debug    dbg = 0;
    Dwarf_Unsigned i = 0;

    dbg = context->cc_dbg;
    abbrev_list->abl_attr = (Dwarf_Half*)
        calloc(abbrev_list->abl_abbrev_count,
            SIZEOFT16);
    if (!abbrev_list->abl_attr) {
        build_alloc_ab_error(dbg,abbrev_list->abl_abbrev_count,
            "abbrev_list->abl_attr",error);
        return DW_DLV_ERROR;
    }
    abbrev_list->abl_form = (Dwarf_Half *)
        calloc(abbrev_list->abl_abbrev_count,
            SIZEOFT16);
    if (!abbrev_list->abl_form) {
        build_alloc_ab_error(dbg,abbrev_list->abl_abbrev_count,
            "abbrev_list->abl_form",error);
        return DW_DLV_ERROR;
    }
    if (abbrev_list->abl_implicit_const_count > 0) {
        abbrev_list->abl_implicit_const = (Dwarf_Signed *)
        calloc(abbrev_list->abl_abbrev_count,
            sizeof(Dwarf_Signed));
        if (!abbrev_list->abl_implicit_const) {
            build_alloc_ab_error(dbg,abbrev_list->abl_abbrev_count,
                "abbrev_list->abl_implicit_const",error);
            return DW_DLV_ERROR;
        }
    }

    for (i = 0; i < abbrev_list->abl_abbrev_count; ++i) {
        Dwarf_Unsigned attr = 0;
        Dwarf_Unsigned attr_form = 0;
        Dwarf_Signed implicit_const = 0;
        int res = 0;

        res = _dwarf_leb128_uword_wrapper(dbg,
            &abbrev_ptr,abbrev_end,&attr,error);
        if (res == DW_DLV_ERROR) {
            return res;
        }
        if (attr > 0xffff) {
            dwarfstring m;
            dwarfstring_constructor(&m);
            dwarfstring_append(&m,
                "DW_DLE_ATTR_FORM_SIZE_BAD :"
                " reading Attribute number ");
            dwarfstring_append(&m," for abbrev list entry"
                " the ULEB number is too large. Corrupt Dwarf.");
            _dwarf_error_string(dbg,error,DW_DLE_ATTR_FORM_SIZE_BAD,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
        abbrev_list->abl_attr[i] = (Dwarf_Half)attr;
        if (attr > DW_AT_hi_user) {
            _dwarf_error(dbg, error,DW_DLE_ATTR_CORRUPT);
            return DW_DLV_ERROR;
        }
        res = _dwarf_leb128_uword_wrapper(dbg,
            &abbrev_ptr,abbrev_end,&attr_form,error);
        if (res == DW_DLV_ERROR) {
            return res;
        }
        if (attr_form > 0xffff) {
            _dwarf_error_string(dbg, error,
                DW_DLE_ATTR_FORM_SIZE_BAD,
                "DW_DLE_ATTR_FORM_SIZE_BAD :"
                " reading attr_form of"
                " an abbrev list entry: "
                "the ULEB form number is too large "
                "to be valid. Corrupt Dwarf.");
            return DW_DLV_ERROR;
        }
        if (!_dwarf_valid_form_we_know(attr_form,attr)) {
            dwarfstring m;

            dwarfstring_constructor(&m);
            dwarfstring_append(&m,"Reading an abbreviation list "
                " we find the attribute  form pair to be "
                " impossible or unknown.");
            dwarfstring_append_printf_u(&m," attr 0x%x ",attr);
            dwarfstring_append_printf_u(&m," attrform 0x%x ",
                attr_form);
            _dwarf_error_string(dbg, error, DW_DLE_UNKNOWN_FORM,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
        abbrev_list->abl_attr[i] = (Dwarf_Half)attr;
        abbrev_list->abl_form[i] = (Dwarf_Half)attr_form;
        if (attr_form == DW_FORM_implicit_const) {
            res = _dwarf_leb128_sword_wrapper(dbg,
                &abbrev_ptr,abbrev_end,&implicit_const,error);
            if (res == DW_DLV_ERROR) {
                return res;
            }
            abbrev_list->abl_implicit_const_count++;
            abbrev_list->abl_implicit_const[i] = implicit_const;
        }
#if 0  /* Do nothing special for DW_FORM_indirect here. Ignore. */
        if (attr_form == DW_FORM_indirect) {
            /*  Do nothing special here. Do not read
                from the DIE till reading for
                a specific DIE, which we are not
                intending here, we do not know
                where the DIE is. */
        }
#endif
    }
    return DW_DLV_OK;
}
