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

#define BASE_NAME	sqrt

#include "dpml_ux.h"

#undef	INDEX_MASK		/* Conflict with sqrt_macros.h */
#include "sqrt_macros.h"
#undef	INDEX_MASK		/* Restore original definition */
#define	INDEX_MASK		MAKE_MASK(INDEX_WIDTH, 0)

#if D_PRECISION < 53
#   error "ux_divide required D_PRECISION >= 53"
#endif

extern const SQRT_COEF_STRUCT D_SQRT_TABLE_NAME[(1<<(NUM_FRAC_BITS+1))];

#if !defined(MAKE_INCLUDE)
#   include STR(BUILD_FILE_NAME)
#endif

/*
** The following routine computes either sqrt(x) or 1/sqrt(x) for an unpacked
** argument x = 2^(2n-d)*f', f' in [1/2, 1), and d = 0 or 1.  Defining
** f = f'/2^d, the basic approach is to convert the high bits of f' to double
** precision and index into the sqrt table to evaluate a polynomial,
**  p(f') ~ 1/sqrt(f), good to about 28 bits.  Then carefully perform a Newton's
** iteration to produce an approximation to sqrt(f) or 1/sqrt(f) good to about
** 72 bits.  This result (represented by two double precision values) is then
** converted to integers and stored as the high 64 bits of the unpacked x-float
** result.  Call this result s.  Then sqrt(x) or 1/sqrt(x) is computed
** via a Newton's iteration in unpacked format as:
**
**		t <-- f*s
**		d <-- (3 - t*s)/2
**		if (sqrt)
**		    result <-- t*d
**		else
**		    result <-- s*d
**
*/

#define EVALUATE_SQRT		0
#define EVALUATE_RSQRT		1

