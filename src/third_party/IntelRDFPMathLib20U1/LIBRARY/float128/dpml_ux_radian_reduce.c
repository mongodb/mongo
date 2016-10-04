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

#if defined(MAKE_INCLUDE)
#   define BASE_NAME       rdx
#elif !defined(DPML_UX_RDX_BUILD_FILE_NAME)
#   define DPML_UX_RDX_BUILD_FILE_NAME	dpml_rdx_x.h
#endif

#include "dpml_ux.h"

/*
** This file contains the code for performing radian argument reduction
** for unpacked x-float arguments.  The code here is liberally borrowed
** from dpml_trig_reduce.c and assumes the existence of a file that contains
** the bits of 4/pi and appropriate definitions for accessing it.  This
** file is denoted by FOUR_OVER_PI_BUILD_FILE_NAME in dpml_names.h.
**
** The reduction routine returns the reduced argument accurate to F_PRECISION +
** EXTRA_PRECISION and the quadrant (modulo 4) that contained the original
** argument.  Special cases like infinites and NaN's are assumed to have been
** screened out prior to calling this routine.
*/

#if !defined(EXTRA_PRECISION)
#   define EXTRA_PRECISION	6
#endif


#if !defined(MAKE_INCLUDE)
//#   undef FOUR_OVER_PI_BUILD_FILE_NAME
#   include STR(DPML_UX_RDX_BUILD_FILE_NAME)
#endif

#define DEFINES
#include STR(FOUR_OVER_PI_BUILD_FILE_NAME)


/*
** BASIC ALGORITHM:
** ----------------
**
** Let z = x + octant*(pi/4).  We want to produce
**
**		y = rem( z, pi/2 )
**
** or equivalently,
**
**		Q = nint( z/(pi/2) )
**		y = z - Q*(pi/2)
**
** Note that the reduce argument is in "radians".  For computational
** purposes, it is convenient to first obtain the reduced argument in
** cycles - i.e. compute y as
**
**              c   = z/(pi/2)
**              Q   = nint(c)
**              w  = c - Q
**              y = w*(pi/2)
**
** If in the above calculations, we substitute x + octant*(pi/4) for x, we get
**
**              c   = x/(pi/2) + octant/2
**              Q   = nint(c)
**              w  = c - Q
**              y = w*(pi/2)
**
** Now, suppose instead of computing, c, Q and w, we compute c' = 2*c, Q' = 2*Q
** and w' = 2*w.  Then the above becomes
**
**              c' = x/(pi/4) + octant
**              Q' = 2*nint(c'/2)
**              w'  = x/(pi/4) + octant - Q'
**              y = w'*(pi/4)
**
** We see that the key operation is to compute x/(pi/4).  With this in mind,
** let x = 2^n*f, where 1/2 <= f < 1 and f has P' ( = 128 ) significant bits.
** If F is defined as F = 2^P'*f, it follows that F is an integer.
** Now
**
**              x/(pi/4) = x*(4/pi)
**                       = (2^n*f)*(4/pi)
**                       = [2^(n-P')]*[2^P'*f] *(4/pi)
**                       = [2^(n-P')]*F*(4/pi)
**                       = F*{2^(n-P')*(4/pi)}
**
** Suppose that we have stored a large bit string that represents the value
** of 4/pi, then we can obtain the value of 2^(n-P')*(4/pi) by moving the
** binary point in 4/pi by n-P' places.  In particular, let
**
**              2^(n-P')*(4/pi) = J*8 + g
**
** That is, J is an integer formed from the first n-P'-3 bits of 4/pi and
** g is value formed by the remaining bits.  It follows that 
**
**              x/(pi/4) = F*{2^(n-P')*(4/pi)}
**                       = F*(J*8 + g)
**                       = F*J*8 + F*g
**
** Note that we need only compute x/(pi/4) modulo 8.  Since F and J are both
** integers, the above gives
**
**              x/(pi/4) (mod 8) = (F*J*8 + F*g) (mod 8)
**                               = F*g (mod 8)
**
** At this point the algorithm for large argument reduction has the following
** flavor:
**
**              (1) index into a precomputed bit string for 4/pi to
**                  obtain g 
**              (2) compute w = F*g (mod 8)
**              (3) w <-- integer part of w + octant (mod 8)
**              (4) Q <-- nint(w)
**              (5) y = w - Q
**              (6) y = y*(pi/4)
**
**			Algorithm I
**			-----------
**
** The following sections describe the implementation issues associated with
** each of the steps in algorithm I as well as present the code for the 
** overall implementation.
**
**
** THE 4/pi TABLE
** --------------
**
** Step (1) of Algorithm I requires indexing into a bit string for 4/pi using
** the exponent field of the argument.  Specifically, if n is the argument
** exponent we want to shift the binary point of 4/pi by n - P' bits to the
** right.  If |x| < pi/4, there is no need to compute x/(pi/4), so we assume
** that we only index into the table if |x| >= 1/2.  Under this assumption,
** it is possible that n - P' is negative.  Thus to facilitate the indexing
** operation, it is necessary for the bit string to have some leading 0's.
**
** Assume the bit string for 4/pi has T leading zeros and that the bits are
** numbered in increasing order starting from 0.  I.e. the string looks like:
**
**	bit number: 0      T
**	            00...001.01000101111.....
**                          ^
**                          |
**		       binary point 
**
** From the above discussion, we want to shift the binary point of the bit
** string n-P' bits to the right and extract g as some (as yet undetermined)
** number of bits, starting 3 bits to the left of the shifted binary point.
** Consequently, the position of the most significant bit we would like to
** access is k = T + n - P' - 2.  Since we want the bit position to be greater
** than or equal to zero, and we are assuming that the argument is greater
** than or equal to 1/2 (i.e. n >= 0), it follows that T >= P' + 2.
*/

