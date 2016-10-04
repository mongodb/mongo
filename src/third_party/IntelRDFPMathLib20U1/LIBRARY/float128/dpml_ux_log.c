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

#define BASE_NAME	log
#include "dpml_ux.h"

#if !defined(MAKE_INCLUDE)
#   include STR(BUILD_FILE_NAME)
#endif


/* The basic design of for the log functions relies on a common evaluation
** routine.  The evaluation routine is based on the identities:
** 
** 	logb(x) = ln(x)/ln(b)					(1)
** 	ln(2^n*f) = n*ln(2) + ln(f)				(2)
** 	ln[(1+x)/(1-x)] = 2*sum{ k = 0,... | x^(2k+1)/(2k+1) }	(3)
** 
** Assuming that x = 2^n*f, where 1/2 <= f < 1, we define g and m as:
** 
** 	g = f;
** 	m = n;
** 	if (f < 1/sqrt(2))
** 	    {
** 	    g = 2*f;
** 	    m = n - 1;
** 	    }
** 
** Then x = 2^m*g where 1/sqrt(2) <= g < sqrt(2).  From (2) and (3) it follows
** that
** 		                                     g - 1
** 		ln(x) = m*ln(2) + z*p(z^2) where z = -----
** 		                                     g + 1
** 
** Then from (1) it follows that
** 
** 		logb(x) = m*ln(2)/ln(b) + z*p(z^2)/ln(b)
**		        = [m + z*r(z^2)]*[1/ln(b)]
**
** UX_LOG_POLY is a convenience functions that allows for the evaluation of
** the log polynomial without having to know the address of the coefficients
** and automatically multiplies by ln2.
*/

void
UX_LOG_POLY( UX_FLOAT * unpacked_argument, UX_FLOAT * unpacked_result)
    {
    EVALUATE_RATIONAL(
        unpacked_argument,
        LOG2_COEF_ARRAY,
        LOG2_COEF_ARRAY_DEGREE,
        NUMERATOR_FLAGS(SQUARE_TERM | POST_MULTIPLY),
        unpacked_result);
    MULTIPLY(unpacked_result, LN_2, unpacked_result);
    }

void
UX_LOG( UX_FLOAT * unpacked_argument, UX_FLOAT * scale,
  UX_FLOAT * unpacked_result)
    {
    UX_FLOAT tmp[2];
    UX_EXPONENT_TYPE m;
    UX_FRACTION_DIGIT_TYPE f_hi; 

    /*
    ** Compute z = (g - 1)/(g + 1).  Make sure to restore the input
    ** argument to its original value in case the caller needs to use
    ** it again.
    */

    m = G_UX_EXPONENT(unpacked_argument);
    f_hi = G_UX_MSD(unpacked_argument);
    if (f_hi <= ONE_OVER_SQRT_2)
        m--;
    UX_DECR_EXPONENT(unpacked_argument, m);
    ADDSUB(unpacked_argument, UX_ONE, ADD_SUB | MAGNITUDE_ONLY, &tmp[0]);
    UX_INCR_EXPONENT(unpacked_argument, m);
    DIVIDE(&tmp[1], &tmp[0], FULL_PRECISION, unpacked_result);
	  /*printf("UX_LOG:  tmp1=(%x %x) %llx %llx, tmp0=(%x %x) %llx %llx, r=(%x %x) %llx %llx\n",
		  tmp[1].sign,tmp[1].exponent,tmp[1].fraction[0],tmp[1].fraction[1],
		  tmp[0].sign,tmp[0].exponent,tmp[0].fraction[0],tmp[0].fraction[1],
		  unpacked_result->sign,unpacked_result->exponent,unpacked_result->fraction[0],unpacked_result->fraction[1]);*/

    /* Evaluate z*p(z^2) */

    EVALUATE_RATIONAL(
        unpacked_result,
        LOG2_COEF_ARRAY,
        LOG2_COEF_ARRAY_DEGREE,
        NUMERATOR_FLAGS(SQUARE_TERM | POST_MULTIPLY),
        &tmp[0]
        );

    /* Get m as a packed value and add to polynomial */

	  /*printf("UX_LOG:  tmp1=(%x %x) %llx %llx, tmp0=(%x %x) %llx %llx, u_res=(%x %x) %llx %llx\n",
		  tmp[1].sign,tmp[1].exponent,tmp[1].fraction[0],tmp[1].fraction[1],
		  tmp[0].sign,tmp[0].exponent,tmp[0].fraction[0],tmp[0].fraction[1],
		  unpacked_result->sign,unpacked_result->exponent,unpacked_result->fraction[0],unpacked_result->fraction[1]);*/
    WORD_TO_UX(m, unpacked_result);
	//printf("m=%llx\n",(long long)m);
    ADDSUB(unpacked_result, &tmp[0], ADD | NO_NORMALIZATION,
      unpacked_result);

    /* multiply by scale */

		//printf("u_res= (%x %x) %llx %llx\n",unpacked_result->sign,unpacked_result->exponent,unpacked_result->fraction[0],unpacked_result->fraction[1]);
 
    if (scale)
        MULTIPLY( unpacked_result, scale, unpacked_result);
 

    return;
    }

