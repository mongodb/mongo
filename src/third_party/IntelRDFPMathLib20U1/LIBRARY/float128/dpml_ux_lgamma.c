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

#define	BASE_NAME	lgamma
#include "dpml_ux.h"

#if !defined(MAKE_INCLUDE)
#   include STR(BUILD_FILE_NAME)
#endif

/* 
** BASIC DESIGN
** ------------
** 
** The implementation of lgamma is based on the following identities:
** 
** 	lgamma(x) = log(Gamma(x))					(1)
** 	Gamma(x + 1) = x*Gamma(x)					(2)
**	lgamma(1+x) = -ln(1+x) + x*(1 - g) + P(x)			(3)
** 	lgamma(x) ~ (x - .5)*ln(x) - x + .5*ln(2*pi) + x*phi(1/x)	(4)
** 	lgamma(-x) = -{ ln[x*sin(pi*x)] + lgamma(x) - ln(pi) }		(5)
** 
** where g is Euler's constant, and
** 
** 	P(x)   = sum { n = 2, ... | (-x)^n*[zeta(n) - 1]/n }
** 	phi(z) = sum { n = 1, ... | B(2n)*z^(2n)/[2n*(2n-1)] }
** 
** where zeta(n) is the Reimann zeta function and B(n) is the n-th Bernoulli
** number.
** 
** The first step in the design is to determine where the asymptotic
** approximation, (4), is applicable.  According to Hart et. al., the error in
** (4) is less than and of the same sign as the first neglected term in
** phi(1/x).  Suppose we truncate phi(1/x) to n terms.  Then the error is
** bounded by the (n+1)st term.  Now the terms in phi(1/x) decrease to a
** certain point, and then begin to increase.  So the trick is to truncate phi
** just before the last decreasing term.  With this in mind, consider the ratio
** of consecutive terms, r(n):
** 
** 	              B(2n+2)           2n*(2n-1)*x^(2n)
** 	r(n) = --------------------- * -----------------
** 	       (2n+2)*(2n+1)*x^(2n+2)       B(2n)
** 
** 	           B(2n+2)*n(2n-1)
** 	     = ----------------------				(6)
** 	       B(2n)*(n+1)*(2n+1)*x^2
** 
** Now the terms of phi will be decreasing in magnitude as long as |r(n)| < 1,
** which is equivalent to
** 
** 		      B(2n+2)    n(2n-1)
** 		x^2 > ------- * ----------		(7)
** 		       B(2n)    (n+1)(2n+1)
** 
** Taking the smallest value of x that satisfies (7) and looking at the (n+1)-st
** term of phi, we would like the magnitude of that term to be less than the
** permissible total error, which we take to be 1/2^124.  So we need to solve
** 
** 	   B(2n+2)    / B(2n+2)    n(2n-1)    \ -(n+1)   1
** 	------------*|  ------- * -----------  |     < -----	(8)
** 	(2n+2)(2n+1)  \  B(2n)    (n+1)(2n+1) /        2^124
** 
** We could at this point convert the B(2n) terms to expressions involving the
** zeta function and factorials, apply Sterlings approximation and take
** limits to simplify the problem.  However, its easier and more accurate to
** solve (8) numerically, giving n = 41 and the minimum x as 12.971.  In order
** to simplify the screening process we will take the minimum x as 16.
** 
** So the basic algorithm is to apply equation (5) when x <= -16 and equation
** (4) when x >= 16.  Otherwise we try to "reduce" the argument to the interval
** [ b, b+1 ) where b is any convenient positive value, and apply equation (3).
** The argument reduction scheme is based on equation (2).  In particular, for
** x < b - 1 and x > b the following reductions can be used:
** 
** 	t <-- 1				t <-- 1
** 	z <-- x				z <-- x
** 	while (z < b)			while (z > b + 1)
** 	    {				    {
** 	    t <-- t*z			    z <-- z - 1
** 	    z <-- z + 1			    t <-- t*z
** 	    }				    }
** 	lgamma(x) <-- -ln(t) + P(z)	lgamma(x) <-- ln(t) + P(z)
** 
** 
** CHOSING b AND EVALUATING P(z)
** -----------------------------
** 
** From an algorithmic point of view, the choice of the reduced argument range,
** [b, b+1) is arbitrary.  However, from an implementation stand point, the
** choice of b has an impact on the "shape" of polynomial or rational
** expression used to evaluate the reduced argument.  This is particularly true
** for the unpacked x-float routines, because the evaluation is done in fixed
** point.
** 
** Because lgamma is rapidly increasing function, rational approximations are
** much more efficient that polynomial approximations.  So we will confine are
** remarks to the rational case.  All of the choices of b that were examined
** produced rational coefficients that initially increased and then decreased
** in both the numerator and denominator.  The choice of b controlled the
** length of the increasing sequence and the size of the ratio between the
** first term and the largest term.  For fixed point evaluations, it is most
** desirable for all of the terms to be decreasing, however, we were unable to 
** find a value of b for which this was true.  What follows below is a
** description of the "best we could do".
**
** Before proceeding, we point out that having the coefficients decrease in
** magnitude is a sufficient condition for having the polynomial evaluation
** routines function correctly, but is it not necessary.  A necessary condition
** is that in the alternating Horner's scheme iteration:
**
**		s(k-1) = c(k-1) - x*s(k)		(9)
**
** s(k-1) not be less than zero.
** 
** We note that gamma(n) = (n-1)! for any integer n, so that
** 
** 	lgamma(1) = log(gamma(1)) = log(0!) = log(1) = 0
** 	lgamma(2) = log(gamma(2)) = log(1!) = log(1) = 0
** 
** So that lgamma(x) has a zeros at x = 1 and x = 2.  Consequently, on the
** interval [1, 2) we can approximate lgamma(x) by an expression of the form
** (x - 1)*(x - 2)*R(x), where R(x) is a rational expression.  In order to
** minimize the sequence of increasing coefficients in R(x), we reduce the
** argument to the interval [-.5, .5) via the substitution, z = x - 3/2.  Then
** the approximation takes on the form (z + .5)*(z - .5)*U(z).  Using the Remes
** algorithm to generate coefficients for U, we see that the first three
** numerator and first two denominator coefficients are increasing.  The
** initial sequence of binary exponents are:
** 
** 	coefficient	 0   1   2   3   4   5   6   7  ...
** 	-----------	--- --- --- --- --- --- --- ---
** 	numerator	-1   1   2   2   1   0  -1  -5
** 	denominator	 1   3   3   3   3   3   2   0
** 
** Now, except when the reduced argument, z, is exactly +/- 1/2, |2*z| < 1, so
** that we can still use the ration evaluation routine for 2*z.  In effect, we
** can scale down each of the coefficients by a appropriate power of two,
** giving binary exponents that look like:
** 
** 	coefficient	 0   1   2   3   4   5   6   7  ...
** 	-----------	--- --- --- --- --- --- --- ---
** 	numerator	-1   0   0  -1  -3  -5  -7  -12
** 	denominator	 1   2   1   0  -1  -2  -4  -7
** 
** So that except for the first two coefficients, all the other terms are
** decreasing.  This means that we need to handle the case of |z| = 1/2
** separately.  However, this is not a problem since when |z| = 1/2, we know
** that lgamma(z) = 0.  We note that (9) is also satisfied.
**
** One last note: the Remes iterations for obtaining U(z) are rather unstable.
** rather than using REMES_FIND_RATIONAL_MODE, we use REMES_STATIC, with
** numerator/denominator degree = 13/14.  This yields an approximation good
** to slightly more that 126 bits.
** 
** With the above mind, the processing for |x| < 16 looks like:
** 
** 	t <-- 1				t <-- 1
** 	w <-- x				w <-- x
** 	while (w < 1)			while (z > 2)
** 	    {				    {
** 	    t <-- t*w			    w <-- w - 1
** 	    w <-- w + 1			    t <-- t*w
** 	    }				    }
**	y <-- 2*w - 3			y <-- 2*w - 3
**      z <-- (y-1)*(y+1)		z <-- (y-1)*(y+1)
** 	lgamma(x) <-- -ln(t) + z*V(y)	lgamma(x) <-- ln(t) + z*V(y)
**
** where V(w) = U(y/2 + 3/2)/4.
** 
** 
** 	NOTE: The following limits are useful for determining the
** 	coefficients of U;
** 
** 		     lgamma(x)
** 		lim  --------- = - euler_gamma
** 		x->1  (x-1)
** 
** 		     lgamma(x)
** 		lim  --------- = euler_gamma - 1
** 		x->2   (x-2)
** 
**
** LARGE ARGUMENTS:
** ----------------
**
** For large argument, the evaluation of lgamma(x) is based on (4) and (5).
** If we substitute (4) into (5) we have
** 	lgamma(x) ~ (x - .5)*ln(x) - x + .5*ln(2*pi) + x*phi(1/x)	(4)
**
** 	lgamma(-x) = -{ ln[x*sin(pi*x)] + lgamma(x) - ln(pi) }
** 	           ~ -{ ln[x*sin(pi*x)] + (x - .5)*ln(x) - x + .5*ln(2*pi) +
**	                  x*phi(1/x) - ln(pi) }
** 	           = -{ ln(x) + ln[sin(pi*x)] + (x - .5)*ln(x) - x +
**	                 .5*ln(2*pi) + x*phi(1/x) - ln(pi) }
** 	           = - { .5*ln(2/pi) + (x + .5)*ln(x) - x + x*phi(1/x) }
**	                 - ln[sin(pi*x)]
**
** If we define c = .5*ln(2/pi) and s = -1, then the above can be written
** as:
**
**	lgamma(-x) ~ s*{ c + [x - s*.5]*ln(x) - x + x*phi(1/x)} - ln[sin(pi*x)]
**
** Similarly, for positive x, if we define c = .5*ln(2*pi) and s = 1, then (4)
** can be written as:
**
** 	lgamma(x) ~ s*{c + (x - s*.5)*ln(x) - x + x*phi(1/x)}
**
** So that negative and positive cases can share a significant portion of code.
**
*/ 


