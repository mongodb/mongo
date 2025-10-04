/*

  Copyright (C) 2015-2015 David Anderson. All Rights Reserved.

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

#include <stdio.h>  /* printf() */
#include <stdlib.h> /* calloc() free() */
#include <string.h> /* memcpy() memset() */

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
#include "dwarf_tsearch.h"
#include "dwarf_tied_decls.h"

#ifdef DEBUG_PRIMARY_DBG /*debug dumpsignature */
void
_dwarf_dumpsig(const char *msg, Dwarf_Sig8 *sig,int lineno)
{
    const char *sigv = 0;
    unsigned u = 0;

    printf("%s 0x",msg);
    sigv = &sig->signature[0];
    for (u = 0; u < 8; u++) {
        printf("%02x",0xff&sigv[u]);
    }
    printf(" line %d\n",lineno);
}
#endif

void *
_dwarf_tied_make_entry(Dwarf_Sig8 *key, Dwarf_CU_Context val)
{
    struct Dwarf_Tied_Entry_s *e = 0;
    e = calloc(1,sizeof(struct Dwarf_Tied_Entry_s));
    if (e) {
        e->dt_key =    *key;
        e->dt_context = val;
    }
    return e;
}

/*  Tied data Key is Dwarf_Sig8.
    A hash needed because we are using a hash search
    here. Would not be needed for the other tree searchs
    like balanced trees..  */
DW_TSHASHTYPE
_dwarf_tied_data_hashfunc(const void *keyp)
{
    const struct Dwarf_Tied_Entry_s * enp = keyp;
    DW_TSHASHTYPE hashv = 0;
    /* Just take some of the 8 bytes of the signature. */
    memcpy(&hashv,enp->dt_key.signature,sizeof(hashv));
    return hashv;
}

int
_dwarf_tied_compare_function(const void *l, const void *r)
{
    const struct Dwarf_Tied_Entry_s * lp = l;
    const struct Dwarf_Tied_Entry_s * rp = r;
    const char *lcp = (const char *)&lp->dt_key.signature;
    const char *rcp = (const char *)&rp->dt_key.signature;
    const char *lcpend = lcp + sizeof(Dwarf_Sig8);

    for ( ;lcp < lcpend; ++lcp,++rcp) {
        if (*lcp < *rcp) {
            return -1;
        }
        if (*lcp > *rcp) {
            return 1;
        }
    }
    /* match. */
    return 0;
}

void
_dwarf_tied_destroy_free_node(void*nodep)
{
    struct Dwarf_Tied_Entry_s * enp = nodep;
    free(enp);
    return;
}

/*  This presumes only we are reading the debug_info
    CUs from tieddbg. That is a reasonable
    requirement, one hopes.
    Currently it reads all the tied CUs at once up to
    the point of finding a match unless there is an error..
    This the only way we call _dwarf_next_cu_header*( )
    on the tied file, so safe.
    */
