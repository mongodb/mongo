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

#define	BASE_NAME	mod
#include "dpml_ux.h"

#if !defined(MAKE_INCLUDE)
#   include STR(BUILD_FILE_NAME)
#endif


#define KMASK     (((U_WORD) 1 << (BITS_PER_INT - 2)) - 1)


/* 
** PRELIMINARIES
** -------------
** 
** The mod and rem functions are defined as:
** 
** 		mod(x,y) = x - y*trunc(x/y)
** 
** 		rem(x,y) = x - y*rint(x/y)
** 
** If we denote by R any of the rounding modes defined in x_float note 19.x,
** then we can consider mod and rem as specific cases of the more general
** function, Mod, defined by: 
** 
** 		Mod(x,y,R) = x - y*rnd_to_int(x/y, R)			(1)
** 
** Now, consider the following decomposition of the binary representation of
** |x|/|y|:
** 
** 		x/y = qqqqqqqqql.rpppppppp....
** 		      \_______/   \__________/
** 		          q             u
** 
** 		    = 2*q + l + r/2 + u/2
** 
** As in note 19.x, if we know the sign of x/y (call it s), and we define k,
** the sticky bit, to be 0 if u = 0 and 1 otherwise, then for each of the
** possible rounding modes, R, there is a binary function, B(R,s,l,r,k), such
** that
** 
** 	rnd_to_int(x/y, R) = (-1)^s*[2*q + l + B(R,s,l,r,k)]
** 	                   = (-1)^[sx + sy]*[2*q + l + B(R,s,l,r,k)]
** 
** where sx and sy are the sign bits of x and y respectively.  If we denote
** B(R,s,l,r,k) by B and 2*q + l by Q, it follows that
** 
** 	Mod(x,y,R) = x - y*rnd_to_int(x/y, R)
** 	           = x - y*(-1)^[sx + sy]*[Q + B]
** 	           = (-1)^sx*|x| - (-1)^sy*|y|*(-1)^[sx + sy]*[Q + B]
** 	           = (-1)^sx*|x| - (-1)^[sx + 2*sy]*|y|*[Q + B]
** 	           = (-1)^sx*|x| - (-1)^sx*|y|*[Q + B]
** 	           = (-1)^sx*{ |x| - |y|*[Q + B] }
** 	           = (-1)^sx*{ |x| - |y|*Q - |y|*B] }
** 
** Now Q = 2*q + l = trunc(x/y) so we have:
** 
** 	Mod(x,y,R) = (-1)^sx*{ |x| - |y|*Q - |y|*B] }
** 	           = (-1)^sx*{ |x| - |y|*trunc(|x|/|y|) - |y|*B] }
** 	           = (-1)^sx*{ mod(|x|, |y|) - |y|*B] }			(2)
** 
** That is, we can compute the generalized Mod function by computing
** mod(|x|,|y|), adjusting by 0 or |y|, and then optionally negating the
** result.  With the above result in mind, for the remainder of this
** discussion, we assume that x and y are positive.
** 
** A slight variation on the above description is to compute mod(2*x, y).  The
** binary expansion of 2x/y is
** 
** 		2x/y = qqqqqqqqqlr.pppppppp....
** 		       \_______/   \__________/
** 		          q             u
** 
** 		    = 4*q + 2*l + r + u
** 
** If we denote Q = trunc(2*x/y), then l and r are the two low bits of Q and 
** k = 0 or 1 if mod(2x,y) = 0 or not.  Now
** 
** 	mod(2*x,y) = 2*x - y*trunc(2*x/y)
** 	          = 2*x - y*(4*q + 2*l + r)
** 	          = 2*x - y*(4*q + 2*l) - y*r
** 	          = 2*[x - y*(2*q + l)] - y*r
** 	          = 2*[x - y*trunc(x/y)] - y*r
** 	          = 2*mod(x,y) - y*r
** 
** Substituting the above result into (2) we get
** 
** 	Mod(x,y,R) = mod(x, y) - y*B
** 	           = [ mod(2*x) + y*r ]/2 - y*B
** 	           = mod(2*x,y)/2 + y*(r/2 - B)
** 
** Since B = 0 or 1 depending on the values of s, l, r and k, and r = 0 or 1,
** it follows that r/2 - B = -1, -1/2, 0 or 1/2 depending on s, l, r and k.
** This means we can define a function, B'(R,s,l,r,k) that takes on the values
** -2, -1, 0 or 1 and compute Mod(x,y,R) as mod(2*x)/2 + y*(B'/2)
** 
** So we are led to the following basic approach to computing the generalized
** mod function:
** 
** 	o Compute u = mod(2*|x|, |y|) keeping track of the low order
** 	  digit, Q, of the quotient 2*|x|/|y|
** 	o Based on the sign bits of x and y, the low 2 bits of Q, and x',
** 	  determine the increment value B'
** 	o Compute the generalized mod function as x' + y*B' with the sign
**	  appropriately adjusted.
**
**
** LONG DIVISION REVISITED
** -----------------------
** 
** Given x = 2^n*f and y = 2^m*g where f and g are in the interval [1/2, 1),
** the basic approach to computing the quotient, 2*x/y, is to do it one
** "digit" at a time.  That is, we essentially perform long division,
** computing one quotient digit at a time while simultaneously producing
** the remainder in the process.  Continuing the analogy to long division,
** the basic process is as follows:
** 
**		(1) At each stage, we know the current remainder, x, and the 
**		    divisor, y.
**		(2) We make a guess at the next quotient digit, Q
**		(3) Compute a new remainder, x', as x' = x - Q*y.
** 
** Since the value of Q at in step (2) was a guess, the new remainder may be
** greater than y or less than zero depending on whether Q was too small or
** too large.  In any case, we can obtain the correct Q and x' by
** incrementing or decrementing Q and adding or subtracting y from x'.
** 
** An important conclusion to draw from the above discussion is that the
** correct computation of Q is very closely tied to the computation of the
** remainder.  In particular, the two computations are not done
** independently from one another, but rather they overlap each other.
** 
** In the discussion, that follows, we present a method for computing Q
** and x' that involve a 3 step process:
** 
** 	Step 1:	Obtain a mediocre estimate of Q, call it Q" based on only
** 		the high digit of x and y.
** 	Step 2: Obtain a fairly good estimate of Q, call it Q', based on
** 		the high two digits of x and Q".  In the process, we compute
** 		the high two digits of the new remainder.
** 	Step 3: Compute the low order digits of the remaider, and adjust it
** 		and Q' accordingly to obtain the final remainder and the 
** 		exact value of Q
*/ 

