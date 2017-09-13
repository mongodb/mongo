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

#define	BASE_NAME	trig
#include "dpml_ux.h"

#if !defined(MAKE_INCLUDE)
#   include STR(BUILD_FILE_NAME)
#endif

/* 
** OVERVIEW
** --------
**
** The implementation of the trig functions is based on four support routines:
** two common evaluation routine (one for sin/cos/sind/cosd and one for
** tan/cot/tand/cotd) together with two argument reduction routines, one for
** radian arguments and one for degree arguments.
** 
** There are various reduction schemes that can be used for trigonometric
** functions.  The polynomial evaluation routines require that the terms in
** the series decrease in magnitude.  For the trig functions, this implies
** that an argument reduction scheme should return a reduce argument with
** magnitude less than or equal to pi/4 is an appropriate choice.  In
** particular, we assume that for a given value, x, the argument reduction
** scheme (for both radian and degrees) produces two integers, I1 and I and an
** unpacked floating point result, z, such that
** 
** 	x = (2*pi)*I1 + I*(pi/2) + z, 	|z| <= pi/4
** 
**		NOTE: having the degree reduction return the reduced
**		argument in radian permits the use of only one set
**		of polynomial coefficient and simplifies the evaluation
**		logic.
**
** The value of I we will refer to as the quadrant bits and z as the reduced
** argument.  We assume also that argument reduction routines returns both I
** and z to its caller.  (I1 is never needed in the subsequent computations,
** so it is not returned.)
** 
** The following table gives an estimate of the number of terms in a polynomial
** and rational approximation for each of the basic trig functions.  For
** rational approximations the degree of the numerator and denominator are
** presented as an ordered pair.  The approximation is assumed to be good to
** 128 bits for |x| <= pi/4.  The values in this table were extrapolated from
** the tables given in Hart et. al.
** 
** 			   approximation form
** 			------------------------
** 	function	polynomial	rational
** 	--------	----------	--------
** 	sin		    12		 (6, 6)
** 	cos		    12		 (6, 6)
** 	tan		    29		 (7, 7)
** 
** So from the above table, it seems most efficient to evaluate sin and cos via
** polynomials and evaluate tangent via a rational approximation.  So we assume
** that for |x| <= pi/4, we have polynomials, S, C, P and Q such that
** 
** 		sin(x) ~ x*S(x^2)
** 		cos(x) ~ C(x^2)
** 		tan(x) ~ x*P(x^2)/Q(x^2)
** 		cot(x) ~ Q(x^2)/*[x*P(x^2)]
** 
** Now, for any argument, x, given its reduced argument, z, and its quadrant
** bits, I, we can evaluate sin, cos, tan and cot of x according to Table 1.
** ( For brevity we denote z*P(z^2) by p, Q(z^2) by q, etc):
** 
** 				   Quadrant bits, I
** 				----------------------------
** 		function	  0	  1	  2	  3
** 		--------	-----	-----	-----	-----
** 		sin		  s	  c	 -s	 -c
** 		cos		  c	 -s	 -c	  s
** 		tan		 p/q	-q/p	 p/q	-q/p
** 		cot		 q/p	-p/q	 q/p	-p/q
** 
** 				   Table 1
** 				   -------
** 
**
** REDUCTION INTERFACE:
** --------------------
** 
** As mentioned earlier, the overall design of the the trig routines is
** dependent on two routines to do argument reduction.  The prototype for 
** these functions is;
** 
** 		WORD
** 		__reduce(
** 		    _UX_FLOAT * unpacked_argument,
** 		    INT_64      octant,
** 		    _UX_FLOAT * reduced_argument
** 		    )
** 
** Assuming that 'unpacked_argument' points to a _UX_FLOAT data item with value
** x, then the semantics of the reduction routines are to compute integers I1
** and I, and a floating point value, z, such that
** 
** 	x + octant*(CYCLE/4) = (2*CYCLE)*I1 + (CYCLE/2) + z,  |z| < CYCLE/4
** 
** Note that performing the reduction on x + octant*(CYCLE/4), rather than x,
** not only allows us to deal with the <name>_vo entry points easily, it also
** permits easy use of the identities cos(x) = sin(x + CYCLE/2) and cot(x) = 
** tan(CYCLE/2) to consolidate the overall processing.
** 
** 
** 
** EVALUATION INTERFACE:
** ---------------------
** 
** The prototypes for each of the two evaluation routines is;
** 
** 	void
** 	__trig_evaluate(
** 	    UX_FLOAT * unpacked_argument,
** 	    WORD       octant,
** 	    U_WORD     function_code,
** 	    UX_FLOAT * unpacked_result
** 	    );
** 
** The evaluation routines need not know whether the evaluation is for degrees
** because the appropriate reduction is done based on the value of
** function_code.
*/ 

#if !defined(UX_RADIAN_REDUCE)
#    define UX_RADIAN_REDUCE		__INTERNAL_NAME(ux_radian_reduce__)
#endif

/*
** The radian reduction code is rather large and has a rather detailed
** explanation.  Consequently, its contained in a separate file and is
** included here.
*/

#if !defined(MAKE_INCLUDE)
#   include "dpml_ux_radian_reduce.c"
#endif

