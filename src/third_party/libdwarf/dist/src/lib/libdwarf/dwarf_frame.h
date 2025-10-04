/*
Copyright (C) 2000, 2004, 2006 Silicon Graphics, Inc.  All Rights Reserved.
Portions Copyright (C) 2021-2023 David Anderson. All Rights Reserved.

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

/*  The dwarf 2.0 standard dictates that only the following
    fields can be read when an unexpected augmentation string
    (in the cie) is encountered: CIE length, CIE_id, version and
    augmentation; FDE: length, CIE pointer, initial location and
    address range. Unfortunately, with the above restrictions, it
    is impossible to read the instruction table from a CIE or a FDE
    when a new augmentation string is encountered.
    To fix this problem, the following layout is used, if the
    augmentation string starts with the string "z".
        CIE                        FDE
        length                     length
        CIE_id                     CIE_pointer
        version                    initial_location
        augmentation               address_range

        -                          length_of_augmented_fields (*NEW*)
        code_alignment_factor      Any new fields as necessary
        data_alignment_factor      instruction_table
        return_address
        length_of_augmented fields
        Any new fields as necessary
        initial_instructions

    The type of all the old data items are the same as what is
    described in dwarf 2.0 standard. The length_of_augmented_fields
    is an LEB128 data item that denotes the size (in bytes) of
    the augmented fields (not including the size of
    "length_of_augmented_fields" itself).

    Handling of cie augmentation strings is necessarily a heuristic.
    See dwarf_frame.c for the currently known augmentation strings.

    ---START SGI-ONLY COMMENT:
    SGI-IRIX versions of cie or fde  were intended
    to use "z1", "z2" as the
    augmenter strings if required for new augmentation.
    However, that never happened (as of March 2005).

    The fde's augmented by the string "z" have a new field
    (signed constant, 4 byte field)
    called offset_into_exception_tables, following the
    length_of_augmented field.   This field contains an offset
    into the "_MIPS_eh_region", which describes
    the IRIX CC exception handling tables.
    ---END SGI-ONLY COMMENT

    GNU .eh_frame has an augmentation string of z[RLP]* (gcc 3.4)
    The similarity to IRIX 'z' (and proposed but never
    implemented IRIX z1, z2 etc) was confusing things.
    If the section is .eh_frame then 'z' means GNU exception
    information 'Augmentation Data' not IRIX 'z'.
    See The Linux Standard Base Core Specification version 3.0
*/

#define DW_DEBUG_FRAME_VERSION     1 /* DWARF2 */
#define DW_DEBUG_FRAME_VERSION3    3 /* DWARF3 */
#define DW_DEBUG_FRAME_VERSION4    4 /* DWARF4 */
/*  The following is SGI/IRIX specific, and probably no longer
    in use anywhere. */
#define DW_DEBUG_FRAME_AUGMENTER_STRING     "mti v1"

/* The value of the offset field for Cie's. */
#define DW_CIE_OFFSET ~(0x0)

/* The augmentation string may be NULL. */
#define DW_EMPTY_STRING ""

#define DW_FRAME_INSTR_OPCODE_SHIFT 6
#define DW_FRAME_INSTR_OFFSET_MASK  0x3f

/*  Frame description instructions expanded.
    Accessed via a function.
*/
struct Dwarf_Frame_Instr_s {
    /*  fp_op, if a base op, has the low 6 bits set zero here */
    Dwarf_Small     fi_op;
    /*  fp_instr_offset is within instructions */
    Dwarf_Unsigned  fi_instr_offset;
    Dwarf_Unsigned  fi_offset; /* frame offset */
    /*  A string like "u","r", or "rsd" denoting the field
        meanings. See the entire list of possibilities.
        Never free. */
    const char     *fi_fields;

    /*  letter r and u both use u struct elements. */
    Dwarf_Unsigned  fi_u0;
    Dwarf_Unsigned  fi_u1;
    Dwarf_Unsigned  fi_u2; /* LLVM extension */
    Dwarf_Signed    fi_s0;
    Dwarf_Signed    fi_s1;
    Dwarf_Unsigned  fi_code_align_factor;
    Dwarf_Signed    fi_data_align_factor;

