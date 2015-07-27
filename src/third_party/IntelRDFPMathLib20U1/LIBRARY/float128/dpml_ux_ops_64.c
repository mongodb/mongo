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

#include "dpml_ux.h"

#if (NUM_UX_FRACTION_DIGITS != 2)
#   error "Must have 64 bit integers"
#endif

/*
** MULTIPLY essentially computes the high 128 bits of the product of two
** unpacked x-float values.  The algorithm attempts to limit the number
** of integer multiplications performed.  The resulting product has roughly
** a 6 lsb error bound in the worst case.
*/

void
MULTIPLY(UX_FLOAT * x, UX_FLOAT *y, UX_FLOAT *z)
    {
    U_WORD x_hi, x_lo, y_hi, y_lo, z_hi, z_lo, p1, p2;

    x_hi = G_UX_MSD(x);
    y_hi = G_UX_MSD(y);

    z_lo = y_hi*x_hi;
        x_lo = G_UX_LSD(x);
        y_lo = G_UX_LSD(y);

    UMULH(y_hi, x_lo, p2);
        P_UX_SIGN(z, G_UX_SIGN(x) ^ G_UX_SIGN(y));
        P_UX_EXPONENT(z, G_UX_EXPONENT(x) + G_UX_EXPONENT(y));

    UMULH(y_lo, x_hi, p1);
        z_lo += p2;
        z_hi = (z_lo < p2);

    UMULH(y_hi, x_hi, p2);
        z_lo = z_lo + p1;
        z_hi += (z_lo < p1);

    P_UX_LSD(z, z_lo);
    z_hi = z_hi + p2;
    P_UX_MSD(z, z_hi);
    }


/*
** EXTENDED_MULTIPLY computes the exact 256 bit product of two unpacked
** x-float values. The result is stored in two unpacked x-float values
** containing the high and low 128 bits of the result
*/

void
EXTENDED_MULTIPLY(UX_FLOAT * x, UX_FLOAT * y, UX_FLOAT * hi, UX_FLOAT * lo)
    {
    UX_EXPONENT_TYPE exponent;
    UX_SIGN_TYPE     sign;
    UX_FRACTION_DIGIT_TYPE x_hi, x_lo, y_hi, y_lo, tmp_digit, carry, p1, p2;

    x_lo = G_UX_LSD(x);
    y_lo = G_UX_LSD(y);

    p1 = y_lo*x_lo;
        x_hi = G_UX_MSD(x);
        y_hi = G_UX_MSD(y);

    UMULH(y_lo, x_lo, tmp_digit);
        P_UX_LSD(lo, p1);
        sign = G_UX_SIGN(x) ^ G_UX_SIGN(y);
        exponent = G_UX_EXPONENT(x) + G_UX_EXPONENT(y);
        P_UX_SIGN(lo, sign);
        P_UX_EXPONENT(lo, exponent - 128);

    p1 = y_lo*x_hi;
        P_UX_SIGN(hi, sign);
        P_UX_EXPONENT(hi, exponent);

    p2 = y_hi*x_lo;
        P_UX_SIGN(lo, sign);
        P_UX_EXPONENT(lo, exponent - 128);
        tmp_digit += p1;
        carry = (tmp_digit < p1);

    p1 = x_hi*y_hi;
        tmp_digit += p2;
        carry += (tmp_digit < p2);
        P_UX_MSD(lo, tmp_digit);

    UMULH(y_hi, x_lo, p2);
        tmp_digit = p1 + carry;
        carry = (tmp_digit < p1);

    UMULH(y_lo, x_hi, p1);
        tmp_digit += p2;
        carry += (tmp_digit < p2);

    UMULH(y_hi, x_hi, p2);
        tmp_digit += p1;
        carry += (tmp_digit < p1);
        P_UX_LSD(hi, tmp_digit);

    tmp_digit = p2 + carry;
    P_UX_MSD(hi, tmp_digit);
    }

/*
** This routine divides two unpacked numbers:
**
**	o The 'flags' argument controls whether a FULL or HALF precision
**	  result is generated.
**	o If the pointer to one of the unpacked results is 0, then that
**	  argument is implicitly treated as being equal to 1.
**	o both argument pointers *CANNOT* be zero.
*
** A detailed description of the algorithm is presented in note 6.2 of the
** X_FLOAT notes conference.  Note that to the extent possible, the variable
** names in this routine were chosen to match the description in the design
** note.  In particular, upper case name imply 64 bit integer data types, while
** double precision values are denoted with lower case names.
*/

