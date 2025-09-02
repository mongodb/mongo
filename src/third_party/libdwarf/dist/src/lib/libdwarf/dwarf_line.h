/*
Copyright (C) 2000, 2004, 2006 Silicon Graphics, Inc.  All Rights Reserved.
Portions Copyright (C) 2009-2023 David Anderson. All Rights Reserved.
Portions Copyright (C) 2010-2012 SN Systems Ltd. All Rights Reserved.

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

#define DW_EXTENDED_OPCODE   0

/*
    This is used as the starting value for an algorithm
    to get the minimum difference between 2 values.
    UINT_MAX is used as our approximation to infinity.
*/
#define MAX_LINE_DIFF       UINT_MAX

/*  This is for a sanity check on line
    table extended opcodes.
    It is entirely arbitrary, and 100 is surely too small if
    someone was inserting strings in the opcode. */
#define DW_LNE_LEN_MAX   100

/*
    This structure is used to build a list of all the
    files that are used in the current compilation unit.
    All of the fields execpt fi_next have meanings that
    are obvious from section 6.2.4 of the Libdwarf Doc.
    Because of DW_LNE_define_file we
    make this a list, not an array.
*/
struct Dwarf_File_Entry_s {
    struct Dwarf_File_Entry_s *fi_next;

    /* Points to string naming the file: DW_LNCT_path. */
    Dwarf_Small *fi_file_name;
    /*  Points to string naming the source, with \n endings
        and null terminated (UTF-8). Embedded source. */
    Dwarf_Small *fi_llvm_source;

    /*  Index into the list of directories of the directory in which
        this file exits.
        For DWARF5, values are 0 to N-1
        For DWARF4 etc values are 1 to N
        so the test for overrun differs. */
    Dwarf_Unsigned fi_dir_index;

    /* Time of last modification of the file. */
    Dwarf_Unsigned fi_time_last_mod;

    /* Length in bytes of the file. */
    Dwarf_Unsigned fi_file_length;

    Dwarf_Small  * fi_gnu_subprogram_name;
    Dwarf_Unsigned fi_gnu_decl_file;
    Dwarf_Unsigned fi_gnu_decl_line;

    Dwarf_Form_Data16   fi_md5_value;
    char           fi_dir_index_present;
    char           fi_time_last_mod_present;
    char           fi_file_length_present;
    char           fi_md5_present;
    char           fi_gnu_decl_file_present;
    char           fi_gnu_decl_line_present;
};

/*  Part of two-level line tables support. */
struct Dwarf_Subprog_Entry_s {
    Dwarf_Small *ds_subprog_name;
    Dwarf_Unsigned ds_decl_file;
    Dwarf_Unsigned ds_decl_line;
};

typedef struct Dwarf_Subprog_Entry_s *Dwarf_Subprog_Entry;

struct Dwarf_Unsigned_Pair_s {
    Dwarf_Unsigned  up_first;
    Dwarf_Unsigned  up_second;
};

/*
    This structure provides the context in which the fields of
    a Dwarf_Line structure are interpreted.  They come from the
    statement program prologue.  **Updated by dwarf_srclines in
    dwarf_line.c.

    lc_magic will be DW_CONTEXT_MAGIC unless there is a serious
    programming error somewhere.
    It's set zero when a Line_Context is deallocated.
    Any other value indicates there is bug somewhere.
*/
#define DW_CONTEXT_MAGIC 0xd00d1111
struct Dwarf_Line_Context_s {
    unsigned    lc_magic;

    /*  lc_new_style_access is non-zero if this was allocated
        via a dwarf_srclines_b() call or equivalent.
        Otherwise this is 0.  */
    unsigned char lc_new_style_access;

    Dwarf_Unsigned lc_unit_length; /* all versions */

    /* The section offset (in .debug_line
        or .debug_line.dwo of the line table */
    Dwarf_Unsigned lc_section_offset;

    /*  2 for DWARF2, 3 for DWARF3, 4 for DWARF4, 5 for DWARF5.
        0xf006 for experimental two-level line tables. */
    Dwarf_Half lc_version_number; /* all versions */

    /* Total length of the line data for this CU */
    Dwarf_Unsigned lc_total_length; /* all versions */

    /* Length of the initial length field itself. */
    Dwarf_Half lc_length_field_length; /* all versions */

    /* address size and segment sizefields new in DWARF5 header.  */
    Dwarf_Small lc_address_size; /* DWARF5 */
    Dwarf_Small lc_segment_selector_size; /* DWARF5 */

    Dwarf_Unsigned lc_header_length; /* all versions */

