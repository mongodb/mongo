/*
Copyright (c) 2019-2023, David Anderson
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
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.  */

#ifndef DWARF_DEBUGLINK_H
#define DWARF_DEBUGLINK_H
#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define  DW_BUILDID_SANE_SIZE (8*1024)

int _dwarf_pathjoinl(dwarfstring *target,dwarfstring * input);

int _dwarf_construct_linkedto_path(
    char         **global_prefixes_in,
    unsigned       length_global_prefixes_in,
    char          *pathname_in,
    char          *link_string_in, /* from debug link */
    dwarfstring   *link_string_fullpath,
#if 0
    unsigned char *crc_in, /* from debug_link, 4 bytes */
#endif

    unsigned char *buildid, /* from gnu buildid */
    unsigned       buildid_length, /* from gnu buildid */
    char        ***paths_out,
    unsigned      *paths_out_length,
    int *errcode);
#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DWARF_DEBUGLINK_H */
