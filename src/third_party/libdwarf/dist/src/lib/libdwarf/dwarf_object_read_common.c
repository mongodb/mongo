/*
Copyright (c) 2018, David Anderson
All rights reserved.

Redistribution and use in source and binary forms, with
or without modification, are permitted provided that the
following conditions are met:

    Redistributions of source code must retain the above
    copyright notice, this list of conditions and the following
    disclaimer.

    Redistributions in binary form must reproduce the above
    copyright notice, this list of conditions and the following
    disclaimer in the documentation and/or other materials
    provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <config.h>
#include <stddef.h> /* size_t */
#include <stdio.h>  /* SEEK_END SEEK_SET */

#include "dwarf.h"
#include "libdwarf.h"
#include "dwarf_local_malloc.h"
#include "libdwarf_private.h"
#include "dwarf_base_types.h"
#include "dwarf_opaque.h"
#include "dwarf_safe_strcpy.h"
#include "dwarf_object_read_common.h"

/*  Neither off_t nor ssize_t is in C90.
    However, both are in Posix:
    IEEE Std 1003.1-1990, aka
    ISO/IEC 9954-1:1990.
    This gets asked to read large sections sometimes.
    The Linux kernel allows at most 0x7ffff000
    bytes in a read()*/
int
_dwarf_object_read_random(int fd, char *out_buf, Dwarf_Unsigned loc,
    Dwarf_Unsigned size, Dwarf_Unsigned filesize, int *errc)
{
    Dwarf_Unsigned endpoint = 0;
    int res = 0;

    if (loc >= filesize) {
        /*  Seek can seek off the end. Lets not allow that.
            The object is corrupt. */
        *errc = DW_DLE_SEEK_OFF_END;
        return DW_DLV_ERROR;
    }
    endpoint = loc+size;
    if (endpoint < loc) {
        /*  Overflow!  The object is corrupt. */
        *errc = DW_DLE_READ_OFF_END;
        return DW_DLV_ERROR;
    }
    if (endpoint > filesize) {
        /*  Let us -not- try to read past end of object.
            The object is corrupt. */
        *errc = DW_DLE_READ_OFF_END;
        return DW_DLV_ERROR;
    }
    res = _dwarf_seekr(fd,loc,SEEK_SET,0);
    if (res != DW_DLV_OK) {
        *errc = DW_DLE_SEEK_ERROR;
        return DW_DLV_ERROR;
    }
    res = _dwarf_readr(fd,out_buf,size,0);
    if (res != DW_DLV_OK) {
        *errc = DW_DLE_READ_ERROR;
        return DW_DLV_ERROR;
    }
    return DW_DLV_OK;
}
