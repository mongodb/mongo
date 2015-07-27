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
#define BID_128RES
#include "bid_internal.h"

/*
 * Takes a BID64 as input and converts it to a BID128 and returns it. 
 */
BID_TYPE0_FUNCTION_ARGTYPE1_NORND_NOFLAGS (BID_UINT128, bid64_to_bid128, BID_UINT64, x)

     BID_UINT128 new_coeff, res;
     BID_UINT64 sign_x;
     int exponent_x;
     BID_UINT64 coefficient_x;

if (!unpack_BID64 (&sign_x, &exponent_x, &coefficient_x, x)) {
if (((x) << 1) >= 0xf000000000000000ull) {
#ifdef BID_SET_STATUS_FLAGS
  if (((x) & SNAN_MASK64) == SNAN_MASK64)	// sNaN
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
  res.w[0] = (coefficient_x & 0x0003ffffffffffffull);
  __mul_64x64_to_128 (res, res.w[0], bid_power10_table_128[18].w[0]);
  res.w[1] |= ((coefficient_x) & 0xfc00000000000000ull);
  BID_RETURN_NOFLAGS (res);
}
}

new_coeff.w[0] = coefficient_x;
new_coeff.w[1] = 0;
bid_get_BID128_very_fast (&res, sign_x,
		      exponent_x + DECIMAL_EXPONENT_BIAS_128 -
		      DECIMAL_EXPONENT_BIAS, new_coeff);
BID_RETURN_NOFLAGS (res);
}	// convert_bid64_to_bid128



/*
 * Takes a BID128 as input and converts it to a BID64 and returns it.
 */
BID_TYPE0_FUNCTION_ARGTYPE1(BID_UINT64, bid128_to_bid64, BID_UINT128, x)

