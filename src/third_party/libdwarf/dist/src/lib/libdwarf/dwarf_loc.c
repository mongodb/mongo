/*
  Copyright (C) 2000-2004 Silicon Graphics, Inc.  All Rights Reserved.
  Portions Copyright (C) 2007-2018 David Anderson. All Rights Reserved.
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

#include <string.h> /* memset() */
#include <stdio.h> /* for debugging printf */

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
#include "dwarf_loc.h"
#include "dwarf_string.h"

#define DEBUG_LOCLIST 1
#undef DEBUG_LOCLIST

static int _dwarf_read_loc_section_dwo(Dwarf_Debug dbg,
    Dwarf_Block_c * return_block,
    Dwarf_Addr * lowpc,
    Dwarf_Addr * highpc,
    Dwarf_Bool * at_end,
    Dwarf_Half * lle_op,
    Dwarf_Off    sec_offset,
    Dwarf_Half   address_size,
    Dwarf_Half   lkind,
    Dwarf_Error *error);

/*  Used to enable sanity checking of these data
    items before we return to caller. */
int
_dwarf_locdesc_c_constructor(Dwarf_Debug dbg, void *locd)
{
    Dwarf_Locdesc_c  ldp = (Dwarf_Locdesc_c)locd;

    if (IS_INVALID_DBG(dbg)) {
        return DW_DLV_ERROR;
    }
    ldp->ld_lle_value = DW_LLE_VALUE_BOGUS;
    ldp->ld_lkind = DW_LKIND_unknown;
    return DW_DLV_OK;
}

static void
_dwarf_lkind_name(unsigned lkind, dwarfstring *m)
{
    switch(lkind) {
    case DW_LKIND_expression:
        dwarfstring_append(m,"DW_LKIND_expression");
        return;
    case DW_LKIND_loclist:
        dwarfstring_append(m,"DW_LKIND_loclist");
        return;
    case DW_LKIND_GNU_exp_list:
        dwarfstring_append(m,"DW_LKIND_GNU_exp_list");
        return;
    case DW_LKIND_loclists:
        dwarfstring_append(m,"DW_LKIND_loclists");
        return;
    case DW_LKIND_unknown:
        dwarfstring_append(m,"DW_LKIND_unknown");
        return;
    default: break;
    }
    dwarfstring_append_printf_u(m,
        "<DW_LKIND location kind is unknown and has value %u>.",
        lkind);
}

static int
determine_location_lkind(unsigned int version,
    unsigned int form,
    Dwarf_Bool is_dwo)
{
    switch(form) {
    case DW_FORM_exprloc: /* only defined for
        DW_CFA_def_cfa_expression */
    case DW_FORM_block:
    case DW_FORM_block1:
    case DW_FORM_block2:
    case DW_FORM_block4:
        return DW_LKIND_expression;
        break;
    case DW_FORM_data4:
    case DW_FORM_data8:
        if (version > 1 && version < 4) {
            return DW_LKIND_loclist;
        }
        break;
    case DW_FORM_sec_offset:
        if (version == 5 ) {
            return DW_LKIND_loclists;
        }
        if (version == 4 &&  is_dwo  ) {
            return DW_LKIND_GNU_exp_list;
        }
        return DW_LKIND_loclist;
        break;
    case DW_FORM_loclistx:
        if (version == 5 ) {
            return DW_LKIND_loclists;
        }
        break;
    default:
        break;
    }
    return DW_LKIND_unknown;
}

static void
_dwarf_free_op_chain(Dwarf_Debug dbg,
    Dwarf_Loc_Chain headloc)
{
    Dwarf_Loc_Chain cur = headloc;

    while (cur) {
        Dwarf_Loc_Chain next = cur->lc_next;
        dwarf_dealloc(dbg, cur, DW_DLA_LOC_CHAIN);
        cur = next;
    }
}

/*  Using a loclist offset to get the in-memory
    address of .debug_loc data to read, returns the loclist
    'header' info in return_block.
*/

#define MAX_ADDR \
    ((address_size == 8)?0xffffffffffffffffULL:0xffffffff)

static int
_dwarf_read_loc_section(Dwarf_Debug dbg,
    Dwarf_Block_c * return_block,
    Dwarf_Addr    * lowpc,
    Dwarf_Addr    * hipc,
    Dwarf_Half    * lle_val,
    Dwarf_Off       sec_offset,
    Dwarf_Half      address_size,
    Dwarf_Error   * error)
{
    Dwarf_Small *beg = dbg->de_debug_loc.dss_data + sec_offset;
    Dwarf_Small *loc_section_end =
        dbg->de_debug_loc.dss_data + dbg->de_debug_loc.dss_size;

    /*  start_addr and end_addr are actually offsets
        of the applicable base address of the CU.
        They are address-size. */
    Dwarf_Addr start_addr = 0;
    Dwarf_Addr end_addr = 0;
    Dwarf_Half exprblock_size = 0;
    Dwarf_Unsigned exprblock_off =
        2 * address_size + DWARF_HALF_SIZE;

    if (sec_offset >= dbg->de_debug_loc.dss_size) {
        /* We're at the end. No more present. */
        return DW_DLV_NO_ENTRY;
    }

    /* If it goes past end, error */
    if (exprblock_off > dbg->de_debug_loc.dss_size) {
        _dwarf_error(dbg, error, DW_DLE_DEBUG_LOC_SECTION_SHORT);
        return DW_DLV_ERROR;
    }

    READ_UNALIGNED_CK(dbg, start_addr, Dwarf_Addr, beg, address_size,
        error,loc_section_end);
    READ_UNALIGNED_CK(dbg, end_addr, Dwarf_Addr,
        beg + address_size, address_size,
        error,loc_section_end);
    if (start_addr == 0 && end_addr == 0) {
        /*  If start_addr and end_addr are 0, it's the end and no
            exprblock_size field follows. */
        exprblock_size = 0;
        exprblock_off -= DWARF_HALF_SIZE;
        *lle_val = DW_LLE_end_of_list;
    } else if (start_addr == MAX_ADDR) {
        /*  End address is a base address,
            no exprblock_size field here either */
        exprblock_size = 0;
        exprblock_off -=  DWARF_HALF_SIZE;
        *lle_val = DW_LLE_base_address;
    } else {
        /*  Here we note the address and length of the
            expression operators, DW_OP_reg0 etc */
        READ_UNALIGNED_CK(dbg, exprblock_size, Dwarf_Half,
            beg + 2 * address_size, DWARF_HALF_SIZE,
            error,loc_section_end);
        /* exprblock_size can be zero, means no expression */
        if ( exprblock_size >= dbg->de_debug_loc.dss_size) {
            _dwarf_error(dbg, error, DW_DLE_DEBUG_LOC_SECTION_SHORT);
            return DW_DLV_ERROR;
        }
        if ((sec_offset +exprblock_off + exprblock_size) >
            dbg->de_debug_loc.dss_size) {
            _dwarf_error(dbg, error, DW_DLE_DEBUG_LOC_SECTION_SHORT);
            return DW_DLV_ERROR;
        }
        *lle_val = DW_LLE_start_end;
    }
    *lowpc = start_addr;
    *hipc = end_addr;

    return_block->bl_len = exprblock_size;
    return_block->bl_kind = DW_LKIND_loclist;
    return_block->bl_data = beg + exprblock_off;
    return_block->bl_section_offset =
        ((Dwarf_Small *) return_block->bl_data) -
        dbg->de_debug_loc.dss_data;
    return DW_DLV_OK;
}

static int
_dwarf_get_loclist_lle_count_dwo(Dwarf_Debug dbg,
    Dwarf_Off loclist_offset,
    Dwarf_Half address_size,
    Dwarf_Half lkind,
    int *loclist_count,
    Dwarf_Error * error)
{
    int count = 0;
    Dwarf_Off offset = loclist_offset;

    for (;;) {
        Dwarf_Block_c b;
        Dwarf_Bool at_end = FALSE;
        Dwarf_Addr lowpc = 0;
        Dwarf_Addr highpc = 0;
        Dwarf_Half lle_op = 0;
        int res = _dwarf_read_loc_section_dwo(dbg, &b,
            &lowpc,
            &highpc,
            &at_end,
            &lle_op,
            offset,
            address_size,
            lkind,
            error);
        if (res != DW_DLV_OK) {
            return res;
        }
        if (at_end) {
            count++;
            break;
        }
        offset = b.bl_len + b.bl_section_offset;
        count++;
    }
    *loclist_count = count;
    return DW_DLV_OK;
}

static int
_dwarf_get_loclist_lle_count(Dwarf_Debug dbg,
    Dwarf_Off loclist_offset,
    Dwarf_Half address_size,
    int *loclist_count,
    Dwarf_Error * error)
{
    int count = 0;
    Dwarf_Off offset = loclist_offset;

    for (;;) {
        Dwarf_Block_c b;
        Dwarf_Addr lowpc = 0;
        Dwarf_Addr highpc = 0;
        Dwarf_Half lle_val = DW_LLE_VALUE_BOGUS;

        int res = _dwarf_read_loc_section(dbg, &b,
            &lowpc, &highpc,
            &lle_val,
            offset, address_size,error);
        if (res != DW_DLV_OK) {
            return res;
        }
        offset = b.bl_len + b.bl_section_offset;
        if (lowpc == 0 && highpc == 0) {
            break;
        }
        count++;
    }
    *loclist_count = count;
    return DW_DLV_OK;
}

