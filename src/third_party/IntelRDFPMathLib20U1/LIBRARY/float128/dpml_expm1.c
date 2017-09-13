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
#   define BASE_NAME EXPM1_BASE_NAME
#endif

#if !defined F_ENTRY_NAME
#   define F_ENTRY_NAME	F_EXPM1_NAME
#endif

/*
** Get build file name.  Since the constant table is shared, alway define
** MAKE_COMMON
*/

#define MAKE_COMMON
#if !defined(BUILD_FILE_NAME)
#   define BUILD_FILE_NAME  F_POW_BUILD_FILE_NAME
#endif


/*
** Pick up the latest default DPML definitions. however, don't let
** dpml_private.h default TABLE_NAME.  This has already been done or over-
** ridden when the constant file was generated.
*/

#define	DONT_DEFAULT_TABLE_NAME		1
#define NEW_DPML_MACROS			1
#include "dpml_private.h"

#if defined(UNDEF_TABLE_NAME)
#   undef TABLE_NAME
#endif

/*
** Pick up common build time constants and definitions from the generated
** power constant table file
*/

#define DEFINE_SYMBOLIC_CONSTANTS	1
#include STR( BUILD_FILE_NAME )

/*
** Pick up common compile time constants and definitions
*/

#include "dpml_pow.h"