static int
_dwarf_loop_reading_debug_info_for_cu(
    Dwarf_Debug tieddbg,
    struct Dwarf_Tied_Entry_s *targsig,
    Dwarf_Error *error)
{
    /*  We will not find tied signatures
        for .debug_addr (or line tables) in .debug_types.
        it seems. Those signatures point from
        'normal' to 'dwo/dwp'  (DWARF4) */
    int is_info = TRUE;
    Dwarf_Unsigned next_cu_offset = 0;

    for (;;) {
        int sres = DW_DLV_OK;
        Dwarf_Half cu_type = 0;
        Dwarf_CU_Context latestcontext = 0;
        Dwarf_Unsigned cu_header_length = 0;
        Dwarf_Unsigned abbrev_offset = 0;
        Dwarf_Half version_stamp = 0;
        Dwarf_Half address_size = 0;
        Dwarf_Half extension_size = 0;
        Dwarf_Half length_size = 0;
        Dwarf_Sig8 signature;
        Dwarf_Bool has_signature = FALSE;
        Dwarf_Unsigned typeoffset = 0;

        memset(&signature,0,sizeof(signature));
        sres = _dwarf_next_cu_header_internal(tieddbg,
            is_info,
            /* no CU die wanted*/ NULL,
            &cu_header_length, &version_stamp,
            &abbrev_offset, &address_size,
            &length_size,&extension_size,
            &signature, &has_signature,
            &typeoffset,
            &next_cu_offset,
            &cu_type, error);
        if (sres == DW_DLV_ERROR) {
            return sres;
        }
        if (sres == DW_DLV_NO_ENTRY) {
            break;
        }

        latestcontext = tieddbg->de_info_reading.de_cu_context;

        if (has_signature) {
            void      *retval = 0;
            Dwarf_Sig8 consign;
            void      *entry = 0;

            if (!latestcontext) {
                /* FAILED might be out of memory.*/
                return DW_DLV_NO_ENTRY;
            }
            consign = latestcontext->cc_signature;
            entry = _dwarf_tied_make_entry(&consign,latestcontext);
            if (!entry) {
                return DW_DLV_NO_ENTRY;
            }
            /* Insert this signature and context. */
            retval = dwarf_tsearch(entry,
                &tieddbg->de_tied_data.td_tied_search,
                _dwarf_tied_compare_function);
            if (!retval) {
                free(entry);
                /* FAILED might be out of memory.*/
                return DW_DLV_NO_ENTRY;
            } else {
                struct Dwarf_Tied_Data_s * retent =
                    *(struct Dwarf_Tied_Data_s**) retval;
                if (retent == entry) {
                    /*  we added a record. */
                    int res = _dwarf_tied_compare_function(
                        targsig,entry);
                    if (!res) {
                        /* Found match,  stop looping */
                        return DW_DLV_OK;
                    }
                    continue;
                } else {
                    /*  found existing, no add */
                    free(entry);
                    continue;
                }
            }
        }
    }
    /*  Apparently we never found the sig we are looking for.
        Pretend ok.  Caller will check for success. */
    return DW_DLV_OK;
}

/*  If out of memory just return DW_DLV_NO_ENTRY.
    This ensures all the tied CU contexts have been
    created though the caller has most likely
    never tried to read CUs in the tied-file.
*/
int
_dwarf_search_for_signature(Dwarf_Debug tieddbg,
    Dwarf_Sig8 sig,
    Dwarf_CU_Context *context_out,
    Dwarf_Error *error)
{

    void *entry2 = 0;
    struct Dwarf_Tied_Entry_s entry;
    struct Dwarf_Tied_Data_s * tied = &tieddbg->de_tied_data;
    int res = 0;

    if (!tied->td_tied_search) {
        dwarf_initialize_search_hash(&tied->td_tied_search,
            _dwarf_tied_data_hashfunc,0);
        if (!tied->td_tied_search) {
            return DW_DLV_NO_ENTRY;
        }
    }
    entry.dt_key = sig;
    entry.dt_context = 0;
    entry2 = dwarf_tfind(&entry,
        &tied->td_tied_search,
        _dwarf_tied_compare_function);
    if (entry2) {
        struct Dwarf_Tied_Entry_s *e2 =
            *(struct Dwarf_Tied_Entry_s **)entry2;
        *context_out = e2->dt_context;
        return DW_DLV_OK;
    }
    /*  We now ensure all tieddbg CUs signatures
        are in the td_tied_search,
        The caller is NOT doing
        info section read operations
        on the tieddbg in this (tied)dbg, so it
        cannot goof up their _dwarf_next_cu_header*().  */
    res  = _dwarf_loop_reading_debug_info_for_cu(tieddbg,&entry,
        error);
    if (res == DW_DLV_ERROR) {
        return res;
    }
    entry2 = dwarf_tfind(&entry,
        &tied->td_tied_search,
        _dwarf_tied_compare_function);
    if (entry2) {
        struct Dwarf_Tied_Entry_s *e2 =
            *(struct Dwarf_Tied_Entry_s **)entry2;
        *context_out = e2->dt_context;
        return DW_DLV_OK;
    }
    return DW_DLV_NO_ENTRY;
}
