/*

   Copyright (C) 2020 David Anderson. All Rights Reserved.

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
   Public License along with this program; if not, write
   the Free Software Foundation, Inc., 51 Franklin Street -
   Fifth Floor, Boston MA 02110-1301, USA.
*/

#include <config.h>

#include <stddef.h> /* size_t */
#include <stdio.h>  /* SEEK_END SEEK_SET */
#include <stdlib.h> /* free() malloc() */
#include <string.h> /* memcpy() */

#include "dwarf.h"
#include "libdwarf.h"
#include "dwarf_local_malloc.h"
#include "libdwarf_private.h"
#include "dwarf_base_types.h"
#include "dwarf_util.h"
#include "dwarf_opaque.h"
#include "dwarf_error.h"

/*  Returns DW_DLV_OK DW_DLV_NO_ENTRY or DW_DLV_ERROR
    crc32 used for debuglink crc calculation.
    Caller passes pointer to an
    uninitialized array of 4 unsigned char
    and if this returns DW_DLV_OK that is filled in.
    The crc is calculated based on reading
    the entire current open
    Dwarf_Debug dbg object file and all bytes in
    the file are read to create  the crc.

    In Windows, where unsigned int is 16 bits, this
    produces different output than on 32bit ints.

    Since lseek() (all systems/versions) returns
    a signed value, we check for < 0 as error
    rather than just check for -1. So it is clear
    to symbolic execution that conversion to
    unsigned does not lose bits.
*/
int
dwarf_crc32 (Dwarf_Debug dbg,unsigned char *crcbuf,
    Dwarf_Error *error)
{
    /*  off_t is signed,    defined by POSIX */
    /*  ssize_t is signed,  defined in POSIX */

    /*  size_t is unsigned, defined in C89. */
    Dwarf_Unsigned   fsize = 0;
    /*  Named with u to remind the reader that this is
        an unsigned value. */
    Dwarf_Unsigned  readlenu = 10000;
    Dwarf_Unsigned  size_left = 0;
    const unsigned char *readbuf = 0;
    unsigned int   tcrc = 0;
    unsigned int   init = 0;
    int            fd = -1;
    Dwarf_Unsigned   sz = 0;
    int            res = 0;

    CHECK_DBG(dbg,error,"dwarf_crc32()");
    if (!crcbuf) {
        return DW_DLV_NO_ENTRY;
    }
    if (!dbg->de_owns_fd) {
        return DW_DLV_NO_ENTRY;
    }
    fd = dbg->de_fd;
    if (fd < 0) {
        return DW_DLV_NO_ENTRY;
    }
    fd = dbg->de_fd;
    if (dbg->de_filesize) {
        fsize = (size_t)dbg->de_filesize;
    } else {
        res = _dwarf_seekr(fd,0,SEEK_END,&sz);
        if (res != DW_DLV_OK) {
            _dwarf_error_string(dbg,error,DW_DLE_SEEK_ERROR,
                "DW_DLE_SEEK_ERROR: dwarf_crc32 seek "
                "to end fails");
            return DW_DLV_ERROR;
        }
        fsize = sz;
    }
    if (fsize <= (Dwarf_Unsigned)500) {
        /*  Not a real object file.
            A random length check. */
        return DW_DLV_NO_ENTRY;
    }
    size_left = fsize;
    res = _dwarf_seekr(fd,0,SEEK_SET,0);
    if (res != DW_DLV_OK) {
        _dwarf_error_string(dbg,error,DW_DLE_SEEK_ERROR,
            "DW_DLE_SEEK_ERROR: dwarf_crc32 seek "
            "to start fails");
        return DW_DLV_ERROR;
    }
    readbuf = (unsigned char *)malloc(readlenu);
    if (!readbuf) {
        _dwarf_error_string(dbg,error,DW_DLE_ALLOC_FAIL,
            "DW_DLE_ALLOC_FAIL: dwarf_crc32 read buffer"
            " alloc fails");
        return DW_DLV_ERROR;
    }
    while (size_left > 0) {
        if (size_left < readlenu) {
            readlenu = size_left;
        }
        res = _dwarf_readr(fd,(char *)readbuf,readlenu,0);
        if (res != DW_DLV_OK) {
            _dwarf_error_string(dbg,error,DW_DLE_READ_ERROR,
                "DW_DLE_READ_ERROR: dwarf_crc32 read fails ");
            free((unsigned char*)readbuf);
            return DW_DLV_ERROR;
        }
        /*  Call the public API function so it gets tested too. */
        tcrc = (unsigned int)dwarf_basic_crc32(readbuf,
            (unsigned long)readlenu,
            (unsigned long)init);
        init = tcrc;
        size_left -= readlenu;
    }
    /*  endianness issues?  */
    free((unsigned char*)readbuf);
    memcpy(crcbuf,(void *)&tcrc,4);
    return DW_DLV_OK;
}
