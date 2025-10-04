/*
  Cropyright (C) 2000,2002,2004 Silicon Graphics, Inc.  All Rights Reserved.
  Portions Copyright (C) 2007-2022 David Anderson. All Rights Reserved.
  Portions Copyright 2012 SN Systems Ltd. All rights reserved.
  Portions Copyright 2020 Google All rights reserved.

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

#include <stddef.h> /* NULL size_t */
#include <stdio.h> /* debugging printf */

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
#include "dwarf_die_deliv.h"
#include "dwarf_string.h"

static int _dwarf_die_attr_unsigned_constant(Dwarf_Die die,
    Dwarf_Half      attr,
    Dwarf_Unsigned *return_val,
    Dwarf_Error    *error);

int dwarf_get_offset_size(Dwarf_Debug dbg,
    Dwarf_Half  *    offset_size,
    Dwarf_Error *    error)
{
    CHECK_DBG(dbg,error,"dwarf_get_offset_size()");
    *offset_size = dbg->de_length_size;
    return DW_DLV_OK;
}

#if 0 /* dump_bytes */
static void
dump_bytes(char * msg,Dwarf_Small * start, long len)
{
    Dwarf_Small *end = start + len;
    Dwarf_Small *cur = start;

    printf("%s ",msg);
    for (; cur < end; cur++) {
        printf("%02x ", *cur);
    }
    printf("\n");
}
#endif /*0*/

/* This is normally reliable.
But not always.
If different compilation
units have different address sizes
this may not give the correct value in all contexts.
If the Elf offset size != address_size
(for example if address_size = 4 but recorded in elf64 object)
this may not give the correct value in all contexts.
*/
int
dwarf_get_address_size(Dwarf_Debug dbg,
    Dwarf_Half *ret_addr_size, Dwarf_Error *error)
{
    Dwarf_Half address_size = 0;

    CHECK_DBG(dbg,error,"dwarf_get_address_size()");
    address_size = dbg->de_pointer_size;
    *ret_addr_size = address_size;
    return DW_DLV_OK;
}

/* This will be correct in all contexts where the
   CU context of a DIE is known.
*/
int
dwarf_get_die_address_size(Dwarf_Die die,
    Dwarf_Half * ret_addr_size, Dwarf_Error *error)
{
    Dwarf_Half address_size = 0;
    CHECK_DIE(die, DW_DLV_ERROR);
    address_size = die->di_cu_context->cc_address_size;
    *ret_addr_size = address_size;
    return DW_DLV_OK;
}

int
dwarf_dieoffset(Dwarf_Die die,
    Dwarf_Off *ret_offset, Dwarf_Error *error)
{
    Dwarf_Small *dataptr = 0;
    Dwarf_Debug dbg = 0;

    CHECK_DIE(die, DW_DLV_ERROR);
    dbg = die->di_cu_context->cc_dbg;
    dataptr = die->di_is_info? dbg->de_debug_info.dss_data:
        dbg->de_debug_types.dss_data;

    *ret_offset = (die->di_debug_ptr - dataptr);
    return DW_DLV_OK;
}

/*  This function returns the offset of
    the die relative to the start of its
    compilation-unit rather than .debug_info.
    Returns DW_DLV_ERROR on error.  */
int
dwarf_die_CU_offset(Dwarf_Die die,
    Dwarf_Off *cu_off, Dwarf_Error *error)
{
    Dwarf_CU_Context cu_context = 0;
    Dwarf_Small *dataptr = 0;
    Dwarf_Debug dbg = 0;

    CHECK_DIE(die, DW_DLV_ERROR);
    cu_context = die->di_cu_context;
    dbg = die->di_cu_context->cc_dbg;
    dataptr = die->di_is_info? dbg->de_debug_info.dss_data:
        dbg->de_debug_types.dss_data;

    *cu_off = (die->di_debug_ptr - dataptr -
        cu_context->cc_debug_offset);
    return DW_DLV_OK;
}

/*  A common function to get both offsets (local and global)
    It's unusual in that it sets both return offsets
    to zero on entry.  Normally we only set any
    output-args (through their pointers) in case
    of success.  */
int
dwarf_die_offsets(Dwarf_Die die,
    Dwarf_Off *off,
    Dwarf_Off *cu_off,
    Dwarf_Error *error)
{
    int res = 0;
    Dwarf_Off lcuoff = 0;
    Dwarf_Off loff = 0;

    res = dwarf_dieoffset(die,&loff,error);
    if (res == DW_DLV_OK) {
        res = dwarf_die_CU_offset(die,&lcuoff,error);
    }
    if (res == DW_DLV_OK) {
        /*  Waiting till both succeed before
            returning any value at all to retain
            normal libdwarf call semantics. */
        *off = loff;
        *cu_off = lcuoff;
    } else {
        *off = 0;
        *cu_off = 0;
    }
    return res;
}

/*  This function returns the global offset
    (meaning the section offset) and length of
    the CU that this die is a part of.
    Used for correctness checking by dwarfdump.  */
int
dwarf_die_CU_offset_range(Dwarf_Die die,
    Dwarf_Off   *cu_off,
    Dwarf_Off   *cu_length,
    Dwarf_Error *error)
{
    Dwarf_CU_Context cu_context = 0;

    CHECK_DIE(die, DW_DLV_ERROR);
    cu_context = die->di_cu_context;

    *cu_off = cu_context->cc_debug_offset;
    *cu_length = cu_context->cc_length + cu_context->cc_length_size
        + cu_context->cc_extension_size;
    return DW_DLV_OK;
}

int
dwarf_tag(Dwarf_Die die, Dwarf_Half *tag, Dwarf_Error *error)
{
    CHECK_DIE(die, DW_DLV_ERROR);
    *tag = die->di_abbrev_list->abl_tag;
    return DW_DLV_OK;
}

static void
free_dwarf_offsets_chain(Dwarf_Debug dbg, Dwarf_Chain_2 head_chain)
{
    Dwarf_Chain_2 cur = head_chain;
    Dwarf_Chain_2 next = 0;

    for ( ; cur ; cur = next ) {
        next = cur->ch_next;
        dwarf_dealloc(dbg, cur, DW_DLA_CHAIN_2);
    }
}

/* Returns the children offsets for the given offset */
int
dwarf_offset_list(Dwarf_Debug dbg,
    Dwarf_Off    offset, Dwarf_Bool      is_info,
    Dwarf_Off  **offbuf, Dwarf_Unsigned *offcnt,
    Dwarf_Error *error)
{
    Dwarf_Die die = 0;
    Dwarf_Die child = 0;
    Dwarf_Die sib_die = 0;
    Dwarf_Die cur_die = 0;
    int       res = 0;
    Dwarf_Unsigned off_count = 0;
    Dwarf_Unsigned i = 0;
    Dwarf_Off    *ret_offsets = 0;
    Dwarf_Chain_2 curr_chain = 0;
    Dwarf_Chain_2 head_chain = 0;
    Dwarf_Chain_2 *plast = &head_chain;

    CHECK_DBG(dbg,error,"dwarf_offset_list()");
    *offbuf = NULL;
    *offcnt = 0;
    res = dwarf_offdie_b(dbg,offset,is_info,&die,error);
    if (DW_DLV_OK != res) {
        return res;
    }

    res = dwarf_child(die,&child,error);
    if (DW_DLV_ERROR == res || DW_DLV_NO_ENTRY == res) {
        return res;
    }
    dwarf_dealloc_die(die);
    cur_die = child;
    child = 0;
    for (;;) {
        if (DW_DLV_OK == res) {
            int dres = 0;
            Dwarf_Off cur_off = 0;

            dres = dwarf_dieoffset(cur_die,&cur_off,error);
            if (dres == DW_DLV_OK) {
                /* Normal. use cur_off. */
            } else if (dres == DW_DLV_ERROR) {
                free_dwarf_offsets_chain(dbg,head_chain);
                dwarf_dealloc_die(cur_die);
                return DW_DLV_ERROR;
            } else { /* DW_DLV_NO_ENTRY */
                /* Impossible, dwarf_dieoffset never returns this */
            }
            /* Record offset in current entry chain */
            curr_chain = (Dwarf_Chain_2)_dwarf_get_alloc(
                dbg,DW_DLA_CHAIN_2,1);
            if (curr_chain == NULL) {
                free_dwarf_offsets_chain(dbg,head_chain);
                dwarf_dealloc_die(cur_die);
                _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
                return DW_DLV_ERROR;
            }

            /* Put current offset on singly_linked list. */
            curr_chain->ch_item = cur_off;
            ++off_count;
            (*plast) = curr_chain;
            plast = &(curr_chain->ch_next);
        }
        /* Move to next sibling next sibling */
        sib_die = 0;
        res = dwarf_siblingof_c(cur_die,&sib_die,error);
        if (cur_die != die) {
            dwarf_dealloc(dbg,cur_die,DW_DLA_DIE);
        }
        if (DW_DLV_ERROR == res) {
            free_dwarf_offsets_chain(dbg,head_chain);
            return res;
        }
        if (DW_DLV_NO_ENTRY == res) {
            /* Done at this level. */
            break;
        }
        /* res == DW_DLV_OK */
        cur_die = sib_die;
    }

    /* Points to contiguous block of Dwarf_Off's. */
    ret_offsets = (Dwarf_Off *) _dwarf_get_alloc(dbg,
        DW_DLA_UARRAY, off_count);
    if (ret_offsets == NULL) {
        free_dwarf_offsets_chain(dbg,head_chain);
        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
        return DW_DLV_ERROR;
    }

    /*  Store offsets in contiguous block,
        and deallocate the chain. */
    curr_chain = head_chain;
    for (i = 0; i < off_count; i++) {
        Dwarf_Chain_2 prev =0;

        ret_offsets[i] = curr_chain->ch_item;
        prev = curr_chain;
        curr_chain = curr_chain->ch_next;
        dwarf_dealloc(dbg, prev, DW_DLA_CHAIN_2);
    }
    *offbuf = ret_offsets;
    *offcnt = off_count;
    return DW_DLV_OK;
}

static void
empty_local_attrlist(Dwarf_Debug dbg,
    Dwarf_Attribute attr)
{
    Dwarf_Attribute cur = 0;
    Dwarf_Attribute next = 0;

    for (cur = attr; cur ; cur = next) {
        next = cur->ar_next;
        dwarf_dealloc(dbg,cur,DW_DLA_ATTR);
    }
}