#define _D_POW_2(n)	((double) ((U_WORD)1 << n))

#define       TWO_POW_62  (_D_POW_2(62))
#define       TWO_POW_124 (TWO_POW_62*TWO_POW_62)
#define RECIP_TWO_POW_16  (1./_D_POW_2(16))
#define RECIP_TWO_POW_60  (1./_D_POW_2(60))
#define RECIP_TWO_POW_184 (4./(TWO_POW_124 * TWO_POW_62 ))


static const UX_FLOAT __ux_one__ = { 0, 1, ((U_WORD) 1 << 63), 0 };
	
void
DIVIDE( UX_FLOAT * aPtr, UX_FLOAT * bPtr, U_WORD flags, UX_FLOAT * cPtr)
    {
    UX_EXPONENT_TYPE exponent;

    UX_FRACTION_DIGIT_TYPE A1, A2, B1, B2, Q1, Q2, S, R, P00, P01, P11,
                           N0, N1, N2, C1, mask, E;
    D_TYPE r, b_hi, b_lo, r_hi, r_lo, a_hi, a_lo, a, q_hi, q_lo;

    /*
    ** for performance reasons, pre-load some of the interesting items even
    ** though we might not actually use them.  Specifically, by loading B1
    ** and B2 before the normalization check allows the compiler to better
    ** schedule the code after the check.
    */

    bPtr = (bPtr == 0) ? (UX_FLOAT *)&__ux_one__ : bPtr;	
    aPtr = (aPtr == 0) ? (UX_FLOAT *)&__ux_one__ : aPtr;
    B1 = G_UX_MSD(bPtr);
    B2 = G_UX_LSD(bPtr);

    if (bPtr == &__ux_one__)
        {
        UX_COPY(aPtr, cPtr);
        return;
        }

    /*
    ** If b isn't normalized, then the whole algorithm falls apart.  So make
    ** sure that b is normalized.
    */

    if ((UX_SIGNED_FRACTION_DIGIT_TYPE) B1 >= 0)
        {
        NORMALIZE(bPtr);
        B1 = G_UX_MSD(bPtr);
        B2 = G_UX_LSD(bPtr);
        }

    /*
    ** The first step is to estimate 1/b in double precsion to more then 70
    ** bits. This is done by getting an initial estimate to 1/b and use a 
    ** variation of Newton's iteration to improve the accuracy.  The basic
    ** approach is
    **
    **		b'    = high 53 bits of b
    **		b_hi' = high 26 bits of b
    **		b_lo' = bits 27 through 80 of b
    **
    **		r'    = 1/b'
    **		r_hi' = high 26 bits of r
    **		r_lo' = [ (1 - b_hi'*r_hi') - b_lo'*r_hi'] * r'
    **
    ** However, there is certain amount of weird scaling of the values that
    ** takes place to deal with the integer to float conversion and subsequent
    ** uses of the results.
    **
    ** Note that the two macros below are used to convert *signed* integers
    ** to and from double precision.  We use signed conversions because they
    ** are generally faster than unsigned conversions.
    */

#   define TO_DOUBLE(a) ((double) ((UX_SIGNED_FRACTION_DIGIT_TYPE) (a)))
#   define TO_DIGIT(a)  ((UX_SIGNED_FRACTION_DIGIT_TYPE) (a))

    r = TWO_POW_124 / TO_DOUBLE( B1 >> 1 );

        /*
        ** While the divide is going on, we can compute all sorts of stuff
        */

        mask = MAKE_MASK( 38, 0 );

        b_hi = TO_DOUBLE((B1 & ~mask) >> 1);
        b_lo = RECIP_TWO_POW_16 * TO_DOUBLE(((B1 & mask) << 15) | (B2 >> 49));

        A1 = G_UX_MSD(aPtr);
        A2 = G_UX_LSD(aPtr);

        P_UX_SIGN(    cPtr, G_UX_SIGN(aPtr) ^ G_UX_SIGN(bPtr) );
        exponent = G_UX_EXPONENT(aPtr) - G_UX_EXPONENT(bPtr);

    /*
    ** Get the high part of r as both an integer and a floating point value.
    ** In the process, bias r_hi downward to insure that r_lo is positive.
    ** (See the design note for details.)
    */

    R = TO_DIGIT( r );
    R = (R - (5 << 8)) & ~MAKE_MASK( 36, 0 );
    r_hi = TO_DOUBLE(R);

    /*
    ** At this point we have:
    **
    ** 		r    = 2^61 * r'
    **		r_hi = 2^61 * r_hi'
    **		b_hi = 2^63 * b_hi'
    **		b_lo = 2^63 * b_lo'
    **
    ** so that
    **
    **		2*r_lo' = [ (2^124 - b_hi*r_hi) - b_lo*r_hi ] * (r/2^184)
    */

    r_lo = D_GROUP(D_GROUP((TWO_POW_124) - b_hi*r_hi) - (b_lo*r_hi)) * (RECIP_TWO_POW_184*r);

    /*
    ** Now that we have 1/b ~ r_hi' + r_lo' (scaling notwithstanding), we can
    ** compute an approximation to q = a/b = a*(1/b), where the product is
    ** performed in high and low pieces:
    **
    **		q = (a_hi' + a_lo') * (r_hi' + r_lo')
    **		  = a_hi' * r_hi' + [ a_lo' * r_hi' + (a_hi' + a_lo') * r_lo' ]
    **		  = a_hi' * r_hi' + [ a_lo' * r_hi' + a' * r_lo' ]
    **		  = q_hi' + q_lo'
    **
    ** Note that in the above, we want to insure that a' ~ a_hi' + a_lo' is
    ** less than the actual value of a to insure that the computed value of
    ** q is less that 2.
    */

    a    = TO_DOUBLE( (A1 >> 11) << 10 );
    a_hi = TO_DOUBLE( (A1 & ~mask) >> 1);
    a_lo = RECIP_TWO_POW_16 * TO_DOUBLE(((A1 & mask) << 15) | (A2 >> 49));

    r_hi = RECIP_TWO_POW_60 * r_hi;
    q_hi = a_hi*r_hi;
    q_lo = a_lo*r_hi + a*r_lo;

    /*
    ** With the above conversions and computations we have
    **
    **		a    = 2^63*a'
    **		a_hi = 2^63*a_hi'
    **		a_lo = 2^63*a_lo'
    **		r_hi = 2*r_hi'
    **		r_lo = 2*r_lo'
    **		q_hi = 2^64 * q_hi'
    **		q_lo = 2^64 * q_lo'
    **
    ** We would like to convert the high 65 bits of q_hi + q_lo into integers,
    ** S' and Q1'.  Note that converting q_hi to an integer can cause an
    ** overflow.  However since q_hi contains only 52 significant bits, we
    ** can convert .25 * q_hi instead which won't overflow.
    */

    Q1 = TO_DIGIT(.25 * q_hi);
    E  = TO_DIGIT( q_lo );

    S = ( Q1 >> 62 );
    Q1 = (4*Q1) + E;
    S += (Q1 < E);
    Q2 = 0;

    if (flags == HALF_PRECISION) goto pack_it;

    /*
    ** While we're at it, compute an integer approximation to 1/b.  I.e. get
    ** and integer R such that R/2^63 ~ 1/b.
    **
    **		 R = 2^63 * (r_hi' + r_lo' )
    **		   = 2^63 * r_hi' + 2^63 * r_lo'
    **		   = 2^63 * r_hi' + 2^62 * r_lo
    **
    ** Recall that in the original computation of r_hi, we previously computed
    ** the integer value R as 2^61*r_hi', so that we can now compute
    ** 
    **		R <-- 4*R + 2^62 * r_lo 
    **
    ** Note that for b very close to 1/2, R will be 2^64 which can't be 
    ** represented in 64 bits.  In this case, we take R = 2^64 - 1 which is
    ** close enough and can be represented in 64 bits.
    */

    R = (R << 2) + TO_DIGIT( TWO_POW_62*r_lo );
    R = ( R == 0 ) ? ( (UX_SIGNED_FRACTION_DIGIT_TYPE) -1 ) : R;

    /*
    ** Using S and Q1 as the current guess for the high 65 bits of the result
    ** compute the remainder:
    **
    **             +----------+----------+
    **             |    A1    |    A2    |                2^128*(2^64*A1 + A2)
    **             +----------+----------+
    **
    **             +----------+----------+
    **             |    B1    |    B2    |              s'*2^128*(2^64*B1 + B2)
    **             +----------+----------+
    **             |       Q1'*B1        |                2^128*Q1'*B1
    **             +----------+----------+----------+
    **                        |       Q1'*B2        |      2^64*Q1'*B2
    **                        +----------+----------+
    **
    **  +----------+----------+----------+----------+
    **  |    N0'   |    N1'   |   N2'    |   N3'    |
    **  +----------+----------+----------+----------+
    **
    ** Start by summing all the products into N0:N1:N2:N3
    **
    **		NOTE: for performance reasons, we don't actually
    **		compute N3'
    */

    mask = -S;

    UMULH( Q1, B2, P11 );
    P01 = Q1 * B1;
    UMULH( Q1, B1, P00 );

    N2 = B2 & mask;	/* N2/N1 = B2/B1 if S = 1, 0 otherwise */
    N1 = B1 & mask;

    N2 += P11;
    C1 =  (N2 < P11);
    N2 += P01;
    C1 += (N2 < P01);

    N1 += P00;
    N0 =  (N1 < P00);
    N1 += C1;
    N0 += (N1 < C1);

    /* Subtract the sum from A1:A2 */

    N0 = -N0;
    C1 = (A2 < N2);
    N2 = A2 - N2;
    N0 -= (A1 < N1);
    N1 = A1 - N1;
    N0 -= (N1 < C1);
    N1 -= C1;

    /*
    ** Since the original estimate to S:Q1 was good to more then 70 bits, the
    ** current value of S:Q1 can be off by at most one.  By looking at the
    ** values of N0 and N1, we can determine an adjustment, E, to S:Q1.
    ** With the adjusted S:Q1 we know that N0 = N1 = 0, so we only need to
    ** adjust N2.
    */

    E = (N0 | (N1 != 0));
    mask = (E == 0) ? B1 : N0;
    N2 = N2 - (mask ^ B1);

    /*
    ** Using R/2^63 ~ 1/b and the adjusted N2, compute an approximation to Q2
    ** Note that if Q2 has it's high bit set, then the original value of E was
    ** one too low.
    */

    UMULH( R, N2, Q2 );

    E += ( ( (UX_SIGNED_FRACTION_DIGIT_TYPE) Q2 ) < 0);
    Q2 = 2*Q2 + ((A1 | A2) != 0);	/* Make sure 0/b is zero */

    /* Adjust S and Q1 using the final value of E */

    Q1 += E;
    S  =  S + (((UX_SIGNED_FRACTION_DIGIT_TYPE) E) >> 63) + (Q1 < E);

    /* Last but not least, pack it */

pack_it:

    P_UX_MSD(     cPtr,        (S << 63) | (Q1 >> S) );
    P_UX_LSD(     cPtr, ((Q1 & S) << 63) | (Q2 >> S) );
    P_UX_EXPONENT(cPtr, exponent + S);

    return;
    }

    
