/*
  Copyright (C) 2020-2023 David Anderson. All Rights Reserved.
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

/*  This is for accessing .debug_gnu_pubnames
    and .debug_gnu_pubtypes.
    It has nothing to do with .gdb_index. */

struct DGI_Entry_s {
    char           *  ge_string;
    Dwarf_Unsigned    ge_debug_info_offset;
    char              ge_flag_byte;
#if 0
    /*  Following is DW_GNUIVIS  bit 7 of flag byte
        shifted right 7. */
    char              ge_flag_global_static;
    /*  Following is DW_GNUIKIND bits 4,5,6 of flag byte
        shifted right 4.
        Bits 0-3 are reserved and should de zero.  */
    char              ge_flag_type;
#endif
};
/*
    each block data on disk looks like:
    LENGTH (which determines offset size)
    version
    offset into .debug_info (offsetsize)
    size  of area in .debug_info (offsetsize)
    For each entry
        offset in debug_info of DIE (offsetsize)
        1 byte flag
        strlen+1 string
    Trailing LENGTH 4 bytes zero.
*/
struct Dwarf_Gnu_IBlock_s {
    Dwarf_Gnu_Index_Head ib_head;
    Dwarf_Unsigned ib_index; /*of this ib */
    Dwarf_Unsigned ib_block_length_offset;
    Dwarf_Unsigned ib_block_length;
    Dwarf_Half     ib_offset_size; /* 4 or 8 */
    Dwarf_Half     ib_extension_size; /* 0 or 4 */
    Dwarf_Half     ib_version;
    Dwarf_Bool     ib_counted_entries; /* see ib_entry_count*/
    Dwarf_Unsigned ib_offset_in_debug_info;
    Dwarf_Unsigned ib_size_in_debug_info;

    /* following the length field*/
    Dwarf_Unsigned ib_b_data_offset;

    Dwarf_Unsigned ib_b_offset; /* offset of entry area */
    Dwarf_Small *  ib_b_data;   /* the entry area */
    /* entrylength =  LENGTH - 2 - 2*offsetsize */
    Dwarf_Unsigned ib_b_entrylength;

    Dwarf_Unsigned              ib_entry_count;
    struct DGI_Entry_s         *ib_entryarray;
};

struct Dwarf_Gnu_Index_Head_s {
    Dwarf_Debug      gi_dbg;
    Dwarf_Small    * gi_section_data;
    Dwarf_Unsigned   gi_section_length;
    Dwarf_Small    * gi_section_end;
    struct Dwarf_Gnu_IBlock_s *gi_blockarray;
    Dwarf_Unsigned   gi_blockcount;
    Dwarf_Bool       gi_is_pubnames; /* if false is pubtypes */
};
void _dwarf_gnu_index_head_destructor(void *incoming);
void _dwarf_free_gnu_index_head_content(Dwarf_Gnu_Index_Head);
