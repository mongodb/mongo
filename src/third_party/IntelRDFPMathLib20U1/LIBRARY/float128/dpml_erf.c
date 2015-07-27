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

#if defined(ERFC)
#   define BASE_NAME		ERFC_BASE_NAME
#   define _F_ENTRY_NAME	F_ERFC_NAME
#   define SELECT(x,y)		y
#   define IF_ERFC(x)		x
#   define IF_ERF(x)	 
#else
#   define BASE_NAME		ERF_BASE_NAME
#   define _F_ENTRY_NAME	F_ERF_NAME
#   define SELECT(x,y)		x
#   define IF_ERFC(x)	 
#   define IF_ERF(x)		x
#endif

#if defined(MAKE_COMMON)

#   define DEFINES_ONLY
#   define COMMON_NAME	erf

#   if  !defined(BUILD_FILE_NAME)
#       define BUILD_FILE_EXTENSION     c
#       define BUILD_SUFFIX             TABLE_SUFFIX
#       define BUILD_FILE_NAME          __BUILD_FILE_NAME(COMMON_NAME)
#   endif

#   if  !defined(MP_FILE_NAME)
#       define MP_FILE_NAME		__MP_FILE_NAME(COMMON_NAME)
#   endif

#   if  !defined(TABLE_NAME)
#      define TABLE_NAME   __F_TABLE_NAME(COMMON_NAME)
#   endif

#   define IF_MAKE_COMMON(x)		x
#   define START_TABLE(name, offset)	START_GLOBAL_TABLE(TABLE_NAME, offset)

#else

#   undef  DEFINES_ONLY
#   define IF_MAKE_COMMON(x)
#   define START_TABLE(name, offset)	START_STATIC_TABLE(TABLE_NAME, offset)

#endif

#define __NEEDS_SIGNED_DENORM_TO_NORM
#define __LOG2_DENORM_SCALE		(F_PRECISION + 3)

#define  NEW_DPML_MACROS	1
#include "dpml_private.h"

/*
 *	NOTE: This routine accesses the special exp entry point.
 *	Consequently it needs to know the alignment of the scale
 *	factor.
 */

#include STR(SPECIAL_EXP_HEADER)

/*
 *  This is a hack on Alpha VMS and NT for the function
 *  F_EXP_SPECIAL_ENTRY_NAME.  In particular, the return argument 
 *  WORD *pow_of_two.  Since WORD is defined as int_32 on these two platforms
 *  and dpml_exp.c changes the WORD definition there to int_64, we need to make
 *  sure *pow_of_two is defined as int_64 (since that is what dpml_exp.c
 *  declared).  These #if defined can be removed as soon as those platforms 
 *  support 64 bits.
 */
#if ((defined(ALPHA) || defined(alpha)) && (defined(wnt) || defined(vms)))
#   define EXP_WORD_TYPE INT_64
#else
#   define EXP_WORD_TYPE WORD
#endif

#if defined(PRECISION_BACKUP_AVAILABLE)
#  	define EXP_OTHER_ARGS EXP_WORD_TYPE *pow_of_two
#else
#  	define EXP_OTHER_ARGS EXP_WORD_TYPE *pow_of_two, F_TYPE *pow2_low
#endif

extern B_TYPE F_EXP_SPECIAL_ENTRY_NAME (F_TYPE x, EXP_OTHER_ARGS);

#if !defined(IEEE_FLOATING)
#   define IEEE_FLOATING	0
#endif

/*
 * GENERAL COMMENTS:
 * -----------------
 *
 * The error function, erf(x) is defined for all values of x by the integral:
 *
 *	                  /\ x
 *	            2     |
 *	erf(x) = -------- | exp(-t^2)dt
 *		 sqrt(pi) |
 *		         \/ 0
 *
 * From the definition and the taylor expansion for exp(x) is follows that
 *
 *		erf(-x) = - erf(x)			(1)
 *
 *	                   ____
 *	            2      \    (-x)^(2k+1)
 *	erf(x) = --------  /    -----------		(2)
 *		 sqrt(pi) /____  k! (2k+1)
 *	                  k = 0
 *
 * The complementary error function, erfc(x) is defined as 1 - erf(x).  erfc(x)
 * can be approximated asymtotically by:
 *
 *	                     /       ____                \
 *	           exp(-x^2) |       \    (-1)^k (2k)!   |
 *	erfc(x) ~ ---------- | 1 +   /    -------------- |	(3)
 *		  x*sqrt(pi) |      /____  4^k k! x^(2k) |
 *	                     \      k = 1                /
 *
 * and computed directly as a continued fraction:
 *
 *	                     /                        \
 *	           exp(-x^2) |  1   1/2  2/2  3/2  4/2 |
 *	erfc(x) = ---------- | ---  ---  ---  ---  --- |	(4)
 *		   sqrt(pi)  | x +  x +  x +  x +  x + |
 *	                     \                        /
 *
 * For large values of x, computing erf(x) via (2) is time consuming and
 * incurs significant roundoff error.  Consequently, for large x, it is
 * best to compute erf(x) as 1 - erfc(x), where erfc(x) is computed via (3).
 * Similarly computing erfc(x) for small values of x is time consuming and
 * inaccurate, so it is best to compute erfc(x) = 1 - erf(x) for small x.
 * Since erf(x) and erfc(x) are bounded between 0 and 1 for positive x, there
 * is no loss of significance in computing 1 - erf(x) or 1 - erfc(x) if
 * erf(x) and erfc(x) are less than or equal to 1/2.  Let s be a point such
 * that erf(s) = 1/2.  It follows from the definition of erfc that erfc(x) =
 * 1/2.
 *
 * IMPLEMENTATION ISSUES:
 * ----------------------
 *
 * When x is small, erf(x) = 2*x/sqrt(pi) to machine precision.  In particular
 * if x satifies
 *
 *		2*x/sqrt(pi) - erf(x)
 *		---------------------- < 2^-(F_PRECISION + 1)
 *		       erf(x)
 *
 * then erf(x) = 2*x/sqrt(pi) to machine precision.  For k >= 1, let rho(k) =
 * 1 + 2^-(F_PRECISION + k), if r satifies
 *
 *			erf(r)/r = 2/(rho(k)*sqrt(pi))
 *
 * then it follows that for |x| < r, erf(x) = 2*x/sqrt(pi) to machine precision.
 * The reason for not fixing k = 1 in the above equation, is because we need
 * to consider the manner in which x*(2/sqrt(pi)) is computed.  Since
 * 2/sqrt(pi) is not exact, but close to 1, we can improve the accuracy of the
 * final approximation by computing erf(x) for small x as:
 *
 *		erf(x) = x + (2/sqrt(pi) - 1)*x
 *
 * The final error will the error in the above computation, plus the error
 * induced by truncating the series.  For k = 1, the induced error is 1/2 bit.
 * In order to keep the final error below 1 lsb, it is better to increase
 * k to 2.  This will limit the induced error to 1/4 lsb.
 *
 * Also note that for very small x, (2/sqrt(pi) - 1)*x will underflow, even
 * though the final result doesn't.  To avoid this problem, we note that
 * (2/sqrt(pi) - 1) > (1/8) and compute erf(x) for small x as:
 *
 *		erf(x) = (8*x + 8*(2/sqrt(pi) - 1)*x)*(1/8)
 *
 * Similarly, when x is small, erfc(x) = 1 to machine precision.  Specifically
 * if r' satisfies
 *
 *			erfc(r') = 1/rho(1),
 *
 * then for |x| < r', erf(x) = 1 to machine precision.
 */

