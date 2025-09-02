/*
  Copyright (C) 2000-2004 Silicon Graphics, Inc.  All Rights Reserved.
  Portions Copyright (C) 2009-2020 David Anderson. All Rights Reserved.

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

#include <stddef.h> /* NULL, size_t */
#include <string.h> /* strlen() */

#if defined(_WIN32) && defined(HAVE_STDAFX_H)
#include "stdafx.h"
#endif /* HAVE_STDAFX_H */

#include "dwarf.h"
#include "libdwarf.h"
#include "dwarf_local_malloc.h"
#include "libdwarf_private.h"
#include "dwarf_base_types.h"
#include "dwarf_opaque.h"
#include "dwarf_error.h"
#include "dwarf_util.h"

int
dwarf_get_str(Dwarf_Debug dbg,
    Dwarf_Off offset,
    char **string,
    Dwarf_Signed * returned_str_len, Dwarf_Error * error)
{
    int res = DW_DLV_ERROR;
    void *secptr = 0;
    void *begin = 0;
    void *end = 0;

    CHECK_DBG(dbg,error,"dwarf_get_str()");
    if (offset == dbg->de_debug_str.dss_size) {
        /*  Normal (if we've iterated thru the set of strings using
            dwarf_get_str and are at the end). */
        return DW_DLV_NO_ENTRY;
    }
    if (offset > dbg->de_debug_str.dss_size) {
        _dwarf_error(dbg, error, DW_DLE_DEBUG_STR_OFFSET_BAD);
        return DW_DLV_ERROR;
    }

    if (!string  || !returned_str_len) {
        _dwarf_error(dbg, error, DW_DLE_STRING_PTR_NULL);
        return DW_DLV_ERROR;
    }

    res = _dwarf_load_section(dbg, &dbg->de_debug_str,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    if (!dbg->de_debug_str.dss_size) {
        return DW_DLV_NO_ENTRY;
    }
    secptr =dbg->de_debug_str.dss_data;
    begin = (char *)secptr + offset;
    end =   (char *)secptr + dbg->de_debug_str.dss_size;

    res = _dwarf_check_string_valid(dbg,secptr,begin,end,
        DW_DLE_DEBUG_STR_OFFSET_BAD,error);
    if (res != DW_DLV_OK) {
        return res;
    }

    *string = (char *) begin;
    *returned_str_len = strlen(*string);
    return DW_DLV_OK;
}