/*
** UX_DEGREE_REDUCE performs argument reduction for degree arguments.  The
** reduction is performed in three phases:
**
**	(1) if |x| >= 2^141, reduce modulo 360 to a value less than 2^141
**	    by operating on the exponent field of x
**	(2) if |x| > 2^15, reduce modulo 360 to a value less that 2^15
**	    by operating on the integer portion of x
**	(3) if |x| < 2^15, compute I = nint(x/90) and the reduced argument
**	    as x - I*90
**
** The details of each of these phases is discussed in more detail in the
** code.
*/

#if !defined(UX_DEGREE_REDUCE)
#    define UX_DEGREE_REDUCE		__INTERNAL_NAME(ux_degree_reduce__)
#endif

static U_WORD
UX_DEGREE_REDUCE( UX_FLOAT * argument, WORD octant, UX_FLOAT * reduced_argument)
    {
    WORD cnt, digit_with_binary_pt, digit_num, w_tmp, quadrant;
    UX_SIGN_TYPE     sign;
    UX_EXPONENT_TYPE exponent, k;
    UX_FRACTION_DIGIT_TYPE current_digit, tmp_digit, sum_digit, borrow;


    sign = G_UX_SIGN(argument);
    exponent = G_UX_EXPONENT(argument);

    if (exponent > (UX_PRECISION + 14))
        {
        /*
        ** This is a very large argument.  We make use of the identity
        **
        **	8*(2^12)^(n+1) = 8*(136)^(n+1)	(mod 360)
        **	               = [8*(136)]*(136)^n
        **	               = (1088)*(136)^n
        **	               = 8*(136)^n	(mod 360)
        **
        ** Or employing induction, 8*(2^12)^n = 8 (mod 360)
        **
        ** If p is the precision of the data type, we begin by writing the
        ** input argument x as:
        **
        **	x = 2^n*f
        **	  = 2^(n-p)*(2^p*f)
        **	  = 2^(n-p)*F
        **
        ** where F = 2^p*f is an integer.  Now let k = floor((n - p - 3)/12)
        ** and r = n - p - 3 - 12*k.  Then
        **
        **	x = 2^(n-p)*F
        **	  = 2^(12k + r + 3)*F
        **	  = 8*2^(12k)]*(2^r*F)
        **	  = [8*(2^12)^k]*(2^r*F)
        **	  = 8*(2^r*F)			(mod 360)
        **	  = 2^(3 + r + p)*f
        **	  = 2^(n - 12*k)*f
        **
        ** So the approach is to find k and subtract 12*k from the exponent
        ** field.  This will reduce the input argument to a number less than
        ** 2^(p + 14)
        **
        ** One last note.  We don't actually do an integer divide to get
        ** k.  Rather we multiply n by an integer that is effectively the 
        ** reciprocal of 12.  This is easier to do if the exponent field
        ** is positive so we want to add a bias to the exponent that is
        ** divisible by 12 and that will force the exponent to be positive.
        ** We assume at this point that |exponent| < (1 << F_EXP_WIDTH).
        **
        ** Let the bias = 12*B, then
        **
        **		k = floor((n - p - 3)/12)
        **		  = floor((n - p - 3 + 12*B - 12*B)/12)
        **		  = floor((n - p - 3 + 12*B)/12 - B)
        **		  = floor((n - p - 3 + 12*B)/12) - B
        **		  = floor((n + (12*B - p - 3))/12) - B
        ** 
        ** 	==> n - 12*k = n - 12*[floor((n + (12*B - p - 3))/12) - B]
        ** 	             = n - 12*floor((n + (12*B - p - 3))/12) - 12*B
        */

#       define BIAS	(12*(((1 << F_EXP_WIDTH) + 11)/12))

        exponent += (BIAS - UX_PRECISION - 3);
        UMULH((UX_FRACTION_DIGIT_TYPE) exponent, RECIP_TWELVE, k);
        exponent = (exponent + (UX_PRECISION + 3)) - 12*k;
        P_UX_EXPONENT(argument, exponent);
        }

    if (exponent >= 16)
        {
        /*
        ** For a medium arguments, 2^15 < |x| < 2^142, we consider the fraction
        ** field of x as a sequence of digit.  The digits that are comprised
        ** entirely of "integer" bits are reduced modulo 360 using the 
        ** identity 8*2^12 = 8 (mod 360).
        **
        ** Begin by writing |x| = 2^n*f, with f in the interval [1/2, 1) and
        ** define s = (n - 15) % k, where k is the number of bits per fraction
        ** digit.  If there are 4 digits per UX_FLOAT, then the following
        ** diagram indicates the relationship between n, s and the binary point
        ** of x:
        **
        **                  |<---------- n - 15 -------->| 15 |<--
        **	            +-----------+-----------+-----------+-----------+
        **             f :  |     F1    |     F2    |    F3     |    F4     | 
        **	            +-----------+-----------+-----------+-----------+
        **	                                 -->|  s |<-- ^
        **                                                 binary pt
        **
        ** Suppose we now shift the bits of f, s bits to the left to get f'.
        ** Then the diagram would look like
        **
        **	                                 -->| 15 |<--
        **	+-----------+-----------+-----------+-----------+-----------+
        **  f': |     F0'   |     F1'   |     F2'   |     F3'   |    F4'    | 
        **	+-----------+-----------+-----------+-----------+-----------+
        **	                                         ^
        **                                            binary pt
        **
        ** and if we denote the number of digits per UX_FLOAT by N, then
        **
        **	x = 2^(n-s)*(F0' + F1'/K + F2'/K^2 + ... + F4'/K^N)
        **
        ** Now n - 15 - s is multiple of k, i.e. n - s = j*k + 15, so that
        ** 2^(n-s) = 2^(j*k+15) = 2^15*K^j and
        **
        **	x = 2^(n-s)*(F0' + F1'/K + F2'/K^2 + ... + FN'/K^N)
        **	  = 2^15*(K^j)*(F0' + F1'/K + F2'/K^2 + .... + FN'/K^N)
        **	  = 2^15*[F0'*K^j + F1'*K^(j-1) + ... + FN'/K^(j-N)]
        **	  = 2^15*A + 2^15*b
        **
        **	A = F0'*K^j + F1'*K^(j-1) + ... + Fj
        **	b = Fj+1'/K + ... + FN'/K^(N-j)
        **
        ** If we denote B = trunc(2^12*b) as B and b' = 2^15*b - 2^3*B, then
        **
        **	x = 2^15*A + 2^15*b
        **	  = 2^15*A + 2^3*B + b'
        **	  = 2^15*A + 2^3*B + b'
        **        = 8*(2^12*A + B) + b'
        **	  = 8*C + b'
        **
        ** Now let C_lo be the low 12 bits of C and C_hi be the remaining
        ** bits, then
        **
        **	8*C = 8*(C_lo + 2^12*C_hi)
        **	    = 8*(C_lo + 136*C_hi)	(mod 360)
        **	    = 8*C_lo + 8*136*C_hi)
        **	    = 8*C_lo + 8*C_hi)		(mod 360)
        **	    = 8*(C_lo + C_hi)
        **
        ** Thus we effectively reduced the value of 8*C by (almost) 12 bits
        ** modulo 360.  Obviously, we can iterate on this process until until
        ** we produce a value C' which is less that 2^12 and 8*C' = 8*C modulo
        ** 360.  In order to increase performance (and simplify the
        ** implementation) the actual code below doesn't do the reduction 12
        ** bits at a time initially.  Rather it first does the reduction 24 or
        ** 60 bits bits at a time (depending on the digit size), and then does
        ** 12 bit reduction on that result.
        **
        **	NOTE: In order to avoid copying the input argument to
        **	a work buffer and to simplify the logic, the follow code
        **	overlays the sign and exponent field of a UX_FLOAT type
        **	with an "extra" digit.
        */

#       if BITS_PER_UX_FRACTION_DIGIT_TYPE > (BITS_PER_UX_EXPONENT_TYPE + \
           BITS_PER_UX_SIGN_TYPE)
#           error "Need work buffer for this UX_FLOAT struct"
#       endif

        digit_with_binary_pt = exponent - 15;
        cnt = digit_with_binary_pt & (BITS_PER_UX_FRACTION_DIGIT_TYPE - 1);
        digit_with_binary_pt >>= __LOG2(BITS_PER_UX_FRACTION_DIGIT_TYPE);
        tmp_digit = 0;
        exponent -= cnt;

        if (cnt)
            { /* shift digit right (in memory) */
            w_tmp = BITS_PER_UX_FRACTION_DIGIT_TYPE - cnt;

            current_digit = G_UX_LSD(argument);
            P_UX_LSD(argument, current_digit << cnt);

#           if NUM_UX_FRACTION_DIGITS == 4

                tmp_digit = G_UX_FRACTION_DIGIT(argument, 2);
                P_UX_FRACTION_DIGIT(argument, 2,
                        (tmp_digit << cnt) | ( current_digit >> w_tmp));

                current_digit = G_UX_FRACTION_DIGIT(argument, 1);
                P_UX_FRACTION_DIGIT(argument, 1,
                        (current_digit << cnt) | ( tmpt_digit >> w_tmp));

#           endif

            tmp_digit = G_UX_MSD(argument);
            P_UX_MSD(argument,
                (tmp_digit << cnt) | ( current_digit >> w_tmp));
            tmp_digit >>= w_tmp;
            }
     /*   P_UX_FRACTION_DIGIT(argument, -1, tmp_digit); */
        /*
        ** Because of the compiler warning we are replacing the above
        ** line in the source.
        */

            *(&(((UX_FLOAT*)(argument))->fraction[0])-1) = tmp_digit;

        /*
        ** Extract B from the digit that contains the binary point
        */

        sum_digit = G_UX_FRACTION_DIGIT(argument, digit_with_binary_pt) >>
                      (BITS_PER_UX_FRACTION_DIGIT_TYPE - 12);

        /*
        ** Loop through the remaining integer digits and add them to B
        */

#       define MOD_360_BITS_PER_DIGIT (12*(BITS_PER_UX_FRACTION_DIGIT_TYPE/12))
#       define MOD_360_DIGIT_MASK     MAKE_MASK(MOD_360_BITS_PER_DIGIT, 0)

        digit_num = digit_with_binary_pt;
        cnt = 0;
        while (digit_num >= 0)
            {
            current_digit = G_UX_FRACTION_DIGIT(argument, --digit_num);
            P_UX_FRACTION_DIGIT(argument, digit_num, 0);
            if (cnt)
                {
                sum_digit += ((current_digit << cnt) & 0xfff);
                w_tmp = 12 - cnt;
                current_digit >>= w_tmp;
                cnt = -w_tmp;
                }

            sum_digit = (sum_digit + (current_digit & MOD_360_DIGIT_MASK))
                         + (current_digit >> MOD_360_BITS_PER_DIGIT);
            cnt += (BITS_PER_UX_FRACTION_DIGIT_TYPE - MOD_360_BITS_PER_DIGIT);
            }

        /*
        ** For 64 bit digits, at this point sum_digit can have five 12 bit
        ** "digits" plus a carry "digit" for a total of six.  So it is
        ** more efficient to compress sum_digit 24 bits at a time rather than
        ** 12 bits at a time.
        */

#       if (BITS_PER_UX_FRACTION_DIGIT_TYPE == 64)

            sum_digit = (sum_digit & 0xffffff) + ((sum_digit >> 24) & 0xffffff)
                          + ((sum_digit >> 48) & 0xffffff);

#       endif

        /*
        ** At this point sum_digit may contain two 12 bit "digits" plus a
        ** carry "digit".  So we recurse (at most twice) to reduce it to 12
        ** bits modulo 360.
        */

        while ((tmp_digit = (sum_digit >> 12)))
            sum_digit = (sum_digit & 0xfff) + tmp_digit;

        /*
        ** Now put the reduced integer into the original fraction field,
        ** normalize the result, and calculate the exponent value.
        */

        current_digit = G_UX_FRACTION_DIGIT(argument, digit_with_binary_pt);
        current_digit &= MAKE_MASK(BITS_PER_UX_FRACTION_DIGIT_TYPE - 12, 0);
        current_digit |= (sum_digit << (BITS_PER_UX_FRACTION_DIGIT_TYPE - 12));
        P_UX_FRACTION_DIGIT(argument, digit_with_binary_pt, current_digit);
        P_UX_EXPONENT(argument, exponent);

        exponent -= NORMALIZE(argument);
        }


    /*
    ** At this point |x| < 2^15 so that if I = nint(x/90), I < 2^9 and 
    ** I*90 requires at most 15 significant bits.  This means that we
    ** can reduce x by working only with its most significant digit.
    **
    ** Let F be the high k bits of the fraction of x, where k is the number
    ** of bits per fraction digit and K = 2^k.  Further, let R an k-1 bit
    ** integer such that 1/90 ~ R/(32*K).  (I.e. R is the high bits of 1/90
    ** unnormalized by one bit.)  We can now write x = 2^n*(F + e)/K and 
    ** 1/90 = (R + d)/(32*K), where |e| < 1 and |d| < 1/2.  Consequently
    ** we have:
    **
    **		x/90 = (2^n*f)*(1/90)
    **		     = 2^n*[(F + e)/K]*[(R + d)/(32*K)]
    **		     = 2^(n-5)*(F*R + e*R + d*F + e*d)/K^2
    **		     = 2^(n-5)*(K*hi(F*R) + lo(F*R) + e*R + d*F + e*d)/K^2
    **
    ** Now K*hi(F*R) > K^2/8 and | lo(F*R) + e*R + d*F + e*d | < 2K and
    ** so the relative error in neglecting lo(F*R) + e*R + d*F + e*d is less
    ** that one part in 2^(k-4).  Since k is at least 32, the relative error
    ** is very small.  We have then
    **
    **		x/90 = 2^(n-5)*[K*hi(F*R) + lo(F*R) + e*R + d*F + e*d]/K^2
    **		     ~ 2^(n-5)*hi(F*R)/K
    */

    w_tmp = exponent - 5;
    P_UX_SIGN(argument, 0);
    current_digit = G_UX_MSD(argument);

    if (w_tmp > 0)
        { UMULH( current_digit, MSD_OF_RECIP_90, tmp_digit); }
    else
        { /* I = 0 */
        w_tmp = 1;
        tmp_digit = 0;
        }

    /* I ~ x/90, "add in octant" and round to nearest integer */

    cnt = BITS_PER_UX_FRACTION_DIGIT_TYPE - w_tmp;
    tmp_digit = (tmp_digit + ((octant & 1) << (cnt - 1)) +
                        SET_BIT(cnt - 1)) & ~MAKE_MASK(cnt, 0);

    /* Get quadrant bits and adjust for sign of the argument */

    quadrant = (tmp_digit >> cnt);
    quadrant = (sign) ? -quadrant : quadrant;
    quadrant += (octant >> 1);

    /* now subtract I*90 from x */