#if defined(MAKE_INCLUDE)

    @divert divertText

    /*
     * The following subroutine "purifies" a floating point number by
     * zeroing out low order bits that will not appear when the floating
     * value is fetched as a WORD into an integer register.  We assume
     * that for VAX formats, the floating point numbers have been
     * "PDP_SHUFFLED"
     */

#   if BITS_PER_WORD < BITS_PER_F_TYPE
#      define NUM_INT_BITS	BITS_PER_WORD
#   else
#      define NUM_INT_BITS	BITS_PER_F_TYPE
#   endif
    
    function purify()
        {
        _n = bexp($1) - NUM_INT_BITS;
        _y = bldexp($1, -_n);
        _y = trunc(_y);
        _y = bldexp(_y, _n);
        return _y;
        }

    /*
     * the following macro solves the equation F(x) = c to a relative error
     * of t.  The input points a and b are two points near the solution that
     * are used as the starting points for the modified Newton's iteration
     */

#   define	FIND_ROOT(a, b, c, t, p, r) \
		old_precision = precision; \
		precision = ceil((p)/MP_RADIX_BITS) + 4; \
		x1 = (a); x2 = (b); \
		f1 = F(x1); f2 = F(x2); \
		while (1) \
		    { \
		    r = ((x1*f2 - x2*f1) + (c)*(x2 - x1))/(f2 - f1); \
		    err = 2*abs((r - x2)/(r + x2)); \
		    if (err < (t)) \
		        break; \
		    x1 = x2; x2 = r; \
		    f1 = f2; f2 = F(x2); \
		    } \
		precision = old_precision;

    /* Set working precision an start computing */
    precision = ceil(F_PRECISION/MP_RADIX_BITS) + 4;

    mu = 2/sqrt(pi);
    rho = 1 + 2^-(F_PRECISION + 2);
    c = mu/rho;

    /*
     * Find the smallest polynomial argument for erf, by solving the
     * equation erf(x)/x = 2/(rho*sqrt(pi)) using Newton's method.  The
     * starting points, a and b, are obtained by truncating the Taylor
     * series for erf(x)/x to 2 and 3 terms respectively and solving for
     * x.  Since f(x) = erf(x)/x = 2/sqrt(pi)*[1 - x^2/3 + x^4/(5*2!) ...]
     * and the solution we are looking for is on the order of 1/2^(p+1),
     * it follows that we must have something on the order of 3*p + 3bits
     * in the MP calculations to insure that f(x2) - f(x1) term in the
     * Newton's iteration has at least p + 1 bits of accuracy
     */

    lambda = 1/(2^(F_PRECISION + 1) + 1);
    a = sqrt(3*lambda);
    b = sqrt((5 - sqrt(25 - 90*lambda))/3);
    tol = 2^-(F_PRECISION + 1);

#   undef  F
#   define F(x)	(erf(x)/x)

    FIND_ROOT(a, b, c, tol, 3*(F_PRECISION + 1), smallest_erf_poly_arg);
    smallest_erf_poly_arg = purify(smallest_erf_poly_arg);

#if 0

    /*
     * Find the smallest polynomial argument for erfc, by solving the
     * equation erfc(x) = 1/rho(1).
     */

    lambda = 1/(1 + 2^(F_PRECISION + 1));
    a = lambda;
    b = a/(1 - a*a/3);

#   undef  F(x)
#   define F(x)	(erf(x))
    FIND_ROOT(a, b, lambda, tol, 2*F_PRECISION, smallest_erfc_poly_arg);

#endif

    /*
     * Find the smallest polynomial argument for erfc, by solving the
     * equation erfc(x) = 1/rho(1).  This is equivalent to solving
     * erf(x) = 1 - 1/rho(1) or letting lambda = 1/(2^(F_PRECISION + 1),
     * x = arc_erf(lambda).  Since lambda is so small, using the Newton's
     * interations for the solution is some what combersom.  However,
     * arc_erf(x) can be Taylor series of the form:
     *
     *		arc_erf(x) =
     *		   sum{ k = 0,.. | C(2k+1)*(x*sqrt(pi)/2)^(2k+1)/(2k+1)! }
     *
     * where C(k) is the constant of the polynomial P(k,x) which is defined
     * recursively by:
     *
     *		P(k+1,x) = P'(k,x) + 2*k*x*P(k,x)
     *
     * Letting z = x*sqrt(pi)/2, it follows that
     *
     *		arc_erf(x) = z + (2/2!)*z^3 + (28/5!)*z^5 + ...
     *
     * Since we are only interested in generating constants good to 
     * machine precision and since lambda < 1/2^F_PRECISION, we need only
     * take two terms in the series.
     */

    lambda = (sqrt(pi)/2)/(1 + 2^(F_PRECISION + 1));
    smallest_erfc_poly_arg = lambda*(1 + lambda*lambda);

    smallest_erfc_poly_arg = purify(smallest_erfc_poly_arg);
    

