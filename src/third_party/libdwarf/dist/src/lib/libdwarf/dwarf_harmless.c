/*
  Copyright (C) 2010-2022 David Anderson. All Rights Reserved.
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

/*  This  implements _dwarf_insert_harmless_error
    and related helper functions for recording
    compiler errors that need not make the input
    unusable.

    Applications can use dwarf_get_harmless_error_list to
    find (and possibly print) a warning about such errors.

    The initial error reported here is
    DW_DLE_DEBUG_FRAME_LENGTH_NOT_MULTIPLE which was a
    bug in a specific compiler.

    It is a fixed length circular list to constrain
    the space used for errors.

    The assumption is that these errors are exceedingly
    rare, and indicate a broken compiler (the one that
    produced the object getting the error(s)).

    dh_maxcount is recorded internally as 1 greater than
    requested.  Hiding the fact we always leave one
    slot unused (at least).   So a user request for
    N slots really gives the user N usable slots.  */

#include <config.h>

#include <stddef.h>  /* size_t */
#include <stdlib.h>  /* free() malloc() */
#include <string.h>  /* memcpy() strcpy() strlen() */

#if defined(_WIN32) && defined(HAVE_STDAFX_H)
#include "stdafx.h"
#endif /* HAVE_STDAFX_H */

#include "dwarf.h"
#include "libdwarf.h"
#include "dwarf_local_malloc.h"
#include "libdwarf_private.h"
#include "dwarf_base_types.h"
#include "dwarf_safe_strcpy.h"
#include "dwarf_opaque.h"
#include "dwarf_util.h"
#include "dwarf_frame.h"
#include "dwarf_harmless.h"

/*  Not user configurable. */
#define DW_HARMLESS_ERROR_MSG_STRING_SIZE 300

/*  The pointers returned here through errmsg_ptrs_array
    become invalidated by any call to libdwarf. Any call.
*/
int
dwarf_get_harmless_error_list(Dwarf_Debug dbg,
    unsigned  count,
    const char ** errmsg_ptrs_array,
    unsigned * errs_count)
{

    struct Dwarf_Harmless_s *dhp = 0;

    if (IS_INVALID_DBG(dbg)) {
        return DW_DLV_NO_ENTRY;
    }
    dhp = &dbg->de_harmless_errors;
    if (!dhp->dh_errors) {
        dhp->dh_errs_count = 0;
        return DW_DLV_NO_ENTRY;
    }
    if (dhp->dh_errs_count == 0) {
        return DW_DLV_NO_ENTRY;
    }
    if (errs_count) {
        *errs_count = dhp->dh_errs_count;
    }
    if (count) {
        /* NULL terminate the array of pointers */
        --count;
        errmsg_ptrs_array[count] = 0;

        if (dhp->dh_next_to_use != dhp->dh_first) {
            unsigned i = 0;
            unsigned cur = dhp->dh_first;
            for (i = 0;  cur != dhp->dh_next_to_use;
                ++i, cur = (cur +1) % dhp->dh_maxcount) {
                if (i >= count ) {
                    /* All output spaces are used. */
                    break;
                }
                errmsg_ptrs_array[i] = dhp->dh_errors[cur];
            }
            errmsg_ptrs_array[i] = 0;
        }
    }
    dhp->dh_next_to_use = 0;
    dhp->dh_first = 0;
    dhp->dh_errs_count = 0;
    return DW_DLV_OK;
}

/*  Insertion made public is only for testing the harmless error code,
    it is not necessarily useful for libdwarf client code aside
    from code testing libdwarf. */