#define EXT_SHIFT(a,s,b,c)	(((a) << (s)) | ((b) >> (c)))

#if BITS_PER_UX_FRACTION_TYPE == 32

#   define EXTENDED_DIGIT_SHIFT_LEFT_UX_FRACTION(g, m)			\
                (m) = G_UX_FRACTION_DIGIT(g,0);				\
		P_UX_FRACTION_DIGIT(g, 0, G_UX_FRACTION_DIGIT(g, 1));	\
		P_UX_FRACTION_DIGIT(g, 1, G_UX_FRACTION_DIGIT(g, 2));	\
		P_UX_FRACTION_DIGIT(g, 2, G_UX_FRACTION_DIGIT(g, 3));	\
		P_UX_FRACTION_DIGIT(g, 3, 0);

#   define EXTENDED_BIT_SHIFT_LEFT_UX_FRACTION(g, m, s, c)		\
		{							\
		UX_FRACTION_DIGIT_TYPE _t0, _t1, _t2, _t3;		\
		 							\
                _t0 = G_UX_FRACTION_DIGIT(g,0);				\
		(m) = _t0 >> (c);					\
                _t1 = G_UX_FRACTION_DIGIT(g,1);				\
		P_UX_FRACTION_DIGIT(g, 0, EXT_SHIFT(_t0, s, _t1, c));	\
                _t2 = G_UX_FRACTION_DIGIT(g,2);				\
		P_UX_FRACTION_DIGIT(g, 1, EXT_SHIFT(_t1, s, _t2, c));	\
                _t3 = G_UX_FRACTION_DIGIT(g,3);				\
		P_UX_FRACTION_DIGIT(g, 2, EXT_SHIFT(_t2, s, _t3, c));	\
		P_UX_FRACTION_DIGIT(g, 1, _t3 << (s));			\
		}

#   define DIGIT_SHIFT_LEFT_UX_FRACTION(g,p)				\
		P_UX_FRACTION_DIGIT(p, 0, G_UX_FRACTION_DIGIT(g, 1));	\
		P_UX_FRACTION_DIGIT(p, 1, G_UX_FRACTION_DIGIT(g, 2));	\
		P_UX_FRACTION_DIGIT(p, 2, G_UX_FRACTION_DIGIT(g, 3));	\
		P_UX_FRACTION_DIGIT(p, 3, 0);

#else

#   define EXTENDED_DIGIT_SHIFT_LEFT_UX_FRACTION(g, m)			\
                (m) = G_UX_FRACTION_DIGIT(g,0);				\
		P_UX_FRACTION_DIGIT(g, 0, G_UX_FRACTION_DIGIT(g, 1));	\
		P_UX_FRACTION_DIGIT(g, 1, 0);