/* Helper routine to avoid code duplication.
*/
static int
_dwarf_setup_loc(Dwarf_Attribute attr,
    Dwarf_Debug *     dbg_ret,
    Dwarf_CU_Context *cucontext_ret,
    Dwarf_Half       *form_ret,
    Dwarf_Error      *error)
{
    Dwarf_Debug dbg = 0;
    Dwarf_Half form = 0;
    int blkres = DW_DLV_ERROR;

    /*  Creating an error with NULL dbg is not a good thing.
        These won't be freed if we later call dealloc
        with a non-NULL dbg.
    */
    if (!attr) {
        _dwarf_error_string(NULL, error, DW_DLE_ATTR_NULL,
            "DW_DLE_ATTR_NULL: the attribute passed to "
            "dwarf_get_loclist_c() is a NULL pointer");
        return DW_DLV_ERROR;
    }
    if (attr->ar_cu_context == NULL) {
        _dwarf_error(NULL, error, DW_DLE_ATTR_NO_CU_CONTEXT);
        return DW_DLV_ERROR;
    }
    *cucontext_ret = attr->ar_cu_context;

    dbg = attr->ar_cu_context->cc_dbg;
    CHECK_DBG(dbg,error,"_dwarf_get_loclist_lle_count()");
    *dbg_ret = dbg;
    blkres = dwarf_whatform(attr, &form, error);
    if (blkres != DW_DLV_OK) {
        return blkres;
    }
    *form_ret = form;
    return DW_DLV_OK;
}

/* Helper routine  to avoid code duplication.
*/
static int
_dwarf_get_loclist_header_start(Dwarf_Debug dbg,
    Dwarf_Attribute attr,
    Dwarf_Unsigned * loclist_offset_out,
    Dwarf_Error * error)
{
    Dwarf_Unsigned loc_sec_size = 0;
    Dwarf_Unsigned loclist_offset = 0;

    int blkres = dwarf_global_formref(attr, &loclist_offset, error);
    if (blkres != DW_DLV_OK) {
        return blkres;
    }
    if (!dbg->de_debug_loc.dss_data) {
        int secload = _dwarf_load_section(dbg,
            &dbg->de_debug_loc,error);
        if (secload != DW_DLV_OK) {
            return secload;
        }
        if (!dbg->de_debug_loc.dss_size) {
            return DW_DLV_NO_ENTRY;
        }
    }
    loc_sec_size = dbg->de_debug_loc.dss_size;
    if (loclist_offset >= loc_sec_size) {
        _dwarf_error(dbg, error, DW_DLE_LOCLIST_OFFSET_BAD);
        return DW_DLV_ERROR;
    }

    {
        int fisres = 0;
        Dwarf_Unsigned fissoff = 0;
        Dwarf_Unsigned size = 0;
        fisres = _dwarf_get_fission_addition_die(attr->ar_die,
            DW_SECT_LOCLISTS,
            &fissoff, &size,error);
        if (fisres != DW_DLV_OK) {
            return fisres;
        }
        if (fissoff >= loc_sec_size) {
            _dwarf_error(dbg, error, DW_DLE_LOCLIST_OFFSET_BAD);
            return DW_DLV_ERROR;
        }
        loclist_offset += fissoff;
        if (loclist_offset >= loc_sec_size) {
            _dwarf_error(dbg, error, DW_DLE_LOCLIST_OFFSET_BAD);
            return DW_DLV_ERROR;
        }
    }
    *loclist_offset_out = loclist_offset;
    return DW_DLV_OK;
}

static int
context_is_cu_not_tu(Dwarf_CU_Context context,
    Dwarf_Bool *r)
{
    int ut = context->cc_unit_type;

    if (ut == DW_UT_type || ut == DW_UT_split_type ) {
        *r =FALSE;
        return DW_DLV_OK;
    }
    *r = TRUE;
    return DW_DLV_OK;
}

/*  Handles only a location expression.
    It returns the location expression as a loclist with
    a single entry.

    Usable to access dwarf expressions from any source, but
    specifically from
        DW_CFA_def_cfa_expression
        DW_CFA_expression
        DW_CFA_val_expression
    expression_in must point to a valid dwarf expression
*/