void
UX_SQRT_EVALUATION( UX_FLOAT * x, WORD evaluation_type, UX_FLOAT * y)
    {
    U_WORD  index, shift, cshift;
    UX_SIGNED_FRACTION_DIGIT_TYPE signed_digit;
    UX_FRACTION_DIGIT_TYPE msd, lsd, tmp_digit;
    UX_EXPONENT_TYPE exponent, exponent_parity;
    UX_FLOAT s, ux_tmp;
    D_UNION u;
    D_TYPE f, f_hi, f_lo, g, g_lo, t, w;
    SQRT_COEF_STRUCT const * p;

    /*
    ** Given x = 2^(2n-d)*f', where d = 0 or 1 and f' is in the interval
    ** [1/2,1), the first step is to get an approximation to f' as a floating
    ** point value.  We will use f' in a polynomial evaluation to approximate
    ** 1/sqrt(f), where f = f'/2^d.  As part of the Newton's iterations we
    ** will need f_hi as the high 24 bits of f and f_lo as the high 53 bits
    ** of f - f_hi.
    */

    msd = G_UX_MSD(x);
    lsd = G_UX_2nd_MSD(x);
    u.D_UNSIGNED_HI_WORD = (msd >> D_EXP_WIDTH) +
          ((WORD) (D_EXP_BIAS - D_NORM - 2) << D_EXP_POS);

    exponent = G_UX_EXPONENT(x);
    exponent_parity = exponent & 1;
    exponent = (exponent + exponent_parity) >> 1;

    shift = (BITS_PER_UX_FRACTION_DIGIT_TYPE + exponent_parity - S_PRECISION);

#   if (NUM_UX_FRACTION_DIGITS == 2)

         lsd = ((msd << (BITS_PER_UX_FRACTION_DIGIT_TYPE - shift)) |
                 (lsd >> shift)) >>
                   (BITS_PER_UX_FRACTION_DIGIT_TYPE - D_PRECISION);
         f_lo = (double) lsd;

#   else  /* this code is *WRONG* */

         u.D_UNSIGNED_LO_WORD =
            (msd << (BITS_PER_WORD - D_EXP_WIDTH - 1) |
                (lsd >> (D_EXP_WIDTH + 1));

         shift = D_EXP_WIDTH + exponent_parity;
         cshift = 32 - shift;

         tmp_digit = G_UX_FRACTION_DIGIT(x, 2);
         tmp_digit = (tmp_digit >> ? ) | (lsd << ?);
         lsd = (lsd >> ?) | ((msd << ?) >> ?);
         f_lo = ((double) msd)*D_TWO_POW_32 + (double) lsd;

#endif

    f = u.f;		/* This is actually f' at this point */
    f_hi = ((double) (msd >> shift)) * D_RECIP_TWO_POW_24;
    f_lo *= D_RECIP_TWO_POW_77;

    /* 
    ** Now compute a polynomial in f to obtain an approximation to 1/sqrt(f'),
    ** call it g.
    **
    ** Get index into the square root polynomial coefficient table as the 
    ** low exponent bit and the high fraction bits and calculate g.
    **
    ** There is a little bit of a problem with using the DPML sqrt tables.
    ** Specifically, the table assume that f' is normalized between 1/2 and
    ** 2, so that the polynomials from the table yield a result in the
    ** interval ( 1/sqrt(2), sqrt(2) ].  For a floating point implementation,
    ** this is not a problem, but for fixed point, the interval crosses an
    ** exponent boundary.  It is much easier in fixed point to normalize
    ** between 1/4 and 1, with the result in the interval ( 1, 2 ] ( We
    ** actually force the interval to be (1, 2)).  The following table
    ** summerizes the "skew" problem and solution:
    **
    **		Exponent				 "Swapped"
    **		 parity	    table gives	   We want	table gives
    **		---------  ------------  ------------	------------
    **		   even	    1/sqrt(f)    1/sqrt(f)	1/sqrt(2f)
    **		   odd	   1/sqrt(2f)    sqrt2/sqrt(f)	1/sqrt(f)
    **
    ** The sqrt table is divide into two halves, depending on the parity
    ** of the exponent.  If we "swap" the sense of the parity, we can
    ** get the result we need by multiplying by sqrt(2).
    */

    index = msd >> (BITS_PER_UX_FRACTION_DIGIT_TYPE - NUM_FRAC_BITS - 1);
    index = (index & MAKE_MASK(NUM_FRAC_BITS + 1, 0)) ^
                      (exponent_parity << NUM_FRAC_BITS);

    p = &D_SQRT_TABLE_NAME[ index ];
    g = ( (p->a)*(f*f) + ((p->b)*f + p->c) );
    g *= D_SQRT_TWO;


    /*
    ** Given g ~ 1/sqrt(f), we want to compute
    ** 
    **		g_lo = (g/8)*(1 - f*g^2)*(7 - 3*f*g^2)
    ** 
    ** However, computing g_lo is somewhat problematic, since there is
    ** a potential for a massive loss of significance when computing
    ** 1 - f*g^2.  To deal with this problem, we introduce the quantity, t,
    ** defined by:
    ** 
    ** 		t <-- (double)((float) f*g)
    **
    ** and reduce g to only 24 significant bits
    */

    f = f_hi + f_lo;
    t = (double) ((float) (f*g));
    g = (double) ((float) g);

    /*
    ** Note that f*g and t are essentially the same values up to about
    ** 24 bits.  Then we can write
    ** 
    **		w = 1 - f*g^2
    **	 	  = 1 - (f*g)*g
    ** 		  = 1 - [t + (f*g - t)]*g
    **	 	  = (1 - t*g) - (f*g - t)*g
    **	 	  ~ (1 - t*g) - [(f_hi + f_lo)*g - t]*g
    **	 	  = (1 - t*g) - [(f_hi*g - t) + f_lo*g]*g
    ** 
    ** Note that since g, t and f_hi have at most 24 significant bits,
    ** and since f*g ~ t, the products t*g and f_hi*g as well as the
    ** sums 1 - t*g and f_hi*g - t are all exact.  This basicly
    ** reduces the roundoff error in computing 1 - f*g^2 to a few lsbs.
    ** With the above in mind, we compute g_lo as
    ** 
    **
    **		g_lo = {g*[(7/16) - ((3/16)*f)*(g*g)]} * w
    */ 

    w    = D_GROUP(D_ONE - t*g) - D_GROUP(D_GROUP(f_hi*g - t) + f_lo*g)*g;
    g_lo =  (g*D_GROUP(D_SEVEN_EIGHTS - (D_THREE_EIGHTS*f)*(g*g)))*w;

    /*
    ** At this point, g + g_lo approximates 1/sqrt(f) to roughly 72 bits
    ** and g_hi contains at most 24 significant bits.
    **
    ** The next step is to convert g + g_lo to an unpacked x-float value
    ** rounding g + g_lo to 64 bits.  This is done a little differently
    ** depending on the size of the integer format.  Also, we must be careful
    ** to insure that the approximation is in the interval [1, 2)
    */

    msd = (UX_FRACTION_DIGIT_TYPE) (D_TWO_POW_24*g);

#   if NUM_UX_FRACTION_DIGITS == 2

        signed_digit = (UX_SIGNED_FRACTION_DIGIT_TYPE) (D_TWO_POW_75*g_lo);
        msd = (msd << 39) + (signed_digit >> 12) + ((signed_digit >> 11) & 1);
        tmp_digit = (msd & SET_BIT(62)) ?
           (UX_MSB - 1) : ((UX_FRACTION_DIGIT_TYPE) -1);
        msd = ((UX_SIGNED_FRACTION_DIGIT_TYPE) msd < 0) ? msd : tmp_digit;

#   else

#       error "Sqrt not NYI for 32 bit integers"        

#   endif

    /*
    ** Get the current approximation, s, into unpacked format and compute
    ** 3 - x*s^2
    */

    P_UX_SIGN(&s, 0);
    P_UX_EXPONENT(&s, 1-exponent);
    P_UX_MSD(&s, msd);
    P_UX_LSD(&s, 0);

    MULTIPLY(&s, x, &ux_tmp);
    MULTIPLY(&s, &ux_tmp, y);
    ADDSUB(UX_THREE, y, SUB | NO_NORMALIZATION, y);

    /*
    ** Depending on the evaluation, multiply by s/2 or x*s/2
    */

    MULTIPLY(y, (evaluation_type == EVALUATE_SQRT) ? &ux_tmp : &s, y);
    UX_DECR_EXPONENT(y, 1);
    
    return;
    }

#if !defined(C_UX_SQRT)
#   define C_UX_SQRT	__INTERNAL_NAME(C_ux_sqrt__)
#endif

#define RND_CHECK_MASK		MAKE_MASK(10, 4)
#define RND_CHECK_INC		SET_BIT(3)
#define HALF_LSB		SET_BIT(14)
#define CLEAR_EXTRA_BITS(l)	((l) & ~MAKE_MASK(15,0))

static void
C_UX_SQRT(_X_FLOAT * packed_argument, WORD evaluation_code,
  _X_FLOAT * packed_result OPT_EXCEPTION_INFO_DECLARATION )
    {    
    WORD      fp_class;
    UX_FRACTION_DIGIT_TYPE lsd, tmp_digit;
    UX_FLOAT unpacked_argument, unpacked_result, diff, hi, lo;

    fp_class  = UNPACK(
        packed_argument,
        & unpacked_argument,
        (EVALUATE_SQRT == evaluation_code) ? SQRT_CLASS_TO_ACTION_MAP :
                                             RSQRT_CLASS_TO_ACTION_MAP,
        packed_result
        OPT_EXCEPTION_INFO_ARGUMENT );

    if (0 > fp_class)
        return;

    UX_SQRT_EVALUATION(&unpacked_argument, evaluation_code, &unpacked_result);

    if (EVALUATE_SQRT == evaluation_code)
        {
        /*
        ** Now we have to fool around with the low digit of the result to
        ** insure that correct rounding takes place.  If the result is 
        ** sufficiently far away from the half way case, the PACK routine will
        ** do the correct rounding.  However, "too close to call", we need
        ** to force the low bits of the result to insure PACK does the right
        ** thing.  We do this by looking at the sign of the difference
        ** argument - (result + 1/2lsb)^2.
        */

        NORMALIZE(&unpacked_result);
        lsd = G_UX_LSD(&unpacked_result);
        if ( 0 == ((lsd + RND_CHECK_INC) & RND_CHECK_MASK))
            { /* too close to the middle.  Check sign of difference */

            lsd = CLEAR_EXTRA_BITS(lsd);
            tmp_digit = lsd + HALF_LSB;
            P_UX_LSD(&unpacked_result, tmp_digit);
            EXTENDED_MULTIPLY(&unpacked_result, &unpacked_result, &hi, &lo);
            ADDSUB(&unpacked_argument, &hi, SUB, &diff);
            ADDSUB(&diff, &lo, SUB, &diff);
            lsd = G_UX_SIGN(&diff) ? lsd : tmp_digit;
            P_UX_LSD(&unpacked_result, lsd);
            } 
        }

    PACK(
        &unpacked_result,
        packed_result,
        NOT_USED,
        NOT_USED
        OPT_EXCEPTION_INFO_ARGUMENT );
    }

/*
** The following two routines implement the user level functions sqrtl and
** rsqrtl calling C_UX_SQRT
*/ 

#if !defined(F_ENTRY_NAME)
#   define F_ENTRY_NAME		F_SQRT_NAME
#endif

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    C_UX_SQRT(
        PASS_ARG_X_FLOAT(packed_argument),
        EVALUATE_SQRT,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);

    }

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_RSQRT_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    C_UX_SQRT(
        PASS_ARG_X_FLOAT(packed_argument),
        EVALUATE_RSQRT,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);

    }

