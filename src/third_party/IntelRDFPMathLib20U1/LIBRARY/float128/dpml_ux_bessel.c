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

#define DYNAMIC
#undef  DYNAMIC

#define	BASE_NAME	bessel
#include "dpml_ux.h"

#if !defined(MAKE_INCLUDE)
#   include STR(BUILD_FILE_NAME)
#endif

#if !defined(DYNAMIC)
#   define DYNAMIC	0
#else
#   undef  DYNAMIC
#   define DYNAMIC	1
#endif

/* 
** This following is a discussion of the implementation of the unpacked x-float
** bessel functions.  The algorithmic aspects of these routines are virtually
** identical to the existing DPML x-float bessel function routines.
** Consequently, the primary focus of the comments in this file is the
** implementation details for the unpacked x-float case.  For details about the
** algorithms used, the reader should refer to the file dpml_bessel.c.
** 
** 
** 1.0 BACKGROUND AND BASICS
** -------------------------
** 
** This note discusses the bessel functions of the first and second kind, j(n,x)
** and y(n,x) respectively.  In this document, we use the notation C(n,x) to
** refer to j(n,x) and y(n,x) simultaneously.  Further, we distinguish between
** the first and second arguments to C(n,x) by the names 'order' and 'argument'
** respectively.
** 
** Broadly speaking, the existing DPML algorithm for C(n,x) is divided into
** three ranges: 
** 
** 	(1) |n| >= 2
** 	(2) asymptotic approximations to C(0,x) and C(1,x)
** 	(3) polynomial approximations to C(0,x) and C(1,x)
** 
** 
** 2.0 IMPLEMENTATION DISCUSSION
** -----------------------------
** 
** In this section we present an overview of the organization of the unpacked
** x-float bessel function routines.  The following sections discuss the
** implementation details on each of the ranges specified in section 1.0.
** 
** Each of the six user level bessel functions call a common interface routine,
** C_BESSEL.  C_BESSEL unpacks the argument and determines s = 1 or
** -1 so that C(n,x) = s*C(|n|,|x|).  C(|n|,|x|) is computed in unpacked form
** by the routine UX_BESSEL, which may call out to UX_ASYMPTOTIC_BESSEL or
** UX_LARGE_ORDER_BESSEL. 
**
** C_BESSEL invokes UX_BESSEL to actually determine which of the three
** evaluation ranges to use and calls UX_ASYMPTOTIC_BESSEL and
** UX_LARGE_ORDER_BESSEL for ranges  (1) and (2), or processes range (3)
** directly.  The reason this is not done directly by C_BESSEL is so that
** UX_BESSEL can be called recursively without having to unpack the arguments
** again.
** 
** 
** 2.1 ASYMPTOTIC RANGE FOR ORDER LESS THAN 2
** ------------------------------------------
** 
** The simplest evaluation region is when the order less than 2 and the
** arguments are large.  (See section 2.3.1 for a more precise definition of
** "large arguments".)  On this range C(n,x) is be approximated as:
** 
** 	j(n,x) = w(x)*{ P(n,z)*cos(X(n,x)) - Q(n,z)*sin(X(n,x)) } (1)
** 	y(n,x) = w(x)*{ P(n,z)*sin(X(n,x)) + Q(n,z)*cos(X(n,x)) }
** 
** where z = 1/x, w(x) = sqrt[2/(x*pi)],  X(n,x) = x - (2n+1)*(pi/4) and
** P(n,z) and Q(n,z) are rational expressions in z.
** 
** In order to make the processing of C(n,x) more uniform, we note that
** cos(x + pi/2) = -sin(x) and sin(x + pi/2) = cos(x), so that we can replace
** the cos and sin terms in (1) with sin(pi/2+X(n,x)) and cos(pi/2+X(n,x))
** respectively.  But pi/2 + X(n,x) = x - (pi/4)*(2n-1) = X(n-1,x) so that we
** have
** 
** 	j(n,x) = w(x)*{ P(n,z)*sin(X(n-1,x)) + Q(n,z)*cos(X(n-1,x)) }
** 	y(n,x) = w(x)*{ P(n,z)*sin(X(n,x))   + Q(n,z)*cos(X(n,x)) }
** 
** Since we are only dealing the cases n = 0 and 1, in order to ease the
** implementation, we pad the coefficients of P(0,z), Q(0,z), P(1,z) and Q(1,z)
** with zeros to insure they all have the same degree.  Further, we assume
** that the coefficients are laid out in memory in the order presented.
*/ 

#if !defined(UX_ASYMPTOTIC_BESSEL)
#   define UX_ASYMPTOTIC_BESSEL		__INTERNAL_NAME(ux_asymptotic_bessel__)
#endif

static void
UX_ASYMPTOTIC_BESSEL( UX_FLOAT * unpacked_argument, WORD order, WORD kind,
  UX_FLOAT * unpacked_result)
    {
    UX_FLOAT tmp[5];
    WORD p_degree, q_degree;
    FIXED_128 * p_coefs, * q_coefs;

    /* Get reciprocal */

    DIVIDE( NOT_USED, unpacked_argument, FULL_PRECISION, &tmp[4]);

    /*
    ** Compute P(x, n) and Q(x,n) as rational functions in z = 2^t/x, where
    ** t = MIN_ASYMPTOTIC_EXPONENT - 1.  Since we eventually need to multiply
    ** the final result by w = sqrt[2/(x*pi)] = sqrt(z)/sqrt[ pi*2^(t-1) ],
    ** we actually compute tmp[0,1] = P and Q respectively, with P = c*P(x,n)
    ** and Q = c*Q(x,n), where c = 1/sqrt[ pi*2^(t-1) ]
    */

    if (0 == order)
        {
        p_degree = P0_DEGREE;
        q_degree = Q0_DEGREE;
        p_coefs  = P0_COEFFICIENTS;
        q_coefs  = Q0_COEFFICIENTS;
        }
    else
        {
        p_degree = P1_DEGREE;
        q_degree = Q1_DEGREE;
        p_coefs  = P1_COEFFICIENTS;
        q_coefs  = Q1_COEFFICIENTS;
        }

    EVALUATE_RATIONAL(
       &tmp[4],
       p_coefs,
       p_degree,
       NUMERATOR_FLAGS( SQUARE_TERM )
          | DENOMINATOR_FLAGS( SQUARE_TERM ) |
          P_SCALE(4),
       &tmp[0]);

    /*
    ** Because the value of q0 is negative and the value of q1 is positive,
    ** and EVALUATE_RATIONAL only deal with positive coefficients, tmp[1]
    ** contains (-1)^(order+1)*Q rather than Q
    */

    EVALUATE_RATIONAL(
       &tmp[4],		/* Already been scaled by previous call */
       q_coefs,
       q_degree,
       NUMERATOR_FLAGS( SQUARE_TERM | POST_MULTIPLY )
          | DENOMINATOR_FLAGS( SQUARE_TERM ),
       &tmp[1]);

    /* get tmp[2,3] = sin and cos values respectively */

    UX_SINCOS(
        unpacked_argument,
        1 - kind - 2*order,
	SINCOS_FUNC,
        &tmp[2]);

    /* Now multiply the results */

    MULTIPLY(&tmp[0], &tmp[2], &tmp[0]);	/* tmp[0] = P*sin	*/
    MULTIPLY(&tmp[1], &tmp[3], &tmp[1]);	/* tmp[1] = +/-Q*cos	*/
    ADDSUB(&tmp[0], &tmp[1], order ? ADD : SUB, &tmp[0]);
    
    /* Get sqrt and do final multiply */

    UX_SQRT(&tmp[4], &tmp[1]);
    MULTIPLY(&tmp[0], &tmp[1], unpacked_result);
    }