    Dwarf_Unsigned lc_prologue_length;
    Dwarf_Unsigned lc_actuals_table_offset;
    Dwarf_Unsigned lc_logicals_table_offset;
    Dwarf_Small lc_minimum_instruction_length;  /* all versions */
    Dwarf_Ubyte lc_maximum_ops_per_instruction; /*DWARF5*/

    /*  Start and end of this CU line area. pf_line_ptr_start +
        pf_total_length + pf_length_field_length == pf_line_ptr_end.
        Meaning lc_line_ptr_start is before the length info. */
    Dwarf_Small *lc_line_ptr_start;
    Dwarf_Small *lc_line_ptr_end;
    /*  Start of the lines themselves. */
    Dwarf_Small *lc_line_ptr_lines;

    /*  Used to check that decoding of the line prologue
        is done right. */
    Dwarf_Small *lc_line_prologue_start;

    Dwarf_Small lc_default_is_stmt; /* all versions */
    Dwarf_Sbyte lc_line_base;  /* all versions */
    Dwarf_Small lc_line_range;  /* all versions */

    /* Highest std opcode (+1).  */
    Dwarf_Small lc_opcode_base; /* all versions */
    /*  pf_opcode_base -1 entries (each a count,
        normally the value of
        each entry is 0 or 1). */
    Dwarf_Small *lc_opcode_length_table; /* all versions */

    /*  The number to treat as standard ops. This is a special
        accommodation of gcc using the new standard opcodes but not
        updating the version number.
        It's legal dwarf2, but much better
        for the user to understand as dwarf3 when 'it looks ok'. */
    Dwarf_Small lc_std_op_count;

    /* ======== includes =========*/
    /*  Points to the portion of .debug_line section that
        contains a list of strings naming the included
        directories.  Do not free().
        No free even DWARF5?
        An array of pointers to strings.  */
    /*  DWARF 2,3,4: does not name the current dir of the compilation.
        DWARF5: Initial entry is the dir of the compilation. */
    Dwarf_Small **lc_include_directories;
    /*  Count of the number of included directories. */
    Dwarf_Unsigned lc_include_directories_count;

    /* count of uleb pairs */
    Dwarf_Unsigned lc_directory_entry_format_count; /* DWARF5 */

    Dwarf_Unsigned lc_directory_entry_values_count; /* DWARF5 */

    /*  This must be freed,malloc space, an array of the
        values of each entry.  DWARF5 */
    struct Dwarf_Unsigned_Pair_s * lc_directory_format_values;

    /* ======== end includes =========*/

    /* ======== file names =========*/

    Dwarf_Unsigned lc_file_name_format_count; /* DWARF5 */
    Dwarf_Unsigned * lc_file_name_format; /* DWARF5 */
    Dwarf_Unsigned lc_file_entry_values_count; /* DWARF5 */
    /*  This must be freed,malloc space, an array of the
        values of each entry. */
    struct Dwarf_Unsigned_Pair_s * lc_file_format_values; /* DWARF5 */

    /*  Points to a singly-linked list of entries providing info
        about source files
        for the current set of Dwarf_Line structures.
        The initial  entry on the list is 'file 1'
        per DWARF2,3,4 rules.
        And so on.  lc_last_entry points at the last entry
        in the list (so we can easily expand the list).
        It's a list (not a table) since we may encounter
        DW_LNE_define_file entries.
        For Dwarf5 the initial entry is 'file 0'
        and must match the CU-DIE DW_AT_name string. */
    Dwarf_File_Entry lc_file_entries;
    Dwarf_File_Entry lc_last_entry;
    /*  Count of number of source files for this set of Dwarf_Line
        structures. */
    Dwarf_Unsigned lc_file_entry_count; /* all versions */
    /*  Values Easing the process of indexing
        through lc_file_entries. */
    Dwarf_Unsigned lc_file_entry_baseindex;
    Dwarf_Unsigned lc_file_entry_endindex;
    /* ======== end file names =========*/

    /*  Points to an array of subprogram entries.
        With Two level line tables this may be non-zero.
        An array of Dwarf_Subprogram_Entry_s structs. */
        Dwarf_Subprog_Entry lc_subprogs;
    /*  Count of the number of subprogram entries
        With Two level line tables this may be non-zero. */
    Dwarf_Unsigned lc_subprogs_count;

    /*  Count of the number of lines for this cu. */
    Dwarf_Unsigned lc_line_count;

    /*  Points to name of compilation directory.
        That string is in a .debug section  (DWARF 2,3,4)
        so do not free this. For DWARF5 must be the same
        as lc_include_directories[0] */
    Dwarf_Small *lc_compilation_directory;
    Dwarf_Debug lc_dbg;
    /*  zero table count is skeleton, or just missing names.
        1 is standard table.
        2 means two-level table (experimental)
        Other is a bug somewhere.  */
    Dwarf_Small lc_table_count;
    Dwarf_Bool lc_is_single_table;

