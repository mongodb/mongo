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

#define BASE_NAME       exp
#include "dpml_ux.h"

#if !defined(MAKE_INCLUDE)
#   include STR(BUILD_FILE_NAME)
#endif

extern _X_FLOAT PACKED_CONSTANT_TABLE[ LAST_CONS_INDEX ];

/*
** UX_EXP_REDUCE performs argument reduction for the exponential family of
** functions.  Given and input argument, x, UX_EXP_REDUCE computes the reduced
** argument, z, and the scale factor, s, as:
**
**			lnb*x = s*ln2 + z,	|z| < ln2/2
**
** where b is equal to e or 10. If |x| > 2^16, UX_EXP_REDUCE returns a value of
** s and z that will force underflow or overflow in the pack routine.
*/

#if !defined(UX_EXP_REDUCE)
#   define UX_EXP_REDUCE	__INTERNAL_NAME(ux_exp_reduce__)
#endif

static WORD
UX_EXP_REDUCE(UX_FLOAT * orig_argument, UX_FLOAT * reduced_argument,
               UX_FRACTION_DIGIT_TYPE * constants )
    {
    U_WORD shift, reduce_constant_exp;
    UX_SIGN_TYPE sign;
    UX_EXPONENT_TYPE exponent, scale_exponent;
    UX_FRACTION_DIGIT_TYPE scale, msd, lsd;
    UX_FLOAT ux_scale, tmp;

    exponent = G_UX_EXPONENT(orig_argument);
    sign = G_UX_SIGN(orig_argument);

    reduce_constant_exp = constants[2];
    if ( (UX_UNSIGNED_EXPONENT_TYPE) (exponent + 1 - reduce_constant_exp) > 18)
        { /* Either no reduction is necessary, or exponent > 17 */

        scale = 0;
        UX_COPY(orig_argument, reduced_argument);
        if (exponent > 0)
            { /* exponent > 17, force underflow or overflow */
            P_UX_EXPONENT(reduced_argument, -128);
            scale = sign ? UX_UNDERFLOW_EXPONENT : UX_OVERFLOW_EXPONENT;
            }
        return scale;
        }
                
    /*
    ** Given an input argument of the form x = 2^n*f, we want to compute
    ** lnb*x = scale*ln2 + z, |z| <= ln2/2.  Or equivalently, scale =
    ** nint(x*lnb/ln2) and z = scale*ln2.  Suppose, the number of bits in a
    ** fraction digit is k, and we define K = 2^k.  Further suppose that F is
    ** the high k-1 bits of f and L is the high k bits of lnb/ln2.  Then
    **
    **		scale = nint(x*lnb/ln2)
    **		      = nint[ 2^n*f*(lnb/ln2) ]
    **		      ~ nint{ 2^n*[F/(K/2)]*[L/(K/4)] }
    **		      = nint{ 2^(n+3)*(F*L)/K^2 }
    **		      = nint{ 2^(n+3)*[ Hi(F*L)*K + Lo(F*L) ]/K^2 }
    **		      ~ nint{ 2^(n+3)*H(F*L)/K }
    **		      = nint{ H(F*L)/2^(k - n - 3) }
    **
    ** so that we can compute scale by computing the high k bits of F*L and
    ** "shifting" right k-n-3 bits.  Since we want to multiply by scale,
    ** we actually mask out the low order bits after rounding.  Note that
    ** since we took only the high k-1 bits of f, there is no possibility
    ** of a carry out on the round.
    */

    msd = G_UX_MSD(orig_argument) >> 1;
    UMULH( msd, constants[0], scale);
    shift = (BITS_PER_UX_FRACTION_DIGIT_TYPE - 3) - exponent;
    scale += SET_BIT(shift - 1);
    scale &= -SET_BIT(shift);

    /*
    ** Now compute (x - scale*high_bits_of_ln2) - scale*low_bits_of_ln2
    ** Begin by make sure scale is normalized.  It could have at most two
    ** leading zeros
    */

    while ((UX_SIGNED_FRACTION_DIGIT_TYPE) scale > 0)
        {
        scale += scale;
        shift++;
        }


    /*
    ** Get scale*high_bits_of_ln2 and subtract from x.  Theres a small
    ** complication that needs to be dealt with here:  When computing
    ** scale*high_bits_of_ln2, it may be unnormalized by one bit.  Which
    ** causes x to be right shifted one bit on the subtraction, there by
    ** losing the last bit of x.  Most of the time, this is unimportant.
    ** However, for very large arguments with a non-zero lsb, this results
    ** in very large error in the final answer, so we need to normalize
    ** scale*high_bits_of_ln2 before subtracting
    */

    scale_exponent = BITS_PER_UX_FRACTION_DIGIT_TYPE - shift;
    EXTENDED_DIGIT_MULTIPLY(scale, constants[1], msd, lsd);
    exponent = scale_exponent;
    if (((UX_SIGNED_FRACTION_DIGIT_TYPE) msd) > 0)
        {
        exponent--;
        msd = (msd + msd) + (lsd >> (BITS_PER_UX_FRACTION_DIGIT_TYPE - 1));
        lsd += lsd;
        }

    /* adjust the product exponent by the exponent of the constant */
    UX_SET_SIGN_EXP_MSD(&tmp, sign, exponent + reduce_constant_exp, msd);
    P_UX_FRACTION_DIGIT(&tmp, 1, lsd);
    ADDSUB(orig_argument, &tmp, SUB, &tmp);

    /* scale*low_bits_of_ln2 and subtract from x - scale*high_bits_of_ln2 */

    UX_SET_SIGN_EXP_MSD(&ux_scale, sign, scale_exponent, scale);
    MULTIPLY(&ux_scale, (UX_FLOAT *)&constants[3], reduced_argument);
    ADDSUB(&tmp, reduced_argument, SUB | NO_NORMALIZATION, reduced_argument);

    scale >>= shift;
    scale = (sign) ? -scale : scale;
    return scale;
    }