    Dwarf_Block     fi_expr;
    /*  Used to prepare a list, which
        is then turned into array and this zeroed. */
    struct Dwarf_Frame_Instr_s *fi_next;
};
typedef struct Dwarf_Frame_Instr_s * Dwarf_Frame_Instr;

struct Dwarf_Frame_Instr_Head_s {
    /*  fp_op, if a base op, has the low 6 bits set zero here */
    Dwarf_Debug     fh_dbg;
    Dwarf_Cie       fh_cie;

    /* array of pointers to Dwarf_Frame_Instr_s */
    Dwarf_Frame_Instr *fh_array;
    Dwarf_Unsigned     fh_array_count;
};

/*
    This struct denotes the rule for a register in a row of
    the frame table.  In other words, it is one element of
    the table.
*/
struct Dwarf_Reg_Rule_s {

    /*  Is a flag indicating whether the rule includes the offset
        field, ie whether the ru_soffset field is valid or not.
        Applies only if DW_EXPR_OFFSET or DW_EXPR_VAL_OFFSET.
        It is important, since reg+offset (offset of 0)
        is different from
        just 'register' since the former means 'read memory at address
        given by the sum of register contents plus offset to get the
        value'. whereas the latter
        means 'the value is in the register'.
    */
    Dwarf_Sbyte ru_is_offset;

    /*  This has to do with evaluating register
        instructions, not with printing frame instruction.
        DW_EXPR_OFFSET         0 ( DWARF2)
        DW_EXPR_VAL_OFFSET     1 (dwarf2/3)
        DW_EXPR_EXPRESSION     2 (dwarf2/3)
        DW_EXPR_VAL_EXPRESSION 3 (dwarf2/3)
        DW_EXPR_ARGS_SIZE  4     (GNU)
    */
    Dwarf_Sbyte ru_value_type;

    /*  Register involved in this rule, real or non-real-register.
        ru_value_type is DW_EXPR_OFFSET or DW_EXPR_VAL_OFFSET.
    */
    Dwarf_Unsigned ru_register;

    /*  Offset to add to register, if indicated by ru_is_offset.
        ru_value_type is DW_EXPR_OFFSET */
    Dwarf_Signed ru_offset;
    Dwarf_Unsigned ru_args_size; /* DW_CFA_GNU_args_size */
    /*  If ru_value_type is DW_EXPR_EXPRESSION
        or DW_EXPR_VAL_EXPRESSION this is filled in. */
    Dwarf_Block    ru_block;

};
/* Internal use only */
typedef struct Dwarf_Regtable_Entry3_s_i {
    Dwarf_Small     dw_offset_relevant;
    Dwarf_Small     dw_value_type;
    Dwarf_Unsigned  dw_regnum;
    Dwarf_Unsigned  dw_offset;
    Dwarf_Unsigned  dw_args_size; /* Not dealt with.  */
    Dwarf_Block     dw_block;
} Dwarf_Regtable_Entry3_i;
/* Internal use only */
typedef struct Dwarf_Regtable3_s_i {
    Dwarf_Regtable_Entry3_i  rt3_cfa_rule;
    Dwarf_Unsigned           rt3_reg_table_size;
    Dwarf_Regtable_Entry3_i *rt3_rules;
} Dwarf_Regtable3_i;

typedef struct Dwarf_Frame_s *Dwarf_Frame;

/*
    This structure represents a row of the frame table.
    Fr_loc is the pc value for this row, and Fr_reg
    contains the rule for each column.

    Entry DW_FRAME_CFA_COL of fr_reg was the traditional MIPS
    way of setting CFA.  cfa_rule is the new one.
*/
struct Dwarf_Frame_s {

    /* Pc value corresponding to this row of the frame table. */
    Dwarf_Addr fr_loc;

    /* Rules for all the registers in this row. */
    struct Dwarf_Reg_Rule_s fr_cfa_rule;

    /*  fr_reg_count is the the number of
        entries of the fr_reg array. */
    Dwarf_Unsigned fr_reg_count;
    struct Dwarf_Reg_Rule_s *fr_reg;

    Dwarf_Frame fr_next;
};

