/*
  Copyright (C) 2015-2023 David Anderson. All Rights Reserved.

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

/*
   dwarf_macro5.h
   For the DWARF5 .debug_macro section
   (also appears as an extension to DWARF4)
*/

struct Dwarf_Macro_Forms_s {
    /* Code means DW_MACRO_define etc. */
    Dwarf_Small         mf_code;

    /* How many entries in mf_formbytes array. */
    Dwarf_Small         mf_formcount;

    /* Never free these, these are in the object file memory */
    const Dwarf_Small * mf_formbytes;
};

struct Dwarf_Macro_OperationsList_s {
    unsigned mol_count;
    struct Dwarf_Macro_Forms_s * mol_data;
};

struct Dwarf_Macro_Operator_s {
    /*  mo_opcode == mo_form->mf_code unless it is
        the final 0 byte in which case all 3 values
        are zero */
    Dwarf_Small      mo_opcode;

    struct Dwarf_Macro_Forms_s * mo_form;

    /*  Points at the first byte of the data, meaning
        it points one-past the macro operation code byte. */
    Dwarf_Small *    mo_data;
};

#define MACRO_OFFSET_SIZE_FLAG 1
#define MACRO_LINE_OFFSET_FLAG 2
#define MACRO_OP_TABLE_FLAG 4
#define DW_MACRO_VERSION4  4  /* GNU Extension for DWARF 4 */
#define DW_MACRO_VERSION5  5  /* DWARF 5 */

/*  Could be reordered to be most space efficient.
    That might be a little harder to read.  Hmm. */
struct Dwarf_Macro_Context_s {
    Dwarf_Unsigned mc_sentinel;
    Dwarf_Half     mc_version_number;

    /* Section_offset in .debug_macro of macro header */
    Dwarf_Unsigned mc_section_offset;
    Dwarf_Unsigned mc_section_size;

    /*  Total length of the macro data for this
        macro unit.
        Calculated, not part of header. */
    Dwarf_Unsigned mc_total_length;

    Dwarf_Half     mc_macro_header_length;

    Dwarf_Small    mc_flags;

    /*  If DW_MACRO_start_file is in the operators of this
        table then the mc_debug_line_offset must be present from
        the header. */
    Dwarf_Unsigned mc_debug_line_offset;

    /* the following three set from the bits in mc_flags  */
    /* If 1, offsets 64 bits */
    Dwarf_Bool     mc_offset_size_flag;

    /* if 1, debug_line offset is present. */
    Dwarf_Bool     mc_debug_line_offset_flag;

    /* 4 or 8, depending on mc_offset_size_flag */
    Dwarf_Small    mc_offset_size;

    /*  If one the operands/opcodes (mc_opcode_forms) table is present
        in the header. If not we use a default table.

        Even when there are operands in the  header
        the standardops may or may not be
        defined in the header. */
    Dwarf_Bool mc_operands_table_flag;

    /*  Count of the Dwarf_Macro_Forms_s structs pointed to by
        mc_opcode_forms.  These from the header. */
    Dwarf_Small                 mc_opcode_count;
    struct Dwarf_Macro_Forms_s *mc_opcode_forms;

    /*  mc_ops must be free()d, but pointers inside
        mc_ops are to static or section data so must not
        be freed. */
    Dwarf_Unsigned                 mc_macro_ops_count;
    Dwarf_Unsigned                 mc_ops_data_length;
    struct Dwarf_Macro_Operator_s *mc_ops;

    Dwarf_Small * mc_macro_header;
    Dwarf_Small * mc_macro_ops;

    /*  These are malloc space, not _dwarf_get_alloc()
        so the  DW_DLA_MACRO_CONTEXT dealloc will
        free them. */
    char **       mc_srcfiles;
    Dwarf_Signed  mc_srcfiles_count;

    /*  These are from CU DIE attribute names.
        They may be NULL or point at data in
        a dwarf section. Do not free().
        This attempts to make up for the lack of a
        base file name
        in DWARF2,3,4 line tables.
    */
    const char *  mc_at_comp_dir;
    const char *  mc_at_name;
    /*  The following is malloc,so macro_context_s destructor
        needs to free it. */
    const char *  mc_file_path;

    Dwarf_Debug      mc_dbg;
    Dwarf_CU_Context mc_cu_context;
};

int _dwarf_macro_constructor(Dwarf_Debug dbg, void *m);