/*
** UX_EXP_COMMON is the unpacked interface to routine that will compute b^x for
** b = e or 10.  It calls UX_EXP_REDUCE to get the exponent and reduced
** argument and then evaluates the exp polynomial
*/

#if !defined(UX_EXP_COMMON)
#   define UX_EXP_COMMON	__INTERNAL_NAME(ux_exp_common__)
#endif

void
UX_EXP_COMMON( UX_FLOAT * unpacked_argument,  UX_FLOAT * unpacked_result,
        UX_FRACTION_DIGIT_TYPE * constant_table)
    {
    UX_EXPONENT_TYPE scale;
    UX_FLOAT reduced_argument;

    /* Get reduced argument */
    scale = UX_EXP_REDUCE(unpacked_argument, &reduced_argument, constant_table);

    /* Compute e^reduced_argument */

    EVALUATE_RATIONAL(
        &reduced_argument,
        (FIXED_128 *) &constant_table[EXP_COEF_INDEX],
        constant_table[EXP_DEGREE_INDEX],
	NUMERATOR_FLAGS(STANDARD),
        unpacked_result);

    /* Scale e^reduced_argument */

    UX_INCR_EXPONENT(unpacked_result, scale);
    }

/*
** UX_EXP is the unpacked interface to the exponential routine.  It calls
** UX_EXP_COMMONN routine to compute its result.
*/

#if !defined(UX_EXP)
#   define UX_EXP	__INTERNAL_NAME(ux_exp__)
#endif

void
UX_EXP( UX_FLOAT * unpacked_argument,  UX_FLOAT * unpacked_result)
    {
    UX_EXP_COMMON(unpacked_argument, unpacked_result,
                       EXP_CONSTANT_TABLE_ADDRESS);
    }


