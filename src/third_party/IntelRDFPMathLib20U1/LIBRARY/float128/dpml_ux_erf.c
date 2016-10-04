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

#define	BASE_NAME	erf
#include "dpml_ux.h"

#if !defined(MAKE_INCLUDE)
#   include STR(BUILD_FILE_NAME)
#endif

/* 
** BASIC DESIGN
** ------------
** 
** The erf/erfc design is based on the following identities:
** 
** 	           2*x     __inf (-x^2)^k
** 	erf(x) = --------  >     --------			(1)
** 	         sqrt(pi) /__0   (k+1)*k!
** 
** 	erfc(x) = 1 - erf(x)					(2)
** 
** 	           exp(-x^2)   __inf     (2k)!
** 	erfc(x) ~  ----------  >     -------------		(3)
** 	           x*sqrt(pi) /__0   k!*(-4*x^2)^k
** 
** 	          exp(-x^2) /  1  1/2 2/2 3/2    \
** 	erfc(x) = --------- | --- --- --- --- ... |		(4)
**	          sqrt(pi)  \ x + x + x + x +    /
** 
** 
** The domain of the two functions is divided into 8 subintervals, symmetrically
** placed around 0.  For each subinterval, the general approach is to perform 
** some primary evaluation and then adjust its sign and add or subtract a
** constant.
** 
** On the first subinterval, from 0 to 1, the primary evaluation is a rational
** approximation to erf(x) of the form x*R(x^2), based on (1).  It should be
** noted here that the upper bound of this interval could be taken as large as
** 2 and still have the terms of R(x^2) be decreasing. However, as the upper
** limit increases past 1, loss of significance when computing 1 - erf(x)
** becomes a problem, so we take the upper limit of the first interval a 1
** because it simplifies the interval determination logic. 
** 
** The second subinterval spans 1 to A, where A is chosen so that if x >= A, the
** correctly rounded value of erf(x) is 1.  On this subinterval, the primary
** evaluation is an approximation to erfc(x), of the form exp(-x*x)*S(x), where
** S(x) is a rational approximation based on (4).
** 
** The third subinterval spans A to B, where B is chosen so that if x >= B, then
** erfc(x) underflows.  On this subinterval, the primary evaluation is an
** approximation to erfc(x) of the form exp(-x*x)*T(1/x^2)/x, where T(1/x^2)
** is a rational approximation based on (3).
** 
** The actual values of A and B are somewhat arbitrary.  For this design we take
** B = 128, since that choice helps simplify the determination of the intervals.
** A is chosen to be 8.75.  The reason for this choice of A is that:
** 
** 	o it meets the requirement that x >= A ==> erf(x) = 1,
** 	o A has very few significant bits, so its fraction can be represented
** 	  in one word
** 	o For this choice of A or larger, the terms in T(1/x^2) decrease
*/ 

#define HI_WORD_OF_8_PT_75		0x8c00000000000000ull