/*
**
** The following two routines evaluate polynomials, P(x), via Horner's
** scheme for positive x:
**
**		s(k) <-- c(k) +/- x*s(k+1)  for k = n-1, ..., 0
**
** where the c(k)'s are the polynomial coefficients and	s(n) = c(n). The
** arguments to these routines (not in order) are
**
**	x		a pointer to the unpacked bits of x
**	cnt		the degree of the polynomial
**	coef		A pointer to pairs of quadwords specifying the hi/lo
**			bits of the coefficient.  We assume the coefficients
**			are stored reverse order: c(n) to c(0)
**	shift		cnt*(x->exp) - This is passed in rather than computed
**			here sense on the calling side, cnt is a known
**			constant, so the multiply can be done by shifts and
**			adds rather than a real integer multiply.
**	p		a pointer to the unpacked result.
**
** The routines return the high bits of the result.
**
** IMPORTANT ASSUMPTIONS:
** ######################
**
**	o This routine assumes that the terms of the polynomial are decreasing.
**	  I.e. that c(k) > x*s(k+1) for all k.
**
**	o shift = cnt*(x->exp), so that if shift is decremented by x->exp
**	  each time cnt decremented, then shift will become 0 before cnt
**	  becomes negative.
*/

static void
__eval_pos_poly(UX_FLOAT * x, WORD shift, FIXED_128 * coef, WORD cnt,
  UX_FLOAT * p)
    {
    UX_FRACTION_DIGIT_TYPE c_hi, c_lo, s_hi, s_lo, p1, p2;
    UX_FRACTION_DIGIT_TYPE x_hi, x_lo, carry;
    UX_EXPONENT_TYPE exponent;
    WORD shift_inc;

    /* Initialize internal copies and accumulators */

    x_hi = G_UX_MSD(x);
    x_lo = G_UX_LSD(x);
    shift_inc = G_UX_EXPONENT(x);
    s_lo = s_hi = 0;


    /*
    ** If the shift count is >= 128, than this product won't contribute to
    ** the final product.  Skip over all of the coefficients that correspond
    ** to large shifts
    */

    if (shift < 128) goto p_check_shift_64_to_127;

    p_shift_ge_128:
        shift += shift_inc;
        coef++;
        cnt--;
        if (shift >= 128) goto p_shift_ge_128;
//printf("Eval_pos_poly, shift=%lld !!\n",shift);

    /*
    ** Each time through this loop, c_hi = 0.  Since we assume that c(k) >
    ** x*s(k+1), if there is a carry out on the sum s(k) = c(k) + x*s(k*1),
    ** then the shift count for the next iteration must be less than 64.
    ** Consequently, we need only worry about the carry out from the sum
    ** when we leave this loop.  That means each time we enter the top of
    ** the loop, both c_hi and s_hi = 0;
    */

    p_check_shift_64_to_127:
        if (shift < 64) goto p_check_shift_1_to_63;

    /*
    ** Depending on the size of shift_inc and the rate at which the
    ** coefficients decrease, several of the next Horner's scheme iterations
    ** will yield zero results, so there is no need to do the multiply.
    ** Since multiplies are likely to be expensive, we check for this case
    ** and skip over them.
    */

    if (s_lo) goto p_shift_64_to_127;

    p_shift_64_to_127_zero_loop:
        s_lo = coef->digits[1] >> (shift - 64);
 	//printf("s_lo, sh, sh_inc, c: %llx, %llx, %llx, %llx (%llx)\n",s_lo,shift, shift_inc,coef->digits[1],coef->digits[0]);
       shift += shift_inc;
        coef++;
        cnt--;
        if (shift < 64) goto p_check_shift_1_to_63;
        if (s_lo == 0) goto p_shift_64_to_127_zero_loop;

    /*
    ** s_lo is no longer zero, so do the multiply and accumulate the
    ** products.
    */

p_shift_64_to_127:
		//printf("s_lo,x_hi,p1: %llx, %llx, %llx\n",s_lo,x_hi,p1);
        UMULH(s_lo, x_hi, p1);
		//printf("s_lo,x_hi,p1: %llx, %llx, %llx\n",s_lo,x_hi,p1);
            c_lo = coef->digits[1] >> (shift - 64);
            shift += shift_inc;
            coef++;
            cnt--;
        s_lo = c_lo + p1;
        if (shift >= 64) goto p_shift_64_to_127;

    /* Set carry out from last add */
    s_hi = (s_lo < p1);
        
    /* 
    ** When shift = 0, the complementary shift is 64.  ANSI C does not
    ** specify the result of a shift by 64, so we need to handle this as
    ** a special case.
    */

    p_check_shift_1_to_63:
        exponent = 0;
        if (shift == 0) goto p_shift_eq_0;

    /*
    ** Depending on the size of shift_inc and the rate at which the
    ** coefficients decrease, several of the next Horner's scheme iterations
    ** will yield zero results for s_hi, so there is no need to do the
    ** multiplies associated with s_hi.  Since multiplies are likely to be
    ** expensive, we check for this case and skip over them.
    */

    if (s_hi) goto p_shift_1_to_63;

    p_shift_1_to_63_zero_loop:
        UMULH(s_lo, x_hi, p1);
            c_hi = coef->digits[1];
            c_lo = coef->digits[0];
            c_lo = (c_lo >> shift) | (c_hi << (64 - shift));
            s_hi = c_hi >> shift;
            shift += shift_inc;
            coef++;
            cnt--;
        s_lo = c_lo + p1;
        s_hi += (s_lo < p1);
        if (shift == 0) goto p_shift_eq_0;
        if (s_hi == 0) goto p_shift_1_to_63_zero_loop;

    p_shift_1_to_63:

    while (cnt >= 0)
        {
        p1 = s_hi*x_hi;
            c_hi = coef->digits[1];
            c_lo = coef->digits[0];
            c_lo = (c_lo >> shift) | (c_hi << (64 - shift));
            c_hi >>= shift;

        UMULH(s_hi, x_lo, p2);
            c_lo += p1;
            carry = (c_lo < p1);
            cnt--;

        UMULH(s_lo, x_hi, p1);
            c_lo += p2;
            carry += (c_lo < p2);
            shift += shift_inc;

        UMULH(s_hi, x_hi, p2);
            s_lo = c_lo + p1;
            carry += (s_lo < p1);
            c_hi += carry;
            carry = (c_hi < carry);
            coef++;

        s_hi = c_hi + p2;
        carry += (s_hi < p2);
        if (carry)
            {
            s_lo = (s_lo >> 1) | (s_hi << 63);
            s_hi = (s_hi >> 1) | SET_BIT(63);
            shift++;
            exponent++;
            }
        if (shift == 0) break;
        }


    p_shift_eq_0:

    while (cnt >= 0)
        {
        p1 = s_hi*x_hi;
            c_hi = coef->digits[1];
            c_lo = coef->digits[0];

        UMULH(s_hi, x_lo, p2);
            c_lo += p1;
            carry = (c_lo < p1);
            cnt--;

        UMULH(s_lo, x_hi, p1);
            c_lo += p2;
            carry += (c_lo < p2);

        UMULH(s_hi, x_hi, p2);
            s_lo = c_lo + p1;
            carry += (s_lo < p1);
            c_hi += carry;
            carry = (c_hi < carry);
            coef++;

        s_hi = c_hi + p2;
        carry += (s_hi < p2);
        if (carry)
            {
            s_lo = (s_lo >> 1) | (s_hi << 63);
            s_hi = (s_hi >> 1) | SET_BIT(63);
            shift = 1;
            exponent++;
            if (cnt >= 0)
                goto p_shift_1_to_63;
            }
        }
        
    P_UX_LSD(p, s_lo);
    P_UX_MSD(p, s_hi);
    P_UX_EXPONENT(p, exponent);
    P_UX_SIGN(p, 0);
    }

