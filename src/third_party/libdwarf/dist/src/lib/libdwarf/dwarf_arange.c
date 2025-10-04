/*
  Copyright (C) 2000-2004 Silicon Graphics, Inc.  All Rights Reserved.
  Portions Copyright (C) 2007-2020 David Anderson. All Rights Reserved.
  Portions Copyright 2012 SN Systems Ltd. All rights reserved.

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
#include <stdio.h> /* debug printf */

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
#include "dwarf_arange.h"
#include "dwarf_global.h"  /* for _dwarf_fixup_* */
#include "dwarf_string.h"

static void
free_aranges_chain(Dwarf_Debug dbg, Dwarf_Chain head)
{
    Dwarf_Chain cur = head;
    Dwarf_Chain next = 0;

    if (!head) {
        return;
    }
    next = head->ch_next;
    for ( ;cur; cur = next) {
        void *item = cur->ch_item;
        int  type = cur->ch_itemtype;

        next = cur->ch_next;
        if (item && type) {
            dwarf_dealloc(dbg,item,type);
            cur->ch_item = 0;
            dwarf_dealloc(dbg,cur,DW_DLA_CHAIN);
        }
    }
}

/*  Common code for two user-visible routines to share.
    Errors here result in memory leaks, but errors here
    are serious (making aranges unusable) so we assume
    callers will not repeat the error often or mind the leaks.
*/
static int
_dwarf_get_aranges_list(Dwarf_Debug dbg,
    Dwarf_Chain  * chain_out,
    Dwarf_Signed * chain_count_out,
    Dwarf_Error  * error)
{
    /* Sweeps through the arange. */
    Dwarf_Small *arange_ptr = 0;
    Dwarf_Small *arange_ptr_start = 0;

    /*  Start of arange header.
        Used for rounding offset of arange_ptr
        to twice the tuple size.  Libdwarf requirement. */
    Dwarf_Small *header_ptr = 0;

    /*  Version of .debug_aranges header. */
    Dwarf_Unsigned version = 0;

    /*  Offset of current set of aranges into .debug_info. */
    Dwarf_Off info_offset = 0;
    /*  Size in bytes of addresses in target. */
    Dwarf_Small address_size = 0;
    /*  Size in bytes of segment offsets in target. */
    Dwarf_Small segment_sel_size = 0;
    /*  Count of total number of aranges. */
    Dwarf_Signed arange_count = 0;
    Dwarf_Arange arange = 0;
    Dwarf_Unsigned section_size = 0;
    Dwarf_Byte_Ptr arange_end_section = 0;
    /*  Used to chain Dwarf_Aranges structs. */
    Dwarf_Chain curr_chain = NULL;
    Dwarf_Chain head_chain = NULL;
    Dwarf_Chain *plast = &head_chain;

    if (!dbg->de_debug_aranges.dss_size) {
        return DW_DLV_NO_ENTRY;
    }
    arange_ptr = dbg->de_debug_aranges.dss_data;
    arange_ptr_start = arange_ptr;
    section_size = dbg->de_debug_aranges.dss_size;
    arange_end_section = arange_ptr + section_size;

    do {
        /*  Length of current set of aranges.
            This is local length, which begins just
            after the length field itself. */
        Dwarf_Unsigned area_length = 0;
        Dwarf_Unsigned remainder = 0;
        Dwarf_Unsigned range_entry_size = 0;
        int local_length_size;
        int local_extension_size = 0;
        Dwarf_Small *end_this_arange = 0;
        int res = 0;

        header_ptr = arange_ptr;
        if (header_ptr >= arange_end_section) {
            free_aranges_chain(dbg,head_chain);
            _dwarf_error(dbg, error,DW_DLE_ARANGES_HEADER_ERROR);
            return DW_DLV_ERROR;
        }
        res = _dwarf_read_area_length_ck_wrapper(dbg,&area_length,
            &arange_ptr,&local_length_size,&local_extension_size,
            section_size,arange_end_section,error);
        if (res != DW_DLV_OK) {
            free_aranges_chain(dbg,head_chain);
            return res;
        }
        /*  arange_ptr has been incremented appropriately past
            the length field by READ_AREA_LENGTH. */

        if (area_length >  dbg->de_debug_aranges.dss_size ||
            (area_length +local_length_size+local_extension_size)
            > dbg->de_debug_aranges.dss_size ) {
            free_aranges_chain(dbg,head_chain);
            _dwarf_error(dbg, error,DW_DLE_ARANGES_HEADER_ERROR);
            return DW_DLV_ERROR;
        }
        if ((area_length + local_length_size + local_extension_size) >
            dbg->de_debug_aranges.dss_size) {
            free_aranges_chain(dbg,head_chain);
            _dwarf_error(dbg, error, DW_DLE_ARANGES_HEADER_ERROR);
            return DW_DLV_ERROR;
        }

        end_this_arange = arange_ptr + area_length;
        if (end_this_arange > arange_end_section) {
            free_aranges_chain(dbg,head_chain);
            _dwarf_error(dbg, error,DW_DLE_ARANGES_HEADER_ERROR);
            return DW_DLV_ERROR;
        }
        if (!area_length) {
            /*  We read 4 bytes of zero, so area-length zero.
                Keep scanning. First seen Nov 27, 2018
                in GNU-cc in windows dll. */
            continue;
        }

        res = _dwarf_read_unaligned_ck_wrapper(dbg,&version,
            arange_ptr,DWARF_HALF_SIZE,end_this_arange,error);
        if (res != DW_DLV_OK) {
            free_aranges_chain(dbg,head_chain);
            return res;
        }
        arange_ptr += DWARF_HALF_SIZE;
        if (arange_ptr >= end_this_arange) {
            free_aranges_chain(dbg,head_chain);
            _dwarf_error(dbg, error, DW_DLE_ARANGES_HEADER_ERROR);
            return DW_DLV_ERROR;
        }
        if (version != DW_ARANGES_VERSION2) {
            free_aranges_chain(dbg,head_chain);
            _dwarf_error(dbg, error, DW_DLE_VERSION_STAMP_ERROR);
            return DW_DLV_ERROR;
        }
        res = _dwarf_read_unaligned_ck_wrapper(dbg,&info_offset,
            arange_ptr,local_length_size,end_this_arange,error);
        if (res != DW_DLV_OK) {
            free_aranges_chain(dbg,head_chain);
            return res;
        }

        arange_ptr += local_length_size;
        if (arange_ptr >= end_this_arange) {
            free_aranges_chain(dbg,head_chain);
            _dwarf_error(dbg, error, DW_DLE_ARANGES_HEADER_ERROR);
            return DW_DLV_ERROR;
        }
        /* This applies to debug_info only, not to debug_types. */
        if (info_offset >= dbg->de_debug_info.dss_size) {
            FIX_UP_OFFSET_IRIX_BUG(dbg, info_offset,
                "arange info offset.a");
            if (info_offset >= dbg->de_debug_info.dss_size) {
                free_aranges_chain(dbg,head_chain);
                _dwarf_error(dbg, error, DW_DLE_ARANGE_OFFSET_BAD);
                return DW_DLV_ERROR;
            }
        }

        address_size = *(Dwarf_Small *) arange_ptr;
        if (address_size  > sizeof(Dwarf_Addr)) {
            free_aranges_chain(dbg,head_chain);
            _dwarf_error(dbg, error, DW_DLE_ADDRESS_SIZE_ERROR);
            return DW_DLV_ERROR;
        }
        if (address_size  ==  0) {
            free_aranges_chain(dbg,head_chain);
            _dwarf_error(dbg, error, DW_DLE_ADDRESS_SIZE_ZERO);
            return DW_DLV_ERROR;
        }
        /*  It is not an error if the sizes differ.
            Unusual, but not an error. */
        arange_ptr = arange_ptr + sizeof(Dwarf_Small);

        /*  The following deref means we better
            check the pointer for off-end. */
        if (arange_ptr >= end_this_arange) {
            free_aranges_chain(dbg,head_chain);
            _dwarf_error(dbg, error, DW_DLE_ARANGE_OFFSET_BAD);
            return DW_DLV_ERROR;
        }

        /*  Even DWARF2 had a segment_sel_size field here,
            meaning
            size in bytes of a segment selector/descriptor
            on the target system.
            In reality it is unlikely any non-zero
            value will work sensibly for the user.  */
        segment_sel_size = *(Dwarf_Small *) arange_ptr;
        if (segment_sel_size > 0) {
            free_aranges_chain(dbg,head_chain);
            _dwarf_error_string(dbg, error,
                DW_DLE_SEGMENT_SIZE_BAD,
                "DW_DLE_SEGMENT_SIZE_BAD: "
                "segment selector size > 0 is not supported");
            return DW_DLV_ERROR;
        }
        arange_ptr = arange_ptr + sizeof(Dwarf_Small);
        /*  Code below will check for == end_this_arange
            as appropriate. */
        if (arange_ptr > end_this_arange) {
            free_aranges_chain(dbg,head_chain);
            _dwarf_error(dbg, error, DW_DLE_ARANGE_OFFSET_BAD);
            return DW_DLV_ERROR;
        }
        range_entry_size = 2*address_size + segment_sel_size;
        /*  Round arange_ptr offset to next multiple of
            address_size. */
        remainder = (Dwarf_Unsigned) ((arange_ptr - header_ptr) %
            (range_entry_size));
        if (remainder != 0) {
            arange_ptr = arange_ptr + (2 * address_size) - remainder;
        }

        do {
            Dwarf_Addr range_address = 0;
            Dwarf_Unsigned segment_selector = 0;
            Dwarf_Unsigned range_length = 0;
            /*  For segmented address spaces, the first field to
                read is a segment selector (new in DWARF4).
                The version number DID NOT CHANGE from 2, which
                is quite surprising.
                Also surprising since the segment_sel_size
                was always there
                in the table header! */
            /*  We want to test cu_version here
                and segment_sel_size, but
                currently with no way segment_sel_size
                can be other than zero.
                We just hope no one using
                segment_selectors, really. FIXME */
            res = _dwarf_read_unaligned_ck_wrapper(dbg,&range_address,
                arange_ptr,address_size,end_this_arange,error);
            if (res != DW_DLV_OK) {
                free_aranges_chain(dbg,head_chain);
                return res;
            }
            arange_ptr += address_size;

            res = _dwarf_read_unaligned_ck_wrapper(dbg,&range_length,
                arange_ptr,address_size,end_this_arange,error);
            if (res != DW_DLV_OK) {
                free_aranges_chain(dbg,head_chain);
                return res;
            }

            arange_ptr += address_size;

            {
                /*  We used to suppress all-zero entries, but
                    now we return all aranges entries so we show
                    the entire content.  March 31, 2010. */

                arange = (Dwarf_Arange)
                    _dwarf_get_alloc(dbg, DW_DLA_ARANGE, 1);
                if (arange == NULL) {
                    free_aranges_chain(dbg,head_chain);
                    _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
                    return DW_DLV_ERROR;
                }

                arange->ar_segment_selector = segment_selector;
                arange->ar_segment_selector_size =
                    segment_sel_size;
                arange->ar_address = range_address;
                arange->ar_length = range_length;
                arange->ar_info_offset = info_offset;
                arange->ar_dbg = dbg;
                arange_count++;

                curr_chain = (Dwarf_Chain)
                    _dwarf_get_alloc(dbg, DW_DLA_CHAIN, 1);
                if (curr_chain == NULL) {
                    dwarf_dealloc(dbg,arange,DW_DLA_ARANGE);
                    free_aranges_chain(dbg,head_chain);
                    _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
                    return DW_DLV_ERROR;
                }

                curr_chain->ch_item = arange;
                curr_chain->ch_itemtype = DW_DLA_ARANGE;
                (*plast) = curr_chain;
                plast = &(curr_chain->ch_next);
            }
            /*  The current set of ranges is terminated by
                range_address 0 and range_length 0, but that
                does not necessarily terminate the ranges for this CU!
                There can be multiple sets in that DWARF
                does not explicitly forbid multiple sets.
                DWARF2,3,4 section 7.20
                We stop short to avoid overrun of the
                end of the CU.  */
        } while (end_this_arange >= (arange_ptr + range_entry_size));

        /*  A compiler could emit some padding bytes here. dwarf2/3
            (dwarf4 sec 7.20) does not clearly make extra padding
            bytes illegal. */
        if (end_this_arange < arange_ptr) {
            Dwarf_Unsigned pad_count = arange_ptr - end_this_arange;
            Dwarf_Unsigned offset = arange_ptr - arange_ptr_start;
            dwarfstring aramsg;

            dwarfstring_constructor(&aramsg);
            /* Safe. Length strictly limited. */
            dwarfstring_append_printf_u(&aramsg,
                "DW_DLE_ARANGE_LENGTH_BAD."
                " 0x%" DW_PR_XZEROS DW_PR_DUx,
                pad_count);
            dwarfstring_append_printf_u(&aramsg,
                " pad bytes at offset 0x%" DW_PR_XZEROS DW_PR_DUx
                " in .debug_aranges",
                offset);
            dwarf_insert_harmless_error(dbg,
                dwarfstring_string(&aramsg));
            dwarfstring_destructor(&aramsg);
        }
        /*  For most compilers, arange_ptr == end_this_arange at
            this point. But not if there were padding bytes */
        arange_ptr = end_this_arange;
    } while (arange_ptr < arange_end_section);

    if (arange_ptr != arange_end_section) {
        free_aranges_chain(dbg,head_chain);
        _dwarf_error(dbg, error, DW_DLE_ARANGE_DECODE_ERROR);
        return DW_DLV_ERROR;
    }
    *chain_out = head_chain;
    *chain_count_out = arange_count;
    return DW_DLV_OK;
}

