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

#define BID_FUNCTION_SETS_BINARY_FLAGS
#include "bid_internal.h"

/*
 * Takes a BID32 as input and converts it to a BID64 and returns it.
 */
BID_TYPE0_FUNCTION_ARGTYPE1_NORND_NOFLAGS (BID_UINT64, bid32_to_bid64, BID_UINT32, x)

     BID_UINT64 res;
     BID_UINT32 sign_x;
     int exponent_x;
     BID_UINT32 coefficient_x;

if (!unpack_BID32 (&sign_x, &exponent_x, &coefficient_x, x)) {
    // Inf, NaN, 0
if (((x) & 0x78000000) == 0x78000000) {
  if (((x) & 0x7e000000) == 0x7e000000) {	// sNaN
#ifdef BID_SET_STATUS_FLAGS
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
  }
  res = (coefficient_x & 0x000fffff);
  res *= 1000000000;
  res |= ((((BID_UINT64) coefficient_x) << 32) & 0xfc00000000000000ull);

  BID_RETURN_NOFLAGS (res);
}
}

res =
very_fast_get_BID64_small_mantissa (((BID_UINT64) sign_x) << 32,
				    exponent_x +
				    DECIMAL_EXPONENT_BIAS -
				    DECIMAL_EXPONENT_BIAS_32,
				    (BID_UINT64) coefficient_x);
BID_RETURN_NOFLAGS (res);
}	// convert_bid32_to_bid64


/*
 * Takes a BID64 as input and converts it to a BID32 and returns it.
 */
BID_TYPE0_FUNCTION_ARGTYPE1(BID_UINT32, bid64_to_bid32, BID_UINT64, x)

BID_UINT128 Q;
  BID_UINT64 sign_x, coefficient_x, remainder_h, carry, Stemp;
  BID_UINT32 res;
  BID_UINT64 t64;
  int_float tempx;
  int exponent_x, bin_expon_cx, extra_digits, rmode = 0, amount;
  unsigned status = 0;

  BID_OPT_SAVE_BINARY_FLAGS()

  // unpack arguments, check for NaN or Infinity, 0
  if (!unpack_BID64 (&sign_x, &exponent_x, &coefficient_x, x)) {
    if (((x) & 0x7800000000000000ull) == 0x7800000000000000ull) {
      t64 = (coefficient_x & 0x0003ffffffffffffull);
      res = t64/1000000000ull;
      res |= ((coefficient_x >> 32) & 0xfc000000);
#ifdef BID_SET_STATUS_FLAGS
      if ((x & SNAN_MASK64) == SNAN_MASK64)	// sNaN
	__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      BID_RETURN (res);
    }
    exponent_x =
      exponent_x - DECIMAL_EXPONENT_BIAS + DECIMAL_EXPONENT_BIAS_32;
    if (exponent_x < 0)
      exponent_x = 0;
    if (exponent_x > DECIMAL_MAX_EXPON_32)
      exponent_x = DECIMAL_MAX_EXPON_32;
    res = (sign_x >> 32) | (exponent_x << 23);
    BID_RETURN (res);
  }

  exponent_x =
    exponent_x - DECIMAL_EXPONENT_BIAS + DECIMAL_EXPONENT_BIAS_32;

  // check number of digits
  if (coefficient_x >= 10000000) {
    tempx.d = (float) coefficient_x;
    bin_expon_cx = ((tempx.i >> 23) & 0xff) - 0x7f;
    extra_digits = bid_estimate_decimal_digits[bin_expon_cx] - 7;
    // add test for range
    if (coefficient_x >= bid_power10_index_binexp[bin_expon_cx])
      extra_digits++;

#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
    rmode = rnd_mode;
    if (sign_x && (unsigned) (rmode - 1) < 2)
      rmode = 3 - rmode;
#else
    rmode = 0;
#endif
#else
    rmode = 0;
#endif

    exponent_x += extra_digits;
    if ((exponent_x < 0) && (exponent_x + MAX_FORMAT_DIGITS_32 >= 0)) {
      status = BID_UNDERFLOW_EXCEPTION;
#if DECIMAL_TINY_DETECTION_AFTER_ROUNDING  
      if (exponent_x == -1)
	if (coefficient_x + bid_round_const_table[rmode][extra_digits] >=
	    bid_power10_table_128[extra_digits + 7].w[0])
	  status = 0;
#endif
      extra_digits -= exponent_x;
      exponent_x = 0;
    }
    coefficient_x += bid_round_const_table[rmode][extra_digits];
    __mul_64x64_to_128 (Q, coefficient_x,
			bid_reciprocals10_64[extra_digits]);

    // now get P/10^extra_digits: shift Q_high right by M[extra_digits]-128
    amount = bid_short_recip_scale[extra_digits];

    coefficient_x = Q.w[1] >> amount;

#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
    if (rmode == 0)	//BID_ROUNDING_TO_NEAREST
#endif
      if (coefficient_x & 1) {
	// check whether fractional part of initial_P/10^extra_digits 
	// is exactly .5

	// get remainder
	remainder_h = Q.w[1] << (64 - amount);

	if (!remainder_h && (Q.w[0] < bid_reciprocals10_64[extra_digits]))
	  coefficient_x--;
      }
#endif

#ifdef BID_SET_STATUS_FLAGS

    {
      status |= BID_INEXACT_EXCEPTION;
      // get remainder
      remainder_h = Q.w[1] << (64 - amount);

      switch (rmode) {
      case BID_ROUNDING_TO_NEAREST:
      case BID_ROUNDING_TIES_AWAY:
	// test whether fractional part is 0
	if (remainder_h == 0x8000000000000000ull
	    && (Q.w[0] < bid_reciprocals10_64[extra_digits]))
	  status = BID_EXACT_STATUS;
	break;
      case BID_ROUNDING_DOWN:
      case BID_ROUNDING_TO_ZERO:
	if (!remainder_h && (Q.w[0] < bid_reciprocals10_64[extra_digits]))
	  status = BID_EXACT_STATUS;
	break;
      default:
	// round up
	__add_carry_out (Stemp, carry, Q.w[0],
			 bid_reciprocals10_64[extra_digits]);
	if ((remainder_h >> (64 - amount)) + carry >=
	    (((BID_UINT64) 1) << amount))
	  status = BID_EXACT_STATUS;
      }

      if (status != BID_EXACT_STATUS)
	__set_status_flags (pfpsf, status);
    }

#endif

  }

  res =
    get_BID32 ((BID_UINT32) (sign_x >> 32),
	       exponent_x, coefficient_x, rnd_mode, pfpsf);
  BID_RETURN (res);

}