#if FOUR_OV_PI_ZERO_PAD_LEN < (UX_PRECISION + 2)
#   error "Insufficient zero padding in 4/pi table"
#endif

/*
** Since most architectures do not efficiently support bit addressing, the
** argument reduction routine assumes that the 4/pi bit string is stored
** in L-bit "digits".  Getting the right bits of 4/pi requires getting the set
** of "digits" that begin with the digit that contains the leading bit and
** doing a sequence of shifts and logical ors.  The index of the digit that
** contains the initial bit is trunc(n/L) and the bit position within that
** digit is n - L*trunc(n/L) = n % L.  For the unpacked reduction routine,
** we require the 4/pi table "digit" and a UX_FRACTION_DIGIT have the same
** length (which implies the digit length is either 32 or 64 bits).
*/
   
#if (BITS_PER_DIGIT != BITS_PER_UX_FRACTION_DIGIT_TYPE)
#   error "Digit type mis-match"
#endif

#define DIGIT_MASK(width,pos)	((( DIGIT_TYPE_CAST 1 << (width)) - 1) << (pos))
#define DIGIT_BIT(pos)		( DIGIT_TYPE_CAST 1 << (pos))

#if defined(MAKE_COMMON) || defined(MAKE_INCLUDE)
#define DIGIT_TYPE_CAST		/* MPHOC doesn't do casts */
#else
#define DIGIT_TYPE_CAST		(DIGIT_TYPE)
#endif

#define DIV_REM_BY_L(n,q,r)	(q) = (n) >> __LOG2(BITS_PER_DIGIT); \
				(r) = (n) & (BITS_PER_DIGIT - 1)


/******************************************************************************/
/*									      */
/*		Generate code for multi-precision multiplication	      */
/*									      */
/******************************************************************************/

/*
** Many of the operation used in the radian reduction scheme depend on the
** digit size.  The following code is used generate macros that hide the
** dependencies on digit size.
*/