/* 
** 2.2 LARGE ORDER RANGE
** ---------------------
** 
** The implementation of bessel functions of large order are based on the
** recurrence relations
** 
** 		           2n
** 		C(n+1,x) = --- C(n,x) - C(n-1,x)		(2)
** 			    x
** 
** For y(n,x), (2) is used by first computing y(0,x) and y(1,x) and iterating
** until y(n,x) is obtained.  This approach is referred as a "forward"
** recurrence.  The same approach can by used for j(n,x), if x > n.
** 
** When x <= n, the forward recurrence for j(n,x) is unstable, and a backward
** recurrence must be used.  This technique is a little more subtle.  It is
** based on the identity
** 
** 	1 = j(0,x) + 2*{ j(2,x) + j(4,x) + j(6,x) ... }		(3)
** 
** and the fact that j(n+1,x)/j(n,x) --> 0 as n gets large.
**
** The process begins by chosing an integer, N, and two real values, t(N+1,x)
** and t(N,x) and define t(k,x) for 0 <= k < N by
**
**		t(k-1,x) = (2k/x)*t(k,x) - t(k+1,x)
**
** Now, we can find two real numbers, A and B such that
**
**		t(N+1,x) = A*j(N+1,x) + B*y(N+1,x)		(4)
**		t(N,x)   = A*j(N,x)   + B*y(N,x)
**
** It follows from (2) and the definition of t(k,x), that
**
**		t(k,x) = A*j(k,x) + B*y(k,x)
**
** Ultimately, we want to find j(n,x) for a given n and x.  If we could 
** arrange it so that the term B*y(n,x) was insignificant to A*j(n,x), then
** to machine precision t(n,x) = A*j(n,x).  Further, if we could estimate
** A, then we could compute j(n,x) to machine precision as t(n,x)/A.
** Toward this end, we solve (4) for A and B:
**
**	A =   [t(N+1,x)*y(N,x) - t(N,x)*y(N+1,x)]/[2/(pi*x)]
**	B = - [t(N+1,x)*j(N,x) - t(N,x)*j(N+1,x)]/[2/(pi*x)]
**
**	NOTE: The above expressions for A and B make use of the identity
**	j(n+1,x)*y(n,x) - j(n,x)*y(n+1,x) = 2/(pi*z)
**
** Now consider the ratio:
**
**	    | B*y(n,x) |   | [t(N+1,x)*j(N,x) - t(N,x)*j(N+1,x)]*y(n,x) |
**	r = | -------- | = | ------------------------------------------ |
**	    | A*j(n,x) |   | [t(N+1,x)*y(N,x) - t(N,x)*y(N+1,x)]*j(n,x) |
**
** Now the choice of t(N+1,x) and t(N,x) was arbitrary, so to simplify things,
** we take t(N+1,x) = 0 and t(N,x) = 1.  Then
**
**			A = - (pi*x/2)*y(N+1,x)]
**			B =   (pi*x/2)*j(N+1,x)]
**
**			    | j(N+1,x)]*y(n,x) |
**			r = | ---------------- |
**			    | y(N+1,x)]*j(n,x) |
**
** Using asymptotic approximations for large orders (See Abramowitz and Stegun,
** page 365, eq 9.3.1), we get
**
**			     [ex/(2N+2)]^(2N+2)
**			r =  ------------------		(5)
**			        [ex/(2n)]^2n
**
** So, if given x and n, we can find N, such that (5) is less that 1/2^(p+1)
** then B*y(n,x) will be insignificant to A*j(n,x).  What we need to do
** now is estimate A.  This is done via the identity in (3).  Specifically,
** letting N' = 2*floor(N/2), we "replace" the j(k,x)'s in (3) with the
** t(k,x)'s to get
**
** 	S = t(0,x) + 2*[ t(2,x) + t(4,x) + t(6,x) ... + t( 2N',x) ]
** 	  = A*{ j(0,x) + 2*[ j(2,x) + j(4,x) + j(6,x) ... + j( 2N',x) ] } + 
** 	       B*{ y(0,x) + 2*[ y(2,x) + y(4,x) + y(6,x) ... + y( 2N',x) ] }
** 	  = A*J + B*Y
**
** The assumption here is that if N is chosen large enough, then J will equal
** 1 to machine precision and that B*Y will be insignificant to A*J.  If this
** true, then j(n,x) = t(n,x)/S.  So the key here is to choose N large enough
** to the process work.
**
** Brent uses the solution to (5) in his MP package.  However, this choice
** of N does not guarantee that that B*Y is small enough.  The DPML bessel
** functions assume that if j(N,x) is insignificant compared to 1, then N is
** big enough.  So the DPML routines use that asymptotic approximation for
** j(n,x) and "solve"
**
**			(ex/(2N))^N
**			------------ < 1/2^(p+1)
**			sqrt(2*pi*N)
**
** for N.  This choice of N "works" in the sense that the answer is accurate,
** however, N chosen this way is much larger than is necessary, especially for
** small n.
**
** There is a passing comment in Abramowitz and Stegun (pg. 386) that
**
**	"The number of correct significant figures in the final
**	 values [ i.e. j(n,x) ] is the same as the number of digits
**	 in the respective trial values. [ i.e. t(n,x) ]"
** 
** Using the asymptotic estimates for j(n,x) and y(n,x) and noting that
** t(n,x) ~ A*j(n,x), we can try to find N such that
**
**	(x/2)* [ 2N/(ex) ]^N * [ ex/(2n) ]^n = 2^t * sqrt(N*n)	(6)
**
** with t = p + 1. This seems to give accurate results without making N unduly
** large.
**
** Solving (6) for N is difficult and requires an iterative numerical approach.
** 
** 
** 2.2.1 ERROR CHECKING
** --------------------
** 
** For large orders and small arguments, y(n,x) can overflow and j(n,x) can
** underflow.  Using the relationships:
** 
** 	| y(n,x) | > (n-1)!*(2/x)^n	| j(n,x) | < (x/2)^n/n!
** 
** We can screen out guaranteed overflow and underflow conditions via the
** comparisons:
** 
** 	(n-1)!*(2/x)^n >= 2^EMAX	(x/2)^n/n! <= 2^EMIN
** 
** where EMAX = F_MAX_BIN_EXP + 1 and EMIN = F_MIN_BIN_EXP - F_PRECISION + 1.
** The above comparisons are equivalent to:
** 
** 	log2[(n-1)!] + n*[1 - log2(x)] >= EMAX
** 	    n*[log2(x) - 1] - log2(n!) <= EMIN
** 
** Noting that x = 2^k*f, f in [1/2, 1) and that log2(n!) = log2[(n-1)!] +
** log2(n), the two comparisons are equivalent to:
** 
** 	          log2[(n-1)!] + n*[1 - k - log2(f)] >= EMAX	(7)
** 	n*[k + log2(f) - 1] - log2[(n-1)!] - log2(n) <= EMIN	(8)
** 
** Now we need to estimate the value of log2[(n-1)!].  Since doing this
** precisely is equivalent to evaluating the lgamma function, we will use an
** upper and lower bound for log2[(n-1)!] in (7) and (8) to get comparisons
** that give less precise error range boundaries, but are easier to compute.
** 
** From Hart, we show that if n = 2^E*g, where g is in the interval [1/2, 1),
** then,
** 
** 	(n-.5)*bexp(n) - n*(1/ln2 + 1) + (1 + .5*log2(pi)) <= log2((n-1)!)
** 	log2((n-1)!) <= (n-.5)*E - n/ln2 + .5 + .5*log2(pi)
** 
** Noting that -1 <= log2(f) < 0, and using the bounds for log2[(n-1)!], we
** can transform (7) and (8) to:
** 
** 	    (n-.5)*E - n*(1/ln2+1) + 1 +.5*log2(pi) + n*(1-k) - EMAX >= 0  (9)
** 	n*(k-1) - (n-.5)*E + n/ln2 - .5 - .5*log2(pi) - (E-1) - EMIN <= 0  (10)
** 
** If we denote the left hand sides of (9) and (10) as A and B respectively,
** the we can define c = (A + B)/2 and d = (A - B)/2 and the above comparisons
** are equivalent to
** 
** 			c + d >= 0
** 			    c <= 0
** 
** where 
** 
** 	c = .5*(3/2 - EMAX - EMIN) - .5*(n + E)
** 	d = n*[ E - k + (1/2 - 1/ln2) ] + [ 1/2 + log2(pi) - EMAX + EMIN ]/2
** 
** 
** 2.2.2 COMPUTING 2*N
** -------------------
** 
** For both the forward and backward recurrence, the computation of 2*k for
** k increasing or decreasing is required.  In the process of creating the
** unpacked representation for the initial value of 2*k, we can create an
** integer value that is an unnormalized representation of 2.  This integer
** can be added/subtracted to the high word of 2*k to get the unpacked
** representation of the next value of 2*k.  If the addition/subtraction
** results in a carry out or borrow from the MSB of the fraction, then the
** exponent of the result and the unnormalized representation of two needs to
** be adjusted.
*/ 

#define	J_BESSEL	0
#define	Y_BESSEL	2

#if !defined UX_LARGE_ORDER_BESSEL
#   define UX_LARGE_ORDER_BESSEL	__INTERNAL_NAME(ux_large_order_bessel__)
#endif

#if !defined(UX_BESSEL)
#   define UX_BESSEL		__INTERNAL_NAME(ux_bessel__)
#endif

static void UX_BESSEL( UX_FLOAT *, WORD, WORD, UX_FLOAT *);

#if (OP_SYSTEM == vms)
#   define S_SUFFIX	PASTE_2(_, S_CHAR)
#else
#   define S_SUFFIX	f
#endif

#ifndef S_LOG2_NAME
#define S_LOG2_NAME	PASTE_2(__SYSTEM_NAME(LOG2_BASE_NAME), S_SUFFIX)
#endif

extern S_TYPE S_LOG2_NAME( S_TYPE );

	
static void
UX_LARGE_ORDER_BESSEL(
  UX_FLOAT * unpacked_argument,
  WORD       order,
  WORD       kind,
  UX_FLOAT * unpacked_result)
    {
    double c, d;
    float forder, fN, fx, log2_n, delta, ftmp, A, B;
    WORD n_exponent, exp_diff, i;
    UX_EXPONENT_TYPE exponent;
    UX_FRACTION_DIGIT_TYPE f_hi, incr, N;
    UX_FLOAT tmp[4], *C0, *C1, *C2, twice_n, sum, *save;

    /*
    ** For both the forward and backward recurrence we need 1/x
    ** and pointers into the tmp[] array to hold the results of
    ** recursion.
    */

    DIVIDE( NOT_USED, unpacked_argument, FULL_PRECISION, &tmp[3]);
    C0 = &tmp[0];
    C1 = &tmp[1];
    C2 = &tmp[2];

    /*
    ** Determine if a forward or backward recurrence is needed.
    ** In the process, do underflow and overflow screening.
    */

    n_exponent = BITS_PER_UX_FRACTION_DIGIT_TYPE - U_WORD_TO_UX(order, &tmp[0]);
    exponent = G_UX_EXPONENT(unpacked_argument);

    c = .5*( 111.5 - (double) (n_exponent + order));
    exp_diff = n_exponent - exponent;
    d = ((double) order)*( (double) exp_diff + .942)
          -16437.924251;

    /* 
    ** if evaluating Y_BESSEL functions or if x >= n, use a
    ** forward recurrence.
    */

    if (kind == Y_BESSEL)
        { /* Check for certain overflow */
        if (c + d > 0)
            {
            exponent = UX_OVERFLOW_EXPONENT;
            goto return_exception;
            } 
        }
    else
        { /* J_BESSEL, check for underflow */
        if (c < 0 )
            {
            exponent = UX_UNDERFLOW_EXPONENT;
            goto return_exception;
            } 

        /*
        ** if x < n use backward recurrence.  Use N as a temporary location
        ** to hold the "aligned" fraction part of x
        */

        f_hi = G_UX_MSD(unpacked_argument);
        N = f_hi >> (BITS_PER_UX_FRACTION_DIGIT_TYPE - n_exponent);
        if ((0 < exp_diff) || ((0 == exp_diff) && (N < order)))
            goto backward_recurrence;
        }

//forward_recurrence:

    /*
    ** We want to compute C(k+1,x) = (2k/x)*C(k,x) - C(k-1,x)
    ** for k = 1,2, ... n-1.  The initialization phase requires
    ** the computation of 2, C(1,x) and C(0,x)
    */

    UX_BESSEL(unpacked_argument, 0, kind, C0);
    UX_BESSEL(unpacked_argument, 1, kind, C1);

    UX_SET_SIGN_EXP_MSD(&twice_n, 0, 2, UX_MSB);
    incr = UX_MSB;

    order--;

    /* Now do the recursions */

    while(1)
        {
        MULTIPLY(&tmp[3], &twice_n, C2);
        MULTIPLY(C1, C2, C2);
        ADDSUB(C2, C0, SUB, C2);

        if ((--order) <= 0)
            break;

        /* Adjust pointers, check for overflow or underflow */

        save = C0;
        C0 = C1;
        C1 = C2;
        C2 = save;

	f_hi = G_UX_MSD(&twice_n) + incr;
        if (f_hi < incr)
            { /* carry out occurred on the addition */
            UX_INCR_EXPONENT(&twice_n, 1);
            f_hi = (f_hi >> 1) + UX_MSB;
            incr >>= 1;
            }
        P_UX_MSD(&twice_n, f_hi);
        }
    
    /* Copy result of iteration to unpacked result */
    UX_COPY(C2, unpacked_result);
    return;


backward_recurrence:

    /*
    ** In order to solve (11) iteratively to find the starting point N, we
    ** set up the recursion
    **
    **		    t*ln2 - log(x/2) - n*log(.5*e*x/n) + .5*log(N*n)
    **		N = ------------------------------------------------
    **		                 log(2N/(ex))
    **
    **		    B + .5*log2(N)
    **		  = --------------
    **		     log2(N) - A
    **
    **	where
    **
    **		A = log2(.5*e*x) and
    **		B = t - .5*A - (n + .5)*[ A - log2(n)] + 1/ln2
    **
    ** The initial choice of N is important for the iteration.  It can be
    ** shown analytically, that n+1 <= N < n + 1 + t.  Experimentally, we
    ** have found that taking N = n + 1 + (x/n)*(C*log2(n) + D) yields
    ** very good results.
    **
    ** Start by computing x/n to get the initial value for N.
    */

#   define MSD_TO_FLOAT(p) \
		(float)(( UX_SIGNED_FRACTION_DIGIT_TYPE) (G_UX_MSD(p) >> 1))
#   define SCALE_DOWN	((float) 1./ S_POW_2(BITS_PER_UX_FRACTION_DIGIT_TYPE - 1))

    fx = MSD_TO_FLOAT(unpacked_argument);
    forder = MSD_TO_FLOAT(&tmp[0]);
    delta = fx/forder;

    exp_diff = (BITS_PER_UX_FRACTION_DIGIT_TYPE - 1) - exp_diff;
    exp_diff = (exp_diff < 0) ? 0 : exp_diff;
    ftmp = (float) (((UX_FRACTION_DIGIT_TYPE) 1) << exp_diff);
    ftmp = delta*ftmp*SCALE_DOWN;

    /* ftmp = x/n at this point.  Get initial value of N */

#define SLOPE		((float) 8.9740928556490771841809829330372159128901 )
#define INTERCEPT	((float) 20.4831861112546093392565170669627840871099 )

    forder = (float) order;
    log2_n = S_LOG2_NAME( forder );
    delta = SLOPE*log2_n + INTERCEPT;
    fN = ftmp*( SLOPE*log2_n + INTERCEPT );
    fN = (fN > delta) ? delta : fN;

    fN = (forder + ((float) 1)) + delta;

    /*
    ** Now compute the constants A and B, so that we can start the iteration
    */

#   define R_LOG2	((float) 1.4426950408889634073599246810018921374266)

    A = S_LOG2_NAME(fx) + (float) (exponent - BITS_PER_UX_FRACTION_DIGIT_TYPE)
          + R_LOG2;
    B = ((((float) F_PRECISION + 1) + R_LOG2) - .5*A)
          - (forder + .5)*(A - log2_n);

    /* Iterate three times to get a good approximation to N */

    for (i = 3; i > 0; i--)
        {
        ftmp = S_LOG2_NAME( fN );
        ftmp = (B + 5.*ftmp)/(ftmp - A);
        fN = .5*(fN + ftmp);
        }

    /*
    ** Convert to integer and do one last check. 
    */

    N = (UX_FRACTION_DIGIT_TYPE) (fN + 9.99999940395355224609375e-1);
    N =  (N < (order + 1) ) ? (order + 1) : N;

    /*
    ** We want to compute C(k-1,x) = (2k/x)*C(k,x) - C(k+1,x)
    ** for k = N,N-1, ... 0.  The initialization phase requires
    ** the computation of 2*N and setting C(N,x) = 1 and
    ** C(N+1, x) = 0 and the running sum to C(N,x) or C(N+1,x)
    ** depending on the parity of n
    */

    UX_SET_SIGN_EXP_MSD(&tmp[0], 0, UX_ZERO_EXPONENT,      0);
    UX_SET_SIGN_EXP_MSD(&tmp[1], 0,                1, UX_MSB);

    P_UX_SIGN(&sum, 0);
    if (N & 1)
        UX_SET_SIGN_EXP_MSD(&sum, 0, UX_ZERO_EXPONENT, 0);
    else
        UX_SET_SIGN_EXP_MSD(&sum, 0, 1, UX_MSB);


    (void) U_WORD_TO_UX( 2*N, &twice_n);
    incr = UX_MSB >> (G_UX_EXPONENT(&twice_n) - 2);
        
    /* Now do the recursions */

    while(1)
        {
        MULTIPLY(&tmp[3], &twice_n, C2);
        MULTIPLY(C1, C2, C2);

        NORMALIZE(C2);
        NORMALIZE(C0);
        ADDSUB(C2, C0, SUB, C2);

        if (--N == 0)
            break;

        /* if N == n, C2 = K*J(n,x).  Save it for later */

        if (N == order)
            UX_COPY(C2, unpacked_result);

        /* Add to sum if N is even */

        if ( 0 == (N & 1) )
            ADDSUB(&sum, C2, ADD, &sum);

        /* Adjust pointers */

        save = C0;
        C0 = C1;
        C1 = C2;
        C2 = save;

        /* decrement twice_n by  2 */

	f_hi = G_UX_MSD(&twice_n) - incr;
        if (f_hi < UX_MSB)
            { /* borrow from MSB on the subtraction */
            UX_DECR_EXPONENT(&twice_n, 1);
            f_hi += f_hi;
            incr += incr;
            }
        P_UX_MSD(&twice_n, f_hi);
        }
   
    /*
    ** at this point sum = K*sum{ k=1,2,... | J(2k,x) }, and C2 points
    ** to K*J(0,x).  Compute K from the relation
    **
    **		1 = J(0,x) + 2*{ J(2,x) + J(4,x) + J(6,x) ... }
    */

    UX_INCR_EXPONENT(&sum, 1);
    ADDSUB(C2, &sum, ADD, &sum);
    DIVIDE( unpacked_result, &sum, FULL_PRECISION, unpacked_result);
    return;

return_exception:
    UX_SET_SIGN_EXP_MSD(
        unpacked_result,
        UX_OVERFLOW_EXPONENT == exponent ? UX_SIGN_BIT : 0,
        exponent,
        UX_MSB);
    }
	    