#   define MSD_OF_NINETY	(((UX_FRACTION_DIGIT_TYPE) 45) << \
				(BITS_PER_UX_FRACTION_DIGIT_TYPE - 6))

    UMULH(tmp_digit, MSD_OF_NINETY, tmp_digit);
    tmp_digit = (current_digit >> 2) - tmp_digit;
    current_digit = (current_digit & 3) | (4*tmp_digit);
    if (((UX_SIGNED_FRACTION_DIGIT_TYPE) tmp_digit) < 0)
        {
        sign ^= UX_SIGN_BIT;

        sum_digit = G_UX_LSD(argument);
        tmp_digit = -sum_digit;
        borrow = (sum_digit != 0);
        P_UX_LSD(argument, tmp_digit);

#       if ( NUM_UX_FRACTION_DIGITS == 4)

            sum_digit = G_UX_FRACTION_DIGIT(argument, 2);
            tmp_digit = - (sum_digit + borrow);
            borrow = (sum_digit != 0) | borrow;
            P_UX_FRACTION_DIGIT(argument, 2, tmp_digit);

            sum_digit = G_UX_FRACTION_DIGIT(argument, 1);
            tmp_digit = - (sum_digit + borrow);
            borrow = (sum_digit != 0) | borrow;
            P_UX_FRACTION_DIGIT(argument, 1, tmp_digit);

#       endif

        current_digit = - (current_digit + borrow);
        }
    P_UX_MSD(argument, current_digit);
    NORMALIZE(argument);

    /* Last by not least, convert to radians */

    MULTIPLY(argument, UX_PI_OVER_180, reduced_argument);
    UX_TOGGLE_SIGN(reduced_argument, sign);

    return quadrant;
    }

