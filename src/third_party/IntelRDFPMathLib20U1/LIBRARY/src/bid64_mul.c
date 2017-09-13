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
 *    BID64 multiply
 *****************************************************************************
 *
 *  Algorithm description:
 *
 *  if(number_digits(coefficient_x)+number_digits(coefficient_y) guaranteed
 *       below 16)
 *      return get_BID64(sign_x^sign_y, exponent_x + exponent_y - dec_bias,
 *                     coefficient_x*coefficient_y)
 *  else
 *      get long product: coefficient_x*coefficient_y
 *      determine number of digits to round off (extra_digits)
 *      rounding is performed as a 128x128-bit multiplication by 
 *         2^M[extra_digits]/10^extra_digits, followed by a shift
 *         M[extra_digits] is sufficiently large for required accuracy 
 *
 ****************************************************************************/

#define BID_FUNCTION_SETS_BINARY_FLAGS
#include "bid_internal.h"

BID_TYPE0_FUNCTION_ARGTYPE1_ARGTYPE2(BID_UINT64, bid64_mul, BID_UINT64, x, BID_UINT64, y)
  BID_UINT128 P, C128, Q_high, Q_low, Stemp;
#if DECIMAL_TINY_DETECTION_AFTER_ROUNDING 
  BID_UINT128 PU;