/*
** UX_LGAMMA is the common processing routine for computing the unpacked lgamma 
** result from an unpacked input
*/

#if !defined(UX_LGAMMA)
#   define UX_LGAMMA		__INTERNAL_NAME(ux_lgamma__)
#endif

static void
UX_LGAMMA(UX_FLOAT * unpacked_argument, int * signgam,
  UX_FLOAT * unpacked_result)
    {
    UX_SIGN_TYPE sign;
    WORD I, floor_2x, exponent, cnt;
    UX_FLOAT fraction_part, tmp[3], reduced_argument;

    /*
    ** For large negative arguments, we need to compute |sin(pi*x)|.  If
    ** we compute 2*x = I + f where |f| < 1/2, then |sin(pi*x)| =
    ** |sin[(pi/2)*f]| or |cos[(pi/2)*f| depending on the parity of I.
    ** For small x, knowing floor(x) (or floor(2x)) makes it easier to set
    ** signgam and perform the loop counts for the argument reduction.
    **
    ** Let I = nint(2*x) and f = 2x - I.  Then floor(2x) = I if f is positive
    ** and I - 1 otherwise.
    */

    exponent = G_UX_EXPONENT( unpacked_argument );
    P_UX_EXPONENT( unpacked_argument, exponent + 1);
    I = UX_RND_TO_INT(unpacked_argument,
      RN_BIT_VECTOR | FRACTION_RESULT, NOT_USED, &fraction_part);
    P_UX_EXPONENT( unpacked_argument, exponent);

    /* Get floor(2x) */

    cnt = G_UX_SIGN(&fraction_part) >> (BITS_PER_UX_SIGN_TYPE - 1);
    sign = G_UX_SIGN(unpacked_argument);
    floor_2x = cnt + (sign ? - I : I);
//printf("DGAMMA 1\n");
    /*
    ** If input was a negative integer, force "underflow" error.  By convention
    ** signgam = 1 for these cases
    */

    if (sign && !(I & 1) && (G_UX_MSD( &fraction_part ) == 0))
        {
        P_UX_EXPONENT( unpacked_result,  UX_UNDERFLOW_EXPONENT);
        P_UX_MSD(unpacked_result, UX_MSB);
        *signgam = 1;
        return;
        }

    /* Set signgam to -1 if x < 0 and int(x) is odd, +1 otherwise */

    *signgam = 1 - ((sign >> (BITS_PER_UX_SIGN_TYPE - 2)) & (floor_2x & 2));

    if (exponent < 5)
	{ /* | x | < 16

        /* Set initial product to 1 and get  */

        UX_SET_SIGN_EXP_MSD(tmp, 0, 1, UX_MSB);
        cnt = floor_2x;

        while (cnt < 2)
            {
            MULTIPLY(tmp, unpacked_argument, tmp);
            ADDSUB(unpacked_argument, UX_ONE, ADD, unpacked_argument);
            cnt += 2;
            }

        while (cnt >= 4)
            {
            ADDSUB(unpacked_argument, UX_ONE, SUB, unpacked_argument);
            MULTIPLY(tmp, unpacked_argument, tmp);
            cnt -= 2;
            }

        /*
        ** Compute u = 2*unpacked_argument-1, w = (u-1)*(u+1) in
        ** preparation for computing lgamma(u/2 + 3/2) = w*R(u)
        */

        UX_INCR_EXPONENT(unpacked_argument, 1);
        ADDSUB(unpacked_argument, UX_THREE, SUB, &reduced_argument);
        ADDSUB(&reduced_argument, UX_ONE, ADD_SUB, &tmp[1]);
        MULTIPLY(&tmp[1], &tmp[2], unpacked_result);

        /*
        ** If the value of w (unpacked_result) is 0, then the original
        ** argument was an integer and lgamma(reduced_argument) is zero
        ** and we don't need to evaluate the rational expression
        */

        if (G_UX_MSD(unpacked_result))
            {
            EVALUATE_RATIONAL(
                &reduced_argument,
                LGAMMA_P_COEF_ARRAY,
	        LGAMMA_P_COEF_ARRAY_DEGREE,
                NUMERATOR_FLAGS( STANDARD ) | DENOMINATOR_FLAGS( STANDARD ),
                &tmp[1]
                );
/*printf("DGAMMA 2: rarg=%x %x %llx %llx, tmp1=%x %x %llx %llx\n",reduced_argument.sign,reduced_argument.exponent,reduced_argument.fraction[0],reduced_argument.fraction[1],
	tmp[1].sign,tmp[1].exponent,tmp[1].fraction[0],tmp[1].fraction[1]);*/
            MULTIPLY(unpacked_result, &tmp[1], unpacked_result);
 /*printf("DGAMMA 2: ures=%x %x %llx %llx, tmp1=%x %x %llx %llx\n",unpacked_result->sign,unpacked_result->exponent,unpacked_result->fraction[0],unpacked_result->fraction[1],
	tmp[1].sign,tmp[1].exponent,tmp[1].fraction[0],tmp[1].fraction[1]);*/
          }


        /*
        ** Now compute log(tmp) and add/sub it to/from the previous
        ** lgamma computation.  Note that if floor_2x == cnt
        ** at this point, tmp = 1, so we don't need to do the computation.
        */

        if (floor_2x != cnt)
            {
            P_UX_SIGN(tmp, 0);
            NORMALIZE(tmp);
            UX_LOG( tmp, UX_LN2, tmp);
//printf("DGAMMA 4\n");


            ADDSUB(unpacked_result, tmp, (floor_2x < cnt) ? SUB : ADD,
              unpacked_result);
            }
        }
    else
        {
        /* use |x| from here on */

        P_UX_SIGN( unpacked_argument, 0);

        /*
        ** x is big, so use asymptotic approximation:
        **
        **	lgamma(x) ~ (x-s*.5)*log(x) - x + c + x*phi(1/x^2)
        */

        UX_LOG(unpacked_argument, UX_LN2, unpacked_result);
        ADDSUB(unpacked_argument, UX_HALF, sign ? ADD : SUB, tmp);
        MULTIPLY(unpacked_result, tmp, unpacked_result);
        ADDSUB(unpacked_result, unpacked_argument, SUB, unpacked_result);
        ADDSUB(
           unpacked_result,
           sign ? UX_HALF_LN_TWO_OVER_PI : UX_HALF_LN_TWO_PI,
           ADD, unpacked_result);
        DIVIDE(0, unpacked_argument, FULL_PRECISION, tmp);
        EVALUATE_RATIONAL(
            tmp,
            LGAMMA_PHI_COEF_ARRAY,
	    LGAMMA_PHI_COEF_ARRAY_DEGREE,
            NUMERATOR_FLAGS(SQUARE_TERM | POST_MULTIPLY)
               | DENOMINATOR_FLAGS(SQUARE_TERM) | P_SCALE(3),
            // unpacked_argument
            &tmp[1]
            );
        // ADDSUB(unpacked_result, unpacked_argument, ADD, unpacked_result);
        ADDSUB(unpacked_result, &tmp[1], ADD, unpacked_result);
        
        if (sign)
            {
            /*
            ** x is big and negative, so we need to negate the result
            ** and subtract ln[x*sin(pi*x)]
            */

            UX_TOGGLE_SIGN(unpacked_result, sign);
            MULTIPLY(&fraction_part, UX_PI_OVER_2, tmp);
            UX_SINCOS(tmp, I << 1, SIN_FUNC, tmp);
            NORMALIZE(tmp);
            UX_LOG( tmp, UX_LN2, tmp);
            ADDSUB(unpacked_result, tmp, SUB, unpacked_result);
            }
        }
   }

