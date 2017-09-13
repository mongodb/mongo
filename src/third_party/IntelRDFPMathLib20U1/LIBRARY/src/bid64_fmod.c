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
 *    BID64 remainder
 *****************************************************************************
 *
 *  Algorithm description:
 *
 *  if(exponent_x < exponent_y)
 *    scale coefficient_y so exponents are aligned
 *    perform coefficient divide (64-bit integer divide), unless
 *            coefficient_y is longer than 64 bits (clearly larger 
 *                                               than coefficient_x) 
 *  else  // exponent_x > exponent_y
 *     use a loop to scale coefficient_x to 18_digits, divide by 
 *         coefficient_y (64-bit integer divide), calculate remainder
 *         as new_coefficient_x and repeat until final remainder is obtained 
 *         (when new_exponent_x < exponent_y)
 *
 ****************************************************************************/

#define BID_FUNCTION_SETS_BINARY_FLAGS

#include "bid_internal.h"

#define MAX_FORMAT_DIGITS     16
#define DECIMAL_EXPONENT_BIAS 398
#define MASK_BINARY_EXPONENT  0x7ff0000000000000ull
#define BINARY_EXPONENT_BIAS  0x3ff
#define UPPER_EXPON_LIMIT     51

BID_TYPE0_FUNCTION_ARGTYPE1_ARGTYPE2_NORND(BID_UINT64, bid64_fmod, BID_UINT64, x, BID_UINT64, y)

  BID_UINT128 CY;
  BID_UINT64 sign_x, sign_y, coefficient_x, coefficient_y, res;
  BID_UINT64 Q, R, T, valid_y, valid_x;
  int_float tempx;
  int exponent_x, exponent_y, bin_expon, e_scale;
  int digits_x, diff_expon;

  BID_OPT_SAVE_BINARY_FLAGS()

  valid_y = unpack_BID64 (&sign_y, &exponent_y, &coefficient_y, y);
  valid_x = unpack_BID64 (&sign_x, &exponent_x, &coefficient_x, x);

  // unpack arguments, check for NaN or Infinity
  if (!valid_x) {
    // x is Inf. or NaN or 0
#ifdef BID_SET_STATUS_FLAGS
    if ((y & SNAN_MASK64) == SNAN_MASK64)	// y is sNaN
      __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif

    // test if x is NaN
    if ((x & 0x7c00000000000000ull) == 0x7c00000000000000ull) {
#ifdef BID_SET_STATUS_FLAGS
      if (((x & SNAN_MASK64) == SNAN_MASK64))
	__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      res = coefficient_x & QUIET_MASK64;;
      BID_RETURN (res);
    }
    // x is Infinity?
    if ((x & 0x7800000000000000ull) == 0x7800000000000000ull) {
      if (((y & NAN_MASK64) != NAN_MASK64)) {
#ifdef BID_SET_STATUS_FLAGS
	__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
	// return NaN
	res = 0x7c00000000000000ull;
	BID_RETURN (res);
      }
    }
    // x is 0
    // return x if y != 0
    if (((y & 0x7800000000000000ull) < 0x7800000000000000ull) &&
	coefficient_y) {
      if ((y & 0x6000000000000000ull) == 0x6000000000000000ull)
	exponent_y = (y >> 51) & 0x3ff;
      else
	exponent_y = (y >> 53) & 0x3ff;

      if (exponent_y < exponent_x)
	exponent_x = exponent_y;

      x = exponent_x;
      x <<= 53;

      res = x | sign_x;
      BID_RETURN (res);
    }

  }
  if (!valid_y) {
    // y is Inf. or NaN

    // test if y is NaN
    if ((y & 0x7c00000000000000ull) == 0x7c00000000000000ull) {
#ifdef BID_SET_STATUS_FLAGS
      if (((y & SNAN_MASK64) == SNAN_MASK64))
	__set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      res = coefficient_y & QUIET_MASK64;;
      BID_RETURN (res);
    }
    // y is Infinity?
    if ((y & 0x7800000000000000ull) == 0x7800000000000000ull) {
      res = very_fast_get_BID64 (sign_x, exponent_x, coefficient_x);
      BID_RETURN (res);
    }
    // y is 0, return NaN
    {
#ifdef BID_SET_STATUS_FLAGS
      __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
      res = 0x7c00000000000000ull;
      BID_RETURN (res);
    }
  }


  diff_expon = exponent_x - exponent_y;
  if (diff_expon <= 0) {
    diff_expon = -diff_expon;

    if (diff_expon > 16) {
      // |x|<|y| in this case
      res = x;
      BID_RETURN (res);
    }
    // set exponent of y to exponent_x, scale coefficient_y
    T = bid_power10_table_128[diff_expon].w[0];
    __mul_64x64_to_128 (CY, coefficient_y, T);

    if (CY.w[1] || CY.w[0] > (coefficient_x)) {
      res = x;
      BID_RETURN (res);
    }

    Q = coefficient_x / CY.w[0];
    R = coefficient_x - Q * CY.w[0];

    res = very_fast_get_BID64 (sign_x, exponent_x, R);
    BID_RETURN (res);
  }


  while (diff_expon > 0) {
    // get number of digits in coeff_x
    tempx.d = (float) coefficient_x;
    bin_expon = ((tempx.i >> 23) & 0xff) - 0x7f;
    digits_x = bid_estimate_decimal_digits[bin_expon];
    // will not use this test, dividend will have 18 or 19 digits
    //if(coefficient_x >= bid_power10_table_128[digits_x].w[0])
    //      digits_x++;

    e_scale = 18 - digits_x;
    if (diff_expon >= e_scale) {
      diff_expon -= e_scale;
    } else {
      e_scale = diff_expon;
      diff_expon = 0;
    }

    // scale dividend to 18 or 19 digits
    coefficient_x *= bid_power10_table_128[e_scale].w[0];

    // quotient
    Q = coefficient_x / coefficient_y;
    // remainder
    coefficient_x -= Q * coefficient_y;

    // check for remainder == 0
    if (!coefficient_x) {
      res = very_fast_get_BID64_small_mantissa (sign_x, exponent_y, 0);
      BID_RETURN (res);
    }
  }

  res = very_fast_get_BID64 (sign_x, exponent_y, coefficient_x);
  BID_RETURN (res);

}
