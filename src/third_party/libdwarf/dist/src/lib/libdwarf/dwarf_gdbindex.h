/*

  Copyright (C) 2014-2023 David Anderson. All Rights Reserved.

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

/*  The following is based on
    The gdb online documentation at
    https://sourceware.org/gdb/onlinedocs/gdb/
    Appendix J, ".gdb_index section format".
*/

/*  These are the two types .gdb_index uses.
    the offset_type (32 bits) and other fields
    defined 64 bits.   We use our own Dwarf_Unsigned
    for all the interfaces, these are just for reading
    the section data.

    The section data is defined to be in little-endian regardless of
    the target machine.
    We use our host endianness in all interfaces.

    We simply assume unsigned int is 32 bits FIXME.
*/

typedef Dwarf_Unsigned gdbindex_64;
enum gdbindex_type_e {
    git_unknown,
    git_std,
    git_address,
    git_cuvec
};

struct Dwarf_Gdbindex_array_instance_s {
    Dwarf_Small *  dg_base;
    Dwarf_Unsigned dg_count;
    /* the in_object struct size. */
    Dwarf_Unsigned dg_entry_length;
    /* The size of a single field in the in-object struct */
    unsigned       dg_fieldlen;
    /* The address_area type is a bit irregular. */
    enum gdbindex_type_e dg_type;
};

struct Dwarf_Gdbindex_s {
    Dwarf_Debug      gi_dbg;
    Dwarf_Small    * gi_section_data;   /* dss_data */
    Dwarf_Unsigned   gi_section_length; /* dss_size */

    Dwarf_Unsigned   gi_version;
    Dwarf_Unsigned   gi_cu_list_offset;
    Dwarf_Unsigned   gi_types_cu_list_offset;
    Dwarf_Unsigned   gi_address_area_offset;
    Dwarf_Unsigned   gi_symbol_table_offset;
    Dwarf_Unsigned   gi_constant_pool_offset;
    struct Dwarf_Gdbindex_array_instance_s  gi_culisthdr;
    struct Dwarf_Gdbindex_array_instance_s  gi_typesculisthdr;
    struct Dwarf_Gdbindex_array_instance_s  gi_addressareahdr;
    struct Dwarf_Gdbindex_array_instance_s  gi_symboltablehdr;
    struct Dwarf_Gdbindex_array_instance_s  gi_cuvectorhdr;
};