static void
__eval_neg_poly(UX_FLOAT * x, WORD shift, FIXED_128 * coef, WORD cnt,
  UX_FLOAT * p)
    {
    UX_FRACTION_DIGIT_TYPE c_hi, c_lo, s_hi, s_lo, p1, p2, tmp;
    UX_FRACTION_DIGIT_TYPE x_hi, x_lo;
    WORD shift_inc;

    x_hi = G_UX_MSD(x);
    x_lo = G_UX_LSD(x);
    shift_inc = G_UX_EXPONENT(x);

    s_lo = s_hi = 0;
    if (shift < 128) goto n_check_shift_64_to_127;

    /* Skip over all the big shifts */

    n_shift_ge_128:
        shift += shift_inc;
        coef++;
        cnt--;
        if (shift >= 128) goto n_shift_ge_128;

    /*
     * Each time through this loop, c_hi = 0.  Since we assume that c(k) >
     * x*s(k+1), s(k) = c(k) - x*s(k*1) < c(k).  Consequently, there is
     * no borrow from the computation of s(k) into it high 64 bits.
     * That means each time we enter the top of the loop, both c_hi and
     * s_hi = 0;
     */

    n_check_shift_64_to_127:
        if (shift < 64) goto n_check_shift_1_to_63;

    /*
     * Depending on the size of shift_inc and the rate at which the
     * coefficients decrease, several of the next Horner's scheme iterations
     * will yield zero results, so there is no need to do the multiply.
     * Since multiplies are likely to be expensive, we check for this case
     * and skip over them.
     */

    if (s_lo) goto n_shift_64_to_127;

    n_shift_64_to_127_zero_loop:
        s_lo = coef->digits[1] >> (shift - 64);
        shift += shift_inc;
        coef++;
        cnt--;
        if (shift < 64) goto n_check_shift_1_to_63;
        if (s_lo == 0) goto n_shift_64_to_127_zero_loop;

    /*
     * s_lo is no longer zero, so do the multiply and accumulate the
     * products.
     */

    n_shift_64_to_127:
        UMULH(s_lo, x_hi, p1);
            c_lo = coef->digits[1] >> (shift - 64);
            shift += shift_inc;
            coef++;
            cnt--;
        s_lo = c_lo - p1;
        if (shift >= 64) goto n_shift_64_to_127;

    /* 
     * When shift = 0, the complementary shift is 64.  ANSI C does not
     * specify the result of a shift by 64, so we need to handle this as
     * a special case.
     */

    n_check_shift_1_to_63:
        if (shift == 0) goto n_shift_eq_0;

    /*
     * Depending on the size of shift_inc and the rate at which the
     * coefficients decrease, several of the next Horner's scheme iterations
     * will yield zero results for s_hi, so there is no need to do the
     * multiplies associated with s_hi.  Since multiplies are likely to be
     * expensive, we check for this case and skip over them.
     */

    if (s_hi) goto n_shift_1_to_63;

    n_shift_1_to_63_zero_loop:
        UMULH(s_lo, x_hi, p1);
            c_hi = coef->digits[1];
            c_lo = coef->digits[0];
            c_lo = (c_lo >> shift) | (c_hi << (64 - shift));
            s_hi = (c_hi >> shift);
            shift += shift_inc;
            coef++;
            cnt--;
        s_lo = c_lo - p1;
        s_hi -= (s_lo > c_lo);
        if (shift == 0) goto n_shift_eq_0;
        if (s_hi == 0) goto n_shift_1_to_63_zero_loop;

    n_shift_1_to_63:
        p1 = s_hi*x_hi;
            c_hi = coef->digits[1];
            c_lo = coef->digits[0];
            c_lo = (c_lo >> shift) | (c_hi << (64 - shift));
            c_hi >>= shift;

        UMULH(s_hi, x_lo, p2);
            tmp = c_lo - p1;
            c_hi -= (tmp > c_lo);
            cnt--;

        UMULH(s_lo, x_hi, p1);
            c_lo = tmp - p2;
            c_hi -= (c_lo > tmp);
            shift += shift_inc;

        UMULH(s_hi, x_hi, p2);
            s_lo = c_lo - p1;
            c_hi -= (s_lo > c_lo);
            coef++;

        s_hi = c_hi - p2;
        if (shift) goto n_shift_1_to_63;


    n_shift_eq_0:

    while (cnt >= 0)
        {
        p1 = s_hi*x_hi;
            c_hi = coef->digits[1];
            c_lo = coef->digits[0];

        UMULH(s_hi, x_lo, p2);
            tmp = c_lo - p1;
            c_hi -= (tmp > c_lo);
            cnt--;

        UMULH(s_lo, x_hi, p1);
            c_lo = tmp - p2;
            c_hi -= (c_lo > tmp);

        UMULH(s_hi, x_hi, p2);
            s_lo = c_lo - p1;
            c_hi -= (s_lo > c_lo);
            coef++;

        s_hi = c_hi - p2;
        }
    P_UX_LSD(p, s_lo);
    P_UX_MSD(p, s_hi);
    P_UX_EXPONENT(p, 0);
    P_UX_SIGN(p, 0);
    }