/*
** F_EXP_NAME is the user level packed x-float exp routine
*/

#undef	F_ENTRY_NAME
#define F_ENTRY_NAME	F_EXP_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument)
    {
    WORD   fp_class;
    UX_FLOAT unpacked_argument, unpacked_result;
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    fp_class  = UNPACK(
        PASS_ARG_X_FLOAT(packed_argument),
        & unpacked_argument,
        EXP_CLASS_TO_ACTION_MAP,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO );

    if (0 > fp_class)
       RETURN_X_FLOAT(packed_result);

    UX_EXP( &unpacked_argument, &unpacked_result);

    PACK(
        &unpacked_result,
        PASS_RET_X_FLOAT(packed_result),
        EXP_UNDERFLOW,
        EXP_OVERFLOW
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);

    }


/*
** F_EXPM1_NAME is the packed x-float expm1 function.  F_EXPM1_NAME exam the
** size of the reduced argument.  If it is small enough, a direct polynomial
** evaluation is perform.  Otherwise, UX_EXP computes expm1(x) = exp(x) - 1
*/

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_EXPM1_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument)
    {
    WORD fp_class;
    UX_EXPONENT_TYPE scale;
    UX_FLOAT unpacked_argument, unpacked_result, reduced_argument, one;
    UX_FRACTION_DIGIT_TYPE * constants;
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    fp_class = UNPACK(
        PASS_ARG_X_FLOAT(packed_argument),
        & unpacked_argument,
        EXPM1_CLASS_TO_ACTION_MAP,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO);

    if (0 > fp_class)
       RETURN_X_FLOAT(packed_result);

    constants = EXP_CONSTANT_TABLE_ADDRESS;
    scale = UX_EXP_REDUCE( &unpacked_argument, &reduced_argument, constants);
    if (scale == 0)
        {
        /*
        ** abs(reduced_argument) < ln2/2. computing expm1(x) as
        ** exp(x) - 1, could result in a serve loss of significance,
        ** so use a direct polynomial evaluation instead.  We use the
	** low EXP_COEF_ARRAY_DEGREE - 1 terms of the exp polynomial.
	** This has the side effect that the exponent field of the 
	** result is 1 to small.
        */
        EVALUATE_RATIONAL(
            &reduced_argument,
            (FIXED_128 *) &constants[EXP_COEF_INDEX],
    	    constants[EXP_DEGREE_INDEX] - 1,
            NUMERATOR_FLAGS(POST_MULTIPLY),/* Post multiply by x */
            &unpacked_result);
        UX_INCR_EXPONENT(&unpacked_result, 1);
        }
    else 
        {
	/*
	** Compute expm1(x) = exp(x) - 1.  Since |scale| >= 1,
        ** exp(x) <= 1/sqrt(2) and exp(x) >= sqrt(2)
        */
        EVALUATE_RATIONAL(
            &reduced_argument,
            (FIXED_128 *) &constants[EXP_COEF_INDEX],
    	    constants[EXP_DEGREE_INDEX],
	    NUMERATOR_FLAGS(STANDARD),
            &unpacked_result);
        UX_INCR_EXPONENT(&unpacked_result, scale);

        ADDSUB(
           &unpacked_result,
           UX_ONE,
           SUB | NO_NORMALIZATION | MAGNITUDE_ONLY,
           &unpacked_result
           ); 
        }

    PACK(
        &unpacked_result,
        PASS_RET_X_FLOAT(packed_result),
        NOT_USED,
        EXPM1_OVERFLOW
        OPT_EXCEPTION_INFO);

    RETURN_X_FLOAT(packed_result);

    }

/*
** F_EXP10_NAME is the user level packed x-float exp10 routine
*/