    /* For standard line tables  the logicals are
        the only tables and linecount_actuals is 0. */
    Dwarf_Line   *lc_linebuf_logicals;
    Dwarf_Unsigned lc_linecount_logicals;

    /* Non-zero only if two-level table with actuals */
    Dwarf_Line   *lc_linebuf_actuals;
    Dwarf_Unsigned lc_linecount_actuals;
};

/*  The line table set of registers.
    The state machine state variables.
    Using names from the DWARF documentation
    but preceded by lr_.  */
struct Dwarf_Line_Registers_s {
    Dwarf_Addr lr_address;        /* DWARF2 */
    Dwarf_Unsigned lr_file ;          /* DWARF2 */
    Dwarf_Unsigned lr_line ;          /* DWARF2 */
    Dwarf_Unsigned lr_column ;        /* DWARF2 */
    Dwarf_Bool lr_is_stmt;        /* DWARF2 */
    Dwarf_Bool lr_basic_block;    /* DWARF2 */
    Dwarf_Bool lr_end_sequence;   /* DWARF2 */
    Dwarf_Bool lr_prologue_end;   /* DWARF3 */
    Dwarf_Bool lr_epilogue_begin; /* DWARF3 */
    Dwarf_Half lr_isa;            /* DWARF3 */
    Dwarf_Unsigned lr_op_index;   /* DWARF4, operation
        within VLIW instruction. */
    Dwarf_Unsigned lr_discriminator; /* DWARF4 */
    Dwarf_Unsigned lr_call_context;       /* EXPERIMENTAL */
    Dwarf_Unsigned lr_subprogram;     /* EXPERIMENTAL */
};
typedef struct Dwarf_Line_Registers_s *Dwarf_Line_Registers;
void _dwarf_set_line_table_regs_default_values(
    Dwarf_Line_Registers regs,
    unsigned lineversion,
    Dwarf_Bool is_stmt);

/*
    This structure defines a row of the line table.
    All of the fields
    same meaning that is defined in Section 6.2.2
    of the Libdwarf Document.

*/
struct Dwarf_Line_s {
    Dwarf_Addr li_address;  /* pc value of machine instr */
    struct li_inner_s {
        /* New as of DWARF4 */
        Dwarf_Unsigned li_discriminator;

        /*  int identifying src file
            li_file is a number 1-N, indexing into a conceptual
            source file table as described in dwarf2/3 spec line
            table doc. (see Dwarf_File_Entry lc_file_entries; and
            Dwarf_Unsigned lc_file_entry_count;) */
        Dwarf_Unsigned li_file;

        /*  In single-level table is line number in
            source file. 1-N
            In logicals table is not used.
            In actuals table is index into logicals table. 1-N*/
        Dwarf_Unsigned li_line;

        Dwarf_Half li_column; /*source file column number 1-N */
        Dwarf_Half li_isa;   /*New as of DWARF4. */

        /*  Two-level line tables.
            Is index from logicals table
            into logicals table. 1-N */
        Dwarf_Unsigned li_call_context;

        /*  Two-level line tables.
            is index into subprograms table. 1-N */
        Dwarf_Unsigned li_subprogram;

        /* To save space, use bit flags. */
        /* indicate start of stmt */
        unsigned li_is_stmt:1;

        /* indicate start basic block */
        unsigned li_basic_block:1;

        /* first post sequence instr */
        unsigned li_end_sequence:1;

        unsigned li_prologue_end:1;
        unsigned li_epilogue_begin:1;

        /* Mark a line record as being DW_LNS_set_address. */
        unsigned li_is_addr_set:1;
    } li_l_data;
    Dwarf_Line_Context li_context; /* assoc Dwarf_Line_Context_s */

    /*  Set only on the actuals table of a two-level line table.
        Assists in the dealloc code.
    */
    Dwarf_Bool li_is_actuals_table;
};

int _dwarf_line_address_offsets(Dwarf_Debug dbg,
    Dwarf_Die die,
    Dwarf_Addr ** addrs,
    Dwarf_Off ** offs,
    Dwarf_Unsigned * returncount,
    Dwarf_Error * err);
int _dwarf_internal_srclines(Dwarf_Die die,
    Dwarf_Bool old_interface,
    Dwarf_Unsigned * version,
    Dwarf_Small     * table_count,
    Dwarf_Line_Context *line_context,
    Dwarf_Line ** linebuf,
    Dwarf_Signed * count,
    Dwarf_Line ** linebuf_actuals,
    Dwarf_Signed * count_actuals,
    Dwarf_Bool doaddrs,
    Dwarf_Bool dolines,
    Dwarf_Error * error);

