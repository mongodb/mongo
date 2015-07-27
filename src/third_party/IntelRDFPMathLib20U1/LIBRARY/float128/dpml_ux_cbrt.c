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

#define BASE_NAME	cbrt
#include "dpml_ux.h"

#if !defined(MAKE_INCLUDE)
#   include STR(BUILD_FILE_NAME)
#endif

/*
**  The algorithms used for the cbrt function are detailed in the X_FLOAT_NOTES
**  file (notes 18.*).
**
**  The basic approach is to factor the input x into f * 2^n, where 
**  1 <= f < 2  and  n = 3*m + i, where i = 0, 1, or 2.  Then 
**
**		cbrt(x) = cbrt(2^n * f)
**		        = cbrt(2^(3*m+i) * f)
**		        = 2^m * cbrt(2^i) * cbrt(f).
**
** To get cbrt(f), we do a poly approx y = P(f) good to about 15 bits, then
** perform one Newton's iterations in double precision to get 45 bits and then
** one Newton's iteration in unpacked format good to about 135 bits. We fetch
** 2^(i/3) from a table in double precision and incorporate during the 
** double precision Newton's iteration.  The result of the unpacked Newton's
** iteration is scaled by m and has its sign bit adjusted to get the final
** result.
**
** The poly coefficients and a small table of the roots, 2^(i/3), is generated
** from dpml_cbrt.c and is shared between this file and the routines generated
** form dpml_cbrt.c
**
** Given z, an approximation to 1/cbrt(f)^2, the double precision Newton's 
** iteration is of the form:
**
**  	y <--  z * f * (14 -  7 * z^3 * f^2  +  2 * z^6 * f^4 ) * 1/9
**
** and the unpacked iteration is:
**
**	       y    y^3 + 2*x
**	y <-- --- * ---------
**	       2    y^3 + x/2
**
**
**  Instead of unbiasing the exponent right away, we add and later subtract
**  small corrective quantities (ADD_ADJUST, SUB_ADJUST) to get rid of the
**  BIAS/3 exactly:
**
**    (true_expon + BIAS + ADD_ADJUST)*(1/3) - SUB_ADJUST =  true_expon/3
**
**  true_expon + BIAS >= 0, so we can do unsigned arithmetic, which has
**  better performance.
*/

#define SUB_ADJUST  (F_PRECISION + F_EXP_BIAS + 2)/3
#define ADD_ADJUST  (3*(SUB_ADJUST))

/*
** Instead of doing integer division, we can multiply by an integer that
** corresponds to 1/3 in "fixed point".
**
** If the number is small enough and in the right form, the compiler may
** optimize the multiply into shifts and adds.
*/

#define ONE_THIRD  0x1111
#define SHIFT_PROD 17
#define DIV_BY_3(num)   (( (10 * num) * ONE_THIRD  +  num) >> SHIFT_PROD)