/*
** UX_SINCOS is the common evaluation routine for all of the sin/cos and
** sind/cosd entry points.  UX_SINCOS invokes the appropriate reduction
** routine (radian or degrees) and then performs 1 or 2 polynomial evaluation
** on the reduced argument to get the result (or results, for sincos and
** sincosd)
*/

#define ODD_POLY_FLAGS		SQUARE_TERM | ALTERNATE_SIGN | POST_MULTIPLY
#define EVEN_POLY_FLAGS		SQUARE_TERM | ALTERNATE_SIGN

#define SIN_POLY_FLAGS		NUMERATOR_FLAGS( ODD_POLY_FLAGS )
#define COS_POLY_FLAGS		DENOMINATOR_FLAGS( EVEN_POLY_FLAGS )

WORD
UX_SINCOS(
  UX_FLOAT * unpacked_argument,
  WORD       octant,
  WORD       function_code,
  UX_FLOAT * unpacked_result)
    {
    WORD quadrant, poly_type;
    UX_FLOAT reduced_argument;
    U_WORD (* reduce)( UX_FLOAT *, WORD, UX_FLOAT *);

    /* Get the quadrant bits and the reduced argument */

    reduce = (function_code & DEGREE) ? UX_DEGREE_REDUCE : UX_RADIAN_REDUCE;
    quadrant = reduce( unpacked_argument, octant, &reduced_argument );
    function_code &= ~DEGREE;

    /*
    ** Select the polynomial coefficients and the form of the 
    ** polynomial based on the quadrant the reduced argument
    ** lies in.  NOTE: the difference between the sin and cos
    ** has been accounted for in the value of octant.
    */

    if ( SINCOS_FUNC == function_code )
        {
        poly_type = SIN_POLY_FLAGS | COS_POLY_FLAGS | NO_DIVIDE;

        /* Adjust location of sin/cos polynomials */
        poly_type |= ( (quadrant & 1) ? SWAP : NULL );
        }
    else if (quadrant & 1)
        /* We need to evaluate C(x^2) */
        poly_type = SKIP | COS_POLY_FLAGS;
    else 
        /* We need to evaluate x*S(x^2) */
        poly_type = SKIP | SIN_POLY_FLAGS;

    /*
    ** Evaluate the polynomial and set the sign based on the quadrant
    */

    EVALUATE_RATIONAL(
        &reduced_argument,
        SINCOS_COEF_ARRAY,
        SINCOS_COEF_ARRAY_DEGREE,
        poly_type,
        unpacked_result);

    if (quadrant & 2)
        UX_TOGGLE_SIGN(&unpacked_result[0], UX_SIGN_BIT);

    /*
    ** If this is a sincos entry point, set the sign on the second
    ** result
    */

    if ((SINCOS_FUNC == function_code) && ((quadrant + 1) & 2))
        UX_TOGGLE_SIGN(&unpacked_result[1], UX_SIGN_BIT);
  
    return 0; /* No error conditions for sin/cos */
    }