/*
 * DENORM PROCESSING:
 * ------------------
 *
 * Since 2/sqrt(pi) > 1, if x is not denormalized, then erf(x) will not be
 * denormalized.  However, for certain values of x just below the denormalized
 * threshold, erf(x) will be normalized.  Consequently, we can deal with
 * denorms by scaling up, multipling and then scaling down.  As in the small
 * case we will want to perform the multiplication as x + (2/sqrt(pi) - 1)*x,
 * so we want to scale up high enough so that (2/sqrt(pi) - 1)*x does not
 * become denormalized.  Since (2/sqrt(pi) - 1) > 1/8, we can scale up by
 * the precision + 3 and still avoid denormalized results.  Use the
 * denorm scaling macros in dpml_private.h by defining an appropriate
 * value of __LOG2_DENORM_SCALE.
 */


/*
 * ERF(x) and ERFC(x) EVALUATION FOR SMALL ARGUMENTS:
 * ---------------------------------------------------
 *
 * Using (2) to approximate erf(x) is fairly straight forward.  We assume that
 * the (2) can be written as x*R(x^2), where R is a rational function.  We note
 * that R(0) = 2/sqrt(pi) ~ 1.12837916.  So we can reformulate the computation
 * and improve the accuracy by evaluating (2) in the form x + x*S(x^2).  Since
 * 2/sqrt(pi) ~ 1.12837916, when x is small, the overhang between x and x*S(x)
 * is 3 bits unless x is close to a power of two, in which case it is 2 bits.
 * As x increases to about .617 the overhang increases to about 13 bits.  As x
 * continues to increase, the overhang decreases until it reaches a 3 bit
 * overhang at .942.  The reason for the dramatic increase in overhang near
 * .617 is that the function x*S(x^2) has zero in that region.  This means
 * that x*S(x^2) has a massive loss of significance near .617.  Fortunately,
 * for x < .617, the alignment shift between x and x*S(x^2) is sufficient to
 * compensate for the loss of significance.  For x > .617, the alignment
 * shift is not as effective at compensatating.
 *
 * We can use (2) to compute erfc(x) when x is small as:
 *
 *		erfc(x) = 1 - x*R(x^2)
 *		        = 1 - {x + x*[R(x^2) - 1]}
 *		        = (1 - x) + x*[R(x^2) - 1]
 *
 * Graphing the overhang between (1-x) and x*[R(x^2) - 1], we note the the
 * overhang decreases with x to a 4 bit overhang near .5, then increases to
 * 12 bits near .617 and then steadily decreases as x gets larger.  At x = .75
 * the overhang is 3 bits and for x > .75 the overhang is less than 3 bits.
 * Problems with loss of significance near .617 are similar to the erf case.
 *
 * The upshot of the above, is that if approximate erf(x) - x on the interval
 * [0, .617] then we can compute both erf(x) and erfc(x) using that
 * approximation and obtain (almost always) a 3 bit overhang on the last
 * add.  This should result in an error bound < 1 ulp on that interval for 
 * both functions.
 *
 * Note that when computing 1 - x in the erfc case, the subtraction is not
 * exact so some care needs to be taken.  Specifically, let z = 1 - x and 
 * y = x + (z - 1), then we can compute erfc as:
 *
 *		erfc(x) = (1 - x) + x*[R(x^2) - 1]
 *		        = (z - y) + x*[R(x^2) - 1]
 *		        = z + { x*[R(x^2) - 1] - y }
 *
 * At first glance, approximating the function on the interval [0, .617] may
 * seem quite wasteful, since the interval is relatively large and consequently
 * the evaluation will be slow.  However, the terms in the series decrease
 * as 1/n! so the convergence is fast.  Also, the alternative is to use an
 * approximation based on equations (3) or (4), both of which require an
 * evaluation of exp(-x^2).
 */


#   undef  F
#   define F(x)	(erf(x)/x)
    a = .617;
    b = .616;
    FIND_ROOT(a, b, 1, tol, 2*F_PRECISION, largest_poly_arg);
    largest_poly_arg = purify(largest_poly_arg);

    old_precision = precision;
    precision = ceil(2*F_PRECISION/MP_RADIX_BITS) + 4;

    function erf_x_over_x ()
        {
        if ($1 == 0)
            return mu;
        else
            return erf($1)/$1;
        }

    remes(REMES_FIND_POLYNOMIAL + REMES_SQUARE_ARG + REMES_RELATIVE_WEIGHT,
       0, largest_poly_arg, erf_x_over_x, F_PRECISION + 1 + 3,
       &erf_poly_degree, &erf_poly_coefs);

    precision = old_precision;

/*
 * ERF(x) and ERFC(x) EVALUATION WHEN x IS NOT SMALL:
 * --------------------------------------------------
 *
 * As x approaches infinity, erf(x) approaches 1.  Eventually, erf(x) becomes
 * indistinguishable from 1 in machine format.
 *
 * Let t satisfy the equation
 *
 *		erf(t) = 1 - 1/2^(F_PRECISION + 1)
 *
 * Then if x >= t, erf(x) = 1 correctly rounded to machine precision.  Note
 * that the above equation is equivalent to 
 *
 *		erfc(t) = 1/2^(F_PRECISION + 1)
 */


    rho = 1 - 1/2^(F_PRECISION + 1);
    a = rho/mu;
    b = a/(1 - a*a/3);

#   undef  F
#   define F(x)	(erfc(x))
    FIND_ROOT(a, b, 1/2^(F_PRECISION + 1), tol, 3*F_PRECISION, erf_max_x);


/*
 * Similarly, ss x approaches minus infinity, erfc(x) approaches 2. Eventually,
 * erf(x) becomes indistinguishable from 2 in machine format.
 *
 * Let t satisfy the equation
 *
 *		erfc(t) = 2 - 1/2^F_PRECISION
 *
 * Then if x < t, erfc(x) = 2 correctly rounded to machine precision.
 */


    rho = 2 - 1/2^F_PRECISION;
    a = -erf_max_x;
    b = a/(1 - a*a/3);

