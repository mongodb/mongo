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

#define	BASE_NAME	inv_hyper
#include "dpml_ux.h"

#if !defined(MAKE_INCLUDE)
#   include STR(BUILD_FILE_NAME)
#endif


/* 
** 1. THE BASIC ALGORITHM:
** -----------------------
** 
** The inverse hyperbolic functions are defined by the identities:
** 
** 		asinh(x) = log[ x + sqrt(x^2 + 1) ]
** 		acosh(x) = log[ x + sqrt(x^2 - 1) ]      x >= 1
** 		atan(x)  = (1/2)*log[ (1 + x)/(1 - x) ] 
** 
** Noting that asinh(x) and atanh(x) are odd functions, then we can assume that
** x >= 0 for all three functions.  Under these circumstances, we see that each
** of the functions is of the form:
** 
** 			log[f(x)]	for x >= b
** 
** where b is 0 for asinh and atanh and 1 for acosh.  We note also that
** f(b) = 1 for all three functions.
** 
** Since log(z) is evaluated as polynomial in (z-1)/(z+1) when z is between
** 1/sqrt(2) and sqrt(2), implementing the inverse hyperbolic functions naively
** as log[f(x)], would result in the computation of w = [f(x) - 1]/[f(x) + 1]
** when x was near b.  However, this computation would result in a severe loss
** of significance.  To avoid this, when x is close to b, we compute w directly
** (and carefully) whenever f(x) is between 1/sqrt(2) and sqrt(2) and invoke
** the polynomial evaluation routines directly.
** 
** Recalling that we are dealing with positive values only, 1/sqrt(2) < f(x) <
** sqrt(2) iff f(x) < sqrt(2).  In all three cases then, we define a constant c
** such that f(x) < sqrt(2) is equivalent to x < c.  Assuming that c = 2^n*g,
** where ** g is in the interval [.5, 1).  We define the 64 bit integer, C, by
** 
** 			C = ceil(2^64*g).
** 
** Then we screen for loss of significance with a check on the exponent and
** first fraction word of x.
** 
** Table 1 gives the value of c and w for each of the inverse hyperbolic
** functions.
** 
** 
** 		Function	      c		        w   
** 		--------	-------------	-------------------
** 		  asinh		  sqrt(2)/4	x/[1 + sqrt(x^2+1)]
** 		  acosh		 3*sqrt(2)/4	 sqrt[(x-1)/(x+1)]
** 		  atanh		[sqrt(2)-1]^2	         x
** 
** 				Table 1
** 				-------
** 
*/ 


#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_ASINH_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument)
    {
    WORD fp_class;
    UX_SIGN_TYPE  sign;
    UX_EXPONENT_TYPE exponent;
    UX_FRACTION_DIGIT_TYPE  f_hi;
    UX_FLOAT unpacked_argument, unpacked_result, tmp;
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    fp_class  = UNPACK(
        PASS_ARG_X_FLOAT(packed_argument),
        & unpacked_argument,
        ASINH_CLASS_TO_ACTION_MAP,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO);

    if (0 >= fp_class)
       RETURN_X_FLOAT(packed_result);

    /* Get |x| */

    sign = G_UX_SIGN(&unpacked_argument);
    P_UX_SIGN(&unpacked_argument, 0);

    /* Compute sqrt(x^2+1) */

    SQUARE(&unpacked_argument, &tmp);
    ADDSUB(&tmp, UX_ONE, ADD, &tmp);
    NORMALIZE(&tmp);
    UX_SQRT(&tmp, &tmp);

    /* Check for small arguments */

    exponent = G_UX_EXPONENT(&unpacked_argument);
    f_hi     = G_UX_MSD(&unpacked_argument);
    if ((exponent < -1) || ((exponent == -1) && (f_hi <= SQRT_2_OV_4)))
        { /* Argument is small, evaluate directly */

        ADDSUB(&tmp, UX_ONE, ADD, &tmp);
        DIVIDE(&unpacked_argument, &tmp, FULL_PRECISION, &tmp);
        UX_LOG_POLY(&tmp, &unpacked_result);
        }
    else
        { /* Argument is not small, use log function */
        ADDSUB(&tmp, &unpacked_argument, ADD, &tmp);
        NORMALIZE(&tmp);
        UX_LOG( &tmp, UX_LN2, &unpacked_result);
        }

    /* Set sign of result and pack */

    P_UX_SIGN(&unpacked_result,  sign);
    PACK(
        &unpacked_result,
        PASS_RET_X_FLOAT(packed_result),
        NOT_USED,
        NOT_USED
        OPT_EXCEPTION_INFO);

    RETURN_X_FLOAT(packed_result);

    }