/*
** EVALUATE_RATIONAL is a driver routine for the two polynomial evaluation
** routines.  Even though it is architecture and word size independent, it
** is included in this file to increase "locality".
**
** EVALUATE_RATIONAL generally computes a rational approximation, however,
** by specifying the appropriate set of flags, one, or two polynomial
** evaluation can be performed.
**
** The following flags are used to independently control the "form" of the
** numerator and denominator polynomials:
**
**		SQUARE_TERM
**		ALTERNATE_SIGN
**		POST_MULTIPLY
**		STANDARD
**
** The following flags control whether or not a rational approximation is
** performed and what form it has:
**
**		SWAP
**		SKIP
**		NO_DIVIDE
**
** If the SKIP flag is specified in conjunction with the flags for either
** the numerator or denominator being zero, only one part of a rational
** will be evaluated.
*/

#define EITHER(n)	   (DENOMINATOR_FLAGS(n) | NUMERATOR_FLAGS(n))
#define NUMERATOR_MASK	   NUMERATOR_FLAGS(MAKE_MASK(NUM_DEN_FIELD_WIDTH, 0))
#define DENOMINATOR_MASK   DENOMINATOR_FLAGS(MAKE_MASK(NUM_DEN_FIELD_WIDTH, 0))

#define UPDATE_COEF_PTR(c,d)	(c) = ((FIXED_128 *)((char *) (c) + (d)))
#define G_EXPONENT(c)		((UX_EXPONENT_TYPE) ((WORD *) (c))[-1])