#   undef  F
#   define F(x)	(erfc(x))
    FIND_ROOT(a, b, rho, tol, 3*F_PRECISION, erfc_min_x);


/*
 * Similarly, as x approaches infinity, erfc(x) approaches 0.  If m is the
 * smallest power of two that is representable, and v statisfies the equation
 *
 *		erfc(v) = 2^(m - 1)
 *
 * it follows that for x > v, the erfc(x) underflows to zero, while for x <= v,
 * erfc(v) is non-zero.
 *
 * For IEEE data types, there is a point at which erfc(x) becomes
 * denormalized.  If m' is the smallest power of 2 that is representable as
 * a normalized number, and u statisfies the equation
 *
 *		erfc(u) = 2^(m' - 1)
 *
 * If follows that if x > u, the erfc(x) is denormalized, while for x <= u,
 * erfc(x) is normalized.
 */


    min_bin_exp =
        IEEE_FLOATING ? (F_MIN_BIN_EXP - F_PRECISION + 1) : F_MIN_BIN_EXP;

    /* use erfc(x) ~ exp(-x^2)/(x*sqrt(pi)) to get a and b */

    a = sqrt(-((min_bin_exp - 1)*log(2) + log(pi)/2));
    b = sqrt(-((min_bin_exp - 1)*log(2) + log(pi)/2) + log(a));
    c = 2^(min_bin_exp - 1);

#   undef  F
#   define F(x)	(erfc(x))
    FIND_ROOT(a, b, c, tol, 3*F_PRECISION, erfc_underflow_x);

    if ( IEEE_FLOATING )
        { /* Compute denorm threshold */
        a = sqrt(-((F_MIN_BIN_EXP - 1)*log(2) + log(pi)/2));
        b = sqrt(-((F_MIN_BIN_EXP - 1)*log(2) + log(pi)/2) + log(a));
        c = 2^F_MIN_BIN_EXP;

        FIND_ROOT(a, b, c, tol, 3*F_PRECISION, erfc_denorm_x);
        }


/*
 * For large x, using the asymtotic approximation (3) is the most efficient
 * means of computing erfc(x) and erf(x) as 1 - erfc(x).  Since (3) is an
 * asymtotic approximation, there is a smallest x for each precision for
 * which (3) can be used to approximate erfc(x).  I.e. there is a value x0,
 * such that if x < x0, then the relative error in (3) is greater than
 * 2^(F_PRECISION + 1), regardless on how many terms are used.  As noted
 * above, there is an x1, such that if x > x1 then erf(x) = 1 to machine
 * precision.  Using equation (3) and Sterling's approximation for n!, it
 * possible to show that x0 and x1 are very close and that x0 < x1.  What
 * this implies is that the evaluation for erf(x) and erfc(x) on the
 * non-polynomial range be divided into two pieces:
 *
 *	Argument range		erf(x) evaluation	erfc(x) evaluation
 *	--------------		-----------------	------------------
 *	    x < x1		based on (4)		based on (4)
 *	   x1 <= x 		   1			based on (3)
 *
 * When evaluating erfc(x) based on (3), we include the constant 1/sqrt(pi)
 * into the polynomial coefficients and consequently the lead coefficient is
 * 1/sqrt(pi) = .5641895... = 1/2 + alpha, where alpha = 1/sqrt(pi) - .5.
 * Note that 1/2 and alpha have a 3 bit alignment shift.  Therefore we can
 * can improve the overall accuracy of the approximation by rewritting (3)
 * in the form:
 *
 *		erfc(x) = exp(-x^2)*z*[.5 + p(z^2)] where z = 1/x	(5)
 *
 */
    

    old_precision = precision;
    precision = ceil(2*F_PRECISION/MP_RADIX_BITS) + 4;

    function x_exp_x2_erfc_x()
        {
        return ($1)*exp($1*$1)*erfc($1);
        }

    erf_max_x = purify(erf_max_x);
    erfc_underflow_x = purify(erfc_underflow_x);

    remes(
       REMES_FIND_POLYNOMIAL + REMES_RECIP_SQUARE_ARG + REMES_RELATIVE_WEIGHT,
       erf_max_x, erfc_underflow_x, x_exp_x2_erfc_x, F_PRECISION + 1 + 3,
       &erfc_poly_degree, &erfc_poly_coefs);

    precision = old_precision;


/*
 * When evaluating erfc(x) using (4) it is useful to note that it can be
 * rewritten as:
 *
 *		erfc(x) = exp(-x*x)*f(x)
 *
 * where f(x) = exp(x*x)*erfc(x).  It can be shown that f(x) positive and
 * monotonicly decreasing.  Further, f(x) decreases ~ 1/x.  If we are
 * going to approximate erfc(x) on the interval [a, b], then for each
 * negative power of two between f(a) and f(b) we can find an interval
 * in [a, b], call it [c(n), c(n+1)], such that
 *
 *		1/2^n - f(c(n))   f(c(n)) - 1/2^(n+1)
 *		--------------- = --------------------
 *		     1/2^n            1/2^(n+1)
 *
 * An then approximate erfc(x) on [c(n), c(n+1)] as
 *
 *		erfc(x) = exp(-x*x)*[1/2^n + R(x)]
 *		        = exp(-x*x)/2^n*[1 + 2^n*R(x)]
 *
 * Where R(x) is a rational function, exp(-x*x)/2^n can be computed by
 * adjusting the scale factor for the special exp entry point and the scale
 * factor of 2^n can be incorporated into the coefficients of R.
 *
 *	NOTE: for the precision we are interested in (23, 53 and 113)
 *	at most 4 intervals are required.  However, time does not
 *	permit implementation of this scheme.  Instead we will use one
 *	expansion with n = 3.
 */
    

    old_precision = precision;
    precision = ceil(2*F_PRECISION/MP_RADIX_BITS) + 4;

    function exp_x2_erfc_x() { return exp($1*$1)*erfc($1); }

    erf_max_x = purify(erf_max_x);

    remes(
       REMES_FIND_RATIONAL + REMES_LINEAR_ARG +
         REMES_RELATIVE_WEIGHT + REMES_INIT_LEFT_CHEBY,
       largest_poly_arg, erf_max_x, exp_x2_erfc_x, F_PRECISION + 1 + 3,
       &erfc_num_degree, &erfc_den_degree, &erfc_rational_coefs);

    precision = old_precision;

    /* Copy numerator coefficients and pad out to same number as denominator */
    first_den_coef = erfc_num_degree + 1;
    for (i = 0; i <= erfc_num_degree; i++)
        erfc_num_coefs[i] = erfc_rational_coefs[i];

    while (erfc_num_degree < erfc_den_degree)
        erfc_num_coefs[++erfc_num_degree] = 0;

    /* Copy denominator coefficients and subtract from numerator */
    for (i = 0; i <= erfc_den_degree; i++)
        {
        erfc_den_coefs[i] = erfc_rational_coefs[i + first_den_coef];
        erfc_num_coefs[i] = 8*erfc_num_coefs[i] - erfc_den_coefs[i];
        }