#   define EXTENDED_BIT_SHIFT_LEFT_UX_FRACTION(g, m, s, c)		\
		{							\
		UX_FRACTION_DIGIT_TYPE _t0, _t1;			\
		 							\
                _t0 = G_UX_FRACTION_DIGIT(g,0);				\
		(m) = _t0 >> (c);					\
                _t1 = G_UX_FRACTION_DIGIT(g,1);				\
		P_UX_FRACTION_DIGIT(g, 0, EXT_SHIFT(_t0, s, _t1, c));	\
		P_UX_FRACTION_DIGIT(g, 1, _t1 << (s));			\
		}

#   define DIGIT_SHIFT_LEFT_UX_FRACTION(g,p)				\
		P_UX_FRACTION_DIGIT(p, 0, G_UX_FRACTION_DIGIT(g, 1));	\
		P_UX_FRACTION_DIGIT(p, 1, 0);

#endif

#define MINUS_2_FLAG		0
#define MINUS_1_FLAG		1
#define ZERO_FLAG		2
#define ONE_FLAG		3
#define FLAGS_BIT_WIDTH		2
#define FLAGS_MASK		MAKE_MASK(2,0)

#if !defined(UX_MOD)
#    define UX_MOD	__INTERNAL_NAME(ux_mod__)
#endif