/* 
** 2.3 POLYNOMIAL RANGE FOR ORDER LESS THAN 2
** ------------------------------------------
** 
** C(n,x) oscillates much like an attenuated sin or cos curve, and consequently
** has infinite number of zeros.  The polynomial range is divided into
** intervals, each of which contains a zero of the function.  We then expand
** C(n,x) in a "polynomial" around that zero.
** 
** The primary issue in the polynomial range is determining the appropriate
** zero and corresponding set of polynomial coefficients for a given argument.
** Generally speaking, if e[i] and e[i+1] are i-th and i+1st extrema locations
** of C(n,x), and z[i] is the zero located between e[i] and e[i+1], then we
** approximate C(n,x) on [ e[i], e[i+1] ) in a polynomial around z[i].
** 
** 	NOTE: The above 'algorithm' requires some special case code when
** 	the function has a zero at x = 0 and for the first interval of
**	y0 and y1.  See the comments in the MPHOC code below for details.
** 
** 
** 2.3.1 CONSTRUCTING THE ARRAYS
** -----------------------------
** 
** The first step in constructing the arrays is to establish the number of
** entries in the arrays.  As a side effect of this computation, we determine
** the range for the asymptotic evaluations.  It should be noted here, that
** while the asymptotic expansion is useful for x as small as 8, if x is less
** that (approximately) 22, the terms of the asymptotic approximation do not
** decrease in magnitude, which is a problem for the unpacked rational
** evaluation routine.  Consequently, we need to force the lower limit of the
** asymptotic range to be at least 22.
** 
** For each of the four bessel functions, f = j0, j1, y0, and y1, denote intial
** local extrema by e(f,0) and recursively define e(f, i+1) to be the first
** extrema value of f after e(f,i).  Further, we define z(f,i) to be the zero
** of f between e(f,i) and e(f,i+1).  Lastly, define n(f) to be the smallest
** their local extrema by e(f,1), e(f,2) ... and define n(f) to be the
** integer such that e(f, n(f)) > 22.
** 
** The precise locations of the extrema points are not critical to the
** algorithm, so we need not store them in full precision.  In fact, all of
** the extrema points are less than 32, so we can store them in true fixed
** point format consisting of one integer word with the binary point after
** the 5-th most significant bit.
** 
** The values of the zeros on the other hand must be stored to twice the normal
** precision.  Toward this end, we represent the zeros using a 256 bit fraction.
** Since the input argument has 113 significant bits, if we compute the reduced
** argument to 128 bits, the zeros need only be accurate to 241 bits, which
** leaves 15 "extra" bits in the 256 bit fraction.  Since the signs of the
** zeros are all positive, and the exponents are small, we can conserve overall
** storage by encoding the exponent of the zeros in the low order 5 bits of the
** fraction field and construct the unpacked form of the zero at run-time.
** 
** The interval data is stored as:
*/

typedef struct {
	UX_FRACTION_DIGIT_TYPE extrema;
        WORD                   eval_data;
#       if (BITS_PER_WORD < 64)
            WORD                   eval_data_hi;
#       endif
	UX_FRACTION_DIGIT_TYPE zero[2*NUM_UX_FRACTION_DIGITS];
	FIXED_128              coefficients[1];
	} INTERVAL_DATA;

#define FIXED_BITS_PER_INTERVAL_DATA \
	    ((2*NUM_UX_FRACTION_DIGITS + 1)*BITS_PER_UX_FRACTION_DIGIT_TYPE \
	      + __NUM_WORDS * BITS_PER_WORD)

#define OFFSET_POS	32
#define OFFSET_WIDTH	10
#define OFFSET_MASK	MAKE_MASK(OFFSET_WIDTH, 0)

#if (BITS_PER_WORD < 64)
#   define __NUM_WORDS	2
#   define G_OFFSET(ip)	((ip)->eval_data_hi & OFFSET_MASK)
#else
#   define __NUM_WORDS	1
#   define G_OFFSET(ip)	((((ip)->eval_data) >> OFFSET_POS) & OFFSET_MASK)
#endif

/*
** where
**
**	extrema		is the fixed point value of the upper limit
**			of the evaluation interval.
**	zero		is the zero associated with this particular
**			interval
**	eval_data	is miscellaneous information about the evaluation
**			on this interval, including the degree of the
**			polynomial
**	eval_data_hi	is a hack to deal with storing all of the evaluation
**			data required in 32 bit chunks.
**
** Since the number of intervals and coefficients per interval vary, we
** create an auxiliary data structure that can be indexed by 'kind' and 'order'
** to determine the minimum asymptotic value and the start of the interval
** data:
*/


typedef struct {
	UX_FRACTION_DIGIT_TYPE min_asymptotic_value;
	WORD                   interval_data_offset;
	WORD                   asymptotic_coef_offset;
	} TABLE_DATA_MAP;

/*
** The following definitions are used to pack and extract data from the
** eval_data field of the INTERVAL_DATA structure.  In order to insure that
** all of the information fits in 32 bit chunks, the format of the eval_data
** field is different depending on whether we are doing a packed or unpacked
** evaluation.
**
** For the unpacked, case, we want to have the eval_data field look like a
** super set of the flags passed to the unpacked rational evaluation routine.
** In this case the eval_data field looks like:
**
**	         2 2 2 2 2     1 1 11 1
**	         4 3 2 1 0     4 3 21 0 8 7  4 3  0
**	+-------+-+-+-+-+-------+-+--+---+----+----+
**	|       |P|X|M|N|   D   |n| O|   |    |    |
**	+-------+-+-+-+-+-------+-+--+---+----+----+
**
**	Bits Name		Meaning
**	--------- -----------------------------------
**	    P	  Packed or unpacked evaluation: 1 = packed
**	    X	  Expand the polynomial around the zero of the interval
**	    M	  Post multiply the result of the polynomial evaluation
**		  by the argument.  I.e. compute z*P(z)
**	    N	  Indicates a Neumann evaluation
**	    D	  The degree of the polynomial
**	    n	  Negate the final result
**	    O	  Indicates how (if needed) to combine the odd and even
**		  terms of the polynomial.  Choices are add/sub/none
**
** Bits 0 through 10 are the standard rational evaluation flags defined in
** dpml_ux.h.
*/

#define BESSEL_PACKED_POLY		SET_BIT(24)
#define BESSEL_USE_ZERO			SET_BIT(23)
#define BESSEL_POST_MULTIPLY		SET_BIT(22)
#define BESSEL_NEUMANN_POLY		SET_BIT(21)
#define BESSEL_NEGATE_POLY		SET_BIT(13)
#define BESSEL_NO_DIVIDE		SET_BIT(2*NUM_DEN_FIELD_WIDTH)
#define BESSEL_COMMON_FLAGS_MASK	(SET_BIT(25) - SET_BIT(21))

#define BESSEL_EVEN_ODD_OP_POS		11
#define BESSEL_EVEN_ODD_OP_WIDTH	2
#define BESSEL_DEGREE_POS		14
#define BESSEL_DEGREE_WIDTH		7
#undef  DEGREE

