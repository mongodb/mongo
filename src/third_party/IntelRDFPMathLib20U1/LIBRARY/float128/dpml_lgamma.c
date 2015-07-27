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

#ifndef BASE_NAME
#    define BASE_NAME   LGAMMA_BASE_NAME
#endif

#include "dpml_private.h"

#if (F_NAME_SUFFIX == DPML_NULL_MACRO_TOKEN)
#    define  SIGNGAM_NAME signgam
#else
#   if (OP_SYSTEM == vms)
#      define  SIGNGAM_NAME PASTE_3(F_NAME_PREFIX,signgam, F_NAME_SUFFIX)
#   else
#      define  SIGNGAM_NAME PASTE_2(signgam, F_NAME_SUFFIX)
#   endif
#endif

#if HACK_GAMMAS_INLINE
#	define SIGNGAM      SIGNGAM_NAME
#else
#	define SIGNGAM      *SIGNGAM_NAME
#endif


    /*
     * Lgamma(x) is defined as the log(|gamma(x)|), where gamma(x) is defined
     * for positive x as
     *
     *      gamma(x) = integral{ 0 to infinity | t^(x-1)e^t dt }
     *
     * From the definition of gamma(x) it follows that
     *
     *              x*gamma(x) = gamma(x+1)             (1)
     *
     * and the limit as x --> +0 of gamma(x) = +infinity.  Equation (1) can be
     * used to extend gamma(x) to negitive numbers by recursively applying:
     *
     *      gamma(-x) = gamma(1 - x)/(-x)               (2)
     *
     * Since gamma(0) = + infinity, it follows that gamma(n) is undefined for
     * any non-positive integer.  An alternative extension of gamma to negative
     * arguments is the reflection fomula
     *
     *      gamma(-x) = -pi/(sin(pi*x)*gamma(1 + x))     (3)
     *
     * Evalutation of lgamma(x) suffers potential loss of significance at
     * its zeros or alternatively, when |gamma(x)| = 1.  From the definition
     * of gamma and (1) we see that |gamma(x)| = 1 for positive x only at
     * x = 1 and 2.  From equation (2), we see that |gamma(x)| = 1 when x
     * is a negative integer  +/- epsilon, where epsilon is on the order
     * of 1/n!.
     *
     * Computation of lgamma(x) is based on two identities:
     *
     *                                    zeta(2)-1      zeta(3)-1
     *   lgamma(1+x) = (1-G)x - ln(1+x) + ---------x^2 - ---------x^3 + ...
     *                                        2              3
     *
     *                     zeta(n)-1      
     *                     ---------(-x)^n ...                    (4)
     *                         n
     *
     *               = -ln(1+x) + x*Q(x)
     *
     * where G is Euler's constant and zeta(n) is the Reimann zeta function:
     *
     *                             1     1     1
     *              zeta(n) = 1 + --- + --- + --- + ...
     *                            2^n   3^n   4^n
     *
     * and Stirlings asymtotic approximation to gamma(x):
     *
     *                   1                      1            1       1
     *      lgamma(x) ~ ---ln(2*pi) - x + (x - ---)*ln(x) + ---*phi(---)    (5)
     *                   2                      2            x      x^2
     *
     *           1      B(2)     B(4)     B(6)      B(8)
     *      phi(---) = ----- - ------- + ------- - ------- .....         (6)
     *          x^2     2*1    4*3*x^2   6*5*x^4   8*7*x^6
     *
     * where B(n) is the n-th bernoulli number.
     */

#   define TMP_FILE             ADD_EXTENSION(BUILD_FILE_NAME,tmp)
#   define RND_TO_FMT(x)        bround(x, F_PRECISION)
#   define PRINT_TABLE_ENTRY(a) PRINT_1_F_TYPE_ENTRY(a, offset)
#   define PRINT_TABLE_VALUE_F_DEFINE(n) \
                PRINT_TABLE_VALUE_DEFINE(n, TABLE_NAME, offset, F_TYPE)
#   define F_PRINT_A_DEFINE(name)  PRINT_TABLE_ADDRESS_DEFINE(name, \
                                          TABLE_NAME, offset, F_TYPE)

