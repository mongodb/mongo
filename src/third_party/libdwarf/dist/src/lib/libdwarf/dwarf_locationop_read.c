/*
  Copyright (C) 2000-2004 Silicon Graphics, Inc.  All Rights Reserved.
  Portions Copyright (C) 2007-2018 David Anderson. All Rights Reserved.
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

#include <config.h>

#include <string.h> /* memcpy() memset() */
#include <stdio.h> /* printf when debugging */

#if defined(_WIN32) && defined(HAVE_STDAFX_H)
#include "stdafx.h"
#endif /* HAVE_STDAFX_H */

#ifdef HAVE_STDINT_H
#include <stdint.h> /* uintptr_t */
#endif /* HAVE_STDINT_H */

#include "dwarf.h"
#include "libdwarf.h"
#include "dwarf_local_malloc.h"
#include "libdwarf_private.h"
#include "dwarf_base_types.h"
#include "dwarf_opaque.h"
#include "dwarf_alloc.h"
#include "dwarf_error.h"
#include "dwarf_util.h"
#include "dwarf_loc.h"
#include "dwarf_string.h"

/*  Richard Henderson, on DW_OP_GNU_encoded_addr:
    The operand is an absolute address.
    The first byte of the value is an encoding length:
    0 2 4 or 8.
    If zero it means the following is address-size.
    The address then follows immediately for
    that number of bytes. */
static int
read_encoded_addr(Dwarf_Small *loc_ptr,
    Dwarf_Debug dbg,
    Dwarf_Small *section_end_ptr,
    Dwarf_Half address_size,
    Dwarf_Unsigned * val_out,
    int * len_out,
    Dwarf_Error *error)
{
    int len = 0;
    Dwarf_Half op = *loc_ptr;
    Dwarf_Unsigned operand = 0;

    len++;
    if (!op) {
        op = address_size;
    }
    switch (op) {
    case 1:
        *val_out = *loc_ptr;
        len++;
        break;

    case 2:
        READ_UNALIGNED_CK(dbg, operand, Dwarf_Unsigned, loc_ptr, 2,
            error,section_end_ptr);
        *val_out = operand;
        len +=2;
        break;
    case 4:
        READ_UNALIGNED_CK(dbg, operand, Dwarf_Unsigned, loc_ptr, 4,
            error,section_end_ptr);
        *val_out = operand;
        len +=4;
        break;
    case 8:
        READ_UNALIGNED_CK(dbg, operand, Dwarf_Unsigned, loc_ptr, 8,
            error,section_end_ptr);
        *val_out = operand;
        len +=8;
        break;
    default:
        /* We do not know how much to read. */
        _dwarf_error(dbg, error, DW_DLE_GNU_OPCODE_ERROR);
        return DW_DLV_ERROR;
    };
    *len_out = len;
    return DW_DLV_OK;
}

