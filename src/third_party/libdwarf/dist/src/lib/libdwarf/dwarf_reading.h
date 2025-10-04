/*
Copyright (c) 2018-2023, David Anderson
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
#ifndef DWARF_READING_H
#define DWARF_READING_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#ifndef DW_DLV_OK
/* DW_DLV_OK  must match libdwarf.h */
/* DW_DLV_NO_ENTRY  must match libdwarf.h  */
#define DW_DLV_OK 0
#define DW_DLV_NO_ENTRY -1
#define DW_DLV_ERROR 1
#endif /* DW_DLV_OK */

#define ALIGN4 4
#define ALIGN8 8

#define PREFIX "\t"
#define LUFMT "%lu"
#define UFMT "%u"
#define DFMT "%d"
#define XFMT "0x%x"

/* even if already seen, values must match, so no #ifdef needed. */
#define DW_DLV_NO_ENTRY  -1
#define DW_DLV_OK         0
#define DW_DLV_ERROR      1

#define P printf
#define F fflush(stdout)

#define RRMOA(f,buf,loc,siz,fsiz,errc) _dwarf_object_read_random(\
    (f),(char *)(buf),(loc),(siz),(fsiz),(errc));

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* DWARF_READING_H */