#ifndef MAKE_INCLUDE

#    include STR(BUILD_FILE_NAME)

#else

    @divert divertText

    precision = ceil(2*F_PRECISION/MP_RADIX_BITS) + 2;

    /*
     * The following macro defines an mphoc routine that finds a root of
     * the function f between x_0 and x_1 to precsion p and returns the
     * result in y.
     */

#   define FIND_ROOT(x_0, x_1, f, p, y) \
        y_0 = f(x_0); y_1 = f(x_1); \
        if (y_0 * y_1 > 0) \
            { \
            printf("Invalid input to FIND_ROOT\n"); \
            exit; \
            } \
        while (1) \
            { \
            delta = x_1 - x_0; \
            if (bexp(x_1) - bexp(delta) > p) \
                break; \
            x = x_1 - y_1*(delta/(y_1 - y_0)); \
            x_0 = x_1; x_1  = x; \
            y_0 = y_1; y_1 = f(x); \
            } \
        y = x_1


    START_STATIC_TABLE(TABLE_NAME, offset);
    TABLE_COMMENT("Miscelaneous constants");

    /*
     * lgamma(x) will overflow for large positive values of x.  Note that 
     * For large negative values of x, x is a negative integer, and hence
     * the function is not defined.  To compute the overflow threshold
     * we need to solve the equatation lgamma(x) = MP_MAX_FLOAT + 1/2 lsb
     * and rounding down the result to working precision.  We do this using
     * the macro FIND_ROOT defined above with f = lgamma(x) - MP_MAX_FLOAT + 
     * 1/2 lsb.  To find the starting values we note that lgamma is ~ x*log(x)
     * and assume that x = 2^k/k*log(2).  Then x*log(x) ~ MP_MAX_FLOAT when
     * k = F_MAX_BIN_EXP
     */

    k = F_MAX_BIN_EXP;
    x0 = 2^k/(k*log(2));
    x1 = 3*x0;

    c = MP_MAX_FLOAT + 2^(bexp(MP_MAX_FLOAT) - F_PRECISION);
    function f() { return lgamma($1) - MP_MAX_FLOAT; }

    FIND_ROOT(x0, x1, f, F_PRECISION + 1, y);
    y = bchop(y, F_PRECISION);

    PRINT_TABLE_VALUE_F_DEFINE(OVERFLOW_THRESHOLD);
    PRINT_TABLE_ENTRY(y);

    /*
     * For large values of x, it is most efficient to use equation (5).
     * When x is very large, 1/x^2 will underflow.  However, long before
     * the underflow threshold is reached, (1/x)*phi(1/x^2) will become
     * insignificant when compared with the other terms in (5). 
     * Consequently, we should stop computing z(x) = (1/x)*phi(1/x^2) when
     * x is big enough.  This is more efficient and avoids the underflow.
     *
     * z(x) will be insignificant when z(x)/lgamma(x) < 1/2^(F_PRECISION + 1),
     * or when
     *
     *   (1 - 2^-(F_PRECISION+1))*lgamma(x)-.5*ln(2*pi)+x-(x-.5)*ln(x) < 0
     *
     * Using the macro, FIND_ROOT, we determine an x that satisfies the above.
     */

    a = 1 - 1/2^(F_PRECISION + 1);
    b = .5*log(2*pi);
    function g()
       {
       s = a*lgamma($1);
       t = ($1 - .5)*log($1) - $1 + b;
       return s - t;
       }

    k = .5*(F_PRECISION + 1 - log2(12.));
    x0 = 2^k/sqrt(k*log(2));
    x1 = x0 + x0;

    FIND_ROOT(x0, x1, g, F_PRECISION + 1, real_big);
    PRINT_TABLE_VALUE_F_DEFINE(REAL_BIG);
    PRINT_TABLE_ENTRY(real_big);

    /*
     * Using equation (5) requires the constant .5*ln(2*pi)
     */
    y = .5*log(2*pi);
    PRINT_TABLE_VALUE_F_DEFINE(HALF_LN_2_PI);
    PRINT_TABLE_ENTRY(y);

    /*
     * For suitably large negative x, we would like to a computation
     * based on equation (3).
     *
     *  lgamma(-x) = ln|gamma(-x)|
     *             = ln|-pi/(sin(pi*x)*x*gamma(x))|
     *             = ln(pi) - ln|sin(pi*x)| - ln(x) - ln(gamma(x))
     *             = ln(pi) - ln|sin(pi*x)| - ln(x) - lgamma(x)
     *             = ln(pi) - ln|sin(pi*x)| - ln(x) - lgamma(x)
     *
     * combined with (5) this gives:
     *
     *  lgamma(-x) ~ ln(pi) - ln|sin(pi*x)| - ln(x) - 
     *                [.5*ln(2*pi) - x + (x - .5)*ln(x) + phi(x)/x]
     *             ~ .5*ln(pi/2) - ln|sin(pi*x)| + x - (x + .5)*ln(x) - phi(x)/x
     *
     *  Consequently, we also need the constants .5*ln(pi/2) and pi
     */

    y = .5*log(pi/2);
    PRINT_TABLE_VALUE_F_DEFINE(HALF_LN_PI_OVER_2);
    PRINT_TABLE_ENTRY(y);

    y = pi;
    PRINT_TABLE_VALUE_F_DEFINE(PI);
    PRINT_TABLE_ENTRY(y);

    /*
     * When x is not large, the computation of lgamma is based on equations
     * (1) and (2).  Specifically, let
     *
     *          lgamma(n+x) = log(F(n,x)) + x*Q(x)
     *
     * where Q(x) is defined by equation (4).  From equation (1) it follows
     * that
     *
     *          lgamma(n+1+x) = log((n+x)*gamma(n+x)
     *                        = log(n+x) + lgamma(n+x)
     *                        = log(n+x) + log(F(n,x)) + x*Q(x)
     *                        = log[(n+x)*F(n,x)] + x*Q(x)
     *
     * From the above and equation (4) it follows that F(1,x) = 1+x and 
     * F(n+1, x) = (n+x)*F(n,x).  Note the F(n,x) is define for both
     * negative and positive integers.
     *
     * Since we know the range of our x value for this evaluation we can
     * increase the accuracy of the computation of x*Q(x) by performing
     * the following transformation:
     *
     *      Given Q(x) = p(x)/q(x), define R(X) as 
     *  
     *          Q(x) = 1/2 - R(x)
     *
     *      This yields
     *
     *          R(x) = p(x) - q(x)/2
     *                 -------------
     *                    q(x)
     *
     * Now x*Q(x) can be computed as x*(1/2 - R(x)), or rather x*(1/2) - x*R(x)
     * which forces, x*(1/2), the most significant term, to be exact.
     *
     *
     *    NOTE: We need coefficients for Q and phi.  From (4) we obtain
     *    Q by approximating 
     *
     *           (lgamma(1+x) + ln(1 + x))/x
     *
     *    on the interval [-.5, .5].  A rational approximation for Q
     *    has competative performance on ALPHA with a polynomial
     *    approximation.
     *
     *    From (5) we obtain phi by approximating
     *
     *           x * [lgamma(x) - .5*ln(2*pi) + x - (x - .5)*ln(x)]
     *
     *    on the range [8,max_val], where max_val is the largest
     *    value of x which will be evaluated by phi (i.e. for X>x, phi(x)
     *    is insignificant to the other terms of the sum in (5).
     */

    old_precision = precision;
    precision = ceil(2*F_PRECISION/MP_RADIX_BITS) + 4;

    function lgamma_approx()
         {
            if ($1 == 0)
                return (1 - euler_gamma);
            else {
                /* logx1(x) is more accurate than ln(x) for |x| < 1/MP_RADIX */
                if ( abs($1) < (1 / MP_RADIX))
                    return (lgamma(1+$1) + logx1($1))/$1;
                else        
                    return (lgamma(1+$1) + ln(1 + $1))/$1;
            }
         }

    /* To shorten our search time we'll make some initial estimates based
       on experience.  (These estimates are on the low side to assure we
       don't over step the optimal degree)
    */