#undef	F_ENTRY_NAME
#define F_ENTRY_NAME	F_EXP10_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument)
    {
    WORD   fp_class;
    UX_FLOAT unpacked_argument, unpacked_result;
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    fp_class  = UNPACK(
        PASS_ARG_X_FLOAT(packed_argument),
        & unpacked_argument,
        EXP_CLASS_TO_ACTION_MAP,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO );

    if (0 > fp_class)
       RETURN_X_FLOAT(packed_result);

    UX_EXP_COMMON( &unpacked_argument, &unpacked_result,
                     EXP10_CONSTANT_TABLE_ADDRESS);

    PACK(
        &unpacked_result,
        PASS_RET_X_FLOAT(packed_result),
        EXP_UNDERFLOW,
        EXP_OVERFLOW
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);

    }

/*
** UX_HYPERBOLIC is the core processing for hyperbolic function of an unpacked
** argument.  Depending on the evaluation flags to UX_HYPERBOLIC, it computes
** one of sinh, cosh, sinhcosh or tanh.  In order to promote "efficiency" and
** clarity, then evaluation flags are divided into three separate fields
** containing (somewhat redundant) evaluation information.  One field contains
** the function to be evaluated (SINH, COSH, SINHCOSH or TANH); one field
** contains the appropriate evaluation flags for EVALUATION_RATIONAL; and
** one field containing the opcode to be used by the ADDSUB routine
*/

#define	__FLAGS(i,w,p)		(((i) >> (p)) & MAKE_MASK(w,0))

#define EVAL_RATIONAL_POS	0
#define EVAL_RATIONAL_WIDTH	(2*NUM_DEN_FIELD_WIDTH + 3)
#define EVAL_RATIONAL_FLAGS(i)	__FLAGS(i,EVAL_RATIONAL_WIDTH,EVAL_RATIONAL_POS)

#define SINH_EVAL	( NUMERATOR_FLAGS( SQUARE_TERM | POST_MULTIPLY ) | SKIP)
#define COSH_EVAL	( SKIP | DENOMINATOR_FLAGS(SQUARE_TERM))
#define TANH_EVAL	( NUMERATOR_FLAGS( SQUARE_TERM | POST_MULTIPLY ) | \
			  DENOMINATOR_FLAGS(SQUARE_TERM) )
#define SINHCOSH_EVAL	( TANH_EVAL | NO_DIVIDE )

#define ADDSUB_POS	(EVAL_RATIONAL_WIDTH + EVAL_RATIONAL_POS)
#define ADDSUB_WIDTH	2
#define ADDSUB_FLAGS(i)	__FLAGS(i, ADDSUB_WIDTH, ADDSUB_POS)

#define FUNC_CODE_POS	(ADDSUB_POS + ADDSUB_WIDTH)
#define	SINH		(1 << FUNC_CODE_POS)
#define	COSH		(2 << FUNC_CODE_POS)
#define	SINHCOSH	(4 << FUNC_CODE_POS)
#define	TANH		(8 << FUNC_CODE_POS)

#define EVAL_FLAGS(f,r,a)	( (f) | ((r) << EVAL_RATIONAL_POS) | \
				  ((a) << ADDSUB_POS))

#define UX_HYPERBOLIC	__INTERNAL_NAME(ux_hyperbolic__)