/*
 *
 * COMPUTING EXP(-x^2)
 * -------------------
 *
 * Expansion (3) involves the compution of exp(-x^2).  Since small variations
 * in the argument to exp results in large errors in the result, it is
 * necessary to compute -x^2 to extra precision.  The basic approach is to
 * compute x^2 in hi and lo pieces and note that exp(-hi) can be computed
 * in extra precision (using the special exp entry point) as exp(-hi) =
 * 2^I*(fhi + flo).   Exp(-lo) can be computed as a polynomial of the form
 * 1 - lo*Q(lo).  Combining the above, we have:
 *
 *	exp(-x^2) = exp(-(hi + lo))
 *	          = exp(-hi) * exp(-lo)
 *	          = 2^I*(fhi + flo) * [1 - lo*Q(lo)]
 *	          = 2^I*{ fhi + flo - (fhi + flo)*lo*Q(lo) }
 *	          = 2^I*{ fhi + [flo - f*lo*Q(lo)]}
 *	          = 2^I*{ fhi + V }
 *
 * Noting the computation of exp(-x^2), using expansion (3) to compute erfc(x) 
 * results in a computation of the form:
 *
 *	erfc(x) = exp(-x^2)*z*[.5 + P(z^2)]			(5)
 *	        = 2^I*[ fhi + V ]*z*[.5 + P(z^2)]
 *	        = 2^(I - 1)*z*[ fhi + V ]*z*[1 + 2*P(z^2)]
 *	        = 2^(I - 1)*z*[ fhi + V + (fhi + V)*2*P(z^2)]
 *	        = 2^(I - 1)*z*[ fhi + U(x)]
 *
 * Based on the above, we have the following approach to computing erfc(x)
 *
 *	(1) get x^2 as hi and lo pieces
 *	(2) call special exp entry to get I(x), fhi and flo
 *	(3) V <-- flo - (fhi + flo)*lo*Q(lo)
 *	(4) z <-- 1/x
 *	(4) U <-- V + (fhi + V)*2*P(z^2)
 *	(5) result <-- 2^(I - 1)*z*(fhi + V)
 *
 * Note that in step 4, the factor of 2 can be incorporated into the
 * coefficients of P
 */


    /* Adjust erfc coefficients */

    for (i = 0; i <= erfc_poly_degree; i++)
        erfc_poly_coefs[i] = 2*erfc_poly_coefs[i];

    erfc_poly_coefs[0] = erfc_poly_coefs[0] - 1;

    @end_divert
#endif

/*
 * In the above discussion, we needed to compute x^2 = hi + lo and Q(lo).
 * There are basically two ways to obtain hi and lo, depending on whether or
 * not there is backup precision.
 *
 * If there is backup precision then
 *
 *		t  <-- ((B_TYPE) x)^2
 *		hi <-- (F_TYPE) t
 *		lo <-- (F_TYPE) (t - (B_TYPE) hi)
 *
 * When computed this way, the alignment shift between hi and lo is at least
 * F_PRECISION + 1 bits.
 *
 * If there is no backup precision, then x must be broken into hi and lo
 * pieces.  Then
 *
 *		x^2 = (xhi + xlo)^2
 *		    = (xhi + xlo)*(xhi + xlo)
 *		    = xhi*(xhi + xlo) + xlo*(xhi + xlo)
 *		    = xhi*xhi + xhi*xlo + xlo*(xhi + xlo)
 *		    = xhi^2 + xlo*(xhi + xhi + xlo)
 *		    = xhi^2 + xlo*(xhi + x)
 *		    = hi + lo
 *
 * There are several ways to obtain xhi and xlo, but for simplicity we will
 * assume that they are obtained by conversion to R_TYPE.
 *
 *	NOTE: the macro SPECIAL_EXP uses a temporary location _scale.
 *	This is to accommodate a hack in exp for Alpha VMS and NT.
 *	When these platforms handle 64 integers, then the use of _scale
 *	can be removed.
 */

#if defined(PRECISION_BACKUP_AVAILABLE)

#   undef  PRECISION_BACKUP_AVAILABLE
#   define PRECISION_BACKUP_AVAILABLE	1
        
#   if !defined(X_SQR_TO_HI_LO)
#	define X_SQR_TO_HI_LO(x, t, hi, lo) { \
	    t = (B_TYPE) x; \
	    t = t*t; \
	    hi = (F_TYPE) t; \
	    lo = (F_TYPE)(t - (B_TYPE) hi) ; \
	    }
#   endif

#   if !defined(SPECIAL_EXP)
#	define SPECIAL_EXP(x, t, i, hi, lo) { \
	    EXP_WORD_TYPE _scale; \
	    t = F_EXP_SPECIAL_ENTRY_NAME(x, &_scale); \
	    i = (WORD) _scale; \
	    hi = (F_TYPE) t; \
	    lo = (F_TYPE) (t - (B_TYPE)hi); \
	    }
#   endif

#else