#if defined(MAKE_INCLUDE)

    @divert -append divertText

    /*
    ** Record FOUR_OVER_PI_BUILD_FILE_NAME so we don't have to keep specifying
    ** it on the command line.
    */
    printf("#if !defined FOUR_OVER_PI_BUILD_FILE_NAME\n");
    printf("#define FOUR_OVER_PI_BUILD_FILE_NAME\t"
        STR(FOUR_OVER_PI_BUILD_FILE_NAME) "\n");
    printf("#endif\n");

    /*
    ** COMPUTING F*g
    ** -------------
    **
    ** The goal of step (2) in Algorithm I is to produce a reduced argument
    ** that is accurate to P + k bits, where k is the specified number of
    ** extra bits of precision. Also, we need to get the quadrant bits, Q.
    ** Consequently, the value of w = F*g, must be accurately computed to
    ** P + k + 3 bits.  Note however, that if x is close to a multiple of
    ** pi/2 the reduced argument will have a large number of leading zeros
    ** (in fixed point) and consequently the actual number of required bits
    ** in w will depend upon the input argument.  Since computing w is the
    ** most time consuming part of the algorithm, we would like to compute
    ** the minimum number of bits possible.  Specifically, compute w to enough
    ** bits so that if x is not near a multiple of pi/2, then the reduced
    ** argument will be accurate.  After w is computed, we can check how close
    ** the original argument was to pi/2 by examining the number of leading
    ** fractional 1's or 0's in w.  If there are too many (i.e. the reduced
    ** argument will not have enough significant bits) then we can compute
    ** additional bits of w.
    **
    ** In order to compute F*g to P + k + 3 bits, we must perform some form of 
    ** extended precision arithmetic.  For the sake of uniformity across data
    ** types and architectures, the implementation described here computes F*g
    ** by expressing F and g as fixed point values in "arrays" of some basic
    ** integer unit of computation.  As indicated above, we shall refer to this
    ** integer unit as a digit.  The choice of digit is arbitrary, however, it
    ** is best if the double length product of two digits is efficiently
    ** computed.
    **
    ** Now we need to represent w to at least P + k + 3 bits.  Since F has P'
    ** significant bits, if we use a finite precision approximation of g, call
    ** it g', then the last P' bits of the product F*g' are inaccurate.
    ** Therefore we need to represent g' to N = P' + P + k + 3 bits.   If the
    ** number of bits in a digit is L, then F and g' must be represented in at
    ** least ceil(P'/L) and D = ceil(N/L) digits respectively.
    */

    num_f_digits = ceil(UX_PRECISION/BITS_PER_DIGIT);
    num_req_bits = (F_PRECISION + UX_PRECISION + EXTRA_PRECISION + 3);
    num_w_digits = ceil(num_req_bits/BITS_PER_DIGIT);
    num_g_digits = num_w_digits;
    num_extra_bits = num_w_digits*BITS_PER_DIGIT - num_req_bits;


    printf("#define NUM_F_DIGITS\t%i\n", num_f_digits);
    printf("#define NUM_G_DIGITS\t%i\n", num_g_digits);
    printf("#define NUM_W_DIGITS\t%i\n", num_w_digits);
    printf("#define NUM_REQ_BITS\t%i\n", num_req_bits);
    printf("#define NUM_EXTRA_BITS\t%i\n", num_extra_bits);
    print;

    /*
    ** Now consider the computation of F*g' in terms of digits.  For the
    ** purpose of discussion, suppose F requires 2 digits and g' requires 4
    ** digits.
    ** Then using "black board" arithmetic F*g' looks like:
    **
    **                              binary point
    **                               |
    **                               |
    **                               |
    **                             +--------+--------+--------+--------+
    **                         g': |   g1   |   g2   |   g3   |   g4   |
    **                             +--------+--------+--------+--------+
    **             +--------+--------+
    **          F: |   F1   |   F2   |
    **             +--------+--------+
    **          ----------------------------------------------------------
    **                               |               +--------+--------+
    **                               |               |      F2*g4      |
    **                               |      +--------+--------+--------+
    **                               |      |      F1*g4      |
    **                               |      +--------+--------+
    **                               |      |      F2*g3      |
    **                             +--------+--------+--------+
    **                             |      F1*g3      |
    **                             +--------+--------+
    **                             |      F2*g2      |
    **                    +--------+--------+--------+
    **                    |      F1*g2      |
    **                    +--------+--------+
    **                    |      F2*g1      |
    **           +--------+--------+--------+
    **           |      F1*g1      | |
    **           +--------+--------+ |
    **                               |
    **          ----------------------------------------------------------
    **           +--------+--------+--------+--------+--------+--------+
    **           |  Not required   |   w1   |   w2   |   w3   |   w4   |
    **           +--------+--------+--------+--------+--------+--------+
    **
    **                              Figure 1
    **                              --------
    **
    ** The high two digits of the product are not required since we are
    ** interested in the result modulo 8.
    **
    ** In general the number of digits used to express g' will contain more
    ** than N bits.  Let the number of bits in excess of N be M.  Then if x is
    ** close to pi/2 and the number of leading fractional 0's or 1's in F*g' is
    ** less than M, F*g' still contains enough significant bits to return an
    ** accurate reduced argument.  If we denote the 3 most significant bits
    ** of w1 as o, then x will be close to pi/2 if o is odd the bits below
    ** o are 1's or o is even and the bits below o are 0's.  Therefore there
    ** will be loss of significance if w1 (in the picture above) has a binary
    ** representation of the form
    **
    **                      +----------------------+
    **                      |xx00000...00000xxxxxxx|
    **                      +----------------------+
    **                              - or -
    **                      +----------------------+
    **                      |xx11111...11111xxxxxxx|
    **                      +----------------------+
    **                         |<-- M+2 -->|
    **
    ** These two bit patterns can be detected by add and mask operations.
    **
    ** Assuming that M+2 0's or 1's appear in w1, we know that there are not
    ** enough significant bits in w to guarantee the accuracy of the answer.
    ** Consequently, we need to generate more bits of w.  This can be done by
    ** getting the next digit of g, computing the product of that digit with
    ** F and adding it into the previous value of w.  This process can be
    ** repeated until there are a sufficient number of significant bits.  Note
    ** that each additional digit of g will add one digit (L bits) of
    ** significance to w.
    **
    ** If the processes of adding additional significant bits is implemented
    ** in a naive fashion, each time through the loop will require an
    ** additional digit of storage.  Consider the situation where the first
    ** additional digit has been added to w and there are still insufficient
    ** significant bits for an accurate result.  This means that there are at
    ** least M + L leading fractional 0's or 1's.  Then w must have the form
    **
    **              |<------------ D + 1 digits ---------->|
    **              +----------+----------+     +----------+
    **              |xx########|######xxxx| ... |xxxxxxxxxx|
    **              +----------+----------+     +----------+
    **                 |<-- M+L+2 -->|
    **
    ** where the #'s indicate a string of 0's or 1's.  Since there are more
    ** than L consecutive 0's or 1's, we can compress the representation of w
    ** by one digit by removing L consecutive 0's or 1's from the first two
    ** digits of w.  If this is done w will look like
    **
    **              |<-------------- D digits ------------>|
    **              +----------+----------+     +----------+
    **              |xx#####xxx|xxxxxxxxxx| ... |xxxxxxxxxx|
    **              +----------+----------+     +----------+
    **              -->|M+2|<--
    **
    ** Which is the same as for when the first additional digit was added.
    ** It follows that we need storage for only D+1 digits of w and a counter
    ** indicating the number of additional digits that were added.
    **
    ** To recap the above discussion, algorithm I is expanded as follows:
    **
    **               (1) s <-- 0
    **               (2) w <-- first D digits of F*g
    **               (3) if w has less than or equal to M leading fractional
    **                   0's or 1's, go to step 9
    **               (4) add an additional digit of F*g to w
    **               (5) if w has less than L leading leading fractional 0's
    **                   or 1's, go to step 9
    **               (6) Compact w by removing L 0's or 1's
    **               (7) s <-- s + 1
    **               (8) go to step 3.
    **               (9) o <-- high three bits of w
    **              (10) z' <-- w - nint(w) (taking into account what
    **		            ever compaction took place, i.e. what the current
    **			    value of s is.)
    **              (11) y = z*(pi/4)
    **		
    **				Algorithm II
    **				------------
    **
    ** The above loop has two exits.  An exit from step 3 yields an
    ** approximation to w containing D digits while an exit from step 5
    ** contains D+1 digits.  In the second case, there are fewer than L
    ** leading 0's and 1's and this implies that there are enough "good" bits
    ** in the first D digits to generate the return values.  Consequently,
    ** from either exit, it is sufficient to use only the first D digits of w.
    **
    ** The exposition above on the number of leading zeros was a little loose,
    ** in that for the general case, the leading zeros and ones may not always
    ** lie entirely in the first digit of w.  In general, there can be as many
    ** as L-1 extra bits, in which case, we would need to examine both the
    ** first and second word of w. However, for the digit sizes we are
    ** considering combined with the number of extra bits we are returning,
    ** examining one digit will suffice.
    */

    p = BITS_PER_DIGIT - (num_extra_bits + 4);
    if (p < 0)
        {
        printf("ERROR: mask spans two digits\n");
        exit;
        }
    else
        {
        i = DIGIT_BIT(p);	/* to 'add 1' at position p */
        m = DIGIT_MASK(num_extra_bits + 1, p + 1);

	printf("#define W_HAS_M_BIT_LOSS\t"
           "(((MSD_OF_W + 0x%..16i) & 0x%..16i) == 0)\n", i, m);
        }

    /*
    ** DIGIT ARITHMETIC
    ** ----------------
    **
    ** In step (2) of Algorithm 2, we are computing the first D digits of the
    ** product F*g.  From figure 1, we see that, (in general) we are computing
    ** a 2*L bit product and incorporating it into the sum of previously
    ** computed 2*L bit products.  If we think of F, g and w as multi-digit
    ** integers with their digits numbered from least significant to most
    ** significant (starting at zero) and denoting the i-th digit of F by F(i)
    ** and the j-th digit of g by g(j), then the product in figure 1 can be
    ** obtained as follows:
    **
    **		t = 0;
    **		for (i = 0; i < num_g_digits; i++)
    **		    {
    **		    for (j = 0; j < num_F_digits; j++)
    **		        t = t + F[j]*g[i]*2^(j*L)
    **		    w[i] = t mod 2^L;
    **		    t = (t >> L);            
    **		    }
    **
    **			      Example 1
    **			      ---------
    **
    ** Note that each time through the loop, t is accumulating the product
    ** g[i]*F plus "the high digits" of g[i-1]*F.  It follows that t can be
    ** represented in (num_F_digits + 1) digits.
    **
    ** If F contains n digits, then the sum in the above loops looks like:
    **
    **	    +--------+     +--------+--------+--------+--------+     +--------+ 
    **   t: |  t(n)  | ... | t(j+3) | t(j+2) | t(j+1) |  t(j)  | ... |  t(0)  |
    **	    +--------+     +--------+--------+--------+--------+     +--------+ 
    **	                                     +--------+--------+
    **	 +                                   |    F[j]*g[i]    |
    **	                                     +--------+--------+
    **     --------------------------------------------------------------------
    **	    +--------+     +--------+--------+--------+--------+     +--------+ 
    **   t: | t'(n)  | ... | t'(j+3)| t'(j+2)| t'(j+1)|  t'(j) | ... |  t(0)  |
    **	    +--------+     +--------+--------+--------+--------+     +--------+ 
    **
    ** Note that t(0) through t(j-1) are unaffected and that t(j+2) through
    ** t(n) are affected only by the carry out when computing t'(j+1).  It
    ** follows that if we keep the carry out of t'(j+1) as a separate quantity,
    ** then the addition in the inner loop only affects two digits of t.  If
    ** we denote the separate carry by c(j), the picture on the next iteration
    ** of the loop (i.e. replace j by j+1) looks like:
    **
    **	    +--------+     +--------+--------+--------+--------+     +--------+ 
    **   t: |  t(n)  | ... | t(j+3) | t(j+2) | t(j+1) |  t(j)  | ... |  t(0)  |
    **	    +--------+     +--------+--------+--------+--------+     +--------+ 
    **	                        +--------+--------+
    **	                        |    F(i)*g(j+1)  |
    **	                        +--------+--------+
    **	                        +--------+
    **	 +                      |  c(j)  |
    **	                        +--------+
    **     --------------------------------------------------------------------
    **	    +--------+     +--------+--------+--------+--------+     +--------+ 
    **  t': |  t(n)  | ... | t(j+3) | t'(j+2)| t'(j+1)|  t(j)  | ... |  t(0)  |
    **	    +--------+     +--------+--------+--------+--------+     +--------+ 
    **	                   +--------+
    **	 +                 | c(k+1) |
    **	                   +--------+
    **
    **				Figure 1
    **				--------
    **
    ** The above gives rise to the notion of a multiply/add primitive that has 5
    ** inputs and 3 output: 
    **
    **	Inputs:		N, M	the most and least significant digits
    **				of t that are being added to
    **			C	the carry out from the previous mul/add
    **			A, B	The two digits that are to be multiplied
    **
    **	Outputs:	C'	The carry out of the final sum
    **			N',M'	The updated values of N and M.
    **
    ** Recalling that the number of bits per digit is denoted by L, the mul/add
    ** primitive is algebraicly defined by:
    **
    **		s  <-- (N + C)*2^L + A*B
    **		M' <-- s % 2^L
    **		N' <-- floor(s/2^L) % 2^L
    **		C' <-- floor(s/2^(2*L)) % 2^L
    **
    ** Note that in example 1, there are several special cases of the mul/add
    ** macro which might be faster depending on the values of i and j:
    **
    **	   i and j			Special case
    **	------------------	---------------------------------
    **	1) i = 0, j = 0		N = M = C = 0, C' = 0
    **	2) i = 0, j < n-1	N = C = 0, C' = 0
    **	3) i = 0, j = n-1	N = C = 0, C' = 0 and N' not needed
    **
    **	4) i > 0, j = 0		C = 0	
    **	5) i > 0, j < n-1	general case
    **	6) i > 0, j = n-1	N = 0, C' not needed
    **
    **	7) i + j = n-2		C' not needed
    **	8) i + j = n-1		C, N, C' and N' not needed
    **		
    ** Note that cases 3 and 7 are functionally identical.  For purposes of
    ** this discussion we will use the mnemonic XMUL to refer to producing a
    ** 2*L-bit product from 2 L-bit digits and XADD/XADDC to refer to the
    ** addition of one 2*L-bit integer to another without/with producing a
    ** carry out.  With this naming convention we denote the following 6
    ** mul/add operations that correspond to the 6 special cases as follows:
    **
    **	case	mul/add operator name
    **	----	---------------------
    **	 1)	 XMUL(A,B, N',M')
    **	 2)	 XMUL_ADD(A,B,M,N',M')
    **	 3)	 MUL_ADD(A,B,M,M')
    **	 4)	 XMUL_XADDC(A,B,N,M,C',N',M')
    **	 5)	 XMUL_XADDC_W_C_IN(C,A,B,N,M,C',N',M')
    **	 6)	 XMUL_XADD_W_C_IN(N,M,C,A,B,C',N',M')
    **
    ** [XMUL_XADD_W_C_IN is described with more parameters than are actually
    **  used.]
    ** [There are 8 cases, two of which are "functionally identical".  That
    ** leaves 7 cases, but only 6 have a "mul/add operator name".]
    **
    ** The mphoc code following these comments generates macros for computing
    ** the initial multiplication of F*g as a function of the number of digits
    ** in both F and g.  It assumes that NUM_F_DIGITS <= NUM_G_DIGITS
    **
    **
    **
    ** The description of digit arithmetic above indicates that we need
    ** NUM_F_DIGITS + 1 temporary locations to hold the intermediate products
    ** and sums plus one extra for dealing with carries.  For adding
    ** additional digits of the product F*g, we need at least 3 temporary
    ** locations.
    */

    num_t_digits = max(3, num_f_digits + 2);

    /*
    **  Print macros for declaring the appropriate number of digits
    */