/* 
** 
** IMPLEMENTATION STRATEGY
** -----------------------
** 
** Based on the above definitions and equation (2) we can construct table 1
** which shows how erf(x) and erfc(x) are computed based on which interval
** they lie in.  In the table we refer to the primary evaluations in the first,
** second and third subintervals as ERF(x), MID(x) and ERFC(x) respectively.
** 
** 
** 	Sub-Interval	Index	  erf(x)	  erfc(x)
** 	------------	-----	-----------	------------
**	(-Inf,  -128]     7	-1              2
**	(-128, -8.75]     6	-1              2
**	[-8.75,   -1)     5	-1 + MID(|x|)   2 - MID(|x|)
**	(-1,       0)     4	 0 - ERF(|x|)   1 + ERF(|x|)
**	( 0,       1)     0	 0 + ERF(|x|)   1 - ERF(|x|)
**	[ 1,    8.75]     1	 1 - MID(|x|)	0 + MID(x)
** 	( 8.75,  128]     2	 1         	0 + ERFC(|x|)
** 	( 128,  +Inf]     3	 1         	underflow
** 
** 			    Table 1
** 			    -------
** 
** Ignoring for the time being, that underflows may need to be signaled, the
** evaluation scheme for each subinterval, for both functions, is of the form:
** 
** 			   c + t*F(x) 			(5)
** where
** 
** 		o t is +/-1
** 		o c is -1, 0, 1 or 2
** 		o F(x) is ERF(x), MID(x), ERFC(x), UNDERFLOW(x) or 0
** 
** Based on the above, we implement erf and erfc as calls into a common
** evaluation routine, C_UX_ERF, that determines the interval the argument
** lies in and then dispatches to the appropriate evaluation code.
** 
** 
** MAPPING INTERVALS TO EVALUATIONS
** --------------------------------
** 
** The mapping from interval to evaluation function can be done via a switch
** statement on the interval.  The cases for ERFC(x) and UNDERFLOW need to check
** for whether an erf(x) or erfc(x) evaluation is being performed.
** 
** The selection of the constants, c can be accomplished by encoding the 
** appropriate values of c for erfc(x) in a "bit string" that can be indexed
** by the interval number.  Actually, rather then encoding the constants
** themselves we encode the index into an unpacked constant table.  Letting
** the index for c = -1, 0, 1 and 2 be c + 1 (i.e the indices 0, 1, 2 and
** 3 correspond to the constants -1, 0, 1 and 2), we can create two integers,
** defined by:
** 
** 		 1   1   1
** 		 4   2   0   8   6   4   2   0: bit position
** 	      +---+---+---+---+---+---+---+---+
** 	erfc: | 3 | 3 | 3 | 2 | 1 | 1 | 1 | 2 |
** 	      +---+---+---+---+---+---+---+---+
** 	      +---+---+---+---+---+---+---+---+
** 	erf:  | 0 | 0 | 0 | 1 | 2 | 2 | 2 | 1 |
** 	      +---+---+---+---+---+---+---+---+
** 
** that map indices of the constants to the intervals. Note that given one of
** the above integers, we can determine if an erf or erfc evaluation is being
** performed by looking at the low bit.
*/ 

#define MAP_BIT_WIDTH			0x2
#define MAP_MASK			MAKE_MASK(MAP_BIT_WIDTH, 0)
#define MAP_IT(a,b,c,s)		 			\
		((a << (7*MAP_BIT_WIDTH)) |		\
		 (a << (6*MAP_BIT_WIDTH)) |		\
		 (a << (5*MAP_BIT_WIDTH)) |		\
		 (b << (4*MAP_BIT_WIDTH)) |		\
		 (c << (3*MAP_BIT_WIDTH)) |		\
		 (c << (2*MAP_BIT_WIDTH)) |		\
		 (c << (1*MAP_BIT_WIDTH)) |		\
		 (b << (0*MAP_BIT_WIDTH)) |		\
		 s)

#define ERFC_INTERVAL_TO_CONSTANT_MAP 	MAP_IT(3, 2, 1, UX_SIGN_BIT)
#define ERF_INTERVAL_TO_CONSTANT_MAP 	MAP_IT(0, 1, 2, 0)

#define IS_ERF_EVALUATION(i)	(i & 1)
#define IS_ERFC_EVALUATION(i)	((i & 1) == 0)

#define INTERVAL(i)	i

/*
** CALCULATING MID(x)
** -------------------
**
** The rational expression that needs to be evaluated for mid(x) is particularly
** ill behaved from the point of view of the general unpacked rational
** evaluation routine.  So ill behaved in fact, that the general routine can
** not be used for the evaluation.  The problem is that over the range
** [1, 8.75), the evaluation cannot be formulated in such a way that the
** terms decrease in magnitude and at the same time have the argument be less
** that 1 is absolute value.  Since this is the evaluation in the math library
** that has these characteristics, the special evaluation code for this case
** is included here.
**
** The solution to the problem is to use a special (less efficient) packed format for
** the evaluation.  See dpml_ux_ops.c for at description of the format.
*/


/*
** C_UX_ERF is the common erf/erfc evaluation routine
*/

#if !defined(C_UX_ERF)
#    define C_UX_ERF	__INTERNAL_NAME(C_ux_erf__)
#endif