#if (F_PRECISION == 24)
    degree = 3;
#elif (F_PRECISION == 53)
    degree = 5;
#elif (F_PRECISION == 113)
    degree = 11;
#else
    degree = 0;
#endif

    tol = 0;
    while (tol < (F_PRECISION + 1 + 3)) {
        den_degree = num_degree = ++degree;
        tol = remes( REMES_STATIC + REMES_LINEAR_ARG + REMES_RELATIVE_WEIGHT,
                     -0.5, 0.5, lgamma_approx, 
                     num_degree, den_degree, &rational_coefs);
    }

    precision = old_precision;


    /* Extract denominator coefficients */
    first_den_coef = num_degree + 1;
    for (i = 0; i <= den_degree; i++)
        q[i] = rational_coefs[i + first_den_coef];

    /* Extract numerator coefficients */
    for (i = 0; i <= num_degree; i++)
        p[i] = rational_coefs[i] - q[i]/2;




    /* Generate constants for Phi */

    old_precision = precision;
    precision = ceil(2*F_PRECISION/MP_RADIX_BITS) + 4;

    half_ln_of_2pi = .5*ln(2*pi);
    function lgamma_asym_approx()
         {
            x = $1;
            if (x == 0)
                return (1/12);      /* B2(0)/2  where B2(x) = x^2 - x + 1/6 */
            else
                return x*(lgamma(x) - half_ln_of_2pi + x - (x - .5)*ln(x));
         }

    max_arg = real_big;
