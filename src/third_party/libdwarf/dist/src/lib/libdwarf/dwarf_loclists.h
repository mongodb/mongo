/*
Copyright (C) 2000, 2004 Silicon Graphics, Inc.  All Rights Rese    rved.
Portions Copyright (C) 2015-2023 David Anderson. All Rights Rese    rved.

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

/*  Dwarf_Loclists_Context_s contains the data from
    the .debug_loclists
    section headers (if that section exists).
    Dwarf 2,3,4 .debug_loc
    has no such data.  The array (one of these per header in
    .debug_loclists) is recorded in Dwarf_Debug. These
    are filled in at startup at the same time .debug_info
    is opened.  Nothing of this struct is exposed to
    libdwarf callers */
struct Dwarf_Loclists_Context_s {
    Dwarf_Debug    lc_dbg;
    Dwarf_Unsigned lc_index; /* An index  assigned by
        libdwarf to each loclists context. Starting
        with zero at the zero offset in .debug_loclists. */

    /* Offset of the .debug_loclists header involved. */
    Dwarf_Unsigned  lc_header_offset;
    Dwarf_Unsigned  lc_length;
    unsigned long   lc_magic;

    /* Many places in in libdwarf this is called length_size. */
    Dwarf_Small     lc_offset_size;

    /*  rc_extension_size is zero unless this is standard
        DWARF3 and later 64bit dwarf using the extension mechanism.
        64bit DWARF3 and later: rc_extension_size is 4.
        64bit DWARF2 MIPS/IRIX: rc_extension_size is zero.
        32bit DWARF:            rc_extension_size is zero.  */
    Dwarf_Small     lc_extension_size;
    Dwarf_Small     lc_address_size;
    Dwarf_Small     lc_segment_selector_size;
    Dwarf_Half      lc_version; /* 5 */
    Dwarf_Unsigned  lc_offset_entry_count;
    /*  lc_offset_entry_count values. Each local offset to
        a locdesc set. We need this as a way to  know
        which lle entry offsets  are relevant from a loclistx.
        as nothing else reveals these special LLE entries. */
    Dwarf_Unsigned *lc_offset_value_array;

    /* offset in the section of the offset entries */
    Dwarf_Unsigned  lc_offsets_off_in_sect;

    /* Do not free. Points into section memory */
    Dwarf_Small   * lc_offsets_array;

    /*  Offset in the .debug_loclists section of the
        first loclist in the set of loclists for the
        CU. */
    Dwarf_Unsigned  lc_first_loclist_offset;
    Dwarf_Unsigned  lc_past_last_loclist_offset;

    /* pointer to 1st byte of loclist header*/
    Dwarf_Small *  lc_loclists_header;
    /*  pointer to first byte of the loclist data
        for loclist involved. Do not free. */
    Dwarf_Small    *lc_startaddr;
    /*  pointer one past end of the loclist data. */
    Dwarf_Small    *lc_endaddr;
};
