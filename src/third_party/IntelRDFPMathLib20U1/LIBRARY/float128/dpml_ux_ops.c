/******************************************************************************
  Copyright (c) 2007-2011, Intel Corp.
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of Intel Corporation nor the names of its contributors
      may be used to endorse or promote products derived from this software
      without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
  THE POSSIBILITY OF SUCH DAMAGE.
******************************************************************************/

#include <stdio.h>
/* File: dpml_ux_ops.c */
/*
**  Facility:
**
**	DPML
**
**  Abstract:
**
** 	This file contains source code for the basic operations used in the
**	unpacked x-float library that are independent of word size: pack,
**	unpack, addsub and normalize
**
**  Modification History:
**
**	1-001   Version 1.  RNH 01-Sep-95
**	1-002   Made PACK and UNPACK take error information.  RNH 17-Sep-95
**	1-003   Adding missing return value in UNPACK2.  RNH 09-May-98
**	1-004	Modified FFS_AND_SHIFT and PACK to respect signed zeros;
**		Changed the representation of the default connonical NaN
**		RNH 29-Jun-07
**
*/

#include "dpml_ux.h"

/* Pick up packed constant table */

#undef  INSTANTIATE_TABLE
#undef  INSTANTIATE_DEFINES
#define INSTANTIATE_TABLE	1
#define INSTANTIATE_DEFINES	0

#include STR(DPML_UX_CONS_FILE_NAME)

/*
** The FFS_AND_SHIFT routine finds the most significant non-zero bit in an
** UX_FLOAT value and aligns it with the MSB of a normalized UX_FLOAT value.
** The flags argument controls the interpretation of the input argument.
** If flags is one of FFS_CVT_WORD or FFS_CVT_U_WORD, then the high fraction
** digit is assumed to be signed of unsigned word and all other fields are
** assumed to be undefined.
*/

WORD
FFS_AND_SHIFT ( UX_FLOAT * argument, U_WORD flags)
    {
    WORD shift, cshift, num_digits, cnt;
    UX_SIGN_TYPE sign;
    UX_EXPONENT_TYPE exponent;
    UX_FRACTION_DIGIT_TYPE msd, lsd OTHER_DIGITS;
    D_UNION u;

    msd = G_UX_MSD(argument);

    if (FFS_NORMALIZE == flags)
        { /* Do a quick check for a normalized argument */
        exponent = G_UX_EXPONENT(argument);
        if ((UX_SIGNED_FRACTION_DIGIT_TYPE) msd < 0)
            return 0;
        }
    else
        {
        sign = 0;
        exponent = BITS_PER_UX_FRACTION_DIGIT_TYPE;

        if ((FFS_CVT_WORD == flags) &&
          ((UX_SIGNED_FRACTION_DIGIT_TYPE) msd < 0))
            {
            sign = UX_SIGN_BIT;
            msd = -msd;
            }
        P_UX_MSD(argument, msd);
        CLR_UX_LOW_FRACTION(argument);
        P_UX_SIGN(argument, sign);
        }

    lsd = G_UX_LSD(argument);
    G_UX_OTHER_DIGITS(argument);
    num_digits = NUM_UX_FRACTION_DIGITS;

    cnt = 0;
    do {
       if (msd)
           goto find_shift;
       DIGIT_SHIFT_FRACTION_LEFT(lsd, msd);
       cnt += BITS_PER_UX_FRACTION_DIGIT_TYPE;
       } while (--num_digits);

    /* 
    ** If we get here, we had a zero fraction.  Set the exponent field
    ** accordingly, force the sign to positive and return.
    */

    P_UX_EXPONENT(argument, UX_ZERO_EXPONENT);
    P_UX_SIGN(argument, 0);
    return cnt;

find_shift:

    /* Quick check to see if its already normalized */

    if ((UX_SIGNED_FRACTION_DIGIT_TYPE) msd >= 0)
        { /* The high bit is not set, see if any of the next four are set */

        shift = (msd >> (BITS_PER_UX_FRACTION_DIGIT_TYPE - 6)) & 0x1e;
        if (shift)
            /* Figure out which bit is set by "table look-up" */
            shift = ((((3 << 2*1) |
                       (2 << 2*2) |
                       (2 << 2*3) |
                       (1 << 2*4) |
                       (1 << 2*5) |
                       (1 << 2*6) |
                       (1 << 2*7) ) >> shift) & 0x3) + 1;
        else
            /*
            ** Get shift by converting to floating point and extracting the
            ** then exponent field.  In the 64 bit case, make sure there is
            ** rounding on the convert.
            */
            {
            if (BITS_PER_UX_FRACTION_DIGIT_TYPE == 32)
                 u.f = (double) msd;
            else
                {
                UX_FRACTION_DIGIT_TYPE itmp;

                itmp = msd & ~0xff;
                itmp = itmp ? itmp : msd;
                u.f = (double) itmp;
                }
            shift = (BITS_PER_UX_FRACTION_DIGIT_TYPE - 1 + D_EXP_BIAS - D_NORM)
                  - (u.D_SIGNED_HI_WORD >> D_EXP_POS) ;
            }
        cshift = BITS_PER_UX_FRACTION_DIGIT_TYPE - shift;
        BIT_SHIFT_FRACTION_LEFT(lsd, msd, shift, cshift);
        cnt += shift;
        }

    P_UX_MSD(argument, msd);
    P_UX_OTHER_DIGITS(argument);
    P_UX_LSD(argument, lsd);
    P_UX_EXPONENT(argument, exponent - cnt);
    return cnt;
    }