/*
** UX_TANCOT is the common evaluation routine fo tan, cot, tand and cotd.
** UX_TANCOT invokes the appropriate reduction routine (radian or degrees) and
** then computes tan or cot as the ratio of two polynomials
**
** An important difference between UX_TANCOT and UX_SINCOS is that for the
** tand/cotd routines, the reduced argument may be zero.  Depending on the
** quadrant bits, the correct result would then be either 0 or +/- Inf.   The
** common tan/cot evaluation routine detects the +/- Inf case and returns an
** unpacked result with its exponent field set to a large positive value,
** denoted by UX_INFINITY_EXPONENT.
*/

#if !defined(UX_TANCOT)
#   define UX_TANCOT		__INTERNAL_NAME(ux_tancot__)
#endif

static WORD
UX_TANCOT(
  UX_FLOAT * unpacked_argument,
  WORD       octant,
  WORD       function_code,
  UX_FLOAT * unpacked_result)
    {
    WORD quadrant, div_flag;
    UX_FLOAT reduced_argument;
    U_WORD (* reduce)(UX_FLOAT *, WORD, UX_FLOAT *);

    /*
    ** Get the quadrant bits and the reduced argument, check for
    ** zero and process accordingly.
    */

    reduce = (function_code & DEGREE) ? UX_DEGREE_REDUCE : UX_RADIAN_REDUCE;
    quadrant = reduce( unpacked_argument, octant, &reduced_argument );
    div_flag = ((quadrant + (function_code >> 3)) & 1) ? SWAP : 0;

    if (0 == G_UX_MSD(&reduced_argument))
        { /* reduced argument is zero */
        UX_SET_SIGN_EXP_MSD(unpacked_result, 0, UX_ZERO_EXPONENT, 0);
	if ( div_flag /* == SWAP */ )
            {
            P_UX_EXPONENT(unpacked_result, UX_INFINITY_EXPONENT);
            P_UX_MSD(unpacked_result, UX_MSB);
            }
        return (function_code & TAN_FUNC) ?
           TAND_ODD_MULTIPLE_OF_90 : COTD_MULTIPLE_OF_180;
        }

    /*
    ** Evaluate z*P(z^2) and and Q(z^2) and perform the appropriate
    ** division.  Set the sign bit according to the quadrant.
    */

    EVALUATE_RATIONAL(
        &reduced_argument,
        TANCOT_COEF_ARRAY,
        TANCOT_COEF_ARRAY_DEGREE,
        NUMERATOR_FLAGS( SQUARE_TERM | ALTERNATE_SIGN | POST_MULTIPLY) |
          DENOMINATOR_FLAGS( SQUARE_TERM | ALTERNATE_SIGN) | div_flag,
        unpacked_result);

    if (quadrant & 1)
        UX_TOGGLE_SIGN(unpacked_result, UX_SIGN_BIT);

    return G_UX_SIGN(unpacked_result) ? COTD_NEG_OVERFLOW : COTD_POS_OVERFLOW;
    }