void
dwarf_insert_harmless_error(Dwarf_Debug dbg,
    char *newerror)
{
    struct Dwarf_Harmless_s *dhp = 0;
    unsigned next = 0;
    unsigned cur = 0;
    char *msgspace = 0;

    if (IS_INVALID_DBG(dbg)) {
        return;
    }
    dhp = &dbg->de_harmless_errors;
    cur = dhp->dh_next_to_use;
    if (!dhp->dh_errors) {
        dhp->dh_errs_count++;
        return;
    }
    msgspace = dhp->dh_errors[cur];
    _dwarf_safe_strcpy(msgspace,
        DW_HARMLESS_ERROR_MSG_STRING_SIZE,
        newerror,
        strlen(newerror));
    next = (cur+1) % dhp->dh_maxcount;
    dhp->dh_errs_count++;
    dhp->dh_next_to_use = next;
    if (dhp->dh_next_to_use ==  dhp->dh_first) {
        /* Array is full set full invariant. */
        dhp->dh_first = (dhp->dh_first+1) % dhp->dh_maxcount;
    }
}

/*  The size of the circular list of strings may be set
    and reset as desired. Returns the previous size of
    the list. If the list is shortened excess error entries
    are simply dropped.
    If the reallocation fails the list size is left unchanged.
    Do not make this a long list!

    Remember the maxcount we record is 1 > the user count,
    so we adjust it so it looks like the user count.
*/
unsigned
dwarf_set_harmless_error_list_size(Dwarf_Debug dbg,
    unsigned maxcount )
{
    struct Dwarf_Harmless_s *dhp = 0;
    unsigned prevcount = 0;

    if (IS_INVALID_DBG(dbg)) {
        return 0;
    }
    dhp = &dbg->de_harmless_errors;
    prevcount = dhp->dh_maxcount;
    if (maxcount != 0) {
        ++maxcount;
        if (maxcount != dhp->dh_maxcount) {
            /*  Assign transfers 'ownership' of the malloc areas
                to oldarray. */
            struct Dwarf_Harmless_s oldarray = *dhp;
            /*  Do not double increment the max, the init() func
                increments it too. */
            _dwarf_harmless_init(dhp,maxcount-1);
            if (oldarray.dh_next_to_use != oldarray.dh_first) {
                unsigned i = 0;
                for (i = oldarray.dh_first;
                    i != oldarray.dh_next_to_use;
                    i = (i+1)%oldarray.dh_maxcount) {
                    dwarf_insert_harmless_error(dbg,
                        oldarray.dh_errors[i]);
                }
                if (oldarray.dh_errs_count > dhp->dh_errs_count) {
                    dhp->dh_errs_count = oldarray.dh_errs_count;
                }
            }
            _dwarf_harmless_cleanout(&oldarray);
        }
    }
    return prevcount-1;
}

/*  Only callable from within libdwarf (as a practical matter)
*/
void
_dwarf_harmless_init(struct Dwarf_Harmless_s *dhp,unsigned size)
{
    unsigned i = 0;
    memset(dhp,0,sizeof(*dhp));
    dhp->dh_maxcount = size +1;
    dhp->dh_errors = (char **)calloc(dhp->dh_maxcount,
        sizeof(char *));
    if (!dhp->dh_errors) {
        dhp->dh_maxcount = 0;
        return;
    }
    for (i = 0; i < dhp->dh_maxcount; ++i) {
        char *newstr =
            (char *)calloc(1, DW_HARMLESS_ERROR_MSG_STRING_SIZE);
        dhp->dh_errors[i] = newstr;
#if 0 /* Commentary about avoiding leak */
        /*  BAD IDEA. just use the NULL pointer,
            so we avoid problems later with
            freeing.  */
        if (!newstr) {
            dhp->dh_maxcount = 0;
            /* Let it leak, the leak is a constrained amount. */
            free(dhp->dh_errors);
            dhp->dh_errors = 0;
            return;
        }
#endif /* 0 */
        dhp->dh_errors[i] = newstr;
    }
}

void
_dwarf_harmless_cleanout(struct Dwarf_Harmless_s *dhp)
{
    unsigned i = 0;
    if (!dhp->dh_errors) {
        return;
    }
    for (i = 0; i < dhp->dh_maxcount; ++i) {
        free(dhp->dh_errors[i]);
        dhp->dh_errors[i] = 0;
    }
    free(dhp->dh_errors);
    dhp->dh_errors = 0;
    dhp->dh_maxcount = 0;
}