/*
** The ADDSUB routine add and/or subtracts two unpacked x-float values.
** The logic to determine the larger value is driven by the exponent fields
** only, so that it may be necessary to explicitly normalize the operands
** prior to calling ADDSUB.  The flags argument allow for producing the
** sum, difference or both for signed or unsigned values
*/

#define DO_NORMALIZATION	(2*NO_NORMALIZATION)

void
ADDSUB ( UX_FLOAT * x, UX_FLOAT *y, U_WORD flags, UX_FLOAT * result)
    {
    WORD shift, cshift, cnt, op, tmp1, tmp2;
    UX_FLOAT * ux_tmp, ux_save;
    UX_SIGN_TYPE sign;
    UX_EXPONENT_TYPE exponent;
    UX_FRACTION_DIGIT_TYPE msd, lsd, tmp_digit, carry OTHER_DIGITS;

    /*
    ** See if we are doing an implicit addition or subtraction.  This logic
    ** depends upon ADD = 0 and SUB = 1
    */

#   if (ADD != 0) || (SUB != 1)
#       error "Must have ADD = 0 and SUB = 1"
#   endif

    sign = G_UX_SIGN(x);
    op = flags << (BITS_PER_UX_SIGN_TYPE - 1);
    tmp1 = (op^sign)^G_UX_SIGN(y);
    tmp2 = flags & MAGNITUDE_ONLY;
    sign = tmp2 ? 0 : sign;
    op   = tmp2 ? op : tmp1;
    op   = (op >> (BITS_PER_UX_SIGN_TYPE - 1)) & 1;


    /*
    ** Determine larger value, call it x and the smaller y.  In the process
    ** keep track of whether or not a swap takes place so that we can get
    ** the correct sign of the second result on a combined operation
    */

    exponent = G_UX_EXPONENT(x);
    shift = exponent - G_UX_EXPONENT(y);
    P_UX_SIGN(&ux_save, 0);
    if (shift < 0)
        {
        ux_tmp = x;
        x = y;
        y = ux_tmp;
        shift = -shift;
        exponent += shift;
        P_UX_SIGN(&ux_save, UX_SIGN_BIT);
        sign ^= ((op == ADD) ? 0 : UX_SIGN_BIT);
        }

    /* Now align digits of the smaller value */

    lsd = G_UX_LSD(y);
    G_UX_OTHER_DIGITS(y);
    msd = G_UX_MSD(y);

    cnt = NUM_UX_FRACTION_DIGITS;
    do  {
        cshift = BITS_PER_UX_FRACTION_DIGIT_TYPE - shift;
        if (cshift > 0)
            goto bit_shift;
        DIGIT_SHIFT_FRACTION_RIGHT(lsd, msd);
        shift = -cshift;
        } while (--cnt);

    /*
    ** If we get here, there was a *VERY* big alignment shift, so we
    ** copy the answer to the result
    */

    UX_COPY(x, result);
    P_UX_SIGN(result, sign);
 
    if ((flags & 0x2))
        {
        result++;
        UX_COPY(x, result);
        P_UX_SIGN(result, sign ^ G_UX_SIGN(&ux_save));
        }

    return;

bit_shift:

    if (shift)
        BIT_SHIFT_FRACTION_RIGHT(lsd, msd, shift, cshift);

    /*
    ** Save shifted value in case we are dealing with an ADD_SUB op
    */

    P_UX_MSD(&ux_save, msd);
    P_UX_LSD(&ux_save, lsd);
    P_UX_OTHER_DIGITS(&ux_save);

    /*
    ** Now do the operation.  The purpose of the do-loop is to ease processing
    ** of the ADD_SUB and SUB_ADD cases.
    */

    do  {

        tmp_digit = G_UX_LSD(x);

        if (op == ADD)
            {

            /*
            ** Addition code.  Turn off normalization
            */

            flags &= (DO_NORMALIZATION - 1);

            lsd += tmp_digit;
            carry = (lsd < tmp_digit);

#       if NUM_UX_FRACTION_DIGITS == 4

                tmp_digit = G_UX_FRACTION_DIGIT(x, 2);
                _F2 += + carry;
                carry = (_F2 < carry);
                _F2 += tmp_digit;
                carry += (_F2 < tmp_digit);

                tmp_digit = G_UX_FRACTION_DIGIT(x, 1);
                _F1 += carry;
                carry = (_F1 < carry);
                _F1 += tmp_digit;
                carry += (_F1 < tmp_digit);

#       endif

            tmp_digit = G_UX_MSD(x);
            msd += carry;
            carry = (msd < carry);
            msd += tmp_digit;
            carry += (msd < tmp_digit);

            /* If carry is set, we need to normalizes fraction field */

            if (carry)
                {
                BIT_SHIFT_FRACTION_RIGHT(lsd, msd, 1,
                  BITS_PER_UX_FRACTION_DIGIT_TYPE - 1);
                msd |= UX_MSB;
                exponent++;
                }
            }

        else
            {

            /*
            ** Subtraction code.  Set normalization flag
            */

            flags -= NO_NORMALIZATION;

            carry = (lsd > tmp_digit);
            lsd = tmp_digit - lsd;

#       if NUM_UX_FRACTION_DIGITS == 4

                tmp_digit = G_UX_FRACTION_DIGIT(x, 2);
                _F2 += carry;
                carry = (_F2 < carry);
                _F2 = tmp_digit - _F2;
                carry += (tmp_digit < _F2);

                tmp_digit = G_UX_FRACTION_DIGIT(x, 1);
                _F1 += carry;
                carry = (_F1 < carry);
                _F1 = tmp_digit - _F1;
                carry += (tmp_digit < _F1);

#       endif

            tmp_digit = G_UX_MSD(x);
            msd += carry;
            carry = (msd < carry);
            msd = tmp_digit - msd;
            carry += (tmp_digit < msd);

            /*
            ** If carry is set, we guessed wrong about which was larger so
            ** negate the result
            */

            if (carry)
                {
                sign ^= UX_SIGN_BIT;
                P_UX_SIGN(&ux_save, UX_SIGN_BIT);
                lsd = -lsd;
                carry = (lsd == 0) ? 0 : -1;

#           if NUM_UX_FRACTION_DIGITS == 4

                    _F2 = carry - F2;
                    carry = (_F2 == 0) ? carry : -1;

                    _F3 = carry - F3;
                    carry = (_F3 == 0) ? carry : -1;

#           endif
                    
                msd = carry - msd;
                }

            }

        P_UX_MSD(result, msd);
        P_UX_LSD(result, lsd);
        P_UX_OTHER_DIGITS(result);
        P_UX_EXPONENT(result, exponent);
        P_UX_SIGN(result, sign);


        if (flags & DO_NORMALIZATION)
            NORMALIZE(result);

        if (0 == (flags & 0x2))
            /* Single op.  Quit now */
            break;

        /* This is a dual op.  Do the second part */

        op = 1 - op;
        flags ^= 0x2; 
        result ++;
        msd = G_UX_MSD(&ux_save);
        lsd = G_UX_LSD(&ux_save);
        G_UX_OTHER_DIGITS(&ux_save);
        sign ^= G_UX_SIGN(&ux_save);
        exponent = G_UX_EXPONENT(x);

        } while (1);
    }