/*
** C_UX_LGAMMA is the common processing routine for the 3 lgamma functions:
** lgamma, gamma and __lgamma.  Each of the lgamma routines calls into the
** C_LGAMMA routine, which unpacks the arguments, computes the results, and
** processes exceptions. 
*/

#if !defined(C_UX_LGAMMA)
#   define C_UX_LGAMMA		__INTERNAL_NAME(C_ux_lgamma__)
#endif

static void
C_UX_LGAMMA(_X_FLOAT * packed_argument, int * signgam,
   _X_FLOAT * packed_result OPT_EXCEPTION_INFO_DECLARATION)
    {
    WORD fp_class;
    UX_FLOAT unpacked_argument, unpacked_result;

    fp_class  = UNPACK(
        packed_argument,
        &unpacked_argument,
        LGAMMA_CLASS_TO_ACTION_MAP,
        packed_result
        OPT_EXCEPTION_INFO_ARGUMENT );

    if (fp_class < 0)
        {
        fp_class &= MAKE_MASK(F_C_CLASS_BIT_WIDTH,0);
        *signgam = (fp_class == F_C_NEG_ZERO) ? -1 : 1;
        return;
        }

    UX_LGAMMA( &unpacked_argument, signgam, &unpacked_result);
    PACK(
        &unpacked_result,
        packed_result,
        LGAMMA_NON_POS_INT,
        LGAMMA_OVERFLOW
        OPT_EXCEPTION_INFO_ARGUMENT );
    }