/*  The LOP, WHAT_IS_OPCODE stuff is here so it can
    be reused in 3 places.  Seemed hard to keep
    the 3 places the same without an inline func or
    a macro.

    Handling the line section where the header and the
    file being processed do not match (unusual, but
    planned for in the  design of .debug_line)
    is too tricky to recode this several times and keep
    it right.

*/
#define LOP_EXTENDED 1
#define LOP_DISCARD  2
#define LOP_STANDARD 3
#define LOP_SPECIAL  4

/* ASSERT: sets type to one of the above 4. Never anything else. */
#define WHAT_IS_OPCODE(type,opcode,base,opcode_length,\
line_ptr,highest_std) \
    if ((opcode) < (base)) {                             \
        /*  we know we must treat as a standard op       \
            or a special case. */                        \
        if ((opcode) == DW_EXTENDED_OPCODE) {            \
            (type) = LOP_EXTENDED;                         \
        } else if (((highest_std)+1) >= (base)) {        \
            /*  == Standard case: compile of             \
                dwarf_line.c and object                  \
                have same standard op codes set.         \
                == Special case: compile of dwarf_line.c \
                has things in standard op codes list     \
                in dwarf.h header not                    \
                in the object: handle this as a standard \
                op code in switch below.                 \
                The header special ops overlap the       \
                object standard ops.                     \
                The new standard op codes will not       \
                appear in the object. */                 \
            (type) = LOP_STANDARD;                         \
        } else  {                                        \
            /* These are standard opcodes in the object  \
            ** that were not defined  in the header      \
            ** at the time dwarf_line.c                  \
            ** was compiled. Provides the ability of     \
            ** out-of-date dwarf reader to read newer    \
            ** line table data transparently.            \
            */                                           \
            (type) = LOP_DISCARD;                          \
        }                                                \
    } else {                                             \
        /* Is  a special op code. */                     \
        (type) =  LOP_SPECIAL;                             \
    }

/*  The following is from  the dwarf definition of 'ubyte'
    and is specifically  mentioned in section  6.2.5.1, page 54
    of the Rev 2.0.0 dwarf specification.
*/

#define MAX_LINE_OP_CODE  255

/*  Operand counts per standard operand.
    The initial zero is for DW_LNS_copy.
    This is an economical way to verify we understand the table
    of standard-opcode-lengths in the line table prologue.  */
#define STANDARD_OPERAND_COUNT_DWARF2 9
#define STANDARD_OPERAND_COUNT_DWARF3 12
/*  For two-level line tables, we have three additional
    standard opcodes. */
#define STANDARD_OPERAND_COUNT_TWO_LEVEL 15

void _dwarf_print_header_issue(Dwarf_Debug dbg,
    const char *specific_msg,
    Dwarf_Small *data_start,
    Dwarf_Signed value,
    unsigned index,
    unsigned tabv,
    unsigned linetabv,
    int *err_count_out);
int _dwarf_decode_line_string_form(Dwarf_Debug dbg,
    Dwarf_Unsigned attrnum,
    Dwarf_Unsigned form,
    Dwarf_Unsigned offset_size,
    Dwarf_Small **line_ptr,
    Dwarf_Small *line_ptr_end,
    char **return_str,
    Dwarf_Error * error);
int _dwarf_decode_line_udata_form(Dwarf_Debug dbg,
    Dwarf_Unsigned attrnum,
    Dwarf_Unsigned form,
    Dwarf_Small **line_ptr,
    Dwarf_Unsigned *return_val,
    Dwarf_Small *line_end_ptr,
    Dwarf_Error * error);
void _dwarf_report_bad_lnct( Dwarf_Debug dbg,
    Dwarf_Unsigned ltype,
    int dlecode,
    const char * dlename,
    Dwarf_Error *err);

void _dwarf_update_chain_list( Dwarf_Chain chain_line,
    Dwarf_Chain *head_chain, Dwarf_Chain *curr_chain);
void _dwarf_free_chain_entries(Dwarf_Debug dbg,Dwarf_Chain head,
    Dwarf_Unsigned count);

int _dwarf_line_context_constructor(Dwarf_Debug dbg, void *m);
void _dwarf_line_context_destructor(void *m);

void _dwarf_print_line_context_record(Dwarf_Debug dbg,
    Dwarf_Line_Context line_context);
void _dwarf_context_src_files_destroy(Dwarf_Line_Context context);
int _dwarf_add_to_files_list(Dwarf_Line_Context context,
    Dwarf_File_Entry fe);