/*
** UNPACK_X_OR_Y unpacks one of two x-float arguments and handles any
** special FP classes based on the class_to_action_map.  If the second
** argument, y, is non-zero, it is unpacked, otherwise the first argument is
** unpacked.
*/

#define SHIFT	F_EXP_WIDTH
#define CSHIFT	(BITS_PER_UX_FRACTION_DIGIT_TYPE - F_EXP_WIDTH)

/*
** The following macro changes/definitions are required to get the standard
** exception dispatcher interface macros to work
*/

#undef  F_TYPE
#define F_TYPE	_X_FLOAT

#undef  P_EXCPTN_VALUE_x
#define P_EXCPTN_VALUE_x(x,v)	x.ld = *((F_TYPE *) v)

#define TYPE_MASK		MAKE_MASK(TYPE_WIDTH-1, TYPE_POS)
#undef  ADD_ERR_CODE_TYPE
#define ADD_ERR_CODE_TYPE(e)	(((F_TYPE_ENUM << TYPE_POS) & TYPE_MASK) \
				  | ((e) & (~TYPE_MASK)))

WORD
UNPACK_X_OR_Y(
  _X_FLOAT     * packed_x,
  _X_FLOAT     * packed_y,
  UX_FLOAT     * unpacked_argument,
  U_WORD const * class_to_action_map,
  _X_FLOAT     * packed_result
  OPT_EXCEPTION_INFO_DECLARATION)
    {
    WORD fp_class, sign, action, disp, index, action_index, map_element,
      shift, index_limit;
    UX_FRACTION_DIGIT_TYPE exponent_digit, cur_digit, next_digit, inf_nan,
      zero_denorm, fract_bits, * digit_ptr;
    UX_EXPONENT_TYPE exponent;
    _X_FLOAT  * packed_value, * packed_argument, *tmp_ptr;
    EXCEPTION_RECORD_DECLARATION

#   if (BITS_PER_WORD == 32)
         WORD tmp;
#   endif

    /*
    ** Start unpacking the argument by fetching the exponent word and
    ** decomposing it into its sign, exponent and high fraction bit components
    */

    index_limit = (packed_y != 0);
    packed_argument = index_limit ? packed_y : packed_x;
    IF_OPTNL_ERROR_INFO( EXCPTN_INFO->args[index_limit] = packed_argument);
    exponent_digit = G_X_DIGIT( packed_argument, 0);
    cur_digit = UX_MSB;

    P_UX_SIGN(unpacked_argument, (exponent_digit & cur_digit) >>
      (BITS_PER_UX_FRACTION_DIGIT_TYPE - BITS_PER_UX_SIGN_TYPE));

    P_UX_EXPONENT(unpacked_argument,
      ((exponent_digit >> F_EXP_POS) & MAKE_MASK( F_EXP_WIDTH, 0))
      - F_EXP_BIAS + 1);

    cur_digit |= (exponent_digit << SHIFT);

    /*
    ** Now get the remaining fraction bits, align them, and put them into
    ** the unpacked argument.  While we're fetching the fraction bits, generate
    ** the logical 'or' of all of them to be used for the classification
    ** logic later on.
    */

    next_digit = G_X_DIGIT( packed_argument, 1);
    fract_bits = cur_digit + cur_digit;
    cur_digit |= (next_digit >> CSHIFT);
    P_UX_MSD( unpacked_argument, cur_digit);
    cur_digit = next_digit << SHIFT;
    fract_bits |= next_digit;

#   if (BITS_PER_WORD == 32)

        next_digit = G_X_DIGIT( packed_argument, 2);
        cur_digit |= (next_digit >> CSHIFT);
        P_UX_FRACTION_DIGIT( unpacked_argument, 1, cur_digit);
        cur_digit = next_digit << SHIFT;
        fract_bits |= next_digit;

        next_digit = G_X_DIGIT( packed_argument, 3);
        cur_digit |= (next_digit >> CSHIFT);
        P_UX_FRACTION_DIGIT( unpacked_argument, 2, cur_digit);
        cur_digit = next_digit << SHIFT;
        fract_bits |= next_digit;

#   endif

    P_UX_LSD( unpacked_argument, cur_digit);

    /*
    ** We've unpacked the argument, now start the classification process
    */

    zero_denorm = exponent_digit - F_HIDDEN_BIT_MASK;
    inf_nan     = exponent_digit + F_HIDDEN_BIT_MASK;
    sign = exponent_digit >> F_SIGN_BIT_POS;
    fp_class = F_C_POS_NORM;

    if ((WORD) (inf_nan ^ zero_denorm) < 0 )
        { /* Input argument is +/-0, +/-denorm, +/- Infinity, [SQ]NaN */
        if ((WORD) (zero_denorm ^ exponent_digit) < 0) 
            { /* argument was +/- zero or +/- denorm */
            if (!fract_bits)
                fp_class = F_C_POS_ZERO;
            else
                { /* denorm, undo hidden bit, adjust exponent and normalize */
                P_UX_MSD(unpacked_argument,
                   G_UX_MSD(unpacked_argument) - UX_MSB);
                UX_INCR_EXPONENT(unpacked_argument, 1);
                NORMALIZE(unpacked_argument);
	        fp_class = F_C_POS_DENORM;
                }
            }
        else
            { /* argument was +/- Inf or [SQ]NaN */
            if (!fract_bits)
                fp_class = F_C_POS_INF;
            else
                { /* NaN */
                fp_class = F_C_SIG_NAN;
                sign = exponent_digit >> (F_EXP_POS - 1) & 1;
                }
            }
        }

    fp_class += sign;
    IF_OPTNL_ERROR_INFO( EXCPTN_INFO->arg_classes = (1 << fp_class));

    /* Now get the class to action mapping index and action */

    shift = fp_class*(INDEX_WIDTH + ACTION_WIDTH);

#   if (BITS_PER_WORD == 64)

        map_element = class_to_action_map[0];
        action_index = map_element >> shift;
        disp = map_element >> 60;

#   else

        map_element = class_to_action_map[0];
        tmp         = class_to_action_map[1];
        if (fp_class < F_C_NEG_NORM)
            action_index = map_element >> shift;
        else
            action_index = map_element >>
             (shift - F_C_NEG_NORM*(INDEX_WIDTH + ACTION_WIDTH));
        disp = ((map_element >> 31) & 0x6) |
                      ((tmp >> 29) & 0x18);

#   endif

    index = action_index & INDEX_MASK;
    action = (action_index >> ACTION_POS) & INDEX_MASK;

    /* Leave now if all we have to do is unpack the argument */

    if (action == RETURN_UNPACKED)
        return fp_class;
//printf("UNPACK %llx, %llx\n", (long long)fp_class, index);

	/*
    ** If index is not 0 or 1, then the base return value is in the class to
    ** action mapping table.  Otherwise, the base value is the input argument
    ** or the auxiliary argument.
    */

    if (index <= index_limit)
	{
        digit_ptr = (UX_FRACTION_DIGIT_TYPE *) (index == 0 ?
          packed_x : packed_y);
	}
    else
	{
        index = WORDS_PER_CLASS_TO_ACTION_MAP*(disp & 0xf) + index - 1;
        index = class_to_action_map[ index ];
        digit_ptr = (UX_FRACTION_DIGIT_TYPE *)
                        & ((_X_FLOAT *) PACKED_CONSTANT_TABLE)[index];
 //printf("UNPACK 3 %llx, %llx d= %llx, %llx\n", (long long)fp_class, index, digit_ptr[0],digit_ptr[1]);
	}

    /*
    ** If this is an error action, process the exception and get the final
    ** return value from the exception handler.  Otherwise, manipulate
    ** the base value to get the final return value.
    */

    if (action == RETURN_ERROR)
        {
        index = ADD_ERR_CODE_TYPE(index);
        GET_EXCEPTION_RESULT_2(index, packed_x, packed_y, *packed_result);
        }
    else
        {
        exponent_digit = G_X_DIGIT(digit_ptr, MSD_NUM);
        switch (action)
            {
            case RETURN_QUIET_NAN:
                // exponent_digit &= (~SET_BIT(F_EXP_POS - 1));
                exponent_digit |= (SET_BIT(F_EXP_POS - 1));
                break;

            case RETURN_NEGATIVE:
                exponent_digit ^= (F_SIGN_BIT_MASK);
                break;
 
            case RETURN_ABSOLUTE:
                exponent_digit &= (~F_SIGN_BIT_MASK);
                break;
 
            case RETURN_CPYSN_ARG_0:
                exponent_digit = (exponent_digit & (~F_SIGN_BIT_MASK)) |
                   (G_X_DIGIT(packed_x, MSD_NUM) & F_SIGN_BIT_MASK);
                break;
 
            case RETURN_VALUE:
            default:
               break;
            }

        /* Copy the final result to the packed result and return */

        P_X_DIGIT(packed_result, MSD_NUM, exponent_digit);

#       if BITS_PER_WORD == 32

            P_X_DIGIT(packed_result, 1, G_X_DIGIT(digit_ptr, 1));
            P_X_DIGIT(packed_result, 2, G_X_DIGIT(digit_ptr, 2));

#       endif

        P_X_DIGIT(packed_result, LSD_NUM, G_X_DIGIT(digit_ptr, LSD_NUM));
        }
    return fp_class | ((WORD) 1 << (BITS_PER_WORD - 1));
    }