int
dwarf_attrlist(Dwarf_Die die,
    Dwarf_Attribute **attrbuf,
    Dwarf_Signed     *attrcnt, Dwarf_Error *error)
{
    Dwarf_Unsigned    attr_count = 0;
    Dwarf_Unsigned    attr = 0;
    Dwarf_Unsigned    attr_form = 0;
    Dwarf_Unsigned    i = 0;
    Dwarf_Abbrev_List abbrev_list = 0;
    Dwarf_Attribute   head_attr = NULL;
    Dwarf_Attribute   curr_attr = NULL;
    Dwarf_Attribute  *last_attr = &head_attr;
    Dwarf_Debug       dbg = 0;
    Dwarf_Byte_Ptr    info_ptr = 0;
    Dwarf_Byte_Ptr    die_info_end = 0;
    int               lres = 0;
    int               bres = 0;
    Dwarf_CU_Context  context = 0;
    Dwarf_Unsigned    highest_code = 0;

    CHECK_DIE(die, DW_DLV_ERROR);
    context = die->di_cu_context;
    dbg = context->cc_dbg;
    die_info_end =
        _dwarf_calculate_info_section_end_ptr(context);
    lres = _dwarf_get_abbrev_for_code(context,
        die->di_abbrev_list->abl_code,
        &abbrev_list,
        &highest_code,error);
    if (lres == DW_DLV_ERROR) {
        return lres;
    }
    if (lres == DW_DLV_NO_ENTRY) {
        dwarfstring m;

        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            "DW_DLE_ABBREV_MISSING "
            "There is no abbrev present for code %u "
            "in this compilation unit. ",
            die->di_abbrev_list->abl_code);
        dwarfstring_append_printf_u(&m,
            "The highest known code "
            "in any compilation unit is %u .",
            highest_code);
        _dwarf_error_string(dbg, error,
            DW_DLE_ABBREV_MISSING,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }

    info_ptr = die->di_debug_ptr;
    {
        /* SKIP_LEB128 */
        Dwarf_Unsigned ignore_this = 0;
        Dwarf_Unsigned len = 0;

        lres = dwarf_decode_leb128((char *)info_ptr,
            &len,&ignore_this,(char *)die_info_end);
        if (lres == DW_DLV_ERROR) {
            /* Stepped off the end SKIPping the leb  */
            dwarfstring m;

            dwarfstring_constructor(&m);
            dwarfstring_append_printf_u(&m,
                "DW_DLE_DIE_BAD: In building an attrlist "
                "we run off the end of the DIE while skipping "
                " the DIE tag, seeing the leb length as 0x%u ",
                len);
            _dwarf_error_string(dbg, error, DW_DLE_DIE_BAD,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
        info_ptr += len;
    }

    if (!abbrev_list->abl_attr) {
        Dwarf_Byte_Ptr abbrev_ptr = abbrev_list->abl_abbrev_ptr;
        Dwarf_Byte_Ptr abbrev_end =
            _dwarf_calculate_abbrev_section_end_ptr(context);
        /* FIXME */
        bres = _dwarf_fill_in_attr_form_abtable(context,
            abbrev_ptr, abbrev_end, abbrev_list,
            error);
        if (bres != DW_DLV_OK) {
            return bres;
        }
        /*  Here we are guaranteed abbrev_list->abl_attr
            is non-null */
    }
    /*  ASSERT  list->abl_addr and list->abl_form
        are non-null and if  list->abl_implicit_const_count > 0
        list->abl_implicit_const is non-null. */

    for ( i = 0; i <abbrev_list->abl_abbrev_count; ++i) {
        Dwarf_Signed implicit_const = 0;
        Dwarf_Half newattr_form = 0;
        int ires = 0;

        attr =  abbrev_list->abl_attr[i];
        attr_form =  abbrev_list->abl_form[i];
        if (attr > DW_AT_hi_user) {
            empty_local_attrlist(dbg,head_attr);
            _dwarf_error(dbg, error,DW_DLE_ATTR_CORRUPT);
            return DW_DLV_ERROR;
        }
        if (attr_form == DW_FORM_implicit_const) {
            implicit_const = abbrev_list->abl_implicit_const[i];
        }
        if (!_dwarf_valid_form_we_know(attr_form,attr)) {
            empty_local_attrlist(dbg,head_attr);
            _dwarf_error(dbg, error, DW_DLE_UNKNOWN_FORM);
            return DW_DLV_ERROR;
        }
        newattr_form = (Dwarf_Half)attr_form;
        if (attr_form == DW_FORM_indirect) {
            Dwarf_Unsigned utmp6 = 0;

            if (_dwarf_reference_outside_section(die,
                (Dwarf_Small*) info_ptr,
                ((Dwarf_Small*) info_ptr )+1)) {
                empty_local_attrlist(dbg,head_attr);
                _dwarf_error_string(dbg, error,
                    DW_DLE_ATTR_OUTSIDE_SECTION,
                    "DW_DLE_ATTR_OUTSIDE_SECTION: "
                    " Reading Attriutes: "
                    "For DW_FORM_indirect there is"
                    " no room for the form. Corrupt Dwarf");
                return DW_DLV_ERROR;
            }
            ires = _dwarf_leb128_uword_wrapper(dbg,
                &info_ptr,die_info_end,&utmp6,error);
            if (ires != DW_DLV_OK) {
                empty_local_attrlist(dbg,head_attr);
                _dwarf_error_string(dbg, error,
                    DW_DLE_ATTR_OUTSIDE_SECTION,
                    "DW_DLE_ATTR_OUTSIDE_SECTION: "
                    "Reading target of a DW_FORM_indirect "
                    "from an abbreviation failed. Corrupt Dwarf");
                return DW_DLV_ERROR;
            }
            attr_form = (Dwarf_Half) utmp6;
            if (attr_form == DW_FORM_implicit_const) {
                empty_local_attrlist(dbg,head_attr);
                _dwarf_error_string(dbg, error,
                    DW_DLE_ATTR_OUTSIDE_SECTION,
                    "DW_DLE_ATTR_OUTSIDE_SECTION: "
                    " Reading Attriutes: an indirect form "
                    "leads to a DW_FORM_implicit_const "
                    "which is not handled. Corrupt Dwarf");
                return DW_DLV_ERROR;
            }
            if (!_dwarf_valid_form_we_know(attr_form,attr)) {
                dwarfstring m;

                dwarfstring_constructor(&m);
                dwarfstring_append_printf_u(&m,
                    "DW_DLE_UNKNOWN_FORM "
                    " form indirect leads to form"
                    " of  0x%x which is unknown",
                    attr_form);
                _dwarf_error_string(dbg, error,
                    DW_DLE_UNKNOWN_FORM,
                    dwarfstring_string(&m));
                dwarfstring_destructor(&m);
                empty_local_attrlist(dbg,head_attr);
                return DW_DLV_ERROR;
            }
            newattr_form = (Dwarf_Half)attr_form;
        }

        if (attr) {
            Dwarf_Attribute new_attr = 0;

            new_attr = (Dwarf_Attribute)
                _dwarf_get_alloc(dbg, DW_DLA_ATTR, 1);
            if (!new_attr) {
                empty_local_attrlist(dbg,head_attr);
                _dwarf_error_string(dbg, error, DW_DLE_ALLOC_FAIL,
                    "DW_DLE_ALLOC_FAIL: attempting to allocate"
                    " a Dwarf_Attribute record");
                return DW_DLV_ERROR;
            }
            new_attr->ar_attribute = (Dwarf_Half)attr;
            new_attr->ar_attribute_form_direct =
                (Dwarf_Half)attr_form;
            new_attr->ar_attribute_form = (Dwarf_Half)newattr_form;
            /*  Here the final address must be *inside* the
                section, as we will read from there, and read
                at least one byte, we think.
                We do not want info_ptr to point past end so
                we add 1 to the end-pointer.  */
            new_attr->ar_cu_context = die->di_cu_context;
            new_attr->ar_debug_ptr = info_ptr;
            new_attr->ar_die = die;
            new_attr->ar_dbg = dbg;
            if ( attr_form != DW_FORM_implicit_const &&
                _dwarf_reference_outside_section(die,
                (Dwarf_Small*) info_ptr,
                ((Dwarf_Small*) info_ptr )+1)) {
                dwarf_dealloc_attribute(new_attr);
                empty_local_attrlist(dbg,head_attr);
                _dwarf_error_string(dbg, error,
                    DW_DLE_ATTR_OUTSIDE_SECTION,
                    "DW_DLE_ATTR_OUTSIDE_SECTION: "
                    " Reading Attriutes: "
                    "We have run off the end of the section. "
                    "Corrupt Dwarf");
                return DW_DLV_ERROR;
            }
            if (attr_form == DW_FORM_implicit_const) {
                /*  The value is here, not in a DIE.
                    Do not increment info_ptr */
                new_attr->ar_implicit_const = implicit_const;
            } else {
                Dwarf_Unsigned sov = 0;
                int vres = 0;

                vres = _dwarf_get_size_of_val(dbg,
                    attr_form,
                    die->di_cu_context->cc_version_stamp,
                    die->di_cu_context->cc_address_size,
                    info_ptr,
                    die->di_cu_context->cc_length_size,
                    &sov,
                    die_info_end,
                    error);
                if (vres!= DW_DLV_OK) {
                    dwarf_dealloc_attribute(new_attr);
                    empty_local_attrlist(dbg,head_attr);
                    return vres;
                }
                info_ptr += sov;
            }
            /*  Add to single linked list */
            *last_attr = new_attr;
            last_attr = &new_attr->ar_next;
            new_attr = 0;
            attr_count++;
        }
    }
    if (!attr_count) {
        *attrbuf = NULL;
        *attrcnt = 0;
        return DW_DLV_NO_ENTRY;
    }
    {
        Dwarf_Attribute *attr_ptr = 0;

        attr_ptr = (Dwarf_Attribute *)
            _dwarf_get_alloc(dbg, DW_DLA_LIST, attr_count);
        if (attr_ptr == NULL) {
            empty_local_attrlist(dbg,head_attr);
            _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
            return DW_DLV_ERROR;
        }
        curr_attr = head_attr;
        for (i = 0; i < attr_count; i++) {
            *(attr_ptr + i) = curr_attr;
            curr_attr = curr_attr->ar_next;
        }
        *attrbuf = attr_ptr;
        *attrcnt = attr_count;
    }
    return DW_DLV_OK;
}

static void
build_alloc_qu_error(Dwarf_Debug dbg,
    const char *fieldname,
    Dwarf_Error *error)
{
    dwarfstring m;

    dwarfstring_constructor(&m);
    dwarfstring_append_printf_s(&m,
        "DW_DLE_ALLOC_FAIL :"
        " Attempt to malloc space for %s failed",
        (char *)fieldname);
    _dwarf_error_string(dbg,error,DW_DLE_ALLOC_FAIL,
        dwarfstring_string(&m));
    dwarfstring_destructor(&m);
}

/*
    This function takes a die, and an attr, and returns
    a pointer to the start of the value of that attr in
    the given die in the .debug_info section.  The form
    is returned in *attr_form.

    If the attr_form is DW_FORM_implicit_const
    (known signed, so most callers)
    that is fine, but in that case we do not
    need to actually set the *ptr_to_value.

    Returns NULL on error, or if attr is not found.
    However, *attr_form is 0 on error, and positive
    otherwise.
*/
static int
_dwarf_get_value_ptr(Dwarf_Die die,
    Dwarf_Half      attrnum_in,
    Dwarf_Half     *attr_form,
    Dwarf_Byte_Ptr *ptr_to_value,
    Dwarf_Signed   *implicit_const_out,
    Dwarf_Error    *error)
{
    Dwarf_Byte_Ptr abbrev_ptr = 0;
    Dwarf_Byte_Ptr abbrev_end = 0;
    Dwarf_Abbrev_List abbrev_list;
    Dwarf_Byte_Ptr info_ptr = 0;
    Dwarf_CU_Context context = die->di_cu_context;
    Dwarf_Byte_Ptr die_info_end = 0;
    Dwarf_Debug    dbg = 0;
    int            lres = 0;
    Dwarf_Unsigned i = 0;
    Dwarf_Unsigned highest_code = 0;

    if (!context) {
        _dwarf_error(NULL,error,DW_DLE_DIE_NO_CU_CONTEXT);
        return DW_DLV_ERROR;
    }
    dbg = context->cc_dbg;
    die_info_end =
        _dwarf_calculate_info_section_end_ptr(context);

    lres = _dwarf_get_abbrev_for_code(context,
        die->di_abbrev_list->abl_code,
        &abbrev_list,&highest_code,error);
    if (lres == DW_DLV_ERROR) {
        return lres;
    }
    if (lres == DW_DLV_NO_ENTRY) {
        dwarfstring m;

        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            "DW_DLE_CU_DIE_NO_ABBREV_LIST "
            "There is no abbrev present for code %u "
            "in this compilation unit. ",
            die->di_abbrev_list->abl_code);
        dwarfstring_append_printf_u(&m,
            "The highest known code "
            "in any compilation unit is %u.",
            highest_code);
        _dwarf_error_string(dbg, error,
            DW_DLE_CU_DIE_NO_ABBREV_LIST,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    abbrev_ptr = abbrev_list->abl_abbrev_ptr;
    abbrev_end = _dwarf_calculate_abbrev_section_end_ptr(context);
    info_ptr = die->di_debug_ptr;
    /* This ensures and checks die_info_end >= info_ptr */
    {
        /* SKIP_LEB128 */
        Dwarf_Unsigned ignore_this = 0;
        Dwarf_Unsigned len = 0;

        lres = dwarf_decode_leb128((char *)info_ptr,
            &len,&ignore_this,(char *)die_info_end);
        if (lres == DW_DLV_ERROR) {
            /* Stepped off the end SKIPping the leb  */
            dwarfstring m;

            dwarfstring_constructor(&m);
            dwarfstring_append_printf_u(&m,
                "DW_DLE_DIE_BAD: In building an attrlist "
                "we run off the end of the DIE while skipping "
                " the DIE tag, seeing the leb length as 0x%u ",
                len);
            _dwarf_error_string(dbg, error, DW_DLE_DIE_BAD,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
        info_ptr += len;
    }
    if (!abbrev_list->abl_attr) {
        int bres = 0;
        /* FIXME */
        bres = _dwarf_fill_in_attr_form_abtable(context,
            abbrev_ptr, abbrev_end, abbrev_list,
            error);
        if (bres != DW_DLV_OK) {
            return bres;
        }
    }
    if (!abbrev_list->abl_form) {
        build_alloc_qu_error(dbg,"abbrev_list->abl_form"
            " in _dwarf_get_value_ptr()", error);
        return DW_DLV_ERROR;
    }
    if (!abbrev_list->abl_attr) {
        build_alloc_qu_error(dbg,"abbrev_list->abl_attr"
            " in _dwarf_get_value_ptr()", error);
        return DW_DLV_ERROR;
    }
    for (i = 0; i < abbrev_list->abl_abbrev_count; ++i) {
        Dwarf_Unsigned curr_attr_form = 0;
        Dwarf_Unsigned curr_attr = 0;
        Dwarf_Unsigned value_size=0;
        Dwarf_Signed implicit_const = 0;
        int res = 0;

        curr_attr = abbrev_list->abl_attr[i];
        curr_attr_form = abbrev_list->abl_form[i];
        if (curr_attr_form == DW_FORM_indirect) {
            Dwarf_Unsigned utmp6;

            /* DECODE_LEB128_UWORD updates info_ptr */
            DECODE_LEB128_UWORD_CK(info_ptr, utmp6,dbg,
                error,die_info_end);
            curr_attr_form = (Dwarf_Half) utmp6;
        }
        if (curr_attr_form == DW_FORM_indirect) {
            _dwarf_error_string(dbg,error,DW_DLE_ATTR_FORM_BAD,
                "DW_DLE_ATTR_FORM_BAD: "
                "A DW_FORM_indirect in an abbreviation "
                " indirects to another "
                "DW_FORM_indirect, which is inappropriate.");
            return DW_DLV_ERROR;
        }
        if (curr_attr_form == DW_FORM_implicit_const) {
            if (!abbrev_list->abl_implicit_const) {
                _dwarf_error_string(dbg,error,DW_DLE_ATTR_FORM_BAD,
                    "DW_DLE_ATTR_FORM_BAD: "
                    "A DW_FORM_implicit_const in an abbreviation "
                    "has no implicit const value. Corrupt dwarf.");
                return DW_DLV_ERROR;
            }
            implicit_const = abbrev_list->abl_implicit_const[i];
        }
        if (curr_attr == attrnum_in) {
            *attr_form = (Dwarf_Half)curr_attr_form;
            if (implicit_const_out) {
                *implicit_const_out = implicit_const;
            }
            *ptr_to_value = info_ptr;
            return DW_DLV_OK;
        }
        res = _dwarf_get_size_of_val(dbg,
            curr_attr_form,
            die->di_cu_context->cc_version_stamp,
            die->di_cu_context->cc_address_size,
            info_ptr,
            die->di_cu_context->cc_length_size,
            &value_size,
            die_info_end,
            error);
        if (res != DW_DLV_OK) {
            return res;
        }
        {
            Dwarf_Unsigned len  = 0;

            /*  ptrdiff_t is generated but not named */
            len = (die_info_end >= info_ptr)?
                (die_info_end - info_ptr):0;
            if (value_size > len) {
                /*  Something badly wrong. We point past end
                    of debug_info or debug_types or a
                    section is unreasonably sized or we are
                    pointing to two different sections? */
                _dwarf_error_string(dbg,error,
                    DW_DLE_DIE_ABBREV_BAD,
                    "DW_DLE_DIE_ABBREV_BAD: in calculating the "
                    "size of a value based on abbreviation data "
                    "we find there is not enough room in "
                    "the .debug_info "
                    "section to contain the attribute value.");
                return DW_DLV_ERROR;
            }
        }
        info_ptr+= value_size;
    }
    return DW_DLV_NO_ENTRY;
}

int
dwarf_die_text(Dwarf_Die die,
    Dwarf_Half   attrnum,
    char       **ret_name,
    Dwarf_Error *error)
{
    Dwarf_Debug dbg = 0;
    int res = DW_DLV_ERROR;
    Dwarf_Attribute attr = 0;
    Dwarf_Error lerr = 0;

    CHECK_DIE(die, DW_DLV_ERROR);

    res = dwarf_attr(die,attrnum,&attr,&lerr);
    dbg = die->di_cu_context->cc_dbg;
    if (res == DW_DLV_ERROR) {
        dwarf_dealloc_error(dbg,lerr);
        return DW_DLV_NO_ENTRY;
    }
    if (res == DW_DLV_NO_ENTRY) {
        return res;
    }
    res = dwarf_formstring(attr,ret_name,error);
    dwarf_dealloc(dbg,attr, DW_DLA_ATTR);
    attr = 0;
    return res;
}

int
dwarf_diename(Dwarf_Die die,
    char       **ret_name,
    Dwarf_Error *error)
{
    return dwarf_die_text(die,DW_AT_name,ret_name,error);
}

/*  Never returns DW_DLV_NO_ENTRY */
int
dwarf_hasattr(Dwarf_Die die,
    Dwarf_Half  attr,
    Dwarf_Bool *return_bool, Dwarf_Error *error)
{
    Dwarf_Half attr_form = 0;
    Dwarf_Byte_Ptr info_ptr = 0;
    int res = 0;
    Dwarf_Signed implicit_const;

    CHECK_DIE(die, DW_DLV_ERROR);

    res = _dwarf_get_value_ptr(die, attr, &attr_form,&info_ptr,
        &implicit_const,error);
    if (res == DW_DLV_ERROR) {
        return res;
    }
    if (res == DW_DLV_NO_ENTRY) {
        *return_bool = FALSE;
    } else {
        *return_bool = TRUE;
    }
    return DW_DLV_OK;
}

int
dwarf_attr(Dwarf_Die die,
    Dwarf_Half attr,
    Dwarf_Attribute *ret_attr, Dwarf_Error *error)
{
    Dwarf_Half attr_form = 0;
    Dwarf_Attribute attrib = 0;
    Dwarf_Byte_Ptr info_ptr = 0;
    Dwarf_Debug dbg = 0;
    int res = 0;
    int lres = 0;
    Dwarf_Signed implicit_const = 0;
    Dwarf_Abbrev_List abbrev_list = 0;
    Dwarf_Unsigned highest_code = 0;
    Dwarf_CU_Context context = 0;

    context = die->di_cu_context;
    dbg = context->cc_dbg;

    CHECK_DIE(die, DW_DLV_ERROR);
    lres = _dwarf_get_abbrev_for_code(die->di_cu_context,
        die->di_abbrev_list->abl_code,
        &abbrev_list,
        &highest_code,error);
    if (lres == DW_DLV_ERROR) {
        return lres;
    }
    if (!abbrev_list->abl_attr) {
        Dwarf_Byte_Ptr abbrev_ptr = abbrev_list->abl_abbrev_ptr;
        Dwarf_Byte_Ptr abbrev_end =
            _dwarf_calculate_abbrev_section_end_ptr(context);
        int bres = 0;

        bres = _dwarf_fill_in_attr_form_abtable(
            die->di_cu_context,
            abbrev_ptr, abbrev_end, abbrev_list,
            error);
        if (bres != DW_DLV_OK) {
            return bres;
        }
    }
    if (!abbrev_list->abl_form) {
        build_alloc_qu_error(dbg,"abbrev_list->abl_form"
            " in dwarf_attr()", error);
        return DW_DLV_ERROR;
    }
    res = _dwarf_get_value_ptr(die, attr, &attr_form,&info_ptr,
        &implicit_const,error);
    if (res == DW_DLV_ERROR) {
        return res;
    }
    if (res == DW_DLV_NO_ENTRY) {
        return res;
    }

    attrib = (Dwarf_Attribute) _dwarf_get_alloc(dbg, DW_DLA_ATTR, 1);
    if (!attrib) {
        _dwarf_error_string(dbg, error, DW_DLE_ALLOC_FAIL,
            "DW_DLE_ALLOC_FAIL allocating a single Dwarf_Attribute"
            " in function dwarf_attr().");
        return DW_DLV_ERROR;
    }

    attrib->ar_attribute = attr;
    attrib->ar_attribute_form = attr_form;
    attrib->ar_attribute_form_direct = attr_form;
    attrib->ar_cu_context = die->di_cu_context;

    /*  Only nonzero if DW_FORM_implicit_const */
    attrib->ar_implicit_const = implicit_const;
    /*  Only nonnull if not DW_FORM_implicit_const */
    attrib->ar_debug_ptr = info_ptr;
    attrib->ar_die = die;
    attrib->ar_dbg = dbg;
    *ret_attr = attrib;
    return DW_DLV_OK;
}

/*  A DWP (.dwp) package object never contains .debug_addr,
    only a normal .o or executable object.
    Error returned here is on dbg, not tieddbg.
    This looks for DW_AT_addr_base and if present
    adds it in appropriately.
    Use _dwarf_look_in_local_and_tied_by_index()
    instead of this, in general.
    */
static int
_dwarf_extract_address_from_debug_addr(Dwarf_Debug dbg,
    Dwarf_CU_Context context,
    Dwarf_Unsigned index_to_addr,
    Dwarf_Addr *addr_out,
    Dwarf_Error *error)
{
    Dwarf_Unsigned address_base = 0;
    Dwarf_Unsigned addrindex = index_to_addr;
    Dwarf_Unsigned addr_offset = 0;
    Dwarf_Unsigned ret_addr = 0;
    int res = 0;
    Dwarf_Byte_Ptr  sectionstart = 0;
    Dwarf_Byte_Ptr  sectionend = 0;
    Dwarf_Unsigned  sectionsize  = 0;

    address_base = context->cc_addr_base_offset;
    res = _dwarf_load_section(dbg, &dbg->de_debug_addr,error);
    if (res != DW_DLV_OK) {
        /*  Ignore the inner error, report something meaningful */
        if (res == DW_DLV_ERROR && error) {
            dwarf_dealloc_error(dbg,*error);
            *error = 0;
        }
        _dwarf_error(dbg,error,
            DW_DLE_MISSING_NEEDED_DEBUG_ADDR_SECTION);
        return DW_DLV_ERROR;
    }
    /*  DW_FORM_addrx has a base value from the CU die:
        DW_AT_addr_base.  DW_OP_addrx and DW_OP_constx
        rely on DW_AT_addr_base too. */
    /*  DW_FORM_GNU_addr_index  relies on DW_AT_GNU_addr_base
        which is in the CU die. */

    sectionstart = dbg->de_debug_addr.dss_data;
    addr_offset = address_base +
        (addrindex * context->cc_address_size);
    /*  The offsets table is a series of address-size entries
        but with a base. */
    sectionsize = dbg->de_debug_addr.dss_size;
    sectionend = sectionstart + sectionsize;
    /*  At this point we have a local .debug_addr table
        Might get here on dbg or tied-dbg. Check either way
        ASSERT: cc_address_size is sensible (small) */
    if (addrindex >= sectionsize ||
        (addrindex*context->cc_address_size) >= sectionsize ||
        addr_offset > sectionsize ||
        addr_offset > (sectionsize - context->cc_address_size)) {
        dwarfstring m;

        /* Was DW_DLE_ATTR_FORM_SIZE_BAD. Regression issue */
        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            "DW_DLE_ATTR_FORM_OFFSET_BAD: "
            "Extracting an address from .debug_addr fails"
            "as the offset is  0x%x ",
            addr_offset);
        dwarfstring_append_printf_u(&m,
            "but the object section is just 0x%x "
            "bytes long so there not enough space"
            " for an address.",
            sectionsize);
        _dwarf_error_string(dbg, error,
            DW_DLE_ATTR_FORM_OFFSET_BAD,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    READ_UNALIGNED_CK(dbg,ret_addr,Dwarf_Addr,
        sectionstart + addr_offset,
        context->cc_address_size,
        error,sectionend);
    *addr_out = ret_addr;
    return DW_DLV_OK;
}

/*  Looks for an address (Dwarf_Addr) value vi an
    index into debug_addr.  If it fails with
    DW_DLE_MISSING_NEEDED_DEBUG_ADDR_SECTION we
    find a context in tieddbg and look there. */
int
_dwarf_look_in_local_and_tied_by_index(
    Dwarf_Debug dbg,
    Dwarf_CU_Context context,
    Dwarf_Unsigned index,
    Dwarf_Addr *return_addr,
    Dwarf_Error *error)
{
    int res2 = 0;

    res2 = _dwarf_extract_address_from_debug_addr(dbg,
        context, index, return_addr, error);
    if (res2 != DW_DLV_OK) {
        if (res2 == DW_DLV_ERROR &&
            error && dwarf_errno(*error) ==
            DW_DLE_MISSING_NEEDED_DEBUG_ADDR_SECTION
            && dbg->de_tied_data.td_tied_object) {
            /* see also DBG_HAS_SECONDARY macro */
            int res3 = 0;

            /*  We do not want to leak error structs... */
            /* *error safe */
            dwarf_dealloc_error(dbg,*error);
            *error = 0; /* *error safe */
            /*  Any error is returned on dbg,
                not tieddbg. */
            res3 = _dwarf_get_addr_from_tied(dbg,
                context,index,return_addr,error);
            return res3;
        }
        return res2;
    }
    return DW_DLV_OK;
}

/*  The DIE here can be any DIE in the relevant CU.
    index is an index into .debug_addr */
int
dwarf_debug_addr_index_to_addr(Dwarf_Die die,
    Dwarf_Unsigned index,
    Dwarf_Addr  *return_addr,
    Dwarf_Error *error)
{
    Dwarf_Debug dbg = 0;
    Dwarf_CU_Context context = 0;
    int res = 0;

    CHECK_DIE(die, DW_DLV_ERROR);
    context = die->di_cu_context;
    dbg = context->cc_dbg;

    /* error is returned on dbg, not tieddbg. */
    res = _dwarf_look_in_local_and_tied_by_index(dbg,
        context,
        index,
        return_addr,
        error);
    return res;
}
/*  ASSERT:
    attr_form == DW_FORM_GNU_addr_index ||
        attr_form == DW_FORM_addrx
    We are looking for data in .debug_addr,
    which could be in base file or in tied-file..
*/
int
_dwarf_look_in_local_and_tied(Dwarf_Half attr_form,
    Dwarf_CU_Context context,
    Dwarf_Small     *info_ptr,
    Dwarf_Addr      *return_addr,
    Dwarf_Error     *error)
{
    int res2 = 0;
    Dwarf_Unsigned index_to_addr = 0;
    Dwarf_Debug dbg = 0;

    /*  We get the index. It might apply here
        or in tied object. Checking that next. */
    dbg = context->cc_dbg;
    res2 = _dwarf_get_addr_index_itself(attr_form,
        info_ptr,dbg,context, &index_to_addr,error);
    if (res2 != DW_DLV_OK) {
        return res2;
    }
    /* error is returned on dbg, not tieddbg. */
    res2 = _dwarf_look_in_local_and_tied_by_index(
        dbg,context,index_to_addr,return_addr,error);
    return res2;

}

static int
_dwarf_lowpc_internal(Dwarf_Die die,
    Dwarf_Half attrnum,
    const char *msg,
    Dwarf_Addr  *return_addr,
    Dwarf_Error *error)
{
    Dwarf_Addr ret_addr = 0;
    Dwarf_Byte_Ptr info_ptr = 0;
    Dwarf_Half attr_form = 0;
    Dwarf_Debug dbg = 0;
    Dwarf_Half address_size = 0;
    Dwarf_Half offset_size = 0;
    int version = 0;
    enum Dwarf_Form_Class class = DW_FORM_CLASS_UNKNOWN;
    int res = 0;
    Dwarf_CU_Context context = 0;
    Dwarf_Small *die_info_end = 0;

    CHECK_DIE(die, DW_DLV_ERROR);
    context = die->di_cu_context;
    dbg = context->cc_dbg;
    address_size = context->cc_address_size;
    offset_size = context->cc_length_size;
    res = _dwarf_get_value_ptr(die, attrnum,
        &attr_form,&info_ptr,0,error);
    if (res == DW_DLV_ERROR) {
        return res;
    }
    if (res == DW_DLV_NO_ENTRY) {
        return res;
    }
    version = context->cc_version_stamp;
    class = dwarf_get_form_class(version,attrnum,
        offset_size,attr_form);
    if (class != DW_FORM_CLASS_ADDRESS) {
        /* Not a correct FORM for low_pc or entry_pc */
        _dwarf_error_string(dbg, error, DW_DLE_LOWPC_WRONG_CLASS,
            (char *)msg);
        return DW_DLV_ERROR;
    }

    if (attr_form == DW_FORM_GNU_addr_index ||
        attr_form == DW_FORM_addrx) {
        /* error is returned on dbg, not tieddbg. */
        res = _dwarf_look_in_local_and_tied(
            attr_form,
            context,
            info_ptr,
            return_addr,
            error);
        return res;
    }
    die_info_end = _dwarf_calculate_info_section_end_ptr(context);
    READ_UNALIGNED_CK(dbg, ret_addr, Dwarf_Addr,
        info_ptr, address_size,
        error,die_info_end);

    *return_addr = ret_addr;
    return DW_DLV_OK;
}
int
dwarf_lowpc(Dwarf_Die die,
    Dwarf_Addr  *return_addr,
    Dwarf_Error *error)
{
    int res = 0;

    res = _dwarf_lowpc_internal (die,DW_AT_low_pc,
        "DW_AT_low_pc data unavailable",
        return_addr,error);
    return res;
}
int
_dwarf_entrypc(Dwarf_Die die,
    Dwarf_Addr  *return_addr,
    Dwarf_Error *error)
{
    int res = 0;

    res = _dwarf_lowpc_internal (die,DW_AT_entry_pc,
        "DW_AT_entry_pc and DW_AT_low_pc data unavailable",
        return_addr,error);
    return res;
}

/*  If 'die' contains the DW_AT_type attribute, it returns
    the (global) offset referenced by the attribute through
    the return_off pointer.
    Returns through return_is_info which section applies.
    In case of DW_DLV_NO_ENTRY or DW_DLV_ERROR it sets offset zero. */
int
dwarf_dietype_offset(Dwarf_Die die,
    Dwarf_Off *return_off,
    Dwarf_Bool *return_is_info,
    Dwarf_Error *error)
{
    int res = 0;
    Dwarf_Off offset = 0;
    Dwarf_Attribute attr = 0;
    Dwarf_Bool is_info = 0;

    CHECK_DIE(die, DW_DLV_ERROR);
    /* Lets see where the input die is */
    is_info =  dwarf_get_die_infotypes_flag(die);
    res = dwarf_attr(die,DW_AT_type,&attr,error);
    if (res == DW_DLV_OK) {
        if (attr->ar_attribute_form_direct == DW_FORM_ref_sig8){
            is_info = FALSE;
        }
        res = dwarf_global_formref(attr,&offset,error);
        if (res == DW_DLV_OK) {
            *return_off = offset;
            *return_is_info = is_info;
        }
        dwarf_dealloc_attribute(attr);
    }
    return res;
}

/*  Only a few values are inherited from the tied
    file. Not rnglists or loclists base offsets?
    merging into main context (dwp) from tieddbg (Skeleton).
    and returning a pointer to the tiedcontext created here.
    (such contexts are freed by dwarf_finish on the tied
    object file).
*/
int
_dwarf_merge_all_base_attrs_of_cu_die(Dwarf_CU_Context context,
    Dwarf_Debug tieddbg,
    Dwarf_CU_Context *tiedcontext_out,
    Dwarf_Error *error)
{
    Dwarf_CU_Context tiedcontext = 0;
    int res = 0;

    if (!tieddbg) {
        return DW_DLV_NO_ENTRY;
    }
    if (!context->cc_signature_present) {
        return DW_DLV_NO_ENTRY;
    }
    res = _dwarf_search_for_signature(tieddbg,
        context->cc_signature,
        &tiedcontext,
        error);
    if ( res == DW_DLV_ERROR) {
        return res;
    }
    if ( res == DW_DLV_NO_ENTRY) {
        return res;
    }
    if (tiedcontext->cc_low_pc_present) {
        /*  A dwo/dwp will not have this, merge from tied
            Needed for rnglists/loclists
        */
        context->cc_low_pc_present =
            tiedcontext->cc_low_pc_present;
        context->        cc_low_pc =
            tiedcontext->cc_low_pc;
    }

    if (tiedcontext->cc_base_address_present) {
        context->cc_base_address_present =
            tiedcontext->cc_base_address_present;
        context->        cc_base_address =
            tiedcontext->cc_base_address;
    }
    if (tiedcontext->cc_addr_base_offset_present) {
        /*  This is a base-offset, not an address. */
        context->        cc_addr_base_offset_present =
            tiedcontext->cc_addr_base_offset_present;
        context->        cc_addr_base_offset=
            tiedcontext->cc_addr_base_offset;
    }
    if (context->cc_version_stamp == DW_CU_VERSION4 ||
        context->cc_version_stamp == DW_CU_VERSION5)  {
#if 0   /* we do not inherit cc_rnglists_base_present */
        /*  This inheritance has been removed from DWARF6
            during 2024. */

        if (!context->cc_rnglists_base_present) {
            context->cc_rnglists_base_present =
                tiedcontext->cc_rnglists_base_present;
            context->cc_rnglists_base =
                tiedcontext->cc_rnglists_base;
        }
#endif
        if (!context->cc_ranges_base_present) {
            context->cc_ranges_base_present=
                tiedcontext->cc_ranges_base_present;
            context->cc_ranges_base=
                tiedcontext->cc_ranges_base;
        }
    }
    if (!context->cc_str_offsets_tab_present) {
        context->        cc_str_offsets_tab_present =
            tiedcontext->cc_str_offsets_tab_present;
        context->        cc_str_offsets_header_offset=
            tiedcontext->cc_str_offsets_header_offset;
        context->        cc_str_offsets_tab_to_array=
            tiedcontext->cc_str_offsets_tab_to_array;
        context->        cc_str_offsets_table_size=
            tiedcontext->cc_str_offsets_table_size;
        context->        cc_str_offsets_version=
            tiedcontext->cc_str_offsets_version;
        context->        cc_str_offsets_offset_size=
            tiedcontext->cc_str_offsets_offset_size;
    }
    if (tiedcontext_out) {
        *tiedcontext_out = tiedcontext;
    }
    return DW_DLV_OK;
}

int
_dwarf_get_string_base_attr_value(Dwarf_Debug dbg,
    Dwarf_CU_Context context,
    Dwarf_Unsigned *sbase_out,
    Dwarf_Error *error)
{
    (void)dbg;
    (void)error;
    if (context->cc_str_offsets_tab_present) {
        *sbase_out = context->cc_str_offsets_header_offset;
        return DW_DLV_OK;
    }
    *sbase_out = 0;
    return DW_DLV_OK;
}
/*  Goes to the CU die and finds the DW_AT_GNU_addr_base
    (or DW_AT_addr_base ) and gets the value from that CU die
    and returns it through abase_out. If we cannot find the value
    it is a serious error in the DWARF.
    */

/*  This works for all versions of DWARF.
    The consumer has to check the return_form or
    return_class to decide if the value returned
    through return_value is an address or an address-offset.

    See  DWARF4 section 2.17.2,
    "Contiguous Address Range".
    */
int
dwarf_highpc_b(Dwarf_Die die,
    Dwarf_Addr *return_value,
    Dwarf_Half *return_form,
    enum Dwarf_Form_Class *return_class,
    Dwarf_Error *error)
{
    Dwarf_Byte_Ptr info_ptr = 0;
    Dwarf_Half attr_form = 0;
    Dwarf_Debug dbg = 0;
    Dwarf_Half address_size = 0;
    Dwarf_Half offset_size = 0;
    enum Dwarf_Form_Class class = DW_FORM_CLASS_UNKNOWN;
    Dwarf_Half version = 0;
    Dwarf_Byte_Ptr die_info_end = 0;
    int res = 0;

    CHECK_DIE(die, DW_DLV_ERROR);
    dbg = die->di_cu_context->cc_dbg;
    address_size = die->di_cu_context->cc_address_size;

    res = _dwarf_get_value_ptr(die, DW_AT_high_pc,
        &attr_form,&info_ptr,0,error);
    if (res == DW_DLV_ERROR) {
        return res;
    }
    if (res == DW_DLV_NO_ENTRY) {
        return res;
    }
    die_info_end = _dwarf_calculate_info_section_end_ptr(
        die->di_cu_context);

    version = die->di_cu_context->cc_version_stamp;
    offset_size = die->di_cu_context->cc_length_size;
    class = dwarf_get_form_class(version,DW_AT_high_pc,
        offset_size,attr_form);

    if (class == DW_FORM_CLASS_ADDRESS) {
        Dwarf_Addr addr = 0;
        if (dwarf_addr_form_is_indexed(attr_form)) {
            Dwarf_Unsigned addr_out = 0;
            Dwarf_Unsigned index_to_addr = 0;
            int res2 = 0;
            Dwarf_CU_Context context = die->di_cu_context;

            /*  index_to_addr we get here might apply
                to this dbg or to tieddbg. */
            /* error is returned on dbg, not tied */
            res2 = _dwarf_get_addr_index_itself(attr_form,
                info_ptr,dbg,context,&index_to_addr,error);
            if (res2 != DW_DLV_OK) {
                return res2;
            }
            res2= _dwarf_look_in_local_and_tied_by_index(dbg,
                context,
                index_to_addr,
                &addr_out,
                error);
            if (res2 != DW_DLV_OK) {
                return res2;
            }
        }
        READ_UNALIGNED_CK(dbg, addr, Dwarf_Addr,
            info_ptr, address_size,
            error,die_info_end);
        *return_value = addr;
    } else {
        int res3 = 0;
        Dwarf_Unsigned v = 0;
        res3 = _dwarf_die_attr_unsigned_constant(die,DW_AT_high_pc,
            &v,error);
        if (res3 != DW_DLV_OK) {
            Dwarf_Byte_Ptr info_ptr2 = 0;

            res3 = _dwarf_get_value_ptr(die, DW_AT_high_pc,
                &attr_form,&info_ptr2,0,error);
            if (res3 == DW_DLV_ERROR) {
                return res3;
            }
            if (res3 == DW_DLV_NO_ENTRY) {
                return res3;
            }
            if (attr_form == DW_FORM_sdata) {
                Dwarf_Signed sval = 0;

                /*  DWARF4 defines the value as an unsigned offset
                    in section 2.17.2. */
                DECODE_LEB128_UWORD_CK(info_ptr2, sval,
                    dbg,error,die_info_end);
                *return_value = (Dwarf_Unsigned)sval;
            } else {
                _dwarf_error(dbg, error, DW_DLE_HIGHPC_WRONG_FORM);
                return DW_DLV_ERROR;
            }
        } else {
            *return_value = v;
        }
    }
    /*  Allow null args starting 22 April 2019. */
    if (return_form) {
        *return_form = attr_form;
    }
    if (return_class) {
        *return_class = class;
    }
    return DW_DLV_OK;
}

/* The dbg and context here are a file with DW_FORM_addrx
    but missing .debug_addr. So go to the tied file
    and using the signature from the current context
    locate the target CU in the tied file Then
    get the address.

*/
int
_dwarf_get_addr_from_tied(Dwarf_Debug primary_dbg,
    Dwarf_CU_Context context,
    Dwarf_Unsigned index,
    Dwarf_Addr *addr_out,
    Dwarf_Error*error)
{
    Dwarf_Debug tieddbg = 0;
    int res = 0;
    Dwarf_Addr local_addr = 0;
    Dwarf_CU_Context tiedcontext = 0;
    Dwarf_Unsigned addrtabsize = 0;

    if (!context->cc_signature_present) {
        _dwarf_error(primary_dbg, error,
            DW_DLE_NO_SIGNATURE_TO_LOOKUP);
        return  DW_DLV_ERROR;
    }
    if (!DBG_HAS_SECONDARY(primary_dbg)) {
        _dwarf_error(primary_dbg, error,
            DW_DLE_NO_TIED_ADDR_AVAILABLE);
        return  DW_DLV_ERROR;
    }
    tieddbg = primary_dbg->de_secondary_dbg;
    res = _dwarf_search_for_signature(tieddbg,
        context->cc_signature,
        &tiedcontext,
        error);
    if (res == DW_DLV_ERROR) {
        return res;
    }
    if ( res == DW_DLV_NO_ENTRY) {
        return res;
    }
    /* We have .debug_addr */
    addrtabsize = tieddbg->de_debug_addr.dss_size;
    if ( (index > tieddbg->de_filesize ||
        index > addrtabsize ||
        (index*tiedcontext->cc_address_size) > addrtabsize)) {
        _dwarf_error_string(primary_dbg,error,
            DW_DLE_ATTR_FORM_OFFSET_BAD,
            "DW_DLE_ATTR_FORM_OFFSET_BAD "
            "Looking for an index from an addr FORM "
            "we find an impossibly large index value for the tied "
            "object. Corrupt DWARF");
        return DW_DLV_ERROR;
    }
    res = _dwarf_extract_address_from_debug_addr(tieddbg,
        tiedcontext,
        index,
        &local_addr,
        error);
    if ( res == DW_DLV_ERROR) {
        return res;
    }
    if ( res == DW_DLV_NO_ENTRY) {
        return res;
    }
    *addr_out = local_addr;
    return DW_DLV_OK;
}

/*
    Takes a die, an attribute attr, and checks if attr
    occurs in die.  Attr is required to be an attribute
    whose form is in the "constant" class.  If attr occurs
    in die, the value is returned.

    It does not really allow for a signed constant, and
    DWARF does not always specify that only non-negative
    values are allowed..

    Returns DW_DLV_OK, DW_DLV_ERROR, or DW_DLV_NO_ENTRY as
    appropriate. Sets the value thru the pointer return_val.

    This function is meant to do all the
    processing for dwarf_bytesize, dwarf_bitsize, dwarf_bitoffset,
    and dwarf_srclang. And it helps in dwarf_highpc_with_form().
*/
static int
_dwarf_die_attr_unsigned_constant(Dwarf_Die die,
    Dwarf_Half      attr,
    Dwarf_Unsigned *return_val,
    Dwarf_Error    *error)
{
    Dwarf_Byte_Ptr info_ptr = 0;
    Dwarf_Half attr_form = 0;
    Dwarf_Unsigned ret_value = 0;
    Dwarf_Signed implicit_const_value = 0;
    Dwarf_Debug dbg = 0;
    int res = 0;
    Dwarf_Byte_Ptr die_info_end = 0;

    CHECK_DIE(die, DW_DLV_ERROR);

    die_info_end =
        _dwarf_calculate_info_section_end_ptr(die->di_cu_context);
    dbg = die->di_cu_context->cc_dbg;
    res = _dwarf_get_value_ptr(die,attr,&attr_form,
        &info_ptr,&implicit_const_value,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    switch (attr_form) {
    case DW_FORM_data1:
        READ_UNALIGNED_CK(dbg, ret_value, Dwarf_Unsigned,
            info_ptr, sizeof(Dwarf_Small),
            error,die_info_end);
        *return_val = ret_value;
        return DW_DLV_OK;

    case DW_FORM_data2:
        READ_UNALIGNED_CK(dbg, ret_value, Dwarf_Unsigned,
            info_ptr, sizeof(Dwarf_Shalf),
            error,die_info_end);
        *return_val = ret_value;
        return DW_DLV_OK;

    case DW_FORM_data4:
        READ_UNALIGNED_CK(dbg, ret_value, Dwarf_Unsigned,
            info_ptr, DWARF_32BIT_SIZE,
            error,die_info_end);
        *return_val = ret_value;
        return DW_DLV_OK;

    case DW_FORM_data8:
        READ_UNALIGNED_CK(dbg, ret_value, Dwarf_Unsigned,
            info_ptr, DWARF_64BIT_SIZE,
            error,die_info_end);
        *return_val = ret_value;
        return DW_DLV_OK;

    case DW_FORM_udata: {
        Dwarf_Unsigned v = 0;

        DECODE_LEB128_UWORD_CK(info_ptr, v,dbg,error,die_info_end);
        *return_val = v;
        return DW_DLV_OK;

    }
    case DW_FORM_implicit_const: {
        if (implicit_const_value < (Dwarf_Signed)0) {
            dwarfstring m;

            dwarfstring_constructor(&m);
            dwarfstring_append_printf_i(&m,
                "DW_DLE_NEGATIVE_SIZE "
                "An implicit const value of "
                "%d is inappropriate as a size",
                implicit_const_value);
            _dwarf_error_string(dbg, error,
                DW_DLE_NEGATIVE_SIZE,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
        *return_val = implicit_const_value;
        return DW_DLV_OK;
    }

    default:
        _dwarf_error(dbg, error, DW_DLE_DIE_BAD);
        return DW_DLV_ERROR;
    }
}

/*  Size Value >= 0 is not specified in DWARF5, but
    a negative value is surely not meaningful. */
int
dwarf_bytesize(Dwarf_Die die,
    Dwarf_Unsigned *ret_size, Dwarf_Error *error)
{
    Dwarf_Unsigned luns = 0;
    int res = _dwarf_die_attr_unsigned_constant(die, DW_AT_byte_size,
        &luns, error);
    *ret_size = luns;
    return res;
}

/*  Size Value >= 0 is not specified in DWARF5, but
    a negative value is not meaningful. */
int
dwarf_bitsize(Dwarf_Die die,
    Dwarf_Unsigned *ret_size, Dwarf_Error *error)
{
    Dwarf_Unsigned luns = 0;
    int res = _dwarf_die_attr_unsigned_constant(die, DW_AT_bit_size,
        &luns, error);
    *ret_size = luns;
    return res;
}

/*  Size Value >= 0 required. DWARF5 sec5.7.6
    The definition of DW_AT_data_bit_offset
    (DWARF4, DWARF5) is radically
    different from DW_AT_bit_offset (DWARF2,
    DWARF3.  */
int
dwarf_bitoffset(Dwarf_Die die,
    Dwarf_Half     *attribute,
    Dwarf_Unsigned *ret_offset,
    Dwarf_Error    *error)
{
    Dwarf_Unsigned luns = 0;
    int res = 0;
    /* DWARF4,5 case */
    res = _dwarf_die_attr_unsigned_constant(die,
        DW_AT_data_bit_offset, &luns, error);
    if (res == DW_DLV_NO_ENTRY) {
        /* DWARF2, DWARF3 case. */
        res = _dwarf_die_attr_unsigned_constant(die,
            DW_AT_bit_offset, &luns, error);
        if (res == DW_DLV_OK) {
            *attribute = DW_AT_bit_offset;
            *ret_offset = luns;
            return DW_DLV_OK;
        }
    } else if (res == DW_DLV_OK) {
        *attribute = DW_AT_data_bit_offset;
        *ret_offset = luns;
        return DW_DLV_OK;
    } else { /* fall to return */ }
    return res;
}

/*  Refer section 3.1, page 21 in Dwarf Definition.
    Language codes are always non-negative
    and specified in the DWARF standard*/
int
dwarf_srclang(Dwarf_Die die,
    Dwarf_Unsigned *ret_name, Dwarf_Error *error)
{
    Dwarf_Unsigned name = 0;
    int res = _dwarf_die_attr_unsigned_constant(die, DW_AT_language,
        &name, error);
    *ret_name = name;
    return res;
}

int
dwarf_srclanglname(Dwarf_Die die,
    Dwarf_Unsigned *ret_name, Dwarf_Error *error)
{
    Dwarf_Unsigned luns = 0;
    int res = _dwarf_die_attr_unsigned_constant(die,
        DW_AT_language_name,
        &luns, error);
    *ret_name = luns;
    return res;
}

/*  Refer section 5.4, page 37 in Dwarf Definition.
    array order values are always non-negative
    and specified in the DWARF standard*/
int
dwarf_arrayorder(Dwarf_Die die,
    Dwarf_Unsigned *ret_size, Dwarf_Error *error)
{
    Dwarf_Unsigned luns = 0;
    int res = _dwarf_die_attr_unsigned_constant(die, DW_AT_ordering,
        &luns, error);
    *ret_size = luns;
    return res;
}

/*  Return DW_DLV_OK if ok
    DW_DLV_ERROR if failure.

    attr must be a valid attribute pointer.

    If the die and the attr are not related the result is
    meaningless.  */
int
dwarf_attr_offset(Dwarf_Die die, Dwarf_Attribute attr,
    Dwarf_Off   *offset /* return offset thru this ptr */,
    Dwarf_Error *error)
{
    Dwarf_Off attroff = 0;
    Dwarf_Small *dataptr = 0;
    Dwarf_Debug dbg = 0;

    CHECK_DIE(die, DW_DLV_ERROR);
    dbg = die->di_cu_context->cc_dbg;
    dataptr = die->di_is_info? dbg->de_debug_info.dss_data:
        dbg->de_debug_types.dss_data;

    attroff = (attr->ar_debug_ptr - dataptr);
    *offset = attroff;
    return DW_DLV_OK;
}

Dwarf_Unsigned
dwarf_die_abbrev_code(Dwarf_Die die)
{
    if (!die) {
        return 0;
    }
    return die->di_abbrev_code;
}

/*  Returns a flag through ablhas_child. Non-zero if
    the DIE has children, zero if it does not.
    It has no Dwarf_Error arg!
*/
int
dwarf_die_abbrev_children_flag(Dwarf_Die die,Dwarf_Half *ab_has_child)
{
    if (!die) {
        return DW_DLV_ERROR;
    }
    if (die->di_abbrev_list) {
        *ab_has_child = die->di_abbrev_list->abl_has_child;
        return DW_DLV_OK;
    }
    return DW_DLV_ERROR;
}

/*  Helper function for finding form class.
    Only called for FORMs that might be offsets
    to one or another section.  */
static enum Dwarf_Form_Class
dw_get_special_offset(Dwarf_Half attrnum,
    Dwarf_Half dwversion)
{
    switch (attrnum) {
    case DW_AT_stmt_list:
        return DW_FORM_CLASS_LINEPTR;
    case DW_AT_macro_info: /* DWARF2-DWARF4 */
        return DW_FORM_CLASS_MACPTR;
    case DW_AT_start_scope:
    case DW_AT_ranges: {
        if (dwversion <= 4) {
            return DW_FORM_CLASS_RANGELISTPTR;
        }
        return DW_FORM_CLASS_RNGLIST;
        }
    case DW_AT_GNU_ranges_base: /* DWARF5-like */
    case DW_AT_rnglists_base:   /* DWARF5 */
        return DW_FORM_CLASS_RNGLISTSPTR;
    case DW_AT_GNU_macros:    /* DWARF5-like */
    case DW_AT_macros:        /* DWARF5 */
        return DW_FORM_CLASS_MACROPTR;
    case DW_AT_loclists_base: /* DWARF5 */
        return DW_FORM_CLASS_LOCLISTSPTR;
    case DW_AT_GNU_addr_base: /* DWARF5-like */
    case DW_AT_addr_base:     /* DWARF5 */
        return DW_FORM_CLASS_ADDRPTR;
    case DW_AT_str_offsets_base: /* DWARF5 */
        return DW_FORM_CLASS_STROFFSETSPTR;

    case DW_AT_location:
    case DW_AT_string_length:
    case DW_AT_return_addr:
    case DW_AT_data_member_location:
    case DW_AT_frame_base:
    case DW_AT_segment:
    case DW_AT_static_link:
    case DW_AT_use_location:
    case DW_AT_vtable_elem_location: {
        if (dwversion <= 4) {
            return DW_FORM_CLASS_LOCLIST;
        }
        return DW_FORM_CLASS_LOCLISTPTR;
        }
    case DW_AT_GNU_locviews:
    case DW_AT_sibling:
    case DW_AT_byte_size :
    case DW_AT_bit_offset :
    case DW_AT_bit_size :
    case DW_AT_discr :
    case DW_AT_import :
    case DW_AT_common_reference:
    case DW_AT_containing_type:
    case DW_AT_default_value:
    case DW_AT_lower_bound:
    case DW_AT_bit_stride:
    case DW_AT_upper_bound:
    case DW_AT_abstract_origin:
    case DW_AT_base_types:
    case DW_AT_count:
    case DW_AT_friend:
    case DW_AT_namelist_item:
    case DW_AT_priority:
    case DW_AT_specification:
    case DW_AT_type:
    case DW_AT_allocated:
    case DW_AT_associated:
    case DW_AT_byte_stride:
    case DW_AT_extension:
    case DW_AT_trampoline:
    case DW_AT_small:
    case DW_AT_object_pointer:
    case DW_AT_signature:
        return DW_FORM_CLASS_REFERENCE;
    case DW_AT_GNU_entry_view:
        return DW_FORM_CLASS_CONSTANT;
    case DW_AT_MIPS_fde: /* SGI/IRIX extension */
        return DW_FORM_CLASS_FRAMEPTR;
    default: break;
    }
    return DW_FORM_CLASS_UNKNOWN;
}

static int
block_means_locexpr(Dwarf_Half attr)
{
    switch(attr) {
    case DW_AT_bit_size:
    case DW_AT_byte_size:
    case DW_AT_call_data_location:
    case DW_AT_call_data_value:
    case DW_AT_call_value:
    case DW_AT_data_member_location:
    case DW_AT_frame_base:
    case DW_AT_GNU_call_site_target:
    case DW_AT_GNU_call_site_value:
    case DW_AT_location:
    case DW_AT_return_addr:
    case DW_AT_segment:
    case DW_AT_static_link:
    case DW_AT_string_length:
    case DW_AT_use_location:
    case DW_AT_vtable_elem_location:
        return TRUE;
    default: break;
    }
    return FALSE;
}

/*  It takes 4 pieces of data (including the FORM)
    to accurately determine the form 'class' as documented
    in the DWARF spec. This is per DWARF4, but will work
    for DWARF2 or 3 as well.  */
enum Dwarf_Form_Class
dwarf_get_form_class(
    Dwarf_Half dwversion,
    Dwarf_Half attrnum,
    Dwarf_Half offset_size,
    Dwarf_Half form)
{
    switch (form) {
    case  DW_FORM_addr:  return DW_FORM_CLASS_ADDRESS;
    case  DW_FORM_data2:  return DW_FORM_CLASS_CONSTANT;

    case  DW_FORM_data4:
        if (dwversion <= 3 && offset_size == 4) {
            enum Dwarf_Form_Class class =
                dw_get_special_offset(attrnum, dwversion);
            if (class != DW_FORM_CLASS_UNKNOWN) {
                return class;
            }
        }
        return DW_FORM_CLASS_CONSTANT;
    case  DW_FORM_data8:
        if (dwversion <= 3 && offset_size == 8) {
            enum Dwarf_Form_Class class =
                dw_get_special_offset(attrnum, dwversion);
            if (class != DW_FORM_CLASS_UNKNOWN) {
                return class;
            }
        }
        return DW_FORM_CLASS_CONSTANT;
    case  DW_FORM_sec_offset:
        {
            enum Dwarf_Form_Class class =
                dw_get_special_offset(attrnum, dwversion);
            if (class != DW_FORM_CLASS_UNKNOWN) {
                return class;
            }
        }
        /* We do not know what this is. */
        return DW_FORM_CLASS_UNKNOWN;
        break;

    case  DW_FORM_string: return DW_FORM_CLASS_STRING;
    case  DW_FORM_strp:   return DW_FORM_CLASS_STRING;

    case  DW_FORM_block:
    case  DW_FORM_block1:
    case  DW_FORM_block2:
    case  DW_FORM_block4:
        if (dwversion <= 3) {
            if (block_means_locexpr(attrnum)) {
                return DW_FORM_CLASS_EXPRLOC;
            }
        }
        return DW_FORM_CLASS_BLOCK;
    /* DWARF4 and DWARF5 */
    case  DW_FORM_exprloc:      return DW_FORM_CLASS_EXPRLOC;

    case  DW_FORM_data16:  return DW_FORM_CLASS_CONSTANT;
    case  DW_FORM_data1:  return DW_FORM_CLASS_CONSTANT;
    case  DW_FORM_sdata:  return DW_FORM_CLASS_CONSTANT;
    case  DW_FORM_udata:  return DW_FORM_CLASS_CONSTANT;

    case  DW_FORM_ref_addr:    return DW_FORM_CLASS_REFERENCE;
    case  DW_FORM_ref1:        return DW_FORM_CLASS_REFERENCE;
    case  DW_FORM_ref2:        return DW_FORM_CLASS_REFERENCE;
    case  DW_FORM_ref4:        return DW_FORM_CLASS_REFERENCE;
    case  DW_FORM_ref8:        return DW_FORM_CLASS_REFERENCE;
    case  DW_FORM_ref_udata:   return DW_FORM_CLASS_REFERENCE;
    case  DW_FORM_ref_sig8:    return DW_FORM_CLASS_REFERENCE;

    case  DW_FORM_flag:         return DW_FORM_CLASS_FLAG;
    case  DW_FORM_flag_present: return DW_FORM_CLASS_FLAG;

    case  DW_FORM_addrx:
    case  DW_FORM_addrx1:
    case  DW_FORM_addrx2:
    case  DW_FORM_addrx3:
    case  DW_FORM_addrx4:
        return DW_FORM_CLASS_ADDRESS; /* DWARF5 */
    case  DW_FORM_GNU_addr_index:  return DW_FORM_CLASS_ADDRESS;
    case  DW_FORM_strx:  /* DWARF5 */
    case  DW_FORM_strx1: /* DWARF5 */
    case  DW_FORM_strx2: /* DWARF5 */
    case  DW_FORM_strx3: /* DWARF5 */
    case  DW_FORM_line_strp: /* DWARF5 */
    case  DW_FORM_strp_sup:  /* DWARF5 */
    case  DW_FORM_GNU_strp_alt:
        return DW_FORM_CLASS_STRING;
    case  DW_FORM_GNU_str_index:   return DW_FORM_CLASS_STRING;

    case  DW_FORM_rnglistx:
        return DW_FORM_CLASS_RNGLIST;    /* DWARF5 */
    case  DW_FORM_loclistx:
        return DW_FORM_CLASS_LOCLIST;    /* DWARF5 */

    case  DW_FORM_GNU_ref_alt:  return DW_FORM_CLASS_REFERENCE;
    case  DW_FORM_implicit_const:
        return DW_FORM_CLASS_CONSTANT; /* DWARF5 */

    case  DW_FORM_indirect:
    default:
        break;
    };
    return DW_FORM_CLASS_UNKNOWN;
}

/*  Given a DIE, figure out what the CU's DWARF version is
    and the size of an offset
    and return it through the *version pointer and return
    DW_DLV_OK.

    If we cannot find a CU,
        return DW_DLV_ERROR on error.
        In case of error no Dwarf_Debug was available,
        so setting a Dwarf_Error is somewhat futile.
    Never returns DW_DLV_NO_ENTRY.
*/
int
dwarf_get_version_of_die(Dwarf_Die die,
    Dwarf_Half *version,
    Dwarf_Half *offset_size)
{
    Dwarf_CU_Context cucontext = 0;
    if (!die) {
        return DW_DLV_ERROR;
    }
    cucontext = die->di_cu_context;
    if (!cucontext) {
        return DW_DLV_ERROR;
    }
    *version = cucontext->cc_version_stamp;
    *offset_size = cucontext->cc_length_size;
    return DW_DLV_OK;
}

Dwarf_Byte_Ptr
_dwarf_calculate_info_section_start_ptr(Dwarf_CU_Context context,
    Dwarf_Unsigned *section_len)
{
    Dwarf_Debug dbg = 0;
    Dwarf_Small *dataptr = 0;
    struct Dwarf_Section_s *sec = 0;

    dbg = context->cc_dbg;
    sec = context->cc_is_info? &dbg->de_debug_info:
        &dbg->de_debug_types;
    dataptr = sec->dss_data;
    *section_len = sec->dss_size;
    return dataptr;
}

Dwarf_Byte_Ptr
_dwarf_calculate_info_section_end_ptr(Dwarf_CU_Context context)
{
    Dwarf_Debug    dbg = 0;
    Dwarf_Byte_Ptr info_end = 0;
    Dwarf_Byte_Ptr info_start = 0;
    Dwarf_Off      off2 = 0;
    Dwarf_Small   *dataptr = 0;

    dbg = context->cc_dbg;
    dataptr = context->cc_is_info? dbg->de_debug_info.dss_data:
        dbg->de_debug_types.dss_data;
    off2 = context->cc_debug_offset;
    info_start = dataptr + off2;
    info_end = info_start + context->cc_length +
        context->cc_length_size +
        context->cc_extension_size;
    return info_end;
}
Dwarf_Byte_Ptr
_dwarf_calculate_abbrev_section_end_ptr(Dwarf_CU_Context context)
{
    Dwarf_Debug dbg = 0;
    Dwarf_Byte_Ptr abbrev_end = 0;
    Dwarf_Byte_Ptr abbrev_start = 0;
    struct Dwarf_Section_s *sec = 0;

    dbg = context->cc_dbg;
    sec = &dbg->de_debug_abbrev;
    abbrev_start = sec->dss_data;
    abbrev_end = abbrev_start + sec->dss_size;
    return abbrev_end;
}

/*  New December 2020.  Any Dwarf_Die will work.
    The values returned are about the CU itself, not a DIE.
    extension_size is set zero unless it offset_size
    is 64 and it is standard Dwarf, in which case
    extension_size is set to 4.
    If there is no signature *signature is set zero,
    offset_of_length is the section offset of the first
    byte of the compilation-unit length field.
    total_byte_length includes the length field and
    all the CU data.
    The offset of the first byte of the CU is therefore
    offset_of_lenth + offset_size + extension_size.
    is_info is always non-zero except if the section
    of the CU is DWARF4 .debug_types.
*/
int
dwarf_cu_header_basics(Dwarf_Die die,
    Dwarf_Half *version,
    Dwarf_Bool *is_info,
    Dwarf_Bool *is_dwo,
    Dwarf_Half *offset_size,
    Dwarf_Half *address_size,
    Dwarf_Half *extension_size,
    Dwarf_Sig8 **signature,
    Dwarf_Off  *offset_of_length,
    Dwarf_Unsigned  *total_byte_length,
    Dwarf_Error *error)
{
    Dwarf_CU_Context context = 0;
    CHECK_DIE(die, DW_DLV_ERROR);

    context= die->di_cu_context;
    if (version) {
        *version = context->cc_version_stamp;
    }
    if (is_info) {
        /*  ASSERT: matches context->cc_is_info */
        *is_info = die->di_is_info;
    }
    if (is_dwo) {
        *is_dwo = context->cc_is_dwo;
    }
    if (offset_size) {
        *offset_size = context->cc_length_size;
    }
    if (address_size) {
        *address_size = context->cc_address_size;
    }
    if (extension_size) {
        *extension_size = context->cc_extension_size;
    }
    if (signature) {
        if (context->cc_signature_present) {
            *signature =  &context->cc_signature;
        } else {
            *signature = 0;
        }
    }
    if (offset_of_length) {
        *offset_of_length = context->cc_debug_offset;
    }
    if (total_byte_length) {
        *total_byte_length = context->cc_length +
            context->cc_length_size + context->cc_extension_size;
    }
    return DW_DLV_OK;
}

int
dwarf_get_universalbinary_count(
    Dwarf_Debug dbg,
    Dwarf_Unsigned *current_index,
    Dwarf_Unsigned *available_count)
{
    if (IS_INVALID_DBG(dbg)) {
        return DW_DLV_NO_ENTRY;
    }
    if (!dbg->de_universalbinary_count ) {
        return DW_DLV_NO_ENTRY;
    }
    if (current_index) {
        *current_index = dbg->de_universalbinary_index;
    }
    if (available_count) {
        *available_count = dbg->de_universalbinary_count;
    }
    return DW_DLV_OK;
}

/*  Never returns DW_DLV_ERROR */
int
dwarf_machine_architecture_a(Dwarf_Debug dbg,
    Dwarf_Small    *dw_ftype,
    Dwarf_Small    *dw_obj_pointersize,
    Dwarf_Bool     *dw_obj_is_big_endian,
    Dwarf_Unsigned *dw_obj_machine,
    Dwarf_Unsigned *dw_obj_type,
    Dwarf_Unsigned *dw_obj_flags,
    Dwarf_Small    *dw_path_source,
    Dwarf_Unsigned *dw_ub_offset,
    Dwarf_Unsigned *dw_ub_count,
    Dwarf_Unsigned *dw_ub_index,
    Dwarf_Unsigned *dw_comdat_groupnumber)
{
    if (IS_INVALID_DBG(dbg)) {
        return DW_DLV_NO_ENTRY;
    }
    if (dw_ftype) {
        *dw_ftype = dbg->de_ftype;
    }
    if (dw_obj_pointersize) {
        *dw_obj_pointersize = dbg->de_pointer_size;
    }
    if (dw_obj_is_big_endian) {
        *dw_obj_is_big_endian = dbg->de_big_endian_object;
    }
    if (dw_obj_machine) {
        *dw_obj_machine = dbg->de_obj_machine;
    }
    if (dw_obj_type) {
        *dw_obj_type = dbg->de_obj_type;
    }
    if (dw_obj_flags) {
        *dw_obj_flags = dbg->de_obj_flags;
    }
    if (dw_path_source) {
        *dw_path_source = dbg->de_path_source;
    }
    if (dw_ub_offset) {
        *dw_ub_offset = dbg->de_obj_ub_offset;
    }
    if (dw_ub_count) {
        *dw_ub_count = dbg->de_universalbinary_count;
    }
    if (dw_ub_index) {
        *dw_ub_index = dbg->de_universalbinary_index;
    }
    if (dw_comdat_groupnumber) {
        *dw_comdat_groupnumber = dbg->de_groupnumber;
    }
    return DW_DLV_OK;
}
int
dwarf_machine_architecture(Dwarf_Debug dbg,
    Dwarf_Small    *dw_ftype,
    Dwarf_Small    *dw_obj_pointersize,
    Dwarf_Bool     *dw_obj_is_big_endian,
    Dwarf_Unsigned *dw_obj_machine,
    Dwarf_Unsigned *dw_obj_flags,
    Dwarf_Small    *dw_path_source,
    Dwarf_Unsigned *dw_ub_offset,
    Dwarf_Unsigned *dw_ub_count,
    Dwarf_Unsigned *dw_ub_index,
    Dwarf_Unsigned *dw_comdat_groupnumber)
{
    return dwarf_machine_architecture_a(dbg,
        dw_ftype,dw_obj_pointersize,
        dw_obj_is_big_endian,
        dw_obj_machine,
        0 /* Ignoring Elf e_type */ ,
        dw_obj_flags, dw_path_source,
        dw_ub_offset, dw_ub_count,
        dw_ub_index,  dw_comdat_groupnumber);
}
/*

    DWARF6 DW_LNAME values are referenced
    by using dwarf_language_version_string()
    (these are permitted in DWARF5, see
    www.dwarfstd.org  )

    For DWARF5 DW_LANG values, call dwarf_srclang()
    instead.

*/

int dwarf_language_version_data(
    Dwarf_Unsigned dw_lang_name,
    int *          dw_default_lower_bound,
    const char   **dw_version_scheme)
{
    const char *lname = 0;
    int res = 0;
    unsigned int ui = 0;
    unsigned int unreasonable_name = 65535;

    if (dw_lang_name >= unreasonable_name) {
        /*  Want to deal with improper code calling here,
            not just trim off upper bits in the cast.  */
        return DW_DLV_NO_ENTRY;
    }
    ui = (unsigned int)dw_lang_name;
    res = dwarf_get_LNAME_name(ui,&lname);
    if (res == DW_DLV_NO_ENTRY) {
        return res;
    }
    switch(dw_lang_name) {
    case DW_LNAME_Ada:
    case DW_LNAME_Cobol:
    case DW_LNAME_Fortran:
    case DW_LNAME_Pascal:
    case DW_LNAME_Algol68:
        *dw_default_lower_bound = 1;
        *dw_version_scheme = "YYYY";
        break;
    case DW_LNAME_BLISS:
    case DW_LNAME_Crystal:
    case DW_LNAME_D :
    case DW_LNAME_Dylan:
    case DW_LNAME_Go :
    case DW_LNAME_Haskell:
    case DW_LNAME_Java:
    case DW_LNAME_Kotlin:
    case DW_LNAME_OCaml:
    case DW_LNAME_OpenCL_C:
    case DW_LNAME_Python:
    case DW_LNAME_RenderScript:
    case DW_LNAME_Rust:
    case DW_LNAME_UPC:
    case DW_LNAME_Zig:
    case DW_LNAME_Assembly:
    case DW_LNAME_C_sharp:
    case DW_LNAME_Mojo:
    case DW_LNAME_Hylo:
    case DW_LNAME_HIP:
        *dw_default_lower_bound = 0;
        *dw_version_scheme = 0;
        break;
    case DW_LNAME_C:
    case DW_LNAME_C_plus_plus:
    case DW_LNAME_ObjC:
    case DW_LNAME_ObjC_plus_plus:
    case DW_LNAME_Move:
    case DW_LNAME_Odin:
        *dw_default_lower_bound = 0;
        *dw_version_scheme = "YYYYMM";
        break;
    case DW_LNAME_Julia:
    case DW_LNAME_Modula2:
    case DW_LNAME_Modula3:
    case DW_LNAME_PLI:
        *dw_default_lower_bound = 1;
        *dw_version_scheme = 0;
        break;
    case DW_LNAME_Swift:
    case DW_LNAME_OpenCL_CPP:
    case DW_LNAME_CPP_for_OpenCL:
        *dw_default_lower_bound = 0;
        *dw_version_scheme = "VVMM";
        break;
    case DW_LNAME_GLSL :
    case DW_LNAME_GLSLES:
    case DW_LNAME_Ruby:
    case DW_LNAME_P4:
    case DW_LNAME_Metal:
    case DW_LNAME_V:
    case DW_LNAME_Nim:
        *dw_default_lower_bound = 0;
        *dw_version_scheme = "VVMMPP";
        break;
    case DW_LNAME_HLSL:
        *dw_default_lower_bound = 0;
        *dw_version_scheme = "YYYY";
        break;
    case DW_LNAME_SYCL:
        *dw_default_lower_bound = 0;
        *dw_version_scheme = "YYYYRR";
        break;
    default:
        return DW_DLV_NO_ENTRY;
    }
    return DW_DLV_OK;
}
/*  OBSOLETE NAME:  Do not use dwarf_language_version_string(). */
int dwarf_language_version_string(
    Dwarf_Unsigned dw_lang_name,
    int *          dw_default_lower_bound,
    const char   **dw_version_scheme)
{
    return dwarf_language_version_data(dw_lang_name,
        dw_default_lower_bound,
        dw_version_scheme);
}