static void
C_UX_ERF(
  _X_FLOAT   * packed_argument,
  U_WORD       interval_to_constant_map,
  _X_FLOAT   * packed_result
  OPT_EXCEPTION_INFO_DECLARATION )
    {  
    WORD fp_class, index;
    WORD const * class_to_action_map;
    UX_SIGN_TYPE  sign;
    UX_EXPONENT_TYPE exponent;
    UX_FLOAT unpacked_argument, tmp[3], *eval_result;

    fp_class = UNPACK(
        packed_argument,
        &unpacked_argument,
        IS_ERF_EVALUATION(interval_to_constant_map) ?
            ERF_CLASS_TO_ACTION_MAP : ERFC_CLASS_TO_ACTION_MAP,
        packed_result
        OPT_EXCEPTION_INFO_ARGUMENT);

    if (0 > fp_class)
        return;

    /* Determine interval */

    exponent = G_UX_EXPONENT(&unpacked_argument);
    if (exponent < 4)
        index = (exponent <= 0) ? 0 : 1;
    else if (exponent > 4)
        index = (exponent < 8) ? 2 : 3;
    else 
        index = (G_UX_MSD(&unpacked_argument) < HI_WORD_OF_8_PT_75) ? 1 : 2;

    index += G_UX_SIGN(&unpacked_argument) ? 4 : 0;
    P_UX_SIGN(&unpacked_argument, 0);

    /*
    ** Branch to appropriate action code.
    */

    sign = UX_SIGN_BIT & interval_to_constant_map;
    eval_result = & tmp[0];
    switch (index)
        {
	case INTERVAL(4):
            sign ^= UX_SIGN_BIT;
            /* Fall through */

	case INTERVAL(0):
            EVALUATE_RATIONAL(
                &unpacked_argument,
                ERF_COEF_ARRAY,
                ERF_COEF_ARRAY_DEGREE,
                NUMERATOR_FLAGS(SQUARE_TERM | POST_MULTIPLY)
                  | DENOMINATOR_FLAGS(SQUARE_TERM),
                eval_result);
	    break;

	case INTERVAL(1):
            sign ^= UX_SIGN_BIT;
            /* Fall through */

	case INTERVAL(5):
             EVALUATE_PACKED_POLY( &unpacked_argument,
                 MID_NUM_COEF_ARRAY_DEGREE, MID_NUM_COEF_ARRAY,
                 MID_NUM_SCALE_MASK, MID_NUM_SCALE_BIAS, &tmp[1]);
             EVALUATE_PACKED_POLY( &unpacked_argument,
                 MID_DEN_COEF_ARRAY_DEGREE, MID_DEN_COEF_ARRAY,
                 MID_DEN_SCALE_MASK, MID_DEN_SCALE_BIAS, &tmp[2]);
             DIVIDE(&tmp[1], &tmp[2], FULL_PRECISION, eval_result);
             goto multiply_by_exp_m_x_sqr;
	     break;

	case INTERVAL(2):
             if (IS_ERF_EVALUATION(interval_to_constant_map))
                 goto default_label;

             /* Compute z*T(z^2) for z = 8/x */

             sign = 0;
             DIVIDE( NOT_USED, &unpacked_argument, FULL_PRECISION, &tmp[2]);
             EVALUATE_RATIONAL(
                &tmp[2],
                ERFC_COEF_ARRAY,
                ERFC_COEF_ARRAY_DEGREE,
                NUMERATOR_FLAGS(SQUARE_TERM | POST_MULTIPLY)
                  | DENOMINATOR_FLAGS(SQUARE_TERM) | P_SCALE(3),
                eval_result);
 
             /* Fall through */

        multiply_by_exp_m_x_sqr:

             /*
             ** In order to avoid excessive errors in the final result, we
             ** compute exp(-x^2) as
             **
             **		exp(-x^2) = exp(-(hi + lo))
             **		          = exp(-hi)*exp(-lo)
             **		          ~ exp(-hi)*(1 - lo)
             **		          = exp(-hi) - lo*exp(-hi)
             */

             EXTENDED_MULTIPLY(&unpacked_argument, &unpacked_argument, &tmp[1],
                 &tmp[2]);
             P_UX_SIGN( &tmp[1], UX_SIGN_BIT);
             UX_EXP( &tmp[1], &tmp[1]);
             MULTIPLY(&tmp[2], &tmp[1], &tmp[2]);
             ADDSUB(&tmp[1], &tmp[2], SUB | NO_NORMALIZATION, &tmp[1]);

             MULTIPLY(&tmp[1], eval_result, eval_result);
	     break;

	case INTERVAL(3):
             if (IS_ERFC_EVALUATION(interval_to_constant_map))
                 { /* Dummy up underflow result and "zero" index */
                 UX_SET_SIGN_EXP_MSD(&tmp[0], 0, UX_UNDERFLOW_EXPONENT, UX_MSB);
	         break;
                 }
             /* Fall through */

        default:
        default_label:
             eval_result = UX_ZERO;
	     break;
        }

    /* Adjust sign of the evaluation and add in constant */

    P_UX_SIGN(&tmp[0], sign);
    index = (interval_to_constant_map >> (MAP_BIT_WIDTH*index)) & MAP_MASK;
    WORD_TO_UX(index - 1, &tmp[1]);
    ADDSUB(eval_result, &tmp[1], ADD | NO_NORMALIZATION, &tmp[0]);

    PACK(
        &tmp[0],
        packed_result,
        ERFC_UNDERFLOW,
        NOT_USED
        OPT_EXCEPTION_INFO_ARGUMENT);
    }


