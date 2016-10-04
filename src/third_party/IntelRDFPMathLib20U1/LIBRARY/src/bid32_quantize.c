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

BID_TYPE0_FUNCTION_ARGTYPE1_ARGTYPE2(BID_UINT32, bid32_quantize, BID_UINT32, x, BID_UINT32, y)
  BID_UINT64 CT;
  BID_UINT32 sign_x, sign_y, coefficient_x, coefficient_y, remainder_h, C64,
    valid_x, CT0;
  BID_UINT32 carry, res;
  int_float tempx;
  int exponent_x, exponent_y, digits_x, extra_digits, amount, amount2;
  int expon_diff, total_digits, bin_expon_cx;
  unsigned rmode, status;

  BID_OPT_SAVE_BINARY_FLAGS()

  valid_x = unpack_BID32 (&sign_x, &exponent_x, &coefficient_x, x);
  // unpack arguments, check for NaN or Infinity
  if (!unpack_BID32 (&sign_y, &exponent_y, &coefficient_y, y)) {
   // Inf. or NaN or 0
#ifdef BID_SET_STATUS_FLAGS
    if ((x & SNAN_MASK32) == SNAN_MASK32)	// y is sNaN
      __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif

    // x=Inf, y=Inf?
    if (((coefficient_x << 1) == 0xf0000000ul)
	&& ((coefficient_y << 1) == 0xf0000000ul)) {
      res = coefficient_x;
      BID_RETURN (res);
    }
    // Inf or NaN?
    if ((y & 0x78000000ul) == 0x78000000ul) {
#ifdef BID_SET_STATUS_FLAGS
      if (((y & SNAN_MASK32) == SNAN_MASK32)	// sNaN
	  || (((y & NAN_MASK32) == INFINITY_MASK32) &&	//Inf
	      ((x & NAN_MASK32) < INFINITY_MASK32)))
	__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      if ((y & NAN_MASK32) != NAN_MASK32)
	coefficient_y = 0;
      if ((x & NAN_MASK32) != NAN_MASK32) {
	res = 0x7c000000ul | (coefficient_y & QUIET_MASK32);
	if (((y & NAN_MASK32) != NAN_MASK32) && ((x & NAN_MASK32) == INFINITY_MASK32))
		res = x;
	BID_RETURN (res);
      }
    }
  }
  // unpack arguments, check for NaN or Infinity
  if (!valid_x) {
    // x is Inf. or NaN or 0

    // Inf or NaN?
    if ((x & INFINITY_MASK32) == INFINITY_MASK32) {
#ifdef BID_SET_STATUS_FLAGS
      if (((x & SNAN_MASK32) == SNAN_MASK32)	// sNaN
	  || ((x & NAN_MASK32) == INFINITY_MASK32))	//Inf 
	__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      if ((x & NAN_MASK32) != NAN_MASK32)
	coefficient_x = 0;
      res = NAN_MASK32 | (coefficient_x & QUIET_MASK32);
      BID_RETURN (res);
    }

    res = very_fast_get_BID32 (sign_x, exponent_y, 0);
    BID_RETURN (res);
  }
  // get number of decimal digits in coefficient_x
  tempx.d = (float) coefficient_x;
  bin_expon_cx = ((tempx.i >> 23) & 0xff) - 0x7f;
  digits_x = bid_estimate_decimal_digits[bin_expon_cx];
  if (coefficient_x >= bid_power10_table_128[digits_x].w[0])
    digits_x++;

  expon_diff = exponent_x - exponent_y;
  total_digits = digits_x + expon_diff;

  // check range of scaled coefficient
  if ((BID_UINT32) (total_digits + 1) <= 8) {
    if (expon_diff >= 0) {
      coefficient_x *= (BID_UINT32)bid_power10_table_128[expon_diff].w[0];
      res = very_fast_get_BID32 (sign_x, exponent_y, coefficient_x);
      BID_RETURN (res);
    }
    // must round off -expon_diff digits
    extra_digits = -expon_diff;
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
    coefficient_x += bid_round_const_table[rmode][extra_digits];

    // get P*(2^M[extra_digits])/10^extra_digits
    CT = (BID_UINT64)coefficient_x * bid_bid_reciprocals10_32[extra_digits];

    // now get P/10^extra_digits: shift C64 right by M[extra_digits]-128
    amount = bid_bid_bid_recip_scale32[extra_digits];
    CT0 = (CT >>32);
	C64 = CT0 >> amount;
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
    if (rnd_mode == 0)
#endif
      if (C64 & 1) {
	// check whether fractional part of initial_P/10^extra_digits 
	// is exactly .5
	// this is the same as fractional part of 
	//   (initial_P + 0.5*10^extra_digits)/10^extra_digits is exactly zero

	// get remainder
	amount2 = 32 - amount;
	remainder_h = 0;
	remainder_h--;
	remainder_h >>= amount2;
	remainder_h = remainder_h & CT0;

	// test whether fractional part is 0
	if (!remainder_h && ((BID_UINT32)CT < (BID_UINT32)bid_bid_reciprocals10_32[extra_digits])) {
	  C64--;
	}
      }
#endif

#ifdef BID_SET_STATUS_FLAGS
    status = BID_INEXACT_EXCEPTION;
    // get remainder
    remainder_h = CT0 << (32 - amount);
    switch (rmode) {
    case BID_ROUNDING_TO_NEAREST:
    case BID_ROUNDING_TIES_AWAY:
      // test whether fractional part is 0
      if ((remainder_h == 0x80000000ul)
	  && ((BID_UINT32)CT < bid_bid_reciprocals10_32[extra_digits]))
	status = BID_EXACT_STATUS;
      break;
    case BID_ROUNDING_DOWN:
    case BID_ROUNDING_TO_ZERO:
      if (!remainder_h && ((BID_UINT32)CT < bid_bid_reciprocals10_32[extra_digits]))
	status = BID_EXACT_STATUS;
      break;
    default:
      // round up
	  carry = ((((BID_UINT32)CT)+((BID_UINT32)bid_bid_reciprocals10_32[extra_digits]))<((BID_UINT32)CT))?1:0;
      if ((remainder_h >> (32 - amount)) + carry >=
	  (((BID_UINT32) 1) << amount))
	status = BID_EXACT_STATUS;
      break;
    }
    __set_status_flags (pfpsf, status);
#endif

    res = very_fast_get_BID32 (sign_x, exponent_y, C64);
    BID_RETURN (res);
  }

  if (total_digits < 0) {
#ifdef BID_SET_STATUS_FLAGS
    __set_status_flags (pfpsf, BID_INEXACT_EXCEPTION);
#endif
    C64 = 0;
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
    rmode = rnd_mode;
    if (sign_x && (unsigned) (rmode - 1) < 2)
      rmode = 3 - rmode;
    if (rmode == BID_ROUNDING_UP)
      C64 = 1;
#endif
#endif
    res = very_fast_get_BID32 (sign_x, exponent_y, C64);
    BID_RETURN (res);
  }
  // else  more than 16 digits in coefficient
#ifdef BID_SET_STATUS_FLAGS
  __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
  res = 0x7c000000ul;
  BID_RETURN (res);

}