/*
** For the packed case, eval_data looks like;
**
**	         2 22  2 2     1 1     
**	         4 32  1 0     4 3     7 6     0
**	+-------+-+-+-+-+-------+-------+-------+
**	|       |P|X|M|N|   D   |   W   |   B   |
**	+-------+-+-+-+-+-------+-------+-------+
**
** Where P, X, M, N nd D ar as above and B and W are used to endcode the
** relative expoenent bias and width for the packed coefficients
*/

#define BESSEL_EXP_BIAS_POS		 0
#define BESSEL_EXP_BIAS_WIDTH		 7
#define BESSEL_EXP_WIDTH_POS		 7
#define BESSEL_EXP_WIDTH_WIDTH		 7

#define EXTR_BITS(name,val)	(((val) >> PASTE_3(BESSEL_,name,_POS)) & \
				  MAKE_MASK(PASTE_3(BESSEL_,name,_WIDTH),0))

/*
** The next 4 definitions are used to extract the exponent information from
** the zero values
*/

#define MIN_ASYMPTOTIC_EXPONENT		5
#define	LAST			(2*NUM_UX_FRACTION_DIGITS-1)
#define ZERO_EXPONENT_BITS	3
#define G_ZERO_EXPONENT(p)	((((INTERVAL_DATA *)(p))->zero[LAST]) & \
				    MAKE_MASK(ZERO_EXPONENT_BITS, 0))

static void
UX_BESSEL( UX_FLOAT * unpacked_argument, WORD order, WORD kind,
  UX_FLOAT * unpacked_result)
    {
    INTERVAL_DATA * interval_data;
    TABLE_DATA_MAP  * table_data_map;
    WORD eval_data, op;
    UX_FRACTION_DIGIT_TYPE f_hi;
    UX_EXPONENT_TYPE exponent;
    UX_FLOAT tmp[3], *multiplier, *poly_argument;

    if (2 <= order)
        {
        UX_LARGE_ORDER_BESSEL(unpacked_argument, order, kind, unpacked_result);
        return;
        }

    f_hi = G_UX_MSD(unpacked_argument);
    exponent = G_UX_EXPONENT(unpacked_argument);

    /*
    ** Compare the input argument with the minimum asymptotic value for this
    ** bessel function
    */

    table_data_map = BESSEL_TABLE_DATA_MAP + (kind + order);

    if ((exponent > MIN_ASYMPTOTIC_EXPONENT) ||
     ((exponent == MIN_ASYMPTOTIC_EXPONENT) &&
      (f_hi > table_data_map->min_asymptotic_value))) 
        {
        UX_ASYMPTOTIC_BESSEL(unpacked_argument, order, kind, unpacked_result);
        return;
        }

    /*
    ** Get the extrema, zeros and coefficients for this particular
    ** function.
    */

    interval_data = (INTERVAL_DATA *) ((char *) TABLE_NAME +
        table_data_map->interval_data_offset);

    /*
    ** Now scan through the extrema values to determine the
    ** nearest zero.  For the comparison, convert the high word
    ** and exponent of the argument to fixed point form
    */

    if (exponent >= 0)
        {
        f_hi >>= (5 - exponent);
        while (1)
            {
            if (f_hi <= interval_data->extrema)
                break;
            interval_data = (INTERVAL_DATA *) ((char *) interval_data +
               G_OFFSET(interval_data));
            }
        }

    /*
    ** Having located the appropriate zero, call it a, put it in
    ** unpacked form and carefully compute the reduced argument,
    ** x - a.
    */

    eval_data = interval_data->eval_data;
    if ((eval_data & BESSEL_USE_ZERO) == 0)
        poly_argument = unpacked_argument;
    else
        {
        COPY_TO_UX_FRACTION(interval_data->zero, &tmp[1]);
        P_UX_SIGN(&tmp[1],  0);
        exponent = G_ZERO_EXPONENT(interval_data);
        P_UX_EXPONENT(&tmp[1], exponent);
        ADDSUB(unpacked_argument, &tmp[1], SUB, &tmp[0]);
        COPY_TO_UX_FRACTION(
          &interval_data->zero[NUM_UX_FRACTION_DIGITS], &tmp[1]);
        P_UX_EXPONENT(&tmp[1], exponent - UX_PRECISION);
        ADDSUB(&tmp[0], &tmp[1], SUB, &tmp[0]);
        poly_argument = &tmp[0];
        }
    /*
    ** Evaluate the polynomial.
    */

    if ( eval_data & BESSEL_PACKED_POLY)
        EVALUATE_PACKED_POLY(
            poly_argument,
            EXTR_BITS( DEGREE, eval_data),
            interval_data->coefficients,
            MAKE_MASK( EXTR_BITS( EXP_WIDTH, eval_data), 0), 
            EXTR_BITS( EXP_BIAS, eval_data),
            unpacked_result);
    else
        {
        EVALUATE_RATIONAL(
            poly_argument,
            interval_data->coefficients,
            EXTR_BITS( DEGREE, eval_data),
            eval_data,
            unpacked_result);

#if 0
	/*
        ** The call to EVALUATE_RATIONAL will have scaled poly_argument, so
        ** unscale it for possible use in the POST_MULTIPLY code.
        */
        UX_DECR_EXPONENT(poly_argument, G_SCALE(eval_data));
#endif
        }

    if ( op = EXTR_BITS( EVEN_ODD_OP, eval_data) )
        ADDSUB(unpacked_result, unpacked_result + 1, op - 1, unpacked_result);

    if ( eval_data & BESSEL_POST_MULTIPLY )
        MULTIPLY( poly_argument, unpacked_result, unpacked_result);

    if ( eval_data & BESSEL_NEGATE_POLY )
        UX_TOGGLE_SIGN( unpacked_result, UX_SIGN_BIT);

    /* For y bessel functions, add in jn(x)*ln(x) term */
    if ( eval_data & BESSEL_NEUMANN_POLY )
        {
        /*
        ** For Y_BESSEL:
        **
        **	y0(x) = (2/pi)*j0(x)*ln(x) - y0_hat(x)			(11)
        **	y1(x) = (2/pi)*j1(x)*ln(x) - (1/pi)/x - y1_hat(x)
        **
        ** where y0_hat(x) and y1_hat(x) are polynomials that
        ** have just been evaluated
        **
        ** The previous call to the polynomial evaluation routines may
	** have implicitly scaled the input argument, so we may need to
	** unscale before proceeding
        */

        if (poly_argument == unpacked_argument)
            UX_DECR_EXPONENT(unpacked_argument, G_SCALE(eval_data));

        if (1 == order)
            {
            DIVIDE( UX_TWO_OVER_PI, unpacked_argument, FULL_PRECISION,
              &tmp[1]);
            ADDSUB( unpacked_result, &tmp[1], ADD, unpacked_result);
            }

        UX_LOG(unpacked_argument, UX_TWO_LN2_OVER_PI, &tmp[0]);
        UX_BESSEL(unpacked_argument, order, J_BESSEL, &tmp[1]);
        MULTIPLY(&tmp[1], &tmp[0], &tmp[0]);

        ADDSUB(&tmp[0], unpacked_result, SUB, unpacked_result);
        }

    return;
    }

    
/*
** All of the bessel functions call a common routine C_BESSEL, to unpacked
** their argument and account for negative orders and arguments.  Some of the
** bessel functions can overflow or underflow.  In order to make the selection
** of the error codes more uniform, we use an array of error codes for the
** bessel functions.  Each user level bessel function will pass C_BESSEL an
** integer, error_map, that consists of three fields corresponding to underflow,
** positive overflow and negative overflow.  These fields will be indices into
** the bessel_error_code table.
*/

#if !defined (BESSEL_ERROR_CODE_TABLE)
#   define BESSEL_ERROR_CODE_TABLE  __TABLE_NAME(bessel_error_codes)
#endif

static WORD const
BESSEL_ERROR_CODE_TABLE[] = {
	NULL,
	BES_J1_UNDERFLOW,
	BES_J1_NEG_UNDERFLOW,
	BES_JN_UNDERFLOW,
	BES_JN_NEG_UNDERFLOW,
	BES_Y1_OVERFLOW,
	BES_YN_POS_OVERFLOW,
	BES_YN_NEG_OVERFLOW,
	};

#define NO_ERROR		0
#define J1_UNDERFLOW		1
#define J1_NEG_UNDERFLOW	2
#define JN_UNDERFLOW		3
#define JN_NEG_UNDERFLOW	4
#define Y1_OVERFLOW		5
#define YN_POS_OVERFLOW		6
#define YN_NEG_OVERFLOW		7

#define _FIELD_WITDTH		8
#define	P_UNDERFLOW_POS		0
#define	N_UNDERFLOW_POS		(P_UNDERFLOW_POS + _FIELD_WITDTH)
#define	P_OVERFLOW_POS		(N_UNDERFLOW_POS + _FIELD_WITDTH)
#define	N_OVERFLOW_POS		(P_OVERFLOW_POS + _FIELD_WITDTH)

#define ERROR_MAP(pu,nu,po,no)	(((pu) << P_UNDERFLOW_POS)	|  \
				 ((nu) << N_UNDERFLOW_POS)	|  \
				 ((po) << P_OVERFLOW_POS)	|  \
				 ((no) << N_OVERFLOW_POS) )

#define MAP_MASK		MAKE_MASK(_FIELD_WITDTH,0)
#define ERROR_INDEX(s,m,n,p)	(m >> (s ? n : p)) & MAP_MASK
#define ERROR(s,m,n,p)		BESSEL_ERROR_CODE_TABLE[ ERROR_INDEX(s,m,n,p) ]
#define OVERFLOW_ERROR(s,m)	ERROR(s, m, N_OVERFLOW_POS,  P_OVERFLOW_POS)
#define UNDERFLOW_ERROR(s,m)	ERROR(s, m, N_UNDERFLOW_POS, P_UNDERFLOW_POS)


#if !defined(C_BESSEL)
#    define C_BESSEL		__INTERNAL_NAME(C_bessel__)
#endif

static void
C_BESSEL(_X_FLOAT * packed_argument, WORD order, WORD bessel_kind,
  U_WORD const * class_to_action_map, WORD const error_map,
  _X_FLOAT * packed_result OPT_EXCEPTION_INFO_DECLARATION )
    {
    WORD fp_class;
    UX_SIGN_TYPE sign, sign_toggle;
    UX_FRACTION_DIGIT_TYPE hi;
    UX_FLOAT unpacked_argument, unpacked_result[2];

    fp_class = UNPACK(
        packed_argument,
        & unpacked_argument,
        class_to_action_map,
        packed_result
        OPT_EXCEPTION_INFO_ARGUMENT );

    /* Map negative arguments onto positive arguments */

    sign = G_UX_SIGN(&unpacked_argument);
    P_UX_SIGN(&unpacked_argument, 0);

    /* Account for reflection formula: C(-n,x) = (-1)^n*C(x) */

    sign_toggle = UX_SIGN_BIT;
    if (order < 0)
        {
        order = -order;
        sign ^= sign_toggle;
        }

    sign_toggle &= ((order & 1) ? sign : 0);

    if (0 > fp_class)
        {
        if (1 < order)
             {
             /*
             ** If orders >= 2, the unpack routine returns C(|n|,|x|), so
             ** we have to adjust the sign of the packed result.
             */
             hi = G_X_DIGIT( packed_result, 0);
             if ( (hi & F_EXP_MASK) != F_EXP_MASK )
                 hi |= (((UX_FRACTION_DIGIT_TYPE) sign_toggle) <<
                    (BITS_PER_UX_FRACTION_DIGIT_TYPE - BITS_PER_UX_SIGN_TYPE));
             P_X_DIGIT( packed_result, 0, hi );
             }
        return;
        }

    UX_BESSEL(&unpacked_argument, order, bessel_kind, unpacked_result);
    UX_TOGGLE_SIGN( unpacked_result, sign_toggle );

    sign_toggle = G_UX_SIGN(unpacked_result);
    PACK(
        unpacked_result,
        packed_result,
        UNDERFLOW_ERROR(sign_toggle, error_map),
        OVERFLOW_ERROR(sign_toggle, error_map)
        OPT_EXCEPTION_INFO_ARGUMENT );
    }