/* ============== the October 2015 interfaces. */
int
_dwarf_loc_block_sanity_check(Dwarf_Debug dbg,
    Dwarf_Block_c *loc_block,Dwarf_Error* error)
{
    unsigned lkind = loc_block->bl_kind;
    if (lkind == DW_LKIND_loclist) {
        Dwarf_Small *loc_ptr = 0;
        Dwarf_Unsigned loc_len = 0;
        Dwarf_Small *end_ptr = 0;

        loc_ptr = loc_block->bl_data;
        loc_len = loc_block->bl_len;
        end_ptr =  dbg->de_debug_loc.dss_size +
            dbg->de_debug_loc.dss_data;
        if ((loc_ptr +loc_len) > end_ptr) {
            dwarfstring m;

            dwarfstring_constructor(&m);
            dwarfstring_append(&m,
                "DW_DLE_DEBUG_LOC_SECTION_SHORT kind: ");
            _dwarf_lkind_name(lkind, &m);
            _dwarf_error_string(dbg,error,
                DW_DLE_DEBUG_LOC_SECTION_SHORT,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
        return DW_DLV_OK;
    }
    if (lkind == DW_LKIND_loclists) {
        Dwarf_Small *loc_ptr = 0;
        Dwarf_Unsigned loc_len = 0;
        Dwarf_Small *end_ptr = 0;

        loc_ptr = loc_block->bl_data;
        loc_len = loc_block->bl_len;
        end_ptr =  dbg->de_debug_loclists.dss_size +
            dbg->de_debug_loclists.dss_data;
        if ((loc_ptr +loc_len) > end_ptr) {
            dwarfstring m;

            dwarfstring_constructor(&m);
            dwarfstring_append(&m,
                "DW_DLE_DEBUG_LOC_SECTION_SHORT "
                "(the .debug_loclists section is short), kind: ");
            _dwarf_lkind_name(lkind, &m);
            _dwarf_error_string(dbg,error,
                DW_DLE_DEBUG_LOC_SECTION_SHORT,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
    }
    return DW_DLV_OK;
}

/*  ld_lkind was checked before calling this, so we
    know its value is an intended value.  */
static const char *kindset[] = {
"DW_LKIND_expression",
"DW_LKIND_loclist",
"DW_LKIND_GNU_exp_list",
"DW_LKIND_unknown3",
"DW_LKIND_unknown4",
"DW_LKIND_loclists"
};
static const char *
get_loc_kind_str(Dwarf_Small lkind)
{
    if (lkind <= DW_LKIND_loclists) {
        return kindset[lkind];
    }
    if (lkind == DW_LKIND_unknown) {
        return "DW_LKIND_unknown";
    }
    return "UNKNOWN DW_LKIND!";
}
static int
validate_lle_value(Dwarf_Debug dbg,
    Dwarf_Locdesc_c locdesc,
    Dwarf_Error *error)
{
    dwarfstring m;

    if (locdesc->ld_lkind != DW_LKIND_GNU_exp_list) {
        switch(locdesc->ld_lle_value) {
        case DW_LLE_end_of_list:
        case DW_LLE_base_addressx:
        case DW_LLE_startx_endx:
        case DW_LLE_startx_length:
        case DW_LLE_offset_pair:
        case DW_LLE_default_location:
        case DW_LLE_base_address:
        case DW_LLE_start_end:
        case DW_LLE_start_length:
            return DW_DLV_OK;
        default: break;
        }
        dwarfstring_constructor(&m);

        dwarfstring_append_printf_s(&m,
            "DW_DLE_LOCATION_ERROR: For location kind %s (",
            (char *)get_loc_kind_str(
                (Dwarf_Small)locdesc->ld_lkind));
        dwarfstring_append_printf_u(&m,"%u) the DW_LLE value is "
            "not properly set",
            locdesc->ld_lkind);
        dwarfstring_append_printf_u(&m," but is %u "
            " which is a libdwarf bug",
            locdesc->ld_lle_value);
        _dwarf_error_string(dbg,error,DW_DLE_LOCATION_ERROR,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    switch(locdesc->ld_lle_value) {
    case DW_LLEX_end_of_list_entry:
    case DW_LLEX_base_address_selection_entry:
    case DW_LLEX_start_end_entry:
    case DW_LLEX_start_length_entry:
    case DW_LLEX_offset_pair_entry:
        return DW_DLV_OK;
    default: break; /* ERROR */
    }
    {
        dwarfstring_constructor(&m);
        dwarfstring_append_printf_s(&m,
            "DW_DLE_LOCATION_ERROR: For location kind %s (",
            (char *)get_loc_kind_str(
                (Dwarf_Small)locdesc->ld_lkind));
        dwarfstring_append_printf_u(&m,"%u) the DW_LLEX value is "
            "not properly set",
            locdesc->ld_lkind);
        dwarfstring_append_printf_u(&m," but is %u "
            " which is a libdwarf bug",
            locdesc->ld_lle_value);
        _dwarf_error_string(dbg,error,DW_DLE_LOCATION_ERROR,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
    }
    return DW_DLV_ERROR;
}
/*  Sets locdesc operator list information in locdesc.
    Sets the locdesc values (rawlow, rawhigh etc).
    This synthesizes the ld_lle_value of the locdesc
    if it's not already provided.
    Not passing in locdesc pointer, the locdesc_index suffices
    to index to the relevant locdesc pointer.
    See also dwarf_loclists.c: build_array_of_lle*/
int
_dwarf_fill_in_locdesc_op_c(Dwarf_Debug dbg,
    Dwarf_Unsigned locdesc_index,
    Dwarf_Loc_Head_c loc_head,
    Dwarf_Block_c * loc_block,
    Dwarf_Half address_size,
    Dwarf_Half offset_size,
    Dwarf_Half version_stamp,
    Dwarf_Addr lowpc,
    Dwarf_Addr highpc,
    Dwarf_Half lle_op,
    Dwarf_Error * error)
{
    /* Offset of current operator from start of block. */
    Dwarf_Unsigned offset = 0;

    /*  Chain the  DW_OPerator structs. */
    Dwarf_Loc_Chain new_loc = NULL;
    Dwarf_Loc_Chain prev_loc = NULL;
    Dwarf_Loc_Chain head_loc = NULL;
    Dwarf_Loc_Chain *plast = &head_loc;

    Dwarf_Unsigned  op_count = 0;

    /*  Contiguous block of Dwarf_Loc_Expr_Op_s
        for Dwarf_Locdesc. */
    Dwarf_Loc_Expr_Op block_loc = 0;

    Dwarf_Locdesc_c locdesc = loc_head->ll_locdesc + locdesc_index;
    Dwarf_Unsigned  i = 0;
    int             res = 0;
    Dwarf_Small    *section_start = 0;
    Dwarf_Unsigned  section_size = 0;
    Dwarf_Small    *section_end = 0;
    const char     *section_name = 0;
    Dwarf_Small    *blockdataptr = 0;
    unsigned lkind = loc_head->ll_lkind;

    /* ***** BEGIN CODE ***** */
    blockdataptr = loc_block->bl_data;
    if (!blockdataptr || !loc_block->bl_len) {
        /*  an empty block has no operations so
            no section or tests need be done.. */
    } else {
        res = _dwarf_what_section_are_we(dbg,
            blockdataptr,&section_name,&section_start,
            &section_size,&section_end);
        if (res != DW_DLV_OK) {
            _dwarf_error(dbg, error,DW_DLE_POINTER_SECTION_UNKNOWN);
            return DW_DLV_ERROR;
        }
        res = _dwarf_loc_block_sanity_check(dbg,loc_block,error);
        if (res != DW_DLV_OK) {
            return res;
        }
    }
    /* New loop getting Loc operators. Non DWO */
    while (offset <= loc_block->bl_len) {
        Dwarf_Unsigned nextoffset = 0;
        struct Dwarf_Loc_Expr_Op_s temp_loc;
        /*  This call is ok even if bl_data NULL and bl_len 0 */
        res = _dwarf_read_loc_expr_op(dbg,loc_block,
            op_count,
            version_stamp,
            offset_size,
            address_size,
            offset,
            section_end,
            &nextoffset,
            &temp_loc,
            error);
        if (res == DW_DLV_ERROR) {
            _dwarf_free_op_chain(dbg,head_loc);
            return res;
        }
        if (res == DW_DLV_NO_ENTRY) {
            /*  Normal end.
                Also the end for an empty loc_block.  */
            break;
        }
        op_count++;
        if (op_count > loc_block->bl_len) {
            _dwarf_free_op_chain(dbg,head_loc);
            _dwarf_error_string(dbg, error, DW_DLE_LOCATION_ERROR,
                "DW_DLE_LOCATION_ERROR:  We have counted more"
                " operators in a location expression than "
                "there are bytes in the block. Corrupt DWARF");
            return DW_DLV_ERROR;
        }
        new_loc = (Dwarf_Loc_Chain) _dwarf_get_alloc(dbg,
            DW_DLA_LOC_CHAIN, 1);
        if (new_loc == NULL) {
            _dwarf_free_op_chain(dbg,head_loc);
            _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
            return DW_DLV_ERROR;
        }

        /* Copying all the fields. DWARF 2,3,4,5. */
        new_loc->lc_atom    = temp_loc.lr_atom;
        new_loc->lc_opnumber= temp_loc.lr_opnumber;
        new_loc->lc_number  = temp_loc.lr_number;
        new_loc->lc_number2 = temp_loc.lr_number2;
        new_loc->lc_number3 = temp_loc.lr_number3;
        new_loc->lc_offset  = temp_loc.lr_offset;
        *plast = new_loc;
        plast= &(new_loc->lc_next);
        offset = nextoffset;
    }
    block_loc =
        (Dwarf_Loc_Expr_Op ) _dwarf_get_alloc(dbg,
        DW_DLA_LOC_BLOCK_C, op_count);
    new_loc = head_loc;
    if (!block_loc) {
        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
        for (i = 0; i < op_count; i++) {
            prev_loc = new_loc;
            new_loc = prev_loc->lc_next;
            dwarf_dealloc(dbg, prev_loc, DW_DLA_LOC_CHAIN);
        }
        return DW_DLV_ERROR;
    }

    /* op_count could be zero. */
    new_loc = head_loc;
    for (i = 0; i < op_count; i++) {
        /* Copying only the fields needed by DWARF 2,3,4 */
        (block_loc + i)->lr_atom = new_loc->lc_atom;
        (block_loc + i)->lr_number = new_loc->lc_number;
        (block_loc + i)->lr_number2 = new_loc->lc_number2;
        (block_loc + i)->lr_number3 = new_loc->lc_number3;
        (block_loc + i)->lr_offset = new_loc->lc_offset;
        (block_loc + i)->lr_opnumber = new_loc->lc_opnumber;
        prev_loc = new_loc;
        new_loc = prev_loc->lc_next;
        dwarf_dealloc(dbg, prev_loc, DW_DLA_LOC_CHAIN);
    }
    /*  Synthesizing the DW_LLE values for the old loclist
        versions. */
    switch(loc_head->ll_lkind) {
    case DW_LKIND_loclist: {
        if (highpc == 0 && lowpc == 0) {
            locdesc->ld_lle_value =  DW_LLE_end_of_list;
        } else if (lowpc == MAX_ADDR) {
            locdesc->ld_lle_value = DW_LLE_base_address;
        } else {
            locdesc->ld_lle_value = DW_LLE_offset_pair;
        }
        }
        break;
    case DW_LKIND_GNU_exp_list:
        /* DW_LKIND_GNU_exp_list */
        locdesc->ld_lle_value = (Dwarf_Small)lle_op;
        break;
    case DW_LKIND_expression:
        /*  This is a kind of fake, but better than 0 */
        locdesc->ld_lle_value =  (Dwarf_Small)DW_LLE_start_end;
        break;
    case DW_LKIND_loclists:
        /* ld_lle_value already set */
        break;
    default:  {
        dwarfstring m;

        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            "DW_DLE_LOCATION_ERROR: An impossible DW_LKIND"
            " value of %u encountered, likely internal "
            "libdwarf error or data corruption",
            (unsigned)loc_head->ll_lkind);
        _dwarf_error_string(dbg,error,
            DW_DLE_LOCATION_ERROR,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        dwarf_dealloc(dbg,block_loc,DW_DLA_LOC_BLOCK_C);
        return DW_DLV_ERROR;
        }
    }
    locdesc->ld_cents = (Dwarf_Half)op_count;
    locdesc->ld_s = block_loc;

    locdesc->ld_lkind = lkind;
    locdesc->ld_section_offset = loc_block->bl_section_offset;
    locdesc->ld_locdesc_offset = loc_block->bl_locdesc_offset;
    locdesc->ld_rawlow = lowpc;
    locdesc->ld_rawhigh = highpc;

    res = validate_lle_value(dbg,locdesc,error);
    if (res != DW_DLV_OK) {
        dwarf_dealloc(dbg,block_loc,DW_DLA_LOC_BLOCK_C);
        locdesc->ld_s = 0;
        return res;
    }
    /*  Leaving the cooked values zero. Filled in later. */
    /*  We have not yet looked for debug_addr, so we'll
        set it as not-missing. */
    locdesc->ld_index_failed = FALSE;
    return DW_DLV_OK;
}

/* Non-standard DWARF4 dwo loclist */
static int
_dwarf_read_loc_section_dwo(Dwarf_Debug dbg,
    Dwarf_Block_c * return_block,
    Dwarf_Addr * lowpc,
    Dwarf_Addr * highpc,
    Dwarf_Bool *at_end,
    Dwarf_Half * lle_op,
    Dwarf_Off sec_offset,
    Dwarf_Half address_size,
    Dwarf_Half lkind,
    Dwarf_Error * error)
{
    Dwarf_Small *beg = dbg->de_debug_loc.dss_data + sec_offset;
    Dwarf_Small *locptr = 0;
    Dwarf_Small llecode = 0;
    Dwarf_Unsigned expr_offset  = sec_offset;
    Dwarf_Byte_Ptr section_end = dbg->de_debug_loc.dss_data
        + dbg->de_debug_loc.dss_size;

    if (sec_offset >= dbg->de_debug_loc.dss_size) {
        /* We're at the end. No more present. */
        return DW_DLV_NO_ENTRY;
    }
    memset(return_block,0,sizeof(*return_block));

    /* not the same as non-split loclist, but still a list. */
    return_block->bl_kind = lkind;

    /* This is non-standard  GNU Dwarf4 loclist */
    return_block->bl_locdesc_offset = sec_offset;
    llecode = *beg;
    locptr = beg +1;
    expr_offset++;
    switch(llecode) {
    case DW_LLEX_end_of_list_entry:
        *at_end = TRUE;
        return_block->bl_section_offset = expr_offset;
        expr_offset++;
        break;
    case DW_LLEX_base_address_selection_entry: {
        Dwarf_Unsigned addr_index = 0;

        DECODE_LEB128_UWORD_CK(locptr,addr_index,
            dbg,error,section_end);
        return_block->bl_section_offset = expr_offset;
        /* So this behaves much like non-dwo loclist */
        *lowpc=MAX_ADDR;
        *highpc=addr_index;
        }
        break;
    case DW_LLEX_start_end_entry: {
        Dwarf_Unsigned addr_indexs = 0;
        Dwarf_Unsigned addr_indexe= 0;
        Dwarf_Unsigned exprlen = 0;
        Dwarf_Unsigned leb128_length = 0;

        DECODE_LEB128_UWORD_LEN_CK(locptr,addr_indexs,
            leb128_length,
            dbg,error,section_end);
        expr_offset += leb128_length;

        DECODE_LEB128_UWORD_LEN_CK(locptr,addr_indexe,
            leb128_length,
            dbg,error,section_end);
        expr_offset +=leb128_length;

        *lowpc=addr_indexs;
        *highpc=addr_indexe;

        READ_UNALIGNED_CK(dbg, exprlen, Dwarf_Unsigned, locptr,
            DWARF_HALF_SIZE,
            error,section_end);
        locptr += DWARF_HALF_SIZE;
        expr_offset += DWARF_HALF_SIZE;

        return_block->bl_len = exprlen;
        return_block->bl_data = locptr;
        return_block->bl_section_offset = expr_offset;

        expr_offset += exprlen;
        if (expr_offset > dbg->de_debug_loc.dss_size) {
            dwarfstring m;

            dwarfstring_constructor(&m);
            dwarfstring_append(&m,
                "DW_DLE_DEBUG_LOC_SECTION_SHORT:");
            dwarfstring_append_printf_u(&m,
                " in DW_LLEX_start_end_entry "
                "The expression offset is 0x%x",
                expr_offset);
            dwarfstring_append_printf_u(&m,
                " which is greater than the section size"
                " of 0x%x. Corrupt Dwarf.",
                dbg->de_debug_loc.dss_size);
            _dwarf_error_string(dbg,error,
                DW_DLE_DEBUG_LOC_SECTION_SHORT,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
        }
        break;
    case DW_LLEX_start_length_entry: {
        Dwarf_Unsigned addr_index = 0;
        Dwarf_Unsigned  range_length = 0;
        Dwarf_Unsigned exprlen = 0;
        Dwarf_Unsigned leb128_length = 0;

        DECODE_LEB128_UWORD_LEN_CK(locptr,addr_index,
            leb128_length,
            dbg,error,section_end);
        expr_offset +=leb128_length;

        READ_UNALIGNED_CK(dbg, range_length, Dwarf_Unsigned, locptr,
            DWARF_32BIT_SIZE,
            error,section_end);
        locptr += DWARF_32BIT_SIZE;
        expr_offset += DWARF_32BIT_SIZE;

        READ_UNALIGNED_CK(dbg, exprlen, Dwarf_Unsigned, locptr,
            DWARF_HALF_SIZE,
            error,section_end);
        locptr += DWARF_HALF_SIZE;
        expr_offset += DWARF_HALF_SIZE;

        *lowpc = addr_index;
        *highpc = range_length;
        return_block->bl_len = exprlen;
        return_block->bl_data = locptr;
        return_block->bl_section_offset = expr_offset;
        /* exprblock_size can be zero, means no expression */

        expr_offset += exprlen;
        if (expr_offset > dbg->de_debug_loc.dss_size) {
            dwarfstring m;

            dwarfstring_constructor(&m);
            dwarfstring_append(&m,
                "DW_DLE_DEBUG_LOC_SECTION_SHORT:");
            dwarfstring_append_printf_u(&m,
                " in DW_LLEX_start_length_entry "
                "The expression offset is 0x%x",
                expr_offset);
            dwarfstring_append_printf_u(&m,
                " which is greater than the section size"
                " of 0x%x. Corrupt Dwarf.",
                dbg->de_debug_loc.dss_size);
            _dwarf_error_string(dbg,error,
                DW_DLE_DEBUG_LOC_SECTION_SHORT,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
        }
        break;
    case DW_LLEX_offset_pair_entry: {
        Dwarf_Unsigned  startoffset = 0;
        Dwarf_Unsigned  endoffset = 0;
        Dwarf_Unsigned exprlen = 0;

        READ_UNALIGNED_CK(dbg, startoffset,
            Dwarf_Unsigned, locptr,
            DWARF_32BIT_SIZE,
            error,section_end);
        locptr += DWARF_32BIT_SIZE;
        expr_offset += DWARF_32BIT_SIZE;

        READ_UNALIGNED_CK(dbg, endoffset,
            Dwarf_Unsigned, locptr,
            DWARF_32BIT_SIZE,
            error,section_end);
        locptr += DWARF_32BIT_SIZE;
        expr_offset +=  DWARF_32BIT_SIZE;
        *lowpc= startoffset;
        *highpc = endoffset;

        READ_UNALIGNED_CK(dbg, exprlen, Dwarf_Unsigned, locptr,
            DWARF_HALF_SIZE,
            error,section_end);
        locptr += DWARF_HALF_SIZE;
        expr_offset += DWARF_HALF_SIZE;

        return_block->bl_len = exprlen;
        return_block->bl_data = locptr;
        return_block->bl_section_offset = expr_offset;

        expr_offset += exprlen;
        if (expr_offset > dbg->de_debug_loc.dss_size) {
            dwarfstring m;

            dwarfstring_constructor(&m);
            dwarfstring_append(&m,
                "DW_DLE_DEBUG_LOC_SECTION_SHORT:");
            dwarfstring_append_printf_u(&m,
                " in DW_LLEX_offset_pair_entry "
                "The expression offset is 0x%x",
                expr_offset);
            dwarfstring_append_printf_u(&m,
                " which is greater than the section size"
                " of 0x%x. Corrupt Dwarf.",
                dbg->de_debug_loc.dss_size);
            _dwarf_error_string(dbg,error,
                DW_DLE_DEBUG_LOC_SECTION_SHORT,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
        }
        break;
    default: {
        dwarfstring m;

        dwarfstring_constructor(&m);
        dwarfstring_append(&m,
            "DW_DLE_LLE_CODE_UNKNOWN:");
        dwarfstring_append_printf_u(&m,
            " in DW_LLEX_ code value "
            " is 0x%x ,not an expected value.",
            llecode);
        _dwarf_error_string(dbg,error,
            DW_DLE_LLE_CODE_UNKNOWN,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    }
    *lle_op = llecode;
    return DW_DLV_OK;
}

int
dwarf_get_loclist_head_kind(Dwarf_Loc_Head_c ll_header,
    unsigned int * kind,
    Dwarf_Error  * error)
{
    if (!ll_header) {
        _dwarf_error_string(NULL, error,DW_DLE_DBG_NULL,
            "DW_DLE_DBG_NULL: "
            "NULL Dwarf_Loc_Head_c "
            "argument passed to "
            "dwarf_get_loclist_head_kind()");
        return DW_DLV_ERROR;
    }
    *kind = ll_header->ll_lkind;
    return DW_DLV_OK;
}

static int
_dwarf_original_loclist_build(Dwarf_Debug dbg,
    Dwarf_Loc_Head_c llhead,
    Dwarf_Attribute  attr,
    Dwarf_Error     *error)
{
    Dwarf_Unsigned loclist_offset = 0;
    Dwarf_Unsigned starting_loclist_offset = 0;
    int            off_res  = DW_DLV_ERROR;
    int            count_res = DW_DLV_ERROR;
    int            loclist_count = 0;
    Dwarf_Unsigned lli = 0;
    unsigned       lkind = llhead->ll_lkind;
    unsigned       address_size = llhead->ll_address_size;
    Dwarf_Unsigned listlen = 0;
    Dwarf_Locdesc_c llbuf = 0;
    Dwarf_CU_Context cucontext;

    off_res = _dwarf_get_loclist_header_start(dbg,
        attr, &loclist_offset, error);
    if (off_res != DW_DLV_OK) {
        return off_res;
    }
    starting_loclist_offset = loclist_offset;

    if (lkind == DW_LKIND_GNU_exp_list) {
        count_res = _dwarf_get_loclist_lle_count_dwo(dbg,
            loclist_offset,
            (Dwarf_Half)address_size,
            (Dwarf_Half)lkind,
            &loclist_count,
            error);
    } else {
        count_res = _dwarf_get_loclist_lle_count(dbg,
            loclist_offset, (Dwarf_Half)address_size,
            &loclist_count,
            error);
    }
    if (count_res != DW_DLV_OK) {
        return count_res;
    }
    if (loclist_count == 0) {
        return DW_DLV_NO_ENTRY;
    }

    listlen = loclist_count;
    llbuf = (Dwarf_Locdesc_c)
        _dwarf_get_alloc(dbg, DW_DLA_LOCDESC_C, listlen);
    if (!llbuf) {
        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
        return DW_DLV_ERROR;
    }
    llbuf->ld_magic = LOCLISTS_MAGIC;
    llhead->ll_locdesc = llbuf;
    llhead->ll_locdesc_count = listlen;
    cucontext = llhead->ll_context;
    llhead->ll_llearea_offset = loclist_offset;
    /* Now get loc ops */
    for (lli = 0; lli < listlen; ++lli) {
        int lres = 0;
        Dwarf_Half lle_op = 0;
        Dwarf_Bool at_end = 0;
        Dwarf_Block_c loc_block;
        Dwarf_Unsigned rawlowpc = 0;
        Dwarf_Unsigned rawhighpc = 0;
        int blkres = 0;

        memset(&loc_block,0,sizeof(loc_block));
        if (lkind == DW_LKIND_GNU_exp_list) {
            blkres = _dwarf_read_loc_section_dwo(dbg,
                &loc_block,
                &rawlowpc, &rawhighpc,
                &at_end, &lle_op,
                loclist_offset,
                address_size,
                lkind,
                error);
        } else {
            blkres = _dwarf_read_loc_section(dbg,
                &loc_block,
                &rawlowpc, &rawhighpc,
                &lle_op,
                loclist_offset,
                address_size,
                error);
        }
        if (blkres != DW_DLV_OK) {
            return blkres;
        }
        /* Fills in the locdesc and its operators list at index lli */
        lres = _dwarf_fill_in_locdesc_op_c(dbg,
            lli,
            llhead,
            &loc_block,
            address_size,
            cucontext->cc_length_size,
            cucontext->cc_version_stamp,
            rawlowpc,
            rawhighpc,
            lle_op,
            error);
        if (lres != DW_DLV_OK) {
            return lres;
        }
        /* Now get to next loclist entry offset. */
        loclist_offset = loc_block.bl_section_offset +
            loc_block.bl_len;
    }
    /*  We need to calculate the cooked values for
        each locldesc entry, that will be done
        in dwarf_get_loclist_d(). */

    llhead->ll_bytes_total = loclist_offset -
        starting_loclist_offset;
    return DW_DLV_OK;
}

static int
_dwarf_original_expression_build(Dwarf_Debug dbg,
    Dwarf_Loc_Head_c llhead,
    Dwarf_Attribute attr,
    Dwarf_Error *error)
{

    Dwarf_Block_c loc_blockc;
    Dwarf_Unsigned rawlowpc = 0;
    Dwarf_Unsigned rawhighpc = 0;
    unsigned form = llhead->ll_attrform;
    int blkres = 0;
    Dwarf_Locdesc_c llbuf = 0;
    unsigned listlen = 1;
    Dwarf_CU_Context cucontext = llhead->ll_context;
    unsigned address_size = llhead->ll_address_size;

    memset(&loc_blockc,0,sizeof(loc_blockc));
    if (form == DW_FORM_exprloc) {
        /*  A bit ugly. dwarf_formexprloc should use a
            Dwarf_Block argument. */
        blkres = dwarf_formexprloc(attr,&loc_blockc.bl_len,
            (Dwarf_Ptr)&loc_blockc.bl_data,error);
        if (blkres != DW_DLV_OK) {
            return blkres;
        }
        loc_blockc.bl_kind = llhead->ll_lkind;
        loc_blockc.bl_section_offset  =
            (char *)loc_blockc.bl_data -
            (char *)dbg->de_debug_info.dss_data;
        loc_blockc.bl_locdesc_offset = 0; /* not relevant */
    } else {
        Dwarf_Block loc_block;

        memset(&loc_block,0,sizeof(loc_block));
        blkres = _dwarf_formblock_internal(dbg,attr,
            llhead->ll_context,
            &loc_block,
            error);
        if (blkres != DW_DLV_OK) {
            return blkres;
        }
        loc_blockc.bl_len = loc_block.bl_len;
        loc_blockc.bl_data = loc_block.bl_data;
        loc_blockc.bl_kind = llhead->ll_lkind;
        loc_blockc.bl_section_offset =
            loc_block.bl_section_offset;
        loc_blockc.bl_locdesc_offset = 0; /* not relevant */
    }
    /*  We will mark the Locdesc_c DW_LLE_start_end
        shortly. Here we fake the address range
        as 'all addresses'. */
    rawlowpc = 0;
    rawhighpc = MAX_ADDR;

    llbuf = (Dwarf_Locdesc_c)
        _dwarf_get_alloc(dbg, DW_DLA_LOCDESC_C, listlen);
    if (!llbuf) {
        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
        return DW_DLV_ERROR;
    }
    llbuf->ld_magic = LOCLISTS_MAGIC;
    llhead->ll_locdesc = llbuf;
    /* Count is one by definition of a location entry. */
    llhead->ll_locdesc_count = listlen;
    llbuf = 0;

    /*  An empty location description (block length 0)
        means the code generator emitted no variable,
        the variable was not generated, it was unused
        or perhaps never tested after being set. Dwarf2,
        section 2.4.1 In other words, it is not an error,
        and we don't test for block length 0 specially here. */

    /*  Fills in the locdesc and its operators list
        at index 0 */
    blkres = _dwarf_fill_in_locdesc_op_c(dbg,
        0, /* fake locdesc is index 0 */
        llhead,
        &loc_blockc,
        llhead->ll_address_size,
        cucontext->cc_length_size,
        cucontext->cc_version_stamp,
        rawlowpc, rawhighpc,
        0,
        error);
    llhead->ll_bytes_total += loc_blockc.bl_len;
    if (blkres != DW_DLV_OK) {
        /* low level error already set: let problem be passed back */
        dwarf_dealloc(dbg,llhead->ll_locdesc,DW_DLA_LOCDESC_C);
        llhead->ll_locdesc = 0;
        llhead->ll_locdesc_count = 0;
        return blkres;
    }
    return DW_DLV_OK;
}

/*  Following the original loclist definition the low
    value is all one bits, the high value is the base
    address. */
static int
cook_original_loclist_contents(Dwarf_Debug dbg,
    Dwarf_Loc_Head_c llhead,
    Dwarf_Error *error)
{
    Dwarf_Unsigned baseaddress = llhead->ll_cu_base_address;
    Dwarf_Unsigned count = llhead->ll_locdesc_count;
    Dwarf_Unsigned i = 0;

    for ( i = 0 ; i < count; ++i) {
        Dwarf_Locdesc_c  llc = 0;

        llc = llhead->ll_locdesc +i;
        switch(llc->ld_lle_value) {
        case DW_LLE_end_of_list: {
            /* nothing to do */
            break;
            }
        case DW_LLE_base_address: {
            llc->ld_lopc =  llc->ld_rawhigh;
            llc->ld_highpc =  llc->ld_rawhigh;
            baseaddress =  llc->ld_rawhigh;
            break;
            }
            /* This is the only way baseaddress is used. */
        case DW_LLE_offset_pair: {
            llc->ld_lopc = llc->ld_rawlow + baseaddress;
            llc->ld_highpc = llc->ld_rawhigh + baseaddress;
            break;
            }
        default: {
            dwarfstring m;

            dwarfstring_constructor(&m);
            dwarfstring_append_printf_u(&m,
                "DW_DLE_LOCLISTS_ERROR: "
                "improper synthesized LLE code "
                "of 0x%x is unknown. In standard DWARF3/4 loclist",
                llc->ld_lle_value);
            _dwarf_error_string(dbg,error,
                DW_DLE_LOCLISTS_ERROR,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
            }
        }
    }
    return DW_DLV_OK;
}

static int
cook_gnu_loclist_contents(Dwarf_Debug dbg,
    Dwarf_Loc_Head_c llhead,
    Dwarf_Error *error)
{
    Dwarf_Unsigned baseaddress = llhead->ll_cu_base_address;
    Dwarf_Unsigned count = llhead->ll_locdesc_count;
    Dwarf_Unsigned i = 0;
    Dwarf_CU_Context cucontext = llhead->ll_context;
    int res = 0;

    for (i = 0 ; i < count ; ++i) {
        Dwarf_Locdesc_c  llc = 0;

        llc = llhead->ll_locdesc +i;
        switch(llc->ld_lle_value) {
        case DW_LLEX_base_address_selection_entry:{
            Dwarf_Addr targaddr = 0;

            res = _dwarf_look_in_local_and_tied_by_index(
                dbg,cucontext,llc->ld_rawhigh,&targaddr,
                error);
            if (res != DW_DLV_OK) {
                llc->ld_index_failed = TRUE;
                llc->ld_lopc = 0;
                llc->ld_highpc = 0;
                if (res == DW_DLV_ERROR && error) {
                    dwarf_dealloc_error(dbg, *error);
                    *error = 0;
                }
            } else {
                llc->ld_lopc = targaddr;
                llc->ld_highpc = targaddr;
            }
            break;
            }
        case DW_LLEX_end_of_list_entry:{
            /* Nothing to do. */
            break;
            }
        case DW_LLEX_start_length_entry:{
            Dwarf_Addr targaddr = 0;
            res = _dwarf_look_in_local_and_tied_by_index(
                dbg,cucontext,llc->ld_rawlow,&targaddr,
                error);
            if (res != DW_DLV_OK) {
                llc->ld_index_failed = TRUE;
                llc->ld_lopc = 0;
                if (res == DW_DLV_ERROR && error) {
                    dwarf_dealloc_error(dbg, *error);
                    *error = 0;
                }
            } else {
                llc->ld_lopc = targaddr;
                llc->ld_highpc = llc->ld_lopc +llc->ld_rawhigh;
            }
            break;
            }
        case DW_LLEX_offset_pair_entry:{
            llc->ld_lopc = llc->ld_rawlow + baseaddress;
            llc->ld_highpc = llc->ld_rawhigh + baseaddress;
            break;
            }
        case DW_LLEX_start_end_entry:{
            Dwarf_Addr targaddr = 0;
            res = _dwarf_look_in_local_and_tied_by_index(
                dbg,cucontext,llc->ld_rawlow,&targaddr,
                error);
            if (res != DW_DLV_OK) {
                llc->ld_index_failed = TRUE;
                llc->ld_lopc = 0;
                if (res == DW_DLV_ERROR && error) {
                    dwarf_dealloc_error(dbg, *error);
                    *error = 0;
                }
            } else {
                llc->ld_lopc = targaddr;
            }
            res = _dwarf_look_in_local_and_tied_by_index(
                dbg,cucontext,llc->ld_rawlow,&targaddr,
                error);
            if (res != DW_DLV_OK) {
                llc->ld_index_failed = TRUE;
                llc->ld_highpc = 0;
                if (res == DW_DLV_ERROR && error) {
                    dwarf_dealloc_error(dbg, *error);
                    *error = 0;
                }
            } else {
                llc->ld_highpc = targaddr;
            }

            break;
            }
        default:{
            dwarfstring m;

            dwarfstring_constructor(&m);
            dwarfstring_append_printf_u(&m,
                "DW_DLE_LOCLISTS_ERROR: improper LLEX code "
                "of 0x%x is unknown. GNU LLEX dwo loclists error",
                llc->ld_lle_value);
            _dwarf_error_string(dbg,error,
                DW_DLE_LOCLISTS_ERROR,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;

            break;
            }
        }
    }
    return DW_DLV_OK;
}

/* DWARF5 */
static int
cook_loclists_contents(Dwarf_Debug dbg,
    Dwarf_Loc_Head_c llhead,
    Dwarf_Error *error)
{
    Dwarf_Unsigned baseaddress = llhead->ll_cu_base_address;
    Dwarf_Unsigned count = llhead->ll_locdesc_count;
    Dwarf_Unsigned i = 0;
    Dwarf_CU_Context cucontext = llhead->ll_context;
    int res = 0;
    Dwarf_Bool base_address_fail = FALSE;
    Dwarf_Bool debug_addr_fail = FALSE;

    if (!llhead->ll_cu_base_address_present) {
        base_address_fail = TRUE;
    }
    for (i = 0 ; i < count ; ++i) {
        Dwarf_Locdesc_c  llc = 0;

        llc = llhead->ll_locdesc +i;
        switch(llc->ld_lle_value) {
        case DW_LLE_base_addressx: {
            Dwarf_Addr targaddr = 0;
            if (debug_addr_fail) {
                res = DW_DLV_NO_ENTRY;
            } else {
                res = _dwarf_look_in_local_and_tied_by_index(
                    dbg,cucontext,llc->ld_rawlow,&targaddr,
                    error);
            }
            if (res != DW_DLV_OK) {
                debug_addr_fail = TRUE;
                base_address_fail = TRUE;
                llc->ld_index_failed = TRUE;
                llc->ld_lopc = 0;
                if (res == DW_DLV_ERROR && error) {
                    dwarf_dealloc_error(dbg, *error);
                    *error = 0;
                }
            } else {
                base_address_fail = FALSE;
                baseaddress = targaddr;
                llc->ld_lopc = targaddr;
            }
            break;
        }
        case DW_LLE_startx_endx:{
            /* two indexes into debug_addr */
            Dwarf_Addr targaddr = 0;
            if (debug_addr_fail) {
                res = DW_DLV_NO_ENTRY;
            } else {
                res = _dwarf_look_in_local_and_tied_by_index(
                    dbg,cucontext,llc->ld_rawlow,&targaddr,
                    error);
            }
            if (res != DW_DLV_OK) {
                debug_addr_fail = TRUE;
                llc->ld_index_failed = TRUE;
                llc->ld_lopc = 0;
                if (res == DW_DLV_ERROR && error) {
                    dwarf_dealloc_error(dbg, *error);
                    *error = 0;
                }
            } else {
                llc->ld_lopc = targaddr;
            }
            if (debug_addr_fail) {
                res = DW_DLV_NO_ENTRY;
            } else {
                res = _dwarf_look_in_local_and_tied_by_index(
                    dbg,cucontext,llc->ld_rawhigh,&targaddr,
                    error);

            }
            if (res != DW_DLV_OK) {
                debug_addr_fail = TRUE;
                llc->ld_index_failed = TRUE;
                llc->ld_highpc = 0;
                if (res == DW_DLV_ERROR && error) {
                    dwarf_dealloc_error(dbg, *error);
                    *error = 0;
                }
            } else {
                llc->ld_highpc = targaddr;
            }
            break;
        }
        case DW_LLE_startx_length:{
            /* one index to debug_addr other a length */
            Dwarf_Addr targaddr = 0;
            if (debug_addr_fail) {
                res = DW_DLV_NO_ENTRY;
            } else  {
                res = _dwarf_look_in_local_and_tied_by_index(
                    dbg,cucontext,llc->ld_rawlow,&targaddr,
                    error);
            }
            if (res != DW_DLV_OK) {
                debug_addr_fail = TRUE;
                llc->ld_index_failed = TRUE;
                llc->ld_lopc = 0;
                if (res == DW_DLV_ERROR && error) {
                    dwarf_dealloc_error(dbg, *error);
                    *error = 0;
                }
            } else {
                llc->ld_lopc = targaddr;
                llc->ld_highpc = targaddr + llc->ld_rawhigh;
            }
            break;
        }
        case DW_LLE_offset_pair:{
            if (base_address_fail) {
                llc->ld_index_failed = TRUE;
                llc->ld_lopc = 0;
                llc->ld_highpc = 0;
            } else {
                /*offsets of the current base address*/
                llc->ld_lopc = llc->ld_rawlow +baseaddress;
                llc->ld_highpc = llc->ld_rawhigh +baseaddress;
            }
            break;
        }
        case DW_LLE_default_location:{
            /*  nothing to do here, just has a counted
                location description */
            break;
        }
        case DW_LLE_base_address:{
            llc->ld_lopc = llc->ld_rawlow;
            llc->ld_highpc = llc->ld_rawlow;
            baseaddress = llc->ld_rawlow;
            base_address_fail = FALSE;
            break;
        }
        case DW_LLE_start_end:{
            llc->ld_lopc = llc->ld_rawlow;
            llc->ld_highpc = llc->ld_rawhigh;
            break;
        }
        case DW_LLE_start_length:{
            llc->ld_lopc = llc->ld_rawlow;
            llc->ld_highpc = llc->ld_rawlow + llc->ld_rawhigh;
            break;
        }
        case DW_LLE_end_of_list:{
            /* do nothing */
            break;
        }
        default: {
            dwarfstring m;

            dwarfstring_constructor(&m);
            dwarfstring_append_printf_u(&m,
                "DW_DLE_LOCLISTS_ERROR: improper DW_LLE code "
                "of 0x%x is unknown. DWARF5 loclists error",
                llc->ld_lle_value);
            _dwarf_error_string(dbg,error,
                DW_DLE_LOCLISTS_ERROR,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
        }
    }
    return DW_DLV_OK;
}

/*  New October 2015
    This interface requires the use of interface functions
    to get data from Dwarf_Locdesc_c.  The structures
    are not visible to callers. */
int
dwarf_get_loclist_c(Dwarf_Attribute attr,
    Dwarf_Loc_Head_c * ll_header_out,
    Dwarf_Unsigned   * listlen_out,
    Dwarf_Error      * error)
{
    Dwarf_Debug        dbg            = 0;
    Dwarf_Half         form           = 0;
    Dwarf_Loc_Head_c   llhead         = 0;
    unsigned           address_size   = 0;
    Dwarf_Half         cuversionstamp = 0;
    Dwarf_Bool         is_cu          = FALSE;
    Dwarf_Unsigned     attrnum        = 0;
    Dwarf_Bool         is_dwo         = 0;
    int                lkind          = 0;
    Dwarf_CU_Context   ctx            = 0;
    Dwarf_Bool         is_loclistx    = FALSE;
    Dwarf_Unsigned     attr_val       = 0;
    Dwarf_Bool         offset_is_info = TRUE;
    int                res = 0;
    int                setup_res = 0;

    if (!attr) {
        _dwarf_error_string(dbg, error,DW_DLE_ATTR_NULL,
            "DW_DLE_ATTR_NULL"
            "NULL Dwarf_Attribute "
            "argument passed to "
            "dwarf_get_loclist_c()");
        return DW_DLV_ERROR;
    }
    setup_res = _dwarf_setup_loc(attr, &dbg,&ctx, &form, error);
    if (setup_res != DW_DLV_OK) {
        return setup_res;
    }

    CHECK_DBG(dbg,error,"dwarf_get_loclist_c()");
    if (form == DW_FORM_loclistx) {
        is_loclistx = TRUE;
    }
    attrnum = attr->ar_attribute;
    cuversionstamp = ctx->cc_version_stamp;
    address_size = ctx->cc_address_size;
    is_dwo = ctx->cc_is_dwo;
    lkind = determine_location_lkind(cuversionstamp,
        form, is_dwo);

    if (form == DW_FORM_loclistx || form == DW_FORM_sec_offset) {
        /* Aimed at DWARF5 and later */
        res = dwarf_global_formref_b(attr,&attr_val,
            &offset_is_info,error);
        if (res != DW_DLV_OK) {
            return res;
        }
    }
    if (lkind == DW_LKIND_loclists) {
        if (is_loclistx) {
            if (ctx->cc_loclists_base_present ||
                dbg->de_loclists_context_count == 1) {
                /*  leave on primary.
                    WARNING: It is not clear whether
                    looking for a context count of 1
                    is actually correct, but it
                    seems to work. */
            } else if (DBG_HAS_SECONDARY(dbg)){
                dbg = dbg->de_secondary_dbg;
                CHECK_DBG(dbg,error,
                    "dwarf_loclists_get_lle_head() "
                    "via attribute(sec)");
            }
        } else {
            /*  attr_val is .debug_loclists[.dwo]
                section global offset
                of a location list*/
            if (!dbg->de_debug_loclists.dss_size ||
                attr_val >= dbg->de_debug_loclists.dss_size) {
                if (DBG_HAS_SECONDARY(dbg)) {
                    dbg = dbg->de_secondary_dbg;
                    CHECK_DBG(dbg,error,
                        "dwarf_loclists_get_lle_head() "
                        "via attribute(secb)");
                } else {
                    /*  There is an error to be
                        generated later */
                }
            }
        }
        res = _dwarf_load_section(dbg,
            &dbg->de_debug_loclists,
            error);
        if (res == DW_DLV_ERROR) {
            return res;
        }
    }

    attrnum = attr->ar_attribute;
    cuversionstamp = ctx->cc_version_stamp;
    address_size = ctx->cc_address_size;
    is_dwo = ctx->cc_is_dwo;
    lkind = determine_location_lkind(cuversionstamp,
        form, is_dwo);
    if (lkind == DW_LKIND_unknown) {
        dwarfstring m;
        const char * formname = "<unknownform>";
        const char * attrname = "<unknown attribute>";

        dwarfstring_constructor(&m);
        dwarf_get_FORM_name((unsigned int)form,&formname);
        dwarf_get_AT_name((unsigned int)attrnum,&attrname);
        dwarfstring_append_printf_u(&m,
            "DW_DLE_LOC_EXPR_BAD: For Compilation Unit "
            "version %u",cuversionstamp);
        dwarfstring_append_printf_u(&m,
            ", attribute 0x%x (",attrnum);
        dwarfstring_append(&m,(char *)attrname);
        dwarfstring_append_printf_u(&m,
            ") form 0x%x (",form);
        dwarfstring_append(&m,(char *)formname);
        if (is_dwo) {
            dwarfstring_append(&m,") (the CU is a .dwo) ");
        } else {
            dwarfstring_append(&m,") (the CU is not a .dwo) ");
        }
        dwarfstring_append(&m," we don't understand the location");
        _dwarf_error_string(dbg,error,DW_DLE_LOC_EXPR_BAD,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    /*  Doing this early (first) to avoid repeating the alloc code
        for each type  */
    llhead = (Dwarf_Loc_Head_c)
        _dwarf_get_alloc(dbg, DW_DLA_LOC_HEAD_C, 1);
    if (!llhead) {
        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
        return DW_DLV_ERROR;
    }
    llhead->ll_cuversion = cuversionstamp;
    llhead->ll_lkind = lkind;
    llhead->ll_attrnum = (Dwarf_Half)attrnum;
    llhead->ll_attrform = (Dwarf_Half)form;
    llhead->ll_dbg = dbg;
    llhead->ll_address_size = address_size;
    llhead->ll_offset_size = ctx->cc_length_size;
    llhead->ll_context = ctx;
    llhead->ll_magic = LOCLISTS_MAGIC;

    llhead->ll_at_loclists_base_present =
        ctx->cc_loclists_base_present;
    llhead->ll_at_loclists_base =  ctx->cc_loclists_base;
    llhead->ll_cu_base_address_present =
        ctx->cc_base_address_present;
    llhead->ll_cu_base_address = ctx->cc_base_address;

    llhead->ll_cu_addr_base_offset = ctx->cc_addr_base_offset;
    llhead->ll_cu_addr_base_offset_present =
        ctx->cc_addr_base_offset_present;

    if (lkind == DW_LKIND_loclist ||
        lkind == DW_LKIND_GNU_exp_list) {
        int ores = 0;
        /* Here we have a loclist to deal with. */
        ores = context_is_cu_not_tu(ctx,&is_cu);
        if (ores != DW_DLV_OK) {
            dwarf_dealloc_loc_head_c(llhead);
            return ores;
        }
        ores = _dwarf_original_loclist_build(dbg,
            llhead, attr, error);
        if (ores != DW_DLV_OK) {
            dwarf_dealloc_loc_head_c(llhead);
            return ores;
        }
        if (lkind == DW_LKIND_loclist) {
            ores = cook_original_loclist_contents(dbg,llhead,
                error);
        } else {
            ores = cook_gnu_loclist_contents(dbg,llhead,error);
        }
        if (ores != DW_DLV_OK) {
            dwarf_dealloc_loc_head_c(llhead);
            return ores;
        }
    } else if (lkind == DW_LKIND_expression) {
        /* DWARF2,3,4,5 */
        int eres = 0;
        eres = _dwarf_original_expression_build(dbg,
            llhead, attr, error);
        if (eres != DW_DLV_OK) {
            dwarf_dealloc_loc_head_c(llhead);
            return eres;
        }
    } else if (lkind == DW_LKIND_loclists) {
        /* DWARF5! */
        int leres = 0;

        leres = _dwarf_loclists_fill_in_lle_head(dbg,
            attr,form,attr_val,llhead,error);
        if (leres != DW_DLV_OK) {
            dwarf_dealloc_loc_head_c(llhead);
            return leres;
        }
        leres = cook_loclists_contents(dbg,llhead,error);
        if (leres != DW_DLV_OK) {
            dwarf_dealloc_loc_head_c(llhead);
            return leres;
        }
    } /* ASSERT else impossible */
    *ll_header_out = llhead;
    *listlen_out = llhead->ll_locdesc_count;
    return DW_DLV_OK;
}

/*  An interface giving us no cu context!
    This is not going to be quite right. */
int
dwarf_loclist_from_expr_c(Dwarf_Debug dbg,
    Dwarf_Ptr expression_in,
    Dwarf_Unsigned expression_length,
    Dwarf_Half address_size,
    Dwarf_Half offset_size,
    Dwarf_Half dwarf_version,
    Dwarf_Loc_Head_c *loc_head,
    Dwarf_Unsigned * listlen,
    Dwarf_Error * error)
{
    /* Dwarf_Block that describes a single location expression. */
    Dwarf_Block_c loc_block;
    Dwarf_Loc_Head_c llhead = 0;
    Dwarf_Locdesc_c llbuf = 0;
    int local_listlen = 1;
    Dwarf_Addr rawlowpc = 0;
    Dwarf_Addr rawhighpc = MAX_ADDR;
    Dwarf_Half version_stamp = dwarf_version;
    int res = 0;

    CHECK_DBG(dbg,error,"dwarf_loclist_from_expr_c()");
    llhead = (Dwarf_Loc_Head_c)_dwarf_get_alloc(dbg,
        DW_DLA_LOC_HEAD_C, 1);
    if (!llhead) {
        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
        return DW_DLV_ERROR;
    }
    llhead->ll_magic = LOCLISTS_MAGIC;
    memset(&loc_block,0,sizeof(loc_block));
    loc_block.bl_len = expression_length;
    loc_block.bl_data = expression_in;
    loc_block.bl_kind = DW_LKIND_expression; /* Not from loclist. */
    loc_block.bl_section_offset = 0; /* Fake. Not meaningful. */
    loc_block.bl_locdesc_offset = 0; /* Fake. Not meaningful. */
    llbuf = (Dwarf_Locdesc_c)
        _dwarf_get_alloc(dbg, DW_DLA_LOCDESC_C, local_listlen);
    if (!llbuf) {
        dwarf_dealloc_loc_head_c(llhead);
        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
        return DW_DLV_ERROR;
    }
    llbuf->ld_magic = LOCLISTS_MAGIC;
    llhead->ll_locdesc = llbuf;
    llhead->ll_locdesc_count = local_listlen;
    llhead->ll_context = 0; /* Not available! */
    llhead->ll_dbg = dbg;
    llhead->ll_lkind = DW_LKIND_expression;

    /*  An empty location description (block length 0)
        means the code generator emitted no variable,
        the variable was not generated,
        it was unused or perhaps never tested
        after being set. Dwarf2,
        section 2.4.1 In other words, it is not
        an error, and we don't
        test for block length 0 specially here.  */

    /* Fills in the locdesc and its operators list at index 0 */
    res = _dwarf_fill_in_locdesc_op_c(dbg,
        0,
        llhead,
        &loc_block,
        address_size,
        offset_size,
        version_stamp,
        rawlowpc,
        rawhighpc,
        DW_LKIND_expression,
        error);
    if (res != DW_DLV_OK) {
        /* low level error already set: let it be passed back */
        dwarf_dealloc(dbg,llbuf,DW_DLA_LOCDESC_C);
        llhead->ll_locdesc = 0;
        llhead->ll_locdesc_count = 0;
        dwarf_dealloc_loc_head_c(llhead);
        return DW_DLV_ERROR;
    }
    *loc_head = llhead;
    *listlen = local_listlen;
    return DW_DLV_OK;
}

/*  New June 2020  Supports all versions of DWARF.
    Distinguishes location entry values as in the
    file directly (raw) from  the computed
    value (lowpc_out,hipc_out) after
    applying base values (if any). */
int
dwarf_get_locdesc_entry_d(Dwarf_Loc_Head_c loclist_head,
    Dwarf_Unsigned   index,
    /* Dwarf_Unsigned   lle_entry_index,  */
    Dwarf_Small    * lle_value_out,
    Dwarf_Unsigned * rawval1,
    Dwarf_Unsigned * rawval2,
    Dwarf_Bool     * debug_addr_unavailable,
    Dwarf_Addr     * lowpc_out, /* 'cooked' value */
    Dwarf_Addr     * hipc_out, /* 'cooked' value */
    Dwarf_Unsigned * loclist_expr_op_count_out,
    /* Returns pointer to the specific locdesc of the index; */
    Dwarf_Locdesc_c* locdesc_entry_out,
    Dwarf_Small    * loclist_source_out, /* 0,1, or 2 */
    Dwarf_Unsigned * expression_offset_out,
    Dwarf_Unsigned * locdesc_offset_out,
    Dwarf_Error    * error)
{
    return dwarf_get_locdesc_entry_e(
        loclist_head, index,
        /*0   base local index,*/
        lle_value_out,
        rawval1,
        rawval2,
        debug_addr_unavailable,
        lowpc_out, /* 'cooked' value */
        hipc_out, /* 'cooked' value */
        loclist_expr_op_count_out,
        0 /* not returning lle_bytecount*/ ,
        /* Returns pointer to the specific locdesc of the index; */
        locdesc_entry_out,
        loclist_source_out, /* 0,1, or 2 */
        expression_offset_out,
        locdesc_offset_out,
        error);
}
int
dwarf_get_locdesc_entry_e(Dwarf_Loc_Head_c loclist_head,
    /*  The context-local offset of an lle set as reverenced
        by an offset_entry_table. */
    Dwarf_Unsigned   index,
    /*Dwarf_Unsigned   base_local_index, */
    Dwarf_Small    * lle_value_out,
    Dwarf_Unsigned * rawval1,
    Dwarf_Unsigned * rawval2,
    Dwarf_Bool     * debug_addr_unavailable,
    Dwarf_Addr     * lowpc_out, /* 'cooked' value */
    Dwarf_Addr     * hipc_out, /* 'cooked' value */
    Dwarf_Unsigned * loclist_expr_op_count_out,
    /* Returns pointer to the specific locdesc of the index; */
    Dwarf_Unsigned * lle_bytecount,
    Dwarf_Locdesc_c* locdesc_entry_out,
    Dwarf_Small    * loclist_source_out, /* 0,1, or 2 */
    Dwarf_Unsigned * expression_offset_out,
    Dwarf_Unsigned * locdesc_offset_out,
    Dwarf_Error    * error)
{
    Dwarf_Locdesc_c descs_base =  0;
    Dwarf_Locdesc_c desc =  0;
    Dwarf_Unsigned  desc_count = 0;
    Dwarf_Debug     dbg = 0;

    if (!loclist_head || loclist_head->ll_magic != LOCLISTS_MAGIC) {
        _dwarf_error_string(dbg, error,DW_DLE_DBG_NULL,
            "DW_DLE_DBG_NULL: "
            "Dwarf_Loc_Head_c NULL or not "
            "marked LOCLISTS_MAGIC "
            "in calling "
            "dwarf_get_locdesc_entry_d()");
        return DW_DLV_ERROR;
    }
    desc_count = loclist_head->ll_locdesc_count;
    descs_base  = loclist_head->ll_locdesc;
    dbg = loclist_head->ll_dbg;
    if (index >= desc_count) {
        _dwarf_error(dbg, error, DW_DLE_LOCLIST_INDEX_ERROR);
        return DW_DLV_ERROR;
    }
    desc = descs_base + index;
    *lle_value_out = desc->ld_lle_value;
    *rawval1 = desc->ld_rawlow;
    *rawval2 = desc->ld_rawhigh;
    *lowpc_out = desc->ld_lopc;
    *hipc_out = desc->ld_highpc;
    *debug_addr_unavailable = desc->ld_index_failed;
    *loclist_expr_op_count_out = desc->ld_cents;
    *locdesc_entry_out = desc;
    *loclist_source_out = (Dwarf_Small)desc->ld_lkind;
    *expression_offset_out = desc->ld_section_offset;
    *locdesc_offset_out = desc->ld_locdesc_offset;
    if (lle_bytecount) {
        *lle_bytecount = desc->ld_lle_bytecount;
    }
    return DW_DLV_OK;
}

int
dwarf_get_location_op_value_c(Dwarf_Locdesc_c locdesc,
    Dwarf_Unsigned   index,
    Dwarf_Small    * atom_out,
    Dwarf_Unsigned * operand1,
    Dwarf_Unsigned * operand2,
    Dwarf_Unsigned * operand3,
    Dwarf_Unsigned * offset_for_branch,
    Dwarf_Error*     error)
{
    Dwarf_Loc_Expr_Op op = 0;
    Dwarf_Unsigned max = 0;

    if (!locdesc) {
        _dwarf_error_string(NULL, error,DW_DLE_DBG_NULL,
            "DW_DLE_DBG_NULL"
            "Dwarf_Locdesc_c_Head_c NULL "
            "in calling "
            "dwarf_get_location_op_value_c()");
        return DW_DLV_ERROR;
    }
    max = locdesc->ld_cents;
    if (index >= max) {
        Dwarf_Debug dbg = locdesc->ld_loclist_head->ll_dbg;
        _dwarf_error(dbg, error, DW_DLE_LOCLIST_INDEX_ERROR);
        return DW_DLV_ERROR;
    }
    op = locdesc->ld_s + index;
    *atom_out = op->lr_atom;
    *operand1 = op->lr_number;
    *operand2 = op->lr_number2;
    *operand3 = op->lr_number3;
    *offset_for_branch = op->lr_offset;
    return DW_DLV_OK;
}
/* ============== End of the October 2015 interfaces. */