/*
** UNPACK2 is an interface layer that deals with processing the input
** arguments for 2 argument functions.  Basicly, this routine call 
** UNPACK_X_OR_Y twice and processes the more complicated class_to_action
** mappings associated with two argument functions.
*/

WORD
UNPACK2(
  _X_FLOAT     * packed_x,
  _X_FLOAT     * packed_y,
  UX_FLOAT     * unpacked_x,
  UX_FLOAT     * unpacked_y,
  U_WORD const * class_to_action_map,
  _X_FLOAT     * packed_result
  OPT_EXCEPTION_INFO_DECLARATION)
    {
    WORD fp_class_x, fp_class_y, disp, shift;
    IF_OPTNL_ERROR_INFO( U_WORD arg_classes; )

    
    fp_class_x = UNPACK(
        packed_x,
        unpacked_x,
        class_to_action_map,
        packed_result
        OPT_EXCEPTION_INFO_ARGUMENT );

    if (0 > fp_class_x)
        return fp_class_x;

    /*
    ** Check for NULL second argument.  This allows for UNPACK2 to
    ** process single arguments if it has to
    */

    if ( ! packed_y )
        return fp_class_x;

    shift = F_C_CLASS_BIT_WIDTH*fp_class_x;

#   if (BITS_PER_WORD == 64)

        disp = (U_WORD) class_to_action_map[1];

#   else 

        if (fp_class_x < (BITS_PER_WORD/F_C_CLASS_BIT_WIDTH))
            disp = (U_WORD) class_to_action_map[2];
        else
            {
            disp = (U_WORD) class_to_action_map[3];
            shift -= (BITS_PER_WORD/F_C_CLASS_BIT_WIDTH);
            }
#   endif

    disp = (disp >> (shift - 3)) & MAKE_MASK(F_C_CLASS_BIT_WIDTH, 3);
    IF_OPTNL_ERROR_INFO( arg_classes = EXCPTN_INFO->arg_classes );

    fp_class_y = UNPACK_X_OR_Y(
        packed_x,
        packed_y,
        unpacked_y,
        (U_WORD *) ((char *) class_to_action_map + disp),
        packed_result
        OPT_EXCEPTION_INFO_ARGUMENT);

    IF_OPTNL_ERROR_INFO( EXCPTN_INFO->arg_classes |= arg_classes );
    return fp_class_y | (fp_class_x << F_C_CLASS_BIT_WIDTH);
    }

