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

#define	BASE_NAME	int
#include "dpml_ux.h"

#if !defined(MAKE_INCLUDE)
#   include STR(BUILD_FILE_NAME)
#endif


/*
** The basic approach is based on the observation that directed rounding
** can be done by "incrementing" the fraction field based on the value of
** four bits.  Consider the following diagram:
**
**	+-+-----------+------------------------+-+-+--------------+
**	|S|    exp    |                        |L|R|              |
**	+-+-----------+------------------------+-+-+--------------+
**	 ^                                     / | \
**	 |                Least significant bit  |  Rounding bit
**	sign bit                          Rounding Position
**
** Define K to be the "sticky" bit - i.e. the 'logical or' of all of the bits
** to the right of R.  Then, for a given rounding mode, the values of S, L, R
** and K uniquely determine whether or not to increment L.  Or to put it
** another way, S, L, R and K defines a binary value I to be added to L.  The
** following table defines I as a function of rounding mode and S, L, R and K
**
**				                  I
**				-------------------------------------
**		S K L R		 RZ	  RP	  RM	  RN	  RV
**		-------		----     ----    ----    ----    ----
**		0 0 0 0		  0	   0	   0	   0	   0
**		0 0 0 1		  0	   1	   0	   0	   1
**		0 0 1 0		  0	   0	   0	   0	   0
**		0 0 1 1		  0	   1	   0	   1	   1
**		0 1 0 0		  0	   1	   0	   0	   0
**		0 1 0 1		  0	   1	   0	   1	   1
**		0 1 1 0		  0	   1	   0	   0	   0
**		0 1 1 1		  0	   1	   0	   1	   1
**		1 0 0 0		  0	   0	   0	   0	   0
**		1 0 0 1		  0	   0	   1	   0	   1
**		1 0 1 0		  0	   0	   0	   0	   0
**		1 0 1 1		  0	   0	   1	   1	   1
**		1 1 0 0		  0	   0	   1	   0	   0
**		1 1 0 1		  0	   0	   1	   1	   1
**		1 1 1 0		  0	   0	   1	   0	   0
**		1 1 1 1		  0	   0	   1	   1	   1
**
** The above table gives rise to bit vectors, one per rounding mode, that
** determines I as a function of index = 8*S + 4*K + 2*L + R
** 
**	#define RZ_BIT_VECTOR	0x0000	(* 0000 0000 0000 0000 *)
**	#define RP_BIT_VECTOR	0x00fa	(* 0000 0000 1111 1010 *)
**	#define RM_BIT_VECTOR	0xfa00	(* 1111 1010 0000 0000 *)
**	#define RN_BIT_VECTOR	0xa8a8	(* 1010 1000 1010 1000 *)
**	#define RV_BIT_VECTOR	0xaaaa	(* 1010 1010 1010 1010 *)
**
** the UX_RND_TO_INT routine is the common logic for all of the "round-to-
** integer" routines.  Most of the arguments are self explanatory.  The
** low 16 bits of the 'flags' is one of the R<Z,P,M,N,V>_BIT_VECTOR's
** described above.  Bits 16 and 17 of 'flags' determine which results to
** compute according to the flags:
**
**		INTEGER_PART
**		FRACTION_PART
**
** Additionally, UX_RND_TO_INT returns the low BITS_PER_WORD of the integer
** result.
*/