#if !defined(C_UX_LOG)
#   define C_UX_LOG	__INTERNAL_NAME(C_ux_log__)
#endif

static void
C_UX_LOG( _X_FLOAT * packed_argument, U_WORD const * class_to_action_map,
   UX_FLOAT * scale, _X_FLOAT * packed_result OPT_EXCEPTION_INFO_DECLARATION )
    {
    WORD    fp_class, index;
    UX_FLOAT unpacked_argument, unpacked_result;

    fp_class  = UNPACK(
        packed_argument,
        & unpacked_argument,
        class_to_action_map,
        packed_result
        OPT_EXCEPTION_INFO_ARGUMENT );

	  //printf("UX_LOG:  packed arg=%llx %llx, unpacked_arg=(%x %x) %llx %llx\n",packed_argument->digit[0],packed_argument->digit[1],unpacked_argument.sign,unpacked_argument.exponent,unpacked_argument.fraction[0],unpacked_argument.fraction[1]);

    if (0 > fp_class)
        return;

    UX_LOG(
        &unpacked_argument,
        scale,
        &unpacked_result);

    PACK(
        &unpacked_result,
        packed_result,
        NOT_USED,
        NOT_USED
        OPT_EXCEPTION_INFO_ARGUMENT );
    }


#undef F_ENTRY_NAME
#define F_ENTRY_NAME F_LN_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result,packed_argument)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    C_UX_LOG( PASS_ARG_X_FLOAT(packed_argument), LOG_CLASS_TO_ACTION_MAP, LN_2,
       PASS_RET_X_FLOAT(packed_result) OPT_EXCEPTION_INFO);

    RETURN_X_FLOAT(packed_result);

    }


#undef F_ENTRY_NAME
#define F_ENTRY_NAME F_LOG2_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result,packed_argument)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    C_UX_LOG( PASS_ARG_X_FLOAT(packed_argument), LOG2_CLASS_TO_ACTION_MAP, 0,
       PASS_RET_X_FLOAT(packed_result) OPT_EXCEPTION_INFO);

    RETURN_X_FLOAT(packed_result);

    }


#undef F_ENTRY_NAME
#define F_ENTRY_NAME F_LOG10_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result,packed_argument)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    C_UX_LOG( PASS_ARG_X_FLOAT(packed_argument), LOG10_CLASS_TO_ACTION_MAP, LOG10_2,       PASS_RET_X_FLOAT(packed_result) OPT_EXCEPTION_INFO);

    RETURN_X_FLOAT(packed_result);

    }

/*
** If we compute log1p(x) as log(1+x), then for small arguments a loss of
** significance will occur when computing the reduced argument for the generic
** log evaluation.  Consequently we screen out x such that
**
**		 1/sqrt(2) <= 1 + x < sqrt(2),
**
** or equivalently,
**
**		1/sqrt(2) - 1 <= x < sqrt(2) - 1	(4)
**
** We do this comparison "approximately" and in several phases.  First we
** screen x to lie in the interval (-1/2, 1/2) by looking at the exponent
** field of x.  Then we eliminate arguments with |x| <= 1/4, since these are
** known to satisfy (4).  At this point |x| = 2^(-1)*f and we can approximate
** 1 + x using only the high fraction digit x, F1.  Letting
** N = BITS_PER_DIGIT_TYPE:
**
**		1 + x = 2^(N-1)/2^(N-1) + 2^(-1)*F1/2^N
**		      = 2^(N-1)/2^(N-1) + F1/2^(N+1)
**		      = [2^(N-1) + F1/4]/2^(N-1)
**
** So we define an integer G such that G/2^(N-1) ~ 1 + x by,
**
**		G <-- F1 >> 2
**		if (x < 0)
**		    G <-- -G
**		G <-- G + (1 << (N-1))
**
** At this point we define two other integers:
**
**		I_RECIP_SQRT_2 <-- nint[2^(N-1)/sqrt(2)]
**		I_SQRT_2       <-- nint[2^(N-1)*sqrt(2)]
**
** Then the range check: 1/sqrt(2) < 1 + x < sqrt(2) is "equivalent" to
**
**		I_RECIP_SQRT_2 < G < I_SQRT_2.
*/

