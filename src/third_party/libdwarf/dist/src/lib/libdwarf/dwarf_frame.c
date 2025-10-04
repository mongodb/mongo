/*
  Copyright (C) 2000-2006 Silicon Graphics, Inc.  All Rights Reserved.
  Portions Copyright (C) 2007-2022 David Anderson. All Rights Reserved.
  Portions Copyright 2012 SN Systems Ltd. All rights reserved.

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

#include <stdlib.h> /* calloc() free() */
#include <string.h> /* memset() */
#include <stdio.h> /* memset() */
#include <limits.h> /* MAX/MIN() */

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
#include "dwarf_frame.h"
#include "dwarf_arange.h" /* Using Arange as a way to build a list */
#include "dwarf_string.h"
#include "dwarf_safe_arithmetic.h"

/*  Dwarf_Unsigned is always 64 bits */
#define INVALIDUNSIGNED(x)  ((x) & (((Dwarf_Unsigned)1) << 63))

/*  Simply assumes error is a Dwarf_Error * in its context */
#define FDE_NULL_CHECKS_AND_SET_DBG(fde,dbg )          \
    do {                                               \
        if ((fde) == NULL) {                           \
            _dwarf_error(NULL, error, DW_DLE_FDE_NULL);\
        return DW_DLV_ERROR;                           \
    }                                                  \
    (dbg)= (fde)->fd_dbg;                              \
    if (IS_INVALID_DBG((dbg))) {                         \
        _dwarf_error_string(NULL, error, DW_DLE_FDE_DBG_NULL,\
            "DW_DLE_FDE_DBG_NULL: An fde contains a stale "\
            "Dwarf_Debug ");                           \
        return DW_DLV_ERROR;                           \
    }                                                  \
    } while (0)

#define MIN(a,b)  (((a) < (b))? (a):(b))

#if 0 /* dump_bytes FOR DEBUGGING */
static void
dump_bytes(const char *msg,Dwarf_Small * start, long len)
{
    Dwarf_Small *end = start + len;
    Dwarf_Small *cur = start;
    printf("%s (0x%lx) ",msg,(unsigned long)start);
    for (; cur < end; cur++) {
        printf("%02x", *cur);
    }
    printf("\n");
}
/* Only used for debugging libdwarf. */
static void dump_frame_rule(char *msg,
    struct Dwarf_Reg_Rule_s *reg_rule);
#endif /*0*/

static int _dwarf_initialize_fde_table(Dwarf_Debug dbg,
    struct Dwarf_Frame_s *fde_table,
    Dwarf_Unsigned table_real_data_size,
    Dwarf_Error * error);
static void _dwarf_free_fde_table(struct Dwarf_Frame_s *fde_table);
static void _dwarf_init_reg_rules_ru(
    struct Dwarf_Reg_Rule_s *base,
    Dwarf_Unsigned first, Dwarf_Unsigned last,
    Dwarf_Unsigned initial_value);
static void _dwarf_init_reg_rules_dw3(
    Dwarf_Regtable_Entry3_i *base,
    Dwarf_Unsigned, Dwarf_Unsigned last,
    Dwarf_Unsigned  initial_value);

/*  The rules for register settings are described
    in libdwarf.pdf and the html version.
    (see Special Frame Registers).
*/
static int
regerror(Dwarf_Debug dbg,Dwarf_Error *error,
    int enumber,
    const char *msg)
{
    _dwarf_error_string(dbg,error,enumber,(char *)msg);
    return DW_DLV_ERROR;
}

int
_dwarf_validate_register_numbers(
    Dwarf_Debug dbg,
    Dwarf_Error *error)
{
    if (dbg->de_frame_same_value_number ==
        dbg->de_frame_undefined_value_number) {
        return regerror(dbg,error,DW_DLE_DEBUGFRAME_ERROR,
            "DW_DLE_DEBUGFRAME_ERROR "
            "same_value == undefined_value");
    }
    if (dbg->de_frame_cfa_col_number ==
        dbg->de_frame_same_value_number) {
        return regerror(dbg,error,DW_DLE_DEBUGFRAME_ERROR,
            "DW_DLE_DEBUGFRAME_ERROR "
            "same_value == cfa_column_number ");
    }
    if (dbg->de_frame_cfa_col_number ==
        dbg->de_frame_undefined_value_number) {
        return regerror(dbg,error,DW_DLE_DEBUGFRAME_ERROR,
            "DW_DLE_DEBUGFRAME_ERROR "
            "undefined_value == cfa_column_number ");
    }
    if ((dbg->de_frame_rule_initial_value !=
        dbg->de_frame_same_value_number) &&
        (dbg->de_frame_rule_initial_value !=
        dbg->de_frame_undefined_value_number)) {
        return regerror(dbg,error,DW_DLE_DEBUGFRAME_ERROR,
            "DW_DLE_DEBUGFRAME_ERROR "
            "initial_value not set to "
            " same_value or undefined_value");
    }
    if (dbg->de_frame_undefined_value_number <=
        dbg->de_frame_reg_rules_entry_count) {
        return regerror(dbg,error,DW_DLE_DEBUGFRAME_ERROR,
            "DW_DLE_DEBUGFRAME_ERROR "
            "undefined_value less than number of registers");
    }
    if (dbg->de_frame_same_value_number <=
        dbg->de_frame_reg_rules_entry_count) {
        return regerror(dbg,error,DW_DLE_DEBUGFRAME_ERROR,
            "DW_DLE_DEBUGFRAME_ERROR "
            "same_value  <= number of registers");
    }
    if (dbg->de_frame_cfa_col_number <=
        dbg->de_frame_reg_rules_entry_count) {
        return regerror(dbg,error,DW_DLE_DEBUGFRAME_ERROR,
            "DW_DLE_DEBUGFRAME_ERROR "
            "cfa_column <= number of registers");
    }
    return DW_DLV_OK;
}

int
dwarf_get_frame_section_name(Dwarf_Debug dbg,
    const char **sec_name,
    Dwarf_Error *error)
{
    struct Dwarf_Section_s *sec = 0;

    CHECK_DBG(dbg,error,"dwarf_get_frame_section_name()");
    if (error != NULL) {
        *error = NULL;
    }
    sec = &dbg->de_debug_frame;
    if (sec->dss_size == 0) {
        /* We don't have such a  section at all. */
        return DW_DLV_NO_ENTRY;
    }
    *sec_name = sec->dss_name;
    return DW_DLV_OK;
}

int
dwarf_get_frame_section_name_eh_gnu(Dwarf_Debug dbg,
    const char **sec_name,
    Dwarf_Error *error)
{
    struct Dwarf_Section_s *sec = 0;

    CHECK_DBG(dbg,error,"dwarf_get_frame_section_name_eh_gnu()");
    if (error != NULL) {
        *error = NULL;
    }
    sec = &dbg->de_debug_frame_eh_gnu;
    if (sec->dss_size == 0) {
        /* We don't have such a  section at all. */
        return DW_DLV_NO_ENTRY;
    }
    *sec_name = sec->dss_name;
    return DW_DLV_OK;
}

/*
    This function is the heart of the debug_frame stuff.  Don't even
    think of reading this without reading both the Libdwarf and
    consumer API carefully first.  This function executes
    frame instructions contained in a Cie or an Fde, but does in a
    number of different ways depending on the information sought.
    Start_instr_ptr points to the first byte of the frame instruction
    stream, and final_instr_ptr one past the last byte.

    The offsets returned in the frame instructions are factored.  That
    is they need to be multiplied by either the code_alignment_factor
    or the data_alignment_factor, as appropriate to obtain the actual
    offset.  This makes it possible to expand an instruction stream
    without the corresponding Cie.  However, when an Fde frame instr
    sequence is being expanded there must be a valid Cie
    with a pointer to an initial table row.

    If successful, returns DW_DLV_OK
        And sets returned_count thru the pointer
        if make_instr is TRUE.
        If make_instr is FALSE returned_count
        should NOT be used by the caller (returned_count
        is set to 0 thru the pointer by this routine...)
    If unsuccessful, returns DW_DLV_ERROR
        and sets returned_error to the error code

    It does not do a whole lot of input validation being a private
    function.  Please make sure inputs are valid.

    (1) If make_instr is TRUE, it makes a list of pointers to
    Dwarf_Frame_Op structures containing the frame instructions
    executed.  A pointer to this list is returned in ret_frame_instr.
    Make_instr is TRUE only when a list of frame instructions is to be
    returned.  In this case since we are not interested
    in the contents
    of the table, the input Cie can be NULL.  This is the only case
    where the input Cie can be NULL.

    (2) If search_pc is TRUE, frame instructions are executed till
    either a location is reached that is greater than the
    search_pc_val
    provided, or all instructions are executed.  At this point the
    last row of the table generated is returned in a structure.
    A pointer to this structure is supplied in table.

    (3) This function is also used to create the initial table row
    defined by a Cie.  In this case, the Dwarf_Cie pointer cie, is
    NULL.  For an FDE, however, cie points to the associated Cie.

    (4) If search_pc is TRUE and (has_more_rows and subsequent_pc
        are non-null) then:
            has_more_rows is set TRUE if there are instruction
            bytes following the detection of search_over.
            If all the instruction bytes have been seen
            then *has_more_rows is set FALSE.

            If *has_more_rows is TRUE then *subsequent_pc
            is set to the pc value that is the following
            row in the table.

    make_instr - make list of frame instr? 0/1
    ret_frame_instr -  Ptr to list of ptrs to frame instrs
    search_pc  - Search for a pc value?  0/1
    search_pc_val -  Search for this pc value
    initial_loc - Initial code location value.
    start_instr_ptr -   Ptr to start of frame instrs.
    final_instr_ptr -   Ptr just past frame instrs.
    table       -     Ptr to struct with last row.
    cie     -   Ptr to Cie used by the Fde.

    Different cies may have distinct address-sizes, so the cie
    is used, not de_pointer_size.

*/

/*  Cleans up the in-process linked list of these
    in case of early exit in
    _dwarf_exec_frame_instr.  */