WORD
UX_RND_TO_INT( UX_FLOAT * unpacked_argument, WORD flags,
  UX_FLOAT * unpacked_result, UX_FLOAT * unpacked_fraction )
    {
    WORD index, num_digits, shift, LR, SKLR;
    UX_EXPONENT_TYPE exponent, int_exponent;
    UX_FRACTION_DIGIT_TYPE *arg_ptr, *int_ptr, current_digit, new_digit,
       incr, sticky, lsd, mask;
    UX_FLOAT dummy;

    /*
    ** Get fraction digits into integer variables and initialize state
    */

    unpacked_result = unpacked_result ? unpacked_result : &dummy;

    sticky        = 0;
    num_digits    = NUM_UX_FRACTION_DIGITS;
    exponent      = G_UX_EXPONENT(unpacked_argument);
    arg_ptr       = &G_UX_LSD(unpacked_argument);
    int_ptr       = &G_UX_LSD(unpacked_result);
    shift         = 128 - exponent;
    current_digit = 0;

    do  {
        current_digit = *arg_ptr--;
        if (shift < BITS_PER_UX_FRACTION_DIGIT_TYPE)
            goto get_LR;

        /*
        ** The current digit is completely to the right of the binary point
        ** so zero out the corresponding digit in the result and accumulate
        ** the current digit into the sticky bits
        */

        *int_ptr--    = 0;
        sticky = current_digit | (sticky != 0);
        shift -= BITS_PER_UX_FRACTION_DIGIT_TYPE;
        } while (--num_digits > 0);

    sticky = (shift) ? (sticky != 0) : sticky;
    current_digit = 0;
    shift  = 0;

get_LR:

    if (shift < 0)
         shift = 0;
    incr = (UX_FRACTION_DIGIT_TYPE) 1 << shift;
    mask = incr - 1;

    /*
    ** At this point, we introduce a bit or a wort, but it makes processing in
    ** other routines easier.  We compute the least significant digit of the
    ** abs(int(x)) as the return value.  This mean we have to fetch one extra
    ** digit.
    */

    new_digit = 2*current_digit;

    if (mask == 0)
        { /* The L and R bits straddle a digit.  Get them back together */ 
        LR = (new_digit & 2) | ((UX_SIGNED_FRACTION_DIGIT_TYPE) sticky < 0);
        sticky += sticky;
        lsd = current_digit;
        }
    else
        { /* L and R are contiguous */
        LR = (current_digit >> (shift - 1)) & 0x3;
        sticky |= (new_digit & mask);
        lsd = (num_digits > 1) ? *arg_ptr : 0;
        lsd = (lsd << (BITS_PER_UX_FRACTION_DIGIT_TYPE - shift)) |
               (current_digit >> shift);
        }

    SKLR = 
        ((G_UX_SIGN(unpacked_argument) >> (BITS_PER_UX_SIGN_TYPE - 3)) & 0x8)
          + (((sticky != 0) << 2) + LR);

    /* Get increment value, add it in and propagate the carry */

    SKLR = (flags >> SKLR) & 1;
    incr = SKLR ? incr : 0;
    current_digit &= ~mask;
    lsd += SKLR;

    while (num_digits-- > 0)
        {
        new_digit = current_digit + incr;
        incr = (new_digit < incr);
        *int_ptr-- = new_digit;
        current_digit = *arg_ptr--;
        }

    if (incr)
        /*
        ** A carry out from the last add ==> result = 2^(exponent + 1) or
        ** 1, depending on whether or not exponent >= 0.
        */

        {
        exponent++;
        exponent = (exponent < 1) ? 1 : exponent;
        int_ptr[1] = UX_MSB;
        }

    P_UX_SIGN(unpacked_result, G_UX_SIGN(unpacked_argument));
    P_UX_EXPONENT(unpacked_result, exponent);

    if ( flags & FRACTION_RESULT )
        /* subtract int_func(x) from x */
        ADDSUB(unpacked_argument, unpacked_result, SUB, unpacked_fraction);

    return lsd;
    }

/*
** Each of the round-to-int functions calls a common routine C_UX_RND_TO_INT,
** to unpack its arguments; handle special input, and pack the results.
*/

#if !defined(C_UX_RND_TO_INT)
#   define C_UX_RND_TO_INT	__INTERNAL_NAME(C_rnd_to_int__)
#endif

static void
C_UX_RND_TO_INT( _X_FLOAT * packed_argument, U_WORD const * class_to_action_map, WORD flags,
  _X_FLOAT * packed_result, _X_FLOAT * packed_fraction
  OPT_EXCEPTION_INFO_DECLARATION )
    {
    WORD fp_class;
    UX_FLOAT unpacked_argument, unpacked_result, unpacked_fraction;

    fp_class  = UNPACK(
        packed_argument,
        & unpacked_argument,
        class_to_action_map,
        packed_result
        OPT_EXCEPTION_INFO_ARGUMENT );

    if (0 > fp_class)
        {  /* Set error value for fraction also */
        if (flags & FRACTION_RESULT)
            (void) UNPACK(
                packed_argument,
                & unpacked_argument,
                class_to_action_map + WORDS_PER_CLASS_TO_ACTION_MAP,
                packed_fraction
                OPT_EXCEPTION_INFO_ARGUMENT );
        return;
        }

    (void) UX_RND_TO_INT( &unpacked_argument, flags, &unpacked_result, 
              &unpacked_fraction );
            
    if (flags & INTEGER_RESULT)
        PACK(
            & unpacked_result,
            packed_result,
            NOT_USED,
            NOT_USED
            OPT_EXCEPTION_INFO_ARGUMENT );

    /* We assume the following call will normalize unpacked_result */

    if (flags & FRACTION_RESULT)
        PACK(
            & unpacked_fraction,
            packed_fraction,
            NOT_USED,
            NOT_USED
            OPT_EXCEPTION_INFO_ARGUMENT );
    }


