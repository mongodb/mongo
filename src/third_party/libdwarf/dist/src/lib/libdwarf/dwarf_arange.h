/*

Copyright (C) 2000 Silicon Graphics, Inc.  All Rights Reserved.
Portions Copyright (C) 2011-2023 David Anderson. All Rights Reserved.

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
  Public License along with this program; if not, write the
  Free Software Foundation, Inc., 51 Franklin Street - Fifth
  Floor, Boston MA 02110-1301, USA.

*/

/*  This structure is used to read an arange into. */
struct Dwarf_Arange_s {

    /*  The segment selector. Only non-zero if Dwarf4, only
        meaningful if ar_segment_selector_size non-zero   */
    Dwarf_Unsigned ar_segment_selector;

    /* Starting address of the arange, ie low-pc. */
    Dwarf_Addr ar_address;

    /* Length of the arange. */
    Dwarf_Unsigned ar_length;

    /*  Offset into .debug_info of the start of the compilation-unit
        containing this set of aranges.
        Applies only to .debug_info, not .debug_types. */
    Dwarf_Off ar_info_offset;

    /* Corresponding Dwarf_Debug. */
    Dwarf_Debug ar_dbg;

    Dwarf_Half ar_segment_selector_size;
};

int
_dwarf_get_aranges_addr_offsets(Dwarf_Debug dbg,
    Dwarf_Addr ** addrs,
    Dwarf_Off ** offsets,
    Dwarf_Signed * count,
    Dwarf_Error * error);
