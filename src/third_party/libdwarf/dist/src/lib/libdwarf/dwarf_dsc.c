/*
  Copyright (C) 2016-2020 David Anderson. All Rights Reserved.

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

#include <stdlib.h> /* calloc() free() */
#include <string.h> /* memcpy() */

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
#include "dwarf_dsc.h"

/*  When called with ary and *arraycount 0
    this just counts the elements found.
    Otherwise it records the values in ary and
    recounts. The arraycount pointer must be
    passed-in non-null always. */
static int
get_dsc_leb_entries(Dwarf_Debug dbg,
    Dwarf_Small    * blockpointer,
    Dwarf_Unsigned   blocklen,
    int dounsigned,
    struct Dwarf_Dsc_Entry_s *ary,
    size_t         * arraycount,
    Dwarf_Error    * error)
{
    Dwarf_Small *p = blockpointer;
    Dwarf_Small *endp = blockpointer + blocklen;
    size_t larraycount = 0;
    size_t iarraycount = *arraycount;

    if (!ary) {
        if (iarraycount) {
            /* Internal botch calling this static function. */
            _dwarf_error(dbg, error, DW_DLE_DISCR_ARRAY_ERROR);
            return DW_DLV_ERROR;
        } else {}
    } else {
        if (!iarraycount) {
            /* Internal botch calling this static function. */
            _dwarf_error(dbg, error, DW_DLE_DISCR_ARRAY_ERROR);
            return DW_DLV_ERROR;
        } else {}
    }
    if (dounsigned) {
        while (p < endp)  {
            Dwarf_Unsigned dsc = 0;
            Dwarf_Unsigned low = 0;
            Dwarf_Unsigned high = 0;

            if (ary && (larraycount >= iarraycount)) {
                _dwarf_error(dbg, error, DW_DLE_DISCR_ARRAY_ERROR);
                return DW_DLV_ERROR;
            }
            DECODE_LEB128_UWORD_CK(p,dsc,
                dbg,error,endp);
            if (!dsc) {
                DECODE_LEB128_UWORD_CK(p,low,
                    dbg,error,endp);
            } else {
                DECODE_LEB128_UWORD_CK(p,low,
                    dbg,error,endp);
                DECODE_LEB128_UWORD_CK(p,high,
                    dbg,error,endp);
            }
            if (ary) {
                struct Dwarf_Dsc_Entry_s *arye =
                    ary+larraycount;

                /*  type reads the same as uleb and leb because
                    it is only zero or one. */
                arye->dsc_type = (Dwarf_Half)dsc;
                arye->dsc_low_u = low;
                arye->dsc_high_u = high;
            }
            larraycount++;
        }
    } else {
        while (p < endp)  {
            Dwarf_Signed dsc = 0;
            Dwarf_Signed low = 0;
            Dwarf_Signed high = 0;
            Dwarf_Unsigned leblen = 0;

            (void)leblen;
            if (ary && (larraycount >= iarraycount)) {
                _dwarf_error(dbg, error, DW_DLE_DISCR_ARRAY_ERROR);
                return DW_DLV_ERROR;
            }
            DECODE_LEB128_SWORD_LEN_CK(p,dsc,
                leblen,dbg,error,endp);
            if (!dsc) {
                DECODE_LEB128_SWORD_LEN_CK(p,low,
                    leblen,dbg,error,endp);
            } else {
                DECODE_LEB128_SWORD_LEN_CK(p,low,
                    leblen,dbg,error,endp);
                DECODE_LEB128_SWORD_LEN_CK(p,high,
                    leblen,dbg,error,endp);
            }
            if (ary) {
                struct Dwarf_Dsc_Entry_s *arye =
                    ary+larraycount;

                /*  type reads the same as uleb and leb because
                    it is only zero or one. */
                arye->dsc_type = (Dwarf_Half)dsc;
                arye->dsc_low_s = low;
                arye->dsc_high_s = high;
            }
            larraycount++;
        }
    }
    if (ary) {
        /*  Just verify this recount matches original */
        if (iarraycount != larraycount) {
            _dwarf_error(dbg, error, DW_DLE_DISCR_ARRAY_ERROR);
            return DW_DLV_ERROR;
        }
    } else {
        /*  This matters for first call with
            ary 0 and iarraycount 0 as we are generating the
            count. */
        *arraycount = larraycount;
    }
    return DW_DLV_OK;
}