/*
** The following code provides the user level interfaces to the trunc, modf,
** nint, ceil, float and nint routines
*/

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_FLOOR_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    C_UX_RND_TO_INT(
        PASS_ARG_X_FLOAT(packed_argument),
	FLOOR_CLASS_TO_ACTION_MAP,
	RM_BIT_VECTOR | INTEGER_RESULT,
        PASS_RET_X_FLOAT(packed_result),
	NOT_USED
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);

    }

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_CEIL_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    C_UX_RND_TO_INT(
        PASS_ARG_X_FLOAT(packed_argument),
	CEIL_CLASS_TO_ACTION_MAP,
	RP_BIT_VECTOR | INTEGER_RESULT,
        PASS_RET_X_FLOAT(packed_result),
        NOT_USED
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);

    }

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_TRUNC_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    C_UX_RND_TO_INT(
        PASS_ARG_X_FLOAT(packed_argument),
	TRUNC_CLASS_TO_ACTION_MAP,
	RZ_BIT_VECTOR | INTEGER_RESULT,
        PASS_RET_X_FLOAT(packed_result),
        NOT_USED
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);

    }

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_NINT_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    C_UX_RND_TO_INT(
        PASS_ARG_X_FLOAT(packed_argument),
	TRUNC_CLASS_TO_ACTION_MAP,
	RV_BIT_VECTOR | INTEGER_RESULT,
        PASS_RET_X_FLOAT(packed_result),
        NOT_USED
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);

    }

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_RINT_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    C_UX_RND_TO_INT(
        PASS_ARG_X_FLOAT(packed_argument),
	TRUNC_CLASS_TO_ACTION_MAP,
	RN_BIT_VECTOR | INTEGER_RESULT,
        PASS_RET_X_FLOAT(packed_result),
        NOT_USED
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);

    }

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_MODF_NAME

X_XXptr_PROTO(F_ENTRY_NAME, packed_result, packed_argument, packed_n)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    C_UX_RND_TO_INT(
        PASS_ARG_X_FLOAT(packed_argument),
	TRUNC_CLASS_TO_ACTION_MAP,
	RZ_BIT_VECTOR | INTEGER_RESULT | FRACTION_RESULT,
        packed_n,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);

    }

#if defined(F_NEAREST_NAME)

#   undef  F_ENTRY_NAME
#   define F_ENTRY_NAME	F_NEAREST_NAME

    X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument)
        {
        EXCEPTION_INFO_DECL
        DECLARE_X_FLOAT(packed_result)

        INIT_EXCEPTION_INFO;
        C_UX_RND_TO_INT(
            PASS_ARG_X_FLOAT(packed_argument),
            TRUNC_CLASS_TO_ACTION_MAP,
            RV_BIT_VECTOR | INTEGER_RESULT,
            PASS_RET_X_FLOAT(packed_result),
            NOT_USED
            OPT_EXCEPTION_INFO );

        RETURN_X_FLOAT(packed_result);

        }
#endif


#if defined(MAKE_INCLUDE)

    @divert -append divertText

#   undef TABLE_NAME

    START_TABLE;

    TABLE_COMMENT("floor class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "FLOOR_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(4) +
            CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
	    CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
	    CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     0) +
	    CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     0) +
	    CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     1) +
	    CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_NEGATIVE,  2) +
	    CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
	    CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     0) );

    TABLE_COMMENT("ceil class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "CEIL_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(3) +
            CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
            CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
            CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     0) +
            CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     0) +
            CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     2) +
            CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_NEGATIVE,  1) +
            CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
            CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     0) );

    /*
    ** the trunc class to action mapping is used by trunc, nint, rint and 
    ** modf.  In order to accommodate returns for both results in modf, there
    ** are actually two mappings, the first one is for the integer result, and
    ** the second one is for the fraction result.
    */

    TABLE_COMMENT("trunc, nint, rint and modf class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "TRUNC_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(2) +
            CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
            CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
            CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     0) +
            CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     0) +
            CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     1) +
            CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     1) +
            CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
            CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     0) );

    TABLE_COMMENT("this class-to-action-mapping used by modf only");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) +
            CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
            CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
            CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     1) +
            CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     1) +
            CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     0) +
            CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     0) +
            CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
            CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     0) );
    
    TABLE_COMMENT("data for the above class to action mappings");
       PRINT_U_TBL_ITEM( /* data 1 */ ZERO );
       PRINT_U_TBL_ITEM( /* data 2 */  ONE );

    END_TABLE;

    @end_divert

    @eval my $tableText;                                                \
          my $outText    = MphocEval( GetStream( "divertText" ) );      \
          my $defineText = Egrep( "#define", $outText, \$tableText );   \
             $outText    = "$tableText\n\n$defineText";                 \
          my $headerText = GetHeaderText( STR(BUILD_FILE_NAME),         \
                           "Definitions and constants 'round to int'" . \
                              " routines", __FILE__ );                  \
             print "$headerText\n\n$outText\n";


#endif


