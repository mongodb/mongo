/*
  Copyright (C) 2000,2002,2004,2005 Silicon Graphics, Inc. All Rights Reserved.
  Portions Copyright 2007-2010 Sun Microsystems, Inc. All rights reserved.
  Portions Copyright 2008-2021 David Anderson. All rights reserved.
  Portions Copyright 2010-2012 SN Systems Ltd. All rights reserved.

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

#include <string.h>  /* memcpy() memset() */
#include <stdio.h>  /* printf() */

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
#include "dwarf_string.h"
#include "dwarf_error.h"
#include "dwarf_util.h"
#include "dwarf_die_deliv.h"
#include "dwarf_str_offsets.h"
#include "dwarf_string.h"
#if 0 /* dump_bytes */
static void
dump_bytes(const char *msg,int line,
    Dwarf_Small * start, long len)
{
    Dwarf_Small *end = start + len;
    Dwarf_Small *cur = start;
    printf("%s (0x%lx) line %d\n ",msg,(unsigned long)start,line);
    for (; cur < end; cur++) {
        printf("%02x", *cur);
    }
    printf("\n");
}
#endif /*0*/

/*  It is necessary at times to cause errors of this sort
    in determining what we really have.  So best to avoid
    too much malloc and free, hence the static constructor
    dwarfstring will use malloc if we guess too-small
    for the size of mbuf. */
static void
generate_form_error(Dwarf_Debug dbg,
    Dwarf_Error *error,
    unsigned form,
    int err_code,
    const char *errname,
    const char *funcname)
{
    dwarfstring m; /* constructor_static ok */
    char mbuf[DWARFSTRING_ALLOC_SIZE];
    const char * defaultname = "<unknown form>";

    dwarfstring_constructor_static(&m,mbuf,
        sizeof(mbuf));
    dwarfstring_append(&m,(char *)errname);
    dwarfstring_append(&m,": In function ");
    dwarfstring_append(&m,(char *)funcname);
    dwarfstring_append_printf_u(&m,
        " on seeing form  0x%x ",form);
    dwarf_get_FORM_name(form,&defaultname);
    dwarfstring_append_printf_s(&m,
        " (%s)",(char *)defaultname);
    _dwarf_error_string(dbg,error,err_code,
        dwarfstring_string(&m));
    dwarfstring_destructor(&m);
}

/*  This code was repeated many times, now it
    is all in one place.
    Never returns DW_DLV_NO_ENTRY  */
static int
get_attr_dbg(Dwarf_Debug *dbg_out,
    Dwarf_CU_Context * cu_context_out,
    Dwarf_Attribute attr,
    Dwarf_Error *error)
{
    Dwarf_CU_Context cup = 0;
    Dwarf_Debug dbg = 0;

    if (!attr) {
        _dwarf_error(NULL, error, DW_DLE_ATTR_NULL);
        return DW_DLV_ERROR;
    }
    cup = attr->ar_cu_context;
    if (!cup) {
        _dwarf_error(NULL, error, DW_DLE_ATTR_NO_CU_CONTEXT);
        return DW_DLV_ERROR;
    }
    dbg = cup->cc_dbg;
    if (IS_INVALID_DBG(dbg)) {
        _dwarf_error_string(NULL, error, DW_DLE_ATTR_DBG_NULL,
            "DW_DLE_ATTR_DBG_NULL: Stale or null Dwarf_Debug"
            "in a Dwarf_CU_Context" );
        return DW_DLV_ERROR;
    }
    if (dbg != attr->ar_dbg) {
        _dwarf_error_string(NULL, error, DW_DLE_ATTR_DBG_NULL,
            "DW_DLE_ATTR_DBG_NULL: an attribute and its "
            "cu_context do not have the same Dwarf_Debug" );
        return DW_DLV_ERROR;
    }
    *cu_context_out = cup;
    *dbg_out        = dbg;
    return DW_DLV_OK;

}

/*  This checks the final-form (after any DW_FORM_indirect
    converted to final form). */