#   define PRECISION_BACKUP_AVAILABLE	0

#   if !defined(X_SQR_TO_HI_LO)
#	define X_SQR_TO_HI_LO(x, t, hi, lo) { \
	    hi = (F_TYPE)((R_TYPE) x); \
	    lo = x - hi; \
	    lo = lo*(hi + x); \
	    hi = hi*hi ; \
	    }
#   endif

#   if !defined(SPECIAL_EXP)
#	define SPECIAL_EXP(x, t, i, hi, lo) { \
	    EXP_WORD_TYPE _scale; \
	    hi = F_EXP_SPECIAL_ENTRY_NAME(x, &_scale, &lo); \
	    i = (WORD) _scale; \
	    }
#   endif

#endif

/*
 * The low order POW2_K bits in the scale factor from the special exp
 * entry point contains the index into the exp table.  Since its use is not
 * required in erf/erfc we want to mask off the low bits.  While we're at it,
 * we can align it with the exponent field.
 */

#define	EXP_SCALE_MASK			((WORD) ~ MAKE_MASK(POW2_K, 0))
#if (F_EXP_POS >= POW2_K)
#   define ADJUST_AND_ALIGN_SCALE(s)	(((s) & EXP_SCALE_MASK) \
					   << (F_EXP_POS - POW2_K));
#else
#   define ADJUST_AND_ALIGN_SCALE(s)	(((s) & EXP_SCALE_MASK) \
					   >> (POW2_K - F_EXP_POS));
#endif

#if defined(MAKE_INCLUDE)
    @divert -append divertText

    if (PRECISION_BACKUP_AVAILABLE)
        {
        t = bround(erfc_underflow_x*erfc_underflow_x, F_PRECISION);
        n = bexp(t);
        max_x_sqr_lo = bldexp(1., n - F_PRECISION);
        }
    else
        {
        n = bexp(erfc_underflow_x);
        t = bround(erfc_underflow_x, R_PRECISION);
        s = bldexp(1., n - (R_PRECISION + 1));
        max_x_sqr_lo = s*(t + erfc_underflow_x);
        }


    /* Now compute the polynomial for exp(lo) */

    old_precision = precision;
    precision = ceil(2*F_PRECISION/MP_RADIX_BITS) + 8;

    function exp_m1_ov_x() 
        {
        if ($1 == 0)
            return 1;
        else
            return expm1(-$1)/(-$1);
        }

    remes(REMES_FIND_POLYNOMIAL + REMES_LINEAR_ARG + REMES_RELATIVE_WEIGHT,
       0, max_x_sqr_lo, exp_m1_ov_x, F_PRECISION + 1, &exp_poly_degree,
       &exp_poly_coefs);

    precision = old_precision;


#   define F_PRINT_A_DEFINE(name)	PRINT_TABLE_ADDRESS_DEFINE(name, \
					    TABLE_NAME, offset, F_TYPE)
#   define F_PRINT_V_DEFINE(name)	PRINT_TABLE_VALUE_DEFINE(name, \
					    TABLE_NAME, offset, F_TYPE)
#   define F_PRINT_ENTRY(value)		PRINT_1_F_TYPE_ENTRY(value, offset)
#   define PRINT_COEFS(a, n, d)		F_PRINT_A_DEFINE(d); \
					    TABLE_COMMENT(STR(a)); \
					    for (i = 0; i <= n; i++) \
					        { F_PRINT_ENTRY(a[i]); }

    printf("\n#include \"dpml_private.h\"\n\n");
    IF_MAKE_COMMON( printf("\n#if !defined(DEFINES_ONLY)\n\n"); )

    START_TABLE(TABLE_NAME, offset);

    TABLE_COMMENT("2/sqrt(pi) - 1, 8 and 1/8" );
    F_PRINT_V_DEFINE(TWO_OVER_SQRT_PI_M1);
    F_PRINT_ENTRY(mu - 1);
    F_PRINT_V_DEFINE(EIGHT);
    F_PRINT_ENTRY(8);
    F_PRINT_V_DEFINE(ONE_EIGTH);
    F_PRINT_ENTRY(1/8);

    erf_poly_coefs[0] = erf_poly_coefs[0] - 1;
    PRINT_COEFS(erf_poly_coefs, erf_poly_degree, ERF_POLY_COEFS);

    PRINT_COEFS(erfc_poly_coefs, erfc_poly_degree, ERFC_POLY_COEFS);

    PRINT_COEFS(exp_poly_coefs, exp_poly_degree, EXP_POLY_COEFS);

    PRINT_COEFS(erfc_num_coefs, erfc_num_degree, ERFC_NUM_COEFS);

    PRINT_COEFS(erfc_den_coefs, erfc_den_degree, ERFC_DEN_COEFS);

    END_TABLE;

    IF_MAKE_COMMON(
        printf("\n#else\n\n");
        printf("    extern const " STR(F_TYPE) " " STR(TABLE_NAME) "[];\n");
        printf("\n#endif\n\n");
        )
    
    printf("#define ERF_POLY(t,z)\t\tPOLY_%i_ALL(t, ERF_POLY_COEFS, z)\n",
        erf_poly_degree);

    printf("#define ERFC_POLY(t,z)\t\tPOLY_%i_ALL(t, ERFC_POLY_COEFS, z)\n",
        erfc_poly_degree);

    printf("#define EXP_POLY(t,z)\t\tPOLY_%i_ALL(t, EXP_POLY_COEFS, z)\n",
        exp_poly_degree);

    printf("#define ERFC_NUM_POLY(t,z)\tPOLY_%i_ALL(t, ERFC_NUM_COEFS, z)\n",
        erfc_num_degree);

    printf("#define ERFC_DEN_POLY(t,z)\tPOLY_%i_ALL(t, ERFC_DEN_COEFS, z)\n",
        erfc_den_degree);


    /*
     * The following function returns an "integer" that has the same bit
     * pattern as the floating point value.  (NOTE: for VAX data types
     * the floating point "bit pattern" is after a PDP_SHUFFLE.) 
     */


#   if IEEE_FLOATING