int dwarf_discr_list(Dwarf_Debug dbg,
    Dwarf_Small    * blockpointer,
    Dwarf_Unsigned   blocklen,
    Dwarf_Dsc_Head * dsc_head_out,
    Dwarf_Unsigned * dsc_array_length_out,
    Dwarf_Error    * error)
{
    Dwarf_Dsc_Head h = 0;
    int res = 0;
    size_t arraycount = 0;
    struct Dwarf_Dsc_Entry_s *ary = 0;
    Dwarf_Small * dscblockp = 0;
    Dwarf_Unsigned dscblocklen = 0;

    CHECK_DBG(dbg,error,"dwarf_discr_list()");
    if (blocklen == 0) {
        return DW_DLV_NO_ENTRY;
    }
    dscblockp = (Dwarf_Small *)calloc(blocklen,sizeof(Dwarf_Small));
    if (!dscblockp) {
        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
        return DW_DLV_ERROR;
    }
    dscblocklen = blocklen;
    memcpy(dscblockp,blockpointer,blocklen);

    res = get_dsc_leb_entries(dbg,dscblockp,dscblocklen,
        /*  TRUE or FALSE here is not important, the arraycount
            returned to us will be identical either way. */
        FALSE, 0, &arraycount,error);
    if (res != DW_DLV_OK) {
        free(dscblockp);
        return res;
    }

    h = (Dwarf_Dsc_Head)_dwarf_get_alloc(dbg,DW_DLA_DSC_HEAD,1);
    if (!h) {
        free(dscblockp);
        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
        return DW_DLV_ERROR;
    }

    h->dsh_block = dscblockp;
    h->dsh_block_len = dscblocklen;
    h->dsh_debug = dbg;
    /* Now the destructor for h will deal with block malloc space. */

    ary = (struct Dwarf_Dsc_Entry_s *)calloc(arraycount,
        sizeof(struct Dwarf_Dsc_Entry_s));
    if (!ary) {
        /*  Coverity scan CID 531838. Mem leak.
            Free the block h points to here too. */
        free(h->dsh_block);
        h->dsh_block = 0;
        h->dsh_block_len = 0;
        dscblockp = 0;
        dwarf_dealloc(dbg,h,DW_DLA_DSC_HEAD);
        _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
        return DW_DLV_ERROR;
    }
    h->dsh_count = arraycount;
    h->dsh_array = ary;
    h->dsh_set_unsigned = 0;
    h->dsh_set_signed = 0;
    *dsc_head_out = h;
    *dsc_array_length_out = arraycount;
    return DW_DLV_OK;
}

/*  NEW September 2016. Allows easy access to DW_AT_discr_list
    entry. Callers must know which is the appropriate
    one of the following two interfaces, though both
    will work. */
int
dwarf_discr_entry_u(Dwarf_Dsc_Head  dsh ,
    Dwarf_Unsigned   entrynum,
    Dwarf_Half     * out_type,
    Dwarf_Unsigned * out_discr_low,
    Dwarf_Unsigned * out_discr_high,
    Dwarf_Error    * error)
{
    struct Dwarf_Dsc_Entry_s *dse = 0;

    (void)error;
    if (entrynum >= dsh->dsh_count) {
        return DW_DLV_NO_ENTRY;
    }
    if (!dsh->dsh_set_unsigned) {
        int res =0;
        int dounsigned = 1;
        size_t count = dsh->dsh_count;

        res = get_dsc_leb_entries(dsh->dsh_debug,
            dsh->dsh_block,
            dsh->dsh_block_len,
            dounsigned,
            dsh->dsh_array,
            &count,
            error);
        if (res != DW_DLV_OK) {
            return res;
        }
        dsh->dsh_set_unsigned = TRUE;
    }
    if (!dsh->dsh_array) {
        _dwarf_error(dsh->dsh_debug, error, DW_DLE_DISCR_ARRAY_ERROR);
        return DW_DLV_ERROR;
    }
    dse = dsh->dsh_array + entrynum;
    *out_type       = dse->dsc_type;
    *out_discr_low  = dse->dsc_low_u;
    *out_discr_high = dse->dsc_high_u;
    return DW_DLV_OK;
}

/*  NEW September 2016. Allows easy access to DW_AT_discr_list
    entry. */
int
dwarf_discr_entry_s(Dwarf_Dsc_Head  dsh,
    Dwarf_Unsigned   entrynum,
    Dwarf_Half     * out_type,
    Dwarf_Signed   * out_discr_low,
    Dwarf_Signed   * out_discr_high,
    Dwarf_Error    * error)
{
    struct Dwarf_Dsc_Entry_s *dse = 0;

    (void)error;
    if (entrynum >= dsh->dsh_count) {
        return DW_DLV_NO_ENTRY;
    }
    if (!dsh->dsh_set_signed) {
        int res =0;
        int dounsigned = 0;
        size_t count = dsh->dsh_count;

        res = get_dsc_leb_entries(dsh->dsh_debug,
            dsh->dsh_block,
            dsh->dsh_block_len,
            dounsigned,
            dsh->dsh_array,
            &count,
            error);
        if (res != DW_DLV_OK) {
            return res;
        }
        dsh->dsh_set_signed = TRUE;
    }
    if (!dsh->dsh_array) {
        _dwarf_error(dsh->dsh_debug, error, DW_DLE_DISCR_ARRAY_ERROR);
        return DW_DLV_ERROR;
    }
    dse = dsh->dsh_array + entrynum;
    *out_type       = dse->dsc_type;
    *out_discr_low  = dse->dsc_low_s;
    *out_discr_high = dse->dsc_high_s;
    return DW_DLV_OK;
}

void
_dwarf_dsc_destructor(void *m)
{
    Dwarf_Dsc_Head h = (Dwarf_Dsc_Head) m;

    free(h->dsh_array);
    h->dsh_array = 0;
    free(h->dsh_block);
    h->dsh_block = 0;
    h->dsh_count = 0;
}
