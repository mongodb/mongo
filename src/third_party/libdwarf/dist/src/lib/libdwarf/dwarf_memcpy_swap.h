/*
    Copyright (C) 2018-2023 David Anderson. All Rights Reserved.

    This program is free software; you can redistribute it
    and/or modify it under the terms of version 2.1 of the
    GNU Lesser General Public License as published by the
    Free Software Foundation.

    This program is distributed in the hope that it would
    be useful, but WITHOUT ANY WARRANTY; without even the
    implied warranty of MERCHANTABILITY or FITNESS FOR A
    PARTICULAR PURPOSE.

    Further, this software is distributed without any warranty
    that it is free of the rightful claim of any third person
    regarding infringement or the like.  Any license provided
    herein, whether implied or otherwise, applies only to
    this software file.  Patent licenses, if any, provided
    herein do not apply to combinations of this program with
    other software, or any other product whatsoever.

    You should have received a copy of the GNU Lesser General
    Public License along with this program; if not, write
    the Free Software Foundation, Inc., 51 Franklin Street -
    Fifth Floor, Boston MA 02110-1301, USA.
*/

#ifndef MEMCPY_SWAP_H
#define MEMCPY_SWAP_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

void _dwarf_memcpy_swap_bytes(void *s1, const void *s2,
    unsigned long len);
/*  It was once inconvenient to use memcpy directly as it
    uses size_t and that requires <stddef.h>,
    although stddef.h is a part of C90, so..ok. */
void _dwarf_memcpy_noswap_bytes(void *s1,
    const void *s2, unsigned long len);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* MEMCPY_SWAP_H */