#undef F_ENTRY_NAME
#define F_ENTRY_NAME F_LOG1P_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result,packed_argument)
    {
    WORD fp_class;
    UX_SIGN_TYPE sign;
    UX_EXPONENT_TYPE exponent;
    UX_FRACTION_DIGIT_TYPE f_hi;
    UX_FLOAT unpacked_argument, unpacked_result, tmp;
    DECLARE_X_FLOAT(packed_result)
    EXCEPTION_INFO_DECL

    INIT_EXCEPTION_INFO;

    fp_class  = UNPACK(
        PASS_ARG_X_FLOAT(packed_argument),
        & unpacked_argument,
        LOG1P_CLASS_TO_ACTION_MAP,
        PASS_RET_X_FLOAT(packed_result) OPT_EXCEPTION_INFO );

    if (0 > fp_class)
       RETURN_X_FLOAT(packed_result);

    /*
    ** Screen out negative values <= -1.  For values less than
    ** -1, force "underflow".  For arguments equal to -1, force
    ** "overflow".
    */

    exponent = G_UX_EXPONENT( &unpacked_argument );
    sign     = G_UX_SIGN( &unpacked_argument );
    f_hi     = G_UX_MSD( &unpacked_argument );

    if (exponent >= 0)
        { /* |arg| >= 1/2.  */

        if ( exponent >= 1 )
            { /* |arg| >= 1.  Check for arg <= -1 */
            if (sign)
                { /* arg <= -1, start by forcing overflow */

                P_UX_MSD( &unpacked_result, UX_MSB);
                P_UX_EXPONENT( &unpacked_result, UX_OVERFLOW_EXPONENT);

                if ((exponent == 1) && (f_hi == UX_MSB) &&
                  UX_LOW_FRACTION_IS_ZERO( &unpacked_argument ))

                    /* This is -1.  Force underflow */
	            P_UX_EXPONENT(&unpacked_result, UX_UNDERFLOW_EXPONENT);
                goto pack_it;
                }
            }
        goto big_argument;
        }

    else if (exponent <= -2)
        /* |arg| <= 1/4. */
        goto small_argument;

    /*
    ** If we get here, 1/4 < |arg| < 1/2.  We need to check see if
    ** 1/sqrt(2) < 1 + x < sqrt(2)
    */

    f_hi = f_hi >> 2;
    f_hi = (sign) ? -f_hi : f_hi;
    f_hi += UX_MSB;

    if ( (UX_FRACTION_DIGIT_TYPE) (f_hi - I_RECIP_SQRT_2) >=
      (I_SQRT_2 - I_RECIP_SQRT_2))
        goto big_argument;

small_argument:

    /*
    ** If we get here, we know 1/sqrt(2) < 1 + x < sqrt(2).  To
    ** avoid loss of significance, compute the reduced argument
    ** as x/(2+x) and evaluate the log polynomial.
    */

    ADDSUB( UX_TWO, &unpacked_argument, ADD, &tmp);
    DIVIDE(&unpacked_argument, &tmp, FULL_PRECISION, &tmp);

    EVALUATE_RATIONAL(
        &tmp,
        LOG2_COEF_ARRAY,
        LOG2_COEF_ARRAY_DEGREE,
        NUMERATOR_FLAGS(SQUARE_TERM | POST_MULTIPLY),
        &unpacked_result
        );
 
    MULTIPLY( &unpacked_result, LN_2, &unpacked_result);
    goto pack_it;


big_argument:

    /* If we get here, just compute 1 + x and call the log */

    ADDSUB( UX_ONE, &unpacked_argument, ADD, &unpacked_result);

    UX_LOG( &unpacked_result, LN_2, &unpacked_result);

pack_it:
    PACK(
        &unpacked_result,
        PASS_RET_X_FLOAT(packed_result),
        LOG_OF_ZERO,
        LOG_OF_NEGATIVE
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);

    }


