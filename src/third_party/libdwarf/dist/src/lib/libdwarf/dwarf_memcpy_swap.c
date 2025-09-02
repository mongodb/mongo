/*
  Copyright (C) 2000-2005 Silicon Graphics, Inc.  All Rights Reserved.
  Portions Copyright (C) 2007-2020 David Anderson. All Rights Reserved.
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

#include <stddef.h> /* size_t */
#include <string.h> /* memcpy() */

#include "dwarf_memcpy_swap.h"

/*
  A byte-swapping version of memcpy
  for cross-endian use.
  Only 2,4,8 should be lengths passed in.
*/
void
_dwarf_memcpy_noswap_bytes(void *s1,
    const void *s2,
    unsigned long len)
{
    memcpy(s1,s2,(size_t)len);
    return;
}

void
_dwarf_memcpy_swap_bytes(void *s1,
    const void *s2,
    unsigned long len)
{
    unsigned char       *targ = (unsigned char *) s1;
    const unsigned char *src = (const unsigned char *) s2;
    unsigned long        i = 0;
    unsigned long        n = (long)(len-1);

    if (len > 8) {
        /*  Really we should not be here!
            Not writing an integer, we think, so
            best to not swap bytes! */
        memcpy(s1,s2,(size_t)len);
        return;
    }
    for ( ; i < len; ++i,--n) {
        targ[n]  = src[i];
    }
    return;
}