void
EVALUATE_RATIONAL(
  UX_FLOAT  * argument,
  FIXED_128 * coefficients,
  U_WORD      degree,
  U_WORD      flags,
  UX_FLOAT  * result)
    {
    WORD tmp;
    WORD sign, shift, byte_length, poly_shift;
    UX_EXPONENT_TYPE exponent;
    UX_FLOAT * first_result, *second_result, arg_squared, *poly_arg;
    void (* poly_func)(UX_FLOAT *, WORD, FIXED_128 *, WORD, UX_FLOAT *);

    /* Scale argument and squared it if its needed */

    sign = flags;
    UX_INCR_EXPONENT(argument, G_SCALE(flags));
    if (flags & EITHER(SQUARE_TERM))
        {
        poly_arg = &arg_squared;
        MULTIPLY(argument, argument, &arg_squared);
        }
    else
        {
        poly_arg = argument;
        tmp = G_UX_SIGN(argument) ? EITHER(ALTERNATE_SIGN) : 0;
        sign = flags ^ tmp;
        }

    /* Start calculation of shift parameter. */

    NORMALIZE(poly_arg);
    exponent = G_UX_EXPONENT(poly_arg);
    P_UX_EXPONENT(poly_arg, exponent);
    shift = -degree*exponent;
    byte_length = (degree + 1)*sizeof(FIXED_128) + sizeof(WORD);

    /* allocate locations for 1st and 2nd result */

    tmp = (((flags & SWAP) == 0) || (flags & SKIP)) ? 0 : 1;
    first_result  = result + tmp;
    second_result = result + 1 - tmp;

    if (NUMERATOR_MASK & flags)
        {
//printf("NUMERATOR_MASK !!\n");
        poly_func =  (ALTERNATE_SIGN & sign) ? __eval_neg_poly :
            __eval_pos_poly;

        first_result = (DENOMINATOR_MASK & flags) ? first_result : result;

        poly_func(
            poly_arg,
            shift,
	    coefficients,
	    degree,
	    first_result);
 		//printf("f_result= (%x %x) %llx %llx\n",first_result->sign,first_result->exponent,first_result->fraction[0],first_result->fraction[1]);

 //printf("fl & NUMERATOR_FLAGS(POST_MULTIPLY) = %llx (%llx)\n", flags & NUMERATOR_FLAGS(POST_MULTIPLY), flags); 
        if (flags & NUMERATOR_FLAGS(POST_MULTIPLY))
            MULTIPLY(argument, first_result, first_result);
 		//printf("result..= (%x %x) %llx %llx\n",result->sign,result->exponent,result->fraction[0],result->fraction[1]);

        UPDATE_COEF_PTR(coefficients, byte_length);
        UX_INCR_EXPONENT(first_result, G_EXPONENT(coefficients));
        }
    else
        {
        second_result = result;
        flags |= NO_DIVIDE;
        if ( flags & SKIP )
            UPDATE_COEF_PTR(coefficients, byte_length);
        }


    if (DENOMINATOR_MASK & flags)
        {
 //printf("DENOMINATOR_MASK !!\n");
       poly_func = ( DENOMINATOR_FLAGS(ALTERNATE_SIGN) & sign ) ?
            __eval_neg_poly : __eval_pos_poly;

        poly_func(
            poly_arg,
            shift,
	    coefficients,
	    degree,
	    second_result);

        if (flags & DENOMINATOR_FLAGS(POST_MULTIPLY))
            MULTIPLY(argument, second_result, second_result);

        UPDATE_COEF_PTR(coefficients, byte_length);
        UX_INCR_EXPONENT(second_result, G_EXPONENT(coefficients));

        if ( flags & SKIP )
            /* Numerator was skipped, we're done */
            return;
        }
    else
        {
        flags |= NO_DIVIDE;
        if ( flags & SKIP )
            UPDATE_COEF_PTR(coefficients, byte_length);
        }

 //printf("fl & NO_DIV = %llx\n", flags & NO_DIVIDE);
 		//printf("result0= (%x %x) %llx %llx\n",result->sign,result->exponent,result->fraction[0],result->fraction[1]);

    if ((flags & NO_DIVIDE) == 0)
        DIVIDE(result, result + 1, FULL_PRECISION, result);
    }


#if 0
U_INT_64 __umulh( U_INT_64 i, U_INT_64 j ) {
    U_INT_64 k;
        {
	U_INT_64 iLo, iHi, jLo, jHi, p0, p1, p2; 
        iLo = __LO(i); iHi = __HI(i);
        jLo = __LO(j); jHi = __HI(j);
	p0  = iLo * jLo;
	p1  = (iLo * jHi);
	p2  = (iHi * jLo) + __HI(p0) + __LO(p1);\
        k   = (iHi * jHi) + __HI(p1) + __HI(p2);
	}
    return k;
}
#endif
