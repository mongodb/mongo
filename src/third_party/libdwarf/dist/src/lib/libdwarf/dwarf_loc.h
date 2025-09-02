/*
  Copyright (C) 2000, 2004 Silicon Graphics, Inc.  All Rights Reserved.
  Portions Copyright (C) 2015-2023 David Anderson. All Rights Reserved.

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
#ifndef DWARF_LOC_H
#define DWARF_LOC_H
#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define LOCLISTS_MAGIC  0xadab4

typedef struct Dwarf_Loc_Chain_s *Dwarf_Loc_Chain;
struct Dwarf_Loc_Chain_s {
    Dwarf_Small lc_atom;
    Dwarf_Unsigned lc_number;
    Dwarf_Unsigned lc_number2;
    Dwarf_Unsigned lc_number3;
    Dwarf_Unsigned lc_offset;
    Dwarf_Unsigned lc_opnumber;
    Dwarf_Loc_Chain lc_next;
};

/*  Contains info on an uninterpreted block of data,
    the data is DWARF location expression operators.  */
struct Dwarf_Block_c_s {
    /* length of block bl_data points at */
    Dwarf_Unsigned  bl_len;

    /*  Uninterpreted data, location expressions,
        DW_OP_reg31 etc */
    Dwarf_Byte_Ptr       bl_data;

    /*  DW_LKIND, see libdwarf.h.in  */
    Dwarf_Half     bl_kind;

    /* Section (not CU) offset which 'data' comes from. */
    Dwarf_Unsigned  bl_section_offset;

    /*  Section offset where the location description
        itself starts.  So a few bytes lower than
        bl_section_offset */
    Dwarf_Unsigned  bl_locdesc_offset;
};
typedef struct Dwarf_Block_c_s Dwarf_Block_c;

/* Location record. Records up to 3 operand values.
   For DWARF5 ops with a 1 byte size and then a block
   of data of that size we the size in an operand
   and follow that with the next operand as a
   pointer to the block. The pointer is inserted
   via  cast, so an ugly hack.
   This struct is opaque. Not visible to callers.
*/
typedef struct Dwarf_Loc_Expr_Op_s *Dwarf_Loc_Expr_Op;
struct Dwarf_Loc_Expr_Op_s {
    Dwarf_Small     lr_atom;    /* Location operation */

    Dwarf_Unsigned  lr_raw1;
    Dwarf_Unsigned  lr_raw2;
    Dwarf_Unsigned  lr_raw3;

    Dwarf_Unsigned  lr_number;  /* First operand */

    /*  Second operand.
        For OP_bregx, OP_bit_piece, OP_[GNU_]const_type,
        OP_[GNU_]deref_type, OP_[GNU_]entry_value,
        OP_implicit_value,
        OP_[GNU_]implicit_pointer, OP_[GNU_]regval_type,
        OP_xderef_type,  */
    Dwarf_Unsigned  lr_number2;

    /*  Third Operand.
        For OP_[GNU_]const type, pointer to
        block of length 'lr_number2'
        FIXME: retrieve the value at the pointer,
        store the value here instead*/
    Dwarf_Unsigned  lr_number3;

    /*  The number assigned. 0 to the number-of-ops-1 in
        the expression we are expanding. */
    Dwarf_Unsigned  lr_opnumber;
    Dwarf_Unsigned  lr_offset;     /* offset  for OP_BRA etc */
    Dwarf_Loc_Expr_Op     lr_next; /* When a list is useful.*/
};

/*  Used at construction to enable verifying we set
    sensibly before returning to callers. */

#define DW_LLE_VALUE_BOGUS 254

/*  Location description DWARF 2,3,4,5
    Adds the DW_LLE value (new in DWARF5).
    This struct is opaque. Not visible to callers. */
struct Dwarf_Locdesc_c_s {
    Dwarf_Half       ld_lkind; /* DW_LKIND */

    /*  A DW_LLEX or DW_LLE value, real or synthesized */
    Dwarf_Small      ld_lle_value;
    /*  Failed means .debug_addr section needed but missing.
        (possibly tied file needed) */
    Dwarf_Bool       ld_index_failed;
    /* the length of data in this lle entry */
    Dwarf_Unsigned   ld_lle_bytecount;
    unsigned long    ld_magic;

    /*  Beginning of active range. This is actually an offset
        of an applicable base address, not a pc value.  */
    Dwarf_Addr       ld_rawlow;
    /*  Translated to address */
    Dwarf_Addr       ld_lopc;

    /*  End of active range. This is actually an offset
        of an applicable base address,
        or a length, never a pc value.  */
    Dwarf_Addr       ld_rawhigh;
    /*  Translated to address */
    Dwarf_Addr       ld_highpc;

    /*  Byte length of the  entire record for this entry,
        including any DW_OP entries */
    Dwarf_Unsigned   ld_entrylen;

    /*   For .debug_loclists, eases building record. */
    Dwarf_Block_c    ld_opsblock;

    /*  count of struct Dwarf_Loc_Expr_Op_s (expression operators)
        in array. */
    Dwarf_Half       ld_cents;
    /* pointer to array of expression operator structs */
    Dwarf_Loc_Expr_Op      ld_s;

    /* Section (not CU) offset where loc-expr begins*/
    Dwarf_Unsigned   ld_section_offset;