/*
** UX_HYPOT computes the sqrt(x^2 + y^2) for unpacked x-float arguments
** x and y.
*/

#if !defined(UX_HYPOT)
#   define UX_HYPOT	__INTERNAL_NAME(ux_hypot__)
#endif

void
UX_HYPOT(
   UX_FLOAT * unpacked_x,
   UX_FLOAT * unpacked_y,
   UX_FLOAT * unpacked_result)
    {
    UX_FLOAT sum;

    MULTIPLY(unpacked_x, unpacked_x, &sum);
    MULTIPLY(unpacked_y, unpacked_y, unpacked_result);
    ADDSUB(unpacked_result, &sum, ADD, &sum);
    NORMALIZE(&sum);
    UX_SQRT(&sum, unpacked_result);
    return;
    }

/*
** F_HYPOT_NAME is the user lever hypot function
*/

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_HYPOT_NAME

X_XX_PROTO(F_ENTRY_NAME, packed_result, packed_x, packed_y)
    {
    WORD    fp_class;
    UX_FLOAT unpacked_x, unpacked_y, unpacked_result;
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    fp_class  = UNPACK2(
        PASS_ARG_X_FLOAT(packed_x),
        PASS_ARG_X_FLOAT(packed_y),
        & unpacked_x,
        & unpacked_y,
        HYPOT_CLASS_TO_ACTION_MAP,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO );

    if (0 > fp_class)
       RETURN_X_FLOAT(packed_result);

    UX_HYPOT( &unpacked_x, &unpacked_y, &unpacked_result);

    PACK(
        &unpacked_result,
        PASS_RET_X_FLOAT(packed_result),
        /* Not Used */ 0,
        CABS_OVERFLOW
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);

    }

