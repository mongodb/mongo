/*
Copyright (C) 2021 David Anderson. All Rights Reserved.

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

#include <string.h> /* memcmp() */
#include <stdio.h> /* printf() debugging */

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

static int
_dwarf_find_CU_Context_given_sig(Dwarf_Debug dbg,
    int context_level,
    Dwarf_Sig8 *sig_in,
    Dwarf_CU_Context *cu_context_out,
    Dwarf_Bool *is_info_out,
    Dwarf_Error *error)
{
    Dwarf_CU_Context cu_context = 0;
    Dwarf_Bool is_info = FALSE;
    int loopcount = 0;
    int lres = 0;
    Dwarf_Debug_InfoTypes dis = 0;
    struct Dwarf_Section_s *secdp = 0;

    /*  Loop once with is_info, once with !is_info.
        Then stop. */
    for ( ; loopcount < 2; ++loopcount) {
        Dwarf_CU_Context prev_cu_context = 0;
        Dwarf_Unsigned section_size = 0;
        Dwarf_Unsigned new_cu_offset = 0;

        is_info = !is_info;

        if (is_info) {
            dis   = &dbg->de_info_reading;
            secdp = &dbg->de_debug_info;
        } else {
            dis   = &dbg->de_types_reading;
            secdp = &dbg->de_debug_types;
        }
        lres = _dwarf_load_die_containing_section(dbg,is_info,error);
        if (lres == DW_DLV_ERROR) {
            return lres;
        }
        if (lres == DW_DLV_NO_ENTRY ) {
            continue;
        }
        /* Lets see if we already have the CU we need. */
        for (cu_context = dis->de_cu_context_list;
            cu_context; cu_context = cu_context->cc_next) {
            prev_cu_context = cu_context;

            if (memcmp(sig_in,&cu_context->cc_signature,
                sizeof(Dwarf_Sig8))) {
                continue;
            }
            if (cu_context->cc_unit_type == DW_UT_split_type||
                cu_context->cc_unit_type == DW_UT_type) {
                *cu_context_out = cu_context;
                *is_info_out = cu_context->cc_is_info;
                return DW_DLV_OK;
            }
        }
        if (context_level > 0) {
            /*  Make no attempt to create new context,
                we are finishing cu die base fields
                on one already.
                Just look for the other context,
                DWARF4 debug_types  */
            continue;
        }
        if (prev_cu_context) {
            Dwarf_CU_Context lcu_context = prev_cu_context;
            new_cu_offset =
                _dwarf_calculate_next_cu_context_offset(
                lcu_context);
        } else {
            new_cu_offset = 0;
        }
        section_size = secdp->dss_size;
        for ( ; new_cu_offset < section_size;
            new_cu_offset =
                _dwarf_calculate_next_cu_context_offset(
                cu_context)) {
#if 0 /* unnecessary load section call, we think. */
            lres = _dwarf_load_die_containing_section(dbg,
                is_info,error);
            if (lres == DW_DLV_ERROR) {
                return lres;
            }
            if (lres == DW_DLV_NO_ENTRY) {
                continue;
            }
#endif /*0*/
            lres = _dwarf_create_a_new_cu_context_record_on_list(
                dbg, dis,is_info,section_size,new_cu_offset,
                &cu_context,NULL,error);
            if (lres == DW_DLV_ERROR) {
                return lres;
            }
            if (lres == DW_DLV_NO_ENTRY) {
                break;
            }
            if (memcmp(sig_in,&cu_context->cc_signature,
                sizeof(Dwarf_Sig8))) {
                continue;
            }
            if (cu_context->cc_unit_type == DW_UT_split_type||
                cu_context->cc_unit_type == DW_UT_type) {
                *cu_context_out = cu_context;
                *is_info_out = cu_context->cc_is_info;
                return DW_DLV_OK;
            }
        }
    }   /* Loop-end.  */
    /*  Not found */
    return DW_DLV_NO_ENTRY;
}

/*  We will search to find a CU with the indicated signature
    The attribute leading us here is often
    We are looking for a DW_UT_split_type or DW_UT_type
    CU.
    DW_AT_type and if DWARF4 that means our first look is
    to !is_info */
int
dwarf_find_die_given_sig8(Dwarf_Debug dbg,
    Dwarf_Sig8 *ref,
    Dwarf_Die  *die_out,
    Dwarf_Bool *is_info,
    Dwarf_Error *error)
{
    int res = 0;
    CHECK_DBG(dbg,error,"dwarf_find_die_given_sig8()");
    res = _dwarf_internal_find_die_given_sig8(
        dbg,0,ref,die_out,is_info,error);
    return res;
}

/*  If context level > 0 restrict what we will do
    to avoid recursion creating CU Contexts */
int
_dwarf_internal_find_die_given_sig8(Dwarf_Debug dbg,
    int context_level,
    Dwarf_Sig8 *ref,
    Dwarf_Die  *die_out,
    Dwarf_Bool *is_info,
    Dwarf_Error *error)
{
    int res                   = 0;
    Dwarf_Die ndi             = 0;
    Dwarf_CU_Context context  = 0;
    Dwarf_Bool result_is_info = FALSE;
    Dwarf_Unsigned dieoffset  = 0;

    res =_dwarf_find_CU_Context_given_sig(dbg,
        context_level,
        ref, &context, &result_is_info,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    dieoffset = context->cc_debug_offset +
        context->cc_signature_offset;
    res = dwarf_offdie_b(dbg,dieoffset,result_is_info,
        &ndi,error);
    if (res == DW_DLV_OK) {
        *die_out = ndi;
        *is_info = result_is_info;
    }
    return res;
}