static WORD
UX_MOD( UX_FLOAT * x, UX_FLOAT * y, WORD rounding_flags, UX_FLOAT * result )

    {
    U_WORD SKLR;
    UX_EXPONENT_TYPE J, tmp, exponent_y;
    UX_SIGN_TYPE sign_x, sign_xor;
    UX_FLOAT ux_tmp, ux_g_lo, ux_q, product, *addend;
    UX_FRACTION_DIGIT_TYPE F1, F2, G1, R, Q, T1, T2;
    UX_FRACTION_DIGIT_TYPE old_quot; 
    WORD quotient;
    D_TYPE r, r_hi, g_hi, g_lo, r_lo;

    sign_x = G_UX_SIGN(x);
    sign_xor = (sign_x ^ G_UX_SIGN(y));
    P_UX_SIGN(x, 0);
    P_UX_SIGN(y, 0);

    /*
    ** At this point, we consider the general algorithm for long division.
    ** With x = 2^n*f, y = 2^m*g consider the following algorithm:
    ** 
    ** 			 (1) J = n - m + 1
    ** 			 (2) if (J < 0) goto (11)
    ** 			 (3) if f < g
    ** 			         f' <-- f
    ** 			         Q <-- 0 
    ** 			     else
    ** 			         Q <-- 1 
    ** 			         f' <-- f - g 
    ** 			 (4) if (J <= 0) goto (11)
    ** 			 (5) t <-- (J >= k) ? k : J
    ** 			 (6) f" <-- 2^t*f'
    ** 			 (7) Q <-- trunc(f"/g)
    ** 			 (8) f' <-- f" - Q*g
    ** 			 (9) J <-- j - t
    ** 			(10) goto 4
    ** 			(11)
    ** 
    ** 				Algorithm 1
    ** 				-----------
    ** 
    ** We state here without proof that at step (11), Q is the low order digit
    ** of the quotient 2*x/y and f' is mod(2*x, y).  
    ** 
    ** The next section of code implements the first four steps of algorithm 1.
    */

    exponent_y = G_UX_EXPONENT(y);
    J = G_UX_EXPONENT(x) - exponent_y + 1;
    P_UX_EXPONENT(x, 0);
    P_UX_EXPONENT(y, 0);
    UX_COPY(x, result);
    Q = 0;

    if (J >= 0)
        {
        ADDSUB(x, y, SUB | NO_NORMALIZATION, &ux_tmp);

        if ( G_UX_SIGN(&ux_tmp) == 0 )
            {
            Q = 1;
            UX_COPY(&ux_tmp, result);
            }
        }

    if (J <= 0)
        goto final_step;

    /* 
    ** COMPUTING Q"
    ** ------------
    ** 
    ** In step 7, we compute an estimate for Q, call it Q" by multiplying the
    ** high digit of 1/g by the high digit of f".  That is, we assume that we
    ** have a k-bit integer R, such that r = 2*R/M is a good approximation to
    ** 1/g.  For reasons that will be discussed later, we want r < 1/g.  In this
    ** section we describe how to compute R.
    ** 
    ** We note that the method for computing of R is almost identical to the
    ** method used to obtain the initial reciprocal approximation in the divide
    ** algorithm (see note 6.x).  The main difference being that in divide
    ** algorithm, r was to be as close to 1/g as possible, and for mod, we want
    ** r to underestimate 1/g. 
    ** 
    ** The value of R is computed using multi-precision floating point
    ** arithmetic and then is converted back to an integer data type.  We
    ** assume the existence of a floating point type with 53 bits of precision.
    */ 

#   if (D_PRECISION < 53)
#       error "Must have D_PRECISION >= 53"
#   endif

    /* 
    ** Let G1 be the high digit of g, K = 2^k and define g" = (G1 + 1)/K. 
    ** Note that 1/g" < 1/g.  The remainder of this section is concerned
    ** with the computation R = trunc[(K/2)/g"].  The computation of R depends
    ** on the relative size of a digit, k, compared to the floating point
    ** precision.
    */ 

    G1 = G_UX_MSD(y);

#   if (BITS_PER_UX_FRACTION_DIGIT_TYPE < D_PRECISION)

        /* 
        ** If k <= 53, then define gt = high k bits of g plus 1/K - i.e.
        ** gt = (G1 + 1)/K.  Then let R = trunc[(K/2)/gt - 1/2^53].  (NOTE:
        ** the 1/2^53 term is to compensate for a possible round up on the
        ** division.)
        */ 

        R = (UX_FRACTION_DIGIT_TYPE)
             (D_TWO_POW_2Km1/((double) (G1 + 1)) - D_TWO_POW_Km53);

#   else

        /* 
        ** If 53 < k < 78, then define the following double precision floating
        ** point values:
        ** 
        ** 		gt   = high 53 bits of g
        ** 		r    = 1/gt
        ** 		r_hi = high 24 bits of r
        ** 		g_hi = high 26 bits of g
        ** 		g_lo = next (k - 26) bits of g incremented by 1/K
        **	 	r_lo = [ (1 - g_hi*r_hi) - g_lo*r_hi] * r
        ** 
        ** and then compute
        **
        **		R = floor[ K*(r_hi + r_lo) - 1/2^78 ]		(1)
        **
        ** The 1/2^78 term corrects for any possible "rounding-up" that might
        ** take place so that taking r = 2*R/K is guaranteed to be less than
        ** 1/g and R is in the interval [K/2, K-1].  In order to ease the
        ** conversion to integer, force r_hi to be less than 1/g.  This will
        ** make r_lo positive.
        */ 

        r    = D_TWO_POW_53 / ((double) (G1 >> 11));
        r_hi = ((double)((float) r)) - D_RECIP_TWO_POW_23;
        g_hi = D_RECIP_TWO_POW_26 * ((double) ((WORD) (G1 >> 38)));
        g_lo = D_RECIP_TWO_POW_64 *
               ((double) ((WORD) ((G1 & MAKE_MASK(38, 0)) + 1)));
        r_lo = (D_GROUP( D_ONE - g_hi*r_hi) - g_lo*r_hi)*r;

        /* 
        ** Some care is required in computing (1) in order to insure no
        ** additional rounding takes place.  We begin by defining
        ** 
        ** 		R1 = floor(2^23*r_hi)
        ** 		R2 = floor(2^78*r_lo)
        ** 
        ** Since r_hi has at most 24 significant bits and r_hi > 1,
        ** R1 = 2^23*r_hi.  Also, since |r_lo| < 1/2^24, no integer overflow
        ** will occur when computing R2.  It follows that 
        ** 
        ** 		r_hi + r_lo = R1/2^23 + (R2 + e)/2^78
        ** 
        ** where 0 <= e < 1 is the error in truncating 2^78*r_lo. This implies
        ** that
        ** 
        ** 		R = floor[ K*(r_hi + r_lo) - 1/2^75 ]
        ** 		  = floor[ K*(R1/2^23 + (R2 + e))/2^78 - 1/2^75]
        ** 		  = floor[ K*(2^25*R1 + R2 + e - 8)/2^78 ]
        ** 		  = floor[ K*(2^25*R1 + R2 + e - 8)/2^78 ]
        ** 		  = 2^*(k-53)*R1 + floor[K*(R2 + e - 8)/2^78 ]
        ** 		  = [R1 << (k-53)] + [(R2 - 8) >> (78-k)]
        ** 
        ** Note that the shift of (R2-8) should be a signed shift.
        */ 

        R = (UX_SIGNED_FRACTION_DIGIT_TYPE) (D_TWO_POW_23*r_hi);
        T1 = (UX_SIGNED_FRACTION_DIGIT_TYPE) (D_TWO_POW_78*r_lo);
        R = (R << (BITS_PER_UX_FRACTION_DIGIT_TYPE - S_PRECISION)) +
	     ((T1 - 8) >> (79 - BITS_PER_UX_FRACTION_DIGIT_TYPE));

#    endif

    /*
    ** Eventually, we will want to compute Q*g exactly.  We will do that
    ** by Q*g in high and low pieces, with g_hi being the first digit of
    ** g and g_lo being the remaining digits.  At this point we create
    ** g_lo by shifting the digits of g "up" one and bringing in a zero.
    **
    ** While we're at it, we create an unpacked x-float quantity, q, so we
    ** can convert the integer Q, to an unpacked format.
    */

    DIGIT_SHIFT_LEFT_UX_FRACTION(y, &ux_g_lo);
    P_UX_SIGN(&ux_g_lo, 0);
    P_UX_EXPONENT(&ux_g_lo, 0);

    UX_SET_SIGN_EXP_MSD(&ux_q, 0, 0, 0);

    do  {

        J -= BITS_PER_UX_FRACTION_DIGIT_TYPE;
        if (J >= 0)
            { 
            EXTENDED_DIGIT_SHIFT_LEFT_UX_FRACTION(result, F1);
	    old_quot = 0; 
            }
        else
            {
            WORD shift, cshift;

            shift = BITS_PER_UX_FRACTION_DIGIT_TYPE + J;
            cshift = -J;
	    old_quot = (Q << shift);
            EXTENDED_BIT_SHIFT_LEFT_UX_FRACTION(result, F1, shift, cshift);
            J = 0;
            }

        /* 
        ** COMPUTING THE REAL Q AND NEW F'
        ** ------------------------------
        ** 
        ** The two key issues associated with algorithm 1 from a computational
        ** point of view is getting Q = trunc(2^t*f'/g) and updating f' to
        ** 2^t*f' - Q*g.  In this section we address these two issues.
        ** 
        ** Denoting the high k bits of f' by F1, the next k bits by F2, the
        ** first 2k bits by F1:F2, and denoting the high k bits of g by G1,
        ** consider the following algorithm:
        ** 
        ** 
        ** 		if (F1 == G1)				// line 1
        ** 		    {					// line 2
        ** 		    Q' <-- K - 1			// line 3
        ** 		    T1:T2 <-- F2 + G1			// line 4
        ** 		    }					// line 5
        ** 		else					// line 6
        ** 		    {					// line 7
        ** 		    Q'    <-- 2*umulh(F1*R)		// line 8
        ** 		    T1:T2 <-- F1:F2 - Q'*G1		// line 9
        ** 		    while ((T1 != 0) || (T2 >= G1))	// line 10
        ** 		        {				// line 11
        ** 		        T1:T2 <-- T1:T2 - 0:G1		// line 12
        ** 		        Q' <-- Q' + 1			// line 13
        ** 		        }				// line 14
        ** 		    }					// line 15
        ** 
        ** 			Algorithm 2
        ** 			-----------
        ** 
        ** We note that since 2*R/K underestimates 1/g and F1/K underestimates
        ** f', the Q' in line 8 underestimates Q.  Lines 9 through 15 continue
        ** to increment Q' by one until the following condition is satisfied:
        ** 
        ** 		0 <= (K*F1 + F2) - Q'*G1 <= G1 - 1	(5)
        ** 
        ** or equivalently
        ** 
        ** 		Q' = trunc[(K*F1 + F2)/G1]
        ** 
        ** We note without proof that Q' <= K-1 and Q <= Q' <= Q + 2.  I.e.
        ** Q' overestimates Q by at most 2.
        */ 

        F2 = G_UX_FRACTION_DIGIT(result, 0);

        if (F1 == G1)
            {
            Q = -1;
            T2 = F2 + G1;
            T1 = (T2 < G1);
            }
        else
            {
            UMULH(F1, R, Q);
            Q += Q;
            EXTENDED_DIGIT_MULTIPLY(Q, G1, T1, T2);
            T2 = F2 - T2;
            T1 = F1 - T1 - (T2 > F2);
            while ((T1 != 0) || (T2 >= G1))
                {
                T1 -= (T2 < G1);
                T2 -= G1;
                Q++;
                }
            }

        /* 
        ** UPDATING F'
        ** -----------
        ** 
        ** With Q' defined as above, define:
        **
        **	fh' 	the high two digits of f'
        **	fl'	the remaining digits of f'
	**	gh	the high digit of g
	**	gl	the remaining digits of g
        **
        ** we proceed with the updating of f':
        ** 
        ** 		f' <-- K*f' - Q'g
        ** 		   <-- K*(fh' + fl') - Q'(gh + gl)
        ** 		   <-- K*[(K*F1 + F2)/K^2 + fl'] - Q'(G1/K + gl)
        ** 		   <-- (K*F1 + F2)/K + K*fl' - Q'*G1/K - Q'*gl
        ** 		   <-- [(K*F1 + F2) - Q'*G1]/K + K*fl' - Q'*gl
        ** 
        ** Now the quantity in square brackets has already been computed by
        ** algorithm 2.  In particular, [(K*F1 + F2) - Q'*G1] = K*T1 + T2,
        ** and T1 is guaranteed to be 0 or 1.  Consequently we can write
        ** 
        ** 		f' <-- [(K*F1 + F2) - Q'*G1]/K + K*fl' - Q'*gl
        ** 		   <-- (K*T1+T2)/K + K*fl' - Q'*gl
        ** 
        ** Since Q' overestimates Q by at most 2, we know that we may need to
        ** adjust f' by adding in g at most twice.
        */ 

	P_UX_MSD(result, T2);
        P_UX_MSD(&ux_q, Q);
        MULTIPLY(&ux_q, &ux_g_lo, &product);
        ADDSUB(result, &product, SUB | NO_NORMALIZATION, result);

        while (G_UX_SIGN(result))
            {
            if (!T1)
                {
                addend = y;
                Q--;
                }
            else
                {
                T1--;
                addend = UX_HALF;
                ADDSUB(result, addend, ADD | NO_NORMALIZATION, result);
                }
            ADDSUB(result, addend, ADD | NO_NORMALIZATION, result);
            }
	    Q |= old_quot; 

        } while (J > 0);

    /*
    ** Now we need to deal with the rounding modes.  Get the SKLR, the sign,
    ** stick, lsb and rounding bit of the quotient and index into the rounding
    ** flags to determine how (if) x should be modified
    */

    NORMALIZE(result);

final_step:

    SKLR = ((sign_xor >> (BITS_PER_UX_SIGN_TYPE - 4)) & 8)
      | (((UX_OR_LOW_FRACTION_DIGITS(result) | G_UX_MSD(result)) == 0) ? 0 : 4)
      | (Q & 3);

    rounding_flags = (rounding_flags >> (SKLR*FLAGS_BIT_WIDTH)) & FLAGS_MASK;

    Q >>= 1;    
    UX_DECR_EXPONENT(result, 1);
    if (rounding_flags != ZERO_FLAG )
        {
        UX_DECR_EXPONENT(y, rounding_flags & 1);
        ADDSUB(result, y, (rounding_flags & 2) ? ADD : SUB, result);
        if (!(rounding_flags & 2))
            Q += 1;
        }

    /* 
    ** remquo returns a pointer to the lo (BITS_PER_INT - 2) bits 
    ** of the signed quotient 
    */

     Q &= KMASK;
     quotient = ((sign_xor) ? -Q : Q );

     /* add final sign */
     UX_TOGGLE_SIGN(result, sign_x);
     UX_INCR_EXPONENT(result, exponent_y + J);
     return quotient;
     }


