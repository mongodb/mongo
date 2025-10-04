/* Copyright (c) 2018-2023, David Anderson
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

#ifndef DWARF_OBJECT_DETECTOR_H
#define DWARF_OBJECT_DETECTOR_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/*  Declares the interface function.
    outpath is a place you provide, of a length outpath_len
    you consider reasonable,
    where the final path used is recorded.
    outpath_len must be larger than strlen(path);

    This matters as for mach-o if the path is a directory
    name the function will look in the standard macho-place
    for the object file (useful for dSYM) and return the
    constructed path in oupath.
    returns DW_DLV_OK, DW_DLV_ERROR, or DW_DLV_NO_ENTRY */

#ifndef DW_FTYPE_UNKNOWN
#define DW_FTYPE_UNKNOWN 0
#define DW_FTYPE_ELF     1
#define DW_FTYPE_MACH_O  2
#define DW_FTYPE_PE      3
#define DW_FTYPE_ARCHIVE 4  /* unix archive */
#endif /* DW_FTYPE_UNKNOWN */

/*  offsetsize refers to the object-file-format.
    Elf 32 or macho-32 or PE 32, for example.
    Not to DWARF offset sizes.  */

/*  Path means look(first) for an dynsym object
    of the same name per MacOS standards,
    making the outpath space needed is more than
    that in path.
    Copies the actual path into outpath, (an error
    if the length in outpath_len is less than needed
    for the object found).
    If DW_DLV_NO_ENTRY or DW_DLV_ERROR returned
    the argument values other than path
    must be considered to be in an unknown state. */

/*  The errcode is a small integer distinct from libdwarf
    and simply printing the integer (returned through
    *errcode when the function returns DW_DLV_ERROR)
    will hopefully suffice for most purposes. */

/*  Added September 2023 for Mach-O universal binaries */
int _dwarf_object_detector_fd_a(int dw_fd,
#if 0
    void           *dw_inmemory,
    Dwarf_Unsigned dw_inmem_size,
#endif
    unsigned int   *dw_ftype,
    unsigned int   *dw_endian,
    unsigned int   *dw_offsetsize,
    Dwarf_Unsigned dw_offset_base,
    Dwarf_Unsigned *dw_filesize,
    int            *dw_errcode);

#if 0
    /*  Used when reading a block of memory
        containing a complete object file. */
int _dwarf_object_detector_inmemory(void *dw_memory,
    Dwarf_Unsigned dw_memsize,
    unsigned int   *dw_ftype,
    unsigned int   *dw_endian,
    unsigned int   *dw_offsetsize,
    Dwarf_Unsigned  dw_offset_base,
    Dwarf_Unsigned *dw_filesize,
    int            *dw_errcode);
#endif

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DWARF_OBJECT_DETECTOR_H */