#endif
  BID_UINT64 sign_x, sign_y, coefficient_x, coefficient_y;
  BID_UINT64 C64, remainder_h, carry, CY, res;
  BID_UINT64 valid_x, valid_y;
  int_double tempx, tempy;
  int extra_digits, exponent_x, exponent_y, bin_expon_cx, bin_expon_cy,
    bin_expon_product;
  int rmode, digits_p, bp, amount, amount2, final_exponent, round_up;
  unsigned status, uf_status;

  BID_OPT_SAVE_BINARY_FLAGS()

  valid_x = unpack_BID64 (&sign_x, &exponent_x, &coefficient_x, x);
  valid_y = unpack_BID64 (&sign_y, &exponent_y, &coefficient_y, y);

  // unpack arguments, check for NaN or Infinity
  if (!valid_x) {

#ifdef BID_SET_STATUS_FLAGS
    if ((y & SNAN_MASK64) == SNAN_MASK64)	// y is sNaN
      __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
    // x is Inf. or NaN

    // test if x is NaN
    if ((x & NAN_MASK64) == NAN_MASK64) {
#ifdef BID_SET_STATUS_FLAGS
      if ((x & SNAN_MASK64) == SNAN_MASK64)	// sNaN
	__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      BID_RETURN (coefficient_x & QUIET_MASK64);
    }
    // x is Infinity?
    if ((x & INFINITY_MASK64) == INFINITY_MASK64) {
      // check if y is 0
      if (((y & INFINITY_MASK64) != INFINITY_MASK64)
	  && !coefficient_y) {
#ifdef BID_SET_STATUS_FLAGS
	__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
	// y==0 , return NaN
	BID_RETURN (NAN_MASK64);
      }
      // check if y is NaN
      if ((y & NAN_MASK64) == NAN_MASK64)
	// y==NaN , return NaN
	BID_RETURN (coefficient_y & QUIET_MASK64);
      // otherwise return +/-Inf
      BID_RETURN (((x ^ y) & 0x8000000000000000ull) | INFINITY_MASK64);
    }
    // x is 0
    if (((y & INFINITY_MASK64) != INFINITY_MASK64)) {
      if ((y & SPECIAL_ENCODING_MASK64) == SPECIAL_ENCODING_MASK64)
	exponent_y = ((BID_UINT32) (y >> 51)) & 0x3ff;
      else
	exponent_y = ((BID_UINT32) (y >> 53)) & 0x3ff;
      sign_y = y & 0x8000000000000000ull;

      exponent_x += exponent_y - DECIMAL_EXPONENT_BIAS;
      if (exponent_x > DECIMAL_MAX_EXPON_64)
	exponent_x = DECIMAL_MAX_EXPON_64;
      else if (exponent_x < 0)
	exponent_x = 0;
      BID_RETURN ((sign_x ^ sign_y) | (((BID_UINT64) exponent_x) << 53));
    }
  }
  if (!valid_y) {
    // y is Inf. or NaN

    // test if y is NaN
    if ((y & NAN_MASK64) == NAN_MASK64) {
#ifdef BID_SET_STATUS_FLAGS
      if ((y & SNAN_MASK64) == SNAN_MASK64)	// sNaN
	__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      BID_RETURN (coefficient_y & QUIET_MASK64);
    }
    // y is Infinity?
    if ((y & INFINITY_MASK64) == INFINITY_MASK64) {
      // check if x is 0
      if (!coefficient_x) {
	__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
	// x==0, return NaN
	BID_RETURN (NAN_MASK64);
      }
      // otherwise return +/-Inf
      BID_RETURN (((x ^ y) & 0x8000000000000000ull) | INFINITY_MASK64);
    }
    // y is 0
    exponent_x += exponent_y - DECIMAL_EXPONENT_BIAS;
    if (exponent_x > DECIMAL_MAX_EXPON_64)
      exponent_x = DECIMAL_MAX_EXPON_64;
    else if (exponent_x < 0)
      exponent_x = 0;
    BID_RETURN ((sign_x ^ sign_y) | (((BID_UINT64) exponent_x) << 53));
  }
  //--- get number of bits in the coefficients of x and y ---
  // version 2 (original)
  tempx.d = (double) coefficient_x;
  bin_expon_cx = ((tempx.i & MASK_BINARY_EXPONENT) >> 52);
  tempy.d = (double) coefficient_y;
  bin_expon_cy = ((tempy.i & MASK_BINARY_EXPONENT) >> 52);

  // magnitude estimate for coefficient_x*coefficient_y is 
  //        2^(unbiased_bin_expon_cx + unbiased_bin_expon_cx)
  bin_expon_product = bin_expon_cx + bin_expon_cy;

  // check if coefficient_x*coefficient_y<2^(10*k+3)
  // equivalent to unbiased_bin_expon_cx + unbiased_bin_expon_cx < 10*k+1
  if (bin_expon_product < UPPER_EXPON_LIMIT + 2 * BINARY_EXPONENT_BIAS) {
    //  easy multiply
    C64 = coefficient_x * coefficient_y;

    res =
      get_BID64_small_mantissa (sign_x ^ sign_y,
				exponent_x + exponent_y -
				DECIMAL_EXPONENT_BIAS, C64, rnd_mode,
				pfpsf);
    BID_RETURN (res);
  } else {
    uf_status = 0;
    // get 128-bit product: coefficient_x*coefficient_y
    __mul_64x64_to_128 (P, coefficient_x, coefficient_y);

    // tighten binary range of P:  leading bit is 2^bp
    // unbiased_bin_expon_product <= bp <= unbiased_bin_expon_product+1
    bin_expon_product -= 2 * BINARY_EXPONENT_BIAS;

    __tight_bin_range_128 (bp, P, bin_expon_product);

    // get number of decimal digits in the product
    digits_p = bid_estimate_decimal_digits[bp];
    if (!(__unsigned_compare_gt_128 (bid_power10_table_128[digits_p], P)))
      digits_p++;	// if bid_power10_table_128[digits_p] <= P

    // determine number of decimal digits to be rounded out
    extra_digits = digits_p - MAX_FORMAT_DIGITS;
    final_exponent =
      exponent_x + exponent_y + extra_digits - DECIMAL_EXPONENT_BIAS;

#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
    rmode = rnd_mode;
    if (sign_x ^ sign_y && (unsigned) (rmode - 1) < 2)
      rmode = 3 - rmode;
#else
    rmode = 0;
#endif
#else
    rmode = 0;
#endif

    round_up = 0;
    if (((unsigned) final_exponent) >= 3 * 256) {
      if (final_exponent < 0) {
	// underflow
	if (final_exponent + 16 < 0) {
	  res = sign_x ^ sign_y;
	  __set_status_flags (pfpsf,
			      BID_UNDERFLOW_EXCEPTION | BID_INEXACT_EXCEPTION);
	  if (rmode == BID_ROUNDING_UP)
	    res |= 1;
	  BID_RETURN (res);
	}

	uf_status = BID_UNDERFLOW_EXCEPTION;
#if DECIMAL_TINY_DETECTION_AFTER_ROUNDING 
	if (final_exponent == -1) {
	  __add_128_64 (PU, P, bid_round_const_table[rmode][extra_digits]);
	  if (__unsigned_compare_ge_128
	      (PU, bid_power10_table_128[extra_digits + 16]))
	    uf_status = 0;
	}
#endif
	extra_digits -= final_exponent;
	final_exponent = 0;

	if (extra_digits > 17) {
	  __mul_128x128_full (Q_high, Q_low, P, bid_reciprocals10_128[16]);

	  amount = bid_recip_scale[16];
	  __shr_128 (P, Q_high, amount);

	  // get sticky bits
	  amount2 = 64 - amount;
	  remainder_h = 0;
	  remainder_h--;
	  remainder_h >>= amount2;
	  remainder_h = remainder_h & Q_high.w[0];

	  extra_digits -= 16;
	  if (remainder_h || (Q_low.w[1] > bid_reciprocals10_128[16].w[1]
			      || (Q_low.w[1] ==
				  bid_reciprocals10_128[16].w[1]
				  && Q_low.w[0] >=
				  bid_reciprocals10_128[16].w[0]))) {
	    round_up = 1;
	    __set_status_flags (pfpsf,
				BID_UNDERFLOW_EXCEPTION |
				BID_INEXACT_EXCEPTION);
	    P.w[0] = (P.w[0] << 3) + (P.w[0] << 1);
	    P.w[0] |= 1;
	    extra_digits++;
	  }
	}
      } else {
	res =
	  fast_get_BID64_check_OF (sign_x ^ sign_y, final_exponent,
				   1000000000000000ull, rnd_mode,
				   pfpsf);
	BID_RETURN (res);
      }
    }


    if (extra_digits > 0) {
      // will divide by 10^(digits_p - 16)

      // add a constant to P, depending on rounding mode
      // 0.5*10^(digits_p - 16) for round-to-nearest
      __add_128_64 (P, P, bid_round_const_table[rmode][extra_digits]);

      // get P*(2^M[extra_digits])/10^extra_digits
      __mul_128x128_full (Q_high, Q_low, P,
			  bid_reciprocals10_128[extra_digits]);

      // now get P/10^extra_digits: shift Q_high right by M[extra_digits]-128
      amount = bid_recip_scale[extra_digits];
      __shr_128 (C128, Q_high, amount);

      C64 = __low_64 (C128);

#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
      if (rmode == 0)	//BID_ROUNDING_TO_NEAREST
#endif
	if ((C64 & 1) && !round_up) {
	  // check whether fractional part of initial_P/10^extra_digits 
	  // is exactly .5
	  // this is the same as fractional part of 
	  // (initial_P + 0.5*10^extra_digits)/10^extra_digits is exactly zero

	  // get remainder
	  remainder_h = Q_high.w[0] << (64 - amount);

	  // test whether fractional part is 0
	  if (!remainder_h
	      && (Q_low.w[1] < bid_reciprocals10_128[extra_digits].w[1]
		  || (Q_low.w[1] == bid_reciprocals10_128[extra_digits].w[1]
		      && Q_low.w[0] <
		      bid_reciprocals10_128[extra_digits].w[0]))) {
	    C64--;
	  }
	}
#endif

#ifdef BID_SET_STATUS_FLAGS
      status = BID_INEXACT_EXCEPTION | uf_status;

      // get remainder
      remainder_h = Q_high.w[0] << (64 - amount);

      switch (rmode) {
      case BID_ROUNDING_TO_NEAREST:
      case BID_ROUNDING_TIES_AWAY:
	// test whether fractional part is 0
	if (remainder_h == 0x8000000000000000ull
	    && (Q_low.w[1] < bid_reciprocals10_128[extra_digits].w[1]
		|| (Q_low.w[1] == bid_reciprocals10_128[extra_digits].w[1]
		    && Q_low.w[0] <
		    bid_reciprocals10_128[extra_digits].w[0])))
	  status = BID_EXACT_STATUS;
	break;
      case BID_ROUNDING_DOWN:
      case BID_ROUNDING_TO_ZERO:
	if (!remainder_h
	    && (Q_low.w[1] < bid_reciprocals10_128[extra_digits].w[1]
		|| (Q_low.w[1] == bid_reciprocals10_128[extra_digits].w[1]
		    && Q_low.w[0] <
		    bid_reciprocals10_128[extra_digits].w[0])))
	  status = BID_EXACT_STATUS;
	break;
      default:
	// round up
	__add_carry_out (Stemp.w[0], CY, Q_low.w[0],
			 bid_reciprocals10_128[extra_digits].w[0]);
	__add_carry_in_out (Stemp.w[1], carry, Q_low.w[1],
			    bid_reciprocals10_128[extra_digits].w[1], CY);
	if ((remainder_h >> (64 - amount)) + carry >=
	    (((BID_UINT64) 1) << amount))
	  status = BID_EXACT_STATUS;
      }

      __set_status_flags (pfpsf, status);
#endif

      // convert to BID and return
      res =
	fast_get_BID64_check_OF (sign_x ^ sign_y, final_exponent, C64,
				 rnd_mode, pfpsf);
      BID_RETURN (res);
    }
    // go to convert_format and exit
    C64 = __low_64 (P);
    res =
      get_BID64 (sign_x ^ sign_y,
		 exponent_x + exponent_y - DECIMAL_EXPONENT_BIAS, C64,
		 rnd_mode, pfpsf);
    BID_RETURN (res);
  }
}