#if !defined(F_ENTRY_NAME)
#   define F_ENTRY_NAME F_CBRT_NAME
#endif

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument)
    {
    DECLARE_X_FLOAT(packed_result) 
    WORD fp_class;
    UX_UNSIGNED_EXPONENT_TYPE m, i, j;
    UX_FRACTION_DIGIT_TYPE msd, tmp_digit;
    UX_FLOAT unpacked_argument, unpacked_result, y_cubed, tmp[2];
    D_UNION u;
    double y, f, z, f2, z2, z4;

    EXCEPTION_INFO_DECL

    INIT_EXCEPTION_INFO;
    fp_class  = UNPACK(
        PASS_ARG_X_FLOAT(packed_argument),
        & unpacked_argument,
        CBRT_CLASS_TO_ACTION_MAP,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO );

    if (0 >= fp_class)
       RETURN_X_FLOAT(packed_result);

    /*
    **  Get f as a double precision value z ~ 1/cbrt(f)^2 by a polynomial
    **  approximation
    */

    msd = G_UX_MSD(&unpacked_argument);
    u.D_HI_WORD = ((WORD)(D_EXP_BIAS-1) << D_EXP_POS) + (msd >> D_EXP_WIDTH);

#   if (BITS_PER_UX_FRACTION_DIGIT_TYPE == 32)

        tmp_digit = G_UX_2nd_MSD(&unpacked_argument);
        u.F_LO_WORD = (msd << (BITS_PER_UX_FRACTION_DIGIT_TYPE - D_EXP_WIDTH))
                       | (lsd >> D_EXP_WIDTH);

#   endif

    f = u.f;
    z = RECIP_CBRT_POLY(f);

    /* Get m and i */

    j = G_UX_EXPONENT(&unpacked_argument) + (ADD_ADJUST - 1);
    m = DIV_BY_3(j);
    i = j - 3*m;

    /*
    ** Now evaluate the Newton's iterations and incorporate the factor of
    ** 2^(i/3).  The grouping chosen here is an attempt to maximize parallelism
    ** and is probably not a good choice on a sequential machine
    */

    z2 = z*z;
    z4 = z2*z2;
    f2 = f*f;
    y = POW_CBRT_2_TABLE[i]*((((FOURTEEN_NINTHS*f)*z)
          - z4*((SEVEN_NINTHS*f)*f2))
            + (z4*(z2*z))*((TWO_NINTHS*f)*(f2*f2)));

    /* Convert the double precision result to unpacked x_float */

    u.f = y;
    msd = u.D_HI_WORD;

    P_UX_EXPONENT(&unpacked_result, (msd >> D_EXP_POS) + m
                                      - (D_EXP_BIAS + SUB_ADJUST - 1));
    P_UX_SIGN(&unpacked_result, G_UX_SIGN(&unpacked_argument));
    msd = (msd << D_EXP_WIDTH) | UX_MSB;

#   if (BITS_PER_UX_FRACTION_DIGIT_TYPE == 32)

        tmp_digit = u.F_LO_WORD;
        P_UX_2nd_MSD(&unpacked_result, tmp_digit << D_EXP_POS);
        msd |= (tmp_digit >> (BITS_PER_WORD - D_EXP_POS));
        P_UX_2nd_LSD(&unpacked_result, 0);
#   endif

    P_UX_MSD(&unpacked_result, msd);
    P_UX_LSD(&unpacked_result, 0);

    /* Do the Newton's iteration */

    MULTIPLY(&unpacked_result, &unpacked_result, &y_cubed);
    MULTIPLY(&unpacked_result, &y_cubed,         &y_cubed);

    UX_INCR_EXPONENT(&unpacked_argument, 1);	/* 2*x */
    ADDSUB(&y_cubed, &unpacked_argument, ADD, &tmp[0]);

    UX_DECR_EXPONENT(&unpacked_argument, 2);	/* x/2 */
    ADDSUB(&y_cubed, &unpacked_argument, ADD, &tmp[1]);

    DIVIDE(&tmp[0], &tmp[1], FULL_PRECISION, &tmp[0]);

    MULTIPLY(&unpacked_result, &tmp[0], &unpacked_result);
    UX_DECR_EXPONENT(&unpacked_result, 1);

    PACK(
        &unpacked_result,
        PASS_RET_X_FLOAT(packed_result),
        NOT_USED,
        NOT_USED
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);

    }

#if defined(MAKE_INCLUDE)

    @divert -append divertText

    function recip_cbrt(z)
        {
        auto t;

        t = cbrt(z);
        return 1/(t * t);
        }


#   undef TABLE_NAME

    START_TABLE;

    TABLE_COMMENT("Cbrt root class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "CBRT_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) +
              CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     0) );

    /* Generate coefficients and polynomial form for 1/cbrt(f)^2 */

    PRINT_R_TBL_COM_ADEF("coefs to approx 1/cbrt(f)^2", "COEFS\t\t\t");
    remes(REMES_FIND_POLYNOMIAL + REMES_ABSOLUTE_WEIGHT + REMES_LINEAR_ARG,
        1.0, 2.0, recip_cbrt, 15, &degree,  &poly_coefs);
    for (i = 0; i <= degree ; i++)
        { PRINT_R_TBL_ITEM( poly_coefs[i] ); }
    GENPOLY(COEFS[%%d], RECIP_CBRT_POLY(x), degree);

    /* Now get powers of cbrt(2) */

    PRINT_R_TBL_COM_ADEF("cube roots of 2^i, i = 0, 1, 2","POW_CBRT_2_TABLE\t");

    c = cbrt(2);
    for( i = 0; i <= 2; i++)
        { PRINT_R_TBL_ITEM(c^i); }

    /* Last but not least, the Newton's iteration constants */

    TABLE_COMMENT("14/9, 7/9 and 2/9 in double precision");

    PRINT_R_TBL_VDEF_ITEM( "FOURTEEN_NINTHS\t\t", 14/9);
    PRINT_R_TBL_VDEF_ITEM( "SEVEN_NINTHS\t\t",     7/9);
    PRINT_R_TBL_VDEF_ITEM( "TWO_NINTHS\t\t",       2/9);

    END_TABLE;

    @end_divert
    @eval my $tableText;						\
          my $outText = MphocEval( GetStream( "divertText" ) );		\
          my $defineText = Egrep( "#define", $outText, \$tableText );	\
          my $polyText   = Egrep( STR(GENPOLY_EXECUTABLE), $tableText,	\
                                     \$tableText );			\
             $polyText   = GenPoly( $polyText );			\
             $outText    = "$tableText\n\n$defineText\n\n$polyText";	\
          my $headerText = GetHeaderText( STR(BUILD_FILE_NAME),         \
                           "Definitions and constants cbrt",  		\
                              __FILE__ );				\
             print "$headerText\n$outText";

#endif