/*
** Each of the of trig routines call a common routine C_UX_TRIG, to unpack the
** input argument and then dispatch the result to UX_SINCOS or UX_TANCOT
** evaluation routine. For sincos and sincosd entry points, if the return
** value is written by the unpack routine, the common routine must take care
** to write the second result.
*/

#if !defined(C_UX_TRIG)
#   define C_UX_TRIG		__INTERNAL_NAME(C_ux_trig__)
#endif

#define F_C_NAN_OR_INF_MASK	(SET_BIT(F_C_INF) | SET_BIT(F_C_NAN))

static void
C_UX_TRIG(
  _X_FLOAT * packed_argument,
  WORD octant,
  WORD function_code,
  U_WORD const * class_to_action_map,
  WORD underflow_error,
  _X_FLOAT * packed_result
  OPT_EXCEPTION_INFO_DECLARATION )
    {
    _X_FLOAT *second_value;
    WORD fp_class, overflow_error;
    UX_FLOAT unpacked_result[3], unpacked_argument;
    WORD (* trig_eval)( UX_FLOAT *, WORD, WORD, UX_FLOAT *);

    trig_eval = (SINCOS_FUNC & function_code) ? UX_SINCOS : UX_TANCOT;

    fp_class = UNPACK(
        packed_argument,
        &unpacked_argument,
        class_to_action_map,
        packed_result
        OPT_EXCEPTION_INFO_ARGUMENT );

   if (0 > fp_class)
        { /* If this is a SINCOS evaluation, write second result */

        if (SINCOS_FUNC == (function_code & ~DEGREE))
            {
            second_value =
                ((1 << F_C_BASE_CLASS(fp_class)) & F_C_NAN_OR_INF_MASK) ?
                &packed_result[0] : (_X_FLOAT *) _X_ONE;
            _X_COPY(second_value, &packed_result[1]);
            }
        return;
        }

    overflow_error = trig_eval(
        &unpacked_argument,
        octant,
	function_code,
        unpacked_result);

    PACK(
        unpacked_result,
        packed_result,
        underflow_error,
        overflow_error
        OPT_EXCEPTION_INFO_ARGUMENT );

    if (SINCOS_FUNC == (function_code & ~DEGREE))
        { /* pack second result for sincos evaluations */
        PACK(
            unpacked_result + 1,
            packed_result + 1,
            NOT_USED,
            NOT_USED
            OPT_EXCEPTION_INFO_ARGUMENT );
        }
    }

/*
** The following 6 entry points implement the user level x-float sin/cos and
** sind/cosd functions
*/

#define TRIG_ENTRY(oct, code, map, under)				 \
        X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument)          \
	    {								 \
	    EXCEPTION_INFO_DECL				                 \
            DECLARE_X_FLOAT(packed_result)                               \
									 \
	    INIT_EXCEPTION_INFO;					 \
	    C_UX_TRIG(							 \
            PASS_ARG_X_FLOAT(packed_argument),               \
	        oct, code, map, under,					 \
            PASS_RET_X_FLOAT(packed_result)              \
	        OPT_EXCEPTION_INFO);					 \
            RETURN_X_FLOAT(packed_result);                               \
	    }