/*
** C_UX_MOD is the common processing for both mod and rem.  It unpacks its
** input arguments and invokes UX_MOD to get the appropriate result.
*/

#if !defined(C_UX_MOD)
#   define C_UX_MOD	__INTERNAL_NAME(C_ux_mod__)
#endif

static WORD
C_UX_MOD(_X_FLOAT * packed_x, _X_FLOAT * packed_y, U_WORD bit_vector,
  WORD underflow_error, const U_WORD * class_to_action_map,
  _X_FLOAT * packed_result  OPT_EXCEPTION_INFO_DECLARATION)
    {
    WORD fp_class, index;
    WORD quot;
    UX_FLOAT unpacked_x, unpacked_y, unpacked_result;

    quot = 0;

    fp_class = UNPACK2(
        packed_x,
        packed_y,
        & unpacked_x,
        & unpacked_y,
        class_to_action_map,
        packed_result
        OPT_EXCEPTION_INFO_ARGUMENT
        );

    if (0 > fp_class)
        return quot;

    quot = UX_MOD(
        &unpacked_x,
        &unpacked_y,
        bit_vector,
        &unpacked_result);

    PACK(
        &unpacked_result,
        packed_result,
        underflow_error,
        NOT_USED
        OPT_EXCEPTION_INFO_ARGUMENT
        );

    return quot;
    }