/* See dwarf_frame.c for the heuristics used to set the
   Dwarf_Cie ci_augmentation_type.

   This succinctly helps interpret the size and
   meaning of .debug_frame
   and (for gcc) .eh_frame.

   In the case of gcc .eh_frame (gcc 3.3, 3.4)
   z may be followed by one or more of
   L R P.

*/
enum Dwarf_augmentation_type {
    aug_empty_string, /* Default empty augmentation string.  */
    aug_irix_exception_table,  /* IRIX  plain  "z",
        for exception handling, IRIX CC compiler.
        Proposed z1 z2 ... never implemented.  */
    aug_gcc_eh_z,       /* gcc z augmentation,  (including
        L R P variations). gcc 3.3 3.4 exception
        handling in eh_frame.  */
    aug_irix_mti_v1,  /* IRIX "mti v1" augmentation string. Probably
        never in any released SGI-IRIX compiler. */
    aug_eh,           /* For gcc .eh_frame, "eh" is the string.,
        gcc 1,2, egcs. Older values.  */
    aug_armcc,  /* "armcc+" meaning the  cfa calculation
        is corrected to be standard (output by
        Arm C RVCT 3.0 SP1 and later). See
        http://sourceware.org/ml/gdb-patches/2006-12/msg00249.html
        for details. */
    aug_unknown,      /* Unknown augmentation, we cannot do much. */

    /*  HC, From http://sourceforge.net/p/elftoolchain/tickets/397/ */
    aug_metaware,

    aug_past_last
};

/*
    This structure contains all the pertinent info for a Cie. Most
    of the fields are taken straight from the definition of a Cie.
    Ci_cie_start points to the address (in .debug_frame) where this
    Cie begins.  Ci_cie_instr_start points to the first byte of the
    frame instructions for this Cie.  Ci_dbg points to the associated
    Dwarf_Debug structure.  Ci_initial_table is a pointer to the table
    row generated by the instructions for this Cie.
*/
struct Dwarf_Cie_s {
    Dwarf_Unsigned ci_length;
    char *ci_augmentation;
    Dwarf_Unsigned ci_code_alignment_factor;
    Dwarf_Signed   ci_data_alignment_factor;
    Dwarf_Unsigned ci_return_address_register;
    Dwarf_Small *ci_cie_start;
    Dwarf_Small *ci_cie_instr_start;
    Dwarf_Small *ci_cie_end;
    Dwarf_Debug ci_dbg;
    Dwarf_Frame ci_initial_table;
    Dwarf_Cie ci_next;
    Dwarf_Half ci_length_size;
    Dwarf_Half ci_extension_size;
    Dwarf_Half ci_cie_version_number;
    enum Dwarf_augmentation_type ci_augmentation_type;

    /*  The following 2 for GNU .eh_frame exception handling
        Augmentation Data. Set if ci_augmentation_type
        is aug_gcc_eh_z. Zero if unused. */
    Dwarf_Unsigned ci_gnu_eh_augmentation_len;
    Dwarf_Ptr      ci_gnu_eh_augmentation_bytes;

    /*  These are extracted from the gnu eh_frame
        augmentation if the
        augmentation begins with 'z'. See Linux LSB documents.
        Otherwize these are zero. */
    unsigned char    ci_gnu_personality_handler_encoding;
    unsigned char    ci_gnu_lsda_encoding;
    unsigned char    ci_gnu_fde_begin_encoding;

    /*  If 'P' augmentation present, is handler addr. Else
        is zero. */
    Dwarf_Addr     ci_gnu_personality_handler_addr;

    /*  In creating list of cie's (which will become an array)
        record the position so fde can get it on fde creation. */
    Dwarf_Unsigned ci_index;
    Dwarf_Small *  ci_section_ptr;
    Dwarf_Unsigned ci_section_length;
    Dwarf_Small *  ci_section_end;
    /*  DWARF4 adds address size and segment size to the CIE:
        the .debug_info
        section may not always be present to allow libdwarf to
        find address_size from the compilation-unit. */
    Dwarf_Half   ci_address_size;
    Dwarf_Half   ci_segment_size;

};