/*
    This function returns the count of the number of
    aranges in the .debug_aranges section.  It sets
    aranges to point to a block of Dwarf_Arange's
    describing the arange's.  It returns DW_DLV_ERROR
    on error.

    Must be identical in most aspects to
        dwarf_get_aranges_addr_offsets!

*/
int
dwarf_get_aranges(Dwarf_Debug dbg,
    Dwarf_Arange ** aranges,
    Dwarf_Signed * returned_count, Dwarf_Error * error)
{
    /* Count of total number of aranges. */
    Dwarf_Signed arange_count = 0;

    Dwarf_Arange *arange_block = 0;

    /* Used to chain Dwarf_Aranges structs. */
    Dwarf_Chain curr_chain = NULL;
    Dwarf_Chain head_chain = NULL;
    Dwarf_Signed i = 0;
    int res = DW_DLV_ERROR;

    /* ***** BEGIN CODE ***** */

    CHECK_DBG(dbg,error,"dwarf_get_aranges()");
    res = _dwarf_load_section(dbg, &dbg->de_debug_aranges, error);
    if (res != DW_DLV_OK) {
        return res;
    }
    /*  aranges points in to info, so if info needs expanding
        we have to load it. */
    res = _dwarf_load_debug_info(dbg, error);
    if (res != DW_DLV_OK) {
        return res;
    }

    res = _dwarf_get_aranges_list(dbg,&head_chain,
        &arange_count,error);
    if (res != DW_DLV_OK) {
        free_aranges_chain(dbg,head_chain);
        return res;
    }

    arange_block = (Dwarf_Arange *)
        _dwarf_get_alloc(dbg, DW_DLA_LIST, arange_count);
    if (arange_block == NULL) {
        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
        free_aranges_chain(dbg,head_chain);
        return DW_DLV_ERROR;
    }

    /* See also free_aranges_chain() above */
    curr_chain = head_chain;
    for (i = 0; i < arange_count; i++) {
        Dwarf_Chain prev = 0;

        /*  Copies pointers. No dealloc of ch_item, */
        *(arange_block + i) = curr_chain->ch_item;
        curr_chain->ch_item = 0;
        prev = curr_chain;
        curr_chain = curr_chain->ch_next;
        dwarf_dealloc(dbg, prev, DW_DLA_CHAIN);
    }
    *aranges = arange_block;
    *returned_count = (arange_count);
    return DW_DLV_OK;
}