/*  Return DW_DLV_NO_ENTRY when at the end of
    the ops for this block (a single Dwarf_Loccesc
    and multiple Dwarf_Locs will eventually result
    from calling this till DW_DLV_NO_ENTRY).

    All op reader code should call this to
    extract operator fields. For any
    DWARF version.

*/
int
_dwarf_read_loc_expr_op(Dwarf_Debug dbg,
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
    Dwarf_Error * error)
{
    Dwarf_Small *loc_ptr = 0;
    Dwarf_Unsigned loc_len = 0;
    Dwarf_Unsigned offset = startoffset_in;
    Dwarf_Unsigned operand1 = 0;
    Dwarf_Unsigned operand2 = 0;
    Dwarf_Unsigned operand3 = 0;
    Dwarf_Small atom = 0;
    Dwarf_Unsigned leb128_length = 0;

    if (offset > loc_block->bl_len) {
        _dwarf_error(dbg,error,DW_DLE_LOCEXPR_OFF_SECTION_END);
        return DW_DLV_ERROR;
    }
    loc_len = loc_block->bl_len;
    if (offset == loc_len) {
        return DW_DLV_NO_ENTRY;
    }

    loc_ptr = (Dwarf_Small*)loc_block->bl_data + offset;
    if ((loc_ptr+1) > section_end) {
        _dwarf_error(dbg,error,DW_DLE_LOCEXPR_OFF_SECTION_END);
        return DW_DLV_ERROR;
    }
    memset(curr_loc,0,sizeof(*curr_loc));

    curr_loc->lr_opnumber = opnumber;
    curr_loc->lr_offset = offset;

    /*  loc_ptr is ok to deref, see loc_ptr+1 test just above. */
    atom = *(Dwarf_Small *) loc_ptr;
    loc_ptr++;
    offset++;
    curr_loc->lr_atom = atom;
    switch (atom) {
    case DW_OP_reg0:
    case DW_OP_reg1:
    case DW_OP_reg2:
    case DW_OP_reg3:
    case DW_OP_reg4:
    case DW_OP_reg5:
    case DW_OP_reg6:
    case DW_OP_reg7:
    case DW_OP_reg8:
    case DW_OP_reg9:
    case DW_OP_reg10:
    case DW_OP_reg11:
    case DW_OP_reg12:
    case DW_OP_reg13:
    case DW_OP_reg14:
    case DW_OP_reg15:
    case DW_OP_reg16:
    case DW_OP_reg17:
    case DW_OP_reg18:
    case DW_OP_reg19:
    case DW_OP_reg20:
    case DW_OP_reg21:
    case DW_OP_reg22:
    case DW_OP_reg23:
    case DW_OP_reg24:
    case DW_OP_reg25:
    case DW_OP_reg26:
    case DW_OP_reg27:
    case DW_OP_reg28:
    case DW_OP_reg29:
    case DW_OP_reg30:
    case DW_OP_reg31:
        break;

    case DW_OP_regx:
        DECODE_LEB128_UWORD_LEN_CK(loc_ptr, operand1,leb128_length,
            dbg,error,section_end);
        offset = offset + leb128_length;
        break;

    case DW_OP_lit0:
    case DW_OP_lit1:
    case DW_OP_lit2:
    case DW_OP_lit3:
    case DW_OP_lit4:
    case DW_OP_lit5:
    case DW_OP_lit6:
    case DW_OP_lit7:
    case DW_OP_lit8:
    case DW_OP_lit9:
    case DW_OP_lit10:
    case DW_OP_lit11:
    case DW_OP_lit12:
    case DW_OP_lit13:
    case DW_OP_lit14:
    case DW_OP_lit15:
    case DW_OP_lit16:
    case DW_OP_lit17:
    case DW_OP_lit18:
    case DW_OP_lit19:
    case DW_OP_lit20:
    case DW_OP_lit21:
    case DW_OP_lit22:
    case DW_OP_lit23:
    case DW_OP_lit24:
    case DW_OP_lit25:
    case DW_OP_lit26:
    case DW_OP_lit27:
    case DW_OP_lit28:
    case DW_OP_lit29:
    case DW_OP_lit30:
    case DW_OP_lit31:
        operand1 = atom - DW_OP_lit0;
        break;

    case DW_OP_addr:
        READ_UNALIGNED_CK(dbg, operand1, Dwarf_Unsigned,
            loc_ptr, address_size,
            error,section_end);
        loc_ptr += address_size;
        offset += address_size;
        break;

    case DW_OP_const1u:
        if (loc_ptr >= section_end) {
            _dwarf_error(dbg,error,DW_DLE_LOCEXPR_OFF_SECTION_END);
            return DW_DLV_ERROR;
        }
        operand1 = *(Dwarf_Small *) loc_ptr;
        loc_ptr = loc_ptr + 1;
        if (loc_ptr > section_end) {
            _dwarf_error(dbg,error,DW_DLE_LOCEXPR_OFF_SECTION_END);
            return DW_DLV_ERROR;
        }
        offset = offset + 1;
        break;

    case DW_OP_const1s:
        if (loc_ptr >= section_end) {
            _dwarf_error(dbg,error,DW_DLE_LOCEXPR_OFF_SECTION_END);
            return DW_DLV_ERROR;
        }
        operand1 = *(Dwarf_Sbyte *) loc_ptr;
        SIGN_EXTEND(operand1,1);
        loc_ptr = loc_ptr + 1;
        if (loc_ptr > section_end) {
            _dwarf_error(dbg,error,DW_DLE_LOCEXPR_OFF_SECTION_END);
            return DW_DLV_ERROR;
        }
        offset = offset + 1;
        break;

    case DW_OP_const2u:
        READ_UNALIGNED_CK(dbg, operand1, Dwarf_Unsigned, loc_ptr, 2,
            error,section_end);
        loc_ptr = loc_ptr + 2;
        offset = offset + 2;
        break;

    case DW_OP_const2s:
        READ_UNALIGNED_CK(dbg, operand1, Dwarf_Unsigned, loc_ptr, 2,
            error, section_end);
        SIGN_EXTEND(operand1,2);
        loc_ptr = loc_ptr + 2;
        offset = offset + 2;
        break;

    case DW_OP_const4u:
        READ_UNALIGNED_CK(dbg, operand1, Dwarf_Unsigned, loc_ptr, 4,
            error, section_end);
        loc_ptr = loc_ptr + 4;
        offset = offset + 4;
        break;

    case DW_OP_const4s:
        READ_UNALIGNED_CK(dbg, operand1, Dwarf_Unsigned, loc_ptr, 4,
            error, section_end);
        SIGN_EXTEND(operand1,4);
        loc_ptr = loc_ptr + 4;
        offset = offset + 4;
        break;

    case DW_OP_const8u:
        READ_UNALIGNED_CK(dbg, operand1, Dwarf_Unsigned, loc_ptr, 8,
            error, section_end);
        loc_ptr = loc_ptr + 8;
        offset = offset + 8;
        break;

    case DW_OP_const8s:
        READ_UNALIGNED_CK(dbg, operand1, Dwarf_Unsigned, loc_ptr, 8,
            error, section_end);
        loc_ptr = loc_ptr + 8;
        offset = offset + 8;
        break;

    case DW_OP_constu:
        DECODE_LEB128_UWORD_LEN_CK(loc_ptr, operand1,leb128_length,
            dbg,error,section_end);
        offset = offset + leb128_length;
        break;

    case DW_OP_consts:
        DECODE_LEB128_SWORD_LEN_CK(loc_ptr, operand1,leb128_length,
            dbg,error,section_end);
        offset = offset + leb128_length;
        break;

    case DW_OP_fbreg:
        DECODE_LEB128_SWORD_LEN_CK(loc_ptr, operand1,leb128_length,
            dbg,error,section_end);
        offset = offset + leb128_length;
        break;

    case DW_OP_breg0:
    case DW_OP_breg1:
    case DW_OP_breg2:
    case DW_OP_breg3:
    case DW_OP_breg4:
    case DW_OP_breg5:
    case DW_OP_breg6:
    case DW_OP_breg7:
    case DW_OP_breg8:
    case DW_OP_breg9:
    case DW_OP_breg10:
    case DW_OP_breg11:
    case DW_OP_breg12:
    case DW_OP_breg13:
    case DW_OP_breg14:
    case DW_OP_breg15:
    case DW_OP_breg16:
    case DW_OP_breg17:
    case DW_OP_breg18:
    case DW_OP_breg19:
    case DW_OP_breg20:
    case DW_OP_breg21:
    case DW_OP_breg22:
    case DW_OP_breg23:
    case DW_OP_breg24:
    case DW_OP_breg25:
    case DW_OP_breg26:
    case DW_OP_breg27:
    case DW_OP_breg28:
    case DW_OP_breg29:
    case DW_OP_breg30:
    case DW_OP_breg31:
        DECODE_LEB128_SWORD_LEN_CK(loc_ptr, operand1,leb128_length,
            dbg,error,section_end);
        offset = offset + leb128_length;
        break;

    case DW_OP_bregx:
        /* uleb reg num followed by sleb offset */
        DECODE_LEB128_UWORD_LEN_CK(loc_ptr, operand1,leb128_length,
            dbg,error,section_end);
        offset = offset + leb128_length;

        DECODE_LEB128_SWORD_LEN_CK(loc_ptr, operand2,leb128_length,
            dbg,error,section_end);
        offset = offset + leb128_length;
        break;

    case DW_OP_dup:
    case DW_OP_drop:
        break;

    case DW_OP_pick:
        if (loc_ptr >= section_end) {
            _dwarf_error(dbg,error,DW_DLE_LOCEXPR_OFF_SECTION_END);
            return DW_DLV_ERROR;
        }
        operand1 = *(Dwarf_Small *) loc_ptr;
        loc_ptr = loc_ptr + 1;
        if (loc_ptr > section_end) {
            _dwarf_error(dbg,error,DW_DLE_LOCEXPR_OFF_SECTION_END);
            return DW_DLV_ERROR;
        }
        offset = offset + 1;
        break;

    case DW_OP_over:
    case DW_OP_swap:
    case DW_OP_rot:
    case DW_OP_deref:
        break;

    case DW_OP_deref_size:
        if (loc_ptr >= section_end) {
            _dwarf_error(dbg,error,DW_DLE_LOCEXPR_OFF_SECTION_END);
            return DW_DLV_ERROR;
        }
        operand1 = *(Dwarf_Small *) loc_ptr;
        loc_ptr = loc_ptr + 1;
        if (loc_ptr > section_end) {
            _dwarf_error(dbg,error,DW_DLE_LOCEXPR_OFF_SECTION_END);
            return DW_DLV_ERROR;
        }
        offset = offset + 1;
        break;

    case DW_OP_xderef:
        break;

    case DW_OP_xderef_type:        /* DWARF5 */
        if (loc_ptr >= section_end) {
            _dwarf_error(dbg,error,DW_DLE_LOCEXPR_OFF_SECTION_END);
            return DW_DLV_ERROR;
        }
        operand1 = *(Dwarf_Small *) loc_ptr;
        loc_ptr = loc_ptr + 1;
        if (loc_ptr > section_end) {
            _dwarf_error(dbg,error,DW_DLE_LOCEXPR_OFF_SECTION_END);
            return DW_DLV_ERROR;
        }
        offset = offset + 1;
        DECODE_LEB128_UWORD_LEN_CK(loc_ptr, operand2,leb128_length,
            dbg,error,section_end);
        offset = offset + leb128_length;

        break;

    case DW_OP_xderef_size:
        if (loc_ptr >= section_end) {
            _dwarf_error(dbg,error,DW_DLE_LOCEXPR_OFF_SECTION_END);
            return DW_DLV_ERROR;
        }
        operand1 = *(Dwarf_Small *) loc_ptr;
        loc_ptr = loc_ptr + 1;
        if (loc_ptr > section_end) {
            _dwarf_error(dbg,error,DW_DLE_LOCEXPR_OFF_SECTION_END);
            return DW_DLV_ERROR;
        }
        offset = offset + 1;
        break;

    case DW_OP_abs:
    case DW_OP_and:
    case DW_OP_div:
    case DW_OP_minus:
    case DW_OP_mod:
    case DW_OP_mul:
    case DW_OP_neg:
    case DW_OP_not:
    case DW_OP_or:
    case DW_OP_plus:
        break;

    case DW_OP_plus_uconst:
        DECODE_LEB128_UWORD_LEN_CK(loc_ptr, operand1,leb128_length,
            dbg,error,section_end);
        offset = offset + leb128_length;
        break;

    case DW_OP_shl:
    case DW_OP_shr:
    case DW_OP_shra:
    case DW_OP_xor:
        break;

    case DW_OP_le:
    case DW_OP_ge:
    case DW_OP_eq:
    case DW_OP_lt:
    case DW_OP_gt:
    case DW_OP_ne:
        break;

    case DW_OP_skip:
    case DW_OP_bra:
        READ_UNALIGNED_CK(dbg, operand1, Dwarf_Unsigned, loc_ptr, 2,
            error,section_end);
        SIGN_EXTEND(operand1,2);
        loc_ptr = loc_ptr + 2;
        offset = offset + 2;
        break;

    case DW_OP_piece:
        DECODE_LEB128_UWORD_LEN_CK(loc_ptr, operand1,leb128_length,
            dbg,error,section_end);
        offset = offset + leb128_length;
        break;
    case DW_OP_nop:
        break;
    case DW_OP_push_object_address: /* DWARF3 */
        break;
    case DW_OP_call2:       /* DWARF3 */
        READ_UNALIGNED_CK(dbg, operand1, Dwarf_Unsigned, loc_ptr, 2,
            error,section_end);
        loc_ptr = loc_ptr + 2;
        offset = offset + 2;
        break;

    case DW_OP_call4:       /* DWARF3 */
        READ_UNALIGNED_CK(dbg, operand1, Dwarf_Unsigned, loc_ptr, 4,
            error,section_end);
        loc_ptr = loc_ptr + 4;
        offset = offset + 4;
        break;
    case DW_OP_call_ref:    /* DWARF3 */
        READ_UNALIGNED_CK(dbg, operand1, Dwarf_Unsigned, loc_ptr,
            offset_size,
            error,section_end);
        loc_ptr = loc_ptr + offset_size;
        offset = offset + offset_size;
        break;

    case DW_OP_form_tls_address:    /* DWARF3f */
        break;
    case DW_OP_call_frame_cfa:      /* DWARF3f */
        break;
    case DW_OP_bit_piece:   /* DWARF3f */
        /* uleb size in bits followed by uleb offset in bits */
        DECODE_LEB128_UWORD_LEN_CK(loc_ptr, operand1,leb128_length,
            dbg,error,section_end);
        offset = offset + leb128_length;

        DECODE_LEB128_UWORD_LEN_CK(loc_ptr, operand2,leb128_length,
            dbg,error,section_end);
        offset = offset + leb128_length;
        break;

        /*  The operator means: push the currently computed
            (by the operations encountered so far in this
            expression) onto the expression stack as the offset
            in thread-local-storage of the variable. */
    case DW_OP_GNU_push_tls_address: /* 0xe0  */
        /* Believed to have no operands. */
        /* Unimplemented in gdb 7.5.1 ? */
        break;
    case DW_OP_deref_type:     /* DWARF5 */
    case DW_OP_GNU_deref_type: /* 0xf6 */
        if (loc_ptr >= section_end) {
            _dwarf_error(dbg,error,DW_DLE_LOCEXPR_OFF_SECTION_END);
            return DW_DLV_ERROR;
        }
        operand1 = *(Dwarf_Small *) loc_ptr;
        loc_ptr = loc_ptr + 1;
        if (loc_ptr > section_end) {
            _dwarf_error(dbg,error,DW_DLE_LOCEXPR_OFF_SECTION_END);
            return DW_DLV_ERROR;
        }
        offset = offset + 1;

        /* die offset (uleb128). */
        DECODE_LEB128_UWORD_LEN_CK(loc_ptr, operand2,leb128_length,
            dbg,error,section_end);
        offset = offset + leb128_length;
        break;

    case DW_OP_implicit_value: /* DWARF4 0xa0 */
        /*  uleb length of value bytes followed by that
            number of bytes of the value. */
        DECODE_LEB128_UWORD_LEN_CK(loc_ptr, operand1,leb128_length,
            dbg,error,section_end);
        offset = offset + leb128_length;

        /*  Second operand is block of 'operand1' bytes of stuff. */
        /*  This using the second operand as a pointer
            is quite ugly. */
        /*  This gets an ugly compiler warning. Sorry. */
        operand2 = (Dwarf_Unsigned)(uintptr_t)loc_ptr;
        offset = offset + operand1;
        loc_ptr = loc_ptr + operand1;
        if (loc_ptr > section_end) {
            _dwarf_error(dbg,error,DW_DLE_LOCEXPR_OFF_SECTION_END);
            return DW_DLV_ERROR;
        }
        break;
    case DW_OP_stack_value:  /* DWARF4 */
        break;
    case DW_OP_GNU_uninit:            /* 0xf0 */
        /*  Unimplemented in gdb 7.5.1  */
        /*  Carolyn Tice: Follows a DW_OP_reg or DW_OP_regx
            and marks the reg as being uninitialized. */
        break;
    case DW_OP_GNU_encoded_addr: {      /*  0xf1 */
        /*  Richard Henderson: The operand is an absolute
            address.  The first byte of the value
            is an encoding length: 0 2 4 or 8.  If zero
            it means the following is address-size.
            The address then follows immediately for
            that number of bytes. */
            int length = 0;
            int reares = 0;

            if (loc_ptr >= section_end) {
                _dwarf_error_string(dbg,error,
                    DW_DLE_LOCEXPR_OFF_SECTION_END,
                    "DW_DLE_LOCEXPR_OFF_SECTION_END "
                    "at DW_OP_GNU_encoded_addr. "
                    "Corrupt DWARF");
                return DW_DLV_ERROR;
            }
            reares = read_encoded_addr(loc_ptr,dbg,
                section_end,
                address_size,
                &operand1, &length,error);
            if (reares != DW_DLV_OK) {
                return reares;
            }
            loc_ptr += length;
            if (loc_ptr > section_end) {
                _dwarf_error(dbg,error,
                    DW_DLE_LOCEXPR_OFF_SECTION_END);
                return DW_DLV_ERROR;
            }
            offset  += length;
        }
        break;
    case DW_OP_implicit_pointer:       /* DWARF5 */
    case DW_OP_GNU_implicit_pointer:{  /* 0xf2 */
        /*  Jakub Jelinek: The value is an optimized-out
            pointer value. Represented as
            an offset_size DIE offset
            (a simple unsigned integer) in DWARF3,4
            followed by a signed leb128 offset.
            For DWARF2, it is actually pointer size
            (address size).
            The offset is global a section offset, not cu-relative.
            Relocation to a different object file is up to
            the user, per DWARF5 Page 41.
            http://www.dwarfstd.org/ShowIssue.php?issue=100831.1 */
        Dwarf_Half iplen = offset_size;
        if (version_stamp == DW_CU_VERSION2 /* 2 */ ) {
            iplen = address_size;
        }
        READ_UNALIGNED_CK(dbg, operand1, Dwarf_Unsigned, loc_ptr,
            iplen,error,section_end);
        loc_ptr = loc_ptr + iplen;
        if (loc_ptr > section_end) {
            _dwarf_error(dbg,error,DW_DLE_LOCEXPR_OFF_SECTION_END);
            return DW_DLV_ERROR;
        }
        offset = offset + iplen;

        DECODE_LEB128_SWORD_LEN_CK(loc_ptr, operand2,leb128_length,
            dbg,error,section_end);
        offset = offset + leb128_length;
        }

        break;
    case DW_OP_entry_value:       /* DWARF5 */
    case DW_OP_GNU_entry_value:       /* 0xf3 */
        /*  Jakub Jelinek: A register reused really soon,
            but the value is unchanged.  So to represent
            that value we have a uleb128 size followed
            by a DWARF expression block that size.
            http://www.dwarfstd.org/ShowIssue.php?issue=100909.1 */

        /*  uleb length of value bytes followed by that
            number of bytes of the value. */
        DECODE_LEB128_UWORD_LEN_CK(loc_ptr, operand1,leb128_length,
            dbg,error,section_end);
        offset = offset + leb128_length;

        /*  Second operand is block of 'operand1' bytes of stuff. */
        /*  This using the second operand as a pointer
            is quite ugly. */
        /*  This gets an ugly compiler warning. Sorry. */
        operand2 = (Dwarf_Unsigned)(uintptr_t)loc_ptr;
        offset = offset + operand1;
        loc_ptr = loc_ptr + operand1;
        if (loc_ptr > section_end) {
            _dwarf_error(dbg,error,DW_DLE_LOCEXPR_OFF_SECTION_END);
            return DW_DLV_ERROR;
        }
        break;
    case DW_OP_const_type:           /* DWARF5 */
    case DW_OP_GNU_const_type:       /* 0xf4 */
        {
        /* die offset as uleb. cu-relative */
        DECODE_LEB128_UWORD_LEN_CK(loc_ptr, operand1,leb128_length,
            dbg,error,section_end);
        offset = offset + leb128_length;

        if (loc_ptr >= section_end) {
            _dwarf_error_string(dbg,error,
                DW_DLE_LOCEXPR_OFF_SECTION_END,
                "DW_DLE_LOCEXPR_OFF_SECTION_END: Error reading "
                "DW_OP_const_type/DW_OP_GNU_const_type content");
            return DW_DLV_ERROR;
        }
        /*  Next byte is size of following data block.  */
        operand2 = *loc_ptr;
        loc_ptr = loc_ptr + 1;
        offset = offset + 1;

        /*  Operand 3 points to a value in the block of size
            just gotten as operand2.
            It must fit in a Dwarf_Unsigned.
            Get the type from the die at operand1
            (a CU relative offset). */
        /*  FIXME: We should do something very different than
            what we do here! */
        operand3 = (Dwarf_Unsigned)(uintptr_t)loc_ptr;
        loc_ptr = loc_ptr + operand2;
        if (loc_ptr > section_end) {
            _dwarf_error(dbg,error,DW_DLE_LOCEXPR_OFF_SECTION_END);
            return DW_DLV_ERROR;
        }
        offset = offset + operand2;
        }
        break;

    case DW_OP_regval_type:           /* DWARF5 */
    case DW_OP_GNU_regval_type:       /* 0xf5 */
        /* reg num uleb*/
        DECODE_LEB128_UWORD_LEN_CK(loc_ptr, operand1,leb128_length,
            dbg,error,section_end);
        offset = offset + leb128_length;
        /* cu die off uleb*/
        DECODE_LEB128_UWORD_LEN_CK(loc_ptr, operand2,leb128_length,
            dbg,error,section_end);
        offset = offset + leb128_length;
        break;
    case DW_OP_convert:           /* DWARF5 */
    case DW_OP_GNU_convert:       /* 0xf7 */
    case DW_OP_reinterpret:       /* DWARF5 */
    case DW_OP_GNU_reinterpret:       /* 0xf9 */
        /* die offset  or zero */
        DECODE_LEB128_UWORD_LEN_CK(loc_ptr, operand1,leb128_length,
            dbg,error,section_end);
        offset = offset + leb128_length;
        break;
    case DW_OP_GNU_parameter_ref :       /* 0xfa */
        /* 4 byte unsigned int */
        READ_UNALIGNED_CK(dbg, operand1, Dwarf_Unsigned, loc_ptr, 4,
            error,section_end);;
        loc_ptr = loc_ptr + 4;
        if (loc_ptr > section_end) {
            _dwarf_error_string(dbg,error,
                DW_DLE_LOCEXPR_OFF_SECTION_END,
                "DW_DLE_LOCEXPR_OFF_SECTION_END: Error reading "
                "DW_OP_GNU_parameter_ref.");
            return DW_DLV_ERROR;
        }
        offset = offset + 4;
        break;
    case DW_OP_addrx :           /* DWARF5 */
    case DW_OP_GNU_addr_index :  /* 0xfb DebugFission */
        /*  Index into .debug_addr. The value in .debug_addr
            is an address. */
        DECODE_LEB128_UWORD_LEN_CK(loc_ptr, operand1,leb128_length,
            dbg,error,section_end);
        offset = offset + leb128_length;
        break;
    case DW_OP_constx :          /* DWARF5 */
    case DW_OP_GNU_const_index : /* 0xfc DebugFission */
        /*  Index into .debug_addr. The value in .debug_addr
            is a constant that fits in an address. */
        DECODE_LEB128_UWORD_LEN_CK(loc_ptr, operand1,leb128_length,
            dbg,error,section_end);
        offset = offset + leb128_length;
        break;
    case DW_OP_GNU_variable_value: { /* 0xfd, 2017 By J Jelinek */
        /*  https://gcc.gnu.org/legacy-ml/gcc-patches/2017-02/
            msg01499.html */
        READ_UNALIGNED_CK(dbg, operand1, Dwarf_Unsigned, loc_ptr,
            offset_size,error,section_end);
        loc_ptr = loc_ptr + offset_size;
        if (loc_ptr > section_end) {
            _dwarf_error_string(dbg,error,
                DW_DLE_LOCEXPR_OFF_SECTION_END,
                "DW_DLE_LOCEXPR_OFF_SECTION_END: Error reading "
                "DW_OP_GNU_variable_value.");
            return DW_DLV_ERROR;
        }
        break;
    }
    /*  See https://www.llvm.org/docs/
        AMDGPUDwarfExtensionsForHeterogeneousDebugging.html */
    case DW_OP_LLVM_form_aspace_address:
    case DW_OP_LLVM_push_lane:
    case DW_OP_LLVM_offset:
    case DW_OP_LLVM_bit_offset:
    case DW_OP_LLVM_undefined:
    case DW_OP_LLVM_piece_end:
        /* no operands on these */
        break;
    case DW_OP_LLVM_offset_uconst: /*uleb operand*/
    case DW_OP_LLVM_call_frame_entry_reg: /*uleb operand*/
        DECODE_LEB128_UWORD_LEN_CK(loc_ptr, operand1,leb128_length,
            dbg,error,section_end);
        offset = offset + leb128_length;
        break;

    case DW_OP_LLVM_aspace_implicit_pointer:
        READ_UNALIGNED_CK(dbg, operand1, Dwarf_Unsigned, loc_ptr,
            offset_size,error,section_end);
        loc_ptr = loc_ptr + offset_size;
        if (loc_ptr > section_end) {
            _dwarf_error_string(dbg,error,
                DW_DLE_LOCEXPR_OFF_SECTION_END,
                "DW_DLE_LOCEXPR_OFF_SECTION_END: Error reading "
                "DW_OP_LLVM_aspace_implicit_pointer.");
            return DW_DLV_ERROR;
        }

        DECODE_LEB128_SWORD_LEN_CK(loc_ptr, operand2,leb128_length,
            dbg,error,section_end);
        offset = offset + leb128_length;
        break;

    case DW_OP_LLVM_aspace_bregx:
    case DW_OP_LLVM_extend:
    case DW_OP_LLVM_select_bit_piece:
        DECODE_LEB128_UWORD_LEN_CK(loc_ptr, operand1,leb128_length,
            dbg,error,section_end);
        offset = offset + leb128_length;

        DECODE_LEB128_UWORD_LEN_CK(loc_ptr, operand2,leb128_length,
            dbg,error,section_end);
        offset = offset + leb128_length;
        break;

    default: {
        dwarfstring m;
        const char *atomname = 0;

        /*  This can happen if the offset_size or address_size
            in the OP stream was incorrect for the object file.*/
        dwarfstring_constructor(&m);
        dwarfstring_append_printf_u(&m,
            "ERROR: DW_DLE_LOC_EXPR_BAD as DW_OP atom "
            "0x%x ",atom);
        dwarfstring_append(&m, "(");
        dwarf_get_OP_name(atom,&atomname);
        dwarfstring_append(&m,(char *)(atomname?
            atomname:"<no name>"));
        dwarfstring_append(&m, ")");
        dwarfstring_append(&m,"is unknown.");
        _dwarf_error_string(dbg, error, DW_DLE_LOC_EXPR_BAD,
            dwarfstring_string(&m));
        dwarfstring_destructor(&m);
        return DW_DLV_ERROR;
    }
    }
    if (loc_ptr > section_end) {
        _dwarf_error(dbg,error,DW_DLE_LOCEXPR_OFF_SECTION_END);
        return DW_DLV_ERROR;
    }
    if (startoffset_in < 0 ||
        offset <= (Dwarf_Unsigned)startoffset_in) {
        dwarfstring m;

        dwarfstring_constructor (&m);
        dwarfstring_append_printf_u(&m,
            "DW_DLE_LOCEXPR_OFF_SECTION_END: "
            "A  DW_OP (0x%02x) has a value that results in "
            "decreasing the section offset from ",atom);
        dwarfstring_append_printf_u(&m," %ld to ",startoffset_in);
        dwarfstring_append_printf_u(&m," %ld ",startoffset_in);
        dwarfstring_append_printf_u(&m," (0x%lx) ",
            (Dwarf_Unsigned)startoffset_in);
        dwarfstring_append_printf_i(&m," to 0x%lx. Corrupt Dwarf ",
            offset);
        _dwarf_error_string(dbg,error,DW_DLE_LOCEXPR_OFF_SECTION_END,
            dwarfstring_string(&m));
        dwarfstring_destructor (&m);
        return DW_DLV_ERROR;
    }
    /* If offset == loc_len this would be normal end-of-expression. */
    if (offset > loc_len) {
        /*  We stepped past the end of the expression.
            This has to be a compiler bug.
            Operators missing their values cannot be detected
            as such except at the end of an expression (like this).
            The results would be wrong if returned.
        */
        _dwarf_error(dbg, error, DW_DLE_LOC_BAD_TERMINATION);
        return DW_DLV_ERROR;
    }
    curr_loc->lr_atom = atom;
    curr_loc->lr_number =  operand1;
    curr_loc->lr_number2 = operand2;
    /*  lr_number 3 is a pointer to a value iff DW_OP_const or
        DW_OP_GNU_const_type */
    curr_loc->lr_number3 = operand3;
    *nextoffset_out = offset;
    return DW_DLV_OK;
}