/*
    This structure contains all the pertinent info for a Fde.
    Most of the fields are taken straight from the definition.
    fd_cie_index is the index of the Cie associated with this
    Fde in the list of Cie's for this debug_frame.  Fd_cie
    points to the corresponding Dwarf_Cie structure.  Fd_fde_start
    points to the start address of the Fde.  Fd_fde_instr_start
    points to the start of the instructions for this Fde.  Fd_dbg
    points to the associated Dwarf_Debug structure.
*/
struct Dwarf_Fde_s {
    Dwarf_Unsigned fd_length;
    Dwarf_Addr     fd_cie_offset;
    Dwarf_Unsigned fd_cie_index;
    Dwarf_Cie      fd_cie;
    Dwarf_Addr     fd_initial_location;
    Dwarf_Small   *fd_initial_loc_pos;
    Dwarf_Addr     fd_address_range;
    Dwarf_Small   *fd_fde_start;
    Dwarf_Small   *fd_fde_instr_start;
    Dwarf_Small   *fd_fde_end;
    Dwarf_Debug    fd_dbg;

    /*  fd_offset_into_exception_tables is SGI/IRIX exception table
        offset. Unused and zero if not IRIX .debug_frame. */
    Dwarf_Signed   fd_offset_into_exception_tables;

    Dwarf_Fde      fd_next;
    Dwarf_Small    fd_length_size;
    Dwarf_Small    fd_extension_size;
    /*  So we know from an fde which 'count' of fde-s in
        Dwarf_Debug applies:  eh or standard. */
    Dwarf_Small    fd_is_eh;
    /*  The following 2 for GNU .eh_frame exception handling
        Augmentation Data. Set if CIE ci_augmentation_type
        is aug_gcc_eh_z. Zero if unused. */
    Dwarf_Unsigned fd_gnu_eh_augmentation_len;
    Dwarf_Bool     fd_gnu_eh_aug_present;
    Dwarf_Ptr      fd_gnu_eh_augmentation_bytes;
    Dwarf_Addr     fd_gnu_eh_lsda; /* If 'L' augmentation letter
        present:  is address of the
        Language Specific Data Area (LSDA). If not 'L" is zero. */

    /* The following 3 are about the Elf section the FDEs come from.*/
    Dwarf_Small   *fd_section_ptr;
    Dwarf_Unsigned fd_section_length;
    Dwarf_Unsigned fd_section_index;
    Dwarf_Small   *fd_section_end;

    /*  If fd_eh_table_value_set is true, then fd_eh_table_value is
        meaningful.  Never meaningful for .debug_frame, is
        part of .eh_frame. */
    Dwarf_Unsigned fd_eh_table_value;
    Dwarf_Bool     fd_eh_table_value_set;

    /* The following are memoization to save recalculation. */
    struct Dwarf_Frame_s fd_fde_table;
    Dwarf_Addr     fd_fde_pc_requested;
    Dwarf_Bool     fd_have_fde_tab;

    /*  Set by dwarf_get_fde_for_die() */
    Dwarf_Bool     fd_fde_owns_cie;

};

int
_dwarf_validate_register_numbers(Dwarf_Debug dbg,Dwarf_Error *error);

int
_dwarf_frame_address_offsets(Dwarf_Debug dbg, Dwarf_Addr ** addrlist,
    Dwarf_Off ** offsetlist,
    Dwarf_Signed * returncount,
    Dwarf_Error * err);

int
_dwarf_get_fde_list_internal(Dwarf_Debug dbg,
    Dwarf_Cie ** cie_data,
    Dwarf_Signed * cie_element_count,
    Dwarf_Fde ** fde_data,
    Dwarf_Signed * fde_element_count,
    Dwarf_Small * section_ptr,
    Dwarf_Unsigned section_index,
    Dwarf_Unsigned section_length,
    Dwarf_Unsigned cie_id_value,
    int use_gnu_cie_calc,  /* If non-zero,
    this is gcc eh_frame. */
    Dwarf_Error * error);

enum Dwarf_augmentation_type
_dwarf_get_augmentation_type(Dwarf_Debug dbg,
    Dwarf_Small *augmentation_string,
    int is_gcc_eh_frame);