/*
**  Design Overview:
**  ---------------
**
**  The implementation of expm1 is based on the implementation of exp(x).
**  Specifically, exp is computed as follows:
**
**	o Let fm be the value of x/ln2 rounded to POW2_K bits, where POW2_K
**	  is a small positive integer value.
**	o Let I and j be the integer and fraction bits of fm and z = x - ln2*fm
**
**  Then
**
**		e^x = 2^(I + j/2^POW2_K)*e^z
**		    = 2^I * 2^(j/2^POW2_K) * [1 + z*p(z)]
**
**  where p(z) is a polynomial approximation to (e^x - 1)/x.  The value of
**  2^(j/2^POW2_K) is obtained from a table in hi and lo pieces: T(j) the
**  correctly rounded value of 2^(j/2^POW2_K) and R(j) =
**  [2^(j/2^POW2_K) - T(j)]/T(j).  Then
**
**		e^x = 2^I * [ T(j) + T(j)* R(j)] * [ 1 + z*p(z) ]
**		    = 2^I * T(j) * [ 1 + R(j)] * [ 1 + z*p(z) ]
**		    = 2^I * T(j) * [ 1 + R(j) + z*p(z) + R(j)*z*p(z) ]
**
**  If we denote by V(I,j) the product 2^I*T(j) we have
**
**		e^x = V(I,j) + V(I,j)*{ R(j) + [1 + R(j)]*z*p(z) }
**
**  Note that V(I,j) is exact and there is an alignment shift of at least
**  POW2_K + 1 bits between V(I,j) and V(I,j)*{ R(j) + [1 + R(j)]*z*p(z) },
**  there by allowing for a very accurate final result.
**
**  Based on the above, e^x - 1 is simply
**
**	expm1(x) = V(I,j) + V(I,j)*{ R(j) + [1 + R(j)]*z*p(z) } - 1.	(1)
**
**  Note that the polynomial p(z) has the form p(z) = 1 + z*q(z).  If we
**  define U(j,z) = R(j)*(1 + z) + z^2*q(z) and W(j,z) = U(j,z) + z, then
**
**	expm1(x) = V(I,j) + V(I,j)*{ R(j) + [1 + R(j)]*z*p(z) } - 1
**	         = V(I,j) + V(I,j)*{ R(j) + [1 + R(j)]*z*[1 + z*q(z)] } - 1
**	         = V(I,j) + V(I,j)*{ R(j) + [1 + R(j)]*z*[1 + z*q(z)] } - 1
**	         = V(I,j) + V(I,j)*{ R(j)*(1 + z) + z^2*q(z) + z +
**                     R(j)*z^2*q(z) } - 1
**	         = V(I,j) + V(I,j)*{ U(j,z) + z + R(j)*z^2*q(z) } - 1
**	         = V(I,j) + V(I,j)*{ W(j,z) + R(j)*z^2*q(z) } - 1.
**
**  Finally, we note that for machine precision arithmetic, the term
**  R(j)*z^2*q(z) is insignificant relative to W(j,z) so that
**
**		expm1(x) = V(I,j) + V(I,j)*W(j,z)) - 1			(2)
**
**  The trick in evaluating (2) is determining where to add the -1 term so
**  that no accuracy is lost.  The tack chosen in this routine is to divide
**  the domain of expm1 into several subdomains based on the value of I.
**
**	NOTE: When backup precision is available, only one of the subdomains,
** 	the polynomial range need be implemented.
**
**
**  Constant Range:
**
**	If I < -(F_PRECISION + 1), all of the terms except -1 are insignificant
**	in the current precision, so just return -1
**
**  Hi/lo Range:
**
**	If -(F_PRECISION + 1) <= I <= -2,  -1 is the dominate term, but
**	the sum V(I,j) - 1 will lose some precision.  In this case, we
**	break V(I,j) - 1 into hi and lo pieces as follows:
**
**		hi = V(I,j) - 1
**		lo = V(I,j) - (hi + 1)
**
**	and then write (2) as
**
**		expm1(x) = hi + [ lo + V(I,j)*W(j,z)]
**
**  Problem Range:
**
**	When I = 0 or -1, V(I,j) is very close to 1, so that V(I,j) - 1 can be
**	very small (zero in fact), which effectively eliminates the alignment
**	shift required to maintain accuracy.  In this case, we need to reorder
**	the sum in (1) in order to restore the original overhang.  Recalling
**	that W(j,z) = U(j,z) + z, and denoting V(I,j) - 1 by V1(I,j),
**
**		expm1(x) = V(I,j) + V(I,j)*W(j,z) - 1
**		         = [V(I,j) - 1] + V(I,j)*[ U(j,z) + z ]
**		         = V1(I,j) + [ V1(I,j) + 1 ]*[ U(j,z) + z ]
**		         = V1(I,j) + U(j,z) + z + V1(I,j)*[ U(j,z) + z ]
**		         = V1(I,j) + z + U(j,z) + V1(I,j)*W(j,z)
**
**	Now, suppose that z, the reduced argument is computed in hi and lo
**	pieces.  Then the above equation becomes:
**
**		expm1(x) = V1(I,j) + z + U(j,z) + V1(I,j)*W(j,z)
**		         = [V1(I,j) + z_hi] + [z_lo + U(j,z) + V1(I,j)*W(j,z)]
**
**	Note that [ V1(I,j) + z_hi ]/[ z_lo + U(j,z) + V1(I,j)*W(j,z) ] ~
**	[ V1(I,j) + z ]/[ z^2/2 + V1(I,j)*z ] so that the first term overhangs
**	the second by at least POW2_K bits.  With all of the above in mind, the
**	final calculation looks like:
**
**		hi = V1(I,j) + z_hi
**		lo = (z_hi - (hi - V1(I,j))) + z_lo
**		expm1(x) =  hi + { lo + [ U(j,z) + V1(I,j)*W(j,z) ] }
**
**  Normal Range:
**
**	When 1 <= I <= F_PRECISION - 1, V(I,j) - 1 is exact and loses at most
**	one bit of alignment shift.  In this case we can compute expm1 as
**
**		expm1(x) = [V(I,j) - 1] + V(I,j)*W(j,z)
**
**  Big range:
**
**	When F_PRECISION <= I, then 1 is insignificant relative to V(I,j).
**	In this case we can compute expm1 as
**
**		expm1(x) = V(I,j) + [ V(I,j)*W(j,z) - 1 ]
**
**	Note that on this range, overflow might occur.
**
**  Polynomial Range:
**
**	When |x| is relatively small, then I and j are both zero and no
**	alignment shift is available using (2).  In this situation, we
**	base are approximation on the Taylor series expansion:
**
**		expm1(x) = sum { x^n/n! | n = 1, ... }
**
**
**  The following diagram summerizes the evaluation ranges for expm1
**
**    -inf				  0				  +inf
**	+----------+---------+--------+---+---+--------+-----------+--------+
**      |          |         |        |       |        |           |        |
**        constant    hi/lo    problem   poly  problem   normal      big
*/


/* Select data type function dependent table values */