/*
** The PACK routine converts unpacked x-float arguments to packed and deals
** with any overflow, underflow or denorm conditions that might result.
*/

void
PACK (
  UX_FLOAT * unpacked_result,
  _X_FLOAT * packed_result,
  U_WORD     underflow_error,
  U_WORD     overflow_error
  OPT_EXCEPTION_INFO_DECLARATION )
    {
    WORD shift, error_code;
    UX_FLOAT tmp;
    _X_FLOAT * x_ptr;
    UX_EXPONENT_TYPE exponent;
    UX_FRACTION_DIGIT_TYPE incr, tmp_digit, next_digit, current_digit,
      * error_val_ptr;
    EXCEPTION_RECORD_DECLARATION

    /*
    ** Start by "normalizing" any denormal results.  Also screen out any
    ** (encoded) zeros, since they screw up the rest of the logic
    */

    NORMALIZE(unpacked_result);
    exponent = G_UX_EXPONENT(unpacked_result);

    if (exponent == UX_ZERO_EXPONENT)
        {
        next_digit = G_UX_SIGN(unpacked_result);

#       if (NUM_UX_DIGITS == 4)

            packed_result->digit[2] = 0; 
            packed_result->digit[3] = 0; 

#       else

            next_digit <<= UX_SIGN_SHIFT;

#       endif

        //packed_result->digit[1] = 0;
        // packed_result->digit[0] = 0;
        P_X_DIGIT(packed_result, LSD_NUM, 0);
        P_X_DIGIT(packed_result, 0, next_digit);

        return;
        }

    shift = (F_MIN_BIN_EXP + 1) - exponent;
    if (shift > 0)
        {
        SET_UX_FRACTION_TO_HALF(&tmp);
        P_UX_EXPONENT(&tmp, exponent + shift);
        P_UX_SIGN(&tmp, G_UX_SIGN(unpacked_result));
        ADDSUB(&tmp, unpacked_result, ADD, unpacked_result);

        /*
        ** We need to distinguish between zero, denoms and underflow here.
        ** In all cases, the fraction field will be correct.  However, we
        ** need to adjust the exponent value to get the right exponent field.
        */

        exponent = 1-F_EXP_BIAS;
        if ((shift > F_PRECISION) &&
          (shift != -(UX_ZERO_EXPONENT - F_MIN_BIN_EXP - 1)))
            /* Force underflow */
            exponent--;
        }

    /* Now round result and shift right */

    incr = (UX_FRACTION_DIGIT_TYPE) 1 << (F_EXP_WIDTH - 1);

    tmp_digit = G_UX_LSD(unpacked_result);
    current_digit = tmp_digit + incr;
    incr = (current_digit < incr);
    current_digit >>= SHIFT;

    tmp_digit = G_UX_2nd_LSD(unpacked_result);
    next_digit = tmp_digit + incr;
    incr = (next_digit < incr);
    current_digit |= (next_digit << CSHIFT); 
    P_X_DIGIT(packed_result, LSD_NUM, current_digit);
    current_digit = (next_digit >> SHIFT);


#   if (NUM_UX_DIGITS == 4)

        tmp_digit = G_UX_FRACTION_DIGIT(unpacked_result, 1);
        next_digit = tmp_digit + incr;
        incr = (next_digit < incr);
        current_digit |= (next_digit << CSHIFT); 
        P_X_DIGIT(packed_result, 2, current_digit);
        current_digit = (next_digit >> SHIFT);

        tmp_digit = G_UX_FRACTION_DIGIT(unpacked_result, 0);
        next_digit = tmp_digit + incr;
        incr = (next_digit < incr);
        current_digit |= (next_digit << CSHIFT); 
        P_X_DIGIT(packed_result, 1, current_digit);
        current_digit = (next_digit >> SHIFT);

#   endif

    /*
    ** At this point, all of the fraction bits except the most significant
    ** have been written to the destination.  Current_digit holds the most
    ** significant fraction (correctly aligned) and incr = 1 iff the rounded
    ** fraction is 1.
    */

    if (incr)
        {
        exponent++;
        current_digit = (UX_MSB >> SHIFT);
        }

    /* Finish packing and check for overflow and underflow */
    /*
    ** Pack sign and exponent.  Be careful to convert to UX_FRACTION_DIGIT_TYPE
    ** first.  Adjust exponent to reflect hidden bit in fraction field
    */

    tmp_digit = exponent + ((F_EXP_BIAS - 1) - 1);
    current_digit += (tmp_digit << (CSHIFT - 1));
    next_digit = G_UX_SIGN(unpacked_result);
    current_digit |= (next_digit << UX_SIGN_SHIFT);
    P_X_DIGIT(packed_result, 0, current_digit);

    /* If no overflow or underflow, we're done */

    if ( tmp_digit < (MAKE_MASK(F_EXP_WIDTH, 0) - 1) )
        /* OK, no overflow or underflow.  */
        return;

    /* Check for denorm and overflow/underflow processing */
    
    if ( ++tmp_digit == 0 )
        {

#       define IEEE_SPECIAL_ENCODING_MASK	( (1 << F_C_QUIET_NAN)	\
						| (1 << F_C_SIG_NAN)	\
						| (1 << F_C_POS_INF)	\
						| (1 << F_C_NEG_INF)	\
						| (1 << F_C_POS_DENORM)	\
						| (1 << F_C_NEG_DENORM)	)
#       define INPUT_WAS_IEEE_SPECIAL_ENCODING	\
		((EXCPTN_INFO->arg_classes & IEEE_SPECIAL_ENCODING_MASK) != 0)


        if ( 
          IF_OPTNL_ERROR_INFO( INPUT_WAS_IEEE_SPECIAL_ENCODING || )
          PROCESS_DENORMS )
             return;
        }

    error_code = (exponent < 0) ? underflow_error : overflow_error;
    error_code = ADD_ERR_CODE_TYPE(error_code);
    GET_EXCEPTION_RESULT_2(error_code, packed_x, packed_y, *packed_result);
    }