#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_ACOSH_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument)
    {
    WORD fp_class;
    UX_SIGN_TYPE  sign;
    UX_EXPONENT_TYPE exponent;
    UX_FRACTION_DIGIT_TYPE f_hi;
    UX_FLOAT *unpacked_argument, *unpacked_result, tmp[3];
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    unpacked_argument = &tmp[2];
    unpacked_result   = &tmp[0];

    INIT_EXCEPTION_INFO;
    fp_class  = UNPACK(
        PASS_ARG_X_FLOAT(packed_argument),
        unpacked_argument,
        ACOSH_CLASS_TO_ACTION_MAP,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO);

    if (0 > fp_class)
       RETURN_X_FLOAT(packed_result);

    /* Only positive arguments get here */ 

    exponent = G_UX_EXPONENT(unpacked_argument);
    f_hi     = G_UX_MSD(unpacked_argument);

    /* Compute x - 1 and x + 1 */

    ADDSUB(unpacked_argument, UX_ONE, ADD_SUB, unpacked_result);

    /* Check for arguments less than one */

    if (G_UX_SIGN(&unpacked_result[1]))
        { /* Arg was less than 1, force "overflow" */
        P_UX_EXPONENT(unpacked_result, UX_OVERFLOW_EXPONENT);
        goto pack_it;
        }

    /* Check for small arguments */

    else if ((exponent == 1) && (f_hi <= THREE_SQRT_2_OV_4))
        { /* Argument is small, evaluate directly */

        DIVIDE(unpacked_result + 1, unpacked_result, FULL_PRECISION,
          unpacked_result);
        UX_SQRT(unpacked_result, unpacked_result + 1);
        UX_LOG_POLY(unpacked_result + 1, unpacked_result);
        }
    else
        { /* Argument is not small, use log function */
        MULTIPLY(unpacked_result + 1, unpacked_result, unpacked_result);
        NORMALIZE(unpacked_result);
        UX_SQRT(unpacked_result, unpacked_result);
        ADDSUB(unpacked_result, unpacked_argument, ADD, unpacked_result);
        UX_LOG(unpacked_result, UX_LN2, unpacked_result);
        }

pack_it:

    PACK(
        unpacked_result,
        PASS_RET_X_FLOAT(packed_result),
        NOT_USED,
        ACOSH_ARG_LT_ONE
        OPT_EXCEPTION_INFO);

    RETURN_X_FLOAT(packed_result);

    }

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_ATANH_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument)
    {
    WORD fp_class, underflow_error;
    UX_SIGN_TYPE sign;
    UX_EXPONENT_TYPE exponent;
    UX_FRACTION_DIGIT_TYPE f_hi;
    UX_FLOAT * unpacked_argument, * unpacked_result, tmp[3];
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    unpacked_argument = &tmp[2];
    unpacked_result = &tmp[0];
    INIT_EXCEPTION_INFO;
    fp_class  = UNPACK(
        PASS_ARG_X_FLOAT(packed_argument),
        unpacked_argument,
        ATANH_CLASS_TO_ACTION_MAP,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO);

    if (0 > fp_class)
       RETURN_X_FLOAT(packed_result);

    /* Get |x| */

    sign = G_UX_SIGN(unpacked_argument);
    P_UX_SIGN(unpacked_argument, 0);

    /* Check for |arg| >= 1 */

    exponent = G_UX_EXPONENT(unpacked_argument);
    f_hi     = G_UX_MSD(unpacked_argument);
    if (exponent >= 1)
        { /* |x| >= 1,  split out |x| == 1 and |x| > 1 */

        P_UX_MSD(unpacked_result, UX_MSB);

        if ((exponent > 1) ||
          !UX_FRACTION_IS_ONE_HALF(unpacked_argument))

            /* |x| > 1, return error by forcing overflow */
            P_UX_EXPONENT(unpacked_result, UX_OVERFLOW_EXPONENT);

        else
            { /* |x| = 1, return error by forcing "underflow" */
            P_UX_EXPONENT(unpacked_result, UX_UNDERFLOW_EXPONENT);
            underflow_error = (sign) ?
               ATANH_OF_NEG_ONE : ATANH_OF_ONE;
            }
        }

    /* Check for x small */

    else if ((exponent < -2) ||
      ((exponent == -2) && (f_hi <= SQRT_2_M1_SQR)))
        { /* Argument is small, evaluate directly */
        UX_LOG_POLY( unpacked_argument, unpacked_result);
        }
    else
        { /* Argument is not small, use log function */
        ADDSUB(unpacked_argument,  UX_ONE, ADD_SUB, unpacked_result);
        DIVIDE(unpacked_result + 1, unpacked_result, FULL_PRECISION,
           unpacked_result);
        NORMALIZE(unpacked_result);
        UX_LOG(unpacked_result, UX_LN2, unpacked_result);
        }

    /* Set sign of result, multiply by 1/2 and pack */

    P_UX_SIGN(unpacked_result, sign);
    UX_DECR_EXPONENT(unpacked_result, 1);
    PACK(
        unpacked_result,
        PASS_RET_X_FLOAT(packed_result),
        underflow_error,
        ATANH_ABS_ARG_GT_ONE
        OPT_EXCEPTION_INFO);

    RETURN_X_FLOAT(packed_result);

    }