#define SUFFIX		BACKUP_SELECT(R, F)
#define EXPM1_HI_CHECK	PASTE( EXPM1_HI_CHECK_,     SUFFIX )
#define EXPM1_LO_CHECK	PASTE( EXPM1_LO_CHECK_,     SUFFIX )
#define POLY_CHECK	PASTE( EXPM1_POLY_CHECK_,   SUFFIX )
#define POW2_MAX_SCALE	PASTE( POW2_MAX_SCALE_,     SUFFIX )
#define POLY(x)		BACKUP_SELECT( EXPM1_POLY_R(x), EXPM1_POLY_F(x) )
#define BIG		ACC_BIG         /* 'big' for accurate pow */

/* Miscellaneous local definitions */

#define	ALIGN_WITH_I(n)    ((WORD) (n) << POW2_K)

F_TYPE
F_ENTRY_NAME( F_TYPE x )
    {
    EXCEPTION_RECORD_DECLARATION
    B_TYPE fm, z, w, t, v, one;
    B_UNION stack_tmp_u;
    F_UNION stack_tmp_v;
    U_WORD status_word;
    WORD   m, i, j;

#   if !USE_BACKUP

         B_TYPE z_hi, z_lo, u, v1;

#   endif


    /*
    ** Weed out: near overflow, certain -1 cases, NaN's, Inf's, denorms and
    ** polynomial cases.
    */

    stack_tmp_v.f = x;
    i = stack_tmp_v.F_HI_WORD;
    m = i & MAKE_MASK(F_SIGN_BIT_POS, 0);
    IF_VAX(i &= MAKE_MASK(F_SIGN_BIT_POS + 1, 0);)

    /*
    ** The product x*(1/ln2) is on the critical path of this routine.
    ** Because the code is structured with a branch prior to the multiply,
    ** it is difficult for some compilers to schedule the load of
    ** the constant 1/ln2 early enough to avoid delaying the reduced argument
    ** computation.  To avoid this delay, we preload (1/ln2)
    */

    t = RECIP_LN2;
    one = ONE;

    /*
    ** Screen out cases where expm1(x) = -1, or overflow as well as x = NaN
    ** or infinity.  We also need to screen out denorms and small argument
    ** (polynomial range).  In order to avoid code schedule issues, pre-compute
    ** the check for the polynomial range. 
    */

    j = (m <= POLY_CHECK);

    if (((U_WORD) i >= EXPM1_HI_CHECK) &&
      ((U_WORD) (i - F_SIGN_BIT_MASK) >= EXPM1_LO_CHECK))
        goto possible_problems;

    if (j)
        goto poly_range;

    /*
    ** compute the reduced argument, z.
    */

    w = ((B_TYPE) x) * t;
    INIT_FPU_STATE_AND_ROUND_TO_NEAREST(status_word);

    t = BIG + w;		/* Save for getting m later on */
    fm = t - BIG;
    BACKUP_SELECT(
        z = w - fm;, z_hi = x - fm*LN2_HI;
                     z_lo = fm*LN2_LO;
                     z = z_hi - z_lo;
        )

    /*
    ** Now get the bits of m as a integer and break it up into I and j
    */

    stack_tmp_u.f = t;
    GET_LOW_32_BITS(m, stack_tmp_u);
    j = (m & POW2_INDEX_MASK) << POW2_INDEX_POS;
    i = m & (~POW2_INDEX_MASK);

    BACKUP_SELECT(
        w = EXPM1_RED_POLY_R(z);, u = POW2_LO_OV_POW2_HI(j)*(one + z);
                                  u = EXPM1_RED_POLY_F( u, z );
                                  w = u + z;
        )

    /* Scale 2^(j/2^POW2_K) by 2^I, so that only one multiply is done */

    IF_SMALL_WORD(IPOW2_LO(stack_tmp_u, j);)
    m = IPOW2(j);
    m = W_ADD_TO_EXP_FIELD(m, ALIGN_SCALE_WITH_EXP(i));
    stack_tmp_u.B_HI_WORD = m;
    IF_VAX( m &= MAKE_MASK(F_SIGN_BIT_POS + 1, 0); )
    v = stack_tmp_u.f;

    /* We no longer care about the rounding mode, so restore it. */

    RESTORE_FPU_STATE(status_word);

#   if USE_BACKUP

        /* Scale polynomial result and check for possible overflow */

        z = (v - one) + v*w;
        if (m > POW2_MAX_SCALE)
            goto boundary_check;

#   else

        /*
        ** Strip out the big case since once thats done, its safe to use
        ** V(i,j) in arithmetic expressions
        */

        if ( i >= ALIGN_WITH_I(F_PRECISION))
            goto big_region;

        /*
         * Normal, problem and hi/lo ranges get here
         */

        v1 = v - one;

        if (i >= ALIGN_WITH_I(1))
             goto normal_region;

        if (i <= ALIGN_WITH_I(-2))
             goto hi_lo_region;

    /* problem_region: */

        t = v1 + z;
        z_lo = (z_hi - (t - v1)) - z_lo;
        z = t + (z_lo + (u + v1*w));
        goto return_z;

    hi_lo_region:

        z = v1 + ((v - (v1 + one)) + w*v);
        goto return_z;

    normal_region:

        z = v1 + w*v;
        goto return_z;

    big_region:

        /*
        ** Screen for overflows with moderate care.  If no overflow possible,
        ** just go ahead and compute the result.  At this point, m is the
        ** hi bits of V(I,j) = 2^I*T(j)
        */

        if (m > POW2_MAX_SCALE)
             goto boundary_check;

        z = v + (v*w - one);
        /* Fall through */

    return_z:

#   endif

       return (F_TYPE) z;


poly_range:

    /*
    ** At this point x is small and m is the high word of |x|.  If x is
    ** really tiny (including denorms), we can just return x.  Otherwise
    ** compute the series evaluation for expm1.
    */

    if (m > ALIGN_W_EXP_FIELD(F_EXP_BIAS - F_PRECISION - F_NORM))
        x = POLY(x);
    return x;


boundary_check:

    /* 
    ** Do the "final" multiple.  However, when no backup is available
    ** the final multiply might involve a NaN or dirty zero, so we need to
    ** do this scaling carefully
    */

    IF_NO_BACKUP(
        /* Multiply by table entry */
        t = POW2_HI(j);
        z = t + t*w;
        )

    stack_tmp_u.f = z;
    m = stack_tmp_u.B_HI_WORD;

    IF_NO_BACKUP(
        /* "Multiply" by 2^i */
        m = W_ADD_TO_EXP_FIELD(m, ALIGN_SCALE_WITH_EXP(i));
        stack_tmp_u.B_HI_WORD = m;
        z = stack_tmp_u.f;
        )

    /* Isolate exponent field and check for overflow */

    j = EXPM1_OVERFLOW;
    IF_VAX( m &= B_SIGN_EXP_MASK; )
    if ((U_WORD) m >= ALIGN_WITH_B_TYPE_EXP(F_MAX_BIN_EXP + B_EXP_BIAS + 1))
       goto do_exception;
    return (F_TYPE) z;


    NaN_or_Inf:

       /*
        * If we get here, i and m are the high words of x and |x|
        * respectively.  If m doesn't contain all of the bits of x,
        * check for non-zero bits in the low word
        */
       m <<= (BITS_PER_WORD - F_EXP_POS);
       IF_SMALL_WORD( m = m OR_LOW_BITS_SET(stack_tmp_v); )
       if ( m == 0 )
           { /* x was +/- infinity */
           j = (i & F_SIGN_BIT_MASK) ? EXPM1_OF_NEG_INF : EXPM1_OF_INF;
           goto do_exception;
           }
       return x;

return_minus_1:
    return (F_TYPE) -one;

possible_problems:

    /*
    ** If we get here, x is: large or an IEEE special case (NaN or Inf).
    ** Start by weeding out NaN and infinities
    */

    IF_IEEE(
        /* Screen out NaN's and Inf's */    
        if (m >= F_EXP_MASK) goto NaN_or_Inf;
        )

    /*
    ** If x is negative, expm1(x) = -1 to machine precision.  Otherwise,
    ** expm1(x) overflows.
    **
    **		NOTE: at this point i is the high word of x and m
    **		is i &= ~F_SIGN_BIT_MASK
    */

    if (i & F_SIGN_BIT_MASK)
        goto return_minus_1;

    j = EXP_OVERFLOW;

do_exception:
    GET_EXCEPTION_RESULT_1(j, x, x);
    return x;
    }