/*
** For very ill behaved polynomial evaluations, we introduce a "packed" form
** of the coefficients to be used by a less efficient evaluation routine
** that unpacks the coefficients and evaluates the polynomial via Horner's
** scheme by calling the add/sub and multiply routines.  
**
** The special format used looks like:
**
**	|<----------------------- 128 bits -------------------->|
**	+---------------------------------------------------+---+-+
**	|          Normalized Fraction                      | K |s|
**	+---------------------------------------------------+---+-+
**	                                                 -->| w |<--
**
** where K is a biased scale field of w-bits and s is the sign bit.  The
** width of the biased scale factor, w, and the actual bias depends on the
** coefficients.  In general it is 4 bits or less.
**
** In a Horner's scheme evaluation of degree n, involving the coefficients
** c(k) and an argument x, the basic iteration (ignoring the signs) is of
** the form:
**
**		T(n) <-- c(n)
**		T(k) <-- c(k) + x*T(k+1)	for k = n-1, n-2, ..., 1, 0
**
** If we consider the binary exponent and fraction fields of the c(k)'s
** separately we can write c[k] = 2^e(k)*f(k), where 1/2 <= f(k) < 1.  We
** now define the k-th scale factor s(k) as
**
**			s(0) = e(0)
**			s(k) = e(k-1) - e(k)	for k = 1, 2, ..., n
**
** Then consider the recursion:
**
**	t(n) <-- 2^s(n)*f(n)
**	t(k) <-- 2^s(k)*[f(k) + x*t(k+1)]	for k = n-1, n-2, ..., 1, 0
**
** It can be shown by induction that t(k) = T(k)/2^e(k) for k >= 1 and that
** t(0) = T(0).
*/

