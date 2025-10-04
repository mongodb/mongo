/*
Copyright (C) 2022-2023 David Anderson. All Rights Reserved.

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

#ifndef DWARF_DEBUGADDR_H
#define DWARF_DEBUGADDR_H
#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define DW_ADDR_TABLE_MAGIC 0xfade

/*  In DWARF5 each portion of .debug_addr has a header.

    In a DWARF4 extension gcc emits .debug_addr
    but without those headers, just with the
    array of data entries, so dwarf_debugaddr.c
    creates a table from data in Dwarf_Debug_S
    assuming every CU has the
    same address_size and version as in the initial
    .debug_info CU. */

struct Dwarf_Debug_Addr_Table_s {
    Dwarf_Unsigned da_magic;
    Dwarf_Debug    da_dbg;
    /*  Length includes the table header and
        the length-field of the header and
        the array of addresses. */
    Dwarf_Unsigned da_length;
    Dwarf_Small    da_length_size; /* 4 or 8 */
    Dwarf_Small    da_extension_size; /* 4 or 0 */
    Dwarf_Unsigned da_table_section_offset;

    /*  Whole section size. >= this table length */
    Dwarf_Unsigned da_section_size ;

    /* pointer to entry[0] */
    Dwarf_Small   *da_data_entries;
    Dwarf_Unsigned da_entry_count;
    /*  One past end of this Debug Addr Table */
    Dwarf_Small   *da_end_table;

    /*  The value appearing in some DW_AT_addr_base:
        da_table_section_offset+da_local_offset_entry0. */
    Dwarf_Unsigned da_addr_base;
    Dwarf_Half     da_version;
    Dwarf_Small    da_address_size;
#if 0
    /*  this is not really handled anywhere by any compiler
        so we do not remember it. Must be zero. */
    Dwarf_Small    da_segment_selector_size;
#endif
};

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DWARF_DEBUGADDR_H */