/*
** The following three routines are the user level entry points for mod,
** rem, and remquo.
**
** The following macros convert the rounding flags defined in dpml_ux.h
** (referred to at B above) to the flags used by UX_MOD (referred to at B' above)
*/

#define R_minus_2B(B,i)		\
		((2 + ((i & 1) - (((2*B) >> i) & 2))) << (i * FLAGS_BIT_WIDTH))

#define CVT_B_TO_B_PRIME(B)	( R_minus_2B(B, 0) | R_minus_2B(B, 1) | \
				  R_minus_2B(B, 2) | R_minus_2B(B, 3) | \
				  R_minus_2B(B, 4) | R_minus_2B(B, 5) | \
				  R_minus_2B(B, 6) | R_minus_2B(B, 7) | \
				  R_minus_2B(B, 8) | R_minus_2B(B, 9) | \
				  R_minus_2B(B,10) | R_minus_2B(B,11) | \
				  R_minus_2B(B,12) | R_minus_2B(B,13) | \
				  R_minus_2B(B,14) | R_minus_2B(B,15) )

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_MOD_NAME

X_XX_PROTO(F_ENTRY_NAME, packed_result, packed_x, packed_y)
    {
     
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)
    INIT_EXCEPTION_INFO;
    C_UX_MOD(
        PASS_ARG_X_FLOAT(packed_x),
        PASS_ARG_X_FLOAT(packed_y),
        CVT_B_TO_B_PRIME(RZ_BIT_VECTOR),
        MOD_UNDERFLOW,
        MOD_CLASS_TO_ACTION_MAP,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);

    }

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_REM_NAME