/*
** Currently, there are 3 flavors of the lgamma function: lgamma, gamma and 
** __lgamma.  For the unpacked library, each of these routines calls into the
** C_UX_LGAMMA routine, which unpacks the arguments, computes the results,
** and processes exceptions.
*/

/*
** Allocate storage for signgaml (appropriately named)
*/

#if (F_NAME_SUFFIX == DPML_NULL_MACRO_TOKEN)
#    define  SIGNGAM_NAME signgam
#else
# define  SIGNGAM_NAME     __signgamq  // new name
// The following IS_DEFINED_SIGNGAM_NAME_OLD macro to be removed in far future (see trackers 73679,73680)
# define     IS_DEFINED_SIGNGAM_NAME_OLD
# if defined(IS_DEFINED_SIGNGAM_NAME_OLD)
#   if (OP_SYSTEM == vms)
#      define  SIGNGAM_NAME_OLD PASTE_3(F_NAME_PREFIX,signgam, F_NAME_SUFFIX)
#   else
#      define  SIGNGAM_NAME_OLD PASTE_2(signgam, F_NAME_SUFFIX)
#   endif
#   endif
#endif

int SIGNGAM_NAME;
#ifdef IS_DEFINED_SIGNGAM_NAME_OLD
int SIGNGAM_NAME_OLD;
#endif

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_LGAMMA_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    C_UX_LGAMMA(
        PASS_ARG_X_FLOAT(packed_argument),
        &SIGNGAM_NAME,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO);

    #ifdef   IS_DEFINED_SIGNGAM_NAME_OLD
        SIGNGAM_NAME_OLD=SIGNGAM_NAME;
    #endif

    RETURN_X_FLOAT(packed_result);

    }

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_GAMMA_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    C_UX_LGAMMA(
        PASS_ARG_X_FLOAT(packed_argument),
        &SIGNGAM_NAME,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO);

    #ifdef   IS_DEFINED_SIGNGAM_NAME_OLD
        SIGNGAM_NAME_OLD=SIGNGAM_NAME;
    #endif

    RETURN_X_FLOAT(packed_result);

    }

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_RT_LGAMMA_NAME