#if defined(MAKE_INCLUDE)

    @divert -append divertText

#   undef TABLE_NAME

    START_TABLE;

    TABLE_COMMENT("asinh class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "ASINH_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) +
              CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     0) );

    TABLE_COMMENT("acosh class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "ACOSH_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_ERROR,     1) );

         PRINT_U_TBL_ITEM( /* data 1 */ ACOSH_ARG_LT_ONE );

    TABLE_COMMENT("atanh class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "ATANH_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     0) );

         PRINT_U_TBL_ITEM( /* data 1 */ ATANH_ABS_ARG_GT_ONE );

    /*
    ** Generate definitions of sqrt(2)/4, 3*sqrt(2)/4 and [sqrt(2) - 1]^2
    */

#   define UX_MSB_OF_(x)  bldexp(x, BITS_PER_UX_FRACTION_DIGIT_TYPE - bexp(x))

    t = sqrt(2);
    PRINT_UX_FRACTION_DIGIT_TBL_VDEF_ITEM("SQRT_2_OV_4\t\t",     UX_MSB_OF_(t/4) );
    PRINT_UX_FRACTION_DIGIT_TBL_VDEF_ITEM("THREE_SQRT_2_OV_4\t", UX_MSB_OF_(3*t/4) );
    PRINT_UX_FRACTION_DIGIT_TBL_VDEF_ITEM("SQRT_2_M1_SQR\t\t",   UX_MSB_OF_((t-1)^2 ));

    TABLE_COMMENT("Unpacked constants 1 and ln2");
    PRINT_UX_TBL_ADEF_ITEM( "UX_ONE\t\t\t", 1);
    PRINT_UX_TBL_ADEF_ITEM( "UX_LN2\t\t\t", log(2));
    END_TABLE;

    @end_divert
    @eval my $tableText;                                                \
          my $outText    = MphocEval( GetStream( "divertText" ) );      \
          my $defineText = Egrep( "#define", $outText, \$tableText );   \
             $outText    = "$tableText\n\n$defineText";                 \
          my $headerText = GetHeaderText( STR(BUILD_FILE_NAME),         \
                           "Definitions and constants for inverse " .   \
                              "hyperbolic routines", __FILE__ );        \
             print "$headerText\n\n$outText\n";


#endif