X_XX_PROTO(F_ENTRY_NAME, packed_result, packed_x, packed_y)
    {
     
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    C_UX_MOD(
        PASS_ARG_X_FLOAT(packed_x),
        PASS_ARG_X_FLOAT(packed_y),
        CVT_B_TO_B_PRIME(RN_BIT_VECTOR),
        REM_UNDERFLOW,
        REM_CLASS_TO_ACTION_MAP,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);

    }

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_REMAINDER_NAME

X_XX_PROTO(F_ENTRY_NAME, packed_result, packed_x, packed_y)
    {
     
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    C_UX_MOD(
        PASS_ARG_X_FLOAT(packed_x),
        PASS_ARG_X_FLOAT(packed_y),
        CVT_B_TO_B_PRIME(RN_BIT_VECTOR),
        REM_UNDERFLOW,
        REM_CLASS_TO_ACTION_MAP,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);

    }

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_REMQUO_NAME

X_XXIptr_PROTO(F_ENTRY_NAME, packed_result, packed_x, packed_y, quotient)
    {
     
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)
    WORD quot;

    INIT_EXCEPTION_INFO;

    quot = C_UX_MOD(
        PASS_ARG_X_FLOAT(packed_x),
        PASS_ARG_X_FLOAT(packed_y),
        CVT_B_TO_B_PRIME(RN_BIT_VECTOR),
        REMQUO_UNDERFLOW,
        REMQUO_CLASS_TO_ACTION_MAP,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO );

    *quotient = (int) quot;

    RETURN_X_FLOAT(packed_result);

    }