#   define PRINT_DECL_DEF(tag,name,k)					\
	/* define 'name'0 thru 'name''k-1' */				\
	printf("#define " tag STR(name) "0");				\
	for (i = 1; i < k; i++) printf(", " STR(name) "%i", i);		\
	    printf("\n")

    PRINT_DECL_DEF("G_DIGITS\t", g, num_g_digits);
    PRINT_DECL_DEF("F_DIGITS\t", F, num_f_digits);
    PRINT_DECL_DEF("TMP_DIGITS\t", t, num_t_digits);
#   undef PRINT_DECL_DEF
    print;

    /*
    ** Print macros for referencing the most significant digits of F and g
    ** as well as declaring the high temporary as the carry digit.
    */

    printf("#define MSD_OF_W\tg%i\n", num_w_digits - 1);
    printf("#define LSD_OF_W\tg%i\n", num_w_digits - 1 - num_f_digits);
    printf("#define SECOND_MSD_OF_W\tg%i\n", num_w_digits - 2);
    printf("#define CARRY_DIGIT\tt%i\n", num_t_digits - 1);
    print;

    /*
    ** GET_F_DIGITS(x) fetches the initial digits of f from x.  We assume
    ** that num_f_digits has the same value as NUM_UX_FRACTION_DIGITS
    **
    ** PUT_W_DIGITS(x) stores the result digits into an UX_FLOAT fraction
    ** field. 
    */

    if (num_f_digits != NUM_UX_FRACTION_DIGITS)
        {
        printf("ERROR: num_f_digits != NUM_UX_FRACTION_DIGITS\n");
        exit;
        }