#
#define TRIG_ENTRY_RR(oct, code, map, under)                 \
        RR_X_PROTO(F_ENTRY_NAME, packed_result1, packed_result2, packed_argument)          \
        {                                \
        EXCEPTION_INFO_DECL                              \
        _X_FLOAT packed_result[2];                               \
                                     \
        INIT_EXCEPTION_INFO;                     \
        C_UX_TRIG(                           \
            PASS_ARG_X_FLOAT(packed_argument),               \
            oct, code, map, under,                   \
            packed_result /*PASS_RET_X_FLOAT(packed_result)*/                \
            OPT_EXCEPTION_INFO);                     \
        *packed_result1 = packed_result[0];                              \
        *packed_result2 = packed_result[1];                              \
        }


#undef  F_ENTRY_NAME
#define F_ENTRY_NAME F_SIN_NAME
	TRIG_ENTRY(0, SIN_FUNC, SIN_CLASS_TO_ACTION_MAP, NOT_USED)

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME F_COS_NAME
	TRIG_ENTRY(2, COS_FUNC, COS_CLASS_TO_ACTION_MAP, NOT_USED)

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME F_SINCOS_NAME
    TRIG_ENTRY_RR(0, SINCOS_FUNC, SINCOS_CLASS_TO_ACTION_MAP, NOT_USED)


#undef  F_ENTRY_NAME
#define F_ENTRY_NAME F_SIND_NAME
	TRIG_ENTRY(0, SIND_FUNC, SIND_CLASS_TO_ACTION_MAP, SIND_UNDERFLOW)

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME F_COSD_NAME
	TRIG_ENTRY(2, COSD_FUNC, COSD_CLASS_TO_ACTION_MAP, NOT_USED)

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME F_SINCOSD_NAME
    TRIG_ENTRY_RR(0, SINCOSD_FUNC, SINCOSD_CLASS_TO_ACTION_MAP, SIND_UNDERFLOW)


/*
** The following 4 entry points implement the user level x-float tan/cot and
** tand/cotd functions
*/

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME F_TAN_NAME
	TRIG_ENTRY(0, TAN_FUNC, TAN_CLASS_TO_ACTION_MAP, NOT_USED)

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME F_COT_NAME
	TRIG_ENTRY(0, COT_FUNC, COT_CLASS_TO_ACTION_MAP, NOT_USED)

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME F_TAND_NAME
	TRIG_ENTRY(0, TAND_FUNC, TAND_CLASS_TO_ACTION_MAP, TAND_UNDERFLOW)

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME F_COTD_NAME
	TRIG_ENTRY(0, COTD_FUNC, COTD_CLASS_TO_ACTION_MAP, NOT_USED)


#if defined(MAKE_INCLUDE)

    @divert -append divertText

    precision = ceil(UX_PRECISION/8) + 4;