static void
_dwarf_free_dfi_list(Dwarf_Frame_Instr fr)
{
    Dwarf_Frame_Instr cur = fr;
    Dwarf_Frame_Instr next = 0;
    for ( ; cur ; cur = next) {
        next = cur->fi_next;
        free(cur);
    }
}
#if 0 /* printlist() for debugging */
static void
printlist(Dwarf_Frame_Instr x)
{
    int i = 0;
    Dwarf_Frame_Instr nxt = 0;

    printf("=========== print cur list of ptrs\n");
    for ( ; x ; x = nxt,++i) {
        printf("%d  inst 0x%lx nxt 0x%lx\n",
            i,(unsigned long)x,
            (unsigned long)x->fi_next);
        nxt = x->fi_next;
    }
    printf("=========== done cur list of ptrs\n");
}
#endif /*0*/

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
    Dwarf_Error *error)
{
/*  The following macro depends on macreg and
    machigh_reg both being unsigned to avoid
    unintended behavior and to avoid compiler warnings when
    high warning levels are turned on.  To avoid
    truncation turning a bogus large value into a smaller
    sensible-seeming value we use Dwarf_Unsigned for register
    numbers. */
#define ERROR_IF_REG_NUM_TOO_HIGH(macreg,machigh_reg)        \
    do {                                                     \
        if ((macreg) >= (machigh_reg)) {                     \
            SER(DW_DLE_DF_REG_NUM_TOO_HIGH); \
        }                                                    \
    } /*CONSTCOND */ while (0)
#define FREELOCALMALLOC                  \
        _dwarf_free_dfi_list(ilisthead); \
        ilisthead =0;                    \
        free(dfi); dfi = 0;              \
        free(localregtab); localregtab = 0;
/* SER === SIMPLE_ERROR_RETURN */
#define SER(code)                     \
        FREELOCALMALLOC;              \
        _dwarf_error(dbg,error,(code)); \
        return DW_DLV_ERROR
#define SERSTRING(code,m)             \
        FREELOCALMALLOC;              \
        _dwarf_error_string(dbg,error,(code),m); \
        return DW_DLV_ERROR
/*  m must be a quoted string */
#define SERINST(m)                    \
        FREELOCALMALLOC;              \
        _dwarf_error_string(dbg,error,DW_DLE_ALLOC_FAIL, \
            "DW_DLE_ALLOC_FAIL: " m); \
        return DW_DLV_ERROR

    /*  Sweeps the frame instructions. */
    Dwarf_Small *instr_ptr = 0;
    Dwarf_Frame_Instr dfi = 0;

    /*  Register numbers not limited to just 255,
        thus not using Dwarf_Small.  */
    typedef Dwarf_Unsigned reg_num_type;

    Dwarf_Unsigned factored_N_value = 0;
    Dwarf_Signed signed_factored_N_value = 0;
    Dwarf_Addr current_loc = initial_loc;       /* code location/
        pc-value corresponding to the frame instructions.
        Starts at zero when the caller has no value to pass in. */

    /*  Must be min de_pointer_size bytes and must be at least 4 */
    Dwarf_Unsigned adv_loc = 0;

    Dwarf_Unsigned reg_count = dbg->de_frame_reg_rules_entry_count;
    struct Dwarf_Reg_Rule_s *localregtab = calloc(reg_count,
        sizeof(struct Dwarf_Reg_Rule_s));

    struct Dwarf_Reg_Rule_s cfa_reg;

    /*  This is used to end executing frame instructions.  */
    /*  Becomes TRUE when search_pc is TRUE and current_loc */
    /*  is greater than search_pc_val.  */
    Dwarf_Bool search_over = FALSE;

    Dwarf_Addr possible_subsequent_pc = 0;

    Dwarf_Half address_size = (cie)? cie->ci_address_size:
        dbg->de_pointer_size;

    /*  Stack_table points to the row (Dwarf_Frame ie) being
        pushed or popped by a remember or restore instruction.
        Top_stack points to
        the top of the stack of rows. */
    Dwarf_Frame stack_table = NULL;
    Dwarf_Frame top_stack = NULL;

    /*  These are used only when make_instr is TRUE. Curr_instr is a
        pointer to the current frame instruction executed.
        Curr_instr_ptr, head_instr_list, and curr_instr_list are
        used to form a chain of Dwarf_Frame_Op structs.
        Dealloc_instr_ptr is
        used to deallocate the structs used to form the chain.
        Head_instr_block points to a contiguous list of
        pointers to the
        Dwarf_Frame_Op structs executed. */
    /*  Build single linked list of instrs, and
        at end turn into array. */
    Dwarf_Frame_Instr ilisthead = 0;
    Dwarf_Frame_Instr *ilistlastptr = &ilisthead;
    /*  Counts the number of frame instructions
        in the returned instrs if instruction
        details are asked for. Else 0. */
    Dwarf_Unsigned instr_count = 0;

    /*  These are the alignment_factors taken from the Cie provided.
        When no input Cie is provided they are set to 1, because only
        factored offsets are required. */
    Dwarf_Signed code_alignment_factor = 1;
    Dwarf_Signed data_alignment_factor = 1;

    /*  This flag indicates when an actual alignment factor
        is needed.
        So if a frame instruction that computes an offset
        using an alignment factor is encountered when this
        flag is set, an error is returned because the Cie
        did not have a valid augmentation. */
    Dwarf_Bool need_augmentation = FALSE;
    Dwarf_Unsigned instr_area_length = 0;

    Dwarf_Unsigned i = 0;

    /*  Initialize first row from associated Cie.
        Using temp regs explicitly */

    if (!localregtab) {
        SER(DW_DLE_ALLOC_FAIL);
    }
    {
        struct Dwarf_Reg_Rule_s *t1reg = localregtab;
        if (cie != NULL && cie->ci_initial_table != NULL) {
            unsigned minregcount = 0;
            unsigned curreg = 0;
            struct Dwarf_Reg_Rule_s *t2reg =
                cie->ci_initial_table->fr_reg;

            if (reg_count != cie->ci_initial_table->fr_reg_count) {
                /*  Should never happen,
                    it makes no sense to have the
                    table sizes change. There
                    is no real allowance for
                    the set of registers
                    to change dynamically
                    in a single Dwarf_Debug
                    (except the size can be set
                    near initial Dwarf_Debug
                    creation time). */
                SER(DW_DLE_FRAME_REGISTER_COUNT_MISMATCH);
            }
            minregcount =
                (unsigned)MIN(reg_count,
                cie->ci_initial_table->fr_reg_count);
            for ( ; curreg < minregcount ;
                curreg++, t1reg++, t2reg++) {
                *t1reg = *t2reg;
            } cfa_reg =
            cie->ci_initial_table->fr_cfa_rule;
        } else {
            _dwarf_init_reg_rules_ru(localregtab,0,reg_count,
                dbg->de_frame_rule_initial_value);
            _dwarf_init_reg_rules_ru(&cfa_reg,0, 1,
                dbg->de_frame_rule_initial_value);
        }
    }
    /*  The idea here is that the code_alignment_factor and
        data_alignment_factor which are needed for certain
        instructions are valid only when the Cie has a proper
        augmentation string. So if the augmentation is not
        right, only Frame instruction can be read. */
    if (cie != NULL && cie->ci_augmentation != NULL) {
        code_alignment_factor = cie->ci_code_alignment_factor;
        data_alignment_factor = cie->ci_data_alignment_factor;
    } else {
        need_augmentation = !make_instr;
    }
    instr_ptr = start_instr_ptr;
    instr_area_length = (uintptr_t)
        (final_instr_ptr - start_instr_ptr);
    while ((instr_ptr < final_instr_ptr) && (!search_over)) {
        Dwarf_Small   instr = 0;
        Dwarf_Small   opcode = 0;
        reg_num_type  reg_no = 0;
        Dwarf_Unsigned adv_pc = 0;
        Dwarf_Off fp_instr_offset = 0;
        Dwarf_Small * base_instr_ptr = 0;

        if (instr_ptr < start_instr_ptr) {
            SERINST("DW_DLE_DF_NEW_LOC_LESS_OLD_LOC: "
                "Following instruction bytes we find impossible "
                "decrease in a pointer");
        }
        fp_instr_offset = instr_ptr - start_instr_ptr;
        if (instr_ptr >= final_instr_ptr) {
            _dwarf_error(NULL, error, DW_DLE_DF_FRAME_DECODING_ERROR);
            return DW_DLV_ERROR;
        }
        instr = *(Dwarf_Small *) instr_ptr;
        instr_ptr += sizeof(Dwarf_Small);
        base_instr_ptr = instr_ptr;
        if ((instr & 0xc0) == 0x00) {
            opcode = instr;     /* is really extended op */
        } else {
            opcode = instr & 0xc0;      /* is base op */
        }
        if (make_instr) {
            dfi = calloc(1,sizeof(*dfi));
            if (!dfi) {
                SERINST("DW_CFA_advance_loc out of memory");
            }
            dfi->fi_op = opcode;
            dfi->fi_instr_offset = fp_instr_offset;
            dfi->fi_fields = "";
        }
        switch (opcode) {
        case DW_CFA_lo_user: {
            if (make_instr) {
                dfi->fi_fields = "";
            }
        }
        break;
        case DW_CFA_advance_loc: {
            Dwarf_Unsigned adv_pc_val = 0;
            int alres = 0;

            /* base op */
            adv_pc_val = instr &DW_FRAME_INSTR_OFFSET_MASK;
            if (need_augmentation) {
                SER(DW_DLE_DF_NO_CIE_AUGMENTATION);
            }

            /* CHECK OVERFLOW */
            alres = _dwarf_uint64_mult(adv_pc_val,
                code_alignment_factor,&adv_pc,dbg,error);
            if (alres == DW_DLV_ERROR) {
                FREELOCALMALLOC;
                return DW_DLV_ERROR;
            }
            if (INVALIDUNSIGNED(adv_pc)) {
                SERSTRING(DW_DLE_ARITHMETIC_OVERFLOW,
                    "DW_DLE_ARITHMETIC_OVERFLOW "
                    "negative new location");
            }

            possible_subsequent_pc = current_loc +
                (Dwarf_Unsigned)adv_pc;
            if (possible_subsequent_pc < current_loc &&
                possible_subsequent_pc < adv_pc) {
                SERSTRING(DW_DLE_ARITHMETIC_OVERFLOW,
                    "DW_DLE_ARITHMETIC_OVERFLOW "
                    "add overflowed");
            }

            search_over = search_pc &&
                (possible_subsequent_pc > search_pc_val);
            /* If gone past pc needed, retain old pc.  */
            if (!search_over) {
                current_loc = possible_subsequent_pc;
            }
            if (make_instr) {
                dfi->fi_fields = "uc";
                dfi->fi_u0 = adv_pc_val;
                dfi->fi_code_align_factor = code_alignment_factor;
            }
            }
            break;
        case DW_CFA_offset: {  /* base op */
            int adres = 0;
            Dwarf_Signed result = 0;
            reg_no = (reg_num_type) (instr &
                DW_FRAME_INSTR_OFFSET_MASK);
            ERROR_IF_REG_NUM_TOO_HIGH(reg_no, reg_count);
            adres = _dwarf_leb128_uword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &factored_N_value,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            if (need_augmentation) {
                SER( DW_DLE_DF_NO_CIE_AUGMENTATION);
            }
            if (INVALIDUNSIGNED(factored_N_value)) {
                SERSTRING(DW_DLE_ARITHMETIC_OVERFLOW,
                    "DW_DLE_ARITHMETIC_OVERFLOW "
                    "negative factored_N_value location");
            }
            /*  CHECK OVERFLOW */
            adres = _dwarf_int64_mult(
                (Dwarf_Signed)factored_N_value,
                data_alignment_factor,
                &result,dbg, error);
            if (adres == DW_DLV_ERROR) {
                FREELOCALMALLOC;
                return DW_DLV_ERROR;
            }
            localregtab[reg_no].ru_offset = result;
            localregtab[reg_no].ru_is_offset = 1;
            localregtab[reg_no].ru_register = reg_num_of_cfa;
            localregtab[reg_no].ru_value_type = DW_EXPR_OFFSET;
            if (make_instr) {
                dfi->fi_fields = "rud";
                dfi->fi_u0 = reg_no;
                dfi->fi_u1 = factored_N_value;
                dfi->fi_data_align_factor =
                    data_alignment_factor;
            }
            }
            break;
        case DW_CFA_restore: { /* base op */
            reg_no = (instr & DW_FRAME_INSTR_OFFSET_MASK);
            ERROR_IF_REG_NUM_TOO_HIGH(reg_no, reg_count);

            if (cie != NULL && cie->ci_initial_table != NULL) {
                localregtab[reg_no] =
                    cie->ci_initial_table->fr_reg[reg_no];
            } else if (!make_instr) {
                SER(DW_DLE_DF_MAKE_INSTR_NO_INIT);
            }
            if (make_instr) {
                dfi->fi_fields = "r";
                dfi->fi_u0 = reg_no;
            }
            }
            break;
        case DW_CFA_set_loc: {
            Dwarf_Addr new_loc = 0;
            int adres = 0;
            adres=_dwarf_read_unaligned_ck_wrapper(dbg,
                &new_loc,
                instr_ptr, address_size,
                final_instr_ptr,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            instr_ptr += address_size;
            if (new_loc != 0 && current_loc != 0) {
                /*  Pre-relocation or before current_loc
                    is set the test comparing new_loc
                    and current_loc makes no
                    sense. Testing for non-zero (above) is a way
                    (fallible) to check that current_loc, new_loc
                    are already relocated.  */
                if (new_loc <= current_loc) {
                    /*  Within a frame, address must increase.
                    Seemingly it has not.
                    Seems to be an error. */
                    SER(DW_DLE_DF_NEW_LOC_LESS_OLD_LOC);
                }
            }
            search_over = search_pc && (new_loc > search_pc_val);
            /* If gone past pc needed, retain old pc.  */
            possible_subsequent_pc =  new_loc;
            if (!search_over) {
                current_loc = possible_subsequent_pc;
            }
            if (make_instr) {
                dfi->fi_fields = "u";
                dfi->fi_u0 = new_loc;
            }
            }
            break;
        case DW_CFA_advance_loc1:
        {
            int adres = 0;
            Dwarf_Unsigned advloc_val = 0;
            adres=_dwarf_read_unaligned_ck_wrapper(dbg,
                &advloc_val,
                instr_ptr, sizeof(Dwarf_Small),
                final_instr_ptr,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            instr_ptr += sizeof(Dwarf_Small);
            if (need_augmentation) {
                SER(DW_DLE_DF_NO_CIE_AUGMENTATION);
            }
            /* CHECK OVERFLOW */
            adres = _dwarf_uint64_mult(
                advloc_val,
                code_alignment_factor,
                &adv_loc,dbg,error);
            if (adres == DW_DLV_ERROR) {
                FREELOCALMALLOC;
                return adres;
            }

            /* CHECK OVERFLOW add */
            possible_subsequent_pc =  current_loc + adv_loc;
            if (possible_subsequent_pc < current_loc &&
                possible_subsequent_pc < adv_loc) {
                SERSTRING(DW_DLE_ARITHMETIC_OVERFLOW,
                    "DW_DLE_ARITHMETIC_OVERFLOW "
                    "add overflowed calcating subsequent pc");
            }
            search_over = search_pc &&
            (possible_subsequent_pc > search_pc_val);

            /* If gone past pc needed, retain old pc.  */
            if (!search_over) {
                current_loc = possible_subsequent_pc;
            }
            if (make_instr) {
                dfi->fi_fields = "uc";
                dfi->fi_u0 = advloc_val;
                dfi->fi_code_align_factor =
                    code_alignment_factor;
            }
            break;
        }

        case DW_CFA_advance_loc2:
        {
            int adres = 0;
            Dwarf_Unsigned advloc_val = 0;
            adres=_dwarf_read_unaligned_ck_wrapper(dbg, &advloc_val,
                instr_ptr, DWARF_HALF_SIZE,
                final_instr_ptr,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            instr_ptr += DWARF_HALF_SIZE;
            if (need_augmentation) {
                SER(DW_DLE_DF_NO_CIE_AUGMENTATION);
            }
            /* CHECK OVERFLOW */
            adres = _dwarf_uint64_mult(
                advloc_val,
                code_alignment_factor,
                &adv_loc,dbg,error);
            if (adres == DW_DLV_ERROR) {
                FREELOCALMALLOC;
                return adres;
            }
            /* CHECK OVERFLOW add */
            if (INVALIDUNSIGNED(adv_loc)) {
                SERSTRING( DW_DLE_ARITHMETIC_OVERFLOW,
                    "DW_DLE_ARITHMETIC_OVERFLOW "
                    "negative new location");
            }

            /* CHECK OVERFLOW add */
            possible_subsequent_pc =  current_loc + adv_loc;
            if (possible_subsequent_pc < current_loc &&
                possible_subsequent_pc < adv_loc) {
                SERSTRING(DW_DLE_ARITHMETIC_OVERFLOW,
                    "DW_DLE_ARITHMETIC_OVERFLOW "
                    "add overflowed");
            }
            search_over = search_pc &&
            (possible_subsequent_pc > search_pc_val);
            /* If gone past pc needed, retain old pc.  */
            if (!search_over) {
                current_loc = possible_subsequent_pc;
            }
            if (make_instr) {
                dfi->fi_fields = "uc";
                dfi->fi_u0 = advloc_val;
                dfi->fi_code_align_factor =
                    code_alignment_factor;
            }
            break;
        }

        case DW_CFA_advance_loc4:
        {
            int adres = 0;
            Dwarf_Unsigned advloc_val = 0;

            adres=_dwarf_read_unaligned_ck_wrapper(dbg, &advloc_val,
                instr_ptr,  DWARF_32BIT_SIZE,
                final_instr_ptr,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            instr_ptr += DWARF_32BIT_SIZE;
            if (need_augmentation) {
                SER(DW_DLE_DF_NO_CIE_AUGMENTATION);
            }
            /* CHECK OVERFLOW */
            adres = _dwarf_uint64_mult(
                advloc_val,
                code_alignment_factor,
                &adv_loc,dbg,error);
            if (adres == DW_DLV_ERROR) {
                FREELOCALMALLOC;
                return adres;
            }
            /* CHECK OVERFLOW add */
            possible_subsequent_pc =  current_loc + adv_loc;
            if (possible_subsequent_pc < current_loc &&
                possible_subsequent_pc < adv_loc) {
                SERSTRING(DW_DLE_ARITHMETIC_OVERFLOW,
                    "DW_DLE_ARITHMETIC_OVERFLOW "
                    "unsigned add overflowed");
            }

            search_over = search_pc &&
                (possible_subsequent_pc > search_pc_val);
            /* If gone past pc needed, retain old pc.  */
            if (!search_over) {
                current_loc = possible_subsequent_pc;
            }
            if (make_instr) {
                dfi->fi_fields = "uc";
                dfi->fi_u0 = advloc_val;
                dfi->fi_code_align_factor =
                    code_alignment_factor;
            }
            break;
        }
        case DW_CFA_MIPS_advance_loc8:
        {
            int adres = 0;
            Dwarf_Unsigned advloc_val = 0;
            adres=_dwarf_read_unaligned_ck_wrapper(dbg, &advloc_val,
                instr_ptr,  DWARF_64BIT_SIZE,
                final_instr_ptr,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            instr_ptr += DWARF_64BIT_SIZE;
            if (need_augmentation) {
                SER(DW_DLE_DF_NO_CIE_AUGMENTATION);
            }
            /* CHECK OVERFLOW */
            adres = _dwarf_uint64_mult(advloc_val,
                code_alignment_factor,&adv_loc,
                dbg,error);
            if (adres == DW_DLV_ERROR) {
                FREELOCALMALLOC;
                return adres;
            }
            /* CHECK OVERFLOW add */
            possible_subsequent_pc =  current_loc + adv_loc;
            if (possible_subsequent_pc < current_loc &&
                possible_subsequent_pc < adv_loc) {
                SERSTRING(DW_DLE_ARITHMETIC_OVERFLOW,
                    "DW_DLE_ARITHMETIC_OVERFLOW "
                    "unsigned add overflowed");
            }
            search_over = search_pc &&
            (possible_subsequent_pc > search_pc_val);
            /* If gone past pc needed, retain old pc.  */
            if (!search_over) {
                current_loc = possible_subsequent_pc;
            }
            if (make_instr) {
                dfi->fi_fields = "u";
                dfi->fi_u0 = advloc_val;
                dfi->fi_code_align_factor =
                    code_alignment_factor;
            }
            break;
        }

        case DW_CFA_offset_extended:
        {
            Dwarf_Unsigned lreg = 0;
            Dwarf_Signed  result = 0;
            int adres = 0;
            adres = _dwarf_leb128_uword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &lreg,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            reg_no = (reg_num_type) lreg;
            ERROR_IF_REG_NUM_TOO_HIGH(reg_no, reg_count);
            adres = _dwarf_leb128_uword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &factored_N_value,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            if (need_augmentation) {
                SER(DW_DLE_DF_NO_CIE_AUGMENTATION);
            }
            if (INVALIDUNSIGNED(factored_N_value)) {
                SERSTRING(DW_DLE_ARITHMETIC_OVERFLOW,
                    "DW_DLE_ARITHMETIC_OVERFLOW "
                    "negative new location");
            }
            /*  CHECK OVERFLOW */
            adres = _dwarf_int64_mult((Dwarf_Signed)factored_N_value,
                data_alignment_factor, &result,
                dbg,error);
            if (adres == DW_DLV_ERROR) {
                FREELOCALMALLOC;
                return adres;
            }
            localregtab[reg_no].ru_is_offset = 1;
            localregtab[reg_no].ru_value_type = DW_EXPR_OFFSET;
            localregtab[reg_no].ru_register = reg_num_of_cfa;
            localregtab[reg_no].ru_offset = result;
            if (make_instr) {
                dfi->fi_fields = "rud";
                dfi->fi_u0 = lreg;
                dfi->fi_u1 = factored_N_value;
                dfi->fi_data_align_factor =
                    data_alignment_factor;
            }
            break;
        }

        case DW_CFA_restore_extended:
        {
            Dwarf_Unsigned lreg = 0;
            int adres = 0;

            adres = _dwarf_leb128_uword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &lreg,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            reg_no = (reg_num_type) lreg;
            ERROR_IF_REG_NUM_TOO_HIGH(reg_no, reg_count);
            if (cie != NULL && cie->ci_initial_table != NULL) {
                localregtab[reg_no] =
                    cie->ci_initial_table->fr_reg[reg_no];
            } else {
                if (!make_instr) {
                    SER(DW_DLE_DF_MAKE_INSTR_NO_INIT);
                }
            }
            if (make_instr) {
                dfi->fi_fields = "r";
                dfi->fi_u0 = lreg;
            }
            break;
        }

        case DW_CFA_undefined:
        {
            Dwarf_Unsigned lreg = 0;
            int adres = 0;

            adres = _dwarf_leb128_uword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &lreg,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            reg_no = (reg_num_type) lreg;
            ERROR_IF_REG_NUM_TOO_HIGH(reg_no, reg_count);
            localregtab[reg_no].ru_is_offset = 0;
            localregtab[reg_no].ru_value_type = DW_EXPR_OFFSET;
            localregtab[reg_no].ru_register =
                dbg->de_frame_undefined_value_number;
            localregtab[reg_no].ru_offset = 0;
            if (make_instr) {
                dfi->fi_fields = "r";
                dfi->fi_u0 = lreg;
            }
            break;
        }

        case DW_CFA_same_value:
        {
            Dwarf_Unsigned lreg = 0;
            int adres = 0;

            adres = _dwarf_leb128_uword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &lreg,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            reg_no = (reg_num_type) lreg;
            ERROR_IF_REG_NUM_TOO_HIGH(reg_no, reg_count);
            localregtab[reg_no].ru_is_offset = 0;
            localregtab[reg_no].ru_value_type = DW_EXPR_OFFSET;
            localregtab[reg_no].ru_register =
                dbg->de_frame_same_value_number;
            localregtab[reg_no].ru_offset = 0;
            if (make_instr) {
                dfi->fi_fields = "r";
                dfi->fi_u0 = lreg;
            }
            break;
        }

        case DW_CFA_register:
        {
            Dwarf_Unsigned lreg;
            reg_num_type reg_noA = 0;
            reg_num_type reg_noB = 0;
            int adres = 0;

            adres = _dwarf_leb128_uword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
            &lreg,error);
                if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            reg_noA = (reg_num_type) lreg;
            ERROR_IF_REG_NUM_TOO_HIGH(reg_noA, reg_count);
            adres = _dwarf_leb128_uword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &lreg,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            reg_noB = (reg_num_type) lreg;
            if (reg_noB > reg_count) {
                SER(DW_DLE_DF_REG_NUM_TOO_HIGH);
            }
            localregtab[reg_noA].ru_is_offset = 0;
            localregtab[reg_noA].ru_value_type = DW_EXPR_OFFSET;
            localregtab[reg_noA].ru_register = reg_noB;
            localregtab[reg_noA].ru_offset = 0;
            if (make_instr) {
                dfi->fi_fields = "rr";
                dfi->fi_u0 = reg_noA;
                dfi->fi_u1 = reg_noB;
            }
            break;
        }

        case DW_CFA_remember_state:
        {
            stack_table = (Dwarf_Frame)
            _dwarf_get_alloc(dbg, DW_DLA_FRAME, 1);
            if (stack_table == NULL) {
                SER(DW_DLE_DF_ALLOC_FAIL);
            }
            for (i = 0; i < reg_count; i++) {
                stack_table->fr_reg[i] = localregtab[i];
            }
            stack_table->fr_cfa_rule = cfa_reg;
            if (top_stack != NULL) {
                stack_table->fr_next = top_stack;
            }
            top_stack = stack_table;
            if (make_instr) {
                dfi->fi_fields = "";
            }
            }
            break;
        case DW_CFA_restore_state:
        {
            if (top_stack == NULL) {
                SER(DW_DLE_DF_POP_EMPTY_STACK);
            }
            stack_table = top_stack;
            top_stack = stack_table->fr_next;
            for (i = 0; i < reg_count; i++) {
                localregtab[i] = stack_table->fr_reg[i];
            }
            cfa_reg = stack_table->fr_cfa_rule;
            dwarf_dealloc(dbg, stack_table, DW_DLA_FRAME);
            if (make_instr) {
                dfi->fi_fields = "";
            }
            break;
        }

        case DW_CFA_def_cfa:
        {
            Dwarf_Unsigned lreg = 0;
            int adres = 0;
            Dwarf_Off nonfactoredoffset = 0;

            adres = _dwarf_leb128_uword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &lreg,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            reg_no = lreg;
            ERROR_IF_REG_NUM_TOO_HIGH(reg_no, reg_count);
            adres = _dwarf_leb128_uword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &nonfactoredoffset,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            if (need_augmentation) {
                SER(DW_DLE_DF_NO_CIE_AUGMENTATION);
            }
            cfa_reg.ru_is_offset = 1;
            cfa_reg.ru_value_type = DW_EXPR_OFFSET;
            cfa_reg.ru_register = reg_no;
            if (INVALIDUNSIGNED(nonfactoredoffset)) {
                SERSTRING(DW_DLE_ARITHMETIC_OVERFLOW,
                    "DW_DLE_ARITHMETIC_OVERFLOW "
                    "DW_CFA_def_cfa offset unrepresantable "
                    "as signed");
            }
            cfa_reg.ru_offset = (Dwarf_Signed)nonfactoredoffset;
            if (make_instr) {
                dfi->fi_fields = "ru";
                dfi->fi_u0 = lreg;
                dfi->fi_u1 = nonfactoredoffset;
            }
            break;
        }

        case DW_CFA_def_cfa_register:
        {
            Dwarf_Unsigned lreg = 0;
            int adres = 0;

            adres = _dwarf_leb128_uword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &lreg,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            reg_no = (reg_num_type) lreg;
            ERROR_IF_REG_NUM_TOO_HIGH(reg_no, reg_count);
            cfa_reg.ru_register = (Dwarf_Half)reg_no;
            /*  Do NOT set ru_offset_or_block_len or
                ru_is_off here.
                See dwarf2/3 spec.  */
            if (make_instr) {
                dfi->fi_fields = "r";
                dfi->fi_u0 = lreg;
            }
            break;
        }

        case DW_CFA_def_cfa_offset:
        {
            int adres = 0;
            adres = _dwarf_leb128_uword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &factored_N_value,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            if (need_augmentation) {
                SER(DW_DLE_DF_NO_CIE_AUGMENTATION);
            }
            /*  Do set ru_is_off here, as here factored_N_value
                counts.  */
            cfa_reg.ru_is_offset = 1;
            cfa_reg.ru_value_type = DW_EXPR_OFFSET;
            if (INVALIDUNSIGNED(factored_N_value)) {
                SERSTRING(DW_DLE_ARITHMETIC_OVERFLOW,
                    "DW_DLE_ARITHMETIC_OVERFLOW "
                    "DW_CFA_def_cfa_offset unrepresantable "
                    "as signed");
            }
            cfa_reg.ru_offset = (Dwarf_Signed)factored_N_value;
            if (make_instr) {
                dfi->fi_fields = "u";
                dfi->fi_u0 = factored_N_value;
            }
            break;
        }
        /*  This is for Metaware with augmentation string HC
            We do not really know what to do with it. */
        case DW_CFA_METAWARE_info:
        {
            int adres = 0;
            adres = _dwarf_leb128_uword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &factored_N_value,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            /* Not really known what the value means or is. */
            cfa_reg.ru_is_offset = 1;
            cfa_reg.ru_value_type = DW_EXPR_OFFSET;
            if (INVALIDUNSIGNED(factored_N_value)) {
                SERSTRING(DW_DLE_ARITHMETIC_OVERFLOW,
                    "DW_DLE_ARITHMETIC_OVERFLOW "
                    "DW_CFA_METAWARE_info unrepresantable as signed");
            }
            cfa_reg.ru_offset = (Dwarf_Signed)factored_N_value;
            if (make_instr) {
                dfi->fi_fields = "u";
                dfi->fi_u0 = factored_N_value;
            }
            break;
        }
        case DW_CFA_nop:
        {
            if (make_instr) {
                dfi->fi_fields = "";
            }
            break;
        }
        /* DWARF3 ops begin here. */
        case DW_CFA_def_cfa_expression: {
            /*  A single DW_FORM_block representing a dwarf
                expression. The form block establishes the way to
                compute the CFA. */
            Dwarf_Unsigned block_len = 0;
            int adres = 0;

            adres = _dwarf_leb128_uword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &block_len,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            cfa_reg.ru_is_offset = 0;  /* arbitrary */
            cfa_reg.ru_value_type = DW_EXPR_EXPRESSION;
            cfa_reg.ru_block.bl_len = block_len;
            cfa_reg.ru_block.bl_data = instr_ptr;
            if (make_instr) {
                dfi->fi_fields = "b";
                dfi->fi_expr.bl_len = block_len;
                dfi->fi_expr.bl_data = instr_ptr;
            }
            if (block_len >= instr_area_length) {
                SERSTRING(DW_DLE_DF_FRAME_DECODING_ERROR,
                    "DW_DLE_DF_FRAME_DECODING_ERROR: "
                    "DW_CFA_def_cfa_expression "
                    "block len overflows instructions "
                    "available range.");
            }
            instr_ptr += block_len;
            if (instr_area_length < block_len ||
                instr_ptr < base_instr_ptr) {
                SERSTRING(DW_DLE_DF_FRAME_DECODING_ERROR,
                    "DW_DLE_DF_FRAME_DECODING_ERROR: "
                    "DW_CFA_def_cfa_expression "
                    "block len overflows instructions "
                    "available range.");
            }
        }
        break;
        case DW_CFA_expression: {
            /*  An unsigned leb128 value is the first operand (a
            register number). The second operand is single
            DW_FORM_block representing a dwarf expression. The
            evaluator pushes the CFA on the evaluation stack
            then evaluates the expression to compute the value
            of the register contents. */
            Dwarf_Unsigned lreg = 0;
            Dwarf_Unsigned block_len = 0;
            int adres = 0;

            adres = _dwarf_leb128_uword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &lreg,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            reg_no = (reg_num_type) lreg;
            ERROR_IF_REG_NUM_TOO_HIGH(reg_no, reg_count);

            adres = _dwarf_leb128_uword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &block_len,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            localregtab[lreg].ru_is_offset = 0; /* arbitrary */
            localregtab[lreg].ru_value_type = DW_EXPR_EXPRESSION;
            localregtab[lreg].ru_block.bl_data = instr_ptr;
            localregtab[lreg].ru_block.bl_len = block_len;
            if (make_instr) {
                dfi->fi_fields = "rb";
                dfi->fi_u0 = lreg;
                dfi->fi_expr.bl_len = block_len;
                dfi->fi_expr.bl_data = instr_ptr;
            }
            instr_ptr += block_len;
            if (instr_area_length < block_len ||
                instr_ptr < base_instr_ptr) {
                SERSTRING(DW_DLE_DF_FRAME_DECODING_ERROR,
                    "DW_DLE_DF_FRAME_DECODING_ERROR: "
                    "DW_CFA_expression "
                    "block len overflows instructions "
                    "available range.");
            }
            }
            break;
        case DW_CFA_offset_extended_sf: {
            /*  The first operand is an unsigned leb128 register
                number. The second is a signed factored offset.
                Identical to DW_CFA_offset_extended except the
                second operand is signed */
            Dwarf_Unsigned lreg = 0;
            int adres = 0;
            Dwarf_Signed result = 0;

            adres = _dwarf_leb128_uword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &lreg,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            reg_no = (reg_num_type) lreg;
            ERROR_IF_REG_NUM_TOO_HIGH(reg_no, reg_count);
            adres = _dwarf_leb128_sword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &signed_factored_N_value,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            if (need_augmentation) {
                SER(DW_DLE_DF_NO_CIE_AUGMENTATION);
            }
            /* CHECK OVERFLOW */
            adres = _dwarf_int64_mult(signed_factored_N_value,
                data_alignment_factor,
                &result,dbg,error);
            if (adres == DW_DLV_ERROR) {
                FREELOCALMALLOC;
                return adres;
            }
            localregtab[reg_no].ru_is_offset = 1;
            localregtab[reg_no].ru_value_type = DW_EXPR_OFFSET;
            localregtab[reg_no].ru_register = reg_num_of_cfa;
            localregtab[reg_no].ru_offset = result;
            if (make_instr) {
                dfi->fi_fields = "rsd";
                dfi->fi_u0 = lreg;
                dfi->fi_s1 = signed_factored_N_value;
                dfi->fi_data_align_factor =
                    data_alignment_factor;
            }
            }
            break;
        case DW_CFA_def_cfa_sf: {
            /*  The first operand is an unsigned leb128 register
                number. The second is a signed leb128 factored
                offset. Identical to DW_CFA_def_cfa except
                that the second operand is signed
                and factored. */
            Dwarf_Unsigned lreg = 0;
            int adres = 0;
            Dwarf_Signed result =0;

            adres = _dwarf_leb128_uword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &lreg,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            reg_no = lreg;
            ERROR_IF_REG_NUM_TOO_HIGH(reg_no, reg_count);
            adres = _dwarf_leb128_sword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &signed_factored_N_value,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            if (need_augmentation) {
                SER(DW_DLE_DF_NO_CIE_AUGMENTATION);
            }
            /*  CHECK OVERFLOW */
            adres = _dwarf_int64_mult(signed_factored_N_value,
                data_alignment_factor,
                &result,dbg,error);
            if (adres == DW_DLV_ERROR) {
                FREELOCALMALLOC;
                return adres;
            }
            cfa_reg.ru_is_offset = 1;
            cfa_reg.ru_value_type = DW_EXPR_OFFSET;
            cfa_reg.ru_register = reg_no;
            cfa_reg.ru_offset = result;
            if (make_instr) {
                dfi->fi_fields = "rsd";
                dfi->fi_u0 = lreg;
                dfi->fi_s1 = signed_factored_N_value;
                dfi->fi_data_align_factor =
                    data_alignment_factor;
            }
            }
            break;
        case DW_CFA_def_cfa_offset_sf: {
            /*  The operand is a signed leb128 operand
                representing a factored offset.  Identical to
                DW_CFA_def_cfa_offset except the operand is
                signed and factored. */
            int adres = 0;
            Dwarf_Signed result = 0;

            adres = _dwarf_leb128_sword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &signed_factored_N_value,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            if (need_augmentation) {
                SER(DW_DLE_DF_NO_CIE_AUGMENTATION);
            }
            /*  CHECK OVERFLOW */
            adres = _dwarf_int64_mult(signed_factored_N_value,
                data_alignment_factor,
                &result,dbg,error);
            if (adres == DW_DLV_ERROR) {
                FREELOCALMALLOC;
                return adres;
            }
            /*  Do set ru_is_off here, as here factored_N_value
                counts.  */
            cfa_reg.ru_is_offset = 1;
            cfa_reg.ru_value_type = DW_EXPR_OFFSET;
            cfa_reg.ru_offset = result;
            if (make_instr) {
                dfi->fi_fields = "sd";
                dfi->fi_s0 = signed_factored_N_value;
                dfi->fi_data_align_factor =
                    data_alignment_factor;
            }
            }
            break;
        case DW_CFA_val_offset: {
            /*  The first operand is an unsigned leb128 register
                number. The second is a factored unsigned offset.
                Makes the register be a val_offset(N)
                rule with N =
                factored_offset*data_alignment_factor. */
            Dwarf_Unsigned lreg = 0;
            int adres = 0;
            Dwarf_Signed result = 0;

            adres = _dwarf_leb128_uword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &lreg,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            reg_no = (reg_num_type) lreg;
            ERROR_IF_REG_NUM_TOO_HIGH(reg_no, reg_count);
            adres = _dwarf_leb128_uword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &factored_N_value,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            if (INVALIDUNSIGNED(factored_N_value) ) {
                SERSTRING(DW_DLE_ARITHMETIC_OVERFLOW,
                    "DW_DLE_ARITHMETIC_OVERFLOW "
                    "in DW_CFA_val_offset factored value");
            }
            if (need_augmentation) {
                SER(DW_DLE_DF_NO_CIE_AUGMENTATION);
            }
            /*  CHECK OVERFLOW */
            adres = _dwarf_int64_mult(
                (Dwarf_Signed)factored_N_value,
                data_alignment_factor,
                &result,dbg,error);
            if (adres == DW_DLV_ERROR) {
                FREELOCALMALLOC;
                return adres;
            }

            /*  Do set ru_is_off here, as here factored_N_value
                counts.  */
            localregtab[reg_no].ru_is_offset = 1;
            localregtab[reg_no].ru_register = reg_num_of_cfa;
            localregtab[reg_no].ru_value_type =
                DW_EXPR_VAL_OFFSET;
            /*  CHECK OVERFLOW */
            localregtab[reg_no].ru_offset = result;
            if (make_instr) {
                dfi->fi_fields = "rud";
                dfi->fi_u0 = lreg;
                dfi->fi_u1 = factored_N_value;
                dfi->fi_data_align_factor =
                    data_alignment_factor;
            }
            break;
        }
        case DW_CFA_val_offset_sf: {
            /*  The first operand is an unsigned leb128 register
                number. The second is a factored signed offset.
                Makes the register be a val_offset(N) rule
                with
                N = factored_offset*data_alignment_factor. */
            Dwarf_Unsigned lreg = 0;
            Dwarf_Signed result = 0;
            int adres = 0;

            adres = _dwarf_leb128_uword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &lreg,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            ERROR_IF_REG_NUM_TOO_HIGH(reg_no, reg_count);
            adres = _dwarf_leb128_sword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &signed_factored_N_value,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            if (need_augmentation) {
                SER(DW_DLE_DF_NO_CIE_AUGMENTATION);
            }
            adres = _dwarf_int64_mult(signed_factored_N_value,
                data_alignment_factor,&result,
                dbg,error);
            if (adres == DW_DLV_ERROR) {
                FREELOCALMALLOC;
                return adres;
            }
            /*  Do set ru_is_off here, as here factored_N_value
                counts.  */
            localregtab[reg_no].ru_is_offset = 1;
            localregtab[reg_no].ru_value_type =
                DW_EXPR_VAL_OFFSET;
            /*  CHECK OVERFLOW */
            localregtab[reg_no].ru_offset = result;
            if (make_instr) {
                dfi->fi_fields = "rsd";
                dfi->fi_u0 = lreg;
                dfi->fi_s1 = signed_factored_N_value;
                dfi->fi_data_align_factor =
                    data_alignment_factor;
            }
            }
            break;
        case DW_CFA_val_expression: {
            /*  The first operand is an unsigned leb128 register
                number. The second is a DW_FORM_block
                representing a
                DWARF expression. The rule for the register
                number becomes a val_expression(E) rule. */
            Dwarf_Unsigned lreg = 0;
            Dwarf_Unsigned block_len = 0;
            int adres = 0;

            adres = _dwarf_leb128_uword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &lreg,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            reg_no = (reg_num_type) lreg;
            ERROR_IF_REG_NUM_TOO_HIGH(reg_no, reg_count);
            adres = _dwarf_leb128_uword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &block_len,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            localregtab[lreg].ru_is_offset = 0; /* arbitrary */
            localregtab[lreg].ru_value_type =
                DW_EXPR_VAL_EXPRESSION;
            localregtab[lreg].ru_offset = 0;
            localregtab[lreg].ru_block.bl_data = instr_ptr;
            localregtab[lreg].ru_block.bl_len = block_len;
            if (make_instr) {
                dfi->fi_fields = "rb";
                dfi->fi_u0 = lreg;
                dfi->fi_expr.bl_len = block_len;
                dfi->fi_expr.bl_data = instr_ptr;
            }
            instr_ptr += block_len;
            if (instr_area_length < block_len ||
                instr_ptr < base_instr_ptr) {
                SERSTRING(DW_DLE_DF_FRAME_DECODING_ERROR,
                    "DW_DLE_DF_FRAME_DECODING_ERROR: "
                    "DW_CFA_val_expression "
                    "block len overflows instructions "
                    "available range.");
            }
        }
        break;
        /* END DWARF3 new ops. */

#ifdef DW_CFA_GNU_window_save
        case DW_CFA_GNU_window_save: {
            /*  No information: this just tells
                unwinder to restore
                the window registers from the previous frame's
                window save area */
            if (make_instr) {
                dfi->fi_fields = "";
            }
        }
        break;
#endif
#ifdef  DW_CFA_GNU_args_size
            /*  Single uleb128 is the current arg area
                size in bytes. No
                register exists yet to save this in.
                the value of must be added to
                an x86 register to get the correct
                stack pointer.
                https://lists.nongnu.org/archive/html/
                libunwind-devel/2016-12/msg00004.html
                https://refspecs.linuxfoundation.org/
                LSB_3.0.0/LSB-PDA/LSB-PDA.junk/dwarfext.html
            */
        case DW_CFA_GNU_args_size: {
            Dwarf_Unsigned asize = 0;
            int adres = 0;

            adres = _dwarf_leb128_uword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &asize,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            /*  Currently not put into ru_* reg rules, not
                sure what to do with it. */
            /*  This is the total size of arguments
                pushed on the stack.  */
            if (make_instr) {
                dfi->fi_fields = "u";
                dfi->fi_u0 = asize;
            }
        }
        break;
#endif
        case DW_CFA_LLVM_def_aspace_cfa: {
            Dwarf_Unsigned lreg = 0;
            Dwarf_Unsigned offset = 0;
            Dwarf_Unsigned addrspace = 0;
            int adres = 0;

            adres = _dwarf_leb128_uword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &lreg,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            ERROR_IF_REG_NUM_TOO_HIGH(lreg, reg_count);
            adres = _dwarf_leb128_uword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &offset,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            adres = _dwarf_leb128_uword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &addrspace,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            if (make_instr) {
                dfi->fi_fields = "rua";
                dfi->fi_u0 = lreg;
                dfi->fi_u1 = offset;
                dfi->fi_u2 = addrspace;
            }
        }
        break;
        case DW_CFA_LLVM_def_aspace_cfa_sf: {
            Dwarf_Unsigned lreg = 0;
            Dwarf_Signed offset = 0;
            Dwarf_Signed result = 0;
            Dwarf_Unsigned addrspace = 0;
            int adres = 0;

            adres = _dwarf_leb128_uword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &lreg,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            ERROR_IF_REG_NUM_TOO_HIGH(lreg, reg_count);
            adres = _dwarf_leb128_sword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &offset,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            adres = _dwarf_leb128_uword_wrapper(dbg,
                &instr_ptr,final_instr_ptr,
                &addrspace,error);
            if (adres != DW_DLV_OK) {
                FREELOCALMALLOC;
                return adres;
            }
            /*  CHECK OVERFLOW */
            adres = _dwarf_int64_mult(
                (Dwarf_Signed)offset,
                data_alignment_factor,
                &result,dbg, error);
            if (adres == DW_DLV_ERROR) {
                FREELOCALMALLOC;
                return DW_DLV_ERROR;
            }
            localregtab[reg_no].ru_is_offset = 1;
            localregtab[reg_no].ru_value_type = DW_EXPR_OFFSET;
            localregtab[reg_no].ru_register = reg_num_of_cfa;
            localregtab[reg_no].ru_offset = result;
            if (make_instr) {
                dfi->fi_fields = "rsda";
                dfi->fi_u0 = lreg;
                dfi->fi_s1 = offset;
                dfi->fi_u2 = addrspace;
                dfi->fi_data_align_factor =
                    data_alignment_factor;
            }
        }
        break;
        default: {
            /*  ERROR, we have an opcode we know nothing
                about. Memory leak here, but an error
                like this is not supposed to
                happen so we ignore the leak.
                These used to be ignored,
                now we notice and report. */
            dwarfstring ms;

            dwarfstring_constructor(&ms);
            dwarfstring_append_printf_u(&ms,
                "DW_DLE_DF_FRAME_DECODING_ERROR:  "
                "instr opcode 0x%x unknown",opcode);
            _dwarf_error_string(dbg,error,
                DW_DLE_DF_FRAME_DECODING_ERROR,
                dwarfstring_string(&ms));
            dwarfstring_destructor(&ms);
            FREELOCALMALLOC;
            return DW_DLV_ERROR;
        }
        }
        if (make_instr) {
            /* add dfi to end of singly-linked list */
            instr_count++;
            (*ilistlastptr) = dfi;
            ilistlastptr = &dfi->fi_next;
            /* dfi itself is stale, the pointer is on the list */
            dfi = 0;
        }
    } /*  end for-loop on ops */

    /*  If frame instruction decoding was right we would
        stop exactly at
        final_instr_ptr. */
    if (instr_ptr > final_instr_ptr) {
        SER(DW_DLE_DF_FRAME_DECODING_ERROR);
    }
    /*  If search_over is set the last instr was an advance_loc
        so we are not done with rows. */
    if ((instr_ptr == final_instr_ptr) && !search_over) {
        if (has_more_rows) {
            *has_more_rows = FALSE;
        }
        if (subsequent_pc) {
            *subsequent_pc = 0;
        }
    } else {
        if (has_more_rows) {
            *has_more_rows = TRUE;
        }
        if (subsequent_pc) {
            *subsequent_pc = possible_subsequent_pc;
        }
    }

    /*  Fill in the actual output table, the space the
        caller passed in. */
    if (table) {

        struct Dwarf_Reg_Rule_s *t2reg = table->fr_reg;
        struct Dwarf_Reg_Rule_s *t3reg = localregtab;
        unsigned minregcount =  (unsigned)MIN(table->fr_reg_count,
            reg_count);
        unsigned curreg = 0;

        table->fr_loc = current_loc;
        for (; curreg < minregcount ; curreg++, t3reg++, t2reg++) {
            *t2reg = *t3reg;
        }

        /*  CONSTCOND */
        /*  Do not update the main table with the cfa_reg.
            Just leave cfa_reg as cfa_reg. */
        table->fr_cfa_rule = cfa_reg;
    }
    /* Dealloc anything remaining on stack. */
    for (; top_stack != NULL;) {
        stack_table = top_stack;
        top_stack = top_stack->fr_next;
        dwarf_dealloc(dbg, stack_table, DW_DLA_FRAME);
    }
    if (make_instr) {
        Dwarf_Frame_Instr_Head head = 0;
        Dwarf_Frame_Instr *instrptrs   = 0;
        Dwarf_Frame_Instr *curinstrptr = 0;
        Dwarf_Frame_Instr cur         = 0;
        Dwarf_Frame_Instr next        = 0;
        Dwarf_Unsigned    ic          = 0;

        head= (Dwarf_Frame_Instr_Head)
            _dwarf_get_alloc(dbg, DW_DLA_FRAME_INSTR_HEAD,1);
        if (!head) {
            SER(DW_DLE_DF_ALLOC_FAIL);
        }
        instrptrs= (Dwarf_Frame_Instr *)
            _dwarf_get_alloc(dbg, DW_DLA_LIST,instr_count);
        if (!instrptrs) {
            dwarf_dealloc(dbg,head,DW_DLA_FRAME_INSTR_HEAD);
            SER(DW_DLE_DF_ALLOC_FAIL);
        }
        head->fh_array = instrptrs;
        head->fh_array_count = instr_count;
        head->fh_dbg = dbg;
        head->fh_cie = cie;
        cur = ilisthead;
        curinstrptr = instrptrs;
        for ( ; cur ; ic++,cur = next,++curinstrptr) {
            *curinstrptr = cur;
            next = cur->fi_next;
            cur->fi_next = 0;
        }
        ilisthead = 0;
        if (ic != instr_count) {
            dwarfstring m;

            FREELOCALMALLOC;
            dwarf_dealloc(dbg,head,DW_DLA_FRAME_INSTR_HEAD);
            dwarfstring_constructor(&m);
            dwarfstring_append_printf_u(&m,
                "DW_DLE_DF_FRAME_DECODING_ERROR: "
                "Instruction array build, instr count %u",
                instr_count);
            dwarfstring_append_printf_u(&m,
                " index i %u. Impossible error.",ic);
            _dwarf_error_string(dbg,error,
                DW_DLE_DF_FRAME_DECODING_ERROR,
                dwarfstring_string(&m));
            dwarfstring_destructor(&m);
            return DW_DLV_ERROR;
        }
        *ret_frame_instr_head = head;
        *returned_frame_instr_count =  instr_count;
    } else {
        if (ret_frame_instr_head) {
            *ret_frame_instr_head = 0;
        }
        if (returned_frame_instr_count) {
            *returned_frame_instr_count = 0;
        }
    }
    FREELOCALMALLOC;
    return DW_DLV_OK;
#undef ERROR_IF_REG_NUM_TOO_HIGH
#undef FREELOCALMALLOC
#undef SER
}

/*  Depending on version, either read the return address register
    as a ubyte or as an leb number.
    The form of this value changed for DWARF3.
*/
int
_dwarf_get_return_address_reg(Dwarf_Small *frame_ptr,
    int version,
    Dwarf_Debug dbg,
    Dwarf_Byte_Ptr section_end,
    unsigned long *size,
    Dwarf_Unsigned *return_address_register,
    Dwarf_Error *error)
{
    Dwarf_Unsigned uvalue = 0;
    Dwarf_Unsigned leb128_length = 0;

    if (version == 1) {
        if (frame_ptr >= section_end) {
            _dwarf_error(NULL, error, DW_DLE_DF_FRAME_DECODING_ERROR);
            return DW_DLV_ERROR;
        }
        *size = 1;
        uvalue = *(unsigned char *) frame_ptr;
        *return_address_register = uvalue;
        return DW_DLV_OK;
    }
    DECODE_LEB128_UWORD_LEN_CK(frame_ptr,uvalue,leb128_length,
        dbg,error,section_end);
    *size = (unsigned long)leb128_length;
    *return_address_register = uvalue;
    return DW_DLV_OK;
}

/* Trivial consumer function.
*/
int
dwarf_get_cie_of_fde(Dwarf_Fde fde,
    Dwarf_Cie * cie_returned, Dwarf_Error * error)
{
    if (!fde) {
        _dwarf_error(NULL, error, DW_DLE_FDE_NULL);
        return DW_DLV_ERROR;
    }

    *cie_returned = fde->fd_cie;
    return DW_DLV_OK;

}

int
dwarf_get_cie_index(
    Dwarf_Cie cie,
    Dwarf_Signed* indx,
    Dwarf_Error* error )
{
    if (cie == NULL)
    {
        _dwarf_error(NULL, error, DW_DLE_CIE_NULL);
        return DW_DLV_ERROR;
    }

    *indx = cie->ci_index;
    return DW_DLV_OK;
}

/*  For g++ .eh_frame fde and cie.
    the cie id is different as the
    definition of the cie_id in an fde
        is the distance back from the address of the
        value to the cie.
    Or 0 if this is a TRUE cie.
    Non standard dwarf, designed this way to be
    convenient at run time for an allocated
    (mapped into memory as part of the running image) section.
*/
int
dwarf_get_fde_list_eh(Dwarf_Debug dbg,
    Dwarf_Cie ** cie_data,
    Dwarf_Signed * cie_element_count,
    Dwarf_Fde ** fde_data,
    Dwarf_Signed * fde_element_count,
    Dwarf_Error * error)
{
    int res = 0;

    CHECK_DBG(dbg,error,"dwarf_get_fde_list_eh()");
    res = _dwarf_load_section(dbg,
        &dbg->de_debug_frame_eh_gnu,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    res = _dwarf_get_fde_list_internal(dbg,
        cie_data,
        cie_element_count,
        fde_data,
        fde_element_count,
        dbg->de_debug_frame_eh_gnu.dss_data,
        dbg->de_debug_frame_eh_gnu.dss_index,
        dbg->de_debug_frame_eh_gnu.dss_size,
        /* cie_id_value */ 0,
        /* use_gnu_cie_calc= */ 1,
        error);
    return res;
}

/*  For standard dwarf .debug_frame
    cie_id is -1  in a cie, and
    is the section offset in the .debug_frame section
    of the cie otherwise.  Standard dwarf
*/
int
dwarf_get_fde_list(Dwarf_Debug dbg,
    Dwarf_Cie ** cie_data,
    Dwarf_Signed * cie_element_count,
    Dwarf_Fde ** fde_data,
    Dwarf_Signed * fde_element_count,
    Dwarf_Error * error)
{
    int res = 0;

    CHECK_DBG(dbg,error,"dwarf_get_fde_list()");
    res = _dwarf_load_section(dbg, &dbg->de_debug_frame,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    res = _dwarf_get_fde_list_internal(dbg, cie_data,
        cie_element_count,
        fde_data,
        fde_element_count,
        dbg->de_debug_frame.dss_data,
        dbg->de_debug_frame.dss_index,
        dbg->de_debug_frame.dss_size,
        (Dwarf_Unsigned)DW_CIE_ID,
        /* use_gnu_cie_calc= */ 0,
        error);

    return res;
}

/*  Only works on dwarf sections, not eh_frame
    because based on DW_AT_MIPS_fde.
    Given a Dwarf_Die, see if it has a
    DW_AT_MIPS_fde attribute and if so use that
    to get an fde offset.
    Then create a Dwarf_Fde to return thru the ret_fde pointer.
    Also creates a cie (pointed at from the Dwarf_Fde).  */
int
dwarf_get_fde_for_die(Dwarf_Debug dbg,
    Dwarf_Die die,
    Dwarf_Fde * ret_fde, Dwarf_Error * error)
{
    Dwarf_Attribute attr;
    Dwarf_Unsigned fde_offset = 0;
    Dwarf_Signed signdval = 0;
    Dwarf_Fde new_fde = 0;
    unsigned char *fde_ptr = 0;
    unsigned char *fde_start_ptr = 0;
    unsigned char *fde_end_ptr = 0;
    unsigned char *cie_ptr = 0;
    Dwarf_Unsigned cie_id = 0;
    Dwarf_Half     address_size = 0;

    /* Fields for the current Cie being read. */
    int res = 0;
    int resattr = 0;
    int sdatares = 0;

    struct cie_fde_prefix_s prefix;
    struct cie_fde_prefix_s prefix_c;

    CHECK_DBG(dbg,error,"dwarf_get_fde_for_die()");
    if (!die ) {
        _dwarf_error_string(NULL, error, DW_DLE_DIE_NULL,
            "DW_DLE_DIE_NUL: in dwarf_get_fde_for_die(): "
            "Called with Dwarf_Die argument null");
        return DW_DLV_ERROR;
    }
    resattr = dwarf_attr(die, DW_AT_MIPS_fde, &attr, error);
    if (resattr != DW_DLV_OK) {
        return resattr;
    }
    /* why is this formsdata? FIX */
    sdatares = dwarf_formsdata(attr, &signdval, error);
    if (sdatares != DW_DLV_OK) {
        dwarf_dealloc_attribute(attr);
        return sdatares;
    }
    res = dwarf_get_die_address_size(die,&address_size,error);
    if (res != DW_DLV_OK) {
        dwarf_dealloc_attribute(attr);
        return res;
    }
    dwarf_dealloc_attribute(attr);
    res = _dwarf_load_section(dbg, &dbg->de_debug_frame,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    fde_offset = signdval;
    fde_start_ptr = dbg->de_debug_frame.dss_data;
    fde_ptr = fde_start_ptr + fde_offset;
    fde_end_ptr = fde_start_ptr + dbg->de_debug_frame.dss_size;
    res = _dwarf_validate_register_numbers(dbg,error);
    if (res == DW_DLV_ERROR) {
        return res;
    }

    /*  First read in the 'common prefix' to figure out
        what we are to do with this entry. */
    memset(&prefix_c, 0, sizeof(prefix_c));
    memset(&prefix, 0, sizeof(prefix));
    res = _dwarf_read_cie_fde_prefix(dbg, fde_ptr,
        dbg->de_debug_frame.dss_data,
        dbg->de_debug_frame.dss_index,
        dbg->de_debug_frame.dss_size,
        &prefix,
        error);
    if (res == DW_DLV_ERROR) {
        return res;
    }
    if (res == DW_DLV_NO_ENTRY) {
        return res;
    }
    fde_ptr = prefix.cf_addr_after_prefix;
    cie_id = prefix.cf_cie_id;
    if (cie_id  >=  dbg->de_debug_frame.dss_size ) {
        _dwarf_error_string(dbg, error, DW_DLE_NO_CIE_FOR_FDE,
            "DW_DLE_NO_CIE_FOR_FDE: "
            "dwarf_get_fde_for_die fails as the CIE id "
            "offset is impossibly large");
        return DW_DLV_ERROR;
    }
    /*  Pass NULL, not section pointer, for 3rd argument.
        de_debug_frame.dss_data has no eh_frame relevance. */
    res = _dwarf_create_fde_from_after_start(dbg, &prefix,
        fde_start_ptr,
        dbg->de_debug_frame.dss_size,
        fde_ptr,
        fde_end_ptr,
        /* use_gnu_cie_calc= */ 0,
        /* Dwarf_Cie = */ 0,
        address_size,
        &new_fde, error);
    if (res == DW_DLV_ERROR) {
        return res;
    }
    if (res == DW_DLV_NO_ENTRY) {
        return res;
    }
    /* DW_DLV_OK */

    /*  This is the only situation this is set.
        and is really dangerous. as fde and cie
        are set for dealloc by dwarf_finish(). */
    new_fde->fd_fde_owns_cie = TRUE;
    /*  Now read the cie corresponding to the fde,
        _dwarf_read_cie_fde_prefix checks
        cie_ptr for being within the section. */
    if (cie_id  >=  dbg->de_debug_frame.dss_size ) {
        _dwarf_error_string(dbg, error, DW_DLE_NO_CIE_FOR_FDE,
            "DW_DLE_NO_CIE_FOR_FDE: "
            "dwarf_get_fde_for_die fails as the CIE id "
            "offset is impossibly large");
        return DW_DLV_ERROR;
    }
    cie_ptr = new_fde->fd_section_ptr + cie_id;
    if ((Dwarf_Unsigned)(uintptr_t)cie_ptr  <
        (Dwarf_Unsigned)(uintptr_t)new_fde->fd_section_ptr ||
        (Dwarf_Unsigned)(uintptr_t)cie_ptr <  cie_id) {
        dwarf_dealloc(dbg,new_fde,DW_DLA_FDE);
        new_fde = 0;
        _dwarf_error_string(dbg, error, DW_DLE_NO_CIE_FOR_FDE,
            "DW_DLE_NO_CIE_FOR_FDE: "
            "dwarf_get_fde_for_die fails as the CIE id "
            "offset is impossibly large");
        return DW_DLV_ERROR;
    }
    res = _dwarf_read_cie_fde_prefix(dbg, cie_ptr,
        dbg->de_debug_frame.dss_data,
        dbg->de_debug_frame.dss_index,
        dbg->de_debug_frame.dss_size,
        &prefix_c, error);
    if (res == DW_DLV_ERROR) {
        dwarf_dealloc(dbg,new_fde,DW_DLA_FDE);
        new_fde = 0;
        return res;
    }
    if (res == DW_DLV_NO_ENTRY) {
        dwarf_dealloc(dbg,new_fde,DW_DLA_FDE);
        new_fde = 0;
        return res;
    }

    cie_ptr = prefix_c.cf_addr_after_prefix;
    cie_id = prefix_c.cf_cie_id;

    if (cie_id == (Dwarf_Unsigned)DW_CIE_ID) {
        int res2 = 0;
        Dwarf_Cie new_cie = 0;

        /*  Pass NULL, not section pointer, for 3rd argument.
            de_debug_frame.dss_data has no eh_frame relevance. */
        res2 = _dwarf_create_cie_from_after_start(dbg,
            &prefix_c,
            fde_start_ptr,
            cie_ptr,
            fde_end_ptr,
            /* cie_count= */ 0,
            /* use_gnu_cie_calc= */
            0, &new_cie, error);
        if (res2 != DW_DLV_OK) {
            dwarf_dealloc(dbg, new_fde, DW_DLA_FDE);
            return res;
        }
        new_fde->fd_cie = new_cie;
    } else {
        dwarf_dealloc(dbg,new_fde,DW_DLA_FDE);
        new_fde = 0;
        _dwarf_error_string(dbg, error, DW_DLE_NO_CIE_FOR_FDE,
            "DW_DLE_NO_CIE_FOR_FDE: "
            "The CIE id is not a true cid id. Corrupt DWARF.");
        return DW_DLV_ERROR;
    }
    *ret_fde = new_fde;
    return DW_DLV_OK;
}

int
dwarf_get_fde_range(Dwarf_Fde fde,
    Dwarf_Addr * low_pc,
    Dwarf_Unsigned * func_length,
    Dwarf_Byte_Ptr * fde_bytes,
    Dwarf_Unsigned * fde_byte_length,
    Dwarf_Off * cie_offset,
    Dwarf_Signed * cie_index,
    Dwarf_Off * fde_offset, Dwarf_Error * error)
{
    Dwarf_Debug dbg;

    if (fde == NULL) {
        _dwarf_error(NULL, error, DW_DLE_FDE_NULL);
        return DW_DLV_ERROR;
    }

    dbg = fde->fd_dbg;
    if (IS_INVALID_DBG(dbg)) {
        _dwarf_error_string(NULL, error, DW_DLE_FDE_DBG_NULL,
            "DW_DLE_FDE_DBG_NULL: Either null or it contains"
            "a stale Dwarf_Debug pointer");
        return DW_DLV_ERROR;
    }
    /*  We have always already done the section load here,
        so no need to load the section. We did the section
        load in order to create the
        Dwarf_Fde pointer passed in here. */
    if (low_pc != NULL)
        *low_pc = fde->fd_initial_location;
    if (func_length != NULL)
        *func_length = fde->fd_address_range;
    if (fde_bytes != NULL)
        *fde_bytes = fde->fd_fde_start;
    if (fde_byte_length != NULL)
        *fde_byte_length = fde->fd_length;
    if (cie_offset != NULL)
        *cie_offset = fde->fd_cie_offset;
    if (cie_index != NULL)
        *cie_index = fde->fd_cie_index;
    if (fde_offset != NULL)
        *fde_offset = fde->fd_fde_start - fde->fd_section_ptr;

    return DW_DLV_OK;
}

/*  IRIX specific function.   The exception tables
    have C++ destructor information and are
    at present undocumented.  */
int
dwarf_get_fde_exception_info(Dwarf_Fde fde,
    Dwarf_Signed *
    offset_into_exception_tables,
    Dwarf_Error * error)
{
    Dwarf_Debug dbg;

    dbg = fde->fd_dbg;
    if (IS_INVALID_DBG(dbg)) {
        _dwarf_error_string(NULL, error, DW_DLE_FDE_DBG_NULL,
            "DW_DLE_FDE_DBG_NULL: Either null or it contains"
            "a stale Dwarf_Debug pointer");
        return DW_DLV_ERROR;
    }

    *offset_into_exception_tables =
        fde->fd_offset_into_exception_tables;
    return DW_DLV_OK;
}

/*  A consumer code function.
    Given a CIE pointer, return the normal CIE data thru
    pointers.
    Special augmentation data is not returned here.
*/
int
dwarf_get_cie_info_b(Dwarf_Cie cie,
    Dwarf_Unsigned *bytes_in_cie,
    Dwarf_Small    *ptr_to_version,
    char          **augmenter,
    Dwarf_Unsigned *code_alignment_factor,
    Dwarf_Signed   *data_alignment_factor,
    Dwarf_Half     *return_address_register,
    Dwarf_Byte_Ptr      *initial_instructions,
    Dwarf_Unsigned *initial_instructions_length,
    Dwarf_Half     *offset_size,
    Dwarf_Error    *error)
{
    Dwarf_Debug dbg = 0;

    if (!cie) {
        _dwarf_error(NULL, error, DW_DLE_CIE_NULL);
        return DW_DLV_ERROR;
    }
    dbg = cie->ci_dbg;
    if (IS_INVALID_DBG(dbg)) {
        _dwarf_error_string(NULL, error, DW_DLE_CIE_DBG_NULL,
            "DW_DLE_CIE_DBG_NULL: Either null or it contains"
            "a stale Dwarf_Debug pointer");
        return DW_DLV_ERROR;
    }
    if (ptr_to_version != NULL)
        *ptr_to_version =
            (Dwarf_Small)cie->ci_cie_version_number;
    if (augmenter != NULL)
        *augmenter = cie->ci_augmentation;
    if (code_alignment_factor != NULL)
        *code_alignment_factor = cie->ci_code_alignment_factor;
    if (data_alignment_factor != NULL)
        *data_alignment_factor = cie->ci_data_alignment_factor;
    if (return_address_register != NULL)
        *return_address_register =
            (Dwarf_Half)cie->ci_return_address_register;
    if (initial_instructions != NULL)
        *initial_instructions = cie->ci_cie_instr_start;
    if (initial_instructions_length != NULL) {
        *initial_instructions_length = cie->ci_length +
            cie->ci_length_size +
            cie->ci_extension_size -
            (cie->ci_cie_instr_start - cie->ci_cie_start);
    }
    if (offset_size) {
        *offset_size = cie->ci_length_size;
    }
    *bytes_in_cie = (cie->ci_length);
    return DW_DLV_OK;
}

/* Return the register rules for all registers at a given pc.
*/
static int
_dwarf_get_fde_info_for_a_pc_row(Dwarf_Fde fde,
    Dwarf_Addr pc_requested,
    Dwarf_Frame table,
    Dwarf_Unsigned cfa_reg_col_num,
    Dwarf_Bool * has_more_rows,
    Dwarf_Addr * subsequent_pc,
    Dwarf_Error * error)
{
    Dwarf_Debug dbg = 0;
    Dwarf_Cie cie = 0;
    int res = 0;

    if (fde == NULL) {
        _dwarf_error(NULL, error, DW_DLE_FDE_NULL);
        return DW_DLV_ERROR;
    }

    dbg = fde->fd_dbg;
    if (IS_INVALID_DBG(dbg)) {
        _dwarf_error(NULL, error, DW_DLE_FDE_DBG_NULL);
        return DW_DLV_ERROR;
    }

    if (pc_requested < fde->fd_initial_location ||
        pc_requested >=
        fde->fd_initial_location + fde->fd_address_range) {
        _dwarf_error(dbg, error, DW_DLE_PC_NOT_IN_FDE_RANGE);
        return DW_DLV_ERROR;
    }

    cie = fde->fd_cie;
    if (cie->ci_initial_table == NULL) {
        Dwarf_Small *instrstart = cie->ci_cie_instr_start;
        Dwarf_Small *instrend = instrstart +cie->ci_length +
            cie->ci_length_size +
            cie->ci_extension_size -
            (cie->ci_cie_instr_start -
            cie->ci_cie_start);
        if (instrend > cie->ci_cie_end) {
            _dwarf_error(dbg, error,DW_DLE_CIE_INSTR_PTR_ERROR);
            return DW_DLV_ERROR;
        }
        cie->ci_initial_table = (Dwarf_Frame)_dwarf_get_alloc(dbg,
            DW_DLA_FRAME, 1);

        if (cie->ci_initial_table == NULL) {
            _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
            return DW_DLV_ERROR;
        }
        _dwarf_init_reg_rules_ru(cie->ci_initial_table->fr_reg,
            0, cie->ci_initial_table->fr_reg_count,
            dbg->de_frame_rule_initial_value);
        _dwarf_init_reg_rules_ru(&cie->ci_initial_table->fr_cfa_rule,
            0,1,dbg->de_frame_rule_initial_value);
        res = _dwarf_exec_frame_instr( /* make_instr= */ FALSE,
            /* search_pc */ FALSE,
            /* search_pc_val */ 0,
            /* location */ 0,
            instrstart,
            instrend,
            cie->ci_initial_table,
            cie, dbg,
            cfa_reg_col_num,
            has_more_rows,
            subsequent_pc,
            NULL,NULL,
            error);
        if (res != DW_DLV_OK) {
            return res;
        }
    }

    {
        Dwarf_Small *instr_end = fde->fd_length +
            fde->fd_length_size +
            fde->fd_extension_size + fde->fd_fde_start;
        if (instr_end > fde->fd_fde_end) {
            _dwarf_error(dbg, error,DW_DLE_FDE_INSTR_PTR_ERROR);
            return DW_DLV_ERROR;
        }
        res = _dwarf_exec_frame_instr( /* make_instr= */ FALSE,
            /* search_pc */ TRUE,
            /* search_pc_val */ pc_requested,
            fde->fd_initial_location,
            fde->fd_fde_instr_start,
            instr_end,
            table,
            cie,dbg,
            cfa_reg_col_num,
            has_more_rows,
            subsequent_pc,
            NULL,NULL,
            error);
    }
    if (res != DW_DLV_OK) {
        return res;
    }

    return DW_DLV_OK;
}

int
dwarf_get_fde_info_for_all_regs3_b(Dwarf_Fde fde,
    Dwarf_Addr pc_requested,
    Dwarf_Regtable3 * reg_table,
    Dwarf_Addr * row_pc,
    Dwarf_Bool * has_more_rows,
    Dwarf_Addr * subsequent_pc,
    Dwarf_Error * error)
{

    struct Dwarf_Frame_s fde_table;
    Dwarf_Unsigned i = 0;
    int res = 0;
    struct Dwarf_Reg_Rule_s *rule = NULL;

    /* Internal-only struct. */
    Dwarf_Regtable_Entry3_i *rule_i = NULL;

    Dwarf_Debug dbg = 0;
    Dwarf_Unsigned output_table_real_data_size = 0;
    Dwarf_Regtable3_i reg_table_i;

    memset(&reg_table_i,0,sizeof(reg_table_i));
    memset(&fde_table,0,sizeof(fde_table));
    FDE_NULL_CHECKS_AND_SET_DBG(fde, dbg);
    output_table_real_data_size = reg_table->rt3_reg_table_size;
    reg_table_i.rt3_reg_table_size = output_table_real_data_size;
    output_table_real_data_size =
        MIN(output_table_real_data_size,
            dbg->de_frame_reg_rules_entry_count);
    res = _dwarf_initialize_fde_table(dbg, &fde_table,
        output_table_real_data_size,
        error);
    if (res != DW_DLV_OK) {
        return res;
    }
    /* Allocate array of internal structs to match,
        in count, what was passed in. */
    reg_table_i.rt3_rules = calloc(reg_table->rt3_reg_table_size,
        sizeof(Dwarf_Regtable_Entry3_i));
    if (!reg_table_i.rt3_rules) {
        _dwarf_free_fde_table(&fde_table);
        _dwarf_error_string(dbg,error,
            DW_DLE_ALLOC_FAIL,
            "Failure allocating Dwarf_Regtable_Entry3_i "
            "in dwarf_get_fde_info_for_all_regs3()");
        return DW_DLV_ERROR;
    }
    /*  _dwarf_get_fde_info_for_a_pc_row will perform
        more sanity checks */
    res = _dwarf_get_fde_info_for_a_pc_row(fde, pc_requested,
        &fde_table,
        dbg->de_frame_cfa_col_number,
        has_more_rows,subsequent_pc,
        error);
    if (res != DW_DLV_OK) {
        free(reg_table_i.rt3_rules);
        reg_table_i.rt3_rules = 0;
        _dwarf_free_fde_table(&fde_table);
        return res;
    }

    rule_i =    &reg_table_i.rt3_rules[0];
    rule = &fde_table.fr_reg[0];
    /* Initialize known rules */
    for (i = 0; i < output_table_real_data_size;
        i++, ++rule_i, ++rule) {
        rule_i->dw_offset_relevant = rule->ru_is_offset;
        rule_i->dw_args_size  = rule->ru_args_size;
        rule_i->dw_value_type = rule->ru_value_type;
        rule_i->dw_regnum = rule->ru_register;
        rule_i->dw_offset = (Dwarf_Unsigned)rule->ru_offset;
        rule_i->dw_block = rule->ru_block;
    }
    /*  If i < reg_table_i.rt3_reg_table_size finish
        initializing register rules */
    _dwarf_init_reg_rules_dw3(&reg_table_i.rt3_rules[0],
        i, reg_table_i.rt3_reg_table_size,
        dbg->de_frame_undefined_value_number);
    {
        /*  Now get this into the real output.
            Truncating rule numbers, and offset set
            unsigned. */
        Dwarf_Unsigned j = 0;
        Dwarf_Regtable_Entry3 *targ = &reg_table->rt3_rules[0];
        Dwarf_Regtable_Entry3_i *src = &reg_table_i.rt3_rules[0];
        for ( ; j < reg_table->rt3_reg_table_size;
            ++j, targ++,src++) {
            targ->dw_offset_relevant = src->dw_offset_relevant;
            targ->dw_args_size = src->dw_args_size;
            targ->dw_value_type = src->dw_value_type;
            targ->dw_regnum = (Dwarf_Half)src->dw_regnum;
            targ->dw_offset= (Dwarf_Unsigned)src->dw_offset;
            targ->dw_block = src->dw_block;
        }
    }
    reg_table->rt3_cfa_rule.dw_offset_relevant =
        fde_table.fr_cfa_rule.ru_is_offset;
    reg_table->rt3_cfa_rule.dw_value_type =
        fde_table.fr_cfa_rule.ru_value_type;
    reg_table->rt3_cfa_rule.dw_regnum =
        (Dwarf_Half)fde_table.fr_cfa_rule.ru_register;
    reg_table->rt3_cfa_rule.dw_offset =
        (Dwarf_Unsigned)fde_table.fr_cfa_rule.ru_offset;
    reg_table->rt3_cfa_rule.dw_block =
        fde_table.fr_cfa_rule.ru_block;
    reg_table->rt3_cfa_rule.dw_args_size =
        fde_table.fr_cfa_rule.ru_args_size;
    if (row_pc != NULL) {
        *row_pc = fde_table.fr_loc;
    }
    free(reg_table_i.rt3_rules);
    reg_table_i.rt3_rules = 0;
    reg_table_i.rt3_reg_table_size = 0;
    _dwarf_free_fde_table(&fde_table);
    return DW_DLV_OK;
}

int
dwarf_get_fde_info_for_all_regs3(Dwarf_Fde fde,
    Dwarf_Addr pc_requested,
    Dwarf_Regtable3 * reg_table,
    Dwarf_Addr * row_pc,
    Dwarf_Error * error)
{
    int res = dwarf_get_fde_info_for_all_regs3_b(fde,pc_requested,
        reg_table,row_pc,NULL,NULL,error);

    return res;
}

/*  Table_column DW_FRAME_CFA_COL is not meaningful.
    Use  dwarf_get_fde_info_for_cfa_reg3_b() to get the CFA.
    Call dwarf_set_frame_cfa_value() to set the correct column
    after calling dwarf_init()
    (DW_FRAME_CFA_COL3 is a sensible column to use).
*/
/*  New May 2018.
    If one is tracking the value of a single table
    column through a function, this lets us
    skip to the next pc value easily.

    if pc_requested is a change from the last
    pc_requested on this pc, this function
    returns *has_more_rows and *subsequent_pc
    (null pointers passed are acceptable, the
    assignment through the pointer is skipped
    if the pointer is null).
    Otherwise *has_more_rows and *subsequent_pc
    are not set.

    The offset returned is Unsigned, which was
    always wrong. Cast to Dwarf_Signed to use it.
*/
int
dwarf_get_fde_info_for_reg3_b(Dwarf_Fde fde,
    Dwarf_Half      table_column,
    Dwarf_Addr      requested,
    Dwarf_Small    *value_type,
    Dwarf_Unsigned *offset_relevant,
    Dwarf_Unsigned *register_num,
    Dwarf_Unsigned *offset,
    Dwarf_Block    *block,
    Dwarf_Addr     *row_pc_out,
    Dwarf_Bool     *has_more_rows,
    Dwarf_Addr     *subsequent_pc,
    Dwarf_Error    *error)
{
    Dwarf_Signed soff = 0;
    int res = 0;

    res = dwarf_get_fde_info_for_reg3_c(
        fde,table_column,requested,
        value_type,offset_relevant,
        register_num,&soff,
        block,row_pc_out,has_more_rows,
        subsequent_pc,error);
    if (offset) {
        *offset = (Dwarf_Unsigned)soff;
    }
    return res;
}
/*  New September 2023.
    The same as dwarf_get_fde_info_for_reg3_b() but here
*/
int
dwarf_get_fde_info_for_reg3_c(Dwarf_Fde fde,
    Dwarf_Half      table_column,
    Dwarf_Addr      pc_requested,
    Dwarf_Small    *value_type,
    Dwarf_Unsigned *offset_relevant,
    Dwarf_Unsigned *register_num,
    Dwarf_Signed   *offset,
    Dwarf_Block    *block,
    Dwarf_Addr     *row_pc_out,
    Dwarf_Bool     *has_more_rows,
    Dwarf_Addr     *subsequent_pc,
    Dwarf_Error    *error)
{
    struct Dwarf_Frame_s * fde_table = &(fde->fd_fde_table);
    int res = DW_DLV_ERROR;

    Dwarf_Debug dbg = 0;
    Dwarf_Unsigned table_real_data_size = 0;

    FDE_NULL_CHECKS_AND_SET_DBG(fde, dbg);

    if (!fde->fd_have_fde_tab  ||
    /*  The test is just in case it's not inside the table.
        For non-MIPS
        it could be outside the table and that is just fine, it was
        really a mistake to put it in the table in 1993.  */
        fde->fd_fde_pc_requested != pc_requested) {
        if (fde->fd_have_fde_tab) {
            _dwarf_free_fde_table(fde_table);
            fde->fd_have_fde_tab = FALSE;
        }
        table_real_data_size = dbg->de_frame_reg_rules_entry_count;
        res = _dwarf_initialize_fde_table(dbg, fde_table,
            table_real_data_size, error);
        if (res != DW_DLV_OK) {
            return res;
        }
        if (table_column >= table_real_data_size) {
            _dwarf_free_fde_table(fde_table);
            fde->fd_have_fde_tab = FALSE;
            _dwarf_error(dbg, error, DW_DLE_FRAME_TABLE_COL_BAD);
            return DW_DLV_ERROR;
        }

        /*  _dwarf_get_fde_info_for_a_pc_row will perform
            more sanity checks */
        res = _dwarf_get_fde_info_for_a_pc_row(fde,
            pc_requested, fde_table,
            dbg->de_frame_cfa_col_number,
            has_more_rows,subsequent_pc,
            error);
        if (res != DW_DLV_OK) {
            _dwarf_free_fde_table(fde_table);
            fde->fd_have_fde_tab = FALSE;
            return res;
        }
    }

    if (register_num) {
        *register_num = fde_table->fr_reg[table_column].ru_register;
    }
    if (offset) {
        *offset = fde_table->fr_reg[table_column].ru_offset;
    }
    if (row_pc_out != NULL) {
        *row_pc_out = fde_table->fr_loc;
    }
    if (block) {
        *block = fde_table->fr_reg[table_column].ru_block;
    }

    /*  Without value_type the data cannot be understood,
        so we insist on it being present, we don't test it. */
    *value_type = fde_table->fr_reg[table_column].ru_value_type;
    *offset_relevant = (fde_table->fr_reg[table_column].ru_is_offset);
    fde->fd_have_fde_tab = TRUE;
    fde->fd_fde_pc_requested = pc_requested;
    return DW_DLV_OK;

}

/*
    This deals with the  CFA by not
    making the CFA a column number, which means
    DW_FRAME_CFA_COL3 is, like DW_CFA_SAME_VALUE,
    a special value, not something one uses as an index.

    Call dwarf_set_frame_cfa_value() to set the correct column
    after calling dwarf_init().
    DW_FRAME_CFA_COL3 is a sensible column to use.
*/
int
dwarf_get_fde_info_for_cfa_reg3_b(Dwarf_Fde fde,
    Dwarf_Addr      pc_requested,
    Dwarf_Small    *value_type,
    Dwarf_Unsigned *offset_relevant,
    Dwarf_Unsigned *register_num,
    Dwarf_Unsigned *offset,
    Dwarf_Block    *block,
    Dwarf_Addr     *row_pc_out,
    Dwarf_Bool     *has_more_rows,
    Dwarf_Addr     *subsequent_pc,
    Dwarf_Error    *error)
{
    Dwarf_Signed soff = 0;
    int res = 0;

    res = dwarf_get_fde_info_for_cfa_reg3_c(fde,
        pc_requested, value_type,offset_relevant,
        register_num,&soff,block, row_pc_out,
        has_more_rows,subsequent_pc,error);
    if (offset) {
        *offset = (Dwarf_Unsigned)soff;
    }
    return res;
}
/*
    New September 2023. With the offset argument
    a signed value.  This is more correct, so
    convert from dwarf_get_fde_info_for_cfa_reg3_b
    when convenient.
*/
int
dwarf_get_fde_info_for_cfa_reg3_c(Dwarf_Fde fde,
    Dwarf_Addr      pc_requested,
    Dwarf_Small    *value_type,
    Dwarf_Unsigned *offset_relevant,
    Dwarf_Unsigned *register_num,
    Dwarf_Signed   *offset,
    Dwarf_Block    *block,
    Dwarf_Addr     *row_pc_out,
    Dwarf_Bool     *has_more_rows,
    Dwarf_Addr     *subsequent_pc,
    Dwarf_Error    *error)
{
    struct Dwarf_Frame_s fde_table;
    int res = DW_DLV_ERROR;
    Dwarf_Debug dbg = 0;

    Dwarf_Unsigned table_real_data_size = 0;

    FDE_NULL_CHECKS_AND_SET_DBG(fde, dbg);

    table_real_data_size = dbg->de_frame_reg_rules_entry_count;
    res = _dwarf_initialize_fde_table(dbg, &fde_table,
        table_real_data_size, error);
    if (res != DW_DLV_OK)
        return res;
    res = _dwarf_get_fde_info_for_a_pc_row(fde, pc_requested,
        &fde_table,
        dbg->de_frame_cfa_col_number,has_more_rows,
        subsequent_pc,error);
    if (res != DW_DLV_OK) {
        _dwarf_free_fde_table(&fde_table);
        return res;
    }
    if (register_num) {
        *register_num = fde_table.fr_cfa_rule.ru_register;
    }
    if (offset) {
        *offset = fde_table.fr_cfa_rule.ru_offset;
    }
    if (row_pc_out != NULL) {
        *row_pc_out = fde_table.fr_loc;
    }
    if (block) {
        *block = fde_table.fr_cfa_rule.ru_block;
    }
    /*  Without value_type the data cannot be
        understood, so we insist
        on it being present, we don't test it. */
    *value_type = fde_table.fr_cfa_rule.ru_value_type;
    *offset_relevant = fde_table.fr_cfa_rule.ru_is_offset;
    _dwarf_free_fde_table(&fde_table);
    return DW_DLV_OK;
}

/*  Return pointer to the instructions in the dwarf fde.  */
int
dwarf_get_fde_instr_bytes(Dwarf_Fde inFde,
    Dwarf_Small   ** outinstrs,
    Dwarf_Unsigned * outinstrslen,
    Dwarf_Error    * error)
{
    Dwarf_Unsigned len = 0;
    Dwarf_Small *instrs = 0;
    Dwarf_Debug dbg = 0;

    if (!inFde) {
        _dwarf_error(dbg, error, DW_DLE_FDE_NULL);
        return DW_DLV_ERROR;
    }
    dbg = inFde->fd_dbg;
    if (IS_INVALID_DBG(dbg)) {
        _dwarf_error_string(NULL, error, DW_DLE_FDE_DBG_NULL,
            "DW_DLE_FDE_DBG_NULL: Either null or it contains"
            "a stale Dwarf_Debug pointer");
        return DW_DLV_ERROR;
    }
    instrs = inFde->fd_fde_instr_start;
    len = inFde->fd_fde_end - inFde->fd_fde_instr_start;
    *outinstrs = instrs;
    *outinstrslen = len;
    return DW_DLV_OK;
}

/*  Allows getting an fde from its table via an index.
    With more error checking than simply indexing oneself.  */
int
dwarf_get_fde_n(Dwarf_Fde * fde_data,
    Dwarf_Unsigned fde_index,
    Dwarf_Fde * returned_fde, Dwarf_Error * error)
{
    Dwarf_Debug dbg = 0;
    Dwarf_Unsigned fdecount = 0;

    if (fde_data == NULL) {
        _dwarf_error(dbg, error, DW_DLE_FDE_PTR_NULL);
        return DW_DLV_ERROR;
    }

    FDE_NULL_CHECKS_AND_SET_DBG(*fde_data, dbg);
    /* Assumes fde_data table has at least one entry. */
    fdecount = fde_data[0]->fd_is_eh?
        dbg->de_fde_count_eh:dbg->de_fde_count;
    if (fde_index >= fdecount) {
        return DW_DLV_NO_ENTRY;
    }
    *returned_fde = (*(fde_data + fde_index));
    return DW_DLV_OK;
}

/*  Lopc and hipc are extensions to the interface to
    return the range of addresses that are described
    by the returned fde.  */
int
dwarf_get_fde_at_pc(Dwarf_Fde * fde_data,
    Dwarf_Addr pc_of_interest,
    Dwarf_Fde * returned_fde,
    Dwarf_Addr * lopc,
    Dwarf_Addr * hipc, Dwarf_Error * error)
{
    Dwarf_Debug dbg = NULL;
    Dwarf_Fde fde = NULL;
    Dwarf_Fde entryfde = NULL;
    Dwarf_Signed fdecount = 0;

    if (fde_data == NULL) {
        _dwarf_error(NULL, error, DW_DLE_FDE_PTR_NULL);
        return DW_DLV_ERROR;
    }

    /*  Assumes fde_data table has at least one entry. */
    entryfde = *fde_data;
    FDE_NULL_CHECKS_AND_SET_DBG(entryfde, dbg);
    fdecount = entryfde->fd_is_eh?
        dbg->de_fde_count_eh:dbg->de_fde_count;
    {
        /*  The fdes are sorted by their addresses. Binary search to
            find correct fde. */
        Dwarf_Signed low = 0;
        Dwarf_Signed high = fdecount - 1L;
        Dwarf_Signed middle = 0;
        Dwarf_Fde cur_fde;

        while (low <= high) {
            middle = (low + high) / 2;
            cur_fde = fde_data[middle];
            if (pc_of_interest < cur_fde->fd_initial_location) {
                high = middle - 1;
            } else if (pc_of_interest >=
                (cur_fde->fd_initial_location +
                cur_fde->fd_address_range)) {
                low = middle + 1;
            } else {
                fde = fde_data[middle];
                break;
            }
        }
    }

    if (fde) {
        if (lopc != NULL)
            *lopc = fde->fd_initial_location;
        if (hipc != NULL)
            *hipc =
                fde->fd_initial_location + fde->fd_address_range - 1;
        *returned_fde = fde;
        return DW_DLV_OK;
    }

    return DW_DLV_NO_ENTRY;
}

/*  Expands a single frame instruction block
    from a specific cie or fde into a
    Dwarf_Frame_Instr_Head.

    Call dwarf_set_frame_cfa_value() to set the correct column
    after calling dwarf_init().
    DW_FRAME_CFA_COL3 is a sensible column to use.
*/
int
dwarf_expand_frame_instructions(Dwarf_Cie cie,
    Dwarf_Small   *instruction,
    Dwarf_Unsigned i_length,
    Dwarf_Frame_Instr_Head * returned_instr_head,
    Dwarf_Unsigned * returned_instr_count,
    Dwarf_Error * error)
{
    int res = DW_DLV_ERROR;
    Dwarf_Debug dbg = 0;
    Dwarf_Small * instr_start = instruction;
    Dwarf_Small * instr_end = (Dwarf_Small *)instruction + i_length;;

    if (cie == 0) {
        _dwarf_error(NULL, error, DW_DLE_DBG_NULL);
        return DW_DLV_ERROR;
    }
    dbg = cie->ci_dbg;

    if (!returned_instr_head  || !returned_instr_count) {
        _dwarf_error_string(dbg, error, DW_DLE_RET_OP_LIST_NULL,
            "DW_DLE_RET_OP_LIST_NULL: "
            "Calling dwarf_expand_frame_instructions without "
            "a non-NULL Dwarf_Frame_Instr_Head pointer and "
            "count pointer seems wrong.");
        return DW_DLV_ERROR;
    }
    if ( instr_end < instr_start) {
        /*  Impossible unless there was wraparond somewhere and
            we missed it. */
        _dwarf_error(dbg, error,DW_DLE_FDE_INSTR_PTR_ERROR);
        return DW_DLV_ERROR;
    }
    res = _dwarf_exec_frame_instr( /* make_instr= */ TRUE,
        /* search_pc */ FALSE,
        /* search_pc_val */ 0,
        /* location */ 0,
        instr_start,
        instr_end,
        /* Dwarf_Frame */ NULL,
        cie,
        dbg,
        dbg->de_frame_cfa_col_number,
        /* has more rows */0,
        /* subsequent_pc */0,
        returned_instr_head,
        returned_instr_count,
        error);
    if (res != DW_DLV_OK) {
        return res;
    }
    return DW_DLV_OK;
}

/*  Call to access  a single CFA frame instruction.
    The 2021 DW_CFA_LLVM addition for hetrogenous
    debugging has a third field,  an address space
    value.  */
int
dwarf_get_frame_instruction(Dwarf_Frame_Instr_Head head,
    Dwarf_Unsigned    instr_index,
    Dwarf_Unsigned  * instr_offset_in_instrs,
    Dwarf_Small     * cfa_operation,
    const char     ** fields_description,
    Dwarf_Unsigned  * u0,
    Dwarf_Unsigned  * u1,
    Dwarf_Signed    * s0,
    Dwarf_Signed    * s1,
    Dwarf_Unsigned  * code_alignment_factor,
    Dwarf_Signed    * data_alignment_factor,
    Dwarf_Block     * expression_block,
    Dwarf_Error     * error)
{
    Dwarf_Unsigned aspace = 0;
    return dwarf_get_frame_instruction_a(head,
        instr_index,
        instr_offset_in_instrs,
        cfa_operation,
        fields_description,
        u0,
        u1,
        & aspace,
        s0,
        s1,
        code_alignment_factor,
        data_alignment_factor,
        expression_block,
        error);
}
int
dwarf_get_frame_instruction_a(Dwarf_Frame_Instr_Head head,
    Dwarf_Unsigned    instr_index,
    Dwarf_Unsigned  * instr_offset_in_instrs,
    Dwarf_Small     * cfa_operation,
    const char     ** fields_description,
    Dwarf_Unsigned  * u0,
    Dwarf_Unsigned  * u1,
    Dwarf_Unsigned  * u2,
    Dwarf_Signed    * s0,
    Dwarf_Signed    * s1,
    Dwarf_Unsigned  * code_alignment_factor,
    Dwarf_Signed    * data_alignment_factor,
    Dwarf_Block     * expression_block,
    Dwarf_Error     * error)
{
    Dwarf_Frame_Instr ip = 0;
    Dwarf_Debug dbg = 0;
    if (!head) {
        _dwarf_error_string(dbg, error,DW_DLE_CFA_INSTRUCTION_ERROR,
            "DW_DLE_CFA_INSTRUCTION_ERROR: Head argument NULL "
            " calling dwarf_get_frame_instruction");
        return DW_DLV_ERROR;
    }
    if (!head->fh_dbg) {
        _dwarf_error_string(dbg, error,DW_DLE_CFA_INSTRUCTION_ERROR,
            "DW_DLE_CFA_INSTRUCTION_ERROR: Head missing "
            "Dwarf_Debug field "
            " calling dwarf_get_frame_instruction");
        return DW_DLV_ERROR;
    }
    dbg = head->fh_dbg;
    if (instr_index >= head->fh_array_count) {
        return DW_DLV_NO_ENTRY;
    }
    ip = head->fh_array[instr_index];
    if (!ip) {
        _dwarf_error_string(dbg, error,DW_DLE_CFA_INSTRUCTION_ERROR,
            "DW_DLE_CFA_INSTRUCTION_ERROR: instr array missing "
            "calling dwarf_get_frame_instruction");
        return DW_DLV_ERROR;
    }
    *instr_offset_in_instrs = ip->fi_instr_offset;
    *cfa_operation = ip->fi_op;
    *fields_description = ip->fi_fields;
    *u0 = ip->fi_u0;
    *u1 = ip->fi_u1;
    *u2 = ip->fi_u2;
    *s0 = ip->fi_s0;
    *s1 = ip->fi_s1;
    /*  These next two might be known to caller already,
        so let caller not pass useless pointers. */
    if (code_alignment_factor) {
        *code_alignment_factor = ip->fi_code_align_factor;
    }
    if (data_alignment_factor) {
        *data_alignment_factor = ip->fi_data_align_factor;
    }
    *expression_block = ip->fi_expr;
    return DW_DLV_OK;
}

/*  Used by dwarfdump -v to print offsets, for debugging
    dwarf info.
    The dwarf_ version is preferred over the obsolete _dwarf version.
    _dwarf version kept for compatibility.
*/
int
_dwarf_fde_section_offset(Dwarf_Debug dbg, Dwarf_Fde in_fde,
    Dwarf_Off * fde_off, Dwarf_Off * cie_off,
    Dwarf_Error * error)
{
    return dwarf_fde_section_offset(dbg,in_fde,fde_off,
        cie_off,error);
}
int
dwarf_fde_section_offset(Dwarf_Debug dbg, Dwarf_Fde in_fde,
    Dwarf_Off * fde_off, Dwarf_Off * cie_off,
    Dwarf_Error * error)
{
    char *start = 0;
    char *loc = 0;

    CHECK_DBG(dbg,error,"dwarf_fde_section_offset()");
    if (!in_fde) {
        _dwarf_error(dbg, error, DW_DLE_FDE_NULL);
        return DW_DLV_ERROR;
    }
    start = (char *) in_fde->fd_section_ptr;
    loc = (char *) in_fde->fd_fde_start;

    *fde_off = (loc - start);
    *cie_off = in_fde->fd_cie_offset;
    return DW_DLV_OK;
}

/* Used by dwarfdump -v to print offsets, for debugging
   dwarf info.
   The dwarf_ version is preferred over the obsolete _dwarf version.
   _dwarf version kept for compatibility.
*/
int
_dwarf_cie_section_offset(Dwarf_Debug dbg, Dwarf_Cie in_cie,
    Dwarf_Off * cie_off, Dwarf_Error * error)
{
    return dwarf_cie_section_offset(dbg,in_cie,cie_off,error);
}

int
dwarf_cie_section_offset(Dwarf_Debug dbg, Dwarf_Cie in_cie,
    Dwarf_Off * cie_off, Dwarf_Error * error)
{
    char *start = 0;
    char *loc = 0;

    CHECK_DBG(dbg,error,"dwarf_cie_section_offset()");
    if (!in_cie) {
        _dwarf_error(dbg, error, DW_DLE_CIE_NULL);
        return DW_DLV_ERROR;
    }
    start = (char *) in_cie->ci_section_ptr;
    loc = (char *) in_cie->ci_cie_start;

    *cie_off = (loc - start);
    return DW_DLV_OK;
}

/*  Returns  a pointer to target-specific augmentation data
    thru augdata
    and returns the length of the data thru augdata_len.

    It's up to the consumer code to know how to interpret the bytes
    of target-specific data (endian issues apply too, these
    are just raw bytes pointed to).
    See  Linux Standard Base Core Specification version 3.0 for
    the details on .eh_frame info.

    Returns DW_DLV_ERROR if fde is NULL or some other serious
    error.
    Returns DW_DLV_NO_ENTRY if there is no target-specific
    augmentation data.

    The bytes pointed to are in the Dwarf_Cie, and as long as that
    is valid the bytes are there. No 'dealloc' call is needed
    for the bytes.  */
int
dwarf_get_cie_augmentation_data(Dwarf_Cie cie,
    Dwarf_Small ** augdata,
    Dwarf_Unsigned * augdata_len,
    Dwarf_Error * error)
{
    if (cie == NULL) {
        _dwarf_error(NULL, error, DW_DLE_CIE_NULL);
        return DW_DLV_ERROR;
    }
    if (cie->ci_gnu_eh_augmentation_len == 0) {
        return DW_DLV_NO_ENTRY;
    }
    *augdata = (Dwarf_Small *) (cie->ci_gnu_eh_augmentation_bytes);
    *augdata_len = cie->ci_gnu_eh_augmentation_len;
    return DW_DLV_OK;
}

/*  Returns  a pointer to target-specific augmentation data
    thru augdata
    and returns the length of the data thru augdata_len.

    It's up to the consumer code to know how to interpret the bytes
    of target-specific data (endian issues apply too, these
    are just raw bytes pointed to).
    See  Linux Standard Base Core Specification version 3.0 for
    the details on .eh_frame info.

    Returns DW_DLV_ERROR if fde is NULL or some other serious
    error.
    Returns DW_DLV_NO_ENTRY if there is no target-specific
    augmentation data.

    The bytes pointed to are in the Dwarf_Fde, and as long as that
    is valid the bytes are there. No 'dealloc' call is needed
    for the bytes.  */
int
dwarf_get_fde_augmentation_data(Dwarf_Fde fde,
    Dwarf_Small * *augdata,
    Dwarf_Unsigned * augdata_len,
    Dwarf_Error * error)
{
    Dwarf_Cie cie = 0;

    if (fde == NULL) {
        _dwarf_error(NULL, error, DW_DLE_FDE_NULL);
        return DW_DLV_ERROR;
    }
    if (!fde->fd_gnu_eh_aug_present) {
        return DW_DLV_NO_ENTRY;
    }
    cie = fde->fd_cie;
    if (cie == NULL) {
        _dwarf_error(NULL, error, DW_DLE_CIE_NULL);
        return DW_DLV_ERROR;
    }
    *augdata = (Dwarf_Small *) fde->fd_gnu_eh_augmentation_bytes;
    *augdata_len = fde->fd_gnu_eh_augmentation_len;
    return DW_DLV_OK;
}

#if 0  /* dump_frame_rule() FOR DEBUGGING */
/* Used solely for debugging libdwarf. */
static void
dump_frame_rule(char *msg, struct Dwarf_Reg_Rule_s *reg_rule)
{
    printf
        ("%s type %s (0x%" DW_PR_XZEROS DW_PR_DUx
        "), is_off %" DW_PR_DUu
        " reg %" DW_PR_DUu " offset 0x%" DW_PR_XZEROS DW_PR_DUx
        " blockp 0x%" DW_PR_XZEROS DW_PR_DUx "\n",
        msg,
        (reg_rule->ru_value_type == DW_EXPR_OFFSET) ?
            "DW_EXPR_OFFSET" :
        (reg_rule->ru_value_type == DW_EXPR_VAL_OFFSET) ?
            "DW_EXPR_VAL_OFFSET" :
        (reg_rule->ru_value_type == DW_EXPR_VAL_EXPRESSION) ?
            "DW_EXPR_VAL_EXPRESSION" :
        (reg_rule->ru_value_type == DW_EXPR_EXPRESSION) ?
            "DW_EXPR_EXPRESSION" : "Unknown",
        (Dwarf_Unsigned) reg_rule->ru_value_type,
        (Dwarf_Unsigned) reg_rule->ru_is_off,
        (Dwarf_Unsigned) reg_rule->ru_register,
        (Dwarf_Unsigned) reg_rule->ru_offset_or_block_len,
        (Dwarf_Unsigned) reg_rule->ru_block);
    return;
}
#endif /*0*/

/*  This allows consumers to set the 'initial value' so that
    an ISA/ABI specific default can be used, dynamically,
    at run time.  Useful for dwarfdump and non-MIPS architectures..
    The value  defaults to one of
        DW_FRAME_SAME_VALUE or DW_FRAME_UNKNOWN_VALUE
    but dwarfdump can dump multiple ISA/ABI objects so
    we may want to get this set to what the ABI says is correct.

    Returns the value that was present before we changed it here.  */
Dwarf_Half
dwarf_set_frame_rule_initial_value(Dwarf_Debug dbg,
    Dwarf_Half value)
{
    Dwarf_Half orig =
        (Dwarf_Half)dbg->de_frame_rule_initial_value;
    dbg->de_frame_rule_initial_value = value;
    return orig;
}

/*  This allows consumers to set the array size of the  reg rules
    table so that
    an ISA/ABI specific value can be used, dynamically,
    at run time.  Useful for non-MIPS architectures.
    The value  defaults  to DW_FRAME_LAST_REG_NUM.
    but dwarfdump can dump multiple ISA/ABI objects so
    consumers want to get this set to what the ABI says is correct.

    Returns the value that was present before we changed it here.
*/

Dwarf_Half
dwarf_set_frame_rule_table_size(Dwarf_Debug dbg, Dwarf_Half value)
{
    Dwarf_Half orig =
        (Dwarf_Half)dbg->de_frame_reg_rules_entry_count;
    dbg->de_frame_reg_rules_entry_count = value;

    /*  Take the caller-specified value, but do not
        let the value be too small. Keep it at least to
        DW_FRAME_LAST_REG_NUM.
        This helps prevent libdwarf (mistakenly) indexing outside
        of of a register array when the ABI reg count
        is really small.  */
    if (value < DW_FRAME_LAST_REG_NUM) {
        dbg->de_frame_reg_rules_entry_count = DW_FRAME_LAST_REG_NUM;
    }
    return orig;
}
/*  This allows consumers to set the CFA register value
    so that an ISA/ABI specific value can be used, dynamically,
    at run time.  Useful for non-MIPS architectures.
    The value  defaults  to DW_FRAME_CFA_COL3 and should be
    higher than any real register in the ABI.
    Dwarfdump can dump multiple ISA/ABI objects so
    consumers want to get this set to what the ABI says is correct.

    Returns the value that was present before we changed it here.  */

Dwarf_Half
dwarf_set_frame_cfa_value(Dwarf_Debug dbg, Dwarf_Half value)
{
    Dwarf_Half orig = (Dwarf_Half)dbg->de_frame_cfa_col_number;
    dbg->de_frame_cfa_col_number = value;
    return orig;
}
/* Similar to above, but for the other crucial fields for frames. */
Dwarf_Half
dwarf_set_frame_same_value(Dwarf_Debug dbg, Dwarf_Half value)
{
    Dwarf_Half orig =
        (Dwarf_Half)dbg->de_frame_same_value_number;
    dbg->de_frame_same_value_number = value;
    return orig;
}
Dwarf_Half
dwarf_set_frame_undefined_value(Dwarf_Debug dbg, Dwarf_Half value)
{
    Dwarf_Half orig =
        (Dwarf_Half)dbg->de_frame_same_value_number;
    dbg->de_frame_undefined_value_number = value;
    return orig;
}

/*  Does something only if value passed in is greater than 0 and
    a size than we can handle (in number of bytes).  */
Dwarf_Small
dwarf_set_default_address_size(Dwarf_Debug dbg,
    Dwarf_Small value  )
{
    Dwarf_Small orig = dbg->de_pointer_size;
    if (value > 0 && value <= sizeof(Dwarf_Addr)) {
        dbg->de_pointer_size = value;
    }
    return orig;
}

static int
init_reg_rules_alloc(Dwarf_Debug dbg,struct Dwarf_Frame_s *f,
    Dwarf_Unsigned count, Dwarf_Error * error)
{
    f->fr_reg_count = count;
    f->fr_reg = (struct Dwarf_Reg_Rule_s *)
        calloc((size_t)count, sizeof(struct Dwarf_Reg_Rule_s));
    if (f->fr_reg == 0) {
        if (error) {
            _dwarf_error(dbg, error, DW_DLE_DF_ALLOC_FAIL);
        }
        return DW_DLV_ERROR;
    }
    _dwarf_init_reg_rules_ru(f->fr_reg,0, count,
        dbg->de_frame_rule_initial_value);
    return DW_DLV_OK;
}
static int
_dwarf_initialize_fde_table(Dwarf_Debug dbg,
    struct Dwarf_Frame_s *fde_table,
    Dwarf_Unsigned table_real_data_size,
    Dwarf_Error * error)
{
    unsigned entry_size = sizeof(struct Dwarf_Frame_s);
    memset(fde_table,0,entry_size);
    fde_table->fr_loc = 0;
    fde_table->fr_next = 0;

    return init_reg_rules_alloc(dbg,fde_table,
        table_real_data_size,error);
}
static void
_dwarf_free_fde_table(struct Dwarf_Frame_s *fde_table)
{
    free(fde_table->fr_reg);
    fde_table->fr_reg_count = 0;
    fde_table->fr_reg = 0;
}

/*  Return DW_DLV_OK if we succeed. else return DW_DLV_ERROR.
*/
int
_dwarf_frame_constructor(Dwarf_Debug dbg, void *frame)
{
    struct Dwarf_Frame_s *fp = frame;

    if (IS_INVALID_DBG(dbg)) {
        return DW_DLV_ERROR;
    }
    return init_reg_rules_alloc(dbg,fp,
        dbg->de_frame_reg_rules_entry_count, 0);
}

void
_dwarf_frame_destructor(void *frame)
{
    struct Dwarf_Frame_s *fp = frame;
    _dwarf_free_fde_table(fp);
}

void
_dwarf_fde_destructor(void *f)
{
    struct Dwarf_Fde_s *fde = f;

    if (fde->fd_fde_owns_cie) {
        Dwarf_Debug dbg = fde->fd_dbg;

        if (!dbg->de_in_tdestroy) {
            /*  This is just for dwarf_get_fde_for_die() and
                must not be applied in alloc tree destruction. */
            dwarf_dealloc(fde->fd_dbg,fde->fd_cie,DW_DLA_CIE);
            fde->fd_cie = 0;
        }
    }
    if (fde->fd_have_fde_tab) {
        _dwarf_free_fde_table(&fde->fd_fde_table);
        fde->fd_have_fde_tab = FALSE;
    }
}
void
_dwarf_frame_instr_destructor(void *f)
{
    Dwarf_Frame_Instr_Head head = f;
    Dwarf_Debug dbg = head->fh_dbg;
    Dwarf_Unsigned count = head->fh_array_count;
    Dwarf_Unsigned i = 0;

    for ( ; i < count ; ++i) {
        free(head->fh_array[i]);
        head->fh_array[i] = 0;
    }
    dwarf_dealloc(dbg,head->fh_array,DW_DLA_LIST);
    head->fh_array = 0;
    head->fh_array_count = 0;
}
void
dwarf_dealloc_frame_instr_head(Dwarf_Frame_Instr_Head h)
{
    if (!h) {
        return;
    }
    dwarf_dealloc(h->fh_dbg,h,DW_DLA_FRAME_INSTR_HEAD);
}

static void
_dwarf_init_reg_rules_ru(struct Dwarf_Reg_Rule_s *base,
    Dwarf_Unsigned first, Dwarf_Unsigned last,
    Dwarf_Unsigned initial_value)
{
    struct Dwarf_Reg_Rule_s *r = base+first;
    Dwarf_Unsigned i = first;
    for (; i < last; ++i,++r) {
        r->ru_is_offset = 0;
        r->ru_value_type = DW_EXPR_OFFSET;
        r->ru_register = (Dwarf_Unsigned)initial_value;
        r->ru_offset = 0;
        r->ru_args_size = 0;
        r->ru_block.bl_data = 0;
        r->ru_block.bl_len = 0;
    }
}

/* For any remaining columns after what fde has. */
static void
_dwarf_init_reg_rules_dw3(
    Dwarf_Regtable_Entry3_i *base,
    Dwarf_Unsigned first, Dwarf_Unsigned last,
    Dwarf_Unsigned initial_value)
{
    Dwarf_Regtable_Entry3_i *r = base+first;
    Dwarf_Unsigned i = first;
    for (; i < last; ++i,++r) {
        r->dw_offset_relevant = 0;
        r->dw_value_type = DW_EXPR_OFFSET;
        r->dw_regnum = initial_value;
        r->dw_offset = 0;
        r->dw_args_size = 0;
        r->dw_block.bl_data = 0;
        r->dw_block.bl_len = 0;
    }
}