#       define _F_SIGN_BIT_POS	F_SIGN_BIT_POS
#       define _F_EXP_POS	F_EXP_POS

#   else

#       define _F_POS_ADJ	(NUM_INT_BITS - 16)
#       define _F_SIGN_BIT_POS	(F_SIGN_BIT_POS + _F_POS_ADJ)
#       define _F_EXP_POS	(F_EXP_POS + _F_POS_ADJ)

#   endif

#   define	HEX_FMT	PASTE_3(HEX_FORMAT_FOR_, NUM_INT_BITS, _BITS)

    function as_int()
        {
        _sign = 0;
        if ($1 < 0) _sign = 1;
        exponent = bexp($1);

        _y = trunc(bldexp($1, _F_EXP_POS + 1 - exponent));
        _i = (_sign << _F_SIGN_BIT_POS) +
             ((exponent + F_EXP_BIAS - F_NORM - 2) << _F_EXP_POS)
             + _y;
        return _i;
        }

    printf("#define MAX_POLY_ARG\t\t" HEX_FMT "\n", as_int(largest_poly_arg));
    printf("#define MIN_ERF_POLY_ARG\t" HEX_FMT "\n",
      as_int(smallest_erf_poly_arg));
    printf("#define MIN_ERFC_POLY_ARG\t" HEX_FMT "\n",
      as_int(smallest_erfc_poly_arg));
    printf("#define ERFC_MAX_CONSTANT_ARG\t(" HEX_FMT " - (U_WORD) "
      HEX_FMT ")\n", as_int(-erfc_min_x), bldexp(1, _F_SIGN_BIT_POS));
    printf("#define ERF_MIN_CONSTANT_ARG\t" HEX_FMT "\n", as_int(erf_max_x));
    printf("#define MIN_ASYMTOTIC_ARG\t" HEX_FMT "\n", as_int(erf_max_x));
    printf("#define MIN_UNDERFLOW_ARG\t" HEX_FMT "\n",
      as_int(erfc_underflow_x));

    @end_divert

#   define TMP_FILE             ADD_EXTENSION(BUILD_FILE_NAME,tmp)
    @eval my $outText = MphocEval( GetStream( "divertText" ) );		\
          my $defineText = Egrep( "#define", $outText, \$tableText);	\
          my $headerText = GetHeaderText( STR(BUILD_FILE_NAME),     	\
                       "Definitions and constants for " .       	\
                       "erf and erfc functions", __FILE__);		\
          print "$headerText\n\n$tableText\n\n$defineText\n";

#endif

#if !defined(MAKE_INCLUDE)
#   include STR(BUILD_FILE_NAME)
#endif

#define	EXP_INC		SET_BIT(F_EXP_POS)

#if IEEE_FLOATING
#    define HAS_ABNORMAL_EXP(e)	((((e) + EXP_INC) & \
				MAKE_MASK(F_EXP_WIDTH - 1, F_EXP_POS + 1)) == 0)
#    define _F_SIGN_BIT_MASK	F_SIGN_BIT_MASK
#else
#    if (BITS_PER_WORD <= BITS_PER_F_TYPE)
#        define _F_SIGN_BIT_MASK	SET_BIT(BITS_PER_WORD - 1)
#    else
#        define _F_SIGN_BIT_MASK	SET_BIT(BITS_PER_F_TYPE - 1)
#    endif
#endif

/*
 * Depending on the sign of x, erfc(x) will be the "computed" value or
 * 2 + the "computed" value.
 */

#if F_COPY_SIGN_IS_FAST
#   define ADD_IN_ERFC_CONST(i,x,u,z)	F_COPY_SIGN((F_TYPE) 1.0, x, u); \
					u = (F_TYPE) 1.0 - u; \
					z += u
#else
#   define ADD_IN_ERFC_CONST(i,x,u,z)	if ((i) < 0) z += (F_TYPE) 2.0
#endif

#if !defined F_ENTRY_NAME
#   define F_ENTRY_NAME	_F_ENTRY_NAME
#endif

