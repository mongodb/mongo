/*

  Copyright (C) 2000-2005 Silicon Graphics, Inc. All Rights Reserved.
  Portions Copyright (C) 2008-2020 David Anderson.  All Rights Reserved.

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

#include <stdio.h>  /* stderr fflush() fprintf() */
#include <stdlib.h> /* calloc() */

#if defined(_WIN32) && defined(HAVE_STDAFX_H)
#include "stdafx.h"
#endif /* HAVE_STDAFX_H */

#include "dwarf.h"
#include "libdwarf.h"
#include "dwarf_local_malloc.h"
#include "libdwarf_private.h"
#include "dwarf_base_types.h"
#include "dwarf_opaque.h"
#include "dwarf_util.h"
#include "dwarf_alloc.h"
#include "dwarf_string.h"
#include "dwarf_error.h"

#undef DEBUG

/* Array to hold string representation of errors. Any time a
   define is added to the list in libdwarf.h, a string should be
   added to this Array
*/
#include "dwarf_errmsg_list.h"

/*  This function performs error handling as described in the
    libdwarf consumer document section 3.  Dbg is the Dwarf_debug
    structure being processed.  Error is a pointer to the pointer
    to the error descriptor that will be returned.  Errval is an
    error code listed in dwarf_error.h.

    If the malloc arena is exhausted we return a pointer to
    a special static error record.  This special singleton
    is mostly ignored by dwarf_dealloc().
    Users should not be storing Dwarf_Error pointers
    for long so this singleton is only going to cause
    confusion when callers try to save an out-of-memory
    Dwarf_Error pointer.
    If the call provides no way to handle the error
    the function simply returns, whereas it used
    (before July 2021) to abort in that case.
*/

/*  The user provides an explanatory string, the error
    number itself explains little.
    This prepends DW_DLE_USER_DECLARED_ERROR to the
    caller-provided string.
    New in April, 2020 .  Used by dwarfdump in a few
    circumstances. */
void
dwarf_error_creation(Dwarf_Debug dbg,
    Dwarf_Error *err,
    char *errmsg)
{
    dwarfstring m;

    if (IS_INVALID_DBG(dbg)) {
        return;
    }
    dwarfstring_constructor(&m);
    dwarfstring_append(&m,"DW_DLE_USER_DECLARED_ERROR: ");
    dwarfstring_append(&m,errmsg);
    _dwarf_error_string(dbg,err,
        DW_DLE_USER_DECLARED_ERROR,
        dwarfstring_string(&m));
    dwarfstring_destructor(&m);
}

/*  In rare cases (bad object files) an error is created
    via malloc with no dbg to attach it to.
    We record a few of those and dealloc and flush
    on any dwarf_finish()
    We do not expect this except on corrupt objects. */

void
_dwarf_error(Dwarf_Debug dbg, Dwarf_Error * error,
    Dwarf_Signed errval)
{
    _dwarf_error_string(dbg,error,errval,0);
}

/*  Errors are all added to the de_primary_dbg, never to
    de_secondary_dbg. */
void
_dwarf_error_string(Dwarf_Debug dbg, Dwarf_Error * error,
    Dwarf_Signed errval,char *msg)
{
    Dwarf_Error errptr = 0;

    /*  Allow NULL dbg on entry, since sometimes that
        can happen and we want to report the upper-level
        error, not the null dbg error. */
    if (error) {
        /*  If dbg is NULL, use the alternate error struct. However,
            this will overwrite the earlier error. */
        if (dbg) {
            /*  ERRORs are always associated with
                de_primary_dbg so they can be returned
                up the tree of calls on the stack
                safely.  */
            errptr =
                (Dwarf_Error) _dwarf_get_alloc(dbg->de_errors_dbg,
                    DW_DLA_ERROR, 1);
            if (!errptr) {
                errptr = &_dwarf_failsafe_error;
                errptr->er_static_alloc = DE_STATIC;
            } else {
                errptr->er_static_alloc = DE_STANDARD;
            }
        } else {
            /*  We have no dbg to work with. dwarf_init
                failed. We hack
                up a special area. */
            errptr = _dwarf_special_no_dbg_error_malloc();
            if (!errptr) {
                errptr = &_dwarf_failsafe_error;
                errptr->er_static_alloc = DE_STATIC;
#ifdef DEBUG
                printf("libdwarf no dbg to dwarf_error_string,"
                    " fullystatic, "
                    "using DE_STATIC alloc, addr"
                    " 0x%lx line %d %s\n",
                    (unsigned long)errptr,
                    __LINE__,__FILE__);
#endif /* DEBUG */
            } else {
                errptr->er_static_alloc = DE_MALLOC;

#ifdef DEBUG
                printf("libdwarf no dbg, add to static_err_list "
                    "static DE_MALLOC alloc, addr"
                    " 0x%lx line %d %s\n",
                    (unsigned long)errptr,
                    __LINE__,__FILE__);
#endif /* DEBUG */
                _dwarf_add_to_static_err_list(errptr);
            }
        }

        errptr->er_errval = errval;
        if (msg && errptr->er_static_alloc != DE_STATIC) {
            dwarfstring *em = 0;

#ifdef DEBUG
            printf("libdwarf ALLOC creating error string"
                " %s errval %ld errptr 0x%lx \n",
                msg,(long)errval,(unsigned long)errptr);
#endif /* DEBUG */
            em = (dwarfstring *)calloc(1,sizeof(dwarfstring));
            if (em) {
                dwarfstring_constructor(em);
                dwarfstring_append(em,msg);
                errptr->er_msg = (void*)em;
            }
        }
        *error = errptr;
        return;
    }

    if (dbg  && dbg->de_errhand != NULL) {
        errptr = (Dwarf_Error) _dwarf_get_alloc(dbg->de_errors_dbg,
            DW_DLA_ERROR, 1);
        if (errptr == NULL) {
            errptr = &_dwarf_failsafe_error;
            errptr->er_static_alloc = DE_STATIC;
        }
        errptr->er_errval = errval;
        dbg->de_errhand(errptr, dbg->de_errors_dbg->de_errarg);
        return;
    }
    fflush(stderr);
    fprintf(stderr,
        "\nlibdwarf is unable to record error %s "
        "No error argument or handler available\n",
        dwarf_errmsg_by_number(errval));
    fflush(stderr);
    return;
}

Dwarf_Unsigned
dwarf_errno(Dwarf_Error error)
{
    if (!error) {
        return (0);
    }
    return (error->er_errval);
}

char*
dwarf_errmsg_by_number(Dwarf_Unsigned errornum )
{
    if (errornum > DW_DLE_LAST) {
        return "Dwarf_Error value out of range";
    }
    return ((char *) &_dwarf_errmsgs[errornum][0]);
}

/*
*/
char *
dwarf_errmsg(Dwarf_Error error)
{
    if (!error) {
        return "Dwarf_Error is NULL";
    }
    if (error->er_msg) {
        return dwarfstring_string(error->er_msg);
    }
    return  dwarf_errmsg_by_number(error->er_errval);
}
