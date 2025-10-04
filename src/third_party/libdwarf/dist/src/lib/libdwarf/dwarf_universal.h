/* Copyright (c) 2023, David Anderson
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

#ifndef DWARF_UNIVERSAL_H
#define DWARF_UNIVERSAL_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

struct Dwarf_Universal_Head_s;
typedef struct Dwarf_Universal_Head_s *  Dwarf_Universal_Head;

int _dwarf_object_detector_universal_head(
    char           *dw_path,
    Dwarf_Unsigned  dw_filesize,
    unsigned int   *dw_contentcount,
    Dwarf_Universal_Head * dw_head,
    int            *errcode);

int _dwarf_object_detector_universal_instance(
    Dwarf_Universal_Head dw_head,
    Dwarf_Unsigned  dw_index_of,
    Dwarf_Unsigned *dw_cpu_type,
    Dwarf_Unsigned *dw_cpu_subtype,
    Dwarf_Unsigned *dw_offset,
    Dwarf_Unsigned *dw_size,
    Dwarf_Unsigned *dw_align,
    int         *errcode);
void _dwarf_dealloc_universal_head(Dwarf_Universal_Head dw_head);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* DWARF_UNIVERSAL_H */