#if 0 /* _dwarf_get_aranges_addr_offsets Unused */
/*
    This function returns DW_DLV_OK if it succeeds
    and DW_DLV_ERR or DW_DLV_OK otherwise.
    count is set to the number of addresses in the
    .debug_aranges section.
    For each address, the corresponding element in
    an array is set to the address itself(aranges) and
    the section offset (offsets).
    Must be identical in most aspects to
        dwarf_get_aranges!
*/
int
_dwarf_get_aranges_addr_offsets(Dwarf_Debug dbg,
    Dwarf_Addr ** addrs,
    Dwarf_Off ** offsets,
    Dwarf_Signed * count,
    Dwarf_Error * error)
{
    Dwarf_Signed i = 0;

    /* Used to chain Dwarf_Aranges structs. */
    Dwarf_Chain curr_chain = NULL;
    Dwarf_Chain head_chain = NULL;

    Dwarf_Signed arange_count = 0;
    Dwarf_Addr *arange_addrs = 0;
    Dwarf_Off *arange_offsets = 0;

    int res = DW_DLV_ERROR;

    /* ***** BEGIN CODE ***** */

    if (error != NULL)
        *error = NULL;

    if (IS_INVALID_DBG(dbg)) {
        _dwarf_error(NULL, error, DW_DLE_DBG_NULL);
        return DW_DLV_ERROR;
    }

    res = _dwarf_load_section(dbg, &dbg->de_debug_aranges,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    /*  aranges points in to info, so if info needs expanding
        we have to load it. */
    res = _dwarf_load_debug_info(dbg, error);
    if (res != DW_DLV_OK) {
        return res;
    }
    res = _dwarf_get_aranges_list(dbg,&head_chain,
        &arange_count,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    arange_addrs = (Dwarf_Addr *)
        _dwarf_get_alloc(dbg, DW_DLA_ADDR, arange_count);
    if (arange_addrs == NULL) {
        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
        return DW_DLV_ERROR;
    }
    arange_offsets = (Dwarf_Off *)
        _dwarf_get_alloc(dbg, DW_DLA_ADDR, arange_count);
    if (arange_offsets == NULL) {
        free_aranges_chain(dbg,head_chain);
        dwarf_dealloc(dbg,arange_addrs,DW_DLA_ADDR);
        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
        return DW_DLV_ERROR;
    }

    curr_chain = head_chain;
    for (i = 0; i < arange_count; i++) {
        Dwarf_Arange ar = curr_chain->ch_item;
        int itemtype = curr_chain->ch_itemtype;
        Dwarf_Chain prev = 0;

        if (!ar) {
            arange_addrs[i] = 0;
            arange_offsets[i] = 0;
            continue;
        }
        curr_chain->ch_item = 0;
        arange_addrs[i] = ar->ar_address;
        arange_offsets[i] = ar->ar_info_offset;
        prev = curr_chain;
        curr_chain = curr_chain->ch_next;
        if (itemtype) {
            dwarf_dealloc(dbg, ar, itemtype);
        }
        dwarf_dealloc(dbg, prev, DW_DLA_CHAIN);
    }
    *count = arange_count;
    *offsets = arange_offsets;
    *addrs = arange_addrs;
    return DW_DLV_OK;
}
#endif /* 0 */

/*
    This function takes a pointer to a block
    of Dwarf_Arange's, and a count of the
    length of the block.  It checks if the
    given address is within the range of an
    address range in the block.  If yes, it
    returns the appropriate Dwarf_Arange.
    If no, it returns DW_DLV_NO_ENTRY;
    On error it returns DW_DLV_ERROR.
*/
int
dwarf_get_arange(Dwarf_Arange * aranges,
    Dwarf_Unsigned arange_count,
    Dwarf_Addr address,
    Dwarf_Arange * returned_arange, Dwarf_Error * error)
{
    Dwarf_Arange curr_arange = 0;
    Dwarf_Unsigned i = 0;

    if (!aranges) {
        _dwarf_error(NULL, error, DW_DLE_ARANGES_NULL);
        return DW_DLV_ERROR;
    }
    for (i = 0; i < arange_count; i++) {
        curr_arange = *(aranges + i);
        if (address >= curr_arange->ar_address &&
            address <
            curr_arange->ar_address + curr_arange->ar_length) {
            *returned_arange = curr_arange;
            return DW_DLV_OK;
        }
    }
    return DW_DLV_NO_ENTRY;
}

/*
    This function takes an Dwarf_Arange,
    and returns the offset of the first
    die in the compilation-unit that the
    arange belongs to.  Returns DW_DLV_ERROR
    on error.

    For an arange, the cu_die can only be from debug_info,
    not debug_types, it seems.
*/
int
dwarf_get_cu_die_offset(Dwarf_Arange arange,
    Dwarf_Off * returned_offset,
    Dwarf_Error * error)
{
    Dwarf_Debug dbg = 0;
    Dwarf_Off offset = 0;
    Dwarf_Unsigned headerlen = 0;
    int cres = 0;

    if (arange == NULL) {
        _dwarf_error(NULL, error, DW_DLE_ARANGE_NULL);
        return DW_DLV_ERROR;
    }
    dbg = arange->ar_dbg;
    offset = arange->ar_info_offset;
    /* This applies to debug_info only, not to debug_types. */
    if (!dbg->de_debug_info.dss_data) {
        int res = _dwarf_load_debug_info(dbg, error);

        if (res != DW_DLV_OK) {
            return res;
        }
    }
    cres = _dwarf_length_of_cu_header(dbg, offset,
        TRUE, &headerlen,error);
    if (cres != DW_DLV_OK) {
        return cres;
    }
    *returned_offset =  headerlen + offset;
    return DW_DLV_OK;
}

/*  This function takes an Dwarf_Arange,
    and returns the offset of the CU header
    in the compilation-unit that the
    arange belongs to.  Returns DW_DLV_ERROR
    on error.
    Ensures .debug_info loaded so
    the cu_offset is meaningful.  */
int
dwarf_get_arange_cu_header_offset(Dwarf_Arange arange,
    Dwarf_Off * cu_header_offset_returned,
    Dwarf_Error * error)
{
    Dwarf_Debug dbg = 0;
    if (arange == NULL) {
        _dwarf_error(NULL, error, DW_DLE_ARANGE_NULL);
        return DW_DLV_ERROR;
    }
    dbg = arange->ar_dbg;
    /* This applies to debug_info only, not to debug_types. */
    /*  Like dwarf_get_arange_info_b() this ensures debug_info loaded:
        the cu_header is in debug_info and will be used else
        we would not call dwarf_get_arange_cu_header_offset. */
    if (!dbg->de_debug_info.dss_data) {
        int res = _dwarf_load_debug_info(dbg, error);
        if (res != DW_DLV_OK) {
            return res;
        }
    }
    *cu_header_offset_returned = arange->ar_info_offset;
    return DW_DLV_OK;
}

/*
    This function takes a Dwarf_Arange, and returns
    TRUE if it is not NULL.  It also stores the start
    address of the range in *start, the length of the
    range in *length, and the offset of the first die
    in the compilation-unit in *cu_die_offset.  It
    returns FALSE on error.
    If cu_die_offset returned ensures .debug_info loaded so
    the cu_die_offset is meaningful.

    New for DWARF4, entries may have segment information.
    *segment is only meaningful
    if *segment_entry_size is non-zero.
    But segment_selectors are not fully defined so
    a non-zero segment_entry_size is not actually
    usable. */
int
dwarf_get_arange_info_b(Dwarf_Arange arange,
    Dwarf_Unsigned*  segment,
    Dwarf_Unsigned*  segment_entry_size,
    Dwarf_Addr    * start,
    Dwarf_Unsigned* length,
    Dwarf_Off     * cu_die_offset,
    Dwarf_Error   * error)
{
    if (arange == NULL) {
        _dwarf_error(NULL, error, DW_DLE_ARANGE_NULL);
        return DW_DLV_ERROR;
    }

    if (segment != NULL) {
        *segment = arange->ar_segment_selector;
    }
    if (segment_entry_size != NULL) {
        *segment_entry_size = arange->ar_segment_selector_size;
    }
    if (start != NULL)
        *start = arange->ar_address;
    if (length != NULL)
        *length = arange->ar_length;
    if (cu_die_offset != NULL) {
        Dwarf_Debug dbg = arange->ar_dbg;
        Dwarf_Off offset = arange->ar_info_offset;
        Dwarf_Unsigned headerlen = 0;
        int cres = 0;

        /* This applies to debug_info only, not to debug_types. */
        if (!dbg->de_debug_info.dss_data) {
            int res = _dwarf_load_debug_info(dbg, error);
            if (res != DW_DLV_OK) {
                return res;
            }
        }
        cres = _dwarf_length_of_cu_header(dbg, offset,
            TRUE, &headerlen,error);
        if (cres != DW_DLV_OK) {
            return cres;
        }
        *cu_die_offset = offset + headerlen;

    }
    return DW_DLV_OK;
}
