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

/*****************************************************************************
 *    BID32 divide
 *****************************************************************************
 *
 *  Algorithm description:
 *
 *  if(coefficient_x<coefficient_y)
 *    p = number_digits(coefficient_y) - number_digits(coefficient_x)
 *    A = coefficient_x*10^p
 *    B = coefficient_y
 *    CA= A*10^(15+j), j=0 for A>=B, 1 otherwise
 *    Q = 0
 *  else
 *    get Q=(int)(coefficient_x/coefficient_y) 
 *        (based on double precision divide)
 *    check for exact divide case
 *    Let R = coefficient_x - Q*coefficient_y
 *    Let m=16-number_digits(Q)
 *    CA=R*10^m, Q=Q*10^m
 *    B = coefficient_y
 *  endif
 *    if (CA<2^64)
 *      Q += CA/B  (64-bit unsigned divide)
 *    else 
 *      get final Q using double precision divide, followed by 3 integer 
 *          iterations
 *    if exact result, eliminate trailing zeros
 *    check for underflow
 *    round coefficient to nearest
 *
 ****************************************************************************/

#define BID_FUNCTION_SETS_BINARY_FLAGS
#include "bid_internal.h"
#include "bid_div_macros.h"

BID_EXTERN_C const BID_UINT32 bid_convert_table[5][128][2];
BID_EXTERN_C const BID_SINT8 bid_factors[][2];
BID_EXTERN_C const BID_UINT8 bid_packed_10000_zeros[];


BID_TYPE_FUNCTION_ARG2(BID_UINT32, bid32_div, x, y)