#if defined(MAKE_INCLUDE)

    @divert -append divertText

#   undef TABLE_NAME

    START_TABLE;

    TABLE_COMMENT("Square root class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "SQRT_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(2) + 
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     0) );

    TABLE_COMMENT("Reciprocal square root class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "RSQRT_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) + 
              CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     4) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ERROR,     2) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_ERROR,     3));

    TABLE_COMMENT("Data for the above mapping");
        PRINT_U_TBL_ITEM( /* data 1 */  SQRT_OF_NEGATIVE );
        PRINT_U_TBL_ITEM( /* data 2 */ RSQRT_OF_POS_ZERO );
        PRINT_U_TBL_ITEM( /* data 3 */ RSQRT_OF_NEG_ZERO );
        PRINT_U_TBL_ITEM( /* data 4 */              ZERO );


    TABLE_COMMENT("Hypot(x,y) root class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "HYPOT_CLASS_TO_ACTION_MAP");

	  /* Index 0: mapping for x */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) + 
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) );

	  /* Index 1: class-to-index mapping */

    PRINT_64_TBL_ITEM( 
	      CLASS_TO_INDEX( F_C_POS_ZERO,   2) +
	      CLASS_TO_INDEX( F_C_NEG_ZERO ,  2) +
	      CLASS_TO_INDEX( F_C_POS_DENORM, 3) +
	      CLASS_TO_INDEX( F_C_NEG_DENORM, 3) +
	      CLASS_TO_INDEX( F_C_POS_NORM,   3) +
	      CLASS_TO_INDEX( F_C_NEG_NORM,   3) +
	      CLASS_TO_INDEX( F_C_POS_INF,    4) +
	      CLASS_TO_INDEX( F_C_NEG_INF,    4) );

	  /* Index 2: y class-to-index mapping for x = +/- zero */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) + 
	      CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ABSOLUTE,  1) +
	      CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_ABSOLUTE,  1) +
	      CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_ABSOLUTE,  1) +
	      CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_ABSOLUTE,  1) +
	      CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_ABSOLUTE,  1) +
	      CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_ABSOLUTE,  1) +
	      CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ABSOLUTE,  1) +
	      CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ABSOLUTE,  1) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1) );

	  /* Index 3: y class-to-index mapping for x = +/- norm or denorm */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) + 
	      CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ABSOLUTE,  0) +
	      CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_ABSOLUTE,  0) +
	      CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ABSOLUTE,  1) +
	      CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ABSOLUTE,  1) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1) );

	  /* Index 4: y class-to-index mapping for x = +/- Inf */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) + 
	      CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ABSOLUTE,  0) +
	      CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_ABSOLUTE,  0) +
	      CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_ABSOLUTE,  0) +
	      CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_ABSOLUTE,  0) +
	      CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_ABSOLUTE,  0) +
	      CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_ABSOLUTE,  0) +
	      CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ABSOLUTE,  0) +
	      CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ABSOLUTE,  0) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1) );

    PAD_IF_NEEDED(MP_BIT_OFFSET, 64);

    /* Print various powers of 2 */

    TABLE_COMMENT("2^n, n = .5, 0, 24, 75, -24, -77 in double precision");

    PRINT_R_TBL_VDEF_ITEM( "D_SQRT_TWO\t",               sqrt(2));
    PRINT_R_TBL_VDEF_ITEM( "D_ONE\t\t",                        1);
    PRINT_R_TBL_VDEF_ITEM( "D_TWO_POW_24\t",     bldexp(1,   24));
    PRINT_R_TBL_VDEF_ITEM( "D_TWO_POW_75\t",     bldexp(1,   75));
    PRINT_R_TBL_VDEF_ITEM( "D_RECIP_TWO_POW_24", bldexp(1,  -24));
    PRINT_R_TBL_VDEF_ITEM( "D_RECIP_TWO_POW_77", bldexp(1,  -77));

    TABLE_COMMENT(
       "Rsqrt iteration (double precision) constants: 7/8 and 3/8");

    PRINT_R_TBL_VDEF_ITEM( "D_SEVEN_EIGHTS", 7/8);
    PRINT_R_TBL_VDEF_ITEM( "D_THREE_EIGHTS", 3/8);

    TABLE_COMMENT("3 in unpacked format");
    PRINT_UX_TBL_ADEF_ITEM( "UX_THREE\t\t",             3);

    END_TABLE;

    @end_divert
    @eval my $tableText;						\
          my $outText    = MphocEval( GetStream( "divertText" ) );	\
          my $defineText = Egrep( "#define", $outText, \$tableText );	\
             $outText    = "$tableText\n\n$defineText";			\
          my $headerText = GetHeaderText( STR(BUILD_FILE_NAME),         \
                           "Definitions and constants square root " .	\
                              "related routines", __FILE__ );		\
             print "$headerText\n\n$outText\n";
#endif