X_XIptr_PROTO(F_ENTRY_NAME, packed_result, packed_argument, signgam_ptr)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    C_UX_LGAMMA(
        PASS_ARG_X_FLOAT(packed_argument),
        (int *) signgam_ptr,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO);

    RETURN_X_FLOAT(packed_result);

    }


/*
** C_UX_LGAMMA is the common processing routine for the 3 lgamma functions:
** lgamma, gamma and __lgamma.  Each of the lgamma routines calls into the
** C_LGAMMA routine, which unpacks the arguments, computes the results, and
** processes exceptions. 
*/

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_TGAMMA_NAME

#define LOG2_BITS_PER_DIGIT __LOG2(BITS_PER_UX_FRACTION_DIGIT_TYPE)
#define DIGIT_MOD_MASK      MAKE_MASK(LOG2_BITS_PER_DIGIT,0)

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument)
    {
    WORD fp_class, exponent, i;
    UX_FRACTION_DIGIT_TYPE msd, mask;
    UX_FLOAT unpacked_argument, unpacked_result, tmp;
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)
 
    INIT_EXCEPTION_INFO;
    fp_class  = UNPACK(
        PASS_ARG_X_FLOAT(packed_argument),
        &unpacked_argument,
        LGAMMA_CLASS_TO_ACTION_MAP,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO );

    if (fp_class < 0)
        {
        SIGNGAM_NAME = (fp_class == F_C_NEG_ZERO) ? -1 : 1;
        RETURN_X_FLOAT(packed_result);
        }

    exponent = G_UX_EXPONENT( &unpacked_argument );
    if ( G_UX_SIGN( &unpacked_argument ) == 0) 
        {
        // Input is positive. Check for sure overflow
        if ( exponent > 11 )
            {
            UX_SET_SIGN_EXP_MSD(&unpacked_result, 0, UX_OVERFLOW_EXPONENT,
                 UX_MSB);
            goto pack_it;
            }
        }
    else if ( exponent > 0 )
        {
        // If input is a negative integer, return NaN and signal an error via
        // an underflow condition
        i   = exponent >> LOG2_BITS_PER_DIGIT;
        mask = (((UX_FRACTION_DIGIT_TYPE) -1) >> (exponent & DIGIT_MOD_MASK));
        msd  = G_UX_FRACTION_DIGIT( &unpacked_argument, i);
        msd &= mask;
        while ( ++i < NUM_UX_FRACTION_DIGITS ) 
            msd |= G_UX_FRACTION_DIGIT( &unpacked_argument, i);
        if ( msd == 0 )
            { // This is a negative integer, force underflow condition
            UX_SET_SIGN_EXP_MSD(&unpacked_result, 0, UX_UNDERFLOW_EXPONENT,
                 UX_MSB);
            goto pack_it;
            }
        }

    // At this point the argument is not too large or a negative integer.
    // Compute t = lgamma(x) and check for overflow when computing exp(t)

    UX_LGAMMA( &unpacked_argument, &SIGNGAM_NAME, &tmp);
    if ( G_UX_EXPONENT( &tmp ) >= 14 )
        // Force overflow condition
        UX_SET_SIGN_EXP_MSD(&unpacked_result, 0, UX_OVERFLOW_EXPONENT, UX_MSB);
    else
        UX_EXP( &tmp, &unpacked_result );