#if NUM_UX_FRACTION_DIGITS == 2
#    define COPY_MIDDLE_DIGITS(coef, ux)
#else
#    define COPY_MIDDLE_DIGITS(coef, ux)			\
		P_UX_FRACTION_DIGIT(ux, 2, coef->digits[1]);	\
		P_UX_FRACTION_DIGIT(ux, 1, coef->digits[2])
#endif

#define UNPACK_COEF_TO_UX(coef, ux, mask, bias, scale, op)		\
		{							\
		UX_FRACTION_DIGIT_TYPE lsd;				\
									\
		P_UX_FRACTION_DIGIT(ux, 0, coef->digits[LSD_NUM]);	\
		COPY_MIDDLE_DIGITS(coef, ux);				\
		lsd = coef->digits[0];					\
		P_UX_FRACTION_DIGIT(ux, LSD_NUM, lsd & ~mask);		\
		op = lsd & 1;						\
		scale = (((lsd >> 1) & mask) - bias);			\
		}


void
EVALUATE_PACKED_POLY( UX_FLOAT * argument, WORD degree, FIXED_128 * coefs,
  U_WORD mask, WORD bias, UX_FLOAT * result)
    {
    WORD op;
    UX_EXPONENT_TYPE scale;
    UX_FLOAT tmp;

    P_UX_SIGN(&tmp, 0);
    P_UX_EXPONENT(&tmp, 0);
    UNPACK_COEF_TO_UX(coefs, result, mask, bias, scale, op);
    P_UX_SIGN(result, (op == ADD) ? 0 : UX_SIGN_BIT );
    P_UX_EXPONENT(result, scale);

    while (--degree >= 0)
        {
        MULTIPLY(argument, result, result);
        NORMALIZE(result);
        coefs++;
        UNPACK_COEF_TO_UX(coefs, &tmp, mask, bias, scale, op);
        ADDSUB(result, &tmp, op, result);
        UX_INCR_EXPONENT(result, scale);
        }
    }

#if !defined GROUP

    double D_GROUP_NAME( double x ) { return x; }

#endif