#   undef TABLE_NAME

    START_TABLE;

    TABLE_COMMENT("sin class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "SIN_CLASS_TO_ACTION_MAP\t");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(6) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ERROR,     2) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     2) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     0) );

    TABLE_COMMENT("cos class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "COS_CLASS_TO_ACTION_MAP\t");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(5) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ERROR,     3) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     3) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     1) );

    TABLE_COMMENT("sincos class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "SINCOS_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(4) +
              CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ERROR,     4) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     4) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     0) );

    TABLE_COMMENT("sind class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "SIND_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(3) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ERROR,     5) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     5) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     0) );

    TABLE_COMMENT("cosd class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "COSD_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(2) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ERROR,     6) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     6) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     1) );

    TABLE_COMMENT("sincosd class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "SINCOSD_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) +
              CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ERROR,     7) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     7) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     0) );

    TABLE_COMMENT("Data for the above mappings");

        PRINT_U_TBL_ITEM( /* data 1 */                 ONE);
        PRINT_U_TBL_ITEM( /* data 2 */     SIN_OF_INFINITY);
        PRINT_U_TBL_ITEM( /* data 3 */     COS_OF_INFINITY);
        PRINT_U_TBL_ITEM( /* data 4 */  SINCOS_OF_INFINITY);
        PRINT_U_TBL_ITEM( /* data 5 */    SIND_OF_INFINITY);
        PRINT_U_TBL_ITEM( /* data 6 */    COSD_OF_INFINITY);
        PRINT_U_TBL_ITEM( /* data 7 */ SINCOSD_OF_INFINITY);

    TABLE_COMMENT("tan class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "TAN_CLASS_TO_ACTION_MAP\t");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) +
              CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     0) );
        PRINT_U_TBL_ITEM( /* data 1 */ TAN_OF_INFINITY);

    TABLE_COMMENT("tand class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "TAND_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) +
              CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     0) );
        PRINT_U_TBL_ITEM( /* data 1 */ TAND_OF_INFINITY);


    TABLE_COMMENT("cot class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "COT_CLASS_TO_ACTION_MAP\t");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) +
              CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ERROR,     2) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_ERROR,     3) );
        PRINT_U_TBL_ITEM( /* data 1 */ COT_OF_INFINITY);
        PRINT_U_TBL_ITEM( /* data 2 */     COT_OF_ZERO);
        PRINT_U_TBL_ITEM( /* data 3 */ COT_OF_NEG_ZERO);

    TABLE_COMMENT("cotd class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "COTD_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) +
              CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_ERROR,     2) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_ERROR,     3) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ERROR,     4) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_ERROR,     5) );
        PRINT_U_TBL_ITEM( /* data 1 */  COTD_OF_INFINITY);
        PRINT_U_TBL_ITEM( /* data 2 */ COTD_POS_OVERFLOW);
        PRINT_U_TBL_ITEM( /* data 3 */ COTD_NEG_OVERFLOW);
        PRINT_U_TBL_ITEM( /* data 4 */      COTD_OF_ZERO);
        PRINT_U_TBL_ITEM( /* data 5 */  COTD_OF_NEG_ZERO);

    TABLE_COMMENT("Unpacked constants pi/180");
    PRINT_UX_TBL_ADEF_ITEM( "UX_PI_OVER_180\t\t",  pi/180);

    TABLE_COMMENT("Packed constants 1");
    PRINT_F_TBL_ADEF_ITEM( "_X_ONE\t\t\t",  1);

    /*
    ** Now we compute the "high" digit of 1/90 and 1/12.  For 1/12, we would
    ** to compute and integer R, such that trunc(E/12) = UMULH(R*E).  We
    ** state without proof here that if the number of bits per digit is
    ** 2*k + d, where d = 0 or 1, then N = 2^(2*k+d) + 2^(3-d) is divisible
    ** by 12 and taking R = N/12 gives the appropriate result.
    */

    PRINT_UX_FRACTION_DIGIT_TBL_VDEF_ITEM( "MSD_OF_RECIP_90\t\t", 
        nint(bldexp(1/90, BITS_PER_UX_FRACTION_DIGIT_TYPE + 5)));

    PRINT_UX_FRACTION_DIGIT_TBL_VDEF_ITEM( "RECIP_TWELVE\t\t", 
        ceil(bldexp(1/12, BITS_PER_UX_FRACTION_DIGIT_TYPE)));

    /*
    ** Now generate coefficients for computing sin.
    */

    function __sin(x)
        {
        if (x == 0)
            return 1;
        else
            return sin(x)/x;
        }

    save_precision = precision;
    precision = ceil(UX_PRECISION/8) + 8;

    max_arg = pi/4;

    remes(REMES_FIND_POLYNOMIAL + REMES_RELATIVE_WEIGHT + REMES_SQUARE_ARG,
           0, max_arg, __sin, UX_PRECISION, &sin_degree, &ux_rational_coefs);

    /*
    ** Now generate coefficients for computing cos and add them to the
    ** ux_rational coefficient array so that they can be accessed by the
    ** rational evaluation routine.
    */

    function __cos(x) { return cos(x); }

    remes(REMES_FIND_POLYNOMIAL + REMES_RELATIVE_WEIGHT + REMES_SQUARE_ARG,
           0, max_arg, __cos, UX_PRECISION, &cos_degree, &dummy_coefs);

    precision = save_precision;

    k = sin_degree + 1;
    for (i = 0; i <= cos_degree; i++)
        ux_rational_coefs[k++] = dummy_coefs[i];

    TABLE_COMMENT("Fixed point coefficients for sin and cos evaluation");
    PRINT_FIXED_128_TBL_ADEF("SINCOS_COEF_ARRAY\t");
    degree = print_ux_rational_coefs(sin_degree, cos_degree, 0);
    PRINT_WORD_DEF("SINCOS_COEF_ARRAY_DEGREE", degree );

    /*
    ** Last but not least, get the rational coefficients for tan/cot
    */

    function __tan(x)
        {
        if (x == 0)
            return 1;
        else
            return tan(x)/x;
        }

    save_precision = precision;
    precision = ceil(UX_PRECISION/8) + 8;

    max_arg = pi/4;

    remes(REMES_FIND_RATIONAL + REMES_RELATIVE_WEIGHT + REMES_SQUARE_ARG,
        0, max_arg, __tan, UX_PRECISION, &num_degree, &den_degree,
        &ux_rational_coefs);

    precision = save_precision;

    TABLE_COMMENT("Fixed point coefficients for tan and cot evaluation");
    PRINT_FIXED_128_TBL_ADEF("TANCOT_COEF_ARRAY\t");
    degree = print_ux_rational_coefs(num_degree, den_degree, 0);
    PRINT_WORD_DEF("TANCOT_COEF_ARRAY_DEGREE", degree );

    TABLE_COMMENT("Unpacked value of pi/4");
    PRINT_UX_TBL_ADEF_ITEM( "UX_PI_OVER_FOUR", pi/4);

    END_TABLE;

    @end_divert
    @eval my $tableText;						\
          my $outText    = MphocEval( GetStream( "divertText" ) );	\
          my $defineText = Egrep( "#define", $outText, \$tableText );	\
             $outText    = "$tableText\n\n$defineText";			\
          my $headerText = GetHeaderText( STR(BUILD_FILE_NAME),         \
                           "Definitions and constants trigonometric " .	\
                              "routines", __FILE__ );			\
             print "$headerText\n\n$outText\n";
#endif