/*
** The following six routines are the user level bessel functions j0, j1, jn,
** y0, y1 and yn.  Each of the interfaces simply passes information onto the
** C_BESSEL routine.
*/

#define BESSEL_0_1_ENTRY(order, kind, class, map)			 \
        X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument) \
            BESSEL_BODY(order, kind, class, map)

#define BESSEL_N_ENTRY(kind, class, map)			\
	X_IX_PROTO(F_ENTRY_NAME, packed_result, order, packed_argument) \
            BESSEL_BODY(order, kind, class, map)

#define BESSEL_BODY(order, kind, class, map)	\
		{				\
		EXCEPTION_INFO_DECL	\
                DECLARE_X_FLOAT(packed_result) \
						\
		INIT_EXCEPTION_INFO;		\
		C_BESSEL(			\
		    PASS_ARG_X_FLOAT(packed_argument),		\
		    order, kind, class, map,	\
		    PASS_RET_X_FLOAT(packed_result)		\
		    OPT_EXCEPTION_INFO);	\
                RETURN_X_FLOAT(packed_result);   \
		}

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME F_J0_NAME
        BESSEL_0_1_ENTRY(0, J_BESSEL, J0_CLASS_TO_ACTION_MAP,
            ERROR_MAP( NO_ERROR, NO_ERROR, NO_ERROR, NO_ERROR ))

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME F_J1_NAME
        BESSEL_0_1_ENTRY(1, J_BESSEL, J1_CLASS_TO_ACTION_MAP,
            ERROR_MAP( J1_UNDERFLOW, J1_NEG_UNDERFLOW, NO_ERROR, NO_ERROR ))

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME F_JN_NAME
        BESSEL_N_ENTRY(J_BESSEL, JN_CLASS_TO_ACTION_MAP,
            ERROR_MAP( JN_UNDERFLOW, JN_NEG_UNDERFLOW, NO_ERROR, NO_ERROR ))

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME F_Y0_NAME
        BESSEL_0_1_ENTRY(0, Y_BESSEL, Y0_CLASS_TO_ACTION_MAP,
            ERROR_MAP( NO_ERROR, NO_ERROR, NO_ERROR, NO_ERROR ))

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME F_Y1_NAME
        BESSEL_0_1_ENTRY(1, Y_BESSEL, Y1_CLASS_TO_ACTION_MAP,
            ERROR_MAP( NO_ERROR, NO_ERROR, NO_ERROR, Y1_OVERFLOW ))

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME F_YN_NAME
        BESSEL_N_ENTRY(Y_BESSEL, YN_CLASS_TO_ACTION_MAP,
            ERROR_MAP( NO_ERROR, NO_ERROR, YN_POS_OVERFLOW, YN_NEG_OVERFLOW ))


#if defined(MAKE_INCLUDE)


#   define ASSERT_TOL(tol, p, str)					\
            if (tol < (p)) {						\
                printf("ERROR: insufficient degree for " str "\n");	\
                exit;							\
                }

    @divert -append divertText

    precision = ceil(UX_PRECISION/8) + 4;

#   undef  TABLE_NAME
#   undef  SET_BIT
#   define SET_BIT(n)	(1 << n)

    START_TABLE;

    TABLE_COMMENT("j0 class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "J0_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(3) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     2) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     2) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     2) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     2) );

    TABLE_COMMENT("j1 class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "J1_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(2) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     0) );

    TABLE_COMMENT("jn class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "JN_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_NEGATIVE,  0) );

    TABLE_COMMENT("Data for the above mappings");

        PRINT_U_TBL_ITEM( /* data 1 */ ZERO );
        PRINT_U_TBL_ITEM( /* data 2 */  ONE );


    TABLE_COMMENT("y0 class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "Y0_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(3) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     2) +
              CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_ERROR,     2) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_ERROR,     2) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ERROR,     3) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_ERROR,     3) );


    TABLE_COMMENT("y1 class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "Y1_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(2) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     4) +
              CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_ERROR,     4) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_ERROR,     4) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ERROR,     5) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_ERROR,     5) );

    TABLE_COMMENT("yn class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "YN_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,  1) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     6) +
              CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_ERROR,     6) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_ERROR,     6) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ERROR,     7) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_ERROR,     7) );

    TABLE_COMMENT("Data for the above mappings");
        PRINT_U_TBL_ITEM( /* data 1 */ ZERO );
        PRINT_U_TBL_ITEM( /* data 2 */ BES_Y0_OF_NEGATIVE );
        PRINT_U_TBL_ITEM( /* data 3 */ BES_Y0_OF_ZERO );
        PRINT_U_TBL_ITEM( /* data 4 */ BES_Y1_OF_NEGATIVE );
        PRINT_U_TBL_ITEM( /* data 5 */ BES_Y1_OF_ZERO );
        PRINT_U_TBL_ITEM( /* data 6 */ BES_YN_OF_NEGATIVE );
        PRINT_U_TBL_ITEM( /* data 7 */ BES_YN_OF_ZERO );


    J0_ENUM = 0;
    J1_ENUM = 1;
    Y0_ENUM = 2;
    Y1_ENUM = 3;

    /*
    ** The following MPHOC code is used to generate polynomials to evaluate
    ** the bessel functions on sub-intervals that are bounded by their
    ** consecutive extrema values.  On each subinterval, we evaluate a
    ** polynomial of the form x^i*p(x^2) or z^i*q(z) where z = x - a and i
    ** is 0 or 1.
    **
    ** The polynomial evaluation process for the bessel function presents
    ** a bit of a problem.  We would like to use the unpacked polynomial
    ** evaluation routine because of its performance characteristics.
    ** However, the unpacked polynomial evaluation routine requires that
    ** the polynomials be "well formed": i.e. the terms decrease in size
    ** and either alternate in sign or have the same sign.  Most of the 
    ** bessel polynomials do not meet this definition of "well formed".  The
    ** good news is that most of the bessel polynomials made into "well formed"
    ** polynomials by evaluating their even and odd terms separately.  There
    ** are a few exceptions for the y0 and y1 function: The first couple of
    ** intervals near zero cannot be made "well formed" so we need to evaluate
    ** in packed form (see the discussion of packed form polynomial evaluation
    ** in dpml_ux_ops.c).
    **
    ** In order to deal with the different types of evaluation strategies
    ** along with the polynomial coefficients, we store a number of flags
    ** defining the evaluation type and any additional information that
    ** might be required for computing the final result.  The flags are
    ** stored in the word preceeding the coefficient and include items
    ** like:
    **
    **		o The form of the polynomial - packed vs. unpacked.
    **		o pre/post processing information 
    **		o The degree of the polynomial
    **		o The bias and exponent mask used for unpacking
    */

    precision = ceil(UX_PRECISION/MP_RADIX_BITS) + 4;

    /*
    ** In order to locate the extrema and zero values as well as generate
    ** the interval coefficient, many auxillary functions are required.
    ** In most cases, we need both jn and yn versions of these function
    ** for n = 0 and 1.  In order to consolidate much of the code, we
    ** parameterize all of the function to deal with the 0 and 1 cases and
    ** refer to the jn and yn cases "indirectly" as follows:
    **
    ** Suppose __jn_func and __yn_func are the two versions of the functions
    ** we are interested in.  Then, when we need to refer to __jn_func, we
    ** include the line
    **
    **		function __bessel_func(x) { return __jn_func(x); }
    **
    ** in the mphoc and use __bessel_func to refer to __jn_func.  Similarly
    ** we can include the line
    **
    **		function __bessel_func(x) { return __yn_func(x); }
    **
    ** in the mphoc and use __bessel_func to refer to __yn_func.
    **
    ** The following "table" give forward definitions for the various
    ** __bessel_<func> that are used and indicates what they are used for.
    ** The forward definitions are required so that mphoc doesn't report
    ** syntax errors.
    */ 

    function __bessel(x)       { return x; }	/* find zeros */
    function __bessel_prime(x) { return x; }	/* find extrema */
    function __bessel_hat(x)   { return x; }	/* find coef about x = a */

    /*
    ** init_bessel sets up global values that are dependent on the order of the
    ** bessel function under consideration (order is 0 or 1).  These values
    ** are use by routines defining the functions that we are going to
    ** approximating with polynomials or rationals.
    */

    recip_pi = 1/pi;

    procedure init_bessel(n)
        {
        bessel_order = n;
        qn_asymptotic_zero_value = bessel_order*bessel_order - 1/4;
        }

    /*
    ** polynomial evaluation of jn'(x).  Used to find the extrema of j0 and j1.
    ** Actually, __jn_prime doesn't calculate jn'(x), rather it calculates
    ** jn'(x)/x^i, where i is chosen so that the leading term of the series
    ** is constant.
    */

    function __jn_prime(x)
        {
        auto z;

        z = -j1(x);
        if (bessel_order == 1)
            z = j0(x) + z/x;
        return z;
        }

    /*
    ** __jn_hat(z) is used to find the Remes coefficients for jn expanded
    ** around one of its zeros, call it a.  Specifically, jn_hat(z) =
    ** j(n,z + a)/z^i, where i = 0 or 1.
    */

    function __jn_hat(z)
        {
        auto x, y;
        if (z == 0)
            y = __jn_hat_zero_result;
        else
            {
            x = z + bessel_zero;
            y = jn(x, bessel_order);
            if (bessel_do_divide)
                y /= z;
            }
        return y;
        }

    /*
    ** __yn_prime is used (primarily) to locate the extrema of y0 and y1 by
    ** finding the zeros of __yn_prime.  Actually, __yn_prime doesn't calculate
    ** yn'(x), rather it calculates yn'(x)/x^i, where i is chosen so that the
    ** leading term of the series is constant.
    */

    function __yn_prime(x)
        {
        auto z;

        z = -y1(x);
        if (bessel_order == 1)
            z = y0(x) + z/x;
        return z;
        }

    /*
    ** __yn_hat(z) is used to find the Remes coefficients for yn(x) expanded
    ** around one of its zeros, call it a.  Specifically,
    ** yn_hat(z) = y(n,x+a)/z.
    */

    function __yn_hat(z)
        {
        auto x, y;
        if (z == 0)
            y = __yn_hat_zero_result;
        else
            y = yn(z + bessel_zero, bessel_order)/z;
        return y;
        }


    /*
    ** __yn_neumann_hat(z) is used to find the Remes coefficients for
    ** neumann_yn(x) expanded around one of its zeros, call it a.
    ** Specifically, __yn_neumann_hat(z) = neumann_yn(n,z+a)/(pi*z^i), where
    ** i = 0 or 1
    */

    function __yn_neumann_hat(z)
        {
        auto x, y;

        if (z == 0)
            y = __yn_hat_zero_result;
        else
            {
            y = neumann_yn(z + bessel_zero, bessel_order)*recip_pi;
            if (bessel_do_divide)
                y /= z;
            }
        return y;
        }

    procedure init_bessel_hat(a, do_divide)
        {
        auto t;

        bessel_zero = a;
        bessel_do_divide = do_divide;

        if (a == 0)
            {
            __jn_hat_zero_result = 1 - .5*bessel_order;
            __yn_hat_zero_result =
                (2*(log(2) - euler_gamma) + bessel_order) *
                (1 - .5*bessel_order) * recip_pi;
            remes_arg_flags = REMES_SQUARE_ARG;
            }
        else
            {
            __jn_hat_zero_result = __jn_prime(a);
            __yn_hat_zero_result = __yn_prime(a);
            remes_arg_flags = REMES_LINEAR_ARG;
            }
        }

    /*
    ** find_bessel_zero attempts to find a zero of jn or yn in the "interval"
    ** [a,b) using an approximate Newton's method to precision p. 
    **
    ** Since we are using the MPHOC find_root operator, a and b must bracket
    ** the root that is being searched for.
    **
    ** __bessel(x) is a dummy function that is redefined later on to be
    ** one of __jn or __yn.
    */

    function find_bessel_zero(a, b, p)
        {
        auto saved_precision, zero;

        saved_precision = precision;
        precision = p;
        zero = find_root(0, a, b, 0, __bessel);
        precision = saved_precision;
        return zero;
        }

    /*
    ** find_next_bessel_extrema(z, p) attempts to find the next extrema after
    ** the extrema, z, to precision p.  It does this by searching for a
    ** bracketing pair of values, (a,b) for a zero of the derivative of the
    ** function, and then uses the MPHOC find_root operator.
    **
    ** __bessel_prime(x) is a dummy function that is redefined later on to be
    ** one of __jn_prime or __yn_prime.
    */

    function find_next_bessel_extrema(z, p)
        {
        auto a, b, saved_precision;

        /*
        ** Since the difference of consecutive zeros of the bessel functions
        ** asymptotically approach pi, take a and b to be z + pi/2 and 
        ** z + 3*pi/2 respectively
        */

        saved_precision = precision;
        precision = p;

        a = z + .5*pi;
        b = a + pi;

        if (__bessel_prime(a)*__bessel_prime(b) > 0)
            {
            printf("ERROR: non-bracketing pair in find_bessel_extrema\n");
            exit;
            }

        z = find_root(0, a, b, 0, __bessel_prime);
        precision = saved_precision;
        return z;
        }

    /*
    ** The following two routines are used to generate the coefficients for
    ** the asymptotic region.
    */

    pn_zero_value = 0; /* Forward references.  Will be defined later */
    qn_zero_value = 0;

    __Pn_Qn_scale = bldexp(1, MIN_ASYMPTOTIC_EXPONENT - 1);

    function __Pn(z)
        {
        if (z == 0)
            return pn_zero_value;
        return pn_zero_value*hankel_p(__Pn_Qn_scale/z, bessel_order);
        }


    function __Qn(z)
        {
        auto x;

        if (z == 0)
            return qn_zero_value;
        x = __Pn_Qn_scale/z;
        return x * pn_zero_value*hankel_q(x, bessel_order);
        }

    /*
    ** As noted above, the coefficients for the bessel functions are not
    ** particularly well behaved:  Sometimes they do not decrease in size
    ** and sometimes, they neither alternate in sign nor all have the same
    ** sign.  The function check_em checks to see that the coefficients are
    ** decreasing and have a "nice" sign pattern.
    */