/*
** The following two entry points implement erfl and erfcl by calling the
** C_UX_ERF routine with the appropriate parameters
*/

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_ERF_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    C_UX_ERF(
        PASS_ARG_X_FLOAT(packed_argument),
        ERF_INTERVAL_TO_CONSTANT_MAP,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO);

    RETURN_X_FLOAT(packed_result);

    }

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_ERFC_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    C_UX_ERF(
        PASS_ARG_X_FLOAT(packed_argument),
        ERFC_INTERVAL_TO_CONSTANT_MAP,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO);

    RETURN_X_FLOAT(packed_result);

    }



#if defined(MAKE_INCLUDE)

    @divert -append divertText

    precision = ceil(UX_PRECISION/8) + 4;

#   undef TABLE_NAME

    START_TABLE;

    TABLE_COMMENT("erf class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "ERF_CLASS_TO_ACTION_MAP\t");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_NEGATIVE,  1) +
              CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     0) );

        PRINT_U_TBL_ITEM( /* data 1 */ ONE );


    TABLE_COMMENT("erfc class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "ERFC_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) +
              CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     3) +
              CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     2) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     2) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     2) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     2) );

        PRINT_U_TBL_ITEM( /* data 1 */ ZERO );
        PRINT_U_TBL_ITEM( /* data 2 */  ONE );
        PRINT_U_TBL_ITEM( /* data 3 */  TWO );

    TABLE_COMMENT("unpacked 0 constant");
    PRINT_UX_TBL_ADEF_ITEM( "UX_ZERO\t\t\t", 0);

    /*
    ** The remaining mphoc computes the coefficients for the various rational
    ** evaluations.  The erf/erfc approximations are rather difficult to
    ** compute and consequently the Remes algorithm requires a long time to
    ** converge.  In order to speed up the process for the normal case, we
    ** compute rational approximation of specific degrees, rather than using
    ** the REMES_FIND_RATIONAL option.
    */