BID_UINT64 CA, CT, PD;
  BID_UINT32 sign_x, sign_y, coefficient_x, coefficient_y, A, B;
  BID_UINT32 Q, Q2, B2, B4, B5, R, T, DU, res;
  BID_UINT32 valid_x, valid_y;
  BID_SINT32 D;
  int_float tempx, tempy, tempq;
  int exponent_x, exponent_y, bin_expon_cx;
  int diff_expon, ed1, ed2, bin_index;
  int rmode, amount;
  int nzeros, i, j, d5;
  BID_UINT32 digit_h, digit_low;

  BID_OPT_SAVE_BINARY_FLAGS()

  valid_x = unpack_BID32 (&sign_x, &exponent_x, &coefficient_x, x);
  valid_y = unpack_BID32 (&sign_y, &exponent_y, &coefficient_y, y);

  // unpack arguments, check for NaN or Infinity
  if (!valid_x) {
    // x is Inf. or NaN
#ifdef BID_SET_STATUS_FLAGS
    if ((y & SNAN_MASK32) == SNAN_MASK32)	// y is sNaN
      __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif

    // test if x is NaN
    if ((x & NAN_MASK32) == NAN_MASK32) {
#ifdef BID_SET_STATUS_FLAGS
      if ((x & SNAN_MASK32) == SNAN_MASK32)	// sNaN
	__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      BID_RETURN (coefficient_x & QUIET_MASK32);
    }
    // x is Infinity?
    if ((x & INFINITY_MASK32) == INFINITY_MASK32) {
      // check if y is Inf or NaN
      if ((y & INFINITY_MASK32) == INFINITY_MASK32) {
	// y==Inf, return NaN 
	if ((y & NAN_MASK32) == INFINITY_MASK32) {	// Inf/Inf
#ifdef BID_SET_STATUS_FLAGS
	  __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
	  BID_RETURN (NAN_MASK32);
	}
      } else {
	// otherwise return +/-Inf
	BID_RETURN (((x ^ y) & 0x80000000) |
		    INFINITY_MASK32);
      }
    }
    // x==0
    if (((y & INFINITY_MASK32) != INFINITY_MASK32)
	&& !(coefficient_y)) {
      // y==0 , return NaN
#ifdef BID_SET_STATUS_FLAGS
      __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      BID_RETURN (NAN_MASK32);
    }
    if (((y & INFINITY_MASK32) != INFINITY_MASK32)) {
      if ((y & SPECIAL_ENCODING_MASK32) == SPECIAL_ENCODING_MASK32)
	exponent_y = ((BID_UINT32) (y >> 21)) & 0xff;
      else
	exponent_y = ((BID_UINT32) (y >> 23)) & 0xff;
      sign_y = y & 0x80000000;

      exponent_x = exponent_x - exponent_y + DECIMAL_EXPONENT_BIAS_32;
      if (exponent_x > DECIMAL_MAX_EXPON_32)
	exponent_x = DECIMAL_MAX_EXPON_32;
      else if (exponent_x < 0)
	exponent_x = 0;
      BID_RETURN ((sign_x ^ sign_y) | (((BID_UINT64) exponent_x) << 23));
    }

  }
  if (!valid_y) {
    // y is Inf. or NaN

    // test if y is NaN
    if ((y & NAN_MASK32) == NAN_MASK32) {
#ifdef BID_SET_STATUS_FLAGS
      if ((y & SNAN_MASK32) == SNAN_MASK32)	// sNaN
	__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      BID_RETURN (coefficient_y & QUIET_MASK32);
    }
    // y is Infinity?
    if ((y & INFINITY_MASK32) == INFINITY_MASK32) {
      // return +/-0
      BID_RETURN (((x ^ y) & 0x80000000));
    }
    // y is 0
#ifdef BID_SET_STATUS_FLAGS
    __set_status_flags (pfpsf, BID_ZERO_DIVIDE_EXCEPTION);
#endif
    BID_RETURN ((sign_x ^ sign_y) | INFINITY_MASK32);
  }
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
  // (void) fegetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
  diff_expon = exponent_x - exponent_y + DECIMAL_EXPONENT_BIAS_32;

  if (coefficient_x < coefficient_y) {

    // get number of decimal digits for c_x, c_y

    //--- get number of bits in the coefficients of x and y ---
    tempx.d = (float) coefficient_x;
    tempy.d = (float) coefficient_y;
    bin_index = (tempy.i - tempx.i) >> 23;

    A = coefficient_x * (BID_UINT32)bid_power10_index_binexp[bin_index];
    B = coefficient_y;

    // compare A, B
    DU = (A - B) >> 31;
    ed1 = 6 + (int) DU;
    ed2 = bid_estimate_decimal_digits[bin_index] + ed1;
    T = bid_power10_table_128[ed1].w[0];
    CA = ((BID_UINT64)A) * T;

    Q = 0;
    diff_expon = diff_expon - ed2;

  } else {
    // get c_x/c_y

    Q = coefficient_x/coefficient_y;

    R = coefficient_x - coefficient_y * Q;

    // will use to get number of dec. digits of Q
	tempq.d = (float)Q;
    bin_expon_cx = (tempq.i >> 23) - 0x7f;

    // exact result ?
    if (R == 0) {
      res =
	get_BID32 (sign_x ^ sign_y, diff_expon, Q, rnd_mode,
		   pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
      // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
      BID_RETURN (res);
    }
    // get decimal digits of Q
    DU = (BID_UINT32)bid_power10_index_binexp[bin_expon_cx] - Q - 1;
    DU >>= 31;

    ed2 = 7 - bid_estimate_decimal_digits[bin_expon_cx] - (int) DU;

    T = bid_power10_table_128[ed2].w[0];
	CA = ((BID_UINT64)R) * T;
    B = coefficient_y;

    Q *= (BID_UINT32)bid_power10_table_128[ed2].w[0];
    diff_expon -= ed2;

  }

    Q2 = CA / B;
    B2 = B + B;
    B4 = B2 + B2;
    R = CA - Q2 * B;
    Q += Q2;
   
#ifdef BID_SET_STATUS_FLAGS
  if (R) {
    // set status flags
    __set_status_flags (pfpsf, BID_INEXACT_EXCEPTION);
//printf("ZZZ R=%x, %x %x\n",R, (BID_UINT32)pfpsf, *pfpsf);
  }
#ifndef LEAVE_TRAILING_ZEROS
  else
#endif
#else
#ifndef LEAVE_TRAILING_ZEROS
  if (!R)
#endif
#endif
#ifndef LEAVE_TRAILING_ZEROS
  {
    // eliminate trailing zeros

    // check whether CX, CY are short
    if ((coefficient_x <= 1024) && (coefficient_y <= 1024)) {
      i = (int) coefficient_y - 1;
      j = (int) coefficient_x - 1;
      // difference in powers of 2 bid_factors for Y and X
      nzeros = ed2 - bid_factors[i][0] + bid_factors[j][0];
      // difference in powers of 5 bid_factors
      d5 = ed2 - bid_factors[i][1] + bid_factors[j][1];
      if (d5 < nzeros)
	nzeros = d5;

	  if(nzeros) {
      CT = ((BID_UINT64)Q) * bid_bid_reciprocals10_32[nzeros];
	  CT >>= 32;

      // now get P/10^extra_digits: shift C64 right by M[extra_digits]-128
      amount = bid_bid_bid_recip_scale32[nzeros];
      Q = (BID_UINT32)(CT >> amount);

      diff_expon += nzeros;
	  }
    } 
	else {
      nzeros = 0;

	// decompose digit
	PD = (BID_UINT64) Q *0x068DB8BBull;
	digit_h = (BID_UINT32) (PD >> 40);
	digit_low = Q - digit_h * 10000;

	if (!digit_low)
	  nzeros += 4;
	else
	  digit_h = digit_low;

	if (!(digit_h & 1)) {
	  nzeros +=
	    3 & (BID_UINT32) (bid_packed_10000_zeros[digit_h >> 3] >>
			  (digit_h & 7));
      }

      if (nzeros) {
	     CT = (BID_UINT64)Q * bid_bid_reciprocals10_32[nzeros];
		 CT >>=32;

	// now get P/10^extra_digits: shift C64 right by M[extra_digits]-128
	amount = bid_bid_bid_recip_scale32[nzeros];
	Q = (BID_UINT32)(CT >> amount);
      }
      diff_expon += nzeros;

    }
    if (diff_expon >= 0) {
      res =
	get_BID32 (sign_x ^ sign_y, diff_expon, Q,
				 rnd_mode, pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
      // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
      BID_RETURN (res);
    }
  }
#endif

  if (diff_expon >= 0) {
#ifdef IEEE_ROUND_NEAREST
    // round to nearest code
    // R*10
    R += R;
    R = (R << 2) + R;
    B5 = B4 + B;

    // compare 10*R to 5*B
    R = B5 - R;
    // correction for (R==0 && (Q&1))
    R -= (Q & 1);
    // R<0 ?
    D = ((BID_UINT32) R) >> 31;
    Q += D;
#else
#ifdef IEEE_ROUND_NEAREST_TIES_AWAY
    // round to nearest code
    // R*10
    R += R;
    R = (R << 2) + R;
    B5 = B4 + B;

    // compare 10*R to 5*B
    R = B5 - R;
    // correction for (R==0 && (Q&1))
    R -= (Q & 1);
    // R<0 ?
    D = ((BID_UINT32) R) >> 31;
    Q += D;
#else
    rmode = rnd_mode;
    if (sign_x ^ sign_y && (unsigned) (rmode - 1) < 2)
      rmode = 3 - rmode;
    switch (rmode) {
    case 0:	// round to nearest code
    case BID_ROUNDING_TIES_AWAY:
      // R*10
      R += R;
      R = (R << 2) + R;
      B5 = B4 + B;
      // compare 10*R to 5*B
      R = B5 - R;
      // correction for (R==0 && (Q&1))
      R -= ((Q | (rmode >> 2)) & 1);
      // R<0 ?
      D = ((BID_UINT32) R) >> 31;
      Q += D;
      break;
    case BID_ROUNDING_DOWN:
    case BID_ROUNDING_TO_ZERO:
      break;
    default:	// rounding up
      Q++;
      break;
    }
#endif
#endif

    res =
      get_BID32 (sign_x ^ sign_y, diff_expon, Q, rnd_mode,
			       pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
    // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
    BID_RETURN (res);
  } else {
    // UF occurs
#ifdef BID_SET_STATUS_FLAGS
    if ((diff_expon + 7 < 0)) {
      // set status flags
      __set_status_flags (pfpsf, BID_INEXACT_EXCEPTION);
    }
#endif
    rmode = rnd_mode;
    res =
      get_BID32_UF (sign_x ^ sign_y, diff_expon, Q, R, rmode, pfpsf);
#ifdef UNCHANGED_BINARY_STATUS_FLAGS
    // (void) fesetexceptflag (&binaryflags, BID_FE_ALL_FLAGS);
#endif
    BID_RETURN (res);

  }
}