#   define FAILED	0
#   define PASSED	1

    function check_em( start, end, index )
        {
        auto i, tmp, last_sign, toggle, new_exp, old_exp;

        i = start + 1;
        old_exp = bexp(ux_rational_coefs[start]);
        ux_rational_coefs[index] = old_exp;
        last_sign = ux_rational_coefs[start] < 0 ? -1 : 1;
        while (i <= end)
            {
            tmp = ux_rational_coefs[i];
            new_exp = bexp(ux_rational_coefs[i]);
            if (new_exp > old_exp)
                {
                /* The second term not less than the first term is OK */
                if ( i <= (start + 1))
                    ux_rational_coefs[index] = new_exp;
                else
                    {
                    TABLE_COMMENT("Exponents don't decrease");
                    return FAILED;
                    }
                }
            old_exp = new_exp;

            if (last_sign*tmp > 0)
                {
                TABLE_COMMENT("Signs don't alternate");
                return FAILED;
                }
            last_sign = -last_sign;
            i++;
            }
        return PASSED;
        }

    /*
    ** As pointed out above, the ill formed coefficients of the bessel
    ** polynomials are can frequently be put into a format that is well
    ** structured.  Specifically, many of the polynomials have their even and
    ** odd coefficients form an alternating series.  That is we can write the
    ** polynomial as:
    **
    **		p(x) = e(x^2) + x*o(x^2)
    ** 
    ** where e(x) and o(x) have alternating signs and decreasing terms even
    ** though p(x) does not have decreasing terms.  The function reform_coefs
    ** takes the the coefficients of p and attempts to rearranges them into
    ** a well formed set.  Failing that, it converts the coefficients to
    ** packed form.
    */
        
# define FLAGS_OFFSET		0
# define NUM_DEGREE_OFFSET	1
# define DEN_DEGREE_OFFSET	2
# define NUM_SCALE_OFFSET	3
# define DEN_SCALE_OFFSET	4
# define NUM_DATA_LOCATIONS	5

    procedure reform_coefs(a, z, b, degree)
        {
        auto j, t, s, k, num_degree, den_degree, status, flags, index, offset;

        /*
        ** As part of the reforming process, we scale the coefficients so
        ** that we normalize the input argument to between 1/2 and 1.
        */

        t = z - a;
        s = b - z;
        if (s > t)
            t = s;

        i = 0;
        t = bexp(t);

        if ( 0 == z )
            {
            /*
            ** The bessel expansions around zero are known to be alternating
            ** in sign, so just scale the coefficients.
            **
            ** We know these polynomials use a square term and are even or
            ** odd depending on the order of the bessel function
            */

            if ( 0 == bessel_order )
                {
                s = 0;
                flags = 0;
                }
            else 
                {
                s = t;
                flags = POST_MULTIPLY;
                }

            flags = NUMERATOR_FLAGS( SQUARE_TERM + ALTERNATE_SIGN + flags );

            num_degree = degree;
            den_degree = 0;
            k = 2*t;
            for (j = 0; j <= num_degree; j++ )
                {
                ux_rational_coefs[ j ] = bldexp(ux_tmp_coefs[ j ], s);
                s += k;
                }

            offset = (128*(num_degree + 1) + BITS_PER_WORD);
            }
        else
            {
            /*
            ** These coefficients need to be split up into even and odd
            ** terms.
            **
            ** if we are dividing out a zero of the function, we need to 
            ** post multiply.  Also, if the first two terms of the original
            ** series have different signs, then we need to subtract the
            ** even and odd terms rather than add them.
            */

            flags =
              DENOMINATOR_FLAGS(POST_MULTIPLY + SQUARE_TERM + ALTERNATE_SIGN) +
              NUMERATOR_FLAGS(SQUARE_TERM + ALTERNATE_SIGN) +
              BESSEL_USE_ZERO + BESSEL_NO_DIVIDE;

            s = 0;
            if (bessel_do_divide)
                {
                flags += BESSEL_POST_MULTIPLY;
                s = t;
                }

            flags += ((((ux_tmp_coefs[0]*ux_tmp_coefs[1] < 0) ?
		SUB : ADD) + 1) << BESSEL_EVEN_ODD_OP_POS);

            if ((ux_tmp_coefs[0] < 0))
                flags += BESSEL_NEGATE_POLY;

            num_degree = floor(degree/2);
            k = num_degree + 1;
            den_degree = degree - k;
            ux_rational_coefs[degree + 1 ] = 0; /* make sure its initialized */

            for (j = 0; j <= num_degree; /* NULL */ )
                {
                ux_rational_coefs[ j++ ] = bldexp(ux_tmp_coefs[ i++ ], s);
                s += t;
                ux_rational_coefs[ k++ ] = bldexp(ux_tmp_coefs[ i++ ], s);
                s += t;
                }

            offset = 2*(128*(num_degree + 1) + BITS_PER_WORD);
            }

        flags += (num_degree << BESSEL_DEGREE_POS);
        index = degree + 1;
        status = check_em( 0, num_degree, index + NUM_SCALE_OFFSET );
        if (den_degree > 0)
            status = status & check_em( num_degree + 1, degree,
               index + DEN_SCALE_OFFSET);

        if (FAILED != status)
            /* Add scale factor for unpacked evaluations */
            flags += (((t > 0) ? ((1 << SCALE_WIDTH) - t) : t) << SCALE_POS);
        else
            {
            /*
            ** Need to use packed evaluation here, so do the conversion.
            **
            ** The call to find_exponent_width and bias sets the global
            ** values packed_exponent_width and packed_exponent_bias.
            ** Since find_exponent_width_and_bias and cvt_to_packed expected
            ** the coefficients to be in the array ux_rational_coefs, copy
            ** them there in the correct order
            */

            for (i = 0; i <= degree; i++)
                ux_rational_coefs[i] = ux_tmp_coefs[i];
            find_exponent_width_and_bias(degree, 0);
            cvt_to_packed(degree, 0, packed_exponent_width,
                packed_exponent_bias);

            if (packed_exponent_width >= (1 << BESSEL_EXP_WIDTH_WIDTH))
                printf(
                  "\tERROR: packed_exponent_width = %i exceeds field width\n",
                  packed_exponent_width);

            if (packed_exponent_bias >= (1 << BESSEL_EXP_BIAS_WIDTH))
                printf(
                  "\tERROR: packed_exponent_bias = %i exceeds field width\n",
                  packed_exponent_bias);

            offset = 128*(degree + 1);
            flags = (flags & BESSEL_COMMON_FLAGS_MASK) + BESSEL_PACKED_POLY +
                ((degree << BESSEL_DEGREE_POS) +
                (packed_exponent_bias << BESSEL_EXP_BIAS_POS) + 
                (packed_exponent_width << BESSEL_EXP_WIDTH_POS)); 
            num_degree = degree;
            den_degree = 0;
            }
        offset = (offset + FIXED_BITS_PER_INTERVAL_DATA) / BITS_PER_CHAR;
        flags += (offset << OFFSET_POS);
        
        ux_rational_coefs[index + FLAGS_OFFSET ]      = flags;
        ux_rational_coefs[index + NUM_DEGREE_OFFSET ] = num_degree;
        ux_rational_coefs[index + DEN_DEGREE_OFFSET ] = den_degree;

        /* Save degree in ux_tmp_coefs[0] in case we need it later */

        ux_tmp_coefs[0] = degree;
        }

    /*
    ** The function foo is used to determine the points at which we can
    ** approximate y0 and y1 using the neumann_yn functions without losing
    ** signficance (see (11)).  In particular, we require the the ratio of
    ** yn and yn(x) - (2/pi)*jn(x)*ln(x) be greater than 1/2.
    */

    two_over_pi = 2*recip_pi;
    function foo(x)
        {
        auto num, den;

        num = yn(x, bessel_order);
        den = num - two_over_pi*jn(x, bessel_order)*log(x);
        return abs(num/den) - .5;
        }

    /*
    ** print_interval_data prints the Remes coefficients and the associated
    ** zeros in the order/format specified in the INTERVAL_DATA structure
    ** definitions.
    **
    ** The Remes coefficients are implicitly passed to this routine via the
    ** global array ux_fraction_digits.  The evaluation flags for the
    ** polynomial, the numerator/denominator degrees and the scale factor
    ** are stored in ux_fraction_digits[index, index+1, index+2, index+3]
    ** respectively
    */

    function five_digits(x) { return nint(100000*x)/100000; }
    function low_32_bits(i) { return i - bldexp(floor(bldexp(i,-32)), 32); }

    function print_interval_data(a, z, b, k, index)
        {
        auto flags, num_degree, den_degree, poly_degree, saved_precision;

        printf("\n\t/* Data for interval %i : [ %r, %r ) - zero = %r */\n", k,
           five_digits(a), five_digits(b), five_digits(z));

        /*
        ** print the most significant digit of the upper limit of the interval
        ** in fixed point and the evaluation flags
        */

        extrema_value_high_word =
          floor(bldexp(b, BITS_PER_UX_FRACTION_DIGIT_TYPE - 5));
        PRINT_64_TBL_ITEM( extrema_value_high_word );

        flags = ux_rational_coefs[index + FLAGS_OFFSET];
        PRINT_64_TBL_ITEM( flags );

        /*
        ** Now print out the zero in extended format.  First, add in the
        ** exponent, and then print out digits from high to low
        */

        saved_precision = precision;
        precision = ceil(2*UX_PRECISION/MP_RADIX_BITS);
        z = bround(z, 2*UX_PRECISION - ZERO_EXPONENT_BITS);
        exponent = bexp(z);
        z = bldexp(z, -exponent) + bldexp(exponent, - 2*UX_PRECISION);

        for (i = 2; i > 0; i--)
            {
            printf( "\t/* %3i */", BYTES(MP_BIT_OFFSET));
            z = print_ux_fraction_digits(z);
            MP_BIT_OFFSET += UX_PRECISION;
            }
        precision = saved_precision;

        num_degree = ux_rational_coefs[index + NUM_DEGREE_OFFSET];
        den_degree = ux_rational_coefs[index + DEN_DEGREE_OFFSET];
        if ( (low_32_bits(flags) & BESSEL_PACKED_POLY) != 0)
            {
            printf("\t/* degree = %i - packed coefficients */\n", num_degree);
            print_packed(num_degree, 0);
            }
        else
            {
            poly_degree = num_degree + den_degree;
            if (den_degree)
                poly_degree++;
            printf("\t/* degree = %i - unpacked coefficients */\n",poly_degree);
            print_ux_poly_coefs(0, num_degree, 0, 0);
            if (den_degree)
                print_ux_poly_coefs(num_degree - den_degree, den_degree,
                  0, num_degree + 1);
            }
        return k+1;
        }

    /*
    ** get_coefficients computes the remes coefficients for "current" function
    ** on the interval [a,b] expanded around the point, z.  When z is zero,
    ** a square term polynomial approximation is assumed.
    ** 
    ** get_coefficients invokes reform_coefs to see if then can be made into
    ** a well formed set of coefficients.  The following table lists the
    ** possible out comes of get_coefficients based on the result reform_coefs
    ** and the value of action
    **
    **		reform
    **		result	action		Processing
    **		------	-------- ------------------------------
    **		FAILED	NO_PRINT returns k
    **			PRINT	 prints packed coefficients; return k+1;
    **			SIGNAL	 print error message and quit
    **		PASSED	NO_PRINT returns k+1
    **			PRINT	 prints packed coefficients; return k+1;
    **			SIGNAL	 prints packed coefficients; return k+1;
    */