#if defined(MAKE_INCLUDE)

    @divert -append divertText

    precision = ceil(UX_PRECISION/8) + 4;

#   undef TABLE_NAME

    START_TABLE;

    TABLE_COMMENT("log class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "LOG_CLASS_TO_ACTION_MAP");

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) +
              CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ERROR,     2) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_ERROR,     1) );

    PRINT_U_TBL_ITEM( /* data 1 */ LOG_OF_NEGATIVE );
    PRINT_U_TBL_ITEM( /* data 2 */ LOG_OF_ZERO );


    TABLE_COMMENT("log2 class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "LOG2_CLASS_TO_ACTION_MAP");

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) +
              CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ERROR,     2) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_ERROR,     1) );

    PRINT_U_TBL_ITEM( /* data 1 */ LOG2_OF_NEGATIVE );
    PRINT_U_TBL_ITEM( /* data 2 */ LOG2_OF_ZERO );


    TABLE_COMMENT("log10 class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "LOG10_CLASS_TO_ACTION_MAP");

    PRINT_64_TBL_ITEM(
              CLASS_TO_ACTION_DISP(1) +
              CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ERROR,     2) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_ERROR,     1) );

    PRINT_U_TBL_ITEM( /* data 1 */ LOG10_OF_NEGATIVE );
    PRINT_U_TBL_ITEM( /* data 2 */ LOG10_OF_ZERO );


    TABLE_COMMENT("log1p class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "LOG1P_CLASS_TO_ACTION_MAP");

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) +
              CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     0) );

    PRINT_U_TBL_ITEM( /* data 1 */ LOG_OF_NEGATIVE);

    /*
    ** NOTE: the fraction fields of 1/sqrt(2) and sqrt(2) are identical, so
    ** that in the above code, the symbolic constants ONE_OVER_SQRT_2 and
    ** I_SQRT_2 have the same numerical value.
    */

    TABLE_COMMENT("MSD of sqrt(2) and 1/sqrt(2) (in fixed point)");
    tmp = trunc(bldexp(sqrt(2), BITS_PER_UX_FRACTION_DIGIT_TYPE - 1));
    PRINT_UX_FRACTION_DIGIT_TBL_VDEF( "ONE_OVER_SQRT_2\t\t");
    PRINT_UX_FRACTION_DIGIT_TBL_VDEF_ITEM( "I_SQRT_2\t\t", tmp);
    PRINT_UX_FRACTION_DIGIT_TBL_VDEF_ITEM( "I_RECIP_SQRT_2\t\t", trunc(tmp/2));

    /*
    ** Now generate coefficients for computing log.
    */

    zero_value = 2/log(2);
    function __log2(x)
        {
        if (x == 0)
            return zero_value;
        else
            return atanh(x)*zero_value/x;
        }

    save_precision = precision;
    precision = ceil(UX_PRECISION/8) + 8;

    max_arg = (sqrt(2) - 1)^2;

    TABLE_COMMENT("Fixed point coefficients for log2 evaluation");
    remes(REMES_FIND_POLYNOMIAL + REMES_RELATIVE_WEIGHT + REMES_SQUARE_ARG,
           0, max_arg, __log2, UX_PRECISION, &degree, &ux_rational_coefs);

    precision = save_precision;

    PRINT_FIXED_128_TBL_ADEF("LOG2_COEF_ARRAY\t\t");
    PRINT_WORD_DEF("LOG2_COEF_ARRAY_DEGREE\t", degree);
    print_ux_rational_coefs(degree, 0, 0);

    TABLE_COMMENT("Unpacked constants 1, 2, log(2) and log(10)");

    PRINT_UX_TBL_ADEF_ITEM( "UX_ONE",         1);
    PRINT_UX_TBL_ADEF_ITEM( "UX_TWO",         2);
    PRINT_UX_TBL_ADEF_ITEM( "LN_2",      log(2));
    PRINT_UX_TBL_ADEF_ITEM( "LOG10_2", log10(2));

    END_TABLE;

    @end_divert
    @eval my $tableText;						\
          my $outText    = MphocEval( GetStream( "divertText" ) );	\
          my $defineText = Egrep( "#define", $outText, \$tableText );	\
             $outText    = "$tableText\n\n$defineText";			\
          my $headerText = GetHeaderText( STR(BUILD_FILE_NAME),         \
                           "Definitions and constants logarithmic" .	\
                              " routines", __FILE__ );			\
             print "$headerText\n\n$outText\n";
#endif