void
UX_HYPERBOLIC( UX_FLOAT * unpacked_argument, WORD evaluation_flags,
  UX_FLOAT * unpacked_result)
    {
    UX_EXPONENT_TYPE scale;
    UX_SIGN_TYPE sign;
    UX_FLOAT reduced_argument, tmp[2];

    /*
    ** save sign of input and its absolute value before performing
    ** argument reduction, x = I*ln2 + z, |z| < ln2/2.  Note that
    ** if this is a cosh(x) evaluation, we treat the sign as positive.
    */

    sign = G_UX_SIGN(unpacked_argument);
    P_UX_SIGN(unpacked_argument, 0);
    sign = ( evaluation_flags & COSH ) ? 0 : sign;
    scale = UX_EXP_REDUCE( unpacked_argument, &reduced_argument,
                           EXP_CONSTANT_TABLE_ADDRESS);

    /*
    ** if scale == 0, then abs(x) < ln2/2 ==> sinh(x) or tanh(x) may have
    ** a loss of significance if computed via the definition, so compute
    ** by polynomial instead.  Otherwise, we compute exp(z) and 
    ** exp(-z) as cosh(z) + sinh(z) and cosh(z) - sinh(z) respectively.
    ** So, if scale == 0, used the passed in evaluation flags, otherwise
    ** Force a SINHCOSH evaluation.
    */

    EVALUATE_RATIONAL(
        &reduced_argument,
        SINHCOSH_COEF_ARRAY,
        SINHCOSH_COEF_ARRAY_DEGREE,
        (scale == 0) ? EVAL_RATIONAL_FLAGS(evaluation_flags) :
                       SINHCOSH_EVAL,
        unpacked_result );

    if (scale)
        {
        /*
        ** We want to compute sinh(x)/cosh(x) = (exp(x) -/+ exp(-x))/2.
        ** Begin by computing exp(z) and exp(-z) and then scale them
        ** to get exp(x)/2 and exp(-x)/2.
        */
        ADDSUB(
            &unpacked_result[1],	/* cosh(z)	*/
            &unpacked_result[0],	/* sinh(z)	*/
            ADD_SUB | NO_NORMALIZATION,
            &tmp[0]			/* exp(z):exp(-z)*/
            );

        UX_INCR_EXPONENT(&tmp[0], (scale - 1));
        UX_DECR_EXPONENT(&tmp[1], (scale + 1));

        /*
        ** Now add/sub exp(x)/2 and exp(-x)/2 to get sinh/cosh, if this
        ** is a tanh evaluation, do the divide
        */

        ADDSUB(
            &tmp[0],			/* exp(x)/2	*/
            &tmp[1],			/* exp(-x)/2	*/
            ADDSUB_FLAGS(evaluation_flags) | MAGNITUDE_ONLY | NO_NORMALIZATION,
            &unpacked_result[0]		/* sinh(x)/cosh(x)	*/
            );

        if (evaluation_flags & TANH)
            DIVIDE(&unpacked_result[0], &unpacked_result[1], FULL_PRECISION,
              &unpacked_result[0]);
        }

    P_UX_SIGN(unpacked_result, sign);
    }

/*
** C_UX_HYPERBOLIC is the common processing routine for the hyperbolic
** routines: sinh, cosh, sinhcosh and tanh.  It unpacks the input argument,
** calls UX_HYPERBOLIC to computes sinh, cosh, sinhcosh or tanh, and packs the
** results.
*/

#define	C_UX_HYPERBOLIC	__INTERNAL_NAME(C_ux_hyperbolic__)

static void
C_UX_HYPERBOLIC( _X_FLOAT * packed_result, _X_FLOAT * packed_argument,
  U_WORD const * class_to_action_map, WORD evaluation_flags,
  WORD overflow_code OPT_EXCEPTION_INFO_DECLARATION )
    {
    WORD    fp_class;
    UX_FLOAT unpacked_argument, unpacked_result[2];

    fp_class = UNPACK(
        packed_argument,
        &unpacked_argument,
        class_to_action_map,
        &packed_result[0]
        OPT_EXCEPTION_INFO_ARGUMENT );

    if (0 > fp_class)
        { /* If this is a SINHCOSH evaluation, write second result */
        if (evaluation_flags & SINHCOSH)
            {
            (void) UNPACK(
                packed_argument,
                &unpacked_argument,
                COSH_CLASS_TO_ACTION_MAP,
                &packed_result[1]
                OPT_EXCEPTION_INFO_ARGUMENT );
            }
        return;
        }

    UX_HYPERBOLIC(
        &unpacked_argument,
        evaluation_flags, 
        &unpacked_result[0]);

    PACK(
        &unpacked_result[0],
        packed_result,
        NOT_USED,
        overflow_code
        OPT_EXCEPTION_INFO_ARGUMENT );

    if (evaluation_flags & SINHCOSH)
        /* This was a sinhcosh evaluation */
        PACK(
            &unpacked_result[1],
            &packed_result[1],
            NOT_USED,
            COSH_OVERFLOW
            OPT_EXCEPTION_INFO_ARGUMENT );
    }