#if defined(MAKE_INCLUDE)

    @divert -append divertText

    precision = ceil(UX_PRECISION/8) + 4;

    START_TABLE;

    /*
    ** Unfortunately, because the error codes for mod and rem are different,
    ** we need to duplicate the class to action mapping tables with different
    ** error codes.  Consequently, we use a subroutine to print them out
    */

    procedure print_class_to_action_table( infinity_error, zero_error )
        {
	   /* Index 0: mapping for x */	

        PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(6) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,      0) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN,  0) );

	   /* Index 1: class-to-index mapping */

        PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(5) +
	       CLASS_TO_INDEX( F_C_POS_INF,    2) +
	       CLASS_TO_INDEX( F_C_NEG_INF,    2) +
	       CLASS_TO_INDEX( F_C_POS_NORM,   3) +
	       CLASS_TO_INDEX( F_C_NEG_NORM,   3) +
	       CLASS_TO_INDEX( F_C_POS_DENORM, 4) +
	       CLASS_TO_INDEX( F_C_NEG_DENORM, 4) +
	       CLASS_TO_INDEX( F_C_POS_ZERO,   5) +
	       CLASS_TO_INDEX( F_C_NEG_ZERO ,  5) );

	    /* Index 2: mapping for y given x was +/- Infinity */

        PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(4) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,	RETURN_QUIET_NAN, 1) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,	RETURN_VALUE,     1) +
	      CLASS_TO_ACTION( F_C_POS_INF,	RETURN_ERROR,	  2) +
	      CLASS_TO_ACTION( F_C_NEG_INF,	RETURN_ERROR,	  2) +
	      CLASS_TO_ACTION( F_C_POS_NORM,	RETURN_ERROR,	  2) +
	      CLASS_TO_ACTION( F_C_NEG_NORM,	RETURN_ERROR,	  2) +
	      CLASS_TO_ACTION( F_C_POS_DENORM,	RETURN_ERROR,	  2) +
	      CLASS_TO_ACTION( F_C_NEG_DENORM,	RETURN_ERROR,	  2) +
	      CLASS_TO_ACTION( F_C_POS_ZERO,	RETURN_ERROR,	  3) +
	      CLASS_TO_ACTION( F_C_NEG_ZERO,	RETURN_ERROR,	  3) );

	    /* Index 3: mapping for y given x was +/- Norm */

        PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(3) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,	RETURN_QUIET_NAN, 1) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,	RETURN_VALUE,	  1) +
	      CLASS_TO_ACTION( F_C_POS_INF,	RETURN_VALUE,	  0) +
	      CLASS_TO_ACTION( F_C_NEG_INF,	RETURN_VALUE,	  0) +
	      CLASS_TO_ACTION( F_C_POS_NORM,	RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_NEG_NORM,	RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_POS_DENORM,	RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_NEG_DENORM,	RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_POS_ZERO,	RETURN_ERROR,	  3) +
	      CLASS_TO_ACTION( F_C_NEG_ZERO,	RETURN_ERROR,	  3) );

	    /* Index 4: mapping for y given x was +/- Denorm */

        PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(2) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,	RETURN_QUIET_NAN, 1) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,	RETURN_VALUE,	  1) +
	      CLASS_TO_ACTION( F_C_POS_INF,	RETURN_VALUE,	  0) +
	      CLASS_TO_ACTION( F_C_NEG_INF,	RETURN_VALUE,	  0) +
	      CLASS_TO_ACTION( F_C_POS_NORM,	RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_NEG_NORM,	RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_DENORM,	RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_NEG_DENORM,	RETURN_UNPACKED,  1) +
	      CLASS_TO_ACTION( F_C_POS_ZERO,	RETURN_ERROR,	  3) +
	      CLASS_TO_ACTION( F_C_NEG_ZERO,	RETURN_ERROR,	  3) );

	    /* Index 5: mapping for y given x was +/- zero */

        PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,	RETURN_QUIET_NAN, 1) +
	      CLASS_TO_ACTION( F_C_QUIET_NAN,	RETURN_VALUE,	  1) +
	      CLASS_TO_ACTION( F_C_POS_INF,	RETURN_VALUE,	  0) +
	      CLASS_TO_ACTION( F_C_NEG_INF,	RETURN_VALUE,	  0) +
	      CLASS_TO_ACTION( F_C_POS_NORM,	RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_NEG_NORM,	RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_DENORM,	RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_NEG_DENORM,	RETURN_VALUE,     0) +
	      CLASS_TO_ACTION( F_C_POS_ZERO,	RETURN_ERROR,	  3) +
	      CLASS_TO_ACTION( F_C_NEG_ZERO,	RETURN_ERROR,	  3) );

	    PRINT_U_TBL_ITEM( /* data 1 */ NULL );
	    PRINT_U_TBL_ITEM( /* data 2 */ infinity_error );
	    PRINT_U_TBL_ITEM( /* data 3 */ zero_error );
	}

    TABLE_COMMENT("mod class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "MOD_CLASS_TO_ACTION_MAP");
    print_class_to_action_table( MOD_OF_INF, MOD_BY_ZERO );

    TABLE_COMMENT("rem class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "REM_CLASS_TO_ACTION_MAP");
    print_class_to_action_table( REM_OF_INF, REM_BY_ZERO );

    TABLE_COMMENT("remquo class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "REMQUO_CLASS_TO_ACTION_MAP");
    print_class_to_action_table( REMQUO_OF_INF, REMQUO_BY_ZERO );

    TABLE_COMMENT("Unpacked constants 1/2");
    PRINT_UX_TBL_ADEF_ITEM( "UX_HALF\t\t", 1/2);

    k = BITS_PER_UX_FRACTION_DIGIT_TYPE;

    TABLE_COMMENT(
      "2^n, for n = -64, -26, -23, 0, 23, 53, 78, k-53, 2k-1, in double");
    PRINT_R_TBL_VDEF_ITEM( "D_RECIP_TWO_POW_64",  bldexp(1,     -64));
    PRINT_R_TBL_VDEF_ITEM( "D_RECIP_TWO_POW_26",  bldexp(1,     -26));
    PRINT_R_TBL_VDEF_ITEM( "D_RECIP_TWO_POW_23",  bldexp(1,     -23));
    PRINT_R_TBL_VDEF_ITEM( "D_ONE\t\t",                           1);
    PRINT_R_TBL_VDEF_ITEM( "D_TWO_POW_23\t",      bldexp(1,      23));
    PRINT_R_TBL_VDEF_ITEM( "D_TWO_POW_53\t",      bldexp(1,      53));
    PRINT_R_TBL_VDEF_ITEM( "D_TWO_POW_78\t",      bldexp(1,      78));
    PRINT_R_TBL_VDEF_ITEM( "D_TWO_POW_Km53\t",    bldexp(1,  k - 53));
    PRINT_R_TBL_VDEF_ITEM( "D_TWO_POW_2Km1\t",    bldexp(1, 2*k - 1));

    END_TABLE;

    @end_divert
    @eval my $tableText;						\
          my $outText    = MphocEval( GetStream( "divertText" ) );	\
          my $defineText = Egrep( "#define", $outText, \$tableText );	\
             $outText    = "$tableText\n\n$defineText";			\
          my $headerText = GetHeaderText( STR(BUILD_FILE_NAME),         \
                           "Definitions and constants remainder " .	\
                              "related routines", __FILE__ );		\
             print "$headerText\n\n$outText\n";
#endif