int
dwarf_hasform(Dwarf_Attribute attr,
    Dwarf_Half dw_form,
    Dwarf_Bool * dw_return_bool, Dwarf_Error * error)
{
    Dwarf_Debug dbg = 0;
    Dwarf_CU_Context cu_context = 0;
    int res = 0;

    res  =get_attr_dbg(&dbg,&cu_context, attr,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    if (!dw_return_bool) {
        _dwarf_error_string(dbg,error,
            DW_DLE_INVALID_NULL_ARGUMENT,
            " DW_DLE_INVALID_NULL_ARGUMENT "
            "calling dwarf_hasform: "
            "dw_return_bool must be passed"
            " as a non-NULL valid pointer");
        return DW_DLV_ERROR;
    }
    *dw_return_bool = (attr->ar_attribute_form == dw_form);
    return DW_DLV_OK;
}

/* Not often called, we do not worry about efficiency here.
   The dwarf_whatform() call does the sanity checks for us.
   The form returned is the original form, which could
   be DW_FORM_indirect.
*/
int
dwarf_whatform_direct(Dwarf_Attribute attr,
    Dwarf_Half * return_form, Dwarf_Error * error)
{
    int res = dwarf_whatform(attr, return_form, error);

    if (res != DW_DLV_OK) {
        return res;
    }

    *return_form = attr->ar_attribute_form_direct;
    return DW_DLV_OK;
}

/*  This code was contributed around 2007.
    As of 2021 it is not clear that Sun Sparc
    compilers are in current use, nor whether
    there is a reason to make reads of
    this data format safe from corrupted object files.

    Pass in the content of a block and the length of that
    content. On success return DW_DLV_OK and set *value_count
    to the size of the array returned through value_array. */
int
dwarf_uncompress_integer_block_a(Dwarf_Debug dbg,
    Dwarf_Unsigned     input_length_in_bytes,
    void             * input_block,
    Dwarf_Unsigned   * value_count,
    Dwarf_Signed    ** value_array,
    Dwarf_Error      * error)
{
    Dwarf_Unsigned output_length_in_units = 0;
    Dwarf_Signed  *output_block = 0;
    unsigned       i = 0;
    char          *ptr = 0;
    Dwarf_Signed   remain = 0;
    Dwarf_Signed  *array = 0;
    Dwarf_Byte_Ptr endptr = (Dwarf_Byte_Ptr)input_block+
        input_length_in_bytes;

    CHECK_DBG(dbg,error,"dwarf_uncompress_integer_block_a()");
    output_length_in_units = 0;
    remain = (Dwarf_Signed)input_length_in_bytes;
    ptr = input_block;
    while (remain > 0) {
        Dwarf_Unsigned len = 0;
        Dwarf_Signed value = 0;
        int rres = 0;

        rres = dwarf_decode_signed_leb128((char *)ptr,
            &len, &value,(char *)endptr);
        if (rres != DW_DLV_OK) {
            _dwarf_error(NULL, error, DW_DLE_LEB_IMPROPER);
            return DW_DLV_ERROR;
        }
        ptr += len;
        remain -= (Dwarf_Signed)len;
        output_length_in_units++;
    }
    if (remain != 0) {
        _dwarf_error(NULL, error, DW_DLE_ALLOC_FAIL);
        return DW_DLV_ERROR;
    }

    output_block = (Dwarf_Signed*)
        _dwarf_get_alloc(dbg,
            DW_DLA_STRING,
            output_length_in_units * sizeof(Dwarf_Signed));
    if (!output_block) {
        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
        return DW_DLV_ERROR;
    }
    array = output_block;
    remain = input_length_in_bytes;
    ptr = input_block;
    for (i=0; i<output_length_in_units && remain>0; i++) {
        Dwarf_Signed num;
        Dwarf_Unsigned len;
        int sres = 0;

        sres = dwarf_decode_signed_leb128((char *)ptr,
            &len, &num,(char *)endptr);
        if (sres != DW_DLV_OK) {
            dwarf_dealloc(dbg,output_block,DW_DLA_STRING);
            _dwarf_error(NULL, error, DW_DLE_LEB_IMPROPER);
            return DW_DLV_ERROR;
        }
        ptr += len;
        remain -= (Dwarf_Signed)len;
        array[i] = num;
    }

    if (remain != 0) {
        dwarf_dealloc(dbg, (unsigned char *)output_block,
            DW_DLA_STRING);
        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
        return DW_DLV_ERROR;
    }

    *value_count = output_length_in_units;
    *value_array = output_block;
    return DW_DLV_OK;
}

/*  This code was contributed around 2007
    and the return value is in the wrong form.
    See dwarf_uncompress_integer_block_a() above.

    As of 2019 it is not clear that Sun Sparc
    compilers are in current use, nor whether
    there is a reason to make reads of
    this data format safe from corrupted object files.
*/

void
dwarf_dealloc_uncompressed_block(Dwarf_Debug dbg, void * space)
{
    dwarf_dealloc(dbg, space, DW_DLA_STRING);
}

/*  Never returns DW_DLV_NO_ENTRY
    Returns the final form after any DW_FORM_indirect
    resolved to final form. */
int
dwarf_whatform(Dwarf_Attribute attr,
    Dwarf_Half * return_form, Dwarf_Error * error)
{
    Dwarf_CU_Context cu_context = 0;
    Dwarf_Debug dbg = 0;

    int res  =get_attr_dbg(&dbg,&cu_context, attr,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    *return_form = attr->ar_attribute_form;
    return DW_DLV_OK;
}

/*
    This function is analogous to dwarf_whatform.
    It returns the attribute in attr instead of
    the form.
*/
int
dwarf_whatattr(Dwarf_Attribute attr,
    Dwarf_Half * return_attr, Dwarf_Error * error)
{
    Dwarf_CU_Context cu_context = 0;
    Dwarf_Debug dbg = 0;

    int res  =get_attr_dbg(&dbg,&cu_context, attr,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    *return_attr = (attr->ar_attribute);
    return DW_DLV_OK;
}

/*  Convert an offset within the local CU into a section-relative
    debug_info (or debug_types) offset.
    See dwarf_global_formref() and dwarf_formref()
    for additional information on conversion rules.
*/
int
dwarf_convert_to_global_offset(Dwarf_Attribute attr,
    Dwarf_Off offset,
    Dwarf_Off * ret_offset,
    Dwarf_Error * error)
{
    Dwarf_Debug dbg = 0;
    Dwarf_CU_Context cu_context = 0;
    int res = 0;

    res  = get_attr_dbg(&dbg,&cu_context,attr,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    switch (attr->ar_attribute_form) {
    case DW_FORM_ref1:
    case DW_FORM_ref2:
    case DW_FORM_ref4:
    case DW_FORM_ref8:
    case DW_FORM_ref_udata:
        /*  It is a cu-local offset. Convert to section-global. */
        /*  It would be nice to put some code to check
            legality of the offset */
        /*  cc_debug_offset always has any DWP Package File
            offset included (when the cu_context created)
            so there is no extra work for DWP.
            Globalize the offset */
        offset += cu_context->cc_debug_offset;

        break;

    case DW_FORM_ref_addr:
        /*  This offset is defined to be debug_info global already, so
            use this value unaltered.

            Since a DWP package file is not relocated there
            is no way that this reference offset to an address in
            any other CU can be correct for a DWP Package File offset
            */
        break;
    default: {
        dwarfstring m;

        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            "DW_DLE_BAD_REF_FORM. The form "
            "code is 0x%x which cannot be converted to a global "
            " offset by "
            "dwarf_convert_to_global_offset()",
            attr->ar_attribute_form);
        _dwarf_error_string(dbg, error, DW_DLE_BAD_REF_FORM,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
        }
    }

    *ret_offset = (offset);
    return DW_DLV_OK;
}

/*  A global offset cannot be returned by this interface:
    see dwarf_global_formref().

    DW_FORM_ref_addr is considered an incorrect form
    for this call because DW_FORM_ref_addr is a global-offset into
    the debug_info section.

    For the same reason DW_FORM_data4/data8 are not returned
    from this function.

    For the same reason DW_FORM_sec_offset is not returned
    from this function, DW_FORM_sec_offset is a global offset
    (to various sections, not a CU relative offset.

    DW_FORM_ref_addr has a value which was documented in
    DWARF2 as address-size but which was always an offset
    so should have always been offset size (wording
    corrected in DWARF3).
    The dwarfstd.org FAQ "How big is a DW_FORM_ref_addr?"
    suggested all should use offset-size, but that suggestion
    seems to have been ignored in favor of doing what the
    DWARF2 and 3 standards actually say.

    November, 2010: *ret_offset is always set now.
    Even in case of error.
    Set to zero for most errors, but for
        DW_DLE_ATTR_FORM_OFFSET_BAD
    *ret_offset is set to the bad offset.

    DW_FORM_addrx
    DW_FORM_strx
    DW_FORM_LLVM_addrx_offset
    DW_FORM_rnglistx
    DW_FORM_GNU_addr_index
    DW_FORM_GNU_str_index
    are not references to .debug_info/.debug_types,
    so they are not allowed here. */

static void
show_not_ref_error(Dwarf_Debug dbg,
    Dwarf_Error *error,
    Dwarf_Half form,
    Dwarf_Half attr)
{
    dwarfstring m;
    const char *fname = 0;
    const char *aname = 0;

    /*DW_DLE_NOT_REF_FORM */
    dwarfstring_constructor(&m);

    dwarf_get_FORM_name(form,&fname);
    dwarf_get_AT_name(attr,&aname);
    dwarfstring_append(&m,"DW_DLE_NOT_REF_FORM: ");
    dwarfstring_append(&m,(char *)fname);
    dwarfstring_append_printf_s(&m," (on attribute %s )",
        (char *)aname);
    dwarfstring_append(&m," for versions >= 4 ");
    dwarfstring_append(&m,"is not a valid reference form");
    _dwarf_error_string(dbg,error,DW_DLE_NOT_REF_FORM,
        dwarfstring_string(&m));
    dwarfstring_destructor(&m);
}

int
dwarf_formref(Dwarf_Attribute attr,
    Dwarf_Off * ret_offset,
    Dwarf_Bool * ret_is_info,
    Dwarf_Error * error)
{
    Dwarf_Debug dbg = 0;
    Dwarf_Unsigned offset = 0;
    Dwarf_CU_Context cu_context = 0;
    Dwarf_Unsigned maximumoffset = 0;
    int res = DW_DLV_ERROR;
    Dwarf_Byte_Ptr section_end = 0;
    Dwarf_Bool is_info = TRUE;

    *ret_offset = 0;
    res  = get_attr_dbg(&dbg,&cu_context,attr,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    section_end =
        _dwarf_calculate_info_section_end_ptr(cu_context);
    is_info = cu_context->cc_is_info;

    switch (attr->ar_attribute_form) {

    case DW_FORM_ref1:
        offset = *(Dwarf_Small *) attr->ar_debug_ptr;
        break;

    case DW_FORM_ref2:
        READ_UNALIGNED_CK(dbg, offset, Dwarf_Unsigned,
            attr->ar_debug_ptr, DWARF_HALF_SIZE,
            error,section_end);
        break;

    case DW_FORM_ref4:
        READ_UNALIGNED_CK(dbg, offset, Dwarf_Unsigned,
            attr->ar_debug_ptr, DWARF_32BIT_SIZE,
            error,section_end);
        break;

    case DW_FORM_ref8:
        READ_UNALIGNED_CK(dbg, offset, Dwarf_Unsigned,
            attr->ar_debug_ptr, DWARF_64BIT_SIZE,
            error,section_end);
        break;

    case DW_FORM_ref_udata: {
        Dwarf_Byte_Ptr ptr = attr->ar_debug_ptr;
        Dwarf_Unsigned localoffset = 0;
        DECODE_LEB128_UWORD_CK(ptr,localoffset,
            dbg,error,section_end);
        offset = localoffset;
        break;
    }
    case DW_FORM_ref_sig8: {
        /*  We need to look for a local reference here.
            The function we are in is only CU_local
            offsets returned. */
#if 0  /* check for a local sig8 reference unimplemented. */
        Dwarf_Sig8 sig8;
        memcpy(&sig8,ptr,sizeof(Dwarf_Sig8));
        res = dwarf_find_die_given_sig8(dbg,
            &sig8, ...
        We could look, then determine if
        resulting offset is actually local.
#endif /*0*/

        /*  We cannot handle this here.
            The reference could be to .debug_types
            or another CU!
            not a .debug_info CU local offset. */
        _dwarf_error(dbg, error, DW_DLE_REF_SIG8_NOT_HANDLED);
        return DW_DLV_ERROR;
    }
    default: {
        dwarfstring m;
        const char * fname = 0;
        const char * aname = 0;

        dwarf_get_FORM_name(attr->ar_attribute_form,&fname);
        dwarf_get_AT_name(attr->ar_attribute,&aname);
        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            "DW_DLE_BAD_REF_FORM. The form "
            "code is 0x%x ",
            attr->ar_attribute_form);
        dwarfstring_append(&m,(char *)fname);
        dwarfstring_append_printf_s(&m," on attribute %s, "
            "which does not have an offset"
            " for dwarf_formref() to return.",(char *)aname);
        _dwarf_error_string(dbg, error, DW_DLE_BAD_REF_FORM,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
        }
    }

    /*  Check that offset is within current
        cu portion of .debug_info. */

    maximumoffset = cu_context->cc_length +
        cu_context->cc_length_size +
        cu_context->cc_extension_size;
    if (offset >= maximumoffset) {
        /*  For the DW_TAG_compile_unit is legal to have the
            DW_AT_sibling attribute outside the current cu portion of
            .debug_info.
            In other words, sibling points to the end of the CU.
            It is used for precompiled headers.
            The valid condition will be: 'offset == maximumoffset'. */
        Dwarf_Half tag = 0;
        int tres = dwarf_tag(attr->ar_die,&tag,error);
        if (tres != DW_DLV_OK) {
            if (tres == DW_DLV_NO_ENTRY) {
                _dwarf_error(dbg, error, DW_DLE_NO_TAG_FOR_DIE);
                return DW_DLV_ERROR;
            }
            return DW_DLV_ERROR;
        }

        if (DW_TAG_compile_unit != tag &&
            DW_AT_sibling != attr->ar_attribute &&
            offset > maximumoffset) {
            _dwarf_error(dbg, error, DW_DLE_ATTR_FORM_OFFSET_BAD);
            /*  Return the incorrect offset for better
                error reporting */
            *ret_offset = (offset);
            return DW_DLV_ERROR;
        }
    }
    *ret_is_info = is_info;
    *ret_offset = (offset);
    return DW_DLV_OK;
}

static int
_dwarf_formsig8_internal(Dwarf_Attribute attr,
    int formexpected,
    Dwarf_Sig8 * returned_sig_bytes,
    Dwarf_Error*     error)
{
    Dwarf_Debug dbg = 0;
    Dwarf_CU_Context cu_context = 0;
    Dwarf_Byte_Ptr  field_end = 0;
    Dwarf_Byte_Ptr  section_end = 0;

    int res  = get_attr_dbg(&dbg,&cu_context,attr,error);
    if (res != DW_DLV_OK) {
        return res;
    }

    if (attr->ar_attribute_form != formexpected) {
        return DW_DLV_NO_ENTRY;
    }
    section_end =
        _dwarf_calculate_info_section_end_ptr(cu_context);
    field_end = attr->ar_debug_ptr + sizeof(Dwarf_Sig8);
    if (field_end > section_end) {
        _dwarf_error(dbg, error, DW_DLE_ATTR_FORM_OFFSET_BAD);
        return DW_DLV_ERROR;
    }

    memcpy(returned_sig_bytes, attr->ar_debug_ptr,
        sizeof(*returned_sig_bytes));
    return DW_DLV_OK;
}

int
dwarf_formsig8_const(Dwarf_Attribute attr,
    Dwarf_Sig8 * returned_sig_bytes,
    Dwarf_Error* error)
{
    int res  =_dwarf_formsig8_internal(attr, DW_FORM_data8,
        returned_sig_bytes,error);
    return res;
}

/*  dwarf_formsig8 returns in the caller-provided 8 byte area
    the 8 bytes of a DW_FORM_ref_sig8 (copying the bytes
    directly to the caller).  Not a string, an 8 byte
    MD5 hash or a signature.
    This function is new in DWARF4 libdwarf and used in
    more places in DWARF5.
*/
int
dwarf_formsig8(Dwarf_Attribute attr,
    Dwarf_Sig8 * returned_sig_bytes,
    Dwarf_Error* error)
{
    int res  = _dwarf_formsig8_internal(attr, DW_FORM_ref_sig8,
        returned_sig_bytes,error);
    return res;
}

/*  This finds a target via a sig8 and if
    DWARF4 is likely finding a reference from .debug_info
    to .debug_types.  So the offset may or may not be
    in the same section if DWARF4.
    context_level prevents infinite loop
    during CU_Context creation */
static int
find_sig8_target_as_global_offset(Dwarf_Attribute attr,
    int context_level,
    Dwarf_Sig8  *sig8,
    Dwarf_Bool  *is_info,
    Dwarf_Off   *targoffset,
    Dwarf_Error *error)
{
    Dwarf_Die  targdie = 0;
    Dwarf_Bool targ_is_info = 0;
    Dwarf_Off  localoff = 0;
    int        res = 0;
    int        resb = 0;

    targ_is_info = attr->ar_cu_context->cc_is_info;
    memcpy(sig8,attr->ar_debug_ptr,sizeof(*sig8));
    res = _dwarf_internal_find_die_given_sig8(attr->ar_dbg,
        context_level,
        sig8,&targdie,&targ_is_info,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    resb = dwarf_die_offsets(targdie,targoffset,&localoff,error);
    if (resb != DW_DLV_OK) {
        dwarf_dealloc_die(targdie);
        return resb;
    }
    *is_info = targdie->di_cu_context->cc_is_info;
    dwarf_dealloc_die(targdie);
    return DW_DLV_OK;
}

/*  Since this returns section-relative debug_info offsets,
    this can represent all REFERENCE forms correctly
    and allows all applicable forms.

    DW_FORM_ref_addr has a value which was documented in
    DWARF2 as address-size but which was always an offset
    so should have always been offset size (wording
    corrected in DWARF3).
        gcc and Go and libdwarf producer code
    define the length of the value of DW_FORM_ref_addr
    per the version. So for V2 it is address-size and V3 and later
    it is offset-size.

    See the DWARF4 document for the 3 cases fitting
    reference forms.  The caller must determine which section the
    reference 'points' to.  The function added in November 2009,
    dwarf_get_form_class(), helps in this regard.

    unlike dwarf_formref(), this allows references to
    sections other than just .debug_info/.debug_types.
    See case DW_FORM_sec_offset:
    case DW_FORM_GNU_ref_alt:   2013 GNU extension
    case DW_FORM_GNU_strp_alt:  2013 GNU extension
    case DW_FORM_strp_sup:      DWARF5, sup string section
    case DW_FORM_line_strp:     DWARF5, .debug_line_str section
*/

/*  This follows DW_FORM_ref_sig8 so could got
    to any CU and from debug_info to debug_types
    (or vice versa?)
    dwarf_global_formref_b is aimed at for DIE references.
    Only the DW_FORM_ref_sig8 form can change
    from a cu_context in .debug_info
    to one in .debug_types (DWARF4 only).
    For references to other sections it is simpler
    to call the original: dwarf_global_formref.
*/
int
dwarf_global_formref(Dwarf_Attribute attr,
    Dwarf_Off * ret_offset,
    Dwarf_Error * error)
{
    Dwarf_Bool is_info = 0;
    int res = 0;

    res = dwarf_global_formref_b(attr,ret_offset,
        &is_info,error);
    return res;
}

/*  If context_level is 0, normal call
    But if non-zero will avoid creating CU Context. */
int
dwarf_global_formref_b(Dwarf_Attribute attr,
    Dwarf_Off * ret_offset,
    Dwarf_Bool * offset_is_info,
    Dwarf_Error * error)
{
    int res = 0;
    int context_level = 0;
    res = _dwarf_internal_global_formref_b( attr,
        context_level,
        ret_offset,
        offset_is_info,
        error);
    return res;
}
/*  If context_level is 0, normal call
    But if non-zero will avoid creating CU Context. */
int
_dwarf_internal_global_formref_b(Dwarf_Attribute attr,
    int context_level,
    Dwarf_Off * ret_offset,
    Dwarf_Bool * offset_is_info,
    Dwarf_Error * error)
{
    Dwarf_Debug dbg = 0;
    Dwarf_Unsigned offset = 0;
    Dwarf_CU_Context cu_context = 0;
    Dwarf_Half context_version = 0;
    Dwarf_Byte_Ptr section_end = 0;
    Dwarf_Bool is_info = TRUE;

    int res  = get_attr_dbg(&dbg,&cu_context,attr,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    section_end =
        _dwarf_calculate_info_section_end_ptr(cu_context);
    context_version = cu_context->cc_version_stamp;
    is_info = cu_context->cc_is_info;
    switch (attr->ar_attribute_form) {

    case DW_FORM_ref1:
        if (attr->ar_debug_ptr >= section_end) {
            _dwarf_error_string(dbg, error,
                DW_DLE_ATTR_FORM_OFFSET_BAD,
                "DW_DLE_ATTR_FORM_OFFSET_BAD: "
                "DW_FORM_ref1 outside of the section.");
            return DW_DLV_ERROR;
        }
        offset = *(Dwarf_Small *) attr->ar_debug_ptr;
        goto fixoffset;

    case DW_FORM_ref2:
        READ_UNALIGNED_CK(dbg, offset, Dwarf_Unsigned,
            attr->ar_debug_ptr, DWARF_HALF_SIZE,
            error,section_end);
        goto fixoffset;

    case DW_FORM_ref4:
        READ_UNALIGNED_CK(dbg, offset, Dwarf_Unsigned,
            attr->ar_debug_ptr, DWARF_32BIT_SIZE,
            error,section_end);
        goto fixoffset;

    case DW_FORM_ref8:
        READ_UNALIGNED_CK(dbg, offset, Dwarf_Unsigned,
            attr->ar_debug_ptr, DWARF_64BIT_SIZE,
            error,section_end);
        goto fixoffset;

    case DW_FORM_ref_udata:
        {
        Dwarf_Byte_Ptr ptr = attr->ar_debug_ptr;
        Dwarf_Unsigned localoffset = 0;

        DECODE_LEB128_UWORD_CK(ptr,localoffset,
            dbg,error,section_end);
        offset = localoffset;

        fixoffset: /* we have a local offset, make it global */

        /* check legality of offset */
        if (offset >= cu_context->cc_length +
            cu_context->cc_length_size +
            cu_context->cc_extension_size) {
            _dwarf_error(dbg, error, DW_DLE_ATTR_FORM_OFFSET_BAD);
            return DW_DLV_ERROR;
        }

        /* globalize the offset */
        offset += cu_context->cc_debug_offset;
        }
        break;

    /*  The DWARF2 document did not make clear that
        DW_FORM_data4( and 8) were references with
        global offsets to some section.
        That was first clearly documented in DWARF3.
        In DWARF4 these two forms are no longer references. */
    case DW_FORM_data4:
        if (context_version >= DW_CU_VERSION4) {
            show_not_ref_error(dbg,error,attr->ar_attribute_form,
                attr->ar_attribute);
            return DW_DLV_ERROR;
        }
        READ_UNALIGNED_CK(dbg, offset, Dwarf_Unsigned,
            attr->ar_debug_ptr, DWARF_32BIT_SIZE,
            error, section_end);
        /* The offset is global. */
        break;
    case DW_FORM_data8:
        if (context_version >= DW_CU_VERSION4) {
            show_not_ref_error(dbg,error,attr->ar_attribute_form,
                attr->ar_attribute);
            return DW_DLV_ERROR;
        }
        READ_UNALIGNED_CK(dbg, offset, Dwarf_Unsigned,
            attr->ar_debug_ptr, DWARF_64BIT_SIZE,
            error,section_end);
        /* The offset is global. */
        break;
    case DW_FORM_ref_addr:
        {
            /*  In Dwarf V2 DW_FORM_ref_addr was defined
                as address-size even though it is a .debug_info
                offset.  Fixed in Dwarf V3 to be offset-size.
                */
            unsigned length_size = 0;
            if (context_version == 2) {
                length_size = cu_context->cc_address_size;
            } else {
                length_size = cu_context->cc_length_size;
            }
            if (length_size == 4) {
                READ_UNALIGNED_CK(dbg, offset, Dwarf_Unsigned,
                    attr->ar_debug_ptr, DWARF_32BIT_SIZE,
                    error,section_end);
            } else if (length_size == 8) {
                READ_UNALIGNED_CK(dbg, offset, Dwarf_Unsigned,
                    attr->ar_debug_ptr, DWARF_64BIT_SIZE,
                    error,section_end);
            } else {
                _dwarf_error(dbg, error,
                    DW_DLE_FORM_SEC_OFFSET_LENGTH_BAD);
                return DW_DLV_ERROR;
            }
        }
        break;
    /*  Index into .debug_rnglists/.debug_loclists section.
        Return the index itself. */
    case DW_FORM_loclistx:
    case DW_FORM_rnglistx: {
        Dwarf_Unsigned val = 0;
        DECODE_LEB128_UWORD_CK(attr->ar_debug_ptr,
            val, dbg,error,section_end);
        offset = val;
        }
        break;
    case DW_FORM_sec_offset:
    case DW_FORM_GNU_ref_alt:  /* 2013 GNU extension */
    case DW_FORM_GNU_strp_alt: /* 2013 GNU extension */
    case DW_FORM_strp_sup:     /* DWARF5, sup string section */
    case DW_FORM_line_strp:    /* DWARF5, .debug_line_str section */
        {
            /*  DW_FORM_sec_offset first exists in DWARF4.*/
            /*  It is up to the caller to know what the offset
                of DW_FORM_sec_offset, DW_FORM_strp_sup
                or DW_FORM_GNU_strp_alt etc refer to,
                the offset is not going to refer to .debug_info! */
            unsigned length_size = cu_context->cc_length_size;
            if (length_size == 4) {
                READ_UNALIGNED_CK(dbg, offset, Dwarf_Unsigned,
                    attr->ar_debug_ptr, DWARF_32BIT_SIZE,
                    error,section_end);
            } else if (length_size == 8) {
                READ_UNALIGNED_CK(dbg, offset, Dwarf_Unsigned,
                    attr->ar_debug_ptr, DWARF_64BIT_SIZE,
                    error,section_end);
            } else {
                _dwarf_error(dbg, error,
                    DW_DLE_FORM_SEC_OFFSET_LENGTH_BAD);
                return DW_DLV_ERROR;
            }
        }
        break;
    case DW_FORM_ref_sig8: {
        /*  This, in DWARF4, is how
            .debug_info refers to .debug_types. */
        Dwarf_Sig8 sig8;
        Dwarf_Bool t_is_info = TRUE;
        Dwarf_Unsigned t_offset = 0;

        if ((attr->ar_debug_ptr + sizeof(Dwarf_Sig8)) > section_end) {
            _dwarf_error_string(dbg, error,
                DW_DLE_REF_SIG8_NOT_HANDLED,
                "DW_DLE_REF_SIG8_NOT_HANDLED: "
                " Dwarf_Sig8 content runs off the end of "
                "its section");
            return DW_DLV_ERROR;
        }
        memcpy(&sig8,attr->ar_debug_ptr,sizeof(Dwarf_Sig8));
        res = find_sig8_target_as_global_offset(attr,
            context_level,
            &sig8,&t_is_info,&t_offset,error);
        if (res == DW_DLV_ERROR) {

            /*  Lets construct an easily usable error number.
                Avoiding resizing strings and avoiding
                using the stack for strings possibly
                a few hundred bytes long */
            if (error) {
                dwarfstring m;
                dwarfstring k;

                dwarfstring_constructor_fixed(&m,400);
                dwarfstring_constructor_fixed(&k,200);
                /* *error non null */
                dwarfstring_append(&k,dwarf_errmsg(*error));
                dwarfstring_append(&m,
                "DW_DLE_REF_SIG8_NOT_HANDLED: "
                " problem finding target. ");
                dwarf_dealloc_error(dbg,*error);/* *error nonnull*/
                *error = 0; /*error nonnull*/
                dwarfstring_append(&m,dwarfstring_string(&k));
                dwarfstring_destructor(&k);
                _dwarf_error_string(dbg, error,
                    DW_DLE_REF_SIG8_NOT_HANDLED,
                    dwarfstring_string(&m));
                dwarfstring_destructor(&m);
            }
            return DW_DLV_ERROR;
        }
        if (res == DW_DLV_NO_ENTRY) {
            return res;
        }
        is_info = t_is_info;
        offset = t_offset;
        break;
    }
    default: {
        dwarfstring m;
        int formcode = attr->ar_attribute_form;
        int fcres = 0;
        const char *name = 0;

        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            "DW_DLE_BAD_REF_FORM: The form code is 0x%x.. ",
            formcode);
        fcres  = dwarf_get_FORM_name (formcode,&name);
        if (fcres != DW_DLV_OK) {
            name="<UnknownFormCode>";
        }
        dwarfstring_append_printf_s(&m,
            " %s.",(char *)name);
        _dwarf_error_string(dbg, error, DW_DLE_BAD_REF_FORM,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
        }
    }

    *offset_is_info = is_info;
    *ret_offset = offset;
    return DW_DLV_OK;
}

/*  Part of DebugFission.  So a consumer can get the index when
    the object with the actual debug_addr  is
    elsewhere.  New May 2014*/

int
_dwarf_get_addr_index_itself(int theform,
    Dwarf_Small *info_ptr,
    Dwarf_Debug dbg,
    Dwarf_CU_Context cu_context,
    Dwarf_Unsigned *val_out,
    Dwarf_Error * error)
{
    Dwarf_Unsigned index = 0;
    Dwarf_Byte_Ptr section_end = 0;

    section_end =
        _dwarf_calculate_info_section_end_ptr(cu_context);
    switch(theform){
    case DW_FORM_LLVM_addrx_offset: {
        Dwarf_Unsigned tmp = 0;
        Dwarf_Unsigned tmp2 = 0;
        DECODE_LEB128_UWORD_CK(info_ptr,tmp,
            dbg,error,section_end);
        READ_UNALIGNED_CK(dbg, tmp2, Dwarf_Unsigned,
            info_ptr, SIZEOFT32,
            error,section_end);
        index = (tmp<<32) | tmp2;
        break;
    }
    case DW_FORM_GNU_addr_index:
    case DW_FORM_addrx:
        DECODE_LEB128_UWORD_CK(info_ptr,index,
            dbg,error,section_end);
        break;
    case DW_FORM_addrx1:
        READ_UNALIGNED_CK(dbg, index, Dwarf_Unsigned,
            info_ptr, 1,
            error,section_end);
        break;
    case DW_FORM_addrx2:
        READ_UNALIGNED_CK(dbg, index, Dwarf_Unsigned,
            info_ptr, 2,
            error,section_end);
        break;
    case DW_FORM_addrx3:
        READ_UNALIGNED_CK(dbg, index, Dwarf_Unsigned,
            info_ptr, 3,
            error,section_end);
        break;
    case DW_FORM_addrx4:
        READ_UNALIGNED_CK(dbg, index, Dwarf_Unsigned,
            info_ptr, 4,
            error,section_end);
        break;
    default:
        _dwarf_error(dbg, error, DW_DLE_ATTR_FORM_NOT_ADDR_INDEX);
        return DW_DLV_ERROR;
    }
    /*  At this point we do not know for sure
        if the index refers
        to a local .debug_addr or a tied file .debug_addr
        so lets be cautious. */
#if 0 /* Attempted check for index uncertain, unwise. Ignore. */
    /* See de_secondary_dbg before using this */
    if (!dbg->de_tied_data.td_tied_object &&
        index > dbg->de_filesize) {
        _dwarf_error_string(dbg,error,DW_DLE_ATTR_FORM_OFFSET_BAD,
            "DW_DLE_ATTR_FORM_OFFSET_BAD "
            "reading an indexed form addr the index "
            "read is impossibly large (no tied file "
            "available). Corrupt Dwarf.");
        return DW_DLV_ERROR;
    }
#endif
    *val_out = index;
    return DW_DLV_OK;
}

int
dwarf_get_debug_addr_index(Dwarf_Attribute attr,
    Dwarf_Unsigned * return_index,
    Dwarf_Error * error)
{
    int theform = 0;
    Dwarf_CU_Context cu_context = 0;
    Dwarf_Debug dbg = 0;

    int res  = get_attr_dbg(&dbg,&cu_context,attr,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    theform = attr->ar_attribute_form;
    if (dwarf_addr_form_is_indexed(theform)) {
        Dwarf_Unsigned index = 0;

        res = _dwarf_get_addr_index_itself(theform,
            attr->ar_debug_ptr,dbg,cu_context,&index,error);
        *return_index = index;
        return res;
    }

    _dwarf_error(dbg, error, DW_DLE_ATTR_FORM_NOT_ADDR_INDEX);
    return DW_DLV_ERROR;
}

/*  The index value here is the value of the
    attribute with this form.
    FORMs passed in are always strx forms. */
static int
dw_read_str_index_val_itself(Dwarf_Debug dbg,
    unsigned theform,
    Dwarf_Small *info_ptr,
    Dwarf_Small *section_end,
    Dwarf_Unsigned *return_index,
    Dwarf_Error *error)
{
    Dwarf_Unsigned index = 0;

    switch(theform) {
    case DW_FORM_strx:
    case DW_FORM_GNU_str_index:
        DECODE_LEB128_UWORD_CK(info_ptr,index,
            dbg,error,section_end);
        break;
    case DW_FORM_strx1:
        READ_UNALIGNED_CK(dbg, index, Dwarf_Unsigned,
            info_ptr, 1,
            error,section_end);
        break;
    case DW_FORM_strx2:
        READ_UNALIGNED_CK(dbg, index, Dwarf_Unsigned,
            info_ptr, 2,
            error,section_end);
        break;
    case DW_FORM_strx3:
        READ_UNALIGNED_CK(dbg, index, Dwarf_Unsigned,
            info_ptr, 3,
            error,section_end);
        break;
    case DW_FORM_strx4:
        READ_UNALIGNED_CK(dbg, index, Dwarf_Unsigned,
            info_ptr, 4,
            error,section_end);
        break;
    default:
        _dwarf_error(dbg, error, DW_DLE_ATTR_FORM_NOT_STR_INDEX);
        return DW_DLV_ERROR;
    }
    *return_index = index;
    return DW_DLV_OK;
}

/*  Part of DebugFission.  So a dwarf dumper application
    can get the index and print it for the user.
    A convenience function.  New May 2014
    Also used with DWARF5 forms.  */
int
dwarf_get_debug_str_index(Dwarf_Attribute attr,
    Dwarf_Unsigned *return_index,
    Dwarf_Error *error)
{
    int theform = attr->ar_attribute_form;
    Dwarf_CU_Context cu_context = 0;
    Dwarf_Debug dbg = 0;
    int res  = 0;
    Dwarf_Byte_Ptr section_end =  0;
    Dwarf_Unsigned index = 0;
    Dwarf_Small *info_ptr = 0;
    int indxres = 0;
    Dwarf_Unsigned length_size = 0;
    Dwarf_Unsigned sectionlen = 0;

    res = get_attr_dbg(&dbg,&cu_context,attr,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    section_end =
        _dwarf_calculate_info_section_end_ptr(cu_context);
    info_ptr = attr->ar_debug_ptr;

    indxres = dw_read_str_index_val_itself(dbg, theform, info_ptr,
        section_end, &index,error);
    if (indxres == DW_DLV_OK) {
        *return_index = index;
        return indxres;
    }
    length_size = cu_context->cc_length_size;
    sectionlen = dbg->de_debug_str_offsets.dss_size;
    if (index > sectionlen ||
        (index *length_size) > sectionlen) {
        _dwarf_error_string(dbg, error,
            DW_DLE_ATTR_FORM_SIZE_BAD,
            "DW_DLE_ATTR_FORM_SIZE_BAD: "
            "An Attribute Value (index  into "
            ".debug_str_offsets) is Impossibly "
            "large. Corrupt Dwarf.");
        return DW_DLV_ERROR;
    }
    return indxres;
}

int
_dwarf_extract_data16(Dwarf_Debug dbg,
    Dwarf_Small *data,
    Dwarf_Small *section_start,
    Dwarf_Small *section_end,
    Dwarf_Form_Data16  * returned_val,
    Dwarf_Error *error)
{
    Dwarf_Small *data16end = 0;

    data16end = data + sizeof(Dwarf_Form_Data16);
    if (data  < section_start ||
        section_end < data16end) {
        _dwarf_error(dbg, error,DW_DLE_DATA16_OUTSIDE_SECTION);
        return DW_DLV_ERROR;
    }
    memcpy(returned_val, data, sizeof(*returned_val));
    return DW_DLV_OK;

}

int
dwarf_formdata16(Dwarf_Attribute attr,
    Dwarf_Form_Data16  * returned_val,
    Dwarf_Error*     error)
{
    Dwarf_Half attrform = 0;
    Dwarf_CU_Context cu_context = 0;
    Dwarf_Debug dbg = 0;
    int res  = 0;
    Dwarf_Small *section_end = 0;
    Dwarf_Unsigned section_length = 0;
    Dwarf_Small *section_start = 0;

    if (!attr) {
        _dwarf_error(NULL, error, DW_DLE_ATTR_NULL);
        return DW_DLV_ERROR;
    }
    if (!returned_val ) {
        _dwarf_error(NULL, error, DW_DLE_ATTR_NULL);
        return DW_DLV_ERROR;
    }
    res  = get_attr_dbg(&dbg,&cu_context,attr,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    attrform = attr->ar_attribute_form;
    if (attrform != DW_FORM_data16) {
        generate_form_error(dbg,error,attrform,
            DW_DLE_ATTR_FORM_BAD,
            "DW_DLE_ATTR_FORM_BAD",
            "dwarf_formdata16");
        return DW_DLV_ERROR;
    }
    section_start = _dwarf_calculate_info_section_start_ptr(
        cu_context,&section_length);
    section_end = section_start + section_length;

    res = _dwarf_extract_data16(dbg, attr->ar_debug_ptr,
        section_start, section_end,
        returned_val,  error);
    return res;
}

/*  The *addrx are DWARF5 standard.
    The GNU form is non-standard gcc DWARF4
    The LLVM form is the newest. */
Dwarf_Bool
dwarf_addr_form_is_indexed(int form)
{
    switch(form) {
    case DW_FORM_addrx:
    case DW_FORM_addrx1:
    case DW_FORM_addrx2:
    case DW_FORM_addrx3:
    case DW_FORM_addrx4:
    case DW_FORM_GNU_addr_index:
    case DW_FORM_LLVM_addrx_offset:
        return TRUE;
    default: break;
    }
    return FALSE;
}

int
dwarf_formaddr(Dwarf_Attribute attr,
    Dwarf_Addr * return_addr, Dwarf_Error * error)
{
    Dwarf_Debug dbg = 0;
    Dwarf_Addr ret_addr = 0;
    Dwarf_CU_Context cu_context = 0;
    Dwarf_Half attrform = 0;
    int res = 0;

    res  = get_attr_dbg(&dbg,&cu_context,attr,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    attrform = attr->ar_attribute_form;
    if (dwarf_addr_form_is_indexed(attrform)) {
        res = _dwarf_look_in_local_and_tied(
            attrform,
            cu_context,
            attr->ar_debug_ptr,
            return_addr,
            error);
        return res;
    }
    if (attrform == DW_FORM_addr ||
        (cu_context->cc_producer == CC_PROD_METROWERKS &&
        attrform == DW_FORM_ref_addr)
            /* Allowance of
            DW_FORM_ref_addr was a mistake. The value returned in that
            case is NOT an address it is a global debug_info
            offset (ie, not CU-relative offset within the CU
            in debug_info).
            A Metrowerks old C generates ref_adder!
            The DWARF2 document refers to it as an address
            (misleadingly) in sec 6.5.4 where it describes
            the reference form. It is
            address-sized so that the linker can easily update it, but
            it is a reference inside the debug_info section. No longer
            allowed except for Metrowerks C. */
        ) {
        Dwarf_Small *section_end =
            _dwarf_calculate_info_section_end_ptr(cu_context);

        READ_UNALIGNED_CK(dbg, ret_addr, Dwarf_Addr,
            attr->ar_debug_ptr,
            cu_context->cc_address_size,
            error,section_end);
        *return_addr = ret_addr;
        return DW_DLV_OK;
    }
    generate_form_error(dbg,error,attrform,
        DW_DLE_ATTR_FORM_BAD,
        "DW_DLE_ATTR_FORM_BAD",
        "dwarf_formaddr");
    return DW_DLV_ERROR;
}

int
dwarf_formflag(Dwarf_Attribute attr,
    Dwarf_Bool * ret_bool, Dwarf_Error * error)
{
    Dwarf_CU_Context cu_context = 0;
    Dwarf_Debug dbg = 0;

    if (!attr) {
        _dwarf_error(NULL, error, DW_DLE_ATTR_NULL);
        return DW_DLV_ERROR;
    }
    cu_context = attr->ar_cu_context;
    if (!cu_context) {
        _dwarf_error(NULL, error, DW_DLE_ATTR_NO_CU_CONTEXT);
        return DW_DLV_ERROR;
    }
    dbg = cu_context->cc_dbg;
    if (IS_INVALID_DBG(dbg)) {
        _dwarf_error_string(NULL, error, DW_DLE_ATTR_DBG_NULL,
            "DW_DLE_ATTR_DBG_NULL: dwarf_formflag() attribute"
            " passed in has NULL or stale Dwarf_Debug pointer");
        return DW_DLV_ERROR;
    }
    if (dbg != attr->ar_dbg) {
        _dwarf_error_string(NULL, error, DW_DLE_ATTR_DBG_NULL,
            "DW_DLE_ATTR_DBG_NULL: an attribute and its "
            "cu_context do not have the same Dwarf_Debug" );
        return DW_DLV_ERROR;
    }
    if (attr->ar_attribute_form == DW_FORM_flag_present) {
        /*  Implicit means we don't read any data at all. Just
            the existence of the Form does it. DWARF4. */
        *ret_bool = 1;
        return DW_DLV_OK;
    }

    if (attr->ar_attribute_form == DW_FORM_flag) {
        *ret_bool = *(Dwarf_Small *)(attr->ar_debug_ptr);
        return DW_DLV_OK;
    }
    generate_form_error(dbg,error,attr->ar_attribute_form,
        DW_DLE_ATTR_FORM_BAD,
        "DW_DLE_ATTR_FORM_BAD",
        "dwarf_formflat");
    return DW_DLV_ERROR;
}

Dwarf_Bool
_dwarf_allow_formudata(unsigned form)
{
    switch(form) {
    case DW_FORM_data1:
    case DW_FORM_data2:
    case DW_FORM_data4:
    case DW_FORM_data8:
    case DW_FORM_flag:
    case DW_FORM_flag_present:
    case DW_FORM_loclistx:
    case DW_FORM_rnglistx:
        return TRUE;
    default:
        break;
    }
    return FALSE;
}
/*  If the form is DW_FORM_constx and the .debug_addr section
    is missing, this returns DW_DLV_ERROR and the error number
    in the Dwarf_Error is  DW_DLE_MISSING_NEEDED_DEBUG_ADDR_SECTION.
    When that arises, a consumer should call
    dwarf_get_debug_addr_index() and use that on the appropriate
    .debug_addr section in the executable or another object.

    Since this accept some signed values, callers
    must not assume a DW_DLV_OK means
    the value is unsigned. The form is the first clue here.
    If DW_FORM_sdata, then signed. Else unknown sign or
    is unsigned.
*/

int
_dwarf_formudata_internal(Dwarf_Debug dbg,
    Dwarf_Attribute attr,
    unsigned form,
    Dwarf_Byte_Ptr data,
    Dwarf_Byte_Ptr section_end,
    Dwarf_Unsigned *return_uval,
    Dwarf_Unsigned *bytes_read,
    Dwarf_Error *error)
{
    Dwarf_Unsigned ret_value = 0;

    switch (form) {
    case DW_FORM_data1:
        READ_UNALIGNED_CK(dbg, ret_value, Dwarf_Unsigned,
            data, sizeof(Dwarf_Small),
            error,section_end);
        *return_uval = ret_value;
        *bytes_read = 1;
        return DW_DLV_OK;
    case DW_FORM_flag_present:
        *return_uval = 1;
        *bytes_read = 0;
        return DW_DLV_OK;
    case DW_FORM_flag:
        /* equivalent to dwarf_formflag() */
        READ_UNALIGNED_CK(dbg, ret_value, Dwarf_Unsigned,
            data, sizeof(Dwarf_Small),
            error,section_end);
        *return_uval = ret_value;
        *bytes_read = 1;
        return DW_DLV_OK;

    /*  READ_UNALIGNED does the right thing as it reads
        the right number bits and generates host order.
        So we can just assign to *return_uval. */
    case DW_FORM_data2:{
        READ_UNALIGNED_CK(dbg, ret_value, Dwarf_Unsigned,
            data, DWARF_HALF_SIZE,
            error,section_end);
        *return_uval = ret_value;
        *bytes_read = DWARF_HALF_SIZE;
        return DW_DLV_OK;
        }

    case DW_FORM_data4:{
        READ_UNALIGNED_CK(dbg, ret_value, Dwarf_Unsigned,
            data,
            DWARF_32BIT_SIZE,
            error,section_end);
        *return_uval = ret_value;
        *bytes_read = DWARF_32BIT_SIZE;;
        return DW_DLV_OK;
        }

    case DW_FORM_data8:{
        READ_UNALIGNED_CK(dbg, ret_value, Dwarf_Unsigned,
            data,
            DWARF_64BIT_SIZE,
            error,section_end);
        *return_uval = ret_value;
        *bytes_read = DWARF_64BIT_SIZE;
        return DW_DLV_OK;
        }
        break;
    /* real udata */
    case DW_FORM_loclistx:
    case DW_FORM_rnglistx:
    case DW_FORM_udata: {
        Dwarf_Unsigned leblen = 0;
        DECODE_LEB128_UWORD_LEN_CK(data, ret_value,leblen,
            dbg,error,section_end);
        *return_uval = ret_value;
        *bytes_read = leblen;
        return DW_DLV_OK;
    }
    /*  IRIX bug 583450. We do not allow reading
        sdata from a udata
        value. Caller can retry, calling sdata */
    default:
        break;
    }
    if (attr) {
        int res = 0;
        Dwarf_Signed s = 0;
        res = dwarf_formsdata(attr,&s,error);
        if (res != DW_DLV_OK) {
            return res;
        }
        if (s < 0 ) {
            _dwarf_error(dbg, error, DW_DLE_UDATA_VALUE_NEGATIVE);
            return DW_DLV_ERROR;
        }
        *return_uval = (Dwarf_Unsigned)s;
        *bytes_read = 0;
        return DW_DLV_OK;
    }
    generate_form_error(dbg,error,form,
        DW_DLE_ATTR_FORM_BAD,
        "DW_DLE_ATTR_FORM_BAD",
        "formudata (internal function)");
    return DW_DLV_ERROR;
}

int
dwarf_formudata(Dwarf_Attribute attr,
    Dwarf_Unsigned * return_uval, Dwarf_Error * error)
{
    Dwarf_Debug dbg = 0;
    Dwarf_CU_Context cu_context = 0;
    Dwarf_Byte_Ptr section_end = 0;
    Dwarf_Unsigned bytes_read = 0;
    Dwarf_Byte_Ptr data =  attr->ar_debug_ptr;
    unsigned form = 0;

    int res  = get_attr_dbg(&dbg,&cu_context,attr,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    section_end =
        _dwarf_calculate_info_section_end_ptr(cu_context);
    form = attr->ar_attribute_form;

    res = _dwarf_formudata_internal(dbg,
        attr,
        form, data, section_end, return_uval,
        &bytes_read, error);
    return res;
}

int
dwarf_formsdata(Dwarf_Attribute attr,
    Dwarf_Signed * return_sval, Dwarf_Error * error)
{
    Dwarf_Signed ret_value = 0;
    Dwarf_Debug dbg = 0;
    Dwarf_CU_Context cu_context = 0;
    Dwarf_Byte_Ptr section_end = 0;

    int res  = get_attr_dbg(&dbg,&cu_context,attr,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    section_end =
        _dwarf_calculate_info_section_end_ptr(cu_context);
    switch (attr->ar_attribute_form) {

    case DW_FORM_data1:
        if ( attr->ar_debug_ptr >= section_end) {
            _dwarf_error(dbg, error, DW_DLE_DIE_BAD);
            return DW_DLV_ERROR;
        }
        *return_sval = (*(Dwarf_Sbyte *) attr->ar_debug_ptr);
        return DW_DLV_OK;

    /*  READ_UNALIGNED does not sign extend.
        So we have to use a cast to get the
        value sign extended in the right way for each case. */
    case DW_FORM_data2:{
        READ_UNALIGNED_CK(dbg, ret_value, Dwarf_Signed,
            attr->ar_debug_ptr,
            DWARF_HALF_SIZE,
            error,section_end);
        *return_sval = (Dwarf_Shalf) ret_value;
        return DW_DLV_OK;

        }

    case DW_FORM_data4:{
        READ_UNALIGNED_CK(dbg, ret_value, Dwarf_Signed,
            attr->ar_debug_ptr,
            DWARF_32BIT_SIZE,
            error,section_end);
        SIGN_EXTEND(ret_value,DWARF_32BIT_SIZE);
        *return_sval = (Dwarf_Signed) ret_value;
        return DW_DLV_OK;
        }

    case DW_FORM_data8:{
        READ_UNALIGNED_CK(dbg, ret_value, Dwarf_Signed,
            attr->ar_debug_ptr,
            DWARF_64BIT_SIZE,
            error,section_end);
        /* No SIGN_EXTEND needed, we are filling all bytes already.*/
        *return_sval = (Dwarf_Signed) ret_value;
        return DW_DLV_OK;
        }

    /*  DW_FORM_implicit_const  is a value in the
        abbreviations, not in the DIEs. */
    case DW_FORM_implicit_const:
        *return_sval = attr->ar_implicit_const;
        return DW_DLV_OK;

    case DW_FORM_sdata: {
        Dwarf_Byte_Ptr tmp = attr->ar_debug_ptr;

        DECODE_LEB128_SWORD_CK(tmp, ret_value,
            dbg,error,section_end);
        *return_sval = ret_value;
        return DW_DLV_OK;

    }

        /* IRIX bug 583450. We do not allow reading sdata from a udata
            value. Caller can retry, calling udata */

    default:
        break;
    }
    generate_form_error(dbg,error,attr->ar_attribute_form,
        DW_DLE_ATTR_FORM_BAD,
        "DW_DLE_ATTR_FORM_BAD",
        "dwarf_formsdata");
    return DW_DLV_ERROR;
}

int
_dwarf_formblock_internal(Dwarf_Debug dbg,
    Dwarf_Attribute attr,
    Dwarf_CU_Context cu_context,
    Dwarf_Block * return_block,
    Dwarf_Error * error)
{
    Dwarf_Small *section_start = 0;
    Dwarf_Small *section_end = 0;
    Dwarf_Unsigned section_length = 0;
    Dwarf_Unsigned length = 0;
    Dwarf_Small *data = 0;

    section_end =
        _dwarf_calculate_info_section_end_ptr(cu_context);
    section_start =
        _dwarf_calculate_info_section_start_ptr(cu_context,
        &section_length);
    switch (attr->ar_attribute_form) {
    case DW_FORM_block1: {
        Dwarf_Small *start       = attr->ar_debug_ptr;
        Dwarf_Small *incremented = start + 1;
        if ( incremented < start ||
            incremented >= section_end) {
            /*  Error if +1 overflows or if points out of section. */
            generate_form_error(dbg,error,attr->ar_attribute_form,
                DW_DLE_ATTR_FORM_BAD,
                "DW_DLE_ATTR_FORM_BAD",
                " DW_FORM_block1 offset invalid");
            return DW_DLV_ERROR;
        }
        length = *start;
        data   = incremented;
        break;
    }
    case DW_FORM_block2:
        READ_UNALIGNED_CK(dbg, length, Dwarf_Unsigned,
            attr->ar_debug_ptr, DWARF_HALF_SIZE,
            error,section_end);
        data = attr->ar_debug_ptr + DWARF_HALF_SIZE;
        break;

    case DW_FORM_block4:
        READ_UNALIGNED_CK(dbg, length, Dwarf_Unsigned,
            attr->ar_debug_ptr, DWARF_32BIT_SIZE,
            error,section_end);
        data = attr->ar_debug_ptr + DWARF_32BIT_SIZE;
        break;

    case DW_FORM_exprloc:
    case DW_FORM_block: {
        Dwarf_Byte_Ptr tmp = attr->ar_debug_ptr;
        Dwarf_Unsigned leblen = 0;

        DECODE_LEB128_UWORD_LEN_CK(tmp, length, leblen,
            dbg,error,section_end);
        data = attr->ar_debug_ptr + leblen;
        break;
        }
    default:
        generate_form_error(dbg,error,attr->ar_attribute_form,
            DW_DLE_ATTR_FORM_BAD,
            "DW_DLE_ATTR_FORM_BAD",
            "dwarf_formblock() finds unknown form");
        return DW_DLV_ERROR;
    }
    /*  We have the data. Check for errors. */
    if (length >= section_length) {
        /*  Sanity test looking for wraparound:
            when length actually added in
            it would not be caught.
            Test could be just >, but >= ok here too.*/
        _dwarf_error_string(dbg, error,
            DW_DLE_FORM_BLOCK_LENGTH_ERROR,
            "DW_DLE_FORM_BLOCK_LENGTH_ERROR: "
            "The length of the block is greater "
            "than the section length! Corrupt Dwarf.");
        return DW_DLV_ERROR;
    }
    if ((attr->ar_debug_ptr + length) > section_end) {
        _dwarf_error_string(dbg, error,
            DW_DLE_FORM_BLOCK_LENGTH_ERROR,
            "DW_DLE_FORM_BLOCK_LENGTH_ERROR: "
            "The block length means the block "
            "runs off the end of the section length!"
            " Corrupt Dwarf.");
        return DW_DLV_ERROR;
    }
    if (data > section_end) {
        _dwarf_error_string(dbg, error,
            DW_DLE_FORM_BLOCK_LENGTH_ERROR,
            "DW_DLE_FORM_BLOCK_LENGTH_ERROR: "
            "The block content is "
            "past the end of the section!"
            " Corrupt Dwarf.");
        _dwarf_error(dbg, error, DW_DLE_FORM_BLOCK_LENGTH_ERROR);
        return DW_DLV_ERROR;
    }
    if ((data + length) > section_end) {
        _dwarf_error_string(dbg, error,
            DW_DLE_FORM_BLOCK_LENGTH_ERROR,
            "DW_DLE_FORM_BLOCK_LENGTH_ERROR: "
            "The end of the block content is "
            "past the end of the section!"
            " Corrupt Dwarf.");
        return DW_DLV_ERROR;
    }
    return_block->bl_len = length;
    return_block->bl_data = data;
    /*  This struct is public so use the old name instead
        of what we now would call it:  bl_kind  */
    return_block->bl_from_loclist =  DW_LKIND_expression;
    return_block->bl_section_offset =  data - section_start;
    return DW_DLV_OK;
}

int
dwarf_formblock(Dwarf_Attribute attr,
    Dwarf_Block ** return_block, Dwarf_Error * error)
{
    Dwarf_CU_Context cu_context = 0;
    Dwarf_Debug dbg = 0;
    Dwarf_Block local_block;
    Dwarf_Block *out_block = 0;
    int res = 0;

    memset(&local_block,0,sizeof(local_block));
    res  = get_attr_dbg(&dbg,&cu_context,attr,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    res = _dwarf_formblock_internal(dbg,attr,
        cu_context, &local_block, error);
    if (res != DW_DLV_OK) {
        return res;
    }
    out_block = (Dwarf_Block *)
        _dwarf_get_alloc(dbg, DW_DLA_BLOCK, 1);
    if (!out_block) {
        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
        return DW_DLV_ERROR;
    }
    *out_block = local_block;
    *return_block = out_block;
    return DW_DLV_OK;
}

/*  This is called for attribute with strx form
    or macro5 with strx form.
    No relation to the Name Table or
    to  FIXME */
int
_dwarf_extract_string_offset_via_str_offsets(Dwarf_Debug dbg,
    Dwarf_Small *data_ptr,
    Dwarf_Small *end_data_ptr,
    Dwarf_Half   attrform,
    Dwarf_CU_Context cu_context,
    Dwarf_Unsigned *str_sect_offset_out,
    Dwarf_Error *error)
{
    Dwarf_Unsigned index_to_offset_entry = 0;
    Dwarf_Unsigned offsettotable = 0;
    Dwarf_Unsigned end_offsettotable = 0;
    Dwarf_Unsigned indexoffset = 0;
    Dwarf_Unsigned baseoffset = 0;
    Dwarf_Unsigned table_offset_to_array = 0;
    int res = 0;
    int idxres = 0;
    Dwarf_Small *sectionptr = 0;
    Dwarf_Unsigned sectionlen = 0;
    Dwarf_Small   *sof_start = 0;
    Dwarf_Small   *sof_end = 0;
    Dwarf_Unsigned str_sect_offset = 0;
    Dwarf_Unsigned length_size  = 0;
    Dwarf_Bool have_array_offset = FALSE;

    res = _dwarf_load_section(dbg, &dbg->de_debug_str_offsets,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    sectionptr = dbg->de_debug_str_offsets.dss_data;
    sectionlen = dbg->de_debug_str_offsets.dss_size;
    length_size = cu_context->cc_length_size;
    /*  If this is a dwp we look there, but I suppose
        we could also look for the section in the tied
        executable object file it is not here. FIXME */
    idxres = dw_read_str_index_val_itself(dbg,
        attrform,data_ptr,end_data_ptr,&index_to_offset_entry,error);
    if ( idxres != DW_DLV_OK) {
        return idxres;
    }
    if (cu_context->cc_str_offsets_array_offset_present) {
        baseoffset = cu_context->cc_str_offsets_array_offset;
        have_array_offset = TRUE;
    } else if (cu_context->cc_str_offsets_tab_present) {
        baseoffset = cu_context->cc_str_offsets_header_offset;
        baseoffset += cu_context->cc_str_offsets_tab_to_array;
        have_array_offset = TRUE;
    } else { /* do nothing */}
    if (baseoffset > sectionlen ||
        (baseoffset+length_size) > sectionlen ||
        (baseoffset+(index_to_offset_entry *length_size)) >
            sectionlen) {
        _dwarf_error_string(dbg, error,
            DW_DLE_ATTR_FORM_SIZE_BAD,
            "DW_DLE_ATTR_FORM_SIZE_BAD: "
            "An Attribute value (offset  into "
            ".debug_str_offsets) is impossibly "
            "large. Corrupt Dwarf.");
        return DW_DLV_ERROR;
    }
    indexoffset = index_to_offset_entry* length_size;
    if (!have_array_offset) {
        /*  missing any connection to a specific
            str_offsets table this guesses at table zero.
            When the compiler/linker have
            combined str offsets into a
            single table this works. */
        Dwarf_Unsigned headeroffset = 0;
        if (cu_context->cc_version_stamp ==  DW_CU_VERSION5 ) {
            /*  A base offset of 0 is ok for either
                DWARF5. but some early GNU compilers emitted
                DWARF4 .debug_str_offsets, so lets check
                the first table.  */
            Dwarf_Unsigned stsize =
                dbg->de_debug_str_offsets.dss_size;
            Dwarf_Unsigned length           = 0;
            Dwarf_Unsigned table_length     = 0;
            Dwarf_Half local_offset_size    = 0;
            Dwarf_Half local_extension_size = 0;
            Dwarf_Half version              = 0;
            Dwarf_Half padding              = 0;

            res = _dwarf_trial_read_dwarf_five_hdr(dbg,
                headeroffset,stsize,
                &table_offset_to_array,
                &table_length,
                &length, &local_offset_size,
                &local_extension_size,
                &version,
                &padding,
                error);
            if (res == DW_DLV_OK) {
                baseoffset = table_offset_to_array+
                    headeroffset;
            } else {
                if (res == DW_DLV_ERROR && error) {
                    dwarf_dealloc_error(dbg,*error);
                    *error = 0;
                } else {}
            }
        }
    }
    offsettotable = indexoffset+ baseoffset;
    end_offsettotable = offsettotable + length_size;
    /*  The offsets table is a series of offset-size entries.
        The == case in the test applies when we are at the last table
        entry, so == is not an error, hence only test >
    */
    if (offsettotable  > sectionlen ||
        end_offsettotable  > sectionlen) {
        dwarfstring m;

        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            "DW_DLE_ATTR_FORM_SIZE_BAD: The end offset of "
            "a .debug_str_offsets table is 0x%x ",
            end_offsettotable);
        dwarfstring_append_printf_u(&m,
            "but the object section is just 0x%x "
            "bytes long",
            sectionlen);
        _dwarf_error_string(dbg, error,
            DW_DLE_ATTR_FORM_SIZE_BAD,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    sof_start = sectionptr+ offsettotable;
    sof_end = sectionptr + end_offsettotable;
    /* Now read the string offset from the offset table. */
    READ_UNALIGNED_CK(dbg,str_sect_offset,Dwarf_Unsigned,
        sof_start,
        length_size,error,sof_end);
    *str_sect_offset_out = str_sect_offset;
    return DW_DLV_OK;
}

int
_dwarf_extract_local_debug_str_string_given_offset(Dwarf_Debug dbg,
    unsigned attrform,
    Dwarf_Unsigned offset,
    char ** return_str,
    Dwarf_Error * error)
{
    if (attrform == DW_FORM_strp ||
        attrform == DW_FORM_line_strp ||
        attrform == DW_FORM_GNU_str_index ||
        attrform == DW_FORM_strx1 ||
        attrform == DW_FORM_strx2 ||
        attrform == DW_FORM_strx3 ||
        attrform == DW_FORM_strx4 ||
        attrform == DW_FORM_strx) {
        /*  The 'offset' into .debug_str or .debug_line_str is given,
            here we turn that into a pointer. */
        Dwarf_Small   *secend = 0;
        Dwarf_Small   *secbegin = 0;
        Dwarf_Small   *strbegin = 0;
        Dwarf_Unsigned secsize = 0;
        int errcode = 0;
        const char *errname = 0;
        int res = 0;

        if (attrform == DW_FORM_line_strp) {
            res = _dwarf_load_section(dbg,
                &dbg->de_debug_line_str,error);
            if (res != DW_DLV_OK) {
                return res;
            }
            errcode = DW_DLE_STRP_OFFSET_BAD;
            errname = "DW_DLE_STRP_OFFSET_BAD";
            secsize = dbg->de_debug_line_str.dss_size;
            secbegin = dbg->de_debug_line_str.dss_data;
            strbegin= dbg->de_debug_line_str.dss_data + offset;
            secend = dbg->de_debug_line_str.dss_data + secsize;
        } else {
            /* DW_FORM_strp  etc */
            res = _dwarf_load_section(dbg, &dbg->de_debug_str,error);
            if (res != DW_DLV_OK) {
                return res;
            }
            errcode = DW_DLE_STRING_OFFSET_BAD;
            errname = "DW_DLE_STRING_OFFSET_BAD";
            secsize = dbg->de_debug_str.dss_size;
            secbegin = dbg->de_debug_str.dss_data;
            strbegin= dbg->de_debug_str.dss_data + offset;
            secend = dbg->de_debug_str.dss_data + secsize;
        }
        if (offset >= secsize) {
            dwarfstring m;
            const char *name = "<unknownform>";

            dwarf_get_FORM_name(attrform,&name);

            dwarfstring_constructor(&m);
            dwarfstring_append(&m,(char *)errname);
            dwarfstring_append_printf_s(&m,
                " Form %s ",(char *)name);
            dwarfstring_append_printf_u(&m,
                "string offset of 0x%" DW_PR_DUx " ",
                offset);
            dwarfstring_append_printf_u(&m,
                "is larger than the string section "
                "size of  0x%" DW_PR_DUx,
                secsize);
            _dwarf_error_string(dbg, error, errcode,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            /*  Badly damaged DWARF here. */
            return DW_DLV_ERROR;
        }
        res= _dwarf_check_string_valid(dbg,secbegin,strbegin, secend,
            errcode,error);
        if (res != DW_DLV_OK) {
            return res;
        }

        *return_str = (char *)strbegin;
        return DW_DLV_OK;
    }
    generate_form_error(dbg,error,attrform,
        DW_DLE_ATTR_FORM_BAD,
        "DW_DLE_ATTR_FORM_BAD",
        "extract debug_str string");
    return DW_DLV_ERROR;
}

/* Contrary to pre-2005 documentation,
   The string pointer returned thru return_str must
   never have dwarf_dealloc() applied to it.
   Documentation fixed July 2005.
*/
int
dwarf_formstring(Dwarf_Attribute attr,
    char **return_str, Dwarf_Error * error)
{
    Dwarf_CU_Context cu_context = 0;
    Dwarf_Debug dbg = 0;
    Dwarf_Unsigned offset = 0;
    int res = DW_DLV_ERROR;
    Dwarf_Small *secdataptr = 0;
    Dwarf_Small *secend = 0;
    Dwarf_Unsigned secdatalen = 0;
    Dwarf_Small *infoptr = attr->ar_debug_ptr;
    Dwarf_Small *contextend = 0;

    res  = get_attr_dbg(&dbg,&cu_context,attr,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    if (cu_context->cc_is_info) {
        secdataptr = (Dwarf_Small *)dbg->de_debug_info.dss_data;
        secdatalen = dbg->de_debug_info.dss_size;
    } else {
        secdataptr = (Dwarf_Small *)dbg->de_debug_types.dss_data;
        secdatalen = dbg->de_debug_types.dss_size;
    }
    contextend = secdataptr +
        cu_context->cc_debug_offset +
        cu_context->cc_length +
        cu_context->cc_length_size +
        cu_context->cc_extension_size;
    secend = secdataptr + secdatalen;
    if (contextend < secend) {
        secend = contextend;
    }
    switch(attr->ar_attribute_form) {
    case DW_FORM_string: {
        Dwarf_Small *begin = attr->ar_debug_ptr;

        res= _dwarf_check_string_valid(dbg,secdataptr,begin, secend,
            DW_DLE_FORM_STRING_BAD_STRING,error);
        if (res != DW_DLV_OK) {
            return res;
        }
        *return_str = (char *) (begin);
        return DW_DLV_OK;
    }
    case DW_FORM_GNU_strp_alt:
    case DW_FORM_strp_sup:  {
        Dwarf_Error alterr = 0;
        Dwarf_Bool is_info = TRUE;
        /*  See dwarfstd.org issue 120604.1
            This is the offset in the .debug_str section
            of another object file.
            The 'tied' file notion should apply.
            It is not clear whether both a supplementary
            and a split object might be needed at the same time
            (hence two 'tied' files simultaneously). */
        Dwarf_Off soffset = 0;

        res = dwarf_global_formref_b(attr, &soffset,
            &is_info,error);
        if (res != DW_DLV_OK) {
            return res;
        }
        res = _dwarf_get_string_from_tied(dbg, soffset,
            return_str, &alterr);
        if (res == DW_DLV_ERROR) {
            if (dwarf_errno(alterr) ==
                DW_DLE_NO_TIED_FILE_AVAILABLE) {

                dwarf_dealloc_error(dbg,alterr);
                if ( attr->ar_attribute_form ==
                    DW_FORM_GNU_strp_alt) {
                    *return_str =
                        (char *)"<DW_FORM_GNU_strp_alt-no-tied-file>";
                } else {
                    *return_str =
                        (char *)"<DW_FORM_strp_sup-no-tied-file>";
                }
                return DW_DLV_OK;
            }
            if (error) {
                *error = alterr;
            } else {
                dwarf_dealloc_error(dbg,alterr);
                alterr = 0;
            }
            return res;
        }
        if (res == DW_DLV_NO_ENTRY) {
            if ( attr->ar_attribute_form == DW_FORM_GNU_strp_alt) {
                *return_str =
                    (char *)"<DW_FORM_GNU_strp_alt-no-tied-file>";
            }else {
                *return_str =
                    (char *)"<DW_FORM_strp_sup-no-tied-file>";
            }
        }
        return res;
    }
    case DW_FORM_GNU_str_index:
    case DW_FORM_strx:
    case DW_FORM_strx1:
    case DW_FORM_strx2:
    case DW_FORM_strx3:
    case DW_FORM_strx4: {
        Dwarf_Unsigned offsettostr= 0;

        res = _dwarf_extract_string_offset_via_str_offsets(dbg,
            infoptr,
            secend,
            attr->ar_attribute_form,
            cu_context,
            &offsettostr,
            error);
        if (res != DW_DLV_OK) {
            return res;
        }
        offset = offsettostr;
        break;
    }
    case DW_FORM_strp:
    case DW_FORM_line_strp:{
        READ_UNALIGNED_CK(dbg, offset, Dwarf_Unsigned,
            infoptr,
            cu_context->cc_length_size,error,secend);
        break;
    }
    default:
        _dwarf_error(dbg, error, DW_DLE_STRING_FORM_IMPROPER);
        return DW_DLV_ERROR;
    }
    /*  Now we have offset so read the string from
        debug_str or debug_line_str. */
    res = _dwarf_extract_local_debug_str_string_given_offset(dbg,
        attr->ar_attribute_form,
        offset,
        return_str,
        error);
    return res;
}

int
_dwarf_get_string_from_tied(Dwarf_Debug dbg,
    Dwarf_Unsigned offset,
    char **return_str,
    Dwarf_Error*error)
{
    Dwarf_Debug tieddbg = 0;
    Dwarf_Small *secend = 0;
    Dwarf_Small *secbegin = 0;
    Dwarf_Small *strbegin = 0;
    int res = DW_DLV_ERROR;
    Dwarf_Error localerror = 0;

    /* Attach errors to dbg, not tieddbg. */
    if (!DBG_HAS_SECONDARY(dbg)) {
        _dwarf_error(dbg, error, DW_DLE_NO_TIED_FILE_AVAILABLE);
        return  DW_DLV_ERROR;
    }
    tieddbg = dbg->de_secondary_dbg;
    /* The 'offset' into .debug_str is set. */
    res = _dwarf_load_section(tieddbg, &tieddbg->de_debug_str,
        &localerror);
    if (res == DW_DLV_ERROR) {
        Dwarf_Unsigned lerrno = dwarf_errno(localerror);
        dwarf_dealloc_error(tieddbg,localerror);
        _dwarf_error(dbg,error,lerrno);
        return res;
    }
    if (res == DW_DLV_NO_ENTRY) {
        return res;
    }
    if (offset >= tieddbg->de_debug_str.dss_size) {
        /*  Badly damaged DWARF here. */
        _dwarf_error(dbg, error,  DW_DLE_NO_TIED_STRING_AVAILABLE);
        return DW_DLV_ERROR;
    }
    secbegin = tieddbg->de_debug_str.dss_data;
    strbegin= tieddbg->de_debug_str.dss_data + offset;
    secend = tieddbg->de_debug_str.dss_data +
        tieddbg->de_debug_str.dss_size;

    /*  Ensure the offset lies within the .debug_str */
    if (offset >= tieddbg->de_debug_str.dss_size) {
        _dwarf_error(dbg, error,  DW_DLE_NO_TIED_STRING_AVAILABLE);
        return DW_DLV_ERROR;
    }
    res= _dwarf_check_string_valid(tieddbg,secbegin,strbegin, secend,
        DW_DLE_NO_TIED_STRING_AVAILABLE,
        &localerror);
    if (res == DW_DLV_ERROR) {
        Dwarf_Unsigned lerrno = dwarf_errno(localerror);
        dwarf_dealloc_error(tieddbg,localerror);
        _dwarf_error(dbg,error,lerrno);
        return res;
    }
    if (res == DW_DLV_NO_ENTRY) {
        return res;
    }
    *return_str = (char *) (tieddbg->de_debug_str.dss_data + offset);
    return DW_DLV_OK;
}

int
dwarf_formexprloc(Dwarf_Attribute attr,
    Dwarf_Unsigned * return_exprlen,
    Dwarf_Ptr  * block_ptr,
    Dwarf_Error * error)
{
    Dwarf_Debug dbg = 0;
    Dwarf_CU_Context cu_context = 0;

    int res  = get_attr_dbg(&dbg,&cu_context,attr,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    if (attr->ar_attribute_form == DW_FORM_exprloc ) {
        Dwarf_Die die = 0;
        Dwarf_Unsigned leb_len = 0;
        Dwarf_Byte_Ptr section_start = 0;
        Dwarf_Unsigned section_len = 0;
        Dwarf_Byte_Ptr section_end = 0;
        Dwarf_Byte_Ptr info_ptr = 0;
        Dwarf_Unsigned exprlen = 0;
        Dwarf_Small * addr = attr->ar_debug_ptr;

        info_ptr = addr;
        section_start =
            _dwarf_calculate_info_section_start_ptr(cu_context,
            &section_len);
        section_end = section_start + section_len;

        DECODE_LEB128_UWORD_LEN_CK(info_ptr, exprlen, leb_len,
            dbg,error,section_end);
        if (exprlen > section_len) {
            /* Corrupted dwarf!  */
            dwarfstring m;

            dwarfstring_constructor(&m);
            dwarfstring_append_printf_u(&m,
                "DW_DLE_ATTR_OUTSIDE_SECTION: "
                "The expression length is %u,",exprlen);
            dwarfstring_append_printf_u(&m,
                " but the section length is just %u. "
                "Corrupt Dwarf.",section_len);
            _dwarf_error_string(dbg, error,
                DW_DLE_ATTR_OUTSIDE_SECTION,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
        die = attr->ar_die;
        /*  Is the block entirely in the section, or is
            there bug somewhere?
            Here the final addr may be 1 past end of section. */
        if (_dwarf_reference_outside_section(die,
            (Dwarf_Small *)addr,
            ((Dwarf_Small *)addr)+exprlen +leb_len)) {
            dwarfstring m;

            dwarfstring_constructor(&m);
            dwarfstring_append_printf_u(&m,
                "DW_DLE_ATTR_OUTSIDE_SECTION: "
                "The expression length %u,",exprlen);
            dwarfstring_append_printf_u(&m,
                " plus the leb value length of "
                "%u ",leb_len);
            dwarfstring_append(&m,
                " runs past the end of the section. "
                "Corrupt Dwarf.");
            _dwarf_error_string(dbg, error,
                DW_DLE_ATTR_OUTSIDE_SECTION,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
        *return_exprlen = exprlen;
        *block_ptr = addr + leb_len;
        return DW_DLV_OK;

    }
    {
        dwarfstring m;
        const char *name = "<name not known>";
        unsigned  mform = attr->ar_attribute_form;

        dwarfstring_constructor(&m);

        dwarf_get_FORM_name (mform,&name);
        dwarfstring_append_printf_u(&m,
            "DW_DLE_ATTR_EXPRLOC_FORM_BAD: "
            "The form is 0x%x ", mform);
        dwarfstring_append_printf_s(&m,
            "(%s) but should be DW_FORM_exprloc. "
            "Corrupt Dwarf.",(char *)name);
        _dwarf_error_string(dbg, error, DW_DLE_ATTR_EXPRLOC_FORM_BAD,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
    }
    return DW_DLV_ERROR;
}