/*
** F_SINH_NAME, F_COSH_NAME, F_SINHCOSH_NAME and F_TANH_NAME are the packed
** x-float sinh, cosh, sinhcosh and tanh routines.  Each of these routines
** simply invokes the common routine C_UX_HYPERBOLIC to unpack its arguments,
** compute the result and pack it.
*/

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_SINH_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    C_UX_HYPERBOLIC(
      PASS_RET_X_FLOAT(packed_result),
      PASS_ARG_X_FLOAT(packed_argument),
      SINH_CLASS_TO_ACTION_MAP,
      EVAL_FLAGS( SINH, SINH_EVAL, SUB ),
      PACKED_ARG_IS_NEG(packed_argument) ? SINH_NEG_OVERFLOW : SINH_OVERFLOW
      OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);

    }

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_COSH_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    C_UX_HYPERBOLIC(
      PASS_RET_X_FLOAT(packed_result),
      PASS_ARG_X_FLOAT(packed_argument),
      COSH_CLASS_TO_ACTION_MAP,
      EVAL_FLAGS( COSH, COSH_EVAL, ADD),
      COSH_OVERFLOW
      OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);

    }

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_SINHCOSH_NAME

RR_X_PROTO(F_ENTRY_NAME, packed_result0, packed_result1, packed_argument)
    {
    EXCEPTION_INFO_DECL
    _X_FLOAT packed_result[2];

    INIT_EXCEPTION_INFO;
    C_UX_HYPERBOLIC(
        packed_result, /*PASS_RET_X_FLOAT(packed_result)*/
        PASS_ARG_X_FLOAT(packed_argument),
        SINH_CLASS_TO_ACTION_MAP,
        EVAL_FLAGS( SINHCOSH, SINHCOSH_EVAL, SUB_ADD), 
        PACKED_ARG_IS_NEG(packed_argument) ? SINH_NEG_OVERFLOW : SINH_OVERFLOW
        OPT_EXCEPTION_INFO );

    *packed_result0 = packed_result[0];
    *packed_result1 = packed_result[1];

    }

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_TANH_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    C_UX_HYPERBOLIC(
        PASS_RET_X_FLOAT(packed_result),
        PASS_ARG_X_FLOAT(packed_argument),
        TANH_CLASS_TO_ACTION_MAP,
        EVAL_FLAGS( TANH, TANH_EVAL, SUB_ADD), 
        NOT_USED
        OPT_EXCEPTION_INFO );
    RETURN_X_FLOAT(packed_result);
    }


#if defined(MAKE_INCLUDE)

    @divert -append divertText

    precision = ceil(UX_PRECISION/8) + 4;