#if QUAD_PRECISION
    max_arg = 100000;       /* This is temporary until mp_remes is corrected */
#endif
    remes(REMES_FIND_POLYNOMIAL+ REMES_RELATIVE_WEIGHT+ REMES_RECIP_SQUARE_ARG,
          8.0, max_arg, lgamma_asym_approx, (F_PRECISION + 1),
          &poly_degree, &r);
    precision = old_precision;



#define PRINT_COEFS(n,p)        for (i = 0; i <= n; i++) \
                                     { PRINT_TABLE_ENTRY(p[i]); }

    TABLE_COMMENT("Rational Coefficents for Q(x)");
    F_PRINT_A_DEFINE(P_COEFS);
    PRINT_COEFS(num_degree, p);
    printf("\n");

    F_PRINT_A_DEFINE(Q_COEFS);
    PRINT_COEFS(den_degree, q);

    TABLE_COMMENT("Polynomial Coefficents phi(x)");
    F_PRINT_A_DEFINE(PHI_COEFS);
    PRINT_COEFS(poly_degree, r);

    END_TABLE;

    /*
     * Print out defines for polynomial and rational approximations
     */

    printf("#define PHI(a,u)         u = a*a; u = a*POLY%i(PHI_COEFS, u)\n",
       poly_degree);
    printf("#define Q(x)             (POLY%i(P_COEFS, x)/POLY%i(Q_COEFS, x))\n",
       num_degree, den_degree);

    @end_divert
    @eval my $outText = MphocEval( GetStream( "divertText" ) ); 		\
          my $defineText = Egrep( "#define",  $outText, \$tableText );	\
          my $headerText = GetHeaderText( STR(BUILD_FILE_NAME),		\
                       "Definitions and constants for " .		\
                       STR(F_ENTRY_NAME),  __FILE__);			\
          print "$headerText\n\n$tableText\n\n$defineText";	
#endif


#if IEEE_FLOATING
#   define SCREEN_SPECIAL_ARGS(x,i)     GET_EXP_WORD(x, i); \
                                        if (F_EXP_WORD_IS_ABNORMAL(i)) \
                                            goto special_args
#else
#   define SCREEN_SPECIAL_ARGS(x,i)
#endif

#define NEG_2_POW_F_PRECISION   ALIGN_W_EXP_FIELD(F_PRECISION + F_EXP_BIAS - F_NORM) + \
                                   F_SIGN_BIT_MASK

#ifdef F_COPY_SIGN_FAST
#   define F_SET_SIGN(val, sign, res)   F_COPY_SIGN(val, sign, result)
#else
#   define F_SET_SIGN(val, sign, res)   res = val; if ((sign) < 0) F_NEGATE(res)
#endif