F_TYPE F_ENTRY_NAME (F_TYPE x)
    {
    EXCEPTION_RECORD_DECLARATION
    F_TYPE z, w, y, u, fhi, flo, hi, lo;
    WORD s_exp_word, exp_word, scale;
    F_UNION _u_;

#   if defined(PRECISION_BACKUP_AVAILABLE)
        B_TYPE t;
#   endif

#   if defined(ERF)
        WORD exp_field, index;
#   else
        F_TYPE v;
#   endif

    _u_.f = x;

    IF_IEEE(s_exp_word = _u_.F_SIGNED_HI_WORD;)
    IF_VAX( s_exp_word = _u_.F_HI_WORD;
            s_exp_word = SIGN_EXTENDED_PDP_SHUFFLE(s_exp_word);)

    IF_IEEE(if (HAS_ABNORMAL_EXP(s_exp_word)) goto ieee_abnormal_arguments;)

    /* Get "|x|" and branch to the right code for the size of x */
    exp_word = s_exp_word & (~(-_F_SIGN_BIT_MASK));

    if (exp_word > MAX_POLY_ARG)
        goto not_a_poly_argument;

    if (exp_word <= SELECT(MIN_ERF_POLY_ARG, MIN_ERFC_POLY_ARG))
        goto identity_range;

    /* Just need to do a polynomial evaluation for these arguments */
    w = x*x;
    ERF_POLY(w, z);
    z = x*z;

    /*
     * Add in last term.  For erf, add in x, for erfc, carefully add in 
     * 1 - x
     */

    SELECT( z = x + z; , y = (F_TYPE) 1. - x;
                         w = x - ((F_TYPE) 1. - y);
                         z = y - (w + z);
          )

    return z;

not_a_poly_argument:

    /*
     * If x is large positive number (erf) or a large negative number (erfc)
     * then we can just return a constant
     */

    if (
       SELECT(exp_word   >  ERF_MIN_CONSTANT_ARG ,
              s_exp_word >= ERFC_MAX_CONSTANT_ARG)
       ) goto return_constant;

    /* If x is a large positive number, erfc will underflow */
  
    IF_ERFC( if (exp_word > MIN_UNDERFLOW_ARG) goto underflow; )

    /*
     * To approximate erf or erfc for arguments in this range, we need to
     * compute exp(-x^2).  Note that the special exp entry returns scale
     * as the value 2^L*n + m, where n is the binary exponent of exp(-x^2)
     * and m is the index into the exp data table.  Since we only need n,
     * mask of low bits and align with exponent field
     */

    X_SQR_TO_HI_LO(x, t, hi, lo);
    SPECIAL_EXP(-hi, t, scale, fhi, flo);
    scale = ADJUST_AND_ALIGN_SCALE(scale);

    /*
     * For erfc, if x is really big, we need to use an asymtotic approximation
     */

    IF_ERFC(if (exp_word > MIN_ASYMTOTIC_ARG) goto needs_asymtotic;)

    /*
     * In the medium range, we use approximation (4) in the form
     *
     *		erfc(x) = exp(-x^2)/4*[1 + R(x)]
     *
     * In this range, we also need to work with |x| and restore the sign
     * at the end.
     */

    F_ABS(x, z);
    ERFC_NUM_POLY(z, u);
    ERFC_DEN_POLY(z, y);
    y = u/y;
    EXP_POLY(lo, u);
    flo = flo - (fhi + flo)*lo*u;

    z = fhi + (flo + (fhi + flo)*y);
    /* z = fhi*y + flo*y; */
    _u_.f = z;
    _u_.F_HI_WORD += (scale - 3*EXP_INC);
    z = _u_.f;
    IF_ERF(z = (F_TYPE) 1.0 - z;)
    if (s_exp_word >= 0) goto return_z;

    /* Negate for erf, subtract from 2 for erfc */
    z = SELECT(-z,  (F_TYPE) 2.0 - z);

return_z:
    return z;

#if defined(ERFC)

needs_asymtotic:

    z = ((F_TYPE) 1.)/x;
    EXP_POLY(lo, u);
    v = flo - (fhi + flo)*lo*u;
    u = fhi + v;
    w = z*z;
    ERFC_POLY(w, y);
    u = v + u*y;
    z = (z*fhi) + (z*u);	/* Slightly better error bound this way */

    /* Scale by power of two, looking out for underflows and denorms */
    _u_.f = z;
    scale -= EXP_INC;	/* Adj for factor of 2 in ERFC_POLY */
    s_exp_word = _u_.F_HI_WORD;
    _u_.F_HI_WORD = s_exp_word + scale;
    scale = (s_exp_word & F_EXP_MASK) + scale;
    if (scale <= 0) goto denorm_or_underflow;
    z = _u_.f;
    return z;
       
denorm_or_underflow:

#   if IEEE_FLOATING

    if ((WORD) (scale + ALIGN_W_EXP_FIELD(F_PRECISION - 1)) >= 0)
        {

        /*
        ** At this point, we have z = 2^n*f is in the denormalized range.
        ** Redefine z to be 2^0*f.
        */

        s_exp_word = (s_exp_word & ~F_SIGN_EXP_MASK) |
          ALIGN_W_EXP_FIELD(F_EXP_BIAS);
        _u_.F_HI_WORD = s_exp_word;
        z = _u_.f;

        /* Get 'w' = 2^k, where k is the number of bits "of denormalization" */

        CLEAR_LOW_BITS(_u_);
        s_exp_word = (s_exp_word & F_SIGN_EXP_MASK) - scale + EXP_INC;
        _u_.F_HI_WORD = s_exp_word;

        /*
        ** compute 2^k + z and unscale the exponent field to get the correct
        ** denormalized result
        */

        _u_.f += z;
        _u_.F_HI_WORD -= (s_exp_word & F_EXP_MASK);
        z = _u_.f;

        if ( (z != (F_TYPE) 0.) && PROCESS_DENORMS)
            return z;
        }

#   endif

#endif

#if defined(ERFC)
underflow:
#endif
    GET_EXCEPTION_RESULT_1(ERFC_UNDERFLOW, x, z);
    return z;

identity_range:
    SELECT(  z = EIGHT*x;
             z = (z + TWO_OVER_SQRT_PI_M1*z)*ONE_EIGTH, z = (F_TYPE) 1.0);
    return z;


#if IEEE_FLOATING

    ieee_abnormal_arguments:

        if ((s_exp_word & F_EXP_MASK) == F_EXP_MASK)
            goto nan_or_inf;

        /* If we get here, x is either 0 or denorm. */

#    if defined(ERFC)
        return (F_TYPE) 1.0;
#    else
        /* Scale up argument so that we can safely muliply by 2/sqrt(pi) - 1 */
        DENORM_TO_NORM(x, z);
        z = z + TWO_OVER_SQRT_PI_M1*z;

        /*
         * Now unscale.  Underflow is not possible here but the result may
         * be denormal.  So we need to check the exponent field.
         */

        _u_.f = z;
        exp_word = _u_.F_HI_WORD;
        exp_field = exp_word & F_EXP_MASK;
        exp_word ^= exp_field;
        index = exp_field - __LOG2_DENORM_SCALE_ALIGNED_W_EXP;
        if (index <= 0) goto erf_denorm;
        _u_.F_HI_WORD = exp_word | index;
        z = _u_.f;
        return z;

    erf_denorm:
        exp_field -= (index - ALIGN_W_EXP_FIELD(1));
        _u_.F_HI_WORD = (exp_word | exp_field) & F_SIGN_EXP_MASK;
        CLEAR_LOW_BITS(_u_);
        _u_.f += z;
        _u_.F_HI_WORD -= exp_field;
        z = _u_.f;
        return z;

#   endif


nan_or_inf:

	/* If x is a NaN, return it. */

	if ((s_exp_word & F_MANTISSA_MASK) OR_LOW_BITS_SET(_u_))
		return x;

	/* Otherwise, x is an infinity. Fall through to return_constant. */

#endif


return_constant:

	if (s_exp_word & _F_SIGN_BIT_MASK)
		z = (F_TYPE) SELECT( -1.0, 2.0);
	else
		z = (F_TYPE) SELECT( 1.0, 0.0);
	return z;

    }