#   if UX_PRECISION != 128
#        error "Rational coefficient degrees may be invalid for this precision"
#   endif
 
    /*
    ** Generate coefficients for erf(x) evaluation on [0,1)
    */

    zero_value = 2/sqrt(pi);
    function __erf(x)
        {
        if (x == 0)
            return zero_value;
        else
            return erf(x)/x;
        }

    save_precision = precision;
    precision = ceil(UX_PRECISION/8) + 8;
    max_arg = 1;

    num_degree = 10;
    den_degree = 10;
    TABLE_COMMENT("Fixed point coefficients for erf(x) evaluation");
    remes(REMES_STATIC + REMES_RELATIVE_WEIGHT + REMES_SQUARE_ARG,
        0, max_arg, __erf, num_degree, den_degree, &ux_rational_coefs);

    precision = save_precision;

    PRINT_FIXED_128_TBL_ADEF("ERF_COEF_ARRAY\t\t");
    degree = print_ux_rational_coefs(num_degree, den_degree, 0);
    PRINT_WORD_DEF("ERF_COEF_ARRAY_DEGREE\t", degree);


    /*
    ** Generate coefficients for erfc(x) evaluation on [8.75, 128)
    */

    zero_value = 1/sqrt(pi);

    function __erfc(z)
        {
        auto x;

        if (z == 0)
            return zero_value;

        x = 8/z;
        return exp(x*x)*x*erfc(x);
        }


    save_precision = precision;
    precision = ceil(UX_PRECISION/8) + 8;
    min_arg = 0;
    max_arg = 8/8.75;

    num_degree = 10;
    den_degree = 10;
    TABLE_COMMENT("Fixed point coefficients for erfc(x) evaluation");
    remes(REMES_STATIC + REMES_RELATIVE_WEIGHT + REMES_SQUARE_ARG,
        min_arg, max_arg, __erfc, num_degree, den_degree, &ux_rational_coefs);

    precision = save_precision;

    PRINT_FIXED_128_TBL_ADEF("ERFC_COEF_ARRAY\t\t");
    degree = print_ux_rational_coefs(num_degree, den_degree, -3);
    PRINT_WORD_DEF("ERFC_COEF_ARRAY_DEGREE\t", degree);

    /*
    ** Generate coefficients for mid(x) evaluation on [1,8.75).
    */

    function __mid(x) { return exp(x*x)*erfc(x); }

    save_precision = precision;
    precision = ceil(UX_PRECISION/8) + 8;
    min_arg = 1;
    max_arg = 8.75;

    num_degree = 16;
    den_degree = 17;
    remes(REMES_STATIC + REMES_RELATIVE_WEIGHT + REMES_LINEAR_ARG +
        REMES_INIT_LEFT_CHEBY, min_arg, max_arg, __mid, num_degree, den_degree,
        &ux_rational_coefs);

    precision = save_precision;


    /*
    ** Now convert numerator and denominator to "packed" form and print them out
    */

    procedure cvt_and_print_packed(degree, base_index)
        {
        find_exponent_width_and_bias(degree, base_index);
        cvt_to_packed(degree, base_index, packed_exponent_width,
          packed_exponent_bias);
        print_packed(degree, base_index);
        }

    TABLE_COMMENT("Packed coefficients for mid numerator evaluation");
    PRINT_FIXED_128_TBL_ADEF("MID_NUM_COEF_ARRAY\t");
    PRINT_WORD_DEF("MID_NUM_COEF_ARRAY_DEGREE", num_degree);
    cvt_and_print_packed(num_degree, 0);
    PRINT_WORD_DEF("MID_NUM_SCALE_BIAS\t", packed_exponent_bias);
    PRINT_WORD_DEF("MID_NUM_SCALE_MASK\t", (1 << packed_exponent_width) - 1);

    TABLE_COMMENT("Packed coefficients for mid denominator evaluation");
    PRINT_FIXED_128_TBL_ADEF("MID_DEN_COEF_ARRAY\t");
    PRINT_WORD_DEF("MID_DEN_COEF_ARRAY_DEGREE", den_degree);
    cvt_and_print_packed(den_degree, num_degree + 1);
    PRINT_WORD_DEF("MID_DEN_SCALE_BIAS\t", packed_exponent_bias);
    PRINT_WORD_DEF("MID_DEN_SCALE_MASK\t", (1 << packed_exponent_width) - 1);

    END_TABLE;

    @end_divert
    @eval my $tableText;						\
          my $outText    = MphocEval( GetStream( "divertText" ) );	\
          my $defineText = Egrep( "#define", $outText, \$tableText );	\
             $outText    = "$tableText\n\n$defineText";			\
          my $headerText = GetHeaderText( STR(BUILD_FILE_NAME),         \
                           "Definitions and constants erf and erfc",	\
                              __FILE__ );				\
             print "$headerText\n\n$outText\n";

#endif