int _dwarf_fde_section_offset(Dwarf_Debug /*dbg*/,
    Dwarf_Fde         /*in_fde*/,
    Dwarf_Off *       /*fde_off*/,
    Dwarf_Off *       /*cie_off*/,
    Dwarf_Error *     /*err*/);
int _dwarf_cie_section_offset(Dwarf_Debug /*dbg*/,
    Dwarf_Cie     /*in_cie*/,
    Dwarf_Off *   /*cie_off */,
    Dwarf_Error * /*err*/);

int _dwarf_get_return_address_reg(Dwarf_Small *frame_ptr,
    int version,
    Dwarf_Debug dbg,
    Dwarf_Byte_Ptr section_end,
    unsigned long *size,
    Dwarf_Unsigned *return_address_register,
    Dwarf_Error *error);

/*  Temporary recording of crucial cie/fde prefix data.
    Vastly simplifies some argument lists.  */
struct cie_fde_prefix_s {
    /*  cf_start_addr is a pointer to the first byte
        of this fde/cie (meaning the length field itself) */
    Dwarf_Small *  cf_start_addr;
    /*  cf_addr_after_prefix is a pointer
        to the first byte of this fde/cie
        we are reading now, immediately following
        the length field read by READ_AREA_LENGTH. */
    Dwarf_Small *  cf_addr_after_prefix;
    /*  cf_length is the length field value from the cie/fde
        header.   */
    Dwarf_Unsigned cf_length;
    int            cf_local_length_size;
    int            cf_local_extension_size;
    Dwarf_Unsigned cf_cie_id;
    Dwarf_Small *  cf_cie_id_addr; /*used for eh_frame calculations.*/

    /*  Simplifies passing around these values to create fde having
        these here. */
    /*  cf_section_ptr is a pointer to the first byte
        of the object section the prefix is read from.  */
    Dwarf_Small *  cf_section_ptr;
    Dwarf_Unsigned cf_section_index;
    Dwarf_Unsigned cf_section_length;
};

int
_dwarf_exec_frame_instr(Dwarf_Bool make_instr,
    Dwarf_Bool search_pc,
    Dwarf_Addr search_pc_val,
    Dwarf_Addr initial_loc,
    Dwarf_Small * start_instr_ptr,
    Dwarf_Small * final_instr_ptr,
    Dwarf_Frame table,
    Dwarf_Cie cie,
    Dwarf_Debug dbg,
    Dwarf_Unsigned reg_num_of_cfa,
    Dwarf_Bool * has_more_rows,
    Dwarf_Addr * subsequent_pc,
    Dwarf_Frame_Instr_Head *ret_frame_instr_head,
    Dwarf_Unsigned * returned_frame_instr_count,
    Dwarf_Error *error);

int _dwarf_read_cie_fde_prefix(Dwarf_Debug dbg,
    Dwarf_Small *frame_ptr_in,
    Dwarf_Small *section_ptr_in,
    Dwarf_Unsigned section_index_in,
    Dwarf_Unsigned section_length_in,
    struct cie_fde_prefix_s *prefix_out,
    Dwarf_Error *error);

int _dwarf_create_fde_from_after_start(Dwarf_Debug dbg,
    struct cie_fde_prefix_s *  prefix,
    Dwarf_Small *section_pointer,
    Dwarf_Unsigned section_length,
    Dwarf_Small *frame_ptr,
    Dwarf_Small *section_ptr_end,
    int use_gnu_cie_calc,
    Dwarf_Cie  cie_ptr_in,
    Dwarf_Half address_size_in,
    Dwarf_Fde *fde_ptr_out,
    Dwarf_Error *error);

int _dwarf_create_cie_from_after_start(Dwarf_Debug dbg,
    struct cie_fde_prefix_s *prefix,
    Dwarf_Small* section_pointer,
    Dwarf_Small* frame_ptr,
    Dwarf_Small *section_ptr_end,
    Dwarf_Unsigned cie_count,
    int use_gnu_cie_calc,
    Dwarf_Cie *cie_ptr_out,
        Dwarf_Error *error);

int _dwarf_frame_constructor(Dwarf_Debug dbg,void * );
void _dwarf_frame_destructor (void *);
void _dwarf_fde_destructor (void *);
void _dwarf_frame_instr_destructor(void *);