#   define NO_PRINT		0
#   define PRINT		1
#   define SIGNAL		2

#   if STANDARD != 0
#       define AUXILIARY	0
#   else
#       define AUXILIARY	1
#   endif

    function get_coefficients(a, z, b, k, p, bessel_enum, type, do_divide,
      tol, action)
        {
        auto low, high, save_precision, actual_tol, index, tmp, flags;

        save_precision = precision;
        precision = p;

	/*
        ** We assume here that if z == 0 ==> a == 0
        */

        if ((z == 0) && (a != 0) )
            {
            printf("\tERROR: Invalid arguments to get_coefficients\n");
            exit;
            }

        low = a - z;
        high = b - z;
        init_bessel_hat(z, do_divide);
        flags = REMES_RELATIVE_WEIGHT + remes_arg_flags;

        if (DYNAMIC)
            {
            flags += REMES_FIND_POLYNOMIAL;
            if ( STANDARD == type)
                remes( flags, low, high, __bessel_hat, tol, &poly_degree,
                  &ux_tmp_coefs);
            else /* need auxillary function */
                remes( flags, low, high, __yn_neumann_hat, tol, &poly_degree,
                   &ux_tmp_coefs);
            }
        else
            {
            /* Extract fixed degree from "packed" list */

            tmp = fixed_degrees[ bessel_enum ] * 64;
            poly_degree = floor(tmp);
            fixed_degrees[ bessel_enum ] = tmp - poly_degree;
            if (0 == poly_degree)
                return k;
            flags += REMES_STATIC;

            if ( STANDARD == type)
                actual_tol = remes( flags, low, high, __bessel_hat,
                  poly_degree, 0, &ux_tmp_coefs);
            else /* need auxillary function */
                actual_tol = remes( flags, low, high, __yn_neumann_hat,
                  poly_degree, 0, &ux_tmp_coefs);

            if (actual_tol < tol)
                {
                printf(
                    "ERROR: insufficient degree for subinterval polynomial\n"
                    "       expected tol = %r, got %r\n", five_digits(tol),
                    five_digits(actual_tol));
                /* exit; */
                }
            }
        precision = save_precision;
        reform_coefs(a, z, b, poly_degree, type);

        /* Check for ill formed coefficients */

        index = poly_degree + 1;
        flags = ux_rational_coefs[index + FLAGS_OFFSET] +
           ((STANDARD == type) ? 0 : BESSEL_NEUMANN_POLY);
        ux_rational_coefs[index + FLAGS_OFFSET] = flags;

        if ((low_32_bits(flags) & BESSEL_PACKED_POLY) != 0)
            {
            if ( SIGNAL == action )
                 {
                 printf("\tERROR: expected well form coefficients\n");
                 exit;
                 }
            else if ( NO_PRINT == action )
                 return k;
            }
        else if (action == NO_PRINT)
            return k+1;

        return print_interval_data(a, z, b, k, index);
        }

    /*
    ** The function, get_neumann_coefficients generates the coefficients of
    ** the neumann function on the interval [a,b] expanded around z, where z
    ** is a zero of the neumann function in the interval [a,b] if it exists 
    ** or .5*(a+b) if it doesn't.
    */
 
    function get_neumann_coefficients(a, b, k, remes_prec, zero_prec,
      bessel_enum, tol)
        {
        auto do_divide, neumann_zero, saved_precision;

        if ((a == 0) && (bessel_order == 1))
            {
            do_divide = TRUE;
            neumann_zero = 0;
            }
        else
            {
            init_bessel_hat(0, 0 != bessel_order);
            do_divide = ( __yn_neumann_hat(a)*__yn_neumann_hat(b) < 0 );

            saved_precision = precision;
            precision = zero_prec;
            neumann_zero = do_divide ?
              find_root(0, a, b, 0, __yn_neumann_hat) : .5*(a + b);
            precision = saved_precision;

            }
        return get_coefficients(a, neumann_zero, b, k, remes_prec, bessel_enum,
           AUXILIARY, do_divide, tol, PRINT);
        }

    /*
    ** the function find_yn_bound is a "helper" function that is used to
    ** locate the boundaries of an interval were using the neumann
    ** approximations will not result in a sever cancellation error.
    */

    function find_yn_bound(z, z_inc)
        {
        auto w;

        w = z;
        while (1)
            {
            w = z + z_inc;
            if (foo(z)*foo(w) < 0)
                break;
            z = w;
            }
        return find_root(0, z, w, 0, foo);
        }
    /*
    ** find_interval_data(bessel_enum, a, x) finds all of the zeros
    ** and extrema values of jn or yn in the interval (0, x) as well as
    ** the first extrema greater than or equal to x.  For each zero, the Remes
    ** coefficients are computed for the bessel function on [e,f], where e and
    ** f are the extrema values that bracket the zero. (There's one exception
    ** to scheme described below.)
    **
    ** The value a is used to determine the location of the "first" extrema.
    ** if a != 0, we find remes coefficients on the interval (0,a) and then
    ** proceed as defined above on the interval (a,x) rather than (0,x).  The
    ** value of 'a' need not actually be the location of the first extrema.
    ** If it is not, then a + pi/2 and a + 3*pi/2 should bracket the location
    ** of the first extrema.
    **
    ** The zeros, extrema values and coefficients are written to the coefficient
    ** table.
    */

    function find_interval_data(bessel_enum, a, x)
        {
        auto b, c, save_precision, zero_precision, extrema_precision, tol,
          remes_precision, order, k, last_extrema, t, poly_degree, flags,
          index;

        /*
        ** In order to insure 'tol' bits in the zeros of jn, we need to
        ** compute bessel to at least 2*'tol' bits.
        */

        if (bessel_enum < Y0_ENUM)
            tol = F_PRECISION + 3;
        else
            tol = F_PRECISION + 1;

        save_precision    = precision;
        extrema_precision = ceil(BITS_PER_WORD/MP_RADIX_BITS) + 4;
        zero_precision    = ceil(2*tol/MP_RADIX_BITS) + 4;
        remes_precision   = ceil(tol/MP_RADIX_BITS) + 6;

        order = bessel_enum % 2;
        k = 0;
        init_bessel(order);
        last_extrema = a;

        table_offset[bessel_enum] = MP_BIT_OFFSET;
        if (bessel_enum < Y0_ENUM)
            {
            printf(
              "\n\t/* Interval polynomial coefficients for j%i */\n", order);

            if (a != 0)
                /* Get coefficients on (0,a) */
                k = get_coefficients(0, 0, a, k, remes_precision, bessel_enum,
                  STANDARD, 1 == order, tol, SIGNAL);
            }
        else
            {
            printf(
              "\n\t/* Interval polynomial coefficients for y%i */\n", order);

            /*
            ** Near 0, we need to compute yn via the neumann functions (see eq.
            ** (11)).  However, if the interval on which we use the neumann
            ** function includes a zero of yn, then we will have accuracy
            ** problems.  So the first thing we do, is find the smallest zero
            ** of yn, call it z, and compute b, so that if t is in [0,b] then
            **
            **              |            yn(t)           |
            **              | -------------------------- | > 1/2
            **              | yn(t) - (2/pi)*jn(x)*ln(x) |
            **
            ** That way, we know there can be no massive loss of significance
            ** when using the neumann functions
            */

            z = find_bessel_zero(a, a+1, zero_precision);
            b = find_yn_bound(z, -.1);
            k = get_neumann_coefficients(0, b, k, remes_precision,
              zero_precision, bessel_enum, tol);

            /*
            ** We know that expansion around the first zero of y0 or y1 between
            ** its first extrema values is ill conditioned and extremely large
            ** (hundreds of terms), so we take a *TINY* interval around the zero
            ** so that polynomial is not too long (i.e.  the performance of the
            ** packed polynomial evaluation is not to bad) and the accuracy will
            ** be OK.
            */

            c = find_yn_bound(z, .1);
            k = get_coefficients(b, z, c, k, remes_precision, bessel_enum,
               STANDARD, TRUE, tol, PRINT);

            /*
            ** We finish up the "first interval" by approximating yn via the
            ** neumann approximation on [c, e] where e is the first extrema
            ** location of yn
            */

            last_extrema = find_next_bessel_extrema(z - pi/4,extrema_precision);
            k = get_neumann_coefficients(c, last_extrema, k, remes_precision,
              zero_precision, bessel_enum, tol);

            a = last_extrema;
            }


        /*
        ** Now loop through the remaining intervals 
        */
 
        flags = 0;
        while (a <= x)
            {
            a = find_next_bessel_extrema(last_extrema, extrema_precision);
            z = find_bessel_zero(last_extrema, a, zero_precision);
            k = get_coefficients(last_extrema, z, a, k, remes_precision,
              bessel_enum, STANDARD, TRUE, tol, PRINT);
            last_extrema = a;
            }

        if (!DYNAMIC)
            { /* Check for the correct number of intervals */
            if (num_intervals[ bessel_enum ] != k)
                {
                printf(
                 "ERROR: Incorrect number of intervals for non DYNAMIC mode\n");
                exit;
                }
            }
        return last_extrema;
        }

    /*
    ** The function, get_neumann_coefficients is a helper function that
    ** generates the coefficients of the neumann functions expanded around
    ** z, where z is a zero of the neumann function in the interval [a,b]
    ** if it exists or .5*(a+b) if it doesn't
    */
 
    function get_neumann_coefficients(a, b, k, remes_prec, zero_prec,
      bessel_enum, tol)
        {
        auto do_divide, neumann_zero, saved_precision;

        if ((a == 0) && (bessel_order == 1))
            {
            do_divide = TRUE;
            neumann_zero = 0;
            }
        else
            {
            init_bessel_hat(0, 0 != bessel_order);
            do_divide = ( __yn_neumann_hat(a)*__yn_neumann_hat(b) < 0 );

            saved_precision = precision;
            precision = zero_prec;
            neumann_zero = do_divide ?
              find_root(0, a, b, 0, __yn_neumann_hat) : .5*(a + b);
            precision = saved_precision;

            }
        return get_coefficients(a, neumann_zero, b, k, remes_prec, bessel_enum,
           AUXILIARY, do_divide, tol, PRINT);
        }

    function find_yn_bound(z, z_inc)
        {
        auto w;

        w = z;
        while (1)
            {
            w = z + z_inc;
            if (foo(z)*foo(w) < 0)
                break;
            z = w;
            }
        return find_root(0, z, w, 0, foo);
        }

    /*
    ** get_asymptotic_coefficients computes the Remes rational approximations
    ** to Pn and Qn for n = 0 and 1.  It also writes its results to the
    ** the coefficient table.
    */

    procedure get_asymptotic_coefficients(j_min, y_min, n)
        {
        auto max_z, saved_precision, remes_precision, num_degree, den_degree,
            degree, remes_base_flags;

        saved_precision = precision;
        remes_precision = ceil(F_PRECISION/MP_RADIX_BITS) + 6;
        precision = remes_precision;

        max_z = bldexp(1, MIN_ASYMPTOTIC_EXPONENT - 1)/min(j_min, y_min);

        if ( max_z >= 1 )
            {
            printf("ERROR: scale factor (%i) too big for min asymptotic x\n",
               MIN_ASYMPTOTIC_EXPONENT - 1);
            exit;
            }

        pn_zero_value = 1/sqrt(bldexp(pi, MIN_ASYMPTOTIC_EXPONENT - 2));
        bessel_order = n;
        qn_zero_value = .5*(n - .25)*pn_zero_value;

        remes_base_flags = REMES_RELATIVE_WEIGHT + REMES_SQUARE_ARG;

        if (DYNAMIC)
            remes( remes_base_flags + REMES_FIND_RATIONAL, 0, max_z, __Pn,
                F_PRECISION + 6, &num_degree, &den_degree, &ux_rational_coefs);
        else
            {
            num_degree = 9;
            den_degree = 9 - n;

            tol = remes( remes_base_flags  + REMES_STATIC, 0, max_z, __Pn,
                 num_degree, den_degree, &ux_rational_coefs);

            ASSERT_TOL(tol, F_PRECISION + 6, "Pn" )
            }

        printf("#define\tP%i_COEFFICIENTS\t\t((FIXED_128 *) ((char *) "
            STR(MP_TABLE_NAME) " + %i))\n", n, BYTES(MP_BIT_OFFSET));
        degree = print_ux_rational_coefs( num_degree, den_degree, 0);
        printf("#define\tP%i_DEGREE\t\t%i\n", n, degree);

        if (DYNAMIC)
            remes( remes_base_flags + REMES_FIND_RATIONAL, 0, max_z, __Qn,
                F_PRECISION + 6, &num_degree, &den_degree, &ux_rational_coefs);
        else
            {
            num_degree = 9;
            den_degree = 10 - n;

            tol = remes( remes_base_flags + REMES_STATIC, 0, max_z, __Qn,
                num_degree, den_degree, &ux_rational_coefs);

            ASSERT_TOL(tol, F_PRECISION + 6, "Qn" )
            }

        printf("#define\tQ%i_COEFFICIENTS\t\t((FIXED_128 *) ((char *) "
            STR(MP_TABLE_NAME) " + %i))\n", n, BYTES(MP_BIT_OFFSET));
        degree = print_ux_rational_coefs( num_degree, den_degree,
          -(MIN_ASYMPTOTIC_EXPONENT - 1));
        printf("#define\tQ%i_DEGREE\t\t%i\n", n, degree);

        precision = saved_precision;
        }

    /*
    ** If we aren't using "FIND" mode, specify the number of intervals and
    ** the associated degrees of the polynomials.
    */

    if (!DYNAMIC)
        {
        num_intervals[J0_ENUM] = 7;
        num_intervals[J1_ENUM] = 8;
        num_intervals[Y0_ENUM] = 10;
        num_intervals[Y1_ENUM] =  9;

#       define PACK6(a,b,c,d,e,f) \
		(a + (b + (c + (d + (e + f/64)/64)/64)/64)/64)/64
#       define PACK7(a,b,c,d,e,f,g)	(a + PACK6(b,c,d,e,f,g))/64
#       define PACK8(a,b,c,d,e,f,g,h)	(a + PACK7(b,c,d,e,f,g,h))/64
#       define PACK9(a,b,c,d,e,f,g,h,i)	   (a + PACK8(b,c,d,e,f,g,h,i))/64
#       define PACK10(a,b,c,d,e,f,g,h,i,j) (a + PACK9(b,c,d,e,f,g,h,i,j))/64

        save_precision = precision;
        precision = ceil(16*6/8) + 1;
        fixed_degrees[ J0_ENUM ] = PACK7(30, 28, 28, 28, 28, 28, 28);
        fixed_degrees[ J1_ENUM ] = PACK8(14, 29, 28, 28, 28, 28, 28, 28);
	fixed_degrees[ Y0_ENUM ] = PACK10(20, 19, 23, 49, 34, 29, 28, 28, 28,
           28);
        fixed_degrees[ Y1_ENUM ] = PACK9(14, 29, 23, 41, 32, 28, 28, 28, 28);
        precision = save_precision;
        }
    else
        __tmp = 0;

    /*
    ** Set up __bessel() to get locations of the extrema and zeros of j0 and j1.
    */

    function __bessel(x)        { return jn(x, bessel_order); }
    function __bessel_hat(x)    { return __jn_hat(x); }
    function __bessel_prime(x)  { return __jn_prime(x); }

    /*
    ** Since the necessary value of "t" used in the each of the calls to 
    ** find_interval_data is known prior to build time and the accuracy of the
    ** algorithm as a hole is not affected by it precision, we pre-compute
    ** t to save time.
    **
    ** For j0 and j1, t is the actual location of the first extrema.
    */

    t = 0;
    min_asymptotic_value[J0_ENUM] = find_interval_data(J0_ENUM, t, 22);

    t = 1.8411837813406593026436295136444433224361;
    min_asymptotic_value[J1_ENUM] = find_interval_data(J1_ENUM, t, 22);

    /*
    ** Now set up __bessel() to get extrema locations of y0 and y1
    */

    function __bessel(x)        { return yn(x, bessel_order); }
    function __bessel_hat(x)    { return __yn_hat(x); }
    function __bessel_prime(x)  { return __yn_prime(x); }

    /*
    ** For y0 and y1 the value of t is chosen as the lower bound of an interval
    ** in which to find the first zero of y0 or y1.   We don't pre-compute
    ** this value, since it need to be known to a specific accuracy.
    */

    t = .65;
    min_asymptotic_value[Y0_ENUM] = find_interval_data(Y0_ENUM, t, 22);

    t = 1.25;
    min_asymptotic_value[Y1_ENUM] = find_interval_data(Y1_ENUM, t, 22);

    TABLE_COMMENT("P0 and Q0 rational coefficients");
    asymptotic_coef_offset[J0_ENUM] = MP_BIT_OFFSET;
    asymptotic_coef_offset[Y0_ENUM] = MP_BIT_OFFSET;
    get_asymptotic_coefficients( min_asymptotic_value[J0_ENUM],
      min_asymptotic_value[Y0_ENUM], 0 );

    TABLE_COMMENT("P1 and Q1 rational coefficients");
    asymptotic_coef_offset[J1_ENUM] = MP_BIT_OFFSET;
    asymptotic_coef_offset[Y1_ENUM] = MP_BIT_OFFSET;
    get_asymptotic_coefficients( min_asymptotic_value[J1_ENUM],
      min_asymptotic_value[Y1_ENUM], 1 );

    printf("#define BESSEL_TABLE_DATA_MAP\t"
       "(TABLE_DATA_MAP *)((char *) TABLE_NAME + %i)\n", BYTES(MP_BIT_OFFSET));

    for (i = 0; i < 4; i++)
        {
        tmp =  min_asymptotic_value[i];
        tmp =  bldexp(tmp, BITS_PER_UX_FRACTION_DIGIT_TYPE - 5);
        PRINT_64_TBL_ITEM( tmp );
        PRINT_64_TBL_ITEM( BYTES(table_offset[i] ));
        PRINT_64_TBL_ITEM( BYTES(asymptotic_coef_offset[i] ));
        }

    /*
    ** Generate miscellaneous constants
    */

    TABLE_COMMENT("1/pi, 2/pi, 2*ln2/pi");

    tmp = 2/pi;    PRINT_UX_TBL_ADEF_ITEM( "UX_TWO_OVER_PI",     tmp);
    tmp *= log(2); PRINT_UX_TBL_ADEF_ITEM( "UX_TWO_LN2_OVER_PI", tmp);

    END_TABLE;

    @end_divert
    @eval my $tableText;						\
          my $outText    = MphocEval( GetStream( "divertText" ) );	\
          my $defineText = Egrep( "#define", $outText, \$tableText );	\
             $outText    = "$tableText\n\n$defineText";			\
          my $headerText = GetHeaderText( STR(BUILD_FILE_NAME),         \
                           "Definitions and constants for bessel " .  \
                              "routines", __FILE__ );  \
             print "$headerText\n\n$outText\n";

#endif

