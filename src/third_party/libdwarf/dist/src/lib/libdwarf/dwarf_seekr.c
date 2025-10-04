/*
Copyright (c) 2018-2024, David Anderson All rights reserved.

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

#include <stdlib.h> /* free() */
#include <stdio.h>  /* SEEK_END SEEK_SET */
#include <string.h> /* memset() strlen() */

#ifdef _WIN32
#ifdef HAVE_STDAFX_H
#include "stdafx.h"
#endif /* HAVE_STDAFX_H */
#include <io.h> /* lseek() off_t ssize_t */
#endif /* _WIN32 */

#ifdef HAVE_UNISTD_H
#include <unistd.h> /* close() */
#endif /* HAVE_UNISTD_H */

#ifdef HAVE_FCNTL_H
#include <fcntl.h> /* open() O_RDONLY */
#endif /* HAVE_FCNTL_H */

#ifdef _WIN64
#ifdef lseek /* defined in msys2 in an io.h */
#undef lseek
#endif /* lseek */
#define lseek _lseeki64
#endif /* _WIN64 */

#ifdef HAVE_UNISTD_H
#include <unistd.h> /* lseek() off_t */
#endif /* HAVE_UNISTD_H */

#ifdef HAVE_FCNTL_H
#include <fcntl.h> /* open() O_RDONLY */
#endif /* HAVE_FCNTL_H */

#include "dwarf.h"
#include "libdwarf.h"
#include "dwarf_local_malloc.h"
#include "libdwarf_private.h"
#include "dwarf_base_types.h"
#include "dwarf_opaque.h"

#ifndef O_BINARY
#define O_BINARY 0
#endif /* O_BINARY */
#ifndef O_RDONLY
#define O_RDONLY 0
#endif /* O_RDONLY */
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif /* O_CLOEXEC */

#if 0 /* debugging only */
static void
dump_bytes(char * msg,Dwarf_Small * start, long len)
{
    Dwarf_Small *end = start + len;
    Dwarf_Small *cur = start;

    printf("%s ",msg);
    for (; cur < end; cur++) {
        printf("%02x ", *cur);
    }
    printf("\n");
}
#endif

int
_dwarf_readr(int fd,
    char *buf,
    Dwarf_Unsigned size,
    Dwarf_Unsigned *sizeread_out)
{

    Dwarf_Signed rcode = 0;
#ifdef _WIN64
    Dwarf_Unsigned max_single_read = 0x1ffff000;
#elif defined(_WIN32)
    Dwarf_Unsigned max_single_read = 0xffff;
#else
    Dwarf_Unsigned max_single_read = 0x1ffff000;
#endif
    Dwarf_Unsigned remaining_bytes = 0;
    Dwarf_Unsigned totalsize = size;

    remaining_bytes = size;
    while(remaining_bytes > 0) {
        if (remaining_bytes > max_single_read) {
            size = max_single_read;
        }
#ifdef _WIN64
        rcode = (Dwarf_Signed)_read(fd,buf,(unsigned const)size);
#elif defined(_WIN32)
        rcode = (Dwarf_Signed)_read(fd,buf,(unsigned const)size);
#else /* linux */
        rcode = (Dwarf_Signed)read(fd,buf,(size_t)size);
#endif
        if (rcode < 0 || rcode != (Dwarf_Signed)size) {
            return DW_DLV_ERROR;
        }
        remaining_bytes -= size;
        buf += size;
        size = remaining_bytes;
    }
    if (sizeread_out) {
        *sizeread_out = totalsize;
    }
    return DW_DLV_OK;
}

int
_dwarf_seekr(int fd,
    Dwarf_Unsigned loc,
    int seektype,
    Dwarf_Unsigned *out_loc)
{
    Dwarf_Signed fsize = 0;
    Dwarf_Signed sloc = 0;

    sloc = (Dwarf_Signed)loc;
    if (sloc < 0) {
        return DW_DLV_ERROR;
    }
#ifdef _WIN64
    fsize = (Dwarf_Signed)lseek(fd,(__int64)loc,seektype);
#elif defined(_WIN32)
    fsize = (Dwarf_Signed)lseek(fd,(off_t)loc,seektype);
#else /* linux */
    fsize = (Dwarf_Signed)lseek(fd,(off_t)loc,seektype);
#endif
    if (fsize < 0) {
        return DW_DLV_ERROR;
    }
    if (out_loc) {
        *out_loc = (Dwarf_Unsigned)fsize;
    }
    return DW_DLV_OK;
}

void
_dwarf_closer( int fd)
{
#ifdef _WIN64
    _close(fd);
#elif defined(_WIN32)
    _close(fd);
#else /* linux */
    close(fd);
#endif
}

int
_dwarf_openr( const char *name)
{

    int fd = -1;
#ifdef _WIN64
    fd = _open(name, O_RDONLY | O_BINARY|O_CLOEXEC);
#elif defined(_WIN32)
    fd = _open(name, O_RDONLY | O_BINARY|O_CLOEXEC);
#else /* linux */
    fd = open(name, O_RDONLY | O_BINARY|O_CLOEXEC);
#endif
    return fd;

}
