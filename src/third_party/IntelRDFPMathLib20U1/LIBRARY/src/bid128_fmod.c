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
#include "bid_div_macros.h"


BID128_FUNCTION_ARG2_NORND ( bid128_fmod, x, y)

     BID_UINT256 P256;
     BID_UINT128 CX, CY, CQ, CR, T, CXS, P128, res;
     BID_UINT64 sign_x, sign_y, valid_y;
     BID_SINT64 D;
     int_float f64, fx;
     int exponent_x, exponent_y, diff_expon, bin_expon_cx, scale,
       scale0;

  BID_OPT_SAVE_BINARY_FLAGS()

  // unpack arguments, check for NaN or Infinity

valid_y = unpack_BID128_value (&sign_y, &exponent_y, &CY, y);

if (!unpack_BID128_value (&sign_x, &exponent_x, &CX, x)) {
#ifdef BID_SET_STATUS_FLAGS
if ((y.w[1] & SNAN_MASK64) == SNAN_MASK64)	// y is sNaN
  __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
    // test if x is NaN
if ((x.w[1] & 0x7c00000000000000ull) == 0x7c00000000000000ull) {
#ifdef BID_SET_STATUS_FLAGS
  if ((x.w[1] & SNAN_MASK64) == SNAN_MASK64)	// y is sNaN
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
  res.w[1] = CX.w[1] & QUIET_MASK64;
  res.w[0] = CX.w[0];
  BID_RETURN (res);
}
    // x is Infinity?
if ((x.w[1] & 0x7800000000000000ull) == 0x7800000000000000ull) {
  // check if y is Inf.
  if (((y.w[1] & 0x7c00000000000000ull) != 0x7c00000000000000ull))
    // return NaN 
  {
#ifdef BID_SET_STATUS_FLAGS
    // set status flags
    __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
    res.w[1] = 0x7c00000000000000ull;
    res.w[0] = 0;
    BID_RETURN (res);
  }

}
    // x is 0
if ((!CY.w[1]) && (!CY.w[0])) {
#ifdef BID_SET_STATUS_FLAGS
  // set status flags
  __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
  // x=y=0, return NaN
  res.w[1] = 0x7c00000000000000ull;
  res.w[0] = 0;
  BID_RETURN (res);
}
if (valid_y || ((y.w[1] & NAN_MASK64) == INFINITY_MASK64)) {
  // return 0
  if ((exponent_x > exponent_y)
      && ((y.w[1] & NAN_MASK64) != INFINITY_MASK64))
    exponent_x = exponent_y;

  res.w[1] = sign_x | (((BID_UINT64) exponent_x) << 49);
  res.w[0] = 0;
  BID_RETURN (res);
}
}
if (!valid_y) {
  // y is Inf. or NaN

  // test if y is NaN
  if ((y.w[1] & 0x7c00000000000000ull) == 0x7c00000000000000ull) {
#ifdef BID_SET_STATUS_FLAGS
    if ((y.w[1] & SNAN_MASK64) == SNAN_MASK64)	// y is sNaN
      __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
    res.w[1] = CY.w[1] & QUIET_MASK64;
    res.w[0] = CY.w[0];
    BID_RETURN (res);
  }
  // y is Infinity?
  if ((y.w[1] & 0x7800000000000000ull) == 0x7800000000000000ull) {
    // return x
    res.w[1] = x.w[1];
    res.w[0] = x.w[0];
    BID_RETURN (res);
  }
  // y is 0
#ifdef BID_SET_STATUS_FLAGS
  // set status flags
  __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
  res.w[1] = 0x7c00000000000000ull;
  res.w[0] = 0;
  BID_RETURN (res);
}

diff_expon = exponent_x - exponent_y;

if (diff_expon <= 0) {
  diff_expon = -diff_expon;

  if (diff_expon > 34) {
    // |x|<|y| in this case
    res = x;
    BID_RETURN (res);
  }
  // set exponent of y to exponent_x, scale coefficient_y
  T = bid_power10_table_128[diff_expon];
  __mul_128x128_to_256 (P256, CY, T);

  if (P256.w[2] || P256.w[3]) {
    // |x|<|y| in this case
    res = x;
    BID_RETURN (res);
  }

  if (__unsigned_compare_gt_128 (P256, CX)) {
    // |x|<|y| in this case
    res = x;
    BID_RETURN (res);
  }

  P128.w[0] = P256.w[0];
  P128.w[1] = P256.w[1];
  bid___div_128_by_128 (&CQ, &CR, CX, P128);

  bid_get_BID128_very_fast (&res, sign_x, exponent_x, CR);
  BID_RETURN (res);
}
  // 2^64
f64.i = 0x5f800000;

scale0 = 38;
if (!CY.w[1])
  scale0 = 34;

while (diff_expon > 0) {
  // get number of digits in CX and scale=38-digits
  // fx ~ CX
  fx.d = (float) CX.w[1] * f64.d + (float) CX.w[0];
  bin_expon_cx = ((fx.i >> 23) & 0xff) - 0x7f;
  scale = scale0 - bid_estimate_decimal_digits[bin_expon_cx];
  // scale = 38-estimate_decimal_digits[bin_expon_cx];
  D = CX.w[1] - bid_power10_index_binexp_128[bin_expon_cx].w[1];
  if (D > 0
      || (!D && CX.w[0] >= bid_power10_index_binexp_128[bin_expon_cx].w[0]))
    scale--;

  if (diff_expon >= scale)
    diff_expon -= scale;
  else {
    scale = diff_expon;
    diff_expon = 0;
  }

  T = bid_power10_table_128[scale];
  __mul_128x128_low (CXS, CX, T);

  bid___div_128_by_128 (&CQ, &CX, CXS, CY);

  // check for remainder == 0
  if (!CX.w[1] && !CX.w[0]) {
    bid_get_BID128_very_fast (&res, sign_x, exponent_y, CX);
    BID_RETURN (res);
  }
}

bid_get_BID128_very_fast (&res, sign_x, exponent_y, CX);
BID_RETURN (res);
}