pack_it:
    PACK(
        &unpacked_result,
        PASS_RET_X_FLOAT(packed_result),
        LGAMMA_NON_POS_INT,
        LGAMMA_OVERFLOW
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);
    }



#if defined(MAKE_INCLUDE)

    @divert -append divertText

    precision = ceil(UX_PRECISION/8) + 4;

#   undef TABLE_NAME

    START_TABLE;

    TABLE_COMMENT("lgamma class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "LGAMMA_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     2) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ERROR,     3) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_ERROR,     3) );

        PRINT_U_TBL_ITEM( /* data 1 */ LGAMMA_POS_INF );
        PRINT_U_TBL_ITEM( /* data 2 */ LGAMMA_NEG_INF );
        PRINT_U_TBL_ITEM( /* data 2 */ LGAMMA_OF_ZERO );


    TABLE_COMMENT(
          "Unpacked values of 1, 1/2, 3, ln2, pi/2, ln(2*pi)/2 and ln(pi/2)/2");

    PRINT_UX_TBL_ADEF_ITEM( "UX_ONE\t\t\t",                      1.0);
    PRINT_UX_TBL_ADEF_ITEM( "UX_HALF\t\t\t",                      .5);
    PRINT_UX_TBL_ADEF_ITEM( "UX_THREE\t\t\t",                    3.0);
    PRINT_UX_TBL_ADEF_ITEM( "UX_LN2\t\t\t",                   log(2));
    PRINT_UX_TBL_ADEF_ITEM( "UX_PI_OVER_2\t\t",                 pi/2);
    PRINT_UX_TBL_ADEF_ITEM( "UX_HALF_LN_TWO_PI\t",      .5*log(2*pi));
    PRINT_UX_TBL_ADEF_ITEM( "UX_HALF_LN_TWO_OVER_PI\t", .5*log(2/pi));

    /*
    ** Get coefficients for lgamma on [1, 2).  Recall that the "reduced
    ** argument", y, is in the interval [-1, 1) and we use the approximation
    ** for .25*lgamma(y/2)/[(y-1)*(y+1)]
    */

    function lgamma_1_2(y)
        {
        auto z, x, t;

        t = (y + 1);
        z = t*(y - 1);

        if (z == 0)
            t = .25 * ((t == 0) ? euler_gamma : 1 - euler_gamma);
        else
            t = lgamma(.5*(y + 3))/z;
        return t;
        }

    /*
    ** The Remes algorithm is quite slow for lgamma on [1,2) so we loosen
    ** convergence criteria and fix (rather than find) the degree of the
    ** numerator and denominator.
    */

    save_precision = precision;
    precision = ceil(UX_PRECISION/8) + 8;

    num_degree = 13;
    den_degree = 14;
    REMES_OPTION_LEVELING_TOL(.1);

    TABLE_COMMENT("Fixed point coefficients for lgamma on [1,2)");
    remes(REMES_STATIC + REMES_RELATIVE_WEIGHT + REMES_LINEAR_ARG,
        -1, 1, lgamma_1_2, num_degree, den_degree, &ux_rational_coefs);

    precision = save_precision;

    PRINT_FIXED_128_TBL_ADEF("LGAMMA_P_COEF_ARRAY\t");
    degree = print_ux_rational_coefs(num_degree, den_degree, 0);
    PRINT_WORD_DEF("LGAMMA_P_COEF_ARRAY_DEGREE", degree);


    /*
    ** Now get the coefficients for the asymptotic range.  This is actually
    ** quite complicated because of the structure of the function we are
    ** trying to evaluate.  Specifically, we have to evaluate the asymptotic
    ** expansion for lgamma(x) in a precision higher than what we need.
    ** This means that there is a minimum x associated with the evaluations
    ** precision for which the asymptotic expansion is valid and below which
    ** the closed form loses significance.  Consequently, the MP function
    ** that is evaluated for the Remes algorithm is broken up into two
    ** subdomains.
    **
    ** The MP function get_bernoulli, computes the values of the Bernoulli
    ** numbers for use in the asymptotic expansion of lgamma according to
    ** the expansion:
    **
    **		sum{ C(n+1, j)*B(j) | j = 0, ... n } = 0.
    **
    ** It makes use of the fact that B(1) = -1/2 and that B(2k+1) = 0 for
    ** k >= 1.
    */


    function get_bernoulli(m_lo, m_hi)
        {
        auto n, C, top, bottom, t;

        for (n = m_lo; n <= m_hi; n += 2)
            {
            C = 1;
            top = n+1;
            bottom = 1;
            t = 0;
            for (j = 0; j < n; j += 2)
                {
                t += (C*B[j]);
                C = C*top*(top - 1)/(bottom*(bottom + 1));
                top -= 2;
                bottom += 2;
                }
            B[n] = .5 - t/(n+1);
            }
        return m_hi;
        }

    /*
    ** The function find_n_and_min_x determine the minimum value that will
    ** converge to the given tol in the lgamma asymptotic expansion.  The
    ** algorithm is based on the discussion around equations (6) through (8)
    */

    function find_n_and_min_x(tol)
        {
        auto n, b_2n, b_2n_plus_2, common, x_sqr, tmp;
    
        n = 4;
        b_2n = B[0];
        while (1)
            {
            if (n > max_bernoulli)
                 max_bernoulli =
                       get_bernoulli(max_bernoulli + 2, max_bernoulli + 128);
            b_2n_plus_2 = B[n];

            common = b_2n_plus_2/(n*(n - 1));
            x_sqr  = -common*(n-2)*(n-3)/b_2n;
            tmp    = abs(common*x_sqr^(-n/2));
            if (tmp < tol)
                break;
            n += 2;
            b_2n = b_2n_plus_2;
            }
        max_terms = n - 2;
        return sqrt(x_sqr);
        }
   
    /*
    ** lgamma_phi computes the asymptotic approximation to lgamma(8*x)
    */

    half_log_two_pi = .5*log(2*pi);

    function lgamma_phi(z)
        {
        auto w, x, term, m, total, tmp;

        total = B[2]/2;
        if (z == 0)
            return total;

        z *= .125;
        if (z > asymptotic_z)
            {
            x = 1/z;
            return (lgamma(x) - (x - .5)*log(x) + x - half_log_two_pi)*x;
            }

        w = z*z;
        m = 4;
        term = 1;
        old_term = total;
        while (m <= max_terms)
            {
            term *= w;
            new_term = term*B[m]/(m*(m-1));
            total += new_term;
            if ((bexp(total) - bexp(new_term)) > bit_precision)
                return total;
            m += 2;
            }
        return total;
        }

    /*
    ** initial the array of Bernoulli numbers and determine the break point
    ** in the domain for lgamma_phi
    */

    save_precision = precision;
    precision = (F_PRECISION/MP_RADIX_BITS) + 8;
    bit_precision = MP_RADIX_BITS*precision;

    max_bernoulli = 2;
    B[0] = 1;
    B[1] = -.5;
    B[2] = 1/6;

    x = find_n_and_min_x(2^-bit_precision);
    asymptotic_z = 1/x;

    /*
    ** Now compute the coefficients
    */

    TABLE_COMMENT("Fixed point coefficients for lgamma(8*x) on [0, 1/16)");
    remes( REMES_FIND_RATIONAL + REMES_SQUARE_ARG + REMES_RELATIVE_WEIGHT,
       0, 8*(1/16), lgamma_phi, UX_PRECISION, &num_degree, &den_degree,
       &ux_rational_coefs);

    precision = save_precision;

    PRINT_FIXED_128_TBL_ADEF("LGAMMA_PHI_COEF_ARRAY\t");
    degree = print_ux_rational_coefs(num_degree, den_degree, -3);
    PRINT_WORD_DEF("LGAMMA_PHI_COEF_ARRAY_DEGREE", degree);

    END_TABLE;

    @end_divert
    @eval my $tableText;						\
          my $outText    = MphocEval( GetStream( "divertText" ) );	\
          my $defineText = Egrep( "#define", $outText, \$tableText );	\
             $outText    = "$tableText\n\n$defineText";			\
          my $headerText = GetHeaderText( STR(BUILD_FILE_NAME),         \
                           "Definitions and constants lgamma",		\
                              __FILE__ );				\
             print "$headerText\n\n$outText\n";
#endif