#if DO_LGAMMA

    int SIGNGAM_NAME = 0;
#   define _F_ENTRY_NAME	F_LGAMMA_NAME
#   define OPT_PTR_ARG
#   define USE_CALL		!HACK_GAMMAS_INLINE

#elif DO_GAMMA

    extern int SIGNGAM_NAME;
#   define _F_ENTRY_NAME	F_GAMMA_NAME
#   define OPT_PTR_ARG
#   define USE_CALL		!HACK_GAMMAS_INLINE

#else

#   define _F_ENTRY_NAME	F_RT_LGAMMA_NAME
#   define OPT_PTR_ARG		, int *SIGNGAM_NAME
#   undef  HACK_GAMMAS_INLINE
#   define USE_CALL		0

#endif

#if !defined F_ENTRY_NAME
#   define F_ENTRY_NAME	_F_ENTRY_NAME
#endif

F_F_PROTO( F_LN_NAME ) ;
F_F_PROTO( F_SIN_NAME ) ;

F_TYPE
F_ENTRY_NAME(F_TYPE x OPT_PTR_ARG)
{
    F_TYPE y;
    WORD i;

#if USE_CALL

    F_FpI_PROTO( F_RT_LGAMMA_NAME ) ;

    y = F_RT_LGAMMA_NAME(x, &i);
    SIGNGAM_NAME = i;
    return y;

#else

    EXCEPTION_RECORD_DECLARATION
    F_TYPE s, t;

    SIGNGAM = 1;

    /* screen for NaNs, infinities, zeros & denorms */
    SCREEN_SPECIAL_ARGS(x, i);

    /*
     * Initialize the SIGNGAM to 1 and send large arguments to asymtotic
     * region.  Note the choice of asymtotic region being |x| >= 8 is
     * fairly arbitrary and need not be symetric.  As the lower bound of
     * the asymtotic region increases, the more multiplies are performed
     * in computing F(n,x).  Eventually, it is faster to use the asymtotic
     * approximations.  Experimentally, it appears that the asymtotic
     * regions are not as accurate.  However, that might be caused by a
     * sloppy implementation in that region.
     *
     * For the non-asymtotic region, we need to compute rint(x).  Get 1/2
     * with the correct sign now.
     */

    F_SET_SIGN((F_TYPE) .5, x, y);

    if (x >= (F_TYPE) 8.)
        goto pos_asymtotic;
    if (x <= (F_TYPE) -8.)
        goto neg_asymtotic;

    /* For small x, get i = rint(x), y = x - i */

    i = (WORD)(x + y);
    y = x - (F_TYPE) i;
    t = (F_TYPE) 1.;

    /*
     * Compute F(n,x) and take its log.  In most cases this switch statement
     * is faster than a loop.
     */

    switch (i)
        {
        case -8:
            t *= (y - 8);
            /* Fall through */

        case -7:
            t *= (y - 7);
            /* Fall through */

        case -6:
            t *= (y - 6);
            /* Fall through */

        case -5:
            t *= (y - 5);
            /* Fall through */

        case -4:
            t *= (y - 4);
            /* Fall through */

        case -3:
            t *= (y - 3);
            /* Fall through */

        case -2:
            t *= (y - 2);
            /* Fall through */

        case -1:
            t *= (y - 1);
            /* Fall through */

        case 0:
            /*
             * Since all of the negative cases come through here, we need
             * to check for integer values and set signgam correctly;
             */
            t *= (y*(y+1));
            if (y == 0) goto non_pos_int;
            if (t < 0)
                SIGNGAM = -1;
            F_ABS(t, t);
            t = - F_LN_NAME(t);
            goto pos_eval;

        case 1:
            t = - F_LN_NAME(x);
            goto pos_eval;

        case 2:
            t = 0;
            goto pos_eval;

        case 8:
            t *= (x-6);
            /* Fall through */

        case 7:
            t *= (x-5);
            /* Fall through */

        case 6:
            t *= (x-4);
            /* Fall through */

        case 5:
            t *= (x-3);
            /* Fall through */

        case 4:
            t *= (x - 2);
            /* Fall through */

        case 3:
            t *= (x - 1);
            t = F_LN_NAME(t);
            goto pos_eval;
        }

pos_eval:
    /*
     * OK - just need to compute rational approximation and we're done.
     */
    t = t + (y*0.5 + y*Q(y));
    return t;

pos_asymtotic:

    /*
     * In this region we compute lgamma using an asymtotic expansion.
     * If x is really big, we don't need phi(x), so we can skip it.
     */
    t = HALF_LN_2_PI;
    if (x > REAL_BIG) goto skip_poly;
    y = 1/x;
    PHI(y, s);
    t += s;

add_in_log:
    y = F_LN_NAME(x);
    s = x * (y - 1);
    s -= (F_TYPE) .5 * y;
    t += s;

    return t;
    
skip_poly:
    /* If x is reaally, really big, result will overflow */
    if (x <= OVERFLOW_THRESHOLD)
        goto add_in_log;

    GET_EXCEPTION_RESULT_1(LGAMMA_OVERFLOW, x, t);
    return t;

neg_asymtotic:

    /*
     * Here we are dealing with large negative arguments we need to
     * determine an integer n, such that n <= x < n+1.  The parity
     * of n determines whether SIGNGAM is + or - 1.  Also, we are
     * going to compute log(|sin(pi*x)|).  If we can find and integer
     * k such that k = rint(x) and define y = x - k, then log(|sin(pi*x)|)
     * = log(sin(|y|*pi)).  We begin by using "+ big - big" to determine
     * k and y.
     */

    x = -x;
    s = F_POW_2(F_PRECISION - 1);
    if (x >= s)
        /* x is so big that it must be an integer */
        goto non_pos_int;
    y = x + s;
 
    /*
     * get the low fraction bits of y. These are the same as the low
     * bits of k
     */
    GET_LO_FRAC_WORD(y,i);
    i = PDP_SHUFFLE(i);
    t = y - s;
    y = x - t;

    /* Figure out n so we can set signgam correctly, and get |y| */
    if (y < 0)
        {
        i--;
        t--;
        y = -y;
        }
    if (x == t) goto non_pos_int;
    SIGNGAM = ((i + i) & 2) - 1;

    /* OK compute aymtotic polynomial approximation for lgamma(|x|) */
    s = ((F_TYPE) 1.)/x;
    PHI(s, t);
    t = HALF_LN_PI_OVER_2 - t;

    /* Get log(|sin(pi*x)|) and remainder of asymtotic approximation */
    s = F_LN_NAME(F_SIN_NAME(y*PI));
    t = x + (t - s);
    s = (x + (F_TYPE) .5)*F_LN_NAME(x);
    t = t - s;
    return t;



special_args:

#if IEEE_FLOATING

    /* Note:  The code below assumes that SIGNGAM has already been set to 1.
              Thus, we only bother to set it here when gamma(x) is known to be
              negative.
    */  

    F_CLASSIFY(x, i);
    switch (i)
        {
        case F_C_POS_INF:
            GET_EXCEPTION_RESULT_1(LGAMMA_POS_INF, x, t);
            break;

        case F_C_NEG_INF:
            GET_EXCEPTION_RESULT_1(LGAMMA_NEG_INF, x, t);
            break;

        case F_C_QUIET_NAN:
        case F_C_SIG_NAN:
            t = x;
            break;

        case F_C_NEG_ZERO:
            SIGNGAM = -1;
            /* fall through */

        case F_C_POS_ZERO:
            GET_EXCEPTION_RESULT_1(LGAMMA_OF_ZERO, x, t);
            break;

        default:            /* +-denorm */
            if (F_C_IS_NEG_CLASS(i)) {
                SIGNGAM = -1;
                F_ABS(x, x);
            }
            t = -F_LN_NAME(x);

        }
    return t;

#endif

non_pos_int:
    GET_EXCEPTION_RESULT_1(LGAMMA_NON_POS_INT, -x, t);
    return t;

#endif
    }