BID_UINT128 CX, T128, TP128, Qh, Ql, Qh1, Stemp, Tmp, Tmp1, CX1;
  BID_UINT64 sign_x, carry, cy, res;
  BID_SINT64 D;
  int_float f64, fx;
  int exponent_x, extra_digits, amount, bin_expon_cx;
  unsigned rmode, status, uf_check = 0;

  BID_OPT_SAVE_BINARY_FLAGS()

  BID_SWAP128 (x);
  // unpack arguments, check for NaN or Infinity or 0
  if (!unpack_BID128_value (&sign_x, &exponent_x, &CX, x)) {
    if ((x.w[1] << 1) >= 0xf000000000000000ull) {
      Tmp.w[1] = (CX.w[1] & 0x00003fffffffffffull);
      Tmp.w[0] = CX.w[0];
      TP128 = bid_reciprocals10_128[18];
      __mul_128x128_full (Qh, Ql, Tmp, TP128);
      amount = bid_recip_scale[18];
      __shr_128 (Tmp, Qh, amount);
      res = (CX.w[1] & 0xfc00000000000000ull) | Tmp.w[0];
#ifdef BID_SET_STATUS_FLAGS
      if ((x.w[1] & SNAN_MASK64) == SNAN_MASK64)	// sNaN
	__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      BID_RETURN_VAL (res);
    }
    exponent_x =
      exponent_x - DECIMAL_EXPONENT_BIAS_128 + DECIMAL_EXPONENT_BIAS;
    if (exponent_x < 0) {
      res = sign_x;
      BID_RETURN_VAL (res);
    }
    if (exponent_x > DECIMAL_MAX_EXPON_64)
      exponent_x = DECIMAL_MAX_EXPON_64;
    res = sign_x | (((BID_UINT64) exponent_x) << 53);
    BID_RETURN_VAL (res);
  }

  if (CX.w[1] || (CX.w[0] >= 10000000000000000ull)) {
    // find number of digits in coefficient
    // 2^64
    f64.i = 0x5f800000;
    // fx ~ CX
    fx.d = (float) CX.w[1] * f64.d + (float) CX.w[0];
    bin_expon_cx = ((fx.i >> 23) & 0xff) - 0x7f;
    extra_digits = bid_estimate_decimal_digits[bin_expon_cx] - 16;
    // scale = 38-estimate_decimal_digits[bin_expon_cx];
    D = CX.w[1] - bid_power10_index_binexp_128[bin_expon_cx].w[1];
    if (D > 0
	|| (!D
	    && CX.w[0] >= bid_power10_index_binexp_128[bin_expon_cx].w[0]))
      extra_digits++;

    exponent_x += extra_digits;

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
    if (exponent_x < DECIMAL_EXPONENT_BIAS_128 - DECIMAL_EXPONENT_BIAS) {
      uf_check = 1;
      if (-extra_digits + exponent_x - DECIMAL_EXPONENT_BIAS_128 +
	  DECIMAL_EXPONENT_BIAS + 35 >= 0) {
	if (exponent_x ==
	    DECIMAL_EXPONENT_BIAS_128 - DECIMAL_EXPONENT_BIAS - 1) {
	  T128 = bid_round_const_table_128[rmode][extra_digits];
	  __add_carry_out (CX1.w[0], carry, T128.w[0], CX.w[0]);
	  CX1.w[1] = CX.w[1] + T128.w[1] + carry;
#if DECIMAL_TINY_DETECTION_AFTER_ROUNDING  
	  if (__unsigned_compare_ge_128
	      (CX1, bid_power10_table_128[extra_digits + 16]))
	    uf_check = 0;
#endif
	}
	extra_digits =
	  extra_digits + DECIMAL_EXPONENT_BIAS_128 -
	  DECIMAL_EXPONENT_BIAS - exponent_x;
	exponent_x = DECIMAL_EXPONENT_BIAS_128 - DECIMAL_EXPONENT_BIAS;
	//uf_check = 2;
      } else
	rmode = BID_ROUNDING_TO_ZERO;
    }

    T128 = bid_round_const_table_128[rmode][extra_digits];
    __add_carry_out (CX.w[0], carry, T128.w[0], CX.w[0]);
    CX.w[1] = CX.w[1] + T128.w[1] + carry;

    TP128 = bid_reciprocals10_128[extra_digits];
    __mul_128x128_full (Qh, Ql, CX, TP128);
    amount = bid_recip_scale[extra_digits];

    if (amount >= 64) {
      CX.w[0] = Qh.w[1] >> (amount - 64);
      CX.w[1] = 0;
    } else {
      __shr_128 (CX, Qh, amount);
    }

#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
#ifndef IEEE_ROUND_NEAREST
    if (!(rmode))
#endif
      if (CX.w[0] & 1) {
	// check whether fractional part of initial_P/10^ed1 is exactly .5

	// get remainder
	__shl_128_long (Qh1, Qh, (128 - amount));

	if (!Qh1.w[1] && !Qh1.w[0]
	    && (Ql.w[1] < bid_reciprocals10_128[extra_digits].w[1]
		|| (Ql.w[1] == bid_reciprocals10_128[extra_digits].w[1]
		    && Ql.w[0] < bid_reciprocals10_128[extra_digits].w[0]))) {
	  CX.w[0]--;
	}
      }
#endif

    {
      status = BID_INEXACT_EXCEPTION;
      // get remainder
      __shl_128_long (Qh1, Qh, (128 - amount));

      switch (rmode) {
      case BID_ROUNDING_TO_NEAREST:
      case BID_ROUNDING_TIES_AWAY:
	// test whether fractional part is 0
	if (Qh1.w[1] == 0x8000000000000000ull && (!Qh1.w[0])
	    && (Ql.w[1] < bid_reciprocals10_128[extra_digits].w[1]
		|| (Ql.w[1] == bid_reciprocals10_128[extra_digits].w[1]
		    && Ql.w[0] < bid_reciprocals10_128[extra_digits].w[0])))
	  status = BID_EXACT_STATUS;
	break;
      case BID_ROUNDING_DOWN:
      case BID_ROUNDING_TO_ZERO:
	if ((!Qh1.w[1]) && (!Qh1.w[0])
	    && (Ql.w[1] < bid_reciprocals10_128[extra_digits].w[1]
		|| (Ql.w[1] == bid_reciprocals10_128[extra_digits].w[1]
		    && Ql.w[0] < bid_reciprocals10_128[extra_digits].w[0])))
	  status = BID_EXACT_STATUS;
	break;
      default:
	// round up
	__add_carry_out (Stemp.w[0], cy, Ql.w[0],
			 bid_reciprocals10_128[extra_digits].w[0]);
	__add_carry_in_out (Stemp.w[1], carry, Ql.w[1],
			    bid_reciprocals10_128[extra_digits].w[1], cy);
	__shr_128_long (Qh, Qh1, (128 - amount));
	Tmp.w[0] = 1;
	Tmp.w[1] = 0;
	__shl_128_long (Tmp1, Tmp, amount);
	Qh.w[0] += carry;
	if (Qh.w[0] < carry)
	  Qh.w[1]++;
	if (__unsigned_compare_ge_128 (Qh, Tmp1))
	  status = BID_EXACT_STATUS;
      }

      if (status != BID_EXACT_STATUS) {
	if (uf_check)
	  status |= BID_UNDERFLOW_EXCEPTION;
#ifdef BID_SET_STATUS_FLAGS
	__set_status_flags (pfpsf, status);
#endif
      }


    }

  }

  res =
    get_BID64 (sign_x,
	       exponent_x - DECIMAL_EXPONENT_BIAS_128 +
	       DECIMAL_EXPONENT_BIAS, CX.w[0], rnd_mode, pfpsf);
  BID_RETURN_VAL (res);

}