    /* Section (not CU) offset where location descr begins*/
    Dwarf_Unsigned   ld_locdesc_offset;

    /* Pointer to our header (in which we are located). */
    Dwarf_Loc_Head_c ld_loclist_head;
    Dwarf_Locdesc_c  ld_next; /*helps building the locdescs*/
};

int _dwarf_locdesc_c_constructor(Dwarf_Debug dbg, void *locd);

/*  A 'header' to the loclist and  the
    location description(s)  attached to an attribute.
    This struct is opaque. The contents not visible to
    callers. */
struct Dwarf_Loc_Head_c_s {
    /*  The array (1 or more entries) of
        struct Loc_Desc_c_s
        If 1 it may really be a locexpr */
    Dwarf_Locdesc_c  ll_locdesc;

    /*  Entry count of the ll_locdesc array.  */
    Dwarf_Unsigned   ll_locdesc_count;

    Dwarf_Half       ll_attrnum;
    Dwarf_Half       ll_attrform;
    Dwarf_Half       ll_cuversion;
    unsigned         ll_address_size;
    unsigned         ll_offset_size;
    /*  The CU Context of this loclist or locexpr. */
    Dwarf_CU_Context ll_context;
    /* DW_LKIND*    */
    Dwarf_Half       ll_lkind; /* DW_LKIND */
    Dwarf_Debug      ll_dbg;
    unsigned long    ll_magic;

    /*  If ll_lkind == DW_LKIND_loclists the following
        pointer is non-null and index is the index
        of the localcontext */
    Dwarf_Unsigned         ll_index;
    Dwarf_Loclists_Context ll_localcontext;

    /*  rh_last and rh_first used during build-up.
        Zero when array rh_loclists built. */
    Dwarf_Locdesc_c  ll_first;
    Dwarf_Locdesc_c  ll_last;
    Dwarf_Unsigned   ll_bytes_total;
    unsigned         ll_segment_selector_size;

    /*  DW_AT_loclists_base */
    Dwarf_Bool       ll_at_loclists_base_present;
    Dwarf_Unsigned   ll_at_loclists_base;

    /*  DW_AT_low_pc originally, , perhaps interited
        into dwp from skeleton. */
    Dwarf_Bool       ll_cu_base_address_present;
    Dwarf_Unsigned   ll_cu_base_address;

    /*  DW_AT_addr_base, so we can use .debug_addr
        if such is needed. */
    Dwarf_Bool       ll_cu_addr_base_offset_present;
    Dwarf_Unsigned   ll_cu_addr_base_offset;

    Dwarf_Small    * ll_llepointer;
    Dwarf_Unsigned   ll_llearea_offset;
    Dwarf_Small    * ll_end_data_area;
};

int _dwarf_fill_in_locdesc_op_c(Dwarf_Debug dbg,
    Dwarf_Unsigned locdesc_index,
    Dwarf_Loc_Head_c loc_head,
    Dwarf_Block_c * loc_block,
    Dwarf_Half address_size,
    Dwarf_Half offset_size,
    Dwarf_Half version_stamp,
    Dwarf_Addr lowpc,
    Dwarf_Addr highpc,
    Dwarf_Half lle_op,
    Dwarf_Error * error);

int _dwarf_loc_block_sanity_check(Dwarf_Debug dbg,
    Dwarf_Block_c *loc_block,Dwarf_Error*error);

int _dwarf_internal_read_loclists_header(Dwarf_Debug dbg,
    Dwarf_Bool temporary,
    Dwarf_Unsigned contextnum,
    Dwarf_Unsigned sectionlength,
    Dwarf_Small *data,
    Dwarf_Small *end_data,
    Dwarf_Unsigned offset,
    Dwarf_Loclists_Context  buildhere,
    Dwarf_Unsigned *next_offset,
    Dwarf_Error *error);

void _dwarf_loclists_head_destructor(void *l);

int _dwarf_loclists_fill_in_lle_head(Dwarf_Debug dbg,
    Dwarf_Attribute attr,
    Dwarf_Half theform,
    Dwarf_Unsigned attr_val,
    Dwarf_Loc_Head_c llhead,
    Dwarf_Error *error);

int _dwarf_loclists_expression_build(Dwarf_Debug dbg,
    Dwarf_Attribute attr,
    Dwarf_Loc_Head_c* llhead,
    Dwarf_Error *error);

int _dwarf_read_loc_expr_op(Dwarf_Debug dbg,
    Dwarf_Block_c * loc_block,
    /* Caller: Start numbering at 0. */
    Dwarf_Signed opnumber,

    /* 2 for DWARF 2 etc. */
    Dwarf_Half version_stamp,
    Dwarf_Half offset_size, /* 4 or 8 */
    Dwarf_Half address_size, /* 2,4, 8  */
    Dwarf_Signed startoffset_in, /* offset in block,
        not section offset */
    Dwarf_Small *section_end,

    /* nextoffset_out so caller knows next entry startoffset */
    Dwarf_Unsigned *nextoffset_out,

    /*  The values picked up. */
    Dwarf_Loc_Expr_Op curr_loc,
    Dwarf_Error * error);
void _dwarf_free_loclists_head_content(Dwarf_Loc_Head_c head);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DWARF_LOC_H */