#   define sMAC2       "; \\\n\t"
#   define MAC2        " \\\n\t"
#   define MAC3        "\n\n"

    printf("#define GET_F_DIGITS(x)" );
    for (i = 0; i < num_f_digits; i++)
        printf( sMAC2 "F%i = G_UX_FRACTION_DIGIT(x, %i)",
          NUM_UX_FRACTION_DIGITS - 1 - i, i);
    printf(MAC3);

    printf("#define PUT_W_DIGITS(x)" );
    for (i = 0; i < num_f_digits; i++)
        printf( sMAC2 "P_UX_FRACTION_DIGIT(x, %i, g%i)",
          i, num_g_digits - 1 - i);
    printf(MAC3);

    /*
    ** NEGATE_W negates the high num_f_digits + 1 digits of w
    */

    printf("#define NEGATE_W {" );
    j = num_g_digits;
    for (i = 0; i <= num_f_digits; i++)
        { j--; printf( " \\\n\t" "g%i = ~g%i;", j, j); }
    printf( " \\\n\t" "g%i += 1; CARRY_DIGIT = (g%i == 0);", j, j);
    for (i = 1; i < num_f_digits; i++)
        {
        j++;
        printf(" \\\n\t" "g%i += CARRY_DIGIT; CARRY_DIGIT = (g%i == 0);", j, j);
        }
    printf(" \\\n\t" "g%i += CARRY_DIGIT; }\n\n", j + 1);


    /*
    **  GET_G_DIGITS_FROM_TABLE fetches the initial digits of g
    **  (and the extra_digit) from the table.
    */

    printf("#define GET_G_DIGITS_FROM_TABLE(p, extra_digit)");

    /* Better performance with DEC C -- don't auto-increment! */
    for (i = num_g_digits - 1; i >= 0; i--)
        printf(MAC2 "g%i = p[%i]; ", i, num_g_digits - 1 - i);
    printf(MAC2 "extra_digit = p[%i]; ", num_g_digits);
    printf(MAC2 "p += %i", num_g_digits + 1);
    printf(MAC3);

    /*
    ** Generate macro that aligns g bits
    **
    ** LEFT_SHIFT_G_DIGITS(lshift,BITS_PER_WORD-lshift,extra_digit) ==
    **		g = (g << lshift) | (extra_digit >> (BITS_PER_WORD-lshift)
    **/

    printf("#define LEFT_SHIFT_G_DIGITS(lshift, rshift, extra_digit)");
    for (i = num_g_digits - 1; i > 0; i--)
        printf(MAC2 "g%i = (g%i << (lshift)) | (g%i >> (rshift));",
                       i,     i,                i-1);
    printf(MAC2 "g0 = (g0 << (lshift)) | (extra_digit >> (rshift))");
    printf(MAC3);


    /*
    ** MULTIPLY_F_AND_G_DIGITS(c) == g = F* g
    */

    printf("#define MULTIPLY_F_AND_G_DIGITS(c)");

    if (num_g_digits == 1)

	printf("\t" "g0 = F0*g0\n");

    else if (num_f_digits == 1) {

        printf(MAC2 "XMUL(F0,g0,t0,g0)");

        for (i = 1; i < num_w_digits - 1; i++)
            printf(sMAC2 "XMUL_ADD(F0,g%i,t0,t0,g%i)", i, i);

        printf(sMAC2 "MUL_ADD(F0,g%i,t0,g%i)", i, i);

    } else {

        /* Get first product */
        printf(MAC2 "XMUL(g0,F0,t1,t0)");

        /*
        ** Accumulate additional products until we use up all of the F
        ** digits, or we no longer need the high digit of the XMUL.
        */

        msd_of_mul_add = 1;
        for (i = 1; i < num_f_digits; i++) {
            msd_of_mul_add++;
            if (msd_of_mul_add >= num_w_digits)
                break;
            printf(sMAC2 "XMUL_ADD(g0,F%i,t%i,t%i,t%i)", i, i, i+1, i);
        }

        /*
        ** If we no longer needed the high digit of the XMUL before using
        ** all of the F digits, add in the low bits of the final product.
        */

        if (msd_of_mul_add >= num_w_digits)
            printf(sMAC2 "MUL_ADD(g0,F%i,t%i)", i, i);

        /* Move the low bits of t to w */
	printf(sMAC2 "g0 = t0");

        /*
        ** Now multiply by the remaining digits of g.  In the code that
        ** follows, the digits of t are reused each time through the loop
        ** modulo (NUM_F_DIGITS + 1).  For example, suppose NUM_F_DIGITS
        ** is 3.  In the multiplications above, the digits of t (in most to
        ** least significant order were t[3]:t[2]:t[1]:t[0].  In the first
        ** iterations below the order is t[0]:t[3]:t[2]:t[1], and on the
        ** next iteration t[1]:t[0]:t[3]:t[2], and so on.  The variables
        ** hi, lo and first are used to track the order of the digits and
        ** the least significant digit.  Note that the high tmp digit is
        ** used as a carry digit.
        */

        for (i = 0; i < num_t_digits - 1; i++)
            next_index[i] = i + 1;
        next_index[num_t_digits - 2] = 0;

#   define UPDATE_DIGIT_INDEX(lo,hi)	lo = hi; hi = next_index[hi]

        first = 0;
        for (i = 1; i < num_w_digits; i++) {

            first = next_index[first];
            lo = first;
            hi = next_index[lo];
            msd_of_mul_add = i + 2;	/* msd is the carry out */

            if (msd_of_mul_add < num_w_digits)
                printf(sMAC2 "XMUL_XADDC(g%i,F0,t%i,t%i,c,t%i,t%i)",
                                              i,    hi, lo,   hi, lo);
            else if (msd_of_mul_add <= num_w_digits)
                printf(sMAC2 "XMUL_XADD(g%i,F0,t%i,t%i,t%i,t%i)",
                                             i,    hi, lo, hi, lo);
            else
                printf(sMAC2 "MUL_ADD(g%i,F0,t%i,t%i)",
                                          i,    lo,  lo);
            UPDATE_DIGIT_INDEX(lo,hi);

            for (j = 1; j < num_f_digits; j++) {

                msd_of_mul_add++;
                if (msd_of_mul_add < num_w_digits) {

                    if (j == (num_f_digits - 1))
                        printf(sMAC2 
                         "XMUL_XADDC(g%i,F%i,c,t%i,c,t%i,t%i)",
                                       i,  j,   lo,   hi, lo);
                    else
                        printf(sMAC2 
                         "XMUL_XADDC_W_C_IN(g%i,F%i,t%i,t%i,c,c,t%i,t%i)",
                                              i,  j, hi, lo,    hi, lo);

                } else if (msd_of_mul_add <= num_w_digits) {

                    if (j == (num_f_digits - 1))
                        printf(sMAC2 
                         "XMUL_XADD(g%i,F%i,c,t%i,t%i,t%i)",
                                      i,  j,   lo, hi, lo);
                    else
                        printf(sMAC2 
                         "XMUL_XADD_W_C_IN(g%i,F%i,t%i,t%i,c,t%i,t%i)",
                                             i,  j, hi, lo,   hi, lo);

                } else if (msd_of_mul_add <= num_w_digits + 1) {

                    printf(sMAC2 "MUL_ADD(g%i,F%i,t%i,t%i)",
                                            i,  j, lo, lo);
                } else
                    break;
                UPDATE_DIGIT_INDEX(lo,hi);
            }

            /* Move low digit of t to W */
	    printf(sMAC2 "g%i = t%i", i, first);
        }
    }
    print;
    print;

    /*
    ** Generate the macro that multiplies F by an additional digit of g
    ** and adds the product to w.
    */

    printf("#define GET_NEXT_PRODUCT(g, w, c)");
    if (num_g_digits == 1)

	printf("\t" "XMUL_XADD(g,F0,g0,w,g0,w)");

    else {

        printf(MAC2 "XMUL_XADDC(g,F0,g0,(DIGIT_TYPE)0,c,g0,w)");

        msd_of_mul_add = 1;
        for (i = 1; i < num_f_digits; i++) {
	    j = i-1;

            if (msd_of_mul_add < num_w_digits)
                printf(sMAC2
                  "XMUL_XADDC_W_C_IN(g,F%i,g%i,g%i,c,c,g%i,g%i)",
                                         i,  i,  j,      i, j);
            else if (msd_of_mul_add <= num_w_digits + 1)
                printf(sMAC2
                  "XMUL_XADD_W_C_IN(g,F%i,g%i,g%i,c,g%i,g%i)",
                                        i,  i,  j,    i, j);
            else if (msd_of_mul_add <= num_w_digits + 2)
                printf(sMAC2
                  "MUL_ADD(g,F%i,g%i,g%i)",
                               i,  j,  j);
            else
                break;
            msd_of_mul_add++;
        }
	printf(";");

        /*
        ** If there was a carry out on the last add and we are not past the
        ** last w digit, then the carry has to be propagated to the remaining
        ** w digits as necessary.
        */

        if (msd_of_mul_add < num_w_digits) {
            if (msd_of_mul_add != (num_w_digits - 1)) {
                printf(MAC2 "if (c) ");
                i = msd_of_mul_add;
                while (i < num_w_digits - 1)
                    printf(MAC2 "if (++g%i == 0) ", i++);
                printf(MAC2 "g%i++", i);
            } else
                printf(MAC2 "g%i += c", i);
        }
    }
    printf(MAC3);

    /* Generate the macro that shifts w left by 1 digit */

    printf("#define LEFT_SHIFT_W_LOW_DIGITS_BY_ONE(extra_w_digit)");
    if (num_w_digits != 1)
        {
        for (i = num_w_digits - 2; i > 0; i--)
            printf(MAC2 "g%i = g%i;", i, i-1);
        printf(MAC2 "g0 = extra_w_digit");
        }
    printf(MAC3);

    print;

    @end_divert
    @eval my $outText = MphocEval( GetStream( "divertText" ) );		\
          my $headerText = GetHeaderText( STR(BUILD_FILE_NAME),         \
                           "Definitions and constants for large " .	\
                              "radian argument reduction",__FILE__ );	\
             print "$headerText\n\n$outText";