#   undef  TABLE_NAME

    START_TABLE;

    TABLE_COMMENT("exp class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "EXP_CLASS_TO_ACTION_MAP\t");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(5) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ERROR,     3) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     2) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     1) );

    TABLE_COMMENT("expm1 class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "EXPM1_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(4) +
              CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_NEGATIVE,  1) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     0) );

    TABLE_COMMENT("sinh class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "SINH_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(3) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     0) );

    TABLE_COMMENT("cosh class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "COSH_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(2) +
              CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_NEGATIVE,  0) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     1) );


    TABLE_COMMENT("tanh class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "TANH_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) +
              CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_NEGATIVE,  1) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     0) );

    TABLE_COMMENT("Data for the class to action mappings");
    PRINT_U_TBL_ITEM( /* data 1 */ ONE  );
    PRINT_U_TBL_ITEM( /* data 2 */ EXP_OF_NEG_INF );
    PRINT_U_TBL_ITEM( /* data 3 */ EXP_OF_INF );

    /*
    ** Create the "table" of exp constants. The table includes the constants
    ** for the argument reduction, the degree of the polynomial and the 
    ** polynomial coefficients.
    */

    TABLE_COMMENT("Constant structure for exp based evaluations");
    PRINT_UX_FRACTION_DIGIT_TBL_ADEF("EXP_CONSTANT_TABLE_ADDRESS");

    save_precision = precision;
    precision = ceil(2*UX_PRECISION/8);

    ln2 = log(2);

    precision = save_precision;

    TABLE_COMMENT("High digits of 1/ln2, ln2 and binary exponent of ln2");
    exp_cons_base_offset = MP_BIT_OFFSET;
    ln2_hi = bround(ln2, BITS_PER_UX_FRACTION_DIGIT_TYPE);
    tmp = bround(bldexp(1/ln2, BITS_PER_UX_FRACTION_DIGIT_TYPE - 2),
               BITS_PER_UX_FRACTION_DIGIT_TYPE);
    PRINT_UX_FRACTION_DIGIT_TBL_ITEM(tmp);
    PRINT_UX_FRACTION_DIGIT_TBL_ITEM(
                        bldexp(ln2, BITS_PER_UX_FRACTION_DIGIT_TYPE) );
    PRINT_UX_FRACTION_DIGIT_TBL_ITEM(0);

    TABLE_COMMENT("ln2_lo = ln2 - ln2_hi in unpacked form");
    PRINT_UX_TBL_ITEM( ln2 - ln2_hi);

    /*
    ** Compute polynomial coefficient for exp and expm1.  Get coefficients
    ** for expm1 and prepend a 1 to the front of the list
    */

    function __expm1(x)
        {
        if (x == 0)
            return 1.;
        else
            return expm1(x)/x;
        }

    save_precision = precision;
    precision = ceil(UX_PRECISION/8) + 8;

    max_arg = ln2/2;

    remes(REMES_FIND_POLYNOMIAL + REMES_RELATIVE_WEIGHT, -max_arg,
       max_arg, __expm1, UX_PRECISION, &degree, &ux_rational_coefs);

    precision = save_precision;

    for (i = degree + 1; i > 0; /* NULL */ )
        ux_rational_coefs[i] = ux_rational_coefs[--i];
    ux_rational_coefs[0] = 1;

    #define __INDEX(z,b)	((z - b)/BITS_PER_UX_FRACTION_DIGIT_TYPE)
    TABLE_COMMENT("Polynomial degree");
    printf("#define EXP_DEGREE_INDEX\t\t%i\n",
           __INDEX(MP_BIT_OFFSET, exp_cons_base_offset));
    PRINT_UX_FRACTION_DIGIT_TBL_ITEM(degree+1);
    TABLE_COMMENT("Fixed point coefficients for exp/expm1 evaluation");
    printf("#define EXP_COEF_INDEX\t\t\t%i\n",
           __INDEX(MP_BIT_OFFSET, exp_cons_base_offset));
    print_ux_rational_coefs(degree + 1, 0, 0);

    TABLE_COMMENT("1 in unpacked format");
    PRINT_UX_TBL_ADEF_ITEM( "UX_ONE\t\t\t", 1);

    /*
    ** Create the "table" of exp10 constants. The layout is the same as for
    ** the exp constants.
    */


    TABLE_COMMENT("Constant structure for exp10 based evaluations");
    PRINT_UX_FRACTION_DIGIT_TBL_ADEF("EXP10_CONSTANT_TABLE_ADDRESS");

    save_precision = precision;
    precision = ceil(2*UX_PRECISION/8);

    ln2_ov_ln10 = log(2)/log(10);

    precision = save_precision;

    TABLE_COMMENT(
        "High digits of ln10/ln2, ln2/ln10 and binary exponent of ln2/ln10");
    exp_cons_base_offset = MP_BIT_OFFSET;
    ln2_ov_ln10_hi = bround(ln2_ov_ln10, BITS_PER_UX_FRACTION_DIGIT_TYPE);
    tmp = bround(bldexp(1/ln2_ov_ln10, BITS_PER_UX_FRACTION_DIGIT_TYPE - 2),
               BITS_PER_UX_FRACTION_DIGIT_TYPE);
    PRINT_UX_FRACTION_DIGIT_TBL_ITEM(tmp);
    PRINT_UX_FRACTION_DIGIT_TBL_ITEM(
                bldexp(ln2_ov_ln10, BITS_PER_UX_FRACTION_DIGIT_TYPE + 1) );
    PRINT_UX_FRACTION_DIGIT_TBL_ITEM(-1);

    TABLE_COMMENT("ln2_ov_ln10_lo = ln2 - ln2_ov_ln10__hi in unpacked form");
    PRINT_UX_TBL_ITEM( ln2_ov_ln10 - ln2_ov_ln10_hi);

    /*
    ** Compute polynomial coefficient for exp10.
    */

    function __exp10(x)
        {
        return exp(x*log(10));
        }

    save_precision = precision;
    precision = ceil(UX_PRECISION/8) + 8;

    max_arg = ln2_ov_ln10/2;

    remes(REMES_FIND_POLYNOMIAL + REMES_RELATIVE_WEIGHT, -max_arg,
       max_arg, __exp10, UX_PRECISION, &degree, &ux_rational_coefs);

    precision = save_precision;

    TABLE_COMMENT("Polynomial degree");
    PRINT_UX_FRACTION_DIGIT_TBL_ITEM(degree);
    TABLE_COMMENT("Fixed point coefficients for exp10 evaluation");
    print_ux_rational_coefs(degree, 0, 0);

    /*
    ** Now get sinh and cosh coefficients in the same array
    */

    function __cosh(x) { return cosh(x); }

    function __sinh(x)
        {
        if (x == 0)
            return 1.;
        else
            return sinh(x)/x;
        }

    save_precision = precision;
    precision = ceil(UX_PRECISION/8) + 8;

    max_arg = ln2/2;

    remes(REMES_FIND_POLYNOMIAL + REMES_RELATIVE_WEIGHT + REMES_SQUARE_ARG,
        0, max_arg, __sinh, UX_PRECISION, &degree, &ux_rational_coefs);

    remes(REMES_FIND_POLYNOMIAL + REMES_RELATIVE_WEIGHT + REMES_SQUARE_ARG,
        0, max_arg, __cosh, UX_PRECISION, &tmp_degree, &tmp_coefs);

    for (i = 0; i <= tmp_degree; i++)
        ux_rational_coefs[i + degree + 1] = tmp_coefs[i];

    TABLE_COMMENT("Fixed point coefficients for sinh/cosh evaluation");
    PRINT_FIXED_128_TBL_ADEF("SINHCOSH_COEF_ARRAY\t");
    degree = print_ux_rational_coefs(degree, tmp_degree, 0);
    PRINT_WORD_DEF("SINHCOSH_COEF_ARRAY_DEGREE", degree);


    END_TABLE;

    @end_divert
    @eval my $tableText;                                                \
          my $outText    = MphocEval( GetStream( "divertText" ) );      \
          my $defineText = Egrep( "#define", $outText, \$tableText );   \
             $outText    = "$tableText\n\n$defineText";                 \
          my $headerText = GetHeaderText( STR(BUILD_FILE_NAME),         \
                           "Definitions and constants exponential" .    \
                              " and hyperbolic routines", __FILE__ );   \
             print "$headerText\n\n$outText\n";


#endif