#endif

#define	TMP_DIGIT	t0
#define	EXTRA_W_DIGIT	t1


static U_WORD
UX_RADIAN_REDUCE( UX_FLOAT * x, WORD octant, UX_FLOAT * reduced_argument )
    {
    WORD offset, scale, j;
    UX_EXPONENT_TYPE exponent;
    UX_SIGN_TYPE sign, sign_x;
    DIGIT_TYPE quadrant;
    DIGIT_TYPE F_DIGITS;		/* declare F0, ... Fm		*/
    DIGIT_TYPE G_DIGITS;		/* declare g0, ... gn		*/
    DIGIT_TYPE TMP_DIGITS;		/* declare t0, ... tm+1		*/
    DIGIT_TYPE next_g_digit;
    const DIGIT_TYPE *p;

    /*
    ** Get the fractional part of x into the fraction digits F.  While
    */

    GET_F_DIGITS(x);

    /*
    ** Assuming the input argument x has the form x = 2^n*f, where .5 <= f < 1,
    ** then F at this point is a multi-precision integer, F = 2^128*f
    **
    ** Now, use the exponent to get the bit offset of the first interesting
    ** bit in the 4/pi table.
    */

    exponent = G_UX_EXPONENT(x);
    sign_x = G_UX_SIGN(x);

    /*
    ** A negative offset would have us access memory before the start of
    ** the 4/pi table.  This indicates that the x was pretty small already,
    ** so we'll make a quick exit.
    */

    if (exponent < 0)
        {
        /*
        ** At this point the argument has absolute value less than pi/4.
        ** We need to compute the quadrant bits based on octant and possibly
        ** adjust x by a +/- pi/4.
        **
        ** If x < 0, then x + octant lies in octant - 1, not octant.
        */

        j = octant + (sign_x >> (BITS_PER_UX_SIGN_TYPE - 1));

        /*
        ** We can now get actual quadrant by looking a the parity of effective
        ** octant.  Depending on whether we round up or down, we might need
        ** to adjust x by +/- pi/4.
        */

        j = j + (j & 1);
        quadrant = j >> 1;
        j = octant - j;

        if ( j )
            ADDSUB(x, UX_PI_OVER_FOUR,  j  < 0 ? SUB : ADD, reduced_argument);
        else
            UX_COPY(x, reduced_argument);
	return quadrant;
        }

    /*
    ** Get the address of the digit containing the first interesting bit,
    ** and its bit offset within that digit.  Load G from the the table,
    ** shifting the digits by that bit offset, so that the interesting bit
    ** will become the high bit of G.
    */

    offset = exponent - ( UX_PRECISION + 2 - FOUR_OV_PI_ZERO_PAD_LEN );
    DIV_REM_BY_L(offset, j, offset);
    p = &FOUR_OVER_PI_TABLE_NAME[j];
    GET_G_DIGITS_FROM_TABLE(p, next_g_digit);
    if (offset)
        {
        j = BITS_PER_DIGIT - offset;
        LEFT_SHIFT_G_DIGITS(offset, j, next_g_digit);
        }

    /*
    **  The extended-precision multiply: w = F*g.
    */

    MULTIPLY_F_AND_G_DIGITS( /* F_DIGITS, G_DIGITS, T_DIGITS, */ CARRY_DIGIT );

    /* 
    ** Add in the variable octant.
    */

    octant = sign_x ? -octant : octant;
    MSD_OF_W += (DIGIT_TYPE)octant << (BITS_PER_DIGIT - 3);

    scale = 0;

    do {
	/*
	**  If there isn't enough significance in w, then:
	**  get more bits from the table, form the new digit into TMP_DIGIT,
	**  and add the partial product F*TMP_DIGIT to w.
	*/

        if ( !W_HAS_M_BIT_LOSS )
            break;

        TMP_DIGIT = next_g_digit;
        next_g_digit = *p++;
        if (offset)
            TMP_DIGIT = (TMP_DIGIT << offset) | (next_g_digit >> j);
        GET_NEXT_PRODUCT(TMP_DIGIT, EXTRA_W_DIGIT, CARRY_DIGIT);

        /*
        **  We're done if the there are fewer than L bits of 0's or 1's.
        */

	TMP_DIGIT = ( SECOND_MSD_OF_W >> (BITS_PER_DIGIT - NUM_EXTRA_BITS - 3))
           | (MSD_OF_W << (NUM_EXTRA_BITS + 3));
        TMP_DIGIT ^= ((SIGNED_DIGIT_TYPE) TMP_DIGIT >> (BITS_PER_DIGIT - 1));
	if ( TMP_DIGIT )
            break;

        /*
        ** Compress the current value of w and increment scale to reflect
        ** the compression 
        */

#       define OCTANT_MASK	MAKE_MASK(3, BITS_PER_DIGIT - 3)

        MSD_OF_W =  (MSD_OF_W & OCTANT_MASK) |
                       (SECOND_MSD_OF_W & ~OCTANT_MASK);
        LEFT_SHIFT_W_LOW_DIGITS_BY_ONE(EXTRA_W_DIGIT);
        EXTRA_W_DIGIT = 0;
        scale += BITS_PER_DIGIT;

    } while (1);

    /*
    ** "Sign extend" w and get the quadrant.  In the process, if the MSD_OF_W
    ** is "all" 0's or 1's, we need to shift up one digit in order to insure
    ** the proper number of significant bits in the final result. 
    */

    quadrant = MSD_OF_W;
    MSD_OF_W = MSD_OF_W << 2;
    MSD_OF_W = ((SIGNED_DIGIT_TYPE) MSD_OF_W) >> 2;
    TMP_DIGIT = MSD_OF_W;
    quadrant -= MSD_OF_W;

    if ( MSD_OF_W == ((SIGNED_DIGIT_TYPE) MSD_OF_W >> (BITS_PER_DIGIT - 1)) )
        {
        MSD_OF_W = SECOND_MSD_OF_W;
        LEFT_SHIFT_W_LOW_DIGITS_BY_ONE(EXTRA_W_DIGIT);
        scale += BITS_PER_DIGIT;
        }

    /*
    ** If the sign bit of the original MSD of w is set, then "negate" the
    ** result
    */

    sign =  ((SIGNED_DIGIT_TYPE) TMP_DIGIT) < 0 ? UX_SIGN_BIT : 0;
    if (sign)
        NEGATE_W

    /*
    ** Put w into unpacked format and normalize.  Make up for any zero bits
    ** that were shift in during the normalization.  Note that by the way the
    ** reduced argument was constructed, normalization shift cannot be bigger
    ** than the digit size.
    */

    quadrant = G_UX_SIGN(x) ? -quadrant : quadrant;
    P_UX_SIGN(reduced_argument, sign ^ sign_x);
    P_UX_EXPONENT(reduced_argument, 3);
    PUT_W_DIGITS(reduced_argument);
    NORMALIZE(reduced_argument);

    exponent = G_UX_EXPONENT(reduced_argument);
    offset = exponent - 3;
    if (offset)
        {
        offset += BITS_PER_DIGIT;
        TMP_DIGIT = G_UX_LSD( reduced_argument);
        TMP_DIGIT |= (LSD_OF_W >> offset);
        P_UX_LSD(reduced_argument, TMP_DIGIT);
        }

    P_UX_EXPONENT(reduced_argument, exponent - scale);
    MULTIPLY(reduced_argument, UX_PI_OVER_FOUR, reduced_argument);

    return quadrant >> (BITS_PER_DIGIT - 2);
    }

